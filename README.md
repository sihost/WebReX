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

WebReX was originally built for a browser port of Halo 3. It contains no game code or game
data: a recompiled game is a separate project that links WebReX and loads its own files.

## How it works

ReXGlue translates a game's PowerPC code into C++ ahead of time, so no CPU is emulated at
runtime. WebReX takes the runtime that recompiled code runs on and makes it work in a browser:

| Area | What WebReX does in the browser |
|---|---|
| **GPU (Xenos)** | Xenos command processor on WebGPU (`src/graphics/webgpu/`): command streams recorded on the game's GPU thread and replayed on a WebGPU device thread, guest memory mirrored with a dirty map, EDRAM emulation with resolves, render-target and texture caches, Xenos texture formats decoded on the GPU, tessellation and vertex memexport as compute passes |
| **Shaders** | Xenos shader microcode → SPIR-V → WGSL through [Tint](https://dawn.googlesource.com/dawn). Translation runs on worker threads and is cached in browser storage (OPFS). The 93 built-in shaders ship pre-translated |
| **Display** | `WebGPUPresenter` draws the game's frames to a canvas once per animation frame, letterboxed, and handles resizes and device loss. Includes the ImGui settings/debug overlay |
| **Threads and memory** | Emscripten pthreads with suspend, APCs and timers like the 360 kernel expects; the 4 GB guest address space inside wasm64 memory |
| **Files** | Game files stream over HTTP Range requests with an OPFS block cache; saves and profiles persist in OPFS across reloads |
| **Audio** | AudioWorklet output fed from a SharedArrayBuffer ring |
| **Input** | Gamepad API, keyboard and mouse with pointer lock |
| **Network** | System link as a virtual UDP LAN over a WebSocket relay (relay server not included) |

Native builds (Windows/Linux with D3D12/Vulkan) still work from the same tree.

## Status

Tested with a full recompiled retail game in Chrome on Windows:

- boots to the main menu, loads maps and plays with audio, controller and mouse/keyboard
- settings and profiles survive a page reload
- two browser tabs find each other and join one system-link lobby
- frames match the native D3D12 build at fixed game ticks (about 36–40 dB PSNR)
- saved films play back and closely match native (some draws still skipped, see below)

Occlusion queries need no host emulation: titles that end a query at a different address than
they begin it get the same fixed results natively.

### Known gaps

| Area | Gap |
|---|---|
| Browsers | Chromium only. Firefox stops at a WebGPU error (`setBindGroup`: dynamic offsets passed as a view of the >2 GB wasm memory); Safari is untested |
| Memory | A large game needs about 8 GB of wasm memory per tab (Halo 3: ~3.5 GB heap break in play) |
| GPU | First boot with an empty shader cache: draws are skipped while their pipelines are created, and render targets sampled for many frames stay wrong (16.7–24.7 dB vs native until the cache is warm) |
| GPU | Some Xenos shaders do not translate (pixel shaders failing SPIR-V validation; "couldn't extract memexport stream constant index") |
| GPU | Draws with no render target are dropped (WebGPU needs an attachment; no dummy target yet) |
| GPU | 16-bit normalized textures are stored as float16 so they stay filterable (precision differs from native) |
| Runtime | Fibers: `Fiber::Create` and switching to another fiber abort (no stack switching on wasm); titles importing `KeSetCurrentStackPointers` will not run |
| Runtime | No host fault handler (wasm traps cannot be caught; guest SEH is software only) and cooperative suspend/terminate only |
| Files | `HttpRangeDevice` reads one synchronous 1 MiB block at a time; parallel download and read-ahead must be done by the page (e.g. a service worker) |
| Audio | Stereo downmix only (no 5.1); ~90 ms output latency; a hidden tab's audio thread is starved by the browser |
| Network | System link only (no Xbox Live); the WebSocket relay server is not included |
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
