/**
 * @file        graphics/webgpu/webgpu_texture_cache.cpp
 * @brief       STUB WebGPU texture cache (WP1): see rex/graphics/webgpu/webgpu_texture_cache.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/webgpu_texture_cache.h>

#include <rex/logging.h>

namespace rex::graphics::webgpu {

WebGPUTextureCache::WebGPUTextureCache(WebGPUCommandProcessor& command_processor,
                                       const RegisterFile& register_file,
                                       WebGPUSharedMemory& shared_memory)
    : command_processor_(command_processor),
      register_file_(register_file),
      shared_memory_(shared_memory) {}

WebGPUTextureCache::~WebGPUTextureCache() {
  Shutdown();
}

bool WebGPUTextureCache::Initialize() {
  REXGPU_WARN(
      "[STUB] WebGPU texture cache: no textures are loaded (WP5); swaps show the last resolve "
      "clear color");
  return true;
}

void WebGPUTextureCache::Shutdown() {}

void WebGPUTextureCache::ClearCache() {}

void WebGPUTextureCache::RequestTextures(uint32_t /*used_texture_mask*/) {}

WGPUTextureView WebGPUTextureCache::RequestSwapTexture(uint32_t& width_out, uint32_t& height_out,
                                                       xenos::TextureFormat& format_out) {
  xenos::xe_gpu_texture_fetch_t fetch = register_file_.GetTextureFetch(0);
  width_out = fetch.size_2d.width + 1;
  height_out = fetch.size_2d.height + 1;
  format_out = fetch.format;
  return nullptr;
}

}  // namespace rex::graphics::webgpu
