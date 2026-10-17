/**
 * @file        ui/webgpu/webgpu_presenter.cpp
 * @brief       WebGPU canvas presenter: guest output mailbox + rAF paint loop.
 *
 * See rex/ui/webgpu/webgpu_presenter.h for the threading model.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/webgpu/webgpu_presenter.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <utility>

#include <emscripten/html5.h>

#include <rex/graphics/webgpu/webgpu_log.h>
#include <rex/ui/surface_web.h>
#include <rex/ui/webgpu/webgpu_immediate_drawer.h>

namespace rex {
namespace ui {
namespace webgpu {

using graphics::webgpu::MakeStringView;
using graphics::webgpu::ToStringView;
using graphics::webgpu::WebGPUDevice;
using graphics::webgpu::WebGPUPresentPass;

class WebGPUPresenter::RefreshContext final
    : public graphics::webgpu::WebGPUGuestOutputRefreshContext {
 public:
  RefreshContext(bool& is_8bpc_out_ref, uint32_t width, uint32_t height, WGPUTexture texture,
                 WGPUTextureView view)
      : WebGPUGuestOutputRefreshContext(is_8bpc_out_ref, width, height),
        texture_(texture),
        view_(view) {}

  WGPUTexture texture() const override { return texture_; }
  WGPUTextureView view() const override { return view_; }

 private:
  WGPUTexture texture_;
  WGPUTextureView view_;
};

namespace {

class WebGPUUIDrawContext final : public UIDrawContext {
 public:
  WebGPUUIDrawContext(Presenter& presenter, uint32_t render_target_width,
                      uint32_t render_target_height)
      : UIDrawContext(presenter, render_target_width, render_target_height) {}
};

// State of a presented-frame readback in flight (device thread, owned by the
// mapAsync callback; independent of the presenter's lifetime).
struct FrameReadback {
  WGPUBuffer buffer = nullptr;
  uint64_t size = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t row_pitch = 0;
  WGPUTextureFormat format = WGPUTextureFormat_Undefined;
  std::vector<WebGPUPresenter::ReadbackCallback> callbacks;
};

void OnFrameReadbackMapped(WGPUMapAsyncStatus status, WGPUStringView message, void* userdata1,
                           void* /*userdata2*/) {
  std::unique_ptr<FrameReadback> readback(static_cast<FrameReadback*>(userdata1));
  if (status == WGPUMapAsyncStatus_Success) {
    auto* pixels = static_cast<const uint8_t*>(
        wgpuBufferGetConstMappedRange(readback->buffer, 0, size_t(readback->size)));
    if (pixels) {
      for (const WebGPUPresenter::ReadbackCallback& callback : readback->callbacks) {
        callback(readback->width, readback->height, readback->format, pixels,
                 readback->row_pitch);
      }
    }
    wgpuBufferUnmap(readback->buffer);
  } else {
    REXWGPU_ERROR("WebGPUPresenter: frame readback mapAsync failed (status {}): {}",
                  uint32_t(status), ToStringView(message));
    for (const WebGPUPresenter::ReadbackCallback& callback : readback->callbacks) {
      callback(0, 0, readback->format, nullptr, 0);
    }
  }
  wgpuBufferRelease(readback->buffer);
}

}  // namespace

std::unique_ptr<WebGPUPresenter> WebGPUPresenter::Create(
    HostGpuLossCallback host_gpu_loss_callback, WebGPUDevice& device) {
  std::unique_ptr<WebGPUPresenter> presenter(new WebGPUPresenter(host_gpu_loss_callback, device));
  if (!presenter->Initialize()) {
    return nullptr;
  }
  return presenter;
}

bool WebGPUPresenter::Initialize() {
  if (!InitializeCommonSurfaceIndependent()) {
    REXWGPU_ERROR("WebGPUPresenter: common initialization failed");
    return false;
  }
  if (device_.is_lost()) {
    return false;
  }
  bool success = false;
  device_.RunSync([this, &success]() {
    present_pass_ = std::make_unique<WebGPUPresentPass>(device_);
    success = present_pass_->Initialize();
    if (!success) {
      present_pass_.reset();
    }
  });
  if (!success) {
    REXWGPU_ERROR("WebGPUPresenter: present pass initialization failed");
    return false;
  }
  if (!device_.has_surface()) {
    REXWGPU_WARN(
        "WebGPUPresenter: the WebGPU device has no canvas surface; guest output can be "
        "captured but not shown");
  }
  return true;
}

WebGPUPresenter::~WebGPUPresenter() {
  // The base destructor detaches from the window (disconnecting painting via
  // our Impl would be too late then, as this part is already destroyed), so stop
  // everything that calls back into this object first.
  device_.RunSync([this]() {
    StopAnimationFrameLoop();
    for (GuestOutputImage& image : guest_output_images_) {
      ReleaseGuestOutputImage(image);
    }
    present_pass_.reset();
    // The recordings' resources are released here (on the device thread).
    std::lock_guard<std::mutex> lock(ui_mutex_);
    ui_recordings_.reset();
  });
  painting_connected_ = false;
}

Surface::TypeFlags WebGPUPresenter::GetSupportedSurfaceTypes() const {
  return device_.has_surface() ? Surface::kTypeFlag_WebCanvas : 0;
}

void WebGPUPresenter::SetBackgroundColor(float r, float g, float b) {
  auto to_unorm8 = [](float value) -> uint32_t {
    value = std::min(1.0f, std::max(0.0f, value));  // Also drops NaN.
    return uint32_t(value * 255.0f + 0.5f);
  };
  uint32_t packed = to_unorm8(r) | (to_unorm8(g) << 8) | (to_unorm8(b) << 16);
  if (background_color_rgb_.exchange(packed, std::memory_order_relaxed) != packed) {
    paint_requested_.store(true, std::memory_order_release);
  }
}

WebGPUPresenter::PaintStats WebGPUPresenter::GetPaintStats() const {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  return stats_;
}

void WebGPUPresenter::SetAnimationFrameIntervalRecording(bool enabled) {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  record_intervals_ = enabled;
  if (!enabled) {
    intervals_.clear();
  }
}

std::vector<float> WebGPUPresenter::TakeAnimationFrameIntervals() {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  std::vector<float> intervals;
  intervals.swap(intervals_);
  return intervals;
}

void WebGPUPresenter::RequestPresentedFrameReadback(ReadbackCallback callback) {
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    pending_readbacks_.push_back(std::move(callback));
  }
  paint_requested_.store(true, std::memory_order_release);
}

void WebGPUPresenter::LoseDeviceForTesting() {
  WebGPUDevice* device = &device_;
  device_.RunAsync([device]() {
    REXWGPU_WARN("WebGPUPresenter: destroying the WebGPU device (test)");
    wgpuDeviceDestroy(device->device());
  });
}

Presenter::SurfacePaintConnectResult WebGPUPresenter::ConnectOrReconnectPaintingToSurfaceFromUIThread(
    Surface& new_surface, uint32_t /*new_surface_width*/, uint32_t /*new_surface_height*/,
    bool /*was_paintable*/, bool& is_vsync_implicit_out) {
  if (new_surface.GetType() != Surface::kTypeIndex_WebCanvas || !device_.has_surface()) {
    return SurfacePaintConnectResult::kFailureSurfaceUnusable;
  }
  if (device_.is_lost()) {
    return SurfacePaintConnectResult::kFailure;
  }
  // The browser paces painting (requestAnimationFrame); the guest output thread
  // must never wait for it.
  is_vsync_implicit_out = true;
  if (painting_connected_) {
    // Resizes are handled by the device thread on the next animation frame.
    paint_requested_.store(true, std::memory_order_release);
    return SurfacePaintConnectResult::kSuccessUnchanged;
  }
  if (!device_.RunSync([this]() {
        last_painted_width_ = 0;
        last_painted_height_ = 0;
        last_painted_mailbox_index_ = UINT32_MAX;
        StartAnimationFrameLoop();
      })) {
    return SurfacePaintConnectResult::kFailure;
  }
  painting_connected_ = true;
  notify_window_on_loss_.store(true, std::memory_order_release);
  paint_requested_.store(true, std::memory_order_release);
  return SurfacePaintConnectResult::kSuccess;
}

void WebGPUPresenter::DisconnectPaintingFromSurfaceFromUIThreadImpl() {
  notify_window_on_loss_.store(false, std::memory_order_release);
  device_.RunSync([this]() { StopAnimationFrameLoop(); });
  painting_connected_ = false;
}

bool WebGPUPresenter::RefreshGuestOutputImpl(
    uint32_t mailbox_index, uint32_t frontbuffer_width, uint32_t frontbuffer_height,
    std::function<bool(GuestOutputRefreshContext& context)> refresher, bool& is_8bpc_out_ref) {
  if (device_.is_lost()) {
    return false;
  }
  GuestOutputImage& producer_image = producer_images_[mailbox_index];
  if (!producer_image.texture || producer_image.width != frontbuffer_width ||
      producer_image.height != frontbuffer_height) {
    // The writable slot is neither acquired nor ready, so the device thread
    // doesn't read it; replacing it there is safe (in-flight GPU work keeps the
    // old texture alive through WebGPU reference counting).
    GuestOutputImage created;
    device_.RunSync([&]() {
      GuestOutputImage& image = guest_output_images_[mailbox_index];
      ReleaseGuestOutputImage(image);
      WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
      desc.label = MakeStringView("rex guest output");
      desc.usage = graphics::webgpu::kGuestOutputTextureUsage;
      desc.dimension = WGPUTextureDimension_2D;
      desc.size = {frontbuffer_width, frontbuffer_height, 1};
      desc.format = graphics::webgpu::kGuestOutputFormat;
      desc.mipLevelCount = 1;
      desc.sampleCount = 1;
      image.texture = wgpuDeviceCreateTexture(device_.device(), &desc);
      if (image.texture) {
        image.view = wgpuTextureCreateView(image.texture, nullptr);
        image.width = frontbuffer_width;
        image.height = frontbuffer_height;
      }
      if (!image.view) {
        ReleaseGuestOutputImage(image);
      }
      created = image;
    });
    producer_image = created;
    if (!producer_image.texture) {
      REXWGPU_ERROR("WebGPUPresenter: failed to create a {}x{} guest output texture",
                    frontbuffer_width, frontbuffer_height);
      return false;
    }
  }
  RefreshContext context(is_8bpc_out_ref, frontbuffer_width, frontbuffer_height,
                         producer_image.texture, producer_image.view);
  // The refresher returns after its writes are submitted on the device thread
  // (webgpu_guest_output.h), so the slot can be published right away.
  return refresher(context);
}

void WebGPUPresenter::AddUIRecordingFromUIThread(
    std::shared_ptr<const WebGPUImmediateRecording> recording) {
  if (in_ui_paint_ && recording && !recording->empty()) {
    ui_recordings_building_.push_back(std::move(recording));
  }
}

Presenter::PaintResult WebGPUPresenter::PaintAndPresentImpl(bool execute_ui_drawers) {
  if (device_.is_lost()) {
    return PaintResult::kGpuLostExternally;
  }
  std::shared_ptr<const UIRecordingList> new_ui;
  if (execute_ui_drawers) {
    // Pacing (see the header): at most one UI paint per animation frame.
    {
      std::unique_lock<std::mutex> lock(stats_mutex_);
      animation_frame_cv_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
        return stats_.animation_frames != last_ui_paint_animation_frame_ || stats_.device_lost;
      });
      last_ui_paint_animation_frame_ = stats_.animation_frames;
      ++stats_.ui_paints;
    }
    // The canvas size the device thread last configured (the canvas texture of
    // the next paint, unless it is resized in between).
    uint32_t width = 0, height = 0;
    device_.GetPublishedCanvasSize(width, height);
    if (width && height) {
      WebGPUUIDrawContext ui_draw_context(*this, width, height);
      ui_recordings_building_.clear();
      in_ui_paint_ = true;
      ExecuteUIDrawersFromUIThread(ui_draw_context);
      in_ui_paint_ = false;
      if (!ui_recordings_building_.empty()) {
        new_ui = std::make_shared<const UIRecordingList>(std::move(ui_recordings_building_));
      }
      ui_recordings_building_.clear();
    }
  }
  bool has_ui = new_ui != nullptr;
  if (has_ui != ui_overlay_shown_) {
    // Transitions only (an overlay opened / closed), for logs and tests.
    ui_overlay_shown_ = has_ui;
    size_t draws = 0;
    if (new_ui) {
      for (const auto& recording : *new_ui) {
        draws += recording->draw_count();
      }
    }
    REXWGPU_INFO("WebGPUPresenter: UI overlay {} ({} recordings, {} draws, UI paint {})",
                 has_ui ? "shown" : "hidden", new_ui ? new_ui->size() : 0, draws,
                 GetPaintStats().ui_paints);
  }
  bool had_ui;
  {
    std::lock_guard<std::mutex> lock(ui_mutex_);
    had_ui = ui_recordings_ != nullptr;
    ui_recordings_ = std::move(new_ui);
    if (ui_recordings_) {
      had_ui = true;
    }
  }
  // Painted by the device thread on its next animation frame. A UI paint with
  // no UI before or after doesn't need to repaint the canvas (the device thread
  // repaints new guest output by itself).
  if (!execute_ui_drawers || had_ui) {
    paint_requested_.store(true, std::memory_order_release);
  }
  return PaintResult::kPresented;
}

bool WebGPUPresenter::CaptureGuestOutput(RawImage& image_out) {
  if (device_.IsDeviceThread() || device_.is_lost()) {
    return false;
  }
  struct CaptureState {
    WebGPUPresenter* presenter;
    RawImage* image;
    bool success = false;
    WGPUBuffer buffer = nullptr;
    uint64_t size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t row_pitch = 0;
    WebGPUDevice::Completion* completion = nullptr;
  };
  CaptureState state;
  state.presenter = this;
  state.image = &image_out;
  bool ran = device_.RunSyncWithCompletion([&state](WebGPUDevice::Completion completion) {
    WebGPUPresenter& self = *state.presenter;
    // All mailbox consumers run on the device thread (see the header).
    uint32_t mailbox_index;
    GuestOutputProperties properties;
    std::unique_lock<std::mutex> consumer_lock =
        self.ConsumeGuestOutput(mailbox_index, &properties, nullptr);
    if (mailbox_index == UINT32_MAX || !self.guest_output_images_[mailbox_index].texture) {
      completion.Finish();
      return;
    }
    const GuestOutputImage& image = self.guest_output_images_[mailbox_index];
    state.width = std::min(properties.frontbuffer_width, image.width);
    state.height = std::min(properties.frontbuffer_height, image.height);
    state.row_pitch = WebGPUPresentPass::ReadbackRowPitch(state.width);
    state.size = uint64_t(state.row_pitch) * state.height;
    WGPUBufferDescriptor buffer_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    buffer_desc.label = MakeStringView("rex guest output capture");
    buffer_desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    buffer_desc.size = state.size;
    state.buffer = wgpuDeviceCreateBuffer(self.device_.device(), &buffer_desc);
    if (!state.buffer) {
      completion.Finish();
      return;
    }
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(self.device_.device(), nullptr);
    WGPUTexelCopyTextureInfo source = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    source.texture = image.texture;
    WGPUTexelCopyBufferInfo dest = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    dest.buffer = state.buffer;
    dest.layout.bytesPerRow = state.row_pitch;
    dest.layout.rowsPerImage = state.height;
    WGPUExtent3D copy_size = {state.width, state.height, 1};
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &source, &dest, &copy_size);
    WGPUCommandBuffer command_buffer = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuQueueSubmit(self.device_.queue(), 1, &command_buffer);
    wgpuCommandBufferRelease(command_buffer);
    wgpuCommandEncoderRelease(encoder);
    // Later writes to the image are ordered after the copy by the queue.
    consumer_lock.unlock();

    state.completion = new WebGPUDevice::Completion(completion);
    WGPUBufferMapCallbackInfo callback_info = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    callback_info.mode = WGPUCallbackMode_AllowSpontaneous;
    callback_info.userdata1 = &state;
    callback_info.callback = [](WGPUMapAsyncStatus status, WGPUStringView message,
                                void* userdata1, void*) {
      auto& state = *static_cast<CaptureState*>(userdata1);
      if (status == WGPUMapAsyncStatus_Success) {
        auto* mapped = static_cast<const uint8_t*>(
            wgpuBufferGetConstMappedRange(state.buffer, 0, size_t(state.size)));
        if (mapped) {
          RawImage& image = *state.image;
          image.width = state.width;
          image.height = state.height;
          image.stride = size_t(state.width) * 4;
          image.data.resize(image.stride * state.height);
          for (uint32_t y = 0; y < state.height; ++y) {
            const uint8_t* source_row = mapped + size_t(y) * state.row_pitch;
            uint8_t* dest_row = image.data.data() + image.stride * y;
            for (uint32_t x = 0; x < state.width; ++x) {
              uint32_t rgb10;
              std::memcpy(&rgb10, source_row + x * 4, sizeof(rgb10));
              uint32_t rgba8 = Packed10bpcRGBTo8bpcBytes(rgb10);
              std::memcpy(dest_row + x * 4, &rgba8, sizeof(rgba8));
            }
          }
          state.success = true;
        }
        wgpuBufferUnmap(state.buffer);
      } else {
        REXWGPU_ERROR("WebGPUPresenter: capture mapAsync failed (status {}): {}",
                      uint32_t(status), ToStringView(message));
      }
      wgpuBufferRelease(state.buffer);
      state.buffer = nullptr;
      WebGPUDevice::Completion* completion = state.completion;
      state.completion = nullptr;
      completion->Finish();
      delete completion;
    };
    wgpuBufferMapAsync(state.buffer, WGPUMapMode_Read, 0, size_t(state.size), callback_info);
  });
  return ran && state.success;
}

void WebGPUPresenter::StartAnimationFrameLoop() {
  loop_should_run_ = true;
  last_animation_frame_time_ = -1.0;
  if (!animation_frame_id_) {
    animation_frame_id_ = emscripten_request_animation_frame(AnimationFrameThunk, this);
  }
}

void WebGPUPresenter::StopAnimationFrameLoop() {
  loop_should_run_ = false;
  if (animation_frame_id_) {
    emscripten_cancel_animation_frame(animation_frame_id_);
    animation_frame_id_ = 0;
  }
  last_animation_frame_time_ = -1.0;
}

bool WebGPUPresenter::AnimationFrameThunk(double time, void* user_data) {
  auto* self = static_cast<WebGPUPresenter*>(user_data);
  self->animation_frame_id_ = 0;
  if (!self->loop_should_run_) {
    return false;
  }
  // One-shot requests rather than emscripten_request_animation_frame_loop, so
  // the pending callback can be cancelled when painting is disconnected.
  self->animation_frame_id_ = emscripten_request_animation_frame(AnimationFrameThunk, self);
  self->OnAnimationFrame(time);
  return false;
}

void WebGPUPresenter::OnAnimationFrame(double time) {
  if (device_.is_lost()) {
    StopAnimationFrameLoop();
    if (!loss_reported_) {
      loss_reported_ = true;
      {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.device_lost = true;
      }
      REXWGPU_ERROR("WebGPUPresenter: WebGPU device lost; stopping canvas painting");
      // The UI thread's forced paint gets kGpuLostExternally from
      // PaintAndPresentImpl and invokes the host GPU loss callback.
      if (notify_window_on_loss_.load(std::memory_order_acquire)) {
        WebCanvasSurface::RequestPaintFromAnyThread(true);
      }
    }
    return;
  }

  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.animation_frames;
    if (record_intervals_ && last_animation_frame_time_ >= 0.0) {
      intervals_.push_back(float(time - last_animation_frame_time_));
    }
  }
  animation_frame_cv_.notify_all();
  last_animation_frame_time_ = time;

  // Picks up a canvas resize (the shared size block is updated by the browser
  // main thread at once) on the first animation frame after it.
  uint32_t canvas_width, canvas_height;
  if (!device_.UpdateSurfaceConfiguration(canvas_width, canvas_height)) {
    return;  // Zero-sized canvas.
  }
  PaintOnDeviceThread(canvas_width, canvas_height);
}

void WebGPUPresenter::PaintOnDeviceThread(uint32_t canvas_width, uint32_t canvas_height) {
  bool resized = canvas_width != last_painted_width_ || canvas_height != last_painted_height_;
  std::vector<ReadbackCallback> readbacks;
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    if (resized && last_painted_width_ && !resize_pending_presentation_frame_) {
      ++stats_.surface_resizes;
      stats_.last_resize_frame = stats_.animation_frames;
      resize_pending_presentation_frame_ = stats_.animation_frames;
    }
    readbacks.swap(pending_readbacks_);
  }
  bool paint_requested = paint_requested_.exchange(false, std::memory_order_acq_rel);

  uint32_t mailbox_index;
  GuestOutputProperties properties;
  GuestOutputPaintConfig paint_config;
  std::unique_lock<std::mutex> consumer_lock =
      ConsumeGuestOutput(mailbox_index, &properties, &paint_config);
  if (!resized && !paint_requested && readbacks.empty() &&
      mailbox_index == last_painted_mailbox_index_) {
    // Nothing changed: the canvas keeps showing the last frame.
    return;
  }

  WebGPUPresentPass::Frame frame;
  uint32_t background = background_color_rgb_.load(std::memory_order_relaxed);
  frame.clear_color = {double(background & 0xFF) / 255.0, double((background >> 8) & 0xFF) / 255.0,
                       double((background >> 16) & 0xFF) / 255.0, 1.0};
  if (mailbox_index != UINT32_MAX && properties.IsActive() &&
      guest_output_images_[mailbox_index].view) {
    // Only bilinear stretching for now (no CAS/FSR on WebGPU), so the flow has
    // a single effect writing the final letterboxed rectangle.
    GuestOutputPaintConfig bilinear_config = paint_config;
    bilinear_config.SetEffect(GuestOutputPaintConfig::Effect::kBilinear);
    uint32_t max_dimension = device_.limits().maxTextureDimension2D;
    GuestOutputPaintFlow flow = GetGuestOutputPaintFlow(
        properties, canvas_width, canvas_height, max_dimension, max_dimension, bilinear_config);
    if (flow.effect_count) {
      const std::pair<uint32_t, uint32_t>& output_size =
          flow.effect_output_sizes[flow.effect_count - 1];
      frame.source = guest_output_images_[mailbox_index].view;
      frame.dest_x = flow.output_x;
      frame.dest_y = flow.output_y;
      frame.dest_width = output_size.first;
      frame.dest_height = output_size.second;
    }
  }

  std::shared_ptr<const UIRecordingList> ui_recordings;
  {
    std::lock_guard<std::mutex> lock(ui_mutex_);
    ui_recordings = ui_recordings_;
  }
  uint32_t ui_draws = 0;
  if (ui_recordings) {
    for (const auto& recording : *ui_recordings) {
      ui_draws += uint32_t(recording->draw_count());
    }
    frame.overlay = [&ui_recordings](WGPUCommandEncoder encoder, WGPUTextureView target,
                                     WGPUTextureFormat format, uint32_t width, uint32_t height) {
      for (const auto& recording : *ui_recordings) {
        recording->Encode(encoder, target, format, width, height);
      }
    };
  }

  std::unique_ptr<FrameReadback> readback;
  if (!readbacks.empty()) {
    readback = std::make_unique<FrameReadback>();
    readback->width = canvas_width;
    readback->height = canvas_height;
    readback->row_pitch = WebGPUPresentPass::ReadbackRowPitch(canvas_width);
    readback->size = uint64_t(readback->row_pitch) * canvas_height;
    readback->format = device_.surface_format();
    WGPUBufferDescriptor buffer_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    buffer_desc.label = MakeStringView("rex present readback");
    buffer_desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    buffer_desc.size = readback->size;
    readback->buffer = wgpuDeviceCreateBuffer(device_.device(), &buffer_desc);
    frame.readback_buffer = readback->buffer;
  }

  uint32_t presented_width, presented_height;
  WebGPUPresentPass::Result result =
      present_pass_->PresentToSurface(frame, presented_width, presented_height);
  // Later writes to the guest output image are ordered after this paint by the
  // queue, so the slot may be released for the producer now.
  consumer_lock.unlock();

  if (result != WebGPUPresentPass::Result::kPresented) {
    // Retry next frame (kSkipped: zero-sized / outdated surface; device loss is
    // handled at the next animation frame).
    last_painted_width_ = 0;
    last_painted_height_ = 0;
    paint_requested_.store(true, std::memory_order_release);
    if (readback) {
      if (readback->buffer) {
        wgpuBufferRelease(readback->buffer);
      }
      std::lock_guard<std::mutex> lock(stats_mutex_);
      pending_readbacks_.insert(pending_readbacks_.begin(), readbacks.begin(), readbacks.end());
    }
    return;
  }

  last_painted_width_ = presented_width;
  last_painted_height_ = presented_height;
  last_painted_mailbox_index_ = mailbox_index;
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.presented_frames;
    if (ui_recordings) {
      ++stats_.ui_overlay_frames;
      stats_.ui_recordings = uint32_t(ui_recordings->size());
    } else {
      stats_.ui_recordings = 0;
    }
    stats_.ui_draws = ui_draws;
    stats_.width = presented_width;
    stats_.height = presented_height;
    if (frame.source) {
      stats_.guest_output_x = frame.dest_x;
      stats_.guest_output_y = frame.dest_y;
      stats_.guest_output_width = frame.dest_width;
      stats_.guest_output_height = frame.dest_height;
    } else {
      stats_.guest_output_x = 0;
      stats_.guest_output_y = 0;
      stats_.guest_output_width = 0;
      stats_.guest_output_height = 0;
    }
    if (resize_pending_presentation_frame_) {
      stats_.last_resize_presented_frame = stats_.animation_frames;
      resize_pending_presentation_frame_ = 0;
    }
  }

  if (readback) {
    if (!readback->buffer || presented_width != readback->width ||
        presented_height != readback->height) {
      // The canvas texture changed size between configuration and acquisition;
      // the copy was not valid. Try again with the next frame.
      if (readback->buffer) {
        wgpuBufferRelease(readback->buffer);
      }
      std::lock_guard<std::mutex> lock(stats_mutex_);
      pending_readbacks_.insert(pending_readbacks_.begin(), readbacks.begin(), readbacks.end());
      paint_requested_.store(true, std::memory_order_release);
      return;
    }
    readback->callbacks = std::move(readbacks);
    WGPUBuffer buffer = readback->buffer;
    uint64_t size = readback->size;
    WGPUBufferMapCallbackInfo callback_info = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    callback_info.mode = WGPUCallbackMode_AllowSpontaneous;
    callback_info.callback = OnFrameReadbackMapped;
    callback_info.userdata1 = readback.release();
    wgpuBufferMapAsync(buffer, WGPUMapMode_Read, 0, size_t(size), callback_info);
  }
}

void WebGPUPresenter::ReleaseGuestOutputImage(GuestOutputImage& image) {
  if (image.view) {
    wgpuTextureViewRelease(image.view);
  }
  if (image.texture) {
    wgpuTextureRelease(image.texture);
  }
  image = GuestOutputImage();
}

}  // namespace webgpu
}  // namespace ui
}  // namespace rex
