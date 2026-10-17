/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cmath>
#include <iterator>
#include <mutex>
#include <string>

#include <rex/dbg.h>
#include <rex/input/flags.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/nop/nop_input_driver.h>
#if REX_PLATFORM_WEB
#include <rex/input/web/web_input.h>
#else  // SDL is not built for the web (browser input: rex/input/web)
#include <rex/input/sdl/sdl_input_driver.h>
#endif
#include <rex/input/xinput/xinput_input_driver.h>
#include <rex/logging.h>

#if REX_PLATFORM_WEB
// "web": Gamepad API driver (+ the MnK driver fed by DOM events, see mnk_mode).
REXCVAR_DEFINE_STRING(input_backend, "web", "Input", "Input backend: web")
    .allowed({"web"});
#else
REXCVAR_DEFINE_STRING(input_backend, "sdl", "Input", "Input backend: sdl, xinput")
    .allowed({"sdl", "xinput"});
#endif

REXCVAR_DEFINE_BOOL(guide_button, false, "Input", "Enable guide button pass-through");
REXCVAR_DEFINE_BOOL(input_log_state, false, "Input",
                    "Log the controller state returned to the guest (XamInputGetState) whenever it "
                    "changes, per user index, with each driver's result (and the web DOM bridge "
                    "counters); diagnostics");
namespace rex::input {

namespace {

// input_log_state: last logged state per user index. Thumbs are compared at 1/16 of the
// range so analog noise does not flood the log.
struct LoggedState {
  bool valid = false;
  X_RESULT result = 0;
  uint16_t buttons = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  int16_t thumbs[4] = {};
  uint64_t calls = 0;
};
std::mutex g_log_mutex;
LoggedState g_logged[4];

void LogGuestState(uint32_t user_index, X_RESULT result, const X_INPUT_STATE& s,
                   const X_RESULT* driver_results, size_t driver_count) {
  if (user_index >= 4) {
    return;
  }
  std::lock_guard lock(g_log_mutex);
  LoggedState& l = g_logged[user_index];
  ++l.calls;
  const uint16_t buttons = result == X_ERROR_SUCCESS ? uint16_t(s.gamepad.buttons) : 0;
  const uint8_t lt = result == X_ERROR_SUCCESS ? uint8_t(s.gamepad.left_trigger) : 0;
  const uint8_t rt = result == X_ERROR_SUCCESS ? uint8_t(s.gamepad.right_trigger) : 0;
  int16_t thumbs[4] = {};
  if (result == X_ERROR_SUCCESS) {
    thumbs[0] = int16_t(s.gamepad.thumb_lx);
    thumbs[1] = int16_t(s.gamepad.thumb_ly);
    thumbs[2] = int16_t(s.gamepad.thumb_rx);
    thumbs[3] = int16_t(s.gamepad.thumb_ry);
  }
  bool changed = !l.valid || l.result != result || l.buttons != buttons ||
                 l.left_trigger != lt || l.right_trigger != rt;
  for (int i = 0; i < 4; ++i) {
    changed |= (thumbs[i] >> 12) != (l.thumbs[i] >> 12);
  }
  if (!changed) {
    return;
  }
  std::string drivers;
  for (size_t i = 0; i < driver_count; ++i) {
    fmt::format_to(std::back_inserter(drivers), "{}{:X}", i ? "," : "", driver_results[i]);
  }
#if REX_PLATFORM_WEB
  const web::BridgeStats b = web::GetBridgeStats();
  REXLOG_INFO(
      "input: user {} GetState -> {:X} buttons {:04X} lt {} rt {} lx {} ly {} rx {} ry {} "
      "packet {} (drivers [{}], call {}) | bridge keys {} mouse {} focus_events {} pad_polls {} "
      "focus {} pointer_lock {} dropped {} forwarded {}",
      user_index, result, buttons, lt, rt, thumbs[0], thumbs[1], thumbs[2], thumbs[3],
      result == X_ERROR_SUCCESS ? uint32_t(s.packet_number) : 0u, drivers, l.calls, b.key_events,
      b.mouse_events, b.focus_events, b.gamepad_polls, b.has_focus, b.pointer_locked,
      b.dropped_events, b.forwarded_events);
#else
  REXLOG_INFO(
      "input: user {} GetState -> {:X} buttons {:04X} lt {} rt {} lx {} ly {} rx {} ry {} "
      "packet {} (drivers [{}], call {})",
      user_index, result, buttons, lt, rt, thumbs[0], thumbs[1], thumbs[2], thumbs[3],
      result == X_ERROR_SUCCESS ? uint32_t(s.packet_number) : 0u, drivers, l.calls);
#endif
  l.valid = true;
  l.result = result;
  l.buttons = buttons;
  l.left_trigger = lt;
  l.right_trigger = rt;
  std::copy(std::begin(thumbs), std::end(thumbs), std::begin(l.thumbs));
}

}  // namespace

InputSystem::InputSystem(rex::ui::Window* window) : window_(window) {}

InputSystem::~InputSystem() = default;

X_STATUS InputSystem::Setup() {
  return X_STATUS_SUCCESS;
}

void InputSystem::Shutdown() {
  drivers_.clear();
}

void InputSystem::AddDriver(std::unique_ptr<InputDriver> driver) {
  drivers_.push_back(std::move(driver));
}

void InputSystem::AttachWindow(rex::ui::Window* window) {
  window_ = window;
  for (auto& driver : drivers_) {
    driver->OnWindowAvailable(window);
  }
}

void InputSystem::SetActiveCallback(std::function<bool()> callback) {
  for (auto& driver : drivers_) {
    driver->set_is_active_callback(callback);
  }
}

X_RESULT InputSystem::GetCapabilities(uint32_t user_index, uint32_t flags,
                                      X_INPUT_CAPABILITIES* out_caps) {
  SCOPE_profile_cpu_f("hid");

  bool any_connected = false;
  for (auto& driver : drivers_) {
    X_RESULT result = driver->GetCapabilities(user_index, flags, out_caps);
    if (result != X_ERROR_DEVICE_NOT_CONNECTED) {
      any_connected = true;
    }
    if (result == X_ERROR_SUCCESS) {
      return result;
    }
  }
  return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT InputSystem::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  SCOPE_profile_cpu_f("hid");

  bool any_connected = false;
  bool first_result = true;
  X_INPUT_STATE merged = {};
  constexpr size_t kMaxLoggedDrivers = 8;
  X_RESULT driver_results[kMaxLoggedDrivers] = {};
  size_t driver_count = 0;

  for (auto& driver : drivers_) {
    X_INPUT_STATE state = {};
    X_RESULT result = driver->GetState(user_index, &state);
    if (driver_count < kMaxLoggedDrivers) {
      driver_results[driver_count++] = result;
    }
    if (result != X_ERROR_DEVICE_NOT_CONNECTED) {
      any_connected = true;
    }
    if (result == X_ERROR_SUCCESS) {
      if (first_result) {
        merged = state;
        first_result = false;
      } else {
        // Merge: OR buttons, max triggers, max-magnitude sticks
        merged.gamepad.buttons = static_cast<uint16_t>(merged.gamepad.buttons) |
                                 static_cast<uint16_t>(state.gamepad.buttons);
        merged.gamepad.left_trigger =
            std::max(merged.gamepad.left_trigger, state.gamepad.left_trigger);
        merged.gamepad.right_trigger =
            std::max(merged.gamepad.right_trigger, state.gamepad.right_trigger);

        auto merge_axis = [](int16_t a, int16_t b) -> int16_t {
          return (std::abs(static_cast<int>(a)) >= std::abs(static_cast<int>(b))) ? a : b;
        };
        merged.gamepad.thumb_lx = merge_axis(merged.gamepad.thumb_lx, state.gamepad.thumb_lx);
        merged.gamepad.thumb_ly = merge_axis(merged.gamepad.thumb_ly, state.gamepad.thumb_ly);
        merged.gamepad.thumb_rx = merge_axis(merged.gamepad.thumb_rx, state.gamepad.thumb_rx);
        merged.gamepad.thumb_ry = merge_axis(merged.gamepad.thumb_ry, state.gamepad.thumb_ry);

        if (static_cast<uint32_t>(state.packet_number) >
            static_cast<uint32_t>(merged.packet_number)) {
          merged.packet_number = state.packet_number;
        }
      }
    }
  }

  if (first_result) {
    X_RESULT result = any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
    if (REXCVAR_GET(input_log_state)) {
      LogGuestState(user_index, result, merged, driver_results, driver_count);
    }
    return result;
  }

  if (REXCVAR_GET(input_log_state)) {
    LogGuestState(user_index, X_ERROR_SUCCESS, merged, driver_results, driver_count);
  }
  if (out_state) {
    *out_state = merged;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT InputSystem::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  SCOPE_profile_cpu_f("hid");

  bool any_connected = false;
  for (auto& driver : drivers_) {
    X_RESULT result = driver->SetState(user_index, vibration);
    if (result != X_ERROR_DEVICE_NOT_CONNECTED) {
      any_connected = true;
    }
    if (result == X_ERROR_SUCCESS) {
      return result;
    }
  }
  return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT InputSystem::GetKeystroke(uint32_t user_index, uint32_t flags,
                                   X_INPUT_KEYSTROKE* out_keystroke) {
  SCOPE_profile_cpu_f("hid");

  bool any_connected = false;
  for (auto& driver : drivers_) {
    X_RESULT result = driver->GetKeystroke(user_index, flags, out_keystroke);
    if (result != X_ERROR_DEVICE_NOT_CONNECTED) {
      any_connected = true;
    }
    if (result == X_ERROR_SUCCESS || result == X_ERROR_EMPTY) {
      return result;
    }
  }
  return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

std::unique_ptr<InputSystem> CreateDefaultInputSystem(bool tool_mode) {
  auto input = std::make_unique<InputSystem>(nullptr);

  if (!tool_mode) {
#if REX_PLATFORM_WIN32
    if (REXCVAR_GET(input_backend) == "xinput") {
      auto xinput_driver = std::make_unique<xinput::XinputInputDriver>(nullptr, 0);
      if (xinput_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(xinput_driver));
      }
    }
#endif

#if REX_PLATFORM_WEB
    if (REXCVAR_GET(input_backend) == "web") {
      auto web_driver = std::make_unique<web::WebGamepadInputDriver>(nullptr, 0);
      if (web_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(web_driver));
      }
    }
#else
    if (REXCVAR_GET(input_backend) == "sdl") {
      auto sdl_driver = std::make_unique<sdl::SDLInputDriver>(nullptr, 0);
      if (sdl_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(sdl_driver));
      }
    }
#endif

    // MnK driver (keyboard/mouse -> controller emulation)
    auto mnk_driver = std::make_unique<mnk::MnkInputDriver>(nullptr, 0);
    if (mnk_driver->Setup() == X_STATUS_SUCCESS) {
      input->AddDriver(std::move(mnk_driver));
    }
  }

  // NOP driver (primary in tool mode, fallback otherwise)
  uint8_t nop_index = tool_mode ? 0 : 1;
  input->AddDriver(std::make_unique<nop::NopInputDriver>(nullptr, nop_index));
  return input;
}

}  // namespace rex::input
