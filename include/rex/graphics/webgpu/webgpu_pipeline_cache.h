/**
 * @file        rex/graphics/webgpu/webgpu_pipeline_cache.h
 * @brief       WebGPU pipeline cache: interface used by the command processor.
 *
 * STUB (WP1). Shaders are loaded and their ucode analyzed (so the draw path can
 * request vertex buffers), but nothing is translated and ConfigurePipeline
 * always fails, so draws are dropped (counted). WP2 replaces the implementation
 * (SPIR-V translator -> Tint -> WGSL, createRenderPipelineAsync into the
 * stream's WebGPUPipelineTable, docs/port/webgpu.md section 3) and may extend
 * WebGPUDrawPipelineState; the calls the command processor makes are the ones
 * declared here.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <unordered_map>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/webgpu/webgpu_command_stream.h>
#include <rex/graphics/xenos.h>
#include <rex/string/buffer.h>

namespace rex::graphics::webgpu {

class WebGPUCommandProcessor;

// Everything the pipeline for one draw depends on, gathered by the command
// processor (the analog of the Vulkan ConfigurePipeline arguments).
struct WebGPUDrawPipelineState {
  Shader* vertex_shader = nullptr;
  // Null if the pixel shader is not needed (depth-only, or no rasterization).
  Shader* pixel_shader = nullptr;
  xenos::PrimitiveType guest_primitive_type = xenos::PrimitiveType::kNone;
  reg::RB_DEPTHCONTROL normalized_depth_control = {};
  uint32_t normalized_color_mask = 0;
  // Interpolators written by the VS and read by the PS.
  uint32_t interpolator_mask = 0;
  // From WebGPURenderTargetCache::last_update_render_pass_key().
  uint64_t render_pass_key = 0;
};

class WebGPUPipelineCache {
 public:
  WebGPUPipelineCache(WebGPUCommandProcessor& command_processor, const RegisterFile& register_file);
  ~WebGPUPipelineCache();

  bool Initialize();
  void Shutdown();
  void ClearCache();
  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking);

  // Returns the (cached by ucode hash) shader object; never null.
  Shader* LoadShader(xenos::ShaderType shader_type, const uint32_t* host_address,
                     uint32_t dword_count);
  void AnalyzeShaderUcode(Shader& shader) {
    if (!shader.is_ucode_analyzed()) {
      shader.AnalyzeUcode(ucode_disasm_buffer_);
    }
  }

  // Pipeline for the current draw, as an id in the stream pipeline table
  // (WebGPUCommandQueue::pipeline_table()). The pipeline may still be compiling
  // when the stream is replayed (the draw is then skipped by the replay).
  // Returns false if no pipeline can be provided; the draw is dropped.
  bool ConfigurePipeline(const WebGPUDrawPipelineState& state, PipelineId& pipeline_out);

  uint64_t shader_count() const { return shaders_.size(); }

 private:
  WebGPUCommandProcessor& command_processor_;
  const RegisterFile& register_file_;
  string::StringBuffer ucode_disasm_buffer_;
  std::unordered_map<uint64_t, std::unique_ptr<Shader>> shaders_;
};

}  // namespace rex::graphics::webgpu
