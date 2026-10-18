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
 * - Fullscreen (Window::SetFullscreen, --fullscreen, F11 in the DOM input bridge):
 *   ApplyNewFullscreen proxies to the browser main thread, which calls
 *   requestFullscreen on Module.rexFullscreenTarget (an element or a selector; default
 *   the canvas). Without user activation (e.g. --fullscreen at startup) the request is
 *   armed for the next pointerdown / keydown on the page. In fullscreen, Keyboard Lock
 *   (--web_keyboard_lock, codes from --web_keyboard_lock_codes, default Escape) makes Esc
 *   reach the game instead of leaving fullscreen and Pointer Lock (holding Esc still
 *   exits). fullscreenchange is reported back to the UI thread (desired fullscreen state)
 *   and to Module.rexOnFullscreenChange(active, keyboardLocked) if the page sets it.
 *   State for pages/tests: Module.rexFullscreen.{want, armed, active, keyboardLocked,
 *   changes, lastError}.
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

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/menu_item.h>
#include <rex/ui/surface_web.h>
#include <rex/ui/window.h>
#include <rex/ui/window_web.h>
#include <rex/ui/windowed_app_context.h>

REXCVAR_DEFINE_BOOL(web_keyboard_lock, true, "UI/Web",
                    "In fullscreen, capture the keys of web_keyboard_lock_codes with the Keyboard "
                    "Lock API (Chromium) so Esc reaches the game instead of leaving fullscreen "
                    "and Pointer Lock (hold Esc to leave)");
REXCVAR_DEFINE_STRING(web_keyboard_lock_codes, "Escape", "UI/Web",
                      "Comma-separated KeyboardEvent.code values locked in fullscreen (e.g. "
                      "Escape,Tab,MetaLeft,MetaRight); empty = every key the browser allows");

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

// Browser main thread. on: 1 = enter, 0 = leave, 2 = toggle (inside a user gesture, F11).
// keyboard_lock / codes (comma-separated, as a double pointer: no wasm64 BigInt question):
// Keyboard Lock while in fullscreen. Installs the page-side state on first use.
EM_JS(void, rexweb_set_fullscreen, (int on, int keyboard_lock, double codes_ptr), {
  if (typeof document == 'undefined') return;
  var fs = Module['rexFullscreen'];
  if (!fs) {
    fs = Module['rexFullscreen'] = {want : false, armed : false, active : false,
                                    keyboardLock : true, codes : ['Escape'],
                                    keyboardLocked : false, changes : 0, lastError : ""};
    var notify = Module['_rex_web_fullscreen_changed'] || wasmExports['rex_web_fullscreen_changed'];
    fs.target = function() {
      var t = Module['rexFullscreenTarget'];
      if (typeof t == 'string') t = document.querySelector(t);
      return t || Module['canvas'] || document.querySelector('#canvas');
    };
    fs.arm = function(why) {
      if (!fs.want || fs.armed) return;
      fs.armed = true;
      console.log('[rex] fullscreen: ' + why + '; entering on the next click or key press');
    };
    fs.request = function() {
      var el = fs.target();
      if (!el || !el.requestFullscreen) {
        fs.lastError = 'requestFullscreen unavailable';
        return;
      }
      if (document.fullscreenElement === el) return;
      var p;
      try {
        p = el.requestFullscreen({navigationUI : 'hide'});
      } catch (e) {
        p = Promise.reject(e);
      }
      Promise.resolve(p).then(function() { fs.armed = false; }, function(e) {
        fs.lastError = String(e);
        fs.arm('request refused (' + (e && e.name) + ')');
      });
    };
    var gesture = function(ev) {
      // Esc is not an activation-triggering key.
      if (!fs.armed || !fs.want || (ev.type == 'keydown' && ev.code == 'Escape')) return;
      fs.armed = false;
      fs.request();
    };
    addEventListener('pointerdown', gesture, true);
    addEventListener('keydown', gesture, true);
    document.addEventListener('fullscreenchange', function() {
      var active = !!document.fullscreenElement;
      fs.active = active;
      fs.changes++;
      var report = function() {
        if (notify) notify(active ? 1 : 0);
        var cb = Module['rexOnFullscreenChange'];
        if (typeof cb == 'function') {
          try {
            cb(active, fs.keyboardLocked);
          } catch (e) {
            console.error(e);
          }
        }
      };
      if (!active) {
        // Left by the user (held Esc, browser UI) or by us: stop wanting it.
        fs.want = false;
        fs.armed = false;
        fs.keyboardLocked = false;
        try {
          if (navigator.keyboard && navigator.keyboard.unlock) navigator.keyboard.unlock();
        } catch (e) {
        }
        report();
        return;
      }
      fs.want = true;
      if (fs.keyboardLock && navigator.keyboard && navigator.keyboard.lock) {
        navigator.keyboard.lock(fs.codes.length ? fs.codes : undefined).then(function() {
          fs.keyboardLocked = true;
          console.log('[rex] fullscreen: keyboard locked [' + (fs.codes.join(',') || 'all') +
                      ']; hold Esc to leave');
          report();
        }, function(e) {
          fs.keyboardLocked = false;
          fs.lastError = String(e);
          console.warn('[rex] fullscreen: Keyboard Lock refused (' + e + '); Esc leaves fullscreen');
          report();
        });
      } else {
        report();
      }
    });
    document.addEventListener('fullscreenerror', function() {
      fs.lastError = 'fullscreenerror';
      fs.arm('request failed');
    });
  }
  fs.keyboardLock = !!keyboard_lock;
  var codes = codes_ptr ? UTF8ToString(codes_ptr) : "";
  fs.codes = codes.split(',').map(function(s) { return s.trim(); }).filter(Boolean);
  if (on == 2) on = document.fullscreenElement ? 0 : 1;
  fs.want = !!on;
  if (on) {
    var ua = navigator.userActivation;
    if (ua && !ua.isActive) {
      fs.arm('no user activation');
    } else {
      fs.request();
    }
  } else {
    fs.armed = false;
    if (document.fullscreenElement && document.exitFullscreen) {
      document.exitFullscreen().catch(function() {});
    }
  }
});
EM_JS_DEPS(rex_window_web, "$UTF8ToString");

struct FullscreenRequest {
  int on;
  int keyboard_lock;
  std::string codes;
};

// Any thread (the DOM input bridge calls it on the browser main thread inside the F11
// gesture, which must not be lost to an asynchronous hop).
void SetBrowserFullscreen(int on) {
  if (emscripten_is_main_runtime_thread()) {
    // No allocation here (the codes are a short string, small-string optimized).
    const std::string codes(REXCVAR_GET(web_keyboard_lock_codes));
    rexweb_set_fullscreen(on, REXCVAR_GET(web_keyboard_lock) ? 1 : 0,
                          double(uintptr_t(codes.c_str())));
    return;
  }
  auto* request = new FullscreenRequest{on, REXCVAR_GET(web_keyboard_lock) ? 1 : 0,
                                        std::string(REXCVAR_GET(web_keyboard_lock_codes))};
  auto run = [](void* arg) {
    auto* r = static_cast<FullscreenRequest*>(arg);
    rexweb_set_fullscreen(r->on, r->keyboard_lock, double(uintptr_t(r->codes.c_str())));
    delete r;
  };
  if (!emscripten_proxy_async(emscripten_proxy_get_system_queue(),
                              emscripten_main_runtime_thread_id(), run, request)) {
    delete request;
  }
}

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

  // UI thread: the browser entered or left fullscreen (also by the user: held Esc, browser
  // UI), so the desired state follows the page.
  void HandleFullscreenChange(bool active) {
    if (IsFullscreen() != active) {
      REXLOG_INFO("web window: fullscreen {}", active ? "entered" : "left");
    }
    OnDesiredFullscreenUpdate(active);
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
  void ApplyNewFullscreen() override { SetBrowserFullscreen(IsFullscreen() ? 1 : 0); }

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
    if (IsFullscreen()) {
      // --fullscreen: no user activation yet at startup, so this arms the request for the
      // first click or key press.
      SetBrowserFullscreen(1);
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

namespace {
std::atomic<bool> g_ui_wants_pointer{false};
}  // namespace

void SetUIWantsPointer(bool wants) {
  if (g_ui_wants_pointer.exchange(wants, std::memory_order_acq_rel) == wants || !wants) {
    return;
  }
  // The pointer lock is the browser main thread's (async: the UI thread must not
  // wait on the main thread).
  MAIN_THREAD_ASYNC_EM_ASM({
    if (typeof document != 'undefined' && document.pointerLockElement) {
      document.exitPointerLock();
    }
  });
}

bool UIWantsPointer() { return g_ui_wants_pointer.load(std::memory_order_acquire); }

void ToggleFullscreenFromGesture() { SetBrowserFullscreen(2); }

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

// Browser main thread (fullscreenchange): tells the UI thread.
extern "C" EMSCRIPTEN_KEEPALIVE void rex_web_fullscreen_changed(int active) {
  using namespace rex::ui;
  PostToOpenWindow([active](WebWindow& window) { window.HandleFullscreenChange(active != 0); });
}
