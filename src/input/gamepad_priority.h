/**
 * @file        input/gamepad_priority.h
 * @brief       Gamepad priority over keyboard/mouse controller emulation (InputSystem).
 *
 * The MnK driver (mnk_mode) is merged into the gamepad state of its user slot. Tools that
 * translate a controller into mouse/keyboard input at the OS level (Steam Input desktop
 * layout, DS4Windows, ...) then double every pad press: Cross = A (jump) plus a synthetic
 * left click = RT (fire). The filter keeps MnK buttons/triggers/sticks out of the merge
 * while the pad is in use:
 *   - pad activity (a button, a trigger past 30, a stick past half) suppresses MnK input
 *     for window_ms after the last activity; MnK controls pressed during it stay
 *     suppressed until released;
 *   - while a pad is connected, a new MnK press is held for holdoff_ms and dropped if the
 *     pad becomes active meanwhile (synthetic events can reach the page before the
 *     Gamepad API shows the press); a tap released within the hold-off is delivered once
 *     when it ends;
 *   - after the window, keyboard/mouse work as before (switching to MnK just works).
 * Pure logic (explicit time), unit-tested in tests/unit/input/gamepad_priority_test.cpp.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>
#include <cstdlib>

namespace rex::input {

struct PriorityPadState {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  int16_t thumb_lx = 0, thumb_ly = 0, thumb_rx = 0, thumb_ry = 0;
};

class GamepadPriorityFilter {
 public:
  struct Config {
    uint32_t window_ms = 1500;
    uint32_t holdoff_ms = 50;
  };
  // Bits 0-15: XInput buttons, 16: left trigger, 17: right trigger.
  static constexpr int kControls = 18;
  static constexpr int kTriggerThreshold = 30;  // XINPUT_GAMEPAD_TRIGGER_THRESHOLD
  static constexpr int kStickThreshold = 16384;

  static bool PadActive(const PriorityPadState& s) {
    auto big = [](int16_t v) { return std::abs(int(v)) > kStickThreshold; };
    return s.buttons != 0 || s.left_trigger > kTriggerThreshold ||
           s.right_trigger > kTriggerThreshold || big(s.thumb_lx) || big(s.thumb_ly) ||
           big(s.thumb_rx) || big(s.thumb_ry);
  }

  static uint32_t ControlBits(const PriorityPadState& s) {
    return uint32_t(s.buttons) | (s.left_trigger ? 1u << 16 : 0u) |
           (s.right_trigger ? 1u << 17 : 0u);
  }

  // pad: merged state of the gamepad drivers (pad_connected: any of them has a pad for
  // this user). Returns the MnK state to merge; suppressed_bits (optional) gets the MnK
  // controls held down but not passed through.
  PriorityPadState Apply(uint64_t now_ms, bool pad_connected, const PriorityPadState& pad,
                         const PriorityPadState& mnk, const Config& config,
                         uint32_t* suppressed_bits = nullptr) {
    const bool pad_active = pad_connected && PadActive(pad);
    if (pad_active) {
      pad_seen_ = true;
      last_pad_active_ms_ = now_ms;
    }
    const bool recent = pad_seen_ && now_ms - last_pad_active_ms_ <= config.window_ms;
    const uint32_t down_bits = ControlBits(mnk);
    uint32_t out_bits = 0, suppressed = 0;
    for (int b = 0; b < kControls; ++b) {
      const uint32_t bit = 1u << b;
      const bool down = (down_bits & bit) != 0;
      Control& c = controls_[b];
      switch (c.state) {
        case kIdle:
          if (!down) break;
          if (recent) {
            c.state = kRejected;
          } else if (pad_connected && config.holdoff_ms) {
            c.state = kPending;
            c.press_ms = now_ms;
            c.released = false;
          } else {
            c.state = kAccepted;
          }
          break;
        case kPending:
          if (pad_active) {
            c.state = down ? kRejected : kIdle;
          } else {
            c.released |= !down;
            if (now_ms - c.press_ms >= config.holdoff_ms) {
              if (c.released) {
                out_bits |= bit;  // a tap shorter than the hold-off: deliver it once
                c.state = kIdle;
              } else {
                c.state = kAccepted;
              }
            }
          }
          break;
        case kAccepted:
          if (!down) {
            c.state = kIdle;
          } else if (pad_active) {
            c.state = kRejected;
          }
          break;
        case kRejected:
          if (!down) c.state = kIdle;
          break;
      }
      if (c.state == kAccepted) out_bits |= bit;
      if (down && !(out_bits & bit)) suppressed |= bit;
    }
    if (suppressed_bits) *suppressed_bits = suppressed;
    PriorityPadState out;
    out.buttons = uint16_t(out_bits & 0xFFFF);
    // MnK triggers are digital (0 / 255); a delivered tap reads as fully pressed.
    out.left_trigger = (out_bits & (1u << 16)) ? (mnk.left_trigger ? mnk.left_trigger : 0xFF) : 0;
    out.right_trigger =
        (out_bits & (1u << 17)) ? (mnk.right_trigger ? mnk.right_trigger : 0xFF) : 0;
    if (!recent) {
      out.thumb_lx = mnk.thumb_lx;
      out.thumb_ly = mnk.thumb_ly;
      out.thumb_rx = mnk.thumb_rx;
      out.thumb_ry = mnk.thumb_ry;
    }
    return out;
  }

 private:
  enum State : uint8_t { kIdle, kPending, kAccepted, kRejected };
  struct Control {
    State state = kIdle;
    bool released = false;
    uint64_t press_ms = 0;
  };
  Control controls_[kControls];
  bool pad_seen_ = false;
  uint64_t last_pad_active_ms_ = 0;
};

}  // namespace rex::input
