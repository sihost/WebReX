/**
 * @file        rex/graphics/webgpu/webgpu_readback.h
 * @brief       Delayed (double-buffered) copy of memexported guest memory back to the CPU.
 *
 * The web counterpart of the native readback_memexport_fast path: memexport results
 * live only in the shared memory buffer on the GPU, so guest memory never holds them,
 * and a later CPU write to the same 4 KiB page uploads the page's stale CPU copy over
 * them. Natively, the D3D12 / Vulkan backends copy the exported ranges into a readback
 * buffer and apply the previous frame's copy. Resolves are not read back this way
 * (only synchronously with --webgpu_readback_resolve; native readback_resolve defaults
 * to none).
 *
 * Here (CP thread unless noted):
 *  - Enqueue records a copyBufferToBuffer of the range from the shared memory buffer
 *    into the current batch, a MapRead buffer, in the open stream (no wait).
 *  - EndFrame (once per guest frame) closes the batch: a submitted callback of the
 *    current stream maps it on the device thread, which copies the mapped bytes into
 *    the batch's CPU array and unmaps. EndFrame also applies the batches mapped since,
 *    in order (usually the previous frame's).
 *  - Apply writes a page of a range into guest memory (plain memcpy: no dirty marking,
 *    the GPU copy is already valid) only if the page still holds GPU-written data that
 *    no upload of CPU data replaced since (SharedMemory::IsPageGpuWritten), and the guest
 *    CPU hasn't written it since the export (web dirty map, unconsumed). Otherwise the
 *    CPU's newer bytes win, as they do on the GPU.
 * Mapping happens one or more frames after the export (the GPU and Chrome's readback
 * latency), so guest memory lags the GPU by about a frame, as natively. Nothing waits;
 * batches still mapping when kMaxBatchesInFlight are outstanding are not started
 * (counted as skipped).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include <webgpu/webgpu.h>

#include <rex/graphics/webgpu/webgpu_command_stream.h>
#include <rex/memory.h>

namespace rex::graphics::webgpu {

class WebGPUCommandProcessor;
class WebGPUSharedMemory;

class WebGPUReadback {
 public:
  enum class Mode {
    kOff,
    // Delayed: this class.
    kAsync,
    // Immediate: the stream is submitted and the CP thread waits for the copy
    // (WebGPURenderTargetCache::ReadbackResolve).
    kSync,
  };
  // Off unless --webgpu_readback_memexport_delayed (then --readback_memexport_fast
  // picks kAsync / kSync, as natively) or the diagnostic --webgpu_readback_memexport
  // (kSync); --readback_memexport=false turns both off. Off by default on the web,
  // unlike natively: measured too costly for Halo (see the cvar in the .cpp).
  static Mode GetMemExportMode();

  static constexpr uint32_t kMaxBatchesInFlight = 8;
  static constexpr uint64_t kMinBatchCapacity = uint64_t(1) << 20;
  static constexpr uint64_t kMaxBatchCapacity = uint64_t(64) << 20;

  struct Statistics {
    uint64_t frames = 0;
    // Ranges and bytes recorded for readback, and those not recorded because too
    // many batches were in flight.
    uint64_t memexport_ranges = 0;
    uint64_t memexport_bytes = 0;
    uint64_t ranges_skipped = 0;
    // Batches mapped / failed, and their latency in frames (sum).
    uint64_t batches_mapped = 0;
    uint64_t batches_failed = 0;
    uint64_t batch_latency_frames = 0;
    // Bytes written into guest memory, and pages left alone: the guest CPU wrote
    // them since the export (dirty), or CPU data was uploaded over the GPU data.
    uint64_t bytes_applied = 0;
    uint64_t pages_skipped_dirty = 0;
    uint64_t pages_skipped_replaced = 0;
    // Batch buffers created (a device-thread round trip each).
    uint64_t buffers_created = 0;
    // CP thread time in Enqueue + EndFrame, device thread time copying mapped
    // data (milliseconds, totals).
    double cp_ms = 0.0;
    double device_ms = 0.0;
  };

  WebGPUReadback(WebGPUCommandProcessor& command_processor, WebGPUSharedMemory& shared_memory);
  ~WebGPUReadback();
  WebGPUReadback(const WebGPUReadback&) = delete;
  WebGPUReadback& operator=(const WebGPUReadback&) = delete;

  // After the memexport of guest physical [address, address + length) has been
  // recorded in the current stream (and RangeWrittenByGpu called).
  void Enqueue(memory::Memory& memory, uint32_t address, uint32_t length);
  // Once per guest frame, before the swap's submission.
  void EndFrame();
  // Releases everything (batches still mapping are freed by their callback).
  void Shutdown();

  const Statistics& statistics() const { return statistics_; }

 private:
  enum : int { kBatchIdle, kBatchMapping, kBatchMapped, kBatchFailed, kBatchAbandoned };
  struct Range {
    uint32_t address;
    uint32_t length;
    uint64_t offset;  // Of the first byte in the batch.
  };
  struct Batch {
    WGPUBuffer buffer = nullptr;  // Device-thread handle.
    uint64_t capacity = 0;
    uint64_t used = 0;
    uint64_t frame = 0;  // EndFrame count when closed.
    std::vector<Range> ranges;
    std::vector<uint8_t> data;  // Mapped bytes (written by the device thread).
    std::atomic<int> state{kBatchIdle};
  };

  static void OnBatchSubmitted(void* user, WebGPUReplayState& state);
  void CloseBatch();
  void Apply(Batch& batch);
  void ReleaseBatch(std::unique_ptr<Batch> batch);

  WebGPUCommandProcessor& command_processor_;
  WebGPUSharedMemory& shared_memory_;
  memory::Memory* memory_ = nullptr;
  std::unique_ptr<Batch> current_;
  std::deque<std::unique_ptr<Batch>> in_flight_;
  std::vector<std::unique_ptr<Batch>> free_;
  Statistics statistics_;
  // Capacity of new batch buffers (grows to a frame's worth of ranges).
  uint64_t next_capacity_ = kMinBatchCapacity;
};

}  // namespace rex::graphics::webgpu
