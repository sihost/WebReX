// node --test tools/web_relay/test/  (public-hosting limits: README "Hardening")
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import net from 'node:net';
import { describe, test } from 'node:test';
import { BROADCAST, HEADER_SIZE, TokenBucket, createRelayServer } from '../lib/relay.mjs';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function start(options = {}) {
  const lines = [];
  const { server, relay, shutdown } = createRelayServer({ log: (l) => lines.push(l), ...options });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  return { port: server.address().port, relay, lines, close: shutdown };
}

/** WebSocket client with a message queue; query is the raw query string ('?room=...'). */
function connect(port, query) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(`ws://127.0.0.1:${port}/net${query}`);
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
    ws.drain = async (ms = 300) => {
      const got = [];
      for (;;) {
        try { got.push(await ws.next(ms)); } catch { return got; }
      }
    };
    ws.addEventListener('open', () => resolve(ws), { once: true });
    ws.addEventListener('error', () => reject(new Error('ws error')), { once: true });
  });
}

/** HTTP status of a WebSocket handshake (101 = accepted; the socket is closed right after). */
function handshakeStatus(port, path) {
  return new Promise((resolve, reject) => {
    const s = net.connect(port, '127.0.0.1');
    let buf = '';
    s.on('connect', () => s.write(`GET ${path} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n` +
      `Sec-WebSocket-Key: ${crypto.randomBytes(16).toString('base64')}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
    s.on('data', (d) => {
      buf += d.toString('latin1');
      const m = /^HTTP\/1\.1 (\d+)/.exec(buf);
      if (m) { s.destroy(); resolve(Number(m[1])); }
    });
    s.on('error', reject);
  });
}

function datagram(dst, port, payload) {
  const b = Buffer.alloc(HEADER_SIZE + payload.length);
  b.writeUInt32BE(dst, 4);
  b.writeUInt16BE(port, 8);
  payload.copy(b, HEADER_SIZE);
  return b;
}

describe('web relay hardening', () => {
  test('TokenBucket: rate, depth, refill, oversized cost in debt, 0 = unlimited', () => {
    const b = new TokenBucket(10, 1000, 0);
    for (let i = 0; i < 10; i++) assert.ok(b.take(1, 0));
    assert.equal(b.take(1, 0), false);
    assert.ok(b.take(1, 100)); // 100 ms = 1 token
    assert.equal(b.take(1, 100), false);
    const big = new TokenBucket(10, 1000, 0);
    assert.ok(big.take(25, 0)); // full bucket: passes, 15 in debt
    assert.equal(big.take(1, 1000), false); // -15 + 10
    assert.ok(new TokenBucket(0, 1000, 0).take(1e9, 0));
  });

  test('room password: set by the first peer, 403 on a wrong/missing one, open rooms refuse a password, forgotten when empty', async () => {
    const s = await start();
    try {
      const a = await connect(s.port, '?room=pr&pass=s3cret');
      assert.equal((await a.next()).id, 1);
      assert.equal(await handshakeStatus(s.port, '/net?room=pr'), 403);
      assert.equal(await handshakeStatus(s.port, '/net?room=pr&pass=wrong'), 403);
      const b = await connect(s.port, '?room=pr&pass=s3cret');
      assert.equal((await b.next()).id, 2);
      const st = JSON.parse(await (await fetch(`http://127.0.0.1:${s.port}/net/status?room=pr`)).text());
      assert.equal(st.locked, true);
      assert.deepEqual(st.rooms, [{ room: 'pr', peers: 2, locked: true }]);
      // An open room with the same rules: a client that expects a password is refused.
      const o = await connect(s.port, '?room=open1');
      await o.next();
      assert.equal(await handshakeStatus(s.port, '/net?room=open1&pass=x'), 403);
      a.close(1000); b.close(1000); o.close(1000);
      for (let i = 0; i < 50 && s.relay.rooms.has('pr'); i++) await sleep(20);
      // Empty room: gone with its password; the next first peer sets a new one.
      const c = await connect(s.port, '?room=pr&pass=other');
      assert.equal((await c.next()).id, 1);
      c.close(1000);
      assert.ok(s.relay.stats.authFailed >= 2);
    } finally {
      await s.close();
    }
  });

  test('operator room passwords, --require-room-pass', async () => {
    const s = await start({ roomPasswords: { fixed: 'op' }, requireRoomPassword: true });
    try {
      assert.equal(await handshakeStatus(s.port, '/net?room=fixed&pass=mine'), 403); // first peer cannot pick another
      const a = await connect(s.port, '?room=fixed&pass=op');
      assert.equal((await a.next()).id, 1);
      assert.equal(await handshakeStatus(s.port, '/net?room=nopass'), 403); // required
      const b = await connect(s.port, '?room=own&pass=pw');
      await b.next();
      a.close(1000); b.close(1000);
    } finally {
      await s.close();
    }
  });

  test('access key on /net and /net/status; lockout (429) after repeated wrong secrets', async () => {
    const s = await start({ accessKeys: ['k1', 'k2'], authFailLimit: 3 });
    try {
      assert.equal(await handshakeStatus(s.port, '/net?room=ak'), 401);
      const a = await connect(s.port, '?room=ak&key=k2');
      assert.equal((await a.next()).id, 1);
      assert.equal((await fetch(`http://127.0.0.1:${s.port}/net/status?room=ak`)).status, 401);
      assert.equal((await fetch(`http://127.0.0.1:${s.port}/net/status?room=ak&key=k1`)).status, 200);
      assert.equal((await fetch(`http://127.0.0.1:${s.port}/healthz`)).status, 200); // not under /net
      // 3 failures so far? one ws + one status = 2; one more, then even the right key is refused.
      assert.equal(await handshakeStatus(s.port, '/net?room=ak&key=bad'), 401);
      assert.equal(await handshakeStatus(s.port, '/net?room=ak&key=k1'), 429);
      assert.equal(s.relay.stats.authFailed, 3);
      a.close(1000);
    } finally {
      await s.close();
    }
  });

  test('per-peer datagram rate limit (broadcast charged per receiver), --rate-kick closes 1008', async () => {
    const s = await start({ rateDatagrams: 50, rateBurstMs: 1000 });
    try {
      const a = await connect(s.port, '?room=rl');
      await a.next();
      const b = await connect(s.port, '?room=rl');
      await b.next();
      await a.next(); // join 2
      for (let i = 0; i < 200; i++) a.send(datagram(2, 1000, Buffer.from([i])));
      const got = (await b.drain()).filter((m) => Buffer.isBuffer(m));
      assert.ok(got.length >= 50 && got.length <= 55, `received ${got.length}`);
      assert.ok(s.relay.stats.rateLimited >= 145, `rateLimited ${s.relay.stats.rateLimited}`);
      // b has its own bucket.
      b.send(datagram(1, 1000, Buffer.from('x')));
      assert.ok(Buffer.isBuffer(await a.next()));
      // Broadcast to 2 receivers costs 2.
      const c = await connect(s.port, '?room=rl');
      await c.next();
      await a.next(); await b.next(); // join 3
      for (let i = 0; i < 50; i++) c.send(datagram(BROADCAST, 1000, Buffer.from([i])));
      const ga = (await a.drain()).length;
      assert.ok(ga >= 25 && ga <= 28, `broadcasts delivered ${ga}`);
      a.close(); b.close(); c.close();
    } finally {
      await s.close();
    }
    const k = await start({ rateDatagrams: 10, rateKick: 20 });
    try {
      const a = await connect(k.port, '?room=kick');
      await a.next();
      const b = await connect(k.port, '?room=kick');
      await b.next();
      for (let i = 0; i < 100; i++) a.send(datagram(2, 1000, Buffer.from([i])));
      for (let i = 0; i < 50 && a.closeCode === null; i++) await sleep(20);
      assert.equal(a.closeCode, 1008);
      assert.equal(k.relay.stats.kicked, 1);
      b.close();
    } finally {
      await k.close();
    }
  });

  test('byte rate limit and --max-datagram (dropped, socket stays, welcome tells the size)', async () => {
    const s = await start({ maxDatagram: 100, rateBytes: 1000, rateBurstMs: 1000, rateDatagrams: 0 });
    try {
      const a = await connect(s.port, '?room=sz');
      assert.equal((await a.next()).maxPayload, 100);
      const b = await connect(s.port, '?room=sz');
      await b.next();
      await a.next();
      a.send(datagram(2, 1, Buffer.alloc(101)));
      a.send(datagram(2, 1, Buffer.alloc(100)));
      const m = await b.next();
      assert.equal(m.length, HEADER_SIZE + 100);
      assert.equal(s.relay.stats.oversize, 1);
      assert.equal(a.readyState, WebSocket.OPEN);
      for (let i = 0; i < 20; i++) a.send(datagram(2, 1, Buffer.alloc(100))); // 900 bytes left in the bucket
      const n = (await b.drain()).length;
      assert.ok(n >= 9 && n <= 10, `received ${n}`);
      a.close(); b.close();
    } finally {
      await s.close();
    }
  });

  test('control messages: rate limit and size limit', async () => {
    const s = await start({ rateText: 5, maxText: 64 });
    try {
      const a = await connect(s.port, '?room=tx');
      await a.next();
      for (let i = 0; i < 30; i++) a.send(JSON.stringify({ type: 'ping', t: i }));
      const pongs = (await a.drain()).filter((m) => m.type === 'pong').length;
      assert.ok(pongs >= 10 && pongs <= 12, `pongs ${pongs}`); // depth: 2 s worth
      await sleep(1000);
      a.send(JSON.stringify({ type: 'ping', t: 'x'.repeat(80) }));
      await assert.rejects(a.next(300), /timeout/);
      a.send(JSON.stringify({ type: 'ping', t: 1 }));
      assert.deepEqual(await a.next(), { type: 'pong', t: 1 });
      assert.ok(s.relay.stats.textLimited >= 18);
      a.close();
    } finally {
      await s.close();
    }
  });

  test('--max-peers-per-ip, X-Forwarded-For only with --trust-proxy, --no-room-list', async () => {
    const s = await start({ maxPeersPerIp: 2, listRooms: false });
    try {
      const a = await connect(s.port, '?room=ip');
      await a.next();
      const b = await connect(s.port, '?room=ip2');
      await b.next();
      assert.equal(await handshakeStatus(s.port, '/net?room=ip3'), 429);
      const st = JSON.parse(await (await fetch(`http://127.0.0.1:${s.port}/net/status?room=ip`)).text());
      assert.deepEqual([st.peers, st.rooms], [1, []]);
      b.close(1000);
      for (let i = 0; i < 50 && s.relay.peersPerIp.get('127.0.0.1') !== 1; i++) await sleep(20);
      const c = await connect(s.port, '?room=ip3');
      await c.next();
      a.close(); c.close();
    } finally {
      await s.close();
    }
  });

  test('welcome: p2p flag and ICE servers for the WebRTC path', async () => {
    const ice = [{ urls: 'stun:stun.example:3478' }, { urls: 'turn:turn.example:3478', username: 'u', credential: 'c' }];
    const s = await start({ iceServers: ice });
    try {
      const a = await connect(s.port, '?room=w');
      const w = await a.next();
      assert.deepEqual([w.p2p, w.iceServers], [true, ice]);
      a.close();
    } finally {
      await s.close();
    }
    const n = await start({ p2p: false, iceServers: ice });
    try {
      const a = await connect(n.port, '?room=w');
      const w = await a.next();
      assert.deepEqual([w.p2p, w.iceServers], [false, undefined]);
      a.close();
    } finally {
      await n.close();
    }
  });
});
