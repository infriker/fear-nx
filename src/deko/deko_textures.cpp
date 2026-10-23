// SPDX-License-Identifier: GPL-3.0-or-later
// F.E.A.R. Xenos texture decoding and ordered deko3d uploads.

#include "deko_textures.h"
#include "deko_profile.h"
#include "deko_texture_layout.h"
#include "deko_texture_reuse.h"
#include "deko_validation_cache.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>

#include <xxhash.h>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/system/xmemory.h>
#include <rex/ui/deko3d/nucleo.h>

namespace fear_nx::deko {

namespace xenos = rex::graphics::xenos;
namespace texture_util = rex::graphics::texture_util;
namespace texture_conversion = rex::graphics::texture_conversion;

namespace {

constexpr uint64_t kMaxCachedGuestTextureBytes = 256ull << 20;
constexpr uint64_t kEvictGuestTextureFrames = 600;

enum class Conversion : uint8_t {
  kCopy,
  kSwapRB565,
  kRGB5A1,
  kRGBA4,
  kR10G11B11ToRGBA16,
  kR11G11B10ToRGBA16,
  kCTX1,
  kDXT3A,
};

struct HostFormat {
  DkImageFormat unorm = DkImageFormat_None;
  DkImageFormat snorm = DkImageFormat_None;
  Conversion conversion = Conversion::kCopy;
  uint32_t host_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  bool compressed = false;
  uint32_t host_bytes_per_texel = 0;
};

constexpr uint32_t kSwizzleRRRR = xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR;
constexpr uint32_t kSwizzleRGGG = xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG;
constexpr uint32_t kSwizzleRGBB = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB;
constexpr uint32_t kSwizzleRGBA = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;

HostFormat GetHostFormat(xenos::TextureFormat format) {
  using F = xenos::TextureFormat;
  switch (format) {
    case F::k_8:
    case F::k_8_A:
    case F::k_8_B:
      return {DkImageFormat_R8_Unorm, DkImageFormat_R8_Snorm, Conversion::kCopy, kSwizzleRRRR};
    case F::k_1_5_5_5:
      return {DkImageFormat_RGB5A1_Unorm, DkImageFormat_None, Conversion::kRGB5A1, kSwizzleRGBA};
    case F::k_5_6_5:
      return {DkImageFormat_RGB565_Unorm, DkImageFormat_None, Conversion::kSwapRB565, kSwizzleRGBB};
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_A:
    case F::k_8_8_8_8_AS_16_16_16_16:
      return {DkImageFormat_RGBA8_Unorm, DkImageFormat_RGBA8_Snorm, Conversion::kCopy, kSwizzleRGBA};
    case F::k_2_10_10_10:
    case F::k_2_10_10_10_AS_16_16_16_16:
      return {DkImageFormat_RGB10A2_Unorm, DkImageFormat_None, Conversion::kCopy, kSwizzleRGBA};
    case F::k_8_8:
      return {DkImageFormat_RG8_Unorm, DkImageFormat_RG8_Snorm, Conversion::kCopy, kSwizzleRGGG};
    case F::k_4_4_4_4:
      return {DkImageFormat_RGBA4_Unorm, DkImageFormat_None, Conversion::kRGBA4, kSwizzleRGBA};
    case F::k_10_11_11:
    case F::k_10_11_11_AS_16_16_16_16:
      return {DkImageFormat_RGBA16_Unorm, DkImageFormat_None,
              Conversion::kR10G11B11ToRGBA16, kSwizzleRGBB, false, 8};
    case F::k_11_11_10:
    case F::k_11_11_10_AS_16_16_16_16:
      return {DkImageFormat_RGBA16_Unorm, DkImageFormat_None,
              Conversion::kR11G11B10ToRGBA16, kSwizzleRGBB, false, 8};
    case F::k_DXT1:
    case F::k_DXT1_AS_16_16_16_16:
      return {DkImageFormat_RGBA_BC1, DkImageFormat_None, Conversion::kCopy, kSwizzleRGBA, true};
    case F::k_DXT2_3:
    case F::k_DXT2_3_AS_16_16_16_16:
      return {DkImageFormat_RGBA_BC2, DkImageFormat_None, Conversion::kCopy, kSwizzleRGBA, true};
    case F::k_DXT4_5:
    case F::k_DXT4_5_AS_16_16_16_16:
      return {DkImageFormat_RGBA_BC3, DkImageFormat_None, Conversion::kCopy, kSwizzleRGBA, true};
    case F::k_DXN:
      return {DkImageFormat_RG_BC5_Unorm, DkImageFormat_RG_BC5_Snorm, Conversion::kCopy,
              kSwizzleRGGG, true};
    case F::k_DXT5A:
      return {DkImageFormat_R_BC4_Unorm, DkImageFormat_R_BC4_Snorm, Conversion::kCopy,
              kSwizzleRRRR, true};
    case F::k_DXT3A:
      return {DkImageFormat_RGBA_BC2, DkImageFormat_None, Conversion::kDXT3A,
              XE_GPU_MAKE_TEXTURE_SWIZZLE(A, A, A, A), true};
    case F::k_CTX1:
      return {DkImageFormat_RG8_Unorm, DkImageFormat_None, Conversion::kCTX1, kSwizzleRGGG,
              false, 2};
    case F::k_16:
      return {DkImageFormat_R16_Unorm, DkImageFormat_R16_Snorm, Conversion::kCopy, kSwizzleRRRR};
    case F::k_16_16:
      return {DkImageFormat_RG16_Unorm, DkImageFormat_RG16_Snorm, Conversion::kCopy, kSwizzleRGGG};
    case F::k_16_16_16_16:
      return {DkImageFormat_RGBA16_Unorm, DkImageFormat_RGBA16_Snorm, Conversion::kCopy, kSwizzleRGBA};
    case F::k_16_EXPAND:
    case F::k_16_FLOAT:
      return {DkImageFormat_R16_Float, DkImageFormat_R16_Float, Conversion::kCopy, kSwizzleRRRR};
    case F::k_16_16_EXPAND:
    case F::k_16_16_FLOAT:
      return {DkImageFormat_RG16_Float, DkImageFormat_RG16_Float, Conversion::kCopy, kSwizzleRGGG};
    case F::k_16_16_16_16_EXPAND:
    case F::k_16_16_16_16_FLOAT:
      return {DkImageFormat_RGBA16_Float, DkImageFormat_RGBA16_Float, Conversion::kCopy, kSwizzleRGBA};
    case F::k_32_FLOAT:
      return {DkImageFormat_R32_Float, DkImageFormat_R32_Float, Conversion::kCopy, kSwizzleRRRR};
    case F::k_32_32_FLOAT:
      return {DkImageFormat_RG32_Float, DkImageFormat_RG32_Float, Conversion::kCopy, kSwizzleRGGG};
    case F::k_32_32_32_32_FLOAT:
      return {DkImageFormat_RGBA32_Float, DkImageFormat_RGBA32_Float, Conversion::kCopy, kSwizzleRGBA};
    default:
      return {};
  }
}

inline uint32_t SwapXor(xenos::Endian endian) {
  switch (endian) {
    case xenos::Endian::k8in16:
      return 1;
    case xenos::Endian::k8in32:
      return 3;
    case xenos::Endian::k16in32:
      return 2;
    default:
      return 0;
  }
}

void ConvertBlock(Conversion conversion, xenos::Endian endian, uint8_t* out, const uint8_t* in,
                  uint32_t guest_bytes, uint32_t out_row_pitch) {
  switch (conversion) {
    case Conversion::kCopy:
      if (guest_bytes < 4 && endian != xenos::Endian::kNone) {
        const uintptr_t xor_mask = SwapXor(endian);
        for (uint32_t i = 0; i < guest_bytes; ++i) {
          out[i] = *reinterpret_cast<const uint8_t*>((reinterpret_cast<uintptr_t>(in) + i) ^ xor_mask);
        }
      } else {
        texture_conversion::CopySwapBlock(endian, out, in, guest_bytes);
      }
      break;
    case Conversion::kSwapRB565: {
      uint16_t v;
      texture_conversion::CopySwapBlock(endian, &v, in, 2);
      v = uint16_t(((v & 0x1F) << 11) | (v & 0x7E0) | (v >> 11));
      std::memcpy(out, &v, sizeof(v));
    } break;
    case Conversion::kRGB5A1: {
      uint16_t v;
      texture_conversion::CopySwapBlock(endian, &v, in, 2);
      const uint32_t r = v & 0x1F;
      const uint32_t g = (v >> 5) & 0x1F;
      const uint32_t b = (v >> 10) & 0x1F;
      const uint32_t a = v >> 15;
      const uint16_t packed = uint16_t((r << 11) | (g << 6) | (b << 1) | a);
      std::memcpy(out, &packed, sizeof(packed));
    } break;
    case Conversion::kRGBA4: {
      uint16_t v;
      texture_conversion::CopySwapBlock(endian, &v, in, 2);
      const uint32_t r = v & 0xF;
      const uint32_t g = (v >> 4) & 0xF;
      const uint32_t b = (v >> 8) & 0xF;
      const uint32_t a = v >> 12;
      const uint16_t packed = uint16_t((r << 12) | (g << 8) | (b << 4) | a);
      std::memcpy(out, &packed, sizeof(packed));
    } break;
    case Conversion::kR10G11B11ToRGBA16:
    case Conversion::kR11G11B10ToRGBA16: {
      uint32_t v;
      texture_conversion::CopySwapBlock(endian, &v, in, sizeof(v));
      uint32_t r, g, b;
      uint16_t rgba[4];
      if (conversion == Conversion::kR10G11B11ToRGBA16) {
        r = v & 0x3FF;
        g = (v >> 10) & 0x7FF;
        b = v >> 21;
        rgba[0] = uint16_t(r * 65535 / 0x3FF);
        rgba[1] = uint16_t(g * 65535 / 0x7FF);
        rgba[2] = uint16_t(b * 65535 / 0x7FF);
      } else {
        r = v & 0x7FF;
        g = (v >> 11) & 0x7FF;
        b = v >> 22;
        rgba[0] = uint16_t(r * 65535 / 0x7FF);
        rgba[1] = uint16_t(g * 65535 / 0x7FF);
        rgba[2] = uint16_t(b * 65535 / 0x3FF);
      }
      rgba[3] = 0xFFFF;
      std::memcpy(out, rgba, sizeof(rgba));
    } break;
    case Conversion::kCTX1:
      texture_conversion::ConvertTexelCTX1ToR8G8(endian, out, in, out_row_pitch);
      break;
    case Conversion::kDXT3A:
      texture_conversion::ConvertTexelDXT3AToDXT3(endian, out, in, 16);
      break;
  }
}

uint32_t GuestToHostSwizzle(uint32_t guest_swizzle, uint32_t host_format_swizzle) {
  uint32_t host_swizzle = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t guest_component = (guest_swizzle >> (3 * i)) & 7;
    const uint32_t host_component =
        guest_component >= xenos::XE_GPU_TEXTURE_SWIZZLE_0
            ? (guest_component & 0b101)
            : ((host_format_swizzle >> (3 * guest_component)) & 7);
    host_swizzle |= host_component << (3 * i);
  }
  return host_swizzle;
}

DkImageSwizzle ToDkSwizzle(uint32_t swizzle, uint32_t component) {
  switch ((swizzle >> (3 * component)) & 7) {
    case 0:
      return DkImageSwizzle_Red;
    case 1:
      return DkImageSwizzle_Green;
    case 2:
      return DkImageSwizzle_Blue;
    case 3:
      return DkImageSwizzle_Alpha;
    case 5:
      return DkImageSwizzle_One;
    default:
      return DkImageSwizzle_Zero;
  }
}

struct LevelLayout {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth = 1;
  uint32_t blocks_x = 0;
  uint32_t blocks_y = 0;
  uint32_t host_row_bytes = 0;
  uint32_t host_rows = 0;
  uint64_t offset = 0;
  uint64_t layer_bytes = 0;
};

struct UntileJob {
  std::array<LevelLayout, xenos::kTextureMaxMips> levels{};
  uint32_t level_count = 0;
  uint32_t layers = 1;
  uint32_t block_width = 1;
  uint32_t block_height = 1;
  uint32_t guest_bytes_per_block = 0;
  uint32_t bytes_per_block_log2 = 0;
  uint32_t host_texel_bytes = 0;
  uint32_t host_bytes_per_block = 0;
  uint64_t base_size = 0;
  uint64_t mip_size = 0;
  texture_util::TextureGuestLayout layout{};
  const uint8_t* base_data = nullptr;
  const uint8_t* mip_data = nullptr;
  uint8_t* staging = nullptr;
  bool tiled = false;
  bool host_blocks = false;
  xenos::DataDimension dimension = xenos::DataDimension::k2DOrStacked;
  xenos::Endian endianness = xenos::Endian::kNone;
  xenos::TextureFormat format = xenos::TextureFormat::k_8;
  Conversion conversion = Conversion::kCopy;
};

void RunUntile(const UntileJob& job) {
  const bool is_3d = job.dimension == xenos::DataDimension::k3D;
  for (uint32_t level = 0; level < job.level_count; ++level) {
    const LevelLayout& out_level = job.levels[level];
    const bool packed = job.layout.packed_level != UINT32_MAX && level >= job.layout.packed_level;
    uint32_t stored_level = packed ? job.layout.packed_level : level;
    const texture_util::TextureGuestLayout::Level* guest_level;
    const uint8_t* level_source;
    if (!level) {
      guest_level = &job.layout.base;
      level_source = job.base_data;
    } else {
      guest_level = &job.layout.mips[stored_level];
      level_source = job.mip_data ? job.mip_data + job.layout.mip_offsets_bytes[stored_level] : nullptr;
    }

    uint32_t packed_x = 0, packed_y = 0, packed_z = 0;
    if (packed) {
      texture_util::GetPackedMipOffset(out_level.width ? job.levels[0].width : 1,
                                       out_level.height ? job.levels[0].height : 1,
                                       job.dimension == xenos::DataDimension::k3D
                                           ? job.levels[0].depth
                                           : 1,
                                       job.format, level, packed_x, packed_y, packed_z);
    }
    if (!level_source) {
      for (uint32_t slice = 0; slice < (is_3d ? out_level.depth : job.layers); ++slice) {
        std::memset(job.staging + out_level.offset + out_level.layer_bytes * slice, 0,
                    size_t(out_level.layer_bytes));
      }
      continue;
    }

    const uint32_t pitch_blocks = guest_level->row_pitch_bytes >> job.bytes_per_block_log2;
    const uint32_t slice_count = is_3d ? out_level.depth : job.layers;
    const uint32_t run_blocks = job.bytes_per_block_log2 == 0
                                    ? 8
                                    : (16u >> std::min(job.bytes_per_block_log2, 4u));
    const bool run_copy = job.tiled && !is_3d && job.conversion == Conversion::kCopy &&
                          run_blocks > 1 &&
                          (job.host_blocks || job.block_width * job.host_texel_bytes ==
                                                  job.guest_bytes_per_block);
    for (uint32_t slice = 0; slice < slice_count; ++slice) {
      uint8_t* out_slice = job.staging + out_level.offset + out_level.layer_bytes * slice;
      const uint64_t slice_offset =
          is_3d ? uint64_t(slice) * guest_level->row_pitch_bytes *
                      guest_level->z_slice_stride_block_rows
                : uint64_t(slice) * guest_level->array_slice_stride_bytes;
      for (uint32_t by = 0; by < out_level.blocks_y; ++by) {
        const uint32_t gy = by + packed_y;
        uint8_t* out_row = out_slice + uint64_t(by) * out_level.host_row_bytes;
        if (!job.tiled && !is_3d && job.conversion == Conversion::kCopy &&
            (job.host_blocks || job.block_width * job.host_texel_bytes ==
                                    job.guest_bytes_per_block)) {
          const uint64_t row_bytes = uint64_t(out_level.blocks_x) * job.guest_bytes_per_block;
          const uint64_t src_offset = slice_offset + uint64_t(gy) * guest_level->row_pitch_bytes +
                                      uint64_t(packed_x) * job.guest_bytes_per_block;
          const uintptr_t src_address = reinterpret_cast<uintptr_t>(level_source + src_offset);
          if (src_offset + row_bytes <= (level ? job.mip_size : job.base_size) &&
              (job.endianness == xenos::Endian::kNone ||
               (!(src_address & 3) && !(row_bytes & 3)))) {
            texture_conversion::CopySwapBlock(job.endianness, out_row, level_source + src_offset,
                                              size_t(row_bytes));
            continue;
          }
        }
        for (uint32_t bx = 0; bx < out_level.blocks_x; ++bx) {
          const uint32_t gx = bx + packed_x;
          if (run_copy && !(gx & (run_blocks - 1)) && bx + run_blocks <= out_level.blocks_x) {
            const uint64_t source = slice_offset + uint32_t(texture_util::GetTiledOffset2D(
                                                        int32_t(gx), int32_t(gy), pitch_blocks,
                                                        job.bytes_per_block_log2));
            const uint64_t bytes = uint64_t(run_blocks) * job.guest_bytes_per_block;
            if (source + bytes <= (level ? job.mip_size : job.base_size)) {
              texture_conversion::CopySwapBlock(
                  job.endianness, out_row + uint64_t(bx) * job.host_bytes_per_block,
                  level_source + source, size_t(bytes));
              bx += run_blocks - 1;
              continue;
            }
          }
          uint64_t source_offset;
          if (job.tiled) {
            source_offset = is_3d
                                ? uint32_t(texture_util::GetTiledOffset3D(
                                      int32_t(gx), int32_t(gy), int32_t(slice + packed_z),
                                      pitch_blocks, guest_level->y_extent_blocks,
                                      job.bytes_per_block_log2))
                                : uint32_t(texture_util::GetTiledOffset2D(
                                      int32_t(gx), int32_t(gy), pitch_blocks,
                                      job.bytes_per_block_log2));
            source_offset += is_3d ? 0 : slice_offset;
          } else {
            source_offset = slice_offset + uint64_t(gy) * guest_level->row_pitch_bytes +
                            uint64_t(gx) * job.guest_bytes_per_block;
          }
          if (source_offset + job.guest_bytes_per_block >
              (level ? job.mip_size : job.base_size)) {
            continue;
          }
          uint8_t* out_block = job.host_blocks
                                   ? out_row + uint64_t(bx) * job.host_bytes_per_block
                                   : out_row + uint64_t(bx) * job.block_width * job.host_texel_bytes;
          ConvertBlock(job.conversion, job.endianness, out_block, level_source + source_offset,
                       job.guest_bytes_per_block, out_level.host_row_bytes);
        }
      }
    }
  }
}

DkWrapMode GetWrapMode(xenos::ClampMode mode) {
  switch (mode) {
    case xenos::ClampMode::kRepeat:
      return DkWrapMode_Repeat;
    case xenos::ClampMode::kMirroredRepeat:
      return DkWrapMode_MirroredRepeat;
    case xenos::ClampMode::kClampToEdge:
      return DkWrapMode_ClampToEdge;
    case xenos::ClampMode::kMirrorClampToEdge:
      return DkWrapMode_MirrorClampToEdge;
    case xenos::ClampMode::kClampToHalfway:
      return DkWrapMode_Clamp;
    case xenos::ClampMode::kMirrorClampToHalfway:
      return DkWrapMode_MirrorClamp;
    case xenos::ClampMode::kClampToBorder:
      return DkWrapMode_ClampToBorder;
    case xenos::ClampMode::kMirrorClampToBorder:
      return DkWrapMode_MirrorClampToBorder;
  }
  return DkWrapMode_ClampToEdge;
}

float GetAnisotropy(xenos::AnisoFilter filter) {
  switch (filter) {
    case xenos::AnisoFilter::kMax_1_1:
      return 1.0f;
    case xenos::AnisoFilter::kMax_2_1:
      return 2.0f;
    case xenos::AnisoFilter::kMax_4_1:
      return 4.0f;
    case xenos::AnisoFilter::kMax_8_1:
      return 8.0f;
    case xenos::AnisoFilter::kMax_16_1:
      return 16.0f;
    default:
      return 1.0f;
  }
}

}  // namespace

struct TextureCache::Texture {
  TextureKey key{};
  uint64_t layout_hash = 0;
  uint64_t content_hash = 0;
  TextureChecks::State check_state{};
  GuestMemoryLease guest_memory{};
  uint64_t last_frame = 0;
  Image image{};
};

bool TextureCache::BindingCurrent(const TextureResourceBinding& binding) const {
  return BindingCurrent(binding.image_slot,binding.image_identity,binding.content_version) &&
      binding.guest_memory.Current(validation_cache_.guest_writes());
}

TextureCache::TextureCache(rex::deko3d::Nucleo& nucleo, rex::memory::Memory* memory,
                           ValidationCache& validation_cache)
    : nucleo_(nucleo), memory_(memory), validation_cache_(validation_cache) {
  retired_work_.resize(std::max<uint32_t>(nucleo_.config().ranuras, 1));
}

TextureCache::~TextureCache() { Shutdown(); }

bool TextureCache::Initialize() {
  if (!memory_ || !nucleo_.TrabajoAbierto()) {
    return false;
  }
  auto make_null = [&](DkImageType type, uint32_t layers, uint32_t slot) -> bool {
    auto created = CreateImage(DkImageFormat_RGBA8_Unorm, 1, 1, type, layers, 1);
    if (!created) {
      return false;
    }
    auto image = std::make_unique<Image>(std::move(*created));
    const uint32_t upload_bytes = layers * 4;
    const rex::deko3d::Subida upload = nucleo_.Reservar(upload_bytes, 256);
    if (!upload) {
      needs_flush_retry_ = true;
      return false;
    }
    std::memset(upload.cpu, 0, upload_bytes);
    DkImageView view;
    dkImageViewDefaults(&view, &image->image);
    const DkCopyBuf source = {upload.gpu, 0, 0};
    const DkImageRect rect = {0, 0, 0, 1, 1, layers};
    dkCmdBufCopyBufferToImage(nucleo_.comandos(), &source, &view, &rect, 0);
    {
      ProfileScope descriptor_scope(profile_.descriptor_ns, profiling_enabled_);
      if (profiling_enabled_) {
        ++profile_.descriptor_calls;
      }
      DkImageDescriptor descriptor;
      dkImageDescriptorInitialize(&descriptor, &view, false, false);
      dkCmdBufPushData(nucleo_.comandos(),
                       nucleo_.DescriptoresImagen() + uint64_t(slot) * sizeof(descriptor),
                       &descriptor, sizeof(descriptor));
    }
    descriptor_versions_.Bind(slot,image->identity,image->content_version);
    image->descriptor_slots.push_back(slot);
    null_images_.push_back(std::move(image));
    return true;
  };

  if (!make_null(DkImageType_2DArray, 1, 0) || !make_null(DkImageType_3D, 1, 1) ||
      !make_null(DkImageType_Cubemap, 6, 2)) {
    return false;
  }
  DkSampler sampler;
  dkSamplerDefaults(&sampler);
  sampler.minFilter = DkFilter_Nearest;
  sampler.magFilter = DkFilter_Nearest;
  sampler.mipFilter = DkMipFilter_None;
  sampler.wrapMode[0] = sampler.wrapMode[1] = sampler.wrapMode[2] = DkWrapMode_ClampToEdge;
  {
    ProfileScope sampler_scope(profile_.sampler_ns, profiling_enabled_);
    if (profiling_enabled_) {
      ++profile_.sampler_calls;
    }
    DkSamplerDescriptor sampler_descriptor;
    dkSamplerDescriptorInitialize(&sampler_descriptor, &sampler);
    dkCmdBufPushData(nucleo_.comandos(), nucleo_.DescriptoresSampler(), &sampler_descriptor,
                     sizeof(sampler_descriptor));
  }
  dkCmdBufBarrier(nucleo_.comandos(), DkBarrier_Full,
                  DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
  return true;
}

void TextureCache::Shutdown() {
  // Also invalidate candidate pointers if shutdown has no open work (that
  // branch waits for the GPU without calling FreeImage on each texture).
  ++binding_cache_epoch_;
  ++image_generation_;
  if (nucleo_.TrabajoAbierto()) {
    for (auto& [hash, texture] : textures_) {
      FreeImage(texture->image);
    }
    for (auto& image : null_images_) {
      if (image) FreeImage(*image);
    }
  } else if (!textures_.empty() || !null_images_.empty()) {
    nucleo_.EsperarGpu();
  }
  textures_.clear();
  last_aged_frame_ = UINT64_MAX;
  cached_texture_bytes_ = 0;
  null_images_.clear();
  samplers_.clear();
}

void TextureCache::BeginWork(uint64_t work) {
  work_ = work;
  needs_flush_retry_ = false;
  RetiredWork& retired = retired_work_[work % retired_work_.size()];
  free_image_slots_.insert(free_image_slots_.end(), retired.descriptor_slots.begin(),
                           retired.descriptor_slots.end());
  retired.descriptor_slots.clear();
}

void TextureCache::BeginFrame(uint64_t frame) {
  frame_ = frame;
  if (!nucleo_.TrabajoAbierto()) {
    return;
  }

  // Match the Vulkan cache's periodic 600-frame aging policy. FreeImage queues
  // allocations and descriptors on this open work slot, so prior GPU uses
  // finish before either can be reclaimed. Work-slot retries can reach this
  // function repeatedly in one frame. Inserts and touches use frame_, so no
  // additional entry can become old enough until the frame changes.
  if (frame % 60 == 0 && last_aged_frame_ != frame) {
    last_aged_frame_ = frame;
    for (auto it = textures_.begin(); it != textures_.end();) {
      Texture& texture = *it->second;
      if (frame > texture.last_frame && frame - texture.last_frame > kEvictGuestTextureFrames) {
        cached_texture_bytes_ -= texture.image.allocation.bytes;
        FreeImage(texture.image);
        it = textures_.erase(it);
      } else {
        ++it;
      }
    }
  }

  // Keep guest texture residency bounded. Evict the least recently used
  // entries at frame boundaries only; textures already used in this frame
  // stay alive until the next boundary. If the frame itself consumes more
  // than the target, the cache can exceed it until those entries become safe.
  while (cached_texture_bytes_ > kMaxCachedGuestTextureBytes) {
    auto oldest = textures_.end();
    for (auto it = textures_.begin(); it != textures_.end(); ++it) {
      if (it->second->last_frame >= frame) {
        continue;
      }
      if (oldest == textures_.end() || it->second->last_frame < oldest->second->last_frame) {
        oldest = it;
      }
    }
    if (oldest == textures_.end()) {
      break;
    }
    cached_texture_bytes_ -= oldest->second->image.allocation.bytes;
    FreeImage(oldest->second->image);
    textures_.erase(oldest);
  }
}

void TextureCache::Touch(Image& image, uint64_t work) {
  if (image.valid) {
    image.last_used_work = std::max(image.last_used_work, work);
  }
}

std::optional<Image> TextureCache::CreateImage(DkImageFormat format, uint32_t width,
                                                uint32_t height, DkImageType type,
                                                uint32_t layers, uint32_t mip_levels,
                                                uint32_t flags) {
  ProfileScope image_alloc_scope(profile_.image_alloc_ns, profiling_enabled_);
  if (profiling_enabled_) {
    ++profile_.image_alloc_calls;
  }
  if (format == DkImageFormat_None || !width || !height || !layers || !mip_levels ||
      !nucleo_.TrabajoAbierto()) {
    return std::nullopt;
  }
  DkImageLayoutMaker maker;
  dkImageLayoutMakerDefaults(&maker, nucleo_.dispositivo());
  maker.type = type;
  maker.flags = flags;
  maker.format = format;
  maker.dimensions[0] = width;
  maker.dimensions[1] = height;
  maker.dimensions[2] = layers;
  maker.mipLevels = mip_levels;
  if (type != DkImageType_3D) {
    const uint32_t rows = (format == DkImageFormat_RGBA_BC1 || format == DkImageFormat_RGBA_BC2 ||
                           format == DkImageFormat_RGBA_BC3 || format == DkImageFormat_R_BC4_Unorm ||
                           format == DkImageFormat_R_BC4_Snorm ||
                           format == DkImageFormat_RG_BC5_Unorm ||
                           format == DkImageFormat_RG_BC5_Snorm)
                              ? (height + 3) / 4
                              : height;
    rex::deko3d::ArreglarTamanoDeTesela(maker, rows);
  }
  DkImageLayout layout;
  dkImageLayoutInitialize(&layout, &maker);
  const uint64_t size = dkImageLayoutGetSize(&layout);
  const uint32_t alignment = dkImageLayoutGetAlignment(&layout);
  if (!size || size > std::numeric_limits<uint32_t>::max() || !alignment) {
    return std::nullopt;
  }
  const rex::deko3d::BloqueImagen allocation =
      nucleo_.ReservarImagen(uint32_t(size), alignment);
  if (!allocation) {
    needs_flush_retry_ = true;
    return std::nullopt;
  }
  Image image;
  image.format = format;
  image.type = type;
  image.flags = flags;
  image.width = width;
  image.height = height;
  image.layers = layers;
  image.mip_levels = mip_levels;
  image.allocation = allocation;
  image.valid = true;
  image.identity=descriptor_versions_.NewImage();
  dkImageInitialize(&image.image, &layout, allocation.bloque, allocation.offset);
  return image;
}

void TextureCache::FreeImage(Image& image) {
  ++image_generation_; // Invalidate memoized pointers before any image retirement.
  if (!image.valid) {
    return;
  }
  if (image.allocation && nucleo_.TrabajoAbierto()) {
    nucleo_.LiberarImagenLuego(image.allocation);
  }
  for (uint32_t slot : image.descriptor_slots) {
    descriptor_versions_.Retire(slot);
    if (slot >= 3) {
      RetireDescriptor(slot);
    }
  }
  image.descriptor_slots.clear();
  image.views.clear();
  image.valid = false;
}

uint32_t TextureCache::AllocateImageDescriptor() {
  if (!free_image_slots_.empty()) {
    const uint32_t slot = free_image_slots_.back();
    free_image_slots_.pop_back();
    return slot;
  }
  if (next_image_slot_ >= nucleo_.config().descriptores_imagen) {
    needs_flush_retry_ = true;
    return UINT32_MAX;
  }
  return next_image_slot_++;
}

void TextureCache::RetireDescriptor(uint32_t slot) {
  if (slot >= 3) {
    retired_work_[work_ % retired_work_.size()].descriptor_slots.push_back(slot);
  }
}

std::optional<uint32_t> TextureCache::GetImageView(Image& image, uint32_t guest_swizzle,
                                                   DkImageType view_type,
                                                   DkDsSource ds_source,
                                                   bool uses_load_store,
                                                   DkImageFormat format_override,
                                                   bool apply_host_swizzle) {
  if (!image.valid) {
    return std::nullopt;
  }
  if (view_type == DkImageType_None) {
    view_type = image.type;
  }
  // Storage copies must not bake a guest resolve's RB copy_dest_swap into pixels.
  const uint32_t swizzle = GuestToHostSwizzle(guest_swizzle, apply_host_swizzle ?
      image.host_format_swizzle : xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA);
  const uint32_t view_key = (swizzle & 0xFFFu) | (uint32_t(view_type) << 12) |
                            (uint32_t(ds_source) << 20) | (uint32_t(uses_load_store) << 21) |
                            ((uint32_t(format_override) & 0x3FFu) << 22);
  auto it = image.views.find(view_key);
  if (it != image.views.end()) {
    Touch(image, work_);
    return it->second;
  }
  const uint32_t slot = AllocateImageDescriptor();
  if (slot == UINT32_MAX) {
    return std::nullopt;
  }
  ProfileScope descriptor_scope(profile_.descriptor_ns, profiling_enabled_);
  if (profiling_enabled_) {
    ++profile_.descriptor_calls;
  }
  DkImageView view;
  dkImageViewDefaults(&view, &image.image);
  view.type = view_type;
  view.format = format_override;
  view.dsSource = ds_source;
  for (uint32_t component = 0; component < 4; ++component) {
    view.swizzle[component] = ToDkSwizzle(swizzle, component);
  }
  DkImageDescriptor descriptor;
  dkImageDescriptorInitialize(&descriptor, &view, uses_load_store, false);
  dkCmdBufPushData(nucleo_.comandos(),
                   nucleo_.DescriptoresImagen() + uint64_t(slot) * sizeof(descriptor),
                   &descriptor, sizeof(descriptor));
  dkCmdBufBarrier(nucleo_.comandos(), DkBarrier_Full, DkInvalidateFlags_Descriptors);
  image.views.emplace(view_key, slot);
  descriptor_versions_.Bind(slot,image.identity,image.content_version);
  image.descriptor_slots.push_back(slot);
  Touch(image, work_);
  return slot;
}

std::optional<uint32_t> TextureCache::GetTextureImageView(
    Image& image, const xenos::xe_gpu_texture_fetch_t& fetch, bool is_signed,
    DkImageType view_type) {
  const HostFormat host_format = GetHostFormat(rex::graphics::GetBaseFormat(fetch.format));
  if (host_format.conversion != Conversion::kCopy || host_format.compressed ||
      host_format.unorm == DkImageFormat_None ||
      (image.format != host_format.unorm && image.format != host_format.snorm)) {
    return GetImageView(image, fetch.swizzle, view_type);
  }

  const DkImageFormat requested_format =
      is_signed && host_format.snorm != DkImageFormat_None ? host_format.snorm : host_format.unorm;
  const DkImageFormat format_override = requested_format == image.format
                                            ? DkImageFormat_None
                                            : requested_format;
  // Keep the image's host swizzle: resolve images may already encode the
  // destination channel order (including copy_dest_swap).
  const auto descriptor =
      GetImageView(image, fetch.swizzle, view_type, DkDsSource_Depth, false, format_override);
  return descriptor;
}

std::optional<uint32_t> TextureCache::GetSampler(
    const xenos::xe_gpu_texture_fetch_t& fetch,
    const rex::graphics::SpirvShader::SamplerBinding& binding, bool force_point) {
  xenos::ClampMode clamp_x, clamp_y, clamp_z;
  texture_util::GetClampModesForDimension(fetch, clamp_x, clamp_y, clamp_z);
  xenos::TextureFilter mag = binding.mag_filter == xenos::TextureFilter::kUseFetchConst
                                 ? fetch.mag_filter
                                 : binding.mag_filter;
  xenos::TextureFilter min = binding.min_filter == xenos::TextureFilter::kUseFetchConst
                                 ? fetch.min_filter
                                 : binding.min_filter;
  xenos::TextureFilter mip = binding.mip_filter == xenos::TextureFilter::kUseFetchConst
                                 ? fetch.mip_filter
                                 : binding.mip_filter;
  xenos::AnisoFilter aniso = binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst
                                 ? fetch.aniso_filter
                                 : binding.aniso_filter;
  if (force_point) {
    mag = min = xenos::TextureFilter::kPoint;
    mip = fetch.mip_max_level > fetch.mip_min_level ? xenos::TextureFilter::kPoint
                                                     : xenos::TextureFilter::kBaseMap;
    aniso = xenos::AnisoFilter::kDisabled;
  }
  bool mag_linear = mag == xenos::TextureFilter::kLinear;
  bool min_linear = min == xenos::TextureFilter::kLinear;
  bool mip_linear = mip == xenos::TextureFilter::kLinear;
  const bool mip_base_map = mip == xenos::TextureFilter::kBaseMap;
  if (aniso != xenos::AnisoFilter::kDisabled) {
    // Xenos anisotropic fetch forces linear filtering on each active axis.
    mag_linear = true;
    min_linear = true;
    mip_linear = true;
  }
  uint32_t mip_min = 0, mip_max = 0;
  texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, nullptr, nullptr,
                                                &mip_min, &mip_max);
  uint32_t key_words[6] = {
      uint32_t(clamp_x) | (uint32_t(clamp_y) << 3) | (uint32_t(clamp_z) << 6),
      uint32_t(mag_linear) | (uint32_t(min_linear) << 1) | (uint32_t(mip_linear) << 2) |
          (uint32_t(mip_base_map) << 3),
      uint32_t(aniso), uint32_t(fetch.border_color), mip_min | (mip_max << 8),
      uint32_t(fetch.lod_bias) & 0x3FFu};
  const uint64_t key = XXH3_64bits(key_words, sizeof(key_words));
  auto found = samplers_.find(key);
  if (found != samplers_.end()) {
    return found->second;
  }
  if (next_sampler_slot_ >= nucleo_.config().descriptores_sampler) {
    REXLOG_WARN("[deko] Sampler descriptor table exhausted ({})",
                nucleo_.config().descriptores_sampler);
    return std::nullopt;
  }

  ProfileScope sampler_scope(profile_.sampler_ns, profiling_enabled_);
  if (profiling_enabled_) {
    ++profile_.sampler_calls;
  }
  DkSampler sampler;
  dkSamplerDefaults(&sampler);
  sampler.magFilter = mag_linear ? DkFilter_Linear : DkFilter_Nearest;
  sampler.minFilter = min_linear ? DkFilter_Linear : DkFilter_Nearest;
  sampler.mipFilter = mip_base_map ? DkMipFilter_None
                                  : (mip_linear ? DkMipFilter_Linear : DkMipFilter_Nearest);
  sampler.wrapMode[0] = GetWrapMode(clamp_x);
  sampler.wrapMode[1] = GetWrapMode(clamp_y);
  sampler.wrapMode[2] = GetWrapMode(clamp_z);
  sampler.lodClampMin = float(mip_min);
  sampler.lodClampMax = mip_base_map ? float(mip_min) + 0.25f : float(mip_max);
  sampler.lodBias = float(fetch.lod_bias) / 32.0f;
  sampler.maxAnisotropy = GetAnisotropy(aniso);
  if (xenos::ClampModeUsesBorder(clamp_x) || xenos::ClampModeUsesBorder(clamp_y) ||
      xenos::ClampModeUsesBorder(clamp_z)) {
    const bool white = fetch.border_color == xenos::BorderColor::k_ABGR_White;
    for (uint32_t c = 0; c < 4; ++c) {
      sampler.borderColor[c].value_f = white ? 1.0f : 0.0f;
    }
  }
  const uint32_t slot = next_sampler_slot_++;
  DkSamplerDescriptor descriptor;
  dkSamplerDescriptorInitialize(&descriptor, &sampler);
  dkCmdBufPushData(nucleo_.comandos(),
                   nucleo_.DescriptoresSampler() + uint64_t(slot) * sizeof(descriptor),
                   &descriptor, sizeof(descriptor));
  dkCmdBufBarrier(nucleo_.comandos(), DkBarrier_Full, DkInvalidateFlags_Descriptors);
  samplers_.emplace(key, slot);
  return slot;
}

std::optional<TextureResourceBinding> TextureCache::GetTexture(
    const xenos::xe_gpu_texture_fetch_t& fetch, xenos::FetchOpDimension dimension,
    bool is_signed, const rex::graphics::SpirvShader::SamplerBinding& sampler_binding,
    const xenos::xe_gpu_texture_fetch_t* sampler_fetch) {
  ProfileScope get_texture_scope(profile_.get_texture_ns, profiling_enabled_);
  if (profiling_enabled_) {
    ++profile_.get_texture_calls;
  }
  BindingMemo<MemoBinding,18>::Key memo_key{};
  static_assert(sizeof(fetch)==6*sizeof(uint32_t));
  std::memcpy(memo_key.data(),&fetch,sizeof(fetch));
  std::memcpy(memo_key.data()+6,sampler_fetch?sampler_fetch:&fetch,sizeof(fetch));
  memo_key[12]=uint32_t(dimension);memo_key[13]=uint32_t(is_signed);
  memo_key[14]=uint32_t(sampler_binding.mag_filter);memo_key[15]=uint32_t(sampler_binding.min_filter);
  memo_key[16]=uint32_t(sampler_binding.mip_filter);memo_key[17]=uint32_t(sampler_binding.aniso_filter);
  const uint64_t memo_hash=BindingBucketHash(memo_key);
  auto memo_stamp=[&]() -> BindingMemo<MemoBinding,18>::Stamp {
    return {binding_cache_epoch_,0,0};
  };
  auto check_contents=[&](TextureChecks::State& state,uint64_t layout_hash,
                           const GuestMemoryLease& lease) {
    const auto* base=lease.base?memory_->TranslatePhysical<const uint8_t*>(lease.base):nullptr;
    const auto* mips=lease.mips?memory_->TranslatePhysical<const uint8_t*>(lease.mips):nullptr;
    TextureChecks::Result checked;
    {
      ProfileScope content_hash_scope(profile_.content_hash_ns,profiling_enabled_);
      checked=texture_checks_.Check(state,frame_,lease.base,layout_hash,
          {base,size_t(lease.base_bytes)},{mips,size_t(lease.mip_bytes)},validation_cache_.enabled(),
          fetch.dimension!=xenos::DataDimension::k3D);
      if(profiling_enabled_){++profile_.content_hash_calls;profile_.content_hash_bytes+=checked.bytes;}
    }
    if(checked.guard_miss)
      REXLOG_WARN("[deko-texture-check] guard miss base={:08X}; full checks stay scheduled; excluded bases={}, sampling disabled={}",
          lease.base,texture_checks_.stats().excluded_bases,texture_checks_.stats().sampling_disabled);
    return checked;
  };
  if(const auto* memo=bindings_.Find(memo_hash,memo_key,memo_stamp());
      validation_cache_.enabled() && memo && BindingCurrent(memo->binding)) {
    // The live descriptor and guest lease precede this raw-node dereference.
    // Like NFSMW's valido_hasta, the texture's OWN check schedule decides how
    // long its exact fetch answer lasts. A geometry epoch/frame boundary still
    // reaches TextureChecks, but no longer rebuilds an unchanged layout/view/
    // sampler. A due check or known overlapping write takes the normal path.
    // Resolved images are selected by Renderer::Texture before this function.
    Texture& texture=*memo->texture;
    if(detail::TextureCheckReusable(texture.check_state,frame_)) {
      // The predicate is exactly Check's already-validated frame/deadline
      // branch. It cannot read guest bytes or change the digest. Avoid address
      // translation, state copying and nested clocks only for that branch.
      // BindingCurrent above still rejects known writes and retired images;
      // a due check below retains hashing, guards and the original schedule.
      memo->validation_epoch=validation_cache_.memo_epoch();memo->frame=frame_;
        // Carry forward the overlap proof, so an actively used texture does
        // not retain an obsolete write-journal serial until its next hash.
        texture.guest_memory.version=memo->binding.guest_memory.version;
        texture.last_frame=frame_;Touch(texture.image,work_);
        if(profiling_enabled_){++profile_.binding_hits;++profile_.scheduled_binding_reuses;}
        return memo->binding;
    }
  }
  if(profiling_enabled_)++profile_.binding_misses;
  const auto null_binding = [&]() -> std::optional<TextureResourceBinding> {
    const uint32_t null_slot =
        dimension == xenos::FetchOpDimension::kCube
            ? null_image_slots_[2]
            : (dimension == xenos::FetchOpDimension::k3DOrStacked &&
                       fetch.dimension == xenos::DataDimension::k3D
                   ? null_image_slots_[1]
                   : null_image_slots_[0]);
    const auto sampler_slot =
        GetSampler(sampler_fetch ? *sampler_fetch : fetch, sampler_binding);
    if (!sampler_slot) {
      return std::nullopt;
    }
    return TextureResourceBinding{null_slot, *sampler_slot, 1, 1, true,descriptor_versions_.Image(null_slot),1};
  };
  if (fetch.type != xenos::FetchConstantType::kTexture) {
    return null_binding();
  }

  uint32_t width_minus_1 = 0, height_minus_1 = 0, depth_minus_1 = 0;
  uint32_t base_page = 0, mip_page = 0, mip_min_level = 0, mip_max_level = 0;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1,
                                                 &depth_minus_1, &base_page, &mip_page,
                                                 &mip_min_level, &mip_max_level);
  if (!base_page && !mip_page) {
    return null_binding();
  }
  const uint32_t width = width_minus_1 + 1;
  const uint32_t height = height_minus_1 + 1;
  const uint32_t depth = depth_minus_1 + 1;
  DkImageType image_type = DkImageType_2DArray;
  uint32_t layers = depth;
  switch (fetch.dimension) {
    case xenos::DataDimension::k2DOrStacked:
      if ((fetch.stacked && dimension != xenos::FetchOpDimension::k3DOrStacked) ||
          (!fetch.stacked && dimension != xenos::FetchOpDimension::k2D)) {
        return std::nullopt;
      }
      image_type = DkImageType_2DArray;
      break;
    case xenos::DataDimension::k3D:
      if (dimension != xenos::FetchOpDimension::k3DOrStacked) {
        return std::nullopt;
      }
      image_type = DkImageType_3D;
      layers = depth;
      break;
    case xenos::DataDimension::kCube:
      if (dimension != xenos::FetchOpDimension::kCube) {
        return std::nullopt;
      }
      image_type = DkImageType_Cubemap;
      layers = 6;
      break;
    case xenos::DataDimension::k1D:
      REXLOG_WARN("[deko] Unsupported Xenos 1D texture fetch");
      return std::nullopt;
  }
  if (!width || !height || !depth || width > 8192 || height > 8192 || depth > 2048) {
    return std::nullopt;
  }

  const xenos::TextureFormat format = rex::graphics::GetBaseFormat(fetch.format);
  const HostFormat host_format = GetHostFormat(format);
  const rex::graphics::FormatInfo* format_info = rex::graphics::FormatInfo::Get(format);
  if (host_format.unorm == DkImageFormat_None || !format_info) {
    REXLOG_WARN("[deko] Unsupported Xenos texture format {} (dimension {}, {}x{}x{})",
                uint32_t(format), uint32_t(fetch.dimension), width, height, depth);
    return std::nullopt;
  }
  const DkImageFormat image_format =
      is_signed && host_format.snorm != DkImageFormat_None ? host_format.snorm : host_format.unorm;

  TextureKey texture_key;
  texture_key.words = {base_page,
                       mip_page,
                       width,
                       height,
                       depth,
                       fetch.pitch,
                       mip_min_level,
                       mip_max_level,
                       uint32_t(format),
                       uint32_t(fetch.dimension),
                       uint32_t(dimension),
                       uint32_t(fetch.endianness),
                       uint32_t(fetch.tiled),
                       uint32_t(fetch.packed_mips),
                       uint32_t(fetch.stacked),
                       uint32_t(image_format)};
  uint64_t layout_hash = XXH3_64bits(texture_key.words.data(), sizeof(texture_key.words));
  auto [first, last] = textures_.equal_range(layout_hash);
  auto existing=last;
  for(auto it=first;it!=last;++it)if(it->second->key==texture_key){existing=it;break;}
  // Every GetGuestTextureLayout input is in the exact key above. Existing
  // images already have validated immutable base/mip extents in their lease;
  // only a new upload needs the full per-mip layout again. Content validation
  // below still runs for the same spans, including strict validation mode.
  auto build_layout=[&]() {
    return texture_util::GetGuestTextureLayout(fetch.dimension,fetch.pitch,width,height,depth,
        fetch.tiled,format,fetch.packed_mips,base_page!=0,mip_max_level);
  };
  texture_util::TextureGuestLayout layout{};
  if(existing==last)layout=build_layout();
  const uint64_t base_address=uint64_t(base_page)<<12;
  const uint64_t mip_address=uint64_t(mip_page)<<12;
  const uint64_t base_size=existing!=last?existing->second->guest_memory.base_bytes:
      (base_page?layout.base.level_data_extent_bytes:0);
  const uint64_t mip_size=existing!=last?existing->second->guest_memory.mip_bytes:
      (mip_page?layout.mips_total_extent_bytes:0);
  if(base_address+base_size>0x20000000ull || mip_address+mip_size>0x20000000ull)
    return std::nullopt;
  // Schedule belongs to this exact texture layout, not a physical-range memo
  // shared with vertex buffers. Keep it across content replacement, but never
  // publish a new digest/schedule without a successfully uploaded image.
  TextureChecks::State check_state=existing!=last?existing->second->check_state:TextureChecks::State{};
  if(existing!=last && !existing->second->guest_memory.Current(validation_cache_.guest_writes()))
    check_state={}; // A known overlapping write bypasses same-frame and scheduled reuse.
  const GuestMemoryLease guest_memory{uint32_t(base_address),uint32_t(base_size),
      uint32_t(mip_address),uint32_t(mip_size),validation_cache_.guest_writes().version()};
  const uint64_t content_hash=check_contents(check_state,layout_hash,guest_memory).hash;
  auto remember=[&](Texture& texture,TextureResourceBinding binding) {
    binding.image_identity=texture.image.identity;binding.content_version=texture.image.content_version;
    binding.guest_memory=texture.guest_memory;
    // Even excluded textures reuse their validated content until the next due
    // frame. Global debug validation-off still checks every request exactly.
    if(validation_cache_.enabled()) {
      binding.reusable_in_draw_cache=true;
      bindings_.Store(memo_hash,memo_key,memo_stamp(),
          {&texture,binding,validation_cache_.memo_epoch(),frame_});
    }
    return binding;
  };
  if(existing!=last) {
    Texture& texture=*existing->second;
    if(texture.content_hash==content_hash) {
      texture.check_state=check_state;
      texture.guest_memory=guest_memory;
      texture.last_frame=frame_;Touch(texture.image,work_);
      const auto image_slot=GetImageView(texture.image,fetch.swizzle,image_type);
      const auto sampler_slot=GetSampler(sampler_fetch?*sampler_fetch:fetch,sampler_binding);
      if(!image_slot||!sampler_slot)return std::nullopt;
      return remember(texture,TextureResourceBinding{*image_slot,*sampler_slot,width,height});
    }
    // The old extents were enough to validate contents; replacement now
    // needs the layout to untile/upload the changed bytes.
    layout=build_layout();
    cached_texture_bytes_-=texture.image.allocation.bytes;
    FreeImage(texture.image);textures_.erase(existing);
  }

  const uint32_t level_count = std::min(mip_max_level + 1, xenos::kTextureMaxMips);
  const uint32_t guest_bytes_per_block = format_info->bytes_per_block();
  if (!guest_bytes_per_block || (guest_bytes_per_block & (guest_bytes_per_block - 1))) {
    REXLOG_WARN("[deko] Texture format {} has unsupported block size {}", uint32_t(format),
                guest_bytes_per_block);
    return std::nullopt;
  }
  const uint8_t* base_data=base_page?memory_->TranslatePhysical<const uint8_t*>(uint32_t(base_address)):nullptr;
  const uint8_t* mip_data=mip_page?memory_->TranslatePhysical<const uint8_t*>(uint32_t(mip_address)):nullptr;
  UntileJob job;
  job.level_count = level_count;
  job.layers = layers;
  job.block_width = format_info->block_width;
  job.block_height = format_info->block_height;
  job.guest_bytes_per_block = guest_bytes_per_block;
  job.bytes_per_block_log2 = rex::log2_floor(guest_bytes_per_block);
  job.host_texel_bytes = host_format.host_bytes_per_texel
                             ? host_format.host_bytes_per_texel
                             : guest_bytes_per_block;
  job.host_blocks = host_format.compressed;
  job.host_bytes_per_block = host_format.compressed
                                 ? (format == xenos::TextureFormat::k_DXT3A ? 16
                                                                           : guest_bytes_per_block)
                                 : job.block_width * job.block_height * job.host_texel_bytes;
  job.base_size = base_size;
  job.mip_size = mip_size;
  job.layout = layout;
  job.base_data = base_data;
  job.mip_data = mip_data;
  job.tiled = fetch.tiled;
  job.dimension = fetch.dimension;
  job.endianness = fetch.endianness;
  job.format = format;
  job.conversion = host_format.conversion;
  uint64_t total_bytes = 0;
  for (uint32_t level = 0; level < level_count; ++level) {
    LevelLayout& out = job.levels[level];
    out.width = std::max(width >> level, 1u);
    out.height = std::max(height >> level, 1u);
    out.depth = fetch.dimension == xenos::DataDimension::k3D ? std::max(depth >> level, 1u) : 1;
    out.blocks_x = (out.width + job.block_width - 1) / job.block_width;
    out.blocks_y = (out.height + job.block_height - 1) / job.block_height;
    out.host_row_bytes = out.blocks_x * job.host_bytes_per_block /
                         (host_format.compressed ? 1u : job.block_height);
    out.host_rows = host_format.compressed ? out.blocks_y : out.blocks_y * job.block_height;
    out.layer_bytes = uint64_t(out.host_row_bytes) * out.host_rows;
    out.offset = total_bytes;
    const uint32_t slices = fetch.dimension == xenos::DataDimension::k3D ? out.depth : layers;
    if (out.layer_bytes > std::numeric_limits<uint32_t>::max() ||
        out.layer_bytes * slices > std::numeric_limits<uint32_t>::max() - total_bytes) {
      return std::nullopt;
    }
    total_bytes += rex::align(out.layer_bytes * slices, uint64_t(16));
  }
  if (!total_bytes || total_bytes > std::numeric_limits<uint32_t>::max()) {
    return std::nullopt;
  }
  const rex::deko3d::Subida upload = nucleo_.Reservar(uint32_t(total_bytes), 256);
  if (!upload) {
    needs_flush_retry_ = true;
    return std::nullopt;
  }
  job.staging = upload.cpu;
  if (profiling_enabled_) {
    ++profile_.decode_upload_calls;
    profile_.decode_upload_bytes += total_bytes;
  }
  {
    ProfileScope decode_scope(profile_.decode_upload_ns, profiling_enabled_);
    std::memset(job.staging, 0, size_t(total_bytes));
    RunUntile(job);
  }

  auto image = CreateImage(image_format, width, height, image_type, layers, level_count);
  if (!image) {
    return std::nullopt;
  }
  image->host_format_swizzle = host_format.host_swizzle;
  for (uint32_t level = 0; level < level_count; ++level) {
    const LevelLayout& layout_out = job.levels[level];
    DkCopyBuf source{};
    if (!detail::MakeTextureCopyBuffer(upload.gpu + layout_out.offset,
                                       layout_out.host_row_bytes, layout_out.host_rows,
                                       layout_out.layer_bytes, source)) {
      REXLOG_WARN("[deko] Invalid texture upload strides for level {} (row {}, rows {}, layer {})",
                  level, layout_out.host_row_bytes, layout_out.host_rows,
                  layout_out.layer_bytes);
      FreeImage(*image);
      return std::nullopt;
    }
    const DkImageView view = [&]() {
      DkImageView v;
      dkImageViewDefaults(&v, &image->image);
      v.mipLevelOffset = uint8_t(level);
      v.mipLevelCount = 1;
      return v;
    }();
    const uint32_t copy_depth = fetch.dimension == xenos::DataDimension::k3D
                                    ? layout_out.depth
                                    : layers;
    const DkImageRect rect = {0, 0, 0, layout_out.width, layout_out.height, copy_depth};
    {
      ProfileScope upload_scope(profile_.decode_upload_ns, profiling_enabled_);
      dkCmdBufCopyBufferToImage(nucleo_.comandos(), &source, &view, &rect, 0);
    }
  }
  dkCmdBufBarrier(nucleo_.comandos(), DkBarrier_Full,
                  DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);

  auto texture = std::make_unique<Texture>();
  texture->key = texture_key;
  texture->layout_hash = layout_hash;
  texture->content_hash = content_hash;
  texture->check_state = check_state;
  texture->guest_memory=guest_memory;
  texture->last_frame = frame_;
  texture->image = std::move(*image);
  Texture& stored = *texture;
  cached_texture_bytes_ += stored.image.allocation.bytes;
  textures_.emplace(layout_hash, std::move(texture));
  Touch(stored.image, work_);
  const auto image_slot = GetImageView(stored.image, fetch.swizzle, image_type);
  const auto sampler_slot = GetSampler(sampler_fetch ? *sampler_fetch : fetch, sampler_binding);
  if (!image_slot || !sampler_slot) {
    return std::nullopt;
  }
  return remember(stored,TextureResourceBinding{*image_slot, *sampler_slot, width, height});
}

}  // namespace fear_nx::deko
