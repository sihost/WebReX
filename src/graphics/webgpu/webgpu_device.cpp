/**
 * @file        graphics/webgpu/webgpu_device.cpp
 * @brief       WebGPU device thread: instance, adapter, device, canvas surface.
 *
 * See rex/graphics/webgpu/webgpu_device.h and docs/port/webgpu.md for the
 * threading model.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/webgpu_device.h>

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <utility>

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>

namespace {

// Whether the calling thread owns the OffscreenCanvas named by `selector`
// ("#id"). GL.offscreenCanvases is the per-worker table Emscripten fills when a
// canvas is transferred at pthread_create (libpthread.js receiveOffscreenCanvases).
// Pointers arrive as BigInt under memory64, hence Number().
EM_JS(int, rexwgpu_canvas_owned_by_this_thread, (const char* selector), {
  var name = UTF8ToString(Number(selector));
  if (name.charAt(0) == '#') name = name.slice(1);
  if (typeof GL == 'undefined' || !GL.offscreenCanvases) return 0;
  var info = GL.offscreenCanvases[name];
  return (info && info.offscreenCanvas) ? 1 : 0;
});

// emdawnwebgpu passes setBindGroup dynamic offsets as (HEAPU32, offset / 4, count): a view
// of the whole wasm memory. Firefox's WebIDL bindings reject every ArrayBufferView over 2 GiB
// ("setBindGroup: Argument 3 can't be an ArrayBuffer or an ArrayBufferView larger than 2 GB";
// the wasm64 memory is 8 GiB), so the call throws and kills the device thread. The port's JS
// library is appended after user --js-library files (it would override a replacement), so
// wrap setBindGroup on this worker's encoder prototypes instead: the offsets are copied into
// a small reusable Uint32Array per count (setBindGroup copies them synchronously). Every
// WebGPU call runs on the device thread, so installing here covers all of them. The other
// heap arguments emdawnwebgpu passes are exact-size views (writeBuffer and writeTexture use
// HEAPU8.subarray(data, data + size); mapped ranges are copied with HEAPU8.set/subarray), and
// the SDK's own EM_JS code passes no heap views to WebGPU. Returns the number of prototypes
// wrapped (0 when already done).
EM_JS(int, rexwgpu_install_dynamic_offset_copy, (), {
  var names = ['GPURenderPassEncoder', 'GPUComputePassEncoder', 'GPURenderBundleEncoder'];
  var wrapped = 0;
  var scratch = [];
  for (var k = 0; k < names.length; k++) {
    var cls = globalThis[names[k]];
    if (!cls || !cls.prototype || cls.prototype.__rexDynamicOffsetCopy) continue;
    var original = cls.prototype.setBindGroup;
    if (typeof original != 'function') continue;
    cls.prototype.setBindGroup = (function(original) {
      return function(index, group, data, start, length) {
        if (arguments.length >= 5 && ArrayBuffer.isView(data)) {
          var n = length >>> 0;
          var s = scratch[n] || (scratch[n] = new Uint32Array(n));
          for (var i = 0; i < n; i++) s[i] = data[start + i];
          return original.call(this, index, group, s, 0, n);
        }
        return original.apply(this, arguments);
      };
    })(original);
    Object.defineProperty(cls.prototype, '__rexDynamicOffsetCopy', {value: true});
    // rexwgpu_pass_batch (webgpu_command_stream.cpp) passes exact-size arrays: native call.
    Object.defineProperty(cls.prototype, '__rexSetBindGroupRaw', {value: original});
    wrapped++;
  }
  return wrapped;
});

const char* FeatureName(WGPUFeatureName feature) {
  switch (feature) {
    case WGPUFeatureName_CoreFeaturesAndLimits:
      return "core-features-and-limits";
    case WGPUFeatureName_DepthClipControl:
      return "depth-clip-control";
    case WGPUFeatureName_Depth32FloatStencil8:
      return "depth32float-stencil8";
    case WGPUFeatureName_TextureCompressionBC:
      return "texture-compression-bc";
    case WGPUFeatureName_TextureCompressionBCSliced3D:
      return "texture-compression-bc-sliced-3d";
    case WGPUFeatureName_TimestampQuery:
      return "timestamp-query";
    case WGPUFeatureName_IndirectFirstInstance:
      return "indirect-first-instance";
    case WGPUFeatureName_ShaderF16:
      return "shader-f16";
    case WGPUFeatureName_RG11B10UfloatRenderable:
      return "rg11b10ufloat-renderable";
    case WGPUFeatureName_Float32Filterable:
      return "float32-filterable";
    case WGPUFeatureName_Float32Blendable:
      return "float32-blendable";
    case WGPUFeatureName_ClipDistances:
      return "clip-distances";
    case WGPUFeatureName_DualSourceBlending:
      return "dual-source-blending";
    case WGPUFeatureName_Subgroups:
      return "subgroups";
    case WGPUFeatureName_TextureFormatsTier1:
      return "texture-formats-tier1";
    case WGPUFeatureName_TextureFormatsTier2:
      return "texture-formats-tier2";
    case WGPUFeatureName_PrimitiveIndex:
      return "primitive-index";
    case WGPUFeatureName_TextureComponentSwizzle:
      return "texture-component-swizzle";
    default:
      return "other";
  }
}

// Optional features the Xenos emulation can use (docs/port/webgpu.md, "Features
// and limits"). All are requested when the adapter has them; the backend keeps a
// fallback for every one.
constexpr WGPUFeatureName kOptionalFeatures[] = {
    WGPUFeatureName_TextureCompressionBC,     WGPUFeatureName_TextureCompressionBCSliced3D,
    WGPUFeatureName_TextureFormatsTier1,      WGPUFeatureName_TextureFormatsTier2,
    WGPUFeatureName_TextureComponentSwizzle,  WGPUFeatureName_ClipDistances,
    WGPUFeatureName_Float32Filterable,        WGPUFeatureName_Float32Blendable,
    WGPUFeatureName_Depth32FloatStencil8,     WGPUFeatureName_DepthClipControl,
    WGPUFeatureName_ShaderF16,                WGPUFeatureName_RG11B10UfloatRenderable,
    WGPUFeatureName_IndirectFirstInstance,    WGPUFeatureName_DualSourceBlending,
    WGPUFeatureName_PrimitiveIndex,
};

const char* BackendName(WGPUBackendType type) {
  switch (type) {
    case WGPUBackendType_WebGPU:
      return "WebGPU";
    case WGPUBackendType_D3D11:
      return "D3D11";
    case WGPUBackendType_D3D12:
      return "D3D12";
    case WGPUBackendType_Metal:
      return "Metal";
    case WGPUBackendType_Vulkan:
      return "Vulkan";
    case WGPUBackendType_OpenGL:
      return "OpenGL";
    case WGPUBackendType_OpenGLES:
      return "OpenGLES";
    default:
      return "unknown";
  }
}

// Limits the browser does not know come back as 0 from fillLimitStruct; asking
// for 0 of an unknown limit would make requestDevice reject, so leave those
// undefined. (Alignment limits are never 0 when known.)
void SanitizeRequiredLimits(WGPULimits& limits) {
#define REXWGPU_U32(name)                    \
  if (limits.name == 0) {                    \
    limits.name = WGPU_LIMIT_U32_UNDEFINED;  \
  }
#define REXWGPU_U64(name)                    \
  if (limits.name == 0) {                    \
    limits.name = WGPU_LIMIT_U64_UNDEFINED;  \
  }
  REXWGPU_U32(maxTextureDimension1D)
  REXWGPU_U32(maxTextureDimension2D)
  REXWGPU_U32(maxTextureDimension3D)
  REXWGPU_U32(maxTextureArrayLayers)
  REXWGPU_U32(maxBindGroups)
  REXWGPU_U32(maxBindGroupsPlusVertexBuffers)
  REXWGPU_U32(maxBindingsPerBindGroup)
  REXWGPU_U32(maxDynamicUniformBuffersPerPipelineLayout)
  REXWGPU_U32(maxDynamicStorageBuffersPerPipelineLayout)
  REXWGPU_U32(maxSampledTexturesPerShaderStage)
  REXWGPU_U32(maxSamplersPerShaderStage)
  REXWGPU_U32(maxStorageBuffersPerShaderStage)
  REXWGPU_U32(maxStorageTexturesPerShaderStage)
  REXWGPU_U32(maxUniformBuffersPerShaderStage)
  REXWGPU_U64(maxUniformBufferBindingSize)
  REXWGPU_U64(maxStorageBufferBindingSize)
  REXWGPU_U32(minUniformBufferOffsetAlignment)
  REXWGPU_U32(minStorageBufferOffsetAlignment)
  REXWGPU_U32(maxVertexBuffers)
  REXWGPU_U64(maxBufferSize)
  REXWGPU_U32(maxVertexAttributes)
  REXWGPU_U32(maxVertexBufferArrayStride)
  REXWGPU_U32(maxInterStageShaderVariables)
  REXWGPU_U32(maxColorAttachments)
  REXWGPU_U32(maxColorAttachmentBytesPerSample)
  REXWGPU_U32(maxComputeWorkgroupStorageSize)
  REXWGPU_U32(maxComputeInvocationsPerWorkgroup)
  REXWGPU_U32(maxComputeWorkgroupSizeX)
  REXWGPU_U32(maxComputeWorkgroupSizeY)
  REXWGPU_U32(maxComputeWorkgroupSizeZ)
  REXWGPU_U32(maxComputeWorkgroupsPerDimension)
  REXWGPU_U32(maxImmediateSize)
#undef REXWGPU_U32
#undef REXWGPU_U64
}

void RunFunction(void* arg) {
  (*static_cast<const std::function<void()>*>(arg))();
}

void RunAndDeleteFunction(void* arg) {
  auto* fn = static_cast<std::function<void()>*>(arg);
  (*fn)();
  delete fn;
}

struct CompletionTask {
  const std::function<void(rex::graphics::webgpu::WebGPUDevice::Completion)>* fn;
};

std::atomic<rex::graphics::webgpu::WebGPUDevice*> g_current_device{nullptr};

}  // namespace

namespace rex::graphics::webgpu {

void WebGPUDevice::Completion::Finish() const {
  emscripten_proxy_finish(ctx_);
}

WebGPUDevice::WebGPUDevice(const Options& options) : options_(options) {}

std::unique_ptr<WebGPUDevice> WebGPUDevice::Create(const Options& options) {
  std::unique_ptr<WebGPUDevice> device(new WebGPUDevice(options));
  device->queue_proxy_ = em_proxying_queue_create();
  if (!device->queue_proxy_) {
    REXWGPU_ERROR("WebGPU: em_proxying_queue_create failed");
    return nullptr;
  }

  auto create_thread = [&](bool with_canvas) -> int {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, device->options_.stack_size);
    if (with_canvas) {
      // Read synchronously by pthread_create; the string outlives the call.
      emscripten_pthread_attr_settransferredcanvases(&attr,
                                                     device->options_.canvas_selector.c_str());
    }
    int result = pthread_create(&device->thread_, &attr, ThreadMain, device.get());
    pthread_attr_destroy(&attr);
    return result;
  };

  bool with_canvas = !device->options_.canvas_selector.empty();
  int create_result = create_thread(with_canvas);
  if (create_result != 0 && with_canvas) {
    REXWGPU_ERROR(
        "WebGPU: could not start the device thread with canvas {} transferred (error {}). "
        "Create() must run on the thread owning the canvas (the main() pthread under "
        "PROXY_TO_PTHREAD) and Module.canvas must be set. Continuing without presentation.",
        device->options_.canvas_selector, create_result);
    device->options_.canvas_selector.clear();
    create_result = create_thread(false);
  }
  if (create_result != 0) {
    REXWGPU_ERROR("WebGPU: pthread_create for the device thread failed ({})", create_result);
    return nullptr;
  }
  device->thread_created_ = true;
  emscripten_set_thread_name(device->thread_, "WebGPU device");

  // Wait for the worker to be running (the canvas arrives with its first message).
  while (!device->thread_started_.load(std::memory_order_acquire)) {
    device->thread_started_.wait(0, std::memory_order_acquire);
  }

  if (!emscripten_proxy_sync_with_ctx(device->queue_proxy_, device->thread_,
                                      InitializeOnDeviceThread, device.get()) ||
      !device->init_success_) {
    REXWGPU_ERROR("WebGPU: device initialization failed");
    return nullptr;
  }
  g_current_device.store(device.get(), std::memory_order_release);
  return device;
}

WebGPUDevice* WebGPUDevice::current() {
  return g_current_device.load(std::memory_order_acquire);
}

std::string WebGPUDevice::lost_message() const {
  std::lock_guard<std::mutex> lock(lost_mutex_);
  return lost_message_;
}

// Device thread.
void DebugLoseDevice(WebGPUDevice& device) {
  if (!device.device_ || device.is_lost()) {
    return;
  }
  REXWGPU_WARN("WebGPU: rex_debug_lose_gpu_device: destroying the device to test device loss");
  device.debug_loss_ = true;
  wgpuDeviceDestroy(device.device_);
}

WebGPUDevice::~WebGPUDevice() {
  WebGPUDevice* self = this;
  g_current_device.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
  if (thread_created_) {
    RunSync([this]() { ReleaseOnDeviceThread(); });
    // The device thread lives in its event loop (exit_with_live_runtime); end it
    // explicitly from inside so pthread_join returns.
    emscripten_proxy_async(
        queue_proxy_, thread_, [](void*) { pthread_exit(nullptr); }, nullptr);
    pthread_join(thread_, nullptr);
    thread_created_ = false;
  }
  if (queue_proxy_) {
    em_proxying_queue_destroy(queue_proxy_);
    queue_proxy_ = nullptr;
  }
}

void* WebGPUDevice::ThreadMain(void* arg) {
  auto* self = static_cast<WebGPUDevice*>(arg);
  self->thread_started_.store(1, std::memory_order_release);
  self->thread_started_.notify_all();
  // Return to the worker's JS event loop and stay alive: every WebGPU call is
  // proxied here, WebGPU promises resolve here, and the canvas is presented in
  // this worker's "update the rendering" step after each task.
  emscripten_exit_with_live_runtime();
}

bool WebGPUDevice::RunSync(const std::function<void()>& fn) {
  if (IsDeviceThread()) {
    fn();
    return true;
  }
  if (!thread_created_) {
    return false;
  }
  return emscripten_proxy_sync(queue_proxy_, thread_, RunFunction,
                               const_cast<std::function<void()>*>(&fn));
}

bool WebGPUDevice::RunSyncWithCompletion(const std::function<void(Completion)>& fn) {
  if (IsDeviceThread()) {
    REXWGPU_ERROR("WebGPU: RunSyncWithCompletion called on the device thread (would deadlock)");
    return false;
  }
  if (!thread_created_) {
    return false;
  }
  CompletionTask task{&fn};
  return emscripten_proxy_sync_with_ctx(
      queue_proxy_, thread_,
      [](em_proxying_ctx* ctx, void* arg) {
        (*static_cast<CompletionTask*>(arg)->fn)(Completion(ctx));
      },
      &task);
}

bool WebGPUDevice::RunAsync(std::function<void()> fn) {
  if (!thread_created_) {
    return false;
  }
  auto* heap_fn = new std::function<void()>(std::move(fn));
  if (!emscripten_proxy_async(queue_proxy_, thread_, RunAndDeleteFunction, heap_fn)) {
    delete heap_fn;
    return false;
  }
  return true;
}

bool WebGPUDevice::HasFeature(WGPUFeatureName feature) const {
  return std::find(features_.begin(), features_.end(), feature) != features_.end();
}

void WebGPUDevice::GetPublishedCanvasSize(uint32_t& width_out, uint32_t& height_out) const {
  uint64_t packed = published_canvas_size_.load(std::memory_order_relaxed);
  width_out = uint32_t(packed);
  height_out = uint32_t(packed >> 32);
}

bool WebGPUDevice::QueryCanvasSize(uint32_t& width_out, uint32_t& height_out) const {
  width_out = 0;
  height_out = 0;
  if (!surface_) {
    return false;
  }
  int width = 0, height = 0;
  if (emscripten_get_canvas_element_size(options_.canvas_selector.c_str(), &width, &height) !=
      EMSCRIPTEN_RESULT_SUCCESS) {
    return false;
  }
  if (width <= 0 || height <= 0) {
    return false;
  }
  width_out = uint32_t(width);
  height_out = uint32_t(height);
  return true;
}

bool WebGPUDevice::UpdateSurfaceConfiguration(uint32_t& width_out, uint32_t& height_out,
                                              bool force) {
  width_out = 0;
  height_out = 0;
  uint32_t width, height;
  if (!QueryCanvasSize(width, height)) {
    return false;
  }
  if (force || width != configured_width_ || height != configured_height_) {
    WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
    config.device = device_;
    config.format = surface_format_;
    // CopySrc: guest output capture / test readback of the presented image.
    config.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    config.width = width;
    config.height = height;
    config.alphaMode = WGPUCompositeAlphaMode_Opaque;
    config.presentMode = WGPUPresentMode_Fifo;
    wgpuSurfaceConfigure(surface_, &config);
    configured_width_ = width;
    configured_height_ = height;
    published_canvas_size_.store(uint64_t(width) | (uint64_t(height) << 32),
                                 std::memory_order_relaxed);
    REXWGPU_INFO("WebGPU: canvas surface configured {}x{}", width, height);
  }
  width_out = configured_width_;
  height_out = configured_height_;
  return true;
}

void WebGPUDevice::InitializeOnDeviceThread(em_proxying_ctx* ctx, void* arg) {
  auto* self = static_cast<WebGPUDevice*>(arg);

  int wrapped = rexwgpu_install_dynamic_offset_copy();
  REXWGPU_INFO("WebGPU: setBindGroup dynamic offsets copied out of the wasm heap ({} encoder "
               "types wrapped)",
               wrapped);

  self->instance_ = wgpuCreateInstance(nullptr);
  if (!self->instance_) {
    REXWGPU_ERROR("WebGPU: wgpuCreateInstance failed (no navigator.gpu in this worker?)");
    self->FinishInitialization(ctx, false);
    return;
  }

  if (!self->options_.canvas_selector.empty()) {
    const char* selector = self->options_.canvas_selector.c_str();
    if (!rexwgpu_canvas_owned_by_this_thread(selector)) {
      REXWGPU_ERROR(
          "WebGPU: canvas {} did not arrive on the device thread (no OffscreenCanvas in "
          "GL.offscreenCanvases; is its DOM id the selector without '#'?). Continuing "
          "without presentation.",
          self->options_.canvas_selector);
      self->options_.canvas_selector.clear();
    } else {
      WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas_source =
          WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
      canvas_source.selector = MakeStringView(self->options_.canvas_selector);
      WGPUSurfaceDescriptor surface_desc = WGPU_SURFACE_DESCRIPTOR_INIT;
      surface_desc.nextInChain = &canvas_source.chain;
      surface_desc.label = MakeStringView("rex canvas");
      self->surface_ = wgpuInstanceCreateSurface(self->instance_, &surface_desc);
      if (!self->surface_) {
        REXWGPU_ERROR("WebGPU: getContext('webgpu') failed for canvas {}",
                      self->options_.canvas_selector);
        self->options_.canvas_selector.clear();
      }
    }
  }

  WGPURequestAdapterOptions adapter_options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
  adapter_options.featureLevel = WGPUFeatureLevel_Core;
  adapter_options.powerPreference = self->options_.power_preference;
  adapter_options.compatibleSurface = self->surface_;
  WGPURequestAdapterCallbackInfo callback_info = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
  // Spontaneous: completes from this worker's event loop when the promise resolves.
  callback_info.mode = WGPUCallbackMode_AllowSpontaneous;
  callback_info.callback = OnAdapter;
  callback_info.userdata1 = self;
  callback_info.userdata2 = ctx;
  wgpuInstanceRequestAdapter(self->instance_, &adapter_options, callback_info);
  // ctx is finished by OnAdapter/OnDevice.
}

void WebGPUDevice::OnAdapter(WGPURequestAdapterStatus status, WGPUAdapter adapter,
                             WGPUStringView message, void* userdata1, void* userdata2) {
  auto* self = static_cast<WebGPUDevice*>(userdata1);
  auto* ctx = static_cast<em_proxying_ctx*>(userdata2);
  if (status != WGPURequestAdapterStatus_Success || !adapter) {
    REXWGPU_ERROR("WebGPU: requestAdapter failed (status {}): {}", uint32_t(status),
                  ToStringView(message));
    self->FinishInitialization(ctx, false);
    return;
  }
  self->adapter_ = adapter;

  WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
  if (wgpuAdapterGetInfo(adapter, &info) == WGPUStatus_Success) {
    self->vendor_id_ = info.vendorID;
    self->adapter_description_ = std::string(ToStringView(info.vendor)) + " / " +
                                 std::string(ToStringView(info.architecture)) + " / " +
                                 std::string(ToStringView(info.device)) + " / " +
                                 std::string(ToStringView(info.description));
    REXWGPU_INFO("WebGPU adapter: {} (backend {}, vendor 0x{:04X}, device 0x{:04X})",
                 self->adapter_description_, BackendName(info.backendType), info.vendorID,
                 info.deviceID);
    wgpuAdapterInfoFreeMembers(info);
  }

  WGPULimits adapter_limits = WGPU_LIMITS_INIT;
  if (wgpuAdapterGetLimits(adapter, &adapter_limits) != WGPUStatus_Success) {
    REXWGPU_ERROR("WebGPU: wgpuAdapterGetLimits failed");
    self->FinishInitialization(ctx, false);
    return;
  }

  // Ask for everything the adapter offers: the Xenos emulation wants the large
  // storage buffer bindings (512 MiB shared memory), more textures/samplers per
  // stage and bigger color attachment budgets than the WebGPU defaults.
  WGPULimits required_limits = adapter_limits;
  required_limits.nextInChain = nullptr;
  SanitizeRequiredLimits(required_limits);

  std::vector<WGPUFeatureName> required_features;
  for (WGPUFeatureName feature : kOptionalFeatures) {
    if (wgpuAdapterHasFeature(adapter, feature)) {
      required_features.push_back(feature);
    }
  }

  WGPUDeviceDescriptor device_desc = WGPU_DEVICE_DESCRIPTOR_INIT;
  device_desc.label = MakeStringView("rex xenos");
  device_desc.requiredFeatureCount = required_features.size();
  device_desc.requiredFeatures = required_features.data();
  device_desc.requiredLimits = &required_limits;
  device_desc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
  device_desc.deviceLostCallbackInfo.callback = OnDeviceLost;
  device_desc.deviceLostCallbackInfo.userdata1 = self;
  device_desc.uncapturedErrorCallbackInfo.callback = OnUncapturedError;
  device_desc.uncapturedErrorCallbackInfo.userdata1 = self;

  WGPURequestDeviceCallbackInfo callback_info = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
  callback_info.mode = WGPUCallbackMode_AllowSpontaneous;
  callback_info.callback = OnDevice;
  callback_info.userdata1 = self;
  callback_info.userdata2 = ctx;
  wgpuAdapterRequestDevice(adapter, &device_desc, callback_info);
}

void WebGPUDevice::OnDevice(WGPURequestDeviceStatus status, WGPUDevice device,
                            WGPUStringView message, void* userdata1, void* userdata2) {
  auto* self = static_cast<WebGPUDevice*>(userdata1);
  auto* ctx = static_cast<em_proxying_ctx*>(userdata2);
  if (status != WGPURequestDeviceStatus_Success || !device) {
    REXWGPU_ERROR("WebGPU: requestDevice failed (status {}): {}", uint32_t(status),
                  ToStringView(message));
    self->FinishInitialization(ctx, false);
    return;
  }
  self->device_ = device;
  self->queue_ = wgpuDeviceGetQueue(device);

  WGPULimits limits = WGPU_LIMITS_INIT;
  if (wgpuDeviceGetLimits(device, &limits) == WGPUStatus_Success) {
    self->limits_ = limits;
    self->limits_.nextInChain = nullptr;
  }
  for (WGPUFeatureName feature : kOptionalFeatures) {
    if (wgpuDeviceHasFeature(device, feature)) {
      self->features_.push_back(feature);
    }
  }
  std::string feature_list;
  for (WGPUFeatureName feature : self->features_) {
    if (!feature_list.empty()) {
      feature_list += ", ";
    }
    feature_list += FeatureName(feature);
  }
  REXWGPU_INFO("WebGPU device features: {}", feature_list.empty() ? "(none)" : feature_list);
  REXWGPU_INFO(
      "WebGPU device limits: maxBufferSize={} maxStorageBufferBindingSize={} "
      "maxUniformBufferBindingSize={} maxTextureDimension2D={} maxTextureArrayLayers={} "
      "maxBindGroups={} maxBindingsPerBindGroup={} maxSampledTexturesPerShaderStage={} "
      "maxSamplersPerShaderStage={} maxStorageBuffersPerShaderStage={} "
      "maxStorageTexturesPerShaderStage={} maxColorAttachments={} "
      "maxColorAttachmentBytesPerSample={} maxInterStageShaderVariables={} "
      "maxComputeWorkgroupStorageSize={} maxComputeInvocationsPerWorkgroup={} "
      "maxImmediateSize={}",
      self->limits_.maxBufferSize, self->limits_.maxStorageBufferBindingSize,
      self->limits_.maxUniformBufferBindingSize, self->limits_.maxTextureDimension2D,
      self->limits_.maxTextureArrayLayers, self->limits_.maxBindGroups,
      self->limits_.maxBindingsPerBindGroup, self->limits_.maxSampledTexturesPerShaderStage,
      self->limits_.maxSamplersPerShaderStage, self->limits_.maxStorageBuffersPerShaderStage,
      self->limits_.maxStorageTexturesPerShaderStage, self->limits_.maxColorAttachments,
      self->limits_.maxColorAttachmentBytesPerSample, self->limits_.maxInterStageShaderVariables,
      self->limits_.maxComputeWorkgroupStorageSize,
      self->limits_.maxComputeInvocationsPerWorkgroup, self->limits_.maxImmediateSize);

  if (self->surface_) {
    WGPUSurfaceCapabilities capabilities = {};
    if (wgpuSurfaceGetCapabilities(self->surface_, self->adapter_, &capabilities) ==
            WGPUStatus_Success &&
        capabilities.formatCount) {
      // emdawnwebgpu lists navigator.gpu.getPreferredCanvasFormat() first.
      self->surface_format_ = capabilities.formats[0];
    } else {
      self->surface_format_ = WGPUTextureFormat_BGRA8Unorm;
    }
    wgpuSurfaceCapabilitiesFreeMembers(capabilities);
    uint32_t width, height;
    if (!self->UpdateSurfaceConfiguration(width, height, true)) {
      REXWGPU_WARN("WebGPU: canvas is zero-sized at startup; configuring on first present");
    }
    REXWGPU_INFO("WebGPU: presenting to canvas {} (format {})", self->options_.canvas_selector,
                 uint32_t(self->surface_format_));
  }
  self->FinishInitialization(ctx, true);
}

void WebGPUDevice::OnDeviceLost(WGPUDevice const* /*device*/, WGPUDeviceLostReason reason,
                                WGPUStringView message, void* userdata1, void* /*userdata2*/) {
  auto* self = static_cast<WebGPUDevice*>(userdata1);
  {
    std::lock_guard<std::mutex> lock(self->lost_mutex_);
    self->lost_message_ = self->debug_loss_ ? std::string("debug hook (rex_debug_lose_gpu_device)")
                                            : std::string(ToStringView(message));
  }
  self->lost_reason_.store(uint32_t(reason), std::memory_order_release);
  self->lost_.store(true, std::memory_order_release);
  if ((reason == WGPUDeviceLostReason_Destroyed ||
       reason == WGPUDeviceLostReason_CallbackCancelled) &&
      !self->debug_loss_) {
    REXWGPU_INFO("WebGPU device released (reason {})", uint32_t(reason));
    return;
  }
  REXWGPU_ERROR("WebGPU device lost (reason {}): {}", uint32_t(reason), ToStringView(message));
}

void WebGPUDevice::OnUncapturedError(WGPUDevice const* /*device*/, WGPUErrorType type,
                                     WGPUStringView message, void* userdata1,
                                     void* /*userdata2*/) {
  auto* self = static_cast<WebGPUDevice*>(userdata1);
  uint32_t count = self->uncaptured_error_count_.fetch_add(1, std::memory_order_relaxed) + 1;
  // Validation errors usually repeat every frame; keep the log readable.
  if (count <= 32 || (count & (count - 1)) == 0) {
    REXWGPU_ERROR("WebGPU uncaptured error #{} (type {}): {}", count, uint32_t(type),
                  ToStringView(message));
  }
}

void WebGPUDevice::FinishInitialization(em_proxying_ctx* ctx, bool success) {
  init_success_ = success;
  emscripten_proxy_finish(ctx);
}

void WebGPUDevice::ReleaseOnDeviceThread() {
  if (surface_) {
    wgpuSurfaceUnconfigure(surface_);
    wgpuSurfaceRelease(surface_);
    surface_ = nullptr;
  }
  if (queue_) {
    wgpuQueueRelease(queue_);
    queue_ = nullptr;
  }
  if (device_) {
    wgpuDeviceDestroy(device_);
    wgpuDeviceRelease(device_);
    device_ = nullptr;
  }
  if (adapter_) {
    wgpuAdapterRelease(adapter_);
    adapter_ = nullptr;
  }
  if (instance_) {
    wgpuInstanceRelease(instance_);
    instance_ = nullptr;
  }
}

}  // namespace rex::graphics::webgpu

// Page debug hook (e.g. Module._rex_debug_lose_gpu_device() from the console or a test):
// destroys the WebGPU device on its thread, which the runtime handles as a device loss
// (WebGPUGraphicsSystem::HandleHostGpuLoss: Module.rexOnGpuLost, files synced, the command
// processor stopped). Any thread; returns at once.
extern "C" EMSCRIPTEN_KEEPALIVE void rex_debug_lose_gpu_device() {
  using rex::graphics::webgpu::WebGPUDevice;
  WebGPUDevice* device = WebGPUDevice::current();
  if (!device) {
    return;
  }
  device->RunAsync([device]() { rex::graphics::webgpu::DebugLoseDevice(*device); });
}
