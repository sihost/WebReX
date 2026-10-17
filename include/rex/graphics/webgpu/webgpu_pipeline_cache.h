/**
 * @file        rex/graphics/webgpu/webgpu_pipeline_cache.h
 * @brief       WebGPU pipeline cache (WP2): Xenos ucode -> SPIR-V -> WGSL -> render pipelines.
 *
 * Per draw (CP thread), ConfigurePipeline turns the draw state into a
 * WebGPUPipelineDescription and GetOrCreate returns its id in the command
 * queue's WebGPUPipelineTable:
 *   - Shaders: SpirvShaderTranslator with WebGPU-class features
 *     (docs/port/webgpu.md section 3), then SpirvToWgsl (Tint, libtint_web.a).
 *     The WGSL of every (ucode hash, modification) is kept in memory and in the
 *     shader storage: <storage>/<tint revision>-t<translator version>/
 *     <ucode hash>_<modification>.<vs|ps>.wgsl, on OPFS on the web (WasmFS,
 *     rex::filesystem::web). A storage hit skips both the SPIR-V translator and
 *     Tint.
 *   - Pipelines: createShaderModule + createRenderPipelineAsync on the device
 *     thread (RunAsync), layout "auto"; the callback stores the pipeline in the
 *     table. Draws recorded before it is ready are skipped by the replay, the
 *     CP never waits.
 * Translation runs on the CP thread (never on the device thread).
 *
 * Without libtint_web.a (REX_WEBGPU_HAS_TINT unset) nothing is converted and
 * ConfigurePipeline fails (draws dropped), as the WP1 stub did.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/webgpu/webgpu_command_stream.h>
#include <rex/graphics/xenos.h>
#include <rex/string/buffer.h>

namespace rex::graphics {
class SpirvShaderTranslator;
}  // namespace rex::graphics

namespace rex::graphics::webgpu {

class WebGPUCommandProcessor;

// Everything the pipeline for one draw depends on, gathered by the command
// processor (the analog of the Vulkan ConfigurePipeline arguments).
struct WebGPUDrawPipelineState {
  Shader* vertex_shader = nullptr;
  // Null if the pixel shader is not needed (depth-only, or no rasterization).
  Shader* pixel_shader = nullptr;
  xenos::PrimitiveType guest_primitive_type = xenos::PrimitiveType::kNone;
  // From PrimitiveProcessor::ProcessingResult (WP3). kNone: derived from the
  // guest primitive type (points / rectangles as VS-expanded strips).
  xenos::PrimitiveType host_primitive_type = xenos::PrimitiveType::kNone;
  Shader::HostVertexShaderType host_vertex_shader_type = Shader::HostVertexShaderType::kVertex;
  bool host_vertex_shader_type_valid = false;
  reg::RB_DEPTHCONTROL normalized_depth_control = {};
  uint32_t normalized_color_mask = 0;
  // Interpolators written by the VS and read by the PS.
  uint32_t interpolator_mask = 0;
  // PsParamGen interpolator (Shader::GetInterpolatorInputMask), UINT32_MAX if none.
  uint32_t ps_param_gen_pos = UINT32_MAX;
  // From WebGPURenderTargetCache::last_update_render_pass_key().
  uint64_t render_pass_key = 0;
  // WebGPURenderTargetCache::GetDepthTextureFormat of the pass depth format.
  WGPUTextureFormat depth_texture_format = WGPUTextureFormat_Undefined;
};

// Hashed and compared with memcmp: no implicit padding, zero-initialized.
struct WebGPUPipelineDescription {
  uint64_t vertex_shader_hash;
  uint64_t vertex_shader_modification;
  // 0 with pixel_shader_modification 0: no guest pixel shader (a depth-only
  // fragment stage is used if the pass needs one).
  uint64_t pixel_shader_hash;
  uint64_t pixel_shader_modification;
  uint32_t render_pass_format_key;  // RenderPassFormatKey
  uint32_t depth_texture_format;    // WGPUTextureFormat
  // Per color target: RB_BLENDCONTROL (0 if blending is off) and write mask.
  uint32_t blend_control[4];
  uint32_t color_write_masks;  // 4 bits per target
  uint32_t depth_control;      // normalized RB_DEPTHCONTROL
  uint8_t stencil_read_mask;
  uint8_t stencil_write_mask;
  uint8_t stencil_read_mask_back;
  uint8_t stencil_write_mask_back;
  uint8_t primitive_topology;  // WGPUPrimitiveTopology
  uint8_t cull_mode;           // WGPUCullMode
  uint8_t front_face_cw;
  uint8_t alpha_to_coverage;
  uint8_t sample_count;
  uint8_t pad[7];
};
static_assert(sizeof(WebGPUPipelineDescription) == 80);

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

  // CP thread. Id of the pipeline for `description` (created asynchronously on
  // the first request), or kInvalidPipelineId if its shaders can't be
  // translated / converted (remembered, not retried).
  PipelineId GetOrCreate(const WebGPUPipelineDescription& description);

  uint64_t shader_count() const { return shaders_.size(); }

  // CP thread.
  struct Statistics {
    uint64_t wgsl_memory_hits = 0;
    uint64_t wgsl_storage_hits = 0;
    uint64_t wgsl_translated = 0;
    uint64_t wgsl_failures = 0;
    uint64_t pipelines_requested = 0;
  };
  const Statistics& statistics() const { return statistics_; }
  // Written by createRenderPipelineAsync callbacks (device thread); shared so a
  // late callback never touches a destroyed cache.
  struct AsyncCounters {
    std::atomic<uint64_t> pipelines_created{0};
    std::atomic<uint64_t> pipelines_failed{0};
  };
  uint64_t pipelines_created() const { return async_counters_->pipelines_created.load(); }
  uint64_t pipelines_failed() const { return async_counters_->pipelines_failed.load(); }

 private:
  struct DescriptionHasher {
    size_t operator()(const WebGPUPipelineDescription& d) const;
  };
  struct DescriptionEqual {
    bool operator()(const WebGPUPipelineDescription& a, const WebGPUPipelineDescription& b) const;
  };
  struct WgslKey {
    uint64_t hash;
    uint64_t modification;
    bool operator==(const WgslKey& o) const {
      return hash == o.hash && modification == o.modification;
    }
  };
  struct WgslKeyHasher {
    size_t operator()(const WgslKey& k) const;
  };

  // WGSL for a shader modification (memory, then storage, then translation).
  // Null on failure. The returned string is immutable and shared with the
  // device thread.
  std::shared_ptr<const std::string> GetWgsl(Shader& shader, uint64_t modification);
  std::shared_ptr<const std::string> GetDepthOnlyFragmentWgsl(uint32_t depth_stencil_mode);
  std::filesystem::path StoragePath(uint64_t hash, uint64_t modification, bool vertex) const;

  WebGPUCommandProcessor& command_processor_;
  const RegisterFile& register_file_;
  string::StringBuffer ucode_disasm_buffer_;
  std::unordered_map<uint64_t, std::unique_ptr<Shader>> shaders_;

  std::unique_ptr<SpirvShaderTranslator> translator_;
  uint32_t max_storage_buffer_range_ = 128 * 1024 * 1024;
  bool clip_distances_ = false;

  std::filesystem::path storage_dir_;
  std::unordered_map<WgslKey, std::shared_ptr<const std::string>, WgslKeyHasher> wgsl_;
  std::unordered_map<uint32_t, std::shared_ptr<const std::string>> depth_only_wgsl_;
  std::unordered_map<WebGPUPipelineDescription, PipelineId, DescriptionHasher, DescriptionEqual>
      pipelines_;
  Statistics statistics_;
  std::shared_ptr<AsyncCounters> async_counters_ = std::make_shared<AsyncCounters>();
};

}  // namespace rex::graphics::webgpu
