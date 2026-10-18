/**
 * @file        graphics/webgpu/webgpu_graphics_system.cpp
 * @brief       See rex/graphics/webgpu/webgpu_graphics_system.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/webgpu_graphics_system.h>

#include <emscripten/emscripten.h>

#include <rex/cvar.h>
#include <rex/graphics/null/command_processor.h>
#include <rex/graphics/null/graphics_system.h>
#include <rex/graphics/webgpu/webgpu_command_processor.h>
#include <rex/graphics/webgpu/webgpu_device.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

// The presenter and its GraphicsProvider are the presenter package's (WP6,
// ui/webgpu). Built without it, the system runs headless (no presenter).
#if __has_include(<rex/ui/webgpu/webgpu_provider.h>)
#include <rex/ui/webgpu/webgpu_provider.h>
#define REX_WEBGPU_HAS_PROVIDER 1
#else
#define REX_WEBGPU_HAS_PROVIDER 0
#endif

REXCVAR_DEFINE_STRING(gpu, "webgpu", "GPU",
                      "Web GPU backend: webgpu (falls back to null without a device) or null "
                      "(PM4 executes, nothing is drawn)")
    .allowed({"webgpu", "null"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

EM_JS(int, rexwgpu_navigator_gpu_available, (), {
  return (typeof navigator !== 'undefined' && navigator.gpu) ? 1 : 0;
});

}  // namespace

namespace rex::graphics::webgpu {

WebGPUGraphicsSystem::WebGPUGraphicsSystem() = default;

WebGPUGraphicsSystem::~WebGPUGraphicsSystem() = default;

bool WebGPUGraphicsSystem::IsAvailable() {
  return rexwgpu_navigator_gpu_available() != 0;
}

std::string WebGPUGraphicsSystem::name() const {
  return device_ ? "WebGPU" : "WebGPU (unavailable, null fallback)";
}

X_STATUS WebGPUGraphicsSystem::Setup(runtime::FunctionDispatcher* function_dispatcher,
                                     system::KernelState* kernel_state,
                                     ui::WindowedAppContext* app_context, bool with_presentation) {
  if (IsAvailable()) {
    WebGPUDevice::Options options;
    // Setup runs on the main() pthread, which owns the canvas under
    // PROXY_TO_PTHREAD; Create transfers it to the device thread.
    if (!with_presentation) {
      options.canvas_selector.clear();
    }
    device_ = WebGPUDevice::Create(options);
  } else {
    REXGPU_WARN("WebGPU: navigator.gpu is not available in this worker");
  }

  if (device_) {
    REXGPU_INFO("WebGPU graphics system: device ready ({}), canvas surface: {}",
                device_->adapter_description(), device_->has_surface());
#if REX_WEBGPU_HAS_PROVIDER
    if (with_presentation) {
      provider_ = ::rex::ui::webgpu::WebGPUProvider::Create(*device_);
      if (!provider_) {
        REXGPU_WARN("WebGPU: presenter provider creation failed; running without presentation");
      }
    }
#else
    REXGPU_WARN("WebGPU: built without the WebGPU presenter (ui/webgpu); nothing is presented");
#endif
  } else {
    REXGPU_WARN(
        "[STUB] WebGPU device unavailable: falling back to the null command processor (guest "
        "command buffers execute, nothing is rendered)");
  }

  return GraphicsSystem::Setup(function_dispatcher, kernel_state, app_context, with_presentation);
}

void WebGPUGraphicsSystem::Shutdown() {
  // Releases the command processor (its ShutdownContext drains the streams),
  // the presenter and the provider, all of which use the device.
  if (command_processor_ && command_processor_->is_paused()) {
    command_processor_->Resume();
  }
  GraphicsSystem::Shutdown();
  device_.reset();
}

std::unique_ptr<CommandProcessor> WebGPUGraphicsSystem::CreateCommandProcessor() {
  if (!device_) {
    return std::make_unique<null::NullCommandProcessor>(this, kernel_state_);
  }
  return std::make_unique<WebGPUCommandProcessor>(this, kernel_state_);
}

std::unique_ptr<GraphicsSystem> CreateWebGraphicsSystem() {
  if (REXCVAR_GET(gpu) == "null") {
    REXGPU_INFO("GPU backend: null (gpu=null)");
    return std::make_unique<null::NullGraphicsSystem>();
  }
  if (!WebGPUGraphicsSystem::IsAvailable()) {
    REXGPU_WARN("GPU backend: null (no navigator.gpu in this worker)");
    return std::make_unique<null::NullGraphicsSystem>();
  }
  REXGPU_INFO("GPU backend: WebGPU");
  return std::make_unique<WebGPUGraphicsSystem>();
}

}  // namespace rex::graphics::webgpu
