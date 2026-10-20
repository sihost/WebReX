// node --test tools/web_relay/test/
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import net from 'node:net';
import { after, before, describe, test } from 'node:test';
import { BROADCAST, HEADER_SIZE, MAX_PAYLOAD, createRelayServer } from '../lib/relay.mjs';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function start(options = {}) {
  const lines = [];
  const { server, relay, shutdown } = createRelayServer({ log: (l) => lines.push(l), ...options });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  const port = server.address().port;
  return { port, relay, lines, close: shutdown };
}

/** Browser-like client (Node 22 global WebSocket; answers pings itself) with a message queue. */
function connect(port, room, headers) {
  return new Promise((resolve, reject) => {
    // room: a room name, or a raw query starting with '?' (e.g. '?room=r&pass=x').
    const q = room === undefined ? '' : room.startsWith('?') ? room : `?room=${encodeURIComponent(room)}`;
    const ws = new WebSocket(`ws://127.0.0.1:${port}/net${q}`, headers ? { headers } : undefined);
    ws.binaryType = 'arraybuffer';
    const queue = [];
    const waiters = [];
    ws.closeCode = null;
    ws.addEventListener('message', (e) => {
      const m = typeof e.data === 'string' ? JSON.parse(e.data) : Buffer.from(e.data);
      const w = waiters.shift();
      if (w) w(m);
      else queue.push(m);
    });
    ws.addEventListener('close', (e) => { ws.closeCode = e.code; });
    ws.next = (ms = 3000) =>
      queue.length
        ? Promise.resolve(queue.shift())
        : new Promise((res, rej) => {
            const w = (m) => { clearTimeout(t); res(m); };
            const t = setTimeout(() => { waiters.splice(waiters.indexOf(w), 1); rej(new Error('timeout waiting for message')); }, ms);
            waiters.push(w);
          });
    ws.addEventListener('open', () => resolve(ws), { once: true });
    ws.addEventListener('error', () => reject(new Error('ws error')), { once: true });
  });
}

/**
 * Raw RFC 6455 client over a TCP socket: sees control frames (server pings) and never answers
 * them unless told to. frames: [{op, payload}] in arrival order.
 */
function rawConnect(port, path, extraHeaders = '') {
  return new Promise((resolve, reject) => {
    const s = net.connect(port, '127.0.0.1');
    const key = crypto.randomBytes(16).toString('base64');
    let buf = Buffer.alloc(0);
    let upgraded = false;
    const frames = [];
    s.frames = frames;
    s.sendFrame = (op, payload) => {
      const mask = crypto.randomBytes(4);
      const len = payload.length;
      const hdr = len < 126 ? Buffer.from([0x80 | op, 0x80 | len])
        : len < 65536 ? Buffer.from([0x80 | op, 0x80 | 126, len >> 8, len & 255])
        : (() => { const h = Buffer.alloc(10); h[0] = 0x80 | op; h[1] = 0x80 | 127; h.writeBigUInt64BE(BigInt(len), 2); return h; })();
      const body = Buffer.from(payload);
      for (let i = 0; i < len; i++) body[i] ^= mask[i & 3];
      s.write(Buffer.concat([hdr, mask, body]));
    };
    s.on('connect', () => s.write(`GET ${path} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n` +
      `Sec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n${extraHeaders}\r\n`));
    s.on('data', (d) => {
      buf = Buffer.concat([buf, d]);
      if (!upgraded) {
        const end = buf.indexOf('\r\n\r\n');
        if (end < 0) return;
        const head = buf.subarray(0, end).toString();
        buf = buf.subarray(end + 4);
        if (!head.startsWith('HTTP/1.1 101')) return reject(new Error(head.split('\r\n')[0]));
        upgraded = true;
        resolve(s);
      }
      while (buf.length >= 2) {
        let len = buf[1] & 0x7f;
        let off = 2;
        if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); off = 4; }
        else if (len === 127) { if (buf.length < 10) return; len = Number(buf.readBigUInt64BE(2)); off = 10; }
        if (buf.length < off + len) return;
        frames.push({ op: buf[0] & 0x0f, payload: Buffer.from(buf.subarray(off, off + len)) });
        buf = buf.subarray(off + len);
      }
    });
    s.on('error', reject);
    s.whenClosed = new Promise((r) => s.on('close', r));
  });
}

function datagram(dst, port, payload) {
  const b = Buffer.alloc(HEADER_SIZE + payload.length);
  b.writeUInt32BE(0xdeadbeef, 0); // bogus src: the relay must overwrite it
  b.writeUInt32BE(dst, 4);
  b.writeUInt16BE(port, 8);
  payload.copy(b, HEADER_SIZE);
  return b;
}

const get = async (port, path) => {
  const r = await fetch(`http://127.0.0.1:${port}${path}`);
  return { status: r.status, headers: r.headers, body: await r.text() };
};

describe('web relay', () => {
  let srv;
  before(async () => { srv = await start(); });
  after(async () => { await srv.close(); });

  test('welcome/join, unicast with src rewrite, echo, broadcast, room isolation, 64 KiB, leave', async () => {
    const a = await connect(srv.port, 'lan1');
    const wa = await a.next();
    assert.match(wa.resume, /^[0-9a-f]{32}$/);
    assert.deepEqual(wa, { type: 'welcome', id: 1, room: 'lan1', peers: [], maxPayload: MAX_PAYLOAD, resume: wa.resume, resumeGraceMs: 30000, resumed: false, p2p: true });
    const b = await connect(srv.port, 'lan1');
    const wb = await b.next();
    assert.deepEqual([wb.type, wb.id, wb.peers, wb.resumed], ['welcome', 2, [1], false]);
    assert.notEqual(wb.resume, wa.resume);
    assert.deepEqual(await a.next(), { type: 'join', id: 2 });

    a.send(datagram(2, 1001, Buffer.from('hello')));
    const atB = await b.next();
    assert.equal(atB.readUInt32BE(0), 1);
    assert.equal(atB.readUInt32BE(4), 2);
    assert.equal(atB.readUInt16BE(8), 1001);
    assert.equal(atB.subarray(HEADER_SIZE).toString(), 'hello');
    b.send(datagram(atB.readUInt32BE(0), atB.readUInt16BE(8), atB.subarray(HEADER_SIZE)));
    const atA = await a.next();
    assert.equal(atA.readUInt32BE(0), 2);
    assert.equal(atA.subarray(HEADER_SIZE).toString(), 'hello');

    const c = await connect(srv.port, 'other');
    assert.equal((await c.next()).id, 1);
    a.send(datagram(BROADCAST, 1002, Buffer.from('disc')));
    const bc = await b.next();
    assert.equal(bc.readUInt32BE(4), BROADCAST);
    assert.equal(bc.subarray(HEADER_SIZE).toString(), 'disc');
    await assert.rejects(c.next(300), /timeout/);
    await assert.rejects(a.next(300), /timeout/);

    // Unknown destination and runt frames are dropped silently.
    a.send(datagram(77, 1, Buffer.from('x')));
    a.send(Buffer.alloc(HEADER_SIZE - 1));
    await assert.rejects(b.next(300), /timeout/);

    const big = Buffer.alloc(MAX_PAYLOAD, 7);
    b.send(datagram(1, 9, big));
    assert.deepEqual((await a.next()).subarray(HEADER_SIZE), big);

    b.close();
    assert.deepEqual(await a.next(), { type: 'leave', id: 2 });
    // Ids are reused lowest-first.
    const d = await connect(srv.port, 'lan1');
    assert.equal((await d.next()).id, 2);
    a.close(); c.close(); d.close();
  });

  test('net shim datagrams (u16 src port, proto, version prefix) pass through unchanged', async () => {
    const a = await connect(srv.port, 'shim');
    await a.next();
    const b = await connect(srv.port, 'shim');
    await b.next();
    await a.next();
    // src/system/web/net_relay.h: payload = u16 src_port, u8 proto (17 UDP, 254 VDP), u8 version 1, data.
    const shim = Buffer.concat([Buffer.from([0x0c, 0x02, 254, 1]), Buffer.from('vdp payload')]);
    a.send(datagram(BROADCAST, 3074, shim));
    const m = await b.next();
    assert.equal(m.readUInt16BE(8), 3074);
    assert.deepEqual(m.subarray(HEADER_SIZE), shim);
    a.close(); b.close();
  });

  test('text ping -> pong; other text ignored; oversize message closes with 1009', async () => {
    const a = await connect(srv.port, 'pp');
    await a.next();
    a.send(JSON.stringify({ type: 'ping', t: 42 }));
    assert.deepEqual(await a.next(), { type: 'pong', t: 42 });
    a.send('not json');
    a.send(JSON.stringify({ type: 'nope' }));
    await assert.rejects(a.next(200), /timeout/);
    a.send(Buffer.alloc(HEADER_SIZE + MAX_PAYLOAD + 1));
    for (let i = 0; i < 50 && a.closeCode === null; i++) await sleep(20);
    assert.equal(a.closeCode, 1009);
  });

  test('/net/status, /net/progress, /healthz, 426 on plain GET /net, 404 elsewhere', async () => {
    const st = (q) => get(srv.port, `/net/status${q}`).then((r) => JSON.parse(r.body));
    assert.deepEqual(await st('?room=st1'), { room: 'st1', peers: 0, maxPeers: 16, players: [], defaultRoom: 'default', rooms: [], locked: false });
    const a = await connect(srv.port, 'st1');
    await a.next();
    const b = await connect(srv.port, 'st1');
    await b.next();
    assert.deepEqual(await a.next(), { type: 'join', id: 2 });
    const c = await connect(srv.port, 'st2');
    await c.next();
    assert.equal((await st('?room=st1')).peers, 2);
    assert.equal((await st('')).room, 'default');
    assert.deepEqual((await st('?room=x')).rooms, [{ room: 'st1', peers: 2, locked: false }, { room: 'st2', peers: 1, locked: false }]);
    assert.equal((await get(srv.port, '/net/progress?room=st1&client=abc&name=Me&have=1&total=4')).status, 204);
    assert.equal((await get(srv.port, '/net/progress?room=st1&client=bad%20id')).status, 400);
    assert.deepEqual((await st('?room=st1')).players.map((p) => [p.client, p.have, p.total, p.ready]), [['abc', 1, 4, false]]);
    // Over the relay: stored with the peer id and forwarded to the others.
    b.send(JSON.stringify({ type: 'progress', client: 'def', name: 'Host<x>', have: 4, total: 4, ready: true }));
    assert.deepEqual(await a.next(), { type: 'progress', id: 2, client: 'def', name: 'Hostx', have: 4, total: 4, ready: true });
    assert.equal((await get(srv.port, '/net')).status, 426);
    assert.equal((await get(srv.port, '/elsewhere')).status, 404);
    const h = await get(srv.port, '/healthz');
    assert.equal(h.status, 200);
    assert.ok(JSON.parse(h.body).datagrams >= 0);
    a.close(); b.close(); c.close();
  });

  test('rejects bad room names and wrong paths', async () => {
    await assert.rejects(connect(srv.port, 'bad room/..'));
    await assert.rejects(rawConnect(srv.port, '/nope'), /404/);
  });
});

describe('web relay limits and options', () => {
  test('room full -> 503 at the handshake; room count limit -> 503', async () => {
    const s = await start({ maxPeersPerRoom: 2, maxRooms: 2 });
    try {
      const a = await connect(s.port, 'r1');
      const b = await connect(s.port, 'r1');
      await assert.rejects(rawConnect(s.port, '/net?room=r1'), /503/);
      const c = await connect(s.port, 'r2');
      await assert.rejects(rawConnect(s.port, '/net?room=r3'), /503/);
      a.close(); b.close(); c.close();
    } finally {
      await s.close();
    }
  });

  test('allowOrigins: listed origins and non-browser clients only', async () => {
    const s = await start({ allowOrigins: ['https://game.example'] });
    try {
      await assert.rejects(rawConnect(s.port, '/net?room=o', 'Origin: https://evil.example\r\n'), /403/);
      (await rawConnect(s.port, '/net?room=o', 'Origin: https://game.example\r\n')).destroy();
      (await rawConnect(s.port, '/net?room=o')).destroy();
      const r = await fetch(`http://127.0.0.1:${s.port}/net/status`, { headers: { origin: 'https://game.example' } });
      assert.equal(r.headers.get('access-control-allow-origin'), 'https://game.example');
      assert.equal(r.headers.get('cross-origin-resource-policy'), 'cross-origin');
    } finally {
      await s.close();
    }
  });

  test('defaultRoom; latency/jitter keep per-receiver order', async () => {
    const s = await start({ defaultRoom: 'play', latencyMs: 120, jitterMs: 80 });
    try {
      const a = await connect(s.port);
      assert.equal((await a.next()).room, 'play');
      const b = await connect(s.port, 'play');
      await b.next();
      await a.next();
      const t0 = Date.now();
      for (let i = 0; i < 8; i++) a.send(datagram(2, 1, Buffer.from(`m${i}`)));
      for (let i = 0; i < 8; i++) assert.equal((await b.next()).subarray(HEADER_SIZE).toString(), `m${i}`);
      assert.ok(Date.now() - t0 >= 120, `delivered after ${Date.now() - t0} ms`);
      a.close(); b.close();
    } finally {
      await s.close();
    }
  });

  test('keepalive: WebSocket pings reach every peer; a browser (auto pong) stays, a silent peer is closed', async () => {
    const s = await start({ keepaliveMs: 100, idleTimeoutMs: 450, resumeGraceMs: 300 });
    try {
      const browser = await connect(s.port, 'ka');
      await browser.next();
      const raw = await rawConnect(s.port, '/net?room=ka');
      await sleep(300);
      assert.ok(raw.frames.some((f) => f.op === 0x9), `server pings seen: ${raw.frames.map((f) => f.op)}`);
      // The raw client never answers: it is closed (1001) and, once its resume grace is over,
      // the browser sees it leave.
      await raw.whenClosed;
      const closeFrame = raw.frames.find((f) => f.op === 0x8);
      assert.equal(closeFrame?.payload.readUInt16BE(0), 1001);
      assert.deepEqual(await browser.next(2000), { type: 'join', id: 2 });
      assert.deepEqual(await browser.next(2000), { type: 'leave', id: 2 });
      // The browser-like client answered every ping and is still connected well past the timeout.
      await sleep(600);
      assert.equal(browser.readyState, WebSocket.OPEN);
      assert.equal(s.relay.peerCount('ka'), 1);
      assert.equal(s.relay.stats.idleClosed, 1);
      browser.close();
    } finally {
      await s.close();
    }
  });

  test('a raw client that answers pings is kept alive', async () => {
    const s = await start({ keepaliveMs: 80, idleTimeoutMs: 300 });
    try {
      const raw = await rawConnect(s.port, '/net?room=kp');
      let answered = 0;
      const timer = setInterval(() => {
        while (raw.frames.length) {
          const f = raw.frames.shift();
          if (f.op === 0x9) { raw.sendFrame(0xa, f.payload); answered++; }
        }
      }, 20);
      await sleep(700);
      clearInterval(timer);
      assert.ok(answered >= 4, `answered ${answered} pings`);
      assert.equal(s.relay.peerCount('kp'), 1);
      raw.destroy();
    } finally {
      await s.close();
    }
  });
});

/** Next text frame (parsed JSON) a raw client received, waiting up to ms. */
async function rawText(raw, ms = 2000) {
  const end = Date.now() + ms;
  while (Date.now() < end) {
    const i = raw.frames.findIndex((f) => f.op === 0x1);
    if (i >= 0) return JSON.parse(raw.frames.splice(i, 1)[0].payload.toString());
    await sleep(10);
  }
  throw new Error('timeout waiting for text frame');
}

describe('web relay resume', () => {
  test('connection lost: id kept, no leave/join; resume token gives the same id back', async () => {
    const s = await start({ resumeGraceMs: 2000 });
    try {
      const a = await connect(s.port, 'rs');
      await a.next();
      const raw = await rawConnect(s.port, '/net?room=rs');
      const w1 = await rawText(raw);
      assert.deepEqual([w1.id, w1.resumed, w1.resumeGraceMs], [2, false, 2000]);
      assert.deepEqual(await a.next(), { type: 'join', id: 2 });
      raw.destroy(); // TCP gone, no close frame
      await sleep(150);
      assert.equal(s.relay.peerCount('rs'), 1);
      // While it is away: datagrams to it are dropped and counted; a newcomer does not get its id
      // and sees it in the peer list.
      a.send(datagram(2, 1, Buffer.from('lost')));
      const c = await connect(s.port, 'rs');
      const wc = await c.next();
      assert.deepEqual([wc.id, wc.peers], [3, [1, 2]]);
      assert.deepEqual(await a.next(), { type: 'join', id: 3 });
      for (let i = 0; i < 50 && s.relay.stats.droppedResuming < 1; i++) await sleep(10);
      assert.equal(s.relay.stats.droppedResuming, 1);
      // Resume: same id, new token, current peers, nobody hears about it.
      const back = await rawConnect(s.port, `/net?room=rs&resume=${w1.resume}`);
      const w2 = await rawText(back);
      assert.deepEqual([w2.id, w2.resumed, w2.peers], [2, true, [1, 3]]);
      assert.notEqual(w2.resume, w1.resume);
      await assert.rejects(a.next(300), /timeout/);
      await assert.rejects(c.next(50), /timeout/);
      // Traffic flows to the resumed peer again.
      a.send(datagram(2, 7, Buffer.from('again')));
      for (let i = 0; i < 100 && !back.frames.some((f) => f.op === 0x2); i++) await sleep(10);
      const bin = back.frames.find((f) => f.op === 0x2);
      assert.equal(bin.payload.readUInt32BE(0), 1);
      assert.equal(bin.payload.subarray(HEADER_SIZE).toString(), 'again');
      // The old token is spent: using it again is a fresh join.
      const stale = await rawConnect(s.port, `/net?room=rs&resume=${w1.resume}`);
      const w3 = await rawText(stale);
      assert.deepEqual([w3.id, w3.resumed], [4, false]);
      assert.equal(s.relay.stats.resumed, 1);
      stale.destroy(); back.destroy(); a.close(); c.close();
    } finally {
      await s.close();
    }
  });

  test('grace expiry: leave broadcast, id freed, token refused', async () => {
    const s = await start({ resumeGraceMs: 250 });
    try {
      const a = await connect(s.port, 'gx');
      await a.next();
      const raw = await rawConnect(s.port, '/net?room=gx');
      const w = await rawText(raw);
      await a.next(); // join 2
      raw.destroy();
      await assert.rejects(a.next(150), /timeout/);
      assert.deepEqual(await a.next(1000), { type: 'leave', id: 2 });
      assert.equal(s.relay.stats.graceExpired, 1);
      const late = await rawConnect(s.port, `/net?room=gx&resume=${w.resume}`);
      const wl = await rawText(late);
      assert.deepEqual([wl.id, wl.resumed], [2, false]); // lowest free id, as a new peer
      assert.deepEqual(await a.next(), { type: 'join', id: 2 });
      late.destroy(); a.close();
    } finally {
      await s.close();
    }
  });

  test('takeover: resuming while the server still holds the old socket closes it with 4001', async () => {
    const s = await start();
    try {
      const a = await connect(s.port, 'tk');
      await a.next();
      const b = await connect(s.port, 'tk');
      const wb = await b.next();
      await a.next(); // join 2
      const b2 = await rawConnect(s.port, `/net?room=tk&resume=${wb.resume}`);
      const w = await rawText(b2);
      assert.deepEqual([w.id, w.resumed, w.peers], [2, true, [1]]);
      for (let i = 0; i < 100 && b.closeCode === null; i++) await sleep(10);
      assert.equal(b.closeCode, 4001);
      await assert.rejects(a.next(300), /timeout/);
      assert.equal(s.relay.peerCount('tk'), 2);
      assert.equal(s.relay.stats.takeovers, 1);
      b2.destroy(); a.close();
    } finally {
      await s.close();
    }
  });

  test('relay restarted (token unknown): &id= keeps the previous id when it is free', async () => {
    const s = await start();
    try {
      const a = await connect(s.port, 'rid');
      await a.next();
      const raw = await rawConnect(s.port, '/net?room=rid&resume=ffffffffffffffffffffffffffffffff&id=5');
      const w = await rawText(raw);
      assert.deepEqual([w.id, w.resumed], [5, false]);
      assert.deepEqual(await a.next(), { type: 'join', id: 5 });
      // Taken (live) -> lowest free; out of range -> lowest free.
      const dup = await rawConnect(s.port, '/net?room=rid&id=5');
      assert.equal((await rawText(dup)).id, 2);
      const big = await rawConnect(s.port, '/net?room=rid&id=300');
      assert.equal((await rawText(big)).id, 3);
      raw.destroy(); dup.destroy(); big.destroy(); a.close();
    } finally {
      await s.close();
    }
  });

  test('clean close (1000) leaves at once; unknown token is a fresh join; grace 0 disables', async () => {
    const s = await start({ resumeGraceMs: 0 });
    try {
      const a = await connect(s.port, 'cc');
      await a.next();
      const raw = await rawConnect(s.port, '/net?room=cc&resume=00000000000000000000000000000000');
      const w = await rawText(raw);
      assert.deepEqual([w.id, w.resumed, w.resumeGraceMs], [2, false, 0]);
      await a.next(); // join 2
      raw.destroy(); // grace 0: immediate leave even without a close frame
      assert.deepEqual(await a.next(), { type: 'leave', id: 2 });
    } finally {
      await s.close();
    }
    const s2 = await start({ resumeGraceMs: 5000 });
    try {
      const a = await connect(s2.port, 'cc');
      await a.next();
      const b = await connect(s2.port, 'cc');
      await b.next();
      await a.next(); // join 2
      b.close(1000);
      assert.deepEqual(await a.next(), { type: 'leave', id: 2 });
      a.close();
    } finally {
      await s2.close();
    }
  });
});
