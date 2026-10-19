/**
 * @file        graphics/webgpu/webgpu_present_pass.cpp
 * @brief       Canvas present pass: clear + bilinear guest output blit (WGSL).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/webgpu_present_pass.h>

#include <algorithm>
#include <cstring>

#include <rex/graphics/webgpu/webgpu_device.h>
#include <rex/graphics/webgpu/webgpu_log.h>

namespace rex::graphics::webgpu {

namespace {

// Fullscreen triangle restricted by the viewport to the (clipped) destination
// rectangle; uv_offset/uv_scale map the clipped part back to source UVs. Texture
// row 0 is the top of the image in both WebGPU textures and the guest output.
constexpr char kBlitWgsl[] = R"(
struct BlitParams {
  uv_offset : vec2<f32>,
  uv_scale : vec2<f32>,
};
@group(0) @binding(0) var<uniform> params : BlitParams;
@group(0) @binding(1) var source_texture : texture_2d<f32>;
@group(0) @binding(2) var source_sampler : sampler;

struct VertexOutput {
  @builtin(position) position : vec4<f32>,
  @location(0) uv : vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) vertex_index : u32) -> VertexOutput {
  let corner = vec2<f32>(f32((vertex_index << 1u) & 2u), f32(vertex_index & 2u));
  var output : VertexOutput;
  output.position = vec4<f32>(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);
  output.uv = params.uv_offset + corner * params.uv_scale;
  return output;
}

@fragment
fn fs_main(input : VertexOutput) -> @location(0) vec4<f32> {
  return vec4<f32>(textureSampleLevel(source_texture, source_sampler, input.uv, 0.0).rgb, 1.0);
}
)";

// The same blit through the guest display gamma ramp (Frame::gamma_ramp): the
// source is read without filtering (VulkanCommandProcessor's apply-gamma
// shaders, which use texelFetch at the pixel position).
constexpr char kGammaBlitWgsl[] = R"(
struct GammaParams {
  uv_offset : vec2<f32>,
  uv_scale : vec2<f32>,
  // 1 = 256-entry table, 2 = PWL.
  mode : u32,
  rb_swap : u32,
};
@group(0) @binding(0) var<uniform> params : GammaParams;
@group(0) @binding(1) var source_texture : texture_2d<f32>;
@group(0) @binding(2) var<storage, read> ramp : array<u32>;

struct VertexOutput {
  @builtin(position) position : vec4<f32>,
  @location(0) uv : vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) vertex_index : u32) -> VertexOutput {
  let corner = vec2<f32>(f32((vertex_index << 1u) & 2u), f32(vertex_index & 2u));
  var output : VertexOutput;
  output.position = vec4<f32>(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);
  output.uv = params.uv_offset + corner * params.uv_scale;
  return output;
}

fn apply_pwl(value_10b : u32, channel : u32) -> f32 {
  let entry = ramp[(value_10b >> 3u) * 3u + channel];
  let value = f32(entry & 0xFFFFu) + f32((value_10b & 7u) * (entry >> 16u)) * 0.125;
  return clamp(value * 1.52737048e-05, 0.0, 1.0);
}

@fragment
fn fs_main(input : VertexOutput) -> @location(0) vec4<f32> {
  let size = textureDimensions(source_texture, 0);
  let texel = min(vec2<u32>(max(input.uv, vec2<f32>(0.0)) * vec2<f32>(size)), size - 1u);
  var color = textureLoad(source_texture, texel, 0).rgb;
  if (params.rb_swap != 0u) {
    color = color.bgr;
  }
  if (params.mode == 2u) {
    let source = vec3<u32>(clamp(color, vec3<f32>(0.0), vec3<f32>(1.0)) * 1023.0 + 0.5);
    return vec4<f32>(apply_pwl(source.r, 0u), apply_pwl(source.g, 1u), apply_pwl(source.b, 2u),
                     1.0);
  }
  let source = vec3<u32>(clamp(color, vec3<f32>(0.0), vec3<f32>(1.0)) * 255.0 + 0.5);
  // DC_LUT_30_COLOR: blue in bits 0:9, green 10:19, red 20:29.
  return vec4<f32>(vec3<f32>(f32((ramp[source.r] >> 20u) & 0x3FFu),
                             f32((ramp[source.g] >> 10u) & 0x3FFu),
                             f32(ramp[source.b] & 0x3FFu)) / 1023.0, 1.0);
}
)";

constexpr uint32_t kGammaRampDwords = 128 * 3;

constexpr uint32_t kUniformSlotSize = 256;  // minUniformBufferOffsetAlignment upper bound.
constexpr uint32_t kUniformSlotCount = 16;  // Blits encodable per submission.

struct BlitParams {
  float uv_offset[2];
  float uv_scale[2];
  // Gamma blit only.
  uint32_t gamma_mode;
  uint32_t gamma_rb_swap;
  uint32_t padding[2];
};

}  // namespace

WebGPUPresentPass::~WebGPUPresentPass() {
  for (Pipeline& pipeline : pipelines_) {
    wgpuBindGroupLayoutRelease(pipeline.bind_group_layout);
    wgpuRenderPipelineRelease(pipeline.pipeline);
  }
  pipelines_.clear();
  if (uniform_buffer_) {
    wgpuBufferRelease(uniform_buffer_);
  }
  if (gamma_ramp_buffer_) {
    wgpuBufferRelease(gamma_ramp_buffer_);
  }
  if (gamma_shader_module_) {
    wgpuShaderModuleRelease(gamma_shader_module_);
  }
  if (sampler_) {
    wgpuSamplerRelease(sampler_);
  }
  if (shader_module_) {
    wgpuShaderModuleRelease(shader_module_);
  }
}

bool WebGPUPresentPass::Initialize() {
  WGPUDevice device = device_.device();
  if (!device) {
    return false;
  }

  WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
  wgsl.code = MakeStringView(kBlitWgsl);
  WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
  module_desc.nextInChain = &wgsl.chain;
  module_desc.label = MakeStringView("rex present blit");
  shader_module_ = wgpuDeviceCreateShaderModule(device, &module_desc);
  if (!shader_module_) {
    REXWGPU_ERROR("WebGPU present: shader module creation failed");
    return false;
  }
  WGPUShaderSourceWGSL gamma_wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
  gamma_wgsl.code = MakeStringView(kGammaBlitWgsl);
  WGPUShaderModuleDescriptor gamma_module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
  gamma_module_desc.nextInChain = &gamma_wgsl.chain;
  gamma_module_desc.label = MakeStringView("rex present gamma blit");
  gamma_shader_module_ = wgpuDeviceCreateShaderModule(device, &gamma_module_desc);
  WGPUBufferDescriptor ramp_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
  ramp_desc.label = MakeStringView("rex present gamma ramp");
  ramp_desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
  ramp_desc.size = kGammaRampDwords * sizeof(uint32_t);
  gamma_ramp_buffer_ = wgpuDeviceCreateBuffer(device, &ramp_desc);

  WGPUSamplerDescriptor sampler_desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
  sampler_desc.label = MakeStringView("rex present bilinear");
  sampler_desc.addressModeU = WGPUAddressMode_ClampToEdge;
  sampler_desc.addressModeV = WGPUAddressMode_ClampToEdge;
  sampler_desc.addressModeW = WGPUAddressMode_ClampToEdge;
  sampler_desc.magFilter = WGPUFilterMode_Linear;
  sampler_desc.minFilter = WGPUFilterMode_Linear;
  sampler_desc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
  sampler_ = wgpuDeviceCreateSampler(device, &sampler_desc);

  WGPUBufferDescriptor buffer_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
  buffer_desc.label = MakeStringView("rex present params");
  buffer_desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
  buffer_desc.size = uint64_t(kUniformSlotSize) * kUniformSlotCount;
  uniform_buffer_ = wgpuDeviceCreateBuffer(device, &buffer_desc);

  return sampler_ && uniform_buffer_;
}

const WebGPUPresentPass::Pipeline* WebGPUPresentPass::GetPipeline(WGPUTextureFormat format,
                                                                  bool gamma) {
  for (const Pipeline& pipeline : pipelines_) {
    if (pipeline.format == format && pipeline.gamma == gamma) {
      return &pipeline;
    }
  }
  WGPUShaderModule module = gamma ? gamma_shader_module_ : shader_module_;
  if (!module) {
    return nullptr;
  }
  WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
  target.format = format;
  target.writeMask = WGPUColorWriteMask_All;
  WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
  fragment.module = module;
  fragment.entryPoint = MakeStringView("fs_main");
  fragment.targetCount = 1;
  fragment.targets = &target;
  WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
  desc.label = MakeStringView("rex present blit");
  desc.layout = nullptr;  // "auto"
  desc.vertex.module = module;
  desc.vertex.entryPoint = MakeStringView("vs_main");
  desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
  desc.primitive.cullMode = WGPUCullMode_None;
  desc.multisample.count = 1;
  desc.fragment = &fragment;
  WGPURenderPipeline render_pipeline = wgpuDeviceCreateRenderPipeline(device_.device(), &desc);
  if (!render_pipeline) {
    REXWGPU_ERROR("WebGPU present: pipeline creation failed for format {}", uint32_t(format));
    return nullptr;
  }
  Pipeline pipeline;
  pipeline.format = format;
  pipeline.gamma = gamma;
  pipeline.pipeline = render_pipeline;
  pipeline.bind_group_layout = wgpuRenderPipelineGetBindGroupLayout(render_pipeline, 0);
  pipelines_.push_back(pipeline);
  return &pipelines_.back();
}

bool WebGPUPresentPass::EncodeClearAndBlit(WGPUCommandEncoder encoder, WGPUTextureView target,
                                           WGPUTextureFormat target_format,
                                           uint32_t target_width, uint32_t target_height,
                                           const Frame& frame) {
  // Clip the destination rectangle (WebGPU viewports must lie inside the target).
  int64_t x0 = std::max<int64_t>(frame.dest_x, 0);
  int64_t y0 = std::max<int64_t>(frame.dest_y, 0);
  int64_t x1 = std::min<int64_t>(int64_t(frame.dest_x) + frame.dest_width, target_width);
  int64_t y1 = std::min<int64_t>(int64_t(frame.dest_y) + frame.dest_height, target_height);
  bool draw_source = !frame.final_draw && frame.source && frame.dest_width && frame.dest_height &&
                     x1 > x0 && y1 > y0;
  if (frame.pre_pass) {
    frame.pre_pass(encoder);
  }

  const Pipeline* pipeline = nullptr;
  WGPUBindGroup bind_group = nullptr;
  bool gamma = frame.gamma_ramp != Frame::GammaRamp::kNone && frame.gamma_ramp_data &&
               gamma_ramp_buffer_;
  if (draw_source) {
    pipeline = GetPipeline(target_format, gamma);
    if (!pipeline) {
      draw_source = false;
    }
  }
  if (draw_source) {
    BlitParams params = {};
    params.gamma_mode = uint32_t(frame.gamma_ramp);
    params.gamma_rb_swap = frame.gamma_rb_swap ? 1 : 0;
    params.uv_offset[0] = float(double(x0 - frame.dest_x) / double(frame.dest_width));
    params.uv_offset[1] = float(double(y0 - frame.dest_y) / double(frame.dest_height));
    params.uv_scale[0] = float(double(x1 - x0) / double(frame.dest_width));
    params.uv_scale[1] = float(double(y1 - y0) / double(frame.dest_height));
    uint64_t slot_offset = uint64_t(uniform_slot_) * kUniformSlotSize;
    uniform_slot_ = (uniform_slot_ + 1) % kUniformSlotCount;
    wgpuQueueWriteBuffer(device_.queue(), uniform_buffer_, slot_offset, &params, sizeof(params));

    WGPUBindGroupEntry entries[3] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
                                     WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].buffer = uniform_buffer_;
    entries[0].offset = slot_offset;
    entries[0].size = sizeof(params);
    entries[1].binding = 1;
    entries[1].textureView = frame.source;
    entries[2].binding = 2;
    if (gamma) {
      uint32_t ramp_dwords = frame.gamma_ramp == Frame::GammaRamp::kPwl ? kGammaRampDwords : 256;
      wgpuQueueWriteBuffer(device_.queue(), gamma_ramp_buffer_, 0, frame.gamma_ramp_data,
                           ramp_dwords * sizeof(uint32_t));
      entries[2].buffer = gamma_ramp_buffer_;
      entries[2].offset = 0;
      entries[2].size = kGammaRampDwords * sizeof(uint32_t);
    } else {
      entries[2].sampler = sampler_;
    }
    WGPUBindGroupDescriptor bind_group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bind_group_desc.layout = pipeline->bind_group_layout;
    bind_group_desc.entryCount = 3;
    bind_group_desc.entries = entries;
    bind_group = wgpuDeviceCreateBindGroup(device_.device(), &bind_group_desc);
    if (!bind_group) {
      draw_source = false;
    }
  }

  WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
  color.view = target;
  color.loadOp = WGPULoadOp_Clear;
  color.storeOp = WGPUStoreOp_Store;
  color.clearValue = frame.clear_color;
  WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
  pass_desc.label = MakeStringView("rex present");
  pass_desc.colorAttachmentCount = 1;
  pass_desc.colorAttachments = &color;
  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
  if (frame.final_draw) {
    frame.final_draw(pass, target_format, target_width, target_height);
  } else if (draw_source) {
    wgpuRenderPassEncoderSetPipeline(pass, pipeline->pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, bind_group, 0, nullptr);
    wgpuRenderPassEncoderSetViewport(pass, float(x0), float(y0), float(x1 - x0), float(y1 - y0),
                                     0.0f, 1.0f);
    wgpuRenderPassEncoderSetScissorRect(pass, uint32_t(x0), uint32_t(y0), uint32_t(x1 - x0),
                                        uint32_t(y1 - y0));
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
  }
  wgpuRenderPassEncoderEnd(pass);
  wgpuRenderPassEncoderRelease(pass);
  if (bind_group) {
    wgpuBindGroupRelease(bind_group);
  }
  return true;
}

WebGPUPresentPass::Result WebGPUPresentPass::PresentToSurface(const Frame& frame,
                                                              uint32_t& width_out,
                                                              uint32_t& height_out) {
  width_out = 0;
  height_out = 0;
  if (device_.is_lost()) {
    return Result::kDeviceLost;
  }
  WGPUSurface surface = device_.surface();
  if (!surface) {
    return Result::kError;
  }
  uint32_t width, height;
  if (!device_.UpdateSurfaceConfiguration(width, height)) {
    return Result::kSkipped;
  }

  WGPUSurfaceTexture surface_texture = WGPU_SURFACE_TEXTURE_INIT;
  wgpuSurfaceGetCurrentTexture(surface, &surface_texture);
  switch (surface_texture.status) {
    case WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal:
    case WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal:
      break;
    case WGPUSurfaceGetCurrentTextureStatus_Timeout:
    case WGPUSurfaceGetCurrentTextureStatus_Outdated:
    case WGPUSurfaceGetCurrentTextureStatus_Lost:
      if (surface_texture.texture) {
        wgpuTextureRelease(surface_texture.texture);
      }
      device_.InvalidateSurfaceConfiguration();
      return Result::kSkipped;
    default:
      if (surface_texture.texture) {
        wgpuTextureRelease(surface_texture.texture);
      }
      REXWGPU_ERROR("WebGPU present: getCurrentTexture failed (status {})",
                    uint32_t(surface_texture.status));
      return Result::kError;
  }
  // The canvas texture is created at the canvas' current size.
  width = wgpuTextureGetWidth(surface_texture.texture);
  height = wgpuTextureGetHeight(surface_texture.texture);

  WGPUTextureView view = wgpuTextureCreateView(surface_texture.texture, nullptr);
  WGPUCommandEncoderDescriptor encoder_desc = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
  encoder_desc.label = MakeStringView("rex present");
  WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device_.device(), &encoder_desc);
  EncodeClearAndBlit(encoder, view, device_.surface_format(), width, height, frame);
  if (frame.overlay) {
    frame.overlay(encoder, view, device_.surface_format(), width, height);
  }
  if (frame.readback_buffer) {
    WGPUTexelCopyTextureInfo copy_source = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    copy_source.texture = surface_texture.texture;
    WGPUTexelCopyBufferInfo copy_dest = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    copy_dest.buffer = frame.readback_buffer;
    copy_dest.layout.bytesPerRow = ReadbackRowPitch(width);
    copy_dest.layout.rowsPerImage = height;
    WGPUExtent3D copy_size = {width, height, 1};
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &copy_source, &copy_dest, &copy_size);
  }
  WGPUCommandBufferDescriptor command_buffer_desc = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
  WGPUCommandBuffer command_buffer = wgpuCommandEncoderFinish(encoder, &command_buffer_desc);
  wgpuQueueSubmit(device_.queue(), 1, &command_buffer);
  wgpuCommandBufferRelease(command_buffer);
  wgpuCommandEncoderRelease(encoder);
  wgpuTextureViewRelease(view);
  wgpuTextureRelease(surface_texture.texture);
  width_out = width;
  height_out = height;
  return Result::kPresented;
}

}  // namespace rex::graphics::webgpu
