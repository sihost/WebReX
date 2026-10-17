/**
 * @file        input/web/dom_input_bridge.cpp
 * @brief       Browser main-thread input bridge (DOM keyboard/mouse/focus + Gamepad
 *              API polling) for the Emscripten build. See rex/input/web/web_input.h.
 *
 * Everything in the "producer" section runs on the browser main thread only, from
 * html5 callbacks registered with EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD and
 * from a requestAnimationFrame loop started there. That thread must never block,
 * so the producer only uses atomics (no mutex, no allocation).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/input/web/web_input.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

#include <rex/cvar.h>
#include <rex/input/flags.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window_web.h>

REXCVAR_DEFINE_BOOL(web_pointer_lock, true, "Input/Web",
                    "Request Pointer Lock on the canvas when it is clicked (mouse look)");
REXCVAR_DEFINE_BOOL(web_mouse_look_requires_pointer_lock, true, "Input/Web",
                    "Only feed mouse motion to the right stick while the pointer is locked");

namespace rex::input::web {

using rex::ui::VirtualKey;

namespace {

// ---------------------------------------------------------------------------
// Shared state (producer: browser main thread; consumers: any thread).
// ---------------------------------------------------------------------------

constexpr uint32_t kRingSize = 1024;  // power of two
uint32_t g_ring[kRingSize];
std::atomic<uint32_t> g_ring_head{0};  // written by producer
std::atomic<uint32_t> g_ring_tail{0};  // written by consumer
std::atomic<bool> g_overflow{false};

std::atomic<uint32_t> g_key_bits[256 / 32];  // authoritative key/button bitmap
std::atomic<int32_t> g_mouse_dx{0};
std::atomic<int32_t> g_mouse_dy{0};
std::atomic<bool> g_has_focus{true};
std::atomic<bool> g_pointer_locked{false};

std::atomic<uint32_t> g_installed{0};
std::atomic<uint32_t> g_key_events{0};
std::atomic<uint32_t> g_mouse_events{0};
std::atomic<uint32_t> g_focus_events{0};
std::atomic<uint32_t> g_dropped_events{0};
std::atomic<uint32_t> g_gamepad_polls{0};
std::atomic<uint32_t> g_forwarded_events{0};

struct PadSlot {
  std::atomic<uint32_t> seq{0};
  std::atomic<uint64_t> w0{0};  // buttons | lt<<16 | rt<<24 | lx<<32 | ly<<48
  std::atomic<uint64_t> w1{0};  // rx | ry<<16 | packet<<32 (31 bits) | connected<<63
};
PadSlot g_pads[kMaxGamepads];
// Producer-private previous values (browser main thread only).
uint64_t g_pad_last_w0[kMaxGamepads];
uint32_t g_pad_last_rxry[kMaxGamepads];
bool g_pad_last_connected[kMaxGamepads];
uint32_t g_pad_packet[kMaxGamepads];

// Mouse buttons whose press was consumed to acquire pointer lock (their release is
// swallowed too). Producer-private.
uint32_t g_swallowed_buttons = 0;

// Fixed buffer: the browser main thread must not allocate (malloc may spin on a lock).
char g_canvas_selector[64] = "#canvas";
int g_canvas_result = 0;

// ---------------------------------------------------------------------------
// Producer helpers.
// ---------------------------------------------------------------------------

bool KeyBit(uint16_t vk) {
  return (g_key_bits[vk >> 5].load(std::memory_order_relaxed) >> (vk & 31)) & 1;
}

void Push(DomEvent::Type type, bool down, uint16_t vk) {
  uint32_t head = g_ring_head.load(std::memory_order_relaxed);
  uint32_t tail = g_ring_tail.load(std::memory_order_acquire);
  if (head - tail >= kRingSize) {
    g_dropped_events.fetch_add(1, std::memory_order_relaxed);
    g_overflow.store(true, std::memory_order_release);
    return;
  }
  g_ring[head & (kRingSize - 1)] =
      (uint32_t(type) << 24) | (uint32_t(down ? 1 : 0) << 16) | uint32_t(vk);
  g_ring_head.store(head + 1, std::memory_order_release);
}

void SetKey(uint16_t vk, bool down) {
  if (vk == 0 || vk >= 256) {
    return;
  }
  if (KeyBit(vk) == down) {
    return;  // no change (e.g. auto-repeat or a release we never saw pressed)
  }
  if (down) {
    g_key_bits[vk >> 5].fetch_or(1u << (vk & 31), std::memory_order_relaxed);
  } else {
    g_key_bits[vk >> 5].fetch_and(~(1u << (vk & 31)), std::memory_order_relaxed);
  }
  Push(DomEvent::kKey, down, vk);
}

void ReleaseAll() {
  for (uint32_t vk = 1; vk < 256; ++vk) {
    if (KeyBit(uint16_t(vk))) {
      SetKey(uint16_t(vk), false);
    }
  }
  g_mouse_dx.store(0, std::memory_order_relaxed);
  g_mouse_dy.store(0, std::memory_order_relaxed);
  g_swallowed_buttons = 0;
}

// Secondary path: the same DOM events go to the web window (rex/ui/window_web.h),
// which dispatches them on the UI thread to the window's listeners (ImGui overlay,
// app hotkeys). The MnK driver ignores window-delivered events on web and reads the
// ring above instead, so nothing is counted twice. PostInputEvent takes a short
// mutex; it is documented as safe on the browser main thread.
void Forward(const rex::ui::web::InputEvent& ev) {
  if (rex::ui::web::PostInputEvent(ev)) {
    g_forwarded_events.fetch_add(1, std::memory_order_relaxed);
  }
}

// Canvas physical pixels (CSS offset * physical size / CSS size), as window_web.h wants.
void CanvasPosition(const EmscriptenMouseEvent* e, int32_t* x, int32_t* y) {
  uint32_t pw = 0, ph = 0;
  rex::ui::web::GetCanvasPhysicalSize(pw, ph);
  // No top-level commas in EM_ASM blocks (macro argument).
  *x = EM_ASM_INT(
      {
        var c = Module['__rexInputCanvas'];
        if (!c) {
          c = document.querySelector('#canvas');
          Module['__rexInputCanvas'] = c;
        }
        if (!c) return 0;
        var r = c.getBoundingClientRect();
        var s = $2 > 0 ? $2 / (r.width || 1) : 1;
        return Math.round(($0 - r.left) * s);
      },
      e->clientX, e->clientY, int(pw));
  *y = EM_ASM_INT(
      {
        var c = Module['__rexInputCanvas'];
        if (!c) return 0;
        var r = c.getBoundingClientRect();
        var s = $2 > 0 ? $2 / (r.height || 1) : 1;
        return Math.round(($1 - r.top) * s);
      },
      e->clientX, e->clientY, int(ph));
}

rex::ui::MouseEvent::Button UiButton(unsigned short button) {
  using B = rex::ui::MouseEvent::Button;
  switch (button) {
    case 0: return B::kLeft;
    case 1: return B::kMiddle;
    case 2: return B::kRight;
    case 3: return B::kX1;
    case 4: return B::kX2;
    default: return B::kNone;
  }
}

// Decodes a KeyboardEvent.key that is exactly one printable code point.
uint32_t SingleCodePoint(const char* s) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
  uint32_t cp;
  int n;
  if (p[0] == 0) return 0;
  if (p[0] < 0x80) { cp = p[0]; n = 1; }
  else if ((p[0] & 0xE0) == 0xC0) { cp = p[0] & 0x1F; n = 2; }
  else if ((p[0] & 0xF0) == 0xE0) { cp = p[0] & 0x0F; n = 3; }
  else return 0;  // outside the BMP: not representable in a 16-bit VirtualKey
  for (int i = 1; i < n; ++i) {
    if ((p[i] & 0xC0) != 0x80) return 0;
    cp = (cp << 6) | (p[i] & 0x3F);
  }
  if (p[n] != 0 || cp < 0x20 || cp == 0x7F) return 0;
  return cp;
}

void SetFocus(bool focus) {
  if (g_has_focus.load(std::memory_order_relaxed) == focus) {
    return;
  }
  g_focus_events.fetch_add(1, std::memory_order_relaxed);
  {
    rex::ui::web::InputEvent ev;
    ev.type = focus ? rex::ui::web::InputEvent::Type::kFocus : rex::ui::web::InputEvent::Type::kBlur;
    Forward(ev);
  }
  if (!focus) {
    ReleaseAll();
  }
  g_has_focus.store(focus, std::memory_order_release);
  Push(DomEvent::kFocus, focus, 0);
}

// KeyboardEvent.code -> VirtualKey (layout independent: physical key positions, the
// same way the Windows build sees scan-code based VKs for a US layout).
uint16_t CodeToVk(const char* code, unsigned int key_code) {
  std::string_view c(code);
  if (c.size() == 4 && c.substr(0, 3) == "Key" && c[3] >= 'A' && c[3] <= 'Z') {
    return uint16_t(c[3]);  // kA..kZ == 'A'..'Z'
  }
  if (c.size() == 6 && c.substr(0, 5) == "Digit" && c[5] >= '0' && c[5] <= '9') {
    return uint16_t(c[5]);  // k0..k9 == '0'..'9'
  }
  if (c.size() == 7 && c.substr(0, 6) == "Numpad" && c[6] >= '0' && c[6] <= '9') {
    return uint16_t(uint16_t(VirtualKey::kNumpad0) + (c[6] - '0'));
  }
  if (c.size() >= 2 && c.size() <= 3 && c[0] == 'F' && c[1] >= '1' && c[1] <= '9') {
    int n = c[1] - '0';
    if (c.size() == 3) {
      if (c[2] < '0' || c[2] > '9') return 0;
      n = n * 10 + (c[2] - '0');
    }
    if (n >= 1 && n <= 24) return uint16_t(uint16_t(VirtualKey::kF1) + (n - 1));
    return 0;
  }
  struct Entry {
    const char* code;
    VirtualKey vk;
  };
  static const Entry kTable[] = {
      {"Space", VirtualKey::kSpace},
      {"Enter", VirtualKey::kReturn},
      {"NumpadEnter", VirtualKey::kReturn},
      {"Escape", VirtualKey::kEscape},
      {"Tab", VirtualKey::kTab},
      {"Backspace", VirtualKey::kBack},
      {"CapsLock", VirtualKey::kCapital},
      {"ShiftLeft", VirtualKey::kLShift},
      {"ShiftRight", VirtualKey::kRShift},
      {"ControlLeft", VirtualKey::kLControl},
      {"ControlRight", VirtualKey::kRControl},
      {"AltLeft", VirtualKey::kLMenu},
      {"AltRight", VirtualKey::kRMenu},
      {"MetaLeft", VirtualKey::kLWin},
      {"MetaRight", VirtualKey::kRWin},
      {"OSLeft", VirtualKey::kLWin},
      {"OSRight", VirtualKey::kRWin},
      {"ContextMenu", VirtualKey::kApps},
      {"ArrowUp", VirtualKey::kUp},
      {"ArrowDown", VirtualKey::kDown},
      {"ArrowLeft", VirtualKey::kLeft},
      {"ArrowRight", VirtualKey::kRight},
      {"PageUp", VirtualKey::kPrior},
      {"PageDown", VirtualKey::kNext},
      {"Home", VirtualKey::kHome},
      {"End", VirtualKey::kEnd},
      {"Insert", VirtualKey::kInsert},
      {"Delete", VirtualKey::kDelete},
      {"Pause", VirtualKey::kPause},
      {"ScrollLock", VirtualKey::kScroll},
      {"NumLock", VirtualKey::kNumLock},
      {"PrintScreen", VirtualKey::kSnapshot},
      {"NumpadMultiply", VirtualKey::kMultiply},
      {"NumpadAdd", VirtualKey::kAdd},
      {"NumpadSubtract", VirtualKey::kSubtract},
      {"NumpadDecimal", VirtualKey::kDecimal},
      {"NumpadDivide", VirtualKey::kDivide},
      {"Semicolon", VirtualKey::kOem1},
      {"Equal", VirtualKey::kOemPlus},
      {"Comma", VirtualKey::kOemComma},
      {"Minus", VirtualKey::kOemMinus},
      {"Period", VirtualKey::kOemPeriod},
      {"Slash", VirtualKey::kOem2},
      {"Backquote", VirtualKey::kOem3},
      {"BracketLeft", VirtualKey::kOem4},
      {"Backslash", VirtualKey::kOem5},
      {"BracketRight", VirtualKey::kOem6},
      {"Quote", VirtualKey::kOem7},
      {"IntlBackslash", VirtualKey::kOem102},
  };
  for (const Entry& e : kTable) {
    if (c == e.code) return uint16_t(e.vk);
  }
  // Unknown/empty code (some synthetic events): legacy keyCode matches Windows VKs
  // for the common keys.
  if (c.empty() && key_code > 0 && key_code < 256) {
    return uint16_t(key_code);
  }
  return 0;
}

// Sided modifier -> generic VK (the default keybinds use "Shift"/"Ctrl"/"Alt").
uint16_t GenericOf(uint16_t vk, uint16_t* other_side) {
  switch (VirtualKey(vk)) {
    case VirtualKey::kLShift: *other_side = uint16_t(VirtualKey::kRShift); return uint16_t(VirtualKey::kShift);
    case VirtualKey::kRShift: *other_side = uint16_t(VirtualKey::kLShift); return uint16_t(VirtualKey::kShift);
    case VirtualKey::kLControl: *other_side = uint16_t(VirtualKey::kRControl); return uint16_t(VirtualKey::kControl);
    case VirtualKey::kRControl: *other_side = uint16_t(VirtualKey::kLControl); return uint16_t(VirtualKey::kControl);
    case VirtualKey::kLMenu: *other_side = uint16_t(VirtualKey::kRMenu); return uint16_t(VirtualKey::kMenu);
    case VirtualKey::kRMenu: *other_side = uint16_t(VirtualKey::kLMenu); return uint16_t(VirtualKey::kMenu);
    default: return 0;
  }
}

bool OnKey(int event_type, const EmscriptenKeyboardEvent* e, void*) {
  uint16_t vk = CodeToVk(e->code, e->keyCode);
  if (!vk) {
    return false;
  }
  const bool down = event_type == EMSCRIPTEN_EVENT_KEYDOWN;
  g_key_events.fetch_add(1, std::memory_order_relaxed);
  if (down && !g_has_focus.load(std::memory_order_relaxed)) {
    SetFocus(true);  // a key event means the page has focus (missed focus event)
  }
  {
    using T = rex::ui::web::InputEvent::Type;
    rex::ui::web::InputEvent ev;
    ev.type = down ? T::kKeyDown : T::kKeyUp;
    ev.virtual_key = VirtualKey(vk);
    ev.prev_state = down ? (e->repeat || KeyBit(vk)) : true;
    ev.shift = e->shiftKey;
    ev.ctrl = e->ctrlKey;
    ev.alt = e->altKey;
    ev.super = e->metaKey;
    Forward(ev);
    if (down && !e->ctrlKey && !e->metaKey) {
      if (uint32_t cp = SingleCodePoint(e->key)) {
        ev.type = T::kKeyChar;
        ev.virtual_key = VirtualKey(cp);
        Forward(ev);
      }
    }
  }
  SetKey(vk, down);
  uint16_t other = 0;
  if (uint16_t generic = GenericOf(vk, &other)) {
    SetKey(generic, down || KeyBit(other));
  }
  // Let browser shortcuts (Ctrl/Cmd+key) and F5/F11/F12 through; swallow the rest so
  // Space/Tab/arrows don't scroll or move focus away from the game.
  if ((e->ctrlKey || e->metaKey) && !GenericOf(vk, &other) && vk != uint16_t(VirtualKey::kLWin) &&
      vk != uint16_t(VirtualKey::kRWin)) {
    return false;
  }
  if (vk == uint16_t(VirtualKey::kF5) || vk == uint16_t(VirtualKey::kF11) ||
      vk == uint16_t(VirtualKey::kF12)) {
    return false;
  }
  return true;
}

uint16_t MouseButtonVk(unsigned short button) {
  switch (button) {
    case 0: return uint16_t(VirtualKey::kLButton);
    case 1: return uint16_t(VirtualKey::kMButton);
    case 2: return uint16_t(VirtualKey::kRButton);
    case 3: return uint16_t(VirtualKey::kXButton1);
    case 4: return uint16_t(VirtualKey::kXButton2);
    default: return 0;
  }
}

bool OnMouseDown(int, const EmscriptenMouseEvent* e, void*) {
  g_mouse_events.fetch_add(1, std::memory_order_relaxed);
  if (!g_has_focus.load(std::memory_order_relaxed)) {
    SetFocus(true);
  }
  uint16_t vk = MouseButtonVk(e->button);
  {
    rex::ui::web::InputEvent ev;
    ev.type = rex::ui::web::InputEvent::Type::kMouseDown;
    ev.button = UiButton(e->button);
    CanvasPosition(e, &ev.x, &ev.y);
    Forward(ev);
  }
  if (REXCVAR_GET(web_pointer_lock) && !g_pointer_locked.load(std::memory_order_relaxed)) {
    // Inside a user gesture: the browser allows the lock request here. The click
    // that acquires the lock is not a game input (it would otherwise fire).
    emscripten_request_pointerlock(g_canvas_selector, false);
    if (REXCVAR_GET(web_mouse_look_requires_pointer_lock) && e->button < 32) {
      g_swallowed_buttons |= 1u << e->button;
      return true;
    }
  }
  SetKey(vk, true);
  return true;
}

bool OnMouseUp(int, const EmscriptenMouseEvent* e, void*) {
  g_mouse_events.fetch_add(1, std::memory_order_relaxed);
  {
    rex::ui::web::InputEvent ev;
    ev.type = rex::ui::web::InputEvent::Type::kMouseUp;
    ev.button = UiButton(e->button);
    CanvasPosition(e, &ev.x, &ev.y);
    Forward(ev);
  }
  if (e->button < 32 && (g_swallowed_buttons & (1u << e->button))) {
    g_swallowed_buttons &= ~(1u << e->button);
    return false;
  }
  SetKey(MouseButtonVk(e->button), false);
  return false;
}

bool OnMouseMove(int, const EmscriptenMouseEvent* e, void*) {
  if (!g_has_focus.load(std::memory_order_relaxed)) {
    return false;
  }
  if (!g_pointer_locked.load(std::memory_order_relaxed)) {
    // Cursor position for UI listeners (meaningless while locked: the cursor is hidden).
    rex::ui::web::InputEvent ev;
    ev.type = rex::ui::web::InputEvent::Type::kMouseMove;
    CanvasPosition(e, &ev.x, &ev.y);
    Forward(ev);
  }
  if (REXCVAR_GET(web_mouse_look_requires_pointer_lock) &&
      !g_pointer_locked.load(std::memory_order_relaxed)) {
    return false;
  }
  g_mouse_events.fetch_add(1, std::memory_order_relaxed);
  g_mouse_dx.fetch_add(e->movementX, std::memory_order_relaxed);
  g_mouse_dy.fetch_add(e->movementY, std::memory_order_relaxed);
  return false;
}

bool OnWheel(int, const EmscriptenWheelEvent* e, void*) {
  // DOM_DELTA_PIXEL: ~100 px per detent in Chrome; DOM_DELTA_LINE: 3 lines per detent.
  double scale = e->deltaMode == DOM_DELTA_LINE ? 40.0 : (e->deltaMode == DOM_DELTA_PAGE ? 360.0 : 1.2);
  rex::ui::web::InputEvent ev;
  ev.type = rex::ui::web::InputEvent::Type::kMouseWheel;
  CanvasPosition(&e->mouse, &ev.x, &ev.y);
  ev.scroll_x = int32_t(std::lround(e->deltaX * scale));
  ev.scroll_y = int32_t(std::lround(-e->deltaY * scale));  // positive = away from the user
  Forward(ev);
  return true;  // don't scroll the page
}

bool OnPointerLockChange(int, const EmscriptenPointerlockChangeEvent* e, void*) {
  g_pointer_locked.store(e->isActive, std::memory_order_release);
  g_mouse_dx.store(0, std::memory_order_relaxed);  // no spike on lock start/end
  g_mouse_dy.store(0, std::memory_order_relaxed);
  return false;
}

bool OnFocus(int event_type, const EmscriptenFocusEvent*, void*) {
  SetFocus(event_type == EMSCRIPTEN_EVENT_FOCUS);
  return false;
}

bool OnVisibility(int, const EmscriptenVisibilityChangeEvent* e, void*) {
  if (e->hidden) {
    SetFocus(false);
  }
  return false;
}

// Standard Gamepad mapping (https://w3c.github.io/gamepad/#remapping) -> XInput.
constexpr uint16_t kStandardButtons[17] = {
    X_INPUT_GAMEPAD_A,          X_INPUT_GAMEPAD_B,
    X_INPUT_GAMEPAD_X,          X_INPUT_GAMEPAD_Y,
    X_INPUT_GAMEPAD_LEFT_SHOULDER, X_INPUT_GAMEPAD_RIGHT_SHOULDER,
    0 /* LT (analog) */,        0 /* RT (analog) */,
    X_INPUT_GAMEPAD_BACK,       X_INPUT_GAMEPAD_START,
    X_INPUT_GAMEPAD_LEFT_THUMB, X_INPUT_GAMEPAD_RIGHT_THUMB,
    X_INPUT_GAMEPAD_DPAD_UP,    X_INPUT_GAMEPAD_DPAD_DOWN,
    X_INPUT_GAMEPAD_DPAD_LEFT,  X_INPUT_GAMEPAD_DPAD_RIGHT,
    X_INPUT_GAMEPAD_GUIDE,
};

int16_t AxisToThumb(double v) {
  if (!(v == v)) return 0;  // NaN
  v = v < -1.0 ? -1.0 : (v > 1.0 ? 1.0 : v);
  return int16_t(std::lround(v * 32767.0));
}

uint8_t ValueToTrigger(double v, bool pressed) {
  if (!(v == v)) v = 0.0;
  v = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
  if (v == 0.0 && pressed) return 0xFF;  // digital-only trigger
  return uint8_t(std::lround(v * 255.0));
}

void PublishPad(uint32_t slot, bool connected, uint64_t w0, uint32_t rxry) {
  if (connected == g_pad_last_connected[slot] && w0 == g_pad_last_w0[slot] &&
      rxry == g_pad_last_rxry[slot]) {
    return;
  }
  g_pad_last_connected[slot] = connected;
  g_pad_last_w0[slot] = w0;
  g_pad_last_rxry[slot] = rxry;
  g_pad_packet[slot] = (g_pad_packet[slot] + 1) & 0x7FFFFFFFu;
  uint64_t w1 = uint64_t(rxry) | (uint64_t(g_pad_packet[slot]) << 32) |
                (uint64_t(connected ? 1 : 0) << 63);
  PadSlot& s = g_pads[slot];
  uint32_t seq = s.seq.load(std::memory_order_relaxed);
  s.seq.store(seq + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  s.w0.store(w0, std::memory_order_relaxed);
  s.w1.store(w1, std::memory_order_relaxed);
  s.seq.store(seq + 2, std::memory_order_release);
}

EmscriptenGamepadEvent g_pad_event;  // ~1.3 KB, main thread only

bool PollGamepads(double, void*) {
  g_gamepad_polls.fetch_add(1, std::memory_order_relaxed);
  bool seen[kMaxGamepads] = {};
  if (emscripten_sample_gamepad_data() == EMSCRIPTEN_RESULT_SUCCESS) {
    int count = emscripten_get_num_gamepads();
    for (int i = 0; i < count; ++i) {
      if (emscripten_get_gamepad_status(i, &g_pad_event) != EMSCRIPTEN_RESULT_SUCCESS) {
        continue;
      }
      const EmscriptenGamepadEvent& ev = g_pad_event;
      if (!ev.connected || ev.index < 0 || uint32_t(ev.index) >= kMaxGamepads) {
        continue;
      }
      // Non-"standard" mappings are read with the standard indices (best effort);
      // browsers report XInput-class pads as "standard".
      uint16_t buttons = 0;
      int nb = ev.numButtons < 17 ? ev.numButtons : 17;
      for (int b = 0; b < nb; ++b) {
        if (ev.digitalButton[b]) buttons |= kStandardButtons[b];
      }
      if (!REXCVAR_GET(guide_button)) {
        buttons &= ~uint16_t(X_INPUT_GAMEPAD_GUIDE);
      }
      uint8_t lt = ev.numButtons > 6 ? ValueToTrigger(ev.analogButton[6], ev.digitalButton[6]) : 0;
      uint8_t rt = ev.numButtons > 7 ? ValueToTrigger(ev.analogButton[7], ev.digitalButton[7]) : 0;
      // Gamepad API: +Y is down; XInput: +Y is up.
      int16_t lx = ev.numAxes > 0 ? AxisToThumb(ev.axis[0]) : 0;
      int16_t ly = ev.numAxes > 1 ? AxisToThumb(-ev.axis[1]) : 0;
      int16_t rx = ev.numAxes > 2 ? AxisToThumb(ev.axis[2]) : 0;
      int16_t ry = ev.numAxes > 3 ? AxisToThumb(-ev.axis[3]) : 0;
      uint64_t w0 = uint64_t(buttons) | (uint64_t(lt) << 16) | (uint64_t(rt) << 24) |
                    (uint64_t(uint16_t(lx)) << 32) | (uint64_t(uint16_t(ly)) << 48);
      uint32_t rxry = uint32_t(uint16_t(rx)) | (uint32_t(uint16_t(ry)) << 16);
      seen[ev.index] = true;
      PublishPad(uint32_t(ev.index), true, w0, rxry);
    }
  }
  for (uint32_t slot = 0; slot < kMaxGamepads; ++slot) {
    if (!seen[slot] && g_pad_last_connected[slot]) {
      PublishPad(slot, false, 0, 0);
    }
  }
  return true;  // keep the rAF loop running
}

void InstallOnMainThread(void*) {
  if (g_installed.load(std::memory_order_acquire)) {
    return;
  }
  const pthread_t kMain = EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD;
  const char* selector = g_canvas_selector;
  // Keyboard on the window: works whether or not the canvas element has DOM focus.
  emscripten_set_keydown_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, OnKey, kMain);
  emscripten_set_keyup_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, OnKey, kMain);
  // Presses only on the canvas; releases/motion on the document (they bubble there
  // from the canvas, and from the lock target while the pointer is locked).
  EMSCRIPTEN_RESULT r = emscripten_set_mousedown_callback_on_thread(selector, nullptr, true,
                                                                    OnMouseDown, kMain);
  g_canvas_result = int(r);  // logged by the caller (no logging on the browser main thread)
  emscripten_set_wheel_callback_on_thread(selector, nullptr, true, OnWheel, kMain);
  emscripten_set_mouseup_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr, true, OnMouseUp, kMain);
  emscripten_set_mousemove_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr, true, OnMouseMove, kMain);
  emscripten_set_pointerlockchange_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr, true,
                                                      OnPointerLockChange, kMain);
  emscripten_set_blur_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, OnFocus, kMain);
  emscripten_set_focus_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, OnFocus, kMain);
  emscripten_set_visibilitychange_callback_on_thread(nullptr, true, OnVisibility, kMain);

  bool focused = EM_ASM_INT({ return (typeof document !== 'undefined' && document.hasFocus()) ? 1 : 0; });
  g_has_focus.store(focused, std::memory_order_release);

  emscripten_request_animation_frame_loop(PollGamepads, nullptr);
  g_installed.store(1, std::memory_order_release);
}

}  // namespace

bool InstallBrowserInput(const char* canvas_selector) {
  if (g_installed.load(std::memory_order_acquire)) {
    return true;
  }
  std::snprintf(g_canvas_selector, sizeof(g_canvas_selector), "%s",
                canvas_selector ? canvas_selector : "#canvas");
  if (emscripten_is_main_runtime_thread()) {
    InstallOnMainThread(nullptr);
  } else if (!emscripten_proxy_sync(emscripten_proxy_get_system_queue(),
                                    emscripten_main_runtime_thread_id(), InstallOnMainThread,
                                    nullptr)) {
    REXLOG_ERROR("web input: failed to proxy listener setup to the browser main thread");
    return false;
  }
  if (g_canvas_result != EMSCRIPTEN_RESULT_SUCCESS) {
    REXLOG_WARN("web input: canvas '{}' not found, no mouse buttons/pointer lock ({})",
                g_canvas_selector, g_canvas_result);
  }
  REXLOG_INFO("web input: DOM keyboard/mouse/focus listeners + Gamepad API poll installed ({})",
              g_canvas_selector);
  return g_installed.load(std::memory_order_acquire) != 0;
}

bool PeekDomEvent(DomEvent* out) {
  uint32_t tail = g_ring_tail.load(std::memory_order_relaxed);
  uint32_t head = g_ring_head.load(std::memory_order_acquire);
  if (tail == head) {
    return false;
  }
  uint32_t v = g_ring[tail & (kRingSize - 1)];
  out->type = uint8_t(v >> 24);
  out->down = uint8_t((v >> 16) & 0xFF);
  out->vk = uint16_t(v & 0xFFFF);
  return true;
}

void PopDomEvent() {
  g_ring_tail.store(g_ring_tail.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

bool TakeOverflow() {
  if (!g_overflow.exchange(false, std::memory_order_acq_rel)) {
    return false;
  }
  // Discard what is queued; the bitmap is the state to resync to.
  g_ring_tail.store(g_ring_head.load(std::memory_order_acquire), std::memory_order_release);
  return true;
}

void SnapshotKeyBitmap(bool (&key_down)[256]) {
  for (uint32_t w = 0; w < 256 / 32; ++w) {
    uint32_t bits = g_key_bits[w].load(std::memory_order_relaxed);
    for (uint32_t b = 0; b < 32; ++b) {
      key_down[w * 32 + b] = (bits >> b) & 1;
    }
  }
}

void TakeMouseDelta(int32_t* dx, int32_t* dy) {
  *dx = g_mouse_dx.exchange(0, std::memory_order_relaxed);
  *dy = g_mouse_dy.exchange(0, std::memory_order_relaxed);
}

bool HasFocus() {
  return g_has_focus.load(std::memory_order_acquire);
}

bool ReadGamepad(uint32_t slot, GamepadSnapshot* out) {
  if (slot >= kMaxGamepads) {
    return false;
  }
  PadSlot& s = g_pads[slot];
  uint64_t w0, w1;
  for (;;) {
    uint32_t s1 = s.seq.load(std::memory_order_acquire);
    if (s1 & 1) {
      continue;
    }
    w0 = s.w0.load(std::memory_order_relaxed);
    w1 = s.w1.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (s.seq.load(std::memory_order_relaxed) == s1) {
      break;
    }
  }
  out->connected = (w1 >> 63) & 1;
  out->buttons = uint16_t(w0);
  out->left_trigger = uint8_t(w0 >> 16);
  out->right_trigger = uint8_t(w0 >> 24);
  out->thumb_lx = int16_t(uint16_t(w0 >> 32));
  out->thumb_ly = int16_t(uint16_t(w0 >> 48));
  out->thumb_rx = int16_t(uint16_t(w1));
  out->thumb_ry = int16_t(uint16_t(w1 >> 16));
  out->packet_number = uint32_t(w1 >> 32) & 0x7FFFFFFFu;
  return out->connected;
}

BridgeStats GetBridgeStats() {
  BridgeStats s;
  s.installed = g_installed.load(std::memory_order_relaxed);
  s.key_events = g_key_events.load(std::memory_order_relaxed);
  s.mouse_events = g_mouse_events.load(std::memory_order_relaxed);
  s.focus_events = g_focus_events.load(std::memory_order_relaxed);
  s.dropped_events = g_dropped_events.load(std::memory_order_relaxed);
  s.gamepad_polls = g_gamepad_polls.load(std::memory_order_relaxed);
  s.pointer_locked = g_pointer_locked.load(std::memory_order_relaxed) ? 1 : 0;
  s.has_focus = g_has_focus.load(std::memory_order_relaxed) ? 1 : 0;
  s.forwarded_events = g_forwarded_events.load(std::memory_order_relaxed);
  return s;
}

}  // namespace rex::input::web
