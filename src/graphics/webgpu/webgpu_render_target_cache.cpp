/**
 * @file        graphics/webgpu/webgpu_render_target_cache.cpp
 * @brief       See rex/graphics/webgpu/webgpu_render_target_cache.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/webgpu_render_target_cache.h>

#include <algorithm>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/webgpu/webgpu_command_processor.h>
#include <rex/graphics/webgpu/webgpu_device.h>
#include <rex/graphics/webgpu/webgpu_shared_memory.h>
#include <rex/graphics/webgpu/webgpu_texture_cache.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(webgpu_readback_resolve, false, "GPU",
                    "WebGPU: also copy resolved data back into guest memory (for games reading "
                    "resolves on the CPU; waits for the GPU on every resolve)");

namespace rex::graphics::webgpu {

// WebGPUEdram and its shaders take the xenos values as plain numbers.
static_assert(uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8) == 0);
static_assert(uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) == 1);
static_assert(uint32_t(xenos::ColorRenderTargetFormat::k_2_10_10_10) == 2);
static_assert(uint32_t(xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT) == 3);
static_assert(uint32_t(xenos::ColorRenderTargetFormat::k_16_16) == 4);
static_assert(uint32_t(xenos::ColorRenderTargetFormat::k_16_16_16_16) == 5);
static_assert(uint32_t(xenos::ColorRenderTargetFormat::k_16_16_FLOAT) == 6);
static_assert(uint32_t(xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT) == 7);
static_assert(uint32_t(xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10) == 10);
static_assert(uint32_t(xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16) == 12);
static_assert(uint32_t(xenos::ColorRenderTargetFormat::k_32_FLOAT) == 14);
static_assert(uint32_t(xenos::ColorRenderTargetFormat::k_32_32_FLOAT) == 15);
static_assert(uint32_t(xenos::DepthRenderTargetFormat::kD24S8) == 0);
static_assert(uint32_t(xenos::DepthRenderTargetFormat::kD24FS8) == 1);
static_assert(uint32_t(xenos::MsaaSamples::k2X) == 1 && uint32_t(xenos::MsaaSamples::k4X) == 2);
static_assert(uint32_t(xenos::CopySampleSelect::k01) == 4 &&
              uint32_t(xenos::CopySampleSelect::k0123) == 6);
static_assert(uint32_t(xenos::Endian128::k8in64) == 4 && uint32_t(xenos::Endian128::k8in128) == 5);
static_assert(uint32_t(xenos::ColorFormat::k_32_32_32_32_FLOAT) == 38);
static_assert(uint32_t(xenos::TextureFormat::k_24_8) == 22);
static_assert(xenos::kEdramTileCount == WebGPUEdram::kTileCount);
static_assert(xenos::kEdramSizeBytes == WebGPUEdram::kBufferSize);

WebGPURenderTargetCache::WebGPURenderTarget::~WebGPURenderTarget() {
  PendingRelease objects = {};
  objects.texture = texture_;
  objects.views[0] = surface_.attachment_view;
  if (surface_.sample_view != surface_.attachment_view) {
    objects.views[1] = surface_.sample_view;
  }
  objects.views[2] = surface_.stencil_view;
  cache_.ReleaseRenderTargetObjects(objects);
}

WebGPURenderTargetCache::WebGPURenderTargetCache(WebGPUCommandProcessor& command_processor,
                                                 const RegisterFile& register_file,
                                                 const memory::Memory& memory,
                                                 TraceWriter& trace_writer)
    : RenderTargetCache(register_file, memory, &trace_writer, 1, 1),
      command_processor_(command_processor),
      trace_writer_(trace_writer) {}

WebGPURenderTargetCache::~WebGPURenderTargetCache() {
  Shutdown(true);
}

bool WebGPURenderTargetCache::Initialize(uint32_t /*shared_memory_binding_count*/) {
  WebGPUDevice& device = command_processor_.device();
  edram_ = std::make_unique<WebGPUEdram>(device);
  bool edram_ok = false;
  device.RunSync([&]() { edram_ok = edram_->Initialize(); });
  if (!edram_ok) {
    device.RunSync([&]() { edram_->Shutdown(); });
    edram_.reset();
    REXGPU_ERROR("WebGPU render target cache: EDRAM initialization failed");
    return false;
  }
  shutting_down_ = false;
  InitializeCommon();
  REXGPU_INFO(
      "WebGPU render target cache: host render targets, max {}x{}, MSAA 1x/4x (2x as 4x), "
      "D24FS8 as {}",
      GetMaxRenderTargetWidth(), GetMaxRenderTargetHeight(),
      edram_->has_depth32float_stencil8() ? "depth32float-stencil8" : "depth24plus-stencil8");
  return true;
}

void WebGPURenderTargetCache::Shutdown(bool from_destructor) {
  // The command processor drains its stream before this, so render target
  // objects can be released right away.
  shutting_down_ = true;
  DestroyAllRenderTargets(true);
  if (edram_ || readback_buffer_) {
    WebGPUDevice& device = command_processor_.device();
    device.RunSync([this]() {
      if (edram_) {
        edram_->Shutdown();
      }
      if (readback_buffer_) {
        wgpuBufferRelease(readback_buffer_);
        readback_buffer_ = nullptr;
      }
    });
    edram_.reset();
    readback_buffer_size_ = 0;
  }
  std::memset(last_update_attachments_, 0, sizeof(last_update_attachments_));
  last_update_render_pass_ = {};
  last_update_render_pass_key_ = 0;
  if (!from_destructor) {
    ShutdownCommon();
  }
}

void WebGPURenderTargetCache::ClearCache() {
  // Render target destructors defer their releases to the current stream.
  RenderTargetCache::ClearCache();
  std::memset(last_update_attachments_, 0, sizeof(last_update_attachments_));
  last_update_render_pass_ = {};
  last_update_render_pass_key_ = 0;
}

void WebGPURenderTargetCache::ReleaseCallback(void* user, WebGPUReplayState& /*state*/) {
  ReleaseObjects(static_cast<PendingRelease*>(user));
}

void WebGPURenderTargetCache::ReleaseObjects(PendingRelease* objects) {
  for (WGPUTextureView view : objects->views) {
    if (view) {
      wgpuTextureViewRelease(view);
    }
  }
  if (objects->texture) {
    // Work already submitted keeps using it; nothing can be recorded with it
    // any more.
    wgpuTextureDestroy(objects->texture);
    wgpuTextureRelease(objects->texture);
  }
  delete objects;
}

void WebGPURenderTargetCache::ReleaseRenderTargetObjects(const PendingRelease& objects) {
  auto* pending = new PendingRelease(objects);
  if (shutting_down_) {
    WebGPUDevice& device = command_processor_.device();
    device.RunSync([pending]() { ReleaseObjects(pending); });
    return;
  }
  // Streams recorded before may still reference the textures until replayed:
  // release after the current stream's submit.
  command_processor_.command_stream().AddSubmittedCallback(ReleaseCallback, pending);
}

uint32_t WebGPURenderTargetCache::GetMaxRenderTargetWidth() const {
  return command_processor_.device().limits().maxTextureDimension2D;
}

uint32_t WebGPURenderTargetCache::GetMaxRenderTargetHeight() const {
  return command_processor_.device().limits().maxTextureDimension2D;
}

RenderTargetCache::RenderTarget* WebGPURenderTargetCache::CreateRenderTarget(
    RenderTargetKey key) {
  if (!edram_) {
    return nullptr;
  }
  WGPUTextureFormat format = key.is_depth ? GetDepthTextureFormat(key.GetDepthFormat())
                                          : GetColorTextureFormat(key.GetColorFormat());
  if (format == WGPUTextureFormat_Undefined) {
    REXGPU_ERROR("WebGPU render target cache: no host format for {}", key.GetDebugName());
    return nullptr;
  }
  WebGPUEdramSurface surface;
  surface.host_format = format;
  surface.width = key.GetWidth() * draw_resolution_scale_x();
  surface.height =
      GetRenderTargetHeight(key.pitch_tiles_at_32bpp, key.msaa_samples) * draw_resolution_scale_y();
  surface.format = key.resource_format;
  surface.is_depth = key.is_depth != 0;
  surface.msaa_samples = uint32_t(key.msaa_samples);
  surface.base_tiles = key.base_tiles;
  surface.pitch_tiles = key.GetPitchTiles();
  if (!surface.width || !surface.height) {
    return nullptr;
  }
  uint32_t sample_count = GetHostSampleCount(key.msaa_samples);
  std::string label = key.GetDebugName();

  WGPUTexture texture = nullptr;
  WebGPUDevice& device = command_processor_.device();
  device.RunSync([&]() {
    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.label = MakeStringView(label);
    desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
    if (sample_count == 1 && !surface.is_depth) {
      // Debugging / tests (MSAA and depth24plus textures can't be copied).
      desc.usage |= WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst;
    }
    desc.size = {surface.width, surface.height, 1};
    desc.format = format;
    desc.sampleCount = sample_count;
    texture = wgpuDeviceCreateTexture(device.device(), &desc);
    if (!texture) {
      return;
    }
    surface.attachment_view = wgpuTextureCreateView(texture, nullptr);
    if (surface.is_depth) {
      WGPUTextureViewDescriptor view_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
      view_desc.aspect = WGPUTextureAspect_DepthOnly;
      surface.sample_view = wgpuTextureCreateView(texture, &view_desc);
      view_desc.aspect = WGPUTextureAspect_StencilOnly;
      surface.stencil_view = wgpuTextureCreateView(texture, &view_desc);
    } else {
      surface.sample_view = surface.attachment_view;
    }
  });
  if (!texture) {
    REXGPU_ERROR("WebGPU render target cache: failed to create {} ({}x{})", label, surface.width,
                 surface.height);
    return nullptr;
  }
  ++statistics_.render_targets_created;
  REXGPU_DEBUG("WebGPU render target cache: created {} ({}x{}, {} samples)", label, surface.width,
               surface.height, sample_count);
  return new WebGPURenderTarget(key, *this, texture, surface);
}

void WebGPURenderTargetCache::PerformTransfersAndResolveClears(
    uint32_t render_target_count, RenderTarget* const* render_targets,
    const std::vector<Transfer>* transfers, const uint64_t* clear_values,
    const Transfer::Rectangle* clear_rectangle) {
  bool clearing = clear_values && clear_rectangle;
  bool any_work = false;
  for (uint32_t i = 0; i < render_target_count; ++i) {
    if (render_targets[i] && (clearing || !transfers[i].empty())) {
      any_work = true;
      break;
    }
  }
  if (!any_work || !edram_) {
    return;
  }
  command_processor_.EndRenderPass();
  WebGPUCommandStream& stream = command_processor_.command_stream();

  // 1: dump every transfer source range to the EDRAM buffer, before any
  // destination is written (a destination may be another one's source).
  bool compute_pass_open = false;
  for (uint32_t i = 0; i < render_target_count; ++i) {
    if (!render_targets[i]) {
      continue;
    }
    for (const Transfer& transfer : transfers[i]) {
      auto* source = static_cast<WebGPURenderTarget*>(transfer.source);
      if (!source) {
        continue;
      }
      if (!compute_pass_open) {
        stream.CmdBeginComputePass();
        compute_pass_open = true;
      }
      // Ranges are within [0, kEdramTileCount); one after the source base
      // wrapped around is addressed past the end (the dump wraps the EDRAM
      // address, not the source-relative tile).
      uint32_t first_tile = transfer.start_tiles;
      if (first_tile < source->key().base_tiles) {
        first_tile += xenos::kEdramTileCount;
      }
      edram_->RecordDump(stream, source->surface(), first_tile, 0,
                         transfer.end_tiles - transfer.start_tiles, 1);
      ++statistics_.transfers;
    }
  }
  if (compute_pass_open) {
    stream.CmdEndComputePass();
  }

  // 2: load each destination's ranges from the EDRAM buffer, then clear.
  for (uint32_t i = 0; i < render_target_count; ++i) {
    auto* dest = static_cast<WebGPURenderTarget*>(render_targets[i]);
    if (!dest) {
      continue;
    }
    RenderTargetKey key = dest->key();
    load_rectangles_.clear();
    for (const Transfer& transfer : transfers[i]) {
      if (!transfer.source) {
        continue;
      }
      Transfer::Rectangle rectangles[Transfer::kMaxRectanglesWithCutout];
      uint32_t rectangle_count =
          transfer.GetRectangles(key.base_tiles, key.GetPitchTiles(), key.msaa_samples,
                                 key.Is64bpp(), rectangles, clearing ? clear_rectangle : nullptr);
      for (uint32_t j = 0; j < rectangle_count; ++j) {
        WebGPUEdramRectangle rectangle;
        rectangle.x = rectangles[j].x_pixels * draw_resolution_scale_x();
        rectangle.y = rectangles[j].y_pixels * draw_resolution_scale_y();
        rectangle.width = rectangles[j].width_pixels * draw_resolution_scale_x();
        rectangle.height = rectangles[j].height_pixels * draw_resolution_scale_y();
        load_rectangles_.push_back(rectangle);
      }
    }
    WebGPUEdramRectangle clear;
    if (clearing) {
      clear.x = clear_rectangle->x_pixels * draw_resolution_scale_x();
      clear.y = clear_rectangle->y_pixels * draw_resolution_scale_y();
      clear.width = clear_rectangle->width_pixels * draw_resolution_scale_x();
      clear.height = clear_rectangle->height_pixels * draw_resolution_scale_y();
    }
    edram_->RecordLoad(stream, dest->surface(), load_rectangles_.data(),
                       uint32_t(load_rectangles_.size()), clearing ? &clear : nullptr,
                       clearing ? clear_values[i] : 0);
  }
}

bool WebGPURenderTargetCache::Update(bool is_rasterization_done,
                                     reg::RB_DEPTHCONTROL normalized_depth_control,
                                     uint32_t normalized_color_mask, const Shader& vertex_shader) {
  if (!RenderTargetCache::Update(is_rasterization_done, normalized_depth_control,
                                 normalized_color_mask, vertex_shader)) {
    last_update_render_pass_key_ = 0;
    return false;
  }
  auto rb_surface_info = register_file().Get<reg::RB_SURFACE_INFO>();
  RenderTarget* const* render_targets = last_update_accumulated_render_targets();

  PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets, render_targets,
                                   last_update_transfers());

  RenderPassFormatKey format_key;
  format_key.msaa_samples = rb_surface_info.msaa_samples;
  WebGPUCommandStream::RenderPassDesc desc;
  uint32_t pass_width = UINT32_MAX, pass_height = UINT32_MAX;
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if (render_targets[i]) {
      const WebGPUEdramSurface& surface =
          static_cast<WebGPURenderTarget*>(render_targets[i])->surface();
      pass_width = std::min(pass_width, surface.width);
      pass_height = std::min(pass_height, surface.height);
    }
  }
  if (render_targets[0]) {
    auto* depth = static_cast<WebGPURenderTarget*>(render_targets[0]);
    format_key.depth_and_color_used |= 1;
    format_key.depth_format = depth->key().GetDepthFormat();
    desc.has_depth_stencil = true;
    WGPURenderPassDepthStencilAttachment& attachment = desc.depth_stencil;
    attachment = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    attachment.view = depth->surface().attachment_view;
    attachment.depthLoadOp = WGPULoadOp_Load;
    attachment.depthStoreOp = WGPUStoreOp_Store;
    attachment.depthClearValue = 1.0f;
    attachment.stencilLoadOp = WGPULoadOp_Load;
    attachment.stencilStoreOp = WGPUStoreOp_Store;
  }
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    auto* color = static_cast<WebGPURenderTarget*>(render_targets[1 + i]);
    if (!color) {
      continue;
    }
    format_key.depth_and_color_used |= uint32_t(1) << (1 + i);
    xenos::ColorRenderTargetFormat color_format = color->key().GetColorFormat();
    switch (i) {
      case 0:
        format_key.color_0_format = color_format;
        break;
      case 1:
        format_key.color_1_format = color_format;
        break;
      case 2:
        format_key.color_2_format = color_format;
        break;
      default:
        format_key.color_3_format = color_format;
        break;
    }
    // Unbound slots below keep a null view (a hole in colorAttachments).
    desc.SetColor(i, color->surface().attachment_view, false);
  }
  if (!format_key.depth_and_color_used) {
    // WebGPU render passes need at least one attachment.
    // TODO(WP4): a dummy attachment for draws without render targets.
    ++statistics_.updates_without_attachments;
    last_update_render_pass_key_ = 0;
    return false;
  }
  if (std::memcmp(last_update_attachments_, render_targets, sizeof(last_update_attachments_))) {
    std::memcpy(last_update_attachments_, render_targets, sizeof(last_update_attachments_));
    if (++attachments_generation_ == 0) {
      attachments_generation_ = 1;
    }
  }
  last_update_render_pass_ = desc;
  last_update_render_pass_key_ = (uint64_t(attachments_generation_) << 32) | format_key.key;
  last_update_render_pass_width_ = pass_width;
  last_update_render_pass_height_ = pass_height;
  return true;
}

void WebGPURenderTargetCache::DumpRenderTargets(uint32_t dump_base,
                                                uint32_t dump_row_length_used,
                                                uint32_t dump_rows, uint32_t dump_pitch) {
  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows, dump_pitch,
                                 dump_rectangles_);
  WebGPUCommandStream& stream = command_processor_.command_stream();
  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    auto* render_target = static_cast<WebGPURenderTarget*>(rectangle.render_target);
    if (!render_target) {
      continue;
    }
    ResolveCopyDumpRectangle::Dispatch dispatches[ResolveCopyDumpRectangle::kMaxDispatches];
    uint32_t dispatch_count =
        rectangle.GetDispatches(dump_pitch, dump_row_length_used, dispatches);
    for (uint32_t i = 0; i < dispatch_count; ++i) {
      const ResolveCopyDumpRectangle::Dispatch& dispatch = dispatches[i];
      edram_->RecordDump(stream, render_target->surface(), dump_base + dispatch.offset,
                         dump_pitch, dispatch.width_tiles, dispatch.height_tiles);
    }
  }
}

bool WebGPURenderTargetCache::Resolve(memory::Memory& memory, WebGPUSharedMemory& shared_memory,
                                      WebGPUTextureCache& texture_cache,
                                      uint32_t& written_address_out,
                                      uint32_t& written_length_out) {
  written_address_out = 0;
  written_length_out = 0;
  if (!edram_) {
    return false;
  }

  draw_util::ResolveInfo resolve_info;
  if (!draw_util::GetResolveInfo(register_file(), memory, trace_writer_, draw_resolution_scale_x(),
                                 draw_resolution_scale_y(), IsFixedRG16TruncatedToMinus1To1(),
                                 IsFixedRGBA16TruncatedToMinus1To1(), resolve_info)) {
    return false;
  }
  if (!resolve_info.coordinate_info.width_div_8 || !resolve_info.height_div_8) {
    return true;
  }
  // Compute and copies: outside the command processor's render pass.
  command_processor_.EndRenderPass();

  bool copied = true;
  if (resolve_info.copy_dest_extent_length) {
    draw_util::ResolveCopyShaderConstants copy_constants;
    uint32_t group_count_x, group_count_y;
    draw_util::ResolveCopyShaderIndex copy_shader = resolve_info.GetCopyShader(
        draw_resolution_scale_x(), draw_resolution_scale_y(), copy_constants, group_count_x,
        group_count_y);
    uint32_t dest_format = uint32_t(resolve_info.copy_dest_info.copy_dest_format);
    uint32_t dest_bpp_log2 = WebGPUEdram::GetResolveDestBppLog2(dest_format);
    uint32_t extent_start = resolve_info.copy_dest_extent_start;
    uint32_t extent_length = resolve_info.copy_dest_extent_length;
    if (copy_shader == draw_util::ResolveCopyShaderIndex::kUnknown ||
        dest_bpp_log2 == UINT32_MAX) {
      REXGPU_WARN("WebGPU resolve: unsupported destination format {}", dest_format);
    } else if (!shared_memory.RequestRange(extent_start, extent_length)) {
      // The gaps between the written pixels must hold current data.
      REXGPU_ERROR("WebGPU resolve: failed to request the destination 0x{:08X} ({} bytes)",
                   extent_start, extent_length);
      copied = false;
    } else {
      bool is_depth = resolve_info.IsCopyingDepth();
      draw_util::ResolveEdramInfo edram_info =
          is_depth ? resolve_info.depth_edram_info : resolve_info.color_edram_info;
      const draw_util::ResolveCopyDestCoordinateInfo& dest_coordinates =
          resolve_info.copy_dest_coordinate_info;
      WebGPUResolveConstants constants = {};
      constants.edram_base_tiles = edram_info.base_tiles;
      constants.edram_pitch_tiles = edram_info.pitch_tiles;
      constants.edram_msaa_samples = uint32_t(edram_info.msaa_samples);
      constants.edram_is_depth = edram_info.is_depth;
      constants.edram_format = edram_info.format;
      constants.edram_is_64bpp = edram_info.format_is_64bpp;
      constants.offset_x = resolve_info.coordinate_info.edram_offset_x_div_8 << 3;
      constants.offset_y = resolve_info.coordinate_info.edram_offset_y_div_8 << 3;
      constants.width = resolve_info.coordinate_info.width_div_8 << 3;
      constants.height = resolve_info.height_div_8 << 3;
      constants.dest_pitch = dest_coordinates.pitch_aligned_div_32 << 5;
      constants.dest_height = dest_coordinates.height_aligned_div_32 << 5;
      constants.dest_offset_x = dest_coordinates.offset_x_div_8 << 3;
      constants.dest_offset_y = dest_coordinates.offset_y_div_8 << 3;
      constants.dest_array = resolve_info.copy_dest_info.copy_dest_array;
      constants.dest_slice = resolve_info.copy_dest_info.copy_dest_slice;
      constants.dest_format = dest_format;
      constants.dest_endian = uint32_t(resolve_info.copy_dest_info.copy_dest_endian);
      constants.dest_exp_bias = resolve_info.copy_dest_info.copy_dest_exp_bias;
      constants.dest_swap = resolve_info.copy_dest_info.copy_dest_swap;
      constants.sample_select = uint32_t(dest_coordinates.copy_sample_select);
      constants.raw = copy_shader <= draw_util::ResolveCopyShaderIndex::kFast64bpp4xMSAA;
      constants.dest_bpp_log2 = dest_bpp_log2;
      // Bind only the destination (from copy_dest_base <= extent start, at
      // the storage offset alignment), not the whole shared memory.
      uint64_t bind_offset = resolve_info.copy_dest_base & ~uint64_t(255);
      uint64_t bind_end = (uint64_t(extent_start) + extent_length + 3) & ~uint64_t(3);
      constants.dest_base = int32_t(resolve_info.copy_dest_base - bind_offset);

      uint32_t dump_base, dump_row_length_used, dump_rows, dump_pitch;
      resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used, dump_rows, dump_pitch);
      WebGPUCommandStream& stream = command_processor_.command_stream();
      stream.CmdBeginComputePass();
      DumpRenderTargets(dump_base, dump_row_length_used, dump_rows, dump_pitch);
      edram_->RecordResolve(stream, constants, shared_memory.buffer(), bind_offset,
                            bind_end - bind_offset);
      stream.CmdEndComputePass();

      // Consumes the web dirty map for the range first (CPU writes before the
      // resolve), then marks it valid and fires the watches.
      shared_memory.RangeWrittenByGpu(extent_start, extent_length);
      texture_cache.MarkRangeAsResolved(extent_start, extent_length);
      written_address_out = extent_start;
      written_length_out = extent_length;
      ++statistics_.resolves;
      statistics_.resolve_bytes += extent_length;
      if (REXCVAR_GET(webgpu_readback_resolve)) {
        ReadbackResolve(memory, shared_memory, extent_start, extent_length);
      }
    }
  }

  if (resolve_info.IsClearingDepth() || resolve_info.IsClearingColor()) {
    Transfer::Rectangle clear_rectangle;
    RenderTarget* clear_render_targets[2];
    // False for an empty region (success) or an error: nothing to do either way.
    if (PrepareHostRenderTargetsResolveClear(resolve_info, clear_rectangle,
                                             clear_render_targets[0], clear_transfers_[0],
                                             clear_render_targets[1], clear_transfers_[1])) {
      uint64_t clear_values[2];
      clear_values[0] = resolve_info.rb_depth_clear;
      clear_values[1] =
          resolve_info.rb_color_clear | (uint64_t(resolve_info.rb_color_clear_lo) << 32);
      PerformTransfersAndResolveClears(2, clear_render_targets, clear_transfers_, clear_values,
                                       &clear_rectangle);
      ++statistics_.resolve_clears;
    }
  }
  return copied;
}

bool WebGPURenderTargetCache::ReadbackResolve(memory::Memory& memory,
                                              WebGPUSharedMemory& shared_memory,
                                              uint32_t address, uint32_t length) {
  uint8_t* guest = memory.TranslatePhysical(address);
  if (!guest || !length) {
    return false;
  }
  uint64_t start = address & ~uint64_t(3);
  uint64_t size = ((uint64_t(address) + length + 3) & ~uint64_t(3)) - start;
  WebGPUDevice& device = command_processor_.device();
  if (size > readback_buffer_size_) {
    uint64_t new_size = (size + 0xFFFF) & ~uint64_t(0xFFFF);
    device.RunSync([&]() {
      if (readback_buffer_) {
        wgpuBufferRelease(readback_buffer_);
      }
      WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
      desc.label = MakeStringView("resolve readback");
      desc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
      desc.size = new_size;
      readback_buffer_ = wgpuDeviceCreateBuffer(device.device(), &desc);
    });
    readback_buffer_size_ = readback_buffer_ ? new_size : 0;
    if (!readback_buffer_) {
      return false;
    }
  }
  command_processor_.EndRenderPass();
  command_processor_.command_stream().CmdCopyBufferToBuffer(shared_memory.buffer(), start,
                                                            readback_buffer_, 0, size);
  command_processor_.Submit(false);

  // mapAsync resolves once the GPU is done with the submission above (queued
  // on the device thread after its replay). The copy into guest memory is a
  // plain memcpy: it doesn't mark the web dirty map, and the range is already
  // valid in the shared memory (RangeWrittenByGpu).
  struct MapState {
    WGPUBuffer buffer;
    uint64_t size;
    uint8_t* guest;
    uint32_t skip;
    uint32_t length;
    bool ok;
    WebGPUDevice::Completion* completion;
  } state{readback_buffer_, size, guest, uint32_t(address - start), length, false, nullptr};
  bool proxied = device.RunSyncWithCompletion([&state](WebGPUDevice::Completion completion) {
    state.completion = new WebGPUDevice::Completion(completion);
    WGPUBufferMapCallbackInfo info = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    info.mode = WGPUCallbackMode_AllowSpontaneous;
    info.userdata1 = &state;
    info.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* userdata1, void*) {
      auto* s = static_cast<MapState*>(userdata1);
      if (status == WGPUMapAsyncStatus_Success) {
        const auto* data =
            static_cast<const uint8_t*>(wgpuBufferGetConstMappedRange(s->buffer, 0, s->size));
        if (data) {
          std::memcpy(s->guest, data + s->skip, s->length);
          s->ok = true;
        }
        wgpuBufferUnmap(s->buffer);
      }
      WebGPUDevice::Completion* completion = s->completion;
      s->completion = nullptr;
      completion->Finish();
      delete completion;
    };
    wgpuBufferMapAsync(state.buffer, WGPUMapMode_Read, 0, state.size, info);
  });
  if (proxied && state.ok) {
    ++statistics_.readbacks;
    return true;
  }
  REXGPU_WARN("WebGPU resolve readback of 0x{:08X} ({} bytes) failed", address, length);
  return false;
}

void WebGPURenderTargetCache::RestoreEdramSnapshot(const void* snapshot) {
  if (!edram_ || !snapshot) {
    return;
  }
  // The snapshot is the guest EDRAM layout, which is what the EDRAM buffer
  // holds: upload it and load a full-EDRAM 32bpp render target from it
  // (rgba8unorm round-trips any dword exactly).
  RenderTarget* render_target = PrepareFullEdram1280xRenderTargetForSnapshotRestoration(
      xenos::ColorRenderTargetFormat::k_8_8_8_8);
  if (!render_target) {
    return;
  }
  void* data = command_processor_.RecordUpload(edram_->buffer(), 0, WebGPUEdram::kBufferSize);
  if (!data) {
    return;
  }
  std::memcpy(data, snapshot, WebGPUEdram::kBufferSize);
  const WebGPUEdramSurface& surface =
      static_cast<WebGPURenderTarget*>(render_target)->surface();
  WebGPUEdramRectangle rectangle;
  rectangle.width = surface.width;
  rectangle.height = surface.height;
  edram_->RecordLoad(command_processor_.command_stream(), surface, &rectangle, 1);
}

}  // namespace rex::graphics::webgpu
