// rex_spirv_to_wgsl: host CLI around rex::graphics::webgpu::SpirvToWgsl (the
// same code the wasm64 build links), plus a re-parse of the output with Tint's
// WGSL reader + resolver restricted to what Chrome accepts. Used for WP2 step 0
// (corpus acceptance) and to generate src/graphics/shaders/webgpu_wgsl/*.h.
//
//   rex_spirv_to_wgsl [--texel-buffers-as-1d] [--no-clip-distances] [--no-validate]
//                     <in.spv> <out.wgsl>
// Exit code 0 = converted (and validated), 1 = conversion failed, 2 = the WGSL
// output failed validation, 3 = usage / IO.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <rex/graphics/webgpu/webgpu_shader.h>

#if TINT_BUILD_WGSL_READER
#include "src/tint/lang/wgsl/reader/reader.h"
#include "src/tint/utils/diagnostic/formatter.h"
#endif

int main(int argc, char** argv) {
  rex::graphics::webgpu::SpirvToWgslOptions options;
  bool validate = true;
  std::vector<const char*> files;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--texel-buffers-as-1d")) {
      options.texel_buffers_as_1d_textures = true;
    } else if (!std::strcmp(argv[i], "--no-clip-distances")) {
      options.clip_distances = false;
    } else if (!std::strcmp(argv[i], "--no-validate")) {
      validate = false;
    } else {
      files.push_back(argv[i]);
    }
  }
  if (files.size() != 2) {
    std::fprintf(stderr,
                 "usage: rex_spirv_to_wgsl [--texel-buffers-as-1d] [--no-clip-distances] "
                 "[--no-validate] <in.spv> <out.wgsl>\n");
    return 3;
  }
  std::ifstream in(files[0], std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (bytes.empty() || bytes.size() % 4) {
    std::fprintf(stderr, "error: cannot read %s\n", files[0]);
    return 3;
  }
  std::vector<uint32_t> words(bytes.size() / 4);
  std::memcpy(words.data(), bytes.data(), bytes.size());

  std::string wgsl, error;
  if (!rex::graphics::webgpu::SpirvToWgsl(words, wgsl, error, options)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }
  std::ofstream(files[1], std::ios::binary).write(wgsl.data(), wgsl.size());

#if TINT_BUILD_WGSL_READER
  if (validate) {
    tint::wgsl::reader::Options parser_options;
    if (options.clip_distances) {
      parser_options.allowed_features.extensions.insert(tint::wgsl::Extension::kClipDistances);
    }
    for (tint::wgsl::LanguageFeature feature : {
             tint::wgsl::LanguageFeature::kReadonlyAndReadwriteStorageTextures,
             tint::wgsl::LanguageFeature::kPacked4X8IntegerDotProduct,
             tint::wgsl::LanguageFeature::kUnrestrictedPointerParameters,
             tint::wgsl::LanguageFeature::kPointerCompositeAccess,
         }) {
      parser_options.allowed_features.features.insert(feature);
    }
    auto source = std::make_unique<tint::Source::File>(files[1], wgsl);
    auto program = tint::wgsl::reader::Parse(source.get(), parser_options);
    if (!program.IsValid()) {
      tint::diag::Formatter formatter;
      std::cerr << "error: WGSL validation: " << formatter.Format(program.Diagnostics()).Plain()
                << "\n";
      return 2;
    }
  }
#else
  (void)validate;
#endif
  return 0;
}
