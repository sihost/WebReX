/**
 * @file        rex/graphics/webgpu/webgpu_tessellator.h
 * @brief       Xenos tessellation on WebGPU (no hull / domain stages): a compute
 *              pre-pass expands the draw's patches into domain points and a
 *              triangle list, which the guest vertex shader then draws as a
 *              vertex shader (SpirvShaderTranslator::Features::tessellation_shader
 *              false: the domain inputs are vertex attributes).
 *
 * What the D3D12 backend does with hull shaders and the fixed-function
 * tessellator (docs/port/webgpu.md section 4):
 *  - factors: discrete / continuous use VGT_HOS_MAX_TESS_LEVEL + 1 for every
 *    edge and the inside (D3D12 *_hs); adaptive reads one float32 edge factor
 *    per control point from the index buffer (endian-swapped, + 1, clamped to
 *    [VGT_HOS_MIN_TESS_LEVEL + 1, VGT_HOS_MAX_TESS_LEVEL + 1]), triangle edges
 *    U=cp1 V=cp2 W=cp0 inside=min, quad edges U0=cp3 V0=cp0 U1=cp1 V1=cp2,
 *    inside U=min(cp0, cp2) V=min(cp1, cp3) (tessellation_adaptive_vs +
 *    adaptive_*_hs);
 *  - partitioning: integer for discrete, fractional_even otherwise, output
 *    triangle_cw;
 *  - the patch input: control point indices (CP-indexed) or the patch index
 *    (patch-indexed: the index of the patch's single control point, or for
 *    adaptive the primitive id), each + VGT_INDX_OFFSET, masked to 24 bits,
 *    clamped to VGT_MIN/MAX_VTX_INDX, as floats.
 * The point placement and connectivity are a WGSL port of the Direct3D 11
 * reference tessellator (Microsoft, MIT; as in Mesa's
 * src/gallium/auxiliary/tessellator), which hardware tessellators reproduce
 * exactly (16.16 fixed point).
 *
 * Each patch gets a fixed slot of points and indices sized for the draw's
 * largest factor; unused index slots repeat the patch's first point
 * (degenerate triangles), so the draw is a plain drawIndexed and keeps the
 * patch order of the hardware.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <vector>

#include <webgpu/webgpu.h>

#include <rex/graphics/primitive_processor.h>
#include <rex/graphics/register_file.h>

namespace rex::graphics::webgpu {

class WebGPUCommandProcessor;
class WebGPUReplayState;

class WebGPUTessellator {
 public:
  // Vertex buffer stride: float4 domain location, float4 patch input.
  static constexpr uint32_t kPointStride = 32;

  WebGPUTessellator(WebGPUCommandProcessor& command_processor, const RegisterFile& register_file);
  ~WebGPUTessellator();

  // CP thread.
  bool Initialize();
  void Shutdown();
  // The pipeline exists and webgpu_tessellation is on.
  bool available() const;

  struct Output {
    WGPUBuffer vertex_buffer = nullptr;
    uint64_t vertex_buffer_size = 0;
    WGPUBuffer index_buffer = nullptr;  // uint32
    uint64_t index_buffer_size = 0;
    uint32_t index_count = 0;
  };
  // CP thread. For a processed draw with a domain host vertex shader type:
  // records the compute pass (ending the open render pass) that writes the
  // domain points and indices. False if the draw can't be tessellated (counted
  // in the statistics, the draw is dropped).
  bool RecordTessellation(const PrimitiveProcessor::ProcessingResult& result, Output& output);

  // Points and indices of one patch for the given partitioning and factors
  // (the counts of the reference tessellator), for slot sizes.
  static void GetPatchCounts(bool quad, bool integer_partitioning, float factor,
                             uint32_t& points_out, uint32_t& indices_out);

  struct Statistics {
    uint64_t draws = 0;
    uint64_t patches = 0;
    uint64_t indices = 0;
    // Refused: index buffer kind the pre-pass can't read, over the buffer
    // limits, no patches.
    uint64_t refused_index_buffer = 0;
    uint64_t refused_size = 0;
    uint32_t max_slot_points = 0;
    uint32_t max_slot_indices = 0;
  };
  const Statistics& statistics() const { return statistics_; }

 private:
  bool EnsureBuffers(uint64_t points_size, uint64_t indices_size);
  // Device thread, after the stream that last used them was submitted.
  static void ReleaseBuffers(void* user, WebGPUReplayState& state);

  WebGPUCommandProcessor& command_processor_;
  const RegisterFile& register_file_;
  WGPUBindGroupLayout layout_ = nullptr;
  WGPUComputePipeline pipeline_ = nullptr;
  WGPUBuffer points_buffer_ = nullptr;
  uint64_t points_buffer_size_ = 0;
  WGPUBuffer index_buffer_ = nullptr;
  uint64_t index_buffer_size_ = 0;
  uint64_t max_binding_size_ = 0;
  uint32_t storage_offset_alignment_ = 256;
  Statistics statistics_;
  uint32_t draws_logged_ = 0;
};

}  // namespace rex::graphics::webgpu
