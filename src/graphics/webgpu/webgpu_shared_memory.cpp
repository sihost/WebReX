/**
 * @file        graphics/webgpu/webgpu_shared_memory.cpp
 * @brief       See rex/graphics/webgpu/webgpu_shared_memory.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/webgpu_shared_memory.h>

#include <algorithm>
#include <cstring>
#include <thread>

#include <rex/cvar.h>
#include <rex/graphics/webgpu/webgpu_command_processor.h>
#include <rex/graphics/webgpu/webgpu_device.h>
#include <rex/hash.h>
#include <rex/logging.h>
#if REX_PLATFORM_WEB
#include <rex/ppc/fold_web.h>
#endif

REXCVAR_DEFINE_BOOL(webgpu_verify_shared_memory, false, "GPU",
                    "Debug: hash every page uploaded to the WebGPU shared memory and recheck "
                    "the fetched vertex/index/texture pages at each draw; pages the CPU changed "
                    "without a dirty mark are logged, counted and re-uploaded");

namespace rex::graphics::webgpu {

WebGPUSharedMemory::WebGPUSharedMemory(WebGPUCommandProcessor& command_processor,
                                       memory::Memory& memory, TraceWriter& trace_writer)
    : SharedMemory(memory), command_processor_(command_processor), trace_writer_(trace_writer) {}

WebGPUSharedMemory::~WebGPUSharedMemory() {
  Shutdown(true);
}

bool WebGPUSharedMemory::Initialize() {
  InitializeCommon();

  WebGPUDevice& device = command_processor_.device();
  const WGPULimits& limits = device.limits();
  if (limits.maxBufferSize < kBufferSize) {
    REXGPU_ERROR(
        "WebGPU shared memory: maxBufferSize {} is below the {} MiB guest physical memory (the "
        "adapter does not offer it)",
        limits.maxBufferSize, kBufferSize >> 20);
    return false;
  }
  // Storage bindings: all of it in one if allowed, else equal power-of-two
  // ranges (so a binding index is address >> log2).
  binding_size_ = kBufferSize;
  while (binding_size_ > limits.maxStorageBufferBindingSize && binding_size_ > (1u << 20)) {
    binding_size_ >>= 1;
  }
  binding_count_ = uint32_t(kBufferSize / binding_size_);

  device.RunSync([&]() {
    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    desc.label = MakeStringView("rex shared memory");
    desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_Index | WGPUBufferUsage_Vertex |
                 WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst;
    desc.size = kBufferSize;
    buffer_ = wgpuDeviceCreateBuffer(device.device(), &desc);
  });
  if (!buffer_) {
    REXGPU_ERROR("WebGPU shared memory: creating the {} MiB buffer failed", kBufferSize >> 20);
    return false;
  }
  REXGPU_INFO("WebGPU shared memory: {} MiB buffer, {} storage binding(s) of {} MiB",
              kBufferSize >> 20, binding_count_, binding_size_ >> 20);
  if (REXCVAR_GET(webgpu_verify_shared_memory)) {
    page_hashes_.assign(kBufferSize >> page_size_log2(), 0);
    chunk_hashes_.assign(kBufferSize >> kVerifyChunkLog2, 0);
    verify_watch_ = RegisterGlobalWatch(VerifyGlobalWatchCallback, this);
    REXGPU_WARN("WebGPU shared memory: verify mode (every upload hashed, fetches rechecked)");
  }
  return true;
}

void WebGPUSharedMemory::VerifyGlobalWatchCallback(
    const std::unique_lock<std::recursive_mutex>& /*global_lock*/, void* context,
    uint32_t address_first, uint32_t address_last, bool /*invalidated_by_gpu*/) {
  // Invalidated by the CPU (consumed dirty pages) or written by the GPU: the
  // shared memory copy is no longer expected to equal the uploaded bytes.
  auto* self = static_cast<WebGPUSharedMemory*>(context);
  if (self->page_hashes_.empty()) {
    return;
  }
  uint32_t log2 = self->page_size_log2();
  uint32_t last = std::min<uint32_t>(address_last >> log2, uint32_t(self->page_hashes_.size()) - 1);
  for (uint32_t page = address_first >> log2; page <= last; ++page) {
    self->page_hashes_[page] = 0;
  }
}

uint32_t WebGPUSharedMemory::VerifyRange(uint32_t start, uint32_t length, const char* what,
                                         uint32_t id) {
  if (page_hashes_.empty() || !length || start >= kBufferSize) {
    return 0;
  }
  length = std::min(length, kBufferSize - start);
  const uint32_t log2 = page_size_log2();
  const uint32_t page_size = UINT32_C(1) << log2;
  uint32_t stale = 0;
  for (uint32_t page = start >> log2; page <= (start + length - 1) >> log2; ++page) {
    uint64_t expected = page_hashes_[page];
    if (!expected) {
      continue;
    }
    ++verified_pages_;
    uint32_t address = page << log2;
    uint64_t actual = XXH3_64bits(memory().TranslatePhysical(address), page_size) | 1;
    if (actual == expected) {
      continue;
    }
#if REX_PLATFORM_WEB
    // A pending dirty mark is a legitimate CPU write the next request consumes.
    bool marked = false;
    for (uint32_t offset = 0; offset < page_size; offset += fold::kDirtyPageSize) {
      marked |= fold::g_phys_dirty[(address + offset) >> fold::kDirtyPageShift] != 0;
    }
    if (!marked) {
      // Guest stores mark AFTER the data (fold_web.h), so a store caught between its data
      // and its mark looks unmarked for a moment: give it a short grace period before
      // calling the page stale.
      for (int spin = 0; spin < 64 && !marked; ++spin) {
        std::this_thread::yield();
        for (uint32_t offset = 0; offset < page_size; offset += fold::kDirtyPageSize) {
          marked |= __atomic_load_n(&fold::g_phys_dirty[(address + offset) >> fold::kDirtyPageShift],
                                    __ATOMIC_ACQUIRE) != 0;
        }
      }
    }
    if (marked) {
      continue;
    }
#endif
    ++stale;
    ++stale_pages_;
    if (stale_logged_ < 256) {
      ++stale_logged_;
      // Which 64-byte chunks changed: only the first bytes = a store straddling from the
      // previous page (the store macros mark the page of the first byte only); the whole
      // page = a bulk host writer (memcpy / file read / DMA).
      const uint8_t* host = memory().TranslatePhysical(address);
      uint32_t changed = 0, first_changed = UINT32_MAX, last_changed = 0;
      const uint32_t chunks = page_size >> kVerifyChunkLog2;
      for (uint32_t chunk = 0; chunk < chunks; ++chunk) {
        uint32_t h = uint32_t(XXH3_64bits(host + (size_t(chunk) << kVerifyChunkLog2),
                                          size_t(1) << kVerifyChunkLog2));
        if (h != chunk_hashes_[(address >> kVerifyChunkLog2) + chunk]) {
          ++changed;
          first_changed = std::min(first_changed, chunk);
          last_changed = chunk;
        }
      }
      // The guest allocation behind it (physical views 0xA0 / 0xC0 / 0xE0).
      uint32_t alloc_base = 0, alloc_size = 0, alloc_protect = 0;
      const uint32_t views[3] = {0xA0000000u | address, 0xC0000000u | address,
                                 address >= 0x1000 ? 0xE0000000u + address - 0x1000 : 0};
      for (uint32_t view : views) {
        memory::BaseHeap* heap = view ? memory().LookupHeap(view) : nullptr;
        memory::HeapAllocationInfo info{};
        if (heap && heap->QueryRegionInfo(view, &info) &&
            (info.state & memory::kMemoryAllocationCommit)) {
          alloc_base = info.allocation_base;
          alloc_size = info.allocation_size;
          alloc_protect = info.allocation_protect;
          break;
        }
      }
      bool prev_marked = false;
#if REX_PLATFORM_WEB
      prev_marked = address >= page_size &&
                    fold::g_phys_dirty[(address - 1) >> fold::kDirtyPageShift] != 0;
#endif
      REXGPU_WARN(
          "WebGPU shared memory verify: page {:08X} ({} {}) changed since its upload without a "
          "dirty mark (stale GPU copy); re-uploading. {}/{} 64 B chunks changed (bytes "
          "0x{:X}-0x{:X}), prev page dirty {}, alloc {:08X}+0x{:X} prot 0x{:X}, verify draw {}",
          address, what, id, changed, chunks,
          changed ? first_changed << kVerifyChunkLog2 : 0,
          changed ? ((last_changed + 1) << kVerifyChunkLog2) - 1 : 0, prev_marked, alloc_base,
          alloc_size, alloc_protect, verified_pages_);
    }
    page_hashes_[page] = 0;
#if REX_PLATFORM_WEB
    memory().MarkPhysicalDirtyPhysical(address, page_size);
    memory().ConsumePhysicalDirty(address, page_size);
#endif
  }
  return stale;
}

void WebGPUSharedMemory::Shutdown(bool from_destructor) {
  if (verify_watch_) {
    UnregisterGlobalWatch(verify_watch_);
    verify_watch_ = nullptr;
  }
  page_hashes_.clear();
  if (buffer_) {
    WGPUBuffer buffer = buffer_;
    buffer_ = nullptr;
    // Recorded streams referencing it are replayed before this task runs
    // (proxied tasks run in order); submitted GPU work keeps it alive.
    command_processor_.device().RunSync([buffer]() {
      wgpuBufferDestroy(buffer);
      wgpuBufferRelease(buffer);
    });
  }
  // From the destructor, the base class destructor calls ShutdownCommon.
  if (!from_destructor) {
    ShutdownCommon();
  }
}

bool WebGPUSharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  if (upload_page_ranges.empty()) {
    return true;
  }
  if (!buffer_) {
    return false;
  }
  const uint32_t page_size_log2_value = page_size_log2();
  const uint32_t chunk_pages = std::max(uint32_t(1), kMaxUploadChunkBytes >> page_size_log2_value);
  for (const std::pair<uint32_t, uint32_t>& upload_range : upload_page_ranges) {
    uint32_t page_start = upload_range.first;
    uint32_t page_count = upload_range.second;
    trace_writer_.WriteMemoryRead(page_start << page_size_log2_value,
                                  size_t(page_count) << page_size_log2_value);
    while (page_count) {
      uint32_t pages = std::min(page_count, chunk_pages);
      uint32_t start = page_start << page_size_log2_value;
      uint32_t length = pages << page_size_log2_value;
      void* upload = command_processor_.RecordUpload(buffer_, start, length);
      if (!upload) {
        REXGPU_ERROR("WebGPU shared memory: could not record an upload of 0x{:X} bytes at 0x{:08X}",
                     length, start);
        return false;
      }
      // Valid before the copy, so an invalidation during the copy isn't lost
      // (the dirty map is set after guest stores, consumed by the next request).
      MakeRangeValid(start, length, false);
      std::memcpy(upload, memory().TranslatePhysical(start), length);
      if (!page_hashes_.empty()) {
        for (uint32_t page = 0; page < pages; ++page) {
          page_hashes_[page_start + page] =
              XXH3_64bits(static_cast<const uint8_t*>(upload) +
                              (size_t(page) << page_size_log2_value),
                          size_t(1) << page_size_log2_value) |
              1;
        }
        if (!chunk_hashes_.empty()) {
          const uint32_t chunk_count = length >> kVerifyChunkLog2;
          for (uint32_t chunk = 0; chunk < chunk_count; ++chunk) {
            chunk_hashes_[(start >> kVerifyChunkLog2) + chunk] = uint32_t(
                XXH3_64bits(static_cast<const uint8_t*>(upload) +
                                (size_t(chunk) << kVerifyChunkLog2),
                            size_t(1) << kVerifyChunkLog2));
          }
        }
      }
      uploaded_bytes_ += length;
      ++upload_count_;
      page_start += pages;
      page_count -= pages;
    }
  }
  return true;
}

}  // namespace rex::graphics::webgpu
