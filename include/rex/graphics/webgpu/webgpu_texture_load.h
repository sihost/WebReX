/**
 * @file        rex/graphics/webgpu/webgpu_texture_load.h
 * @brief       WebGPU texture loading: host format table, compute untile/convert loads into a
 *              scratch buffer + copyBufferToTexture, and the sampler mapping.
 *
 * docs/port/webgpu.md section 7 (WP5). Kept free of rexcore (REX_WEBGPU_STANDALONE builds it
 * into the tools/webgpu_tests/wp5_* test page together with texture_util), so the texture
 * cache (webgpu_texture_cache.h) and the test exercise the same code:
 *  - WebGPUTextureFormatTable: the load-shader / host-format table keyed by (Xenos format,
 *    signedness, feature set). WebGPU has no 16-bit packed formats, no 4:2:2 formats and no
 *    mutable-format views (signed and unsigned views of one texture), and BC / 16-bit
 *    normalized formats are optional features, so the table differs from Vulkan's.
 *  - WebGPUTextureLoader: the xenia texture load compute shaders (the SPIR-V the Vulkan backend
 *    runs, converted to WGSL by native Tint: webgpu_texture_load_shaders.inc) plus the new
 *    WebGPU-only loads (16bpp -> rgba8 expansions, DXT3A as 1111 -> rgba8, signed 2_10_10_10
 *    -> rgba16snorm), dispatched into a scratch storage buffer (the Vulkan host layout with
 *    256-byte row pitch, the copyBufferToTexture rule), optionally followed by a second
 *    "normalized 16-bit -> float16" pass when texture-formats-tier1 is missing, then one
 *    copyBufferToTexture per mip level. Packed mip tails are loaded whole and copied out per
 *    level, as on Vulkan.
 *  - WebGPUSamplerParameters + MakeSamplerDescriptor: Xenos sampler state -> GPUSampler.
 *
 * Threading: Initialize/Shutdown/RecordLoad run on a recording thread (the CP), never on the
 * device thread; device objects are created with WebGPUDevice::RunSync.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <webgpu/webgpu.h>

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::webgpu {

class WebGPUCommandStream;
class WebGPUDevice;

// Optional device features the host format table depends on. Each can be forced off (cvars
// in the texture cache, explicit in the tests) to exercise the fallbacks.
struct WebGPUTextureFeatures {
  // texture-compression-bc: DXT1/DXT2_3/DXT4_5/DXN/DXT5A stay BC1/2/3/5/4 on the host (when
  // the base size is 4-aligned); otherwise they are decompressed in compute.
  bool bc = false;
  // texture-compression-bc-sliced-3d: BC also for 3D textures.
  bool bc_sliced_3d = false;
  // texture-formats-tier1: r/rg/rgba16 unorm/snorm host formats; without it 16-bit
  // normalized data (including the 10_11_11 / 11_11_10 / signed 2_10_10_10 expansions) is
  // stored as float16.
  bool formats_tier1 = false;
  // float32-filterable: 32_FLOAT textures can be filtered (else samplers use nearest).
  bool float32_filterable = false;
};

enum class WebGPUTextureLoadShader : uint8_t {
  kUnknown,
  // Copies (with endian swap and untiling).
  k8bpb,
  k16bpb,
  k32bpb,
  k64bpb,
  k128bpb,
  // New: 16bpp packed formats expanded to rgba8unorm (WebGPU has no 16-bit packed formats).
  kR5G5B5A1ToRGBA8,
  kR5G6B5ToRGBA8,
  kR5G5B6ToRGBA8,
  kR4G4B4A4ToRGBA8,
  // 4:2:2 -> rgba8unorm.
  kGBGR8ToRGB8,
  kBGRG8ToRGB8,
  // 10_11_11 / 11_11_10 -> rgba16 unorm / snorm.
  kR10G11B11ToRGBA16,
  kR10G11B11ToRGBA16SNorm,
  kR11G11B10ToRGBA16,
  kR11G11B10ToRGBA16SNorm,
  // New: signed 2_10_10_10 -> rgba16snorm.
  kR10G10B10A2ToRGBA16SNorm,
  // 16-bit normalized -> float16 (single pass from guest memory, or the second pass).
  kR16UNormToFloat,
  kR16SNormToFloat,
  kRG16UNormToFloat,
  kRG16SNormToFloat,
  kRGBA16UNormToFloat,
  kRGBA16SNormToFloat,
  // Decompression.
  kDXT1ToRGBA8,
  kDXT3ToRGBA8,
  kDXT5ToRGBA8,
  kDXNToRG8,
  kDXT3AToR8,
  // New: DXT3A as 1_1_1_1 -> rgba8unorm.
  kDXT3AAs1111ToRGBA8,
  kDXT5AToR8,
  kCTX1ToRG8,
  // Depth -> r32float.
  kDepthUnorm,
  kDepthFloat,

  kCount,
};

struct WebGPUTextureLoadShaderInfo {
  const char* name;
  // Bytes written per host block: per guest block when the host format keeps the guest
  // block (copies, BC passthrough), else per host texel (decompression, 4:2:2 expansion).
  uint32_t bytes_per_host_block;
  // Guest blocks along X per invocation (log2); workgroups are 4 x 32 invocations.
  uint32_t guest_x_blocks_per_thread_log2;
};
const WebGPUTextureLoadShaderInfo& GetTextureLoadShaderInfo(WebGPUTextureLoadShader shader);

struct WebGPUTextureHostFormat {
  WebGPUTextureLoadShader load_shader = WebGPUTextureLoadShader::kUnknown;
  // If not kUnknown, a second pass converting load_shader's normalized 16-bit output to
  // float16 (texture-formats-tier1 missing).
  WebGPUTextureLoadShader convert_shader = WebGPUTextureLoadShader::kUnknown;
  WGPUTextureFormat format = WGPUTextureFormat_Undefined;
  // The host format keeps the guest compression block (BC passthrough).
  bool block_compressed = false;
  bool filterable = true;
};

struct WebGPUTextureHostFormatPair {
  WebGPUTextureHostFormat format_unsigned;
  WebGPUTextureHostFormat format_signed;
  // Xenos component -> host component swizzle (XE_GPU_TEXTURE_SWIZZLE_*).
  uint32_t swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
};

// The load-shader / host-format table keyed by (Xenos format, signedness, feature set).
class WebGPUTextureFormatTable {
 public:
  explicit WebGPUTextureFormatTable(const WebGPUTextureFeatures& features = {});
  void Initialize(const WebGPUTextureFeatures& features);

  const WebGPUTextureFeatures& features() const { return features_; }

  // The pair for a texture: BC formats fall back to decompression when the base size is not
  // 4-aligned or (without bc-sliced-3d) the texture is 3D.
  const WebGPUTextureHostFormatPair& Get(xenos::TextureFormat format,
                                         xenos::DataDimension dimension, uint32_t width,
                                         uint32_t height) const;
  // The best pair for the format regardless of size (for logging and sampler filtering).
  const WebGPUTextureHostFormatPair& GetForFormat(xenos::TextureFormat format) const {
    return formats_[uint32_t(format) & 63];
  }
  // Whether the signed version needs its own host texture (a different host format or load
  // shader; WebGPU views cannot reinterpret unorm <-> snorm).
  static bool IsSignedVersionSeparate(const WebGPUTextureHostFormatPair& pair);
  // The host format a texture with this key uses: the signed one if the versions are separate
  // and signed_separate, or if there is only a signed format.
  static const WebGPUTextureHostFormat& GetHostFormat(const WebGPUTextureHostFormatPair& pair,
                                                      bool signed_separate);

 private:
  WebGPUTextureFeatures features_;
  std::array<WebGPUTextureHostFormatPair, 64> formats_;
  // Decompressing fallbacks for BC formats that can't stay compressed for a given texture.
  WebGPUTextureHostFormatPair dxt1_decompress_;
  WebGPUTextureHostFormatPair dxt2_3_decompress_;
  WebGPUTextureHostFormatPair dxt4_5_decompress_;
  WebGPUTextureHostFormatPair dxn_decompress_;
  WebGPUTextureHostFormatPair dxt5a_decompress_;
};

// A guest texture to load (the TextureCache::TextureKey fields the load depends on).
struct WebGPUTextureLoadDesc {
  xenos::TextureFormat format = xenos::TextureFormat::k_8_8_8_8;
  xenos::DataDimension dimension = xenos::DataDimension::k2DOrStacked;
  uint32_t width = 1;
  uint32_t height = 1;
  // Depth for 3D, 6 for cube, array layers for 1D/2D.
  uint32_t depth_or_array_size = 1;
  // Base row pitch in texels / 32 (fetch constant `pitch`).
  uint32_t pitch = 1;
  bool tiled = false;
  bool packed_mips = false;
  // Base level present (base_page != 0).
  bool has_base = true;
  xenos::Endian endianness = xenos::Endian::kNone;
  uint32_t mip_max_level = 0;
  // TextureKey::signed_separate (selects the signed host format when it's separate).
  bool signed_separate = false;
  // Guest data: a storage buffer (the shared memory) and the byte offsets of the base and of
  // the mips (multiples of minStorageBufferOffsetAlignment; page-aligned in guest memory).
  WGPUBuffer source = nullptr;
  uint64_t source_size = 0;
  uint64_t base_offset = 0;
  uint64_t mips_offset = 0;
  bool load_base = true;
  bool load_mips = false;
  // Optional precomputed layout (TextureCache::Texture::guest_layout()); else computed.
  const texture_util::TextureGuestLayout* guest_layout = nullptr;
};

// Host texture creation parameters for a texture with this table entry.
struct WebGPUHostTextureInfo {
  WGPUTextureFormat format = WGPUTextureFormat_Undefined;
  WGPUTextureDimension dimension = WGPUTextureDimension_2D;
  WGPUExtent3D size = {1, 1, 1};
  uint32_t mip_level_count = 1;
  // Approximate memory use (all levels and layers).
  uint64_t memory_bytes = 0;
};
bool GetHostTextureInfo(const WebGPUTextureFormatTable& table, xenos::TextureFormat format,
                        xenos::DataDimension dimension, uint32_t width, uint32_t height,
                        uint32_t depth_or_array_size, uint32_t mip_max_level,
                        bool signed_separate, WebGPUHostTextureInfo& info_out);
uint32_t GetTextureFormatBytesPerBlock(WGPUTextureFormat format, uint32_t* block_size_out);

class WebGPUTextureLoader {
 public:
  struct Statistics {
    uint64_t loads = 0;
    uint64_t dispatches = 0;
    uint64_t copies = 0;
    uint64_t scratch_bytes = 0;
    uint64_t failures = 0;
  };

  WebGPUTextureLoader() = default;
  WebGPUTextureLoader(const WebGPUTextureLoader&) = delete;
  WebGPUTextureLoader& operator=(const WebGPUTextureLoader&) = delete;
  ~WebGPUTextureLoader();

  // Recording thread (not the device thread). Creates every load pipeline.
  bool Initialize(WebGPUDevice& device);
  // Recording thread; streams referencing the loader's objects must have been replayed.
  void Shutdown();
  bool initialized() const { return device_ != nullptr; }

  // Records the load of the requested levels of `texture` (created from GetHostTextureInfo)
  // into `stream`, which must be outside passes. Returns false (recording nothing) if the
  // format isn't supported or a limit is exceeded.
  bool RecordLoad(WebGPUCommandStream& stream, const WebGPUTextureFormatTable& table,
                  const WebGPUTextureLoadDesc& desc, WGPUTexture texture);

  const Statistics& statistics() const { return statistics_; }

 private:
  struct Scratch {
    WGPUBuffer buffer = nullptr;
    uint64_t size = 0;
  };
  bool EnsureScratch(WebGPUCommandStream& stream, Scratch& scratch, uint64_t size);

  WebGPUDevice* device_ = nullptr;
  WGPUBindGroupLayout bind_group_layout_ = nullptr;
  WGPUPipelineLayout pipeline_layout_ = nullptr;
  std::array<WGPUComputePipeline, size_t(WebGPUTextureLoadShader::kCount)> pipelines_{};
  uint64_t max_scratch_size_ = 0;
  uint64_t max_binding_size_ = 0;
  uint32_t storage_offset_alignment_ = 256;
  // [0]: load output; [1]: second-pass (float16 conversion) output.
  Scratch scratch_[2];
  Statistics statistics_;
};

// ---------------------------------------------------------------------------
// Samplers.
// ---------------------------------------------------------------------------

// Normalized Xenos sampler state, directly convertible to a GPUSampler (same packing as the
// Vulkan backend's SamplerParameters).
union WebGPUSamplerParameters {
  uint32_t value;
  struct {
    xenos::ClampMode clamp_x : 3;         // 3
    xenos::ClampMode clamp_y : 3;         // 6
    xenos::ClampMode clamp_z : 3;         // 9
    xenos::BorderColor border_color : 2;  // 11
    uint32_t mag_linear : 1;              // 12
    uint32_t min_linear : 1;              // 13
    uint32_t mip_linear : 1;              // 14
    xenos::AnisoFilter aniso_filter : 3;  // 17
    uint32_t mip_min_level : 4;           // 21
    uint32_t mip_base_map : 1;            // 22
  };
  WebGPUSamplerParameters() : value(0) {}
  bool operator==(const WebGPUSamplerParameters& other) const { return value == other.value; }
};
static_assert(sizeof(WebGPUSamplerParameters) == sizeof(uint32_t));

// WebGPU has no border colors and no mirror-clamp address modes: border -> clamp-to-edge,
// mirror-clamp(-to-edge/-halfway/-border) -> mirror-repeat, clamp-to-halfway -> clamp-to-edge.
xenos::ClampMode NormalizeWebGPUClampMode(xenos::ClampMode clamp_mode);
// Fills `desc` (WGPU_SAMPLER_DESCRIPTOR_INIT first) for the normalized parameters. Anisotropy
// (maxAnisotropy > 1) is only used when all filters are linear, as WebGPU requires.
void MakeSamplerDescriptor(WebGPUSamplerParameters parameters, WGPUSamplerDescriptor& desc);

}  // namespace rex::graphics::webgpu
