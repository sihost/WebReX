# WebGPU presenter demo + self-test

A small browser program built against the wasm64 SDK. It uses the same presentation path an
Xbox 360 title uses on the web: `WebGPUDevice` → `WebGPUProvider` → `WebGPUPresenter` on a
`ui::Window` canvas, run by the `WebWindowedAppContext` UI loop. A separate thread stands in
for the GPU command processor and hands the presenter a 320x180 rgb10a2 "guest frame" the way
a Xenos front-buffer swap does.

It checks five things, then keeps showing an animated guest frame:

| Check | Pass condition |
|---|---|
| clear | before any guest frame, every canvas pixel is the background color (GPU readback) |
| checker | a checkerboard guest frame letterboxed 16:9, cell centers exact (readback) |
| resize | after the page resizes the canvas, the next frame has the new size and the same image |
| pacing | about 60 Hz (or the display rate) of `requestAnimationFrame` presents over 5 s |
| errors | 0 uncaptured WebGPU errors |

The header badge shows **PASS** or **FAIL**, and the page logs a `RESULT {json}` line.

## Build and run

1. Build and install the SDK for the web (see the main [README](../../README.md#building)):
   `cmake --preset wasm64-release`, `cmake --build out/w64r`, `cmake --install out/w64r`.
2. Build the demo: `build.bat` on Windows or `./build.sh` elsewhere, with emsdk activated.
   Pass a different SDK install prefix as the first argument if needed.
3. Serve it: `node serve.mjs`. This sends the COOP/COEP headers that wasm threads
   (SharedArrayBuffer) need. Then open <http://localhost:8099/> in Chrome or Edge 113+.

Keep the tab in front during the pacing check: browsers throttle background tabs.
