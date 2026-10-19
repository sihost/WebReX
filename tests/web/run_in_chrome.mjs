#!/usr/bin/env node
// Runs an Emscripten test page (em++ -o x.html, -sENVIRONMENT=web,worker) in headless Chrome and
// forwards its console. Serves the page's directory on 127.0.0.1 with COOP/COEP (pthreads need a
// SharedArrayBuffer), or opens --url instead (e.g. a page served by the Halo server, same origin
// as its /data/). Exit code 0 when a line matches --done and the first number on it is 0 when
// --zero is given (e.g. "N checks, 0 failures"), 1 otherwise or on timeout.
//
//   node tests/web/run_in_chrome.mjs <page.html | --url URL> [--done REGEX] [--seconds N]
//        [--channel chrome] [--headed] [--playwright DIR]
//
// Playwright is not an SDK dependency: pass --playwright <node_modules dir> or set
// REX_PLAYWRIGHT (default: the Halo 3 project's server/node_modules next to the SDK checkout).
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { createRequire } from 'node:module';

const argv = process.argv.slice(2);
const opt = { done: '(\\d+) failures', seconds: 120, channel: 'chrome', headed: false };
for (let i = 0; i < argv.length; i++) {
  const k = argv[i];
  const v = () => argv[++i];
  if (k === '--done') opt.done = v();
  else if (k === '--seconds') opt.seconds = Number(v());
  else if (k === '--channel') opt.channel = v();
  else if (k === '--headed') opt.headed = true;
  else if (k === '--playwright') opt.playwright = v();
  else if (k === '--url') opt.url = v();
  else opt.page = k;
}
const pwDir =
  opt.playwright ||
  process.env.REX_PLAYWRIGHT ||
  'C:/Users/Riley/Downloads/halo3_cache_release_recomp-main/halo3_cache_release_recomp-main/server/node_modules';
const require = createRequire(path.join(pwDir, 'noop.js'));
const { chromium } = require('playwright');

const types = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.wasm': 'application/wasm' };
let server;
let url = opt.url;
if (!url) {
  const root = path.dirname(path.resolve(opt.page));
  server = http.createServer((req, res) => {
    const p = path.join(root, decodeURIComponent(new URL(req.url, 'http://x').pathname));
    if (!p.startsWith(root) || !fs.existsSync(p) || fs.statSync(p).isDirectory()) {
      res.writeHead(404).end();
      return;
    }
    res.writeHead(200, {
      'Content-Type': types[path.extname(p)] || 'application/octet-stream',
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
      'Cache-Control': 'no-store',
    });
    fs.createReadStream(p).pipe(res);
  });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  url = `http://127.0.0.1:${server.address().port}/${path.basename(opt.page)}`;
}

const browser = await chromium.launch({
  channel: opt.channel,
  headless: !opt.headed,
  args: ['--no-proxy-server', '--no-first-run', '--no-default-browser-check'],
});
const page = await browser.newPage();
const doneRe = new RegExp(opt.done);
let code = 1;
const finished = new Promise((resolve) => {
  page.on('console', (m) => {
    const text = m.text();
    console.log(text);
    const match = doneRe.exec(text);
    if (match) {
      code = match[1] === undefined || Number(match[1]) === 0 ? 0 : 1;
      resolve();
    }
  });
  page.on('pageerror', (e) => console.log(`[pageerror] ${e.message}`));
  setTimeout(() => {
    console.log(`[run_in_chrome] timeout after ${opt.seconds} s`);
    resolve();
  }, opt.seconds * 1000);
});
// Navigation to a local server occasionally times out on this machine: retry.
for (let attempt = 1; ; attempt++) {
  try {
    await page.goto(url, { timeout: 20000 });
    break;
  } catch (e) {
    if (attempt >= 4) throw e;
    console.log(`[run_in_chrome] goto failed (${e.message.split('\n')[0]}), retrying`);
  }
}
await finished;
await browser.close();
server?.close();
process.exit(code);
