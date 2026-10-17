/**
 * @file        rex/graphics/webgpu/webgpu_guest_output.h
 * @brief       Contract between the WebGPU command processor and the presenter for the
 *              guest output image (the mailbox texture written on IssueSwap).
 *
 * Header only, so the presenter (ui::webgpu, WP6) and the command processor
 * (graphics::webgpu, WP1) only share this interface.
 *
 * Flow: WebGPUCommandProcessor::IssueSwap calls
 * ui::Presenter::RefreshGuestOutput(width, height, aspect_x, aspect_y, refresher).
 * The presenter's RefreshGuestOutputImpl passes a context deriving from
 * WebGPUGuestOutputRefreshContext. The refresher (CP thread) static_casts to it,
 * records the writes to texture()/view() into its WebGPUCommandStream, submits
 * the stream and blocks until the device thread has replayed it and called
 * wgpuQueueSubmit. So when the refresher returns true, every write to the
 * texture is already on the device queue, and the presenter may publish the
 * mailbox slot at once: a later paint on the device thread is ordered after it
 * by the queue.
 *
 * texture()/view() are device-thread handles (emdawnwebgpu objects live in the
 * device worker's table). The CP never calls WebGPU with them on its own
 * thread; it only stores their values in the stream.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>

#include <webgpu/webgpu.h>

#include <rex/ui/presenter.h>

namespace rex::graphics::webgpu {

// Guest output (mailbox) texture format: 10 bits per channel like the Xenos
// 2_10_10_10 front buffers, filterable for the present blit.
constexpr WGPUTextureFormat kGuestOutputFormat = WGPUTextureFormat_RGB10A2Unorm;
// Usage the presenter must create the mailbox textures with: the CP renders
// (gamma / scale passes) or copies into them; the presenter samples them; tests
// and CaptureGuestOutput copy out of them.
constexpr WGPUTextureUsage kGuestOutputTextureUsage =
    WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding |
    WGPUTextureUsage_CopyDst | WGPUTextureUsage_CopySrc;

class WebGPUGuestOutputRefreshContext : public ::rex::ui::Presenter::GuestOutputRefreshContext {
 public:
  // The mailbox texture being written, kGuestOutputFormat, width() x height(),
  // one mip, one layer, kGuestOutputTextureUsage.
  virtual WGPUTexture texture() const = 0;
  // A 2D view of texture() usable as a render attachment and for sampling.
  virtual WGPUTextureView view() const = 0;

  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }

 protected:
  WebGPUGuestOutputRefreshContext(bool& is_8bpc_out_ref, uint32_t width, uint32_t height)
      : GuestOutputRefreshContext(is_8bpc_out_ref), width_(width), height_(height) {}

 private:
  uint32_t width_;
  uint32_t height_;
};

}  // namespace rex::graphics::webgpu
