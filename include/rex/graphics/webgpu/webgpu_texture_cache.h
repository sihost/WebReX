/**
 * @file        rex/graphics/webgpu/webgpu_texture_cache.h
 * @brief       WebGPU texture cache (WP5): TextureCache with compute untile/convert loads.
 *
 * docs/port/webgpu.md section 7. The shared TextureCache does keys, layouts, the LRU, memory
 * watches and binding bookkeeping; this backend creates the host textures and records loads
 * into the CP's command stream (outside render passes) through WebGPUTextureLoader:
 * shared memory -> compute (untiling, endian swap, format conversion, decompression) ->
 * scratch storage buffer -> copyBufferToTexture per mip level. The host format table
 * (WebGPUTextureFormatTable) depends on optional features, each of which can be forced off
 * by a cvar (webgpu_texture_bc, webgpu_texture_bc_sliced_3d, webgpu_texture_formats_tier1,
 * webgpu_texture_component_swizzle, webgpu_float32_filterable).
 *
 * Signedness: WebGPU views can't reinterpret unorm as snorm, so a texture fetched signed
 * with a format whose signed host format differs gets a second host texture
 * (TextureKey::signed_separate), loaded lazily by the same machinery.
 *
 * Dirty map (section 8): on web no store faults, so RequestTextures and RequestSwapTexture
 * consume Memory::ConsumePhysicalDirty for the guest ranges of every texture they return.
 * A dirty page fires the shared memory watch (the texture becomes outdated) and the texture
 * is reloaded right away.
 *
 * Device objects (textures, views, samplers) are created on the device thread with RunSync
 * on a cache miss and released after the stream that last could reference them is submitted.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <webgpu/webgpu.h>

#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/webgpu/webgpu_texture_load.h>
#include <rex/graphics/xenos.h>

namespace rex::memory {
class Memory;
}  // namespace rex::memory

namespace rex::graphics {
class Shader;
}  // namespace rex::graphics

namespace rex::graphics::webgpu {

class WebGPUCommandProcessor;
class WebGPUSharedMemory;

class WebGPUTextureCache final : public TextureCache {
 public:
  struct Statistics {
    uint64_t textures_created = 0;
    uint64_t loads = 0;
    uint64_t load_failures = 0;
    // Reloads caused by the web dirty map (RequestTextures / RequestSwapTexture).
    uint64_t dirty_reloads = 0;
    uint64_t views_created = 0;
    uint64_t samplers_created = 0;
    // Draw resolution scaling: scaled resolve windows allocated, loads of scaled textures.
    uint64_t scaled_windows = 0;
    uint64_t scaled_loads = 0;
  };

  // draw_resolution_scale_x/y: TextureCache::GetConfigDrawResolutionScale (1 = no scaling).
  WebGPUTextureCache(WebGPUCommandProcessor& command_processor, const RegisterFile& register_file,
                     WebGPUSharedMemory& shared_memory, memory::Memory& memory,
                     uint32_t draw_resolution_scale_x = 1, uint32_t draw_resolution_scale_y = 1);
  ~WebGPUTextureCache() override;

  bool Initialize();
  void Shutdown();
  void ClearCache() override;

  // Makes the textures in used_texture_mask (fetch constant bits) resident and up to date
  // (also against the web dirty map). Records loads into the CP stream (ends the render pass
  // if anything is loaded).
  void RequestTextures(uint32_t used_texture_mask) override;
  // RequestTextures for the fetch constants the shaders' ucode references.
  void RequestTexturesForShaders(const Shader& vertex_shader, const Shader* pixel_shader);

  // Device-thread view of the active binding for a shader texture binding of `dimension`
  // (2d-array for 1D/2D, 3d, cube), or a null (zero) view of that dimension.
  WGPUTextureView GetActiveBindingOrNullView(uint32_t fetch_constant_index,
                                             xenos::FetchOpDimension dimension, bool is_signed);
  // The null (zero) view of a binding dimension.
  WGPUTextureView GetNullView(xenos::FetchOpDimension dimension) const;
  // Whether the active binding's host format can be linearly filtered (false: bind it as
  // unfilterable-float with a non-filtering sampler).
  bool IsActiveTextureFilterable(uint32_t fetch_constant_index, bool is_signed) const;
  // Whether views apply the host swizzle (texture-component-swizzle); if not, shaders must
  // apply GetActiveTextureHostSwizzle themselves.
  bool views_apply_swizzle() const { return component_swizzle_; }

  // Normalized sampler state for a shader sampler binding (fetch constant + overrides).
  WebGPUSamplerParameters GetSamplerParameters(uint32_t fetch_constant,
                                               xenos::TextureFilter mag_filter,
                                               xenos::TextureFilter min_filter,
                                               xenos::TextureFilter mip_filter,
                                               xenos::AnisoFilter aniso_filter) const;
  // Any binding type with fetch_constant / mag_filter / min_filter / mip_filter /
  // aniso_filter (SpirvShader::SamplerBinding).
  template <typename SamplerBinding>
  WebGPUSamplerParameters GetSamplerParameters(const SamplerBinding& binding) const {
    return GetSamplerParameters(binding.fetch_constant, binding.mag_filter, binding.min_filter,
                                binding.mip_filter, binding.aniso_filter);
  }
  // Device-thread sampler for the parameters (created on first use, kept until shutdown).
  WGPUSampler UseSampler(WebGPUSamplerParameters parameters);

  // The front buffer (texture fetch constant 0) as a 2D single-mip view on the device
  // thread, or null. width/height: host texture size (times the draw resolution scale if
  // *scaled_out). If the host swizzle is not applied by the view and swaps red and blue,
  // *swap_source_needs_rb_swap_out is set.
  WGPUTextureView RequestSwapTexture(uint32_t& width_out, uint32_t& height_out,
                                     xenos::TextureFormat& format_out,
                                     bool* swap_source_needs_rb_swap_out = nullptr,
                                     bool* scaled_out = nullptr);

  // ---- Scaled resolve memory (draw resolution scaling). ----
  // WebGPU has no sparse buffers, and the scaled copy of the 512 MiB shared memory would be
  // 2-4.5 GiB, so it is kept in lazily created windows: window j holds the unscaled range
  // [j * W, j * W + 2 * W) (W = scaled_window_unscaled_size()) times the scale, so any range
  // up to W long lies within one window (the one of its start) for reading; a resolve writes
  // every window overlapping its range (up to 3).
  struct ScaledResolveWindow {
    WGPUBuffer buffer = nullptr;
    uint32_t start_unscaled = 0;
    uint64_t size_scaled = 0;
  };
  bool scaled_resolve_available() const { return scaled_window_log2_ != 0; }
  uint32_t scaled_window_unscaled_size() const {
    return scaled_window_log2_ ? UINT32_C(1) << scaled_window_log2_ : 0;
  }
  // Creates (if needed) and returns the windows overlapping [start, start + length), or
  // false if the range can't be resolution-scaled (longer than W, no scaling, or a window
  // could not be created); the resolve then writes only the shared memory
  // (MarkRangeAsResolved(..., false)).
  bool GetScaledResolveWriteWindows(uint32_t start_unscaled, uint32_t length_unscaled,
                                    std::vector<ScaledResolveWindow>& windows_out);
  bool EnsureScaledResolveMemoryCommitted(uint32_t start_unscaled, uint32_t length_unscaled,
                                          uint32_t length_scaled_alignment_log2 = 0) override;

  const WebGPUTextureFormatTable& format_table() const { return format_table_; }
  const WebGPUTextureFeatures& features() const { return format_table_.features(); }
  const Statistics& statistics() const { return statistics_; }
  const WebGPUTextureLoader::Statistics& loader_statistics() const {
    return loader_.statistics();
  }

 protected:
  bool IsSignedVersionSeparateForFormat(TextureKey key) const override;
  bool IsScaledResolveSupportedForFormat(TextureKey key) const override;
  uint32_t GetHostFormatSwizzle(TextureKey key) const override;

  uint32_t GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const override;
  uint32_t GetMaxHostTextureDepthOrArraySize(xenos::DataDimension dimension) const override;

  std::unique_ptr<Texture> CreateTexture(TextureKey key) override;

  bool LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                             bool load_mips) override;

  void UpdateTextureBindingsImpl(uint32_t fetch_constant_mask) override;

 private:
  class WebGPUTexture final : public Texture {
   public:
    WebGPUTexture(WebGPUTextureCache& texture_cache, const TextureKey& key, WGPUTexture texture,
                  WGPUTextureFormat format, uint32_t mip_level_count, uint64_t memory_bytes);
    ~WebGPUTexture() override;

    WGPUTexture texture() const { return texture_; }
    WGPUTextureFormat format() const { return format_; }
    uint32_t mip_level_count() const { return mip_level_count_; }

    // 2d-array (1D/2D/stacked), 3d or cube view of all levels, or with base_level_only a 2d
    // view of layer 0 / level 0 (swap). host_swizzle is applied if the cache supports it.
    WGPUTextureView GetView(uint32_t host_swizzle, bool base_level_only);

   private:
    WGPUTexture texture_;
    WGPUTextureFormat format_;
    uint32_t mip_level_count_;
    std::unordered_map<uint32_t, WGPUTextureView> views_;
  };

  struct WebGPUTextureBinding {
    WGPUTextureView view_unsigned = nullptr;
    WGPUTextureView view_signed = nullptr;
    void Reset() {
      view_unsigned = nullptr;
      view_signed = nullptr;
    }
  };

  // Handles released on the device thread after the current stream is submitted (or right
  // away during shutdown, after the CP has drained the queue).
  struct ReleaseBatch {
    std::vector<WGPUTexture> textures;
    std::vector<WGPUTextureView> views;
  };
  void DeferRelease(WGPUTexture texture, const std::vector<WGPUTextureView>& views);
  static void ReleaseBatchOnDeviceThread(ReleaseBatch& batch);

  static constexpr bool AreDimensionsCompatible(xenos::FetchOpDimension binding_dimension,
                                                xenos::DataDimension resource_dimension) {
    switch (binding_dimension) {
      case xenos::FetchOpDimension::k1D:
      case xenos::FetchOpDimension::k2D:
        return resource_dimension == xenos::DataDimension::k1D ||
               resource_dimension == xenos::DataDimension::k2DOrStacked;
      case xenos::FetchOpDimension::k3DOrStacked:
        return resource_dimension == xenos::DataDimension::k3D;
      case xenos::FetchOpDimension::kCube:
        return resource_dimension == xenos::DataDimension::kCube;
      default:
        return false;
    }
  }

  const WebGPUTextureHostFormatPair& GetHostFormatPair(TextureKey key) const {
    return format_table_.Get(key.format, key.dimension, key.GetWidth(), key.GetHeight());
  }
  // Starts a new TextureCache submission when the CP's stream changed (LRU bookkeeping).
  void UpdateSubmission();
  // Consumes the web dirty map for the texture's guest ranges and reloads it if that (or an
  // earlier invalidation) made it outdated.
  void RefreshTexture(Texture& texture);

  // The read window of a range: [start, start + length) must be at most W long. Creates it
  // if needed; null on failure.
  const ScaledResolveWindow* GetScaledResolveReadWindow(uint32_t start_unscaled,
                                                        uint32_t length_unscaled);
  // Window j, created on first use (CP thread, RunSync on the device thread).
  const ScaledResolveWindow* GetScaledResolveWindow(uint32_t index);

  WebGPUCommandProcessor& command_processor_;
  memory::Memory& memory_;
  // log2 of W, 0 when not scaling (or no window fits the device's buffer limits).
  uint32_t scaled_window_log2_ = 0;
  std::vector<ScaledResolveWindow> scaled_windows_;
  WebGPUTextureFormatTable format_table_;
  WebGPUTextureLoader loader_;
  bool component_swizzle_ = false;
  bool shutting_down_ = false;

  std::array<WebGPUTextureBinding, xenos::kTextureFetchConstantCount> webgpu_bindings_;

  // Null (zero) textures for unbound fetches: 2d-array, cube, 3d.
  WGPUTexture null_texture_2d_ = nullptr;
  WGPUTexture null_texture_3d_ = nullptr;
  WGPUTextureView null_view_2d_array_ = nullptr;
  WGPUTextureView null_view_cube_ = nullptr;
  WGPUTextureView null_view_3d_ = nullptr;

  std::unordered_map<uint32_t, WGPUSampler> samplers_;

  ReleaseBatch* pending_release_ = nullptr;
  uint64_t pending_release_submission_ = 0;
  ReleaseBatch shutdown_release_;

  uint64_t tracked_submission_ = 0;
  Statistics statistics_;
  bool border_color_logged_ = false;
  bool rb_swap_logged_ = false;
};

}  // namespace rex::graphics::webgpu
