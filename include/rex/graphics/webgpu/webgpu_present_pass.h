/**
 * @file        rex/graphics/webgpu/webgpu_present_pass.h
 * @brief       Canvas present pass: clear (letterbox) + bilinear guest output blit.
 *
 * Device-thread-only. This is the GPU half of WebGPUPresenter::PaintAndPresent
 * and of the standalone clear-color test page (tools/webgpu_clear). After
 * PresentToSurface returns, the device thread must go back to its event loop
 * for the browser to show the frame.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <vector>

#include <webgpu/webgpu.h>

namespace rex::graphics::webgpu {

class WebGPUDevice;

class WebGPUPresentPass {
 public:
  struct Frame {
    // Letterbox / background color (the whole target is cleared to it).
    WGPUColor clear_color = {0.0, 0.0, 0.0, 1.0};
    // Optional source image (any filterable float 2D view, e.g. the rgb10a2unorm
    // guest output) stretched with bilinear filtering into the destination
    // rectangle, which may extend past the target (overscan); it is clipped.
    WGPUTextureView source = nullptr;
    int32_t dest_x = 0;
    int32_t dest_y = 0;
    uint32_t dest_width = 0;
    uint32_t dest_height = 0;
    // Optional: copy the whole presented image here after drawing (usage must
    // include CopyDst, row pitch ReadbackRowPitch(width), at least
    // ReadbackRowPitch(width) * height bytes). For tests and capture.
    WGPUBuffer readback_buffer = nullptr;
  };

  enum class Result {
    kPresented,
    // Zero-sized canvas, outdated surface, etc.; try again next frame.
    kSkipped,
    kError,
    kDeviceLost,
  };

  explicit WebGPUPresentPass(WebGPUDevice& device) : device_(device) {}
  ~WebGPUPresentPass();
  WebGPUPresentPass(const WebGPUPresentPass&) = delete;
  WebGPUPresentPass& operator=(const WebGPUPresentPass&) = delete;

  bool Initialize();

  // Acquires the canvas texture (reconfiguring the surface on resize), clears
  // it, draws the source, optionally copies it to frame.readback_buffer and
  // submits. Outputs the surface size used.
  Result PresentToSurface(const Frame& frame, uint32_t& width_out, uint32_t& height_out);

  // Encodes the same clear + blit into an arbitrary render target.
  bool EncodeClearAndBlit(WGPUCommandEncoder encoder, WGPUTextureView target,
                          WGPUTextureFormat target_format, uint32_t target_width,
                          uint32_t target_height, const Frame& frame);

  static constexpr uint32_t ReadbackRowPitch(uint32_t width) {
    return (width * 4 + 255) & ~uint32_t(255);
  }

 private:
  struct Pipeline {
    WGPUTextureFormat format;
    WGPURenderPipeline pipeline;
    WGPUBindGroupLayout bind_group_layout;
  };
  const Pipeline* GetPipeline(WGPUTextureFormat format);

  WebGPUDevice& device_;
  WGPUShaderModule shader_module_ = nullptr;
  WGPUSampler sampler_ = nullptr;
  WGPUBuffer uniform_buffer_ = nullptr;
  uint32_t uniform_slot_ = 0;
  std::vector<Pipeline> pipelines_;
};

}  // namespace rex::graphics::webgpu
