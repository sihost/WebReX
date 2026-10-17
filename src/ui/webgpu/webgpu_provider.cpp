/**
 * @file        ui/webgpu/webgpu_provider.cpp
 * @brief       ui::GraphicsProvider over a graphics::webgpu::WebGPUDevice.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/webgpu/webgpu_provider.h>

#include <rex/graphics/webgpu/webgpu_log.h>
#include <rex/ui/webgpu/webgpu_presenter.h>

namespace rex {
namespace ui {
namespace webgpu {

std::unique_ptr<WebGPUProvider> WebGPUProvider::Create(graphics::webgpu::WebGPUDevice& device) {
  if (device.is_lost()) {
    REXWGPU_ERROR("WebGPUProvider: the WebGPU device is lost");
    return nullptr;
  }
  return std::unique_ptr<WebGPUProvider>(new WebGPUProvider(device));
}

std::unique_ptr<Presenter> WebGPUProvider::CreatePresenter(
    Presenter::HostGpuLossCallback host_gpu_loss_callback) {
  return WebGPUPresenter::Create(host_gpu_loss_callback, device_);
}

std::unique_ptr<ImmediateDrawer> WebGPUProvider::CreateImmediateDrawer() { return nullptr; }

}  // namespace webgpu
}  // namespace ui
}  // namespace rex
