// ReXGlue web port (Workstream G): browser main-thread side of the /net relay shim
// (src/system/web/net_relay.cpp, protocol: tools/web_relay/README.md). Evaluated once on the
// main thread; the expression evaluates to a factory that returns the Module.rexNet object.
//
// Reconnect: a socket that ends without close() being called (relay restart, Wi-Fi or tunnel
// blip, half-open TCP found by the ping below) is reopened with backoff (0.5 s doubling to
// 8 s, +-25% jitter; at once when the browser comes back online), forever. The reconnect URL
// carries &resume=<token from the last welcome>&id=<last id>: the relay gives the same id back
// (no leave/join for the others) while the old one is in its resume grace period, or the same
// id if it is free (relay restarted), so the virtual address 10.80.0.N normally survives.
// While the link is down the state is 'connecting', the id stays published, and datagrams
// are dropped and counted (TX_DROPPED), never queued. Liveness: {"type":"ping"} every 10 s;
// no message at all for 20 s after a ping = dead socket, reconnect.
//
// Inbound binary frames are copied into a single-producer ring in wasm memory:
//   ctl (Int32Array, 16 words): see the Ctl enum in net_relay.cpp
//   record: u32 length (little-endian) + frame bytes, padded to 4; u32 0xFFFFFFFF = wrap
// The producer (this thread) only uses Atomics and never blocks; consumers wait on SEQ.
//
// Peer-to-peer path (WebRTC): next to the relay, each pair of peers that both run this code
// opens an RTCPeerConnection with one DataChannel (negotiated id 0, unordered, maxRetransmits 0:
// UDP-like). The relay carries the signalling (hello, offer/answer, ICE) as control frames: an
// ordinary relay datagram to dst port 0 whose shim header has src port 0 and proto 0x50 ('P'),
// JSON payload. Every relay forwards them unchanged, and an older client drops them (no socket
// on port 0), so mixed rooms and old relays keep working on the relay alone. The lower id sends
// the offer. A DataChannel carries the same complete relay frames as the WebSocket; the
// receiver overwrites `src` with the channel's peer id (as the relay does), so the guest sees the
// same virtual LAN. Per peer the path is the DataChannel while it is open and answers its probes,
// else the relay; a broadcast goes over every open channel plus one relay unicast per peer
// without one (or one relay broadcast when no channel is open). Failed or lost channels are
// retried with backoff; datagrams never wait for them. RTT probes run on both paths once a second
// (stats().p2p, and a summary line every 30 s). Options: Module.rexNetOptions = {p2p, iceServers,
// probeMs, statsLogMs}, the page's ?p2p=0|1, ?p2p_ice=<url>[,<url>], ?p2p_probe_ms=N, and the
// relay's welcome (p2p false turns it off; iceServers are used when the page set none).
(function rexNetRelayFactory(env) {
  const HEAD = 0, TAIL = 1, SEQ = 2, STATE = 3, ID = 4, PEERS = 5, RING_DROPPED = 6;
  const RX_FRAMES = 7, TX_FRAMES = 8, CLOSE_CODE = 9, TX_DROPPED = 10, RECONNECTS = 11;
  const BACKOFF_MIN_MS = 500, BACKOFF_MAX_MS = 8000;
  const PING_INTERVAL_MS = 10000, PING_TIMEOUT_MS = 20000;
  const BROADCAST = 0xffffffff;
  const CTL_PROTO = 0x50; // shim proto byte of control frames (dst port 0, src port 0)
  // ICE between two tabs on one Windows PC with Chrome's mDNS host candidates connects only in
  // some attempts (each attempt has new mDNS names), so attempts are short and retried soon.
  const P2P_OPEN_TIMEOUT_MS = 8000, P2P_RETRY_MIN_MS = 2000, P2P_RETRY_MAX_MS = 60000;
  const P2P_DISCONNECTED_MS = 5000; // ICE 'disconnected' this long = path lost
  const PROBE_LOST_MS = 5000, PROBE_UNHEALTHY = 3; // 3 lost channel probes in a row: use the relay
  const DC_MAX_BUFFERED = 1 << 20;
  const RTT_SAMPLES = 512;
  const log = env.log;
  const ringPtr = env.ringPtr, ringSize = env.ringSize, mask = env.ringSize - 1;
  let buf = null, ctl = null, u8 = null, dv = null;
  const views = () => {
    const h = env.heap();
    if (h.buffer !== buf) {
      buf = h.buffer;
      u8 = h;
      ctl = new Int32Array(buf, env.ctlPtr, 16);
      dv = new DataView(buf);
    }
  };
  views();
  const wake = () => {
    Atomics.add(ctl, SEQ, 1);
    Atomics.notify(ctl, SEQ);
  };
  const setState = (s) => {
    Atomics.store(ctl, STATE, s);
    wake();
  };

  function push(bytes) {
    views();
    const n = bytes.length;
    const rec = (4 + n + 3) & ~3;
    const head = Atomics.load(ctl, HEAD) >>> 0;
    const tail = Atomics.load(ctl, TAIL) >>> 0;
    const used = (head - tail) >>> 0;
    const off = head & mask;
    const tailRoom = ringSize - off;
    const need = rec <= tailRoom ? rec : tailRoom + rec;
    if (rec > ringSize / 2 || need > ringSize - used) {
      Atomics.add(ctl, RING_DROPPED, 1);
      return;
    }
    let h = head, o = off;
    if (rec > tailRoom) {
      dv.setUint32(ringPtr + o, 0xffffffff, true);
      h = (h + tailRoom) >>> 0;
      o = 0;
    }
    dv.setUint32(ringPtr + o, n, true);
    u8.set(bytes, ringPtr + o + 4);
    Atomics.store(ctl, HEAD, (h + rec) | 0);
    Atomics.add(ctl, RX_FRAMES, 1);
    wake();
    if (env.onRx) env.onRx(); // tests only
  }

  const resolveUrl = (url, room) => {
    if (url) return url;
    let r = room;
    if (!r && typeof location !== 'undefined') r = new URLSearchParams(location.search).get('room');
    r = r || 'default';
    const proto = typeof location !== 'undefined' && location.protocol === 'https:' ? 'wss://' : 'ws://';
    const host = typeof location !== 'undefined' ? location.host : '127.0.0.1';
    return `${proto}${host}/net?room=${encodeURIComponent(r)}`;
  };

  let ws = null;
  let peers = new Set();
  let token = ''; // resume token of the last welcome (one-time: every welcome brings a new one)
  let lastId = 0;
  let everUp = false;
  let closing = false; // close() was called: no reconnect
  let attempt = 0; // reconnect attempts since the last welcome
  let retryTimer = 0;
  let pingTimer = 0;
  let pingSentAt = 0;
  let lastRxAt = 0;
  let downSince = 0;
  const now = () => (typeof performance !== 'undefined' ? performance.now() : Date.now());
  const wsUp = () => ws && ws.readyState === 1;

  // --- options --------------------------------------------------------------------------
  const opts = env.options || {};
  const query = typeof location !== 'undefined' ? new URLSearchParams(location.search) : new URLSearchParams();
  const qBool = (k) => (query.has(k) ? !/^(0|false|off|no)$/i.test(query.get(k)) : undefined);
  let p2pWanted = qBool('p2p') ?? (opts.p2p !== undefined ? !!opts.p2p : true);
  let p2pRelayAllows = true;
  const pageIce = query.get('p2p_ice')
    ? query.get('p2p_ice').split(',').filter(Boolean).map((urls) => ({ urls }))
    : Array.isArray(opts.iceServers) ? opts.iceServers : null;
  let iceServers = pageIce || [];
  const probeMs = Math.max(100, Number(query.get('p2p_probe_ms')) || Number(opts.probeMs) || 1000);
  const statsLogMs = Number(opts.statsLogMs ?? 30000);
  const debug = qBool('p2p_debug') ?? !!opts.debug; // log every signalling message
  const haveRtc = typeof RTCPeerConnection === 'function';
  const p2pOn = () => p2pWanted && p2pRelayAllows && haveRtc;

  // --- peer-to-peer links ------------------------------------------------------------------
  const enc = new TextEncoder(), dec = new TextDecoder();
  const links = new Map(); // peer id -> link
  const newRtt = () => ({ sent: 0, recv: 0, lost: 0, lostRun: 0, last: 0, samples: [], pending: new Map(), seq: 0 });
  function link(id) {
    let l = links.get(id);
    if (!l) {
      l = {
        id, capable: false, helloAt: 0, hellos: 0, pc: null, dc: null, sess: 0, state: 'relay',
        startAt: 0, openMs: 0, discAt: 0, retries: 0, retryAt: 0, remoteSet: false, pendingIce: [],
        earlyIce: [], cand: '', maxMsg: 65536, healthy: true, why: '',
        rtt: { relay: newRtt(), p2p: newRtt() }, tx: { relay: 0, p2p: 0 }, rx: { relay: 0, p2p: 0 },
      };
      links.set(id, l);
    }
    return l;
  }
  const usable = (l) => l && l.dc && l.state === 'open' && l.healthy && l.dc.readyState === 'open' &&
    l.dc.bufferedAmount < DC_MAX_BUFFERED;
  const isCtl = (b) => b.length >= 14 && b[8] === 0 && b[9] === 0 && b[10] === 0 && b[11] === 0 && b[12] === CTL_PROTO;
  function ctlFrame(dst, obj) {
    const body = enc.encode(JSON.stringify(obj));
    const f = new Uint8Array(14 + body.length);
    const d = new DataView(f.buffer);
    d.setUint32(0, lastId);
    d.setUint32(4, dst);
    f[12] = CTL_PROTO;
    f[13] = 1;
    f.set(body, 14);
    return f;
  }
  // Control message to peer `id` over the relay (via 'relay') or its DataChannel ('p2p').
  function sendCtl(id, obj, via = 'relay') {
    try {
      if (via === 'p2p') {
        const l = links.get(id);
        if (l && l.dc && l.dc.readyState === 'open') l.dc.send(ctlFrame(id, obj));
        else return false;
      } else if (wsUp()) {
        ws.send(ctlFrame(id, obj));
      } else {
        return false;
      }
      return true;
    } catch {
      return false;
    }
  }
  function retryDelay(l) {
    return Math.min(P2P_RETRY_MAX_MS, P2P_RETRY_MIN_MS * 2 ** Math.min(l.retries, 10));
  }
  function teardown(l) {
    const { pc, dc } = l;
    l.pc = null;
    l.dc = null;
    l.state = 'relay';
    l.remoteSet = false;
    l.pendingIce = [];
    l.discAt = 0;
    l.healthy = true;
    l.rtt.p2p.pending.clear();
    l.rtt.p2p.lostRun = 0;
    try {
      if (dc) dc.close();
    } catch {}
    try {
      if (pc) pc.close();
    } catch {}
  }
  function fail(l, why, tellPeer = true) {
    const was = l.state;
    if (tellPeer && l.sess) sendCtl(l.id, { t: 'bye', s: l.sess, why });
    teardown(l);
    l.why = why;
    l.retryAt = now() + retryDelay(l);
    l.retries++;
    if (was === 'open') {
      log(`[net] peer ${l.id}: direct path lost (${why}); datagrams go through the relay`);
    } else if (l.retries <= 3 || l.retries % 10 === 0) {
      log(`[net] peer ${l.id}: direct path not established (${why}); using the relay, retry in ${Math.round((l.retryAt - now()) / 1000)} s`);
    }
  }
  function newPc(l, sess) {
    teardown(l);
    const pc = new RTCPeerConnection({ iceServers });
    l.pc = pc;
    l.sess = sess;
    l.state = 'connecting';
    l.startAt = now();
    // Local candidates are held back until releaseIce(): the offerer releases its own only once
    // the answer is applied, so both sides start their connectivity checks within a few ms of each
    // other. With a host firewall that drops unsolicited inbound UDP (Windows: Chrome has only an
    // mDNS inbound rule) only such a simultaneous open gets through.
    pc.iceOut = [];
    pc.iceReleased = false;
    pc.onicecandidate = (e) => {
      if (l.pc !== pc || !e.candidate) return;
      if (pc.iceReleased) sendCtl(l.id, { t: 'ice', s: sess, c: e.candidate.toJSON() });
      else pc.iceOut.push(e.candidate.toJSON());
    };
    pc.onconnectionstatechange = () => {
      if (l.pc !== pc) return;
      const st = pc.connectionState;
      if (debug) {
        log(`[net] p2p peer ${l.id}: connection ${st} at ${Math.round(now() - l.startAt)} ms`);
        pc.getStats().then((s) => s.forEach((r) => {
          if (r.type === 'candidate-pair' || r.type === 'remote-candidate' || r.type === 'local-candidate') {
            log(`[net] p2p peer ${l.id}:   ${JSON.stringify(r)}`);
          }
        }));
      }
      if (st === 'failed' || st === 'closed') fail(l, `connection ${st}`);
      else if (st === 'disconnected') l.discAt = l.discAt || now();
      else l.discAt = 0;
    };
    // Both sides create the same negotiated channel: no ondatachannel round trip.
    const dc = pc.createDataChannel('rex-net', { negotiated: true, id: 0, ordered: false, maxRetransmits: 0 });
    dc.binaryType = 'arraybuffer';
    l.dc = dc;
    dc.onopen = () => {
      if (l.dc !== dc) return;
      l.state = 'open';
      l.openMs = Math.round(now() - l.startAt);
      l.retries = 0;
      l.why = '';
      l.maxMsg = (pc.sctp && pc.sctp.maxMessageSize) || 65536;
      probe(l);
      describePath(l, pc).then(() => {
        log(`[net] peer ${l.id} (10.80.0.${l.id}): direct WebRTC path open after ${l.openMs} ms (${l.cand || '?'}); unicast and broadcast datagrams to it skip the relay`);
      });
    };
    dc.onclose = () => {
      if (l.dc === dc) fail(l, 'channel closed');
    };
    dc.onmessage = (e) => {
      if (l.dc !== dc || typeof e.data === 'string') return;
      const b = new Uint8Array(e.data);
      if (b.length < 10) return;
      // src is the channel's peer, whatever the frame says (the relay does the same).
      b[0] = (l.id >>> 24) & 255;
      b[1] = (l.id >>> 16) & 255;
      b[2] = (l.id >>> 8) & 255;
      b[3] = l.id & 255;
      if (isCtl(b)) {
        onCtl(l.id, b, 'p2p');
        return;
      }
      l.rx.p2p++;
      push(b);
    };
    // Candidates that arrived before this session existed on our side.
    const early = l.earlyIce.filter((x) => x.s === sess).map((x) => x.c);
    l.earlyIce = [];
    l.pendingIce.push(...early);
    return pc;
  }
  async function describePath(l, pc) {
    try {
      const st = await pc.getStats();
      let pair = null;
      st.forEach((r) => {
        if (r.type === 'transport' && r.selectedCandidatePairId) pair = st.get(r.selectedCandidatePairId);
      });
      if (!pair) st.forEach((r) => {
        if (!pair && r.type === 'candidate-pair' && r.state === 'succeeded' && r.nominated) pair = r;
      });
      if (!pair) return;
      const lc = st.get(pair.localCandidateId), rc = st.get(pair.remoteCandidateId);
      l.cand = `${lc ? lc.candidateType : '?'}/${rc ? rc.candidateType : '?'} ${lc ? lc.protocol || '' : ''}`.trim();
    } catch {}
  }
  async function flushIce(l, pc) {
    const list = l.pendingIce;
    l.pendingIce = [];
    for (const c of list) await pc.addIceCandidate(c).catch(() => {});
  }
  function releaseIce(l, pc) {
    if (pc.iceReleased) return;
    pc.iceReleased = true;
    for (const c of pc.iceOut) sendCtl(l.id, { t: 'ice', s: l.sess, c });
    pc.iceOut = [];
  }
  // Descriptions go out without candidates (they follow as 'ice' messages, see newPc).
  const noCandidates = (sdp) => sdp.split(/\r?\n/).filter((x) => !x.startsWith('a=candidate:') && x !== 'a=end-of-candidates').join('\r\n');
  async function startOffer(l) {
    const sess = (Math.random() * 0x7fffffff) >>> 0 || 1;
    const pc = newPc(l, sess);
    try {
      const offer = await pc.createOffer();
      if (l.pc !== pc) return;
      await pc.setLocalDescription(offer);
      if (l.pc !== pc) return;
      if (!sendCtl(l.id, { t: 'offer', s: sess, sdp: noCandidates(pc.localDescription.sdp) })) fail(l, 'relay down', false);
    } catch (e) {
      if (l.pc === pc) fail(l, `offer: ${e}`);
    }
  }
  async function onOffer(l, m) {
    if (!p2pOn()) {
      sendCtl(l.id, { t: 'bye', s: m.s, why: 'p2p off' });
      return;
    }
    const pc = newPc(l, m.s);
    try {
      await pc.setRemoteDescription({ type: 'offer', sdp: m.sdp });
      if (l.pc !== pc) return;
      const answer = await pc.createAnswer();
      if (l.pc !== pc) return;
      await pc.setLocalDescription(answer);
      if (l.pc !== pc) return;
      sendCtl(l.id, { t: 'answer', s: m.s, sdp: noCandidates(pc.localDescription.sdp) });
      releaseIce(l, pc);
      l.remoteSet = true;
      await flushIce(l, pc);
    } catch (e) {
      if (l.pc === pc) fail(l, `answer: ${e}`);
    }
  }
  // The lower id offers once both sides said hello.
  function maybeStart(l) {
    if (!p2pOn() || !l.capable || l.pc || !lastId || lastId >= l.id || !wsUp()) return;
    if (now() < l.retryAt) return;
    startOffer(l);
  }
  function sayHello(l, ack = false) {
    if (!p2pOn()) return;
    if (sendCtl(l.id, { t: 'hello', v: 1, ack: ack ? 1 : 0 })) {
      l.helloAt = now();
      l.hellos++;
    }
  }
  function recordRtt(r, ms) {
    r.recv++;
    r.last = ms;
    r.lostRun = 0;
    if (r.samples.length >= RTT_SAMPLES) r.samples.shift();
    r.samples.push(ms);
  }
  function probe(l) {
    const t = now();
    for (const via of ['relay', 'p2p']) {
      if (via === 'p2p' && !(l.dc && l.state === 'open')) continue;
      const r = l.rtt[via];
      const n = ++r.seq;
      if (sendCtl(l.id, { t: 'pq', n, ts: t }, via)) {
        r.sent++;
        r.pending.set(n, t);
      }
    }
  }
  function onCtl(src, b, via) {
    let m;
    try {
      m = JSON.parse(dec.decode(b.subarray(14)));
    } catch {
      return;
    }
    if (!m || typeof m !== 'object' || !src || src === lastId) return;
    if (debug && m.t !== 'pq' && m.t !== 'pr') log(`[net] p2p <- ${src} via ${via}: ${m.t} s=${m.s ?? ''} ${m.c ? m.c.candidate : ''}`);
    const l = link(src);
    switch (m.t) {
      case 'pq': // probe: answer on the path it came
        sendCtl(src, { t: 'pr', n: m.n, ts: m.ts }, via);
        return;
      case 'pr': {
        const r = l.rtt[via];
        const sentAt = r.pending.get(m.n);
        if (sentAt === undefined) return;
        r.pending.delete(m.n);
        recordRtt(r, now() - sentAt);
        if (via === 'p2p' && !l.healthy) {
          l.healthy = true;
          log(`[net] peer ${src}: direct path answers again; back on it`);
        }
        return;
      }
      case 'hello':
        l.capable = true;
        if (!m.ack) {
          // A (re)announced peer: retry at once instead of waiting out the backoff.
          if (!l.pc) l.retryAt = 0;
          sayHello(l, true);
        }
        maybeStart(l);
        return;
      case 'offer':
        l.capable = true;
        if (typeof m.sdp === 'string') onOffer(l, m);
        return;
      case 'answer':
        if (l.pc && m.s === l.sess && !l.remoteSet && typeof m.sdp === 'string') {
          const pc = l.pc;
          pc.setRemoteDescription({ type: 'answer', sdp: m.sdp }).then(() => {
            if (l.pc !== pc) return;
            l.remoteSet = true;
            releaseIce(l, pc);
            return flushIce(l, pc);
          }).catch((e) => {
            if (l.pc === pc) fail(l, `answer rejected: ${e}`);
          });
        }
        return;
      case 'ice':
        if (!m.c) return;
        if (l.pc && m.s === l.sess) {
          if (l.remoteSet) l.pc.addIceCandidate(m.c).catch(() => {});
          else l.pendingIce.push(m.c);
        } else if (l.earlyIce.length < 64) {
          l.earlyIce.push({ s: m.s, c: m.c });
        }
        return;
      case 'bye':
        if (l.pc && m.s === l.sess) fail(l, `peer: ${m.why || 'bye'}`, false);
        if (m.why === 'p2p off') l.retryAt = now() + P2P_RETRY_MAX_MS;
        return;
      default:
    }
  }
  function dropLink(id) {
    const l = links.get(id);
    if (!l) return;
    teardown(l);
    links.delete(id);
  }
  function syncLinks() {
    if (!p2pOn()) {
      for (const l of links.values()) if (l.pc) fail(l, 'p2p off');
      return;
    }
    for (const id of links.keys()) if (!peers.has(id)) dropLink(id);
    for (const id of peers) {
      const l = link(id);
      if (!l.capable && l.hellos === 0) sayHello(l);
      maybeStart(l);
    }
  }
  const pct = (sorted, p) => (sorted.length ? sorted[Math.min(sorted.length - 1, Math.floor(p * sorted.length))] : 0);
  const r2 = (x) => Math.round(x * 100) / 100;
  function rttSummary(r) {
    const s = [...r.samples].sort((a, b) => a - b);
    return {
      sent: r.sent, recv: r.recv, lost: r.lost, last: r2(r.last),
      min: r2(s[0] || 0), p50: r2(pct(s, 0.5)), p95: r2(pct(s, 0.95)), p99: r2(pct(s, 0.99)), max: r2(s[s.length - 1] || 0),
    };
  }
  function linkStats(l) {
    return {
      id: l.id, state: l.state, path: usable(l) ? 'p2p' : 'relay', capable: l.capable, cand: l.cand,
      openMs: l.openMs, retries: l.retries, why: l.why, healthy: l.healthy,
      tx: { ...l.tx }, rx: { ...l.rx }, rtt: { relay: rttSummary(l.rtt.relay), p2p: rttSummary(l.rtt.p2p) },
    };
  }
  let lastProbeAt = 0, lastSummaryAt = 0;
  function p2pTick() {
    const t = now();
    const doProbe = t - lastProbeAt >= probeMs;
    if (doProbe) lastProbeAt = t;
    for (const l of links.values()) {
      for (const via of ['relay', 'p2p']) {
        const r = l.rtt[via];
        for (const [n, at] of r.pending) {
          if (t - at < PROBE_LOST_MS) continue;
          r.pending.delete(n);
          r.lost++;
          r.lostRun++;
        }
      }
      if (l.state === 'open' && l.healthy && l.rtt.p2p.lostRun >= PROBE_UNHEALTHY && l.rtt.relay.lostRun === 0) {
        l.healthy = false;
        log(`[net] peer ${l.id}: direct path stopped answering (${l.rtt.p2p.lostRun} probes lost); datagrams go through the relay`);
      }
      if (l.state === 'connecting' && t - l.startAt > P2P_OPEN_TIMEOUT_MS) fail(l, `not open after ${P2P_OPEN_TIMEOUT_MS} ms`);
      if (l.pc && l.discAt && t - l.discAt > P2P_DISCONNECTED_MS) fail(l, 'ICE disconnected');
      if (!p2pOn()) continue;
      if (!l.capable && l.hellos < 3 && t - l.helloAt > 5000 && peers.has(l.id)) sayHello(l);
      maybeStart(l);
      if (doProbe) probe(l);
    }
    if (statsLogMs > 0 && links.size && t - lastSummaryAt >= statsLogMs) {
      lastSummaryAt = t;
      const f = (r) => `p50 ${r.p50} p95 ${r.p95} ms (n ${r.recv}, lost ${r.lost})`;
      for (const l of links.values()) {
        const s = linkStats(l);
        log(`[net] p2p peer ${l.id}: path ${s.path}${s.cand ? ` (${s.cand})` : ''}, state ${s.state}; rtt p2p ${f(s.rtt.p2p)} | relay ${f(s.rtt.relay)}; tx p2p ${s.tx.p2p} relay ${s.tx.relay}, rx p2p ${s.rx.p2p} relay ${s.rx.relay}`);
      }
    }
  }
  let p2pTimer = 0;
  const notePeer = (type, id) => {
    self.events.push(`${type} ${id}`);
    if (self.events.length > 64) self.events.splice(0, self.events.length - 64);
  };
  const resumeUrl = () => {
    if (!token && !lastId) return self.url;
    try {
      const u = new URL(self.url);
      if (token) u.searchParams.set('resume', token);
      if (lastId) u.searchParams.set('id', String(lastId));
      return u.toString();
    } catch {
      return self.url;
    }
  };
  const onOnline = () => {
    if (retryTimer && !closing) {
      clearTimeout(retryTimer);
      retryTimer = 0;
      log('[net] browser back online: reconnecting to the relay now');
      open();
    }
  };
  if (typeof addEventListener === 'function') addEventListener('online', onOnline);

  function scheduleReconnect(why) {
    if (closing || retryTimer) return;
    if (!downSince) downSince = now();
    const base = Math.min(BACKOFF_MAX_MS, BACKOFF_MIN_MS * 2 ** Math.min(attempt, 8));
    const delay = Math.round(base * (0.75 + Math.random() * 0.5));
    attempt++;
    const offline = typeof navigator !== 'undefined' && navigator.onLine === false;
    log(`[net] relay link down (${why}); reconnect #${attempt} in ${delay} ms` +
        `${offline ? ' (browser offline: or as soon as it is back online)' : ''}` +
        `${lastId ? `, resuming 10.80.0.${lastId}` : ''}`);
    setState(1);
    retryTimer = setTimeout(() => {
      retryTimer = 0;
      open();
    }, delay);
  }

  function pingTick() {
    if (!ws || ws.readyState !== 1 || Atomics.load(ctl, STATE) !== 2) return;
    const t = now();
    // Timers are throttled in hidden tabs (down to once a minute), so judge by the last ping
    // sent rather than by wall time since the last message.
    if (pingSentAt && lastRxAt < pingSentAt && t - pingSentAt > PING_TIMEOUT_MS) {
      log(`[net] relay ${self.url}: no answer for ${Math.round((t - pingSentAt) / 1000)} s; dropping the socket`);
      const dead = ws;
      ws = null;
      try {
        dead.close(4000, 'ping timeout'); // not 1000: the relay keeps our id for the grace period
      } catch {}
      Atomics.store(ctl, CLOSE_CODE, 4000);
      scheduleReconnect('ping timeout');
      return;
    }
    try {
      ws.send(JSON.stringify({ type: 'ping', t: Math.round(t) }));
      pingSentAt = t;
    } catch {}
  }

  function onWelcome(j) {
    const prevPeers = peers;
    const reconnect = everUp;
    peers = new Set(j.peers);
    token = typeof j.resume === 'string' ? j.resume : '';
    if (lastId && j.id !== lastId) {
      log(`[net] relay gave a new id ${j.id} (was ${lastId}): this tab is now 10.80.0.${j.id}`);
      // The others know our channels under the old id: start over.
      for (const id of [...links.keys()]) dropLink(id);
    }
    const allows = j.p2p !== false;
    if (allows !== p2pRelayAllows) {
      p2pRelayAllows = allows;
      if (!allows) log('[net] the relay asks for no peer-to-peer path: every datagram goes through the relay');
    }
    if (!pageIce && Array.isArray(j.iceServers)) iceServers = j.iceServers;
    lastId = j.id;
    attempt = 0;
    Atomics.store(ctl, ID, j.id);
    Atomics.store(ctl, PEERS, peers.size);
    if (reconnect) {
      Atomics.add(ctl, RECONNECTS, 1);
      // Others that came or went while we were away (a resume hides only our own gap).
      for (const id of prevPeers) if (!peers.has(id)) notePeer('leave', id);
      for (const id of peers) if (!prevPeers.has(id)) notePeer('join', id);
      const gap = downSince ? Math.round(now() - downSince) : 0;
      log(`[net] relay ${self.url}: ${j.resumed ? 'resumed' : 'reconnected'} as id ${j.id} = 10.80.0.${j.id} after ${gap} ms, peers [${j.peers}]`);
    } else {
      log(`[net] relay ${self.url}: id ${j.id} = 10.80.0.${j.id}, room ${j.room}, peers [${j.peers}]`);
    }
    everUp = true;
    downSince = 0;
    setState(2);
    if (!reconnect && p2pOn()) log(`[net] peer-to-peer path on (WebRTC DataChannels, ${iceServers.length} ICE server(s)); relay fallback per peer`);
    syncLinks();
  }

  function open() {
    if (closing) return;
    const url = resumeUrl();
    let sock;
    try {
      sock = new WebSocket(url);
    } catch (e) {
      log(`[net] relay ${self.url}: WebSocket failed: ${e}`);
      // A malformed URL or blocked scheme (SyntaxError/SecurityError) at the first connect
      // cannot be fixed by retrying.
      if (!everUp && e && (e.name === 'SecurityError' || e.name === 'SyntaxError')) setState(3);
      else scheduleReconnect(String(e));
      return;
    }
    ws = sock;
    sock.binaryType = 'arraybuffer';
    sock.onmessage = (e) => {
      if (sock !== ws) return;
      lastRxAt = now();
      if (typeof e.data !== 'string') {
        const b = new Uint8Array(e.data);
        const src = b.length >= 4 ? ((b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]) >>> 0 : 0;
        if (isCtl(b)) {
          onCtl(src, b, 'relay');
          return;
        }
        const l = links.get(src);
        if (l) l.rx.relay++;
        push(b);
        return;
      }
      let j;
      try {
        j = JSON.parse(e.data);
      } catch {
        return;
      }
      if (j.type === 'welcome') {
        onWelcome(j);
      } else if (j.type === 'join' || j.type === 'leave') {
        if (j.type === 'join') peers.add(j.id);
        else peers.delete(j.id);
        Atomics.store(ctl, PEERS, peers.size);
        notePeer(j.type, j.id);
        log(`[net] peer ${j.id} (10.80.0.${j.id}) ${j.type === 'join' ? 'joined' : 'left'}; peers [${[...peers]}]`);
        wake();
        if (j.type === 'leave') dropLink(j.id);
        else syncLinks();
      }
    };
    const ended = (code, reason) => {
      if (sock !== ws) return; // an older socket we already gave up on
      ws = null;
      Atomics.store(ctl, CLOSE_CODE, code);
      if (closing) {
        log(`[net] relay ${self.url} closed (code ${code})`);
        setState(3);
        return;
      }
      scheduleReconnect(`closed, code ${code}${reason ? ` ${reason}` : ''}`);
    };
    sock.onclose = (e) => ended(e.code, e.reason);
    // Browsers fire close after a failed connect; some WebSocket implementations (node's
    // undici) only fire error and leave the socket CONNECTING, which would stall the retries.
    sock.onerror = () => {
      if (sock.readyState !== 1) {
        ended(1006, 'connect failed');
        try {
          sock.close();
        } catch {}
      }
    };
  }

  const self = {
    url: '',
    events: [],
    connect(url, room) {
      self.url = resolveUrl(url, room);
      closing = false;
      setState(1);
      if (!pingTimer) pingTimer = setInterval(pingTick, PING_INTERVAL_MS);
      if (!p2pTimer && haveRtc) p2pTimer = setInterval(p2pTick, 250);
      open();
    },
    // ptr/len: one complete relay frame in wasm memory (copied here; the caller frees it).
    // The copy is a plain ArrayBuffer (DataChannel.send refuses SharedArrayBuffer views).
    send(ptr, len) {
      views();
      const frame = u8.slice(ptr, ptr + len);
      if (links.size && len >= 10) {
        const dst = ((frame[4] << 24) | (frame[5] << 16) | (frame[6] << 8) | frame[7]) >>> 0;
        if (dst === BROADCAST) {
          let direct = 0;
          for (const l of links.values()) if (usable(l) && len <= l.maxMsg) direct++;
          if (direct) {
            // Fan out: channel where open, relay unicast to the rest.
            let sent = 0;
            for (const id of peers) {
              const l = links.get(id);
              if (usable(l) && len <= l.maxMsg) {
                try {
                  l.dc.send(frame);
                  l.tx.p2p++;
                  sent++;
                  continue;
                } catch {}
              }
              if (wsUp()) {
                const c = frame.slice();
                new DataView(c.buffer).setUint32(4, id);
                ws.send(c);
                if (l) l.tx.relay++;
                sent++;
              }
            }
            Atomics.add(ctl, sent ? TX_FRAMES : TX_DROPPED, 1);
            return;
          }
        } else {
          const l = links.get(dst);
          if (usable(l) && len <= l.maxMsg) {
            try {
              l.dc.send(frame);
              l.tx.p2p++;
              Atomics.add(ctl, TX_FRAMES, 1);
              return;
            } catch {}
          }
          if (l && wsUp()) l.tx.relay++;
        }
      }
      if (!wsUp()) {
        Atomics.add(ctl, TX_DROPPED, 1);
        return;
      }
      ws.send(frame);
      Atomics.add(ctl, TX_FRAMES, 1);
    },
    // Turn the peer-to-peer path on or off at run time (off: channels are closed, all datagrams
    // go through the relay).
    setP2p(on) {
      p2pWanted = !!on;
      if (p2pWanted) {
        for (const l of links.values()) {
          l.retryAt = 0;
          l.retries = 0;
        }
        if (wsUp()) for (const l of links.values()) sayHello(l);
      }
      syncLinks();
      return p2pOn();
    },
    close() {
      closing = true;
      if (retryTimer) clearTimeout(retryTimer);
      retryTimer = 0;
      if (pingTimer) clearInterval(pingTimer);
      pingTimer = 0;
      if (p2pTimer) clearInterval(p2pTimer);
      p2pTimer = 0;
      for (const id of [...links.keys()]) dropLink(id);
      if (ws) ws.close(1000); // 1000: the relay frees the id at once (no resume grace)
      else setState(3);
    },
    // Testing: drop the socket as a network failure would (no close frame the relay can tell
    // from a dead link: code 4000), then reconnect with resume like after a real outage.
    simulateDrop() {
      if (!ws) return false;
      const dead = ws;
      ws = null;
      try {
        dead.close(4000, 'simulated drop');
      } catch {}
      Atomics.store(ctl, CLOSE_CODE, 4000);
      scheduleReconnect('simulated drop');
      return true;
    },
    stats() {
      views();
      return {
        url: self.url,
        state: ['idle', 'connecting', 'up', 'down'][Atomics.load(ctl, STATE)] ?? '?',
        id: Atomics.load(ctl, ID),
        reconnects: Atomics.load(ctl, RECONNECTS),
        reconnectAttempt: attempt,
        downMs: downSince ? Math.round(now() - downSince) : 0,
        closeCode: Atomics.load(ctl, CLOSE_CODE),
        peers: [...peers],
        rxFrames: Atomics.load(ctl, RX_FRAMES),
        txFrames: Atomics.load(ctl, TX_FRAMES),
        txDropped: Atomics.load(ctl, TX_DROPPED),
        ringDropped: Atomics.load(ctl, RING_DROPPED),
        ringUsed: ((Atomics.load(ctl, HEAD) >>> 0) - (Atomics.load(ctl, TAIL) >>> 0)) >>> 0,
        events: self.events.slice(-16),
        p2p: {
          on: p2pOn(), wanted: p2pWanted, relayAllows: p2pRelayAllows, webrtc: haveRtc,
          iceServers: iceServers.length, probeMs,
          links: [...links.values()].map(linkStats),
        },
      };
    },
  };
  return self;
})
