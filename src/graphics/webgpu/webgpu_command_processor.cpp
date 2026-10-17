/**
 * @file        graphics/webgpu/webgpu_command_processor.cpp
 * @brief       See rex/graphics/webgpu/webgpu_command_processor.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/webgpu_command_processor.h>

#include <algorithm>
#include <cstring>

#include <rex/bit.h>
#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/webgpu/webgpu_device.h>
#include <rex/graphics/webgpu/webgpu_graphics_system.h>
#include <rex/graphics/webgpu/webgpu_guest_output.h>
#include <rex/graphics/webgpu/webgpu_pipeline_cache.h>
#include <rex/graphics/webgpu/webgpu_present_pass.h>
#include <rex/graphics/webgpu/webgpu_primitive_processor.h>
#include <rex/graphics/webgpu/webgpu_render_target_cache.h>
#include <rex/graphics/webgpu/webgpu_shared_memory.h>
#include <rex/graphics/webgpu/webgpu_texture_cache.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/system/xtypes.h>

REXCVAR_DEFINE_BOOL(webgpu_log_frames, false, "GPU",
                    "WebGPU: log submission/upload statistics on every swap (else every 600)");

namespace rex::graphics::webgpu {

WebGPUCommandProcessor::WebGPUCommandProcessor(WebGPUGraphicsSystem* graphics_system,
                                               system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state),
      webgpu_graphics_system_(graphics_system),
      device_(*graphics_system->webgpu_device()) {}

WebGPUCommandProcessor::~WebGPUCommandProcessor() = default;

void WebGPUCommandProcessor::ClearCaches() {
  CommandProcessor::ClearCaches();
  vertex_buffers_in_sync_[0] = 0;
  vertex_buffers_in_sync_[1] = 0;
  if (pipeline_cache_) {
    // Shader objects are referenced by the shared CP's active shaders.
    active_vertex_shader_ = nullptr;
    active_pixel_shader_ = nullptr;
    pipeline_cache_->ClearCache();
  }
  if (render_target_cache_) {
    render_target_cache_->ClearCache();
  }
  if (texture_cache_) {
    texture_cache_->ClearCache();
  }
}

void WebGPUCommandProcessor::InvalidateGpuMemory() {
  if (shared_memory_) {
    shared_memory_->InvalidateAllPages();
  }
}

void WebGPUCommandProcessor::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                                     uint32_t title_id, bool blocking) {
  CommandProcessor::InitializeShaderStorage(cache_root, title_id, blocking);
  if (pipeline_cache_) {
    pipeline_cache_->InitializeShaderStorage(cache_root, title_id, blocking);
  }
}

void WebGPUCommandProcessor::TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) {
  if (shared_memory_) {
    shared_memory_->MemoryInvalidationCallback(base_ptr, length, true);
  }
}

void WebGPUCommandProcessor::RestoreEdramSnapshot(const void* snapshot) {
  if (render_target_cache_) {
    EndRenderPass();
    render_target_cache_->RestoreEdramSnapshot(snapshot);
  }
}

bool WebGPUCommandProcessor::SetupContext() {
  // CP thread.
  command_queue_ = std::make_unique<WebGPUCommandQueue>(device_, submission_tracker_);
  if (!command_queue_->Initialize()) {
    REXGPU_ERROR("WebGPU: command queue initialization failed");
    return false;
  }

  shared_memory_ = std::make_unique<WebGPUSharedMemory>(*this, *memory_, trace_writer_);
  if (!shared_memory_->Initialize()) {
    REXGPU_ERROR("WebGPU: shared memory initialization failed");
    return false;
  }

  // ---- WP3: primitive processor (docs/port/webgpu.md section 4). ----
  primitive_processor_ = std::make_unique<WebGPUPrimitiveProcessor>(
      *register_file_, *memory_, trace_writer_, *shared_memory_, *command_queue_,
      [this]() { EndRenderPass(); }, shared_memory_->buffer());
  if (!primitive_processor_->Initialize()) {
    REXGPU_ERROR("WebGPU: primitive processor initialization failed");
    return false;
  }
  // ---- end WP3 ----

  texture_cache_ =
      std::make_unique<WebGPUTextureCache>(*this, *register_file_, *shared_memory_, *memory_);
  if (!texture_cache_->Initialize()) {
    REXGPU_ERROR("WebGPU: texture cache initialization failed");
    return false;
  }

  render_target_cache_ =
      std::make_unique<WebGPURenderTargetCache>(*this, *register_file_, *memory_, trace_writer_);
  if (!render_target_cache_->Initialize(shared_memory_->binding_count())) {
    REXGPU_ERROR("WebGPU: render target cache initialization failed");
    return false;
  }

  pipeline_cache_ = std::make_unique<WebGPUPipelineCache>(*this, *register_file_);
  if (!pipeline_cache_->Initialize()) {
    REXGPU_ERROR("WebGPU: pipeline cache initialization failed");
    return false;
  }

  bool present_pass_ok = false;
  device_.RunSync([&]() {
    present_pass_ = new WebGPUPresentPass(device_);
    present_pass_ok = present_pass_->Initialize();
  });
  if (!present_pass_ok) {
    REXGPU_ERROR("WebGPU: swap pass initialization failed");
    return false;
  }

  REXGPU_INFO("WebGPU command processor ready (adapter: {})", device_.adapter_description());
  return true;
}

void WebGPUCommandProcessor::ShutdownContext() {
  // CP thread. Drain the stream first: the caches' objects may be in it.
  if (command_queue_) {
    Submit(true);
  }
  if (pipeline_cache_) {
    pipeline_cache_->Shutdown();
  }
  active_vertex_shader_ = nullptr;
  active_pixel_shader_ = nullptr;
  pipeline_cache_.reset();
  if (render_target_cache_) {
    render_target_cache_->Shutdown();
  }
  render_target_cache_.reset();
  if (texture_cache_) {
    texture_cache_->Shutdown();
  }
  texture_cache_.reset();
  primitive_processor_.reset();  // WP3
  if (shared_memory_) {
    shared_memory_->Shutdown();
  }
  shared_memory_.reset();
  if (present_pass_) {
    WebGPUPresentPass* present_pass = present_pass_;
    present_pass_ = nullptr;
    device_.RunSync([present_pass]() { delete present_pass; });
  }
  if (command_queue_) {
    command_queue_->Shutdown();
  }
  command_queue_.reset();
}

void WebGPUCommandProcessor::EndRenderPass() {
  WebGPUCommandStream& stream = command_stream();
  if (stream.in_pass()) {
    stream.EndPass();
  }
  open_render_pass_key_ = 0;
}

void WebGPUCommandProcessor::BeginRenderPass(const WebGPUCommandStream::RenderPassDesc& desc,
                                             uint64_t key) {
  WebGPUCommandStream& stream = command_stream();
  if (stream.in_render_pass() && key && key == open_render_pass_key_) {
    return;
  }
  EndRenderPass();
  stream.CmdBeginRenderPass(desc);
  open_render_pass_key_ = key;
}

void* WebGPUCommandProcessor::RecordUpload(WGPUBuffer destination, uint64_t destination_offset,
                                           uint64_t size) {
  EndRenderPass();
  if (command_stream().upload_bytes() + size > kStreamUploadFlushBytes &&
      command_stream().upload_bytes()) {
    Submit(false);
  }
  return command_stream().CmdUploadToBuffer(destination, destination_offset, size);
}

uint64_t WebGPUCommandProcessor::Submit(bool wait_for_replay) {
  EndRenderPass();
  uint64_t submission =
      wait_for_replay ? command_queue_->SubmitAndWaitReplayed() : command_queue_->Submit();
  ++statistics_.submissions;
  if (render_target_cache_) {
    render_target_cache_->OnStreamSubmitted();
  }
  return submission;
}

void WebGPUCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  CommandProcessor::WriteRegister(index, value);
  if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
      index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    uint32_t fetch_dword = index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    if (texture_cache_) {
      texture_cache_->TextureFetchConstantWritten(fetch_dword / 6);
    }
    uint32_t vfetch_index = fetch_dword / 2;
    vertex_buffers_in_sync_[vfetch_index >> 6] &= ~(uint64_t(1) << (vfetch_index & 63));
  }
}

Shader* WebGPUCommandProcessor::LoadShader(xenos::ShaderType shader_type,
                                           uint32_t /*guest_address*/,
                                           const uint32_t* host_address, uint32_t dword_count) {
  return pipeline_cache_->LoadShader(shader_type, host_address, dword_count);
}

bool WebGPUCommandProcessor::RequestVertexBuffers(const Shader& vertex_shader) {
  const RegisterFile& regs = *register_file_;
  const Shader::ConstantRegisterMap& constant_map = vertex_shader.constant_register_map();
  for (uint32_t i = 0; i < rex::countof(constant_map.vertex_fetch_bitmap); ++i) {
    uint32_t vfetch_bits_remaining = constant_map.vertex_fetch_bitmap[i];
    uint32_t j;
    while (rex::bit_scan_forward(vfetch_bits_remaining, &j)) {
      vfetch_bits_remaining &= ~(uint32_t(1) << j);
      uint32_t vfetch_index = i * 32 + j;
      uint64_t vfetch_bit = uint64_t(1) << (vfetch_index & 63);
      if (vertex_buffers_in_sync_[vfetch_index >> 6] & vfetch_bit) {
        continue;
      }
      xenos::xe_gpu_vertex_fetch_t vfetch_constant = regs.GetVertexFetch(vfetch_index);
      switch (vfetch_constant.type) {
        case xenos::FetchConstantType::kVertex:
          break;
        case xenos::FetchConstantType::kInvalidVertex:
          if (REXCVAR_GET(gpu_allow_invalid_fetch_constants)) {
            break;
          }
          REXGPU_WARN("WebGPU: vertex fetch constant {} ({:08X} {:08X}) has the \"invalid\" type",
                      vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
          return false;
        default:
          REXGPU_WARN("WebGPU: vertex fetch constant {} ({:08X} {:08X}) is invalid", vfetch_index,
                      vfetch_constant.dword_0, vfetch_constant.dword_1);
          return false;
      }
      if (!shared_memory_->RequestRange(vfetch_constant.address << 2, vfetch_constant.size << 2)) {
        REXGPU_ERROR("WebGPU: failed to request vertex buffer 0x{:08X} ({} bytes)",
                     vfetch_constant.address << 2, vfetch_constant.size << 2);
        return false;
      }
      vertex_buffers_in_sync_[vfetch_index >> 6] |= vfetch_bit;
    }
  }
  return true;
}

bool WebGPUCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t /*index_count*/,
                                       IndexBufferInfo* /*index_buffer_info*/,
                                       bool /*major_mode_explicit*/) {
  const RegisterFile& regs = *register_file_;
  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    return IssueCopy();
  }
  ++statistics_.draws;

  Shader* vertex_shader = active_vertex_shader();
  if (!vertex_shader) {
    REXGPU_ERROR("WebGPU IssueDraw: no vertex shader");
    return false;
  }
  pipeline_cache_->AnalyzeShaderUcode(*vertex_shader);
  bool memexport_used_vertex = vertex_shader->memexport_eM_written() != 0;
  if (memexport_used_vertex && !memexport_logged_) {
    // docs/port/webgpu.md section 4: vertex memexport needs the compute path.
    memexport_logged_ = true;
    REXGPU_WARN("WebGPU: first vertex shader with memexport seen (ucode hash {:016X})",
                vertex_shader->ucode_data_hash());
  }

  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool is_rasterization_done = draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  if (regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 0 && is_rasterization_done) {
    return true;
  }
  Shader* pixel_shader = nullptr;
  if (is_rasterization_done) {
    if (edram_mode == xenos::EdramMode::kColorDepth) {
      pixel_shader = active_pixel_shader();
      if (pixel_shader) {
        pipeline_cache_->AnalyzeShaderUcode(*pixel_shader);
        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) {
          pixel_shader = nullptr;
        }
      }
    }
  } else if (!memexport_used_vertex) {
    // No rasterization and no memexport: no effect.
    return true;
  }

  reg::RB_DEPTHCONTROL normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);
  uint32_t normalized_color_mask =
      pixel_shader ? draw_util::GetNormalizedColorMask(regs, pixel_shader->writes_color_targets())
                   : 0;
  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      pixel_shader ? (vertex_shader->writes_interpolators() &
                      pixel_shader->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(),
                                                             regs.Get<reg::SQ_CONTEXT_MISC>(),
                                                             ps_param_gen_pos))
                   : 0;

  // Guest memory the draw reads must be current in the shared memory before
  // anything else (on web this consumes the dirty map, docs/port/webgpu.md
  // section 8). Uploads are recorded outside render passes.
  if (!RequestVertexBuffers(*vertex_shader)) {
    return false;
  }
  // ---- WP3: primitive processing. Index conversion (endianness stays for the
  // VS, non-0xFFFF reset indices, quad lists / fans / loops), point and
  // rectangle lists as VS-expanded two-triangle strips, and the guest DMA index
  // range request in the shared memory. ----
  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  if (!primitive_processor_->Process(primitive_processing_result)) {
    return false;
  }
  if (!primitive_processing_result.host_draw_vertex_count) {
    // Nothing to draw.
    return true;
  }
  // ---- end WP3 ----

  // Render targets (WP4), then the pipeline for them (WP2).
  if (!render_target_cache_->Update(is_rasterization_done, normalized_depth_control,
                                    normalized_color_mask, *vertex_shader)) {
    ++statistics_.draws_dropped;
    return true;
  }
  WebGPUDrawPipelineState pipeline_state;
  pipeline_state.vertex_shader = vertex_shader;
  pipeline_state.pixel_shader = pixel_shader;
  pipeline_state.guest_primitive_type = prim_type;
  pipeline_state.normalized_depth_control = normalized_depth_control;
  pipeline_state.normalized_color_mask = normalized_color_mask;
  pipeline_state.interpolator_mask = interpolator_mask;
  pipeline_state.ps_param_gen_pos = ps_param_gen_pos;
  pipeline_state.host_primitive_type = primitive_processing_result.host_primitive_type;
  pipeline_state.host_vertex_shader_type = primitive_processing_result.host_vertex_shader_type;
  pipeline_state.host_vertex_shader_type_valid = true;
  pipeline_state.render_pass_key = render_target_cache_->last_update_render_pass_key();
  pipeline_state.depth_texture_format = render_target_cache_->GetDepthTextureFormat(
      WebGPURenderTargetCache::GetRenderPassFormatKey(pipeline_state.render_pass_key)
          .depth_format);
  PipelineId pipeline;
  if (!pipeline_cache_->ConfigurePipeline(pipeline_state, pipeline)) {
    ++statistics_.draws_dropped;
    return true;
  }

  BeginRenderPass(render_target_cache_->last_update_render_pass(),
                  render_target_cache_->last_update_render_pass_key());
  WebGPUCommandStream& stream = command_stream();
  stream.CmdSetPipeline(pipeline);
  // TODO(WP2/WP4): constant buffers (AllocateConstants + bind group 1; the
  // index-related system constants come from
  // WebGPUPrimitiveProcessor::ApplyIndexSystemConstants), shared memory /
  // texture bind groups, viewport/scissor from draw_util.
  // ---- WP3: the processed draw's host index buffer. ----
  WebGPUPrimitiveProcessor::IndexBufferBinding index_buffer_binding;
  if (primitive_processor_->GetIndexBufferBinding(primitive_processing_result,
                                                  index_buffer_binding)) {
    stream.CmdSetIndexBuffer(index_buffer_binding.buffer, index_buffer_binding.format,
                             index_buffer_binding.offset, index_buffer_binding.size);
    stream.CmdDrawIndexed(primitive_processing_result.host_draw_vertex_count);
  } else {
    stream.CmdDraw(primitive_processing_result.host_draw_vertex_count);
  }
  // ---- end WP3 ----
  return true;
}

bool WebGPUCommandProcessor::IssueCopy() {
  ++statistics_.copies;
  const RegisterFile& regs = *register_file_;
  if (regs.Get<reg::RB_COPY_CONTROL>().color_clear_enable) {
    has_last_color_clear_ = true;
    last_color_clear_ = regs[XE_GPU_REG_RB_COLOR_CLEAR];
  }
  // Resolves are compute work and copies: outside render passes.
  EndRenderPass();
  uint32_t written_address, written_length;
  return render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_,
                                       written_address, written_length);
}

void WebGPUCommandProcessor::EncodeSwap(void* user, WebGPUReplayState& /*state*/,
                                        WGPUCommandEncoder encoder) {
  auto* self = static_cast<WebGPUCommandProcessor*>(user);
  const SwapBlit& blit = self->swap_blit_;
  WebGPUPresentPass::Frame frame;
  frame.clear_color = blit.clear_color;
  frame.source = blit.source;
  frame.dest_x = 0;
  frame.dest_y = 0;
  frame.dest_width = blit.width;
  frame.dest_height = blit.height;
  self->present_pass_->EncodeClearAndBlit(encoder, blit.target, kGuestOutputFormat, blit.width,
                                          blit.height, frame);
}

void WebGPUCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height) {
  uint64_t swap_index = ++statistics_.swaps;
  vertex_buffers_in_sync_[0] = 0;
  vertex_buffers_in_sync_[1] = 0;
  // WP4: render targets bound in the previous frame are not kept accumulated.
  render_target_cache_->BeginFrame();

  uint32_t texture_width = 0, texture_height = 0;
  xenos::TextureFormat texture_format = xenos::TextureFormat(0);
  WGPUTextureView swap_view =
      texture_cache_->RequestSwapTexture(texture_width, texture_height, texture_format);
  uint32_t output_width = std::clamp<uint32_t>(
      frontbuffer_width ? frontbuffer_width : texture_width, 1, 8192);
  uint32_t output_height = std::clamp<uint32_t>(
      frontbuffer_height ? frontbuffer_height : texture_height, 1, 8192);

  WGPUColor clear_color = {0.0, 0.0, 0.0, 1.0};
  if (has_last_color_clear_) {
    // Debug fill while there is no swap texture: the last resolve clear value
    // read as 8_8_8_8 (R in the low byte).
    clear_color.r = double(last_color_clear_ & 0xFF) / 255.0;
    clear_color.g = double((last_color_clear_ >> 8) & 0xFF) / 255.0;
    clear_color.b = double((last_color_clear_ >> 16) & 0xFF) / 255.0;
  }

  bool refreshed = false;
  ui::Presenter* presenter = graphics_system_->presenter();
  if (presenter && !device_.is_lost()) {
    system::X_VIDEO_MODE video_mode;
    kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
    uint32_t display_width = std::max(uint32_t(1), uint32_t(video_mode.display_width));
    uint32_t display_height = std::max(uint32_t(1), uint32_t(video_mode.display_height));
    refreshed = presenter->RefreshGuestOutput(
        output_width, output_height, display_width, display_height,
        [&](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
          auto& webgpu_context = static_cast<WebGPUGuestOutputRefreshContext&>(context);
          swap_blit_.target = webgpu_context.view();
          swap_blit_.source = swap_view;
          swap_blit_.width = webgpu_context.width();
          swap_blit_.height = webgpu_context.height();
          swap_blit_.clear_color = clear_color;
          EndRenderPass();
          command_stream().CmdEncoderCallback(EncodeSwap, this);
          // The contract: the writes are queue-submitted when this returns.
          Submit(true);
          context.SetIs8bpc(true);
          return true;
        });
  }
  uint64_t swap_submission = refreshed ? submission_tracker_.submitted() : Submit(false);

  // Bound the GPU latency: wait for the swap kMaxFramesInFlight swaps back.
  uint64_t& slot = swap_submissions_[swap_index % kMaxFramesInFlight];
  if (slot && !submission_tracker_.AwaitCompleted(slot)) {
    REXGPU_WARN("WebGPU: submission {} not completed after 10 s (lost: {})", slot,
                device_.is_lost());
  }
  slot = swap_submission;
  primitive_processor_->EndFrame();  // WP3: after the latency wait

  if (swap_index == 1 || swap_index % 600 == 0 || REXCVAR_GET(webgpu_log_frames)) {
    const WebGPUReplayStats replay = command_queue_->last_replay_stats();
    REXGPU_INFO(
        "WebGPU: swap #{} front buffer {:08X} {}x{} presented={} | draws {} (dropped {}) "
        "resolves {} | submissions {} replayed {} completed {} | shared memory uploads {} "
        "({} MiB) | last replay {} commands, encode {:.3f} ms, submit {:.3f} ms | uncaptured "
        "errors {}",
        swap_index, frontbuffer_ptr, frontbuffer_width, frontbuffer_height, refreshed,
        statistics_.draws, statistics_.draws_dropped, statistics_.copies,
        submission_tracker_.submitted(), submission_tracker_.replayed(),
        submission_tracker_.completed(), shared_memory_->upload_count(),
        shared_memory_->uploaded_bytes() >> 20, replay.commands, replay.encode_ms,
        replay.submit_ms, device_.uncaptured_error_count());
  }
}

}  // namespace rex::graphics::webgpu
