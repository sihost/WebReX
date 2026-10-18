# Security Policy

## Supported versions

WebReX is experimental. Only the latest commit on `main` gets security fixes.

## Reporting a vulnerability

**Please do not open a public issue for security problems.**

Report privately through GitHub:
**[Security → Report a vulnerability](https://github.com/sihost/WebReX/security/advisories/new)**.

Include:

- what is affected (file, component, or feature)
- steps to reproduce or a proof of concept
- the impact you expect (what an attacker can do)
- browser, OS and build you used

You should get a first reply within **7 days**. We will keep you updated while we work on a fix
and credit you in the advisory unless you prefer to stay anonymous.

## Scope

In scope:

- the WebReX runtime compiled to WebAssembly (memory safety issues reachable from game data,
  network packets or page input; escapes from the wasm sandbox assumptions the runtime relies on)
- the web glue (`*.js` in the tree), the WebGPU backend, storage (OPFS) handling
- the reference relay server in `tools/web_relay` (e.g. room isolation, denial of service,
  traffic injection between rooms)
- build and CI configuration in this repository

Out of scope:

- bugs in browsers, Emscripten, Dawn/Tint or other upstream projects (report those upstream;
  tell us too if WebReX makes them reachable)
- issues that need a malicious recompiled game: a game linked against WebReX runs with the
  same trust as any web page it is served from
- relay deployments you do not control, and denial of service by sending excessive traffic
- issues in [rexglue-sdk](https://github.com/rexglue/rexglue-sdk) code that are not specific
  to the web port: please report those to the upstream project

## Disclosure

We aim to fix confirmed issues within 90 days and publish a GitHub Security Advisory when the
fix is on `main`.
