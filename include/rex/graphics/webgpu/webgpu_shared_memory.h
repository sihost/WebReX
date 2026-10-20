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
#include <mutex>
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

  // --webgpu_verify_shared_memory: finds CPU-written pages whose guest bytes
  // changed since their upload with no dirty mark pending and no invalidation
  // since (a host writer that bypassed the web dirty map, docs/port/webgpu.md
  // section 8). Such pages are marked dirty and consumed, so the next request
  // uploads them again and textures/index caches on them are invalidated.
  // Returns the number of stale pages found. No-op when the cvar is off.
  uint32_t VerifyRange(uint32_t start, uint32_t length, const char* what, uint32_t id);
  bool verifying() const { return !page_hashes_.empty(); }
  uint64_t stale_pages() const { return stale_pages_; }
  uint64_t verified_pages() const { return verified_pages_; }

 protected:
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) override;

 private:
  static void VerifyGlobalWatchCallback(const std::unique_lock<std::recursive_mutex>& global_lock,
                                        void* context, uint32_t address_first,
                                        uint32_t address_last, bool invalidated_by_gpu);

  WebGPUCommandProcessor& command_processor_;
  TraceWriter& trace_writer_;
  WGPUBuffer buffer_ = nullptr;
  uint32_t binding_count_ = 1;
  uint64_t binding_size_ = kBufferSize;
  uint64_t uploaded_bytes_ = 0;
  uint64_t upload_count_ = 0;
  // Per page: XXH3 of the uploaded bytes | 1, 0 = not uploaded since the last
  // invalidation / GPU write (verify mode only).
  std::vector<uint64_t> page_hashes_;
  // Per 64-byte chunk: low 32 bits of XXH3 of the uploaded bytes (verify mode only), so a
  // stale page log says which bytes changed (a straddling store vs a bulk host writer).
  std::vector<uint32_t> chunk_hashes_;
  static constexpr uint32_t kVerifyChunkLog2 = 6;
  GlobalWatchHandle verify_watch_ = nullptr;
  uint64_t stale_pages_ = 0;
  uint64_t verified_pages_ = 0;
  uint32_t stale_logged_ = 0;
  // Probe (verify mode): per page, steady-clock ms of the last upload / invalidation, so a
  // stale log tells how long after the upload the unmarked write happened.
  std::vector<float> page_upload_ms_;
  std::vector<float> page_invalidate_ms_;
  // Changed unmarked pages whose mark arrived after the yield grace (a writer delayed between
  // its data store and its mark), not counted as stale.
  uint64_t late_marks_ = 0;
  uint32_t late_logged_ = 0;
};

}  // namespace rex::graphics::webgpu
