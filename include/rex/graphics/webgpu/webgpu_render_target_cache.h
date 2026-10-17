/**
 * @file        rex/graphics/webgpu/webgpu_render_target_cache.h
 * @brief       WebGPU render target cache: interface used by the command processor.
 *
 * STUB (WP1). No host render targets exist: Update() fails (the draw is
 * dropped), Resolve() records nothing. WP4 replaces this with
 * WebGPURenderTargetCache : RenderTargetCache (Path::kHostRenderTargets only,
 * docs/port/webgpu.md sections 5 and 6) keeping these entry points:
 *  - Update(): bind the guest RTs for the draw; afterwards the command
 *    processor calls BeginRenderPass(last_update_render_pass()) unless that
 *    pass is already open in the current stream (same key, same stream).
 *  - Resolve(): EDRAM -> shared memory (compute, recorded into the CP stream)
 *    plus EDRAM clears; reports the written range so the CP can mark it
 *    written by the GPU in the shared memory and the texture cache.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/webgpu/webgpu_command_stream.h>
#include <rex/memory.h>

namespace rex::graphics::webgpu {

class WebGPUCommandProcessor;
class WebGPUSharedMemory;
class WebGPUTextureCache;

class WebGPURenderTargetCache {
 public:
  WebGPURenderTargetCache(WebGPUCommandProcessor& command_processor,
                          const RegisterFile& register_file);
  ~WebGPURenderTargetCache();

  // shared_memory_binding_count: storage bindings the shared memory needs
  // (WebGPUSharedMemory::binding_count()).
  bool Initialize(uint32_t shared_memory_binding_count);
  void Shutdown();
  void ClearCache();
  void RestoreEdramSnapshot(const void* snapshot);

  // Called by the command processor when its stream has been submitted: any
  // render pass it had open is closed now.
  void OnStreamSubmitted() {}

  bool Update(bool is_rasterization_done, reg::RB_DEPTHCONTROL normalized_depth_control,
              uint32_t normalized_color_mask, const Shader& vertex_shader);
  const WebGPUCommandStream::RenderPassDesc& last_update_render_pass() const {
    return last_update_render_pass_;
  }
  uint64_t last_update_render_pass_key() const { return last_update_render_pass_key_; }

  bool Resolve(memory::Memory& memory, WebGPUSharedMemory& shared_memory,
               WebGPUTextureCache& texture_cache, uint32_t& written_address_out,
               uint32_t& written_length_out);

 private:
  WebGPUCommandProcessor& command_processor_;
  const RegisterFile& register_file_;
  WebGPUCommandStream::RenderPassDesc last_update_render_pass_;
  uint64_t last_update_render_pass_key_ = 0;
};

}  // namespace rex::graphics::webgpu
