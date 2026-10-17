/**
 * @file        rex/graphics/webgpu/webgpu_graphics_system.h
 * @brief       WebGPU GraphicsSystem: owns the WebGPU device (thread) and the command processor.
 *
 * Setup runs on the main() pthread (ReXApp::OnInitialize under
 * PROXY_TO_PTHREAD), which owns "#canvas"; WebGPUDevice::Create moves the canvas
 * to the device thread there. If <rex/ui/webgpu/webgpu_provider.h> is built
 * (WP6), its WebGPUProvider supplies the presenter. If no device can be created
 * (no navigator.gpu in the worker, adapter or device request failed), the
 * system falls back to the null command processor (PM4 executes, nothing is
 * drawn), so the guest still runs.
 *
 * The device outlives the presenter and the provider: Shutdown destroys it
 * after GraphicsSystem::Shutdown has released them.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <string>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>

namespace rex::graphics::webgpu {

class WebGPUDevice;

class WebGPUGraphicsSystem : public GraphicsSystem {
 public:
  WebGPUGraphicsSystem();
  ~WebGPUGraphicsSystem() override;

  // Whether this worker exposes navigator.gpu (cheap; the adapter may still be
  // refused at Setup, which then falls back to the null command processor).
  static bool IsAvailable();

  std::string name() const override;

  X_STATUS Setup(runtime::FunctionDispatcher* function_dispatcher,
                 system::KernelState* kernel_state, ui::WindowedAppContext* app_context,
                 bool with_presentation) override;
  void Shutdown() override;

  // Null if Setup fell back to the null command processor.
  WebGPUDevice* webgpu_device() const { return device_.get(); }

 private:
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;

  std::unique_ptr<WebGPUDevice> device_;
};

// The web default backend (ReXApp): WebGPUGraphicsSystem unless the `gpu` cvar
// is "null" or the worker has no navigator.gpu, else null::NullGraphicsSystem.
std::unique_ptr<GraphicsSystem> CreateWebGraphicsSystem();

}  // namespace rex::graphics::webgpu
