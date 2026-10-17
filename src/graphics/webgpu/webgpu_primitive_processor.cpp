/**
 * @file        graphics/webgpu/webgpu_primitive_processor.cpp
 * @brief       See rex/graphics/webgpu/webgpu_primitive_processor.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/webgpu_primitive_processor.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

#include <rex/graphics/webgpu/webgpu_command_stream.h>
#include <rex/graphics/webgpu/webgpu_device.h>
#include <rex/graphics/webgpu/webgpu_log.h>
#include <rex/graphics/webgpu/webgpu_submission_tracker.h>
#include <rex/math.h>

namespace rex::graphics::webgpu {

namespace {

WGPUBuffer CreateIndexBuffer(WebGPUDevice& device, uint64_t size, const char* label) {
  WGPUBuffer buffer = nullptr;
  device.RunSync([&]() {
    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    desc.label = MakeStringView(label);
    desc.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
    desc.size = size;
    buffer = wgpuDeviceCreateBuffer(device.device(), &desc);
  });
  return buffer;
}

void DestroyBuffer(WebGPUDevice& device, WGPUBuffer buffer) {
  if (!buffer) {
    return;
  }
  // Proxied tasks run in order, so streams recorded before this are replayed
  // first; submitted GPU work keeps the buffer alive.
  device.RunSync([buffer]() {
    wgpuBufferDestroy(buffer);
    wgpuBufferRelease(buffer);
  });
}

}  // namespace

WebGPUPrimitiveProcessor::WebGPUPrimitiveProcessor(const RegisterFile& register_file,
                                                   memory::Memory& memory,
                                                   TraceWriter& trace_writer,
                                                   SharedMemory& shared_memory,
                                                   WebGPUCommandQueue& queue,
                                                   std::function<void()> end_render_pass,
                                                   WGPUBuffer shared_memory_buffer)
    : PrimitiveProcessor(register_file, memory, trace_writer, shared_memory),
      queue_(queue),
      end_render_pass_(std::move(end_render_pass)),
      shared_memory_buffer_(shared_memory_buffer) {}

WebGPUPrimitiveProcessor::~WebGPUPrimitiveProcessor() {
  Shutdown(true);
}

bool WebGPUPrimitiveProcessor::Initialize() {
  // docs/port/webgpu.md section 4: no geometry shaders, no fans / loops / quads,
  // full 32-bit indices (WebGPU drawIndexed takes any uint32 index).
  if (!InitializeCommon(true, false, false, false, false, false)) {
    Shutdown();
    return false;
  }
  REXWGPU_INFO("WebGPU primitive processor: built-in index buffer {} KiB",
               builtin_index_buffer_size_ >> 10);
  return true;
}

void WebGPUPrimitiveProcessor::Shutdown(bool from_destructor) {
  WebGPUDevice& device = queue_.device();
  frame_index_buffers_.clear();
  for (auto& slot : frame_chunks_) {
    for (auto& chunk : slot) {
      DestroyBuffer(device, chunk->buffer);
    }
    slot.clear();
  }
  frame_last_submission_ = {};
  frame_slot_ = 0;
  frame_chunk_ = 0;
  stream_writes_.clear();
  stream_writes_free_.clear();
  DestroyBuffer(device, builtin_index_buffer_);
  builtin_index_buffer_ = nullptr;
  builtin_index_buffer_size_ = 0;
  if (!from_destructor) {
    ShutdownCommon();
  }
}

void WebGPUPrimitiveProcessor::EndFrame() {
  ClearPerFrameCache();
  frame_index_buffers_.clear();
  frame_slot_ = (frame_slot_ + 1) % kFrameSlots;
  frame_chunk_ = 0;
  // The CP bounds latency to fewer frames than kFrameSlots, so this normally
  // returns at once; it makes chunk reuse safe on its own. GPU-side reuse is
  // ordered by the queue (writeBuffer after the earlier submits).
  uint64_t last_submission = frame_last_submission_[frame_slot_];
  WebGPUSubmissionTracker& tracker = queue_.tracker();
  if (last_submission && tracker.replayed() < last_submission &&
      !tracker.AwaitReplayed(last_submission)) {
    REXWGPU_WARN("WebGPU primitive processor: submission {} not replayed, reusing its indices",
                 last_submission);
  }
  frame_last_submission_[frame_slot_] = 0;
  for (auto& chunk : frame_chunks_[frame_slot_]) {
    chunk->used = 0;
  }
}

bool WebGPUPrimitiveProcessor::GetIndexBufferBinding(const ProcessingResult& result,
                                                     IndexBufferBinding& out) const {
  uint64_t index_size = result.host_index_format == xenos::IndexFormat::kInt16 ? 2 : 4;
  out.format = result.host_index_format == xenos::IndexFormat::kInt16 ? WGPUIndexFormat_Uint16
                                                                      : WGPUIndexFormat_Uint32;
  out.size = index_size * result.host_draw_vertex_count;
  switch (result.index_buffer_type) {
    case ProcessedIndexBufferType::kNone:
      return false;
    case ProcessedIndexBufferType::kGuestDMA:
      out.buffer = shared_memory_buffer_;
      out.offset = result.guest_index_base;
      break;
    case ProcessedIndexBufferType::kHostConverted:
      if (result.host_index_buffer_handle >= frame_index_buffers_.size()) {
        return false;
      }
      out.buffer = frame_index_buffers_[result.host_index_buffer_handle].first;
      out.offset = frame_index_buffers_[result.host_index_buffer_handle].second;
      break;
    case ProcessedIndexBufferType::kHostBuiltinForAuto:
    case ProcessedIndexBufferType::kHostBuiltinForDMA:
      out.buffer = builtin_index_buffer_;
      out.offset = GetBuiltinIndexBufferOffsetBytes(result.host_index_buffer_handle);
      break;
  }
  return out.buffer != nullptr;
}

WGPUPrimitiveTopology WebGPUPrimitiveProcessor::GetTopology(
    xenos::PrimitiveType host_primitive_type) {
  switch (host_primitive_type) {
    case xenos::PrimitiveType::kPointList:
      return WGPUPrimitiveTopology_PointList;
    case xenos::PrimitiveType::kLineList:
      return WGPUPrimitiveTopology_LineList;
    case xenos::PrimitiveType::kLineStrip:
      return WGPUPrimitiveTopology_LineStrip;
    case xenos::PrimitiveType::kTriangleList:
      return WGPUPrimitiveTopology_TriangleList;
    case xenos::PrimitiveType::kTriangleStrip:
      return WGPUPrimitiveTopology_TriangleStrip;
    default:
      // Rectangle / quad lists, fans and loops never reach the host (Initialize
      // disables them); tessellated types aren't supported on WebGPU.
      return WGPUPrimitiveTopology_Undefined;
  }
}

WGPUIndexFormat WebGPUPrimitiveProcessor::GetStripIndexFormat(const ProcessingResult& result) {
  // WebGPU rejects indexed draws with a strip pipeline whose stripIndexFormat
  // is undefined, and restarts strips at the format's maximum value whenever it
  // is set: there is no "restart disabled" state for indexed strips. So every
  // indexed strip gets the format, whether or not the guest enabled the reset.
  // (A reset-disabled guest strip that uses vertex 0xFFFF / 0xFFFFFFFF as a real
  // index would restart there; not handled.)
  if (result.index_buffer_type == ProcessedIndexBufferType::kNone) {
    return WGPUIndexFormat_Undefined;
  }
  WGPUPrimitiveTopology topology = GetTopology(result.host_primitive_type);
  if (topology != WGPUPrimitiveTopology_LineStrip &&
      topology != WGPUPrimitiveTopology_TriangleStrip) {
    return WGPUIndexFormat_Undefined;
  }
  return result.host_index_format == xenos::IndexFormat::kInt16 ? WGPUIndexFormat_Uint16
                                                                : WGPUIndexFormat_Uint32;
}

bool WebGPUPrimitiveProcessor::GetMemExportComputeDispatch(uint32_t vertex_count,
                                                           uint32_t& groups_x,
                                                           uint32_t& groups_y) {
  // Must match SpirvShaderTranslator::kMemExportComputeDispatchX (the header is
  // not included here, see ApplyIndexSystemConstants).
  constexpr uint32_t kDispatchX = 65535;
  if (!vertex_count) {
    groups_x = groups_y = 0;
    return false;
  }
  groups_x = std::min(vertex_count, kDispatchX);
  groups_y = (vertex_count + kDispatchX - 1) / kDispatchX;
  return true;
}

bool WebGPUPrimitiveProcessor::InitializeBuiltinIndexBuffer(
    size_t size_bytes, std::function<void(void*)> fill_callback) {
  assert_not_zero(size_bytes);
  assert_null(builtin_index_buffer_);
  uint64_t size = rex::align(uint64_t(size_bytes), uint64_t(4));
  std::vector<uint8_t> data(size_t(size), 0);
  fill_callback(data.data());
  WebGPUDevice& device = queue_.device();
  builtin_index_buffer_ = CreateIndexBuffer(device, size, "rex built-in index buffer");
  if (!builtin_index_buffer_) {
    REXWGPU_ERROR("WebGPU primitive processor: creating the {}-byte built-in index buffer failed",
                  size);
    return false;
  }
  // Queue-ordered before every later submission.
  WGPUBuffer buffer = builtin_index_buffer_;
  device.RunSync([&]() { wgpuQueueWriteBuffer(device.queue(), buffer, 0, data.data(), size); });
  builtin_index_buffer_size_ = size;
  return true;
}

WebGPUPrimitiveProcessor::Chunk* WebGPUPrimitiveProcessor::AllocateChunk(uint32_t min_size) {
  auto chunk = std::make_unique<Chunk>();
  chunk->size = std::max(kChunkSize, rex::align(min_size, uint32_t(4)));
  chunk->data.reset(new (std::nothrow) uint8_t[chunk->size]);
  if (!chunk->data) {
    return nullptr;
  }
  chunk->buffer = CreateIndexBuffer(queue_.device(), chunk->size, "rex converted indices");
  if (!chunk->buffer) {
    REXWGPU_ERROR("WebGPU primitive processor: creating a {}-byte index chunk failed",
                  chunk->size);
    return nullptr;
  }
  ++statistics_.chunks;
  std::vector<std::unique_ptr<Chunk>>& slot = frame_chunks_[frame_slot_];
  slot.push_back(std::move(chunk));
  return slot.back().get();
}

WebGPUPrimitiveProcessor::StreamWrites* WebGPUPrimitiveProcessor::GetCurrentStreamWrites() {
  WebGPUSubmissionTracker& tracker = queue_.tracker();
  // The stream being recorded gets the next submission id.
  uint64_t submission = tracker.submitted() + 1;
  if (!stream_writes_.empty() && stream_writes_.back()->submission == submission) {
    return stream_writes_.back().get();
  }
  // Recycle the lists of replayed streams (their callbacks have run).
  uint64_t replayed = tracker.replayed();
  while (!stream_writes_.empty() && stream_writes_.front()->submission <= replayed) {
    stream_writes_.front()->writes.clear();
    stream_writes_free_.push_back(std::move(stream_writes_.front()));
    stream_writes_.pop_front();
  }
  std::unique_ptr<StreamWrites> writes;
  if (!stream_writes_free_.empty()) {
    writes = std::move(stream_writes_free_.back());
    stream_writes_free_.pop_back();
  } else {
    writes = std::make_unique<StreamWrites>();
  }
  writes->submission = submission;
  // The callback reads the list when the stream is replayed, after recording
  // (and thus every conversion of this stream) is done. Callbacks are recorded
  // outside passes; Process runs before the draw opens its pass, so at most one
  // pass per stream is split here.
  WebGPUCommandStream& stream = queue_.stream();
  if (stream.in_pass()) {
    end_render_pass_();
    ++statistics_.upload_stream_pass_breaks;
  }
  stream.CmdEncoderCallback(WriteStreamIndices, writes.get());
  ++statistics_.upload_streams;
  stream_writes_.push_back(std::move(writes));
  return stream_writes_.back().get();
}

void WebGPUPrimitiveProcessor::WriteStreamIndices(void* user, WebGPUReplayState& state,
                                                  WGPUCommandEncoder /*encoder*/) {
  // Device thread, while the stream is encoded: writeBuffer is queue-ordered
  // before this stream's submit, so all its draws see the indices.
  auto* writes = static_cast<StreamWrites*>(user);
  WGPUQueue queue = state.device().queue();
  for (const StreamWrites::Write& write : writes->writes) {
    wgpuQueueWriteBuffer(queue, write.buffer, write.offset, write.data, write.size);
  }
}

void* WebGPUPrimitiveProcessor::RequestHostConvertedIndexBufferForCurrentFrame(
    xenos::IndexFormat format, uint32_t index_count, bool coalign_for_simd,
    uint32_t coalignment_original_address, size_t& backend_handle_out) {
  uint32_t index_size = format == xenos::IndexFormat::kInt16 ? sizeof(uint16_t) : sizeof(uint32_t);
  uint32_t padding = coalign_for_simd ? uint32_t(XE_GPU_PRIMITIVE_PROCESSOR_SIMD_SIZE) : 0;
  uint32_t size = rex::align(index_size * index_count + padding, uint32_t(4));
  if (!size) {
    size = 4;
  }

  std::vector<std::unique_ptr<Chunk>>& slot = frame_chunks_[frame_slot_];
  Chunk* chunk = nullptr;
  while (frame_chunk_ < slot.size()) {
    Chunk* candidate = slot[frame_chunk_].get();
    if (candidate->size - candidate->used >= size) {
      chunk = candidate;
      break;
    }
    ++frame_chunk_;
  }
  if (!chunk) {
    chunk = AllocateChunk(size);
    if (!chunk) {
      return nullptr;
    }
    frame_chunk_ = slot.size() - 1;
  }

  StreamWrites* stream_writes = GetCurrentStreamWrites();
  uint32_t offset = chunk->used;
  chunk->used += size;
  uint8_t* mapping = chunk->data.get() + offset;
  uint32_t binding_offset = offset;
  if (coalign_for_simd) {
    ptrdiff_t coalignment_offset = GetSimdCoalignmentOffset(mapping, coalignment_original_address);
    mapping += coalignment_offset;
    binding_offset += uint32_t(coalignment_offset);
  }
  // The whole 4-aligned allocation is written (writeBuffer needs 4-byte
  // offsets and sizes); merge with the previous write of the stream if adjacent.
  if (!stream_writes->writes.empty() && stream_writes->writes.back().buffer == chunk->buffer &&
      stream_writes->writes.back().offset + stream_writes->writes.back().size == offset) {
    stream_writes->writes.back().size += size;
  } else {
    stream_writes->writes.push_back({chunk->buffer, chunk->data.get() + offset, offset, size});
  }
  frame_last_submission_[frame_slot_] = stream_writes->submission;

  ++statistics_.converted_buffers;
  statistics_.converted_bytes += size;
  backend_handle_out = frame_index_buffers_.size();
  frame_index_buffers_.emplace_back(chunk->buffer, binding_offset);
  return mapping;
}

}  // namespace rex::graphics::webgpu
