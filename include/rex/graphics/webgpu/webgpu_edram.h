/**
 * @file        rex/graphics/webgpu/webgpu_edram.h
 * @brief       WebGPU EDRAM buffer and the fixed EDRAM shaders (dump, load/clear,
 *              resolve) used by WebGPURenderTargetCache.
 *
 * docs/port/webgpu.md sections 5 and 6 (WP4). The guest EDRAM is emulated with
 * host render targets (RenderTargetCache::Path::kHostRenderTargets). Every
 * EDRAM-level operation goes through a 10 MiB storage buffer holding the
 * guest EDRAM bit layout (webgpu_edram_shaders.h):
 *  - Dump: render target -> EDRAM buffer (compute). Used by resolves and as
 *    the first half of ownership transfers.
 *  - Load: EDRAM buffer -> render target (fullscreen-triangle draws per
 *    rectangle). The second half of ownership transfers: dumping the source
 *    and loading the destination reinterprets the bits for any pair of
 *    formats (color <-> depth, 32 <-> 64 bpp, 1x <-> 2x/4x MSAA) with 2 x 4
 *    shaders instead of a shader per format pair. Stencil is loaded with one
 *    draw per bit (no stencil export in WebGPU).
 *  - Clear: the load pipelines with a constant instead of the buffer.
 *  - Resolve: EDRAM buffer -> shared memory (compute) with format conversion,
 *    sample averaging, exponent bias, endian swap and 2D/3D tiling.
 *
 * Depends only on the stream/device (no rexcore), so the standalone browser
 * test (tools/webgpu_tests/wp4_edram in the project) drives it directly.
 * Formats are passed as xenos::ColorRenderTargetFormat /
 * DepthRenderTargetFormat / ColorFormat numeric values.
 *
 * Threads: Initialize/Shutdown on the device thread; Record* on the recording
 * thread (they only store handles in the stream). Record* expect the stream
 * outside a pass except RecordDump/RecordResolve, which expect a compute pass
 * open, so several dispatches share one pass.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <cstdint>

#include <webgpu/webgpu.h>

#include <rex/graphics/webgpu/webgpu_command_stream.h>

namespace rex::graphics::webgpu {

class WebGPUDevice;

// A host render target as the EDRAM shaders see it.
struct WebGPUEdramSurface {
  // All aspects (render attachment).
  WGPUTextureView attachment_view = nullptr;
  // Color: same as attachment_view; depth: depth-only aspect.
  WGPUTextureView sample_view = nullptr;
  // Depth: stencil-only aspect.
  WGPUTextureView stencil_view = nullptr;
  WGPUTextureFormat host_format = WGPUTextureFormat_Undefined;
  // Texture size in pixels.
  uint32_t width = 0;
  uint32_t height = 0;
  // xenos::ColorRenderTargetFormat or xenos::DepthRenderTargetFormat value.
  uint32_t format = 0;
  bool is_depth = false;
  // xenos::MsaaSamples value: 0 = 1x, 1 = 2x (4 host samples), 2 = 4x.
  uint32_t msaa_samples = 0;
  uint32_t base_tiles = 0;
  // In tiles, doubled for 64bpp (RenderTargetKey::GetPitchTiles).
  uint32_t pitch_tiles = 0;
};

struct WebGPUEdramRectangle {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

// Uniform block of the resolve shader (kResolveMain), unpacked on the CPU from
// draw_util::ResolveInfo.
struct WebGPUResolveConstants {
  uint32_t edram_base_tiles;
  uint32_t edram_pitch_tiles;
  uint32_t edram_msaa_samples;
  uint32_t edram_is_depth;
  uint32_t edram_format;
  uint32_t edram_is_64bpp;
  // Origin relative to the base tile, in pixels.
  uint32_t offset_x;
  uint32_t offset_y;
  // In pixels (multiples of 8).
  uint32_t width;
  uint32_t height;
  // Aligned to 32.
  uint32_t dest_pitch;
  uint32_t dest_height;
  uint32_t dest_offset_x;
  uint32_t dest_offset_y;
  uint32_t dest_array;
  uint32_t dest_slice;
  // xenos::ColorFormat.
  uint32_t dest_format;
  // xenos::Endian128.
  uint32_t dest_endian;
  int32_t dest_exp_bias;
  uint32_t dest_swap;
  // xenos::CopySampleSelect.
  uint32_t sample_select;
  // Bit copy of the selected sample instead of conversion.
  uint32_t raw;
  uint32_t dest_bpp_log2;
  // Byte offset of the copy destination base in the bound destination range.
  int32_t dest_base;
};
static_assert(sizeof(WebGPUResolveConstants) == 96);

class WebGPUEdram {
 public:
  static constexpr uint32_t kTileCount = 2048;
  static constexpr uint32_t kTileBytes = 80 * 16 * 4;
  static constexpr uint32_t kBufferSize = kTileCount * kTileBytes;

  explicit WebGPUEdram(WebGPUDevice& device) : device_(device) {}
  WebGPUEdram(const WebGPUEdram&) = delete;
  WebGPUEdram& operator=(const WebGPUEdram&) = delete;
  // Shutdown() must have run on the device thread.
  ~WebGPUEdram() = default;

  // ---- Device thread. ----
  bool Initialize();
  void Shutdown();

  // ---- Any thread after Initialize. ----
  WGPUBuffer buffer() const { return buffer_; }
  bool has_depth32float_stencil8() const { return has_depth32float_stencil8_; }
  // Host formats (docs/port/webgpu.md section 5). Undefined for invalid input.
  static WGPUTextureFormat GetColorHostFormat(uint32_t color_format);
  // 8_8_8_8_GAMMA render targets are linear rgba16unorm (gamma_render_target_as_unorm16
  // and texture-formats-tier1), set by Initialize before any render target exists;
  // otherwise rgba8unorm with the PWL gamma applied in the pixel shaders.
  static bool gamma_as_unorm16() { return gamma_as_unorm16_; }
  WGPUTextureFormat GetDepthHostFormat(uint32_t depth_format) const;
  // 1 or 4 (2x is emulated with 4x).
  static uint32_t GetHostSampleCount(uint32_t msaa_samples) { return msaa_samples ? 4 : 1; }
  // Bits per pixel log2 of a resolve destination xenos::ColorFormat, or
  // UINT32_MAX if it can't be resolved to.
  static uint32_t GetResolveDestBppLog2(uint32_t color_format);

  // ---- Recording thread. ----

  // Compute pass open. Dumps width_tiles x height_tiles tiles starting at the
  // (not wrapped) EDRAM tile first_tile with rows dispatch_pitch_tiles apart
  // from `source` to the EDRAM buffer.
  void RecordDump(WebGPUCommandStream& stream, const WebGPUEdramSurface& source,
                  uint32_t first_tile, uint32_t dispatch_pitch_tiles, uint32_t width_tiles,
                  uint32_t height_tiles);
  // Outside passes. Opens a render pass on `dest`, loads the rectangles (dest
  // pixels) from the EDRAM buffer, then optionally clears clear_rectangle to
  // clear_value (EDRAM bits: depth (d24 << 8) | stencil, 64bpp color low
  // dword first), and closes the pass.
  void RecordLoad(WebGPUCommandStream& stream, const WebGPUEdramSurface& dest,
                  const WebGPUEdramRectangle* rectangles, uint32_t rectangle_count,
                  const WebGPUEdramRectangle* clear_rectangle = nullptr,
                  uint64_t clear_value = 0);
  // Compute pass open. dest_buffer [dest_offset, dest_offset + dest_size) is
  // bound as the destination (dest_offset a multiple of 256);
  // constants.dest_base is relative to dest_offset.
  void RecordResolve(WebGPUCommandStream& stream, const WebGPUResolveConstants& constants,
                     WGPUBuffer dest_buffer, uint64_t dest_offset, uint64_t dest_size);

  struct Statistics {
    uint64_t dumps = 0;
    uint64_t load_rectangles = 0;
    uint64_t clears = 0;
    uint64_t resolves = 0;
  };
  const Statistics& statistics() const { return statistics_; }

 private:
  static constexpr uint32_t kColorHostFormatCount = 7;
  static constexpr uint32_t kDepthPipelineVariants = 9;  // 0: depth + stencil ref, 1-8: bits.

  static int GetColorHostFormatIndex(WGPUTextureFormat format);
  WGPUComputePipeline CreateComputePipeline(const char* label, const char* const* sources,
                                            uint32_t source_count, WGPUBindGroupLayout layout);
  void DrawRectangles(WebGPUCommandStream& stream, const WebGPUEdramSurface& dest,
                      bool from_constant, uint64_t constant,
                      const WebGPUEdramRectangle* rectangles, uint32_t rectangle_count);

  WebGPUDevice& device_;
  bool has_depth32float_stencil8_ = false;
  static inline bool gamma_as_unorm16_ = false;

  // Device-thread objects (handles used for recording on any thread).
  WGPUBuffer buffer_ = nullptr;
  // Dump: [color 1x, color MS, depth 1x, depth MS].
  WGPUBindGroupLayout dump_layouts_[4] = {};
  WGPUComputePipeline dump_pipelines_[4] = {};
  WGPUBindGroupLayout load_layout_ = nullptr;
  WGPUShaderModule load_module_ = nullptr;
  // [host format index][0 = 1 sample, 1 = 4 samples].
  WGPURenderPipeline color_load_pipelines_[kColorHostFormatCount][2] = {};
  // Set (device thread, error scope callback) when the 4-sample load pipeline of a format
  // WebGPU does not guarantee multisampling for (rg32float) failed validation: Chrome
  // accepts it, Firefox does not. Such loads are skipped instead of using the error object.
  std::atomic<bool> color_load_msaa_failed_[kColorHostFormatCount] = {};
  // [0 = depth24plus-stencil8, 1 = depth32float-stencil8][samples][variant].
  WGPURenderPipeline depth_load_pipelines_[2][2][kDepthPipelineVariants] = {};
  WGPUBindGroupLayout resolve_layout_ = nullptr;
  WGPUComputePipeline resolve_pipeline_ = nullptr;

  Statistics statistics_;
};

}  // namespace rex::graphics::webgpu
