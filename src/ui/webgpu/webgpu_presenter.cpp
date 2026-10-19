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
#include <iterator>
#include <utility>

#include <emscripten/html5.h>

#include <rex/graphics/webgpu/webgpu_log.h>
#include <rex/ui/surface_web.h>
#include <rex/ui/webgpu/webgpu_immediate_drawer.h>

// WGSL converted from ../shaders/vulkan_spirv at build time (see the headers).
#include "../shaders/webgpu_wgsl/guest_output_bilinear_dither_ps.h"
#include "../shaders/webgpu_wgsl/guest_output_bilinear_ps.h"
#include "../shaders/webgpu_wgsl/guest_output_ffx_cas_resample_dither_ps.h"
#include "../shaders/webgpu_wgsl/guest_output_ffx_cas_resample_ps.h"
#include "../shaders/webgpu_wgsl/guest_output_ffx_cas_sharpen_dither_ps.h"
#include "../shaders/webgpu_wgsl/guest_output_ffx_cas_sharpen_ps.h"
#include "../shaders/webgpu_wgsl/guest_output_ffx_fsr_easu_ps.h"
#include "../shaders/webgpu_wgsl/guest_output_ffx_fsr_rcas_dither_ps.h"
#include "../shaders/webgpu_wgsl/guest_output_ffx_fsr_rcas_ps.h"
#include "../shaders/webgpu_wgsl/guest_output_triangle_strip_rect_vs.h"

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
    ReleaseEffectChain();
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
  // Effect chain state referenced by frame.pre_pass / final_draw.
  GuestOutputPaintConfig effect_config;
  GuestOutputPaintFlow effect_flow = {};
  bool use_effect_chain = false;
  uint32_t effect_count = 0;
  uint32_t effect_mask = 0;
  uint32_t background = background_color_rgb_.load(std::memory_order_relaxed);
  frame.clear_color = {double(background & 0xFF) / 255.0, double((background >> 8) & 0xFF) / 255.0,
                       double((background >> 16) & 0xFF) / 255.0, 1.0};
  if (mailbox_index != UINT32_MAX && properties.IsActive() &&
      guest_output_images_[mailbox_index].view) {
    // --present_effect: bilinear, CAS or FSR (EASU passes + RCAS), as natively.
    // The temporal fsr2 / fsr3 need the FidelityFX runtime: spatial FSR here.
    effect_config = paint_config;
    if (effect_config.GetEffect() == GuestOutputPaintConfig::Effect::kFsr2 ||
        effect_config.GetEffect() == GuestOutputPaintConfig::Effect::kFsr3) {
      effect_config.SetEffect(GuestOutputPaintConfig::Effect::kFsr);
    }
    uint32_t max_dimension = device_.limits().maxTextureDimension2D;
    GuestOutputPaintFlow flow = GetGuestOutputPaintFlow(
        properties, canvas_width, canvas_height, max_dimension, max_dimension, effect_config);
    if (flow.effect_count &&
        (flow.effect_count > 1 || flow.effects[0] != GuestOutputPaintEffect::kBilinear)) {
      // Effect passes (anything but the plain bilinear stretch, which the
      // present pass draws itself).
      bool chain_ready = InitializeEffectChain();
      for (size_t i = 0; chain_ready && i + 1 < flow.effect_count; ++i) {
        EffectChain::Intermediate& intermediate = effect_chain_.intermediates[i];
        const std::pair<uint32_t, uint32_t>& size = flow.effect_output_sizes[i];
        if (intermediate.texture && intermediate.width == size.first &&
            intermediate.height == size.second) {
          continue;
        }
        if (intermediate.view) {
          wgpuTextureViewRelease(intermediate.view);
        }
        if (intermediate.texture) {
          wgpuTextureRelease(intermediate.texture);
        }
        intermediate = EffectChain::Intermediate();
        WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
        desc.label = MakeStringView("rex guest output effect intermediate");
        desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
        desc.dimension = WGPUTextureDimension_2D;
        desc.size = {size.first, size.second, 1};
        desc.format = graphics::webgpu::kGuestOutputFormat;
        desc.mipLevelCount = 1;
        desc.sampleCount = 1;
        intermediate.texture = wgpuDeviceCreateTexture(device_.device(), &desc);
        if (intermediate.texture) {
          intermediate.view = wgpuTextureCreateView(intermediate.texture, nullptr);
        }
        if (!intermediate.view) {
          REXWGPU_ERROR("WebGPUPresenter: failed to create a {}x{} effect intermediate texture",
                        size.first, size.second);
          chain_ready = false;
          break;
        }
        intermediate.width = size.first;
        intermediate.height = size.second;
      }
      if (chain_ready) {
        effect_flow = flow;
        use_effect_chain = true;
      } else {
        // Fall back to the bilinear stretch.
        GuestOutputPaintConfig bilinear_config = effect_config;
        bilinear_config.SetEffect(GuestOutputPaintConfig::Effect::kBilinear);
        bilinear_config.SetDither(false);
        flow = GetGuestOutputPaintFlow(properties, canvas_width, canvas_height, max_dimension,
                                       max_dimension, bilinear_config);
      }
    }
    if (flow.effect_count) {
      const std::pair<uint32_t, uint32_t>& output_size =
          flow.effect_output_sizes[flow.effect_count - 1];
      frame.source = guest_output_images_[mailbox_index].view;
      frame.dest_x = flow.output_x;
      frame.dest_y = flow.output_y;
      frame.dest_width = output_size.first;
      frame.dest_height = output_size.second;
      effect_count = uint32_t(flow.effect_count);
      for (size_t i = 0; i < flow.effect_count; ++i) {
        effect_mask |= uint32_t(1) << uint32_t(flow.effects[i]);
      }
    }
  }
  if (use_effect_chain) {
    WGPUTextureView guest_output_view = guest_output_images_[mailbox_index].view;
    frame.pre_pass = [this, &effect_flow, &effect_config,
                      guest_output_view](WGPUCommandEncoder encoder) {
      for (size_t i = 0; i + 1 < effect_flow.effect_count; ++i) {
        const EffectChain::Intermediate& target = effect_chain_.intermediates[i];
        WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        color.view = target.view;
        color.loadOp = WGPULoadOp_Clear;
        color.storeOp = WGPUStoreOp_Store;
        color.clearValue = {0.0, 0.0, 0.0, 1.0};
        WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        pass_desc.label = MakeStringView("rex guest output effect");
        pass_desc.colorAttachmentCount = 1;
        pass_desc.colorAttachments = &color;
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
        EncodeEffect(pass, effect_flow, i, effect_config,
                     i ? effect_chain_.intermediates[i - 1].view : guest_output_view,
                     graphics::webgpu::kGuestOutputFormat, target.width, target.height);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
      }
    };
    frame.final_draw = [this, &effect_flow, &effect_config, guest_output_view](
                           WGPURenderPassEncoder pass, WGPUTextureFormat target_format,
                           uint32_t target_width, uint32_t target_height) {
      size_t i = effect_flow.effect_count - 1;
      EncodeEffect(pass, effect_flow, i, effect_config,
                   i ? effect_chain_.intermediates[i - 1].view : guest_output_view, target_format,
                   target_width, target_height);
    };
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
    stats_.guest_output_effect_count = effect_count;
    stats_.guest_output_effect_mask = effect_mask;
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

bool WebGPUPresenter::InitializeEffectChain() {
  EffectChain& chain = effect_chain_;
  if (chain.initialized) {
    return !chain.failed;
  }
  chain.initialized = true;
  chain.failed = true;
  WGPUDevice device = device_.device();

  WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
  wgsl.code = MakeStringView(guest_output_triangle_strip_rect_vs_wgsl);
  WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
  module_desc.nextInChain = &wgsl.chain;
  module_desc.label = MakeStringView("rex guest output rectangle");
  chain.vertex_module = wgpuDeviceCreateShaderModule(device, &module_desc);

  WGPUBindGroupLayoutEntry source_entries[2] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
                                                WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
  source_entries[0].binding = 0;
  source_entries[0].visibility = WGPUShaderStage_Fragment;
  source_entries[0].texture.sampleType = WGPUTextureSampleType_Float;
  source_entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
  source_entries[1].binding = 1;
  source_entries[1].visibility = WGPUShaderStage_Fragment;
  source_entries[1].sampler.type = WGPUSamplerBindingType_Filtering;
  WGPUBindGroupLayoutDescriptor source_layout_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
  source_layout_desc.label = MakeStringView("rex guest output effect source");
  source_layout_desc.entryCount = 2;
  source_layout_desc.entries = source_entries;
  chain.source_layout = wgpuDeviceCreateBindGroupLayout(device, &source_layout_desc);

  WGPUBindGroupLayoutEntry constants_entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
  constants_entry.binding = 0;
  constants_entry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
  constants_entry.buffer.type = WGPUBufferBindingType_Uniform;
  WGPUBindGroupLayoutDescriptor constants_layout_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
  constants_layout_desc.label = MakeStringView("rex guest output effect constants");
  constants_layout_desc.entryCount = 1;
  constants_layout_desc.entries = &constants_entry;
  chain.constants_layout = wgpuDeviceCreateBindGroupLayout(device, &constants_layout_desc);

  if (chain.source_layout && chain.constants_layout) {
    WGPUBindGroupLayout layouts[2] = {chain.source_layout, chain.constants_layout};
    WGPUPipelineLayoutDescriptor layout_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    layout_desc.label = MakeStringView("rex guest output effect");
    layout_desc.bindGroupLayoutCount = 2;
    layout_desc.bindGroupLayouts = layouts;
    chain.pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &layout_desc);
  }

  // As the native presenters' guest output sampler: bilinear, clamped.
  WGPUSamplerDescriptor sampler_desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
  sampler_desc.label = MakeStringView("rex guest output effect");
  sampler_desc.addressModeU = WGPUAddressMode_ClampToEdge;
  sampler_desc.addressModeV = WGPUAddressMode_ClampToEdge;
  sampler_desc.addressModeW = WGPUAddressMode_ClampToEdge;
  sampler_desc.magFilter = WGPUFilterMode_Linear;
  sampler_desc.minFilter = WGPUFilterMode_Linear;
  sampler_desc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
  chain.sampler = wgpuDeviceCreateSampler(device, &sampler_desc);

  WGPUBufferDescriptor buffer_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
  buffer_desc.label = MakeStringView("rex guest output effect constants");
  buffer_desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
  buffer_desc.size = uint64_t(EffectChain::kUniformSlotSize) * EffectChain::kUniformSlotCount;
  chain.uniform_buffer = wgpuDeviceCreateBuffer(device, &buffer_desc);

  if (!chain.vertex_module || !chain.pipeline_layout || !chain.sampler || !chain.uniform_buffer) {
    REXWGPU_ERROR(
        "WebGPUPresenter: guest output effect initialization failed; using bilinear stretching");
    return false;
  }
  chain.failed = false;
  REXWGPU_INFO("WebGPUPresenter: guest output effects (CAS / FSR) initialized");
  return true;
}

void WebGPUPresenter::ReleaseEffectChain() {
  EffectChain& chain = effect_chain_;
  for (EffectChain::Pipeline& pipeline : chain.pipelines) {
    wgpuRenderPipelineRelease(pipeline.pipeline);
  }
  chain.pipelines.clear();
  for (EffectChain::Intermediate& intermediate : chain.intermediates) {
    if (intermediate.view) {
      wgpuTextureViewRelease(intermediate.view);
    }
    if (intermediate.texture) {
      wgpuTextureRelease(intermediate.texture);
    }
    intermediate = EffectChain::Intermediate();
  }
  for (WGPUShaderModule& module : chain.pixel_modules) {
    if (module) {
      wgpuShaderModuleRelease(module);
      module = nullptr;
    }
  }
  if (chain.vertex_module) {
    wgpuShaderModuleRelease(chain.vertex_module);
  }
  if (chain.pipeline_layout) {
    wgpuPipelineLayoutRelease(chain.pipeline_layout);
  }
  if (chain.constants_layout) {
    wgpuBindGroupLayoutRelease(chain.constants_layout);
  }
  if (chain.source_layout) {
    wgpuBindGroupLayoutRelease(chain.source_layout);
  }
  if (chain.sampler) {
    wgpuSamplerRelease(chain.sampler);
  }
  if (chain.uniform_buffer) {
    wgpuBufferRelease(chain.uniform_buffer);
  }
  chain = EffectChain();
}

WGPURenderPipeline WebGPUPresenter::GetEffectPipeline(GuestOutputPaintEffect effect,
                                                      WGPUTextureFormat format) {
  EffectChain& chain = effect_chain_;
  for (const EffectChain::Pipeline& pipeline : chain.pipelines) {
    if (pipeline.effect == effect && pipeline.format == format) {
      return pipeline.pipeline;
    }
  }
  // In GuestOutputPaintEffect order.
  static const char* const kPixelShaders[] = {
      guest_output_bilinear_ps_wgsl,
      guest_output_bilinear_dither_ps_wgsl,
      guest_output_ffx_cas_sharpen_ps_wgsl,
      guest_output_ffx_cas_sharpen_dither_ps_wgsl,
      guest_output_ffx_cas_resample_ps_wgsl,
      guest_output_ffx_cas_resample_dither_ps_wgsl,
      guest_output_ffx_fsr_easu_ps_wgsl,
      guest_output_ffx_fsr_rcas_ps_wgsl,
      guest_output_ffx_fsr_rcas_dither_ps_wgsl,
  };
  static_assert(std::size(kPixelShaders) == size_t(GuestOutputPaintEffect::kCount));
  size_t effect_index = size_t(effect);
  if (effect_index >= std::size(kPixelShaders)) {
    return nullptr;
  }
  WGPUShaderModule& pixel_module = chain.pixel_modules[effect_index];
  if (!pixel_module) {
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = MakeStringView(kPixelShaders[effect_index]);
    WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    module_desc.nextInChain = &wgsl.chain;
    module_desc.label = MakeStringView("rex guest output effect");
    pixel_module = wgpuDeviceCreateShaderModule(device_.device(), &module_desc);
    if (!pixel_module) {
      return nullptr;
    }
  }
  WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
  target.format = format;
  target.writeMask = WGPUColorWriteMask_All;
  WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
  fragment.module = pixel_module;
  fragment.entryPoint = MakeStringView("main");
  fragment.targetCount = 1;
  fragment.targets = &target;
  WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
  desc.label = MakeStringView("rex guest output effect");
  desc.layout = chain.pipeline_layout;
  desc.vertex.module = chain.vertex_module;
  desc.vertex.entryPoint = MakeStringView("main");
  desc.primitive.topology = WGPUPrimitiveTopology_TriangleStrip;
  desc.primitive.cullMode = WGPUCullMode_None;
  desc.multisample.count = 1;
  desc.fragment = &fragment;
  WGPURenderPipeline render_pipeline = wgpuDeviceCreateRenderPipeline(device_.device(), &desc);
  if (!render_pipeline) {
    REXWGPU_ERROR("WebGPUPresenter: guest output effect {} pipeline creation failed (format {})",
                  uint32_t(effect), uint32_t(format));
    return nullptr;
  }
  chain.pipelines.push_back({effect, format, render_pipeline});
  return render_pipeline;
}

bool WebGPUPresenter::EncodeEffect(WGPURenderPassEncoder pass, const GuestOutputPaintFlow& flow,
                                   size_t effect_index, const GuestOutputPaintConfig& config,
                                   WGPUTextureView source, WGPUTextureFormat target_format,
                                   uint32_t target_width, uint32_t target_height) {
  EffectChain& chain = effect_chain_;
  GuestOutputPaintEffect effect = flow.effects[effect_index];
  WGPURenderPipeline pipeline = GetEffectPipeline(effect, target_format);
  if (!pipeline || !source || !target_width || !target_height) {
    return false;
  }

  // The Vulkan push constants: the rectangle (vertex shader) at 0, the effect
  // constants (pixel shader) from 16.
  struct Constants {
    float rect[4];
    uint8_t effect[32];
  } constants = {};
  int32_t rect_x, rect_y;
  flow.GetEffectOutputOffset(effect_index, rect_x, rect_y);
  const std::pair<uint32_t, uint32_t>& rect_size = flow.effect_output_sizes[effect_index];
  float x_to_ndc = 2.0f / float(target_width);
  float y_to_ndc = 2.0f / float(target_height);
  // WebGPU NDC Y points up (Vulkan: down), so the strip goes from the top edge
  // downwards. The pixel shaders work from the pixel position, not from
  // interpolants, so only the covered pixels matter.
  constants.rect[0] = -1.0f + float(rect_x) * x_to_ndc;
  constants.rect[1] = 1.0f - float(rect_y) * y_to_ndc;
  constants.rect[2] = float(rect_size.first) * x_to_ndc;
  constants.rect[3] = -float(rect_size.second) * y_to_ndc;
  auto put = [&constants](const auto& effect_constants) {
    static_assert(sizeof(effect_constants) <= sizeof(constants.effect));
    std::memcpy(constants.effect, &effect_constants, sizeof(effect_constants));
  };
  switch (effect) {
    case GuestOutputPaintEffect::kBilinear:
    case GuestOutputPaintEffect::kBilinearDither: {
      BilinearConstants effect_constants;
      effect_constants.Initialize(flow, effect_index);
      put(effect_constants);
    } break;
    case GuestOutputPaintEffect::kCasSharpen:
    case GuestOutputPaintEffect::kCasSharpenDither: {
      CasSharpenConstants effect_constants;
      effect_constants.Initialize(flow, effect_index, config);
      put(effect_constants);
    } break;
    case GuestOutputPaintEffect::kCasResample:
    case GuestOutputPaintEffect::kCasResampleDither: {
      CasResampleConstants effect_constants;
      effect_constants.Initialize(flow, effect_index, config);
      put(effect_constants);
    } break;
    case GuestOutputPaintEffect::kFsrEasu: {
      FsrEasuConstants effect_constants;
      effect_constants.Initialize(flow, effect_index);
      put(effect_constants);
    } break;
    case GuestOutputPaintEffect::kFsrRcas:
    case GuestOutputPaintEffect::kFsrRcasDither: {
      FsrRcasConstants effect_constants;
      effect_constants.Initialize(flow, effect_index, config);
      put(effect_constants);
    } break;
    default:
      return false;
  }
  // Distinct slots for the passes of one submission (at most
  // kMaxGuestOutputPaintEffects); writes for later submissions are ordered
  // after the earlier ones by the queue.
  uint64_t slot_offset = uint64_t(chain.uniform_slot) * EffectChain::kUniformSlotSize;
  chain.uniform_slot = (chain.uniform_slot + 1) % EffectChain::kUniformSlotCount;
  wgpuQueueWriteBuffer(device_.queue(), chain.uniform_buffer, slot_offset, &constants,
                       sizeof(constants));

  WGPUBindGroupEntry source_entries[2] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
  source_entries[0].binding = 0;
  source_entries[0].textureView = source;
  source_entries[1].binding = 1;
  source_entries[1].sampler = chain.sampler;
  WGPUBindGroupDescriptor source_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
  source_desc.layout = chain.source_layout;
  source_desc.entryCount = 2;
  source_desc.entries = source_entries;
  WGPUBindGroup source_group = wgpuDeviceCreateBindGroup(device_.device(), &source_desc);
  WGPUBindGroupEntry constants_entry = WGPU_BIND_GROUP_ENTRY_INIT;
  constants_entry.binding = 0;
  constants_entry.buffer = chain.uniform_buffer;
  constants_entry.offset = slot_offset;
  constants_entry.size = sizeof(constants);
  WGPUBindGroupDescriptor constants_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
  constants_desc.layout = chain.constants_layout;
  constants_desc.entryCount = 1;
  constants_desc.entries = &constants_entry;
  WGPUBindGroup constants_group = wgpuDeviceCreateBindGroup(device_.device(), &constants_desc);
  if (source_group && constants_group) {
    wgpuRenderPassEncoderSetPipeline(pass, pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, source_group, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(pass, 1, constants_group, 0, nullptr);
    wgpuRenderPassEncoderSetViewport(pass, 0.0f, 0.0f, float(target_width), float(target_height),
                                     0.0f, 1.0f);
    wgpuRenderPassEncoderSetScissorRect(pass, 0, 0, target_width, target_height);
    wgpuRenderPassEncoderDraw(pass, 4, 1, 0, 0);
  }
  if (source_group) {
    wgpuBindGroupRelease(source_group);
  }
  if (constants_group) {
    wgpuBindGroupRelease(constants_group);
  }
  return source_group && constants_group;
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
