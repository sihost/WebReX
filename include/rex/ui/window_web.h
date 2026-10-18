/**
 * @file        rex/ui/window_web.h
 * @brief       Web (Emscripten) window hooks: canvas size and the input-event sink.
 *
 * The web build has one window, backed by the page's "#canvas" (Module.canvas).
 * src/ui/window_web.cpp implements it:
 * - The canvas size follows its CSS box times devicePixelRatio. A ResizeObserver
 *   on the browser main thread calls emscripten_set_canvas_element_size (that
 *   resizes the OffscreenCanvas on whichever pthread owns it, the WebGPU device
 *   thread when WebGPU presents) and then posts Window::OnActualSizeUpdate to the
 *   UI thread.
 * - Surface: WebCanvasSurface (rex/ui/surface_web.h).
 * - Paint requests are posted to the UI thread (Window::OnPaint).
 *
 * INPUT HOOK (for the DOM input bridge, WS-F, src/input/web/): DOM events
 * fire on the browser main thread while the Window lives on the UI pthread
 * (main() under PROXY_TO_PTHREAD). The bridge converts each event to a
 * web::InputEvent and calls web::PostInputEvent from any thread. Events are queued
 * in order and dispatched on the UI thread to Window::OnKeyDown/OnKeyUp/
 * OnKeyChar/OnMouseDown/OnMouseMove/OnMouseUp/OnMouseWheel/OnFocusUpdate. From
 * there they reach the window's listeners and input listeners, e.g. the MnK
 * input driver registered through Window::AddInputListener. Mouse coordinates are
 * in canvas physical pixels (CSS offset * canvas.width / clientWidth), the same
 * space as the surface size. web::SetWindowLifecycleCallback reports when the
 * window opens or closes so the bridge can install or remove its DOM listeners.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <functional>

#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>

namespace rex {
namespace ui {

class Window;

namespace web {

// CSS selector of the canvas the web window presents to.
constexpr const char kCanvasSelector[] = "#canvas";

struct InputEvent {
  enum class Type : uint8_t {
    kKeyDown,
    kKeyUp,
    kKeyChar,
    kMouseDown,
    kMouseMove,
    kMouseUp,
    kMouseWheel,
    // Canvas / page focus gained or lost (blur must clear held keys downstream).
    kFocus,
    kBlur,
  };
  Type type = Type::kMouseMove;

  // Keys (kKeyDown/kKeyUp/kKeyChar). For kKeyChar, virtual_key carries the
  // character code like the Win32 WM_CHAR path does.
  VirtualKey virtual_key = VirtualKey::kNone;
  int repeat_count = 1;
  bool prev_state = false;  // Key was already down (auto-repeat).
  bool shift = false;
  bool ctrl = false;
  bool alt = false;
  bool super = false;

  // Mouse. Position in canvas physical pixels; wheel in
  // MouseEvent::kScrollPerDetent units, positive scroll_y = away from the user.
  MouseEvent::Button button = MouseEvent::Button::kNone;
  int32_t x = 0;
  int32_t y = 0;
  int32_t scroll_x = 0;
  int32_t scroll_y = 0;
};

// Any thread (including the browser main thread). Queues the event for the open
// web window and wakes the UI thread. Returns false (event dropped) if no web
// window is open.
bool PostInputEvent(const InputEvent& event);

// Called on the UI thread with opened = true right after the web window opens
// (canvas ready, size known) and with opened = false before it closes. Set it
// before the window opens; if a window is already open when the callback is
// set, it is called with opened = true immediately on the calling thread.
// Pass an empty function to unregister.
using WindowLifecycleCallback = std::function<void(bool opened, Window* window)>;
void SetWindowLifecycleCallback(WindowLifecycleCallback callback);

// Any thread. Current canvas size in physical pixels, as last published by the
// resize observer (0x0 before the window opens).
void GetCanvasPhysicalSize(uint32_t& width_out, uint32_t& height_out);

// Whether the UI (an ImGui overlay window that takes input) owns the mouse. Set by
// the ImGui drawer on the UI thread every UI frame; read by the DOM input bridge
// on the browser main thread, which then does not request Pointer Lock on a canvas
// click (the overlay needs a visible cursor and absolute positions). Turning it on
// releases an active Pointer Lock (asynchronously, on the browser main thread).
void SetUIWantsPointer(bool wants);
bool UIWantsPointer();

// Browser main thread, inside a user gesture (the DOM input bridge's F11 with
// --web_fullscreen_key): enters fullscreen (Module.rexFullscreenTarget, default the canvas)
// or leaves it. The window's desired fullscreen state follows through fullscreenchange.
// Window::SetFullscreen works from the UI thread too (armed for the next gesture when
// the page has no user activation).
void ToggleFullscreenFromGesture();

}  // namespace web
}  // namespace ui
}  // namespace rex
