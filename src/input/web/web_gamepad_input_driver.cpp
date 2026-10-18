/**
 * @file        input/web/web_gamepad_input_driver.cpp
 * @brief       Gamepad API input driver (Emscripten). The browser main thread polls
 *              navigator.getGamepads() (dom_input_bridge.cpp); this driver reads the
 *              published per-slot snapshots from guest threads without blocking.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/input/web/web_input.h>

#include <cstring>
#include <mutex>

#include <emscripten/emscripten.h>

#include <rex/input/input.h>
#include <rex/logging.h>

namespace rex::input::web {

namespace {
std::mutex g_pad_log_mutex;
}  // namespace

// Logs what the poll loop queued (pads connecting, their layout, user assignment), so a
// player's log shows which device the game reads and how.
void WebGamepadInputDriver::LogPadEvents() {
  std::unique_lock lock(g_pad_log_mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    return;
  }
  PadEvent e;
  while (PopPadEvent(&e)) {
    switch (e.kind) {
      case PadEvent::kConnected:
        REXLOG_INFO(
            "web gamepad: navigator pad {} connected: \"{}\" vendor {:04x} product {:04x} layout "
            "{} (axes: LT {} RT {} RX {} RY {} hat {})",
            e.browser_index, e.id, e.vendor, e.product, PadLayoutName(e.layout), e.lt_axis,
            e.rt_axis, e.rx_axis, e.ry_axis, e.hat_axis);
        break;
      case PadEvent::kDisconnected:
        REXLOG_INFO("web gamepad: navigator pad {} disconnected (\"{}\")", e.browser_index, e.id);
        break;
      case PadEvent::kAssigned:
        if (e.browser_index < 0) {
          REXLOG_INFO("web gamepad: XInput user {} -> no pad", e.user);
        } else {
          REXLOG_INFO("web gamepad: XInput user {} -> navigator pad {} \"{}\" ({})", e.user,
                      e.browser_index, e.id, PadLayoutName(e.layout));
        }
        break;
    }
  }
}

WebGamepadInputDriver::WebGamepadInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

WebGamepadInputDriver::~WebGamepadInputDriver() = default;

X_STATUS WebGamepadInputDriver::Setup() {
  if (!InstallBrowserInput()) {
    return X_STATUS_UNSUCCESSFUL;
  }
  REXLOG_INFO("Web Gamepad API input driver initialized");
  return X_STATUS_SUCCESS;
}

X_RESULT WebGamepadInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                                X_INPUT_CAPABILITIES* out_caps) {
  (void)flags;
  LogPadEvents();
  GamepadSnapshot pad;
  if (!ReadGamepad(user_index, &pad)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
    out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
    out_caps->flags = 0;
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    out_caps->vibration.left_motor_speed = 0xFFFF;
    out_caps->vibration.right_motor_speed = 0xFFFF;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT WebGamepadInputDriver::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  LogPadEvents();
  GamepadSnapshot pad;
  if (!ReadGamepad(user_index, &pad)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_state) {
    std::memset(out_state, 0, sizeof(*out_state));
    out_state->packet_number = pad.packet_number;
    // Hidden page, unfocused with web_gamepad_requires_focus, or the ImGui overlay owning
    // input: neutral but connected.
    if (GamepadInputAllowed() && is_active()) {
      out_state->gamepad.buttons = pad.buttons;
      out_state->gamepad.left_trigger = pad.left_trigger;
      out_state->gamepad.right_trigger = pad.right_trigger;
      out_state->gamepad.thumb_lx = pad.thumb_lx;
      out_state->gamepad.thumb_ly = pad.thumb_ly;
      out_state->gamepad.thumb_rx = pad.thumb_rx;
      out_state->gamepad.thumb_ry = pad.thumb_ry;
    }
  }
  return X_ERROR_SUCCESS;
}

X_RESULT WebGamepadInputDriver::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  GamepadSnapshot pad;
  if (!ReadGamepad(user_index, &pad)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (!vibration) {
    return X_ERROR_SUCCESS;
  }
  uint32_t left = static_cast<uint16_t>(vibration->left_motor_speed);    // low-frequency
  uint32_t right = static_cast<uint16_t>(vibration->right_motor_speed);  // high-frequency
  uint32_t packed = (left << 16) | right;
  if (packed == last_rumble_[user_index]) {
    return X_ERROR_SUCCESS;  // XInput vibration is level-triggered; only send changes
  }
  last_rumble_[user_index] = packed;
  const int browser_index = GamepadBrowserIndex(user_index);
  if (browser_index < 0) {
    return X_ERROR_SUCCESS;
  }
  // GamepadHapticActuator lives on the browser main thread; fire and forget.
  // (No top-level commas allowed in the EM_ASM block: it is a macro argument.)
  MAIN_THREAD_ASYNC_EM_ASM(
      {
        try {
          var pads = navigator.getGamepads ? navigator.getGamepads() : [];
          var g = pads[$0];
          if (!g || !g.vibrationActuator) return;
          var a = g.vibrationActuator;
          if ($1 == 0 && $2 == 0) {
            if (a.reset) {
              var r = a.reset();
              if (r && r.catch) r.catch(function() {});
            }
            return;
          }
          var o = {};
          o.startDelay = 0;
          o.duration = 5000;
          o.strongMagnitude = $1 / 65535;
          o.weakMagnitude = $2 / 65535;
          var p = a.playEffect('dual-rumble', o);
          if (p && p.catch) p.catch(function() {});
        } catch (e) {
        }
      },
      browser_index, left, right);
  return X_ERROR_SUCCESS;
}

X_RESULT WebGamepadInputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,
                                             X_INPUT_KEYSTROKE* out_keystroke) {
  (void)flags;
  (void)out_keystroke;
  GamepadSnapshot pad;
  if (!ReadGamepad(user_index, &pad)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return X_ERROR_EMPTY;  // no VK_PAD keystroke synthesis (same as the MnK driver)
}

}  // namespace rex::input::web
