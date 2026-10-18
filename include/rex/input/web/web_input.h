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
 *     and published per XInput user slot through a seqlock.
 *
 * Gamepads: every navigator.getGamepads() entry is read with a layout picked from its
 * mapping and id: "standard" (the W3C mapping: Chrome/Edge XInput pads, DualShock 4,
 * DualSense; triggers = buttons 6/7 with an analog value), or for unmapped ("") pads
 * by USB vendor id: Sony raw HID (Square Cross Circle Triangle ..., triggers on axes
 * 3/4, right stick 2/5, hat), Xbox Bluetooth raw HID (A B - X Y - LB RB ..., triggers
 * on axes 3/4), older Xbox Bluetooth firmware and anything else (standard button
 * positions; triggers = axes resting at -1 and/or buttons 6/7; right stick = the next
 * two axes; a hat axis, neutral outside [-1, 1], drives the d-pad). Trigger axes read
 * -1..1 once seen below -0.5, else 0..1. The pads are then ordered for the XInput user
 * slots: pads that have been used (button, trigger or stick), in the order of their
 * first use, then standard-mapped pads, then by browser index. So an idle second
 * device or virtual pad at navigator index 0 no longer takes user 0 from the pad being
 * played with. Connect/disconnect/assignment events are queued for the consumer to log
 * (PopPadEvent; the web gamepad driver logs them).
 * Browsers expose a gamepad to a page only after one of its buttons is pressed while
 * the page is visible (Firefox: and focused), so a player has to press a button once
 * after the page loads before the pad shows up. Gamepad input reaches the game while
 * the page is visible, focused or not (web_gamepad_requires_focus=true restores the
 * focus requirement); a hidden page gets none (no rAF there, the state would be stale).
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
// navigator.getGamepads() entries considered (Chrome lists 4, others may list more).
constexpr int kMaxBrowserGamepads = 8;

// How a browser pad's buttons/axes are read (see the file comment).
enum PadLayout : uint8_t {
  kPadLayoutStandard = 0,
  kPadLayoutSonyHid = 1,
  kPadLayoutXboxHid = 2,
  kPadLayoutXboxHidLegacy = 3,
  kPadLayoutGeneric = 4,
};

// Gamepad change queued by the poll loop for logging.
struct PadEvent {
  enum Kind : uint8_t {
    kConnected = 0,     // browser_index: layout, ids and axes below
    kDisconnected = 1,  // browser_index
    kAssigned = 2,      // XInput user now reads browser_index (-1: none)
  };
  Kind kind = kConnected;
  int8_t browser_index = -1;
  int8_t user = -1;
  uint8_t layout = 0;
  uint16_t vendor = 0, product = 0;
  int8_t lt_axis = -1, rt_axis = -1, rx_axis = -1, ry_axis = -1, hat_axis = -1;
  char id[64] = {};
};

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
// Whether the gamepad driver passes pad input to the game now (page visible, and
// focused if web_gamepad_requires_focus).
bool GamepadInputAllowed();

// slot = XInput user index; the browser pad behind it is GamepadBrowserIndex(slot).
bool ReadGamepad(uint32_t slot, GamepadSnapshot* out);
int GamepadBrowserIndex(uint32_t user_index);
// Consumer side of the pad event queue (single consumer at a time; callers serialize).
bool PopPadEvent(PadEvent* out);
const char* PadLayoutName(uint8_t layout);
BridgeStats GetBridgeStats();

// Gamepad API driver: XInput user index N = the Nth pad in the bridge's order (see the
// file comment). No deadzone is applied (the game applies its own).
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
  void LogPadEvents();

  uint32_t last_rumble_[kMaxGamepads] = {};
};

#endif  // REX_PLATFORM_WEB

}  // namespace rex::input::web
