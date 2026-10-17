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
#include <functional>
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
    // Optional guest display gamma ramp applied to the source (the command
    // processor's swap, as VulkanCommandProcessor's apply-gamma pass): the source
    // texel is read exactly (no filtering; the destination should be the source
    // size) and mapped through kTable256 = 256 DC_LUT_30_COLOR dwords indexed by
    // the 8-bit value, or kPwl = 128 x 3 DC_LUT_PWL_DATA dwords (10-bit value,
    // increment 3). gamma_rb_swap swaps red and blue of the source first.
    // One gamma blit per submission (the ramp buffer is rewritten each time).
    enum class GammaRamp : uint32_t { kNone, kTable256, kPwl };
    GammaRamp gamma_ramp = GammaRamp::kNone;
    const uint32_t* gamma_ramp_data = nullptr;
    bool gamma_rb_swap = false;
    // Optional: PresentToSurface encodes this after the blit into the same canvas
    // texture (in its own render passes, loading the blit result), before the
    // readback copy. The UI overlay (WebGPUImmediateDrawer recordings).
    std::function<void(WGPUCommandEncoder encoder, WGPUTextureView target,
                       WGPUTextureFormat target_format, uint32_t target_width,
                       uint32_t target_height)>
        overlay;
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
    bool gamma;
    WGPURenderPipeline pipeline;
    WGPUBindGroupLayout bind_group_layout;
  };
  const Pipeline* GetPipeline(WGPUTextureFormat format, bool gamma);

  WebGPUDevice& device_;
  WGPUShaderModule shader_module_ = nullptr;
  WGPUShaderModule gamma_shader_module_ = nullptr;
  // 384 dwords (the larger of the two ramp layouts).
  WGPUBuffer gamma_ramp_buffer_ = nullptr;
  WGPUSampler sampler_ = nullptr;
  WGPUBuffer uniform_buffer_ = nullptr;
  uint32_t uniform_slot_ = 0;
  std::vector<Pipeline> pipelines_;
};

}  // namespace rex::graphics::webgpu
