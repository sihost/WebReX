# ReXGlue web port: the Xbox 360 runtime on WebGPU + wasm64

This is a fork of [rexglue-sdk](https://github.com/rexglue/rexglue-sdk) (branch `web-port`). It
runs ReXGlue-recompiled Xbox 360 titles in the browser: the SDK runtime (kernel, memory,
threads, filesystem, audio, input) is built with Emscripten for **wasm64**, and the Xenos GPU is
emulated on **WebGPU**.

No game code or game data is included. A recompiled title is a separate project that links
this SDK and loads its own assets.

## What the web port adds

| Area | Web implementation |
|---|---|
| GPU (Xenos) | `src/graphics/webgpu/`: command processor, command stream record/replay on a device thread, shared memory with a dirty map, primitive processor, EDRAM emulation + resolves, render-target cache, texture loader/cache, tessellation (compute pre-pass), vertex memexport (compute) |
| Shaders | Xenos microcode → SPIR-V (`SpirvShaderTranslator`) → WGSL via Tint (`thirdparty/tint`, Dawn `31e25af2`); 93 fixed shaders pre-converted in `src/graphics/shaders/webgpu_wgsl/`; conversion on worker threads, cached in OPFS |
| Presentation | `src/ui/webgpu/`: `WebGPUPresenter` (mailbox, rAF paint, letterbox, device loss), ImGui overlay (`WebGPUImmediateDrawer`), web canvas window |
| Platform | `REX_PLATFORM_WEB` / `REX_ARCH_WASM`; web threading (cooperative suspend, APCs, timers), fibers, memory + SEH backends, monotonic clock |
| Filesystem | HTTP Range device with an OPFS block cache, OPFS mount for saves/profiles |
| Audio | AudioWorklet driver on a SharedArrayBuffer ring |
| Input | Gamepad API driver, DOM keyboard/mouse bridge with pointer lock |
| Network | virtual UDP LAN over a WebSocket relay (system link); needs a relay server, not included |

Native builds (Windows/Linux, D3D12/Vulkan) are unchanged and still build.

## Status

Tested with a full recompiled retail title in Chrome. It boots to the menu, loads and plays
maps with audio and input, and keeps profiles across reloads. System link works between two
tabs. Its frames match the native D3D12 build at fixed game ticks (about 36–40 dB PSNR).
Known gaps: occlusion-query draws are skipped, and a full map currently needs about 8 GiB of
wasm memory per tab.

## Building (wasm64)

Requirements: [emsdk](https://emscripten.org/docs/getting_started/downloads.html) (activated),
CMake ≥ 3.25, Ninja, Git, Python 3, and a browser with WebGPU (Chrome/Edge 113+).

```sh
git submodule update --init --recursive
scripts/fetch_dawn.sh            # or scripts\fetch_dawn.bat - Tint sources (~150 MB)
cmake --preset wasm64-release    # binary dir out/w64r
cmake --build out/w64r           # first build also builds Tint (~10 min)
cmake --install out/w64r         # -> out/install/wasm64
```

Pages that use the runtime must be served with `Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp` (wasm threads need SharedArrayBuffer).

## Example

[`examples/webgpu_presenter_demo`](examples/webgpu_presenter_demo) is a small program that uses
the installed SDK. It runs the presenter path, checks the presented pixels by GPU readback, and
then shows an animated guest frame.

## License

BSD 3-Clause, same as upstream: see [LICENSE](LICENSE). Portions are derived from
[Xenia](https://xenia.jp). This project is not affiliated with Microsoft or Xbox.
