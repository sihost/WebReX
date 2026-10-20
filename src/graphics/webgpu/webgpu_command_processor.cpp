/**
 * @file        graphics/webgpu/webgpu_command_processor.cpp
 * @brief       See rex/graphics/webgpu/webgpu_command_processor.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/webgpu_command_processor.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include <emscripten/heap.h>
#include <malloc.h>
#include <unistd.h>
#endif

#include <rex/bit.h>
#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
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
#include <rex/graphics/webgpu/webgpu_tessellator.h>
#include <rex/graphics/webgpu/webgpu_texture_cache.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/system/xtypes.h>

REXCVAR_DEFINE_BOOL(webgpu_log_frames, false, "GPU",
                    "WebGPU: log submission/upload statistics on every swap (else every 600)");
REXCVAR_DEFINE_BOOL(webgpu_vertex_memexport, true, "GPU",
                    "WebGPU: run memexporting vertex shaders as compute dispatches "
                    "(HostVertexShaderType::kMemExportCompute) before their draw. Off: the "
                    "exports are dropped (WGSL has no vertex-stage storage writes). Compared "
                    "with native D3D12 through gpu_draw_dump mx-in/mx-out "
                    "(docs/port/gates/README.md, gate 4)");
REXCVAR_DEFINE_BOOL(webgpu_readback_memexport, false, "GPU",
                    "WebGPU: after each vertex memexport dispatch, copy the exported ranges "
                    "back into guest memory (synchronous, like readback_memexport with "
                    "readback_memexport_fast=false natively). Diagnostics: makes the "
                    "gpu_draw_dump_swap memexport output hashes comparable with native");
REXCVAR_DEFINE_BOOL(webgpu_swap_gamma_ramp, true, "GPU",
                    "WebGPU: apply the guest display gamma ramp (PWL for 2_10_10_10 front "
                    "buffers, else the 256-entry table) when copying the front buffer to the "
                    "guest output, as the Vulkan and D3D12 backends do");

REXCVAR_DEFINE_INT32(webgpu_occlusion_query_log, 0, "GPU",
                     "WebGPU: log the first N guest occlusion query events (EVENT_WRITE_ZPD begin / "
                     "end, the sample count address, and the draws since the previous event)");

REXCVAR_DEFINE_BOOL(webgpu_dummy_attachment_for_no_output, false, "GPU",
                    "WebGPU: also draw the draws without any render target that write nothing "
                    "(normally skipped, they have no effect) into the dummy attachment that draws "
                    "without render targets but with effects (pixel shader memexport) get. "
                    "Diagnostics for that path");

REXCVAR_DEFINE_INT32(webgpu_frame_stats_interval_ms, 0, "GPU",
                     "WebGPU: every N ms of swaps, log frame pacing: swaps/s, the swap-to-swap "
                     "interval p50/p95/p99/max, the CP thread's wait for the GPU (frame latency "
                     "bound), device-thread replay time, uploads and draws per frame. 0 = off");

REXCVAR_DEFINE_INT32(webgpu_pipeline_wait_ms, 0, "GPU",
                     "WebGPU: per-frame budget (ms) of the GPU command thread waiting for the "
                     "pipelines of draws whose shaders are still converting or whose pipeline the "
                     "browser is still creating; such draws are otherwise skipped. -1 = wait for "
                     "every pipeline (as synchronous creation), 0 = never wait");

namespace rex::graphics::webgpu {

using SpirvTr = SpirvShaderTranslator;

struct WebGPUCommandProcessor::DrawState {
  SpirvTr::SystemConstants system_constants;

  // Constant buffers written into the stream of submission
  // constants_submission (bit per SpirvTr::ConstantBuffer in constants_valid).
  uint64_t constants_submission = 0;
  uint32_t constants_valid = 0;
  uint32_t constant_offsets[SpirvTr::kConstantBufferCount] = {};
  // Float constants packed into the last written VS / PS buffers.
  uint64_t float_map_vertex[4] = {};
  uint64_t float_map_pixel[4] = {};

  // State set in the render pass with serial pass_serial.
  uint64_t pass_serial = UINT64_MAX;
  PipelineId pipeline = kInvalidPipelineId;
  bool shared_memory_bound = false;
  bool constants_bound = false;
  uint32_t bound_offsets[SpirvTr::kConstantBufferCount] = {};
  bool texture_group_bound[2] = {};
  uint64_t texture_group_hash[2] = {};
  bool viewport_set = false;
  float viewport[6] = {};
  bool scissor_set = false;
  uint32_t scissor[4] = {};
  bool blend_constant_set = false;
  float blend_constant[4] = {};
  bool stencil_reference_set = false;
  uint32_t stencil_reference = 0;

  std::vector<WGPUBindGroupEntry> texture_entries[2];
  bool unfilterable_logged = false;

  DrawState() { std::memset(&system_constants, 0, sizeof(system_constants)); }
  void ResetPass(uint64_t serial) {
    pass_serial = serial;
    pipeline = kInvalidPipelineId;
    shared_memory_bound = false;
    constants_bound = false;
    texture_group_bound[0] = texture_group_bound[1] = false;
    viewport_set = false;
    scissor_set = false;
    blend_constant_set = false;
    stencil_reference_set = false;
  }
};

WebGPUCommandProcessor::WebGPUCommandProcessor(WebGPUGraphicsSystem* graphics_system,
                                               system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state),
      webgpu_graphics_system_(graphics_system),
      device_(*graphics_system->webgpu_device()),
      draw_state_(std::make_unique<DrawState>()) {}

WebGPUCommandProcessor::~WebGPUCommandProcessor() = default;

void WebGPUCommandProcessor::ClearCaches() {
  CommandProcessor::ClearCaches();
  vertex_buffers_in_sync_[0] = 0;
  vertex_buffers_in_sync_[1] = 0;
  // Shader bindings and pipelines go away with the pipeline cache.
  draw_state_->constants_valid = 0;
  draw_state_->ResetPass(UINT64_MAX);
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

  // Draw resolution scaling (as VulkanCommandProcessor::SetupContext).
  if (!TextureCache::GetConfigDrawResolutionScale(draw_resolution_scale_x_,
                                                  draw_resolution_scale_y_)) {
    REXGPU_WARN("WebGPU: the draw resolution scale is clamped to {}x{}", draw_resolution_scale_x_,
                draw_resolution_scale_y_);
  }
  if (draw_resolution_scale_x_ > 1 || draw_resolution_scale_y_ > 1) {
    REXGPU_INFO("WebGPU: draw resolution scale {}x{}", draw_resolution_scale_x_,
                draw_resolution_scale_y_);
  }
  texture_cache_ = std::make_unique<WebGPUTextureCache>(
      *this, *register_file_, *shared_memory_, *memory_, draw_resolution_scale_x_,
      draw_resolution_scale_y_);
  if (!texture_cache_->Initialize()) {
    REXGPU_ERROR("WebGPU: texture cache initialization failed");
    return false;
  }

  render_target_cache_ = std::make_unique<WebGPURenderTargetCache>(
      *this, *register_file_, *memory_, trace_writer_, draw_resolution_scale_x_,
      draw_resolution_scale_y_);
  if (!render_target_cache_->Initialize(shared_memory_->binding_count())) {
    REXGPU_ERROR("WebGPU: render target cache initialization failed");
    return false;
  }

  pipeline_cache_ = std::make_unique<WebGPUPipelineCache>(*this, *register_file_);
  if (!pipeline_cache_->Initialize()) {
    REXGPU_ERROR("WebGPU: pipeline cache initialization failed");
    return false;
  }

  // Tessellated draws: not fatal if missing (they are dropped then).
  tessellator_ = std::make_unique<WebGPUTessellator>(*this, *register_file_);
  if (!tessellator_->Initialize()) {
    tessellator_.reset();
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
  if (tessellator_) {
    tessellator_->Shutdown();
  }
  tessellator_.reset();
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
  ++render_pass_serial_;
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
    draw_state_->constants_valid &= ~(UINT32_C(1) << SpirvTr::kConstantBufferFetch);
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
             index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    draw_state_->constants_valid &=
        ~(UINT32_C(1) << (index >= XE_GPU_REG_SHADER_CONSTANT_256_X
                              ? SpirvTr::kConstantBufferFloatPixel
                              : SpirvTr::kConstantBufferFloatVertex));
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    draw_state_->constants_valid &= ~(UINT32_C(1) << SpirvTr::kConstantBufferBoolLoop);
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
      if (shared_memory_->VerifyRange(vfetch_constant.address << 2, vfetch_constant.size << 2,
                                      "vertex fetch", vfetch_index) &&
          !shared_memory_->RequestRange(vfetch_constant.address << 2,
                                        vfetch_constant.size << 2)) {
        return false;
      }
      vertex_buffers_in_sync_[vfetch_index >> 6] |= vfetch_bit;
    }
  }
  return true;
}

bool WebGPUCommandProcessor::UpdateSystemConstantValues(
    bool primitive_polygonal, const PrimitiveProcessor::ProcessingResult& primitive_result,
    const draw_util::ViewportInfo& viewport_info, uint32_t used_texture_mask,
    uint32_t compute_memexport_vertex_count) {
  // VulkanCommandProcessor::UpdateSystemConstantValues for host render targets
  // at 1x resolution (no FSI, no resolution-scaled textures).
  const RegisterFile& regs = *register_file_;
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();

  SpirvTr::SystemConstants& stored = draw_state_->system_constants;
  SpirvTr::SystemConstants c;
  std::memcpy(&c, &stored, sizeof(c));

  uint32_t flags = 0;
  WebGPUPrimitiveProcessor::ApplyIndexSystemConstants<SpirvTr>(
      regs, primitive_result, compute_memexport_vertex_count, flags, c);
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= SpirvTr::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= SpirvTr::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= SpirvTr::kSysFlag_WNotReciprocal;
  }
  if (primitive_polygonal) {
    flags |= SpirvTr::kSysFlag_PrimitivePolygonal;
  }
  if (draw_util::IsPrimitiveLine(regs)) {
    flags |= SpirvTr::kSysFlag_PrimitiveLine;
  }
  flags |= uint32_t(rb_surface_info.msaa_samples) << SpirvTr::kSysFlag_MsaaSamples_Shift;
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= SpirvTr::kSysFlag_DepthFloat24;
  }
  xenos::CompareFunction alpha_test_function = rb_colorcontrol.alpha_test_enable
                                                   ? rb_colorcontrol.alpha_func
                                                   : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function) << SpirvTr::kSysFlag_AlphaPassIfLess_Shift;
  reg::RB_COLOR_INFO color_infos[xenos::kMaxColorRenderTargets];
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    color_infos[i] = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    // 8_8_8_8_GAMMA: linear rgba16unorm (no conversion in the shaders), or
    // rgba8unorm with the PWL gamma in the shaders (WebGPUEdram::gamma_as_unorm16).
    if (color_infos[i].color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
        !WebGPUEdram::gamma_as_unorm16()) {
      flags |= SpirvTr::kSysFlag_ConvertColor0ToGamma << i;
    }
  }
  c.flags = flags;

  // User clip planes, tightly packed.
  if (!pa_cl_clip_cntl.clip_disable) {
    float* plane = c.user_clip_planes[0];
    uint32_t planes_remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t plane_index;
    while (rex::bit_scan_forward(planes_remaining, &plane_index)) {
      planes_remaining &= ~(UINT32_C(1) << plane_index);
      std::memcpy(plane, &regs[XE_GPU_REG_PA_CL_UCP_0_X + plane_index * 4], 4 * sizeof(float));
      plane += 4;
    }
  }

  c.tessellation_factor_range_min = regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  c.tessellation_factor_range_max = regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;

  for (uint32_t i = 0; i < 3; ++i) {
    c.ndc_scale[i] = viewport_info.ndc_scale[i];
    c.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    c.point_vertex_diameter_min = float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
    c.point_vertex_diameter_max = float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
    c.point_constant_diameter[0] = float(pa_su_point_size.width) * (2.0f / 16.0f);
    c.point_constant_diameter[1] = float(pa_su_point_size.height) * (2.0f / 16.0f);
    c.point_screen_diameter_to_ndc_radius[0] =
        float(draw_resolution_scale_x_) / float(std::max(viewport_info.xy_extent[0], uint32_t(1)));
    c.point_screen_diameter_to_ndc_radius[1] =
        float(draw_resolution_scale_y_) / float(std::max(viewport_info.xy_extent[1], uint32_t(1)));
  }

  // Texture signedness and, since the translator never relies on view
  // swizzles (image_view_format_swizzle = false), the host swizzle: identity
  // when the view applies it already (texture-component-swizzle).
  c.textures_resolution_scaled = 0;
  bool views_swizzle = texture_cache_->views_apply_swizzle();
  uint32_t textures_remaining = used_texture_mask;
  uint32_t texture_index;
  while (rex::bit_scan_forward(textures_remaining, &texture_index)) {
    textures_remaining &= ~(UINT32_C(1) << texture_index);
    c.textures_resolution_scaled |=
        uint32_t(texture_cache_->IsActiveTextureResolutionScaled(texture_index)) << texture_index;
    uint32_t& signs = c.texture_swizzled_signs[texture_index >> 2];
    uint32_t signs_shift = 8 * (texture_index & 3);
    signs = (signs & ~(UINT32_C(0xFF) << signs_shift)) |
            (uint32_t(texture_cache_->GetActiveTextureSwizzledSigns(texture_index)) << signs_shift);
    uint32_t& swizzles = c.texture_swizzles[texture_index >> 1];
    uint32_t swizzle_shift = 12 * (texture_index & 1);
    uint32_t swizzle = views_swizzle ? uint32_t(xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA)
                                     : texture_cache_->GetActiveTextureHostSwizzle(texture_index);
    swizzles = (swizzles & ~(UINT32_C(0xFFF) << swizzle_shift)) | ((swizzle & 0xFFF) << swizzle_shift);
  }

  c.alpha_test_reference = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  c.alpha_to_mask = rb_colorcontrol.alpha_to_mask_enable
                        ? (rb_colorcontrol.value >> 24) | (UINT32_C(1) << 8)
                        : 0;

  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    int32_t color_exp_bias = color_infos[i].color_exp_bias;
    // 16_16 and 16_16_16_16 are float16 targets holding the guest value with the
    // full -32...32 range (the EDRAM dump/load shaders convert 0x7FFF <-> 32.0).
    // Unlike Vulkan's snorm16 host targets, nothing is remapped to -1...1, so no
    // extra -5 exponent bias here (that made those targets 32x too dark).
    uint32_t scale_bits = UINT32_C(0x3F800000) + uint32_t(color_exp_bias << 23);
    std::memcpy(&c.color_exp_bias[i], &scale_bits, sizeof(float));
  }

  if (!std::memcmp(&c, &stored, sizeof(c))) {
    return false;
  }
  std::memcpy(&stored, &c, sizeof(c));
  return true;
}

bool WebGPUCommandProcessor::UpdateConstantBuffers(const Shader& vertex_shader,
                                                   const Shader* pixel_shader,
                                                   uint32_t* offsets_out) {
  const RegisterFile& regs = *register_file_;
  DrawState& ds = *draw_state_;
  uint64_t submission = GetCurrentSubmission();
  if (ds.constants_submission != submission) {
    // A new stream: its constant arena is empty.
    ds.constants_submission = submission;
    ds.constants_valid = 0;
  }
  const Shader::ConstantRegisterMap& map_vertex = vertex_shader.constant_register_map();
  if (std::memcmp(ds.float_map_vertex, map_vertex.float_bitmap, sizeof(ds.float_map_vertex))) {
    std::memcpy(ds.float_map_vertex, map_vertex.float_bitmap, sizeof(ds.float_map_vertex));
    ds.constants_valid &= ~(UINT32_C(1) << SpirvTr::kConstantBufferFloatVertex);
  }
  uint64_t map_pixel[4] = {};
  if (pixel_shader) {
    std::memcpy(map_pixel, pixel_shader->constant_register_map().float_bitmap, sizeof(map_pixel));
  }
  if (std::memcmp(ds.float_map_pixel, map_pixel, sizeof(map_pixel))) {
    std::memcpy(ds.float_map_pixel, map_pixel, sizeof(map_pixel));
    ds.constants_valid &= ~(UINT32_C(1) << SpirvTr::kConstantBufferFloatPixel);
  }

  WebGPUCommandStream& stream = command_stream();
  auto allocate = [&](uint32_t buffer, uint32_t size) -> uint8_t* {
    uint32_t offset;
    auto* data = static_cast<uint8_t*>(stream.AllocateConstants(size, offset));
    ds.constant_offsets[buffer] = offset;
    ds.constants_valid |= UINT32_C(1) << buffer;
    statistics_.constant_bytes += size;
    return data;
  };
  auto write_floats = [&](uint32_t buffer, const uint64_t* map, uint32_t first_register) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      count += rex::bit_count(map[i]);
    }
    uint8_t* data = allocate(buffer, uint32_t(sizeof(float) * 4 * std::max(count, 1u)));
    if (!data) {
      return false;
    }
    for (uint32_t i = 0; i < 4; ++i) {
      uint64_t bits = map[i];
      uint32_t index;
      while (rex::bit_scan_forward(bits, &index)) {
        bits &= ~(UINT64_C(1) << index);
        std::memcpy(data, &regs[first_register + (i << 8) + (index << 2)], sizeof(float) * 4);
        data += sizeof(float) * 4;
      }
    }
    return true;
  };
  if (!(ds.constants_valid & (UINT32_C(1) << SpirvTr::kConstantBufferSystem))) {
    uint8_t* data = allocate(SpirvTr::kConstantBufferSystem, sizeof(SpirvTr::SystemConstants));
    if (!data) {
      return false;
    }
    std::memcpy(data, &ds.system_constants, sizeof(SpirvTr::SystemConstants));
  }
  if (!(ds.constants_valid & (UINT32_C(1) << SpirvTr::kConstantBufferFloatVertex)) &&
      !write_floats(SpirvTr::kConstantBufferFloatVertex, ds.float_map_vertex,
                    XE_GPU_REG_SHADER_CONSTANT_000_X)) {
    return false;
  }
  if (!(ds.constants_valid & (UINT32_C(1) << SpirvTr::kConstantBufferFloatPixel)) &&
      !write_floats(SpirvTr::kConstantBufferFloatPixel, ds.float_map_pixel,
                    XE_GPU_REG_SHADER_CONSTANT_256_X)) {
    return false;
  }
  if (!(ds.constants_valid & (UINT32_C(1) << SpirvTr::kConstantBufferBoolLoop))) {
    uint8_t* data = allocate(SpirvTr::kConstantBufferBoolLoop,
                             WebGPUPipelineCache::kBoolLoopConstantsBindingSize);
    if (!data) {
      return false;
    }
    std::memcpy(data, &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                WebGPUPipelineCache::kBoolLoopConstantsBindingSize);
  }
  if (!(ds.constants_valid & (UINT32_C(1) << SpirvTr::kConstantBufferFetch))) {
    uint8_t* data =
        allocate(SpirvTr::kConstantBufferFetch, WebGPUPipelineCache::kFetchConstantsBindingSize);
    if (!data) {
      return false;
    }
    std::memcpy(data, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],
                WebGPUPipelineCache::kFetchConstantsBindingSize);
  }
  std::memcpy(offsets_out, ds.constant_offsets, sizeof(ds.constant_offsets));
  return true;
}

bool WebGPUCommandProcessor::IssueMemExportCompute(
    Shader& vertex_shader, bool primitive_polygonal,
    const PrimitiveProcessor::ProcessingResult& primitive_result) {
  const RegisterFile& regs = *register_file_;
  uint32_t vertex_count = regs.Get<reg::VGT_DRAW_INITIATOR>().num_indices;
  uint32_t groups_x, groups_y;
  if (!WebGPUPrimitiveProcessor::GetMemExportComputeDispatch(vertex_count, groups_x, groups_y)) {
    return true;
  }
  std::vector<draw_util::MemExportRange> ranges;
  draw_util::AddMemExportRanges(regs, vertex_shader, ranges);
  if (ranges.empty()) {
    // No valid stream constant: nothing can be written.
    return true;
  }
  const WebGPUShaderBindings* bindings = nullptr;
  PipelineId pipeline = pipeline_cache_->GetMemExportComputePipeline(vertex_shader, bindings);
  if (pipeline == kInvalidPipelineId || !bindings) {
    ++statistics_.memexport_unsupported;
    return true;
  }
  WaitForPipeline(pipeline);
  // The destination must hold current data around the exported elements.
  for (const draw_util::MemExportRange& range : ranges) {
    if (!shared_memory_->RequestRange(range.base_address_dwords << 2, range.size_bytes)) {
      REXGPU_ERROR("WebGPU: failed to request the memexport stream at 0x{:08X} ({} bytes)",
                   range.base_address_dwords << 2, range.size_bytes);
      return false;
    }
  }
  texture_cache_->RequestTexturesForShaders(vertex_shader, nullptr);

  uint32_t used_texture_mask = 0;
  for (const Shader::TextureBinding& binding : vertex_shader.texture_bindings()) {
    used_texture_mask |= UINT32_C(1) << (binding.fetch_constant & 31);
  }
  DrawState& draw_state = *draw_state_;
  // The viewport is not used by the compute variant.
  draw_util::ViewportInfo viewport_info = {};
  if (UpdateSystemConstantValues(primitive_polygonal, primitive_result, viewport_info,
                                 used_texture_mask, vertex_count)) {
    draw_state.constants_valid &= ~(UINT32_C(1) << SpirvTr::kConstantBufferSystem);
  }
  uint32_t constant_offsets[SpirvTr::kConstantBufferCount];
  if (!UpdateConstantBuffers(vertex_shader, nullptr, constant_offsets)) {
    ++statistics_.memexport_unsupported;
    return true;
  }

  // VS textures and samplers (group 2), as for draws.
  std::vector<WGPUBindGroupEntry> texture_entries;
  uint32_t binding_index = 0;
  for (const WebGPUShaderBindings::Texture& texture : bindings->textures) {
    WGPUBindGroupEntry& entry = texture_entries.emplace_back();
    entry = WGPU_BIND_GROUP_ENTRY_INIT;
    entry.binding = binding_index++;
    entry.textureView =
        texture_cache_->IsActiveTextureFilterable(texture.fetch_constant, texture.is_signed)
            ? texture_cache_->GetActiveBindingOrNullView(texture.fetch_constant, texture.dimension,
                                                          texture.is_signed)
            : texture_cache_->GetNullView(texture.dimension);
  }
  for (const WebGPUShaderBindings::Sampler& sampler : bindings->samplers) {
    WGPUBindGroupEntry& entry = texture_entries.emplace_back();
    entry = WGPU_BIND_GROUP_ENTRY_INIT;
    entry.binding = binding_index++;
    entry.sampler = texture_cache_->UseSampler(texture_cache_->GetSamplerParameters(sampler));
    if (!entry.sampler) {
      ++statistics_.memexport_unsupported;
      return true;
    }
  }

  EndRenderPass();
  WebGPUCommandStream& stream = command_stream();
  stream.CmdBeginComputePass();
  stream.CmdSetComputePipeline(pipeline);
  WGPUBindGroupEntry shared_entries[8];
  uint32_t shared_count = std::min<uint32_t>(shared_memory_->binding_count(), 8);
  for (uint32_t i = 0; i < shared_count; ++i) {
    shared_entries[i] = WGPU_BIND_GROUP_ENTRY_INIT;
    shared_entries[i].binding = i;
    shared_entries[i].buffer = shared_memory_->buffer();
    shared_entries[i].offset = shared_memory_->binding_size() * i;
    shared_entries[i].size = shared_memory_->binding_size();
  }
  stream.CmdSetBindGroup(0, pipeline_cache_->compute_shared_memory_layout(), shared_entries,
                         shared_count);
  WGPUBindGroupEntry constant_entries[SpirvTr::kConstantBufferCount];
  const uint32_t sizes[SpirvTr::kConstantBufferCount] = {
      uint32_t(sizeof(SpirvTr::SystemConstants)),
      WebGPUPipelineCache::kFloatConstantsBindingSize,
      WebGPUPipelineCache::kFloatConstantsBindingSize,
      WebGPUPipelineCache::kBoolLoopConstantsBindingSize,
      WebGPUPipelineCache::kFetchConstantsBindingSize,
  };
  for (uint32_t i = 0; i < SpirvTr::kConstantBufferCount; ++i) {
    constant_entries[i] = WGPU_BIND_GROUP_ENTRY_INIT;
    constant_entries[i].binding = i;
    constant_entries[i].buffer = StreamConstantsBuffer();
    constant_entries[i].offset = 0;
    constant_entries[i].size = sizes[i];
  }
  stream.CmdSetBindGroup(1, pipeline_cache_->constants_layout(), constant_entries,
                         SpirvTr::kConstantBufferCount, SpirvTr::kConstantBufferCount,
                         constant_offsets);
  stream.CmdSetBindGroup(2, bindings->layout, texture_entries.data(),
                         uint32_t(texture_entries.size()));
  stream.CmdDispatch(groups_x, groups_y, 1);
  stream.CmdEndComputePass();
  // The draw that follows (if any) opens a new render pass and rebinds.
  ++statistics_.memexport_dispatches;

  // Consumes the dirty map first on web, marks valid, fires the watches
  // (textures over the exported data are reloaded).
  for (const draw_util::MemExportRange& range : ranges) {
    shared_memory_->RangeWrittenByGpu(range.base_address_dwords << 2, range.size_bytes);
  }
  // Guest memory copy: off by default on the web, delayed with
  // --webgpu_readback_memexport_delayed, immediate with --webgpu_readback_memexport
  // (WebGPUReadback::GetMemExportMode).
  for (const draw_util::MemExportRange& range : ranges) {
    render_target_cache_->ReadbackMemExport(*memory_, *shared_memory_,
                                            range.base_address_dwords << 2, range.size_bytes);
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
  ++query_draws_.draws;
  if (prim_type == xenos::PrimitiveType::kPointList) {
    ++query_draws_.point_lists;
  }

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
  if (normalized_depth_control.z_enable) {
    ++query_draws_.depth_test;
  }
  if (!normalized_color_mask) {
    ++query_draws_.no_color;
  }
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
  if (memexport_used_vertex) {
    // The exports run as a compute dispatch before the draw (if it rasterizes).
    if (!REXCVAR_GET(webgpu_vertex_memexport)) {
      ++statistics_.memexport_unsupported;
    } else if (!IssueMemExportCompute(*vertex_shader, primitive_polygonal,
                                      primitive_processing_result)) {
      return false;
    }
    if (!is_rasterization_done) {
      return true;
    }
  }
  if (!primitive_processing_result.host_draw_vertex_count) {
    // Nothing to draw.
    return true;
  }
  // ---- end WP3 ----

  if (shared_memory_->verifying()) {
    // --webgpu_verify_shared_memory: the guest DMA indices and the first 64 KiB
    // of each fetched texture's base and mip data.
    auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
    if (vgt_draw_initiator.source_select == xenos::SourceSelect::kDMA) {
      auto vgt_dma_size = regs.Get<reg::VGT_DMA_SIZE>();
      uint32_t index_bytes =
          vgt_dma_size.num_words *
          (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16 ? 2u : 4u);
      shared_memory_->VerifyRange(regs[XE_GPU_REG_VGT_DMA_BASE] & 0x1FFFFFFF, index_bytes,
                                  "index buffer", vgt_draw_initiator.num_indices);
    }
    for (const Shader* shader : {static_cast<const Shader*>(vertex_shader),
                                 static_cast<const Shader*>(pixel_shader)}) {
      if (!shader) {
        continue;
      }
      for (const Shader::TextureBinding& binding : shader->texture_bindings()) {
        xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(binding.fetch_constant & 31);
        if (fetch.base_address) {
          shared_memory_->VerifyRange(fetch.base_address << 12, 0x10000, "texture base",
                                      binding.fetch_constant);
        }
        if (fetch.mip_address) {
          shared_memory_->VerifyRange(fetch.mip_address << 12, 0x10000, "texture mips",
                                      binding.fetch_constant);
        }
      }
    }
  }

  // Textures (WP5): loads are recorded outside render passes, before the pass
  // of this draw is opened.
  texture_cache_->RequestTexturesForShaders(*vertex_shader, pixel_shader);

  // Render targets (WP4), then the pipeline for them (WP2).
  bool render_targets_bound = render_target_cache_->Update(
      is_rasterization_done, normalized_depth_control, normalized_color_mask, *vertex_shader);
  if (!render_targets_bound && render_target_cache_->last_update_without_attachments()) {
    ++query_draws_.no_attachments;
    // No render target at all. A WebGPU render pass needs an attachment, so a
    // draw whose rasterization still has an effect gets a dummy one (zero write
    // mask): pixel shader memexport (WebGPU has no host occlusion queries: the
    // guest's ZPD results are query_occlusion_fake_sample_count, as on D3D12 /
    // Vulkan once a guest query doesn't end at its begin address, and
    // VIZ_QUERY always reports visible, CommandProcessor::
    // ExecutePacketType3_VIZ_QUERY). Vertex memexport was dispatched as compute
    // above, the rasterization adds nothing to it. Draws writing nothing at all
    // (no color mask, depth, stencil or pixel memexport; for example Halo 3's
    // point-list draws inside a VIZ_QUERY) rasterize into no attachment
    // natively too, which has no effect: skipped, not dropped.
    bool rasterization_has_effect = normalized_color_mask || normalized_depth_control.z_enable ||
                                    normalized_depth_control.stencil_enable ||
                                    (pixel_shader && pixel_shader->memexport_eM_written());
    if (!rasterization_has_effect && !REXCVAR_GET(webgpu_dummy_attachment_for_no_output)) {
      ++statistics_.draws_no_output;
      if (memexport_used_vertex) {
        ++statistics_.draws_no_output_memexport_vs;
      }
      return true;
    }
    render_targets_bound = render_target_cache_->UpdateDummyAttachment();
    if (render_targets_bound) {
      ++statistics_.draws_dummy_attachment;
      if (!rasterization_has_effect) {
        ++statistics_.draws_dummy_attachment_no_output;
      }
    }
  }
  if (!render_targets_bound) {
    ++statistics_.draws_dropped;
    ++statistics_.drops_render_targets;
    if (memexport_used_vertex) {
      ++statistics_.drops_rt_memexport_vs;
    }
    if (!is_rasterization_done) {
      ++statistics_.drops_rt_no_rasterization;
    }
    // The first 8, and with webgpu_occlusion_query_log 8 more once a guest
    // occlusion query was seen (in game, with the query context).
    if (drop_details_logged_ < 8 ||
        (zpd_events_logged_ && drop_details_logged_ < 16 &&
         zpd_events_logged_ < uint32_t(std::max(REXCVAR_GET(webgpu_occlusion_query_log), 0)))) {
      ++drop_details_logged_;
      REXGPU_WARN(
          "WebGPU: draw without render targets dropped: PA_SC_VIZ_QUERY {:08X} indices {} "
          "draw initiator {:08X}, ZPD events so far {}",
          regs[XE_GPU_REG_PA_SC_VIZ_QUERY], regs.Get<reg::VGT_DRAW_INITIATOR>().num_indices,
          regs[XE_GPU_REG_VGT_DRAW_INITIATOR], zpd_events_logged_);
      REXGPU_WARN(
          "WebGPU: draw without render targets dropped: vs {:016X} (memexport {:X}) ps {:016X} "
          "(memexport {:X}) prim {} rasterization {} edram mode {} color mask {:04X} depth {} "
          "stencil {} surface pitch {} color formats {} {} {} {} PA_CL_CLIP_CNTL {:08X} "
          "PA_SU_SC_MODE_CNTL {:08X}",
          vertex_shader->ucode_data_hash(), vertex_shader->memexport_eM_written(),
          pixel_shader ? pixel_shader->ucode_data_hash() : 0,
          pixel_shader ? pixel_shader->memexport_eM_written() : 0, uint32_t(prim_type),
          is_rasterization_done, uint32_t(edram_mode), normalized_color_mask,
          uint32_t(normalized_depth_control.z_enable), uint32_t(normalized_depth_control.stencil_enable),
          regs.Get<reg::RB_SURFACE_INFO>().surface_pitch,
          uint32_t(regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[0]).color_format),
          uint32_t(regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[1]).color_format),
          uint32_t(regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[2]).color_format),
          uint32_t(regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[3]).color_format),
          regs[XE_GPU_REG_PA_CL_CLIP_CNTL], regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL]);
    }
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
  bool tessellated =
      Shader::IsHostVertexShaderTypeDomain(primitive_processing_result.host_vertex_shader_type);
  pipeline_state.tessellation_available = tessellator_ && tessellator_->available();
  if (primitive_processing_result.index_buffer_type !=
      PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
    pipeline_state.index_format =
        primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16
            ? WGPUIndexFormat_Uint16
            : WGPUIndexFormat_Uint32;
  }
  pipeline_state.render_pass_key = render_target_cache_->last_update_render_pass_key();
  pipeline_state.depth_texture_format = render_target_cache_->GetDepthTextureFormat(
      WebGPURenderTargetCache::GetRenderPassFormatKey(pipeline_state.render_pass_key)
          .depth_format);
  PipelineId pipeline;
  WebGPUDrawBindings draw_bindings;
  if (!pipeline_cache_->ConfigurePipeline(pipeline_state, pipeline, draw_bindings)) {
    ++statistics_.draws_dropped;
    ++statistics_.drops_pipeline;
    return true;
  }
  WaitForPipeline(pipeline);

  // Viewport and scissor, within the pass attachments (WebGPU validates both
  // against the render target size).
  uint32_t rt_width = std::max(render_target_cache_->last_update_render_pass_width(), 1u);
  uint32_t rt_height = std::max(render_target_cache_->last_update_render_pass_height(), 1u);
  draw_util::ViewportInfo viewport_info;
  // Host D24FS8 depth is guest * 0.5 (full_float24_in_0_to_1, as Vulkan; the
  // render target cache stores it that way); no float24 rounding in the PS.
  // origin_bottom_left = true as on D3D12: WebGPU NDC +Y points up (towards
  // framebuffer row 0), unlike Vulkan's. With false every draw was flipped
  // vertically within its viewport and the face winding (culling) reversed.
  draw_util::GetHostViewportInfo(regs, draw_resolution_scale_x_, draw_resolution_scale_y_, true,
                                 rt_width, rt_height, false, normalized_depth_control, false,
                                 true, pixel_shader && pixel_shader->writes_depth(),
                                 viewport_info);
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  scissor.offset[0] *= draw_resolution_scale_x_;
  scissor.offset[1] *= draw_resolution_scale_y_;
  scissor.extent[0] *= draw_resolution_scale_x_;
  scissor.extent[1] *= draw_resolution_scale_y_;
  for (uint32_t i = 0; i < 2; ++i) {
    uint32_t size = i ? rt_height : rt_width;
    scissor.offset[i] = std::min(scissor.offset[i], size);
    scissor.extent[i] = std::min(scissor.extent[i], size - scissor.offset[i]);
  }
  if (!viewport_info.xy_extent[0] || !viewport_info.xy_extent[1] || !scissor.extent[0] ||
      !scissor.extent[1]) {
    ++statistics_.draws_culled;
    return true;
  }

  // Constants (system constants first: they read the texture bindings).
  uint32_t used_texture_mask = 0;
  for (const Shader* shader : {static_cast<const Shader*>(vertex_shader),
                               static_cast<const Shader*>(pixel_shader)}) {
    if (shader) {
      for (const Shader::TextureBinding& binding : shader->texture_bindings()) {
        used_texture_mask |= UINT32_C(1) << (binding.fetch_constant & 31);
      }
    }
  }
  DrawState& draw_state = *draw_state_;
  if (UpdateSystemConstantValues(primitive_polygonal, primitive_processing_result, viewport_info,
                                 used_texture_mask)) {
    draw_state.constants_valid &= ~(UINT32_C(1) << SpirvTr::kConstantBufferSystem);
  }
  uint32_t constant_offsets[SpirvTr::kConstantBufferCount];
  if (!UpdateConstantBuffers(*vertex_shader, pixel_shader, constant_offsets)) {
    ++statistics_.draws_dropped;
    ++statistics_.drops_constants;
    return true;
  }

  // Texture + sampler groups (2: VS, 3: PS): textures at 0...T-1, samplers
  // after them.
  const WebGPUShaderBindings* stage_bindings[2] = {draw_bindings.vertex, draw_bindings.pixel};
  uint64_t texture_group_hashes[2];
  for (uint32_t stage = 0; stage < 2; ++stage) {
    std::vector<WGPUBindGroupEntry>& entries = draw_state.texture_entries[stage];
    entries.clear();
    const WebGPUShaderBindings* bindings = stage_bindings[stage];
    WGPUBindGroupLayout layout =
        bindings ? bindings->layout : pipeline_cache_->empty_layout();
    if (bindings) {
      uint32_t binding_index = 0;
      for (const WebGPUShaderBindings::Texture& texture : bindings->textures) {
        WGPUBindGroupEntry& entry = entries.emplace_back();
        entry = WGPU_BIND_GROUP_ENTRY_INIT;
        entry.binding = binding_index++;
        if (texture_cache_->IsActiveTextureFilterable(texture.fetch_constant, texture.is_signed)) {
          entry.textureView = texture_cache_->GetActiveBindingOrNullView(
              texture.fetch_constant, texture.dimension, texture.is_signed);
        } else {
          // The layout says float (filterable): without float32-filterable an
          // r32float etc. texture can't be bound there.
          if (!draw_state.unfilterable_logged) {
            draw_state.unfilterable_logged = true;
            REXGPU_WARN(
                "WebGPU: texture fetch constant {} has an unfilterable host format; bound as "
                "the null texture (logged once)",
                texture.fetch_constant);
          }
          entry.textureView = texture_cache_->GetNullView(texture.dimension);
        }
      }
      for (const WebGPUShaderBindings::Sampler& sampler : bindings->samplers) {
        WGPUBindGroupEntry& entry = entries.emplace_back();
        entry = WGPU_BIND_GROUP_ENTRY_INIT;
        entry.binding = binding_index++;
        entry.sampler =
            texture_cache_->UseSampler(texture_cache_->GetSamplerParameters(sampler));
        if (!entry.sampler) {
          ++statistics_.draws_dropped;
          ++statistics_.drops_sampler;
          return true;
        }
      }
    }
    texture_group_hashes[stage] =
        XXH3_64bits(entries.data(), entries.size() * sizeof(WGPUBindGroupEntry)) ^
        uint64_t(reinterpret_cast<uintptr_t>(layout));
  }

  // Tessellated draws: the compute pre-pass writing the domain points and the
  // triangle list (outside the render pass, which is reopened below).
  WebGPUTessellator::Output tessellation;
  if (tessellated && !tessellator_->RecordTessellation(primitive_processing_result, tessellation)) {
    ++statistics_.draws_dropped;
    ++statistics_.drops_tessellation;
    return true;
  }

  BeginRenderPass(render_target_cache_->last_update_render_pass(),
                  render_target_cache_->last_update_render_pass_key());
  if (draw_state.pass_serial != render_pass_serial_) {
    draw_state.ResetPass(render_pass_serial_);
  }
  WebGPUCommandStream& stream = command_stream();
  if (draw_state.pipeline != pipeline) {
    stream.CmdSetPipeline(pipeline);
    draw_state.pipeline = pipeline;
  }
  if (!draw_state.shared_memory_bound) {
    WGPUBindGroupEntry entries[8];
    uint32_t count = std::min<uint32_t>(shared_memory_->binding_count(), 8);
    for (uint32_t i = 0; i < count; ++i) {
      entries[i] = WGPU_BIND_GROUP_ENTRY_INIT;
      entries[i].binding = i;
      entries[i].buffer = shared_memory_->buffer();
      entries[i].offset = shared_memory_->binding_size() * i;
      entries[i].size = shared_memory_->binding_size();
    }
    stream.CmdSetBindGroup(0, pipeline_cache_->shared_memory_layout(), entries, count);
    draw_state.shared_memory_bound = true;
  }
  if (!draw_state.constants_bound ||
      std::memcmp(draw_state.bound_offsets, constant_offsets, sizeof(constant_offsets))) {
    WGPUBindGroupEntry entries[SpirvTr::kConstantBufferCount];
    const uint32_t sizes[SpirvTr::kConstantBufferCount] = {
        uint32_t(sizeof(SpirvTr::SystemConstants)),
        WebGPUPipelineCache::kFloatConstantsBindingSize,
        WebGPUPipelineCache::kFloatConstantsBindingSize,
        WebGPUPipelineCache::kBoolLoopConstantsBindingSize,
        WebGPUPipelineCache::kFetchConstantsBindingSize,
    };
    for (uint32_t i = 0; i < SpirvTr::kConstantBufferCount; ++i) {
      entries[i] = WGPU_BIND_GROUP_ENTRY_INIT;
      entries[i].binding = i;
      entries[i].buffer = StreamConstantsBuffer();
      entries[i].offset = 0;
      entries[i].size = sizes[i];
    }
    stream.CmdSetBindGroup(1, pipeline_cache_->constants_layout(), entries,
                           SpirvTr::kConstantBufferCount, SpirvTr::kConstantBufferCount,
                           constant_offsets);
    std::memcpy(draw_state.bound_offsets, constant_offsets, sizeof(constant_offsets));
    draw_state.constants_bound = true;
  }
  for (uint32_t stage = 0; stage < 2; ++stage) {
    if (draw_state.texture_group_bound[stage] &&
        draw_state.texture_group_hash[stage] == texture_group_hashes[stage]) {
      continue;
    }
    const std::vector<WGPUBindGroupEntry>& entries = draw_state.texture_entries[stage];
    const WebGPUShaderBindings* bindings = stage_bindings[stage];
    stream.CmdSetBindGroup(2 + stage, bindings ? bindings->layout : pipeline_cache_->empty_layout(),
                           entries.data(), uint32_t(entries.size()));
    draw_state.texture_group_bound[stage] = true;
    draw_state.texture_group_hash[stage] = texture_group_hashes[stage];
  }

  float viewport[6] = {float(viewport_info.xy_offset[0]), float(viewport_info.xy_offset[1]),
                       float(viewport_info.xy_extent[0]), float(viewport_info.xy_extent[1]),
                       std::clamp(viewport_info.z_min, 0.0f, 1.0f),
                       std::clamp(viewport_info.z_max, 0.0f, 1.0f)};
  viewport[5] = std::max(viewport[5], viewport[4]);
  if (!draw_state.viewport_set || std::memcmp(draw_state.viewport, viewport, sizeof(viewport))) {
    stream.CmdSetViewport(viewport[0], viewport[1], viewport[2], viewport[3], viewport[4],
                          viewport[5]);
    std::memcpy(draw_state.viewport, viewport, sizeof(viewport));
    draw_state.viewport_set = true;
  }
  uint32_t scissor_rect[4] = {scissor.offset[0], scissor.offset[1], scissor.extent[0],
                              scissor.extent[1]};
  if (!draw_state.scissor_set ||
      std::memcmp(draw_state.scissor, scissor_rect, sizeof(scissor_rect))) {
    stream.CmdSetScissorRect(scissor_rect[0], scissor_rect[1], scissor_rect[2], scissor_rect[3]);
    std::memcpy(draw_state.scissor, scissor_rect, sizeof(scissor_rect));
    draw_state.scissor_set = true;
  }
  float blend_constant[4] = {regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
                             regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
                             regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
                             regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA)};
  if (!draw_state.blend_constant_set ||
      std::memcmp(draw_state.blend_constant, blend_constant, sizeof(blend_constant))) {
    stream.CmdSetBlendConstant(
        {blend_constant[0], blend_constant[1], blend_constant[2], blend_constant[3]});
    std::memcpy(draw_state.blend_constant, blend_constant, sizeof(blend_constant));
    draw_state.blend_constant_set = true;
  }
  if (normalized_depth_control.stencil_enable) {
    // WebGPU has one reference for both faces: the back face one only when
    // drawing back faces only (as Vulkan without separateStencilMaskRef).
    uint32_t reference_register = XE_GPU_REG_RB_STENCILREFMASK;
    if (primitive_polygonal && normalized_depth_control.backface_enable &&
        regs.Get<reg::PA_SU_SC_MODE_CNTL>().cull_front) {
      reference_register = XE_GPU_REG_RB_STENCILREFMASK_BF;
    }
    uint32_t reference = regs.Get<reg::RB_STENCILREFMASK>(reference_register).stencilref;
    if (!draw_state.stencil_reference_set || draw_state.stencil_reference != reference) {
      stream.CmdSetStencilReference(reference);
      draw_state.stencil_reference = reference;
      draw_state.stencil_reference_set = true;
    }
  }
  ++statistics_.draws_recorded;
  ++query_draws_.recorded;
  if (tessellated) {
    ++statistics_.draws_tessellated;
    stream.CmdSetVertexBuffer(0, tessellation.vertex_buffer, 0, tessellation.vertex_buffer_size);
    stream.CmdSetIndexBuffer(tessellation.index_buffer, WGPUIndexFormat_Uint32, 0,
                             tessellation.index_buffer_size);
    stream.CmdDrawIndexed(tessellation.index_count);
    return true;
  }
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

bool WebGPUCommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader,
                                                                uint32_t packet, uint32_t count) {
  if (zpd_events_logged_ < uint32_t(std::max(REXCVAR_GET(webgpu_occlusion_query_log), 0))) {
    ++zpd_events_logged_;
    uint32_t address = register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR];
    const auto* counts =
        memory_->TranslatePhysical<const xenos::xe_gpu_depth_sample_counts*>(address);
    const uint32_t kQueryFinished = rex::byte_swap(0xFFFFFEED);
    bool is_end = counts && ((counts->ZPass_A == kQueryFinished &&
                              counts->ZPass_B == kQueryFinished) ||
                             (counts->ZFail_A == kQueryFinished &&
                              counts->ZFail_B == kQueryFinished));
    REXGPU_INFO(
        "WebGPU ZPD #{}: {} sample counts {:08X} (swap {}) | draws since the last ZPD: {} "
        "(recorded {}, dropped without attachments {}, depth test {}, no color writes {}, "
        "point lists {})",
        zpd_events_logged_, is_end ? "end" : "begin", address, statistics_.swaps,
        query_draws_.draws, query_draws_.recorded, query_draws_.no_attachments,
        query_draws_.depth_test, query_draws_.no_color, query_draws_.point_lists);
  }
  query_draws_ = {};
  return CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);
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
  frame.gamma_ramp = WebGPUPresentPass::Frame::GammaRamp(blit.gamma_mode);
  frame.gamma_ramp_data = blit.gamma_ramp;
  frame.gamma_rb_swap = blit.gamma_rb_swap;
  frame.dest_x = 0;
  frame.dest_y = 0;
  frame.dest_width = blit.width;
  frame.dest_height = blit.height;
  self->present_pass_->EncodeClearAndBlit(encoder, blit.target, kGuestOutputFormat, blit.width,
                                          blit.height, frame);
}

namespace {
double SteadyMs() {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

void WebGPUCommandProcessor::UpdateFramePacing(double gpu_wait_ms) {
  FramePacing& p = frame_pacing_;
  const double now = SteadyMs();
  const double replay_ms = command_queue_->replay_ms_total();
  if (p.interval_start_ms == 0.0) {
    p.interval_start_ms = p.last_swap_ms = now;
    p.replay_ms_start = replay_ms;
    p.upload_bytes_start = shared_memory_->uploaded_bytes();
    p.constant_bytes_start = statistics_.constant_bytes;
    p.draws_start = statistics_.draws;
    return;
  }
  p.intervals_ms.push_back(float(now - p.last_swap_ms));
  p.last_swap_ms = now;
  p.gpu_wait_ms += gpu_wait_ms;
  const double elapsed_ms = now - p.interval_start_ms;
  if (elapsed_ms < double(REXCVAR_GET(webgpu_frame_stats_interval_ms))) {
    return;
  }
  const size_t n = p.intervals_ms.size();
  std::vector<float> sorted = p.intervals_ms;
  std::sort(sorted.begin(), sorted.end());
  auto pct = [&](double q) { return sorted[std::min(n - 1, size_t(q * double(n - 1) + 0.5))]; };
  const uint64_t upload_bytes = shared_memory_->uploaded_bytes();
  REXGPU_INFO(
      "WebGPU: frame pacing: {} swaps in {:.0f} ms = {:.1f}/s | interval ms p50 {:.2f} p95 {:.2f} "
      "p99 {:.2f} max {:.2f} | per frame: CP wait for GPU {:.2f} ms, device replay {:.2f} ms, "
      "draws {:.0f}, shared memory uploads {:.2f} MiB, constants {:.2f} MiB",
      n, elapsed_ms, double(n) * 1000.0 / elapsed_ms, pct(0.5), pct(0.95), pct(0.99), sorted.back(),
      p.gpu_wait_ms / double(n), (replay_ms - p.replay_ms_start) / double(n),
      double(statistics_.draws - p.draws_start) / double(n),
      double(upload_bytes - p.upload_bytes_start) / double(n) / 1048576.0,
      double(statistics_.constant_bytes - p.constant_bytes_start) / double(n) / 1048576.0);
  p.intervals_ms.clear();
  p.interval_start_ms = now;
  p.gpu_wait_ms = 0.0;
  p.replay_ms_start = replay_ms;
  p.upload_bytes_start = upload_bytes;
  p.constant_bytes_start = statistics_.constant_bytes;
  p.draws_start = statistics_.draws;
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
  bool swap_needs_rb_swap = false;
  bool swap_scaled = false;
  WGPUTextureView swap_view = texture_cache_->RequestSwapTexture(
      texture_width, texture_height, texture_format, &swap_needs_rb_swap, &swap_scaled);

  // Display gamma ramp (VulkanCommandProcessor::IssueSwap: D3D initializes the
  // 256-entry table for 8_8_8_8 output and the PWL ramp for 2_10_10_10).
  using GammaRamp = WebGPUPresentPass::Frame::GammaRamp;
  GammaRamp gamma_mode = GammaRamp::kNone;
  if (swap_view && REXCVAR_GET(webgpu_swap_gamma_ramp)) {
    bool pwl = texture_format == xenos::TextureFormat::k_2_10_10_10 ||
               texture_format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;
    gamma_mode = pwl ? GammaRamp::kPwl : GammaRamp::kTable256;
    if (pwl) {
      static_assert(sizeof(swap_blit_.gamma_ramp) == 128 * 3 * sizeof(reg::DC_LUT_PWL_DATA));
      std::memcpy(swap_blit_.gamma_ramp, gamma_ramp_pwl_rgb(), sizeof(swap_blit_.gamma_ramp));
    } else {
      std::memcpy(swap_blit_.gamma_ramp, gamma_ramp_256_entry_table(),
                  256 * sizeof(reg::DC_LUT_30_COLOR));
    }
    uint64_t ramp_hash = XXH3_64bits(swap_blit_.gamma_ramp, sizeof(swap_blit_.gamma_ramp)) ^
                         uint64_t(gamma_mode);
    if (ramp_hash != logged_gamma_ramp_hash_) {
      logged_gamma_ramp_hash_ = ramp_hash;
      // A few points of the curve (10-bit output for inputs 0, 1/4, 1/2, 3/4, 1).
      auto sample = [&](uint32_t channel, float x) -> uint32_t {
        if (pwl) {
          uint32_t v = std::min(uint32_t(x * 1023.0f + 0.5f), 1023u);
          reg::DC_LUT_PWL_DATA e;
          e.value = swap_blit_.gamma_ramp[(v >> 3) * 3 + channel];
          return std::min(uint32_t((e.base + ((v & 7) * e.delta) / 8) >> 6), 1023u);
        }
        uint32_t v = std::min(uint32_t(x * 255.0f + 0.5f), 255u);
        return (swap_blit_.gamma_ramp[v] >> (20 - 10 * channel)) & 0x3FF;
      };
      REXGPU_INFO(
          "WebGPU: swap gamma ramp {} (front buffer format {}): R {} {} {} {} {}, G {} {} {} {} "
          "{}, B {} {} {} {} {} (of 1023)",
          pwl ? "PWL" : "256-entry table", uint32_t(texture_format), sample(0, 0.0f),
          sample(0, 0.25f), sample(0, 0.5f), sample(0, 0.75f), sample(0, 1.0f), sample(1, 0.0f),
          sample(1, 0.25f), sample(1, 0.5f), sample(1, 0.75f), sample(1, 1.0f), sample(2, 0.0f),
          sample(2, 0.25f), sample(2, 0.5f), sample(2, 0.75f), sample(2, 1.0f));
    }
  }
  // A resolution-scaled front buffer is presented at the host resolution.
  uint32_t output_scale_x = swap_scaled ? draw_resolution_scale_x_ : 1;
  uint32_t output_scale_y = swap_scaled ? draw_resolution_scale_y_ : 1;
  uint32_t output_width = std::clamp<uint32_t>(
      frontbuffer_width ? frontbuffer_width * output_scale_x : texture_width, 1,
      8192 * output_scale_x);
  uint32_t output_height = std::clamp<uint32_t>(
      frontbuffer_height ? frontbuffer_height * output_scale_y : texture_height, 1,
      8192 * output_scale_y);

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
          swap_blit_.gamma_mode = uint32_t(gamma_mode);
          swap_blit_.gamma_rb_swap = swap_needs_rb_swap;
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
  const bool frame_stats = REXCVAR_GET(webgpu_frame_stats_interval_ms) > 0;
  const double gpu_wait_start_ms = frame_stats ? SteadyMs() : 0.0;
  if (slot && !submission_tracker_.AwaitCompleted(slot)) {
    REXGPU_WARN("WebGPU: submission {} not completed after 10 s (lost: {})", slot,
                device_.is_lost());
  }
  slot = swap_submission;
  if (frame_stats) {
    UpdateFramePacing(SteadyMs() - gpu_wait_start_ms);
  }
  primitive_processor_->EndFrame();  // WP3: after the latency wait
  // As D3D12CommandProcessor at the end of a frame (clear_memory_page_state,
  // on by default): pages not written by the GPU become invalid, so vertex and
  // index data are uploaded again on their next request even if a CPU write
  // escaped the dirty map. Costly (MW2: every vertex/index page of the frame,
  // ~17 MiB/frame, vs ~1 MiB with it off), but still needed: with it off,
  // --webgpu_verify_shared_memory finds MW2 menu vertex/index pages near
  // 0x1FC00000 changed without a dirty mark (some guest writer escapes the map).
  if (REXCVAR_GET(clear_memory_page_state)) {
    shared_memory_->SetSystemPageBlocksValidWithGpuDataWritten();
  }

  pipeline_wait_frame_ms_ = 0.0;

  if (swap_index == 1 || swap_index % 600 == 0 || REXCVAR_GET(webgpu_log_frames)) {
    const WebGPUReplayStats replay = command_queue_->last_replay_stats();
    const WebGPUPipelineCache::Statistics& pipelines = pipeline_cache_->statistics();
    const WebGPUTextureCache::Statistics& textures = texture_cache_->statistics();
    REXGPU_INFO(
        "WebGPU: swap #{} front buffer {:08X} {}x{} presented={} swap texture {}x{} fmt {} ({}) | "
        "draws {} recorded {} dropped {} culled {} unsupported {} | resolves {} | no render "
        "target: {} skipped (no output, {} with vs memexport), {} on the dummy "
        "attachment ({} without output) | drops: render targets {} (no attachments {}, vs "
        "memexport {}, no rasterization {}), pipeline {} (ps memexport {}, bindings {}, "
        "tessellation {}, cull both {}, no pipeline {}), constants {}, sampler {} | pipelines {} "
        "requested {} created {} failed, WGSL failures {} | textures {} loads {} failures {} | "
        "constants {} KiB | "
        "submissions {} replayed {} completed {} | shared memory uploads {} ({} MiB) | last "
        "replay {} commands, {} draws ({} skipped, pipeline not ready), {} bind groups, encode "
        "{:.3f} ms, submit {:.3f} ms, {} redundant state calls skipped | uncaptured errors {} | "
        "verify: {} pages checked, {} "
        "stale | vs memexport compute: {} dispatches, {} without a compute variant or off | "
        "tessellation: {} draws ({} patches, slot max {} points / {} indices), {} dropped by "
        "the tessellator | skipped (pipeline not ready) {} in all replays, pipeline waits {} "
        "({} timed out, {:.0f} ms)",
        swap_index, frontbuffer_ptr, frontbuffer_width, frontbuffer_height, refreshed,
        texture_width, texture_height, uint32_t(texture_format), swap_view ? "found" : "none",
        statistics_.draws, statistics_.draws_recorded, statistics_.draws_dropped,
        statistics_.draws_culled, pipelines.draws_unsupported, statistics_.copies,
        statistics_.draws_no_output, statistics_.draws_no_output_memexport_vs,
        statistics_.draws_dummy_attachment, statistics_.draws_dummy_attachment_no_output,
        statistics_.drops_render_targets,
        render_target_cache_->statistics().updates_without_attachments -
            statistics_.draws_no_output - statistics_.draws_dummy_attachment,
        statistics_.drops_rt_memexport_vs, statistics_.drops_rt_no_rasterization,
        statistics_.drops_pipeline, pipelines.draws_refused_ps_memexport,
        pipelines.draws_refused_bindings, pipelines.draws_refused_tessellation,
        pipelines.draws_refused_cull_both, pipelines.draws_refused_no_pipeline,
        statistics_.drops_constants, statistics_.drops_sampler,
        pipelines.pipelines_requested, pipeline_cache_->pipelines_created(),
        pipeline_cache_->pipelines_failed(), pipelines.wgsl_failures, textures.textures_created,
        textures.loads,
        textures.load_failures, statistics_.constant_bytes >> 10, submission_tracker_.submitted(),
        submission_tracker_.replayed(), submission_tracker_.completed(),
        shared_memory_->upload_count(), shared_memory_->uploaded_bytes() >> 20, replay.commands,
        replay.draws, replay.draws_skipped, replay.bind_groups_created, replay.encode_ms,
        replay.submit_ms, replay.redundant_state_skipped, device_.uncaptured_error_count(),
        shared_memory_->verified_pages(),
        shared_memory_->stale_pages(), statistics_.memexport_dispatches,
        statistics_.memexport_unsupported, statistics_.draws_tessellated,
        tessellator_ ? tessellator_->statistics().patches : 0,
        tessellator_ ? tessellator_->statistics().max_slot_points : 0,
        tessellator_ ? tessellator_->statistics().max_slot_indices : 0,
        statistics_.drops_tessellation, command_queue_->draws_skipped_total(),
        statistics_.pipeline_waits, statistics_.pipeline_wait_timeouts,
        statistics_.pipeline_wait_ms);
#ifdef __EMSCRIPTEN__
    // The malloc heap's break never moves down, so it is the peak of host heap plus guest window.
    // mallinfo: bytes in use by malloc now (guest window, thread stacks and host heap) and free
    // bytes inside the break (fragmentation / freed peaks).
    uint64_t heap_break = uint64_t(reinterpret_cast<uintptr_t>(sbrk(0)));
    struct mallinfo heap_info = mallinfo();
    REXGPU_INFO(
        "WebGPU: swap #{} memory: heap break {} MiB of {} MiB wasm memory (max {} MiB), malloc "
        "in use {} MiB, free below the break {} MiB",
        swap_index, heap_break >> 20, uint64_t(emscripten_get_heap_size()) >> 20,
        uint64_t(emscripten_get_heap_max()) >> 20, uint64_t(size_t(heap_info.uordblks)) >> 20,
        uint64_t(size_t(heap_info.fordblks)) >> 20);
#endif
    // Pipeline polygon state (cumulative counts of draws given a pipeline).
    static uint64_t polygon_log_swap = 0, polygon_log_depth_bias = 0;
    uint64_t frames = swap_index > polygon_log_swap ? swap_index - polygon_log_swap : 1;
    REXGPU_INFO(
        "WebGPU: swap #{} polygon state: {} draws with depth bias ({:.1f} per frame since the "
        "previous line), {} with depth clipping off, {} with the back face stencil masks",
        swap_index, pipelines.draws_depth_bias,
        double(pipelines.draws_depth_bias - polygon_log_depth_bias) / double(frames),
        pipelines.draws_unclipped_depth, pipelines.draws_stencil_back_masks);
    polygon_log_swap = swap_index;
    polygon_log_depth_bias = pipelines.draws_depth_bias;
  }
#ifdef __EMSCRIPTEN__
  // Running out of wasm memory aborts the tab (rex_web_pre.js reports it as out of memory): warn
  // once when the heap break comes within 512 MiB of the maximum memory size (the growth limit,
  // or the fixed size).
  static bool heap_headroom_warned = false;
  if (!heap_headroom_warned) {
    uint64_t heap_break = uint64_t(reinterpret_cast<uintptr_t>(sbrk(0)));
    uint64_t heap_max = uint64_t(emscripten_get_heap_max());
    if (heap_break + (uint64_t(512) << 20) > heap_max) {
      heap_headroom_warned = true;
      REXGPU_WARN(
          "WebGPU: swap #{} memory: heap break {} MiB is within 512 MiB of the {} MiB maximum "
          "wasm memory; an allocation past it aborts the tab (raise rexglue_configure_target "
          "WEB_MAXIMUM_MEMORY / REXGLUE_WEB_MAXIMUM_MEMORY)",
          swap_index, heap_break >> 20, heap_max >> 20);
    }
  }
#endif
}

void WebGPUCommandProcessor::WaitForPipeline(PipelineId pipeline) {
  int32_t budget = REXCVAR_GET(webgpu_pipeline_wait_ms);
  if (!budget || pipeline == kInvalidPipelineId) {
    return;
  }
  int32_t timeout = -1;
  if (budget > 0) {
    double left = double(budget) - pipeline_wait_frame_ms_;
    if (left < 1.0) {
      return;
    }
    timeout = int32_t(left);
  }
  double waited = pipeline_cache_->WaitForPipeline(pipeline, timeout);
  if (waited >= 0.1) {
    ++statistics_.pipeline_waits;
    statistics_.pipeline_wait_ms += waited;
    pipeline_wait_frame_ms_ += waited;
    if (timeout >= 0 && waited >= double(timeout)) {
      ++statistics_.pipeline_wait_timeouts;
    }
  }
}

}  // namespace rex::graphics::webgpu
