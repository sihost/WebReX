/**
 * @file        rex/graphics/webgpu/webgpu_texture_cache.h
 * @brief       WebGPU texture cache: interface used by the command processor.
 *
 * STUB (WP1). No textures are loaded and there is no swap texture, so IssueSwap
 * fills the guest output with the last resolve clear color. WP5 replaces this
 * with WebGPUTextureCache : TextureCache (compute untile/convert into a buffer,
 * then copyBufferToTexture, docs/port/webgpu.md section 7), keeping these entry
 * points. Texture uploads must be recorded into the CP stream outside render
 * passes (WebGPUCommandProcessor::EndRenderPass first).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>

#include <webgpu/webgpu.h>

#include <rex/graphics/register_file.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::webgpu {

class WebGPUCommandProcessor;
class WebGPUSharedMemory;

class WebGPUTextureCache {
 public:
  WebGPUTextureCache(WebGPUCommandProcessor& command_processor, const RegisterFile& register_file,
                     WebGPUSharedMemory& shared_memory);
  ~WebGPUTextureCache();

  bool Initialize();
  void Shutdown();
  void ClearCache();

  void TextureFetchConstantWritten(uint32_t /*index*/) {}
  void TextureFetchConstantsWritten(uint32_t /*first_index*/, uint32_t /*last_index*/) {}
  // Makes the textures in used_texture_mask (fetch constant bits) resident.
  void RequestTextures(uint32_t used_texture_mask);
  // Marks guest memory written by a resolve (textures overlapping it are
  // reloaded).
  void MarkRangeAsResolved(uint32_t /*start_unscaled*/, uint32_t /*length_unscaled*/) {}

  // The front buffer (texture fetch constant 0) as a 2D view on the device
  // thread, or null. width/height: texture size (unscaled).
  WGPUTextureView RequestSwapTexture(uint32_t& width_out, uint32_t& height_out,
                                     xenos::TextureFormat& format_out);

 private:
  WebGPUCommandProcessor& command_processor_;
  const RegisterFile& register_file_;
  WebGPUSharedMemory& shared_memory_;
};

}  // namespace rex::graphics::webgpu
