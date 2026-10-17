/**
 * @file        rex/input/web/web_input.h
 * @brief       Browser input for the Emscripten (wasm64) build: DOM keyboard/mouse
 *              bridge into the MnK driver, and a Gamepad API input driver.
 *
 * Threading (PROXY_TO_PTHREAD): DOM events and navigator.getGamepads() only exist on
 * the browser main thread, while XamInputGetState runs on guest threads. All DOM
 * listeners are registered with EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD, so
 * the callbacks run synchronously on the browser main thread (which must never
 * block) and only touch lock-free state:
 *   - keyboard / mouse-button / focus events -> SPSC ring (producer: browser main
 *     thread, consumer: MnkInputDriver under its state mutex), with an atomic key
 *     bitmap as the resync source if the ring overflows;
 *   - pointer-lock mouse motion -> atomic accumulators (coalesced, never dropped);
 *   - gamepads -> polled by a requestAnimationFrame loop on the browser main thread
 *     and published per slot through a seqlock.
 * The bridge registers its own DOM listeners on the canvas element (by CSS
 * selector) and owns Pointer Lock. It additionally forwards every event to the web
 * window through rex::ui::web::PostInputEvent (rex/ui/window_web.h, WP6), which
 * dispatches them on the UI thread to the window's listeners (ImGui overlay etc.).
 * On web the MnK driver ignores those window-delivered key/mouse events and reads
 * the ring instead (lock-free, tap-preserving, Pointer Lock movementX/Y deltas), so
 * nothing is applied twice.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>

#include <rex/input/input_driver.h>
#include <rex/platform.h>

namespace rex::input::web {

// Number of gamepad slots exposed (XInput user indices 0-3).
constexpr uint32_t kMaxGamepads = 4;

// One DOM event queued for the MnK driver.
struct DomEvent {
  enum Type : uint8_t {
    kKey = 0,     // vk = rex::ui::VirtualKey value (incl. kLButton/kRButton/kMButton)
    kFocus = 1,   // down = 1 gained focus, 0 lost focus (blur / hidden tab)
  };
  uint8_t type;
  uint8_t down;
  uint16_t vk;
};

// Neutral-centered XInput-layout gamepad state as published by the poll loop.
struct GamepadSnapshot {
  bool connected = false;
  uint16_t buttons = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t thumb_lx = 0;
  int16_t thumb_ly = 0;
  int16_t thumb_rx = 0;
  int16_t thumb_ry = 0;
  uint32_t packet_number = 0;  // incremented by the poll loop whenever the state changes
};

// Bridge counters (diagnostics and tests).
struct BridgeStats {
  uint32_t installed;
  uint32_t key_events;
  uint32_t mouse_events;
  uint32_t focus_events;
  uint32_t dropped_events;   // ring overflow (consumer resyncs from the key bitmap)
  uint32_t gamepad_polls;
  uint32_t pointer_locked;
  uint32_t has_focus;
  uint32_t forwarded_events;  // also accepted by rex::ui::web::PostInputEvent (window open)
};

#if REX_PLATFORM_WEB

// Installs the DOM listeners on the browser main thread and starts the gamepad poll
// loop. Idempotent, callable from any thread (blocks until installed when called off
// the main thread). canvas_selector: CSS selector of the game canvas ("#canvas").
bool InstallBrowserInput(const char* canvas_selector = "#canvas");

// Consumer side of the DOM ring (single consumer at a time; callers serialize).
bool PeekDomEvent(DomEvent* out);
void PopDomEvent();
// True once if the ring overflowed since the last call; the consumer must then
// resync its key state from SnapshotKeyBitmap().
bool TakeOverflow();
void SnapshotKeyBitmap(bool (&key_down)[256]);
// Accumulated pointer-lock (or, if web_mouse_look_requires_pointer_lock=false, plain)
// mouse motion in CSS pixels since the last call.
void TakeMouseDelta(int32_t* dx, int32_t* dy);
bool HasFocus();

bool ReadGamepad(uint32_t slot, GamepadSnapshot* out);
BridgeStats GetBridgeStats();

// Gamepad API driver: XInput user index N = navigator.getGamepads()[N] (standard
// mapping). No deadzone is applied (the game applies its own).
class WebGamepadInputDriver final : public InputDriver {
 public:
  explicit WebGamepadInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~WebGamepadInputDriver() override;

  X_STATUS Setup() override;

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;

 private:
  uint32_t last_rumble_[kMaxGamepads] = {};
};

#endif  // REX_PLATFORM_WEB

}  // namespace rex::input::web
