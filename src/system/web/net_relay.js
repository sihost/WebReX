// ReXGlue web port (Workstream G): browser main-thread side of the /net relay shim
// (src/system/web/net_relay.cpp, protocol: server/README.md). Evaluated once on the main
// thread; the expression evaluates to a factory that returns the Module.rexNet object.
//
// Inbound binary frames are copied into a single-producer ring in wasm memory:
//   ctl (Int32Array, 16 words): see the Ctl enum in net_relay.cpp
//   record: u32 length (little-endian) + frame bytes, padded to 4; u32 0xFFFFFFFF = wrap
// The producer (this thread) only uses Atomics and never blocks; consumers wait on SEQ.
(function rexNetRelayFactory(env) {
  const HEAD = 0, TAIL = 1, SEQ = 2, STATE = 3, ID = 4, PEERS = 5, RING_DROPPED = 6;
  const RX_FRAMES = 7, TX_FRAMES = 8, CLOSE_CODE = 9, TX_DROPPED = 10;
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
  const self = {
    url: '',
    events: [],
    connect(url, room) {
      self.url = resolveUrl(url, room);
      setState(1);
      try {
        ws = new WebSocket(self.url);
      } catch (e) {
        log(`[net] relay ${self.url}: WebSocket failed: ${e}`);
        setState(3);
        return;
      }
      ws.binaryType = 'arraybuffer';
      ws.onmessage = (e) => {
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
          peers = new Set(j.peers);
          Atomics.store(ctl, ID, j.id);
          Atomics.store(ctl, PEERS, peers.size);
          log(`[net] relay ${self.url}: id ${j.id} = 10.80.0.${j.id}, room ${j.room}, peers [${j.peers}]`);
          setState(2);
        } else if (j.type === 'join' || j.type === 'leave') {
          if (j.type === 'join') peers.add(j.id);
          else peers.delete(j.id);
          Atomics.store(ctl, PEERS, peers.size);
          self.events.push(`${j.type} ${j.id}`);
          log(`[net] peer ${j.id} (10.80.0.${j.id}) ${j.type === 'join' ? 'joined' : 'left'}; peers [${[...peers]}]`);
          wake();
        }
      };
      ws.onclose = (e) => {
        log(`[net] relay ${self.url} closed (code ${e.code})`);
        Atomics.store(ctl, CLOSE_CODE, e.code);
        setState(3);
      };
      ws.onerror = () => {};
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
      if (ws) ws.close(1000);
    },
    stats() {
      views();
      return {
        url: self.url,
        state: ['idle', 'connecting', 'up', 'down'][Atomics.load(ctl, STATE)] ?? '?',
        id: Atomics.load(ctl, ID),
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
