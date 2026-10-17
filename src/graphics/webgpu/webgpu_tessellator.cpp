/**
 * @file        graphics/webgpu/webgpu_tessellator.cpp
 * @brief       See rex/graphics/webgpu/webgpu_tessellator.h.
 *
 * The WGSL tessellator below ports CHWTessellator (TessellateTriDomain,
 * TessellateQuadDomain and their helpers) of the Direct3D 11 reference
 * tessellator:
 *
 *   Copyright (c) Microsoft Corporation
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to
 *   deal in the Software without restriction, including without limitation the
 *   rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *   sell copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions: The above
 *   copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED
 *   "AS IS", WITHOUT WARRANTY OF ANY KIND.
 *
 * Only the integer and fractional_even partitionings (what the Xenos modes map
 * to) and triangle_cw output are kept.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/webgpu_tessellator.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/webgpu/webgpu_command_processor.h>
#include <rex/graphics/webgpu/webgpu_device.h>
#include <rex/graphics/webgpu/webgpu_log.h>
#include <rex/graphics/webgpu/webgpu_primitive_processor.h>
#include <rex/graphics/webgpu/webgpu_shared_memory.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(webgpu_tessellation, true, "GPU",
                    "WebGPU: draw tessellated (patch / VGT_OUTPUT_PATH tessellation) draws "
                    "through the compute tessellator (WebGPUTessellator). Off: they are "
                    "dropped, as before gate 7");
REXCVAR_DEFINE_INT32(webgpu_tessellation_log_draws, 8, "GPU",
                     "WebGPU: log the parameters of the first N tessellated draws");

namespace rex::graphics::webgpu {

namespace {

// Matches the WGSL Params struct.
struct Params {
  uint32_t patch_count;
  uint32_t flags;
  uint32_t index_first;
  uint32_t index_endian;
  uint32_t vertex_base_index;
  uint32_t vertex_index_min;
  uint32_t vertex_index_max;
  uint32_t slot_points;
  uint32_t slot_indices;
  float factor_min;
  float factor_max;
  uint32_t pad;
};
static_assert(sizeof(Params) == 48);

constexpr uint32_t kFlagQuad = 1;
constexpr uint32_t kFlagInteger = 2;
constexpr uint32_t kFlagAdaptive = 4;
constexpr uint32_t kFlagCPIndexed = 8;
constexpr uint32_t kFlagIndexed = 16;
constexpr uint32_t kFlagIndex32 = 32;

constexpr uint32_t kWorkgroupSize = 64;

const char kTessellatorWgsl[] = R"(
struct Params {
  patch_count : u32,
  flags : u32,
  index_first : u32,
  index_endian : u32,
  vertex_base_index : u32,
  vertex_index_min : u32,
  vertex_index_max : u32,
  slot_points : u32,
  slot_indices : u32,
  factor_min : f32,
  factor_max : f32,
  pad : u32,
}

const kFlagQuad : u32 = 1u;
const kFlagInteger : u32 = 2u;
const kFlagAdaptive : u32 = 4u;
const kFlagCPIndexed : u32 = 8u;
const kFlagIndexed : u32 = 16u;
const kFlagIndex32 : u32 = 32u;

@group(0) @binding(0) var<uniform> params : Params;
@group(0) @binding(1) var<storage, read> guest_indices : array<u32>;
@group(0) @binding(2) var<storage, read_write> points : array<vec4<f32>>;
@group(0) @binding(3) var<storage, read_write> out_indices : array<u32>;

const FXP_ONE : i32 = 65536;
const FXP_ONE_HALF : i32 = 32768;
const FXP_ONE_THIRD : i32 = 0x5555;
const FXP_TWO_THIRDS : i32 = 0xAAAA;

const DIAGONALS_INSIDE_TO_OUTSIDE : i32 = 0;
const DIAGONALS_INSIDE_TO_OUTSIDE_EXCEPT_MIDDLE : i32 = 1;
const DIAGONALS_MIRRORED : i32 = 2;

var<private> kFixedReciprocal : array<i32, 65> = array<i32, 65>(
  -1, 0x10000, 0x8000, 0x5555, 0x4000, 0x3333, 0x2aab, 0x2492, 0x2000, 0x1c72, 0x199a, 0x1746,
  0x1555, 0x13b1, 0x1249, 0x1111, 0x1000, 0xf0f, 0xe39, 0xd79, 0xccd, 0xc31, 0xba3, 0xb21, 0xaab,
  0xa3d, 0x9d9, 0x97b, 0x925, 0x8d4, 0x889, 0x842, 0x800, 0x7c2, 0x788, 0x750, 0x71c, 0x6eb,
  0x6bd, 0x690, 0x666, 0x63e, 0x618, 0x5f4, 0x5d1, 0x5b0, 0x591, 0x572, 0x555, 0x539, 0x51f,
  0x505, 0x4ec, 0x4d5, 0x4be, 0x4a8, 0x492, 0x47e, 0x46a, 0x457, 0x444, 0x432, 0x421, 0x410,
  0x400);
var<private> kFinalPointPosition : array<i32, 33> = array<i32, 33>(
  0, 32, 16, 8, 17, 4, 18, 9, 19, 2, 20, 10, 21, 5, 22, 11, 23, 1, 24, 12, 25, 6, 26, 13, 27, 3,
  28, 14, 29, 7, 30, 15, 31);
var<private> kLoopStart : array<i32, 33> = array<i32, 33>(
  1, 1, 17, 9, 9, 5, 5, 5, 5, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
  2, 2);
var<private> kLoopEnd : array<i32, 33> = array<i32, 33>(
  0, 0, 17, 17, 25, 25, 25, 25, 29, 29, 29, 29, 29, 29, 29, 29, 31, 31, 31, 31, 31, 31, 31, 31,
  31, 31, 31, 31, 31, 31, 31, 31, 32);

struct Ctx {
  frac : i32,
  half_points : i32,
  split : i32,
  inv_floor : i32,
  inv_ceil : i32,
}

// Output of the current patch.
var<private> g_point_base : u32;
var<private> g_index_base : u32;
var<private> g_patch_input : vec4<f32>;
var<private> g_quad : bool;

// Index patching (CHWTessellator::PatchIndexValue).
var<private> g_patched : bool;
var<private> g_patched2 : bool;
var<private> p_in_delta : i32;
var<private> p_in_bad : i32;
var<private> p_in_repl : i32;
var<private> p_out_base : i32;
var<private> p_out_delta : i32;
var<private> p_out_bad : i32;
var<private> p_out_repl : i32;
var<private> p2_base : i32;
var<private> p2_bad : i32;
var<private> p2_repl : i32;
var<private> p2_end : i32;

fn fxp_ceil(x : i32) -> i32 {
  if ((x & 0xFFFF) != 0) {
    return (x & 0x7FFF0000) + FXP_ONE;
  }
  return x;
}

fn fxp_floor(x : i32) -> i32 {
  return x & 0x7FFF0000;
}

fn remove_msb(v : i32) -> i32 {
  if (v <= 0) {
    return 0;
  }
  return v & ~(1 << firstLeadingBit(u32(v)));
}

// floatToIDotF<15, 16, unsigned>: round to nearest even.
fn float_to_fixed(x : f32) -> i32 {
  let bits = bitcast<i32>(x);
  if (bits < 0 || x != x) {
    return 0;
  }
  let exponent = ((bits >> 23u) & 0xFF) - 127;
  if (exponent >= 15) {
    return -1;
  }
  if (exponent < -17) {
    return 0;
  }
  var m = (bits & 0x7FFFFF) | 0x800000;
  let extra = 7 - exponent;
  if (extra >= 0) {
    let lsb = 1 << u32(extra);
    let mask = lsb - 1;
    let round_bias = lsb >> 1u;
    if ((m & lsb) != 0 || (m & mask) > round_bias) {
      m += round_bias;
    }
    m = m >> u32(extra);
  } else {
    m = m << u32(-extra);
  }
  return m;
}

fn fixed_to_float(x : i32) -> f32 {
  return f32(x >> 16u) + f32(x & 0xFFFF) / 65536.0;
}

fn is_even(x : f32) -> bool {
  return (i32(x) & 1) == 0;
}

// tess_fmin(upper, tess_fmax(lower, x)): NaN becomes lower.
fn tess_clamp(x : f32, lower : f32, upper : f32) -> f32 {
  var r = x;
  if (!(x > lower)) {
    r = lower;
  }
  if (upper < r) {
    r = upper;
  }
  return r;
}

fn compute_ctx(fxp_factor : i32, odd : bool) -> Ctx {
  var ctx : Ctx;
  var half_factor = (fxp_factor + 1) / 2;
  if (odd || half_factor == FXP_ONE_HALF) {
    half_factor += FXP_ONE_HALF;
  }
  let fl = fxp_floor(half_factor);
  let ce = fxp_ceil(half_factor);
  ctx.frac = half_factor - fl;
  ctx.half_points = ce >> 16u;
  if (ce == fl) {
    ctx.split = ctx.half_points + 1;
  } else if (odd) {
    if (fl == FXP_ONE) {
      ctx.split = 0;
    } else {
      ctx.split = (remove_msb((fl >> 16u) - 1) << 1u) + 1;
    }
  } else {
    ctx.split = (remove_msb(fl >> 16u) << 1u) + 1;
  }
  var floor_segments = (fl * 2) >> 16u;
  var ceil_segments = (ce * 2) >> 16u;
  if (odd) {
    floor_segments -= 1;
    ceil_segments -= 1;
  }
  ctx.inv_floor = kFixedReciprocal[clamp(floor_segments, 0, 64)];
  ctx.inv_ceil = kFixedReciprocal[clamp(ceil_segments, 0, 64)];
  return ctx;
}

fn num_points_for(fxp_factor : i32, odd : bool) -> i32 {
  if (odd) {
    return (fxp_ceil(FXP_ONE_HALF + (fxp_factor + 1) / 2) * 2) >> 16u;
  }
  return ((fxp_ceil((fxp_factor + 1) / 2) * 2) >> 16u) + 1;
}

fn place_point(ctx : Ctx, point_in : i32, odd : bool) -> i32 {
  var pt = point_in;
  var flip = false;
  if (pt >= ctx.half_points) {
    pt = (ctx.half_points << 1u) - pt;
    if (odd) {
      pt -= 1;
    }
    flip = true;
  }
  if (pt == ctx.half_points) {
    return FXP_ONE_HALF;
  }
  let index_ceil = u32(pt);
  var index_floor = index_ceil;
  if (pt > ctx.split) {
    index_floor -= 1u;
  }
  let on_floor = index_floor * u32(ctx.inv_floor);
  let on_ceil = index_ceil * u32(ctx.inv_ceil);
  var fxp = on_floor * u32(FXP_ONE - ctx.frac) + on_ceil * u32(ctx.frac);
  fxp = (fxp + u32(FXP_ONE_HALF)) >> 16u;
  var placed = i32(fxp);
  if (flip) {
    placed = FXP_ONE - placed;
  }
  return placed;
}

fn patch_index_value(index_in : i32) -> i32 {
  var index = index_in;
  if (g_patched) {
    if (index >= p_out_base) {
      if (index == p_out_bad) {
        index = p_out_repl;
      } else {
        index += p_out_delta;
      }
    } else {
      if (index == p_in_bad) {
        index = p_in_repl;
      } else {
        index += p_in_delta;
      }
    }
  } else if (g_patched2) {
    if (index >= p2_base) {
      if (index == p2_bad) {
        index = p2_repl;
      } else {
        index = p2_end - index;
      }
    } else if (index == p2_bad) {
      index = p2_repl;
    }
  }
  return index;
}

fn define_point(u : i32, v : i32, offset : i32) {
  if (offset < 0 || u32(offset) >= params.slot_points) {
    return;
  }
  let uf = fixed_to_float(u);
  let vf = fixed_to_float(v);
  var domain = vec4<f32>(uf, vf, 0.0, 0.0);
  if (!g_quad) {
    domain.z = 1.0 - uf - vf;
  }
  let slot = (g_point_base + u32(offset)) * 2u;
  points[slot] = domain;
  points[slot + 1u] = g_patch_input;
}

fn define_index(index : i32, offset : i32) {
  let patched = patch_index_value(index);
  if (offset < 0 || u32(offset) >= params.slot_indices) {
    return;
  }
  var value = g_point_base;
  if (patched >= 0 && u32(patched) < params.slot_points) {
    value = g_point_base + u32(patched);
  }
  out_indices[g_index_base + u32(offset)] = value;
}

fn define_cw_triangle(i0 : i32, i1 : i32, i2 : i32, base : i32) {
  define_index(i0, base);
  define_index(i1, base + 1);
  define_index(i2, base + 2);
}

fn stitch_regular(trapezoid : bool, diagonals : i32, base_in : i32, num_inside : i32,
                  inside_base : i32, outside_base : i32) {
  var base = base_in;
  var ip = inside_base;
  var op = outside_base;
  if (trapezoid) {
    define_cw_triangle(op, op + 1, ip, base);
    base += 3;
    op += 1;
  }
  var p = 0;
  if (diagonals == DIAGONALS_INSIDE_TO_OUTSIDE) {
    for (p = 0; p < num_inside - 1; p++) {
      define_cw_triangle(ip, op, op + 1, base);
      base += 3;
      define_cw_triangle(ip, op + 1, ip + 1, base);
      base += 3;
      ip += 1;
      op += 1;
    }
  } else if (diagonals == DIAGONALS_INSIDE_TO_OUTSIDE_EXCEPT_MIDDLE) {
    for (p = 0; p < num_inside / 2 - 1; p++) {
      define_cw_triangle(op, op + 1, ip, base);
      base += 3;
      define_cw_triangle(ip, op + 1, ip + 1, base);
      base += 3;
      ip += 1;
      op += 1;
    }
    define_cw_triangle(op, ip + 1, ip, base);
    base += 3;
    define_cw_triangle(op, op + 1, ip + 1, base);
    base += 3;
    ip += 1;
    op += 1;
    p += 2;
    for (; p < num_inside; p++) {
      define_cw_triangle(op, op + 1, ip, base);
      base += 3;
      define_cw_triangle(ip, op + 1, ip + 1, base);
      base += 3;
      ip += 1;
      op += 1;
    }
  } else {
    for (p = 0; p < num_inside / 2; p++) {
      define_cw_triangle(op, ip + 1, ip, base);
      base += 3;
      define_cw_triangle(op, op + 1, ip + 1, base);
      base += 3;
      ip += 1;
      op += 1;
    }
    for (; p < num_inside - 1; p++) {
      define_cw_triangle(ip, op, op + 1, base);
      base += 3;
      define_cw_triangle(ip, op + 1, ip + 1, base);
      base += 3;
      ip += 1;
      op += 1;
    }
  }
  if (trapezoid) {
    define_cw_triangle(op, op + 1, ip, base);
  }
}

fn stitch_transition(base_in : i32, inside_base : i32, inside_half_in : i32, inside_odd : bool,
                     outside_base : i32, outside_half_in : i32, outside_odd : bool) {
  var base = base_in;
  var inside_half = inside_half_in;
  var outside_half = outside_half_in;
  if (inside_odd) {
    inside_half -= 1;
  }
  if (outside_odd) {
    outside_half -= 1;
  }
  inside_half = clamp(inside_half, 0, 32);
  outside_half = clamp(outside_half, 0, 32);
  var op = outside_base;
  var ip = inside_base;
  let i_start = min(kLoopStart[inside_half], kLoopStart[outside_half]);
  let i_end = max(kLoopEnd[inside_half], kLoopEnd[outside_half]);
  if (kFinalPointPosition[0] < outside_half) {
    define_cw_triangle(op, op + 1, ip, base);
    base += 3;
    op += 1;
  }
  for (var i = i_start; i <= i_end; i++) {
    if (kFinalPointPosition[i] < inside_half) {
      define_cw_triangle(ip, op, ip + 1, base);
      base += 3;
      ip += 1;
    }
    if (kFinalPointPosition[i] < outside_half) {
      define_cw_triangle(op, op + 1, ip, base);
      base += 3;
      op += 1;
    }
  }
  if (inside_odd != outside_odd || inside_odd) {
    if (inside_odd == outside_odd) {
      define_cw_triangle(ip, op, ip + 1, base);
      base += 3;
      define_cw_triangle(ip + 1, op, op + 1, base);
      base += 3;
      ip += 1;
      op += 1;
    } else if (!inside_odd) {
      define_cw_triangle(ip, op, op + 1, base);
      base += 3;
      op += 1;
    } else {
      define_cw_triangle(ip, op, ip + 1, base);
      base += 3;
      ip += 1;
    }
  }
  for (var i = i_end; i >= i_start; i--) {
    if (kFinalPointPosition[i] < outside_half) {
      define_cw_triangle(op, op + 1, ip, base);
      base += 3;
      op += 1;
    }
    if (kFinalPointPosition[i] < inside_half) {
      define_cw_triangle(ip, op, ip + 1, base);
      base += 3;
      ip += 1;
    }
  }
  if (kFinalPointPosition[0] < outside_half) {
    define_cw_triangle(op, op + 1, ip, base);
  }
}

// CHWTessellator::TessellateTriDomain (edges U=0, V=0, W=0, inside).
fn tessellate_tri(f_u0 : f32, f_v0 : f32, f_w0 : f32, f_inside : f32, integer : bool) {
  if (!(f_u0 > 0.0) || !(f_v0 > 0.0) || !(f_w0 > 0.0)) {
    return;
  }
  let lower = select(2.0, 1.0, integer);
  let upper = 64.0;
  var factors = array<f32, 3>(tess_clamp(f_u0, lower, upper), tess_clamp(f_v0, lower, upper),
                              tess_clamp(f_w0, lower, upper));
  var factor_inside = tess_clamp(f_inside, lower, upper);
  var odd_out = array<bool, 3>(false, false, false);
  var odd_in = false;
  if (integer) {
    for (var e = 0; e < 3; e++) {
      factors[e] = ceil(factors[e]);
      odd_out[e] = !is_even(factors[e]);
    }
    factor_inside = ceil(factor_inside);
    odd_in = !(is_even(factor_inside) || factor_inside == 1.0);
  }
  var fxp_out : array<i32, 3>;
  for (var e = 0; e < 3; e++) {
    fxp_out[e] = float_to_fixed(factors[e]);
  }
  let fxp_in = float_to_fixed(factor_inside);
  if (integer && fxp_in == FXP_ONE && fxp_out[0] == FXP_ONE && fxp_out[1] == FXP_ONE &&
      fxp_out[2] == FXP_ONE) {
    define_point(0, FXP_ONE, 0);
    define_point(0, 0, 1);
    define_point(FXP_ONE, 0, 2);
    define_cw_triangle(0, 1, 2, 0);
    return;
  }
  var ctx_out : array<Ctx, 3>;
  var points_out : array<i32, 3>;
  var num_points = 0;
  for (var e = 0; e < 3; e++) {
    ctx_out[e] = compute_ctx(fxp_out[e], odd_out[e]);
    points_out[e] = num_points_for(fxp_out[e], odd_out[e]);
    num_points += points_out[e];
  }
  num_points -= 3;
  let ctx_in = compute_ctx(fxp_in, odd_in);
  let points_in = max(select(3, 4, odd_in), num_points_for(fxp_in, odd_in));
  let inside_edge_point_base_offset = num_points;

  // TriGeneratePoints.
  var point_offset = 0;
  for (var e = 0; e < 3; e++) {
    let parity = (e & 1) != 0;
    let end_point = points_out[e] - 1;
    for (var p = 0; p < end_point; p++) {
      let q = select(end_point - p, p, parity);
      let param = place_point(ctx_out[e], q, odd_out[e]);
      if (e == 0) {
        define_point(0, param, point_offset);
      } else {
        define_point(param, select(0, FXP_ONE - param, e == 2), point_offset);
      }
      point_offset += 1;
    }
  }
  let num_rings = points_in >> 1u;
  for (var ring = 1; ring < num_rings; ring++) {
    let start_point = ring;
    let end_point = points_in - 1 - start_point;
    for (var e = 0; e < 3; e++) {
      let parity = (e & 1) != 0;
      var perp = place_point(ctx_in, start_point, odd_in);
      perp = (perp * FXP_TWO_THIRDS + FXP_ONE_HALF) >> 16u;
      let d = (perp + 1) / 2;
      for (var p = start_point; p < end_point; p++) {
        let q = select(end_point - (p - start_point), p, parity);
        let param = place_point(ctx_in, q, odd_in);
        if (e == 0) {
          define_point(perp, param - d, point_offset);
        } else if (e == 1) {
          define_point(param - d, perp, point_offset);
        } else {
          define_point(param - d, FXP_ONE - (param - d) - perp, point_offset);
        }
        point_offset += 1;
      }
    }
  }
  if (!odd_in) {
    define_point(FXP_ONE_THIRD, FXP_ONE_THIRD, point_offset);
  }

  // TriGenerateConnectivity.
  let rings = (points_in + 1) >> 1u;
  var outside_half = array<i32, 3>(ctx_out[0].half_points, ctx_out[1].half_points,
                                   ctx_out[2].half_points);
  var outside_odd = odd_out;
  var outside_points = points_out;
  var inside_edge_point_base = inside_edge_point_base_offset;
  var outside_edge_point_base = 0;
  var num_indices = 0;
  for (var ring = 1; ring < rings; ring++) {
    let inside_edge_points = points_in - 2 * ring;
    let edge0_inside_base = inside_edge_point_base;
    let edge0_outside_base = outside_edge_point_base;
    for (var e = 0; e < 3; e++) {
      let num_triangles = inside_edge_points + outside_points[e] - 2;
      var inside_base = inside_edge_point_base;
      var outside_base = outside_edge_point_base;
      if (e == 2) {
        p_in_delta = inside_edge_point_base;
        p_in_bad = inside_edge_points - 1;
        p_in_repl = edge0_inside_base;
        p_out_base = p_in_bad + 1;
        p_out_delta = outside_edge_point_base - p_out_base;
        p_out_bad = p_out_base + outside_points[e] - 1;
        p_out_repl = edge0_outside_base;
        g_patched = true;
        inside_base = 0;
        outside_base = p_out_base;
      }
      if (ring == 1) {
        stitch_transition(num_indices, inside_base, ctx_in.half_points, odd_in, outside_base,
                          outside_half[e], outside_odd[e]);
      } else {
        stitch_regular(true, DIAGONALS_MIRRORED, num_indices, inside_edge_points, inside_base,
                       outside_base);
      }
      g_patched = false;
      num_indices += num_triangles * 3;
      outside_edge_point_base += outside_points[e] - 1;
      inside_edge_point_base += inside_edge_points - 1;
      outside_points[e] = inside_edge_points;
    }
    if (ring == 1) {
      for (var e = 0; e < 3; e++) {
        outside_half[e] = ctx_in.half_points;
        outside_odd[e] = odd_in;
      }
    }
  }
  if (odd_in) {
    define_cw_triangle(outside_edge_point_base, outside_edge_point_base + 1,
                       outside_edge_point_base + 2, num_indices);
  }
}

// CHWTessellator::TessellateQuadDomain (edges U=0, V=0, U=1, V=1, inside U, V).
fn tessellate_quad(f_u0 : f32, f_v0 : f32, f_u1 : f32, f_v1 : f32, f_inside_u : f32,
                   f_inside_v : f32, integer : bool) {
  if (!(f_u0 > 0.0) || !(f_v0 > 0.0) || !(f_u1 > 0.0) || !(f_v1 > 0.0)) {
    return;
  }
  let lower = select(2.0, 1.0, integer);
  let upper = 64.0;
  var factors = array<f32, 4>(tess_clamp(f_u0, lower, upper), tess_clamp(f_v0, lower, upper),
                              tess_clamp(f_u1, lower, upper), tess_clamp(f_v1, lower, upper));
  var factors_in = array<f32, 2>(tess_clamp(f_inside_u, lower, upper),
                                 tess_clamp(f_inside_v, lower, upper));
  var odd_out = array<bool, 4>(false, false, false, false);
  var odd_in = array<bool, 2>(false, false);
  if (integer) {
    for (var e = 0; e < 4; e++) {
      factors[e] = ceil(factors[e]);
      odd_out[e] = !is_even(factors[e]);
    }
    for (var a = 0; a < 2; a++) {
      factors_in[a] = ceil(factors_in[a]);
      odd_in[a] = !(is_even(factors_in[a]) || factors_in[a] == 1.0);
    }
  }
  var fxp_out : array<i32, 4>;
  for (var e = 0; e < 4; e++) {
    fxp_out[e] = float_to_fixed(factors[e]);
  }
  var fxp_in = array<i32, 2>(float_to_fixed(factors_in[0]), float_to_fixed(factors_in[1]));
  if (integer && fxp_in[0] == FXP_ONE && fxp_in[1] == FXP_ONE && fxp_out[0] == FXP_ONE &&
      fxp_out[1] == FXP_ONE && fxp_out[2] == FXP_ONE && fxp_out[3] == FXP_ONE) {
    define_point(0, 0, 0);
    define_point(FXP_ONE, 0, 1);
    define_point(FXP_ONE, FXP_ONE, 2);
    define_point(0, FXP_ONE, 3);
    define_cw_triangle(0, 1, 3, 0);
    define_cw_triangle(1, 2, 3, 3);
    return;
  }
  var ctx_out : array<Ctx, 4>;
  var points_out : array<i32, 4>;
  var num_points = 0;
  for (var e = 0; e < 4; e++) {
    ctx_out[e] = compute_ctx(fxp_out[e], odd_out[e]);
    points_out[e] = num_points_for(fxp_out[e], odd_out[e]);
    num_points += points_out[e];
  }
  num_points -= 4;
  var ctx_in = array<Ctx, 2>(compute_ctx(fxp_in[0], odd_in[0]), compute_ctx(fxp_in[1], odd_in[1]));
  var points_in : array<i32, 2>;
  for (var a = 0; a < 2; a++) {
    points_in[a] = max(select(3, 4, odd_in[a]), num_points_for(fxp_in[a], odd_in[a]));
  }
  let inside_edge_point_base_offset = num_points;

  // QuadGeneratePoints.
  var point_offset = 0;
  for (var e = 0; e < 4; e++) {
    let parity = (e & 1) != 0;
    let end_point = points_out[e] - 1;
    for (var p = 0; p < end_point; p++) {
      let q = select(end_point - p, p, e == 1 || e == 2);
      let param = place_point(ctx_out[e], q, odd_out[e]);
      if (parity) {
        define_point(param, select(0, FXP_ONE, e == 3), point_offset);
      } else {
        define_point(select(0, FXP_ONE, e == 2), param, point_offset);
      }
      point_offset += 1;
    }
  }
  let num_rings = min(points_in[0], points_in[1]) >> 1u;
  for (var ring = 1; ring < num_rings; ring++) {
    let start_point = ring;
    var end_point = array<i32, 2>(points_in[0] - 1 - start_point, points_in[1] - 1 - start_point);
    for (var e = 0; e < 4; e++) {
      let parity0 = e & 1;
      let parity1 = (e + 1) & 1;
      let perp_point = select(end_point[parity0], start_point, e < 2);
      let perp = place_point(ctx_in[parity0], perp_point, odd_in[parity0]);
      for (var p = start_point; p < end_point[parity1]; p++) {
        let q = select(end_point[parity1] - (p - start_point), p, e == 1 || e == 2);
        let param = place_point(ctx_in[parity1], q, odd_in[parity1]);
        if (parity1 != 0) {
          define_point(perp, param, point_offset);
        } else {
          define_point(param, perp, point_offset);
        }
        point_offset += 1;
      }
    }
  }
  if (points_in[0] > points_in[1] && !odd_in[1]) {
    let start_point = num_rings;
    let end_point = points_in[0] - 1 - start_point;
    for (var p = start_point; p <= end_point; p++) {
      let param = place_point(ctx_in[0], p, odd_in[0]);
      define_point(param, FXP_ONE_HALF, point_offset);
      point_offset += 1;
    }
  } else if (points_in[1] >= points_in[0] && !odd_in[0]) {
    let start_point = num_rings;
    let end_point = points_in[1] - 1 - start_point;
    for (var p = end_point; p >= start_point; p--) {
      let param = place_point(ctx_in[1], p, odd_in[1]);
      define_point(FXP_ONE_HALF, param, point_offset);
      point_offset += 1;
    }
  }

  // QuadGenerateConnectivity.
  var rows_to_center = array<i32, 2>((points_in[0] + 1) >> 1u, (points_in[1] + 1) >> 1u);
  let rings = min(rows_to_center[0], rows_to_center[1]);
  var degenerate_ring = array<i32, 2>(select(-1, rows_to_center[1] - 1, !odd_in[1]),
                                      select(-1, rows_to_center[0] - 1, !odd_in[0]));
  var outside_half = array<i32, 4>(ctx_out[0].half_points, ctx_out[1].half_points,
                                   ctx_out[2].half_points, ctx_out[3].half_points);
  var outside_odd = odd_out;
  var outside_points = points_out;
  var inside_edge_point_base = inside_edge_point_base_offset;
  var outside_edge_point_base = 0;
  var num_indices = 0;
  for (var ring = 1; ring < rings; ring++) {
    var inside_edge_points = array<i32, 2>(points_in[0] - 2 * ring, points_in[1] - 2 * ring);
    let edge0_inside_base = inside_edge_point_base;
    let edge0_outside_base = outside_edge_point_base;
    for (var e = 0; e < 4; e++) {
      let parity = (e + 1) & 1;
      let num_triangles = inside_edge_points[parity] + outside_points[e] - 2;
      var inside_base = inside_edge_point_base;
      var outside_base = outside_edge_point_base;
      if (e == 3) {
        if (ring == degenerate_ring[parity]) {
          p2_base = inside_edge_point_base + 1;
          p2_bad = outside_edge_point_base + outside_points[e] - 1;
          p2_repl = edge0_outside_base;
          p2_end = (p2_base << 1u) - 1;
          inside_base = p2_base;
          outside_base = outside_edge_point_base;
          g_patched2 = true;
        } else {
          p_in_delta = inside_edge_point_base;
          p_in_bad = inside_edge_points[parity] - 1;
          p_in_repl = edge0_inside_base;
          p_out_base = p_in_bad + 1;
          p_out_delta = outside_edge_point_base - p_out_base;
          p_out_bad = p_out_base + outside_points[e] - 1;
          p_out_repl = edge0_outside_base;
          inside_base = 0;
          outside_base = p_out_base;
          g_patched = true;
        }
      } else if (e == 2 && ring == degenerate_ring[parity]) {
        p2_base = inside_edge_point_base;
        p2_bad = -1;
        p2_repl = -1;
        p2_end = p2_base << 1u;
        inside_base = p2_base;
        outside_base = outside_edge_point_base;
        g_patched2 = true;
      }
      if (ring == 1) {
        stitch_transition(num_indices, inside_base, ctx_in[parity].half_points, odd_in[parity],
                          outside_base, outside_half[e], outside_odd[e]);
      } else {
        stitch_regular(true, DIAGONALS_MIRRORED, num_indices, inside_edge_points[parity],
                       inside_base, outside_base);
      }
      g_patched = false;
      g_patched2 = false;
      num_indices += num_triangles * 3;
      outside_edge_point_base += outside_points[e] - 1;
      if (e == 2 && ring == degenerate_ring[parity]) {
        inside_edge_point_base -= inside_edge_points[parity] - 1;
      } else {
        inside_edge_point_base += inside_edge_points[parity] - 1;
      }
      outside_points[e] = inside_edge_points[parity];
    }
    if (ring == 1) {
      for (var e = 0; e < 4; e++) {
        outside_half[e] = ctx_in[e & 1].half_points;
        outside_odd[e] = odd_in[e & 1];
      }
    }
  }
  if (points_in[0] > points_in[1] && odd_in[1]) {
    g_patched2 = true;
    let strip_quads = (((points_in[0] >> 1u) - (points_in[1] >> 1u)) << 1u) +
                      select(1, 2, !odd_in[0]);
    p2_base = outside_edge_point_base + strip_quads + 2;
    p2_bad = p2_base;
    p2_repl = outside_edge_point_base;
    p2_end = p2_base + p2_base + strip_quads;
    stitch_regular(false, DIAGONALS_INSIDE_TO_OUTSIDE, num_indices, strip_quads + 1, p2_base,
                   outside_edge_point_base + 1);
    g_patched2 = false;
  } else if (points_in[1] >= points_in[0] && odd_in[0]) {
    g_patched2 = true;
    let strip_quads = (((points_in[1] >> 1u) - (points_in[0] >> 1u)) << 1u) +
                      select(1, 2, !odd_in[1]);
    p2_base = outside_edge_point_base + strip_quads + 1;
    p2_bad = -1;
    p2_end = p2_base + p2_base + strip_quads;
    let diagonals = select(DIAGONALS_INSIDE_TO_OUTSIDE_EXCEPT_MIDDLE, DIAGONALS_INSIDE_TO_OUTSIDE,
                           !odd_in[1]);
    stitch_regular(false, diagonals, num_indices, strip_quads + 1, p2_base,
                   outside_edge_point_base);
    g_patched2 = false;
  }
}

fn endian_swap(value : u32, endian : u32) -> u32 {
  var v = value;
  if (endian == 1u || endian == 2u) {
    v = ((v << 8u) & 0xFF00FF00u) | ((v >> 8u) & 0x00FF00FFu);
  }
  if (endian == 2u || endian == 3u) {
    v = (v << 16u) | (v >> 16u);
  }
  return v;
}

// The guest index buffer element (endian-swapped), or the element number for
// auto-indexed draws.
fn guest_index(element : u32) -> u32 {
  if ((params.flags & kFlagIndexed) == 0u) {
    return element;
  }
  let e = params.index_first + element;
  if ((params.flags & kFlagIndex32) != 0u) {
    return endian_swap(guest_indices[e], params.index_endian);
  }
  let word = guest_indices[e >> 1u];
  return endian_swap((word >> ((e & 1u) * 16u)) & 0xFFFFu, params.index_endian);
}

// tessellation_indexed_vs / the patch index of the hull shaders.
fn vertex_index_as_float(value : u32) -> f32 {
  var v = (value + params.vertex_base_index) & 0xFFFFFFu;
  v = max(v, params.vertex_index_min);
  v = min(v, params.vertex_index_max);
  return f32(v);
}

// tessellation_adaptive_vs: the float32 edge factor + 1, clamped (DXBC max /
// min: a NaN operand gives the other one).
fn edge_factor(element : u32) -> f32 {
  var f = bitcast<f32>(guest_index(element)) + 1.0;
  f = select(params.factor_min, f, f >= params.factor_min);
  f = select(params.factor_max, f, f <= params.factor_max);
  return f;
}

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) id : vec3<u32>) {
  let patch_index = id.x;
  if (patch_index >= params.patch_count) {
    return;
  }
  g_point_base = patch_index * params.slot_points;
  g_index_base = patch_index * params.slot_indices;
  g_quad = (params.flags & kFlagQuad) != 0u;
  g_patched = false;
  g_patched2 = false;
  let integer = (params.flags & kFlagInteger) != 0u;
  // Unused index slots: degenerate triangles on the patch's first point.
  for (var i = 0u; i < params.slot_indices; i++) {
    out_indices[g_index_base + i] = g_point_base;
  }
  var patch_in = vec4<f32>(0.0);
  if ((params.flags & kFlagAdaptive) != 0u) {
    patch_in.x = vertex_index_as_float(patch_index);
    g_patch_input = patch_in;
    if (g_quad) {
      let f0 = edge_factor(patch_index * 4u);
      let f1 = edge_factor(patch_index * 4u + 1u);
      let f2 = edge_factor(patch_index * 4u + 2u);
      let f3 = edge_factor(patch_index * 4u + 3u);
      tessellate_quad(f3, f0, f1, f2, min(f2, f0), min(f1, f3), integer);
    } else {
      let f0 = edge_factor(patch_index * 3u);
      let f1 = edge_factor(patch_index * 3u + 1u);
      let f2 = edge_factor(patch_index * 3u + 2u);
      tessellate_tri(f1, f2, f0, min(min(f2, f1), f0), integer);
    }
    return;
  }
  if ((params.flags & kFlagCPIndexed) != 0u) {
    let cp_count = select(3u, 4u, g_quad);
    for (var i = 0u; i < cp_count; i++) {
      patch_in[i] = vertex_index_as_float(guest_index(patch_index * cp_count + i));
    }
  } else {
    patch_in.x = vertex_index_as_float(guest_index(patch_index));
  }
  g_patch_input = patch_in;
  let f = params.factor_max;
  if (g_quad) {
    tessellate_quad(f, f, f, f, f, f, integer);
  } else {
    tessellate_tri(f, f, f, f, integer);
  }
}
)";

// Counts of CHWTessellator for uniform factors (slot sizes).
int32_t FloatToFixed(float x) {
  if (!(x > 0.0f)) {
    return 0;
  }
  return int32_t(std::nearbyint(double(x) * 65536.0));
}
int32_t FxpCeil(int32_t x) { return (x & 0xFFFF) ? (x & 0x7FFF0000) + 65536 : x; }
int32_t NumPointsFor(int32_t fxp, bool odd) {
  if (odd) {
    return (FxpCeil(32768 + (fxp + 1) / 2) * 2) >> 16;
  }
  return ((FxpCeil((fxp + 1) / 2) * 2) >> 16) + 1;
}

}  // namespace

void WebGPUTessellator::GetPatchCounts(bool quad, bool integer_partitioning, float factor,
                                       uint32_t& points_out, uint32_t& indices_out) {
  float lower = integer_partitioning ? 1.0f : 2.0f;
  float f = std::isnan(factor) ? lower : std::min(std::max(factor, lower), 64.0f);
  bool odd = false;
  bool odd_in = false;
  if (integer_partitioning) {
    f = std::ceil(f);
    odd = (int32_t(f) & 1) != 0;
    odd_in = odd && f != 1.0f;
  }
  int32_t fxp = FloatToFixed(f);
  if (integer_partitioning && fxp == 65536) {
    points_out = quad ? 4 : 3;
    indices_out = quad ? 6 : 3;
    return;
  }
  int32_t points_edge = NumPointsFor(fxp, odd);
  int32_t points_in = std::max(odd_in ? 4 : 3, NumPointsFor(fxp, odd_in));
  int32_t edges = quad ? 4 : 3;
  int32_t points = edges * points_edge - edges;
  int32_t triangles = 0;
  if (quad) {
    points += (points_in - 2) * (points_in - 2);
    int32_t rings = (points_in + 1) >> 1;
    int32_t outside = points_edge;
    for (int32_t ring = 1; ring < rings; ++ring) {
      int32_t inside = points_in - 2 * ring;
      triangles += edges * (inside + outside - 2);
      outside = inside;
    }
    // Same factors on both axes: no center strip (it needs U != V point counts
    // or an odd axis with an even other one... both odd: one row of quads).
    if (odd_in) {
      triangles += 2 * 1;
    }
  } else {
    int32_t rings_in = (points_in >> 1) - 1;
    points += odd_in ? 3 * (rings_in * (rings_in + 1) - rings_in)
                     : 3 * (rings_in * (rings_in + 1)) + 1;
    int32_t rings = (points_in + 1) >> 1;
    int32_t outside = points_edge;
    for (int32_t ring = 1; ring < rings; ++ring) {
      int32_t inside = points_in - 2 * ring;
      triangles += edges * (inside + outside - 2);
      outside = inside;
    }
    if (odd_in) {
      triangles += 1;
    }
  }
  points_out = uint32_t(std::max(points, 1));
  indices_out = uint32_t(std::max(triangles, 1) * 3);
}

WebGPUTessellator::WebGPUTessellator(WebGPUCommandProcessor& command_processor,
                                     const RegisterFile& register_file)
    : command_processor_(command_processor), register_file_(register_file) {}

WebGPUTessellator::~WebGPUTessellator() { Shutdown(); }

bool WebGPUTessellator::available() const {
  return pipeline_ != nullptr && REXCVAR_GET(webgpu_tessellation);
}

bool WebGPUTessellator::Initialize() {
  WebGPUDevice& device = command_processor_.device();
  max_binding_size_ = std::min<uint64_t>(device.limits().maxStorageBufferBindingSize,
                                         device.limits().maxBufferSize);
  storage_offset_alignment_ =
      std::max<uint32_t>(device.limits().minStorageBufferOffsetAlignment, 4);
  device.RunSync([this, &device]() {
    WGPUDevice d = device.device();
    WGPUBindGroupLayoutEntry entries[4];
    for (uint32_t i = 0; i < 4; ++i) {
      entries[i] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
      entries[i].binding = i;
      entries[i].visibility = WGPUShaderStage_Compute;
    }
    entries[0].buffer.type = WGPUBufferBindingType_Uniform;
    entries[0].buffer.hasDynamicOffset = WGPU_TRUE;
    entries[0].buffer.minBindingSize = sizeof(Params);
    entries[1].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
    entries[2].buffer.type = WGPUBufferBindingType_Storage;
    entries[3].buffer.type = WGPUBufferBindingType_Storage;
    WGPUBindGroupLayoutDescriptor layout_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    layout_desc.label = MakeStringView("xe tessellator");
    layout_desc.entryCount = 4;
    layout_desc.entries = entries;
    layout_ = wgpuDeviceCreateBindGroupLayout(d, &layout_desc);
    if (!layout_) {
      return;
    }
    WGPUPipelineLayoutDescriptor pipeline_layout_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pipeline_layout_desc.bindGroupLayoutCount = 1;
    pipeline_layout_desc.bindGroupLayouts = &layout_;
    WGPUPipelineLayout pipeline_layout = wgpuDeviceCreatePipelineLayout(d, &pipeline_layout_desc);
    WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
    source.code = MakeStringView(kTessellatorWgsl);
    WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    module_desc.nextInChain = &source.chain;
    module_desc.label = MakeStringView("xe tessellator");
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(d, &module_desc);
    WGPUComputePipelineDescriptor desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    desc.label = MakeStringView("xe tessellator");
    desc.layout = pipeline_layout;
    desc.compute.module = module;
    desc.compute.entryPoint = MakeStringView("main");
    pipeline_ = wgpuDeviceCreateComputePipeline(d, &desc);
    if (pipeline_layout) {
      wgpuPipelineLayoutRelease(pipeline_layout);
    }
    if (module) {
      wgpuShaderModuleRelease(module);
    }
  });
  if (!pipeline_) {
    REXGPU_ERROR("WebGPU tessellator: creating the compute pipeline failed");
    return false;
  }
  REXGPU_INFO("WebGPU tessellator: compute tessellation pre-pass ready (webgpu_tessellation={})",
              REXCVAR_GET(webgpu_tessellation));
  return true;
}

void WebGPUTessellator::Shutdown() {
  if (!layout_ && !pipeline_ && !points_buffer_ && !index_buffer_) {
    return;
  }
  command_processor_.device().RunSync([this]() {
    if (pipeline_) {
      wgpuComputePipelineRelease(pipeline_);
    }
    if (layout_) {
      wgpuBindGroupLayoutRelease(layout_);
    }
    if (points_buffer_) {
      wgpuBufferRelease(points_buffer_);
    }
    if (index_buffer_) {
      wgpuBufferRelease(index_buffer_);
    }
  });
  pipeline_ = nullptr;
  layout_ = nullptr;
  points_buffer_ = nullptr;
  index_buffer_ = nullptr;
  points_buffer_size_ = 0;
  index_buffer_size_ = 0;
}

void WebGPUTessellator::ReleaseBuffers(void* user, WebGPUReplayState& /*state*/) {
  auto* buffers = static_cast<std::vector<WGPUBuffer>*>(user);
  for (WGPUBuffer buffer : *buffers) {
    if (buffer) {
      wgpuBufferRelease(buffer);
    }
  }
  delete buffers;
}

bool WebGPUTessellator::EnsureBuffers(uint64_t points_size, uint64_t indices_size) {
  if (points_buffer_size_ >= points_size && index_buffer_size_ >= indices_size) {
    return true;
  }
  // Streams recorded earlier still reference the old buffers: released after
  // the current stream (the last one that may use them) is submitted.
  auto* old_buffers = new std::vector<WGPUBuffer>();
  uint64_t new_points_size = points_buffer_size_;
  uint64_t new_index_size = index_buffer_size_;
  if (points_buffer_size_ < points_size) {
    old_buffers->push_back(points_buffer_);
    new_points_size =
        std::min(std::max<uint64_t>((points_size + 0xFFFFF) & ~uint64_t(0xFFFFF),
                                    points_buffer_size_ * 2),
                 max_binding_size_);
  }
  if (index_buffer_size_ < indices_size) {
    old_buffers->push_back(index_buffer_);
    new_index_size = std::min(std::max<uint64_t>((indices_size + 0xFFFFF) & ~uint64_t(0xFFFFF),
                                                 index_buffer_size_ * 2),
                              max_binding_size_);
  }
  WGPUBuffer new_points = points_buffer_;
  WGPUBuffer new_indices = index_buffer_;
  WebGPUDevice& device = command_processor_.device();
  device.RunSync([&]() {
    WGPUDevice d = device.device();
    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    if (new_points_size != points_buffer_size_) {
      desc.label = MakeStringView("xe tessellator points");
      desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_Vertex;
      desc.size = new_points_size;
      new_points = wgpuDeviceCreateBuffer(d, &desc);
    }
    if (new_index_size != index_buffer_size_) {
      desc.label = MakeStringView("xe tessellator indices");
      desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_Index;
      desc.size = new_index_size;
      new_indices = wgpuDeviceCreateBuffer(d, &desc);
    }
  });
  if (!new_points || !new_indices) {
    REXGPU_ERROR("WebGPU tessellator: creating the {} / {} byte output buffers failed",
                 new_points_size, new_index_size);
    // Keep whatever exists; the new ones (if any) are released with the old.
    if (new_points && new_points != points_buffer_) {
      old_buffers->push_back(new_points);
    }
    if (new_indices && new_indices != index_buffer_) {
      old_buffers->push_back(new_indices);
    }
    for (WGPUBuffer& buffer : *old_buffers) {
      if (buffer == points_buffer_ || buffer == index_buffer_) {
        buffer = nullptr;
      }
    }
    command_processor_.command_stream().AddSubmittedCallback(ReleaseBuffers, old_buffers);
    return false;
  }
  points_buffer_ = new_points;
  points_buffer_size_ = new_points_size;
  index_buffer_ = new_indices;
  index_buffer_size_ = new_index_size;
  command_processor_.command_stream().AddSubmittedCallback(ReleaseBuffers, old_buffers);
  return points_buffer_size_ >= points_size && index_buffer_size_ >= indices_size;
}

bool WebGPUTessellator::RecordTessellation(const PrimitiveProcessor::ProcessingResult& result,
                                           Output& output) {
  output = {};
  if (!pipeline_) {
    return false;
  }
  using HVST = Shader::HostVertexShaderType;
  const RegisterFile& regs = register_file_;
  HVST type = result.host_vertex_shader_type;
  bool quad = type == HVST::kQuadDomainCPIndexed || type == HVST::kQuadDomainPatchIndexed;
  bool cp_indexed = type == HVST::kTriangleDomainCPIndexed || type == HVST::kQuadDomainCPIndexed;
  bool adaptive = result.tessellation_mode == xenos::TessellationMode::kAdaptive;
  bool integer = result.tessellation_mode == xenos::TessellationMode::kDiscrete;
  uint32_t control_points = (adaptive || cp_indexed) ? (quad ? 4 : 3) : 1;
  uint32_t patch_count = result.guest_draw_vertex_count / control_points;
  if (!patch_count) {
    return false;
  }
  float factor_min = regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  float factor_max = regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;

  Params params = {};
  params.patch_count = patch_count;
  params.flags = (quad ? kFlagQuad : 0) | (integer ? kFlagInteger : 0) |
                 (adaptive ? kFlagAdaptive : 0) | (cp_indexed ? kFlagCPIndexed : 0);
  params.factor_min = factor_min;
  params.factor_max = factor_max;
  params.vertex_base_index = regs[XE_GPU_REG_VGT_INDX_OFFSET];
  params.vertex_index_min = regs[XE_GPU_REG_VGT_MIN_VTX_INDX];
  params.vertex_index_max = regs[XE_GPU_REG_VGT_MAX_VTX_INDX];

  // The guest index buffer (or the factors) as a read-only storage window of
  // the shared memory buffer.
  WebGPUSharedMemory& shared_memory = *command_processor_.shared_memory();
  WGPUBuffer index_window_buffer = shared_memory.buffer();
  uint64_t index_window_offset = 0;
  uint64_t index_window_size = 256;
  if (result.index_buffer_type == PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA) {
    uint32_t index_size = result.host_index_format == xenos::IndexFormat::kInt16 ? 2 : 4;
    uint64_t start = result.guest_index_base;
    uint64_t end = start + uint64_t(result.guest_draw_vertex_count) * index_size;
    index_window_offset = start & ~uint64_t(storage_offset_alignment_ - 1);
    index_window_size = ((end + 3) & ~uint64_t(3)) - index_window_offset;
    params.index_first = uint32_t((start - index_window_offset) / index_size);
    params.flags |= kFlagIndexed | (index_size == 4 ? kFlagIndex32 : 0);
    // As ApplyIndexSystemConstants: 8-in-32 / 16-in-32 on 16-bit indices.
    xenos::Endian endian = regs.Get<reg::VGT_DMA_SIZE>().swap_mode;
    if (index_size == 2 && endian != xenos::Endian::kNone && endian != xenos::Endian::k8in16) {
      endian = endian == xenos::Endian::k8in32 ? xenos::Endian::k8in16 : xenos::Endian::kNone;
    }
    params.index_endian = uint32_t(endian);
  } else if (result.index_buffer_type != PrimitiveProcessor::ProcessedIndexBufferType::kNone ||
             adaptive) {
    ++statistics_.refused_index_buffer;
    return false;
  }

  uint32_t slot_points, slot_indices;
  // Adaptive factors are clamped to factor_max: the counts at it bound every patch.
  GetPatchCounts(quad, integer, factor_max, slot_points, slot_indices);
  params.slot_points = slot_points;
  params.slot_indices = slot_indices;
  uint64_t points_size = uint64_t(patch_count) * slot_points * kPointStride;
  uint64_t indices_size = uint64_t(patch_count) * slot_indices * sizeof(uint32_t);
  if (points_size > max_binding_size_ || indices_size > max_binding_size_ ||
      index_window_size > max_binding_size_) {
    ++statistics_.refused_size;
    if (statistics_.refused_size <= 4) {
      REXGPU_WARN(
          "WebGPU tessellator: draw of {} patches (factor {}) needs {} / {} bytes, over the {} "
          "byte binding limit; dropped",
          patch_count, factor_max, points_size, indices_size, max_binding_size_);
    }
    return false;
  }
  if (!EnsureBuffers(points_size, indices_size)) {
    ++statistics_.refused_size;
    return false;
  }

  if (draws_logged_ < uint32_t(std::max(REXCVAR_GET(webgpu_tessellation_log_draws), 0))) {
    ++draws_logged_;
    REXGPU_INFO(
        "WebGPU tessellator: draw {}: host vs type {} tess mode {} prim {} patches {} factor "
        "range {}..{} slot {} points / {} indices, index buffer type {} base {:08X} format {} "
        "endian {} base index {} min/max {}/{}",
        statistics_.draws, uint32_t(type), uint32_t(result.tessellation_mode),
        uint32_t(result.guest_primitive_type), patch_count, factor_min, factor_max, slot_points,
        slot_indices, uint32_t(result.index_buffer_type), result.guest_index_base,
        uint32_t(result.host_index_format), params.index_endian, int32_t(params.vertex_base_index),
        params.vertex_index_min, params.vertex_index_max);
  }

  command_processor_.EndRenderPass();
  WebGPUCommandStream& stream = command_processor_.command_stream();
  uint32_t params_offset;
  void* params_data = stream.AllocateConstants(sizeof(Params), params_offset);
  if (!params_data) {
    return false;
  }
  std::memcpy(params_data, &params, sizeof(Params));
  stream.CmdBeginComputePass();
  stream.CmdSetComputePipelineObject(pipeline_);
  WGPUBindGroupEntry entries[4];
  for (uint32_t i = 0; i < 4; ++i) {
    entries[i] = WGPU_BIND_GROUP_ENTRY_INIT;
    entries[i].binding = i;
  }
  entries[0].buffer = StreamConstantsBuffer();
  entries[0].offset = 0;
  entries[0].size = sizeof(Params);
  entries[1].buffer = index_window_buffer;
  entries[1].offset = index_window_offset;
  entries[1].size = index_window_size;
  entries[2].buffer = points_buffer_;
  entries[2].offset = 0;
  entries[2].size = points_size;
  entries[3].buffer = index_buffer_;
  entries[3].offset = 0;
  entries[3].size = indices_size;
  stream.CmdSetBindGroup(0, layout_, entries, 4, 1, &params_offset);
  stream.CmdDispatch((patch_count + kWorkgroupSize - 1) / kWorkgroupSize, 1, 1);
  stream.CmdEndComputePass();

  output.vertex_buffer = points_buffer_;
  output.vertex_buffer_size = points_size;
  output.index_buffer = index_buffer_;
  output.index_buffer_size = indices_size;
  output.index_count = patch_count * slot_indices;
  ++statistics_.draws;
  statistics_.patches += patch_count;
  statistics_.indices += output.index_count;
  statistics_.max_slot_points = std::max(statistics_.max_slot_points, slot_points);
  statistics_.max_slot_indices = std::max(statistics_.max_slot_indices, slot_indices);
  return true;
}

}  // namespace rex::graphics::webgpu
