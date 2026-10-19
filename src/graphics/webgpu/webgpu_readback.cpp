/**
 * @file        graphics/webgpu/webgpu_readback.cpp
 * @brief       Delayed copy of GPU-written guest memory back to the CPU (see the header).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/webgpu_readback.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/webgpu/webgpu_command_processor.h>
#include <rex/graphics/webgpu/webgpu_device.h>
#include <rex/graphics/webgpu/webgpu_log.h>
#include <rex/graphics/webgpu/webgpu_shared_memory.h>
#include <rex/logging.h>
#include <rex/ppc/fold_web.h>

REXCVAR_DECLARE(bool, webgpu_readback_memexport);

REXCVAR_DEFINE_BOOL(webgpu_readback_memexport_delayed, false, "GPU",
                    "WebGPU: copy vertex memexport results back into guest memory one or two "
                    "frames late without waiting (the native readback_memexport_fast path; "
                    "readback_memexport_fast=false makes it synchronous). Off by default on the "
                    "web: Halo's riverworld exports about 13 MiB per frame, and the copy + map + "
                    "apply cost exceeds the 0.5 ms per frame budget (webgpu_readback_log_frames "
                    "logs it)");
REXCVAR_DEFINE_INT32(webgpu_readback_log_frames, 600, "GPU",
                     "WebGPU: log the delayed memexport readback statistics every N "
                     "guest frames (0 = never)");

namespace rex::graphics::webgpu {

namespace {

// Device-thread time of the map callbacks (a batch may outlive its owner).
std::atomic<uint64_t> g_readback_device_ns{0};

double NowMs() {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

WebGPUReadback::Mode WebGPUReadback::GetMemExportMode() {
  if (!REXCVAR_GET(readback_memexport)) {
    return Mode::kOff;
  }
  if (REXCVAR_GET(webgpu_readback_memexport)) {
    return Mode::kSync;
  }
  // Off by default on the web, unlike natively (readback_memexport defaults to true there):
  // Halo's riverworld exports ~13 MiB per frame (about 40 dispatches over whole particle
  // buffers); see the cvar.
  if (!REXCVAR_GET(webgpu_readback_memexport_delayed)) {
    return Mode::kOff;
  }
  return REXCVAR_GET(readback_memexport_fast) ? Mode::kAsync : Mode::kSync;
}

WebGPUReadback::WebGPUReadback(WebGPUCommandProcessor& command_processor,
                               WebGPUSharedMemory& shared_memory)
    : command_processor_(command_processor), shared_memory_(shared_memory) {}

WebGPUReadback::~WebGPUReadback() { Shutdown(); }

void WebGPUReadback::Enqueue(memory::Memory& memory, uint32_t address, uint32_t length) {
  if (!length || address >= SharedMemory::kBufferSize) {
    return;
  }
  length = std::min(length, SharedMemory::kBufferSize - address);
  double start_ms = NowMs();
  memory_ = &memory;
  // copyBufferToBuffer: offsets and size multiples of 4.
  uint64_t copy_start = address & ~uint64_t(3);
  uint64_t copy_size = ((uint64_t(address) + length + 3) & ~uint64_t(3)) - copy_start;
  if (current_ && current_->used + copy_size > current_->capacity && current_->used) {
    // Grow the next batches to a frame's worth, so a frame is normally one batch.
    next_capacity_ = std::min(kMaxBatchCapacity,
                              std::max(next_capacity_, (current_->used + copy_size) * 2));
    CloseBatch();
  }
  if (!current_) {
    if (in_flight_.size() >= kMaxBatchesInFlight) {
      ++statistics_.ranges_skipped;
      statistics_.cp_ms += NowMs() - start_ms;
      return;
    }
    if (!free_.empty()) {
      current_ = std::move(free_.back());
      free_.pop_back();
    } else {
      current_ = std::make_unique<Batch>();
    }
  }
  Batch& batch = *current_;
  if (batch.capacity < std::max(copy_size, next_capacity_) || !batch.buffer) {
    uint64_t capacity = std::max(next_capacity_, (copy_size + 0xFFFF) & ~uint64_t(0xFFFF));
    capacity = (capacity + 0xFFFF) & ~uint64_t(0xFFFF);
    WebGPUDevice& device = command_processor_.device();
    WGPUBuffer old_buffer = batch.buffer;
    WGPUBuffer new_buffer = nullptr;
    device.RunSync([&]() {
      if (old_buffer) {
        wgpuBufferRelease(old_buffer);
      }
      WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
      desc.label = MakeStringView("rex delayed readback");
      desc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
      desc.size = capacity;
      new_buffer = wgpuDeviceCreateBuffer(device.device(), &desc);
    });
    ++statistics_.buffers_created;
    batch.buffer = new_buffer;
    batch.capacity = new_buffer ? capacity : 0;
    if (!new_buffer) {
      ++statistics_.ranges_skipped;
      statistics_.cp_ms += NowMs() - start_ms;
      return;
    }
  }
  command_processor_.EndRenderPass();
  command_processor_.command_stream().CmdCopyBufferToBuffer(shared_memory_.buffer(), copy_start,
                                                            batch.buffer, batch.used, copy_size);
  batch.ranges.push_back({address, length, batch.used + (address - copy_start)});
  batch.used += copy_size;
  ++statistics_.memexport_ranges;
  statistics_.memexport_bytes += length;
  statistics_.cp_ms += NowMs() - start_ms;
}

void WebGPUReadback::CloseBatch() {
  if (!current_) {
    return;
  }
  if (current_->ranges.empty()) {
    free_.push_back(std::move(current_));
    return;
  }
  Batch* batch = current_.get();
  batch->frame = statistics_.frames;
  batch->state.store(kBatchMapping, std::memory_order_release);
  // After this stream's submit, so the mapping waits for every copy into the
  // batch (all recorded in this stream or in earlier ones).
  command_processor_.command_stream().AddSubmittedCallback(OnBatchSubmitted, batch);
  in_flight_.push_back(std::move(current_));
}

void WebGPUReadback::OnBatchSubmitted(void* user, WebGPUReplayState& /*state*/) {
  // Device thread.
  Batch* batch = static_cast<Batch*>(user);
  WGPUBufferMapCallbackInfo info = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
  info.mode = WGPUCallbackMode_AllowSpontaneous;
  info.userdata1 = batch;
  info.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* userdata1, void*) {
    auto* batch = static_cast<Batch*>(userdata1);
    auto start = std::chrono::steady_clock::now();
    int result = kBatchFailed;
    if (status == WGPUMapAsyncStatus_Success) {
      const auto* mapped =
          static_cast<const uint8_t*>(wgpuBufferGetConstMappedRange(batch->buffer, 0, batch->used));
      if (mapped) {
        batch->data.resize(batch->used);
        std::memcpy(batch->data.data(), mapped, batch->used);
        result = kBatchMapped;
      }
      wgpuBufferUnmap(batch->buffer);
    }
    g_readback_device_ns.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count()),
        std::memory_order_relaxed);
    if (batch->state.exchange(result, std::memory_order_acq_rel) == kBatchAbandoned) {
      // The owner shut down meanwhile.
      if (batch->buffer) {
        wgpuBufferRelease(batch->buffer);
      }
      delete batch;
    }
  };
  wgpuBufferMapAsync(batch->buffer, WGPUMapMode_Read, 0, size_t(batch->used), info);
}

void WebGPUReadback::Apply(Batch& batch) {
  if (!memory_) {
    return;
  }
  constexpr uint32_t kPageShift = rex::fold::kDirtyPageShift;
  static_assert(kPageShift == 12);
  const uint8_t* data = batch.data.data();
  for (const Range& range : batch.ranges) {
    uint32_t address = range.address;
    uint32_t end = range.address + range.length;
    while (address < end) {
      uint32_t page = address >> kPageShift;
      uint32_t piece_end = std::min(end, (page + 1) << kPageShift);
      uint32_t piece = piece_end - address;
      if (rex::fold::g_phys_dirty[page]) {
        // The guest CPU wrote the page after the export (the export consumed the
        // dirty bits of its pages): its bytes are newer.
        ++statistics_.pages_skipped_dirty;
      } else if (!shared_memory_.IsPageGpuWritten(address)) {
        // CPU data was uploaded over the GPU data since (the invalidation clears
        // the GPU-written flag): the guest memory copy is the current one.
        ++statistics_.pages_skipped_replaced;
      } else {
        uint8_t* guest = memory_->TranslatePhysical(address);
        if (guest) {
          std::memcpy(guest, data + range.offset + (address - range.address), piece);
          statistics_.bytes_applied += piece;
        }
      }
      address = piece_end;
    }
  }
}

void WebGPUReadback::EndFrame() {
  double start_ms = NowMs();
  ++statistics_.frames;
  CloseBatch();
  // In order: a later batch may hold newer data for the same range.
  while (!in_flight_.empty()) {
    Batch& batch = *in_flight_.front();
    int state = batch.state.load(std::memory_order_acquire);
    if (state == kBatchMapping) {
      break;
    }
    if (state == kBatchMapped) {
      ++statistics_.batches_mapped;
      statistics_.batch_latency_frames += statistics_.frames - batch.frame;
      Apply(batch);
    } else {
      ++statistics_.batches_failed;
    }
    batch.state.store(kBatchIdle, std::memory_order_relaxed);
    batch.used = 0;
    batch.ranges.clear();
    free_.push_back(std::move(in_flight_.front()));
    in_flight_.pop_front();
  }
  statistics_.cp_ms += NowMs() - start_ms;
  statistics_.device_ms = double(g_readback_device_ns.load(std::memory_order_relaxed)) * 1.0e-6;

  int32_t log_frames = REXCVAR_GET(webgpu_readback_log_frames);
  if (log_frames > 0 && statistics_.frames % uint64_t(log_frames) == 0 &&
      statistics_.memexport_ranges) {
    const Statistics& s = statistics_;
    double frames = double(s.frames);
    uint64_t batches = s.batches_mapped + s.batches_failed;
    REXGPU_INFO(
        "WebGPU delayed readback: {} frames | memexport {} ranges ({:.1f} KiB/frame), {} "
        "skipped (batches in flight) | batches {} mapped, {} failed, "
        "latency {:.2f} frames | applied {:.1f} KiB/frame, pages left to the CPU {} (written "
        "since) + {} (re-uploaded) | buffers created {} | CP {:.3f} ms/frame, device {:.3f} "
        "ms/frame",
        s.frames, s.memexport_ranges, double(s.memexport_bytes) / 1024.0 / frames, s.ranges_skipped,
        s.batches_mapped, s.batches_failed,
        batches ? double(s.batch_latency_frames) / double(batches) : 0.0,
        double(s.bytes_applied) / 1024.0 / frames, s.pages_skipped_dirty,
        s.pages_skipped_replaced, s.buffers_created, s.cp_ms / frames, s.device_ms / frames);
  }
}

void WebGPUReadback::ReleaseBatch(std::unique_ptr<Batch> batch) {
  if (!batch) {
    return;
  }
  if (batch->state.exchange(kBatchAbandoned, std::memory_order_acq_rel) == kBatchMapping) {
    // Its map callback frees it.
    batch.release();
    return;
  }
  WGPUBuffer buffer = batch->buffer;
  if (buffer) {
    command_processor_.device().RunSync([buffer]() { wgpuBufferRelease(buffer); });
  }
}

void WebGPUReadback::Shutdown() {
  if (current_) {
    ReleaseBatch(std::move(current_));
  }
  while (!in_flight_.empty()) {
    ReleaseBatch(std::move(in_flight_.front()));
    in_flight_.pop_front();
  }
  for (std::unique_ptr<Batch>& batch : free_) {
    ReleaseBatch(std::move(batch));
  }
  free_.clear();
  memory_ = nullptr;
}

}  // namespace rex::graphics::webgpu
