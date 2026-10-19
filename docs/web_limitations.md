# Web (wasm64 + WebGPU) limitations and behavior

This page lists what a ReXGlue title does differently in a browser, and why. Some items are WebGPU or
browser limits that cannot be fixed. Others are known gaps with a workaround. It also describes the
crash, page lifecycle and degraded-feature reporting that a web page can hook into.

## WebGPU limits (won't fix)

### MSAA 2x is emulated with 4x
WebGPU only allows sample counts 1 and 4. Guest 2x render targets use 4 host samples: guest samples 0/1
map to the top pair and 2/3 to the bottom pair (sample mask `0b1001`). See
`include/rex/graphics/webgpu/webgpu_render_target_cache.h` (MSAA comment, `IsMsaa2xSupported() == false`)
and `webgpu_pipeline_cache.cpp` (sample mask). The output is the same. The cost is twice the memory and
bandwidth of a native 2x target. Halo 3 renders mostly 1x, so the cost is small there.

### No rasterizer-ordered views or fragment shader interlock
WebGPU has no ROV/FSI. Only the host render-target path exists, not the D3D12/Vulkan "pixel shader
interlock" EDRAM emulation. As a result:
- Blending happens in the host format. 7e3 (`k_2_10_10_10_FLOAT`) targets blend as rgba16float, and
  `k_16_16` / `k_16_16_16_16` fixed-point targets are stored and blended as float16. That is about 11 bits
  of precision, with a range of -32…32 that is not truncated to -1…1 (gap audit item 14).
- No programmable blending, so there is no exact emulation of Xenos blend quirks that the host blender
  cannot express.
- EDRAM aliasing between formats goes through explicit transfer passes. It is not bit-exact the way the
  ROV path is.

### No wireframe or point fill mode
WebGPU has no polygon mode. D3D12 maps `PA_SU_SC_MODE_CNTL` poly modes to `D3D12_FILL_MODE_WIREFRAME`
(`d3d12/pipeline_cache.cpp`). On the web, such draws are filled. Retail titles rarely use these modes.

### No cull distance
Vertex kill in AND mode (`PA_CL_CLIP_CNTL.vtx_kill_or == 0`) becomes OR: the vertex is killed through a
NaN position. Vulkan uses `CullDistance` for this.

## Threads

### Thread priority and affinity have no effect
`src/core/threading_web.cpp` records the priority and affinity and reports them back, but browsers
schedule Workers themselves. A title that relies on priority to avoid starvation in a spin-wait (a
high-priority thread spinning on a lower-priority producer) may run slower or stall where the console
would not. All ~25 threads compete equally for the cores the browser gives the tab.

### Hidden tabs
A hidden tab keeps running at full speed. The guest vblank timer keeps its phase across Chrome's coarse
background timer resolution (`GraphicsSystem` VSync thread), and a running AudioContext keeps the
renderer's audio thread alive. Measured on Halo 3: 30.0 ticks/s and 60 fps hidden, the same as visible.
- `--web_pause_when_hidden=true` stops the guest vblanks while the page is hidden or frozen. A game
  paced by vblank stops advancing, which saves CPU and GPU. It continues where it stopped when the tab is
  shown again. Do not use it on a System Link host: the peers time out. Measured on Halo 3 riverworld
  with an emulated hidden page (`gate1.mjs` `hide`/`show` actions): the game stopped at tick 1501 for
  the 60 s it was hidden and continued at 20-30 ticks/s afterwards. The tab still used 150-250% of a
  core while hidden, against 180-280% visible, because only the guest stops; the host threads keep
  running. A page frozen through CDP (`Page.setWebLifecycleState`) used 4.5%, and the game resumed
  normally after it was thawed.
- The System Link relay WebSocket lives on the browser main thread. Its keep-alive is judged by the time
  of the last ping instead of timers, because Chrome throttles main-thread timers in hidden tabs to once
  a minute after 5 minutes.

## Page lifecycle and Memory Saver

Chrome's Memory Saver (and tab discarding under memory pressure) can discard an inactive heavy tab. A
game tab with an 8 GB wasm memory is a prime target. The page is not told beforehand: the session is
simply gone, and the next visit reloads it with `document.wasDiscarded == true`. Mitigations:
- **An audible tab is not discarded.** The SDK's AudioContext counts once it runs (after the first user
  gesture) and plays sound. A muted tab or a suspended context does not protect the page.
- The user can add the site to "Always keep these sites active" (`chrome://settings/performance`).
- The page can warn about it. `src/ui/rex_web_pre.js` calls these hooks on the page's `Module`:
  - `Module.rexOnLifecycle(state, info)`: `state` is `hidden`, `visible` (`info.hidden_s`), `frozen`,
    `resumed`, `pagehide` (`info.persisted`), `pageshow` or `restored-after-discard`.
  - `Module.rexOnDiscardRisk({reason})`: the page became hidden while no audio is playing.
  - `Module.rexOnDiscarded()`: this load follows a discard.
- C++ can read the state without a lock through `rex/web_page.h`: `rex::web::GetPageState()`
  (`kPageHidden`, `kPageFrozen`), `IsPageHidden()` and `GetPageHiddenCount()`.

Saves go through WasmFS OPFS sync access handles, so they are on disk when the write returns. A discard
only loses unsaved progress.

## Crash reports

A wasm trap or `abort()` in a Worker used to kill only that thread, leaving "Aborted(...)" in the
console. Now every fatal path on any thread ends in one structured report delivered to the page on the
browser main thread:

```js
Module.rexOnFatal = (report) => { /* show "crashed, reload" + a copy button for report.text */ };
```

`report`: `{kind, message, note, js: {message, stack}, stack, thread: {tid, id}, threads, uptime_s,
build: {sdk, compiled}, cvars: {modified name: value}, log, page: {url, script, userAgent, time,
hidden}, text}`.

- `kind` is one of: `fatal` (`rex::FatalError`), `assert` (`rex_assert_fail`), `guest-exception` (an
  unhandled guest exception in `seh_rethrow`), `abort` (any other `std::abort`, `REX_FATAL*`,
  `std::terminate`), or `trap` (unreachable, out of bounds, null `call_indirect`, stack overflow).
- `stack` has function names, because release builds link with `--profiling-funcs`.
- `log` holds the last `--web_crash_report_log_lines` (default 120) log lines from a lock-free ring.
- `threads` comes from `rex::thread::DescribeThreadsForDiagnostics()`.
- Without a `rexOnFatal`, the page's `Module.onAbort(text)` is called instead, and the full report goes
  to `console.error`. It is also kept as `Module.rexLastFatal`.
- Only the first crash is reported; later errors are listed in `report.followups`.

To try a page's crash UX: `--web_crash_test=fatal|abort|trap|guest` with `--web_crash_test_delay_s=N`.

Mechanism: the pre-js defines `Module.onAbort` in every pthread Worker. Because of this, Emscripten no
longer proxies the page's `onAbort` from Workers. The pre-js also adds a Worker `error` listener. Both
call the `rex_web_crash_collect` export on the crashing thread, which builds the JSON and posts it to the
main thread. If a Worker dies before the report is built, a window `error` fallback sends a minimal
report after 1.5 s.

## Optional WebGPU features and their fallbacks

At device creation, the SDK logs one line, `WebGPU degraded features: …` (or `none`), and shows it in the
F3 debug overlay. The fallbacks and the cvars that force each one for testing:

| Missing feature | Fallback | Force it with |
|---|---|---|
| `texture-compression-bc` | DXT/DXN decompressed in compute (4-8x texture memory) | `--webgpu_texture_bc=false` |
| `texture-compression-bc-sliced-3d` | 3D BC textures decompressed | `--webgpu_texture_bc_sliced_3d=false` |
| `texture-formats-tier1` | 8_8_8_8_GAMMA render targets stored as rgba8 with PWL gamma in the shaders: blending happens in gamma space | `--gamma_render_target_as_unorm16=false` |
| `float32-filterable` | 32-bit float textures sampled nearest | `--webgpu_float32_filterable=false` |
| `clip-distances` | user clip planes ignored | `--webgpu_clip_distances=false` (so far it changes only the report line; the pipeline cache does not read it yet) |
| `depth-clip-control` | depth clipping cannot be turned off (`clip_disable`) | — |
| `depth32float-stencil8` | D24FS8 depth stored in depth24plus | — |
| `texture-component-swizzle` | swizzles applied in shaders (same result) | `--webgpu_texture_component_swizzle=false` |

## Input

- **`XInputGetKeystroke` `VK_PAD_*` synthesis** is missing in the SDK mouse-and-keyboard and web drivers.
  Natively, the project's Win32 hook covers it; on the web nothing does. Add it only if a title needs it.
- **Keyboard layout.** Keys are mapped by `KeyboardEvent.code` (US positions,
  `src/input/web/dom_input_bridge.cpp`), which is right for WASD on every layout. Keybind labels and the
  ImGui bind UI should use `navigator.keyboard.getLayoutMap()` to show the user's own key caps; they do
  not yet.
- **IME.** Composition text is not forwarded to `XamShowKeyboardUI`, which only decodes single code
  points.
- **Touch.** Only an opt-in on-screen gamepad: set `Module.rexTouchGamepad = true` (or a config with
  `show`, `canvas`, `opacity`, `size` and `layout`, documented in `src/ui/rex_web_pre.js`). It draws a
  virtual Standard Gamepad over the canvas, driven by Pointer Events, and adds it to
  `navigator.getGamepads()`, so the SDK reads it like a real pad. There is no direct touch-to-guest
  input, and wasm64 plus about 8 GB of memory keeps most phones out anyway.

## Files and memory

- **File mappings fail cleanly** (`src/core/memory_web.cpp`). Only the guest window used them, and it does
  not on the web.
- **Disc images, STFS and GOD containers do not stream.** `DiscImageDevice` and the STFS container
  device open their file with `MappedMemory::Open`. On the web, that is `mapped_memory_posix` over WasmFS,
  which reads the whole file into the wasm heap, and there is no HTTP-range variant. Serve titles as an
  extracted folder through `HttpRangeDevice` (`/data/` plus `manifest.json`). Extract title updates and
  DLC (STFS packages) on the server the same way, for example with xenia's or extract-xiso's extraction,
  and mount the result as an extra `HttpRangeDevice` root. A block-reader interface over HttpRangeDevice
  blocks (reusing its OPFS cache) would lift this.
- **Draw resolution scaling** (`draw_resolution_scale_x/y`) is not supported on WebGPU. It runs at 1x
  only.
