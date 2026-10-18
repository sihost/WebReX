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
(function rexNetRelayFactory(env) {
  const HEAD = 0, TAIL = 1, SEQ = 2, STATE = 3, ID = 4, PEERS = 5, RING_DROPPED = 6;
  const RX_FRAMES = 7, TX_FRAMES = 8, CLOSE_CODE = 9, TX_DROPPED = 10, RECONNECTS = 11;
  const BACKOFF_MIN_MS = 500, BACKOFF_MAX_MS = 8000;
  const PING_INTERVAL_MS = 10000, PING_TIMEOUT_MS = 20000;
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
    }
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
        push(new Uint8Array(e.data));
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
      open();
    },
    // ptr/len: one complete relay frame in wasm memory (copied here; the caller frees it).
    send(ptr, len) {
      views();
      if (!ws || ws.readyState !== 1) {
        Atomics.add(ctl, TX_DROPPED, 1);
        return;
      }
      ws.send(u8.slice(ptr, ptr + len));
      Atomics.add(ctl, TX_FRAMES, 1);
    },
    close() {
      closing = true;
      if (retryTimer) clearTimeout(retryTimer);
      retryTimer = 0;
      if (pingTimer) clearInterval(pingTimer);
      pingTimer = 0;
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
      };
    },
  };
  return self;
})
