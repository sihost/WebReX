/**
 * @file        graphics/webgpu/webgpu_texture_cache.cpp
 * @brief       See rex/graphics/webgpu/webgpu_texture_cache.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/webgpu_texture_cache.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include <rex/bit.h>
#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/webgpu/webgpu_command_processor.h>
#include <rex/graphics/webgpu/webgpu_device.h>
#include <rex/graphics/webgpu/webgpu_shared_memory.h>
#include <rex/logging.h>
#include <rex/memory.h>

REXCVAR_DEFINE_BOOL(webgpu_texture_bc, true, "GPU",
                    "WebGPU: keep DXT1/DXT2_3/DXT4_5/DXN/DXT5A textures BC-compressed when the "
                    "device has texture-compression-bc (false: decompress in compute)");
REXCVAR_DEFINE_BOOL(webgpu_texture_bc_sliced_3d, true, "GPU",
                    "WebGPU: BC-compressed 3D textures with texture-compression-bc-sliced-3d "
                    "(false: decompress 3D BC textures)");
REXCVAR_DEFINE_BOOL(webgpu_texture_formats_tier1, false, "GPU",
                    "WebGPU: r/rg/rgba16 unorm/snorm textures with texture-formats-tier1. These "
                    "are unfilterable-float in WebGPU, so such textures are then bound as the "
                    "null texture (the texture layouts are filterable float); false (default): "
                    "16-bit normalized data as float16, filterable");
REXCVAR_DEFINE_BOOL(webgpu_texture_component_swizzle, true, "GPU",
                    "WebGPU: apply texture swizzles in views with texture-component-swizzle "
                    "(false: shaders swizzle)");
REXCVAR_DEFINE_BOOL(webgpu_float32_filterable, true, "GPU",
                    "WebGPU: filter 32-bit float textures with float32-filterable (false: "
                    "nearest)");

namespace rex::graphics::webgpu {

namespace {

constexpr WGPUComponentSwizzle ToComponentSwizzle(uint32_t xenos_component) {
  switch (xenos_component) {
    case xenos::XE_GPU_TEXTURE_SWIZZLE_R:
      return WGPUComponentSwizzle_R;
    case xenos::XE_GPU_TEXTURE_SWIZZLE_G:
      return WGPUComponentSwizzle_G;
    case xenos::XE_GPU_TEXTURE_SWIZZLE_B:
      return WGPUComponentSwizzle_B;
    case xenos::XE_GPU_TEXTURE_SWIZZLE_A:
      return WGPUComponentSwizzle_A;
    case xenos::XE_GPU_TEXTURE_SWIZZLE_0:
      return WGPUComponentSwizzle_Zero;
    case xenos::XE_GPU_TEXTURE_SWIZZLE_1:
      return WGPUComponentSwizzle_One;
    default:
      return WGPUComponentSwizzle_Undefined;
  }
}

}  // namespace

WebGPUTextureCache::WebGPUTextureCache(WebGPUCommandProcessor& command_processor,
                                       const RegisterFile& register_file,
                                       WebGPUSharedMemory& shared_memory, memory::Memory& memory)
    : TextureCache(register_file, shared_memory, 1, 1),
      command_processor_(command_processor),
      memory_(memory) {}

WebGPUTextureCache::~WebGPUTextureCache() {
  Shutdown();
}

bool WebGPUTextureCache::Initialize() {
  WebGPUDevice& device = command_processor_.device();
  WebGPUTextureFeatures features;
  features.bc = REXCVAR_GET(webgpu_texture_bc) &&
                device.HasFeature(WGPUFeatureName_TextureCompressionBC);
  features.bc_sliced_3d = features.bc && REXCVAR_GET(webgpu_texture_bc_sliced_3d) &&
                          device.HasFeature(WGPUFeatureName_TextureCompressionBCSliced3D);
  features.formats_tier1 = REXCVAR_GET(webgpu_texture_formats_tier1) &&
                           device.HasFeature(WGPUFeatureName_TextureFormatsTier1);
  features.float32_filterable = REXCVAR_GET(webgpu_float32_filterable) &&
                                device.HasFeature(WGPUFeatureName_Float32Filterable);
  component_swizzle_ = REXCVAR_GET(webgpu_texture_component_swizzle) &&
                       device.HasFeature(WGPUFeatureName_TextureComponentSwizzle);
  format_table_.Initialize(features);

  if (!loader_.Initialize(device)) {
    REXGPU_ERROR("WebGPU texture cache: texture loader initialization failed");
    return false;
  }

  // Null textures (zero-initialized by WebGPU) for unbound or invalid fetches, matching the
  // D3D12 null SRV (all components 0).
  device.RunSync([&]() {
    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.label = MakeStringView("rex null texture 2d");
    desc.usage = WGPUTextureUsage_TextureBinding;
    desc.dimension = WGPUTextureDimension_2D;
    desc.format = WGPUTextureFormat_RGBA8Unorm;
    desc.size = {1, 1, 6};
    null_texture_2d_ = wgpuDeviceCreateTexture(device.device(), &desc);
    desc.label = MakeStringView("rex null texture 3d");
    desc.dimension = WGPUTextureDimension_3D;
    desc.size = {1, 1, 1};
    null_texture_3d_ = wgpuDeviceCreateTexture(device.device(), &desc);
    if (null_texture_2d_) {
      WGPUTextureViewDescriptor view_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
      view_desc.dimension = WGPUTextureViewDimension_2DArray;
      view_desc.arrayLayerCount = 1;
      null_view_2d_array_ = wgpuTextureCreateView(null_texture_2d_, &view_desc);
      view_desc.dimension = WGPUTextureViewDimension_Cube;
      view_desc.arrayLayerCount = 6;
      null_view_cube_ = wgpuTextureCreateView(null_texture_2d_, &view_desc);
    }
    if (null_texture_3d_) {
      WGPUTextureViewDescriptor view_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
      view_desc.dimension = WGPUTextureViewDimension_3D;
      null_view_3d_ = wgpuTextureCreateView(null_texture_3d_, &view_desc);
    }
  });
  if (!null_view_2d_array_ || !null_view_cube_ || !null_view_3d_) {
    REXGPU_ERROR("WebGPU texture cache: creating the null textures failed");
    return false;
  }

  REXGPU_INFO(
      "WebGPU texture cache: BC {}, BC 3D {}, 16-bit normalized formats {}, float32 filtering "
      "{}, view swizzle {}",
      features.bc ? "native" : "decompressed", features.bc_sliced_3d ? "native" : "decompressed",
      features.formats_tier1 ? "native (unfilterable, bound as null)" : "as float16",
      features.float32_filterable ? "yes" : "no",
      component_swizzle_ ? "yes" : "no (in shaders)");
  return true;
}

void WebGPUTextureCache::Shutdown() {
  if (shutting_down_) {
    return;
  }
  // The command processor has drained its queue: everything recorded so far is replayed, so
  // the handles can be released directly.
  // A batch still attached to the open stream (if any) is released when that stream is
  // submitted; it is left alone here.
  shutting_down_ = true;
  pending_release_ = nullptr;
  DestroyAllTextures();
  WebGPUDevice& device = command_processor_.device();
  std::unordered_map<uint32_t, WGPUSampler> samplers = std::move(samplers_);
  samplers_.clear();
  device.RunSync([&]() {
    ReleaseBatchOnDeviceThread(shutdown_release_);
    for (auto& sampler : samplers) {
      wgpuSamplerRelease(sampler.second);
    }
    for (WGPUTextureView* view :
         {&null_view_2d_array_, &null_view_cube_, &null_view_3d_}) {
      if (*view) {
        wgpuTextureViewRelease(*view);
        *view = nullptr;
      }
    }
    for (WGPUTexture* texture : {&null_texture_2d_, &null_texture_3d_}) {
      if (*texture) {
        wgpuTextureRelease(*texture);
        *texture = nullptr;
      }
    }
  });
  loader_.Shutdown();
}

void WebGPUTextureCache::ClearCache() {
  TextureCache::ClearCache();
}

void WebGPUTextureCache::DeferRelease(WGPUTexture texture,
                                      const std::vector<WGPUTextureView>& views) {
  if (shutting_down_) {
    // Released together at the end of Shutdown (the CP has drained its queue).
    if (texture) {
      shutdown_release_.textures.push_back(texture);
    }
    shutdown_release_.views.insert(shutdown_release_.views.end(), views.begin(), views.end());
    return;
  }
  uint64_t submission = command_processor_.GetCurrentSubmission();
  if (!pending_release_ || pending_release_submission_ != submission) {
    // One batch per stream, released after that stream's submit (the stream and the ones
    // before it are the only places the handles can be referenced from).
    pending_release_ = new ReleaseBatch;
    pending_release_submission_ = submission;
    command_processor_.command_stream().AddSubmittedCallback(
        [](void* user, WebGPUReplayState&) {
          ReleaseBatch* batch = static_cast<ReleaseBatch*>(user);
          ReleaseBatchOnDeviceThread(*batch);
          delete batch;
        },
        pending_release_);
  }
  if (texture) {
    pending_release_->textures.push_back(texture);
  }
  pending_release_->views.insert(pending_release_->views.end(), views.begin(), views.end());
}

void WebGPUTextureCache::ReleaseBatchOnDeviceThread(ReleaseBatch& batch) {
  for (WGPUTextureView view : batch.views) {
    wgpuTextureViewRelease(view);
  }
  for (WGPUTexture texture : batch.textures) {
    wgpuTextureDestroy(texture);
    wgpuTextureRelease(texture);
  }
  batch.views.clear();
  batch.textures.clear();
}

void WebGPUTextureCache::UpdateSubmission() {
  uint64_t current = command_processor_.GetCurrentSubmission();
  if (current > tracked_submission_) {
    tracked_submission_ = current;
    BeginSubmission(current);
    CompletedSubmissionUpdated(command_processor_.GetCompletedSubmission());
  }
}

void WebGPUTextureCache::RefreshTexture(Texture& texture) {
  // memory.md section 7: no write faults on web; the dirty map is the only record of CPU
  // writes since the texture was loaded. Consuming it fires the shared memory watch.
  const TextureKey& key = texture.key();
  bool dirty = false;
  if (texture.GetGuestBaseSize()) {
    dirty |= memory_.ConsumePhysicalDirty(key.base_page << 12, texture.GetGuestBaseSize());
  }
  if (texture.GetGuestMipsSize()) {
    dirty |= memory_.ConsumePhysicalDirty(key.mip_page << 12, texture.GetGuestMipsSize());
  }
  if (texture.outdated_mask()) {
    if (dirty) {
      ++statistics_.dirty_reloads;
    }
    LoadTextureData(texture);
  }
}

void WebGPUTextureCache::RequestTextures(uint32_t used_texture_mask) {
  UpdateSubmission();
  TextureCache::RequestTextures(used_texture_mask);
  // Dirty map, and mark everything bound as used by this stream.
  Texture* refreshed[2 * xenos::kTextureFetchConstantCount];
  uint32_t refreshed_count = 0;
  uint32_t remaining = used_texture_mask;
  uint32_t index;
  while (rex::bit_scan_forward(remaining, &index)) {
    remaining &= ~(UINT32_C(1) << index);
    const TextureBinding* binding = GetValidTextureBinding(index);
    if (!binding) {
      continue;
    }
    for (Texture* texture : {binding->texture, binding->texture_signed}) {
      if (!texture ||
          std::find(refreshed, refreshed + refreshed_count, texture) != refreshed + refreshed_count) {
        continue;
      }
      refreshed[refreshed_count++] = texture;
      RefreshTexture(*texture);
      texture->MarkAsUsed();
    }
  }
}

void WebGPUTextureCache::RequestTexturesForShaders(const Shader& vertex_shader,
                                                   const Shader* pixel_shader) {
  uint32_t used_texture_mask = 0;
  for (const Shader::TextureBinding& binding : vertex_shader.texture_bindings()) {
    used_texture_mask |= UINT32_C(1) << (binding.fetch_constant & 31);
  }
  if (pixel_shader) {
    for (const Shader::TextureBinding& binding : pixel_shader->texture_bindings()) {
      used_texture_mask |= UINT32_C(1) << (binding.fetch_constant & 31);
    }
  }
  if (used_texture_mask) {
    RequestTextures(used_texture_mask);
  }
}

WGPUTextureView WebGPUTextureCache::GetActiveBindingOrNullView(uint32_t fetch_constant_index,
                                                               xenos::FetchOpDimension dimension,
                                                               bool is_signed) {
  WGPUTextureView view = nullptr;
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);
  if (binding && AreDimensionsCompatible(dimension, binding->key.dimension)) {
    const WebGPUTextureBinding& webgpu_binding = webgpu_bindings_[fetch_constant_index];
    view = is_signed ? webgpu_binding.view_signed : webgpu_binding.view_unsigned;
  }
  return view ? view : GetNullView(dimension);
}

WGPUTextureView WebGPUTextureCache::GetNullView(xenos::FetchOpDimension dimension) const {
  switch (dimension) {
    case xenos::FetchOpDimension::k3DOrStacked:
      return null_view_3d_;
    case xenos::FetchOpDimension::kCube:
      return null_view_cube_;
    default:
      return null_view_2d_array_;
  }
}

bool WebGPUTextureCache::IsActiveTextureFilterable(uint32_t fetch_constant_index,
                                                   bool is_signed) const {
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);
  if (!binding) {
    return true;
  }
  const WebGPUTextureHostFormatPair& pair = GetHostFormatPair(binding->key);
  bool separate = WebGPUTextureFormatTable::IsSignedVersionSeparate(pair);
  return WebGPUTextureFormatTable::GetHostFormat(pair, is_signed && separate).filterable;
}

WebGPUSamplerParameters WebGPUTextureCache::GetSamplerParameters(
    uint32_t fetch_constant, xenos::TextureFilter binding_mag_filter,
    xenos::TextureFilter binding_min_filter, xenos::TextureFilter binding_mip_filter,
    xenos::AnisoFilter binding_aniso_filter) const {
  // VulkanTextureCache::GetSamplerParameters with WebGPU's address modes (no border colors,
  // no mirror-clamp) and filterability.
  xenos::xe_gpu_texture_fetch_t fetch = register_file().GetTextureFetch(fetch_constant & 31);
  WebGPUSamplerParameters parameters;
  xenos::ClampMode clamp_x, clamp_y, clamp_z;
  texture_util::GetClampModesForDimension(fetch, clamp_x, clamp_y, clamp_z);
  parameters.clamp_x = NormalizeWebGPUClampMode(clamp_x);
  parameters.clamp_y = NormalizeWebGPUClampMode(clamp_y);
  parameters.clamp_z = NormalizeWebGPUClampMode(clamp_z);
  parameters.border_color = xenos::BorderColor::k_ABGR_Black;

  xenos::TextureFilter mag_filter = binding_mag_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.mag_filter
                                        : binding_mag_filter;
  parameters.mag_linear = mag_filter == xenos::TextureFilter::kLinear;
  xenos::TextureFilter min_filter = binding_min_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.min_filter
                                        : binding_min_filter;
  parameters.min_linear = min_filter == xenos::TextureFilter::kLinear;
  xenos::TextureFilter mip_filter = binding_mip_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.mip_filter
                                        : binding_mip_filter;
  parameters.mip_linear = mip_filter == xenos::TextureFilter::kLinear;
  xenos::AnisoFilter aniso_filter = binding_aniso_filter == xenos::AnisoFilter::kUseFetchConst
                                        ? fetch.aniso_filter
                                        : binding_aniso_filter;
  aniso_filter = std::min(aniso_filter, xenos::AnisoFilter::kMax_16_1);
  bool linear_filterable = true;
  if (parameters.mag_linear || parameters.min_linear || parameters.mip_linear ||
      aniso_filter != xenos::AnisoFilter::kDisabled) {
    TextureKey texture_key;
    uint8_t texture_swizzled_signs;
    BindingInfoFromFetchConstant(fetch, texture_key, &texture_swizzled_signs);
    if (texture_key.is_valid) {
      const WebGPUTextureHostFormatPair& pair = GetHostFormatPair(texture_key);
      if ((texture_util::IsAnySignNotSigned(texture_swizzled_signs) &&
           !pair.format_unsigned.filterable) ||
          (texture_util::IsAnySignSigned(texture_swizzled_signs) &&
           !pair.format_signed.filterable)) {
        linear_filterable = false;
      }
    } else {
      linear_filterable = false;
    }
    if (!linear_filterable) {
      parameters.mag_linear = 0;
      parameters.min_linear = 0;
      parameters.mip_linear = 0;
      aniso_filter = xenos::AnisoFilter::kDisabled;
    }
  }
  bool min_mag_linear = parameters.mag_linear && parameters.min_linear;
  bool mip_filter_bilinear_or_trilinear =
      mip_filter == xenos::TextureFilter::kPoint || mip_filter == xenos::TextureFilter::kLinear;
  parameters.mip_base_map = mip_filter == xenos::TextureFilter::kBaseMap;
  uint32_t mip_min_level, mip_max_level;
  texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, nullptr, nullptr,
                                                 &mip_min_level, &mip_max_level);
  parameters.mip_min_level = mip_min_level;
  bool has_mips = mip_max_level > mip_min_level;
  int32_t anisotropic_override = REXCVAR_GET(anisotropic_override);
  if (linear_filterable && anisotropic_override > -1 && anisotropic_override < 6 && has_mips &&
      !parameters.mip_base_map && min_mag_linear && mip_filter_bilinear_or_trilinear) {
    aniso_filter = xenos::AnisoFilter(anisotropic_override);
  }
  aniso_filter = std::min(aniso_filter, xenos::AnisoFilter::kMax_16_1);
  if (aniso_filter != xenos::AnisoFilter::kDisabled) {
    // As on D3D12 / Vulkan, anisotropic filtering forces linear min/mag/mip (WebGPU also
    // requires all-linear filters for maxAnisotropy > 1).
    parameters.mag_linear = 1;
    parameters.min_linear = 1;
    parameters.mip_linear = 1;
  }
  parameters.aniso_filter = aniso_filter;
  return parameters;
}

WGPUSampler WebGPUTextureCache::UseSampler(WebGPUSamplerParameters parameters) {
  auto it = samplers_.find(parameters.value);
  if (it != samplers_.end()) {
    return it->second;
  }
  WGPUSampler sampler = nullptr;
  WebGPUDevice& device = command_processor_.device();
  device.RunSync([&]() {
    WGPUSamplerDescriptor desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    MakeSamplerDescriptor(parameters, desc);
    sampler = wgpuDeviceCreateSampler(device.device(), &desc);
  });
  if (sampler) {
    samplers_.emplace(parameters.value, sampler);
    ++statistics_.samplers_created;
  }
  return sampler;
}

WGPUTextureView WebGPUTextureCache::RequestSwapTexture(uint32_t& width_out, uint32_t& height_out,
                                                       xenos::TextureFormat& format_out,
                                                       bool* swap_source_needs_rb_swap_out) {
  UpdateSubmission();
  xenos::xe_gpu_texture_fetch_t fetch = register_file().GetTextureFetch(0);
  width_out = fetch.size_2d.width + 1;
  height_out = fetch.size_2d.height + 1;
  format_out = fetch.format;
  if (swap_source_needs_rb_swap_out) {
    *swap_source_needs_rb_swap_out = false;
  }
  TextureKey key;
  BindingInfoFromFetchConstant(fetch, key, nullptr);
  if (!key.is_valid || key.base_page == 0 || key.dimension != xenos::DataDimension::k2DOrStacked) {
    return nullptr;
  }
  WebGPUTexture* texture = static_cast<WebGPUTexture*>(FindOrCreateTexture(key));
  if (!texture) {
    return nullptr;
  }
  uint32_t host_swizzle = GuestToHostSwizzle(fetch.swizzle, GetHostFormatSwizzle(key));
  if (!component_swizzle_) {
    auto component = [host_swizzle](uint32_t i) { return (host_swizzle >> (3 * i)) & 0b111; };
    bool rb_swap = component(0) == xenos::XE_GPU_TEXTURE_SWIZZLE_B &&
                   component(1) == xenos::XE_GPU_TEXTURE_SWIZZLE_G &&
                   component(2) == xenos::XE_GPU_TEXTURE_SWIZZLE_R;
    if (swap_source_needs_rb_swap_out) {
      *swap_source_needs_rb_swap_out = rb_swap;
    } else if (rb_swap && !rb_swap_logged_) {
      rb_swap_logged_ = true;
      REXGPU_WARN(
          "WebGPU texture cache: the front buffer swizzle swaps red and blue, but views can't "
          "swizzle without texture-component-swizzle");
    }
  }
  RefreshTexture(*texture);
  if (texture->outdated_mask()) {
    // Load failed (or the data isn't resident).
    return nullptr;
  }
  WGPUTextureView view = texture->GetView(host_swizzle, true);
  texture->MarkAsUsed();
  key = texture->key();
  width_out = key.GetWidth();
  height_out = key.GetHeight();
  format_out = key.format;
  // A frame boundary: rebind (and recheck) everything next frame.
  BeginFrame();
  return view;
}

bool WebGPUTextureCache::IsSignedVersionSeparateForFormat(TextureKey key) const {
  return WebGPUTextureFormatTable::IsSignedVersionSeparate(GetHostFormatPair(key));
}

uint32_t WebGPUTextureCache::GetHostFormatSwizzle(TextureKey key) const {
  return GetHostFormatPair(key).swizzle;
}

uint32_t WebGPUTextureCache::GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const {
  const WGPULimits& limits = command_processor_.device().limits();
  switch (dimension) {
    case xenos::DataDimension::k1D:
    case xenos::DataDimension::k2DOrStacked:
    case xenos::DataDimension::kCube:
      return limits.maxTextureDimension2D;
    case xenos::DataDimension::k3D:
      return limits.maxTextureDimension3D;
    default:
      return 0;
  }
}

uint32_t WebGPUTextureCache::GetMaxHostTextureDepthOrArraySize(
    xenos::DataDimension dimension) const {
  const WGPULimits& limits = command_processor_.device().limits();
  switch (dimension) {
    case xenos::DataDimension::k1D:
    case xenos::DataDimension::k2DOrStacked:
      return limits.maxTextureArrayLayers;
    case xenos::DataDimension::k3D:
      return limits.maxTextureDimension3D;
    case xenos::DataDimension::kCube:
      return 6;
    default:
      return 0;
  }
}

std::unique_ptr<TextureCache::Texture> WebGPUTextureCache::CreateTexture(TextureKey key) {
  WebGPUHostTextureInfo info;
  if (!GetHostTextureInfo(format_table_, key.format, key.dimension, key.GetWidth(),
                          key.GetHeight(), key.GetDepthOrArraySize(), key.mip_max_level,
                          key.signed_separate, info)) {
    return nullptr;
  }
  WGPUTexture texture = nullptr;
  WebGPUDevice& device = command_processor_.device();
  device.RunSync([&]() {
    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.usage =
        WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst | WGPUTextureUsage_CopySrc;
    desc.dimension = info.dimension;
    desc.size = info.size;
    desc.format = info.format;
    desc.mipLevelCount = info.mip_level_count;
    desc.sampleCount = 1;
    texture = wgpuDeviceCreateTexture(device.device(), &desc);
  });
  if (!texture) {
    return nullptr;
  }
  ++statistics_.textures_created;
  return std::unique_ptr<Texture>(new WebGPUTexture(*this, key, texture, info.format,
                                                    info.mip_level_count, info.memory_bytes));
}

bool WebGPUTextureCache::LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                                               bool load_mips) {
  WebGPUTexture& webgpu_texture = static_cast<WebGPUTexture&>(texture);
  const TextureKey& key = texture.key();
  WebGPUTextureLoadDesc desc;
  desc.format = key.format;
  desc.dimension = key.dimension;
  desc.width = key.GetWidth();
  desc.height = key.GetHeight();
  desc.depth_or_array_size = key.GetDepthOrArraySize();
  desc.pitch = key.pitch;
  desc.tiled = key.tiled;
  desc.packed_mips = key.packed_mips;
  desc.has_base = key.base_page != 0;
  desc.endianness = key.endianness;
  // The host texture may have fewer levels than the key (WebGPU's mip count limit).
  desc.mip_max_level = std::min(uint32_t(key.mip_max_level), webgpu_texture.mip_level_count() - 1);
  desc.signed_separate = key.signed_separate;
  desc.source = static_cast<WebGPUSharedMemory&>(shared_memory()).buffer();
  desc.source_size = SharedMemory::kBufferSize;
  desc.base_offset = uint64_t(key.base_page) << 12;
  desc.mips_offset = uint64_t(key.mip_page) << 12;
  desc.load_base = load_base;
  desc.load_mips = load_mips && desc.mip_max_level != 0;
  desc.guest_layout = &texture.guest_layout();
  if (!desc.load_base && !desc.load_mips) {
    // Only mips beyond the host's levels were outdated.
    return true;
  }
  // Compute passes and copies: outside render passes.
  command_processor_.EndRenderPass();
  bool loaded = loader_.RecordLoad(command_processor_.command_stream(), format_table_, desc,
                                   webgpu_texture.texture());
  texture.MarkAsUsed();
  if (loaded) {
    ++statistics_.loads;
  } else {
    ++statistics_.load_failures;
    texture.LogAction("Failed to load");
  }
  return loaded;
}

void WebGPUTextureCache::UpdateTextureBindingsImpl(uint32_t fetch_constant_mask) {
  uint32_t bindings_remaining = fetch_constant_mask;
  uint32_t binding_index;
  while (rex::bit_scan_forward(bindings_remaining, &binding_index)) {
    bindings_remaining &= ~(UINT32_C(1) << binding_index);
    WebGPUTextureBinding& webgpu_binding = webgpu_bindings_[binding_index];
    webgpu_binding.Reset();
    const TextureBinding* binding = GetValidTextureBinding(binding_index);
    if (!binding) {
      continue;
    }
    const WebGPUTextureHostFormatPair& pair = GetHostFormatPair(binding->key);
    bool uses_unsigned = texture_util::IsAnySignNotSigned(binding->swizzled_signs);
    bool uses_signed = texture_util::IsAnySignSigned(binding->swizzled_signs);
    bool has_unsigned = pair.format_unsigned.format != WGPUTextureFormat_Undefined;
    bool has_signed = pair.format_signed.format != WGPUTextureFormat_Undefined;
    if (WebGPUTextureFormatTable::IsSignedVersionSeparate(pair)) {
      if (binding->texture && uses_unsigned && has_unsigned) {
        webgpu_binding.view_unsigned =
            static_cast<WebGPUTexture*>(binding->texture)->GetView(binding->host_swizzle, false);
      }
      if (binding->texture_signed && uses_signed && has_signed) {
        webgpu_binding.view_signed = static_cast<WebGPUTexture*>(binding->texture_signed)
                                         ->GetView(binding->host_swizzle, false);
      }
    } else if (binding->texture) {
      WGPUTextureView view =
          static_cast<WebGPUTexture*>(binding->texture)->GetView(binding->host_swizzle, false);
      if (uses_unsigned && has_unsigned) {
        webgpu_binding.view_unsigned = view;
      }
      if (uses_signed && has_signed) {
        webgpu_binding.view_signed = view;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// WebGPUTexture.
// ---------------------------------------------------------------------------

WebGPUTextureCache::WebGPUTexture::WebGPUTexture(WebGPUTextureCache& texture_cache,
                                                 const TextureKey& key, WGPUTexture texture,
                                                 WGPUTextureFormat format,
                                                 uint32_t mip_level_count, uint64_t memory_bytes)
    : Texture(texture_cache, key),
      texture_(texture),
      format_(format),
      mip_level_count_(mip_level_count) {
  SetHostMemoryUsage(memory_bytes);
}

WebGPUTextureCache::WebGPUTexture::~WebGPUTexture() {
  std::vector<WGPUTextureView> views;
  views.reserve(views_.size());
  for (const auto& view : views_) {
    views.push_back(view.second);
  }
  static_cast<WebGPUTextureCache&>(texture_cache()).DeferRelease(texture_, views);
}

WGPUTextureView WebGPUTextureCache::WebGPUTexture::GetView(uint32_t host_swizzle,
                                                           bool base_level_only) {
  WebGPUTextureCache& cache = static_cast<WebGPUTextureCache&>(texture_cache());
  if (!cache.component_swizzle_) {
    host_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  }
  host_swizzle &= 0xFFF;
  uint32_t view_key = host_swizzle | (uint32_t(base_level_only) << 12);
  auto it = views_.find(view_key);
  if (it != views_.end()) {
    return it->second;
  }
  WGPUTextureView view = nullptr;
  WebGPUDevice& device = cache.command_processor_.device();
  device.RunSync([&]() {
    WGPUTextureViewDescriptor desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    desc.format = format_;
    desc.aspect = WGPUTextureAspect_All;
    if (base_level_only) {
      desc.dimension = WGPUTextureViewDimension_2D;
      desc.baseMipLevel = 0;
      desc.mipLevelCount = 1;
      desc.baseArrayLayer = 0;
      desc.arrayLayerCount = 1;
    } else {
      switch (key().dimension) {
        case xenos::DataDimension::k3D:
          desc.dimension = WGPUTextureViewDimension_3D;
          break;
        case xenos::DataDimension::kCube:
          desc.dimension = WGPUTextureViewDimension_Cube;
          break;
        default:
          desc.dimension = WGPUTextureViewDimension_2DArray;
          break;
      }
    }
    WGPUTextureComponentSwizzleDescriptor swizzle_desc =
        WGPU_TEXTURE_COMPONENT_SWIZZLE_DESCRIPTOR_INIT;
    if (host_swizzle != xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA) {
      swizzle_desc.swizzle.r = ToComponentSwizzle(host_swizzle & 7);
      swizzle_desc.swizzle.g = ToComponentSwizzle((host_swizzle >> 3) & 7);
      swizzle_desc.swizzle.b = ToComponentSwizzle((host_swizzle >> 6) & 7);
      swizzle_desc.swizzle.a = ToComponentSwizzle((host_swizzle >> 9) & 7);
      desc.nextInChain = &swizzle_desc.chain;
    }
    view = wgpuTextureCreateView(texture_, &desc);
  });
  if (view) {
    views_.emplace(view_key, view);
    ++cache.statistics_.views_created;
  }
  return view;
}

}  // namespace rex::graphics::webgpu
