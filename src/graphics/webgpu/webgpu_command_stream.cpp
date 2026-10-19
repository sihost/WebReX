/**
 * @file        graphics/webgpu/webgpu_command_stream.cpp
 * @brief       WebGPU command stream recording / replay and stream submission.
 *
 * See rex/graphics/webgpu/webgpu_command_stream.h and docs/port/webgpu.md.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/webgpu_command_stream.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <utility>

#include <emscripten/emscripten.h>

#include <rex/graphics/webgpu/webgpu_device.h>
#include <rex/graphics/webgpu/webgpu_log.h>
#include <rex/graphics/webgpu/webgpu_submission_tracker.h>

namespace rex::graphics::webgpu {

// ---------------------------------------------------------------------------
// Record layouts.
// ---------------------------------------------------------------------------

enum class WebGPUCommandStream::Command : uint32_t {
  kBeginRenderPass,
  kEndRenderPass,
  kBeginComputePass,
  kEndComputePass,
  kSetPipelineId,
  kSetRenderPipeline,
  kSetVertexBuffer,
  kSetIndexBuffer,
  kSetViewport,
  kSetScissorRect,
  kSetBlendConstant,
  kSetStencilReference,
  kDraw,
  kDrawIndexed,
  kSetComputePipelineId,
  kSetComputePipeline,
  kDispatch,
  kSetBindGroup,
  kSetBindGroupDesc,
  kCopyBufferToBuffer,
  kCopyBufferToTexture,
  kCopyTextureToBuffer,
  kCopyTextureToTexture,
  kClearBuffer,
  kUploadToBuffer,
  kUploadToTexture,
  kEncoderCallback,
};

namespace {

struct CommandHeader {
  uint32_t command;
  // Whole record size in bytes (header included), a multiple of 8.
  uint32_t size;
};
static_assert(sizeof(CommandHeader) == 8);

struct ArgsBeginRenderPass {
  uint32_t color_count;
  uint32_t has_depth_stencil;
  WGPUQuerySet occlusion_query_set;
  // Followed by color_count WGPURenderPassColorAttachment, then one
  // WGPURenderPassDepthStencilAttachment if has_depth_stencil.
};

struct ArgsHandle {
  const void* handle;
};

struct ArgsId {
  uint32_t id;
};

struct ArgsSetVertexBuffer {
  uint32_t slot;
  WGPUBuffer buffer;
  uint64_t offset;
  uint64_t size;
};

struct ArgsSetIndexBuffer {
  WGPUBuffer buffer;
  WGPUIndexFormat format;
  uint64_t offset;
  uint64_t size;
};

struct ArgsSetViewport {
  float x, y, width, height, min_depth, max_depth;
};

struct ArgsSetScissorRect {
  uint32_t x, y, width, height;
};

struct ArgsSetBlendConstant {
  WGPUColor color;
};

struct ArgsDraw {
  uint32_t vertex_count, instance_count, first_vertex, first_instance;
};

struct ArgsDrawIndexed {
  uint32_t index_count, instance_count, first_index;
  int32_t base_vertex;
  uint32_t first_instance;
};

struct ArgsDispatch {
  uint32_t x, y, z;
};

struct ArgsSetBindGroup {
  uint32_t group_index;
  uint32_t dynamic_offset_count;
  WGPUBindGroup group;
  // Followed by dynamic_offset_count uint32_t.
};

struct ArgsSetBindGroupDesc {
  uint32_t group_index;
  uint32_t entry_count;
  uint32_t dynamic_offset_count;
  uint32_t reserved;
  uint64_t hash;
  // Compared byte-wise for deduplication: layout + entries.
  WGPUBindGroupLayout layout;
  // Followed by entry_count WGPUBindGroupEntry (padding zeroed), then
  // dynamic_offset_count uint32_t.
};

struct ArgsCopyBufferToBuffer {
  WGPUBuffer source;
  uint64_t source_offset;
  WGPUBuffer destination;
  uint64_t destination_offset;
  uint64_t size;
};

struct ArgsCopyBufferTexture {
  WGPUTexelCopyBufferInfo buffer;
  WGPUTexelCopyTextureInfo texture;
  WGPUExtent3D size;
};

struct ArgsCopyTextureToTexture {
  WGPUTexelCopyTextureInfo source;
  WGPUTexelCopyTextureInfo destination;
  WGPUExtent3D size;
};

struct ArgsClearBuffer {
  WGPUBuffer buffer;
  uint64_t offset;
  uint64_t size;
};

struct ArgsUploadToBuffer {
  WGPUBuffer destination;
  uint64_t destination_offset;
  uint64_t upload_offset;
  uint64_t size;
};

struct ArgsUploadToTexture {
  WGPUTexelCopyTextureInfo destination;
  WGPUExtent3D size;
  uint64_t upload_offset;
  uint32_t bytes_per_row;
  uint32_t rows_per_image;
};

struct ArgsEncoderCallback {
  WebGPUCommandStream::EncoderCallback callback;
  void* user;
};

uint64_t HashBytes(const void* data, size_t size, uint64_t hash = 0xCBF29CE484222325ull) {
  // FNV-1a over 8-byte words (records are 8-byte aligned and sized).
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  size_t i = 0;
  for (; i + 8 <= size; i += 8) {
    uint64_t word;
    std::memcpy(&word, bytes + i, 8);
    hash = (hash ^ word) * 0x100000001B3ull;
  }
  for (; i < size; ++i) {
    hash = (hash ^ bytes[i]) * 0x100000001B3ull;
  }
  return hash;
}

constexpr uint64_t AlignUp(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

WGPUTexelCopyTextureInfo SanitizeTextureInfo(const WGPUTexelCopyTextureInfo& info) {
  WGPUTexelCopyTextureInfo result = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
  result.texture = info.texture;
  result.mipLevel = info.mipLevel;
  result.origin = info.origin;
  result.aspect = info.aspect;
  return result;
}

WGPUTexelCopyBufferInfo SanitizeBufferInfo(const WGPUTexelCopyBufferInfo& info) {
  WGPUTexelCopyBufferInfo result = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
  result.layout = info.layout;
  result.buffer = info.buffer;
  return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// WebGPUPipelineTable.
// ---------------------------------------------------------------------------

void WebGPUPipelineTable::SetRender(PipelineId id, WGPURenderPipeline pipeline) {
  if (id == kInvalidPipelineId) {
    if (pipeline) {
      wgpuRenderPipelineRelease(pipeline);
    }
    return;
  }
  if (id >= render_.size()) {
    render_.resize(std::max<size_t>(id + 1, render_.size() * 2), nullptr);
  }
  if (render_[id]) {
    wgpuRenderPipelineRelease(render_[id]);
  }
  render_[id] = pipeline;
}

void WebGPUPipelineTable::SetCompute(PipelineId id, WGPUComputePipeline pipeline) {
  if (id == kInvalidPipelineId) {
    if (pipeline) {
      wgpuComputePipelineRelease(pipeline);
    }
    return;
  }
  if (id >= compute_.size()) {
    compute_.resize(std::max<size_t>(id + 1, compute_.size() * 2), nullptr);
  }
  if (compute_[id]) {
    wgpuComputePipelineRelease(compute_[id]);
  }
  compute_[id] = pipeline;
}

void WebGPUPipelineTable::Release() {
  for (WGPURenderPipeline pipeline : render_) {
    if (pipeline) {
      wgpuRenderPipelineRelease(pipeline);
    }
  }
  render_.clear();
  for (WGPUComputePipeline pipeline : compute_) {
    if (pipeline) {
      wgpuComputePipelineRelease(pipeline);
    }
  }
  compute_.clear();
}

// ---------------------------------------------------------------------------
// WebGPUReplayState.
// ---------------------------------------------------------------------------

WebGPUReplayState::WebGPUReplayState(WebGPUDevice& device, WebGPUPipelineTable* pipelines)
    : device_(device), pipelines_(pipelines) {}

WebGPUReplayState::~WebGPUReplayState() {
  if (staging_buffer_) {
    wgpuBufferRelease(staging_buffer_);
  }
  if (constant_buffer_) {
    wgpuBufferRelease(constant_buffer_);
  }
}

bool WebGPUReplayState::EnsureBuffer(WGPUBuffer& buffer, uint64_t& buffer_size, uint64_t needed,
                                     WGPUBufferUsage usage, const char* label) {
  if (buffer && buffer_size >= needed) {
    return true;
  }
  // Grow geometrically (whole MiBs) so steady state never reallocates.
  uint64_t new_size = std::max<uint64_t>(AlignUp(needed, 1 << 20), buffer_size * 2);
  new_size = std::min<uint64_t>(new_size, device_.limits().maxBufferSize);
  if (new_size < needed) {
    REXWGPU_ERROR("WebGPU replay: {} needs {} bytes, above maxBufferSize {}", label, needed,
                  device_.limits().maxBufferSize);
    return false;
  }
  if (buffer) {
    // Already-submitted work keeps the old buffer alive.
    wgpuBufferRelease(buffer);
    buffer = nullptr;
    buffer_size = 0;
  }
  WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
  desc.label = MakeStringView(label);
  desc.usage = usage;
  desc.size = new_size;
  buffer = wgpuDeviceCreateBuffer(device_.device(), &desc);
  if (!buffer) {
    REXWGPU_ERROR("WebGPU replay: creating the {} ({} bytes) failed", label, new_size);
    return false;
  }
  buffer_size = new_size;
  return true;
}

// ---------------------------------------------------------------------------
// WebGPUCommandStream: recording.
// ---------------------------------------------------------------------------

size_t WebGPUCommandStream::Arena::Allocate(size_t bytes, size_t alignment) {
  size_t offset = (size + alignment - 1) & ~(alignment - 1);
  size_t new_size = offset + bytes;
  if (new_size > capacity) {
    size_t new_capacity = std::max<size_t>(std::max<size_t>(capacity * 2, 64 * 1024), new_size);
    auto* new_data = static_cast<uint8_t*>(std::realloc(data, new_capacity));
    if (!new_data) {
      throw std::bad_alloc();
    }
    data = new_data;
    capacity = new_capacity;
  }
  size = new_size;
  return offset;
}

void WebGPUCommandStream::Arena::Free() {
  std::free(data);
  data = nullptr;
  size = 0;
  capacity = 0;
}

WebGPUCommandStream::~WebGPUCommandStream() {
  commands_.Free();
  uploads_.Free();
  constants_.Free();
}

void WebGPUCommandStream::Reset() {
  commands_.size = 0;
  uploads_.size = 0;
  constants_.size = 0;
  submitted_callbacks_.clear();
  pass_ = Pass::kNone;
  recorded_commands_ = 0;
  dropped_commands_ = 0;
}

void* WebGPUCommandStream::Record(Command command, size_t payload_bytes) {
  size_t total = sizeof(CommandHeader) + AlignUp(payload_bytes, 8);
  size_t offset = commands_.Allocate(total, 8);
  auto* header = reinterpret_cast<CommandHeader*>(commands_.data + offset);
  header->command = uint32_t(command);
  header->size = uint32_t(total);
  ++recorded_commands_;
  return header + 1;
}

bool WebGPUCommandStream::Require(Pass pass, const char* what) {
  if (pass_ == pass) {
    return true;
  }
  ++dropped_commands_;
  // Log the first few only: a broken caller tends to repeat every frame.
  static std::atomic<uint32_t> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
    static const char* const kPassNames[] = {"outside passes", "in a render pass",
                                             "in a compute pass"};
    REXWGPU_ERROR("WebGPU stream: {} dropped: it must be recorded {}, but the stream is {}", what,
                  kPassNames[uint32_t(pass)], kPassNames[uint32_t(pass_)]);
  }
  return false;
}

void WebGPUCommandStream::RenderPassDesc::SetColor(uint32_t index, WGPUTextureView view,
                                                   bool clear, WGPUColor clear_value) {
  if (index >= kMaxColorAttachments) {
    return;
  }
  WGPURenderPassColorAttachment& attachment = color_attachments[index];
  attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
  attachment.view = view;
  attachment.loadOp = clear ? WGPULoadOp_Clear : WGPULoadOp_Load;
  attachment.storeOp = WGPUStoreOp_Store;
  attachment.clearValue = clear_value;
  color_attachment_count = std::max(color_attachment_count, index + 1);
}

void WebGPUCommandStream::CmdBeginRenderPass(const RenderPassDesc& desc) {
  if (!Require(Pass::kNone, "BeginRenderPass")) {
    return;
  }
  uint32_t color_count = std::min(desc.color_attachment_count, RenderPassDesc::kMaxColorAttachments);
  size_t size = sizeof(ArgsBeginRenderPass) + sizeof(WGPURenderPassColorAttachment) * color_count +
                (desc.has_depth_stencil ? sizeof(WGPURenderPassDepthStencilAttachment) : 0);
  auto* args = static_cast<ArgsBeginRenderPass*>(Record(Command::kBeginRenderPass, size));
  args->color_count = color_count;
  args->has_depth_stencil = desc.has_depth_stencil ? 1 : 0;
  args->occlusion_query_set = desc.occlusion_query_set;
  auto* colors = reinterpret_cast<WGPURenderPassColorAttachment*>(args + 1);
  for (uint32_t i = 0; i < color_count; ++i) {
    colors[i] = desc.color_attachments[i];
    colors[i].nextInChain = nullptr;
  }
  if (desc.has_depth_stencil) {
    auto* depth = reinterpret_cast<WGPURenderPassDepthStencilAttachment*>(colors + color_count);
    *depth = desc.depth_stencil;
    depth->nextInChain = nullptr;
  }
  pass_ = Pass::kRender;
}

void WebGPUCommandStream::CmdEndRenderPass() {
  if (!Require(Pass::kRender, "EndRenderPass")) {
    return;
  }
  Record(Command::kEndRenderPass, 0);
  pass_ = Pass::kNone;
}

void WebGPUCommandStream::CmdBeginComputePass() {
  if (!Require(Pass::kNone, "BeginComputePass")) {
    return;
  }
  Record(Command::kBeginComputePass, 0);
  pass_ = Pass::kCompute;
}

void WebGPUCommandStream::CmdEndComputePass() {
  if (!Require(Pass::kCompute, "EndComputePass")) {
    return;
  }
  Record(Command::kEndComputePass, 0);
  pass_ = Pass::kNone;
}

void WebGPUCommandStream::EndPass() {
  if (pass_ == Pass::kRender) {
    CmdEndRenderPass();
  } else if (pass_ == Pass::kCompute) {
    CmdEndComputePass();
  }
}

void WebGPUCommandStream::CmdSetPipeline(PipelineId id) {
  if (!Require(Pass::kRender, "SetPipeline")) {
    return;
  }
  static_cast<ArgsId*>(Record(Command::kSetPipelineId, sizeof(ArgsId)))->id = id;
}

void WebGPUCommandStream::CmdSetRenderPipeline(WGPURenderPipeline pipeline) {
  if (!Require(Pass::kRender, "SetRenderPipeline")) {
    return;
  }
  static_cast<ArgsHandle*>(Record(Command::kSetRenderPipeline, sizeof(ArgsHandle)))->handle =
      pipeline;
}

void WebGPUCommandStream::CmdSetVertexBuffer(uint32_t slot, WGPUBuffer buffer, uint64_t offset,
                                             uint64_t size) {
  if (!Require(Pass::kRender, "SetVertexBuffer")) {
    return;
  }
  auto* args =
      static_cast<ArgsSetVertexBuffer*>(Record(Command::kSetVertexBuffer, sizeof(ArgsSetVertexBuffer)));
  args->slot = slot;
  args->buffer = buffer;
  args->offset = offset;
  args->size = size;
}

void WebGPUCommandStream::CmdSetIndexBuffer(WGPUBuffer buffer, WGPUIndexFormat format,
                                            uint64_t offset, uint64_t size) {
  if (!Require(Pass::kRender, "SetIndexBuffer")) {
    return;
  }
  auto* args =
      static_cast<ArgsSetIndexBuffer*>(Record(Command::kSetIndexBuffer, sizeof(ArgsSetIndexBuffer)));
  args->buffer = buffer;
  args->format = format;
  args->offset = offset;
  args->size = size;
}

void WebGPUCommandStream::CmdSetViewport(float x, float y, float width, float height,
                                         float min_depth, float max_depth) {
  if (!Require(Pass::kRender, "SetViewport")) {
    return;
  }
  *static_cast<ArgsSetViewport*>(Record(Command::kSetViewport, sizeof(ArgsSetViewport))) = {
      x, y, width, height, min_depth, max_depth};
}

void WebGPUCommandStream::CmdSetScissorRect(uint32_t x, uint32_t y, uint32_t width,
                                            uint32_t height) {
  if (!Require(Pass::kRender, "SetScissorRect")) {
    return;
  }
  *static_cast<ArgsSetScissorRect*>(Record(Command::kSetScissorRect, sizeof(ArgsSetScissorRect))) =
      {x, y, width, height};
}

void WebGPUCommandStream::CmdSetBlendConstant(const WGPUColor& color) {
  if (!Require(Pass::kRender, "SetBlendConstant")) {
    return;
  }
  static_cast<ArgsSetBlendConstant*>(
      Record(Command::kSetBlendConstant, sizeof(ArgsSetBlendConstant)))
      ->color = color;
}

void WebGPUCommandStream::CmdSetStencilReference(uint32_t reference) {
  if (!Require(Pass::kRender, "SetStencilReference")) {
    return;
  }
  static_cast<ArgsId*>(Record(Command::kSetStencilReference, sizeof(ArgsId)))->id = reference;
}

void WebGPUCommandStream::CmdDraw(uint32_t vertex_count, uint32_t instance_count,
                                  uint32_t first_vertex, uint32_t first_instance) {
  if (!Require(Pass::kRender, "Draw")) {
    return;
  }
  *static_cast<ArgsDraw*>(Record(Command::kDraw, sizeof(ArgsDraw))) = {
      vertex_count, instance_count, first_vertex, first_instance};
}

void WebGPUCommandStream::CmdDrawIndexed(uint32_t index_count, uint32_t instance_count,
                                         uint32_t first_index, int32_t base_vertex,
                                         uint32_t first_instance) {
  if (!Require(Pass::kRender, "DrawIndexed")) {
    return;
  }
  *static_cast<ArgsDrawIndexed*>(Record(Command::kDrawIndexed, sizeof(ArgsDrawIndexed))) = {
      index_count, instance_count, first_index, base_vertex, first_instance};
}

void WebGPUCommandStream::CmdSetComputePipeline(PipelineId id) {
  if (!Require(Pass::kCompute, "SetComputePipeline")) {
    return;
  }
  static_cast<ArgsId*>(Record(Command::kSetComputePipelineId, sizeof(ArgsId)))->id = id;
}

void WebGPUCommandStream::CmdSetComputePipelineObject(WGPUComputePipeline pipeline) {
  if (!Require(Pass::kCompute, "SetComputePipelineObject")) {
    return;
  }
  static_cast<ArgsHandle*>(Record(Command::kSetComputePipeline, sizeof(ArgsHandle)))->handle =
      pipeline;
}

void WebGPUCommandStream::CmdDispatch(uint32_t x, uint32_t y, uint32_t z) {
  if (!Require(Pass::kCompute, "Dispatch")) {
    return;
  }
  *static_cast<ArgsDispatch*>(Record(Command::kDispatch, sizeof(ArgsDispatch))) = {x, y, z};
}

void WebGPUCommandStream::CmdSetBindGroup(uint32_t group_index, WGPUBindGroup group,
                                          uint32_t dynamic_offset_count,
                                          const uint32_t* dynamic_offsets) {
  if (pass_ == Pass::kNone) {
    Require(Pass::kRender, "SetBindGroup");
    return;
  }
  auto* args = static_cast<ArgsSetBindGroup*>(
      Record(Command::kSetBindGroup,
             sizeof(ArgsSetBindGroup) + sizeof(uint32_t) * dynamic_offset_count));
  args->group_index = group_index;
  args->dynamic_offset_count = dynamic_offset_count;
  args->group = group;
  if (dynamic_offset_count) {
    std::memcpy(args + 1, dynamic_offsets, sizeof(uint32_t) * dynamic_offset_count);
  }
}

void WebGPUCommandStream::CmdSetBindGroup(uint32_t group_index, WGPUBindGroupLayout layout,
                                          const WGPUBindGroupEntry* entries, uint32_t entry_count,
                                          uint32_t dynamic_offset_count,
                                          const uint32_t* dynamic_offsets) {
  if (pass_ == Pass::kNone) {
    Require(Pass::kRender, "SetBindGroup (descriptor)");
    return;
  }
  size_t entries_size = sizeof(WGPUBindGroupEntry) * entry_count;
  size_t size = sizeof(ArgsSetBindGroupDesc) + entries_size + sizeof(uint32_t) * dynamic_offset_count;
  auto* args = static_cast<ArgsSetBindGroupDesc*>(Record(Command::kSetBindGroupDesc, size));
  // Zero first so struct padding is deterministic for hashing and comparison.
  std::memset(args, 0, size);
  args->group_index = group_index;
  args->entry_count = entry_count;
  args->dynamic_offset_count = dynamic_offset_count;
  args->layout = layout;
  auto* recorded_entries = reinterpret_cast<WGPUBindGroupEntry*>(args + 1);
  for (uint32_t i = 0; i < entry_count; ++i) {
    WGPUBindGroupEntry& entry = recorded_entries[i];
    entry.binding = entries[i].binding;
    entry.buffer = entries[i].buffer;
    entry.offset = entries[i].offset;
    entry.size = entries[i].size;
    entry.sampler = entries[i].sampler;
    entry.textureView = entries[i].textureView;
  }
  if (dynamic_offset_count) {
    std::memcpy(recorded_entries + entry_count, dynamic_offsets,
                sizeof(uint32_t) * dynamic_offset_count);
  }
  args->hash = HashBytes(&args->layout, sizeof(args->layout) + entries_size);
}

void WebGPUCommandStream::CmdCopyBufferToBuffer(WGPUBuffer source, uint64_t source_offset,
                                                WGPUBuffer destination,
                                                uint64_t destination_offset, uint64_t size) {
  if (!Require(Pass::kNone, "CopyBufferToBuffer")) {
    return;
  }
  *static_cast<ArgsCopyBufferToBuffer*>(
      Record(Command::kCopyBufferToBuffer, sizeof(ArgsCopyBufferToBuffer))) = {
      source, source_offset, destination, destination_offset, size};
}

void WebGPUCommandStream::CmdCopyBufferToTexture(const WGPUTexelCopyBufferInfo& source,
                                                 const WGPUTexelCopyTextureInfo& destination,
                                                 const WGPUExtent3D& size) {
  if (!Require(Pass::kNone, "CopyBufferToTexture")) {
    return;
  }
  auto* args = static_cast<ArgsCopyBufferTexture*>(
      Record(Command::kCopyBufferToTexture, sizeof(ArgsCopyBufferTexture)));
  args->buffer = SanitizeBufferInfo(source);
  args->texture = SanitizeTextureInfo(destination);
  args->size = size;
}

void WebGPUCommandStream::CmdCopyTextureToBuffer(const WGPUTexelCopyTextureInfo& source,
                                                 const WGPUTexelCopyBufferInfo& destination,
                                                 const WGPUExtent3D& size) {
  if (!Require(Pass::kNone, "CopyTextureToBuffer")) {
    return;
  }
  auto* args = static_cast<ArgsCopyBufferTexture*>(
      Record(Command::kCopyTextureToBuffer, sizeof(ArgsCopyBufferTexture)));
  args->buffer = SanitizeBufferInfo(destination);
  args->texture = SanitizeTextureInfo(source);
  args->size = size;
}

void WebGPUCommandStream::CmdCopyTextureToTexture(const WGPUTexelCopyTextureInfo& source,
                                                  const WGPUTexelCopyTextureInfo& destination,
                                                  const WGPUExtent3D& size) {
  if (!Require(Pass::kNone, "CopyTextureToTexture")) {
    return;
  }
  auto* args = static_cast<ArgsCopyTextureToTexture*>(
      Record(Command::kCopyTextureToTexture, sizeof(ArgsCopyTextureToTexture)));
  args->source = SanitizeTextureInfo(source);
  args->destination = SanitizeTextureInfo(destination);
  args->size = size;
}

void WebGPUCommandStream::CmdClearBuffer(WGPUBuffer buffer, uint64_t offset, uint64_t size) {
  if (!Require(Pass::kNone, "ClearBuffer")) {
    return;
  }
  *static_cast<ArgsClearBuffer*>(Record(Command::kClearBuffer, sizeof(ArgsClearBuffer))) = {
      buffer, offset, size};
}

void* WebGPUCommandStream::CmdUploadToBuffer(WGPUBuffer destination, uint64_t destination_offset,
                                             uint64_t size) {
  if (!Require(Pass::kNone, "UploadToBuffer")) {
    return nullptr;
  }
  if (!size) {
    return nullptr;
  }
  // copyBufferToBuffer: offsets and size are multiples of 4.
  size = AlignUp(size, 4);
  size_t upload_offset = uploads_.Allocate(size_t(size), 16);
  *static_cast<ArgsUploadToBuffer*>(Record(Command::kUploadToBuffer, sizeof(ArgsUploadToBuffer))) =
      {destination, destination_offset, upload_offset, size};
  return uploads_.data + upload_offset;
}

void* WebGPUCommandStream::CmdUploadToTexture(const WGPUTexelCopyTextureInfo& destination,
                                              uint32_t bytes_per_row, uint32_t rows_per_image,
                                              const WGPUExtent3D& size, uint64_t data_size) {
  if (!Require(Pass::kNone, "UploadToTexture")) {
    return nullptr;
  }
  if (!data_size) {
    return nullptr;
  }
  // Offset: a multiple of the texel block size; 256 covers every format.
  size_t upload_offset = uploads_.Allocate(size_t(AlignUp(data_size, 4)), 256);
  auto* args =
      static_cast<ArgsUploadToTexture*>(Record(Command::kUploadToTexture, sizeof(ArgsUploadToTexture)));
  args->destination = SanitizeTextureInfo(destination);
  args->size = size;
  args->upload_offset = upload_offset;
  args->bytes_per_row = bytes_per_row;
  args->rows_per_image = rows_per_image;
  return uploads_.data + upload_offset;
}

void WebGPUCommandStream::CmdEncoderCallback(EncoderCallback callback, void* user) {
  if (!Require(Pass::kNone, "EncoderCallback")) {
    return;
  }
  *static_cast<ArgsEncoderCallback*>(
      Record(Command::kEncoderCallback, sizeof(ArgsEncoderCallback))) = {callback, user};
}

void* WebGPUCommandStream::AllocateConstants(uint32_t size, uint32_t& offset_out) {
  size_t offset = constants_.Allocate(size_t(AlignUp(size, 4)), kConstantsAlignment);
  offset_out = uint32_t(offset);
  return constants_.data + offset;
}

void WebGPUCommandStream::AddSubmittedCallback(SubmittedCallback callback, void* user) {
  submitted_callbacks_.push_back({callback, user});
}

// ---------------------------------------------------------------------------
// WebGPUCommandStream: replay.
// ---------------------------------------------------------------------------

bool WebGPUCommandStream::Execute(WebGPUReplayState& state, WebGPUReplayStats* stats_out) {
  WebGPUReplayStats stats;
  double time_start = emscripten_get_now();
  WebGPUDevice& device = state.device();
  WGPUDevice wgpu_device = device.device();
  WGPUQueue queue = device.queue();
  bool success = wgpu_device && queue && !device.is_lost();

  // Constants slack: a binding window of up to maxUniformBufferBindingSize may
  // start at the last allocated offset.
  constexpr uint64_t kConstantBindingSlack = 64 * 1024;
  if (success && uploads_.size) {
    uint64_t size = AlignUp(uploads_.size, 4);
    if (state.EnsureBuffer(state.staging_buffer_, state.staging_buffer_size_, size,
                           WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst,
                           "rex stream staging")) {
      // The arena capacity covers the alignment: allocations are 4-aligned.
      wgpuQueueWriteBuffer(queue, state.staging_buffer_, 0, uploads_.data, size_t(size));
      stats.upload_bytes = size;
    } else {
      success = false;
    }
  }
  if (success && (constants_.size || !state.constant_buffer_)) {
    uint64_t size = AlignUp(constants_.size, 4);
    if (state.EnsureBuffer(state.constant_buffer_, state.constant_buffer_size_,
                           size + kConstantBindingSlack,
                           WGPUBufferUsage_Uniform | WGPUBufferUsage_Storage |
                               WGPUBufferUsage_CopyDst,
                           "rex stream constants")) {
      if (size) {
        wgpuQueueWriteBuffer(queue, state.constant_buffer_, 0, constants_.data, size_t(size));
        stats.constant_bytes = size;
      }
    } else {
      success = false;
    }
  }

  WGPUCommandEncoder encoder = nullptr;
  if (success) {
    WGPUCommandEncoderDescriptor encoder_desc = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    encoder_desc.label = MakeStringView("rex stream");
    encoder = wgpuDeviceCreateCommandEncoder(wgpu_device, &encoder_desc);
    success = encoder != nullptr;
  }

  if (success) {
    WGPURenderPassEncoder render_pass = nullptr;
    WGPUComputePassEncoder compute_pass = nullptr;
    // Whether the current pass has a usable pipeline; draws/dispatches without
    // one are skipped (their pipeline id was not ready yet).
    bool pipeline_ready = false;
    state.transient_bind_groups_.clear();
    // --webgpu_log_skipped_pipelines (WebGPUCommandQueue::set_log_skipped_pipelines).
    const bool log_skips = state.log_skipped_pipelines_;
    const uint32_t replay_index = ++state.replay_index_;
    PipelineId current_id = kInvalidPipelineId;
    auto skip_stats = [&state](PipelineId id) -> WebGPUReplayState::PipelineSkipStats& {
      if (id >= state.skip_stats_.size()) {
        state.skip_stats_.resize(size_t(id) + 256);
      }
      return state.skip_stats_[id];
    };
    auto note_skipped = [&]() {
      if (!log_skips || current_id == kInvalidPipelineId) {
        return;
      }
      WebGPUReplayState::PipelineSkipStats& s = skip_stats(current_id);
      if (!s.skipped++) {
        s.first_skip_replay = replay_index;
      }
      s.last_skip_replay = replay_index;
    };
    auto note_replayed = [&]() {
      if (!log_skips || current_id == kInvalidPipelineId) {
        return;
      }
      WebGPUReplayState::PipelineSkipStats& s = skip_stats(current_id);
      if (!s.drawn++ && s.skipped) {
        REXWGPU_INFO(
            "WebGPU: pipeline {} first replayed in replay {} after {} skipped draws / dispatches "
            "in replays {}-{}",
            current_id, replay_index, s.skipped, s.first_skip_replay, s.last_skip_replay);
      }
    };
    const uint8_t* record = commands_.data;
    const uint8_t* records_end = commands_.data + commands_.size;
    std::vector<WGPUBindGroupEntry> entries_scratch;
    while (record < records_end) {
      const auto* header = reinterpret_cast<const CommandHeader*>(record);
      const void* args_ptr = header + 1;
      ++stats.commands;
      switch (Command(header->command)) {
        case Command::kBeginRenderPass: {
          const auto* args = static_cast<const ArgsBeginRenderPass*>(args_ptr);
          const auto* colors = reinterpret_cast<const WGPURenderPassColorAttachment*>(args + 1);
          WGPURenderPassDescriptor desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
          desc.colorAttachmentCount = args->color_count;
          desc.colorAttachments = colors;
          desc.depthStencilAttachment =
              args->has_depth_stencil
                  ? reinterpret_cast<const WGPURenderPassDepthStencilAttachment*>(
                        colors + args->color_count)
                  : nullptr;
          desc.occlusionQuerySet = args->occlusion_query_set;
          render_pass = wgpuCommandEncoderBeginRenderPass(encoder, &desc);
          pipeline_ready = false;
          current_id = kInvalidPipelineId;
          ++stats.render_passes;
          break;
        }
        case Command::kEndRenderPass:
          wgpuRenderPassEncoderEnd(render_pass);
          wgpuRenderPassEncoderRelease(render_pass);
          render_pass = nullptr;
          break;
        case Command::kBeginComputePass: {
          WGPUComputePassDescriptor desc = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
          compute_pass = wgpuCommandEncoderBeginComputePass(encoder, &desc);
          pipeline_ready = false;
          current_id = kInvalidPipelineId;
          ++stats.compute_passes;
          break;
        }
        case Command::kEndComputePass:
          wgpuComputePassEncoderEnd(compute_pass);
          wgpuComputePassEncoderRelease(compute_pass);
          compute_pass = nullptr;
          break;
        case Command::kSetPipelineId: {
          WGPURenderPipeline pipeline =
              state.pipelines()
                  ? state.pipelines()->GetRender(static_cast<const ArgsId*>(args_ptr)->id)
                  : nullptr;
          pipeline_ready = pipeline != nullptr;
          current_id = static_cast<const ArgsId*>(args_ptr)->id;
          if (pipeline) {
            wgpuRenderPassEncoderSetPipeline(render_pass, pipeline);
          }
          break;
        }
        case Command::kSetRenderPipeline: {
          auto pipeline = static_cast<WGPURenderPipeline>(
              const_cast<void*>(static_cast<const ArgsHandle*>(args_ptr)->handle));
          pipeline_ready = pipeline != nullptr;
          current_id = kInvalidPipelineId;
          if (pipeline) {
            wgpuRenderPassEncoderSetPipeline(render_pass, pipeline);
          }
          break;
        }
        case Command::kSetVertexBuffer: {
          const auto* args = static_cast<const ArgsSetVertexBuffer*>(args_ptr);
          wgpuRenderPassEncoderSetVertexBuffer(render_pass, args->slot, args->buffer, args->offset,
                                               args->size);
          break;
        }
        case Command::kSetIndexBuffer: {
          const auto* args = static_cast<const ArgsSetIndexBuffer*>(args_ptr);
          wgpuRenderPassEncoderSetIndexBuffer(render_pass, args->buffer, args->format,
                                              args->offset, args->size);
          break;
        }
        case Command::kSetViewport: {
          const auto* args = static_cast<const ArgsSetViewport*>(args_ptr);
          wgpuRenderPassEncoderSetViewport(render_pass, args->x, args->y, args->width,
                                           args->height, args->min_depth, args->max_depth);
          break;
        }
        case Command::kSetScissorRect: {
          const auto* args = static_cast<const ArgsSetScissorRect*>(args_ptr);
          wgpuRenderPassEncoderSetScissorRect(render_pass, args->x, args->y, args->width,
                                              args->height);
          break;
        }
        case Command::kSetBlendConstant:
          wgpuRenderPassEncoderSetBlendConstant(
              render_pass, &static_cast<const ArgsSetBlendConstant*>(args_ptr)->color);
          break;
        case Command::kSetStencilReference:
          wgpuRenderPassEncoderSetStencilReference(render_pass,
                                                   static_cast<const ArgsId*>(args_ptr)->id);
          break;
        case Command::kDraw: {
          if (!pipeline_ready) {
            ++stats.draws_skipped;
            note_skipped();
            break;
          }
          const auto* args = static_cast<const ArgsDraw*>(args_ptr);
          wgpuRenderPassEncoderDraw(render_pass, args->vertex_count, args->instance_count,
                                    args->first_vertex, args->first_instance);
          ++stats.draws;
          note_replayed();
          break;
        }
        case Command::kDrawIndexed: {
          if (!pipeline_ready) {
            ++stats.draws_skipped;
            note_skipped();
            break;
          }
          const auto* args = static_cast<const ArgsDrawIndexed*>(args_ptr);
          wgpuRenderPassEncoderDrawIndexed(render_pass, args->index_count, args->instance_count,
                                           args->first_index, args->base_vertex,
                                           args->first_instance);
          ++stats.draws;
          note_replayed();
          break;
        }
        case Command::kSetComputePipelineId: {
          WGPUComputePipeline pipeline =
              state.pipelines()
                  ? state.pipelines()->GetCompute(static_cast<const ArgsId*>(args_ptr)->id)
                  : nullptr;
          pipeline_ready = pipeline != nullptr;
          current_id = static_cast<const ArgsId*>(args_ptr)->id;
          if (pipeline) {
            wgpuComputePassEncoderSetPipeline(compute_pass, pipeline);
          }
          break;
        }
        case Command::kSetComputePipeline: {
          auto pipeline = static_cast<WGPUComputePipeline>(
              const_cast<void*>(static_cast<const ArgsHandle*>(args_ptr)->handle));
          pipeline_ready = pipeline != nullptr;
          current_id = kInvalidPipelineId;
          if (pipeline) {
            wgpuComputePassEncoderSetPipeline(compute_pass, pipeline);
          }
          break;
        }
        case Command::kDispatch: {
          if (!pipeline_ready) {
            ++stats.draws_skipped;
            note_skipped();
            break;
          }
          const auto* args = static_cast<const ArgsDispatch*>(args_ptr);
          wgpuComputePassEncoderDispatchWorkgroups(compute_pass, args->x, args->y, args->z);
          ++stats.dispatches;
          note_replayed();
          break;
        }
        case Command::kSetBindGroup: {
          const auto* args = static_cast<const ArgsSetBindGroup*>(args_ptr);
          const auto* offsets = reinterpret_cast<const uint32_t*>(args + 1);
          if (render_pass) {
            wgpuRenderPassEncoderSetBindGroup(render_pass, args->group_index, args->group,
                                              args->dynamic_offset_count, offsets);
          } else if (compute_pass) {
            wgpuComputePassEncoderSetBindGroup(compute_pass, args->group_index, args->group,
                                               args->dynamic_offset_count, offsets);
          }
          break;
        }
        case Command::kSetBindGroupDesc: {
          const auto* args = static_cast<const ArgsSetBindGroupDesc*>(args_ptr);
          const auto* entries = reinterpret_cast<const WGPUBindGroupEntry*>(args + 1);
          const auto* offsets = reinterpret_cast<const uint32_t*>(entries + args->entry_count);
          size_t compare_size = sizeof(args->layout) + sizeof(WGPUBindGroupEntry) * args->entry_count;
          WGPUBindGroup group = nullptr;
          for (const WebGPUReplayState::TransientBindGroup& transient :
               state.transient_bind_groups_) {
            if (transient.hash != args->hash) {
              continue;
            }
            const auto* other = reinterpret_cast<const ArgsSetBindGroupDesc*>(
                commands_.data + transient.record_offset);
            if (other->entry_count == args->entry_count &&
                !std::memcmp(&other->layout, &args->layout, compare_size)) {
              group = transient.bind_group;
              ++stats.bind_groups_reused;
              break;
            }
          }
          if (!group && args->layout) {
            entries_scratch.assign(entries, entries + args->entry_count);
            for (WGPUBindGroupEntry& entry : entries_scratch) {
              if (entry.buffer == StreamConstantsBuffer()) {
                entry.buffer = state.constant_buffer_;
              }
            }
            WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            desc.layout = args->layout;
            desc.entryCount = entries_scratch.size();
            desc.entries = entries_scratch.data();
            group = wgpuDeviceCreateBindGroup(wgpu_device, &desc);
            if (group) {
              state.transient_bind_groups_.push_back(
                  {args->hash, size_t(reinterpret_cast<const uint8_t*>(args) - commands_.data),
                   group});
              ++stats.bind_groups_created;
            }
          }
          if (group) {
            if (render_pass) {
              wgpuRenderPassEncoderSetBindGroup(render_pass, args->group_index, group,
                                                args->dynamic_offset_count, offsets);
            } else if (compute_pass) {
              wgpuComputePassEncoderSetBindGroup(compute_pass, args->group_index, group,
                                                 args->dynamic_offset_count, offsets);
            }
          }
          break;
        }
        case Command::kCopyBufferToBuffer: {
          const auto* args = static_cast<const ArgsCopyBufferToBuffer*>(args_ptr);
          wgpuCommandEncoderCopyBufferToBuffer(encoder, args->source, args->source_offset,
                                               args->destination, args->destination_offset,
                                               args->size);
          ++stats.copies;
          break;
        }
        case Command::kCopyBufferToTexture: {
          const auto* args = static_cast<const ArgsCopyBufferTexture*>(args_ptr);
          wgpuCommandEncoderCopyBufferToTexture(encoder, &args->buffer, &args->texture,
                                                &args->size);
          ++stats.copies;
          break;
        }
        case Command::kCopyTextureToBuffer: {
          const auto* args = static_cast<const ArgsCopyBufferTexture*>(args_ptr);
          wgpuCommandEncoderCopyTextureToBuffer(encoder, &args->texture, &args->buffer,
                                                &args->size);
          ++stats.copies;
          break;
        }
        case Command::kCopyTextureToTexture: {
          const auto* args = static_cast<const ArgsCopyTextureToTexture*>(args_ptr);
          wgpuCommandEncoderCopyTextureToTexture(encoder, &args->source, &args->destination,
                                                 &args->size);
          ++stats.copies;
          break;
        }
        case Command::kClearBuffer: {
          const auto* args = static_cast<const ArgsClearBuffer*>(args_ptr);
          wgpuCommandEncoderClearBuffer(encoder, args->buffer, args->offset, args->size);
          break;
        }
        case Command::kUploadToBuffer: {
          const auto* args = static_cast<const ArgsUploadToBuffer*>(args_ptr);
          wgpuCommandEncoderCopyBufferToBuffer(encoder, state.staging_buffer_,
                                               args->upload_offset, args->destination,
                                               args->destination_offset, args->size);
          ++stats.copies;
          break;
        }
        case Command::kUploadToTexture: {
          const auto* args = static_cast<const ArgsUploadToTexture*>(args_ptr);
          WGPUTexelCopyBufferInfo source = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
          source.buffer = state.staging_buffer_;
          source.layout.offset = args->upload_offset;
          source.layout.bytesPerRow = args->bytes_per_row;
          source.layout.rowsPerImage = args->rows_per_image;
          wgpuCommandEncoderCopyBufferToTexture(encoder, &source, &args->destination,
                                                &args->size);
          ++stats.copies;
          break;
        }
        case Command::kEncoderCallback: {
          const auto* args = static_cast<const ArgsEncoderCallback*>(args_ptr);
          args->callback(args->user, state, encoder);
          break;
        }
      }
      record += header->size;
    }
    // Recording closes passes before submission; be safe if it did not.
    if (render_pass) {
      wgpuRenderPassEncoderEnd(render_pass);
      wgpuRenderPassEncoderRelease(render_pass);
    }
    if (compute_pass) {
      wgpuComputePassEncoderEnd(compute_pass);
      wgpuComputePassEncoderRelease(compute_pass);
    }
  }

  double time_encoded = emscripten_get_now();
  if (encoder) {
    WGPUCommandBufferDescriptor command_buffer_desc = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
    WGPUCommandBuffer command_buffer = wgpuCommandEncoderFinish(encoder, &command_buffer_desc);
    if (command_buffer) {
      wgpuQueueSubmit(queue, 1, &command_buffer);
      wgpuCommandBufferRelease(command_buffer);
    } else {
      success = false;
    }
    wgpuCommandEncoderRelease(encoder);
  }
  for (const WebGPUReplayState::TransientBindGroup& transient : state.transient_bind_groups_) {
    wgpuBindGroupRelease(transient.bind_group);
  }
  state.transient_bind_groups_.clear();
  double time_submitted = emscripten_get_now();

  // Callbacks run even if the device is gone: they release their own objects.
  for (const SubmittedCallbackEntry& entry : submitted_callbacks_) {
    entry.callback(entry.user, state);
  }

  if (state.log_skipped_pipelines_ && !(state.replay_index_ % 1000)) {
    // Ids whose draws were skipped and that were never replayed since: what the
    // guest drew only while they were not ready is missing from its targets.
    uint32_t never = 0, never_skipped = 0, later = 0;
    std::string list;
    for (PipelineId id = 0; id < state.skip_stats_.size(); ++id) {
      const WebGPUReplayState::PipelineSkipStats& s = state.skip_stats_[id];
      if (!s.skipped) {
        continue;
      }
      if (s.drawn) {
        ++later;
        continue;
      }
      ++never;
      never_skipped += s.skipped;
      if (never <= 48) {
        list += " " + std::to_string(id) + "x" + std::to_string(s.skipped) + "@" +
                std::to_string(s.first_skip_replay) + "-" + std::to_string(s.last_skip_replay);
      }
    }
    REXWGPU_INFO(
        "WebGPU: replay {}: {} pipelines had skipped draws and were replayed later, {} "
        "({} draws) were never replayed after their skips (id x skips @ replays):{}",
        state.replay_index_, later, never, never_skipped, list);
  }
  stats.encode_ms = time_encoded - time_start;
  stats.submit_ms = time_submitted - time_encoded;
  if (stats_out) {
    *stats_out = stats;
  }
  return success;
}

// ---------------------------------------------------------------------------
// WebGPUCommandQueue.
// ---------------------------------------------------------------------------

WebGPUCommandQueue::WebGPUCommandQueue(WebGPUDevice& device, WebGPUSubmissionTracker& tracker)
    : device_(device), tracker_(tracker) {}

WebGPUCommandQueue::~WebGPUCommandQueue() {
  Shutdown();
}

bool WebGPUCommandQueue::Initialize(uint32_t stream_count) {
  if (initialized_) {
    return true;
  }
  if (device_.IsDeviceThread()) {
    REXWGPU_ERROR("WebGPUCommandQueue::Initialize must not run on the device thread");
    return false;
  }
  stream_count = std::max<uint32_t>(stream_count, 2);
  streams_.clear();
  free_streams_.clear();
  for (uint32_t i = 0; i < stream_count; ++i) {
    streams_.push_back(std::make_unique<WebGPUCommandStream>());
    if (i) {
      free_streams_.push_back(streams_.back().get());
    }
  }
  current_ = streams_.front().get();
  if (!device_.RunSync([this]() {
        replay_state_ = new WebGPUReplayState(device_, &pipeline_table_);
      })) {
    REXWGPU_ERROR("WebGPUCommandQueue: the device thread is not running");
    return false;
  }
  initialized_ = true;
  return true;
}

void WebGPUCommandQueue::Shutdown() {
  if (!initialized_) {
    return;
  }
  uint64_t last = tracker_.submitted();
  if (last && !tracker_.AwaitReplayed(last, std::chrono::milliseconds(30000))) {
    REXWGPU_WARN("WebGPUCommandQueue: submission {} not replayed at shutdown", last);
  }
  device_.RunSync([this]() {
    delete replay_state_;
    replay_state_ = nullptr;
    pipeline_table_.Release();
  });
  {
    std::lock_guard<std::mutex> lock(free_mutex_);
    free_streams_.clear();
  }
  current_ = nullptr;
  streams_.clear();
  initialized_ = false;
}

void WebGPUCommandQueue::ReplayOnDeviceThread(WebGPUCommandStream* stream, uint64_t submission) {
  WebGPUReplayStats stats;
  if (replay_state_) {
    replay_state_->log_skipped_pipelines_ = log_skipped_pipelines_.load(std::memory_order_relaxed);
  }
  bool executed = replay_state_ && stream->Execute(*replay_state_, &stats);
  if (device_.is_lost()) {
    tracker_.MarkLost();
  } else if (executed) {
    tracker_.TrackCompletion(device_.queue(), submission);
  } else {
    tracker_.MarkCompleted(submission);
  }
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    last_stats_ = stats;
  }
  replay_count_.fetch_add(1, std::memory_order_relaxed);
  draws_skipped_total_.fetch_add(stats.draws_skipped, std::memory_order_relaxed);
  stream->Reset();
  {
    std::lock_guard<std::mutex> lock(free_mutex_);
    free_streams_.push_back(stream);
  }
  free_cond_.notify_one();
  tracker_.MarkReplayed(submission);
}

uint64_t WebGPUCommandQueue::Submit() {
  if (!initialized_ || !current_) {
    return 0;
  }
  WebGPUCommandStream* stream = current_;
  stream->EndPass();
  uint64_t submission = tracker_.AllocateSubmission();
  if (!device_.RunAsync([this, stream, submission]() { ReplayOnDeviceThread(stream, submission); })) {
    // The device thread is gone: nothing will ever run.
    stream->Reset();
    {
      std::lock_guard<std::mutex> lock(free_mutex_);
      free_streams_.push_back(stream);
    }
    tracker_.MarkLost();
  }
  std::unique_lock<std::mutex> lock(free_mutex_);
  while (free_streams_.empty()) {
    if (!free_cond_.wait_for(lock, std::chrono::milliseconds(5000),
                             [this]() { return !free_streams_.empty(); })) {
      REXWGPU_WARN("WebGPUCommandQueue: still waiting for the device thread to replay a stream");
    }
  }
  current_ = free_streams_.back();
  free_streams_.pop_back();
  return submission;
}

uint64_t WebGPUCommandQueue::SubmitAndWaitReplayed() {
  uint64_t submission = Submit();
  if (submission) {
    tracker_.AwaitReplayed(submission);
  }
  return submission;
}

WebGPUReplayStats WebGPUCommandQueue::last_replay_stats() const {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  return last_stats_;
}

}  // namespace rex::graphics::webgpu
