/**
 * @file        ui/window_web.cpp
 * @brief       Window/MenuItem for the Emscripten (wasm64) build: the page canvas.
 *
 * Threads (docs/port/webgpu.md section 1):
 * - Browser main thread: owns the DOM. A ResizeObserver on "#canvas" (installed
 *   from OpenImpl) computes the physical size (device-pixel content box, or CSS
 *   size * devicePixelRatio) and calls rex_web_canvas_resized, which calls
 *   emscripten_set_canvas_element_size. That writes the size into the canvas'
 *   shared size block and resizes the OffscreenCanvas on whichever pthread owns
 *   it. With WebGPU, the owner is the WebGPU device thread: it gets the canvas
 *   from the main() pthread at WebGPUDevice::Create. Without WebGPU, the owner is
 *   the main() pthread, which received it at startup (PROXY_TO_PTHREAD). Then the
 *   new size goes to the UI thread (OnActualSizeUpdate -> presenter
 *   OnSurfaceResize). The WebGPU presenter also reads the shared size on every
 *   animation frame on the device thread, so it reconfigures on the first frame
 *   after the resize without waiting for the UI thread.
 * - UI thread (main() pthread): the Window itself. Paint requests, input events
 *   (rex/ui/window_web.h, web::PostInputEvent) and resizes are posted to it with
 *   WindowedAppContext::CallInUIThreadDeferred. Blocking waits on Emscripten
 *   pthreads still process their mailbox, but the posted functions only run in
 *   the pending-function pump.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

#include <rex/ui/menu_item.h>
#include <rex/ui/surface_web.h>
#include <rex/ui/window.h>
#include <rex/ui/window_web.h>
#include <rex/ui/windowed_app_context.h>

namespace rex {
namespace ui {

namespace {

class WebWindow;

struct WebWindowGlobals {
  std::mutex mutex;
  // The open window receiving canvas events, and the context to post to it.
  WebWindow* open_window = nullptr;
  WindowedAppContext* app_context = nullptr;
  // Incremented on every open / close so posted functions can tell whether the
  // window they were posted for is still the open one.
  uint64_t generation = 0;
  web::WindowLifecycleCallback lifecycle;
  std::vector<web::InputEvent> input_queue;
  bool input_drain_posted = false;

  // Written by the browser main thread (resize observer), read anywhere.
  std::atomic<uint64_t> canvas_size{0};
  std::atomic<uint32_t> device_pixel_ratio_milli{1000};
};

WebWindowGlobals& Globals() {
  static WebWindowGlobals globals;
  return globals;
}

uint64_t PackSize(uint32_t width, uint32_t height) {
  return uint64_t(width) | (uint64_t(height) << 32);
}

// Runs fn(window) on the UI thread if the window open at the time of the call
// is still open then. Any thread. Returns false if no window is open.
bool PostToOpenWindow(std::function<void(WebWindow&)> fn);

// Browser main thread: install (or replace) the canvas resize observer. Returns
// the current physical size packed as width | height << 16, or 0 if there is no
// canvas in the DOM.
EM_JS(int, rexweb_install_canvas_observer, (), {
  if (typeof document == 'undefined') return 0;
  var canvas = Module['canvas'] || document.querySelector('#canvas');
  if (!canvas) return 0;
  var state = Module['rexCanvasResize'] || (Module['rexCanvasResize'] = {});
  if (state.observer) state.observer.disconnect();
  var notify = Module['_rex_web_canvas_resized'] || wasmExports['rex_web_canvas_resized'];
  var dpr = () => (globalThis.devicePixelRatio || 1);
  var last = [0, 0];
  var history = [];
  var apply = (w, h) => {
    w = Math.min(Math.max(Math.round(w), 0), 16384);
    h = Math.min(Math.max(Math.round(h), 0), 16384);
    if (w == last[0] && h == last[1]) return;
    // A canvas laid out at its intrinsic size (no CSS width) would grow on every
    // resize with devicePixelRatio != 1; pin its CSS size if that happens.
    var now = performance.now();
    history.push(now);
    while (history.length && now - history[0] > 1000) history.shift();
    if (history.length > 30) {
      console.warn('[rex] canvas resize feedback loop; pinning the canvas CSS size');
      canvas.style.width = canvas.clientWidth + 'px';
      canvas.style.height = canvas.clientHeight + 'px';
      history.length = 0;
    }
    last = [w, h];
    notify(w, h, Math.round(dpr() * 1000));
  };
  state.observer = new ResizeObserver((entries) => {
    for (var entry of entries) {
      var box = entry.devicePixelContentBoxSize && entry.devicePixelContentBoxSize[0];
      if (box) {
        apply(box.inlineSize, box.blockSize);
      } else {
        apply(entry.contentRect.width * dpr(), entry.contentRect.height * dpr());
      }
    }
  });
  try {
    state.observer.observe(canvas, {box: 'device-pixel-content-box'});
  } catch (e) {
    state.observer.observe(canvas);
  }
  var w = Math.min(Math.round(canvas.clientWidth * dpr()), 16384);
  var h = Math.min(Math.round(canvas.clientHeight * dpr()), 16384);
  if (!w || !h) {
    // Not laid out (display: none): keep the attribute size until observed.
    w = canvas.width;
    h = canvas.height;
  }
  return w | (h << 16);
});

EM_JS(void, rexweb_remove_canvas_observer, (), {
  var state = Module['rexCanvasResize'];
  if (state && state.observer) {
    state.observer.disconnect();
    state.observer = null;
  }
});

// The EM_JS functions above touch the DOM, so they must run on the browser main
// thread (the main runtime thread under PROXY_TO_PTHREAD).
int InstallCanvasObserver() {
  if (emscripten_is_main_runtime_thread()) {
    return rexweb_install_canvas_observer();
  }
  int result = 0;
  if (!emscripten_proxy_sync(
          emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(),
          [](void* arg) { *static_cast<int*>(arg) = rexweb_install_canvas_observer(); }, &result)) {
    return 0;
  }
  return result;
}

void RemoveCanvasObserver() {
  if (emscripten_is_main_runtime_thread()) {
    rexweb_remove_canvas_observer();
    return;
  }
  emscripten_proxy_async(
      emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(),
      [](void*) { rexweb_remove_canvas_observer(); }, nullptr);
}

class WebWindow final : public Window {
 public:
  WebWindow(WindowedAppContext& app_context, const std::string_view title,
            uint32_t desired_logical_width, uint32_t desired_logical_height)
      : Window(app_context, title, desired_logical_width, desired_logical_height) {}

  ~WebWindow() override {
    Unregister();
    EnterDestructor();
  }

  // UI thread.
  void HandleCanvasResize() {
    uint64_t packed = Globals().canvas_size.load(std::memory_order_acquire);
    WindowDestructionReceiver destruction_receiver(this);
    OnActualSizeUpdate(uint32_t(packed), uint32_t(packed >> 32), destruction_receiver);
  }

  // UI thread.
  void HandlePaint(bool force_paint) {
    paint_posted_.store(false, std::memory_order_relaxed);
    OnPaint(force_paint);
  }

  // UI thread. Returns false if the window has been destroyed or closed.
  bool DispatchInput(const web::InputEvent& e) {
    using Type = web::InputEvent::Type;
    WindowDestructionReceiver destruction_receiver(this);
    switch (e.type) {
      case Type::kKeyDown:
      case Type::kKeyUp:
      case Type::kKeyChar: {
        KeyEvent key_event(this, e.virtual_key, e.repeat_count, e.prev_state, e.shift, e.ctrl,
                           e.alt, e.super);
        if (e.type == Type::kKeyDown) {
          OnKeyDown(key_event, destruction_receiver);
        } else if (e.type == Type::kKeyUp) {
          OnKeyUp(key_event, destruction_receiver);
        } else {
          OnKeyChar(key_event, destruction_receiver);
        }
      } break;
      case Type::kMouseDown:
      case Type::kMouseMove:
      case Type::kMouseUp:
      case Type::kMouseWheel: {
        MouseEvent mouse_event(this, e.button, e.x, e.y, e.scroll_x, e.scroll_y);
        if (e.type == Type::kMouseDown) {
          OnMouseDown(mouse_event, destruction_receiver);
        } else if (e.type == Type::kMouseMove) {
          OnMouseMove(mouse_event, destruction_receiver);
        } else if (e.type == Type::kMouseUp) {
          OnMouseUp(mouse_event, destruction_receiver);
        } else {
          OnMouseWheel(mouse_event, destruction_receiver);
        }
      } break;
      case Type::kFocus:
      case Type::kBlur:
        OnFocusUpdate(e.type == Type::kFocus, destruction_receiver);
        break;
    }
    return !destruction_receiver.IsWindowDestroyedOrClosed();
  }

 protected:
  uint32_t GetLatestDpiImpl() const override {
    uint32_t milli = Globals().device_pixel_ratio_milli.load(std::memory_order_relaxed);
    return std::max(uint32_t(1), (GetMediumDpi() * milli + 500) / 1000);
  }

  bool OpenImpl() override {
    WebWindowGlobals& globals = Globals();
    {
      std::lock_guard<std::mutex> lock(globals.mutex);
      globals.open_window = this;
      globals.app_context = &app_context();
      ++globals.generation;
      globals.input_queue.clear();
      globals.input_drain_posted = false;
    }
    registered_ = true;

    int packed = InstallCanvasObserver();
    uint32_t width = uint32_t(packed) & 0xFFFF;
    uint32_t height = (uint32_t(packed) >> 16) & 0xFFFF;
    if (!width || !height) {
      std::fprintf(stderr,
                   "ui::WebWindow: no #canvas in the page; using the desired size %ux%u "
                   "without presentation\n",
                   GetDesiredLogicalWidth(), GetDesiredLogicalHeight());
      width = GetDesiredLogicalWidth();
      height = GetDesiredLogicalHeight();
    } else {
      // Only publish if the observer hasn't delivered something newer already.
      uint64_t expected = 0;
      globals.canvas_size.compare_exchange_strong(expected, PackSize(width, height),
                                                  std::memory_order_acq_rel);
      uint64_t current = globals.canvas_size.load(std::memory_order_acquire);
      width = uint32_t(current);
      height = uint32_t(current >> 32);
    }

    WindowDestructionReceiver destruction_receiver(this);
    OnActualSizeUpdate(width, height, destruction_receiver);
    if (destruction_receiver.IsWindowDestroyedOrClosed()) {
      return true;
    }
    OnFocusUpdate(true, destruction_receiver);
    if (destruction_receiver.IsWindowDestroyedOrClosed()) {
      return true;
    }
    web::WindowLifecycleCallback lifecycle;
    {
      std::lock_guard<std::mutex> lock(globals.mutex);
      lifecycle = globals.lifecycle;
    }
    if (lifecycle) {
      lifecycle(true, this);
    }
    return true;
  }

  void RequestCloseImpl() override {
    web::WindowLifecycleCallback lifecycle;
    {
      std::lock_guard<std::mutex> lock(Globals().mutex);
      lifecycle = Globals().lifecycle;
    }
    if (lifecycle && registered_) {
      lifecycle(false, this);
    }
    WindowDestructionReceiver destruction_receiver(this);
    OnBeforeClose(destruction_receiver);
    if (destruction_receiver.IsWindowDestroyed()) {
      return;
    }
    Unregister();
    OnAfterClose();
  }

  std::unique_ptr<Surface> CreateSurfaceImpl(Surface::TypeFlags allowed_types) override {
    if (!(allowed_types & Surface::kTypeFlag_WebCanvas)) {
      return nullptr;
    }
    return std::make_unique<WebCanvasSurface>(web::kCanvasSelector, Globals().canvas_size);
  }

  void RequestPaintImpl() override {
    // Any thread; coalesce until the UI thread has handled the last request.
    if (paint_posted_.exchange(true, std::memory_order_relaxed)) {
      return;
    }
    if (!PostToOpenWindow([](WebWindow& window) { window.HandlePaint(false); })) {
      paint_posted_.store(false, std::memory_order_relaxed);
    }
  }

 private:
  void Unregister() {
    if (!registered_) {
      return;
    }
    registered_ = false;
    WebWindowGlobals& globals = Globals();
    {
      std::lock_guard<std::mutex> lock(globals.mutex);
      if (globals.open_window == this) {
        globals.open_window = nullptr;
        globals.app_context = nullptr;
        ++globals.generation;
        globals.input_queue.clear();
        globals.input_drain_posted = false;
      }
    }
    RemoveCanvasObserver();
  }

  bool registered_ = false;
  std::atomic<bool> paint_posted_{false};
};

bool PostToOpenWindow(std::function<void(WebWindow&)> fn) {
  WebWindowGlobals& globals = Globals();
  WindowedAppContext* app_context;
  uint64_t generation;
  {
    std::lock_guard<std::mutex> lock(globals.mutex);
    if (!globals.open_window || !globals.app_context) {
      return false;
    }
    app_context = globals.app_context;
    generation = globals.generation;
  }
  return app_context->CallInUIThreadDeferred([fn = std::move(fn), generation]() {
    WebWindow* window;
    {
      std::lock_guard<std::mutex> lock(Globals().mutex);
      if (!Globals().open_window || Globals().generation != generation) {
        return;
      }
      window = Globals().open_window;
    }
    // Windows are destroyed only on this (UI) thread, so it stays valid here.
    fn(*window);
  });
}

void DrainInputQueue(WebWindow& window) {
  std::vector<web::InputEvent> events;
  {
    std::lock_guard<std::mutex> lock(Globals().mutex);
    events.swap(Globals().input_queue);
    Globals().input_drain_posted = false;
  }
  for (const web::InputEvent& event : events) {
    if (!window.DispatchInput(event)) {
      return;
    }
  }
}

class WebMenuItem final : public MenuItem {
 public:
  WebMenuItem(Type type, const std::string& text, const std::string& hotkey,
              std::function<void()> callback)
      : MenuItem(type, text, hotkey, std::move(callback)) {}
};

}  // namespace

void WebCanvasSurface::RequestPaintFromAnyThread(bool force_paint) {
  PostToOpenWindow([force_paint](WebWindow& window) { window.HandlePaint(force_paint); });
}

namespace web {

bool PostInputEvent(const InputEvent& event) {
  WebWindowGlobals& globals = Globals();
  bool post_drain;
  {
    std::lock_guard<std::mutex> lock(globals.mutex);
    if (!globals.open_window) {
      return false;
    }
    globals.input_queue.push_back(event);
    post_drain = !globals.input_drain_posted;
    globals.input_drain_posted = true;
  }
  if (post_drain && !PostToOpenWindow(DrainInputQueue)) {
    std::lock_guard<std::mutex> lock(globals.mutex);
    globals.input_drain_posted = false;
    return false;
  }
  return true;
}

void SetWindowLifecycleCallback(WindowLifecycleCallback callback) {
  Window* open_window;
  {
    std::lock_guard<std::mutex> lock(Globals().mutex);
    Globals().lifecycle = callback;
    open_window = Globals().open_window;
  }
  if (callback && open_window) {
    callback(true, open_window);
  }
}

void GetCanvasPhysicalSize(uint32_t& width_out, uint32_t& height_out) {
  uint64_t packed = Globals().canvas_size.load(std::memory_order_acquire);
  width_out = uint32_t(packed);
  height_out = uint32_t(packed >> 32);
}

}  // namespace web

std::unique_ptr<Window> Window::Create(WindowedAppContext& app_context,
                                       const std::string_view title, uint32_t desired_logical_width,
                                       uint32_t desired_logical_height) {
  return std::make_unique<WebWindow>(app_context, title, desired_logical_width,
                                     desired_logical_height);
}

std::unique_ptr<ui::MenuItem> MenuItem::Create(Type type, const std::string& text,
                                               const std::string& hotkey,
                                               std::function<void()> callback) {
  return std::make_unique<WebMenuItem>(type, text, hotkey, std::move(callback));
}

}  // namespace ui
}  // namespace rex

// Browser main thread (called by the resize observer): applies the new physical
// canvas size and tells the UI thread.
extern "C" EMSCRIPTEN_KEEPALIVE void rex_web_canvas_resized(int width, int height,
                                                            int device_pixel_ratio_milli) {
  using namespace rex::ui;
  if (width <= 0 || height <= 0) {
    width = 0;
    height = 0;
  } else {
    // Resizes the OffscreenCanvas on its owner thread (asynchronously) and
    // updates the shared size block that emscripten_get_canvas_element_size
    // reads on every thread.
    emscripten_set_canvas_element_size(web::kCanvasSelector, width, height);
  }
  Globals().device_pixel_ratio_milli.store(
      uint32_t(device_pixel_ratio_milli > 0 ? device_pixel_ratio_milli : 1000),
      std::memory_order_relaxed);
  Globals().canvas_size.store(PackSize(uint32_t(width), uint32_t(height)),
                              std::memory_order_release);
  PostToOpenWindow([](WebWindow& window) { window.HandleCanvasResize(); });
}
