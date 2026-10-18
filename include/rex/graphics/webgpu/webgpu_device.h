/**
 * @file        rex/graphics/webgpu/webgpu_device.h
 * @brief       WebGPU instance/adapter/device/surface on a dedicated device thread.
 *
 * Why a dedicated thread (docs/port/webgpu.md, "Threading"):
 * - emdawnwebgpu keeps every WebGPU JS object in a per-worker table
 *   (WebGPU.Internals.jsObjects), so a WGPU* handle is only valid on the
 *   pthread (Web Worker) that created it. There is no cross-thread proxying.
 * - WebGPU promises (requestAdapter, requestDevice, mapAsync,
 *   onSubmittedWorkDone, device.lost) only resolve when that worker returns to
 *   its JS event loop. TimedWaitAny needs Asyncify/JSPI, which this build does
 *   not use.
 * - A WebGPU OffscreenCanvas is presented in the "update the rendering" step of
 *   the owning dedicated worker's event loop (wgpuSurfacePresent aborts in
 *   emdawnwebgpu).
 * The GPU command processor and the UI thread are blocking pthreads, so neither
 * can own the device. The device thread instead parks in its event loop
 * (emscripten_exit_with_live_runtime) and runs work proxied to it with
 * emscripten_proxy_* (RunSync / RunSyncWithCompletion / RunAsync).
 *
 * The canvas: under -sPROXY_TO_PTHREAD -sOFFSCREENCANVAS_SUPPORT the
 * OffscreenCanvas for "#canvas" is owned by the main() pthread (the UI thread).
 * Create() must therefore run on the thread that owns the canvas; it is passed
 * on to the device thread through
 * emscripten_pthread_attr_settransferredcanvases at pthread_create.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <pthread.h>

#include <emscripten/proxying.h>
#include <webgpu/webgpu.h>

#include <rex/graphics/webgpu/webgpu_log.h>

namespace rex::graphics::webgpu {

class WebGPUDevice {
 public:
  struct Options {
    // CSS selector of the canvas to transfer to the device thread and present
    // to. "#canvas" means Module.canvas (its DOM id). Empty: headless device
    // (no surface), e.g. for GPU emulation without presentation.
    std::string canvas_selector = "#canvas";
    WGPUPowerPreference power_preference = WGPUPowerPreference_HighPerformance;
    // Stack of the device thread. It only runs WebGPU encoding, so it is small.
    size_t stack_size = 1024 * 1024;
  };

  // Token for RunSyncWithCompletion: Finish() must be called exactly once, on
  // the device thread, possibly from a later WebGPU callback.
  class Completion {
   public:
    void Finish() const;

   private:
    friend class WebGPUDevice;
    explicit Completion(em_proxying_ctx* ctx) : ctx_(ctx) {}
    em_proxying_ctx* ctx_;
  };

  // Starts the device thread (transferring the canvas to it) and blocks until
  // the adapter and the device are ready. Returns nullptr on failure (no
  // navigator.gpu, no adapter, device request rejected, canvas not found or not
  // owned by the calling thread).
  static std::unique_ptr<WebGPUDevice> Create(const Options& options);

  WebGPUDevice(const WebGPUDevice&) = delete;
  WebGPUDevice& operator=(const WebGPUDevice&) = delete;
  ~WebGPUDevice();

  // ---- Cross-thread entry points (callable from any thread). ----

  bool IsDeviceThread() const { return pthread_equal(pthread_self(), thread_) != 0; }
  // Runs fn on the device thread and waits for it to return. Inline when
  // already on the device thread. Returns false if the thread is gone.
  bool RunSync(const std::function<void()>& fn);
  // Runs fn on the device thread and waits until it (or a callback it set up)
  // calls Completion::Finish(). For asynchronous WebGPU operations (mapAsync,
  // onSubmittedWorkDone): the device thread keeps serving its event loop while
  // the caller blocks. Must not be called on the device thread.
  bool RunSyncWithCompletion(const std::function<void(Completion)>& fn);
  // Queues fn on the device thread and returns immediately.
  bool RunAsync(std::function<void()> fn);

  // Immutable after Create().
  const WGPULimits& limits() const { return limits_; }
  bool HasFeature(WGPUFeatureName feature) const;
  const std::vector<WGPUFeatureName>& features() const { return features_; }
  const std::string& adapter_description() const { return adapter_description_; }
  uint32_t vendor_id() const { return vendor_id_; }
  bool has_surface() const { return surface_ != nullptr; }
  WGPUTextureFormat surface_format() const { return surface_format_; }

  bool is_lost() const { return lost_.load(std::memory_order_acquire); }
  // After is_lost(): the WGPUDeviceLostReason and the browser's message (any thread). A
  // device destroyed by rex_debug_lose_gpu_device reports Destroyed and "debug hook".
  uint32_t lost_reason() const { return lost_reason_.load(std::memory_order_acquire); }
  std::string lost_message() const;
  // The device of the running WebGPU graphics system (for page debug hooks), or null.
  static WebGPUDevice* current();
  uint32_t uncaptured_error_count() const {
    return uncaptured_error_count_.load(std::memory_order_relaxed);
  }

  // Last canvas size seen by the device thread (configure/present). Any thread.
  void GetPublishedCanvasSize(uint32_t& width_out, uint32_t& height_out) const;

  // ---- Device-thread-only accessors. ----

  WGPUInstance instance() const { return instance_; }
  WGPUAdapter adapter() const { return adapter_; }
  WGPUDevice device() const { return device_; }
  WGPUQueue queue() const { return queue_; }
  WGPUSurface surface() const { return surface_; }

  // Device thread: queries the current OffscreenCanvas size.
  bool QueryCanvasSize(uint32_t& width_out, uint32_t& height_out) const;
  // Device thread: (re)configures the surface for the current canvas size if it
  // changed (or if force). Returns false if there is no surface or the canvas
  // is zero-sized. Outputs the configured size.
  bool UpdateSurfaceConfiguration(uint32_t& width_out, uint32_t& height_out, bool force = false);
  // Device thread: drops the current configuration (next update reconfigures).
  void InvalidateSurfaceConfiguration() { configured_width_ = configured_height_ = 0; }

 private:
  explicit WebGPUDevice(const Options& options);

  static void* ThreadMain(void* arg);
  static void InitializeOnDeviceThread(em_proxying_ctx* ctx, void* arg);
  static void OnAdapter(WGPURequestAdapterStatus status, WGPUAdapter adapter,
                        WGPUStringView message, void* userdata1, void* userdata2);
  static void OnDevice(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message,
                       void* userdata1, void* userdata2);
  static void OnDeviceLost(WGPUDevice const* device, WGPUDeviceLostReason reason,
                           WGPUStringView message, void* userdata1, void* userdata2);
  static void OnUncapturedError(WGPUDevice const* device, WGPUErrorType type,
                                WGPUStringView message, void* userdata1, void* userdata2);
  void FinishInitialization(em_proxying_ctx* ctx, bool success);
  void ReleaseOnDeviceThread();

  Options options_;
  pthread_t thread_ = {};
  bool thread_created_ = false;
  std::atomic<uint32_t> thread_started_{0};
  em_proxying_queue* queue_proxy_ = nullptr;

  bool init_success_ = false;

  // Device thread objects.
  WGPUInstance instance_ = nullptr;
  WGPUSurface surface_ = nullptr;
  WGPUAdapter adapter_ = nullptr;
  WGPUDevice device_ = nullptr;
  WGPUQueue queue_ = nullptr;
  uint32_t configured_width_ = 0;
  uint32_t configured_height_ = 0;

  // Written during initialization, read-only afterwards.
  WGPULimits limits_ = WGPU_LIMITS_INIT;
  std::vector<WGPUFeatureName> features_;
  std::string adapter_description_;
  uint32_t vendor_id_ = 0;
  WGPUTextureFormat surface_format_ = WGPUTextureFormat_Undefined;

  std::atomic<bool> lost_{false};
  std::atomic<uint32_t> lost_reason_{0};
  mutable std::mutex lost_mutex_;
  std::string lost_message_;
  // Set on the device thread before the debug hook destroys the device: that Destroyed is
  // a loss, not the normal release.
  bool debug_loss_ = false;
  friend void DebugLoseDevice(WebGPUDevice& device);
  std::atomic<uint32_t> uncaptured_error_count_{0};
  std::atomic<uint64_t> published_canvas_size_{0};
};

}  // namespace rex::graphics::webgpu
