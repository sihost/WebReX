/**
 * @file        rex/graphics/webgpu/webgpu_command_stream.h
 * @brief       CP-thread recording of WebGPU commands, replayed on the device thread.
 *
 * docs/port/webgpu.md section 1: WGPU* handles only work on the device thread
 * and a proxy round trip per draw is too slow, so the command processor records
 * into a WebGPUCommandStream (the analog of vulkan/deferred_command_buffer) and
 * hands each finished stream to the device thread with one RunAsync. There
 * WebGPUCommandStream::Execute encodes it into one command buffer and submits.
 *
 * A stream holds three CPU-side arenas, all read in place by the device thread
 * (wasm memory is shared):
 *  - commands: fixed-layout records (8-byte aligned), replayed in order;
 *  - uploads:  data for CmdUploadToBuffer / CmdUploadToTexture. Replay writes the
 *              whole arena into a staging buffer with one wgpuQueueWriteBuffer
 *              before encoding, and each upload becomes a copy from the staging
 *              buffer at its position in the stream, so uploads stay ordered with
 *              the GPU work around them (a plain writeBuffer per upload would
 *              run before the whole command buffer);
 *  - constants: data for AllocateConstants, written into the stream constant
 *              buffer before the command buffer runs, so every draw of the
 *              stream sees its own slice (bind it through StreamConstantsBuffer()
 *              with a dynamic offset). Queue order makes reusing one staging and
 *              one constant buffer for every stream safe: the next stream's
 *              writeBuffer runs on the queue timeline after this stream's work.
 *
 * Handles recorded into a stream must stay alive until the stream has been
 * replayed (WebGPUSubmissionTracker::replayed); after wgpuQueueSubmit WebGPU
 * keeps the objects alive for the GPU itself. Release them on the device thread
 * with RunAsync after the submit, or in a host callback.
 *
 * Structural mistakes (draw outside a render pass, copy inside a pass) are
 * dropped at record time and counted (dropped_commands()), so a replayed stream
 * never produces encoder validation errors from its own structure.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <webgpu/webgpu.h>

namespace rex::graphics::webgpu {

class WebGPUDevice;
class WebGPUSubmissionTracker;

// ---------------------------------------------------------------------------
// Pipelines by id (asynchronous creation).
// ---------------------------------------------------------------------------

// Pipelines are referenced by id in streams so that they can be created with
// createRenderPipelineAsync after the draw that needs them has been recorded.
// A draw whose pipeline is not ready when the stream is replayed is skipped
// (WebGPUReplayStats::draws_skipped). Ids are allocated on any thread; the
// table itself is only touched on the device thread.
using PipelineId = uint32_t;
constexpr PipelineId kInvalidPipelineId = 0;

class WebGPUPipelineTable {
 public:
  WebGPUPipelineTable() = default;
  WebGPUPipelineTable(const WebGPUPipelineTable&) = delete;
  WebGPUPipelineTable& operator=(const WebGPUPipelineTable&) = delete;
  // Release() must have run on the device thread before destruction if any
  // pipeline was set.
  ~WebGPUPipelineTable() = default;

  // Any thread.
  PipelineId Allocate() { return next_id_.fetch_add(1, std::memory_order_relaxed); }

  // Device thread. Set* take ownership of the reference (released by Release
  // or when replaced).
  void SetRender(PipelineId id, WGPURenderPipeline pipeline);
  void SetCompute(PipelineId id, WGPUComputePipeline pipeline);
  WGPURenderPipeline GetRender(PipelineId id) const {
    return id < render_.size() ? render_[id] : nullptr;
  }
  WGPUComputePipeline GetCompute(PipelineId id) const {
    return id < compute_.size() ? compute_[id] : nullptr;
  }
  void Release();

 private:
  std::atomic<PipelineId> next_id_{1};
  std::vector<WGPURenderPipeline> render_;
  std::vector<WGPUComputePipeline> compute_;
};

// ---------------------------------------------------------------------------
// Replay state and statistics (device thread).
// ---------------------------------------------------------------------------

struct WebGPUReplayStats {
  uint32_t commands = 0;
  uint32_t draws = 0;
  // Draws dropped because their pipeline id had no pipeline yet.
  uint32_t draws_skipped = 0;
  uint32_t dispatches = 0;
  uint32_t render_passes = 0;
  uint32_t compute_passes = 0;
  uint32_t copies = 0;
  uint32_t bind_groups_created = 0;
  uint32_t bind_groups_reused = 0;
  // Pass state calls dropped because the same value was already set in the pass
  // (--webgpu_filter_redundant_state).
  uint32_t redundant_state_skipped = 0;
  uint64_t upload_bytes = 0;
  uint64_t constant_bytes = 0;
  // Device-thread wall time of Execute: arena writes + encoding, and the
  // finish + submit part.
  double encode_ms = 0.0;
  double submit_ms = 0.0;
};

// Device-thread objects that persist between replays: the staging and stream
// constant buffers (grown on demand) and the pipeline table.
class WebGPUReplayState {
 public:
  WebGPUReplayState(WebGPUDevice& device, WebGPUPipelineTable* pipelines);
  // Device thread.
  ~WebGPUReplayState();
  WebGPUReplayState(const WebGPUReplayState&) = delete;
  WebGPUReplayState& operator=(const WebGPUReplayState&) = delete;

  WebGPUDevice& device() const { return device_; }
  WebGPUPipelineTable* pipelines() const { return pipelines_; }

 private:
  friend class WebGPUCommandStream;
  friend class WebGPUCommandQueue;
  bool EnsureBuffer(WGPUBuffer& buffer, uint64_t& buffer_size, uint64_t needed,
                    WGPUBufferUsage usage, const char* label);

  WebGPUDevice& device_;
  WebGPUPipelineTable* pipelines_;
  WGPUBuffer staging_buffer_ = nullptr;
  uint64_t staging_buffer_size_ = 0;
  WGPUBuffer constant_buffer_ = nullptr;
  uint64_t constant_buffer_size_ = 0;

  // --webgpu_log_skipped_pipelines: per pipeline id, the draws / dispatches
  // skipped while it was not ready and those replayed after it was.
  struct PipelineSkipStats {
    uint32_t skipped = 0;
    uint32_t drawn = 0;
    uint32_t first_skip_replay = 0;
    uint32_t last_skip_replay = 0;
  };
  std::vector<PipelineSkipStats> skip_stats_;
  uint32_t replay_index_ = 0;
  bool log_skipped_pipelines_ = false;

  // Bind groups created from descriptors that cannot be cached (an entry with a
  // nextInChain), released at the end of the Execute that created them.
  struct TransientBindGroup {
    uint64_t hash;
    size_t record_offset;  // Descriptor in the stream being replayed.
    WGPUBindGroup bind_group;
  };
  std::vector<TransientBindGroup> transient_bind_groups_;

  // Bind groups created from descriptors, kept across replays
  // (--webgpu_bind_group_cache_replays): most draws of a frame bind the same
  // textures / samplers / buffer ranges as in the previous frame, and
  // wgpuDeviceCreateBindGroup is one of the most expensive device-thread calls
  // on the web (JS descriptor objects -> GC). Each cached entry holds a reference
  // to its buffers, samplers and texture views, so their handles cannot be
  // reused by new objects while the entry exists (a key match is always the same
  // objects). The key is the recorded layout + entries (the stream constant
  // placeholder stays in the key; the cache is flushed when the real constant
  // buffer is reallocated).
  struct CachedBindGroup {
    std::vector<uint8_t> key;
    WGPUBindGroup bind_group;
    uint32_t last_used_replay;
  };
  std::unordered_multimap<uint64_t, CachedBindGroup> bind_group_cache_;
  WGPUBuffer bind_group_cache_constant_buffer_ = nullptr;
  uint32_t bind_group_cache_last_trim_ = 0;
  void ReleaseCachedBindGroup(CachedBindGroup& cached);
  void ClearBindGroupCache();
  // Releases entries not used in the last max_age replays.
  void TrimBindGroupCache(uint32_t replay_index, uint32_t max_age);
};

// ---------------------------------------------------------------------------
// The stream.
// ---------------------------------------------------------------------------

// Placeholder buffer handle meaning "the stream constant buffer" in bind group
// descriptors recorded with CmdSetBindGroup(layout, entries...). Replay
// substitutes the replay state's constant buffer.
inline WGPUBuffer StreamConstantsBuffer() {
  return reinterpret_cast<WGPUBuffer>(uintptr_t(1));
}

class WebGPUCommandStream {
 public:
  // Alignment of AllocateConstants offsets: the largest legal
  // minUniformBufferOffsetAlignment, so it is valid on every device.
  static constexpr uint32_t kConstantsAlignment = 256;

  struct RenderPassDesc {
    static constexpr uint32_t kMaxColorAttachments = 8;
    uint32_t color_attachment_count = 0;
    WGPURenderPassColorAttachment color_attachments[kMaxColorAttachments] = {};
    bool has_depth_stencil = false;
    WGPURenderPassDepthStencilAttachment depth_stencil = {};
    WGPUQuerySet occlusion_query_set = nullptr;

    // Convenience: attachment i = view with clear (or load if !clear) + store.
    void SetColor(uint32_t index, WGPUTextureView view, bool clear,
                  WGPUColor clear_value = {0.0, 0.0, 0.0, 0.0});
  };

  // Called on the device thread at the callback's position in the stream
  // (outside passes) with the command encoder of the stream, so a subsystem
  // can encode its own work (for example the present pass). Must not submit.
  using EncoderCallback = void (*)(void* user, WebGPUReplayState& state,
                                   WGPUCommandEncoder encoder);
  // Called on the device thread after the stream's wgpuQueueSubmit (for
  // example to start a mapAsync on a readback buffer, or to release objects).
  using SubmittedCallback = void (*)(void* user, WebGPUReplayState& state);

  WebGPUCommandStream() = default;
  WebGPUCommandStream(const WebGPUCommandStream&) = delete;
  WebGPUCommandStream& operator=(const WebGPUCommandStream&) = delete;
  ~WebGPUCommandStream();

  // ---- Recording (one thread at a time, normally the CP). ----

  void Reset();
  bool empty() const { return commands_.size == 0 && submitted_callbacks_.empty(); }
  bool in_render_pass() const { return pass_ == Pass::kRender; }
  bool in_compute_pass() const { return pass_ == Pass::kCompute; }
  bool in_pass() const { return pass_ != Pass::kNone; }
  size_t command_bytes() const { return commands_.size; }
  size_t upload_bytes() const { return uploads_.size; }
  size_t constant_bytes() const { return constants_.size; }
  uint32_t recorded_commands() const { return recorded_commands_; }
  uint32_t dropped_commands() const { return dropped_commands_; }

  void CmdBeginRenderPass(const RenderPassDesc& desc);
  void CmdEndRenderPass();
  void CmdBeginComputePass();
  void CmdEndComputePass();
  // Ends whichever pass is open.
  void EndPass();

  // Render pass.
  void CmdSetPipeline(PipelineId id);
  void CmdSetRenderPipeline(WGPURenderPipeline pipeline);
  void CmdSetVertexBuffer(uint32_t slot, WGPUBuffer buffer, uint64_t offset = 0,
                          uint64_t size = WGPU_WHOLE_SIZE);
  void CmdSetIndexBuffer(WGPUBuffer buffer, WGPUIndexFormat format, uint64_t offset = 0,
                         uint64_t size = WGPU_WHOLE_SIZE);
  void CmdSetViewport(float x, float y, float width, float height, float min_depth = 0.0f,
                      float max_depth = 1.0f);
  void CmdSetScissorRect(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
  void CmdSetBlendConstant(const WGPUColor& color);
  void CmdSetStencilReference(uint32_t reference);
  void CmdDraw(uint32_t vertex_count, uint32_t instance_count = 1, uint32_t first_vertex = 0,
               uint32_t first_instance = 0);
  void CmdDrawIndexed(uint32_t index_count, uint32_t instance_count = 1, uint32_t first_index = 0,
                      int32_t base_vertex = 0, uint32_t first_instance = 0);

  // Compute pass.
  void CmdSetComputePipeline(PipelineId id);
  void CmdSetComputePipelineObject(WGPUComputePipeline pipeline);
  void CmdDispatch(uint32_t x, uint32_t y = 1, uint32_t z = 1);

  // Either pass. An existing bind group:
  void CmdSetBindGroup(uint32_t group_index, WGPUBindGroup group,
                       uint32_t dynamic_offset_count = 0,
                       const uint32_t* dynamic_offsets = nullptr);
  // A bind group created during replay from this descriptor (deduplicated
  // within the stream). Entries may use StreamConstantsBuffer().
  void CmdSetBindGroup(uint32_t group_index, WGPUBindGroupLayout layout,
                       const WGPUBindGroupEntry* entries, uint32_t entry_count,
                       uint32_t dynamic_offset_count = 0,
                       const uint32_t* dynamic_offsets = nullptr);

  // Outside passes.
  void CmdCopyBufferToBuffer(WGPUBuffer source, uint64_t source_offset, WGPUBuffer destination,
                             uint64_t destination_offset, uint64_t size);
  void CmdCopyBufferToTexture(const WGPUTexelCopyBufferInfo& source,
                              const WGPUTexelCopyTextureInfo& destination,
                              const WGPUExtent3D& size);
  void CmdCopyTextureToBuffer(const WGPUTexelCopyTextureInfo& source,
                              const WGPUTexelCopyBufferInfo& destination,
                              const WGPUExtent3D& size);
  void CmdCopyTextureToTexture(const WGPUTexelCopyTextureInfo& source,
                               const WGPUTexelCopyTextureInfo& destination,
                               const WGPUExtent3D& size);
  void CmdClearBuffer(WGPUBuffer buffer, uint64_t offset, uint64_t size);
  // Returns `size` writable bytes (valid until the stream is submitted) whose
  // contents are copied to destination at this point of the stream. size and
  // destination_offset must be multiples of 4. Returns nullptr inside a pass.
  void* CmdUploadToBuffer(WGPUBuffer destination, uint64_t destination_offset, uint64_t size);
  // Same for a texture region; bytes_per_row must be a multiple of 256
  // (copyBufferToTexture rule), data_size = bytes_per_row * rows * layers.
  void* CmdUploadToTexture(const WGPUTexelCopyTextureInfo& destination, uint32_t bytes_per_row,
                           uint32_t rows_per_image, const WGPUExtent3D& size, uint64_t data_size);
  void CmdEncoderCallback(EncoderCallback callback, void* user);

  // Constants visible to the whole stream: returns `size` writable bytes at
  // `offset_out` (a multiple of kConstantsAlignment) in the stream constant
  // buffer. Usable inside passes.
  void* AllocateConstants(uint32_t size, uint32_t& offset_out);

  // Runs after this stream's submit (any pass state).
  void AddSubmittedCallback(SubmittedCallback callback, void* user);

  // ---- Replay (device thread). ----

  // Writes the arenas, encodes every command, submits one command buffer, and
  // runs the submitted callbacks. Returns false if the encoder could not be
  // created (device lost).
  bool Execute(WebGPUReplayState& state, WebGPUReplayStats* stats_out = nullptr);

 private:
  enum class Pass : uint8_t { kNone, kRender, kCompute };
  enum class Command : uint32_t;

  // Growable byte arena without value-initialization.
  struct Arena {
    uint8_t* data = nullptr;
    size_t size = 0;
    size_t capacity = 0;
    // Returns the offset of `bytes` new bytes aligned to `alignment` (power of
    // two, relative to the arena start).
    size_t Allocate(size_t bytes, size_t alignment);
    void Free();
  };

  void* Record(Command command, size_t payload_bytes);
  bool Require(Pass pass, const char* what);

  Arena commands_;
  Arena uploads_;
  Arena constants_;
  struct SubmittedCallbackEntry {
    SubmittedCallback callback;
    void* user;
  };
  std::vector<SubmittedCallbackEntry> submitted_callbacks_;
  Pass pass_ = Pass::kNone;
  uint32_t recorded_commands_ = 0;
  uint32_t dropped_commands_ = 0;
};

// ---------------------------------------------------------------------------
// Submission of streams to the device thread.
// ---------------------------------------------------------------------------

// CP-side owner of a small pool of streams, the device-thread replay state and
// the pipeline table. Submit() hands the current stream to the device thread
// (one RunAsync per submission) and switches to a free stream; when all are in
// flight it waits for the device thread to replay one (backpressure on the
// CPU side; GPU-side latency is bounded by the caller with
// WebGPUSubmissionTracker::AwaitCompleted).
class WebGPUCommandQueue {
 public:
  WebGPUCommandQueue(WebGPUDevice& device, WebGPUSubmissionTracker& tracker);
  ~WebGPUCommandQueue();
  WebGPUCommandQueue(const WebGPUCommandQueue&) = delete;
  WebGPUCommandQueue& operator=(const WebGPUCommandQueue&) = delete;

  // Not on the device thread.
  bool Initialize(uint32_t stream_count = 3);
  // Waits for every submitted stream to be replayed, then releases the
  // device-thread state (pipeline table included).
  void Shutdown();

  WebGPUDevice& device() const { return device_; }
  WebGPUSubmissionTracker& tracker() const { return tracker_; }
  WebGPUPipelineTable& pipeline_table() { return pipeline_table_; }

  // The stream being recorded.
  WebGPUCommandStream& stream() { return *current_; }

  // Ends an open pass, submits the current stream (even an empty one, so the
  // submission counters always advance) and returns its submission id.
  uint64_t Submit();
  // Submit() + wait until the device thread has replayed it.
  uint64_t SubmitAndWaitReplayed();

  // Any thread: log per pipeline id the draws skipped while it was not ready
  // (the first replay that draws it after skips, and every 1000 replays the ids
  // skipped and never drawn since).
  void set_log_skipped_pipelines(bool log) {
    log_skipped_pipelines_.store(log, std::memory_order_relaxed);
  }

  // Statistics of the most recently replayed stream, and running totals.
  WebGPUReplayStats last_replay_stats() const;
  uint64_t replay_count() const { return replay_count_.load(std::memory_order_relaxed); }
  // Device-thread wall time of every Execute so far (encode + submit), ms.
  double replay_ms_total() const;
  // Draws and dispatches skipped by all replays so far (pipeline not ready).
  uint64_t draws_skipped_total() const {
    return draws_skipped_total_.load(std::memory_order_relaxed);
  }

 private:
  void ReplayOnDeviceThread(WebGPUCommandStream* stream, uint64_t submission);

  WebGPUDevice& device_;
  WebGPUSubmissionTracker& tracker_;
  WebGPUPipelineTable pipeline_table_;
  // Created and destroyed on the device thread.
  WebGPUReplayState* replay_state_ = nullptr;

  std::vector<std::unique_ptr<WebGPUCommandStream>> streams_;
  WebGPUCommandStream* current_ = nullptr;
  std::mutex free_mutex_;
  std::condition_variable free_cond_;
  std::vector<WebGPUCommandStream*> free_streams_;

  mutable std::mutex stats_mutex_;
  WebGPUReplayStats last_stats_;
  double replay_ms_total_ = 0.0;
  std::atomic<uint64_t> replay_count_{0};
  std::atomic<uint64_t> draws_skipped_total_{0};
  std::atomic<bool> log_skipped_pipelines_{false};
  bool initialized_ = false;
};

}  // namespace rex::graphics::webgpu
