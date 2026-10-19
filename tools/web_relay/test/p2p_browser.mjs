#!/usr/bin/env node
// Browser test of the net shim's peer-to-peer path (src/system/web/net_relay.js) without a game:
// two Chrome pages (separate browser contexts, like two players) each run net_relay.js on a fake
// wasm heap against this directory's relay, plus one old-style client (a plain WebSocket that
// knows nothing about WebRTC) in the same room. Checks: the DataChannel opens, unicast and
// broadcast reach the right peers with the right src on both paths, the old client still gets
// everything over the relay, switching P2P off/on at run time loses nothing, and the relay's
// `p2p: false` keeps everything on the relay. Measures the round trip of every datagram (page 1
// sends, page 2 echoes) over the DataChannel and over the relay.
//   node tools/web_relay/test/p2p_browser.mjs [--latency MS[:JITTER]] [--count 500] [--interval-ms 10]
//        [--size 200] [--headed] [--channel chrome] [--json out.json] [--no-stun] [--host 127.0.0.1]
// The relay runs its STUN responder (relay option stun) unless --no-stun; --host is the address the
// pages use for the relay (and so for its STUN server).
// Needs Playwright: found via $PLAYWRIGHT_MODULE (path to the package dir), a normal import,
// or a game repo's web_runner/server/node_modules.
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { BROADCAST, createRelayServer } from '../lib/relay.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const shimJs = path.resolve(here, '../../../src/system/web/net_relay.js');
const argv = process.argv.slice(2);
const opt = { latency: 0, jitter: 0, count: 500, intervalMs: 10, size: 200, headed: false, channel: 'chrome', json: '', stun: true, host: '127.0.0.1' };
for (let i = 0; i < argv.length; i++) {
  const k = argv[i];
  const v = () => argv[++i];
  if (k === '--latency') [opt.latency, opt.jitter = 0] = v().split(':').map(Number);
  else if (k === '--count') opt.count = Number(v());
  else if (k === '--interval-ms') opt.intervalMs = Number(v());
  else if (k === '--size') opt.size = Number(v());
  else if (k === '--headed') opt.headed = true;
  else if (k === '--channel') opt.channel = v();
  else if (k === '--json') opt.json = v();
  else if (k === '--chrome-arg') (opt.chromeArgs ??= []).push(v());
  else if (k === '--no-stun') opt.stun = false;
  else if (k === '--host') opt.host = v();
  else throw new Error(`unknown arg ${k}`);
}

async function loadPlaywright() {
  const tries = [process.env.PLAYWRIGHT_MODULE, 'playwright',
    'C:/rex/mw2/mw2_recomp/web_runner/server/node_modules/playwright'].filter(Boolean);
  for (const t of tries) {
    try {
      if (t === 'playwright') return await import('playwright');
      const req = createRequire(pathToFileURL(path.join(t, 'package.json')));
      return await import(pathToFileURL(req.resolve(t)).href);
    } catch {}
  }
  console.log('SKIP: playwright not found (set PLAYWRIGHT_MODULE to its package directory)');
  process.exit(0);
}

// Page side: net_relay.js on a fake heap (plain ArrayBuffer: Atomics work, nobody waits).
const PAGE = `<!doctype html><meta charset="utf-8"><title>p2p test</title><script>
window.makeNet = async (url, options) => {
  const src = await (await fetch('/net_relay.js')).text();
  const factory = (0, eval)(src);
  const ringSize = 1 << 19, ringPtr = 64, scratch = 64 + ringSize;
  const heap = new Uint8Array(scratch + 70000);
  const ctl = new Int32Array(heap.buffer, 0, 16);
  const dv = new DataView(heap.buffer);
  const t = { frames: 0, onFrame: null, logs: [] };
  const drain = () => {
    let head = Atomics.load(ctl, 0) >>> 0, tail = Atomics.load(ctl, 1) >>> 0;
    while (tail !== head) {
      const off = tail & (ringSize - 1);
      const len = dv.getUint32(ringPtr + off, true);
      if (len === 0xffffffff) { tail = (tail + ringSize - off) >>> 0; continue; }
      const f = heap.slice(ringPtr + off + 4, ringPtr + off + 4 + len);
      tail = (tail + ((4 + len + 3) & ~3)) >>> 0;
      t.frames++;
      if (t.onFrame) t.onFrame(f);
    }
    Atomics.store(ctl, 1, tail | 0);
  };
  const net = factory({ log: (m) => { t.logs.push(m); console.log(m); }, heap: () => heap, ctlPtr: 0, ringPtr, ringSize, options, onRx: drain });
  t.net = net;
  // One shim datagram: relay header (src, dst, port) + shim header (src port, proto 17, ver 1) + payload.
  t.send = (dst, port, payload) => {
    const n = 14 + payload.length;
    dv.setUint32(scratch, 0); dv.setUint32(scratch + 4, dst); dv.setUint16(scratch + 8, port);
    dv.setUint16(scratch + 10, 1000); heap[scratch + 12] = 17; heap[scratch + 13] = 1;
    heap.set(payload, scratch + 14);
    net.send(scratch, n);
  };
  net.connect(url, '');
  window.T = t;
  return true;
};
// Echo every datagram back to its sender (same port, same payload).
window.startEcho = () => {
  T.onFrame = (f) => {
    const d = new DataView(f.buffer);
    T.send(d.getUint32(0), d.getUint16(10), f.subarray(14));
  };
};
// Send count datagrams to dst every intervalMs, each stamped; collect the echoes' RTT.
window.rttRun = (dst, count, intervalMs, size) => new Promise((resolve) => {
  const rtt = new Array(count).fill(-1);
  const via = new Array(count).fill('');
  T.onFrame = (f) => {
    const d = new DataView(f.buffer, 14);
    if (d.getUint32(0) !== 0x52545431) return; // 'RTT1'
    const i = d.getUint32(4);
    if (i < count && rtt[i] < 0) rtt[i] = performance.now() - d.getFloat64(8);
  };
  let i = 0;
  const p = new Uint8Array(Math.max(16, size));
  const pd = new DataView(p.buffer);
  const tick = () => {
    if (i >= count) {
      setTimeout(() => resolve({ rtt, via }), 1500);
      return;
    }
    pd.setUint32(0, 0x52545431); pd.setUint32(4, i); pd.setFloat64(8, performance.now());
    const l = T.net.stats().p2p.links.find((x) => x.id === dst);
    via[i] = l ? l.path : 'relay';
    T.send(dst, 1000, p);
    i++;
    setTimeout(tick, intervalMs);
  };
  tick();
});
</script>`;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const pct = (s, p) => (s.length ? s[Math.min(s.length - 1, Math.floor(p * s.length))] : NaN);
const summarize = (arr) => {
  const ok = arr.filter((x) => x >= 0).sort((a, b) => a - b);
  const r = (x) => Math.round(x * 100) / 100;
  return { n: arr.length, recv: ok.length, lost: arr.length - ok.length, min: r(ok[0]), p50: r(pct(ok, 0.5)), p95: r(pct(ok, 0.95)), p99: r(pct(ok, 0.99)), max: r(ok[ok.length - 1]), mean: r(ok.reduce((a, b) => a + b, 0) / (ok.length || 1)) };
};

let failures = 0;
const check = (cond, what) => {
  console.log(`${cond ? 'ok  ' : 'FAIL'} ${what}`);
  if (!cond) failures++;
};

async function oldClient(port, room) {
  const ws = new WebSocket(`ws://127.0.0.1:${port}/net?room=${room}`);
  ws.binaryType = 'arraybuffer';
  ws.got = [];
  ws.welcome = new Promise((r) => ws.addEventListener('message', (e) => {
    if (typeof e.data === 'string') { const j = JSON.parse(e.data); if (j.type === 'welcome') r(j); }
    else ws.got.push(Buffer.from(e.data));
  }));
  await new Promise((r) => ws.addEventListener('open', r, { once: true }));
  return ws;
}

async function scenario(chromium, relayOpts, label) {
  const lines = [];
  const { server, relay, shutdown } = createRelayServer({ log: (l) => lines.push(l), latencyMs: opt.latency, jitterMs: opt.jitter, stun: opt.stun, stunBind: '0.0.0.0', ...relayOpts });
  await new Promise((r) => server.listen(0, opt.host === '127.0.0.1' ? '127.0.0.1' : '0.0.0.0', r));
  for (let i = 0; opt.stun && !relay.stunPort && i < 50; i++) await sleep(20);
  const relayPort = server.address().port;
  const web = http.createServer((req, res) => {
    if (req.url.startsWith('/net_relay.js')) {
      res.writeHead(200, { 'content-type': 'text/javascript' });
      res.end(fs.readFileSync(shimJs));
    } else {
      res.writeHead(200, { 'content-type': 'text/html' });
      res.end(PAGE);
    }
  });
  await new Promise((r) => web.listen(0, '127.0.0.1', r));
  const webPort = web.address().port;
  const browser = await chromium.launch({ channel: opt.channel, headless: !opt.headed, args: opt.chromeArgs ?? [] });
  const pages = [];
  const result = { label, relay: relayOpts };
  try {
    const url = `ws://${opt.host}:${relayPort}/net?room=p2p`;
    for (let n = 1; n <= 2; n++) {
      const ctx = await browser.newContext();
      const page = await ctx.newPage();
      page.on('console', (m) => console.log(`  [page${n}] ${m.text()}`));
      page.on('pageerror', (e) => { console.log(`  [page${n}] pageerror ${e}`); failures++; });
      await page.goto(`http://127.0.0.1:${webPort}/`);
      await page.evaluate(([u, o]) => window.makeNet(u, o), [url, { statsLogMs: 0, debug: !!process.env.P2P_DEBUG }]);
      pages.push(page);
      await sleep(300);
    }
    const old = await oldClient(relayPort, 'p2p');
    const ow = await old.welcome;
    check(ow.id === 3, `old-style client joined as id ${ow.id}`);
    const stats = (p) => p.evaluate(() => T.net.stats());
    const wantOpen = relayOpts.p2p !== false;
    const t0 = Date.now();
    let link = null;
    while (Date.now() - t0 < 60000) {
      link = (await stats(pages[0])).p2p.links.find((l) => l.id === 2);
      if (link && (link.state === 'open' || !wantOpen) && Date.now() - t0 > 1500) break;
      await sleep(200);
    }
    result.open = link;
    if (wantOpen) {
      check(link && link.state === 'open', `page1 -> page2 DataChannel open after ${link?.openMs} ms (${link?.cand})`);
      const l3 = (await stats(pages[0])).p2p.links.find((l) => l.id === 3);
      check(l3 && l3.state === 'relay' && !l3.capable, 'old-style client: no channel, relay path');
    } else {
      check((!link || link.state === 'relay') && (await stats(pages[0])).p2p.on === false, 'relay p2p:false: no channel, p2p off');
    }
    // Broadcast from page 1: page 2 (channel) and the old client (relay unicast) get it, src 1.
    await pages[1].evaluate(() => { T.bc = []; T.onFrame = (f) => T.bc.push([...f.subarray(0, 4)]); });
    old.got.length = 0;
    await pages[0].evaluate((b) => T.send(b, 1001, new TextEncoder().encode('getinfo')), BROADCAST);
    await sleep(500);
    const bc = await pages[1].evaluate(() => T.bc);
    check(bc.length === 1 && bc[0].join() === '0,0,0,1', `broadcast reached page 2 once with src 1 (${JSON.stringify(bc)})`);
    const og = old.got.filter((b) => b.length > 14 && b[12] === 17);
    check(og.length === 1 && og[0].readUInt32BE(0) === 1 && og[0].subarray(14).toString() === 'getinfo', `broadcast reached the old client over the relay (${og.length})`);
    // Signalling never reaches the old client's sockets: only proto 0x50 frames to port 0.
    check(old.got.every((b) => b[12] === 17 || (b.readUInt16BE(8) === 0 && b[12] === 0x50)), 'old client saw only datagrams and port-0 control frames');
    // Per-datagram RTT, page 1 -> page 2 -> page 1.
    await pages[1].evaluate(() => window.startEcho());
    const runOnce = async (name) => {
      const r = await pages[0].evaluate(([c, iv, sz]) => window.rttRun(2, c, iv, sz), [opt.count, opt.intervalMs, opt.size]);
      const s = summarize(r.rtt);
      const vias = r.via.reduce((m, v) => ({ ...m, [v]: (m[v] || 0) + 1 }), {});
      console.log(`  ${label} ${name}: ${JSON.stringify(s)} via ${JSON.stringify(vias)}`);
      return { ...s, via: vias };
    };
    result.first = await runOnce(wantOpen ? 'p2p' : 'relay');
    check(result.first.lost === 0, `${result.first.n} datagrams echoed, ${result.first.lost} lost`);
    if (wantOpen) {
      check(result.first.via.p2p === opt.count, 'every datagram went over the DataChannel');
      // Switch P2P off on both pages while a stream runs: nothing lost, path relay.
      const during = pages[0].evaluate(([c, iv, sz]) => window.rttRun(2, c, iv, sz), [200, opt.intervalMs, opt.size]);
      await sleep(500);
      await pages[0].evaluate(() => T.net.setP2p(false));
      await pages[1].evaluate(() => T.net.setP2p(false));
      const sw = summarize((await during).rtt);
      check(sw.lost === 0, `switch to relay mid-stream: ${sw.recv}/${sw.n} echoed`);
      result.relay = await runOnce('relay');
      check(result.relay.lost === 0 && result.relay.via.relay === opt.count, 'relay run: all echoed, all via relay');
      // And back on: the channel reopens.
      await pages[0].evaluate(() => T.net.setP2p(true));
      await pages[1].evaluate(() => T.net.setP2p(true));
      const t1 = Date.now();
      let back = null;
      while (Date.now() - t1 < 60000) {
        back = (await stats(pages[0])).p2p.links.find((l) => l.id === 2);
        if (back?.state === 'open') break;
        await sleep(200);
      }
      check(back?.state === 'open', `channel reopened after setP2p(true) in ${Date.now() - t1} ms`);
      result.p2pAgain = await runOnce('p2p again');
      // Probe RTT (both paths, measured by the shim itself once a second).
      await sleep(3000);
      result.probes = (await stats(pages[0])).p2p.links.find((l) => l.id === 2).rtt;
      console.log(`  ${label} shim probes: ${JSON.stringify(result.probes)}`);
    }
    old.close();
  } finally {
    await browser.close().catch(() => {});
    await shutdown();
    web.close();
  }
  return result;
}

const pw = await loadPlaywright();
const chromium = pw.chromium ?? pw.default?.chromium;
const results = [];
results.push(await scenario(chromium, {}, `relay latency ${opt.latency}${opt.jitter ? `:${opt.jitter}` : ''} ms`));
results.push(await scenario(chromium, { p2p: false }, 'relay says p2p:false'));
if (opt.json) fs.writeFileSync(opt.json, JSON.stringify({ opt, results }, null, 2));
console.log(failures ? `RESULT FAIL (${failures})` : 'RESULT PASS');
process.exit(failures ? 1 : 0);
