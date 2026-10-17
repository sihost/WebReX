/**
 * @file        rex/graphics/webgpu/webgpu_log.h
 * @brief       Logging shim for the WebGPU backend (Emscripten only).
 *
 * The device/present code is also compiled into a standalone test page
 * (REX_WEBGPU_STANDALONE, tools/webgpu_clear in the project), which has no
 * rexcore logging, so it logs through std::format to stderr there. Inside the
 * SDK it forwards to the REXGPU_* category macros. Format strings use the
 * common {fmt}/std::format subset.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#if !defined(__EMSCRIPTEN__)
#error "rex/graphics/webgpu is Emscripten-only (REXGLUE_USE_WEBGPU)"
#endif

#include <string_view>

#include <webgpu/webgpu.h>

#if defined(REX_WEBGPU_STANDALONE)
#include <cstdio>
#include <format>
#include <string>
#define REXWGPU_LOG_IMPL(level, ...)                                 \
  do {                                                               \
    std::string rexwgpu_log_message_ = std::format(__VA_ARGS__);     \
    std::fprintf(stderr, "[webgpu %s] %s\n", level,                  \
                 rexwgpu_log_message_.c_str());                      \
  } while (0)
#define REXWGPU_DEBUG(...) REXWGPU_LOG_IMPL("D", __VA_ARGS__)
#define REXWGPU_INFO(...) REXWGPU_LOG_IMPL("I", __VA_ARGS__)
#define REXWGPU_WARN(...) REXWGPU_LOG_IMPL("W", __VA_ARGS__)
#define REXWGPU_ERROR(...) REXWGPU_LOG_IMPL("E", __VA_ARGS__)
#else
#include <rex/logging.h>
#define REXWGPU_DEBUG(...) REXGPU_DEBUG(__VA_ARGS__)
#define REXWGPU_INFO(...) REXGPU_INFO(__VA_ARGS__)
#define REXWGPU_WARN(...) REXGPU_WARN(__VA_ARGS__)
#define REXWGPU_ERROR(...) REXGPU_ERROR(__VA_ARGS__)
#endif

namespace rex::graphics::webgpu {

// WGPUStringView -> std::string_view (null data = empty, WGPU_STRLEN = NUL-terminated).
inline std::string_view ToStringView(WGPUStringView view) {
  if (!view.data) {
    return {};
  }
  if (view.length == WGPU_STRLEN) {
    return std::string_view(view.data);
  }
  return std::string_view(view.data, view.length);
}

inline WGPUStringView MakeStringView(std::string_view view) {
  return WGPUStringView{view.data(), view.size()};
}

}  // namespace rex::graphics::webgpu
