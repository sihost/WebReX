/**
 * @file        graphics/webgpu/webgpu_edram_shaders.h
 * @brief       Hand-written WGSL for the WebGPU EDRAM emulation (WP4): render target
 *              dump (RT -> EDRAM buffer), load/clear (EDRAM buffer or a constant ->
 *              RT) and resolve (EDRAM buffer -> shared memory).
 *
 * docs/port/webgpu.md sections 5 and 6. These are fixed shaders, so they are
 * written directly in WGSL instead of being converted from SPIR-V: the
 * Vulkan dump/transfer shaders are generated at runtime with spv::Builder and
 * the resolve shaders are compiled HLSL, neither of which WebGPU can take.
 *
 * EDRAM buffer layout (the one the Vulkan/D3D12 dump and resolve shaders use,
 * render_target_cache.cpp GetDumpPipeline): 2048 tiles of 5120 bytes. A 32bpp
 * tile is 80x16 samples of one dword, a 64bpp tile 40x16 samples of two
 * dwords, row-major within the tile. Depth tiles have their two 40-sample
 * halves swapped. Element index = (tile & 2047) * tile_w * 16 + y * tile_w + x.
 * Guest samples: 2x MSAA = 1x2 samples per pixel (0 top, 1 bottom), 4x = 2x2
 * (bit 0 horizontal, bit 1 vertical). Host 2x is emulated with 4x (host
 * samples 0/1 = guest top, 2/3 = guest bottom; dumps read host 0 and 3).
 *
 * Bit patterns per guest format (xenos::ColorRenderTargetFormat values):
 *  0/1 8_8_8_8(_GAMMA): R in bits 0:7; 2/10 2_10_10_10: R 0:9 .. A 30:31;
 *  3/12 2_10_10_10_FLOAT: 7e3 RGB, 2-bit unorm A; 4/5 16_16(_16_16): snorm
 *  -32..32 (0x8001 = -32); 6/7 16_16(_16_16)_FLOAT: half; 14/15 32(_32)_FLOAT.
 *  Depth: (depth24 << 8) | stencil, D24S8 unorm24, D24FS8 20e4. Host D24FS8
 *  depth is guest * 0.5 (the Vulkan convention: [0, 2) -> [0, 1)).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

namespace rex::graphics::webgpu::edram_wgsl {

// Shared by every module.
inline constexpr char kPrelude[] = R"(
diagnostic(off, derivative_uniformity);

const kTileCount : u32 = 2048u;

fn xe_is_64bpp(format : u32) -> bool {
  return format == 5u || format == 7u || format == 15u;
}

// Element index in the EDRAM buffer (in tile_w-sample units: dwords for 32bpp,
// dword pairs for 64bpp).
fn xe_edram_index(tile_non_wrapped : u32, x : u32, y : u32, tile_w : u32, is_depth : bool) -> u32 {
  var sx = x;
  if (is_depth) {
    let half = tile_w >> 1u;
    if (sx < half) {
      sx = sx + half;
    } else {
      sx = sx - half;
    }
  }
  return (tile_non_wrapped & (kTileCount - 1u)) * (tile_w * 16u) + y * tile_w + sx;
}

fn xe_f7e3_to_f32(v : u32) -> f32 {
  let e = (v >> 7u) & 7u;
  let m = v & 0x7Fu;
  if (e == 0u) {
    return ldexp(f32(m), -9);
  }
  return bitcast<f32>(((e + 124u) << 23u) | (m << 16u));
}

fn xe_f32_to_7e3(f : f32) -> u32 {
  var u = bitcast<u32>(f);
  // Negative (including -0) and NaN -> 0, clamp to 31.875.
  if ((u & 0x80000000u) != 0u || u > 0x7F800000u) {
    return 0u;
  }
  u = min(u, 0x41FF0000u);
  var b : u32;
  if (u < 0x3E800000u) {
    b = ((u & 0x7FFFFFu) | 0x800000u) >> min(125u - (u >> 23u), 24u);
  } else {
    b = u - 0x3E000000u;
  }
  return ((b + 0x7FFFu + ((b >> 16u) & 1u)) >> 16u) & 0x3FFu;
}

fn xe_f20e4_to_f32(v : u32) -> f32 {
  let e = (v >> 20u) & 0xFu;
  let m = v & 0xFFFFFu;
  if (e == 0u) {
    return ldexp(f32(m), -34);
  }
  return bitcast<f32>(((e + 112u) << 23u) | (m << 3u));
}

// Truncating (xenia's default, depth_float24_round off). Input in [0, 2).
fn xe_f32_to_20e4(f : f32) -> u32 {
  var u = bitcast<u32>(f);
  if ((u & 0x80000000u) != 0u || u > 0x7F800000u) {
    return 0u;
  }
  u = min(u, 0x3FFFFFF8u);
  var b : u32;
  if (u < 0x38800000u) {
    b = ((u & 0x7FFFFFu) | 0x800000u) >> min(113u - (u >> 23u), 24u);
  } else {
    b = u - 0x38000000u;
  }
  return (b >> 3u) & 0xFFFFFFu;
}

fn xe_snorm16_32_to_f32(v : u32) -> f32 {
  let s = i32(v << 16u) >> 16u;
  return max(f32(s), -32767.0) * (32.0 / 32767.0);
}

fn xe_f32_to_snorm16_32(f : f32) -> u32 {
  let c = clamp(f * (32767.0 / 32.0), -32767.0, 32767.0);
  return u32(i32(round(c))) & 0xFFFFu;
}

fn xe_unorm(f : f32, scale : f32) -> u32 {
  return u32(saturate(f) * scale + 0.5);
}

// EDRAM color bits -> the value stored in the host render target.
fn xe_decode_color(format : u32, d : vec2<u32>) -> vec4<f32> {
  switch (format) {
    case 0u, 1u: {
      return unpack4x8unorm(d.x);
    }
    case 2u, 10u: {
      return vec4<f32>(f32(d.x & 0x3FFu) / 1023.0, f32((d.x >> 10u) & 0x3FFu) / 1023.0,
                       f32((d.x >> 20u) & 0x3FFu) / 1023.0, f32(d.x >> 30u) / 3.0);
    }
    case 3u, 12u: {
      return vec4<f32>(xe_f7e3_to_f32(d.x & 0x3FFu), xe_f7e3_to_f32((d.x >> 10u) & 0x3FFu),
                       xe_f7e3_to_f32((d.x >> 20u) & 0x3FFu), f32(d.x >> 30u) / 3.0);
    }
    case 4u: {
      return vec4<f32>(xe_snorm16_32_to_f32(d.x & 0xFFFFu), xe_snorm16_32_to_f32(d.x >> 16u), 0.0,
                       0.0);
    }
    case 5u: {
      return vec4<f32>(xe_snorm16_32_to_f32(d.x & 0xFFFFu), xe_snorm16_32_to_f32(d.x >> 16u),
                       xe_snorm16_32_to_f32(d.y & 0xFFFFu), xe_snorm16_32_to_f32(d.y >> 16u));
    }
    case 6u: {
      return vec4<f32>(unpack2x16float(d.x), 0.0, 0.0);
    }
    case 7u: {
      return vec4<f32>(unpack2x16float(d.x), unpack2x16float(d.y));
    }
    case 14u: {
      return vec4<f32>(bitcast<f32>(d.x), 0.0, 0.0, 0.0);
    }
    case 15u: {
      return vec4<f32>(bitcast<f32>(d.x), bitcast<f32>(d.y), 0.0, 0.0);
    }
    default: {
      return vec4<f32>(0.0);
    }
  }
}

// Host render target value -> EDRAM color bits.
fn xe_encode_color(format : u32, c : vec4<f32>) -> vec2<u32> {
  switch (format) {
    case 0u, 1u: {
      return vec2<u32>(xe_unorm(c.x, 255.0) | (xe_unorm(c.y, 255.0) << 8u) |
                       (xe_unorm(c.z, 255.0) << 16u) | (xe_unorm(c.w, 255.0) << 24u), 0u);
    }
    case 2u, 10u: {
      return vec2<u32>(xe_unorm(c.x, 1023.0) | (xe_unorm(c.y, 1023.0) << 10u) |
                       (xe_unorm(c.z, 1023.0) << 20u) | (xe_unorm(c.w, 3.0) << 30u), 0u);
    }
    case 3u, 12u: {
      return vec2<u32>(xe_f32_to_7e3(c.x) | (xe_f32_to_7e3(c.y) << 10u) |
                       (xe_f32_to_7e3(c.z) << 20u) | (xe_unorm(c.w, 3.0) << 30u), 0u);
    }
    case 4u: {
      return vec2<u32>(xe_f32_to_snorm16_32(c.x) | (xe_f32_to_snorm16_32(c.y) << 16u), 0u);
    }
    case 5u: {
      return vec2<u32>(xe_f32_to_snorm16_32(c.x) | (xe_f32_to_snorm16_32(c.y) << 16u),
                       xe_f32_to_snorm16_32(c.z) | (xe_f32_to_snorm16_32(c.w) << 16u));
    }
    case 6u: {
      return vec2<u32>(pack2x16float(c.xy), 0u);
    }
    case 7u: {
      return vec2<u32>(pack2x16float(c.xy), pack2x16float(c.zw));
    }
    case 14u: {
      return vec2<u32>(bitcast<u32>(c.x), 0u);
    }
    case 15u: {
      return vec2<u32>(bitcast<u32>(c.x), bitcast<u32>(c.y));
    }
    default: {
      return vec2<u32>(0u);
    }
  }
}

// unorm24 <-> float32 without the f32 division (2.5 ULP in WGSL) and without
// the rounding of d * (2^24 - 1): with 24-bit values both would be off by 1-2
// units. u / (2^24 - 1) = u * 2^-24 + u * 2^-48 + ..., where both products are
// exact and the sum is correctly rounded; d * 2^24 is exact, so
// d * 2^24 - d is within 0.5 of u for that d.
fn xe_unorm24_to_f32(u : u32) -> f32 {
  let f = f32(u);
  return f * 5.9604644775390625e-8 + f * 3.552713678800501e-15;
}

fn xe_f32_to_unorm24(d : f32) -> u32 {
  let s = saturate(d);
  return min(u32(round(s * 16777216.0 - s)), 0xFFFFFFu);
}

// Host depth (+ stencil) -> EDRAM depth dword. format: 0 D24S8, 1 D24FS8.
fn xe_encode_depth(format : u32, depth : f32, stencil : u32) -> u32 {
  var d24 : u32;
  if (format == 1u) {
    d24 = xe_f32_to_20e4(depth * 2.0);
  } else {
    d24 = xe_f32_to_unorm24(depth);
  }
  return (d24 << 8u) | (stencil & 0xFFu);
}

fn xe_decode_depth(format : u32, d : u32) -> f32 {
  let d24 = d >> 8u;
  if (format == 1u) {
    return xe_f20e4_to_f32(d24) * 0.5;
  }
  return xe_unorm24_to_f32(d24);
}
)";

// ---------------------------------------------------------------------------
// Dump: host render target -> EDRAM buffer (compute, 8x16 samples per group).
// Four variants: {color, depth} x {1 sample, multisampled}, selected by
// prepending one of the kDumpSource* blocks.
// ---------------------------------------------------------------------------

inline constexpr char kDumpSourceColor1x[] = R"(
const kDumpIsDepth : bool = false;
@group(0) @binding(2) var xe_source : texture_2d<f32>;
fn xe_load_color(p : vec2<i32>, s : u32) -> vec4<f32> { return textureLoad(xe_source, p, 0); }
fn xe_load_depth(p : vec2<i32>, s : u32) -> f32 { return 0.0; }
fn xe_load_stencil(p : vec2<i32>, s : u32) -> u32 { return 0u; }
)";

inline constexpr char kDumpSourceColorMs[] = R"(
const kDumpIsDepth : bool = false;
@group(0) @binding(2) var xe_source : texture_multisampled_2d<f32>;
fn xe_load_color(p : vec2<i32>, s : u32) -> vec4<f32> { return textureLoad(xe_source, p, i32(s)); }
fn xe_load_depth(p : vec2<i32>, s : u32) -> f32 { return 0.0; }
fn xe_load_stencil(p : vec2<i32>, s : u32) -> u32 { return 0u; }
)";

inline constexpr char kDumpSourceDepth1x[] = R"(
const kDumpIsDepth : bool = true;
@group(0) @binding(2) var xe_source : texture_depth_2d;
@group(0) @binding(3) var xe_source_stencil : texture_2d<u32>;
fn xe_load_color(p : vec2<i32>, s : u32) -> vec4<f32> { return vec4<f32>(0.0); }
fn xe_load_depth(p : vec2<i32>, s : u32) -> f32 { return textureLoad(xe_source, p, 0); }
fn xe_load_stencil(p : vec2<i32>, s : u32) -> u32 { return textureLoad(xe_source_stencil, p, 0).x; }
)";

inline constexpr char kDumpSourceDepthMs[] = R"(
const kDumpIsDepth : bool = true;
@group(0) @binding(2) var xe_source : texture_depth_multisampled_2d;
@group(0) @binding(3) var xe_source_stencil : texture_multisampled_2d<u32>;
fn xe_load_color(p : vec2<i32>, s : u32) -> vec4<f32> { return vec4<f32>(0.0); }
fn xe_load_depth(p : vec2<i32>, s : u32) -> f32 { return textureLoad(xe_source, p, i32(s)); }
fn xe_load_stencil(p : vec2<i32>, s : u32) -> u32 {
  return textureLoad(xe_source_stencil, p, i32(s)).x;
}
)";

inline constexpr char kDumpMain[] = R"(
struct DumpParams {
  first_tile : u32,
  dispatch_pitch_tiles : u32,
  source_base_tiles : u32,
  source_pitch_tiles : u32,
  format : u32,
  msaa_samples : u32,
  width_samples : u32,
  height_samples : u32,
};
@group(0) @binding(0) var<storage, read_write> xe_edram : array<u32>;
@group(0) @binding(1) var<uniform> xe_dump : DumpParams;

@compute @workgroup_size(8, 16, 1)
fn main(@builtin(global_invocation_id) id : vec3<u32>) {
  if (id.x >= xe_dump.width_samples || id.y >= xe_dump.height_samples) {
    return;
  }
  let wide = !kDumpIsDepth && xe_is_64bpp(xe_dump.format);
  let tile_w = select(80u, 40u, wide);
  let tile_x = id.x / tile_w;
  let sample_x = id.x % tile_w;
  let tile_y = id.y >> 4u;
  let sample_y = id.y & 15u;
  // Not wrapped yet, so the tile relative to the source base is not negative.
  let edram_tile = xe_dump.first_tile + xe_dump.dispatch_pitch_tiles * tile_y + tile_x;
  let index = xe_edram_index(edram_tile, sample_x, sample_y, tile_w, kDumpIsDepth);
  let source_tile = edram_tile - xe_dump.source_base_tiles;
  let ssx = (source_tile % xe_dump.source_pitch_tiles) * tile_w + sample_x;
  let ssy = (source_tile / xe_dump.source_pitch_tiles) * 16u + sample_y;
  var px = ssx;
  var py = ssy;
  var s = 0u;
  if (xe_dump.msaa_samples >= 1u) {
    py = ssy >> 1u;
    if (xe_dump.msaa_samples >= 2u) {
      px = ssx >> 1u;
      s = (ssx & 1u) | ((ssy & 1u) << 1u);
    } else {
      s = select(0u, 3u, (ssy & 1u) != 0u);
    }
  }
  let p = vec2<i32>(i32(px), i32(py));
  if (kDumpIsDepth) {
    xe_edram[index] = xe_encode_depth(xe_dump.format, xe_load_depth(p, s), xe_load_stencil(p, s));
    return;
  }
  let bits = xe_encode_color(xe_dump.format, xe_load_color(p, s));
  if (wide) {
    xe_edram[index * 2u] = bits.x;
    xe_edram[index * 2u + 1u] = bits.y;
  } else {
    xe_edram[index] = bits.x;
  }
}
)";

// ---------------------------------------------------------------------------
// Load / clear: EDRAM buffer (or a constant) -> host render target. A
// fullscreen triangle per scissored rectangle. fs_color for color targets;
// fs_depth writes depth and the pipeline replaces stencil with the reference;
// fs_stencil_bit (override kStencilBit, stencil write mask 1 << bit,
// reference 0xFF) sets one stencil bit where it is 1 in the source, since
// WebGPU has no stencil export.
// ---------------------------------------------------------------------------

inline constexpr char kLoadMain[] = R"(
struct LoadParams {
  base_tiles : u32,
  pitch_tiles : u32,
  format : u32,
  msaa_samples : u32,
  from_constant : u32,
  constant_lo : u32,
  constant_hi : u32,
  pad : u32,
};
@group(0) @binding(0) var<storage, read> xe_edram : array<u32>;
@group(0) @binding(1) var<uniform> xe_load : LoadParams;
override kStencilBit : u32 = 0u;

@vertex
fn vs(@builtin(vertex_index) i : u32) -> @builtin(position) vec4<f32> {
  let uv = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u));
  return vec4<f32>(uv * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0), 0.0, 1.0);
}

fn xe_fetch(pos : vec4<f32>, s : u32, is_depth : bool) -> vec2<u32> {
  if (xe_load.from_constant != 0u) {
    return vec2<u32>(xe_load.constant_lo, xe_load.constant_hi);
  }
  let wide = !is_depth && xe_is_64bpp(xe_load.format);
  let tile_w = select(80u, 40u, wide);
  var sx = u32(pos.x);
  var sy = u32(pos.y);
  if (xe_load.msaa_samples >= 1u) {
    sy = (sy << 1u) | (s >> 1u);
    if (xe_load.msaa_samples >= 2u) {
      sx = (sx << 1u) | (s & 1u);
    }
  }
  let tile = xe_load.base_tiles + (sy >> 4u) * xe_load.pitch_tiles + sx / tile_w;
  let index = xe_edram_index(tile, sx % tile_w, sy & 15u, tile_w, is_depth);
  if (wide) {
    return vec2<u32>(xe_edram[index * 2u], xe_edram[index * 2u + 1u]);
  }
  return vec2<u32>(xe_edram[index], 0u);
}

@fragment
fn fs_color(@builtin(position) pos : vec4<f32>, @builtin(sample_index) s : u32)
    -> @location(0) vec4<f32> {
  return xe_decode_color(xe_load.format, xe_fetch(pos, s, false));
}

struct DepthOutput {
  @builtin(frag_depth) depth : f32,
};

@fragment
fn fs_depth(@builtin(position) pos : vec4<f32>, @builtin(sample_index) s : u32) -> DepthOutput {
  var output : DepthOutput;
  output.depth = xe_decode_depth(xe_load.format, xe_fetch(pos, s, true).x);
  return output;
}

@fragment
fn fs_stencil_bit(@builtin(position) pos : vec4<f32>, @builtin(sample_index) s : u32) {
  let d = xe_fetch(pos, s, true).x;
  if (((d >> kStencilBit) & 1u) == 0u) {
    discard;
  }
}
)";

// ---------------------------------------------------------------------------
// Resolve: EDRAM buffer -> shared memory (compute, 8x8 invocations per group).
// One invocation writes one dword-aligned group of pixels: 4 at 8bpp, 2 at
// 16bpp, else 1. raw != 0: bit copy of the selected sample (the "fast" resolve
// shaders: depth, or bitwise-equivalent formats); else unpack, sample
// average, exponent bias, red/blue swap and pack (the "full" shaders). Then
// the 128-bit endian swap and the Xenos 2D/3D tiled address.
// ---------------------------------------------------------------------------

inline constexpr char kResolveMain[] = R"(
struct ResolveParams {
  edram_base_tiles : u32,
  edram_pitch_tiles : u32,
  edram_msaa_samples : u32,
  edram_is_depth : u32,
  edram_format : u32,
  edram_is_64bpp : u32,
  offset_x : u32,
  offset_y : u32,
  width : u32,
  height : u32,
  dest_pitch : u32,
  dest_height : u32,
  dest_offset_x : u32,
  dest_offset_y : u32,
  dest_array : u32,
  dest_slice : u32,
  dest_format : u32,
  dest_endian : u32,
  dest_exp_bias : i32,
  dest_swap : u32,
  sample_select : u32,
  raw : u32,
  dest_bpp_log2 : u32,
  dest_base : i32,
};
@group(0) @binding(0) var<storage, read> xe_edram : array<u32>;
@group(0) @binding(1) var<storage, read_write> xe_dest : array<u32>;
@group(0) @binding(2) var<uniform> xe_resolve : ResolveParams;

fn xe_source_index(x : u32, y : u32, s : u32, tile_w : u32) -> u32 {
  var sx = xe_resolve.offset_x + x;
  var sy = xe_resolve.offset_y + y;
  if (xe_resolve.edram_msaa_samples >= 1u) {
    if (xe_resolve.edram_msaa_samples >= 2u) {
      sx = (sx << 1u) | (s & 1u);
      sy = (sy << 1u) | ((s >> 1u) & 1u);
    } else {
      sy = (sy << 1u) | (s & 1u);
    }
  }
  let tile = xe_resolve.edram_base_tiles + (sy >> 4u) * xe_resolve.edram_pitch_tiles + sx / tile_w;
  return xe_edram_index(tile, sx % tile_w, sy & 15u, tile_w, xe_resolve.edram_is_depth != 0u);
}

fn xe_load_source(x : u32, y : u32, s : u32, tile_w : u32) -> vec2<u32> {
  let index = xe_source_index(x, y, s, tile_w);
  if (xe_resolve.edram_is_64bpp != 0u) {
    return vec2<u32>(xe_edram[index * 2u], xe_edram[index * 2u + 1u]);
  }
  return vec2<u32>(xe_edram[index], 0u);
}

fn xe_load_color(x : u32, y : u32, tile_w : u32) -> vec4<f32> {
  var first = xe_resolve.sample_select;
  var count = 1u;
  if (xe_resolve.sample_select == 4u) {
    first = 0u;
    count = 2u;
  } else if (xe_resolve.sample_select == 5u) {
    first = 2u;
    count = 2u;
  } else if (xe_resolve.sample_select == 6u) {
    first = 0u;
    count = 4u;
  }
  var sum = vec4<f32>(0.0);
  for (var i = 0u; i < count; i = i + 1u) {
    sum = sum + xe_decode_color(xe_resolve.edram_format, xe_load_source(x, y, first + i, tile_w));
  }
  return sum / f32(count);
}

fn xe_pack(format : u32, c : vec4<f32>) -> vec4<u32> {
  switch (format) {
    case 2u, 8u, 9u: {
      return vec4<u32>(xe_unorm(c.x, 255.0), 0u, 0u, 0u);
    }
    case 3u: {
      return vec4<u32>(xe_unorm(c.x, 31.0) | (xe_unorm(c.y, 31.0) << 5u) |
                       (xe_unorm(c.z, 31.0) << 10u) | (xe_unorm(c.w, 1.0) << 15u), 0u, 0u, 0u);
    }
    case 4u: {
      return vec4<u32>(xe_unorm(c.x, 31.0) | (xe_unorm(c.y, 63.0) << 5u) |
                       (xe_unorm(c.z, 31.0) << 11u), 0u, 0u, 0u);
    }
    case 5u: {
      return vec4<u32>(xe_unorm(c.x, 31.0) | (xe_unorm(c.y, 31.0) << 5u) |
                       (xe_unorm(c.z, 63.0) << 10u), 0u, 0u, 0u);
    }
    case 6u, 14u, 50u: {
      return vec4<u32>(xe_unorm(c.x, 255.0) | (xe_unorm(c.y, 255.0) << 8u) |
                       (xe_unorm(c.z, 255.0) << 16u) | (xe_unorm(c.w, 255.0) << 24u), 0u, 0u, 0u);
    }
    case 7u, 54u: {
      return vec4<u32>(xe_unorm(c.x, 1023.0) | (xe_unorm(c.y, 1023.0) << 10u) |
                       (xe_unorm(c.z, 1023.0) << 20u) | (xe_unorm(c.w, 3.0) << 30u), 0u, 0u, 0u);
    }
    case 10u: {
      return vec4<u32>(xe_unorm(c.x, 255.0) | (xe_unorm(c.y, 255.0) << 8u), 0u, 0u, 0u);
    }
    case 15u: {
      return vec4<u32>(xe_unorm(c.x, 15.0) | (xe_unorm(c.y, 15.0) << 4u) |
                       (xe_unorm(c.z, 15.0) << 8u) | (xe_unorm(c.w, 15.0) << 12u), 0u, 0u, 0u);
    }
    case 16u, 55u: {
      return vec4<u32>(xe_unorm(c.x, 2047.0) | (xe_unorm(c.y, 2047.0) << 11u) |
                       (xe_unorm(c.z, 1023.0) << 22u), 0u, 0u, 0u);
    }
    case 17u, 56u: {
      return vec4<u32>(xe_unorm(c.x, 1023.0) | (xe_unorm(c.y, 2047.0) << 10u) |
                       (xe_unorm(c.z, 2047.0) << 21u), 0u, 0u, 0u);
    }
    case 24u: {
      return vec4<u32>(xe_unorm(c.x, 65535.0), 0u, 0u, 0u);
    }
    case 25u: {
      return vec4<u32>(xe_unorm(c.x, 65535.0) | (xe_unorm(c.y, 65535.0) << 16u), 0u, 0u, 0u);
    }
    case 26u: {
      return vec4<u32>(xe_unorm(c.x, 65535.0) | (xe_unorm(c.y, 65535.0) << 16u),
                       xe_unorm(c.z, 65535.0) | (xe_unorm(c.w, 65535.0) << 16u), 0u, 0u);
    }
    case 30u: {
      return vec4<u32>(pack2x16float(vec2<f32>(c.x, 0.0)) & 0xFFFFu, 0u, 0u, 0u);
    }
    case 31u: {
      return vec4<u32>(pack2x16float(c.xy), 0u, 0u, 0u);
    }
    case 32u: {
      return vec4<u32>(pack2x16float(c.xy), pack2x16float(c.zw), 0u, 0u);
    }
    case 36u: {
      return vec4<u32>(bitcast<u32>(c.x), 0u, 0u, 0u);
    }
    case 37u: {
      return vec4<u32>(bitcast<u32>(c.x), bitcast<u32>(c.y), 0u, 0u);
    }
    case 38u: {
      return bitcast<vec4<u32>>(c);
    }
    default: {
      return vec4<u32>(0u);
    }
  }
}

fn xe_swap_8in32(v : vec4<u32>) -> vec4<u32> {
  return ((v & vec4<u32>(0xFFu)) << vec4<u32>(24u)) | ((v & vec4<u32>(0xFF00u)) << vec4<u32>(8u)) |
         ((v >> vec4<u32>(8u)) & vec4<u32>(0xFF00u)) | (v >> vec4<u32>(24u));
}

fn xe_endian_swap(v : vec4<u32>, endian : u32) -> vec4<u32> {
  switch (endian) {
    case 1u: {
      return ((v & vec4<u32>(0x00FF00FFu)) << vec4<u32>(8u)) |
             ((v & vec4<u32>(0xFF00FF00u)) >> vec4<u32>(8u));
    }
    case 2u: {
      return xe_swap_8in32(v);
    }
    case 3u: {
      return (v << vec4<u32>(16u)) | (v >> vec4<u32>(16u));
    }
    case 4u: {
      return xe_swap_8in32(v).yxwz;
    }
    case 5u: {
      return xe_swap_8in32(v).wzyx;
    }
    default: {
      return v;
    }
  }
}

// texture_util::GetTiledOffset2D.
fn xe_tiled_offset_2d(x : i32, y : i32, pitch_in : u32, bpb_log2 : u32) -> i32 {
  let pitch = i32((pitch_in + 31u) & ~31u);
  let mac = ((x >> 5u) + (y >> 5u) * (pitch >> 5u)) << (bpb_log2 + 7u);
  let micro = ((x & 7) + ((y & 0xE) << 2u)) << bpb_log2;
  let offset = mac + ((micro & ~0xF) << 1u) + (micro & 0xF) + ((y & 1) << 4u);
  return ((offset & ~0x1FF) << 3u) + ((y & 16) << 7u) + ((offset & 0x1C0) << 2u) +
         (((((y & 8) >> 2u) + (x >> 3u)) & 3) << 6u) + (offset & 0x3F);
}

// texture_util::GetTiledOffset3D.
fn xe_tiled_offset_3d(x : i32, y : i32, z : i32, pitch_in : u32, height_in : u32,
                      bpb_log2 : u32) -> i32 {
  let pitch = i32((pitch_in + 31u) & ~31u);
  let height = i32((height_in + 31u) & ~31u);
  let macro_outer = ((y >> 4u) + (z >> 2u) * (height >> 4u)) * (pitch >> 5u);
  let mac = ((((x >> 5u) + macro_outer) << (bpb_log2 + 6u)) & 0xFFFFFFF) << 1u;
  let micro = (((x & 7) + ((y & 6) << 2u)) << (bpb_log2 + 6u)) >> 6u;
  let offset_outer = ((y >> 3u) + (z >> 2u)) & 1;
  let offset1 = offset_outer + ((((x >> 3u) + (offset_outer << 1u)) & 3) << 1u);
  let offset2 = ((mac + (micro & ~15)) << 1u) + (micro & 15) + ((z & 3) << (bpb_log2 + 6u)) +
                ((y & 1) << 4u);
  var address = (offset1 & 1) << 3u;
  address = address + ((offset2 >> 6u) & 7);
  address = address << 3u;
  address = address + (offset1 & ~1);
  address = address << 2u;
  address = address + (offset2 & ~511);
  address = address << 3u;
  address = address + (offset2 & 63);
  return address;
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) id : vec3<u32>) {
  let bpp_log2 = xe_resolve.dest_bpp_log2;
  let ppi_log2 = select(0u, 2u - bpp_log2, bpp_log2 < 2u);
  let x0 = id.x << ppi_log2;
  let y = id.y;
  if (x0 >= xe_resolve.width || y >= xe_resolve.height) {
    return;
  }
  let tile_w = select(80u, 40u, xe_resolve.edram_is_64bpp != 0u);
  var words = vec4<u32>(0u);
  if (xe_resolve.raw != 0u) {
    words = vec4<u32>(xe_load_source(x0, y, xe_resolve.sample_select, tile_w), 0u, 0u);
    if (xe_resolve.dest_swap != 0u) {
      // Red/blue swap of the bitwise-equivalent formats, as xenia's
      // resolve_fast_32bpp/64bpp shaders (by the EDRAM format): 8_8_8_8(_GAMMA)
      // bytes 0 <-> 2, 2_10_10_10 (incl. FLOAT / AS_10_10_10_10 / AS_16_16_16_16)
      // bits 0-9 <-> 20-29, 16_16_16_16(_FLOAT) the low halves of the two words.
      let f = xe_resolve.edram_format;
      if (xe_resolve.edram_is_64bpp != 0u) {
        if (f == 5u || f == 7u) {
          words = vec4<u32>((words.x & 0xFFFF0000u) | (words.y & 0xFFFFu),
                            (words.y & 0xFFFF0000u) | (words.x & 0xFFFFu), 0u, 0u);
        }
      } else if (f == 0u || f == 1u) {
        words.x = (words.x & 0xFF00FF00u) | ((words.x & 0xFFu) << 16u) | ((words.x >> 16u) & 0xFFu);
      } else if (f == 2u || f == 3u || f == 10u || f == 12u) {
        words.x = (words.x & 0xC00FFC00u) | ((words.x & 0x3FFu) << 20u) |
                  ((words.x >> 20u) & 0x3FFu);
      }
    }
  } else {
    let scale = ldexp(1.0, xe_resolve.dest_exp_bias);
    for (var k = 0u; k < (1u << ppi_log2); k = k + 1u) {
      var c = xe_load_color(x0 + k, y, tile_w) * scale;
      if (xe_resolve.dest_swap != 0u) {
        c = c.zyxw;
      }
      let p = xe_pack(xe_resolve.dest_format, c);
      if (bpp_log2 < 2u) {
        words.x = words.x | (p.x << (k << (3u + bpp_log2)));
      } else {
        words = p;
      }
    }
  }
  words = xe_endian_swap(words, xe_resolve.dest_endian);
  let dx = i32(xe_resolve.dest_offset_x + x0);
  let dy = i32(xe_resolve.dest_offset_y + y);
  var address : i32;
  if (xe_resolve.dest_array != 0u) {
    address = xe_resolve.dest_base +
              xe_tiled_offset_3d(dx, dy, i32(xe_resolve.dest_slice), xe_resolve.dest_pitch,
                                 xe_resolve.dest_height, bpp_log2);
  } else {
    address = xe_resolve.dest_base + xe_tiled_offset_2d(dx, dy, xe_resolve.dest_pitch, bpp_log2);
  }
  if (address < 0) {
    return;
  }
  let dword = u32(address) >> 2u;
  let dword_count = max((1u << bpp_log2) >> 2u, 1u);
  let dest_dwords = arrayLength(&xe_dest);
  for (var i = 0u; i < dword_count; i = i + 1u) {
    if (dword + i < dest_dwords) {
      xe_dest[dword + i] = words[i];
    }
  }
}
)";

}  // namespace rex::graphics::webgpu::edram_wgsl
