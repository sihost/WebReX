#pragma once
/**
 * @file        graphics/webgpu/webgpu_shader.h
 * @brief       SPIR-V (SpirvShaderTranslator output) -> WGSL via Tint.
 *
 * Implemented in src/graphics/webgpu/webgpu_shader_tint.cpp, which is compiled
 * together with Tint (thirdparty/tint wrapper around thirdparty/dawn, pinned to
 * the emdawnwebgpu Dawn revision) into libtint_web.a. Design: project
 * docs/port/webgpu.md section 3; step 0 results: docs/port/webgpu_wp2.md.
 *
 * Thread safety: SpirvToWgsl is reentrant (no global state). Call it on the
 * command processor thread or a translator worker, never on the device thread.
 * A Tint internal compiler error traps (aborts the module), so inputs must be
 * translator output covered by the step 0 corpus.
 */

#include <cstdint>
#include <span>
#include <string>

namespace rex::graphics::webgpu {

struct SpirvToWgslOptions {
  // Rewrite texel buffers (Dim Buffer images: uniform / storage texel buffers)
  // to 1D textures, since WGSL has no texel buffers. Fetches get an explicit
  // LOD 0. The host must bind texture_1d / texture_storage_1d instead. Used for
  // the fixed shaders (gamma ramp lookups).
  bool texel_buffers_as_1d_textures = false;
  // Push constant blocks (fixed shaders) become uniform buffers at
  // @group(push_constants_group) @binding(push_constants_binding + i).
  // UINT32_MAX = the group after the highest descriptor set the module uses.
  uint32_t push_constants_group = UINT32_MAX;
  uint32_t push_constants_binding = 0;
  // WGSL extensions the device enabled (GPUFeatureName "clip-distances",
  // "dual-source-blending"). Output needing a disallowed one fails.
  bool clip_distances = true;
  bool dual_source_blending = false;
};

// Converts a Vulkan-flavored SPIR-V module to WGSL. The output starts with
// `diagnostic(off, derivative_uniformity);` when the module has derivatives in
// non-uniform control flow (Xenos predicated fetches). Returns false and sets
// error_out on failure.
bool SpirvToWgsl(std::span<const uint32_t> spirv, std::string& wgsl_out, std::string& error_out,
                 const SpirvToWgslOptions& options = {});

// The Dawn/Tint revision the converter was built from, for shader cache keys.
const char* TintRevision();

}  // namespace rex::graphics::webgpu
