/**
 * @file        rex/ui/webgpu/webgpu_provider.h
 * @brief       ui::GraphicsProvider over a graphics::webgpu::WebGPUDevice (Emscripten).
 *
 * The device (and the canvas surface) belong to WebGPUGraphicsSystem, which
 * creates it on the main() pthread before GraphicsSystem::Setup and destroys it
 * after the presenter and this provider. The provider only references it.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>

#include <rex/graphics/webgpu/webgpu_device.h>
#include <rex/ui/graphics_provider.h>

namespace rex {
namespace ui {
namespace webgpu {

class WebGPUProvider final : public GraphicsProvider {
 public:
  // Never fails for a live device; returns nullptr if the device is lost.
  static std::unique_ptr<WebGPUProvider> Create(graphics::webgpu::WebGPUDevice& device);

  graphics::webgpu::WebGPUDevice& device() const { return device_; }

  // UI thread (GraphicsSystem::Setup). Returns a WebGPUPresenter, or nullptr if
  // the device has no canvas surface or initialization fails.
  std::unique_ptr<Presenter> CreatePresenter(
      Presenter::HostGpuLossCallback host_gpu_loss_callback =
          Presenter::FatalErrorHostGpuLossCallback) override;

  // No WebGPU immediate drawer (ImGui overlay) until gate 4: nullptr, so the app
  // creates no ImGui drawer.
  std::unique_ptr<ImmediateDrawer> CreateImmediateDrawer() override;

 private:
  explicit WebGPUProvider(graphics::webgpu::WebGPUDevice& device) : device_(device) {}

  graphics::webgpu::WebGPUDevice& device_;
};

}  // namespace webgpu
}  // namespace ui
}  // namespace rex
