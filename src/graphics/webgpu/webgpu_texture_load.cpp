/**
 * @file        graphics/webgpu/webgpu_texture_load.cpp
 * @brief       See rex/graphics/webgpu/webgpu_texture_load.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/webgpu_texture_load.h>

#include <algorithm>
#include <cstring>
#include <string>

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/webgpu/webgpu_command_stream.h>
#include <rex/graphics/webgpu/webgpu_device.h>
#include <rex/graphics/webgpu/webgpu_log.h>

namespace rex::graphics::webgpu {

namespace {

// xenia's texture_load_*.cs.xesl via SPIR-V -> native Tint (gen_wgsl.py in the project).
#include "webgpu_texture_load_shaders.inc"

// ---------------------------------------------------------------------------
// Hand-written WGSL for the WebGPU-only loads, same bindings, constants, workgroup size
// (4 x 32) and host layout rules as the converted shaders. One guest block per invocation.
// ---------------------------------------------------------------------------

constexpr char kWgslPrelude[] = R"WGSL(
struct XeTextureLoadConstants {
  is_tiled_3d_endian_scale : u32,
  guest_offset : u32,
  guest_pitch_aligned : u32,
  guest_z_stride_block_rows_aligned : u32,
  size_blocks : vec3<u32>,
  host_offset : u32,
  host_pitch : u32,
  height_texels : u32,
}
@group(0) @binding(0) var<storage, read_write> xe_dest : array<u32>;
@group(0) @binding(1) var<storage, read> xe_source : array<u32>;
@group(0) @binding(2) var<uniform> xe_c : XeTextureLoadConstants;

fn xe_endian() -> u32 {
  return (xe_c.is_tiled_3d_endian_scale >> 2u) & 3u;
}
// XeEndianSwap32: k8in16 = 1, k8in32 = 2, k16in32 = 3.
fn xe_endian_swap32(v : u32) -> u32 {
  let endian = xe_endian();
  var r = v;
  if (endian == 1u || endian == 2u) {
    r = ((r & 0x00FF00FFu) << 8u) | ((r & 0xFF00FF00u) >> 8u);
  }
  if (endian == 2u || endian == 3u) {
    r = (r << 16u) | (r >> 16u);
  }
  return r;
}
// texture_util::GetTiledOffset2D (pitch in blocks, aligned to 32).
fn xe_tiled_offset_2d(p : vec2<i32>, pitch : u32, bpb_log2 : u32) -> i32 {
  let mac = ((p.x >> 5u) + (p.y >> 5u) * i32(pitch >> 5u)) << (bpb_log2 + 7u);
  let mic = ((p.x & 7) + ((p.y & 14) << 2u)) << bpb_log2;
  let offset = mac + ((mic & ~15) << 1u) + (mic & 15) + ((p.y & 1) << 4u);
  return ((offset & ~511) << 3u) + ((p.y & 16) << 7u) + ((offset & 448) << 2u) +
         (((((p.y & 8) >> 2u) + (p.x >> 3u)) & 3) << 6u) + (offset & 63);
}
// texture_util::GetTiledOffset3D (pitch and height in blocks, aligned to 32).
fn xe_tiled_offset_3d(p : vec3<i32>, pitch : u32, height : u32, bpb_log2 : u32) -> i32 {
  let macro_outer = ((p.y >> 4u) + (p.z >> 2u) * i32(height >> 4u)) * i32(pitch >> 5u);
  let mac = ((((p.x >> 5u) + macro_outer) << (bpb_log2 + 6u)) & 0xFFFFFFF) << 1u;
  let mic = (((p.x & 7) + ((p.y & 6) << 2u)) << (bpb_log2 + 6u)) >> 6u;
  let offset_outer = ((p.y >> 3u) + (p.z >> 2u)) & 1;
  let offset1 = offset_outer + ((((p.x >> 3u) + (offset_outer << 1u)) & 3) << 1u);
  let offset2 = ((mac + (mic & ~15)) << 1u) + (mic & 15) + ((p.z & 3) << (bpb_log2 + 6u)) +
                ((p.y & 1) << 4u);
  var address = (offset1 & 1) << 3u;
  address += (offset2 >> 6u) & 7;
  address <<= 3u;
  address += offset1 & ~1;
  address <<= 2u;
  address += offset2 & ~511;
  address <<= 3u;
  address += offset2 & 63;
  return address;
}
// Byte offset of a guest block in the source binding.
fn xe_guest_block_offset(p : vec3<u32>, bpb : u32, bpb_log2 : u32) -> u32 {
  let flags = xe_c.is_tiled_3d_endian_scale;
  let pi = vec3<i32>(p);
  var offset : i32;
  if ((flags & 1u) != 0u) {
    if ((flags & 2u) != 0u) {
      offset = xe_tiled_offset_3d(pi, xe_c.guest_pitch_aligned,
                                  xe_c.guest_z_stride_block_rows_aligned, bpb_log2);
    } else {
      offset = xe_tiled_offset_2d(pi.xy, xe_c.guest_pitch_aligned, bpb_log2);
    }
  } else {
    offset = pi.x * i32(bpb) +
             (pi.z * i32(xe_c.guest_z_stride_block_rows_aligned) + pi.y) *
                 i32(xe_c.guest_pitch_aligned);
  }
  return u32(i32(xe_c.guest_offset) + offset);
}
// round(v * 255 / (2^bits - 1)).
fn xe_unorm8(v : u32, bits : u32) -> u32 {
  let m = (1u << bits) - 1u;
  return (v * 255u + (m >> 1u)) / m;
}
)WGSL";

// 16bpp -> rgba8unorm (R in the low byte). Endian like xenia's 16bpp loads: only k8in16
// swaps (bytes within 16 bits). XE_CONVERT is spliced in per format.
constexpr char kWgsl16bppToRGBA8Main[] = R"WGSL(
@compute @workgroup_size(4, 32, 1)
fn main(@builtin(global_invocation_id) id : vec3<u32>) {
  if (any(id.xy >= xe_c.size_blocks.xy)) {
    return;
  }
  let guest = xe_guest_block_offset(id, 2u, 1u);
  var word = xe_source[guest >> 2u];
  if (xe_endian() == 1u) {
    word = ((word & 0x00FF00FFu) << 8u) | ((word & 0xFF00FF00u) >> 8u);
  }
  let t = (word >> ((guest & 2u) << 3u)) & 0xFFFFu;
  let host = xe_c.host_offset + id.x * 4u + (id.z * xe_c.size_blocks.y + id.y) * xe_c.host_pitch;
  xe_dest[host >> 2u] = xe_convert(t);
}
)WGSL";

constexpr char kWgslConvertR5G5B5A1[] = R"WGSL(
fn xe_convert(t : u32) -> u32 {
  return xe_unorm8(t & 31u, 5u) | (xe_unorm8((t >> 5u) & 31u, 5u) << 8u) |
         (xe_unorm8((t >> 10u) & 31u, 5u) << 16u) | (select(0u, 255u, (t & 0x8000u) != 0u) << 24u);
}
)WGSL";
constexpr char kWgslConvertR5G6B5[] = R"WGSL(
fn xe_convert(t : u32) -> u32 {
  return xe_unorm8(t & 31u, 5u) | (xe_unorm8((t >> 5u) & 63u, 6u) << 8u) |
         (xe_unorm8(t >> 11u, 5u) << 16u) | 0xFF000000u;
}
)WGSL";
constexpr char kWgslConvertR5G5B6[] = R"WGSL(
fn xe_convert(t : u32) -> u32 {
  return xe_unorm8(t & 31u, 5u) | (xe_unorm8((t >> 5u) & 31u, 5u) << 8u) |
         (xe_unorm8(t >> 10u, 6u) << 16u) | 0xFF000000u;
}
)WGSL";
constexpr char kWgslConvertR4G4B4A4[] = R"WGSL(
fn xe_convert(t : u32) -> u32 {
  return ((t & 15u) * 17u) | ((((t >> 4u) & 15u) * 17u) << 8u) |
         ((((t >> 8u) & 15u) * 17u) << 16u) | (((t >> 12u) * 17u) << 24u);
}
)WGSL";

// DXT3A as 1_1_1_1: each 4-bit DXT3A value is 4 1-bit components, bit 3 -> R, 2 -> G, 1 -> B,
// 0 -> A (the Vulkan load writes them to B4G4R4A4 this way). Output rgba8unorm 0 / 255.
constexpr char kWgslDXT3AAs1111ToRGBA8[] = R"WGSL(
@compute @workgroup_size(4, 32, 1)
fn main(@builtin(global_invocation_id) id : vec3<u32>) {
  if (any(id.xy >= xe_c.size_blocks.xy)) {
    return;
  }
  let guest = xe_guest_block_offset(id, 8u, 3u);
  let block = vec2<u32>(xe_endian_swap32(xe_source[guest >> 2u]),
                        xe_endian_swap32(xe_source[(guest >> 2u) + 1u]));
  let texel_y = id.y * 4u;
  for (var r = 0u; r < 4u; r++) {
    if (texel_y + r >= xe_c.height_texels) {
      break;
    }
    let row = (select(block.y, block.x, r < 2u) >> ((r & 1u) * 16u)) & 0xFFFFu;
    let host = xe_c.host_offset + id.x * 16u +
               (id.z * xe_c.height_texels + texel_y + r) * xe_c.host_pitch;
    for (var i = 0u; i < 4u; i++) {
      let a = (row >> (i * 4u)) & 15u;
      xe_dest[(host >> 2u) + i] =
          select(0u, 0x000000FFu, (a & 8u) != 0u) | select(0u, 0x0000FF00u, (a & 4u) != 0u) |
          select(0u, 0x00FF0000u, (a & 2u) != 0u) | select(0u, 0xFF000000u, (a & 1u) != 0u);
    }
  }
}
)WGSL";

// Signed 2_10_10_10 -> rgba16snorm: two's complement components, -2^(n-1) clamped to
// -(2^(n-1) - 1) (both are -1.0), then round(s * 32767 / (2^(n-1) - 1)).
constexpr char kWgslR10G10B10A2ToRGBA16SNorm[] = R"WGSL(
fn xe_snorm16(v : u32, bits : u32) -> u32 {
  let sign_bit = 1u << (bits - 1u);
  let m = i32(sign_bit - 1u);
  let s = max(i32(v ^ sign_bit) - i32(sign_bit), -m);
  let r = (abs(s) * 32767 + (m >> 1u)) / m;
  return u32(select(r, -r, s < 0)) & 0xFFFFu;
}
@compute @workgroup_size(4, 32, 1)
fn main(@builtin(global_invocation_id) id : vec3<u32>) {
  if (any(id.xy >= xe_c.size_blocks.xy)) {
    return;
  }
  let guest = xe_guest_block_offset(id, 4u, 2u);
  let w = xe_endian_swap32(xe_source[guest >> 2u]);
  let host = (xe_c.host_offset + id.x * 8u +
              (id.z * xe_c.size_blocks.y + id.y) * xe_c.host_pitch) >> 2u;
  xe_dest[host] = xe_snorm16(w & 1023u, 10u) | (xe_snorm16((w >> 10u) & 1023u, 10u) << 16u);
  xe_dest[host + 1u] =
      xe_snorm16((w >> 20u) & 1023u, 10u) | (xe_snorm16(w >> 30u, 2u) << 16u);
}
)WGSL";

using Shader = WebGPUTextureLoadShader;

constexpr WebGPUTextureLoadShaderInfo kShaderInfo[size_t(Shader::kCount)] = {
    {"unknown", 0, 0},
    {"8bpb", 1, 4},
    {"16bpb", 2, 4},
    {"32bpb", 4, 3},
    {"64bpb", 8, 2},
    {"128bpb", 16, 1},
    {"r5g5b5a1_rgba8", 4, 0},
    {"r5g6b5_rgba8", 4, 0},
    {"r5g5b6_rgba8", 4, 0},
    {"r4g4b4a4_rgba8", 4, 0},
    // Per host texel (the Vulkan table's 8 is per guest 2-texel block).
    {"gbgr8_rgb8", 4, 3},
    {"bgrg8_rgb8", 4, 3},
    {"r10g11b11_rgba16", 8, 3},
    {"r10g11b11_rgba16_snorm", 8, 3},
    {"r11g11b10_rgba16", 8, 3},
    {"r11g11b10_rgba16_snorm", 8, 3},
    {"r10g10b10a2_rgba16_snorm", 8, 0},
    {"r16_unorm_float", 2, 4},
    {"r16_snorm_float", 2, 4},
    {"rg16_unorm_float", 4, 3},
    {"rg16_snorm_float", 4, 3},
    {"rgba16_unorm_float", 8, 2},
    {"rgba16_snorm_float", 8, 2},
    {"dxt1_rgba8", 4, 2},
    {"dxt3_rgba8", 4, 1},
    {"dxt5_rgba8", 4, 1},
    {"dxn_rg8", 2, 1},
    {"dxt3a", 1, 2},
    {"dxt3aas1111_rgba8", 4, 0},
    {"dxt5a_r8", 1, 2},
    {"ctx1", 2, 2},
    {"depth_unorm", 4, 3},
    {"depth_float", 4, 3},
};

std::string GetShaderSource(Shader shader) {
  switch (shader) {
    case Shader::k8bpb:
      return kTextureLoadWgsl_8bpb;
    case Shader::k16bpb:
      return kTextureLoadWgsl_16bpb;
    case Shader::k32bpb:
      return kTextureLoadWgsl_32bpb;
    case Shader::k64bpb:
      return kTextureLoadWgsl_64bpb;
    case Shader::k128bpb:
      return kTextureLoadWgsl_128bpb;
    case Shader::kR5G5B5A1ToRGBA8:
      return std::string(kWgslPrelude) + kWgslConvertR5G5B5A1 + kWgsl16bppToRGBA8Main;
    case Shader::kR5G6B5ToRGBA8:
      return std::string(kWgslPrelude) + kWgslConvertR5G6B5 + kWgsl16bppToRGBA8Main;
    case Shader::kR5G5B6ToRGBA8:
      return std::string(kWgslPrelude) + kWgslConvertR5G5B6 + kWgsl16bppToRGBA8Main;
    case Shader::kR4G4B4A4ToRGBA8:
      return std::string(kWgslPrelude) + kWgslConvertR4G4B4A4 + kWgsl16bppToRGBA8Main;
    case Shader::kGBGR8ToRGB8:
      return kTextureLoadWgsl_gbgr8_rgb8;
    case Shader::kBGRG8ToRGB8:
      return kTextureLoadWgsl_bgrg8_rgb8;
    case Shader::kR10G11B11ToRGBA16:
      return kTextureLoadWgsl_r10g11b11_rgba16;
    case Shader::kR10G11B11ToRGBA16SNorm:
      return kTextureLoadWgsl_r10g11b11_rgba16_snorm;
    case Shader::kR11G11B10ToRGBA16:
      return kTextureLoadWgsl_r11g11b10_rgba16;
    case Shader::kR11G11B10ToRGBA16SNorm:
      return kTextureLoadWgsl_r11g11b10_rgba16_snorm;
    case Shader::kR10G10B10A2ToRGBA16SNorm:
      return std::string(kWgslPrelude) + kWgslR10G10B10A2ToRGBA16SNorm;
    case Shader::kR16UNormToFloat:
      return kTextureLoadWgsl_r16_unorm_float;
    case Shader::kR16SNormToFloat:
      return kTextureLoadWgsl_r16_snorm_float;
    case Shader::kRG16UNormToFloat:
      return kTextureLoadWgsl_rg16_unorm_float;
    case Shader::kRG16SNormToFloat:
      return kTextureLoadWgsl_rg16_snorm_float;
    case Shader::kRGBA16UNormToFloat:
      return kTextureLoadWgsl_rgba16_unorm_float;
    case Shader::kRGBA16SNormToFloat:
      return kTextureLoadWgsl_rgba16_snorm_float;
    case Shader::kDXT1ToRGBA8:
      return kTextureLoadWgsl_dxt1_rgba8;
    case Shader::kDXT3ToRGBA8:
      return kTextureLoadWgsl_dxt3_rgba8;
    case Shader::kDXT5ToRGBA8:
      return kTextureLoadWgsl_dxt5_rgba8;
    case Shader::kDXNToRG8:
      return kTextureLoadWgsl_dxn_rg8;
    case Shader::kDXT3AToR8:
      return kTextureLoadWgsl_dxt3a;
    case Shader::kDXT3AAs1111ToRGBA8:
      return std::string(kWgslPrelude) + kWgslDXT3AAs1111ToRGBA8;
    case Shader::kDXT5AToR8:
      return kTextureLoadWgsl_dxt5a_r8;
    case Shader::kCTX1ToRG8:
      return kTextureLoadWgsl_ctx1;
    case Shader::kDepthUnorm:
      return kTextureLoadWgsl_depth_unorm;
    case Shader::kDepthFloat:
      return kTextureLoadWgsl_depth_float;
    default:
      return {};
  }
}

// Same 40 bytes as TextureCache::LoadConstants, padded to the 48-byte WGSL struct size.
struct LoadConstants {
  uint32_t is_tiled_3d_endian_scale;
  uint32_t guest_offset;
  uint32_t guest_pitch_aligned;
  uint32_t guest_z_stride_block_rows_aligned;
  uint32_t size_blocks[3];
  uint32_t host_offset;
  uint32_t host_pitch;
  uint32_t height_texels;
  uint32_t padding[2];
};
static_assert(sizeof(LoadConstants) == 48);

constexpr uint32_t kLoadGuestXThreadsPerGroupLog2 = 2;
constexpr uint32_t kLoadGuestYBlocksPerGroupLog2 = 5;
constexpr uint64_t kRowPitchAlignment = 256;
constexpr uint64_t kScratchMinSize = uint64_t(4) << 20;
constexpr uint64_t kScratchMaxSize = uint64_t(256) << 20;

template <typename T>
constexpr T AlignUp(T value, T alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

WebGPUTextureHostFormat HF(Shader load, WGPUTextureFormat format, bool block_compressed = false,
                           Shader convert = Shader::kUnknown) {
  WebGPUTextureHostFormat f;
  f.load_shader = load;
  f.convert_shader = convert;
  f.format = format;
  f.block_compressed = block_compressed;
  return f;
}

WebGPUTextureHostFormatPair Pair(WebGPUTextureHostFormat u, WebGPUTextureHostFormat s,
                                 uint32_t swizzle) {
  WebGPUTextureHostFormatPair p;
  p.format_unsigned = u;
  p.format_signed = s;
  p.swizzle = swizzle;
  return p;
}

uint32_t Log2Floor(uint32_t v) {
  uint32_t r = 0;
  while (v >>= 1) {
    ++r;
  }
  return r;
}

}  // namespace

const WebGPUTextureLoadShaderInfo& GetTextureLoadShaderInfo(WebGPUTextureLoadShader shader) {
  uint32_t index = uint32_t(shader);
  return kShaderInfo[index < uint32_t(Shader::kCount) ? index : 0];
}

// ---------------------------------------------------------------------------
// Format table.
// ---------------------------------------------------------------------------

WebGPUTextureFormatTable::WebGPUTextureFormatTable(const WebGPUTextureFeatures& features) {
  Initialize(features);
}

void WebGPUTextureFormatTable::Initialize(const WebGPUTextureFeatures& features) {
  features_ = features;
  using F = xenos::TextureFormat;
  const WebGPUTextureHostFormat none;
  constexpr uint32_t kRRRR = xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR;
  constexpr uint32_t kRGGG = xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG;
  constexpr uint32_t kRGBB = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB;
  constexpr uint32_t kRGBA = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  for (uint32_t i = 0; i < 64; ++i) {
    formats_[i] = Pair(none, none, kRGBA);
  }
  auto set = [this](F format, const WebGPUTextureHostFormatPair& pair) {
    formats_[uint32_t(format)] = pair;
  };
  const bool tier1 = features.formats_tier1;
  const bool bc = features.bc;

  set(F::k_1_REVERSE, Pair(none, none, kRRRR));
  set(F::k_1, Pair(none, none, kRRRR));
  set(F::k_8, Pair(HF(Shader::k8bpb, WGPUTextureFormat_R8Unorm),
                   HF(Shader::k8bpb, WGPUTextureFormat_R8Snorm), kRRRR));
  set(F::k_1_5_5_5, Pair(HF(Shader::kR5G5B5A1ToRGBA8, WGPUTextureFormat_RGBA8Unorm), none, kRGBA));
  set(F::k_5_6_5, Pair(HF(Shader::kR5G6B5ToRGBA8, WGPUTextureFormat_RGBA8Unorm), none, kRGBB));
  set(F::k_6_5_5, Pair(HF(Shader::kR5G5B6ToRGBA8, WGPUTextureFormat_RGBA8Unorm), none, kRGBB));
  WebGPUTextureHostFormatPair rgba8 = Pair(HF(Shader::k32bpb, WGPUTextureFormat_RGBA8Unorm),
                                           HF(Shader::k32bpb, WGPUTextureFormat_RGBA8Snorm), kRGBA);
  set(F::k_8_8_8_8, rgba8);
  set(F::k_8_8_8_8_AS_16_16_16_16, rgba8);
  WebGPUTextureHostFormatPair rgb10a2 = Pair(
      HF(Shader::k32bpb, WGPUTextureFormat_RGB10A2Unorm),
      tier1 ? HF(Shader::kR10G10B10A2ToRGBA16SNorm, WGPUTextureFormat_RGBA16Snorm)
            : HF(Shader::kR10G10B10A2ToRGBA16SNorm, WGPUTextureFormat_RGBA16Float, false,
                 Shader::kRGBA16SNormToFloat),
      kRGBA);
  set(F::k_2_10_10_10, rgb10a2);
  set(F::k_2_10_10_10_AS_16_16_16_16, rgb10a2);
  set(F::k_8_A, formats_[uint32_t(F::k_8)]);
  set(F::k_8_B, Pair(none, none, kRRRR));
  set(F::k_8_8, Pair(HF(Shader::k16bpb, WGPUTextureFormat_RG8Unorm),
                     HF(Shader::k16bpb, WGPUTextureFormat_RG8Snorm), kRGGG));
  // No 4:2:2 formats in WebGPU: always expanded (signed is unsupported, as on D3D12/Vulkan).
  set(F::k_Cr_Y1_Cb_Y0_REP,
      Pair(HF(Shader::kGBGR8ToRGB8, WGPUTextureFormat_RGBA8Unorm), none, kRGBB));
  set(F::k_Y1_Cr_Y0_Cb_REP,
      Pair(HF(Shader::kBGRG8ToRGB8, WGPUTextureFormat_RGBA8Unorm), none, kRGBB));
  set(F::k_16_16_EDRAM, Pair(none, none, kRGGG));
  set(F::k_8_8_8_8_A, Pair(none, none, kRGBA));
  set(F::k_4_4_4_4, Pair(HF(Shader::kR4G4B4A4ToRGBA8, WGPUTextureFormat_RGBA8Unorm), none, kRGBA));
  WebGPUTextureHostFormatPair r11g11b10 =
      tier1 ? Pair(HF(Shader::kR11G11B10ToRGBA16, WGPUTextureFormat_RGBA16Unorm),
                   HF(Shader::kR11G11B10ToRGBA16SNorm, WGPUTextureFormat_RGBA16Snorm), kRGBB)
            : Pair(HF(Shader::kR11G11B10ToRGBA16, WGPUTextureFormat_RGBA16Float, false,
                      Shader::kRGBA16UNormToFloat),
                   HF(Shader::kR11G11B10ToRGBA16SNorm, WGPUTextureFormat_RGBA16Float, false,
                      Shader::kRGBA16SNormToFloat),
                   kRGBB);
  set(F::k_10_11_11, r11g11b10);
  set(F::k_10_11_11_AS_16_16_16_16, r11g11b10);
  WebGPUTextureHostFormatPair r10g11b11 =
      tier1 ? Pair(HF(Shader::kR10G11B11ToRGBA16, WGPUTextureFormat_RGBA16Unorm),
                   HF(Shader::kR10G11B11ToRGBA16SNorm, WGPUTextureFormat_RGBA16Snorm), kRGBB)
            : Pair(HF(Shader::kR10G11B11ToRGBA16, WGPUTextureFormat_RGBA16Float, false,
                      Shader::kRGBA16UNormToFloat),
                   HF(Shader::kR10G11B11ToRGBA16SNorm, WGPUTextureFormat_RGBA16Float, false,
                      Shader::kRGBA16SNormToFloat),
                   kRGBB);
  set(F::k_11_11_10, r10g11b11);
  set(F::k_11_11_10_AS_16_16_16_16, r10g11b11);

  dxt1_decompress_ = Pair(HF(Shader::kDXT1ToRGBA8, WGPUTextureFormat_RGBA8Unorm), none, kRGBA);
  dxt2_3_decompress_ = Pair(HF(Shader::kDXT3ToRGBA8, WGPUTextureFormat_RGBA8Unorm), none, kRGBA);
  dxt4_5_decompress_ = Pair(HF(Shader::kDXT5ToRGBA8, WGPUTextureFormat_RGBA8Unorm), none, kRGBA);
  dxn_decompress_ = Pair(HF(Shader::kDXNToRG8, WGPUTextureFormat_RG8Unorm), none, kRGGG);
  dxt5a_decompress_ = Pair(HF(Shader::kDXT5AToR8, WGPUTextureFormat_R8Unorm), none, kRRRR);
  WebGPUTextureHostFormatPair dxt1 =
      bc ? Pair(HF(Shader::k64bpb, WGPUTextureFormat_BC1RGBAUnorm, true), none, kRGBA)
         : dxt1_decompress_;
  WebGPUTextureHostFormatPair dxt2_3 =
      bc ? Pair(HF(Shader::k128bpb, WGPUTextureFormat_BC2RGBAUnorm, true), none, kRGBA)
         : dxt2_3_decompress_;
  WebGPUTextureHostFormatPair dxt4_5 =
      bc ? Pair(HF(Shader::k128bpb, WGPUTextureFormat_BC3RGBAUnorm, true), none, kRGBA)
         : dxt4_5_decompress_;
  set(F::k_DXT1, dxt1);
  set(F::k_DXT1_AS_16_16_16_16, dxt1);
  set(F::k_DXT2_3, dxt2_3);
  set(F::k_DXT2_3_AS_16_16_16_16, dxt2_3);
  set(F::k_DXT4_5, dxt4_5);
  set(F::k_DXT4_5_AS_16_16_16_16, dxt4_5);
  set(F::k_DXN, bc ? Pair(HF(Shader::k128bpb, WGPUTextureFormat_BC5RGUnorm, true), none, kRGGG)
                   : dxn_decompress_);
  set(F::k_DXT5A, bc ? Pair(HF(Shader::k64bpb, WGPUTextureFormat_BC4RUnorm, true), none, kRRRR)
                     : dxt5a_decompress_);
  set(F::k_DXT3A, Pair(HF(Shader::kDXT3AToR8, WGPUTextureFormat_R8Unorm), none, kRRRR));
  set(F::k_CTX1, Pair(HF(Shader::kCTX1ToRG8, WGPUTextureFormat_RG8Unorm), none, kRGGG));
  set(F::k_DXT3A_AS_1_1_1_1,
      Pair(HF(Shader::kDXT3AAs1111ToRGBA8, WGPUTextureFormat_RGBA8Unorm), none, kRGBA));
  set(F::k_16_16_16_16_EDRAM, Pair(none, none, kRGBA));

  // Depth: same r32float data for both signednesses (parity with D3D12/Vulkan).
  set(F::k_24_8, Pair(HF(Shader::kDepthUnorm, WGPUTextureFormat_R32Float),
                      HF(Shader::kDepthUnorm, WGPUTextureFormat_R32Float), kRRRR));
  set(F::k_24_8_FLOAT, Pair(HF(Shader::kDepthFloat, WGPUTextureFormat_R32Float),
                            HF(Shader::kDepthFloat, WGPUTextureFormat_R32Float), kRRRR));
  formats_[uint32_t(F::k_24_8)].format_unsigned.filterable = features.float32_filterable;
  formats_[uint32_t(F::k_24_8)].format_signed.filterable = features.float32_filterable;
  formats_[uint32_t(F::k_24_8_FLOAT)].format_unsigned.filterable = features.float32_filterable;
  formats_[uint32_t(F::k_24_8_FLOAT)].format_signed.filterable = features.float32_filterable;

  // 16-bit normalized: native with texture-formats-tier1, else float16.
  if (tier1) {
    set(F::k_16, Pair(HF(Shader::k16bpb, WGPUTextureFormat_R16Unorm),
                      HF(Shader::k16bpb, WGPUTextureFormat_R16Snorm), kRRRR));
    set(F::k_16_16, Pair(HF(Shader::k32bpb, WGPUTextureFormat_RG16Unorm),
                         HF(Shader::k32bpb, WGPUTextureFormat_RG16Snorm), kRGGG));
    set(F::k_16_16_16_16, Pair(HF(Shader::k64bpb, WGPUTextureFormat_RGBA16Unorm),
                               HF(Shader::k64bpb, WGPUTextureFormat_RGBA16Snorm), kRGBA));
  } else {
    set(F::k_16, Pair(HF(Shader::kR16UNormToFloat, WGPUTextureFormat_R16Float),
                      HF(Shader::kR16SNormToFloat, WGPUTextureFormat_R16Float), kRRRR));
    set(F::k_16_16, Pair(HF(Shader::kRG16UNormToFloat, WGPUTextureFormat_RG16Float),
                         HF(Shader::kRG16SNormToFloat, WGPUTextureFormat_RG16Float), kRGGG));
    set(F::k_16_16_16_16,
        Pair(HF(Shader::kRGBA16UNormToFloat, WGPUTextureFormat_RGBA16Float),
             HF(Shader::kRGBA16SNormToFloat, WGPUTextureFormat_RGBA16Float), kRGBA));
  }
  WebGPUTextureHostFormatPair r16f = Pair(HF(Shader::k16bpb, WGPUTextureFormat_R16Float),
                                          HF(Shader::k16bpb, WGPUTextureFormat_R16Float), kRRRR);
  WebGPUTextureHostFormatPair rg16f = Pair(HF(Shader::k32bpb, WGPUTextureFormat_RG16Float),
                                           HF(Shader::k32bpb, WGPUTextureFormat_RG16Float), kRGGG);
  WebGPUTextureHostFormatPair rgba16f =
      Pair(HF(Shader::k64bpb, WGPUTextureFormat_RGBA16Float),
           HF(Shader::k64bpb, WGPUTextureFormat_RGBA16Float), kRGBA);
  set(F::k_16_EXPAND, r16f);
  set(F::k_16_16_EXPAND, rg16f);
  set(F::k_16_16_16_16_EXPAND, rgba16f);
  set(F::k_16_FLOAT, r16f);
  set(F::k_16_16_FLOAT, rg16f);
  set(F::k_16_16_16_16_FLOAT, rgba16f);
  set(F::k_32, Pair(none, none, kRRRR));
  set(F::k_32_32, Pair(none, none, kRGGG));
  set(F::k_32_32_32_32, Pair(none, none, kRGBA));
  // 32-bit float: only filterable with float32-filterable (samplers fall back to nearest).
  set(F::k_32_FLOAT, Pair(HF(Shader::k32bpb, WGPUTextureFormat_R32Float),
                          HF(Shader::k32bpb, WGPUTextureFormat_R32Float), kRRRR));
  set(F::k_32_32_FLOAT, Pair(HF(Shader::k64bpb, WGPUTextureFormat_RG32Float),
                             HF(Shader::k64bpb, WGPUTextureFormat_RG32Float), kRGGG));
  set(F::k_32_32_32_32_FLOAT, Pair(HF(Shader::k128bpb, WGPUTextureFormat_RGBA32Float),
                                   HF(Shader::k128bpb, WGPUTextureFormat_RGBA32Float), kRGBA));
  for (F format : {F::k_32_FLOAT, F::k_32_32_FLOAT, F::k_32_32_32_32_FLOAT}) {
    formats_[uint32_t(format)].format_unsigned.filterable = features.float32_filterable;
    formats_[uint32_t(format)].format_signed.filterable = features.float32_filterable;
  }
  set(F::k_32_32_32_FLOAT, Pair(none, none, kRGBB));
}

const WebGPUTextureHostFormatPair& WebGPUTextureFormatTable::Get(xenos::TextureFormat format,
                                                                 xenos::DataDimension dimension,
                                                                 uint32_t width,
                                                                 uint32_t height) const {
  const WebGPUTextureHostFormatPair& pair = GetForFormat(format);
  if (!pair.format_unsigned.block_compressed && !pair.format_signed.block_compressed) {
    return pair;
  }
  // BC host textures need a 4-aligned base size (WebGPU; "texture compression unaligned"
  // isn't relied on) and, for 3D, texture-compression-bc-sliced-3d.
  bool aligned = !(width & 3) && !(height & 3);
  bool dimension_ok = dimension != xenos::DataDimension::k3D || features_.bc_sliced_3d;
  if (aligned && dimension_ok) {
    return pair;
  }
  switch (format) {
    case xenos::TextureFormat::k_DXT1:
    case xenos::TextureFormat::k_DXT1_AS_16_16_16_16:
      return dxt1_decompress_;
    case xenos::TextureFormat::k_DXT2_3:
    case xenos::TextureFormat::k_DXT2_3_AS_16_16_16_16:
      return dxt2_3_decompress_;
    case xenos::TextureFormat::k_DXT4_5:
    case xenos::TextureFormat::k_DXT4_5_AS_16_16_16_16:
      return dxt4_5_decompress_;
    case xenos::TextureFormat::k_DXN:
      return dxn_decompress_;
    case xenos::TextureFormat::k_DXT5A:
      return dxt5a_decompress_;
    default:
      return pair;
  }
}

bool WebGPUTextureFormatTable::IsSignedVersionSeparate(const WebGPUTextureHostFormatPair& pair) {
  if (pair.format_unsigned.format == WGPUTextureFormat_Undefined ||
      pair.format_signed.format == WGPUTextureFormat_Undefined) {
    return false;
  }
  return pair.format_unsigned.format != pair.format_signed.format ||
         pair.format_unsigned.load_shader != pair.format_signed.load_shader ||
         pair.format_unsigned.convert_shader != pair.format_signed.convert_shader;
}

const WebGPUTextureHostFormat& WebGPUTextureFormatTable::GetHostFormat(
    const WebGPUTextureHostFormatPair& pair, bool signed_separate) {
  bool use_signed;
  if (IsSignedVersionSeparate(pair)) {
    use_signed = signed_separate;
  } else {
    use_signed = pair.format_unsigned.load_shader == WebGPUTextureLoadShader::kUnknown;
  }
  return use_signed ? pair.format_signed : pair.format_unsigned;
}

uint32_t GetTextureFormatBytesPerBlock(WGPUTextureFormat format, uint32_t* block_size_out) {
  uint32_t block_size = 1;
  uint32_t bytes = 0;
  switch (format) {
    case WGPUTextureFormat_R8Unorm:
    case WGPUTextureFormat_R8Snorm:
      bytes = 1;
      break;
    case WGPUTextureFormat_RG8Unorm:
    case WGPUTextureFormat_RG8Snorm:
    case WGPUTextureFormat_R16Unorm:
    case WGPUTextureFormat_R16Snorm:
    case WGPUTextureFormat_R16Float:
      bytes = 2;
      break;
    case WGPUTextureFormat_RGBA8Unorm:
    case WGPUTextureFormat_RGBA8Snorm:
    case WGPUTextureFormat_RGB10A2Unorm:
    case WGPUTextureFormat_RG16Unorm:
    case WGPUTextureFormat_RG16Snorm:
    case WGPUTextureFormat_RG16Float:
    case WGPUTextureFormat_R32Float:
      bytes = 4;
      break;
    case WGPUTextureFormat_RGBA16Unorm:
    case WGPUTextureFormat_RGBA16Snorm:
    case WGPUTextureFormat_RGBA16Float:
    case WGPUTextureFormat_RG32Float:
      bytes = 8;
      break;
    case WGPUTextureFormat_RGBA32Float:
      bytes = 16;
      break;
    case WGPUTextureFormat_BC1RGBAUnorm:
    case WGPUTextureFormat_BC4RUnorm:
      block_size = 4;
      bytes = 8;
      break;
    case WGPUTextureFormat_BC2RGBAUnorm:
    case WGPUTextureFormat_BC3RGBAUnorm:
    case WGPUTextureFormat_BC5RGUnorm:
      block_size = 4;
      bytes = 16;
      break;
    default:
      break;
  }
  if (block_size_out) {
    *block_size_out = block_size;
  }
  return bytes;
}

bool GetHostTextureInfo(const WebGPUTextureFormatTable& table, xenos::TextureFormat format,
                        xenos::DataDimension dimension, uint32_t width, uint32_t height,
                        uint32_t depth_or_array_size, uint32_t mip_max_level,
                        bool signed_separate, WebGPUHostTextureInfo& info_out) {
  const WebGPUTextureHostFormat& host_format = WebGPUTextureFormatTable::GetHostFormat(
      table.Get(format, dimension, width, height), signed_separate);
  if (host_format.format == WGPUTextureFormat_Undefined ||
      host_format.load_shader == WebGPUTextureLoadShader::kUnknown) {
    return false;
  }
  bool is_3d = dimension == xenos::DataDimension::k3D;
  info_out.format = host_format.format;
  info_out.dimension = is_3d ? WGPUTextureDimension_3D : WGPUTextureDimension_2D;
  info_out.size = {width, height, depth_or_array_size};
  uint32_t max_size = std::max(width, height);
  if (is_3d) {
    max_size = std::max(max_size, depth_or_array_size);
  }
  info_out.mip_level_count = std::min(mip_max_level + 1, Log2Floor(max_size) + 1);
  uint32_t block_size;
  uint32_t bytes_per_block = GetTextureFormatBytesPerBlock(host_format.format, &block_size);
  uint64_t bytes = 0;
  for (uint32_t level = 0; level < info_out.mip_level_count; ++level) {
    uint64_t w = (std::max(width >> level, 1u) + block_size - 1) / block_size;
    uint64_t h = (std::max(height >> level, 1u) + block_size - 1) / block_size;
    uint64_t d = is_3d ? std::max(depth_or_array_size >> level, 1u) : depth_or_array_size;
    bytes += w * h * d * bytes_per_block;
  }
  info_out.memory_bytes = bytes;
  return true;
}

// ---------------------------------------------------------------------------
// Loader.
// ---------------------------------------------------------------------------

WebGPUTextureLoader::~WebGPUTextureLoader() {
  Shutdown();
}

bool WebGPUTextureLoader::Initialize(WebGPUDevice& device) {
  Shutdown();
  const WGPULimits& limits = device.limits();
  max_binding_size_ = limits.maxStorageBufferBindingSize;
  max_scratch_size_ = std::min<uint64_t>(
      {limits.maxStorageBufferBindingSize, limits.maxBufferSize, kScratchMaxSize});
  max_scratch_size_ &= ~(kRowPitchAlignment - 1);
  storage_offset_alignment_ = std::max<uint32_t>(limits.minStorageBufferOffsetAlignment, 4);

  std::vector<std::string> sources(static_cast<size_t>(Shader::kCount));
  for (size_t i = 1; i < size_t(Shader::kCount); ++i) {
    sources[i] = GetShaderSource(Shader(i));
  }
  uint32_t created = 0;
  device.RunSync([&]() {
    WGPUDevice d = device.device();
    WGPUBindGroupLayoutEntry entries[3] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
                                           WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
                                           WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Compute;
    entries[0].buffer.type = WGPUBufferBindingType_Storage;
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Compute;
    entries[1].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
    entries[2].binding = 2;
    entries[2].visibility = WGPUShaderStage_Compute;
    entries[2].buffer.type = WGPUBufferBindingType_Uniform;
    entries[2].buffer.hasDynamicOffset = 1;
    entries[2].buffer.minBindingSize = sizeof(LoadConstants);
    WGPUBindGroupLayoutDescriptor layout_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    layout_desc.label = MakeStringView("rex texture load");
    layout_desc.entryCount = 3;
    layout_desc.entries = entries;
    bind_group_layout_ = wgpuDeviceCreateBindGroupLayout(d, &layout_desc);
    WGPUPipelineLayoutDescriptor pipeline_layout_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pipeline_layout_desc.bindGroupLayoutCount = 1;
    pipeline_layout_desc.bindGroupLayouts = &bind_group_layout_;
    pipeline_layout_ = wgpuDeviceCreatePipelineLayout(d, &pipeline_layout_desc);
    for (size_t i = 1; i < size_t(Shader::kCount); ++i) {
      if (sources[i].empty()) {
        continue;
      }
      std::string label = std::string("rex texture load ") + kShaderInfo[i].name;
      WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
      wgsl.code = MakeStringView(sources[i]);
      WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
      module_desc.nextInChain = &wgsl.chain;
      module_desc.label = MakeStringView(label);
      WGPUShaderModule module = wgpuDeviceCreateShaderModule(d, &module_desc);
      if (!module) {
        continue;
      }
      WGPUComputePipelineDescriptor pipeline_desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
      pipeline_desc.label = MakeStringView(label);
      pipeline_desc.layout = pipeline_layout_;
      pipeline_desc.compute.module = module;
      pipeline_desc.compute.entryPoint = MakeStringView("main");
      pipelines_[i] = wgpuDeviceCreateComputePipeline(d, &pipeline_desc);
      wgpuShaderModuleRelease(module);
      if (pipelines_[i]) {
        ++created;
      }
    }
  });
  if (!bind_group_layout_ || !pipeline_layout_) {
    REXWGPU_ERROR("WebGPU texture loader: creating the pipeline layout failed");
    device_ = &device;
    Shutdown();
    return false;
  }
  device_ = &device;
  REXWGPU_INFO("WebGPU texture loader: {} load pipelines, scratch limit {} MiB", created,
               max_scratch_size_ >> 20);
  return true;
}

void WebGPUTextureLoader::Shutdown() {
  if (!device_) {
    return;
  }
  WebGPUDevice& device = *device_;
  device.RunSync([this]() {
    for (WGPUComputePipeline& pipeline : pipelines_) {
      if (pipeline) {
        wgpuComputePipelineRelease(pipeline);
        pipeline = nullptr;
      }
    }
    if (pipeline_layout_) {
      wgpuPipelineLayoutRelease(pipeline_layout_);
      pipeline_layout_ = nullptr;
    }
    if (bind_group_layout_) {
      wgpuBindGroupLayoutRelease(bind_group_layout_);
      bind_group_layout_ = nullptr;
    }
    for (Scratch& scratch : scratch_) {
      if (scratch.buffer) {
        wgpuBufferRelease(scratch.buffer);
        scratch.buffer = nullptr;
        scratch.size = 0;
      }
    }
  });
  device_ = nullptr;
}

bool WebGPUTextureLoader::EnsureScratch(WebGPUCommandStream& stream, Scratch& scratch,
                                        uint64_t size) {
  if (scratch.buffer && scratch.size >= size) {
    return true;
  }
  uint64_t new_size = std::max<uint64_t>(size, kScratchMinSize);
  if (scratch.size) {
    new_size = std::max(new_size, scratch.size * 2);
  }
  new_size = std::min(AlignUp<uint64_t>(new_size, kRowPitchAlignment), max_scratch_size_);
  if (new_size < size) {
    return false;
  }
  WGPUBuffer old_buffer = scratch.buffer;
  WGPUBuffer new_buffer = nullptr;
  device_->RunSync([&]() {
    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    desc.label = MakeStringView("rex texture load scratch");
    desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst;
    desc.size = new_size;
    new_buffer = wgpuDeviceCreateBuffer(device_->device(), &desc);
  });
  if (!new_buffer) {
    return false;
  }
  if (old_buffer) {
    // The current and earlier streams may reference it: release after this stream's submit.
    stream.AddSubmittedCallback(
        [](void* user, WebGPUReplayState&) { wgpuBufferRelease(static_cast<WGPUBuffer>(user)); },
        old_buffer);
  }
  scratch.buffer = new_buffer;
  scratch.size = new_size;
  return true;
}

bool WebGPUTextureLoader::RecordLoad(WebGPUCommandStream& stream,
                                     const WebGPUTextureFormatTable& table,
                                     const WebGPUTextureLoadDesc& desc, WGPUTexture texture) {
  if (!device_ || !texture || !desc.source || (!desc.load_base && !desc.load_mips) ||
      stream.in_pass()) {
    ++statistics_.failures;
    return false;
  }
  const WebGPUTextureHostFormatPair& pair =
      table.Get(desc.format, desc.dimension, desc.width, desc.height);
  const WebGPUTextureHostFormat& host_format =
      WebGPUTextureFormatTable::GetHostFormat(pair, desc.signed_separate);
  Shader load_shader = host_format.load_shader;
  if (load_shader == Shader::kUnknown || !pipelines_[size_t(load_shader)]) {
    ++statistics_.failures;
    return false;
  }
  WGPUComputePipeline load_pipeline = pipelines_[size_t(load_shader)];
  WGPUComputePipeline convert_pipeline = nullptr;
  const WebGPUTextureLoadShaderInfo* convert_info = nullptr;
  if (host_format.convert_shader != Shader::kUnknown) {
    convert_pipeline = pipelines_[size_t(host_format.convert_shader)];
    convert_info = &GetTextureLoadShaderInfo(host_format.convert_shader);
    if (!convert_pipeline) {
      ++statistics_.failures;
      return false;
    }
  }
  const WebGPUTextureLoadShaderInfo& load_info = GetTextureLoadShaderInfo(load_shader);

  texture_util::TextureGuestLayout computed_layout;
  const texture_util::TextureGuestLayout* guest_layout = desc.guest_layout;
  if (!guest_layout) {
    computed_layout = texture_util::GetGuestTextureLayout(
        desc.dimension, desc.pitch, desc.width, desc.height, desc.depth_or_array_size, desc.tiled,
        desc.format, desc.packed_mips, desc.has_base, desc.mip_max_level);
    guest_layout = &computed_layout;
  }

  // The rest follows VulkanTextureCache::LoadTextureDataFromResidentMemoryImpl (1x scale),
  // with 256-byte host row pitches (copyBufferToTexture) and block-row rowsPerImage.
  const bool is_3d = desc.dimension == xenos::DataDimension::k3D;
  const uint32_t width = desc.width;
  const uint32_t height = desc.height;
  const uint32_t depth = is_3d ? desc.depth_or_array_size : 1;
  const uint32_t array_size = is_3d ? 1 : desc.depth_or_array_size;
  const FormatInfo* guest_format_info = FormatInfo::Get(desc.format);
  const uint32_t block_width = guest_format_info->block_width;
  const uint32_t block_height = guest_format_info->block_height;
  const uint32_t bytes_per_block = guest_format_info->bytes_per_block();
  const uint32_t level_first = desc.load_base ? 0 : 1;
  const uint32_t level_last = desc.load_mips ? desc.mip_max_level : 0;
  if (level_first > level_last) {
    ++statistics_.failures;
    return false;
  }
  const uint32_t level_packed = guest_layout->packed_level;
  const uint32_t level_stored_first = std::min(level_first, level_packed);
  const uint32_t level_stored_last = std::min(level_last, level_packed);
  uint32_t loop_level_first, loop_level_last;
  if (level_packed == 0) {
    // Iteration 0: the base packed tail, iteration 1: the mips packed tail.
    loop_level_first = uint32_t(level_first != 0);
    loop_level_last = uint32_t(level_last != 0);
  } else {
    loop_level_first = level_stored_first;
    loop_level_last = level_stored_last;
  }

  const uint32_t host_block_width = host_format.block_compressed ? block_width : 1;
  const uint32_t host_block_height = host_format.block_compressed ? block_height : 1;
  uint32_t host_x_blocks_per_thread = UINT32_C(1) << load_info.guest_x_blocks_per_thread_log2;
  if (!host_format.block_compressed) {
    host_x_blocks_per_thread *= block_width;
  }
  if (convert_info) {
    host_x_blocks_per_thread = std::max(
        host_x_blocks_per_thread, UINT32_C(1) << convert_info->guest_x_blocks_per_thread_log2);
  }
  const uint32_t bytes_per_host_block = load_info.bytes_per_host_block;

  struct HostLayout {
    uint64_t offset = 0;
    uint64_t slice_size = 0;
    uint32_t row_bytes = 0;
    uint32_t x_pitch_blocks = 0;
    uint32_t y_pitch_blocks = 0;
    uint32_t batch = 0;
  };
  // Indexed by the loop level (see above).
  HostLayout host_layouts[xenos::kTextureMaxMips + 1];
  for (uint32_t loop_level = loop_level_first; loop_level <= loop_level_last; ++loop_level) {
    bool is_base = loop_level == 0;
    uint32_t level = level_packed == 0 ? 0 : loop_level;
    uint32_t x_texels, y_texels, z_texels;
    if (level == level_packed) {
      const texture_util::TextureGuestLayout::Level& packed =
          is_base ? guest_layout->base : guest_layout->mips[level];
      x_texels = packed.x_extent_blocks * block_width;
      y_texels = packed.y_extent_blocks * block_height;
      z_texels = packed.z_extent;
    } else {
      x_texels = std::max(width >> level, UINT32_C(1));
      y_texels = std::max(height >> level, UINT32_C(1));
      z_texels = std::max(depth >> level, UINT32_C(1));
    }
    HostLayout& host_layout = host_layouts[loop_level];
    uint32_t x_pitch_blocks =
        AlignUp((x_texels + host_block_width - 1) / host_block_width, host_x_blocks_per_thread);
    host_layout.row_bytes =
        uint32_t(AlignUp<uint64_t>(uint64_t(bytes_per_host_block) * x_pitch_blocks,
                                   kRowPitchAlignment));
    host_layout.x_pitch_blocks = host_layout.row_bytes / bytes_per_host_block;
    host_layout.y_pitch_blocks = (y_texels + host_block_height - 1) / host_block_height;
    host_layout.slice_size =
        uint64_t(host_layout.row_bytes) * host_layout.y_pitch_blocks * z_texels;
    if (host_layout.slice_size * array_size > max_scratch_size_) {
      REXWGPU_WARN(
          "WebGPU texture loader: {}x{}x{} {} level needs {} MiB of scratch, over the {} MiB "
          "limit",
          width, height, desc.depth_or_array_size, guest_format_info->name,
          (host_layout.slice_size * array_size) >> 20, max_scratch_size_ >> 20);
      ++statistics_.failures;
      return false;
    }
  }
  // Batches of loop levels that fit the scratch buffer together.
  uint32_t batch_count = 0;
  uint64_t batch_sizes[xenos::kTextureMaxMips + 1] = {};
  {
    uint64_t batch_size = 0;
    for (uint32_t loop_level = loop_level_first; loop_level <= loop_level_last; ++loop_level) {
      HostLayout& host_layout = host_layouts[loop_level];
      uint64_t part_size = host_layout.slice_size * array_size;
      uint64_t offset = AlignUp(batch_size, kRowPitchAlignment);
      if (batch_count == 0 || offset + part_size > max_scratch_size_) {
        if (batch_count) {
          batch_sizes[batch_count - 1] = batch_size;
        }
        ++batch_count;
        offset = 0;
      }
      host_layout.offset = offset;
      host_layout.batch = batch_count - 1;
      batch_size = offset + part_size;
    }
    batch_sizes[batch_count - 1] = batch_size;
  }

  // Guest source ranges.
  const uint64_t source_alignment = 16;
  uint64_t base_range = 0, mips_range = 0;
  if (level_first == 0) {
    base_range = AlignUp<uint64_t>(guest_layout->base.level_data_extent_bytes, source_alignment);
    if (!base_range || desc.base_offset % storage_offset_alignment_ ||
        desc.base_offset + base_range > desc.source_size || base_range > max_binding_size_) {
      ++statistics_.failures;
      return false;
    }
  }
  if (level_last != 0) {
    mips_range = AlignUp<uint64_t>(guest_layout->mips_total_extent_bytes, source_alignment);
    if (!mips_range || desc.mips_offset % storage_offset_alignment_ ||
        desc.mips_offset + mips_range > desc.source_size || mips_range > max_binding_size_) {
      ++statistics_.failures;
      return false;
    }
  }

  const uint32_t is_tiled_3d_endian_scale =
      uint32_t(desc.tiled) | (uint32_t(is_3d) << 1) | (uint32_t(desc.endianness) << 2) |
      (UINT32_C(1) << 4) | (UINT32_C(1) << 7);
  const uint32_t guest_x_blocks_per_group_log2 =
      kLoadGuestXThreadsPerGroupLog2 + load_info.guest_x_blocks_per_thread_log2;

  auto level_extent = [&](uint32_t loop_level, uint32_t& w, uint32_t& h, uint32_t& d) {
    bool is_base = loop_level == 0;
    uint32_t level = level_packed == 0 ? 0 : loop_level;
    if (level == level_packed) {
      const texture_util::TextureGuestLayout::Level& packed =
          is_base ? guest_layout->base : guest_layout->mips[level];
      w = packed.x_extent_blocks * block_width;
      h = packed.y_extent_blocks * block_height;
      d = packed.z_extent;
    } else {
      w = std::max(width >> level, UINT32_C(1));
      h = std::max(height >> level, UINT32_C(1));
      d = std::max(depth >> level, UINT32_C(1));
    }
  };

  WGPUBindGroupEntry entries[3] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
                                   WGPU_BIND_GROUP_ENTRY_INIT};
  entries[0].binding = 0;
  entries[1].binding = 1;
  entries[2].binding = 2;
  entries[2].buffer = StreamConstantsBuffer();
  entries[2].offset = 0;
  entries[2].size = sizeof(LoadConstants);

  for (uint32_t batch = 0; batch < batch_count; ++batch) {
    uint64_t batch_size = AlignUp(batch_sizes[batch], kRowPitchAlignment);
    if (!EnsureScratch(stream, scratch_[0], batch_size) ||
        (convert_pipeline && !EnsureScratch(stream, scratch_[1], batch_size))) {
      ++statistics_.failures;
      return false;
    }
    WGPUBuffer load_output = scratch_[0].buffer;
    WGPUBuffer copy_source = convert_pipeline ? scratch_[1].buffer : load_output;
    statistics_.scratch_bytes += batch_size;

    stream.CmdBeginComputePass();
    stream.CmdSetComputePipelineObject(load_pipeline);
    entries[0].buffer = load_output;
    entries[0].offset = 0;
    entries[0].size = batch_size;
    for (uint32_t loop_level = loop_level_first; loop_level <= loop_level_last; ++loop_level) {
      const HostLayout& host_layout = host_layouts[loop_level];
      if (host_layout.batch != batch) {
        continue;
      }
      bool is_base = loop_level == 0;
      uint32_t level = level_packed == 0 ? 0 : loop_level;
      const texture_util::TextureGuestLayout::Level& level_guest_layout =
          is_base ? guest_layout->base : guest_layout->mips[level];
      entries[1].buffer = desc.source;
      entries[1].offset = is_base ? desc.base_offset : desc.mips_offset;
      entries[1].size = is_base ? base_range : mips_range;

      LoadConstants constants = {};
      constants.is_tiled_3d_endian_scale = is_tiled_3d_endian_scale;
      constants.guest_offset = is_base ? 0 : guest_layout->mip_offsets_bytes[level];
      uint32_t guest_pitch = level_guest_layout.row_pitch_bytes;
      if (desc.tiled) {
        // Shaders take the pitch in blocks for tiled textures.
        guest_pitch /= bytes_per_block;
      }
      constants.guest_pitch_aligned = guest_pitch;
      constants.guest_z_stride_block_rows_aligned = level_guest_layout.z_slice_stride_block_rows;
      uint32_t level_width, level_height, level_depth;
      level_extent(loop_level, level_width, level_height, level_depth);
      constants.size_blocks[0] = (level_width + block_width - 1) / block_width;
      constants.size_blocks[1] = (level_height + block_height - 1) / block_height;
      constants.size_blocks[2] = level_depth;
      constants.height_texels = level_height;
      constants.host_offset = uint32_t(host_layout.offset);
      constants.host_pitch = host_layout.row_bytes;
      uint32_t group_count_x =
          (constants.size_blocks[0] + ((UINT32_C(1) << guest_x_blocks_per_group_log2) - 1)) >>
          guest_x_blocks_per_group_log2;
      uint32_t group_count_y =
          (constants.size_blocks[1] + ((UINT32_C(1) << kLoadGuestYBlocksPerGroupLog2) - 1)) >>
          kLoadGuestYBlocksPerGroupLog2;
      for (uint32_t slice = 0; slice < array_size; ++slice) {
        uint32_t constants_offset;
        void* constants_data = stream.AllocateConstants(sizeof(constants), constants_offset);
        std::memcpy(constants_data, &constants, sizeof(constants));
        stream.CmdSetBindGroup(0, bind_group_layout_, entries, 3, 1, &constants_offset);
        stream.CmdDispatch(group_count_x, group_count_y, constants.size_blocks[2]);
        ++statistics_.dispatches;
        constants.guest_offset += level_guest_layout.array_slice_stride_bytes;
        constants.host_offset += uint32_t(host_layout.slice_size);
      }
    }
    if (convert_pipeline) {
      // Normalized 16-bit intermediate -> float16, linear, same layout, second buffer (a
      // buffer can't be read-only and writable storage in one dispatch).
      stream.CmdSetComputePipelineObject(convert_pipeline);
      entries[0].buffer = scratch_[1].buffer;
      entries[0].offset = 0;
      entries[0].size = batch_size;
      entries[1].buffer = load_output;
      entries[1].offset = 0;
      entries[1].size = batch_size;
      const uint32_t convert_x_blocks_per_group_log2 =
          kLoadGuestXThreadsPerGroupLog2 + convert_info->guest_x_blocks_per_thread_log2;
      for (uint32_t loop_level = loop_level_first; loop_level <= loop_level_last;
           ++loop_level) {
        const HostLayout& host_layout = host_layouts[loop_level];
        if (host_layout.batch != batch) {
          continue;
        }
        LoadConstants constants = {};
        constants.is_tiled_3d_endian_scale =
            (uint32_t(is_3d) << 1) | (UINT32_C(1) << 4) | (UINT32_C(1) << 7);
        uint32_t level_width, level_height, level_depth;
        level_extent(loop_level, level_width, level_height, level_depth);
        constants.size_blocks[0] = (level_width + block_width - 1) / block_width;
        constants.size_blocks[1] = (level_height + block_height - 1) / block_height;
        constants.size_blocks[2] = level_depth;
        constants.height_texels = level_height;
        constants.guest_offset = uint32_t(host_layout.offset);
        constants.guest_pitch_aligned = host_layout.row_bytes;
        constants.guest_z_stride_block_rows_aligned = host_layout.y_pitch_blocks;
        constants.host_offset = uint32_t(host_layout.offset);
        constants.host_pitch = host_layout.row_bytes;
        uint32_t group_count_x =
            (constants.size_blocks[0] + ((UINT32_C(1) << convert_x_blocks_per_group_log2) - 1)) >>
            convert_x_blocks_per_group_log2;
        uint32_t group_count_y =
            (constants.size_blocks[1] + ((UINT32_C(1) << kLoadGuestYBlocksPerGroupLog2) - 1)) >>
            kLoadGuestYBlocksPerGroupLog2;
        for (uint32_t slice = 0; slice < array_size; ++slice) {
          uint32_t constants_offset;
          void* constants_data = stream.AllocateConstants(sizeof(constants), constants_offset);
          std::memcpy(constants_data, &constants, sizeof(constants));
          stream.CmdSetBindGroup(0, bind_group_layout_, entries, 3, 1, &constants_offset);
          stream.CmdDispatch(group_count_x, group_count_y, constants.size_blocks[2]);
          ++statistics_.dispatches;
          constants.guest_offset += uint32_t(host_layout.slice_size);
          constants.host_offset += uint32_t(host_layout.slice_size);
        }
      }
    }
    stream.CmdEndComputePass();

    // Copies of the levels whose data is in this batch.
    for (uint32_t level = level_first; level <= level_last; ++level) {
      uint32_t loop_level = level_packed == 0 ? uint32_t(level != 0) : std::min(level, level_packed);
      const HostLayout& host_layout = host_layouts[loop_level];
      if (host_layout.batch != batch) {
        continue;
      }
      uint32_t level_width = std::max(width >> level, UINT32_C(1));
      uint32_t level_height = std::max(height >> level, UINT32_C(1));
      uint32_t level_depth = std::max(depth >> level, UINT32_C(1));
      uint64_t offset = host_layout.offset;
      if (level >= level_packed) {
        uint32_t offset_blocks_x, offset_blocks_y, offset_z;
        texture_util::GetPackedMipOffset(width, height, depth, desc.format, level,
                                         offset_blocks_x, offset_blocks_y, offset_z);
        uint64_t offset_host_x = offset_blocks_x;
        uint64_t offset_host_y = offset_blocks_y;
        if (!host_format.block_compressed) {
          offset_host_x *= block_width;
          offset_host_y *= block_height;
        }
        offset += uint64_t(bytes_per_host_block) * offset_host_x +
                  uint64_t(host_layout.row_bytes) *
                      (offset_host_y + uint64_t(host_layout.y_pitch_blocks) * offset_z);
      }
      WGPUTexelCopyBufferInfo source = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
      source.buffer = copy_source;
      source.layout.offset = offset;
      source.layout.bytesPerRow = host_layout.row_bytes;
      source.layout.rowsPerImage = host_layout.y_pitch_blocks;
      WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
      destination.texture = texture;
      destination.mipLevel = level;
      destination.aspect = WGPUTextureAspect_All;
      WGPUExtent3D copy_size;
      copy_size.width = level_width;
      copy_size.height = level_height;
      copy_size.depthOrArrayLayers = is_3d ? level_depth : array_size;
      if (host_format.block_compressed) {
        // Whole blocks (the physical size of small mips).
        copy_size.width = AlignUp(copy_size.width, host_block_width);
        copy_size.height = AlignUp(copy_size.height, host_block_height);
      }
      stream.CmdCopyBufferToTexture(source, destination, copy_size);
      ++statistics_.copies;
    }
  }
  ++statistics_.loads;
  return true;
}

// ---------------------------------------------------------------------------
// Samplers.
// ---------------------------------------------------------------------------

xenos::ClampMode NormalizeWebGPUClampMode(xenos::ClampMode clamp_mode) {
  switch (clamp_mode) {
    case xenos::ClampMode::kRepeat:
    case xenos::ClampMode::kMirroredRepeat:
    case xenos::ClampMode::kClampToEdge:
      return clamp_mode;
    case xenos::ClampMode::kClampToHalfway:
    case xenos::ClampMode::kClampToBorder:
      return xenos::ClampMode::kClampToEdge;
    case xenos::ClampMode::kMirrorClampToEdge:
    case xenos::ClampMode::kMirrorClampToHalfway:
    case xenos::ClampMode::kMirrorClampToBorder:
    default:
      return xenos::ClampMode::kMirroredRepeat;
  }
}

void MakeSamplerDescriptor(WebGPUSamplerParameters parameters, WGPUSamplerDescriptor& desc) {
  auto address_mode = [](xenos::ClampMode mode) {
    switch (NormalizeWebGPUClampMode(mode)) {
      case xenos::ClampMode::kRepeat:
        return WGPUAddressMode_Repeat;
      case xenos::ClampMode::kMirroredRepeat:
        return WGPUAddressMode_MirrorRepeat;
      default:
        return WGPUAddressMode_ClampToEdge;
    }
  };
  desc.addressModeU = address_mode(parameters.clamp_x);
  desc.addressModeV = address_mode(parameters.clamp_y);
  desc.addressModeW = address_mode(parameters.clamp_z);
  desc.magFilter = parameters.mag_linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
  desc.minFilter = parameters.min_linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
  desc.mipmapFilter =
      parameters.mip_linear ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
  // LOD bias is applied in shaders.
  desc.lodMinClamp = float(parameters.mip_min_level);
  // Base map mip filter: stay on mip_min_level (as on Vulkan).
  desc.lodMaxClamp = parameters.mip_base_map ? desc.lodMinClamp + 0.25f : 32.0f;
  desc.compare = WGPUCompareFunction_Undefined;
  desc.maxAnisotropy = 1;
  if (parameters.aniso_filter != xenos::AnisoFilter::kDisabled &&
      parameters.aniso_filter != xenos::AnisoFilter::kUseFetchConst && parameters.mag_linear &&
      parameters.min_linear && parameters.mip_linear) {
    uint32_t shift = uint32_t(parameters.aniso_filter) - uint32_t(xenos::AnisoFilter::kMax_1_1);
    desc.maxAnisotropy = uint16_t(std::min(UINT32_C(1) << shift, UINT32_C(16)));
  }
}

}  // namespace rex::graphics::webgpu
