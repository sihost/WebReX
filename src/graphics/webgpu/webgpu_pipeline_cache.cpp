/**
 * @file        graphics/webgpu/webgpu_pipeline_cache.cpp
 * @brief       STUB WebGPU pipeline cache (WP1): see rex/graphics/webgpu/webgpu_pipeline_cache.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/webgpu_pipeline_cache.h>

#include <atomic>

#include <rex/hash.h>
#include <rex/logging.h>

namespace rex::graphics::webgpu {

WebGPUPipelineCache::WebGPUPipelineCache(WebGPUCommandProcessor& command_processor,
                                         const RegisterFile& register_file)
    : command_processor_(command_processor), register_file_(register_file) {}

WebGPUPipelineCache::~WebGPUPipelineCache() {
  Shutdown();
}

bool WebGPUPipelineCache::Initialize() {
  REXGPU_WARN(
      "[STUB] WebGPU pipeline cache: shaders are not translated (WP2), draws are dropped");
  return true;
}

void WebGPUPipelineCache::Shutdown() {
  ClearCache();
}

void WebGPUPipelineCache::ClearCache() {
  // Draws only hold Shader pointers for the duration of IssueDraw.
  shaders_.clear();
}

void WebGPUPipelineCache::InitializeShaderStorage(const std::filesystem::path& /*cache_root*/,
                                                  uint32_t /*title_id*/, bool /*blocking*/) {}

Shader* WebGPUPipelineCache::LoadShader(xenos::ShaderType shader_type,
                                        const uint32_t* host_address, uint32_t dword_count) {
  uint64_t hash = XXH3_64bits(host_address, dword_count * sizeof(uint32_t));
  auto it = shaders_.find(hash);
  if (it != shaders_.end()) {
    return it->second.get();
  }
  auto shader = std::make_unique<Shader>(shader_type, hash, host_address, dword_count);
  Shader* result = shader.get();
  shaders_.emplace(hash, std::move(shader));
  return result;
}

bool WebGPUPipelineCache::ConfigurePipeline(const WebGPUDrawPipelineState& /*state*/,
                                            PipelineId& pipeline_out) {
  pipeline_out = kInvalidPipelineId;
  return false;
}

}  // namespace rex::graphics::webgpu
