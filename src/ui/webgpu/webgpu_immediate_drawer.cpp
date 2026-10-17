/**
 * @file        ui/webgpu/webgpu_immediate_drawer.cpp
 * @brief       WebGPU immediate drawer: UI-thread recording, device-thread encoding (WGSL).
 *
 * See rex/ui/webgpu/webgpu_immediate_drawer.h for the threading model.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/webgpu/webgpu_immediate_drawer.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>

#include <rex/assert.h>
#include <rex/graphics/webgpu/webgpu_log.h>
#include <rex/ui/webgpu/webgpu_presenter.h>

namespace rex {
namespace ui {
namespace webgpu {

using graphics::webgpu::MakeStringView;
using graphics::webgpu::WebGPUDevice;

namespace {

// The vertices are transformed to NDC on the UI thread, so there are no
// uniforms. ImmediateVertex: float2 position, float2 uv, RGBA8 color (ImGui's
// IM_COL32 byte order R, G, B, A).
constexpr char kImmediateWgsl[] = R"(
@group(0) @binding(0) var ui_texture : texture_2d<f32>;
@group(0) @binding(1) var ui_sampler : sampler;

struct VertexOutput {
  @builtin(position) position : vec4<f32>,
  @location(0) uv : vec2<f32>,
  @location(1) color : vec4<f32>,
};

@vertex
fn vs_main(@location(0) position : vec2<f32>, @location(1) uv : vec2<f32>,
           @location(2) color : vec4<f32>) -> VertexOutput {
  var output : VertexOutput;
  output.position = vec4<f32>(position, 0.0, 1.0);
  output.uv = uv;
  output.color = color;
  return output;
}

@fragment
fn fs_main(input : VertexOutput) -> @location(0) vec4<f32> {
  return input.color * textureSample(ui_texture, ui_sampler, input.uv);
}
)";

constexpr uint64_t kMinBufferSize = 64 * 1024;

template <typename T>
std::shared_ptr<T> MakeDeviceThreadOwned(WebGPUDevice& device, T* object) {
  WebGPUDevice* device_ptr = &device;
  return std::shared_ptr<T>(object, [device_ptr](T* p) {
    if (device_ptr->IsDeviceThread()) {
      delete p;
    } else if (!device_ptr->RunAsync([p]() { delete p; })) {
      // The device thread is gone; its WGPU handles cannot be released anywhere.
      REXWGPU_WARN("WebGPUImmediateDrawer: device thread gone, leaking UI resources");
    }
  });
}

}  // namespace

struct WebGPUImmediateTextureResources {
  WGPUTexture texture = nullptr;
  WGPUTextureView view = nullptr;
  WGPUBindGroup bind_group = nullptr;

  ~WebGPUImmediateTextureResources() {
    if (bind_group) {
      wgpuBindGroupRelease(bind_group);
    }
    if (view) {
      wgpuTextureViewRelease(view);
    }
    if (texture) {
      wgpuTextureRelease(texture);
    }
  }
};

struct WebGPUImmediateDeviceResources {
  struct Pipeline {
    WGPUTextureFormat format;
    bool lines;
    WGPURenderPipeline pipeline;
  };

  explicit WebGPUImmediateDeviceResources(WebGPUDevice& device) : device(device) {}

  ~WebGPUImmediateDeviceResources() {
    white.reset();
    for (Pipeline& pipeline : pipelines) {
      wgpuRenderPipelineRelease(pipeline.pipeline);
    }
    for (WGPUSampler sampler : samplers) {
      if (sampler) {
        wgpuSamplerRelease(sampler);
      }
    }
    if (vertex_buffer) {
      wgpuBufferRelease(vertex_buffer);
    }
    if (index_buffer) {
      wgpuBufferRelease(index_buffer);
    }
    ReleaseRetiredBuffers();
    if (pipeline_layout) {
      wgpuPipelineLayoutRelease(pipeline_layout);
    }
    if (bind_group_layout) {
      wgpuBindGroupLayoutRelease(bind_group_layout);
    }
    if (shader_module) {
      wgpuShaderModuleRelease(shader_module);
    }
  }

  // Device thread.
  bool Initialize() {
    WGPUDevice wgpu_device = device.device();
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = MakeStringView(kImmediateWgsl);
    WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    module_desc.nextInChain = &wgsl.chain;
    module_desc.label = MakeStringView("rex immediate drawer");
    shader_module = wgpuDeviceCreateShaderModule(wgpu_device, &module_desc);

    WGPUBindGroupLayoutEntry entries[2] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
                                           WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Fragment;
    entries[0].texture.sampleType = WGPUTextureSampleType_Float;
    entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Fragment;
    entries[1].sampler.type = WGPUSamplerBindingType_Filtering;
    WGPUBindGroupLayoutDescriptor bgl_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bgl_desc.label = MakeStringView("rex immediate drawer");
    bgl_desc.entryCount = 2;
    bgl_desc.entries = entries;
    bind_group_layout = wgpuDeviceCreateBindGroupLayout(wgpu_device, &bgl_desc);

    WGPUPipelineLayoutDescriptor layout_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    layout_desc.label = MakeStringView("rex immediate drawer");
    layout_desc.bindGroupLayoutCount = 1;
    layout_desc.bindGroupLayouts = &bind_group_layout;
    pipeline_layout = wgpuDeviceCreatePipelineLayout(wgpu_device, &layout_desc);

    // [linear][repeat]
    for (uint32_t i = 0; i < 4; ++i) {
      bool linear = (i & 2) != 0;
      bool repeat = (i & 1) != 0;
      WGPUSamplerDescriptor sampler_desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
      sampler_desc.label = MakeStringView("rex immediate drawer");
      WGPUAddressMode address = repeat ? WGPUAddressMode_Repeat : WGPUAddressMode_ClampToEdge;
      sampler_desc.addressModeU = address;
      sampler_desc.addressModeV = address;
      sampler_desc.addressModeW = address;
      WGPUFilterMode filter = linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
      sampler_desc.magFilter = filter;
      sampler_desc.minFilter = filter;
      sampler_desc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
      samplers[i] = wgpuDeviceCreateSampler(wgpu_device, &sampler_desc);
    }

    if (!shader_module || !bind_group_layout || !pipeline_layout || !samplers[0] ||
        !samplers[1] || !samplers[2] || !samplers[3]) {
      return false;
    }

    // Untextured draws sample an opaque white texel.
    auto* white_resources = new WebGPUImmediateTextureResources();
    white = MakeDeviceThreadOwned(device, white_resources);
    const uint8_t white_texel[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    return CreateTexture(*white_resources, 1, 1, ImmediateTextureFilter::kNearest, false,
                         white_texel);
  }

  // Device thread.
  bool CreateTexture(WebGPUImmediateTextureResources& resources, uint32_t width, uint32_t height,
                     ImmediateTextureFilter filter, bool is_repeated, const uint8_t* data) {
    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.label = MakeStringView("rex immediate texture");
    desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = {width, height, 1};
    desc.format = WGPUTextureFormat_RGBA8Unorm;
    desc.mipLevelCount = 1;
    desc.sampleCount = 1;
    resources.texture = wgpuDeviceCreateTexture(device.device(), &desc);
    if (!resources.texture) {
      return false;
    }
    if (data) {
      WGPUTexelCopyTextureInfo dest = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
      dest.texture = resources.texture;
      WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
      layout.bytesPerRow = width * 4;
      layout.rowsPerImage = height;
      WGPUExtent3D size = {width, height, 1};
      wgpuQueueWriteTexture(device.queue(), &dest, data, size_t(width) * height * 4, &layout,
                            &size);
    }
    resources.view = wgpuTextureCreateView(resources.texture, nullptr);
    if (!resources.view) {
      return false;
    }
    WGPUBindGroupEntry entries[2] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].textureView = resources.view;
    entries[1].binding = 1;
    entries[1].sampler =
        samplers[(filter == ImmediateTextureFilter::kLinear ? 2 : 0) | (is_repeated ? 1 : 0)];
    WGPUBindGroupDescriptor bind_group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bind_group_desc.label = MakeStringView("rex immediate texture");
    bind_group_desc.layout = bind_group_layout;
    bind_group_desc.entryCount = 2;
    bind_group_desc.entries = entries;
    resources.bind_group = wgpuDeviceCreateBindGroup(device.device(), &bind_group_desc);
    return resources.bind_group != nullptr;
  }

  // Device thread.
  WGPURenderPipeline GetPipeline(WGPUTextureFormat format, bool lines) {
    for (const Pipeline& pipeline : pipelines) {
      if (pipeline.format == format && pipeline.lines == lines) {
        return pipeline.pipeline;
      }
    }
    WGPUVertexAttribute attributes[3] = {WGPU_VERTEX_ATTRIBUTE_INIT, WGPU_VERTEX_ATTRIBUTE_INIT,
                                         WGPU_VERTEX_ATTRIBUTE_INIT};
    attributes[0].format = WGPUVertexFormat_Float32x2;
    attributes[0].offset = offsetof(ImmediateVertex, x);
    attributes[0].shaderLocation = 0;
    attributes[1].format = WGPUVertexFormat_Float32x2;
    attributes[1].offset = offsetof(ImmediateVertex, u);
    attributes[1].shaderLocation = 1;
    attributes[2].format = WGPUVertexFormat_Unorm8x4;
    attributes[2].offset = offsetof(ImmediateVertex, color);
    attributes[2].shaderLocation = 2;
    WGPUVertexBufferLayout vertex_layout = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    vertex_layout.stepMode = WGPUVertexStepMode_Vertex;
    vertex_layout.arrayStride = sizeof(ImmediateVertex);
    vertex_layout.attributeCount = 3;
    vertex_layout.attributes = attributes;

    // As the Vulkan / D3D12 immediate drawers: straight alpha over the target.
    WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
    blend.color.operation = WGPUBlendOperation_Add;
    blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
    blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blend.alpha.operation = WGPUBlendOperation_Add;
    blend.alpha.srcFactor = WGPUBlendFactor_One;
    blend.alpha.dstFactor = WGPUBlendFactor_One;
    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = format;
    target.blend = &blend;
    target.writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = shader_module;
    fragment.entryPoint = MakeStringView("fs_main");
    fragment.targetCount = 1;
    fragment.targets = &target;

    WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    desc.label = MakeStringView(lines ? "rex immediate lines" : "rex immediate triangles");
    desc.layout = pipeline_layout;
    desc.vertex.module = shader_module;
    desc.vertex.entryPoint = MakeStringView("vs_main");
    desc.vertex.bufferCount = 1;
    desc.vertex.buffers = &vertex_layout;
    desc.primitive.topology =
        lines ? WGPUPrimitiveTopology_LineList : WGPUPrimitiveTopology_TriangleList;
    desc.primitive.cullMode = WGPUCullMode_None;
    desc.multisample.count = 1;
    desc.fragment = &fragment;
    WGPURenderPipeline render_pipeline = wgpuDeviceCreateRenderPipeline(device.device(), &desc);
    if (!render_pipeline) {
      REXWGPU_ERROR("WebGPUImmediateDrawer: pipeline creation failed for format {}",
                    uint32_t(format));
      return nullptr;
    }
    pipelines.push_back({format, lines, render_pipeline});
    return render_pipeline;
  }

  // Device thread. Writes data to buffer at a 4-aligned offset (appending
  // within one command encoder, growing the buffer if needed). size must be a
  // multiple of 4.
  bool Upload(WGPUBuffer& buffer, uint64_t& buffer_size, uint64_t& used, WGPUBufferUsage usage,
              const char* label, const void* data, uint64_t size, uint64_t& offset_out) {
    if (used + size > buffer_size) {
      uint64_t new_size = std::max(kMinBufferSize, buffer_size);
      while (new_size < size * 2) {
        new_size *= 2;
      }
      WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
      desc.label = MakeStringView(label);
      desc.usage = usage | WGPUBufferUsage_CopyDst;
      desc.size = new_size;
      WGPUBuffer new_buffer = wgpuDeviceCreateBuffer(device.device(), &desc);
      if (!new_buffer) {
        return false;
      }
      // Draws already encoded into the current encoder may use the old buffer;
      // release it only once that encoder has been submitted.
      if (buffer) {
        retired_buffers.push_back(buffer);
      }
      buffer = new_buffer;
      buffer_size = new_size;
      used = 0;
    }
    wgpuQueueWriteBuffer(device.queue(), buffer, used, data, size_t(size));
    offset_out = used;
    used += size;
    return true;
  }

  // Device thread: at a new command encoder (the previous one is submitted).
  void BeginEncoder(WGPUCommandEncoder encoder) {
    if (encoder == last_encoder) {
      return;
    }
    last_encoder = encoder;
    vertex_buffer_used = 0;
    index_buffer_used = 0;
    ReleaseRetiredBuffers();
  }

  void ReleaseRetiredBuffers() {
    for (WGPUBuffer buffer : retired_buffers) {
      wgpuBufferRelease(buffer);
    }
    retired_buffers.clear();
  }

  WebGPUDevice& device;
  WGPUShaderModule shader_module = nullptr;
  WGPUBindGroupLayout bind_group_layout = nullptr;
  WGPUPipelineLayout pipeline_layout = nullptr;
  WGPUSampler samplers[4] = {};
  std::shared_ptr<WebGPUImmediateTextureResources> white;
  std::vector<Pipeline> pipelines;

  // Queue writes go before the submission that uses them, so recordings
  // encoded into the same command encoder must use different ranges; a new
  // encoder starts from 0 again.
  WGPUCommandEncoder last_encoder = nullptr;
  WGPUBuffer vertex_buffer = nullptr;
  uint64_t vertex_buffer_size = 0;
  uint64_t vertex_buffer_used = 0;
  WGPUBuffer index_buffer = nullptr;
  uint64_t index_buffer_size = 0;
  uint64_t index_buffer_used = 0;
  std::vector<WGPUBuffer> retired_buffers;
};

class WebGPUImmediateDrawer::WebGPUImmediateTexture final : public ImmediateTexture {
 public:
  WebGPUImmediateTexture(uint32_t width, uint32_t height,
                         std::shared_ptr<WebGPUImmediateTextureResources> resources)
      : ImmediateTexture(width, height), resources_(std::move(resources)) {}
  const std::shared_ptr<WebGPUImmediateTextureResources>& resources() const { return resources_; }

 private:
  std::shared_ptr<WebGPUImmediateTextureResources> resources_;
};

void WebGPUImmediateRecording::Encode(WGPUCommandEncoder encoder, WGPUTextureView target,
                                      WGPUTextureFormat format, uint32_t target_width,
                                      uint32_t target_height) const {
  if (draws_.empty() || !resources_ || !target || !target_width || !target_height ||
      vertices_.empty()) {
    return;
  }
  WebGPUImmediateDeviceResources& resources = *resources_;
  resources.BeginEncoder(encoder);
  uint64_t vertex_offset = 0, index_offset = 0;
  uint64_t vertex_bytes = uint64_t(vertices_.size()) * sizeof(ImmediateVertex);
  uint64_t index_bytes = uint64_t(indices_.size()) * sizeof(uint16_t);  // Padded to 4 at End.
  if (!resources.Upload(resources.vertex_buffer, resources.vertex_buffer_size,
                        resources.vertex_buffer_used, WGPUBufferUsage_Vertex,
                        "rex immediate vertices", vertices_.data(), vertex_bytes, vertex_offset)) {
    return;
  }
  WGPUBuffer vertex_buffer = resources.vertex_buffer;
  WGPUBuffer index_buffer = nullptr;
  if (index_bytes) {
    if (!resources.Upload(resources.index_buffer, resources.index_buffer_size,
                          resources.index_buffer_used, WGPUBufferUsage_Index,
                          "rex immediate indices", indices_.data(), index_bytes, index_offset)) {
      return;
    }
    index_buffer = resources.index_buffer;
  }

  WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
  color.view = target;
  color.loadOp = WGPULoadOp_Load;
  color.storeOp = WGPUStoreOp_Store;
  WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
  pass_desc.label = MakeStringView("rex immediate drawer");
  pass_desc.colorAttachmentCount = 1;
  pass_desc.colorAttachments = &color;
  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
  wgpuRenderPassEncoderSetVertexBuffer(pass, 0, vertex_buffer, vertex_offset, vertex_bytes);
  if (index_buffer) {
    wgpuRenderPassEncoderSetIndexBuffer(pass, index_buffer, WGPUIndexFormat_Uint16, index_offset,
                                        index_bytes);
  }
  bool rescale = target_width != render_target_width_ || target_height != render_target_height_;
  double scale_x = double(target_width) / double(std::max(render_target_width_, uint32_t(1)));
  double scale_y = double(target_height) / double(std::max(render_target_height_, uint32_t(1)));
  WGPURenderPipeline current_pipeline = nullptr;
  WGPUBindGroup current_bind_group = nullptr;
  for (const DrawCommand& draw : draws_) {
    uint32_t x0 = draw.scissor_x, y0 = draw.scissor_y;
    uint32_t x1 = draw.scissor_x + draw.scissor_width, y1 = draw.scissor_y + draw.scissor_height;
    if (rescale) {
      x0 = uint32_t(double(x0) * scale_x + 0.5);
      y0 = uint32_t(double(y0) * scale_y + 0.5);
      x1 = uint32_t(double(x1) * scale_x + 0.5);
      y1 = uint32_t(double(y1) * scale_y + 0.5);
    }
    x1 = std::min(x1, target_width);
    y1 = std::min(y1, target_height);
    if (x1 <= x0 || y1 <= y0) {
      continue;
    }
    WGPURenderPipeline pipeline = resources.GetPipeline(format, draw.lines);
    const WebGPUImmediateTextureResources* texture =
        draw.texture ? draw.texture.get() : resources.white.get();
    if (!pipeline || !texture || !texture->bind_group) {
      continue;
    }
    if (pipeline != current_pipeline) {
      wgpuRenderPassEncoderSetPipeline(pass, pipeline);
      current_pipeline = pipeline;
    }
    if (texture->bind_group != current_bind_group) {
      wgpuRenderPassEncoderSetBindGroup(pass, 0, texture->bind_group, 0, nullptr);
      current_bind_group = texture->bind_group;
    }
    wgpuRenderPassEncoderSetScissorRect(pass, x0, y0, x1 - x0, y1 - y0);
    if (draw.indexed) {
      wgpuRenderPassEncoderDrawIndexed(pass, draw.count, 1, draw.first, draw.base_vertex, 0);
    } else {
      wgpuRenderPassEncoderDraw(pass, draw.count, 1, draw.first, 0);
    }
  }
  wgpuRenderPassEncoderEnd(pass);
  wgpuRenderPassEncoderRelease(pass);
}

std::unique_ptr<WebGPUImmediateDrawer> WebGPUImmediateDrawer::Create(WebGPUDevice& device) {
  if (device.is_lost()) {
    return nullptr;
  }
  std::unique_ptr<WebGPUImmediateDrawer> drawer(new WebGPUImmediateDrawer(device));
  if (!drawer->Initialize()) {
    return nullptr;
  }
  return drawer;
}

bool WebGPUImmediateDrawer::Initialize() {
  auto* resources = new WebGPUImmediateDeviceResources(device_);
  resources_ = MakeDeviceThreadOwned(device_, resources);
  bool success = false;
  if (!device_.RunSync([&]() { success = resources->Initialize(); }) || !success) {
    REXWGPU_ERROR("WebGPUImmediateDrawer: initialization failed");
    resources_.reset();
    return false;
  }
  return true;
}

WebGPUImmediateDrawer::~WebGPUImmediateDrawer() {
  // Recordings still held by the presenter keep the device resources alive.
  recording_.reset();
  resources_.reset();
}

std::unique_ptr<ImmediateTexture> WebGPUImmediateDrawer::CreateTexture(
    uint32_t width, uint32_t height, ImmediateTextureFilter filter, bool is_repeated,
    const uint8_t* data) {
  if (!width || !height || device_.is_lost()) {
    return nullptr;
  }
  uint32_t max_dimension = device_.limits().maxTextureDimension2D;
  if (width > max_dimension || height > max_dimension) {
    REXWGPU_ERROR("WebGPUImmediateDrawer: {}x{} texture is too large", width, height);
    return nullptr;
  }
  auto* texture_resources = new WebGPUImmediateTextureResources();
  std::shared_ptr<WebGPUImmediateTextureResources> shared =
      MakeDeviceThreadOwned(device_, texture_resources);
  bool success = false;
  WebGPUImmediateDeviceResources* resources = resources_.get();
  device_.RunSync([&]() {
    success =
        resources->CreateTexture(*texture_resources, width, height, filter, is_repeated, data);
  });
  if (!success) {
    REXWGPU_ERROR("WebGPUImmediateDrawer: failed to create a {}x{} texture", width, height);
    return nullptr;
  }
  return std::make_unique<WebGPUImmediateTexture>(width, height, std::move(shared));
}

void WebGPUImmediateDrawer::Begin(UIDrawContext& ui_draw_context, float coordinate_space_width,
                                  float coordinate_space_height) {
  ImmediateDrawer::Begin(ui_draw_context, coordinate_space_width, coordinate_space_height);
  recording_ = std::make_shared<WebGPUImmediateRecording>();
  recording_->resources_ = resources_;
  recording_->render_target_width_ = ui_draw_context.render_target_width();
  recording_->render_target_height_ = ui_draw_context.render_target_height();
  batch_open_ = false;
}

void WebGPUImmediateDrawer::BeginDrawBatch(const ImmediateDrawBatch& batch) {
  assert_false(batch_open_);
  batch_open_ = false;
  if (!recording_ || !batch.vertices || batch.vertex_count <= 0) {
    return;
  }
  std::vector<ImmediateVertex>& vertices = recording_->vertices_;
  std::vector<uint16_t>& indices = recording_->indices_;
  batch_vertex_base_ = uint32_t(vertices.size());
  batch_index_base_ = uint32_t(indices.size());
  batch_vertex_count_ = uint32_t(batch.vertex_count);
  batch_has_index_buffer_ = batch.indices && batch.index_count > 0;
  batch_index_count_ = batch_has_index_buffer_ ? uint32_t(batch.index_count) : 0;
  // Coordinate space -> NDC (WebGPU: y up).
  float scale_x = 2.0f / coordinate_space_width();
  float scale_y = -2.0f / coordinate_space_height();
  vertices.resize(vertices.size() + batch_vertex_count_);
  ImmediateVertex* dest = vertices.data() + batch_vertex_base_;
  for (uint32_t i = 0; i < batch_vertex_count_; ++i) {
    ImmediateVertex vertex = batch.vertices[i];
    vertex.x = vertex.x * scale_x - 1.0f;
    vertex.y = vertex.y * scale_y + 1.0f;
    dest[i] = vertex;
  }
  if (batch_has_index_buffer_) {
    indices.insert(indices.end(), batch.indices, batch.indices + batch_index_count_);
  }
  batch_open_ = true;
}

void WebGPUImmediateDrawer::Draw(const ImmediateDraw& draw) {
  if (!batch_open_ || draw.count <= 0) {
    return;
  }
  WebGPUImmediateRecording::DrawCommand command;
  command.lines = draw.primitive_type == ImmediatePrimitiveType::kLines;
  command.indexed = batch_has_index_buffer_;
  command.count = uint32_t(draw.count);
  if (command.indexed) {
    // Out-of-range indexed draws would fail validation and the whole pass.
    if (draw.index_offset < 0 || uint64_t(draw.index_offset) + command.count > batch_index_count_) {
      return;
    }
    command.first = batch_index_base_ + uint32_t(draw.index_offset);
    command.base_vertex = int32_t(batch_vertex_base_) + draw.base_vertex;
  } else {
    if (draw.base_vertex < 0 || uint64_t(draw.base_vertex) + command.count > batch_vertex_count_) {
      return;
    }
    command.first = batch_vertex_base_ + uint32_t(draw.base_vertex);
  }
  if (!ScissorToRenderTarget(draw, command.scissor_x, command.scissor_y, command.scissor_width,
                             command.scissor_height)) {
    return;
  }
  if (draw.texture) {
    command.texture = static_cast<WebGPUImmediateTexture*>(draw.texture)->resources();
  }
  recording_->draws_.push_back(std::move(command));
}

void WebGPUImmediateDrawer::EndDrawBatch() { batch_open_ = false; }

void WebGPUImmediateDrawer::End() {
  if (recording_) {
    // Queue writes must be multiples of 4 bytes.
    if (recording_->indices_.size() & 1) {
      recording_->indices_.push_back(0);
    }
    if (!recording_->empty()) {
      auto& presenter = static_cast<WebGPUPresenter&>(ui_draw_context()->presenter());
      presenter.AddUIRecordingFromUIThread(std::move(recording_));
    }
    recording_.reset();
  }
  batch_open_ = false;
  ImmediateDrawer::End();
}

}  // namespace webgpu
}  // namespace ui
}  // namespace rex
