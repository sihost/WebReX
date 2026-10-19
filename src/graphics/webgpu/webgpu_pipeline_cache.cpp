/**
 * @file        graphics/webgpu/webgpu_pipeline_cache.cpp
 * @brief       WebGPU pipeline cache (WP2): see rex/graphics/webgpu/webgpu_pipeline_cache.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/webgpu_pipeline_cache.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/math.h>
#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/webgpu/webgpu_command_processor.h>
#include <rex/graphics/webgpu/webgpu_device.h>
#include <rex/graphics/webgpu/webgpu_log.h>
#include <rex/graphics/webgpu/webgpu_render_target_cache.h>
#include <rex/graphics/webgpu/webgpu_shader.h>
#include <rex/graphics/webgpu/webgpu_shared_memory.h>
#include <rex/graphics/webgpu/webgpu_tessellator.h>
#include <rex/hash.h>
#include <rex/logging.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten/em_js.h>
#include <rex/filesystem/web_storage.h>
#endif

REXCVAR_DEFINE_BOOL(webgpu_draw_with_pipelines, true, "GPU",
                    "WebGPU: record draws with the translated pipelines (diagnostics: false still "
                    "translates, stores and creates the pipelines, but drops every draw)");

REXCVAR_DECLARE(bool, webgpu_clip_distances);

REXCVAR_DEFINE_BOOL(webgpu_polygon_offset, true, "GPU",
                    "WebGPU: apply the guest polygon offset (PA_SU_POLY_OFFSET_*) as the pipeline "
                    "depth bias, as D3D12 does (false: no depth bias, for A/B comparisons)");
REXCVAR_DEFINE_BOOL(webgpu_depth_clip_control, true, "GPU",
                    "WebGPU: disable depth clipping for draws with PA_CL_CLIP_CNTL.clip_disable "
                    "(unclippedDepth) when the device has depth-clip-control");

REXCVAR_DEFINE_INT32(webgpu_wgsl_threads, 2, "GPU",
                     "WebGPU: threads converting the translated SPIR-V to WGSL (Tint, ~0.1-0.3 s "
                     "per shader in the browser) off the CP thread; draws of a pipeline whose "
                     "shaders are still converting are skipped, as while the browser creates it. "
                     "0 = convert on the CP thread (it stalls; read at the first conversion)");

REXCVAR_DEFINE_BOOL(webgpu_log_skipped_pipelines, false, "GPU",
                    "WebGPU: log every render pipeline id with its shaders and pass, and per id "
                    "the draws the replay skipped while it was not ready (first replay after the "
                    "skips; every 1000 replays the ids never replayed since)");
REXCVAR_DEFINE_STRING(webgpu_wgsl_bundle_url, "", "GPU",
                      "WebGPU (web): URL prefix of a precompiled WGSL bundle (e.g. /wgsl/). At "
                      "the first shader request the CP fetches <prefix><storage dir name>.wgslpack "
                      "once per storage directory and writes the shaders it lacks into the OPFS "
                      "shader storage, so a first boot finds them as a warm one does. Empty = off. "
                      "Bundles: tools/wgsl_bundle in the game repo");

namespace rex::graphics::webgpu {

namespace {

#if defined(__EMSCRIPTEN__)
// Synchronous GET (the CP is a pthread, so sync XHR is allowed) into a JS-side
// slot; returns the byte length, -HTTP status, or -1. Pointers as doubles
// (wasm64, as http_range_device.cpp), no malloc from JS.
EM_JS_DEPS(rex_wgsl_bundle, "$UTF8ToString");
// clang-format off
EM_JS(double, rex_wgsl_bundle_fetch, (double url_ptr), {
  var xhr = new XMLHttpRequest();
  xhr.open("GET", UTF8ToString(url_ptr), false);
  xhr.responseType = "arraybuffer";
  try { xhr.send(null); } catch (e) { return -1; }
  if (xhr.status !== 200 || !xhr.response) return -(xhr.status || 1);
  globalThis["__rexWgslBundle"] = new Uint8Array(xhr.response);
  return globalThis["__rexWgslBundle"].length;
});
EM_JS(void, rex_wgsl_bundle_take, (double dst), {
  var b = globalThis["__rexWgslBundle"];
  if (b) HEAPU8.set(b, dst);
  globalThis["__rexWgslBundle"] = undefined;
});
// clang-format on
#endif

using HVST = Shader::HostVertexShaderType;
using RenderPassFormatKey = WebGPURenderTargetCache::RenderPassFormatKey;

#if REX_WEBGPU_HAS_TINT
constexpr bool kHasTint = true;
#else
constexpr bool kHasTint = false;
// Not linked without libtint_web.a; never called (kHasTint).
bool SpirvToWgslStub(std::span<const uint32_t>, std::string&, std::string& error_out) {
  error_out = "built without libtint_web.a";
  return false;
}
const char* TintRevisionStub() { return "none"; }
#endif

bool ConvertSpirv(const std::vector<uint8_t>& spirv, bool clip_distances, std::string& wgsl,
                  std::string& error) {
  std::span<const uint32_t> words(reinterpret_cast<const uint32_t*>(spirv.data()),
                                  spirv.size() / sizeof(uint32_t));
#if REX_WEBGPU_HAS_TINT
  SpirvToWgslOptions options;
  options.clip_distances = clip_distances;
  return SpirvToWgsl(words, wgsl, error, options);
#else
  (void)clip_distances;
  return SpirvToWgslStub(words, wgsl, error);
#endif
}

const char* GetTintRevision() {
#if REX_WEBGPU_HAS_TINT
  return TintRevision();
#else
  return TintRevisionStub();
#endif
}

WGPUCompareFunction ToCompare(xenos::CompareFunction f) {
  // Same order as the guest enum, after Undefined.
  return WGPUCompareFunction(uint32_t(WGPUCompareFunction_Never) + uint32_t(f));
}

WGPUStencilOperation ToStencilOp(xenos::StencilOp op) {
  switch (op) {
    case xenos::StencilOp::kKeep:
      return WGPUStencilOperation_Keep;
    case xenos::StencilOp::kZero:
      return WGPUStencilOperation_Zero;
    case xenos::StencilOp::kReplace:
      return WGPUStencilOperation_Replace;
    case xenos::StencilOp::kIncrementClamp:
      return WGPUStencilOperation_IncrementClamp;
    case xenos::StencilOp::kDecrementClamp:
      return WGPUStencilOperation_DecrementClamp;
    case xenos::StencilOp::kInvert:
      return WGPUStencilOperation_Invert;
    case xenos::StencilOp::kIncrementWrap:
      return WGPUStencilOperation_IncrementWrap;
    default:
      return WGPUStencilOperation_DecrementWrap;
  }
}

WGPUBlendFactor ToBlendFactor(xenos::BlendFactor f) {
  switch (f) {
    case xenos::BlendFactor::kZero:
      return WGPUBlendFactor_Zero;
    case xenos::BlendFactor::kOne:
      return WGPUBlendFactor_One;
    case xenos::BlendFactor::kSrcColor:
      return WGPUBlendFactor_Src;
    case xenos::BlendFactor::kOneMinusSrcColor:
      return WGPUBlendFactor_OneMinusSrc;
    case xenos::BlendFactor::kSrcAlpha:
      return WGPUBlendFactor_SrcAlpha;
    case xenos::BlendFactor::kOneMinusSrcAlpha:
      return WGPUBlendFactor_OneMinusSrcAlpha;
    case xenos::BlendFactor::kDstColor:
      return WGPUBlendFactor_Dst;
    case xenos::BlendFactor::kOneMinusDstColor:
      return WGPUBlendFactor_OneMinusDst;
    case xenos::BlendFactor::kDstAlpha:
      return WGPUBlendFactor_DstAlpha;
    case xenos::BlendFactor::kOneMinusDstAlpha:
      return WGPUBlendFactor_OneMinusDstAlpha;
    // WebGPU has one blend constant (RGBA); the CP sets it from RB_BLEND_*.
    // Constant alpha as a color factor is approximated by the constant.
    case xenos::BlendFactor::kConstantColor:
    case xenos::BlendFactor::kConstantAlpha:
      return WGPUBlendFactor_Constant;
    case xenos::BlendFactor::kOneMinusConstantColor:
    case xenos::BlendFactor::kOneMinusConstantAlpha:
      return WGPUBlendFactor_OneMinusConstant;
    case xenos::BlendFactor::kSrcAlphaSaturate:
      return WGPUBlendFactor_SrcAlphaSaturated;
    default:
      return WGPUBlendFactor_One;
  }
}

WGPUBlendOperation ToBlendOp(xenos::BlendOp op) {
  switch (op) {
    case xenos::BlendOp::kSubtract:
      return WGPUBlendOperation_Subtract;
    case xenos::BlendOp::kMin:
      return WGPUBlendOperation_Min;
    case xenos::BlendOp::kMax:
      return WGPUBlendOperation_Max;
    case xenos::BlendOp::kRevSubtract:
      return WGPUBlendOperation_ReverseSubtract;
    default:
      return WGPUBlendOperation_Add;
  }
}

WGPUBlendComponent ToBlendComponent(xenos::BlendFactor src, xenos::BlendOp op,
                                    xenos::BlendFactor dst) {
  WGPUBlendComponent c = WGPU_BLEND_COMPONENT_INIT;
  c.operation = ToBlendOp(op);
  if (op == xenos::BlendOp::kMin || op == xenos::BlendOp::kMax) {
    // WebGPU: min / max require One factors (the guest ignores them).
    c.srcFactor = WGPUBlendFactor_One;
    c.dstFactor = WGPUBlendFactor_One;
  } else {
    c.srcFactor = ToBlendFactor(src);
    c.dstFactor = ToBlendFactor(dst);
  }
  return c;
}

WGPUPrimitiveTopology ToTopology(xenos::PrimitiveType type) {
  switch (type) {
    case xenos::PrimitiveType::kPointList:
      return WGPUPrimitiveTopology_PointList;
    case xenos::PrimitiveType::kLineList:
      return WGPUPrimitiveTopology_LineList;
    case xenos::PrimitiveType::kLineStrip:
      return WGPUPrimitiveTopology_LineStrip;
    case xenos::PrimitiveType::kTriangleStrip:
      return WGPUPrimitiveTopology_TriangleStrip;
    default:
      return WGPUPrimitiveTopology_TriangleList;
  }
}

bool IsBlendable(WGPUTextureFormat format) {
  // 32-bit float formats need the float32-blendable feature.
  return format != WGPUTextureFormat_R32Float && format != WGPUTextureFormat_RG32Float &&
         format != WGPUTextureFormat_RGBA32Float;
}

// RB_BLENDCONTROL equivalent to no blending (src One, dst Zero, add).
constexpr uint32_t kBlendControlOff = 0x00010001;

uint32_t NormalizeBlendControl(reg::RB_BLENDCONTROL bc) {
  bc._pad_13 = 0;
  bc._pad_29 = 0;
  return bc.value == kBlendControlOff ? 0 : bc.value;
}

}  // namespace

size_t WebGPUPipelineCache::DescriptionHasher::operator()(const WebGPUPipelineDescription& d) const {
  return size_t(XXH3_64bits(&d, sizeof(d)));
}

bool WebGPUPipelineCache::DescriptionEqual::operator()(const WebGPUPipelineDescription& a,
                                                       const WebGPUPipelineDescription& b) const {
  return std::memcmp(&a, &b, sizeof(a)) == 0;
}

size_t WebGPUPipelineCache::WgslKeyHasher::operator()(const WgslKey& k) const {
  return size_t(k.hash ^ (k.modification * 0x9E3779B97F4A7C15ull));
}

WebGPUPipelineCache::WebGPUPipelineCache(WebGPUCommandProcessor& command_processor,
                                         const RegisterFile& register_file)
    : command_processor_(command_processor), register_file_(register_file) {}

WebGPUPipelineCache::~WebGPUPipelineCache() {
  Shutdown();
}

bool WebGPUPipelineCache::Initialize() {
  WebGPUDevice& device = command_processor_.device();
  // docs/port/webgpu.md section 3 translator features.
  SpirvShaderTranslator::Features features(false);
  uint64_t binding_size = command_processor_.shared_memory()
                              ? command_processor_.shared_memory()->binding_size()
                              : device.limits().maxStorageBufferBindingSize;
  max_storage_buffer_range_ = uint32_t(std::min<uint64_t>(binding_size, UINT32_MAX));
  features.max_storage_buffer_range = max_storage_buffer_range_;
  features.full_draw_index_uint32 = true;
  features.vertex_pipeline_stores_and_atomics = false;
  features.fragment_stores_and_atomics = true;
  clip_distances_ =
      REXCVAR_GET(webgpu_clip_distances) && device.HasFeature(WGPUFeatureName_ClipDistances);
  features.clip_distance = clip_distances_;
  depth_clip_control_ = device.HasFeature(WGPUFeatureName_DepthClipControl);
  features.cull_distance = false;
  features.image_view_format_swizzle = false;
  // Tessellated draws: the guest VS is a vertex shader over the domain points of
  // WebGPUTessellator (attributes 0 and 1), not a tessellation evaluation shader.
  features.tessellation_shader = false;
  translator_ = std::make_unique<SpirvShaderTranslator>(
      features, WebGPURenderTargetCache::IsMsaa2xSupported(),
      WebGPURenderTargetCache::IsMsaa2xSupported(), false,
      command_processor_.draw_resolution_scale_x(), command_processor_.draw_resolution_scale_y());

  // Shared bind group layouts (see the header).
  uint32_t shared_memory_bindings =
      command_processor_.shared_memory() ? command_processor_.shared_memory()->binding_count() : 1;
  device.RunSync([&]() {
    WGPUDevice d = device.device();
    std::vector<WGPUBindGroupLayoutEntry> entries;
    for (uint32_t i = 0; i < shared_memory_bindings; ++i) {
      WGPUBindGroupLayoutEntry& entry = entries.emplace_back();
      entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
      entry.binding = i;
      entry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
      entry.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
    }
    WGPUBindGroupLayoutDescriptor desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    desc.label = MakeStringView("xe shared memory");
    desc.entryCount = entries.size();
    desc.entries = entries.data();
    shared_memory_layout_ = wgpuDeviceCreateBindGroupLayout(d, &desc);
    // The vertex memexport compute shaders write it.
    for (WGPUBindGroupLayoutEntry& entry : entries) {
      entry.visibility = WGPUShaderStage_Compute;
      entry.buffer.type = WGPUBufferBindingType_Storage;
    }
    desc.label = MakeStringView("xe shared memory (memexport compute)");
    compute_shared_memory_layout_ = wgpuDeviceCreateBindGroupLayout(d, &desc);
    entries.clear();
    for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
      WGPUBindGroupLayoutEntry& entry = entries.emplace_back();
      entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
      entry.binding = i;
      entry.visibility =
          WGPUShaderStage_Vertex | WGPUShaderStage_Fragment | WGPUShaderStage_Compute;
      entry.buffer.type = WGPUBufferBindingType_Uniform;
      entry.buffer.hasDynamicOffset = WGPU_TRUE;
    }
    desc.label = MakeStringView("xe constants");
    desc.entryCount = entries.size();
    desc.entries = entries.data();
    constants_layout_ = wgpuDeviceCreateBindGroupLayout(d, &desc);
    desc.label = MakeStringView("xe no textures");
    desc.entryCount = 0;
    desc.entries = nullptr;
    empty_layout_ = wgpuDeviceCreateBindGroupLayout(d, &desc);
  });
  if (!shared_memory_layout_ || !compute_shared_memory_layout_ || !constants_layout_ ||
      !empty_layout_) {
    REXGPU_ERROR("WebGPU pipeline cache: creating the bind group layouts failed");
    return false;
  }
  if (!kHasTint) {
    REXGPU_WARN(
        "WebGPU pipeline cache: built without libtint_web.a, shaders are not converted and "
        "draws are dropped");
  } else {
    REXGPU_INFO("WebGPU pipeline cache: SPIR-V -> WGSL with Tint {}, storage range {} MiB, "
                "clip distances {}",
                GetTintRevision(), max_storage_buffer_range_ >> 20, clip_distances_);
  }
  WebGPUCommandQueue& queue = command_processor_.command_queue();
  queue.set_log_skipped_pipelines(REXCVAR_GET(webgpu_log_skipped_pipelines));
  return true;
}

void WebGPUPipelineCache::Shutdown() {
  StopConversionThreads();
  ClearCache();
  translator_.reset();
  std::vector<WGPUBindGroupLayout> layouts;
  for (auto& layout : texture_layouts_) {
    layouts.push_back(layout.second);
  }
  texture_layouts_.clear();
  for (WGPUBindGroupLayout* layout : {&shared_memory_layout_, &compute_shared_memory_layout_,
                                      &constants_layout_, &empty_layout_}) {
    if (*layout) {
      layouts.push_back(*layout);
      *layout = nullptr;
    }
  }
  if (!layouts.empty()) {
    // Pipelines and bind groups keep their layouts alive on their own.
    command_processor_.device().RunSync([&layouts]() {
      for (WGPUBindGroupLayout layout : layouts) {
        wgpuBindGroupLayoutRelease(layout);
      }
    });
  }
}

uint32_t WebGPUPipelineCache::system_constants_binding_size() {
  return uint32_t(sizeof(SpirvShaderTranslator::SystemConstants));
}

const WebGPUShaderBindings* WebGPUPipelineCache::GetShaderBindings(Shader& shader,
                                                                   uint64_t modification) {
  auto it = shader_bindings_.find(shader.ucode_data_hash());
  if (it != shader_bindings_.end()) {
    return it->second.get();
  }
  // SpirvShader gathers the bindings in the translation of any modification;
  // with WGSL from the storage nothing may have been translated yet.
  AnalyzeShaderUcode(shader);
  Shader::Translation* translation = shader.GetOrCreateTranslation(modification);
  if (!translation->is_translated()) {
    translator_->TranslateAnalyzedShader(*translation);
  }
  bool vertex = shader.type() == xenos::ShaderType::kVertex;
  std::unique_ptr<WebGPUShaderBindings> bindings;
  if (translation->is_valid()) {
    const auto& spirv_shader = static_cast<const SpirvShader&>(shader);
    bindings = std::make_unique<WebGPUShaderBindings>();
    for (const SpirvShader::TextureBinding& t : spirv_shader.GetTextureBindingsAfterTranslation()) {
      bindings->textures.push_back({t.fetch_constant, t.dimension, t.is_signed != 0});
    }
    for (const SpirvShader::SamplerBinding& s : spirv_shader.GetSamplerBindingsAfterTranslation()) {
      bindings->samplers.push_back(
          {s.fetch_constant, s.mag_filter, s.min_filter, s.mip_filter, s.aniso_filter});
    }
    const WGPULimits& limits = command_processor_.device().limits();
    if (bindings->textures.size() > limits.maxSampledTexturesPerShaderStage ||
        bindings->samplers.size() > limits.maxSamplersPerShaderStage) {
      // docs/port/webgpu.md section 9: fail the shader and log it.
      REXGPU_ERROR(
          "WebGPU: {} shader {:016X} uses {} textures and {} samplers, over the device limits "
          "({} / {}); its draws are dropped",
          vertex ? "vertex" : "pixel", shader.ucode_data_hash(), bindings->textures.size(),
          bindings->samplers.size(), limits.maxSampledTexturesPerShaderStage,
          limits.maxSamplersPerShaderStage);
      bindings.reset();
    } else {
      bindings->layout = GetTextureLayout(vertex, *bindings);
      if (!bindings->layout) {
        bindings.reset();
      }
    }
  }
  const WebGPUShaderBindings* result = bindings.get();
  shader_bindings_.emplace(shader.ucode_data_hash(), std::move(bindings));
  return result;
}

WGPUBindGroupLayout WebGPUPipelineCache::GetTextureLayout(bool vertex,
                                                          const WebGPUShaderBindings& bindings) {
  if (bindings.textures.empty() && bindings.samplers.empty()) {
    return empty_layout_;
  }
  std::string key(1, vertex ? 'v' : 'p');
  for (const WebGPUShaderBindings::Texture& texture : bindings.textures) {
    key.push_back(char('0' + uint32_t(texture.dimension)));
  }
  key += fmt::format(":{}", bindings.samplers.size());
  auto it = texture_layouts_.find(key);
  if (it != texture_layouts_.end()) {
    return it->second;
  }
  WGPUBindGroupLayout layout = nullptr;
  WebGPUDevice& device = command_processor_.device();
  device.RunSync([&]() {
    // Vertex shader textures are also bound to the memexport compute variant.
    WGPUShaderStage stage =
        vertex ? (WGPUShaderStage_Vertex | WGPUShaderStage_Compute) : WGPUShaderStage_Fragment;
    std::vector<WGPUBindGroupLayoutEntry> entries;
    uint32_t binding = 0;
    for (const WebGPUShaderBindings::Texture& texture : bindings.textures) {
      WGPUBindGroupLayoutEntry& entry = entries.emplace_back();
      entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
      entry.binding = binding++;
      entry.visibility = stage;
      entry.texture.sampleType = WGPUTextureSampleType_Float;
      switch (texture.dimension) {
        case xenos::FetchOpDimension::k3DOrStacked:
          entry.texture.viewDimension = WGPUTextureViewDimension_3D;
          break;
        case xenos::FetchOpDimension::kCube:
          entry.texture.viewDimension = WGPUTextureViewDimension_Cube;
          break;
        default:
          entry.texture.viewDimension = WGPUTextureViewDimension_2DArray;
          break;
      }
    }
    for (size_t i = 0; i < bindings.samplers.size(); ++i) {
      WGPUBindGroupLayoutEntry& entry = entries.emplace_back();
      entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
      entry.binding = binding++;
      entry.visibility = stage;
      entry.sampler.type = WGPUSamplerBindingType_Filtering;
    }
    std::string label = "xe textures " + key;
    WGPUBindGroupLayoutDescriptor desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    desc.label = MakeStringView(label);
    desc.entryCount = entries.size();
    desc.entries = entries.data();
    layout = wgpuDeviceCreateBindGroupLayout(device.device(), &desc);
  });
  if (!layout) {
    REXGPU_ERROR("WebGPU: creating the texture bind group layout {} failed", key);
    return nullptr;
  }
  texture_layouts_.emplace(std::move(key), layout);
  return layout;
}

void WebGPUPipelineCache::ClearCache() {
  // Draws only hold Shader pointers for the duration of IssueDraw. Pipelines
  // stay in the command queue's table (released with it).
  pipelines_.clear();
  compute_pipelines_.clear();
  {
    // Pending slots stay alive in their jobs and deferred pipelines.
    std::lock_guard<std::mutex> lock(wgsl_mutex_);
    wgsl_.clear();
  }
  depth_only_wgsl_.clear();
  shader_bindings_.clear();
  shaders_.clear();
}

void WebGPUPipelineCache::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                                  uint32_t title_id, bool /*blocking*/) {
  storage_initialized_ = true;
  storage_dir_.clear();
  std::filesystem::path root;
#if defined(__EMSCRIPTEN__)
  // OPFS through WasmFS (synchronous file I/O from this pthread).
  std::string mount = rex::filesystem::web::MountPersistentStorage();
  if (!mount.empty()) {
    root = std::filesystem::path(mount) / "rex-cache";
  }
#endif
  if (root.empty()) {
    root = cache_root;
  }
  if (root.empty()) {
    REXGPU_WARN("WebGPU shader storage: no storage directory, WGSL is not persisted");
    return;
  }
  // The resolution scale changes the translated shaders (pixel position, scaled texture
  // coordinates), so scaled storage gets its own directory ("-s2x2").
  uint32_t scale_x = command_processor_.draw_resolution_scale_x();
  uint32_t scale_y = command_processor_.draw_resolution_scale_y();
  storage_dir_ = root / "shaders" / fmt::format("{:08X}", title_id) / "webgpu" /
                 fmt::format("{}-t{}-r{}{}", GetTintRevision(),
                             SpirvShaderTranslator::Modification::kVersion,
                             SpirvShaderTranslator::GetSharedMemoryStorageBufferCountLog2(
                                 max_storage_buffer_range_) |
                                 (clip_distances_ ? 4 : 0),
                             (scale_x > 1 || scale_y > 1)
                                 ? fmt::format("-s{}x{}", scale_x, scale_y)
                                 : std::string());
  std::error_code ec;
  std::filesystem::create_directories(storage_dir_, ec);
  if (ec) {
    REXGPU_WARN("WebGPU shader storage: cannot create {}: {}", storage_dir_.string(),
                ec.message());
    storage_dir_.clear();
    return;
  }
  REXGPU_INFO("WebGPU shader storage: {}", storage_dir_.string());
  ImportWgslBundle();
}

void WebGPUPipelineCache::ImportWgslBundle() {
#if defined(__EMSCRIPTEN__)
  std::string prefix = REXCVAR_GET(webgpu_wgsl_bundle_url);
  if (prefix.empty() || storage_dir_.empty()) {
    return;
  }
  // Once per storage directory: the marker holds the bundle size imported.
  std::filesystem::path marker = storage_dir_ / "bundle.imported";
  std::error_code ec;
  if (std::filesystem::exists(marker, ec)) {
    REXGPU_INFO("WebGPU shader storage: WGSL bundle already imported ({})", marker.string());
    return;
  }
  std::string url = prefix + storage_dir_.filename().string() + ".wgslpack";
  auto start = std::chrono::steady_clock::now();
  double length = rex_wgsl_bundle_fetch(double(uintptr_t(url.c_str())));
  if (length <= 0) {
    REXGPU_INFO("WebGPU shader storage: no WGSL bundle at {} ({})", url, length);
    return;
  }
  std::string data(size_t(length), '\0');
  rex_wgsl_bundle_take(double(uintptr_t(data.data())));
  double fetch_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  // "REXWGSL1 <storage dir name>\n", then per shader "<file name> <bytes>\n<bytes>\n".
  size_t pos = data.find('\n');
  std::string expected = "REXWGSL1 " + storage_dir_.filename().string();
  if (pos == std::string::npos || data.compare(0, pos, expected)) {
    REXGPU_WARN("WebGPU shader storage: {} is not a bundle for {}", url, expected);
    return;
  }
  ++pos;
  uint32_t written = 0, present = 0;
  uint64_t written_bytes = 0;
  bool valid = true;
  while (pos < data.size()) {
    size_t line_end = data.find('\n', pos);
    size_t space = data.find(' ', pos);
    if (line_end == std::string::npos || space == std::string::npos || space > line_end) {
      valid = false;
      break;
    }
    std::string name = data.substr(pos, space - pos);
    uint64_t size = std::strtoull(data.c_str() + space + 1, nullptr, 10);
    pos = line_end + 1;
    if (size > data.size() - pos || name.size() < 6 ||
        name.compare(name.size() - 5, 5, ".wgsl") || name.find_first_of("/\\:") != std::string::npos ||
        name.find("..") != std::string::npos) {
      valid = false;
      break;
    }
    std::filesystem::path path = storage_dir_ / name;
    if (std::filesystem::exists(path, ec)) {
      ++present;
    } else {
      std::filesystem::path temp = path;
      temp += ".tmp";
      {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(data.data() + pos, std::streamsize(size));
      }
      std::filesystem::rename(temp, path, ec);
      if (!ec) {
        ++written;
        written_bytes += size;
      }
    }
    pos += size_t(size) + 1;
  }
  if (valid) {
    std::ofstream out(marker, std::ios::binary | std::ios::trunc);
    out << data.size() << "\n";
  }
  REXGPU_INFO(
      "WebGPU shader storage: WGSL bundle {} ({} KiB, fetched in {:.0f} ms): {} shaders written "
      "({} KiB), {} already present{}, {:.0f} ms in total",
      url, data.size() >> 10, fetch_ms, written, written_bytes >> 10, present,
      valid ? "" : ", malformed after them",
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
#endif
}

std::filesystem::path WebGPUPipelineCache::StoragePath(uint64_t hash, uint64_t modification,
                                                       bool vertex) const {
  return storage_dir_ /
         fmt::format("{:016X}_{:016X}.{}.wgsl", hash, modification, vertex ? "vs" : "ps");
}

Shader* WebGPUPipelineCache::LoadShader(xenos::ShaderType shader_type,
                                        const uint32_t* host_address, uint32_t dword_count) {
  uint64_t hash = XXH3_64bits(host_address, dword_count * sizeof(uint32_t));
  auto it = shaders_.find(hash);
  if (it != shaders_.end()) {
    return it->second.get();
  }
  auto shader = std::make_unique<SpirvShader>(shader_type, hash, host_address, dword_count);
  Shader* result = shader.get();
  shaders_.emplace(hash, std::move(shader));
  return result;
}

std::shared_ptr<WebGPUPipelineCache::WgslSlot> WebGPUPipelineCache::RequestWgsl(
    Shader& shader, uint64_t modification) {
  WgslKey key{shader.ucode_data_hash(), modification};
  {
    std::lock_guard<std::mutex> lock(wgsl_mutex_);
    auto it = wgsl_.find(key);
    if (it != wgsl_.end()) {
      ++statistics_.wgsl_memory_hits;
      return it->second;
    }
  }
#if defined(__EMSCRIPTEN__)
  if (!storage_initialized_) {
    // GraphicsSystem::InitializeShaderStorage is not called on the web
    // (kStoreShaders); the WGSL storage is on OPFS regardless. Keys are ucode
    // hashes, so one directory for all titles is safe.
    InitializeShaderStorage({}, 0, false);
  }
#endif
  bool vertex = shader.type() == xenos::ShaderType::kVertex;
  auto slot = std::make_shared<WgslSlot>();
  std::filesystem::path path;
  if (!storage_dir_.empty()) {
    path = StoragePath(key.hash, modification, vertex);
    std::ifstream file(path, std::ios::binary);
    if (file) {
      std::ostringstream text;
      text << file.rdbuf();
      if (file.good() || file.eof()) {
        std::string wgsl = std::move(text).str();
        if (!wgsl.empty()) {
          ++statistics_.wgsl_storage_hits;
          slot->text = std::make_shared<const std::string>(std::move(wgsl));
          slot->ready = true;
        }
      }
    }
  }
  std::vector<uint8_t> spirv;
  if (!slot->ready) {
    // The SPIR-V translator is not thread-safe (and fast): CP thread.
    if (kHasTint) {
      AnalyzeShaderUcode(shader);
      Shader::Translation* translation = shader.GetOrCreateTranslation(modification);
      if (!translation->is_translated()) {
        translator_->TranslateAnalyzedShader(*translation);
      }
      if (!translation->is_valid() || translation->translated_binary().empty()) {
        REXGPU_ERROR("WebGPU: {} shader {:016X} modification {:016X} failed SPIR-V translation",
                     vertex ? "vertex" : "pixel", key.hash, modification);
      } else {
        spirv = translation->translated_binary();
      }
    }
    if (spirv.empty()) {
      // Failures are remembered as ready with null text.
      async_counters_->wgsl_failures.fetch_add(1, std::memory_order_relaxed);
      slot->ready = true;
    }
  }
  std::lock_guard<std::mutex> lock(wgsl_mutex_);
  if (!slot->ready && conversion_threads_.empty() && !conversion_stop_) {
    int32_t threads = std::clamp(REXCVAR_GET(webgpu_wgsl_threads), int32_t(0), int32_t(8));
    for (int32_t i = 0; i < threads; ++i) {
      conversion_threads_.emplace_back([this]() { ConversionThreadMain(); });
    }
    REXGPU_INFO("WebGPU pipeline cache: {} WGSL conversion thread(s)", threads);
  }
  if (!slot->ready && conversion_threads_.empty()) {
    // --webgpu_wgsl_threads=0: on the CP thread (nothing else can see the slot).
    slot->text = ConvertAndStore(spirv, path, key.hash, modification, vertex);
    slot->ready = true;
  }
  wgsl_.emplace(key, slot);
  if (!slot->ready) {
    async_counters_->wgsl_converting.fetch_add(1, std::memory_order_relaxed);
    conversion_jobs_.push_back(
        ConversionJob{slot, std::move(spirv), std::move(path), key.hash, modification, vertex});
    wgsl_jobs_cv_.notify_one();
  }
  return slot;
}

std::shared_ptr<const std::string> WebGPUPipelineCache::GetWgsl(Shader& shader,
                                                                 uint64_t modification) {
  std::shared_ptr<WgslSlot> slot = RequestWgsl(shader, modification);
  std::unique_lock<std::mutex> lock(wgsl_mutex_);
  wgsl_ready_cv_.wait(lock, [&]() { return slot->ready || conversion_stop_; });
  return slot->text;
}

std::shared_ptr<const std::string> WebGPUPipelineCache::ConvertAndStore(
    const std::vector<uint8_t>& spirv, const std::filesystem::path& storage_path, uint64_t hash,
    uint64_t modification, bool vertex) {
  std::string wgsl, error;
  if (!ConvertSpirv(spirv, clip_distances_, wgsl, error)) {
    REXGPU_ERROR("WebGPU: {} shader {:016X} modification {:016X}: SPIR-V -> WGSL failed: {}",
                 vertex ? "vertex" : "pixel", hash, modification, error);
    async_counters_->wgsl_failures.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  async_counters_->wgsl_translated.fetch_add(1, std::memory_order_relaxed);
  if (!storage_path.empty()) {
    // Write to a temporary name, then rename: a torn file is never read.
    std::filesystem::path temp = storage_path;
    temp += ".tmp";
    {
      std::ofstream out(temp, std::ios::binary | std::ios::trunc);
      out.write(wgsl.data(), std::streamsize(wgsl.size()));
    }
    std::error_code ec;
    std::filesystem::rename(temp, storage_path, ec);
    if (ec) {
      REXGPU_WARN("WebGPU shader storage: cannot write {}: {}", storage_path.string(),
                  ec.message());
    }
  }
  return std::make_shared<const std::string>(std::move(wgsl));
}

void WebGPUPipelineCache::CompleteSlot(const std::shared_ptr<WgslSlot>& slot,
                                       std::shared_ptr<const std::string> text) {
  std::vector<std::shared_ptr<DeferredPipeline>> complete;
  {
    std::lock_guard<std::mutex> lock(wgsl_mutex_);
    slot->text = std::move(text);
    slot->ready = true;
    for (std::shared_ptr<DeferredPipeline>& pipeline : slot->waiters) {
      if (!--pipeline->pending) {
        complete.push_back(std::move(pipeline));
      }
    }
    slot->waiters.clear();
  }
  async_counters_->wgsl_converting.fetch_sub(1, std::memory_order_relaxed);
  wgsl_ready_cv_.notify_all();
  // Every slot of these is ready: their texts don't change any more.
  for (const std::shared_ptr<DeferredPipeline>& pipeline : complete) {
    std::shared_ptr<const std::string> vs_wgsl = pipeline->vs->text;
    std::shared_ptr<const std::string> ps_wgsl =
        pipeline->ps ? pipeline->ps->text : pipeline->ps_text;
    if (!vs_wgsl || (pipeline->ps && !ps_wgsl)) {
      // Its draws stay skipped (the id never gets a pipeline).
      async_counters_->pipelines_failed.fetch_add(1, std::memory_order_relaxed);
      async_counters_->Settle(pipeline->id);
      REXGPU_ERROR("WebGPU: render pipeline {} ({}) not created: a shader failed to convert",
                   pipeline->id, pipeline->label);
      continue;
    }
    CreateRenderPipelineAsync(pipeline->id, pipeline->description, std::move(vs_wgsl),
                              std::move(ps_wgsl), pipeline->label, pipeline->layouts);
  }
}

void WebGPUPipelineCache::ConversionThreadMain() {
  for (;;) {
    ConversionJob job;
    {
      std::unique_lock<std::mutex> lock(wgsl_mutex_);
      wgsl_jobs_cv_.wait(lock, [this]() { return conversion_stop_ || !conversion_jobs_.empty(); });
      if (conversion_stop_) {
        return;
      }
      job = std::move(conversion_jobs_.front());
      conversion_jobs_.pop_front();
    }
    CompleteSlot(job.slot,
                 ConvertAndStore(job.spirv, job.storage_path, job.hash, job.modification, job.vertex));
  }
}

void WebGPUPipelineCache::StopConversionThreads() {
  {
    std::lock_guard<std::mutex> lock(wgsl_mutex_);
    conversion_stop_ = true;
    conversion_jobs_.clear();
  }
  wgsl_jobs_cv_.notify_all();
  wgsl_ready_cv_.notify_all();
  for (std::thread& thread : conversion_threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  conversion_threads_.clear();
}

std::shared_ptr<const std::string> WebGPUPipelineCache::GetDepthOnlyFragmentWgsl(
    uint32_t depth_stencil_mode) {
  auto it = depth_only_wgsl_.find(depth_stencil_mode);
  if (it != depth_only_wgsl_.end()) {
    return it->second;
  }
  std::shared_ptr<const std::string> result;
  if (kHasTint) {
    std::vector<uint8_t> spirv = translator_->CreateDepthOnlyFragmentShader(
        SpirvShaderTranslator::Modification::DepthStencilMode(depth_stencil_mode));
    std::string wgsl, error;
    if (!spirv.empty() && ConvertSpirv(spirv, clip_distances_, wgsl, error)) {
      result = std::make_shared<const std::string>(std::move(wgsl));
    } else {
      REXGPU_ERROR("WebGPU: depth-only fragment shader (mode {}) failed: {}", depth_stencil_mode,
                   error);
    }
  }
  depth_only_wgsl_.emplace(depth_stencil_mode, result);
  return result;
}

bool WebGPUPipelineCache::ConfigurePipeline(const WebGPUDrawPipelineState& state,
                                            PipelineId& pipeline_out,
                                            WebGPUDrawBindings& bindings_out) {
  pipeline_out = kInvalidPipelineId;
  bindings_out = {};
  if (!kHasTint || !state.vertex_shader || !translator_) {
    return false;
  }
  const RegisterFile& regs = register_file_;
  Shader& vs = *state.vertex_shader;
  AnalyzeShaderUcode(vs);

  // Host vertex shader type and primitive topology.
  HVST host_vs_type = state.host_vertex_shader_type;
  xenos::PrimitiveType host_primitive = state.host_primitive_type;
  if (!state.host_vertex_shader_type_valid) {
    host_vs_type = HVST::kVertex;
    if (state.guest_primitive_type == xenos::PrimitiveType::kPointList) {
      host_vs_type = HVST::kPointListAsTriangleStrip;
    } else if (state.guest_primitive_type == xenos::PrimitiveType::kRectangleList) {
      host_vs_type = HVST::kRectangleListAsTriangleStrip;
    }
  }
  if (host_primitive == xenos::PrimitiveType::kNone) {
    host_primitive = host_vs_type == HVST::kVertex ? state.guest_primitive_type
                                                   : xenos::PrimitiveType::kTriangleStrip;
  }
  bool domain_as_vertex = Shader::IsHostVertexShaderTypeDomain(host_vs_type);
  if (host_vs_type == HVST::kMemExportCompute ||
      (domain_as_vertex && !state.tessellation_available)) {
    // Memexport compute is not a render pipeline; tessellated draws need the
    // WebGPUTessellator pre-pass (the domain points as vertex attributes).
    ++statistics_.draws_unsupported;
    ++statistics_.draws_refused_tessellation;
    return false;
  }
  if (domain_as_vertex) {
    // The tessellator writes a triangle list of domain points.
    host_primitive = xenos::PrimitiveType::kTriangleList;
  }

  WebGPUPipelineDescription desc;
  std::memset(&desc, 0, sizeof(desc));

  // Vertex shader modification (VulkanPipelineCache::GetCurrentVertexShaderModification
  // for a device without cull distances).
  SpirvShaderTranslator::Modification vs_mod(translator_->GetDefaultVertexShaderModification(
      vs.GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg),
      host_vs_type));
  vs_mod.vertex.interpolator_mask = state.interpolator_mask;
  vs_mod.vertex.tessellation_mode = 0;
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  uint32_t user_clip_planes =
      (pa_cl_clip_cntl.clip_disable || !clip_distances_) ? 0 : pa_cl_clip_cntl.ucp_ena;
  vs_mod.vertex.user_clip_plane_cull = 0;
  vs_mod.vertex.user_clip_plane_count = rex::bit_count(user_clip_planes);
  vs_mod.vertex.point_ps_ucp_mode = pa_cl_clip_cntl.ps_ucp_mode;
  bool ps_param_gen_used = state.pixel_shader && state.ps_param_gen_pos < xenos::kMaxInterpolators;
  if (host_vs_type == HVST::kPointListAsTriangleStrip) {
    vs_mod.vertex.output_point_parameters = uint32_t(ps_param_gen_used);
  } else {
    vs_mod.vertex.output_point_parameters =
        uint32_t((vs.writes_point_size_edge_flag_kill_vertex() & 0b001) &&
                 state.guest_primitive_type == xenos::PrimitiveType::kPointList);
  }
  vs_mod.vertex.vertex_kill_and = 0;
  // Centroid sampling is a pixel shader property on Xenos, but WGSL requires
  // the vertex outputs to declare the same interpolation sampling as the
  // fragment inputs (Dawn ValidateInterStageMatching), so the VS variant
  // carries the centroid mask of the PS it is linked with (part of the
  // modification, so of the pipeline and the WGSL storage keys).
  uint32_t interpolators_centroid = 0;
  if (state.pixel_shader) {
    interpolators_centroid =
        state.interpolator_mask & ~xenos::GetInterpolatorSamplingPattern(
                                      regs.Get<reg::RB_SURFACE_INFO>().msaa_samples,
                                      regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
                                      regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);
  }
  vs_mod.vertex.interpolators_centroid = interpolators_centroid;
  desc.vertex_shader_hash = vs.ucode_data_hash();
  desc.vertex_shader_modification = vs_mod.value;
  bindings_out.vertex = GetShaderBindings(vs, vs_mod.value);
  if (!bindings_out.vertex) {
    ++statistics_.draws_unsupported;
    ++statistics_.draws_refused_bindings;
    return false;
  }

  // Pixel shader modification.
  RenderPassFormatKey pass_key = WebGPURenderTargetCache::GetRenderPassFormatKey(state.render_pass_key);
  if (state.pixel_shader) {
    Shader& ps = *state.pixel_shader;
    AnalyzeShaderUcode(ps);
    SpirvShaderTranslator::Modification ps_mod(translator_->GetDefaultPixelShaderModification(
        ps.GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().ps_num_reg)));
    ps_mod.pixel.interpolator_mask = state.interpolator_mask;
    ps_mod.pixel.interpolators_centroid = interpolators_centroid;
    if (ps_param_gen_used) {
      ps_mod.pixel.param_gen_enable = 1;
      ps_mod.pixel.param_gen_interpolator = state.ps_param_gen_pos;
      ps_mod.pixel.param_gen_point =
          uint32_t(state.guest_primitive_type == xenos::PrimitiveType::kPointList);
    }
    using DepthStencilMode = SpirvShaderTranslator::Modification::DepthStencilMode;
    ps_mod.pixel.depth_stencil_mode =
        (ps.implicit_early_z_write_allowed() &&
         (!ps.writes_color_target(0) ||
          !draw_util::DoesCoverageDependOnAlpha(regs.Get<reg::RB_COLORCONTROL>())))
            ? DepthStencilMode::kEarlyHint
            : DepthStencilMode::kNoModifiers;
    desc.pixel_shader_hash = ps.ucode_data_hash();
    desc.pixel_shader_modification = ps_mod.value;
    if (ps.memexport_eM_written()) {
      // The translated PS declares the shared memory read_write, which can't
      // share group 0 with the read-only VS binding (and the shared memory is
      // also the index buffer of the pass). TODO: a separate layout + pass.
      ++statistics_.draws_unsupported;
      ++statistics_.draws_refused_ps_memexport;
      return false;
    }
    bindings_out.pixel = GetShaderBindings(ps, ps_mod.value);
    if (!bindings_out.pixel) {
      ++statistics_.draws_unsupported;
      ++statistics_.draws_refused_bindings;
      return false;
    }
  }

  // Render pass formats and output state.
  desc.render_pass_format_key = pass_key.key;
  desc.sample_count = uint8_t(WebGPURenderTargetCache::GetHostSampleCount(pass_key.msaa_samples));
  bool has_depth = (pass_key.depth_and_color_used & 1) != 0;
  desc.depth_texture_format = has_depth ? uint32_t(state.depth_texture_format) : 0;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (!(pass_key.depth_and_color_used & (1u << (1 + i)))) {
      continue;
    }
    uint32_t mask = state.pixel_shader ? (state.normalized_color_mask >> (4 * i)) & 0xF : 0;
    desc.color_write_masks |= mask << (4 * i);
    WGPUTextureFormat format = WebGPURenderTargetCache::GetColorTextureFormat(pass_key.color_format(i));
    if (mask && IsBlendable(format)) {
      desc.blend_control[i] = NormalizeBlendControl(
          regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[i]));
    }
  }
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  auto sc_mode = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  bool stencil_back_masks = false;
  WGPUPrimitiveTopology topology = ToTopology(host_primitive);
  desc.primitive_topology = uint8_t(topology);
  if (has_depth) {
    reg::RB_DEPTHCONTROL dc = state.normalized_depth_control;
    desc.depth_control = dc.value;
    if (dc.stencil_enable) {
      // One mask pair for both faces: the back face ones only when drawing back
      // faces only (D3D12PipelineCache; the CP picks the reference the same way).
      uint32_t ref_mask_register = XE_GPU_REG_RB_STENCILREFMASK;
      if (primitive_polygonal && dc.backface_enable && sc_mode.cull_front) {
        ref_mask_register = XE_GPU_REG_RB_STENCILREFMASK_BF;
        stencil_back_masks = true;
      }
      auto ref_mask = regs.Get<reg::RB_STENCILREFMASK>(ref_mask_register);
      desc.stencil_read_mask = uint8_t(ref_mask.stencilmask);
      desc.stencil_write_mask = uint8_t(ref_mask.stencilwritemask);
    }
    // Polygon offset (D3D12PipelineCache::GetCurrentStateDescription). WebGPU
    // requires 0 for point and line topologies; guest points / rectangles drawn
    // as host triangle strips use the guest's non-polygonal (para) offset.
    if (REXCVAR_GET(webgpu_polygon_offset) &&
        (topology == WGPUPrimitiveTopology_TriangleList ||
         topology == WGPUPrimitiveTopology_TriangleStrip)) {
      float polygon_offset, polygon_offset_scale;
      draw_util::GetPreferredFacePolygonOffset(regs, primitive_polygonal, polygon_offset_scale,
                                               polygon_offset);
      // Keep the integer conversion in range (32 = 32x the whole depth range).
      polygon_offset = std::clamp(polygon_offset, -32.0f, 32.0f);
      // Units of the host format: float24 in depth32float (exact, the host value
      // is guest * 0.5, which lowers the exponent the float bias is relative to
      // by 1 as well, as on Vulkan), unorm24 for D24S8 and for D24FS8 stored as
      // depth24plus-stencil8 (no depth32float-stencil8).
      xenos::DepthRenderTargetFormat bias_format =
          state.depth_texture_format == WGPUTextureFormat_Depth32FloatStencil8
              ? xenos::DepthRenderTargetFormat::kD24FS8
              : xenos::DepthRenderTargetFormat::kD24S8;
      if (std::isfinite(polygon_offset)) {
        desc.depth_bias = draw_util::GetD3D10IntegerPolygonOffset(bias_format, polygon_offset);
      }
      // With resolution scaling, the slope in host pixels is smaller (as Vulkan: the worst
      // case of non-square scaling, max).
      float slope = polygon_offset_scale * xenos::kPolygonOffsetScaleSubpixelUnit *
                    float(std::max(command_processor_.draw_resolution_scale_x(),
                                   command_processor_.draw_resolution_scale_y()));
      desc.depth_bias_slope_scale = std::isfinite(slope) ? slope : 0.0f;
      if (desc.depth_bias_slope_scale == 0.0f) {
        desc.depth_bias_slope_scale = 0.0f;  // -0 -> +0 for the memcmp key
      }
    }
  }
  if (depth_clip_control_ && pa_cl_clip_cntl.clip_disable &&
      REXCVAR_GET(webgpu_depth_clip_control)) {
    desc.unclipped_depth = 1;
  }
  if (!domain_as_vertex && (topology == WGPUPrimitiveTopology_TriangleStrip ||
                            topology == WGPUPrimitiveTopology_LineStrip)) {
    desc.strip_index_format = uint8_t(state.index_format);
  }
  if (primitive_polygonal) {
    auto mode = sc_mode;
    desc.front_face_cw = uint8_t(mode.face);
    if (mode.cull_front && mode.cull_back) {
      // Everything culled; WebGPU has no front-and-back culling.
      ++statistics_.draws_refused_cull_both;
      return false;
    }
    desc.cull_mode = uint8_t(mode.cull_front  ? WGPUCullMode_Front
                             : mode.cull_back ? WGPUCullMode_Back
                                              : WGPUCullMode_None);
  } else {
    desc.cull_mode = uint8_t(WGPUCullMode_None);
  }
  desc.alpha_to_coverage = uint8_t(state.pixel_shader && desc.sample_count > 1 &&
                                   state.pixel_shader->writes_color_target(0) &&
                                   (desc.color_write_masks & 0xF) &&
                                   regs.Get<reg::RB_COLORCONTROL>().alpha_to_mask_enable);

  statistics_.draws_depth_bias += (desc.depth_bias || desc.depth_bias_slope_scale != 0.0f);
  statistics_.draws_unclipped_depth += desc.unclipped_depth;
  statistics_.draws_stencil_back_masks += stencil_back_masks;
  pipeline_out = GetOrCreate(desc);
  if (!REXCVAR_GET(webgpu_draw_with_pipelines)) {
    // Diagnostics: pipelines are created, draws are dropped.
    pipeline_out = kInvalidPipelineId;
    ++statistics_.draws_refused_no_pipeline;
    return false;
  }
  if (pipeline_out == kInvalidPipelineId) {
    ++statistics_.draws_refused_no_pipeline;
    return false;
  }
  return true;
}

PipelineId WebGPUPipelineCache::GetMemExportComputePipeline(
    Shader& vs, const WebGPUShaderBindings*& bindings_out) {
  bindings_out = nullptr;
  if (!kHasTint || !translator_ || !compute_shared_memory_layout_) {
    return kInvalidPipelineId;
  }
  AnalyzeShaderUcode(vs);
  if (!vs.memexport_eM_written()) {
    return kInvalidPipelineId;
  }
  SpirvShaderTranslator::Modification mod(translator_->GetDefaultVertexShaderModification(
      vs.GetDynamicAddressableRegisterCount(register_file_.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg),
      HVST::kMemExportCompute));
  WgslKey key{vs.ucode_data_hash(), mod.value};
  auto it = compute_pipelines_.find(key);
  if (it != compute_pipelines_.end()) {
    if (it->second != kInvalidPipelineId) {
      bindings_out = GetShaderBindings(vs, mod.value);
    }
    return bindings_out ? it->second : kInvalidPipelineId;
  }
  bindings_out = GetShaderBindings(vs, mod.value);
  std::shared_ptr<const std::string> wgsl = bindings_out ? GetWgsl(vs, mod.value) : nullptr;
  if (!wgsl) {
    bindings_out = nullptr;
    compute_pipelines_.emplace(key, kInvalidPipelineId);
    REXGPU_ERROR("WebGPU: vertex shader {:016X} has no memexport compute variant",
                 vs.ucode_data_hash());
    return kInvalidPipelineId;
  }
  WebGPUPipelineTable& table = command_processor_.command_queue().pipeline_table();
  PipelineId id = table.Allocate();
  compute_pipelines_.emplace(key, id);
  ++statistics_.pipelines_requested;
  REXGPU_INFO("WebGPU: memexport compute pipeline {} for vertex shader {:016X}", id,
              vs.ucode_data_hash());
  std::array<WGPUBindGroupLayout, 3> layout_array = {compute_shared_memory_layout_,
                                                     constants_layout_, bindings_out->layout};
  WebGPUDevice& device = command_processor_.device();
  std::shared_ptr<AsyncCounters> stats = async_counters_;
  std::string label = fmt::format("xe vs {:016X}:{:016X} memexport compute", key.hash, mod.value);
  device.RunAsync([&device, &table, stats, id, wgsl, label, layout_array]() {
    if (device.is_lost()) {
      stats->Settle(id);
      return;
    }
    WGPUDevice wgpu_device = device.device();
    WGPUPipelineLayoutDescriptor layout_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    layout_desc.bindGroupLayoutCount = layout_array.size();
    layout_desc.bindGroupLayouts = layout_array.data();
    WGPUPipelineLayout pipeline_layout = wgpuDeviceCreatePipelineLayout(wgpu_device, &layout_desc);
    WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
    source.code = MakeStringView(*wgsl);
    WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    module_desc.nextInChain = &source.chain;
    module_desc.label = MakeStringView(label);
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(wgpu_device, &module_desc);
    WGPUComputePipelineDescriptor desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    desc.label = MakeStringView(label);
    desc.layout = pipeline_layout;
    desc.compute.module = module;
    desc.compute.entryPoint = MakeStringView("main");
    struct Context {
      WebGPUPipelineTable* table;
      std::shared_ptr<AsyncCounters> stats;
      PipelineId id;
      std::string label;
    };
    WGPUCreateComputePipelineAsyncCallbackInfo callback =
        WGPU_CREATE_COMPUTE_PIPELINE_ASYNC_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = [](WGPUCreatePipelineAsyncStatus status, WGPUComputePipeline pipeline,
                           WGPUStringView message, void* userdata1, void*) {
      std::unique_ptr<Context> context(static_cast<Context*>(userdata1));
      if (status == WGPUCreatePipelineAsyncStatus_Success && pipeline) {
        context->table->SetCompute(context->id, pipeline);
        context->stats->pipelines_created.fetch_add(1, std::memory_order_relaxed);
      } else {
        if (pipeline) {
          wgpuComputePipelineRelease(pipeline);
        }
        context->stats->pipelines_failed.fetch_add(1, std::memory_order_relaxed);
        REXGPU_ERROR("WebGPU: compute pipeline {} ({}) failed: {}", context->id, context->label,
                     ToStringView(message));
      }
      context->stats->Settle(context->id);
    };
    callback.userdata1 = new Context{&table, stats, id, label};
    wgpuDeviceCreateComputePipelineAsync(wgpu_device, &desc, callback);
    if (pipeline_layout) {
      wgpuPipelineLayoutRelease(pipeline_layout);
    }
    if (module) {
      wgpuShaderModuleRelease(module);
    }
  });
  return id;
}

PipelineId WebGPUPipelineCache::GetOrCreate(const WebGPUPipelineDescription& description) {
  auto it = pipelines_.find(description);
  if (it != pipelines_.end()) {
    return it->second;
  }
  const WebGPUPipelineDescription& d = description;
  // Shaders: SPIR-V translation here (CP thread), Tint on the conversion threads.
  std::shared_ptr<WgslSlot> vs_slot, ps_slot;
  std::shared_ptr<const std::string> ps_fixed;
  auto vs_it = shaders_.find(d.vertex_shader_hash);
  if (vs_it != shaders_.end()) {
    vs_slot = RequestWgsl(*vs_it->second, d.vertex_shader_modification);
  }
  RenderPassFormatKey pass_key;
  pass_key.key = d.render_pass_format_key;
  bool has_color = (pass_key.depth_and_color_used >> 1) != 0;
  bool pixel_ok = true;
  if (d.pixel_shader_hash || d.pixel_shader_modification) {
    auto ps_it = shaders_.find(d.pixel_shader_hash);
    if (ps_it != shaders_.end()) {
      ps_slot = RequestWgsl(*ps_it->second, d.pixel_shader_modification);
    }
    pixel_ok = ps_slot != nullptr;
  } else if (has_color) {
    // Color attachments without a guest pixel shader: an empty fragment stage
    // with zero write masks keeps the pipeline compatible with the pass.
    ps_fixed = GetDepthOnlyFragmentWgsl(
        uint32_t(SpirvShaderTranslator::Modification::DepthStencilMode::kNoModifiers));
    pixel_ok = ps_fixed != nullptr;
  }

  // Explicit layout (see the header); ConfigurePipeline has looked up the
  // bindings of both shaders.
  WGPUBindGroupLayout layouts[4] = {shared_memory_layout_, constants_layout_, empty_layout_,
                                    empty_layout_};
  auto vs_bindings_it = shader_bindings_.find(d.vertex_shader_hash);
  if (vs_bindings_it != shader_bindings_.end() && vs_bindings_it->second) {
    layouts[2] = vs_bindings_it->second->layout;
  }
  if (d.pixel_shader_hash || d.pixel_shader_modification) {
    auto ps_bindings_it = shader_bindings_.find(d.pixel_shader_hash);
    if (ps_bindings_it != shader_bindings_.end() && ps_bindings_it->second) {
      layouts[3] = ps_bindings_it->second->layout;
    }
  }

  std::string label =
      fmt::format("xe vs {:016X}:{:016X} ps {:016X}:{:016X}", d.vertex_shader_hash,
                  d.vertex_shader_modification, d.pixel_shader_hash, d.pixel_shader_modification);
  std::array<WGPUBindGroupLayout, 4> layout_array = {layouts[0], layouts[1], layouts[2],
                                                     layouts[3]};

  WebGPUPipelineTable& table = command_processor_.command_queue().pipeline_table();
  PipelineId id = kInvalidPipelineId;
  std::shared_ptr<const std::string> vs_wgsl, ps_wgsl = ps_fixed;
  bool deferred = false;
  {
    // Checked and registered atomically with the conversion threads completing slots.
    std::lock_guard<std::mutex> lock(wgsl_mutex_);
    if (!vs_slot || !pixel_ok || (vs_slot->ready && !vs_slot->text) ||
        (ps_slot && ps_slot->ready && !ps_slot->text)) {
      // A shader is missing or failed: remembered, the draws are dropped.
      pipelines_.emplace(description, kInvalidPipelineId);
      return kInvalidPipelineId;
    }
    id = table.Allocate();
    uint32_t pending = uint32_t(!vs_slot->ready) + uint32_t(ps_slot && !ps_slot->ready);
    if (pending) {
      auto pipeline = std::make_shared<DeferredPipeline>(
          DeferredPipeline{id, d, label, layout_array, vs_slot, ps_slot, ps_fixed, pending});
      for (const std::shared_ptr<WgslSlot>& slot : {vs_slot, ps_slot}) {
        if (slot && !slot->ready) {
          slot->waiters.push_back(pipeline);
        }
      }
      deferred = true;
    } else {
      vs_wgsl = vs_slot->text;
      if (ps_slot) {
        ps_wgsl = ps_slot->text;
      }
    }
  }
  pipelines_.emplace(description, id);
  ++statistics_.pipelines_requested;
  if (REXCVAR_GET(webgpu_log_skipped_pipelines)) {
    REXGPU_INFO(
        "WebGPU pipeline {}: {} pass {:08X} topology {} color masks {:04X} depth {:08X} "
        "(shaders {})",
        id, label, d.render_pass_format_key, uint32_t(d.primitive_topology), d.color_write_masks,
        d.depth_control, deferred ? "converting" : "ready");
  }
  if (statistics_.pipelines_requested <= 2 || !(statistics_.pipelines_requested % 32)) {
    const Statistics& statistics = this->statistics();
    REXGPU_INFO(
        "WebGPU pipeline cache: {} pipelines requested ({} created, {} failed); WGSL: {} "
        "storage hits, {} translated, {} converting, {} failures, {} memory hits",
        statistics.pipelines_requested, async_counters_->pipelines_created.load(),
        async_counters_->pipelines_failed.load(), statistics.wgsl_storage_hits,
        statistics.wgsl_translated, async_counters_->wgsl_converting.load(),
        statistics.wgsl_failures, statistics.wgsl_memory_hits);
  }
  if (!deferred) {
    CreateRenderPipelineAsync(id, d, std::move(vs_wgsl), std::move(ps_wgsl), std::move(label),
                              layout_array);
  }
  return id;
}

double WebGPUPipelineCache::WaitForPipeline(PipelineId id, int32_t timeout_ms) {
  if (id == kInvalidPipelineId || !timeout_ms) {
    return 0.0;
  }
  if (id < settled_known_.size() && settled_known_[id]) {
    return 0.0;
  }
  AsyncCounters& counters = *async_counters_;
  auto start = std::chrono::steady_clock::now();
  bool settled;
  {
    std::unique_lock<std::mutex> lock(counters.settled_mutex);
    auto is_settled = [&]() { return counters.settled.count(id) != 0; };
    if (timeout_ms < 0) {
      counters.settled_cv.wait(lock, is_settled);
      settled = true;
    } else {
      settled = counters.settled_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                             is_settled);
    }
  }
  if (settled) {
    if (id >= settled_known_.size()) {
      settled_known_.resize(size_t(id) + 1024);
    }
    settled_known_[id] = true;
  }
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
      .count();
}

void WebGPUPipelineCache::CreateRenderPipelineAsync(
    PipelineId id, const WebGPUPipelineDescription& d, std::shared_ptr<const std::string> vs_wgsl,
    std::shared_ptr<const std::string> ps_wgsl, std::string label,
    const std::array<WGPUBindGroupLayout, 4>& layout_array) {
  WebGPUDevice& device = command_processor_.device();
  WebGPUPipelineTable& table = command_processor_.command_queue().pipeline_table();
  std::shared_ptr<AsyncCounters> stats = async_counters_;
  device.RunAsync([&device, &table, stats, id, d, vs_wgsl, ps_wgsl, label, layout_array]() {
    if (device.is_lost()) {
      stats->Settle(id);
      return;
    }
    WGPUDevice wgpu_device = device.device();
    WGPUPipelineLayoutDescriptor layout_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    layout_desc.bindGroupLayoutCount = layout_array.size();
    layout_desc.bindGroupLayouts = layout_array.data();
    WGPUPipelineLayout pipeline_layout = wgpuDeviceCreatePipelineLayout(wgpu_device, &layout_desc);
    auto create_module = [&](const std::string& code, const char* stage) {
      WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
      source.code = MakeStringView(code);
      WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
      module_desc.nextInChain = &source.chain;
      std::string module_label = label + " " + stage;
      module_desc.label = MakeStringView(module_label);
      return wgpuDeviceCreateShaderModule(wgpu_device, &module_desc);
    };
    WGPUShaderModule vs_module = create_module(*vs_wgsl, "vs");
    WGPUShaderModule ps_module = ps_wgsl ? create_module(*ps_wgsl, "ps") : nullptr;

    RenderPassFormatKey key;
    key.key = d.render_pass_format_key;
    WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    desc.label = MakeStringView(label);
    desc.layout = pipeline_layout;
    desc.vertex.module = vs_module;
    desc.vertex.entryPoint = MakeStringView("main");
    // Tessellated draws: the domain points of WebGPUTessellator, two float4
    // attributes per vertex (location 0 domain location, 1 patch input).
    WGPUVertexAttribute domain_attributes[2];
    WGPUVertexBufferLayout domain_layout = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    if (Shader::IsHostVertexShaderTypeDomain(
            SpirvShaderTranslator::Modification(d.vertex_shader_modification)
                .vertex.host_vertex_shader_type)) {
      for (uint32_t i = 0; i < 2; ++i) {
        domain_attributes[i] = WGPU_VERTEX_ATTRIBUTE_INIT;
        domain_attributes[i].format = WGPUVertexFormat_Float32x4;
        domain_attributes[i].offset = 16 * i;
        domain_attributes[i].shaderLocation = i;
      }
      domain_layout.stepMode = WGPUVertexStepMode_Vertex;
      domain_layout.arrayStride = WebGPUTessellator::kPointStride;
      domain_layout.attributeCount = 2;
      domain_layout.attributes = domain_attributes;
      desc.vertex.bufferCount = 1;
      desc.vertex.buffers = &domain_layout;
    }
    desc.primitive.topology = WGPUPrimitiveTopology(d.primitive_topology);
    desc.primitive.stripIndexFormat = WGPUIndexFormat(d.strip_index_format);
    desc.primitive.cullMode = WGPUCullMode(d.cull_mode);
    desc.primitive.frontFace = d.front_face_cw ? WGPUFrontFace_CW : WGPUFrontFace_CCW;
    // Only set on a device with depth-clip-control (ConfigurePipeline).
    desc.primitive.unclippedDepth = d.unclipped_depth ? WGPU_TRUE : WGPU_FALSE;
    desc.multisample.count = d.sample_count ? d.sample_count : 1;
    // Guest 2x is host 4x with samples 0 and 3 (WebGPURenderTargetCache).
    desc.multisample.mask = key.msaa_samples == xenos::MsaaSamples::k2X ? 0b1001u : 0xFFFFFFFFu;
    desc.multisample.alphaToCoverageEnabled = d.alpha_to_coverage != 0;

    WGPUDepthStencilState depth_stencil = WGPU_DEPTH_STENCIL_STATE_INIT;
    if (key.depth_and_color_used & 1) {
      reg::RB_DEPTHCONTROL dc;
      dc.value = d.depth_control;
      depth_stencil.format = WGPUTextureFormat(d.depth_texture_format);
      depth_stencil.depthWriteEnabled =
          (dc.z_enable && dc.z_write_enable) ? WGPUOptionalBool_True : WGPUOptionalBool_False;
      depth_stencil.depthCompare =
          dc.z_enable ? ToCompare(dc.zfunc) : WGPUCompareFunction_Always;
      auto face = [](xenos::CompareFunction func, xenos::StencilOp fail, xenos::StencilOp zfail,
                     xenos::StencilOp pass) {
        WGPUStencilFaceState s = WGPU_STENCIL_FACE_STATE_INIT;
        s.compare = ToCompare(func);
        s.failOp = ToStencilOp(fail);
        s.depthFailOp = ToStencilOp(zfail);
        s.passOp = ToStencilOp(pass);
        return s;
      };
      if (dc.stencil_enable) {
        depth_stencil.stencilFront =
            face(dc.stencilfunc, dc.stencilfail, dc.stencilzfail, dc.stencilzpass);
        depth_stencil.stencilBack =
            dc.backface_enable
                ? face(dc.stencilfunc_bf, dc.stencilfail_bf, dc.stencilzfail_bf, dc.stencilzpass_bf)
                : depth_stencil.stencilFront;
        // WebGPU has one read / write mask for both faces (see the description).
        depth_stencil.stencilReadMask = d.stencil_read_mask;
        depth_stencil.stencilWriteMask = d.stencil_write_mask;
      } else {
        depth_stencil.stencilFront = face(xenos::CompareFunction::kAlways, xenos::StencilOp::kKeep,
                                          xenos::StencilOp::kKeep, xenos::StencilOp::kKeep);
        depth_stencil.stencilBack = depth_stencil.stencilFront;
        depth_stencil.stencilReadMask = 0xFF;
        depth_stencil.stencilWriteMask = 0;
      }
      depth_stencil.depthBias = d.depth_bias;
      depth_stencil.depthBiasSlopeScale = d.depth_bias_slope_scale;
      depth_stencil.depthBiasClamp = 0.0f;
      desc.depthStencil = &depth_stencil;
    }

    WGPUColorTargetState targets[xenos::kMaxColorRenderTargets];
    WGPUBlendState blends[xenos::kMaxColorRenderTargets];
    uint32_t target_count = 0;
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      targets[i] = WGPU_COLOR_TARGET_STATE_INIT;
      if (!(key.depth_and_color_used & (1u << (1 + i)))) {
        // Hole: no attachment at this slot in the pass either.
        targets[i].format = WGPUTextureFormat_Undefined;
        continue;
      }
      target_count = i + 1;
      targets[i].format = WebGPURenderTargetCache::GetColorTextureFormat(key.color_format(i));
      targets[i].writeMask = WGPUColorWriteMask((d.color_write_masks >> (4 * i)) & 0xF);
      if (d.blend_control[i]) {
        reg::RB_BLENDCONTROL bc;
        bc.value = d.blend_control[i];
        blends[i].color = ToBlendComponent(bc.color_srcblend, bc.color_comb_fcn, bc.color_destblend);
        blends[i].alpha = ToBlendComponent(bc.alpha_srcblend, bc.alpha_comb_fcn, bc.alpha_destblend);
        targets[i].blend = &blends[i];
      }
    }
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    if (ps_module) {
      fragment.module = ps_module;
      fragment.entryPoint = MakeStringView("main");
      fragment.targetCount = target_count;
      fragment.targets = targets;
      desc.fragment = &fragment;
    }

    struct Context {
      WebGPUPipelineTable* table;
      std::shared_ptr<AsyncCounters> stats;
      PipelineId id;
      std::string label;
    };
    WGPUCreateRenderPipelineAsyncCallbackInfo callback = WGPU_CREATE_RENDER_PIPELINE_ASYNC_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = [](WGPUCreatePipelineAsyncStatus status, WGPURenderPipeline pipeline,
                           WGPUStringView message, void* userdata1, void*) {
      std::unique_ptr<Context> context(static_cast<Context*>(userdata1));
      if (status == WGPUCreatePipelineAsyncStatus_Success && pipeline) {
        context->table->SetRender(context->id, pipeline);
        context->stats->pipelines_created.fetch_add(1, std::memory_order_relaxed);
      } else {
        if (pipeline) {
          wgpuRenderPipelineRelease(pipeline);
        }
        context->stats->pipelines_failed.fetch_add(1, std::memory_order_relaxed);
        REXGPU_ERROR("WebGPU: render pipeline {} ({}) failed: {}", context->id, context->label,
                     ToStringView(message));
      }
      context->stats->Settle(context->id);
    };
    callback.userdata1 = new Context{&table, stats, id, label};
    wgpuDeviceCreateRenderPipelineAsync(wgpu_device, &desc, callback);
    if (pipeline_layout) {
      wgpuPipelineLayoutRelease(pipeline_layout);
    }
    wgpuShaderModuleRelease(vs_module);
    if (ps_module) {
      wgpuShaderModuleRelease(ps_module);
    }
  });
}

}  // namespace rex::graphics::webgpu
