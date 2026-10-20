<h1 align="center">WebReX</h1>

<p align="center">
  <b>The ReXGlue Xbox 360 runtime in the browser, with Xenos on WebGPU</b>
</p>

<p align="center">
  <img src="docs/media/presenter_demo.png" width="560" alt="WebReX presenter demo running in Chrome">
</p>

> [!CAUTION]
> WebReX is experimental. It needs a recent Chromium browser with WebGPU and wasm64 support, a
> lot of memory, and a game recompiled with ReXGlue. APIs will change.

WebReX runs Xbox 360 games that were recompiled with [ReXGlue](https://github.com/rexglue/rexglue-sdk)
in a web browser, without plugins. The runtime (kernel, threads, memory, filesystem, audio,
input and system-link networking) is compiled to **WebAssembly (wasm64)** with Emscripten.
The Xbox 360 GPU, **Xenos**, is emulated on **WebGPU**.

WebReX was originally built for a browser port of Halo 3, and also runs Call of Duty: Modern
Warfare 2 multiplayer. It contains no game code or game data: a recompiled game is a separate
project that links WebReX and loads its own files.

## How it works

ReXGlue translates a game's PowerPC code into C++ ahead of time, so no CPU is emulated at
runtime. WebReX takes the runtime that recompiled code runs on and makes it work in a browser:

| Area | What WebReX does in the browser |
|---|---|
| **GPU (Xenos)** | Xenos command processor on WebGPU (`src/graphics/webgpu/`): command streams recorded on the game's GPU thread and replayed on a WebGPU device thread (render-pass calls batched into one JS call, bind groups reused across frames), guest memory mirrored with a dirty map (only changed pages are uploaded; `--webgpu_verify_shared_memory` checks it), EDRAM emulation with resolves, render-target and texture caches, Xenos texture formats decoded on the GPU, tessellation and vertex memexport as compute passes. Draw resolution scaling (`--draw_resolution_scale_x/y`, 2x/3x) |
| **Shaders** | Xenos shader microcode → SPIR-V → WGSL through [Tint](https://dawn.googlesource.com/dawn). Translation runs on worker threads and is cached in browser storage (OPFS). The 93 built-in shaders ship pre-translated |
| **Display** | `WebGPUPresenter` draws the game's frames to a canvas once per animation frame, letterboxed, and handles resizes and device loss. Same CAS/FSR output effects as native (`--present_effect`), fullscreen with Keyboard Lock (F11). Includes the ImGui settings/debug overlay |
| **Threads and memory** | Emscripten pthreads with suspend, APCs and timers like the 360 kernel expects; guest vblanks paced by the browser's animation frames; the 4 GB guest address space inside wasm64 memory, which starts at 3 GB and grows as needed (per-title `REXGLUE_WEB_INITIAL_MEMORY` / `REXGLUE_WEB_MAXIMUM_MEMORY`) with a clear out-of-memory report |
| **Files** | Game files stream over HTTP Range requests with an OPFS block cache; saves and profiles persist in OPFS across reloads |
| **Audio** | AudioWorklet output fed from a SharedArrayBuffer ring, with adaptive latency and optional 5.1 |
| **Input** | Gamepad API, keyboard and mouse with raw pointer lock, and an opt-in on-screen touch gamepad (`Module.rexTouchGamepad`) |
| **Network** | System link as a virtual UDP LAN over a WebSocket relay: guest `NetDll_*` sockets (UDP and VDP, broadcast) run on the relay, and XNet (title address, secure addresses, keys, QoS) and `XSessionCreate` are implemented for System Link. Tabs connect peer-to-peer over WebRTC DataChannels when they can (signalled through the relay, which also answers STUN), and fall back to the relay. A standalone reference relay with room passwords, access keys and rate limits is in [`tools/web_relay`](tools/web_relay/README.md) |
| **Codegen** | Finds functions that are only reached through pointers or tail branches by itself (no manual `[functions]` entries); a call to an address with no generated code can log and continue (`REXGLUE_CHECKED_INDIRECT_CALLS`). Functions that call setjmp go into their own translation unit, compiled at -O0 for the web (`setjmp_separate_tu`), so no wasm function exceeds V8's 7.65 MB limit |
| **Page** | Crash reports from any thread (`Module.rexOnFatal`) and page lifecycle hooks for hidden tabs and Memory Saver discards (`rexOnLifecycle`, `rexOnDiscardRisk`, `rexOnDiscarded`). See [`docs/web_limitations.md`](docs/web_limitations.md) |

Native builds (Windows/Linux with D3D12/Vulkan) still work from the same tree.

## Status

Tested in Chrome on Windows with two recompiled retail games, Halo 3 and Modern Warfare 2
multiplayer:

- boots to the main menu, loads maps and plays with audio, controller and mouse/keyboard
- settings and profiles survive a page reload
- two browser tabs find each other, join one system-link lobby and play the same match
- a hidden tab keeps running at full speed, with clean audio
- frames match the native D3D12 build at fixed game ticks of a saved film (about 38–40 dB PSNR for most of the film, 35–37 dB on a cold first boot, 32 dB at the last tick)
- saved films play back and closely match native (some draws still skipped, see below)

Occlusion queries need no host emulation: titles that end a query at a different address than
they begin it get the same fixed results natively.

### Known gaps

| Area | Gap |
|---|---|
| Browsers | Chromium is the target. Firefox runs games with 0 WebGPU errors but slowly: it compiles pipelines one at a time (about 1.5 frames/s in Halo while a map's pipelines compile, no long stalls); Safari is untested |
| Memory | A large game needs 3-4 GB of wasm memory per tab in play (MW2 3.4 GB, Halo 3 ~3.5 GB); memory grows up to 8 GB by default |
| GPU | First boot with an empty shader cache: a precompiled WGSL bundle covers most pipelines, but frames right after the first map load (tick ~150) can still differ from native until the rest are created |
| GPU | Memexport results stay on the GPU by default; `--webgpu_readback_memexport_delayed` copies them back into guest memory about a frame late without waiting (the native `readback_memexport_fast` path). Off by default: one of three film runs had a wrong frame at the first map load with it on |
| GPU | 16-bit normalized textures are stored as float16 so they stay filterable (precision differs from native) |
| GPU | WebGPU limits: MSAA 2x runs as 4x, no ROV/FSI, no fill mode, no cull distance (see [`docs/web_limitations.md`](docs/web_limitations.md)) |
| Runtime | No host fault handler (wasm traps cannot be caught; guest SEH is software only); suspend/terminate are cooperative (kernel exports are safe points) |
| Audio | ~90 ms output latency on a busy CPU (adaptive, lower when idle) |
| Page | Chrome's Memory Saver can discard an inactive tab that plays no audio; the page is only told on the next load (`rexOnDiscarded`) |
| Storage | Chrome 155+ (`StaticStorageQuota`) makes `navigator.storage.estimate()` report usage + 10 GiB whatever the real quota is, so the runtime only learns about a small quota when an OPFS write fails; the block cache then evicts and retries. Data outside OPFS counts against the same quota |
| Network | System link only (no Xbox Live). `--xnet_unique_xuid` gives each tab its own player XUID; `--xnet_trace` logs every XNet and socket call |
| Input | Browsers expose a gamepad only after a button press; rumble depends on the browser's `vibrationActuator` |

## Example

[`examples/webgpu_presenter_demo`](examples/webgpu_presenter_demo) is a small program built
against WebReX. It sends frames through the same display path a game uses, checks the result
by reading pixels back from the GPU, and then shows an animated frame (the image above).
Its README has build and run steps.

## Building

Requirements: [emsdk](https://emscripten.org/docs/getting_started/downloads.html) (activated),
CMake 3.25+, Ninja, Git, Python 3, and Chrome or Edge with WebGPU.

```sh
git submodule update --init --recursive
scripts/fetch_dawn.sh            # Windows: scripts\fetch_dawn.bat (Tint sources, ~150 MB)
cmake --preset wasm64-release    # build dir: out/w64r
cmake --build out/w64r           # the first build also compiles Tint (~10 min)
cmake --install out/w64r         # installs to out/install/wasm64
```

Pages that use WebReX must be served with `Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp`, because wasm threads need SharedArrayBuffer.
The example's `serve.mjs` does this.

For the recompiler itself (turning a game into C++), see the
[ReXGlue wiki](https://github.com/rexglue/rexglue-sdk/wiki).

## Credits

- [ReXGlue](https://github.com/rexglue/rexglue-sdk) by Tom (crack), Loreaxe, Soren/Roxxsen and
  Toby: the recompiler and runtime WebReX is built on.
- [Xenia](https://github.com/xenia-project/xenia): the Xbox 360 emulator whose GPU, kernel and
  system work ReXGlue (and so WebReX) is derived from.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and
  [rexdex's recompiler](https://github.com/rexdex/recompiler): the static recompilation
  approach ReXGlue follows.
- [Dawn/Tint](https://dawn.googlesource.com/dawn) and [Emscripten](https://emscripten.org).
- Built with help from Claude.

## Disclaimer

WebReX is not affiliated with or endorsed by Microsoft or Xbox. All trademarks belong to their
owners. It does not include any game code or data, and is not meant to enable piracy: use it
only with games you own.

## License

BSD 3-Clause, the same as ReXGlue: see [LICENSE](LICENSE). Portions are derived from
[Xenia](https://xenia.jp).
