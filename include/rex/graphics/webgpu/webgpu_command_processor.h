/**
 * @file        rex/graphics/webgpu/webgpu_command_processor.h
 * @brief       WebGPU command processor: PM4 on the CP thread, GPU work recorded into
 *              WebGPUCommandStreams and replayed on the WebGPU device thread.
 *
 * docs/port/webgpu.md sections 1, 2 and 10 (WP1). The shared CommandProcessor
 * executes PM4 (registers, fences, interrupts, swaps); this class turns draws,
 * resolves and swaps into stream commands:
 *  - IssueDraw: shader analysis, vertex/index buffer residency in the shared
 *    memory (uploads consume the web dirty map), textures (WP5), render
 *    targets (WP4), the pipeline (WP2), then the bind groups of the explicit
 *    pipeline layout (WebGPUPipelineCache): shared memory, the 5 constant
 *    buffers (system constants, VS/PS floats, bool/loop, fetch; written into
 *    the stream constant arena when they changed, bound with dynamic offsets),
 *    VS and PS textures + samplers; viewport, scissor, blend constant and
 *    stencil reference. State already set in the open pass is not repeated.
 *  - IssueCopy: render target cache Resolve (WP4); the color clear value is
 *    remembered for the stub swap fill.
 *  - IssueSwap: refreshes the presenter's guest output texture through the
 *    WebGPUGuestOutputRefreshContext contract (webgpu_guest_output.h): a
 *    clear + blit of the swap texture (WP5) recorded into the stream,
 *    submitted, replayed before the refresher returns. Frame latency is bounded
 *    by kMaxFramesInFlight completed submissions.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>

#include <webgpu/webgpu.h>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/primitive_processor.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/webgpu/webgpu_command_stream.h>
#include <rex/graphics/webgpu/webgpu_submission_tracker.h>

namespace rex::graphics::webgpu {

class WebGPUDevice;
class WebGPUGraphicsSystem;
class WebGPUPipelineCache;
class WebGPUPresentPass;
class WebGPUPrimitiveProcessor;
class WebGPURenderTargetCache;
class WebGPUSharedMemory;
class WebGPUTessellator;
class WebGPUTextureCache;

class WebGPUCommandProcessor : public CommandProcessor {
 public:
  // Swaps whose GPU work may be outstanding before IssueSwap waits.
  static constexpr uint32_t kMaxFramesInFlight = 3;
  // An open stream is submitted once its upload arena passes this (between
  // uploads, outside passes), bounding the CPU copy of in-flight data.
  static constexpr size_t kStreamUploadFlushBytes = 32 * 1024 * 1024;

  WebGPUCommandProcessor(WebGPUGraphicsSystem* graphics_system, system::KernelState* kernel_state);
  ~WebGPUCommandProcessor() override;

  void ClearCaches() override;
  void InvalidateGpuMemory() override;
  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking) override;
  void TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) override;
  void RestoreEdramSnapshot(const void* snapshot) override;

  // ---- For the caches (CP thread). ----

  WebGPUDevice& device() const { return device_; }
  WebGPUCommandQueue& command_queue() const { return *command_queue_; }
  WebGPUCommandStream& command_stream() const { return command_queue_->stream(); }
  WebGPUSubmissionTracker& submission_tracker() { return submission_tracker_; }
  WebGPUSharedMemory* shared_memory() const { return shared_memory_.get(); }
  // WP3: host index buffers and primitive type conversion.
  WebGPUPrimitiveProcessor* primitive_processor() const { return primitive_processor_.get(); }

  // Id the next Submit() will get, and the last one the GPU finished.
  uint64_t GetCurrentSubmission() const { return submission_tracker_.submitted() + 1; }
  uint64_t GetCompletedSubmission() const { return submission_tracker_.completed(); }

  // Closes the render pass (or compute pass) open in the current stream, so
  // copies and uploads can be recorded.
  void EndRenderPass();
  // Opens `desc` unless the pass with the same nonzero key is already open in
  // the current stream.
  void BeginRenderPass(const WebGPUCommandStream::RenderPassDesc& desc, uint64_t key);
  // Records an upload into the current stream (ending any open pass, and
  // submitting the stream first if its upload arena is over
  // kStreamUploadFlushBytes). Returns `size` writable bytes or null.
  void* RecordUpload(WGPUBuffer destination, uint64_t destination_offset, uint64_t size);
  // Submits the current stream; optionally waits until the device thread has
  // replayed it. Returns the submission id.
  uint64_t Submit(bool wait_for_replay = false);

  struct Statistics {
    uint64_t swaps = 0;
    uint64_t draws = 0;
    uint64_t draws_dropped = 0;
    // draws_dropped by reason: RenderTargetCache::Update failed (of which
    // WebGPURenderTargetCache::Statistics::updates_without_attachments minus
    // draws_no_output and draws_dummy_attachment had no attachment at all and
    // the dummy one couldn't be made), ConfigurePipeline refused (WebGPUPipelineCache
    // draws_refused_*), the constant arena was full, a sampler couldn't be made.
    uint64_t drops_render_targets = 0;
    // Of drops_render_targets: the vertex shader memexports; rasterization is off.
    uint64_t drops_rt_memexport_vs = 0;
    uint64_t drops_rt_no_rasterization = 0;
    // Draws without any render target (WebGPURenderTargetCache::
    // last_update_without_attachments), not counted as dropped: skipped because
    // they write nothing (no color mask, depth, stencil or pixel memexport;
    // natively they have no effect either), of which with a vertex memexport
    // (dispatched as compute before); and drawn into the render target cache's
    // dummy attachment (they have effects, or
    // --webgpu_dummy_attachment_for_no_output), of which without output.
    uint64_t draws_no_output = 0;
    uint64_t draws_no_output_memexport_vs = 0;
    uint64_t draws_dummy_attachment = 0;
    uint64_t draws_dummy_attachment_no_output = 0;
    // Vertex memexport as compute (webgpu_vertex_memexport): dispatches
    // recorded, and memexporting draws whose compute variant isn't available.
    uint64_t memexport_dispatches = 0;
    uint64_t memexport_unsupported = 0;
    uint64_t drops_pipeline = 0;
    // WebGPUTessellator refused the draw (index buffer kind, size).
    uint64_t drops_tessellation = 0;
    // Tessellated draws recorded (compute pre-pass + indexed draw).
    uint64_t draws_tessellated = 0;
    uint64_t drops_constants = 0;
    uint64_t drops_sampler = 0;
    // Draws with an empty viewport or scissor (nothing to rasterize).
    uint64_t draws_culled = 0;
    // Draws recorded with a pipeline and bind groups.
    uint64_t draws_recorded = 0;
    uint64_t constant_bytes = 0;
    uint64_t copies = 0;
    uint64_t submissions = 0;
    // webgpu_pipeline_wait_ms: draws / dispatches whose pipeline the CP waited
    // for, of which still not ready when the frame's budget ran out, and the
    // total wait.
    uint64_t pipeline_waits = 0;
    uint64_t pipeline_wait_timeouts = 0;
    double pipeline_wait_ms = 0.0;
  };
  const Statistics& statistics() const { return statistics_; }

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  void WriteRegister(uint32_t index, uint32_t value) override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override;

  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;

  // Guest occlusion queries (ZPD begin / end around the query draws).
  bool ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader, uint32_t packet,
                                          uint32_t count) override;

 private:
  // Device thread, recorded with CmdEncoderCallback by IssueSwap.
  static void EncodeSwap(void* user, WebGPUReplayState& state, WGPUCommandEncoder encoder);
  bool RequestVertexBuffers(const Shader& vertex_shader);
  // Per-draw state of the bind groups (defined in the .cpp: holds the
  // translator's SystemConstants).
  struct DrawState;
  // Updates the system constants for the draw; returns whether they changed.
  bool UpdateSystemConstantValues(bool primitive_polygonal,
                                  const PrimitiveProcessor::ProcessingResult& primitive_result,
                                  const draw_util::ViewportInfo& viewport_info,
                                  uint32_t used_texture_mask,
                                  uint32_t compute_memexport_vertex_count = 0);
  // Records the kMemExportCompute dispatch of a memexporting vertex shader
  // (outside render passes) and marks the exported ranges as GPU-written.
  // False only on a fatal error (the draw then fails).
  bool IssueMemExportCompute(Shader& vertex_shader, bool primitive_polygonal,
                             const PrimitiveProcessor::ProcessingResult& primitive_result);
  // Writes the constant buffers that changed (or weren't written into the
  // current stream yet) and returns their dynamic offsets (binding order).
  bool UpdateConstantBuffers(const Shader& vertex_shader, const Shader* pixel_shader,
                             uint32_t* offsets_out);

  WebGPUGraphicsSystem* webgpu_graphics_system_;
  WebGPUDevice& device_;
  WebGPUSubmissionTracker submission_tracker_;
  std::unique_ptr<WebGPUCommandQueue> command_queue_;
  std::unique_ptr<WebGPUSharedMemory> shared_memory_;
  std::unique_ptr<WebGPUPrimitiveProcessor> primitive_processor_;  // WP3
  std::unique_ptr<WebGPUTextureCache> texture_cache_;
  std::unique_ptr<WebGPURenderTargetCache> render_target_cache_;
  std::unique_ptr<WebGPUPipelineCache> pipeline_cache_;
  // Tessellated draws (compute pre-pass; null if it couldn't be created).
  std::unique_ptr<WebGPUTessellator> tessellator_;
  // Device-thread object.
  WebGPUPresentPass* present_pass_ = nullptr;

  // Key of the render pass open in the current stream (0 = none / unkeyed).
  uint64_t open_render_pass_key_ = 0;
  // Incremented whenever BeginRenderPass opens a pass (pass-local state).
  uint64_t render_pass_serial_ = 0;
  std::unique_ptr<DrawState> draw_state_;

  // Vertex fetch constants whose buffers were requested since the last swap or
  // fetch constant write (bit per vfetch slot, 96 slots).
  uint64_t vertex_buffers_in_sync_[2] = {};

  // IssueSwap -> EncodeSwap (the refresher waits for the replay, so one is enough).
  struct SwapBlit {
    WGPUTextureView target = nullptr;
    WGPUTextureView source = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    WGPUColor clear_color = {0.0, 0.0, 0.0, 1.0};
    // WebGPUPresentPass::Frame::GammaRamp and its data (PWL: 128 x 3 dwords,
    // table: the first 256).
    uint32_t gamma_mode = 0;
    bool gamma_rb_swap = false;
    uint32_t gamma_ramp[128 * 3] = {};
  };
  SwapBlit swap_blit_;
  uint64_t logged_gamma_ramp_hash_ = 0;
  // Last RB_COLOR_CLEAR seen in a resolve with color clear.
  bool has_last_color_clear_ = false;
  uint32_t last_color_clear_ = 0;

  std::array<uint64_t, kMaxFramesInFlight> swap_submissions_ = {};
  Statistics statistics_;
  bool memexport_logged_ = false;
  uint32_t drop_details_logged_ = 0;
  // webgpu_pipeline_wait_ms spent in the current frame (reset by IssueSwap).
  double pipeline_wait_frame_ms_ = 0.0;
  // Waits for the pipeline of a draw or dispatch about to be recorded, within
  // the frame's webgpu_pipeline_wait_ms budget.
  void WaitForPipeline(PipelineId pipeline);

  // Draws since the last ZPD event (webgpu_occlusion_query_log).
  struct QueryDrawCounts {
    uint32_t draws = 0;
    uint32_t recorded = 0;
    uint32_t no_attachments = 0;
    uint32_t depth_test = 0;
    uint32_t point_lists = 0;
    uint32_t no_color = 0;
  };
  QueryDrawCounts query_draws_;
  uint32_t zpd_events_logged_ = 0;
};

}  // namespace rex::graphics::webgpu
