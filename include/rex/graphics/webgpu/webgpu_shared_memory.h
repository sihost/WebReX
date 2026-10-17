/**
 * @file        rex/graphics/webgpu/webgpu_shared_memory.h
 * @brief       Guest physical memory mirror in one 512 MiB WebGPU storage buffer.
 *
 * docs/port/webgpu.md sections 8 and 9. WebGPU has no sparse binding, so the
 * whole buffer is created up front (maxBufferSize >= 512 MiB is requested from
 * the adapter). If maxStorageBufferBindingSize is smaller, shaders bind it as
 * binding_count() ranges of binding_size() bytes.
 *
 * Uploads: RequestRange (shared SharedMemory) consumes the web dirty map
 * (Memory::ConsumePhysicalDirty) and then calls UploadRanges for the invalid
 * pages, which copies them from guest memory into the command stream's upload
 * arena at record time (so later CPU writes cannot leak into this draw) and
 * records a staging -> shared memory copy at that point of the stream.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include <webgpu/webgpu.h>

#include <rex/graphics/shared_memory.h>
#include <rex/graphics/trace_writer.h>
#include <rex/memory.h>

namespace rex::graphics::webgpu {

class WebGPUCommandProcessor;

class WebGPUSharedMemory : public SharedMemory {
 public:
  // Upload chunk: a single stream upload is at most this large, so the command
  // processor can flush a stream between chunks.
  static constexpr uint32_t kMaxUploadChunkBytes = 4 * 1024 * 1024;

  WebGPUSharedMemory(WebGPUCommandProcessor& command_processor, memory::Memory& memory,
                     TraceWriter& trace_writer);
  ~WebGPUSharedMemory() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);

  // Device-thread handle (Storage | Index | Vertex | CopySrc | CopyDst).
  WGPUBuffer buffer() const { return buffer_; }
  uint32_t binding_count() const { return binding_count_; }
  uint64_t binding_size() const { return binding_size_; }

  uint64_t uploaded_bytes() const { return uploaded_bytes_; }
  uint64_t upload_count() const { return upload_count_; }

 protected:
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) override;

 private:
  WebGPUCommandProcessor& command_processor_;
  TraceWriter& trace_writer_;
  WGPUBuffer buffer_ = nullptr;
  uint32_t binding_count_ = 1;
  uint64_t binding_size_ = kBufferSize;
  uint64_t uploaded_bytes_ = 0;
  uint64_t upload_count_ = 0;
};

}  // namespace rex::graphics::webgpu
