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
 *     thread (RunAsync); the callback stores the pipeline in the table. Draws
 *     recorded before it is ready are skipped by the replay, the CP never waits.
 *   - Layout (explicit, shared by every guest pipeline; the translator's
 *     descriptor sets, SpirvShaderTranslator::DescriptorSet):
 *       group 0  shared memory, read-only storage, bindings 0...N-1
 *                (WebGPUSharedMemory::binding_count()), vertex + fragment;
 *       group 1  constants, 5 uniform buffers with dynamic offsets (system,
 *                VS floats, PS floats, bool/loop, fetch), vertex + fragment;
 *       group 2  VS textures (bindings 0...T-1) then samplers (T...), vertex;
 *       group 3  PS textures and samplers, fragment.
 *     Groups 2 and 3 have one layout per distinct texture dimension list +
 *     sampler count (WebGPUShaderBindings::layout), the empty layout without
 *     textures. Textures are float-sampled, samplers filtering.
 * The SPIR-V translation runs on the CP thread (never on the device thread),
 * the SPIR-V -> WGSL conversion (Tint, ~0.1-0.3 s per shader in the browser)
 * on --webgpu_wgsl_threads conversion threads: a pipeline whose shaders are
 * still converting gets its id at once and is created by the thread finishing
 * the last of them, so its draws are skipped meanwhile, as while
 * createRenderPipelineAsync runs. Converting on the CP thread stalled it for
 * ~20 s on a first map load (gates/README.md gate 5), so no frame was presented.
 *
 * Without libtint_web.a (REX_WEBGPU_HAS_TINT unset) nothing is converted and
 * ConfigurePipeline fails (draws dropped), as the WP1 stub did.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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
  // Domain host vertex shader types: whether WebGPUTessellator provides the
  // domain points (else the tessellated draw is refused).
  bool tessellation_available = false;
  // Index format of an indexed draw (WebGPU strip pipelines must match it),
  // Undefined for non-indexed draws.
  WGPUIndexFormat index_format = WGPUIndexFormat_Undefined;
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

// Texture and sampler bindings of a translated shader (from SpirvShader after
// the SPIR-V translation of any modification) and the layout of its bind group
// (group 2 for vertex, 3 for pixel shaders).
struct WebGPUShaderBindings {
  struct Texture {
    uint32_t fetch_constant;
    xenos::FetchOpDimension dimension;
    bool is_signed;
  };
  // Field names as WebGPUTextureCache::GetSamplerParameters expects.
  struct Sampler {
    uint32_t fetch_constant;
    xenos::TextureFilter mag_filter;
    xenos::TextureFilter min_filter;
    xenos::TextureFilter mip_filter;
    xenos::AnisoFilter aniso_filter;
  };
  // Binding i, then samplers at textures.size() + j.
  std::vector<Texture> textures;
  std::vector<Sampler> samplers;
  // Never null (the empty layout if there are no textures and samplers).
  WGPUBindGroupLayout layout = nullptr;
};

// Resources of the draw's pipeline, for the command processor's bind groups.
struct WebGPUDrawBindings {
  const WebGPUShaderBindings* vertex = nullptr;
  // Null if the pipeline has no guest pixel shader (bind the empty group).
  const WebGPUShaderBindings* pixel = nullptr;
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
  uint8_t strip_index_format;  // WGPUIndexFormat, strips only
  uint8_t pad[6];
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
  bool ConfigurePipeline(const WebGPUDrawPipelineState& state, PipelineId& pipeline_out,
                         WebGPUDrawBindings& bindings_out);

  // Layouts shared by all guest pipelines (group 0, group 1, and the empty
  // layout of groups 2 / 3 for shaders without textures). Device-thread
  // objects, valid between Initialize and Shutdown.
  WGPUBindGroupLayout shared_memory_layout() const { return shared_memory_layout_; }
  WGPUBindGroupLayout constants_layout() const { return constants_layout_; }
  WGPUBindGroupLayout empty_layout() const { return empty_layout_; }
  // Group 0 of the vertex memexport compute pipelines: the shared memory
  // bindings read_write (compute only). Groups 1 and 2 are the constants and
  // the VS texture layouts (visible to compute too).
  WGPUBindGroupLayout compute_shared_memory_layout() const { return compute_shared_memory_layout_; }

  // CP thread. The vertex shader as a HostVertexShaderType::kMemExportCompute
  // compute pipeline (one invocation per guest vertex, eM# exports written to
  // the shared memory; webgpu.md section 4), created asynchronously (the
  // dispatch is skipped by the replay until it exists). bindings_out: the VS
  // texture/sampler bindings (group 2). kInvalidPipelineId if it can't be made.
  PipelineId GetMemExportComputePipeline(Shader& vertex_shader,
                                         const WebGPUShaderBindings*& bindings_out);
  // Binding sizes of the group 1 uniform buffers (fixed, the dynamic offset
  // selects the data; the stream keeps 64 KiB of slack after its constants).
  static constexpr uint32_t kFloatConstantsBindingSize = 256 * 4 * sizeof(float);
  static constexpr uint32_t kBoolLoopConstantsBindingSize = (8 + 32) * sizeof(uint32_t);
  static constexpr uint32_t kFetchConstantsBindingSize = 32 * 6 * sizeof(uint32_t);
  // sizeof(SpirvShaderTranslator::SystemConstants), checked in the .cpp.
  static uint32_t system_constants_binding_size();

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
    // Draws refused because of a resource limit or an unsupported shader
    // (pixel shader memexport: a writable shared memory binding in a pass that
    // also reads it). Sum of the draws_refused_* counters below.
    uint64_t draws_unsupported = 0;
    // ConfigurePipeline refusals by reason.
    uint64_t draws_refused_ps_memexport = 0;
    // Shader texture/sampler bindings over the device limits (or failed layout).
    uint64_t draws_refused_bindings = 0;
    // Tessellated (domain) host vertex shaders.
    uint64_t draws_refused_tessellation = 0;
    // Front and back faces culled (WebGPU has no cull-both mode); nothing drawn.
    uint64_t draws_refused_cull_both = 0;
    // Shader translation failed (cached kInvalidPipelineId) or the
    // webgpu_draw_with_pipelines diagnostic is off.
    uint64_t draws_refused_no_pipeline = 0;
  };
  const Statistics& statistics() const {
    statistics_.wgsl_translated = async_counters_->wgsl_translated.load(std::memory_order_relaxed);
    statistics_.wgsl_failures = async_counters_->wgsl_failures.load(std::memory_order_relaxed);
    return statistics_;
  }
  // Written by createRenderPipelineAsync callbacks (device thread) and the WGSL
  // conversion threads; shared so a late callback never touches a destroyed cache.
  struct AsyncCounters {
    std::atomic<uint64_t> pipelines_created{0};
    std::atomic<uint64_t> pipelines_failed{0};
    std::atomic<uint64_t> wgsl_translated{0};
    std::atomic<uint64_t> wgsl_failures{0};
    // Shaders queued for or in conversion.
    std::atomic<uint64_t> wgsl_converting{0};
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

  // WGSL of one (ucode hash, modification). Ready with null text = failed.
  // Guarded by wgsl_mutex_; the text never changes once ready.
  struct DeferredPipeline;
  struct WgslSlot {
    bool ready = false;
    std::shared_ptr<const std::string> text;
    // Render pipelines waiting for this slot (while not ready).
    std::vector<std::shared_ptr<DeferredPipeline>> waiters;
  };
  // A render pipeline whose shaders were still converting when requested.
  struct DeferredPipeline {
    PipelineId id;
    WebGPUPipelineDescription description;
    std::string label;
    std::array<WGPUBindGroupLayout, 4> layouts;
    std::shared_ptr<WgslSlot> vs;
    // Null with ps_text set (the depth-only fragment stage) or without a PS.
    std::shared_ptr<WgslSlot> ps;
    std::shared_ptr<const std::string> ps_text;
    uint32_t pending = 0;  // slots not ready yet
  };
  struct ConversionJob {
    std::shared_ptr<WgslSlot> slot;
    std::vector<uint8_t> spirv;
    std::filesystem::path storage_path;
    uint64_t hash = 0;
    uint64_t modification = 0;
    bool vertex = false;
  };

  // CP thread. The WGSL slot of a shader modification: from memory, from the
  // storage, or translated to SPIR-V here and converted on a conversion thread
  // (pending; converted here with --webgpu_wgsl_threads=0).
  std::shared_ptr<WgslSlot> RequestWgsl(Shader& shader, uint64_t modification);
  // CP thread. RequestWgsl, waiting for a pending conversion. Null on failure.
  // The returned string is immutable and shared with the device thread.
  std::shared_ptr<const std::string> GetWgsl(Shader& shader, uint64_t modification);
  // Any thread: Tint, then the storage file. Null on failure.
  std::shared_ptr<const std::string> ConvertAndStore(const std::vector<uint8_t>& spirv,
                                                     const std::filesystem::path& storage_path,
                                                     uint64_t hash, uint64_t modification,
                                                     bool vertex);
  // Any thread: marks the slot ready and creates the pipelines it completes.
  void CompleteSlot(const std::shared_ptr<WgslSlot>& slot,
                    std::shared_ptr<const std::string> text);
  void ConversionThreadMain();
  void StopConversionThreads();
  // Any thread: createShaderModule + createRenderPipelineAsync on the device
  // thread; the callback stores the pipeline in the table under `id`.
  void CreateRenderPipelineAsync(PipelineId id, const WebGPUPipelineDescription& d,
                                 std::shared_ptr<const std::string> vs_wgsl,
                                 std::shared_ptr<const std::string> ps_wgsl, std::string label,
                                 const std::array<WGPUBindGroupLayout, 4>& layouts);
  std::shared_ptr<const std::string> GetDepthOnlyFragmentWgsl(uint32_t depth_stencil_mode);
  std::filesystem::path StoragePath(uint64_t hash, uint64_t modification, bool vertex) const;
  // Bindings of `shader` (translating `modification` if no modification of it
  // is translated yet, as on a WGSL storage hit). Null if the shader can't be
  // bound (translation failure, over the per-stage texture / sampler limits).
  const WebGPUShaderBindings* GetShaderBindings(Shader& shader, uint64_t modification);
  WGPUBindGroupLayout GetTextureLayout(bool vertex, const WebGPUShaderBindings& bindings);

  WebGPUCommandProcessor& command_processor_;
  const RegisterFile& register_file_;
  string::StringBuffer ucode_disasm_buffer_;
  std::unordered_map<uint64_t, std::unique_ptr<Shader>> shaders_;

  std::unique_ptr<SpirvShaderTranslator> translator_;
  uint32_t max_storage_buffer_range_ = 128 * 1024 * 1024;
  bool clip_distances_ = false;

  bool storage_initialized_ = false;
  std::filesystem::path storage_dir_;
  // wgsl_, the slots, conversion_jobs_ and conversion_stop_.
  std::mutex wgsl_mutex_;
  std::condition_variable wgsl_jobs_cv_;
  std::condition_variable wgsl_ready_cv_;
  std::unordered_map<WgslKey, std::shared_ptr<WgslSlot>, WgslKeyHasher> wgsl_;
  std::deque<ConversionJob> conversion_jobs_;
  std::vector<std::thread> conversion_threads_;
  bool conversion_stop_ = false;
  std::unordered_map<uint32_t, std::shared_ptr<const std::string>> depth_only_wgsl_;
  std::unordered_map<WebGPUPipelineDescription, PipelineId, DescriptionHasher, DescriptionEqual>
      pipelines_;
  // Vertex memexport compute pipelines by (ucode hash, modification).
  std::unordered_map<WgslKey, PipelineId, WgslKeyHasher> compute_pipelines_;
  WGPUBindGroupLayout compute_shared_memory_layout_ = nullptr;

  WGPUBindGroupLayout shared_memory_layout_ = nullptr;
  WGPUBindGroupLayout constants_layout_ = nullptr;
  WGPUBindGroupLayout empty_layout_ = nullptr;
  // Texture group layouts by stage + dimension list + sampler count (kept
  // until Shutdown).
  std::unordered_map<std::string, WGPUBindGroupLayout> texture_layouts_;
  // By ucode hash; null = the shader can't be bound.
  std::unordered_map<uint64_t, std::unique_ptr<WebGPUShaderBindings>> shader_bindings_;
  mutable Statistics statistics_;
  std::shared_ptr<AsyncCounters> async_counters_ = std::make_shared<AsyncCounters>();
};

}  // namespace rex::graphics::webgpu
