/**
 * @file        graphics/webgpu/webgpu_render_target_cache.cpp
 * @brief       STUB WebGPU render target cache (WP1): see
 *              rex/graphics/webgpu/webgpu_render_target_cache.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/webgpu_render_target_cache.h>

#include <rex/logging.h>

namespace rex::graphics::webgpu {

WebGPURenderTargetCache::WebGPURenderTargetCache(WebGPUCommandProcessor& command_processor,
                                                 const RegisterFile& register_file)
    : command_processor_(command_processor), register_file_(register_file) {}

WebGPURenderTargetCache::~WebGPURenderTargetCache() {
  Shutdown();
}

bool WebGPURenderTargetCache::Initialize(uint32_t /*shared_memory_binding_count*/) {
  REXGPU_WARN(
      "[STUB] WebGPU render target cache: no host render targets (WP4), draws are dropped and "
      "resolves write nothing");
  return true;
}

void WebGPURenderTargetCache::Shutdown() {}

void WebGPURenderTargetCache::ClearCache() {}

void WebGPURenderTargetCache::RestoreEdramSnapshot(const void* /*snapshot*/) {}

bool WebGPURenderTargetCache::Update(bool /*is_rasterization_done*/,
                                     reg::RB_DEPTHCONTROL /*normalized_depth_control*/,
                                     uint32_t /*normalized_color_mask*/,
                                     const Shader& /*vertex_shader*/) {
  last_update_render_pass_ = {};
  last_update_render_pass_key_ = 0;
  return false;
}

bool WebGPURenderTargetCache::Resolve(memory::Memory& /*memory*/,
                                      WebGPUSharedMemory& /*shared_memory*/,
                                      WebGPUTextureCache& /*texture_cache*/,
                                      uint32_t& written_address_out,
                                      uint32_t& written_length_out) {
  written_address_out = 0;
  written_length_out = 0;
  return true;
}

}  // namespace rex::graphics::webgpu
