// STUN binding responder (lib/stun.mjs) and its announcement in the relay's welcome.
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import dgram from 'node:dgram';
import test from 'node:test';
import { createRelayServer } from '../lib/relay.mjs';
import { createStunResponder, stunReply } from '../lib/stun.mjs';

const request = () => {
  const b = Buffer.alloc(20);
  b.writeUInt16BE(0x0001, 0);
  b.writeUInt32BE(0x2112a442, 4);
  crypto.randomBytes(12).copy(b, 8);
  return b;
};
const mapped = (r) => {
  assert.equal(r.readUInt16BE(0), 0x0101);
  assert.equal(r.readUInt16BE(20), 0x0020);
  const port = r.readUInt16BE(26) ^ 0x2112;
  const ip = [0, 1, 2, 3].map((i) => r[28 + i] ^ r[4 + i]).join('.');
  return { ip, port };
};

test('stunReply: XOR-MAPPED-ADDRESS of the source, transaction id echoed, junk ignored', () => {
  const q = request();
  const r = stunReply(q, '10.0.0.249', 54321);
  assert.deepEqual(mapped(r), { ip: '10.0.0.249', port: 54321 });
  assert.deepEqual(r.subarray(8, 20), q.subarray(8, 20));
  assert.deepEqual(mapped(stunReply(q, '::ffff:192.168.1.7', 9)), { ip: '192.168.1.7', port: 9 });
  assert.equal(stunReply(Buffer.from('hello world, not stun'), '1.2.3.4', 1), null);
  const resp = request();
  resp.writeUInt16BE(0x0101, 0);
  assert.equal(stunReply(resp, '1.2.3.4', 1), null);
  assert.equal(stunReply(request(), '::1', 5).length, 20 + 8 + 16);
});

test('responder answers over UDP; relay welcome announces stun:<Host header>:<port>', async () => {
  const s = await createStunResponder({ port: 0, bind: '127.0.0.1' });
  const c = dgram.createSocket('udp4');
  await new Promise((r) => c.bind(0, '127.0.0.1', r));
  const got = new Promise((r) => c.once('message', r));
  c.send(request(), s.port, '127.0.0.1');
  assert.deepEqual(mapped(await got), { ip: '127.0.0.1', port: c.address().port });
  c.close();
  await s.close();

  const { server, relay, shutdown } = createRelayServer({ stun: true, iceServers: [{ urls: 'stun:far.example:3478' }] });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  for (let i = 0; !relay.stunPort && i < 100; i++) await new Promise((r) => setTimeout(r, 10));
  const port = server.address().port;
  assert.equal(relay.stunPort, port);
  const ws = new WebSocket(`ws://localhost:${port}/net?room=stun`);
  const welcome = await new Promise((r) => ws.addEventListener('message', (e) => r(JSON.parse(e.data))));
  assert.deepEqual(welcome.iceServers, [{ urls: `stun:localhost:${port}` }, { urls: 'stun:far.example:3478' }]);
  ws.close();
  await shutdown();
});
