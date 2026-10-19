#!/usr/bin/env node
// Standalone reference relay for the ReXGlue web net shim. Usage: see README.md.
//   node tools/web_relay/server.mjs [--port 8090] [--bind 0.0.0.0] [--allow-origin <origin|*>]...
//     [--default-room default] [--max-peers 16] [--max-rooms 64] [--keepalive-ms 20000]
//     [--idle-timeout-ms 90000] [--resume-grace-ms 30000] [--latency 0[:jitter]] [--tls-pfx file --tls-pass pw |
//     --tls-cert file --tls-key file] [--quiet]
//     hardening: [--access-key K]... [--room-pass ROOM=PW]... [--require-room-pass] [--max-datagram 65536]
//     [--rate-datagrams 2000] [--rate-bytes 8388608] [--rate-burst-ms 1000] [--rate-text 20] [--rate-kick 0]
//     [--max-peers-per-ip 0] [--auth-fail-limit 10] [--trust-proxy] [--no-room-list]
//     peer-to-peer: [--no-p2p] [--ice-server URL[|user|credential]]...
import fs from 'node:fs';
import { createRelayServer } from './lib/relay.mjs';

const opt = { port: 8090, bind: '0.0.0.0', allowOrigins: [], defaultRoom: 'default', accessKeys: [], roomPasswords: {}, iceServers: [] };
const args = process.argv.slice(2);
const usage = () => {
  console.log(fs.readFileSync(new URL(import.meta.url), 'utf8').split('\n').slice(1, 10).map((l) => l.replace(/^\/\/ ?/, '')).join('\n'));
  process.exit(0);
};
for (let i = 0; i < args.length; i++) {
  const a = args[i];
  const v = () => {
    if (i + 1 >= args.length) throw new Error(`${a} needs a value`);
    return args[++i];
  };
  if (a === '--port') opt.port = Number(v());
  else if (a === '--bind') opt.bind = v();
  else if (a === '--allow-origin') opt.allowOrigins.push(v());
  else if (a === '--default-room') opt.defaultRoom = v();
  else if (a === '--max-peers') opt.maxPeersPerRoom = Number(v());
  else if (a === '--max-rooms') opt.maxRooms = Number(v());
  else if (a === '--keepalive-ms') opt.keepaliveMs = Number(v());
  else if (a === '--idle-timeout-ms') opt.idleTimeoutMs = Number(v());
  else if (a === '--resume-grace-ms') opt.resumeGraceMs = Number(v());
  else if (a === '--latency') [opt.latencyMs, opt.jitterMs] = v().split(':').map(Number);
  else if (a === '--tls-pfx') opt.pfx = v();
  else if (a === '--tls-pass') opt.passphrase = v();
  else if (a === '--tls-cert') opt.cert = v();
  else if (a === '--tls-key') opt.key = v();
  else if (a === '--quiet') opt.quiet = true;
  else if (a === '--access-key') opt.accessKeys.push(v());
  else if (a === '--room-pass') {
    const s = v();
    const eq = s.indexOf('=');
    if (eq < 1) throw new Error('--room-pass needs ROOM=PASSWORD');
    opt.roomPasswords[s.slice(0, eq)] = s.slice(eq + 1);
  } else if (a === '--require-room-pass') opt.requireRoomPassword = true;
  else if (a === '--max-datagram') opt.maxDatagram = Number(v());
  else if (a === '--rate-datagrams') opt.rateDatagrams = Number(v());
  else if (a === '--rate-bytes') opt.rateBytes = Number(v());
  else if (a === '--rate-burst-ms') opt.rateBurstMs = Number(v());
  else if (a === '--rate-text') opt.rateText = Number(v());
  else if (a === '--rate-kick') opt.rateKick = Number(v());
  else if (a === '--max-peers-per-ip') opt.maxPeersPerIp = Number(v());
  else if (a === '--auth-fail-limit') opt.authFailLimit = Number(v());
  else if (a === '--trust-proxy') opt.trustProxy = true;
  else if (a === '--no-room-list') opt.listRooms = false;
  else if (a === '--no-p2p') opt.p2p = false;
  else if (a === '--ice-server') {
    const [urls, username, credential] = v().split('|');
    opt.iceServers.push(username !== undefined ? { urls, username, credential } : { urls });
  }
  else if (a === '--help' || a === '-h') usage();
  else throw new Error(`unknown argument ${a} (--help)`);
}
if (!opt.allowOrigins.length) opt.allowOrigins = ['*'];
let tls;
if (opt.pfx) tls = { pfx: fs.readFileSync(opt.pfx), passphrase: opt.passphrase };
else if (opt.cert || opt.key) tls = { cert: fs.readFileSync(opt.cert), key: fs.readFileSync(opt.key) };

const log = opt.quiet ? () => {} : (l) => console.log(`${new Date().toISOString()} ${l}`);
const { server, shutdown } = createRelayServer({ ...opt, tls, log });
server.listen(opt.port, opt.bind, () => {
  const scheme = tls ? 'wss' : 'ws';
  console.log(`relay listening on ${scheme}://${opt.bind}:${server.address().port}/net?room=<name> ` +
    `(origins: ${opt.allowOrigins.join(' ')}${opt.accessKeys.length ? ', access key required' : ''}` +
    `${opt.p2p === false ? ', p2p off' : ''})`);
});
const stop = () => {
  shutdown().then(() => process.exit(0));
  setTimeout(() => process.exit(0), 2000).unref();
};
process.on('SIGINT', stop);
process.on('SIGTERM', stop);
