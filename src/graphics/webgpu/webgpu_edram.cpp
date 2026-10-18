/**
 * @file        graphics/webgpu/webgpu_edram.cpp
 * @brief       See rex/graphics/webgpu/webgpu_edram.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/webgpu_edram.h>

#include <algorithm>
#include <cstring>
#include <string>

#include <rex/graphics/flags.h>
#include <rex/graphics/webgpu/webgpu_device.h>
#include <rex/graphics/webgpu/webgpu_log.h>

#include "webgpu_edram_shaders.h"

namespace rex::graphics::webgpu {

namespace {

struct DumpParams {
  uint32_t first_tile;
  uint32_t dispatch_pitch_tiles;
  uint32_t source_base_tiles;
  uint32_t source_pitch_tiles;
  uint32_t format;
  uint32_t msaa_samples;
  uint32_t width_samples;
  uint32_t height_samples;
};
static_assert(sizeof(DumpParams) == 32);

struct LoadParams {
  uint32_t base_tiles;
  uint32_t pitch_tiles;
  uint32_t format;
  uint32_t msaa_samples;
  uint32_t from_constant;
  uint32_t constant_lo;
  uint32_t constant_hi;
  uint32_t pad;
};
static_assert(sizeof(LoadParams) == 32);

constexpr WGPUTextureFormat kColorHostFormats[] = {
    WGPUTextureFormat_RGBA8Unorm,  WGPUTextureFormat_RGB10A2Unorm, WGPUTextureFormat_RGBA16Float,
    WGPUTextureFormat_RG16Float,   WGPUTextureFormat_R32Float,     WGPUTextureFormat_RG32Float,
    // Linear 8_8_8_8_GAMMA (texture-formats-tier1 only).
    WGPUTextureFormat_RGBA16Unorm,
};

WGPUShaderModule CreateModule(WGPUDevice device, const char* label, const std::string& code) {
  WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
  wgsl.code = MakeStringView(code);
  WGPUShaderModuleDescriptor desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
  desc.nextInChain = &wgsl.chain;
  desc.label = MakeStringView(label);
  return wgpuDeviceCreateShaderModule(device, &desc);
}

WGPUBindGroupLayoutEntry BufferEntry(uint32_t binding, WGPUShaderStage visibility,
                                     WGPUBufferBindingType type, uint64_t min_size = 0) {
  WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
  entry.binding = binding;
  entry.visibility = visibility;
  entry.buffer.type = type;
  entry.buffer.hasDynamicOffset = type == WGPUBufferBindingType_Uniform;
  entry.buffer.minBindingSize = min_size;
  return entry;
}

WGPUBindGroupLayoutEntry TextureEntry(uint32_t binding, WGPUTextureSampleType sample_type,
                                      bool multisampled) {
  WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
  entry.binding = binding;
  entry.visibility = WGPUShaderStage_Compute;
  entry.texture.sampleType = sample_type;
  entry.texture.viewDimension = WGPUTextureViewDimension_2D;
  entry.texture.multisampled = multisampled;
  return entry;
}

WGPUBindGroupLayout CreateLayout(WGPUDevice device, const WGPUBindGroupLayoutEntry* entries,
                                 uint32_t count) {
  WGPUBindGroupLayoutDescriptor desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
  desc.entryCount = count;
  desc.entries = entries;
  return wgpuDeviceCreateBindGroupLayout(device, &desc);
}

WGPUPipelineLayout CreatePipelineLayout(WGPUDevice device, WGPUBindGroupLayout layout) {
  WGPUPipelineLayoutDescriptor desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
  desc.bindGroupLayoutCount = 1;
  desc.bindGroupLayouts = &layout;
  return wgpuDeviceCreatePipelineLayout(device, &desc);
}

WGPUBindGroupEntry BufferBinding(uint32_t binding, WGPUBuffer buffer, uint64_t offset,
                                 uint64_t size) {
  WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
  entry.binding = binding;
  entry.buffer = buffer;
  entry.offset = offset;
  entry.size = size;
  return entry;
}

WGPUBindGroupEntry TextureBinding(uint32_t binding, WGPUTextureView view) {
  WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
  entry.binding = binding;
  entry.textureView = view;
  return entry;
}

}  // namespace

WGPUTextureFormat WebGPUEdram::GetColorHostFormat(uint32_t color_format) {
  switch (color_format) {
    case 0:  // k_8_8_8_8
      return WGPUTextureFormat_RGBA8Unorm;
    case 1:  // k_8_8_8_8_GAMMA: linear, or PWL gamma in the shaders
      return gamma_as_unorm16_ ? WGPUTextureFormat_RGBA16Unorm : WGPUTextureFormat_RGBA8Unorm;
    case 2:   // k_2_10_10_10
    case 10:  // k_2_10_10_10_AS_10_10_10_10
      return WGPUTextureFormat_RGB10A2Unorm;
    case 3:   // k_2_10_10_10_FLOAT (7e3 values are exact in float16)
    case 12:  // k_2_10_10_10_FLOAT_AS_16_16_16_16
    case 5:   // k_16_16_16_16 (-32...32 as float)
    case 7:   // k_16_16_16_16_FLOAT
      return WGPUTextureFormat_RGBA16Float;
    case 4:  // k_16_16 (-32...32 as float)
    case 6:  // k_16_16_FLOAT
      return WGPUTextureFormat_RG16Float;
    case 14:  // k_32_FLOAT
      return WGPUTextureFormat_R32Float;
    case 15:  // k_32_32_FLOAT
      return WGPUTextureFormat_RG32Float;
    default:
      return WGPUTextureFormat_Undefined;
  }
}

WGPUTextureFormat WebGPUEdram::GetDepthHostFormat(uint32_t depth_format) const {
  if (depth_format == 1 && has_depth32float_stencil8_) {
    // D24FS8: float24 is exact in float32.
    return WGPUTextureFormat_Depth32FloatStencil8;
  }
  return WGPUTextureFormat_Depth24PlusStencil8;
}

uint32_t WebGPUEdram::GetResolveDestBppLog2(uint32_t color_format) {
  switch (color_format) {
    case 2:   // k_8
    case 8:   // k_8_A
    case 9:   // k_8_B
      return 0;
    case 3:   // k_1_5_5_5
    case 4:   // k_5_6_5
    case 5:   // k_6_5_5
    case 10:  // k_8_8
    case 15:  // k_4_4_4_4
    case 24:  // k_16
    case 30:  // k_16_FLOAT
      return 1;
    case 6:   // k_8_8_8_8
    case 7:   // k_2_10_10_10
    case 14:  // k_8_8_8_8_A
    case 16:  // k_10_11_11
    case 17:  // k_11_11_10
    case 22:  // k_24_8 (depth)
    case 23:  // k_24_8_FLOAT (depth)
    case 25:  // k_16_16
    case 31:  // k_16_16_FLOAT
    case 36:  // k_32_FLOAT
    case 50:  // k_8_8_8_8_AS_16_16_16_16
    case 54:  // k_2_10_10_10_AS_16_16_16_16
    case 55:  // k_10_11_11_AS_16_16_16_16
    case 56:  // k_11_11_10_AS_16_16_16_16
      return 2;
    case 26:  // k_16_16_16_16
    case 32:  // k_16_16_16_16_FLOAT
    case 37:  // k_32_32_FLOAT
      return 3;
    case 38:  // k_32_32_32_32_FLOAT
      return 4;
    default:
      return UINT32_MAX;
  }
}

int WebGPUEdram::GetColorHostFormatIndex(WGPUTextureFormat format) {
  for (uint32_t i = 0; i < kColorHostFormatCount; ++i) {
    if (kColorHostFormats[i] == format) {
      return int(i);
    }
  }
  return -1;
}

WGPUComputePipeline WebGPUEdram::CreateComputePipeline(const char* label,
                                                       const char* const* sources,
                                                       uint32_t source_count,
                                                       WGPUBindGroupLayout layout) {
  WGPUDevice device = device_.device();
  std::string code;
  for (uint32_t i = 0; i < source_count; ++i) {
    code += sources[i];
  }
  WGPUShaderModule module = CreateModule(device, label, code);
  WGPUPipelineLayout pipeline_layout = CreatePipelineLayout(device, layout);
  WGPUComputePipelineDescriptor desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
  desc.label = MakeStringView(label);
  desc.layout = pipeline_layout;
  desc.compute.module = module;
  desc.compute.entryPoint = MakeStringView("main");
  WGPUComputePipeline pipeline = wgpuDeviceCreateComputePipeline(device, &desc);
  wgpuPipelineLayoutRelease(pipeline_layout);
  wgpuShaderModuleRelease(module);
  return pipeline;
}

bool WebGPUEdram::Initialize() {
  WGPUDevice device = device_.device();
  has_depth32float_stencil8_ = device_.HasFeature(WGPUFeatureName_Depth32FloatStencil8);
  // As D3D12/Vulkan gamma_render_target_as_unorm16: linear storage, so blending
  // into 8_8_8_8_GAMMA happens in linear space (the native D3D12 default).
  gamma_as_unorm16_ = REXCVAR_GET(gamma_render_target_as_unorm16) &&
                      device_.HasFeature(WGPUFeatureName_TextureFormatsTier1);
  // After the prelude: its diagnostic() directive must come first.
  const std::string prelude = std::string(edram_wgsl::kPrelude) +
                              "const kGammaRtLinear : bool = " +
                              (gamma_as_unorm16_ ? "true" : "false") + ";\n";

  WGPUBufferDescriptor buffer_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
  buffer_desc.label = MakeStringView("EDRAM");
  buffer_desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst;
  buffer_desc.size = kBufferSize;
  buffer_ = wgpuDeviceCreateBuffer(device, &buffer_desc);
  if (!buffer_) {
    REXWGPU_ERROR("WebGPU EDRAM: failed to create the {} MiB buffer", kBufferSize >> 20);
    return false;
  }

  // Dump.
  static const char* const kDumpLabels[4] = {"EDRAM dump color 1x", "EDRAM dump color MSAA",
                                             "EDRAM dump depth 1x", "EDRAM dump depth MSAA"};
  static const char* const kDumpSources[4] = {
      edram_wgsl::kDumpSourceColor1x, edram_wgsl::kDumpSourceColorMs,
      edram_wgsl::kDumpSourceDepth1x, edram_wgsl::kDumpSourceDepthMs};
  for (uint32_t i = 0; i < 4; ++i) {
    bool is_depth = i >= 2;
    bool multisampled = (i & 1) != 0;
    WGPUBindGroupLayoutEntry entries[4];
    entries[0] = BufferEntry(0, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage);
    entries[1] = BufferEntry(1, WGPUShaderStage_Compute, WGPUBufferBindingType_Uniform,
                             sizeof(DumpParams));
    entries[2] = TextureEntry(
        2, is_depth ? WGPUTextureSampleType_Depth : WGPUTextureSampleType_UnfilterableFloat,
        multisampled);
    entries[3] = TextureEntry(3, WGPUTextureSampleType_Uint, multisampled);
    dump_layouts_[i] = CreateLayout(device, entries, is_depth ? 4 : 3);
    const char* sources[] = {prelude.c_str(), kDumpSources[i], edram_wgsl::kDumpMain};
    dump_pipelines_[i] = CreateComputePipeline(kDumpLabels[i], sources, 3, dump_layouts_[i]);
  }

  // Load / clear.
  {
    WGPUBindGroupLayoutEntry entries[2];
    entries[0] =
        BufferEntry(0, WGPUShaderStage_Fragment, WGPUBufferBindingType_ReadOnlyStorage);
    entries[1] = BufferEntry(1, WGPUShaderStage_Fragment, WGPUBufferBindingType_Uniform,
                             sizeof(LoadParams));
    load_layout_ = CreateLayout(device, entries, 2);
    load_module_ = CreateModule(device, "EDRAM load",
                                prelude + edram_wgsl::kLoadMain);
    WGPUPipelineLayout pipeline_layout = CreatePipelineLayout(device, load_layout_);

    WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    desc.layout = pipeline_layout;
    desc.vertex.module = load_module_;
    desc.vertex.entryPoint = MakeStringView("vs");
    desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    desc.multisample.mask = 0xFFFFFFFFu;

    for (uint32_t f = 0; f < kColorHostFormatCount; ++f) {
      if (kColorHostFormats[f] == WGPUTextureFormat_RGBA16Unorm && !gamma_as_unorm16_) {
        continue;
      }
      WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
      target.format = kColorHostFormats[f];
      target.writeMask = WGPUColorWriteMask_All;
      WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
      fragment.module = load_module_;
      fragment.entryPoint = MakeStringView("fs_color");
      fragment.targetCount = 1;
      fragment.targets = &target;
      desc.fragment = &fragment;
      desc.depthStencil = nullptr;
      for (uint32_t ms = 0; ms < 2; ++ms) {
        desc.label = MakeStringView("EDRAM load color");
        desc.multisample.count = ms ? 4 : 1;
        // The spec guarantees 4x for every format here except rg32float (k_32_32_FLOAT);
        // Chrome supports it, Firefox reports a validation error. Catch it in a scope
        // instead of an uncaptured error and skip such loads (RecordLoad).
        const bool msaa_optional = ms && kColorHostFormats[f] == WGPUTextureFormat_RG32Float;
        if (msaa_optional) {
          wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
        }
        color_load_pipelines_[f][ms] = wgpuDeviceCreateRenderPipeline(device, &desc);
        if (msaa_optional) {
          WGPUPopErrorScopeCallbackInfo pop = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
          pop.mode = WGPUCallbackMode_AllowSpontaneous;
          pop.callback = [](WGPUPopErrorScopeStatus status, WGPUErrorType type,
                            WGPUStringView message, void* userdata1, void*) {
            if (status != WGPUPopErrorScopeStatus_Success || type == WGPUErrorType_NoError) {
              return;
            }
            static_cast<std::atomic<bool>*>(userdata1)->store(true, std::memory_order_release);
            REXWGPU_INFO(
                "WebGPU EDRAM: no 4x MSAA for rg32float (k_32_32_FLOAT) on this browser, its "
                "EDRAM loads are skipped: {}",
                std::string_view(message.data ? message.data : "",
                                 message.data ? (message.length == WGPU_STRLEN
                                                     ? std::strlen(message.data)
                                                     : message.length)
                                              : 0)
                    .substr(0, 200));
          };
          pop.userdata1 = &color_load_msaa_failed_[f];
          wgpuDevicePopErrorScope(device, pop);
        }
      }
    }

    for (uint32_t df = 0; df < 2; ++df) {
      if (df == 1 && !has_depth32float_stencil8_) {
        continue;
      }
      WGPUDepthStencilState depth_stencil = WGPU_DEPTH_STENCIL_STATE_INIT;
      depth_stencil.format =
          df ? WGPUTextureFormat_Depth32FloatStencil8 : WGPUTextureFormat_Depth24PlusStencil8;
      depth_stencil.depthCompare = WGPUCompareFunction_Always;
      WGPUStencilFaceState face = WGPU_STENCIL_FACE_STATE_INIT;
      face.compare = WGPUCompareFunction_Always;
      face.failOp = WGPUStencilOperation_Keep;
      face.depthFailOp = WGPUStencilOperation_Keep;
      face.passOp = WGPUStencilOperation_Replace;
      depth_stencil.stencilFront = face;
      depth_stencil.stencilBack = face;
      depth_stencil.stencilReadMask = 0xFF;
      for (uint32_t variant = 0; variant < kDepthPipelineVariants; ++variant) {
        WGPUConstantEntry constant = WGPU_CONSTANT_ENTRY_INIT;
        constant.key = MakeStringView("kStencilBit");
        constant.value = double(variant ? variant - 1 : 0);
        WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module = load_module_;
        if (variant == 0) {
          fragment.entryPoint = MakeStringView("fs_depth");
          depth_stencil.depthWriteEnabled = WGPUOptionalBool_True;
          depth_stencil.stencilWriteMask = 0xFF;
        } else {
          fragment.entryPoint = MakeStringView("fs_stencil_bit");
          fragment.constantCount = 1;
          fragment.constants = &constant;
          depth_stencil.depthWriteEnabled = WGPUOptionalBool_False;
          depth_stencil.stencilWriteMask = uint32_t(1) << (variant - 1);
        }
        desc.fragment = &fragment;
        desc.depthStencil = &depth_stencil;
        for (uint32_t ms = 0; ms < 2; ++ms) {
          desc.label = MakeStringView(variant ? "EDRAM load stencil bit" : "EDRAM load depth");
          desc.multisample.count = ms ? 4 : 1;
          depth_load_pipelines_[df][ms][variant] = wgpuDeviceCreateRenderPipeline(device, &desc);
        }
      }
    }
    wgpuPipelineLayoutRelease(pipeline_layout);
  }

  // Resolve.
  {
    WGPUBindGroupLayoutEntry entries[3];
    entries[0] = BufferEntry(0, WGPUShaderStage_Compute, WGPUBufferBindingType_ReadOnlyStorage);
    entries[1] = BufferEntry(1, WGPUShaderStage_Compute, WGPUBufferBindingType_Storage);
    entries[2] = BufferEntry(2, WGPUShaderStage_Compute, WGPUBufferBindingType_Uniform,
                             sizeof(WebGPUResolveConstants));
    resolve_layout_ = CreateLayout(device, entries, 3);
    const char* sources[] = {prelude.c_str(), edram_wgsl::kResolveMain};
    resolve_pipeline_ = CreateComputePipeline("EDRAM resolve", sources, 2, resolve_layout_);
  }

  bool ok = resolve_pipeline_ && load_module_;
  for (uint32_t i = 0; i < 4; ++i) {
    ok = ok && dump_pipelines_[i];
  }
  for (uint32_t f = 0; f < kColorHostFormatCount; ++f) {
    if (kColorHostFormats[f] == WGPUTextureFormat_RGBA16Unorm && !gamma_as_unorm16_) {
      continue;
    }
    ok = ok && color_load_pipelines_[f][0] && color_load_pipelines_[f][1];
  }
  for (uint32_t v = 0; v < kDepthPipelineVariants; ++v) {
    ok = ok && depth_load_pipelines_[0][0][v] && depth_load_pipelines_[0][1][v];
  }
  if (!ok) {
    REXWGPU_ERROR("WebGPU EDRAM: failed to create the EDRAM pipelines");
    return false;
  }
  REXWGPU_INFO("WebGPU EDRAM: {} MiB buffer, depth32float-stencil8 {}, 8_8_8_8_GAMMA {}",
               kBufferSize >> 20,
               has_depth32float_stencil8_ ? "available (D24FS8 exact)"
                                          : "unavailable (D24FS8 in depth24plus)",
               gamma_as_unorm16_ ? "linear rgba16unorm (blending in linear space)"
                                 : "rgba8unorm (PWL gamma in the shaders)");
  return true;
}

void WebGPUEdram::Shutdown() {
  if (resolve_pipeline_) {
    wgpuComputePipelineRelease(resolve_pipeline_);
    resolve_pipeline_ = nullptr;
  }
  if (resolve_layout_) {
    wgpuBindGroupLayoutRelease(resolve_layout_);
    resolve_layout_ = nullptr;
  }
  for (auto& by_format : depth_load_pipelines_) {
    for (auto& by_samples : by_format) {
      for (WGPURenderPipeline& pipeline : by_samples) {
        if (pipeline) {
          wgpuRenderPipelineRelease(pipeline);
          pipeline = nullptr;
        }
      }
    }
  }
  for (auto& by_format : color_load_pipelines_) {
    for (WGPURenderPipeline& pipeline : by_format) {
      if (pipeline) {
        wgpuRenderPipelineRelease(pipeline);
        pipeline = nullptr;
      }
    }
  }
  if (load_module_) {
    wgpuShaderModuleRelease(load_module_);
    load_module_ = nullptr;
  }
  if (load_layout_) {
    wgpuBindGroupLayoutRelease(load_layout_);
    load_layout_ = nullptr;
  }
  for (uint32_t i = 0; i < 4; ++i) {
    if (dump_pipelines_[i]) {
      wgpuComputePipelineRelease(dump_pipelines_[i]);
      dump_pipelines_[i] = nullptr;
    }
    if (dump_layouts_[i]) {
      wgpuBindGroupLayoutRelease(dump_layouts_[i]);
      dump_layouts_[i] = nullptr;
    }
  }
  if (buffer_) {
    wgpuBufferDestroy(buffer_);
    wgpuBufferRelease(buffer_);
    buffer_ = nullptr;
  }
}

void WebGPUEdram::RecordDump(WebGPUCommandStream& stream, const WebGPUEdramSurface& source,
                             uint32_t first_tile, uint32_t dispatch_pitch_tiles,
                             uint32_t width_tiles, uint32_t height_tiles) {
  if (!width_tiles || !height_tiles || !source.sample_view || !source.pitch_tiles) {
    return;
  }
  uint32_t variant = (source.is_depth ? 2 : 0) | (source.msaa_samples ? 1 : 0);
  bool is_64bpp = !source.is_depth && (source.format == 5 || source.format == 7 ||
                                       source.format == 15);
  uint32_t constants_offset;
  auto* params =
      static_cast<DumpParams*>(stream.AllocateConstants(sizeof(DumpParams), constants_offset));
  if (!params) {
    return;
  }
  params->first_tile = first_tile;
  params->dispatch_pitch_tiles = dispatch_pitch_tiles;
  params->source_base_tiles = source.base_tiles;
  params->source_pitch_tiles = source.pitch_tiles;
  params->format = source.format;
  params->msaa_samples = source.msaa_samples;
  params->width_samples = width_tiles * (is_64bpp ? 40 : 80);
  params->height_samples = height_tiles * 16;

  WGPUBindGroupEntry entries[4];
  entries[0] = BufferBinding(0, buffer_, 0, kBufferSize);
  entries[1] = BufferBinding(1, StreamConstantsBuffer(), 0, sizeof(DumpParams));
  entries[2] = TextureBinding(2, source.sample_view);
  entries[3] = TextureBinding(3, source.stencil_view);
  stream.CmdSetComputePipelineObject(dump_pipelines_[variant]);
  stream.CmdSetBindGroup(0, dump_layouts_[variant], entries, source.is_depth ? 4 : 3, 1,
                         &constants_offset);
  // 8x16 samples per group; tiles are 80 or 40 samples wide and 16 high.
  stream.CmdDispatch(params->width_samples / 8, height_tiles);
  ++statistics_.dumps;
}

void WebGPUEdram::DrawRectangles(WebGPUCommandStream& stream, const WebGPUEdramSurface& dest,
                                 bool from_constant, uint64_t constant,
                                 const WebGPUEdramRectangle* rectangles,
                                 uint32_t rectangle_count) {
  uint32_t constants_offset;
  auto* params =
      static_cast<LoadParams*>(stream.AllocateConstants(sizeof(LoadParams), constants_offset));
  if (!params) {
    return;
  }
  params->base_tiles = dest.base_tiles;
  params->pitch_tiles = dest.pitch_tiles;
  params->format = dest.format;
  params->msaa_samples = dest.msaa_samples;
  params->from_constant = from_constant ? 1 : 0;
  params->constant_lo = uint32_t(constant);
  params->constant_hi = uint32_t(constant >> 32);
  params->pad = 0;

  uint32_t ms = dest.msaa_samples ? 1 : 0;
  WGPURenderPipeline pipelines[kDepthPipelineVariants] = {};
  uint32_t pipeline_count;
  if (dest.is_depth) {
    uint32_t df = dest.host_format == WGPUTextureFormat_Depth32FloatStencil8 ? 1 : 0;
    // A clear sets the stencil with the reference in one draw; a load sets it
    // to 0 with the depth, then sets the 1 bits one draw each.
    pipeline_count = from_constant ? 1 : kDepthPipelineVariants;
    for (uint32_t i = 0; i < pipeline_count; ++i) {
      pipelines[i] = depth_load_pipelines_[df][ms][i];
    }
  } else {
    int index = GetColorHostFormatIndex(dest.host_format);
    if (index < 0) {
      return;
    }
    pipelines[0] = ms && color_load_msaa_failed_[index].load(std::memory_order_acquire)
                       ? nullptr
                       : color_load_pipelines_[index][ms];
    pipeline_count = 1;
  }

  WGPUBindGroupEntry entries[2];
  entries[0] = BufferBinding(0, buffer_, 0, kBufferSize);
  entries[1] = BufferBinding(1, StreamConstantsBuffer(), 0, sizeof(LoadParams));
  stream.CmdSetViewport(0.0f, 0.0f, float(dest.width), float(dest.height));
  for (uint32_t p = 0; p < pipeline_count; ++p) {
    if (!pipelines[p]) {
      continue;
    }
    stream.CmdSetRenderPipeline(pipelines[p]);
    stream.CmdSetBindGroup(0, load_layout_, entries, 2, 1, &constants_offset);
    if (dest.is_depth) {
      stream.CmdSetStencilReference(p ? 0xFF : (from_constant ? uint32_t(constant) & 0xFF : 0));
    }
    for (uint32_t i = 0; i < rectangle_count; ++i) {
      const WebGPUEdramRectangle& r = rectangles[i];
      uint32_t x = std::min(r.x, dest.width);
      uint32_t y = std::min(r.y, dest.height);
      uint32_t width = std::min(r.width, dest.width - x);
      uint32_t height = std::min(r.height, dest.height - y);
      if (!width || !height) {
        continue;
      }
      stream.CmdSetScissorRect(x, y, width, height);
      stream.CmdDraw(3);
    }
  }
}

void WebGPUEdram::RecordLoad(WebGPUCommandStream& stream, const WebGPUEdramSurface& dest,
                             const WebGPUEdramRectangle* rectangles, uint32_t rectangle_count,
                             const WebGPUEdramRectangle* clear_rectangle, uint64_t clear_value) {
  if ((!rectangle_count && !clear_rectangle) || !dest.attachment_view) {
    return;
  }
  WebGPUCommandStream::RenderPassDesc desc;
  if (dest.is_depth) {
    desc.has_depth_stencil = true;
    WGPURenderPassDepthStencilAttachment& depth = desc.depth_stencil;
    depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    depth.view = dest.attachment_view;
    depth.depthLoadOp = WGPULoadOp_Load;
    depth.depthStoreOp = WGPUStoreOp_Store;
    depth.depthClearValue = 1.0f;
    depth.stencilLoadOp = WGPULoadOp_Load;
    depth.stencilStoreOp = WGPUStoreOp_Store;
  } else {
    desc.SetColor(0, dest.attachment_view, false);
  }
  stream.CmdBeginRenderPass(desc);
  if (rectangle_count) {
    DrawRectangles(stream, dest, false, 0, rectangles, rectangle_count);
    statistics_.load_rectangles += rectangle_count;
  }
  if (clear_rectangle) {
    DrawRectangles(stream, dest, true, clear_value, clear_rectangle, 1);
    ++statistics_.clears;
  }
  stream.CmdEndRenderPass();
}

void WebGPUEdram::RecordResolve(WebGPUCommandStream& stream,
                                const WebGPUResolveConstants& constants, WGPUBuffer dest_buffer,
                                uint64_t dest_offset, uint64_t dest_size) {
  if (!constants.width || !constants.height || constants.dest_bpp_log2 > 4 || !dest_size) {
    return;
  }
  uint32_t constants_offset;
  void* params = stream.AllocateConstants(sizeof(WebGPUResolveConstants), constants_offset);
  if (!params) {
    return;
  }
  std::memcpy(params, &constants, sizeof(constants));
  WGPUBindGroupEntry entries[3];
  entries[0] = BufferBinding(0, buffer_, 0, kBufferSize);
  entries[1] = BufferBinding(1, dest_buffer, dest_offset, (dest_size + 3) & ~uint64_t(3));
  entries[2] = BufferBinding(2, StreamConstantsBuffer(), 0, sizeof(WebGPUResolveConstants));
  stream.CmdSetComputePipelineObject(resolve_pipeline_);
  stream.CmdSetBindGroup(0, resolve_layout_, entries, 3, 1, &constants_offset);
  uint32_t ppi_log2 = constants.dest_bpp_log2 < 2 ? 2 - constants.dest_bpp_log2 : 0;
  uint32_t invocations_x = constants.width >> ppi_log2;
  stream.CmdDispatch((invocations_x + 7) / 8, (constants.height + 7) / 8);
  ++statistics_.resolves;
}

}  // namespace rex::graphics::webgpu
