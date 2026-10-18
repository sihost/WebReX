#!/usr/bin/env node
// Standalone reference relay for the ReXGlue web net shim. Usage: see README.md.
//   node tools/web_relay/server.mjs [--port 8090] [--bind 0.0.0.0] [--allow-origin <origin|*>]...
//     [--default-room default] [--max-peers 16] [--max-rooms 64] [--keepalive-ms 20000]
//     [--idle-timeout-ms 90000] [--resume-grace-ms 30000] [--latency 0[:jitter]] [--tls-pfx file --tls-pass pw |
//     --tls-cert file --tls-key file] [--quiet]
import fs from 'node:fs';
import { createRelayServer } from './lib/relay.mjs';

const opt = { port: 8090, bind: '0.0.0.0', allowOrigins: [], defaultRoom: 'default' };
const args = process.argv.slice(2);
const usage = () => {
  console.log(fs.readFileSync(new URL(import.meta.url), 'utf8').split('\n').slice(1, 6).map((l) => l.replace(/^\/\/ ?/, '')).join('\n'));
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
    `(origins: ${opt.allowOrigins.join(' ')})`);
});
const stop = () => {
  shutdown().then(() => process.exit(0));
  setTimeout(() => process.exit(0), 2000).unref();
};
process.on('SIGINT', stop);
process.on('SIGTERM', stop);
