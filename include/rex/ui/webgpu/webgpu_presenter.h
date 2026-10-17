/**
 * @file        rex/ui/webgpu/webgpu_presenter.h
 * @brief       ui::Presenter for WebGPU on an HTML canvas (Emscripten).
 *
 * Threads (docs/port/webgpu.md sections 1 and 10 WP6):
 * - Guest output thread (the WebGPU command processor): RefreshGuestOutput.
 *   RefreshGuestOutputImpl gives the refresher a
 *   graphics::webgpu::WebGPUGuestOutputRefreshContext for one of the 3 mailbox
 *   textures (rgb10a2unorm, created on the device thread when the size
 *   changes). The refresher records the writes into the CP stream and returns
 *   only after the device thread has submitted them (webgpu_guest_output.h), so
 *   the slot is published right away.
 * - WebGPU device thread: a requestAnimationFrame loop. Every animation frame it
 *   reconfigures the canvas surface if the canvas size changed and, if there is
 *   anything new (a new mailbox image, resize, paint request, readback), takes
 *   the latest guest output (ConsumeGuestOutput), works out the letterboxed
 *   rectangle (GetGuestOutputPaintFlow, bilinear) and draws it with
 *   WebGPUPresentPass. The browser shows the frame when the device thread goes
 *   back to its event loop. All consumers of the mailbox (painting, capture) run
 *   on this thread, so the consumer lock never blocks across threads.
 * - UI thread: connects/disconnects painting (starts/stops the loop).
 *   PaintAndPresentImpl only flags a repaint for the next animation frame.
 *   Painting is paced by the browser, so the connection reports implicit vsync
 *   and the guest output thread never waits for presentation.
 * - Device loss: the loop stops and asks the UI thread to paint (forced).
 *   PaintAndPresentImpl then returns kGpuLostExternally, which goes to the
 *   presenter's host GPU loss callback. RefreshGuestOutputImpl and
 *   CaptureGuestOutput fail once the device is lost.
 *
 * UI drawers (ImGui) aren't drawn yet: the provider has no immediate drawer
 * until gate 4.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include <webgpu/webgpu.h>

#include <rex/graphics/webgpu/webgpu_device.h>
#include <rex/graphics/webgpu/webgpu_guest_output.h>
#include <rex/graphics/webgpu/webgpu_present_pass.h>
#include <rex/ui/presenter.h>
#include <rex/ui/surface.h>

namespace rex {
namespace ui {

class WebCanvasSurface;

namespace webgpu {

class WebGPUPresenter final : public Presenter {
 public:
  // Called on the device thread with the pixels of a presented frame (rows
  // top-down, row_pitch bytes apart, in the canvas format, usually
  // BGRA8Unorm or RGBA8Unorm).
  using ReadbackCallback =
      std::function<void(uint32_t width, uint32_t height, WGPUTextureFormat format,
                         const uint8_t* pixels, uint32_t row_pitch)>;

  struct PaintStats {
    // Animation frame callbacks run since painting was connected.
    uint64_t animation_frames = 0;
    // Frames actually drawn to the canvas.
    uint64_t presented_frames = 0;
    // Surface (re)configurations for a new canvas size.
    uint64_t surface_resizes = 0;
    // Animation frame index of the latest reconfiguration (frames are counted by
    // animation_frames), and of the first frame presented after it.
    uint64_t last_resize_frame = 0;
    uint64_t last_resize_presented_frame = 0;
    // Size of the canvas texture of the last presented frame.
    uint32_t width = 0;
    uint32_t height = 0;
    // Destination rectangle of the guest output in the last presented frame
    // (0 size if no guest output was drawn).
    int32_t guest_output_x = 0;
    int32_t guest_output_y = 0;
    uint32_t guest_output_width = 0;
    uint32_t guest_output_height = 0;
    bool device_lost = false;
  };

  static std::unique_ptr<WebGPUPresenter> Create(HostGpuLossCallback host_gpu_loss_callback,
                                                 graphics::webgpu::WebGPUDevice& device);

  ~WebGPUPresenter() override;

  graphics::webgpu::WebGPUDevice& device() const { return device_; }

  Surface::TypeFlags GetSupportedSurfaceTypes() const override;

  // Any thread except the device thread.
  bool CaptureGuestOutput(RawImage& image_out) override;

  // Any thread. Color of the letterbox / of the whole canvas without guest
  // output (linear 0..1 values written to the canvas as is). Default black.
  void SetBackgroundColor(float r, float g, float b);

  // Any thread.
  PaintStats GetPaintStats() const;

  // Any thread. While enabled, the intervals between animation frame callbacks
  // (milliseconds, from the rAF timestamps) are recorded; Take returns and
  // clears them.
  void SetAnimationFrameIntervalRecording(bool enabled);
  std::vector<float> TakeAnimationFrameIntervals();

  // Any thread. The next presented frame (forced, even without changes) is
  // copied back and passed to the callback on the device thread.
  void RequestPresentedFrameReadback(ReadbackCallback callback);

  // Any thread. Tests: destroys the WebGPU device (the device lost path).
  void LoseDeviceForTesting();

 protected:
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      Surface& new_surface, uint32_t new_surface_width, uint32_t new_surface_height,
      bool was_paintable, bool& is_vsync_implicit_out) override;
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override;
  bool RefreshGuestOutputImpl(uint32_t mailbox_index, uint32_t frontbuffer_width,
                              uint32_t frontbuffer_height,
                              std::function<bool(GuestOutputRefreshContext& context)> refresher,
                              bool& is_8bpc_out_ref) override;
  PaintResult PaintAndPresentImpl(bool execute_ui_drawers) override;

 private:
  struct GuestOutputImage {
    WGPUTexture texture = nullptr;
    WGPUTextureView view = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
  };

  class RefreshContext;

  WebGPUPresenter(HostGpuLossCallback host_gpu_loss_callback,
                  graphics::webgpu::WebGPUDevice& device)
      : Presenter(host_gpu_loss_callback), device_(device) {}

  bool Initialize();

  // Device thread.
  void StartAnimationFrameLoop();
  void StopAnimationFrameLoop();
  static bool AnimationFrameThunk(double time, void* user_data);
  void OnAnimationFrame(double time);
  void PaintOnDeviceThread(uint32_t canvas_width, uint32_t canvas_height);
  void ReleaseGuestOutputImage(GuestOutputImage& image);

  graphics::webgpu::WebGPUDevice& device_;

  // Device thread objects.
  std::unique_ptr<graphics::webgpu::WebGPUPresentPass> present_pass_;
  std::array<GuestOutputImage, kGuestOutputMailboxSize> guest_output_images_;
  bool loop_should_run_ = false;
  int animation_frame_id_ = 0;  // Pending requestAnimationFrame, 0 if none.
  double last_animation_frame_time_ = -1.0;
  uint32_t last_painted_mailbox_index_ = UINT32_MAX;
  uint32_t last_painted_width_ = 0;
  uint32_t last_painted_height_ = 0;
  bool loss_reported_ = false;
  uint64_t resize_pending_presentation_frame_ = 0;

  // Guest output thread (the only producer): its copy of the mailbox image
  // handles, so an unchanged size needs no device-thread round trip.
  std::array<GuestOutputImage, kGuestOutputMailboxSize> producer_images_;

  // UI thread.
  bool painting_connected_ = false;

  // Any thread.
  std::atomic<bool> paint_requested_{false};
  std::atomic<uint32_t> background_color_rgb_{0};  // R | G << 8 | B << 16 (unorm8).
  // Whether the device thread should ask the window to repaint on device loss.
  std::atomic<bool> notify_window_on_loss_{false};

  mutable std::mutex stats_mutex_;
  PaintStats stats_;
  bool record_intervals_ = false;
  std::vector<float> intervals_;
  std::vector<ReadbackCallback> pending_readbacks_;
};

}  // namespace webgpu
}  // namespace ui
}  // namespace rex
