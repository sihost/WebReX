/**
 * @file        graphics/webgpu/webgpu_shader_tint.cpp
 * @brief       SpirvToWgsl: SPIR-V -> WGSL with Tint (Dawn 31e25af2).
 *
 * Compiled by thirdparty/tint/CMakeLists.txt together with Tint's SPIR-V
 * reader and WGSL writer (libtint_web.a on wasm64, rex_spirv_to_wgsl on the
 * host), not by rexgraphics, so it can use Tint's internal include layout.
 *
 * Before Tint, the SPIR-V gets small, semantics-preserving rewrites for things
 * Tint's reader or WGSL do not take (project docs/port/webgpu_wp2.md):
 * - Restrict / Aliased decorations: aliasing hints with no WGSL equivalent; the
 *   reader raises an internal compiler error on member Restrict.
 * - Invariant only on the position built-in (WGSL rule); a block-level
 *   Invariant moves to the gl_Position member.
 * - ExecutionMode EarlyFragmentTests dropped (no WGSL attribute).
 * - OpIsNan -> exponent/mantissa bit test (no isNan in WGSL / the reader).
 * - Push constants -> uniform buffer (WGSL immediates are optional).
 * - Optionally, texel buffers -> 1D textures (SpirvToWgslOptions).
 */

#include <rex/graphics/webgpu/webgpu_shader.h>

#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "src/tint/lang/core/ir/module.h"
#include "src/tint/lang/spirv/reader/reader.h"
#include "src/tint/lang/wgsl/writer/writer.h"

#ifndef REX_TINT_REVISION
#define REX_TINT_REVISION "dawn-unknown"
#endif

namespace rex::graphics::webgpu {


namespace {

// SPIR-V enumerants used below (spirv.hpp values).
constexpr uint32_t kSpvMagic = 0x07230203;
constexpr uint32_t kOpExecutionMode = 16;
constexpr uint32_t kOpCapability = 17;
constexpr uint32_t kOpTypeVoid = 19;

constexpr uint32_t kOpTypeInt = 21;
constexpr uint32_t kOpTypeVector = 23;
constexpr uint32_t kOpTypeImage = 25;
constexpr uint32_t kOpTypePointer = 32;
constexpr uint32_t kOpConstant = 43;
constexpr uint32_t kOpConstantComposite = 44;
constexpr uint32_t kOpFunction = 54;
constexpr uint32_t kOpVariable = 59;
constexpr uint32_t kOpLoad = 61;
constexpr uint32_t kOpDecorate = 71;
constexpr uint32_t kOpMemberDecorate = 72;
constexpr uint32_t kOpImageFetch = 95;
constexpr uint32_t kOpImage = 100;
constexpr uint32_t kOpImageQuerySizeLod = 103;
constexpr uint32_t kOpImageQuerySize = 104;
constexpr uint32_t kOpBitcast = 124;
constexpr uint32_t kOpIsNan = 156;
constexpr uint32_t kOpUGreaterThan = 172;
constexpr uint32_t kOpBitwiseAnd = 199;
constexpr uint32_t kExecutionModeEarlyFragmentTests = 9;
constexpr uint32_t kStorageClassUniform = 2;
constexpr uint32_t kStorageClassPushConstant = 9;
constexpr uint32_t kDecorationBuiltIn = 11;
constexpr uint32_t kDecorationInvariant = 18;
constexpr uint32_t kDecorationRestrict = 19;
constexpr uint32_t kDecorationAliased = 20;
constexpr uint32_t kDecorationBinding = 33;
constexpr uint32_t kDecorationDescriptorSet = 34;
constexpr uint32_t kBuiltInPosition = 0;
constexpr uint32_t kBuiltInFragCoord = 15;
constexpr uint32_t kDim1D = 0;
constexpr uint32_t kDimBuffer = 5;
constexpr uint32_t kCapabilitySampled1D = 43;
constexpr uint32_t kCapabilityImage1D = 44;
constexpr uint32_t kCapabilitySampledBuffer = 46;
constexpr uint32_t kCapabilityImageBuffer = 47;
constexpr uint32_t kImageOperandsLod = 0x2;

uint32_t Op(uint32_t opcode, uint32_t word_count) { return (word_count << 16) | opcode; }

uint64_t Target(uint32_t id, uint32_t member_plus_1 = 0) {
  return (uint64_t(id) << 32) | member_plus_1;
}

// Returns false with error on malformed or unsupported input.
bool PreprocessSpirv(std::span<const uint32_t> in, std::vector<uint32_t>& out,
                     const SpirvToWgslOptions& options, std::string& error) {
  if (in.size() < 5 || in[0] != kSpvMagic) {
    error = "not a SPIR-V module";
    return false;
  }
  uint32_t bound = in[3];

  // Pass 1: collect what the rewrites need.
  std::unordered_set<uint32_t> buffer_image_types;
  uint32_t int_type = 0, int_zero = 0, uint_type = 0;
  // Targets with BuiltIn Position / FragCoord, the only ones WGSL allows
  // @invariant on.
  std::unordered_set<uint64_t> position_targets;
  // Vertex output gl_PerVertex member with BuiltIn Position, to move an
  // Invariant of the whole block variable to.
  uint64_t position_member = 0;
  std::unordered_set<uint64_t> invariant_targets;
  std::vector<uint32_t> push_constant_vars;
  int32_t max_set = -1;
  std::unordered_map<uint32_t, uint32_t> vector_sizes;            // vector type -> count
  std::unordered_map<uint64_t, uint32_t> vector_types;            // component << 32 | count -> id
  std::unordered_map<uint32_t, uint32_t> uint_constants;          // value -> id
  std::unordered_set<uint32_t> is_nan_sizes;
  for (size_t i = 5; i < in.size();) {
    uint32_t wc = in[i] >> 16, op = in[i] & 0xFFFF;
    if (!wc || i + wc > in.size()) {
      error = "truncated SPIR-V instruction";
      return false;
    }
    const uint32_t* w = &in[i];
    switch (op) {
      case kOpTypeImage:
        if (options.texel_buffers_as_1d_textures && wc >= 9 && w[3] == kDimBuffer) {
          buffer_image_types.insert(w[1]);
        }
        break;
      case kOpTypeInt:
        if (wc == 4 && w[2] == 32) {
          if (w[3] == 1 && !int_type) int_type = w[1];
          if (w[3] == 0 && !uint_type) uint_type = w[1];
        }
        break;
      case kOpTypeVector:
        if (wc == 4) {
          vector_sizes[w[1]] = w[3];
          vector_types.emplace((uint64_t(w[2]) << 32) | w[3], w[1]);
        }
        break;
      case kOpConstant:
        if (wc == 4 && int_type && w[1] == int_type && w[3] == 0 && !int_zero) int_zero = w[2];
        if (wc == 4 && uint_type && w[1] == uint_type) uint_constants.emplace(w[3], w[2]);
        break;
      case kOpDecorate:
        if (wc >= 4 && w[2] == kDecorationBuiltIn &&
            (w[3] == kBuiltInPosition || w[3] == kBuiltInFragCoord)) {
          position_targets.insert(Target(w[1]));
        } else if (wc >= 4 && w[2] == kDecorationDescriptorSet) {
          max_set = std::max(max_set, int32_t(w[3]));
        } else if (wc >= 3 && w[2] == kDecorationInvariant) {
          invariant_targets.insert(Target(w[1]));
        }
        break;
      case kOpMemberDecorate:
        if (wc >= 5 && w[3] == kDecorationBuiltIn &&
            (w[4] == kBuiltInPosition || w[4] == kBuiltInFragCoord)) {
          position_targets.insert(Target(w[1], w[2] + 1));
          if (w[4] == kBuiltInPosition) position_member = Target(w[1], w[2] + 1);
        } else if (wc >= 4 && w[3] == kDecorationInvariant) {
          invariant_targets.insert(Target(w[1], w[2] + 1));
        }
        break;
      case kOpVariable:
        if (wc >= 4 && w[3] == kStorageClassPushConstant) push_constant_vars.push_back(w[2]);
        break;
      case kOpIsNan:
        if (wc == 4) {
          auto it = vector_sizes.find(w[1]);
          is_nan_sizes.insert(it != vector_sizes.end() ? it->second : 1);
        }
        break;
      default:
        break;
    }
    i += wc;
  }

  // Declarations added before the first function.
  std::vector<uint32_t> new_globals;
  if (!buffer_image_types.empty() && !int_zero) {
    if (!int_type) {
      int_type = bound++;
      new_globals.insert(new_globals.end(), {Op(kOpTypeInt, 4), int_type, 32, 1});
    }
    int_zero = bound++;
    new_globals.insert(new_globals.end(), {Op(kOpConstant, 4), int_type, int_zero, 0});
  }
  // isnan(x) -> (bitcast<u32>(x) & 0x7FFFFFFF) > 0x7F800000: Tint's reader has no
  // OpIsNan (WGSL has no isNan), and a bit test can't be folded away by
  // finite-math assumptions.
  std::unordered_map<uint32_t, uint32_t> nan_uint_types, nan_abs_masks, nan_inf_values;
  if (!is_nan_sizes.empty()) {
    if (!uint_type) {
      uint_type = bound++;
      new_globals.insert(new_globals.end(), {Op(kOpTypeInt, 4), uint_type, 32, 0});
    }
    auto scalar_constant = [&](uint32_t value) {
      auto it = uint_constants.find(value);
      if (it != uint_constants.end()) return it->second;
      uint32_t id = bound++;
      new_globals.insert(new_globals.end(), {Op(kOpConstant, 4), uint_type, id, value});
      uint_constants.emplace(value, id);
      return id;
    };
    uint32_t abs_mask = scalar_constant(0x7FFFFFFF), inf_value = scalar_constant(0x7F800000);
    for (uint32_t n : is_nan_sizes) {
      if (n == 1) {
        nan_uint_types[1] = uint_type;
        nan_abs_masks[1] = abs_mask;
        nan_inf_values[1] = inf_value;
        continue;
      }
      uint32_t vector_type;
      auto it = vector_types.find((uint64_t(uint_type) << 32) | n);
      if (it != vector_types.end()) {
        vector_type = it->second;
      } else {
        vector_type = bound++;
        new_globals.insert(new_globals.end(), {Op(kOpTypeVector, 4), vector_type, uint_type, n});
      }
      nan_uint_types[n] = vector_type;
      const std::pair<uint32_t, std::unordered_map<uint32_t, uint32_t>*> composites[] = {
          {abs_mask, &nan_abs_masks}, {inf_value, &nan_inf_values}};
      for (auto [value, map] : composites) {
        uint32_t id = bound++;
        new_globals.insert(new_globals.end(), {Op(kOpConstantComposite, 3 + n), vector_type, id});
        new_globals.insert(new_globals.end(), n, value);
        (*map)[n] = id;
      }
    }
  }

  // Push constants -> a uniform buffer (WGSL immediates are optional in
  // WebGPU), by default in the first group after the ones the module uses.
  uint32_t push_constants_group = options.push_constants_group != UINT32_MAX
                                      ? options.push_constants_group
                                      : uint32_t(max_set + 1);

  // Pass 2: rewrite.
  out.clear();
  out.reserve(in.size() + new_globals.size() + 64);
  out.insert(out.end(), in.begin(), in.begin() + 5);
  std::unordered_set<uint32_t> capabilities;
  std::unordered_map<uint32_t, uint32_t> value_types;  // OpLoad / OpImage result -> type
  bool annotations_closed = false, globals_emitted = false, invariant_dropped = false;
  for (size_t i = 5; i < in.size();) {
    uint32_t wc = in[i] >> 16, op = in[i] & 0xFFFF;
    std::span<const uint32_t> inst = in.subspan(i, wc);
    i += wc;
    if (!annotations_closed && op >= kOpTypeVoid && op <= kOpTypePointer) {
      // Annotations end where the type declarations start.
      annotations_closed = true;
      uint32_t binding = options.push_constants_binding;
      for (uint32_t var : push_constant_vars) {
        out.insert(out.end(), {Op(kOpDecorate, 4), var, kDecorationDescriptorSet,
                               push_constants_group, Op(kOpDecorate, 4), var, kDecorationBinding,
                               binding++});
      }
      if (invariant_dropped && position_member && !invariant_targets.count(position_member)) {
        // OpDecorate %gl_PerVertex_var Invariant -> on the Position member.
        out.insert(out.end(), {Op(kOpMemberDecorate, 4), uint32_t(position_member >> 32),
                               uint32_t(position_member) - 1, kDecorationInvariant});
      }
    }
    if (!globals_emitted && op == kOpFunction) {
      globals_emitted = true;
      out.insert(out.end(), new_globals.begin(), new_globals.end());
    }
    switch (op) {
      case kOpCapability: {
        uint32_t cap = inst[1];
        if (!buffer_image_types.empty()) {
          if (cap == kCapabilitySampledBuffer) cap = kCapabilitySampled1D;
          if (cap == kCapabilityImageBuffer) cap = kCapabilityImage1D;
        }
        if (capabilities.insert(cap).second) {
          out.push_back(inst[0]);
          out.push_back(cap);
        }
        continue;
      }
      case kOpExecutionMode:
        // WGSL has no early fragment tests attribute. Only used as a hint when
        // the shader has no kill / depth write / memexport, where implementations
        // test early anyway.
        if (wc >= 3 && inst[2] == kExecutionModeEarlyFragmentTests) {
          continue;
        }
        break;
      case kOpMemberDecorate:
        if (wc >= 4 && (inst[3] == kDecorationRestrict || inst[3] == kDecorationAliased)) {
          continue;
        }
        if (wc >= 4 && inst[3] == kDecorationInvariant &&
            !position_targets.count(Target(inst[1], inst[2] + 1))) {
          invariant_dropped = true;
          continue;
        }
        break;
      case kOpDecorate:
        if (wc >= 3 && inst[2] == kDecorationAliased) {
          continue;
        }
        if (wc >= 3 && inst[2] == kDecorationInvariant && !position_targets.count(Target(inst[1]))) {
          invariant_dropped = true;
          continue;
        }
        break;
      case kOpTypePointer:
        if (wc == 4 && inst[2] == kStorageClassPushConstant) {
          out.insert(out.end(), {inst[0], inst[1], kStorageClassUniform, inst[3]});
          continue;
        }
        break;
      case kOpVariable:
        if (wc >= 4 && inst[3] == kStorageClassPushConstant) {
          out.insert(out.end(), inst.begin(), inst.end());
          out[out.size() - wc + 3] = kStorageClassUniform;
          continue;
        }
        break;
      case kOpTypeImage:
        if (buffer_image_types.count(inst[1])) {
          out.insert(out.end(), inst.begin(), inst.end());
          out[out.size() - wc + 3] = kDim1D;
          continue;
        }
        break;
      case kOpLoad:
      case kOpImage:
        if (wc >= 4) value_types[inst[2]] = inst[1];
        break;
      case kOpImageFetch:
        if (wc >= 5 && !buffer_image_types.empty()) {
          auto it = value_types.find(inst[3]);
          if (it != value_types.end() && buffer_image_types.count(it->second)) {
            if (wc != 5) {
              error = "texel buffer fetch with image operands is not supported";
              return false;
            }
            out.insert(out.end(), {Op(kOpImageFetch, 7), inst[1], inst[2], inst[3], inst[4],
                                   kImageOperandsLod, int_zero});
            continue;
          }
        }
        break;
      case kOpImageQuerySize:
        if (wc == 4 && !buffer_image_types.empty()) {
          auto it = value_types.find(inst[3]);
          if (it != value_types.end() && buffer_image_types.count(it->second)) {
            out.insert(out.end(),
                       {Op(kOpImageQuerySizeLod, 5), inst[1], inst[2], inst[3], int_zero});
            continue;
          }
        }
        break;
      case kOpIsNan:
        if (wc == 4) {
          auto it = vector_sizes.find(inst[1]);
          uint32_t n = it != vector_sizes.end() ? it->second : 1;
          uint32_t bits = bound++, magnitude = bound++;
          out.insert(out.end(),
                     {Op(kOpBitcast, 4), nan_uint_types[n], bits, inst[3],
                      Op(kOpBitwiseAnd, 5), nan_uint_types[n], magnitude, bits, nan_abs_masks[n],
                      Op(kOpUGreaterThan, 5), inst[1], inst[2], magnitude, nan_inf_values[n]});
          continue;
        }
        break;
      default:
        break;
    }
    out.insert(out.end(), inst.begin(), inst.end());
  }
  out[3] = bound;
  return true;
}

}  // namespace

bool SpirvToWgsl(std::span<const uint32_t> spirv, std::string& wgsl_out, std::string& error_out,
                 const SpirvToWgslOptions& options) {
  wgsl_out.clear();
  error_out.clear();
  std::vector<uint32_t> words;
  if (!PreprocessSpirv(spirv, words, options, error_out)) {
    return false;
  }

  tint::spirv::reader::Options reader_options;
  auto ir = tint::spirv::reader::ReadIR(words, reader_options);
  if (ir != tint::Success) {
    std::ostringstream ss;
    ss << "SPIR-V reader: " << ir.Failure();
    error_out = ss.str();
    return false;
  }

  tint::wgsl::writer::Options writer_options;
  // Xenos shaders sample inside dynamic branches (predicated fetches), which
  // WGSL uniformity analysis rejects (webgpu.md section 3).
  writer_options.allow_non_uniform_derivatives = true;
  if (options.clip_distances) {
    writer_options.allowed_features.extensions.insert(tint::wgsl::Extension::kClipDistances);
  }
  if (options.dual_source_blending) {
    writer_options.allowed_features.extensions.insert(tint::wgsl::Extension::kDualSourceBlending);
  }
  // Language features Chrome ships (navigator.gpu.wgslLanguageFeatures).
  for (tint::wgsl::LanguageFeature feature : {
           tint::wgsl::LanguageFeature::kReadonlyAndReadwriteStorageTextures,
           tint::wgsl::LanguageFeature::kPacked4X8IntegerDotProduct,
           tint::wgsl::LanguageFeature::kUnrestrictedPointerParameters,
           tint::wgsl::LanguageFeature::kPointerCompositeAccess,
       }) {
    writer_options.allowed_features.features.insert(feature);
  }
  auto result = tint::wgsl::writer::WgslFromIR(ir.Get(), writer_options);
  if (result != tint::Success) {
    std::ostringstream ss;
    ss << "WGSL writer: " << result.Failure();
    error_out = ss.str();
    return false;
  }
  wgsl_out = std::move(result.Get().wgsl);
  return true;
}

const char* TintRevision() { return REX_TINT_REVISION; }

}  // namespace rex::graphics::webgpu
