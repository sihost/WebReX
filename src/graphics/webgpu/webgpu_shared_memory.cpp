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

#include <rex/graphics/webgpu/webgpu_command_processor.h>
#include <rex/graphics/webgpu/webgpu_device.h>
#include <rex/logging.h>

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
  return true;
}

void WebGPUSharedMemory::Shutdown(bool from_destructor) {
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
      uploaded_bytes_ += length;
      ++upload_count_;
      page_start += pages;
      page_count -= pages;
    }
  }
  return true;
}

}  // namespace rex::graphics::webgpu
