/**
 * @file        rex/graphics/webgpu/webgpu_render_target_cache.h
 * @brief       WebGPU render target cache: EDRAM emulated with host render targets.
 *
 * WP4, docs/port/webgpu.md sections 5 and 6. RenderTargetCache with
 * Path::kHostRenderTargets only (no FSI/ROV in WebGPU). The shared base does
 * the EDRAM tile ownership tracking; this class creates the host textures and
 * performs the GPU work through WebGPUEdram (webgpu_edram.h):
 *  - Ownership transfers (EDRAM aliasing between render targets): the source
 *    tiles are dumped to the EDRAM buffer in the guest bit layout, then drawn
 *    into the destination with its own format's decoding. That covers color
 *    <-> depth, 32 <-> 64 bpp and 1x <-> 2x/4x in two steps. Stencil is
 *    written one bit per draw (no stencil export), so it survives depth ->
 *    color -> depth round trips.
 *  - Resolves: dump of the owning render targets, then a compute shader
 *    writes the shared memory (format conversion, sample averaging, exponent
 *    bias, endian swap, tiling); the range is marked with
 *    SharedMemory::RangeWrittenByGpu (MakeRangeValid(written_by_gpu) after
 *    consuming the web dirty map) and in the texture cache. With
 *    --webgpu_readback_resolve the data is also copied back into guest memory
 *    (Memory::TranslatePhysical, plain memcpy: no dirty marking).
 *  - Resolve clears: draws with the clear value (scissored).
 *
 * Host formats: see WebGPUEdram::GetColorHostFormat / GetDepthHostFormat.
 * 16_16(_16_16) are stored as float (full -32...32 range, not truncated).
 * MSAA: 1x and 4x only; guest 2x uses 4x host samples (0/1 top, 2/3 bottom;
 * draws should use sample mask 0b1001). D24FS8 host depth is the guest value
 * * 0.5 (pass full_float24_in_0_to_1 = false to GetHostViewportInfo, as on
 * Vulkan). Draw resolution scaling is not supported (1x).
 *
 * Contract with the command processor (WP1): Update() binds the guest render
 * targets for the draw; the CP then calls
 * BeginRenderPass(last_update_render_pass(), last_update_render_pass_key()).
 * Transfers are recorded before that (the CP's open pass is ended first).
 * Pipeline cache (WP2): the low 32 bits of last_update_render_pass_key() are
 * a RenderPassFormatKey (formats + sample count, all a pipeline needs); the
 * high 32 bits identify the bound textures so the CP never reuses an open pass
 * for different attachments.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <webgpu/webgpu.h>

#include <rex/graphics/pipeline/render_target/cache.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/trace_writer.h>
#include <rex/graphics/webgpu/webgpu_command_stream.h>
#include <rex/graphics/webgpu/webgpu_edram.h>
#include <rex/graphics/xenos.h>
#include <rex/memory.h>

namespace rex::graphics::webgpu {

class WebGPUCommandProcessor;
class WebGPUSharedMemory;
class WebGPUTextureCache;

class WebGPURenderTargetCache final : public RenderTargetCache {
 public:
  // Formats and sample count of a render pass (the low 32 bits of
  // last_update_render_pass_key()).
  union RenderPassFormatKey {
    struct {
      xenos::MsaaSamples msaa_samples : xenos::kMsaaSamplesBits;  // 2
      // << 0 is depth, << 1...4 is color.
      uint32_t depth_and_color_used : 1 + xenos::kMaxColorRenderTargets;   // 7
      xenos::DepthRenderTargetFormat depth_format : xenos::kDepthRenderTargetFormatBits;  // 8
      // Resource formats (8_8_8_8_GAMMA stored as 8_8_8_8).
      xenos::ColorRenderTargetFormat color_0_format : xenos::kColorRenderTargetFormatBits;  // 12
      xenos::ColorRenderTargetFormat color_1_format : xenos::kColorRenderTargetFormatBits;  // 16
      xenos::ColorRenderTargetFormat color_2_format : xenos::kColorRenderTargetFormatBits;  // 20
      xenos::ColorRenderTargetFormat color_3_format : xenos::kColorRenderTargetFormatBits;  // 24
    };
    uint32_t key = 0;
    xenos::ColorRenderTargetFormat color_format(uint32_t index) const {
      switch (index) {
        case 0:
          return color_0_format;
        case 1:
          return color_1_format;
        case 2:
          return color_2_format;
        default:
          return color_3_format;
      }
    }
  };
  static_assert(sizeof(RenderPassFormatKey) == sizeof(uint32_t));
  static RenderPassFormatKey GetRenderPassFormatKey(uint64_t render_pass_key) {
    RenderPassFormatKey key;
    key.key = uint32_t(render_pass_key);
    return key;
  }

  WebGPURenderTargetCache(WebGPUCommandProcessor& command_processor,
                          const RegisterFile& register_file, const memory::Memory& memory,
                          TraceWriter& trace_writer);
  ~WebGPURenderTargetCache() override;

  // shared_memory_binding_count: storage bindings the shared memory needs
  // (WebGPUSharedMemory::binding_count()); resolves bind only the written
  // range, so any count works.
  bool Initialize(uint32_t shared_memory_binding_count);
  void Shutdown(bool from_destructor = false);
  void ClearCache() override;
  void RestoreEdramSnapshot(const void* snapshot);

  // Called by the command processor when its stream has been submitted: any
  // render pass it had open is closed now.
  void OnStreamSubmitted() {}

  Path GetPath() const override { return Path::kHostRenderTargets; }

  bool Update(bool is_rasterization_done, reg::RB_DEPTHCONTROL normalized_depth_control,
              uint32_t normalized_color_mask, const Shader& vertex_shader) override;
  const WebGPUCommandStream::RenderPassDesc& last_update_render_pass() const {
    return last_update_render_pass_;
  }
  uint64_t last_update_render_pass_key() const { return last_update_render_pass_key_; }

  bool Resolve(memory::Memory& memory, WebGPUSharedMemory& shared_memory,
               WebGPUTextureCache& texture_cache, uint32_t& written_address_out,
               uint32_t& written_length_out);

  // ---- For the pipeline cache (WP2) and the command processor. ----
  static WGPUTextureFormat GetColorTextureFormat(xenos::ColorRenderTargetFormat format) {
    return WebGPUEdram::GetColorHostFormat(uint32_t(format));
  }
  WGPUTextureFormat GetDepthTextureFormat(xenos::DepthRenderTargetFormat format) const {
    return edram_ ? edram_->GetDepthHostFormat(uint32_t(format))
                  : WGPUTextureFormat_Depth24PlusStencil8;
  }
  static uint32_t GetHostSampleCount(xenos::MsaaSamples msaa_samples) {
    return WebGPUEdram::GetHostSampleCount(uint32_t(msaa_samples));
  }
  static constexpr bool IsMsaa2xSupported() { return false; }
  // D24FS8: host depth = guest depth * 0.5 (full_float24_in_0_to_1 = false).
  static constexpr bool depth_float24_remapped_to_0_to_0_5() { return true; }
  static constexpr bool IsFixedRG16TruncatedToMinus1To1() { return false; }
  static constexpr bool IsFixedRGBA16TruncatedToMinus1To1() { return false; }
  WGPUBuffer edram_buffer() const { return edram_ ? edram_->buffer() : nullptr; }

  struct Statistics {
    uint64_t render_targets_created = 0;
    uint64_t transfers = 0;
    uint64_t resolves = 0;
    uint64_t resolve_bytes = 0;
    uint64_t resolve_clears = 0;
    uint64_t readbacks = 0;
    uint64_t updates_without_attachments = 0;
  };
  const Statistics& statistics() const { return statistics_; }
  const WebGPUEdram::Statistics* edram_statistics() const {
    return edram_ ? &edram_->statistics() : nullptr;
  }

 protected:
  uint32_t GetMaxRenderTargetWidth() const override;
  uint32_t GetMaxRenderTargetHeight() const override;

  RenderTarget* CreateRenderTarget(RenderTargetKey key) override;

  // Host depth is the guest value (unorm24 in depth24plus, float24 exactly in
  // depth32float), no separate host-precision copy is tracked.
  bool IsHostDepthEncodingDifferent(xenos::DepthRenderTargetFormat /*format*/) const override {
    return false;
  }
  // 8_8_8_8_GAMMA shares rgba8unorm with 8_8_8_8 (PWL gamma in the shaders).
  bool IsGammaFormatHostStorageSeparate() const override { return false; }

 private:
  class WebGPURenderTarget final : public RenderTarget {
   public:
    WebGPURenderTarget(RenderTargetKey key, WebGPURenderTargetCache& cache, WGPUTexture texture,
                       const WebGPUEdramSurface& surface)
        : RenderTarget(key), cache_(cache), texture_(texture), surface_(surface) {}
    ~WebGPURenderTarget() override;
    const WebGPUEdramSurface& surface() const { return surface_; }

   private:
    WebGPURenderTargetCache& cache_;
    WGPUTexture texture_;
    WebGPUEdramSurface surface_;
  };

  struct PendingRelease {
    WGPUTexture texture;
    WGPUTextureView views[3];
  };
  // Releases after the current stream is replayed (or now when shutting down).
  void ReleaseRenderTargetObjects(const PendingRelease& objects);
  static void ReleaseCallback(void* user, WebGPUReplayState& state);
  // Device thread; deletes `objects`.
  static void ReleaseObjects(PendingRelease* objects);

  // Transfers for render_targets[i] (and clears render_targets[i] to
  // clear_values[i] in *clear_rectangle if given). Records outside the
  // command processor's render pass.
  void PerformTransfersAndResolveClears(uint32_t render_target_count,
                                        RenderTarget* const* render_targets,
                                        const std::vector<Transfer>* transfers,
                                        const uint64_t* clear_values = nullptr,
                                        const Transfer::Rectangle* clear_rectangle = nullptr);
  // Dumps the render targets owning the tile span to the EDRAM buffer. Compute
  // pass must be open.
  void DumpRenderTargets(uint32_t dump_base, uint32_t dump_row_length_used, uint32_t dump_rows,
                         uint32_t dump_pitch);
  bool ReadbackResolve(memory::Memory& memory, WebGPUSharedMemory& shared_memory,
                       uint32_t address, uint32_t length);

  WebGPUCommandProcessor& command_processor_;
  TraceWriter& trace_writer_;
  std::unique_ptr<WebGPUEdram> edram_;
  bool shutting_down_ = false;

  WebGPUCommandStream::RenderPassDesc last_update_render_pass_;
  uint64_t last_update_render_pass_key_ = 0;
  // Attachments of the last update, to give a changed set a new pass key.
  RenderTarget* last_update_attachments_[1 + xenos::kMaxColorRenderTargets] = {};
  uint32_t attachments_generation_ = 0;

  std::vector<ResolveCopyDumpRectangle> dump_rectangles_;
  std::vector<WebGPUEdramRectangle> load_rectangles_;
  std::vector<Transfer> clear_transfers_[2];

  // Resolve readback (--webgpu_readback_resolve), device-thread buffer.
  WGPUBuffer readback_buffer_ = nullptr;
  uint64_t readback_buffer_size_ = 0;

  Statistics statistics_;
};

}  // namespace rex::graphics::webgpu
