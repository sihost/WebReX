/**
 * @file        rex/graphics/webgpu/webgpu_command_processor.h
 * @brief       WebGPU command processor: PM4 on the CP thread, GPU work recorded into
 *              WebGPUCommandStreams and replayed on the WebGPU device thread.
 *
 * docs/port/webgpu.md sections 1, 2 and 10 (WP1). The shared CommandProcessor
 * executes PM4 (registers, fences, interrupts, swaps); this class turns draws,
 * resolves and swaps into stream commands:
 *  - IssueDraw: shader analysis, vertex/index buffer residency in the shared
 *    memory (uploads consume the web dirty map), then render targets (WP4) and
 *    the pipeline (WP2). While those are stubs the draw is dropped and counted.
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
#include <rex/graphics/webgpu/webgpu_command_stream.h>
#include <rex/graphics/webgpu/webgpu_submission_tracker.h>

namespace rex::graphics::webgpu {

class WebGPUDevice;
class WebGPUGraphicsSystem;
class WebGPUPipelineCache;
class WebGPUPresentPass;
class WebGPURenderTargetCache;
class WebGPUSharedMemory;
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
    uint64_t copies = 0;
    uint64_t submissions = 0;
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

 private:
  // Device thread, recorded with CmdEncoderCallback by IssueSwap.
  static void EncodeSwap(void* user, WebGPUReplayState& state, WGPUCommandEncoder encoder);
  bool RequestVertexBuffers(const Shader& vertex_shader);

  WebGPUGraphicsSystem* webgpu_graphics_system_;
  WebGPUDevice& device_;
  WebGPUSubmissionTracker submission_tracker_;
  std::unique_ptr<WebGPUCommandQueue> command_queue_;
  std::unique_ptr<WebGPUSharedMemory> shared_memory_;
  std::unique_ptr<WebGPUTextureCache> texture_cache_;
  std::unique_ptr<WebGPURenderTargetCache> render_target_cache_;
  std::unique_ptr<WebGPUPipelineCache> pipeline_cache_;
  // Device-thread object.
  WebGPUPresentPass* present_pass_ = nullptr;

  // Key of the render pass open in the current stream (0 = none / unkeyed).
  uint64_t open_render_pass_key_ = 0;

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
  };
  SwapBlit swap_blit_;
  // Last RB_COLOR_CLEAR seen in a resolve with color clear.
  bool has_last_color_clear_ = false;
  uint32_t last_color_clear_ = 0;

  std::array<uint64_t, kMaxFramesInFlight> swap_submissions_ = {};
  Statistics statistics_;
  bool memexport_logged_ = false;
};

}  // namespace rex::graphics::webgpu
