/**
 * @file        rex/graphics/webgpu/webgpu_primitive_processor.h
 * @brief       WebGPU primitive processor: guest primitive types without geometry
 *              shaders, host index buffers (built-in and per-frame converted).
 *
 * docs/port/webgpu.md sections 4, 8 and 10 (WP3). WebGPU has no geometry,
 * tessellation or point-size support, so the shared PrimitiveProcessor runs with
 * InitializeCommon(full_32bit_indices=true, triangle_fans=false, line_loops=false,
 * quad_lists=false, point_sprites_without_vs_expansion=false,
 * rectangle_lists_without_vs_expansion=false):
 *  - point lists and rectangle lists are drawn as two-triangle strips from the
 *    built-in index buffer (HostVertexShaderType kPointListAsTriangleStrip /
 *    kRectangleListAsTriangleStrip): the VS pulls the guest index from the shared
 *    memory storage buffer (kSysFlag_ComputeOrPrimitiveVertexIndexLoad, endian
 *    swap in the shader) and expands the corner from the host vertex index;
 *  - quad lists, triangle fans and line loops become lists / strips, through the
 *    built-in index buffer (auto-indexed) or per-frame converted indices (DMA);
 *  - guest DMA indices are bound directly from the shared memory buffer and
 *    endian-swapped in the VS (vertex_index_endian); strips restart only at
 *    0xFFFF / 0xFFFFFFFF (GetStripIndexFormat for the pipeline), other reset
 *    indices take the shared conversion path.
 *
 * Converted indices go into per-frame CPU chunks, each mirrored by a GPU index
 * buffer. The CPU bytes reach the GPU with wgpuQueueWriteBuffer from an encoder
 * callback recorded once per stream (outside passes), which runs on the device
 * thread before that stream's queue submit, so every draw in the stream sees
 * them without breaking render passes per draw. A frame's chunks are reused
 * kFrameSlots frames later, after the submissions that read them were replayed.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

#include <webgpu/webgpu.h>

#include <rex/graphics/primitive_processor.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::webgpu {

class WebGPUCommandQueue;
class WebGPUCommandStream;
class WebGPUReplayState;

class WebGPUPrimitiveProcessor final : public PrimitiveProcessor {
 public:
  // Converted index chunk size. The largest single conversion is a 32-bit
  // triangle fan of 0xFFFF indices: 3 * 0xFFFD * 4 bytes (768 KiB).
  static constexpr uint32_t kChunkSize = 2 * 1024 * 1024;
  // Frames whose converted indices may be in flight (CP latency bound + 1).
  static constexpr uint32_t kFrameSlots = 4;

  // queue: the command processor's stream queue (device, tracker, stream).
  // end_render_pass: closes the pass open in the current stream (the CP's
  // EndRenderPass, which also forgets its open-pass key).
  // shared_memory_buffer: the WebGPU buffer mirroring `shared_memory` (bound as
  // the index buffer for guest DMA indices; needs the Index usage).
  WebGPUPrimitiveProcessor(const RegisterFile& register_file, memory::Memory& memory,
                           TraceWriter& trace_writer, SharedMemory& shared_memory,
                           WebGPUCommandQueue& queue, std::function<void()> end_render_pass,
                           WGPUBuffer shared_memory_buffer);
  ~WebGPUPrimitiveProcessor() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);

  // Call after the swap's latency wait: drops the per-frame conversion cache and
  // moves to the next frame slot.
  void EndFrame();

  // Index buffer for a processed draw. Returns false for non-indexed draws
  // (ProcessedIndexBufferType::kNone): use Draw(host_draw_vertex_count).
  struct IndexBufferBinding {
    WGPUBuffer buffer = nullptr;
    uint64_t offset = 0;
    uint64_t size = 0;
    WGPUIndexFormat format = WGPUIndexFormat_Undefined;
  };
  bool GetIndexBufferBinding(const ProcessingResult& result, IndexBufferBinding& out) const;

  // ---- For the pipeline cache (WP2) and the system constants. ----

  // Host topology of a processed draw (only the types Process can return).
  static WGPUPrimitiveTopology GetTopology(xenos::PrimitiveType host_primitive_type);
  // GPUPrimitiveState.stripIndexFormat: the host index format for every indexed
  // strip draw (WebGPU requires it there and then always restarts at 0xFFFF /
  // 0xFFFFFFFF), Undefined for lists and non-indexed draws.
  static WGPUIndexFormat GetStripIndexFormat(const ProcessingResult& result);
  // Workgroup counts for a HostVertexShaderType::kMemExportCompute dispatch of
  // `vertex_count` guest vertices (local size 1; the translator derives the
  // vertex from global_invocation_id with SpirvShaderTranslator::
  // kMemExportComputeDispatchX/Y = 65535). Returns false if there is nothing to do.
  static bool GetMemExportComputeDispatch(uint32_t vertex_count, uint32_t& groups_x,
                                          uint32_t& groups_y);

  // Index-related system constants (the Vulkan UpdateSystemConstantValues
  // subset): ORs the vertex index flags into `flags` (clear them first) and sets
  // vertex_index_load_address, vertex_index_reset, compute_memexport_vertex_count,
  // vertex_index_endian, line_loop_closing_index, vertex_base_index and
  // vertex_index_min/max. `Translator` is SpirvShaderTranslator (a template so
  // this header needs no SPIR-V headers). compute_memexport_vertex_count is the
  // guest vertex count for a draw with a kMemExportCompute dispatch, else 0.
  // Returns whether any value changed.
  template <typename Translator>
  static bool ApplyIndexSystemConstants(const RegisterFile& regs, const ProcessingResult& result,
                                        uint32_t compute_memexport_vertex_count, uint32_t& flags,
                                        typename Translator::SystemConstants& constants);

  struct Statistics {
    uint64_t converted_buffers = 0;
    uint64_t converted_bytes = 0;
    // Streams that carried converted indices (one write callback each), and
    // how many of those had to close an open render pass to record it.
    uint64_t upload_streams = 0;
    uint64_t upload_stream_pass_breaks = 0;
    uint32_t chunks = 0;
  };
  const Statistics& statistics() const { return statistics_; }

 protected:
  bool InitializeBuiltinIndexBuffer(size_t size_bytes,
                                    std::function<void(void*)> fill_callback) override;
  void* RequestHostConvertedIndexBufferForCurrentFrame(xenos::IndexFormat format,
                                                       uint32_t index_count, bool coalign_for_simd,
                                                       uint32_t coalignment_original_address,
                                                       size_t& backend_handle_out) override;

 private:
  struct Chunk {
    std::unique_ptr<uint8_t[]> data;
    WGPUBuffer buffer = nullptr;
    uint32_t size = 0;
    uint32_t used = 0;
  };
  // Converted-index bytes one stream must write before its submit.
  struct StreamWrites {
    uint64_t submission = 0;
    struct Write {
      WGPUBuffer buffer;
      const uint8_t* data;
      uint32_t offset;
      uint32_t size;
    };
    std::vector<Write> writes;
  };

  Chunk* AllocateChunk(uint32_t min_size);
  StreamWrites* GetCurrentStreamWrites();
  // Device thread (EncoderCallback of the stream).
  static void WriteStreamIndices(void* user, WebGPUReplayState& state, WGPUCommandEncoder encoder);

  WebGPUCommandQueue& queue_;
  std::function<void()> end_render_pass_;
  WGPUBuffer shared_memory_buffer_;

  WGPUBuffer builtin_index_buffer_ = nullptr;
  uint64_t builtin_index_buffer_size_ = 0;

  // Per frame slot: chunks (reused when the slot comes around) and the last
  // submission that may read them.
  std::array<std::vector<std::unique_ptr<Chunk>>, kFrameSlots> frame_chunks_;
  std::array<uint64_t, kFrameSlots> frame_last_submission_ = {};
  uint32_t frame_slot_ = 0;
  // Chunk of the current slot being filled.
  size_t frame_chunk_ = 0;
  // Indexed by the backend handles, cleared at EndFrame.
  std::deque<std::pair<WGPUBuffer, uint64_t>> frame_index_buffers_;

  // Recorded but possibly not yet replayed streams (front = oldest).
  std::deque<std::unique_ptr<StreamWrites>> stream_writes_;
  std::vector<std::unique_ptr<StreamWrites>> stream_writes_free_;

  Statistics statistics_;
};

template <typename Translator>
bool WebGPUPrimitiveProcessor::ApplyIndexSystemConstants(
    const RegisterFile& regs, const ProcessingResult& result,
    uint32_t compute_memexport_vertex_count, uint32_t& flags,
    typename Translator::SystemConstants& constants) {
  auto vgt_dma_size = regs.Get<reg::VGT_DMA_SIZE>();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  bool dirty = false;

  // WebGPU indices are always full 32-bit: kSysFlag_VertexIndexLoad is never
  // needed. Point / rectangle expansion and the memexport compute shader load
  // the guest index from the shared memory buffer themselves.
  if (result.index_buffer_type == ProcessedIndexBufferType::kHostBuiltinForDMA ||
      result.index_buffer_type == ProcessedIndexBufferType::kGuestDMA ||
      result.index_buffer_type == ProcessedIndexBufferType::kHostConverted) {
    flags |= Translator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad;
    if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32) {
      flags |= Translator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit;
    }
    // Point and rectangle expansion restart only in the built-in strip index
    // buffer; the guest indices are not reset-based there.
    if (result.host_primitive_reset_enabled &&
        result.host_vertex_shader_type != Shader::HostVertexShaderType::kPointListAsTriangleStrip &&
        result.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) {
      flags |= Translator::kSysFlag_ComputeOrPrimitiveVertexIndexReset;
    }
  }

  if (flags & (Translator::kSysFlag_VertexIndexLoad |
               Translator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad)) {
    dirty |= constants.vertex_index_load_address != result.guest_index_base;
    constants.vertex_index_load_address = result.guest_index_base;
  }
  uint32_t vertex_index_reset = (flags & Translator::kSysFlag_ComputeOrPrimitiveVertexIndexReset)
                                    ? regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx
                                    : 0;
  dirty |= constants.vertex_index_reset != vertex_index_reset;
  constants.vertex_index_reset = vertex_index_reset;
  dirty |= constants.compute_memexport_vertex_count != compute_memexport_vertex_count;
  constants.compute_memexport_vertex_count = compute_memexport_vertex_count;

  // The same normalization as the primitive processor (8-in-32 / 16-in-32 on
  // 16-bit indices).
  xenos::Endian guest_index_endian = vgt_dma_size.swap_mode;
  if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16 &&
      guest_index_endian != xenos::Endian::kNone && guest_index_endian != xenos::Endian::k8in16) {
    guest_index_endian =
        guest_index_endian == xenos::Endian::k8in32 ? xenos::Endian::k8in16 : xenos::Endian::kNone;
  }
  // With full 32-bit indices the host index buffer keeps the guest endianness
  // in every path (host_shader_index_endian == guest), so the kVertex VS (host
  // index) and the expansion / compute shaders (guest index) agree.
  xenos::Endian index_endian = (flags & Translator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad)
                                   ? guest_index_endian
                                   : result.host_shader_index_endian;
  dirty |= constants.vertex_index_endian != index_endian;
  constants.vertex_index_endian = index_endian;

  dirty |= constants.line_loop_closing_index != result.line_loop_closing_index;
  constants.line_loop_closing_index = result.line_loop_closing_index;

  int32_t vgt_indx_offset = regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);
  dirty |= constants.vertex_base_index != vgt_indx_offset;
  constants.vertex_base_index = vgt_indx_offset;

  uint32_t vgt_min_vtx_indx = regs.Get<uint32_t>(XE_GPU_REG_VGT_MIN_VTX_INDX);
  uint32_t vgt_max_vtx_indx = regs.Get<uint32_t>(XE_GPU_REG_VGT_MAX_VTX_INDX);
  dirty |= constants.vertex_index_min != vgt_min_vtx_indx;
  dirty |= constants.vertex_index_max != vgt_max_vtx_indx;
  constants.vertex_index_min = vgt_min_vtx_indx;
  constants.vertex_index_max = vgt_max_vtx_indx;
  return dirty;
}

}  // namespace rex::graphics::webgpu
