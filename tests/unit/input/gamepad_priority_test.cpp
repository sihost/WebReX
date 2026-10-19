// GamepadPriorityFilter (src/input/gamepad_priority.h): keyboard/mouse emulation vs gamepad.
#include <catch2/catch_test_macros.hpp>

#include "input/gamepad_priority.h"

using rex::input::GamepadPriorityFilter;
using rex::input::PriorityPadState;

namespace {
constexpr uint16_t kA = 0x1000, kX = 0x4000;
constexpr GamepadPriorityFilter::Config kConfig{1500, 50};

PriorityPadState Pad(uint16_t buttons, uint8_t lt = 0, uint8_t rt = 0) {
  PriorityPadState s;
  s.buttons = buttons;
  s.left_trigger = lt;
  s.right_trigger = rt;
  return s;
}
PriorityPadState Mouse(bool lmb, bool rmb = false, int16_t rx = 0) {
  PriorityPadState s;
  s.right_trigger = lmb ? 0xFF : 0;
  s.left_trigger = rmb ? 0xFF : 0;
  s.thumb_rx = rx;
  return s;
}
}  // namespace

TEST_CASE("No pad connected: MnK passes through immediately", "[input][priority]") {
  GamepadPriorityFilter f;
  auto out = f.Apply(1000, false, Pad(0), Mouse(true, false, 1234), kConfig);
  CHECK(out.right_trigger == 0xFF);
  CHECK(out.thumb_rx == 1234);
}

TEST_CASE("Synthetic click before the pad press is dropped", "[input][priority]") {
  GamepadPriorityFilter f;
  uint32_t suppressed = 0;
  // Click arrives at t=1000 while the pad is still neutral (connected): held back.
  auto out = f.Apply(1000, true, Pad(0), Mouse(true), kConfig, &suppressed);
  CHECK(out.right_trigger == 0);
  CHECK(suppressed == (1u << 17));
  // Pad A shows up 20 ms later: the click is rejected for as long as it is held.
  out = f.Apply(1020, true, Pad(kA), Mouse(true), kConfig);
  CHECK(out.right_trigger == 0);
  out = f.Apply(1100, true, Pad(0), Mouse(true), kConfig);
  CHECK(out.right_trigger == 0);
  out = f.Apply(5000, true, Pad(0), Mouse(true), kConfig);
  CHECK(out.right_trigger == 0);
  // Released: back to idle.
  out = f.Apply(5016, true, Pad(0), Mouse(false), kConfig);
  CHECK(out.right_trigger == 0);
}

TEST_CASE("Synthetic click after the pad press is dropped", "[input][priority]") {
  GamepadPriorityFilter f;
  f.Apply(1000, true, Pad(kA), Mouse(false), kConfig);
  auto out = f.Apply(1010, true, Pad(kA), Mouse(true, true), kConfig);
  CHECK(out.right_trigger == 0);
  CHECK(out.left_trigger == 0);
  // Pad released, click still held (within and after the window): stays rejected.
  out = f.Apply(1200, true, Pad(0), Mouse(true, true), kConfig);
  CHECK(out.right_trigger == 0);
  out = f.Apply(4000, true, Pad(0), Mouse(true, true), kConfig);
  CHECK(out.right_trigger == 0);
}

TEST_CASE("Mouse look and MnK buttons are ignored within the window", "[input][priority]") {
  GamepadPriorityFilter f;
  f.Apply(1000, true, Pad(0, 200, 0), Mouse(false), kConfig);  // LT pulled (aim)
  auto out = f.Apply(1500, true, Pad(0), Mouse(false, false, 9000), kConfig);
  CHECK(out.thumb_rx == 0);
  out = f.Apply(2400, true, Pad(0), Mouse(true, false, 9000), kConfig);
  CHECK(out.thumb_rx == 0);
  CHECK(out.right_trigger == 0);
}

TEST_CASE("Keyboard/mouse work again after the pad goes idle", "[input][priority]") {
  GamepadPriorityFilter f;
  f.Apply(1000, true, Pad(kA), Mouse(false), kConfig);
  f.Apply(1100, true, Pad(0), Mouse(false), kConfig);
  // 1.5 s after the last pad input: mouse look immediately, a new click after the hold-off.
  auto out = f.Apply(2700, true, Pad(0), Mouse(true, false, 500), kConfig);
  CHECK(out.thumb_rx == 500);
  CHECK(out.right_trigger == 0);
  out = f.Apply(2730, true, Pad(0), Mouse(true), kConfig);
  CHECK(out.right_trigger == 0);
  out = f.Apply(2750, true, Pad(0), Mouse(true), kConfig);
  CHECK(out.right_trigger == 0xFF);
  out = f.Apply(3500, true, Pad(0), Mouse(true), kConfig);
  CHECK(out.right_trigger == 0xFF);
  // Pad used again while the mouse button is held: the pad wins.
  out = f.Apply(3510, true, Pad(kA), Mouse(true), kConfig);
  CHECK(out.right_trigger == 0);
  out = f.Apply(3520, true, Pad(0), Mouse(false), kConfig);
  CHECK(out.right_trigger == 0);
}

TEST_CASE("A key tap shorter than the hold-off is delivered once", "[input][priority]") {
  GamepadPriorityFilter f;
  PriorityPadState key;
  key.buttons = kX;
  auto out = f.Apply(1000, true, Pad(0), key, kConfig);
  CHECK(out.buttons == 0);
  out = f.Apply(1020, true, Pad(0), PriorityPadState{}, kConfig);
  CHECK(out.buttons == 0);
  out = f.Apply(1060, true, Pad(0), PriorityPadState{}, kConfig);
  CHECK(out.buttons == kX);
  out = f.Apply(1080, true, Pad(0), PriorityPadState{}, kConfig);
  CHECK(out.buttons == 0);
}

TEST_CASE("Resting sticks and light triggers are not pad activity", "[input][priority]") {
  PriorityPadState s;
  s.thumb_lx = 8000;
  s.thumb_ry = -12000;
  s.left_trigger = 20;
  CHECK_FALSE(GamepadPriorityFilter::PadActive(s));
  s.thumb_rx = 20000;
  CHECK(GamepadPriorityFilter::PadActive(s));
}
