#include "gpu/textures/texture_codec.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gpu/guest_state.h"

namespace kameo::gpu {
HostTextureFormat MapTextureFormat(uint32_t xenos_format) {
  HostTextureFormat f;
  switch (xenos_format) {
    case kXenosFormat_8:
      f.format = plume::RenderFormat::R8_UNORM;
      f.block_bytes = 1;
      break;
    case kXenosFormat_8_8:
      f.format = plume::RenderFormat::R8G8_UNORM;
      f.block_bytes = 2;
      break;
    case kXenosFormat_8_8_8_8:
    case kXenosFormat_8_8_8_8_AS_16_16_16_16:
      f.format = plume::RenderFormat::R8G8B8A8_UNORM;
      f.block_bytes = 4;
      break;
    case kXenosFormat_1_5_5_5:
    case kXenosFormat_5_6_5:
    case kXenosFormat_4_4_4_4:
      // Expanded on the CPU: the DXGI equivalents are either missing or have
      // the channels in a different order, and these are rare enough that a
      // conversion costs nothing measurable.
      f.format = plume::RenderFormat::R8G8B8A8_UNORM;
      f.block_bytes = 2;
      f.expand_to_rgba8 = true;
      break;
    case kXenosFormat_2_10_10_10:
    case kXenosFormat_2_10_10_10_AS_16_16_16_16:
      f.format = plume::RenderFormat::R8G8B8A8_UNORM;
      f.block_bytes = 4;
      f.expand_to_rgba8 = true;
      break;
    case kXenosFormat_16_16_16_16_FLOAT:
      f.format = plume::RenderFormat::R16G16B16A16_FLOAT;
      f.block_bytes = 8;
      break;
    case kXenosFormat_16_16_FLOAT:
      f.format = plume::RenderFormat::R16G16_FLOAT;
      f.block_bytes = 4;
      break;
    case kXenosFormat_DXT1:
    case kXenosFormat_DXT1_AS_16_16_16_16:
      f.format = plume::RenderFormat::BC1_UNORM;
      f.block = 4;
      f.block_bytes = 8;
      break;
    case kXenosFormat_DXT2_3:
    case kXenosFormat_DXT2_3_AS_16_16_16_16:
      f.format = plume::RenderFormat::BC2_UNORM;
      f.block = 4;
      f.block_bytes = 16;
      break;
    case kXenosFormat_DXT4_5:
    case kXenosFormat_DXT4_5_AS_16_16_16_16:
      f.format = plume::RenderFormat::BC3_UNORM;
      f.block = 4;
      f.block_bytes = 16;
      break;
    case kXenosFormat_DXT5A:
      f.format = plume::RenderFormat::BC4_UNORM;
      f.block = 4;
      f.block_bytes = 8;
      break;
    case kXenosFormat_DXN:
      f.format = plume::RenderFormat::BC5_UNORM;
      f.block = 4;
      f.block_bytes = 16;
      break;
    default:
      break;
  }
  return f;
}

// The SNORM twin of a host format, for textures the fetch constant flags as
// signed. UNKNOWN when there is none.
plume::RenderFormat SnormTwin(plume::RenderFormat f) {
  switch (f) {
    case plume::RenderFormat::R8_UNORM: return plume::RenderFormat::R8_SNORM;
    case plume::RenderFormat::R8G8_UNORM: return plume::RenderFormat::R8G8_SNORM;
    case plume::RenderFormat::R8G8B8A8_UNORM: return plume::RenderFormat::R8G8B8A8_SNORM;
    case plume::RenderFormat::R16_UNORM: return plume::RenderFormat::R16_SNORM;
    case plume::RenderFormat::R16G16_UNORM: return plume::RenderFormat::R16G16_SNORM;
    case plume::RenderFormat::R16G16B16A16_UNORM: return plume::RenderFormat::R16G16B16A16_SNORM;
    default: return plume::RenderFormat::UNKNOWN;
  }
}

// Xbox 360 2D tiling, the standard XGAddress2DTiledOffset swizzle. Returns a
// BLOCK index: for compressed formats the caller works in 4x4 blocks and passes
// the block pitch, exactly as the console stores them.
uint32_t TiledOffset2D(uint32_t x, uint32_t y, uint32_t width, uint32_t log2_bpp) {
  const uint32_t aligned_width = (width + 31u) & ~31u;
  const uint32_t macro = ((x >> 5) + (y >> 5) * (aligned_width >> 5)) << (log2_bpp + 7);
  const uint32_t micro = ((x & 7u) + ((y & 6u) << 2)) << log2_bpp;
  const uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15u) +
                          ((y & 8u) << (3 + log2_bpp)) + ((y & 1u) << 4);
  return ((((offset & ~511u) << 3) + ((offset & 448u) << 2) + (offset & 63u) +
           ((y & 16u) << 7) + (((((y & 8u) >> 2) + (x >> 3)) & 3u) << 6)) >>
          log2_bpp);
}

// Xbox 360 volume tiling, XGRAPHICS::TileVolume. Block index like TiledOffset2D, but the tile is
// 32x32x4 blocks so Z participates in both the macro row and the micro offset.
uint32_t TiledOffset3D(uint32_t x, uint32_t y, uint32_t z, uint32_t pitch, uint32_t height, uint32_t log2_bpp) {
  const uint32_t aligned_pitch = (pitch + 31u) & ~31u;
  const uint32_t aligned_height = (height + 31u) & ~31u;
  const uint32_t macro_outer = ((y >> 4) + (z >> 2) * (aligned_height >> 4)) * (aligned_pitch >> 5);
  const uint32_t macro = ((((x >> 5) + macro_outer) << (log2_bpp + 6)) & 0xFFFFFFFu) << 1;
  const uint32_t micro = (((x & 7u) + ((y & 6u) << 2)) << (log2_bpp + 6)) >> 6;
  const uint32_t offset_outer = ((y >> 3) + (z >> 2)) & 1u;
  const uint32_t offset1 = offset_outer + ((((x >> 3) + (offset_outer << 1)) & 3u) << 1);
  const uint32_t offset2 = ((macro + (micro & ~15u)) << 1) + (micro & 15u) +
                           ((z & 3u) << (log2_bpp + 6)) + ((y & 1u) << 4);
  uint32_t address = (offset1 & 1u) << 3;
  address += (offset2 >> 6) & 7u;
  address <<= 3;
  address += offset1 & ~1u;
  address <<= 2;
  address += offset2 & ~511u;
  address <<= 3;
  address += offset2 & 63u;
  return address >> log2_bpp;
}

uint32_t Log2(uint32_t value) {
  uint32_t result = 0;
  while ((1u << result) < value) {
    ++result;
  }
  return result;
}

void PackedTextureBaseOffset(uint32_t width, uint32_t height, uint32_t block_size, bool packed, uint32_t& x, uint32_t& y) {
  x = 0;
  y = 0;
  if (!packed || !width || !height || !block_size || (width > 16 && height > 16)) {
    return;
  }
  if (Log2(width) > Log2(height)) {
    y = 16 / block_size;
  } else {
    x = 16 / block_size;
  }
}

// Undoes the fetch constant's endian field on a block of pixel data.
void ApplyEndian(uint8_t* data, size_t bytes, uint32_t endian) {
  switch (endian) {
    case 1: {  // 8in16
      for (size_t i = 0; i + 1 < bytes; i += 2) {
        std::swap(data[i], data[i + 1]);
      }
      break;
    }
    case 2: {  // 8in32
      for (size_t i = 0; i + 3 < bytes; i += 4) {
        std::swap(data[i], data[i + 3]);
        std::swap(data[i + 1], data[i + 2]);
      }
      break;
    }
    case 3: {  // 16in32
      for (size_t i = 0; i + 3 < bytes; i += 4) {
        std::swap(data[i], data[i + 2]);
        std::swap(data[i + 1], data[i + 3]);
      }
      break;
    }
    default:
      break;
  }
}

void ExpandPackedToRgba8(const uint8_t* src, uint8_t* dst, uint32_t texels, uint32_t format,
                         uint32_t src_bytes) {
  for (uint32_t i = 0; i < texels; ++i) {
    uint32_t value = 0;
    std::memcpy(&value, src + src_bytes * i, src_bytes);
    uint8_t r = 0, g = 0, b = 0, a = 255;
    switch (format) {
      case kXenosFormat_2_10_10_10:
      case kXenosFormat_2_10_10_10_AS_16_16_16_16:
        r = uint8_t(((value >> 20) & 0x3FF) >> 2);
        g = uint8_t(((value >> 10) & 0x3FF) >> 2);
        b = uint8_t((value & 0x3FF) >> 2);
        a = uint8_t(((value >> 30) & 0x3) * 85);
        break;
      case kXenosFormat_5_6_5:
        r = uint8_t(((value >> 11) & 0x1F) * 255 / 31);
        g = uint8_t(((value >> 5) & 0x3F) * 255 / 63);
        b = uint8_t((value & 0x1F) * 255 / 31);
        break;
      case kXenosFormat_1_5_5_5:
        a = (value & 0x8000) ? 255 : 0;
        r = uint8_t(((value >> 10) & 0x1F) * 255 / 31);
        g = uint8_t(((value >> 5) & 0x1F) * 255 / 31);
        b = uint8_t((value & 0x1F) * 255 / 31);
        break;
      case kXenosFormat_4_4_4_4:
        a = uint8_t(((value >> 12) & 0xF) * 17);
        r = uint8_t(((value >> 8) & 0xF) * 17);
        g = uint8_t(((value >> 4) & 0xF) * 17);
        b = uint8_t((value & 0xF) * 17);
        break;
      default:
        break;
    }
    dst[4 * i + 0] = r;
    dst[4 * i + 1] = g;
    dst[4 * i + 2] = b;
    dst[4 * i + 3] = a;
  }
}

// Bytes per block of a host format, so a block_bytes that disagrees with its format is skipped rather than removing the device.
uint32_t FormatBytesPerBlock(plume::RenderFormat format) {
  switch (format) {
    case plume::RenderFormat::R8_UNORM:
    case plume::RenderFormat::R8_UINT:
      return 1;
    case plume::RenderFormat::R8G8_UNORM:
    case plume::RenderFormat::R16_FLOAT:
    case plume::RenderFormat::R16_UNORM:
      return 2;
    case plume::RenderFormat::R8G8B8A8_UNORM:
    case plume::RenderFormat::R8G8B8A8_SNORM:
    case plume::RenderFormat::R8G8B8A8_UINT:
    case plume::RenderFormat::B8G8R8A8_UNORM:
    case plume::RenderFormat::R16G16_FLOAT:
    case plume::RenderFormat::R16G16_UNORM:
    case plume::RenderFormat::R16G16_SNORM:
    case plume::RenderFormat::R32_FLOAT:
    case plume::RenderFormat::R32_UINT:
      return 4;
    case plume::RenderFormat::R16G16B16A16_FLOAT:
    case plume::RenderFormat::R16G16B16A16_UNORM:
    case plume::RenderFormat::R16G16B16A16_SNORM:
    case plume::RenderFormat::R16G16B16A16_UINT:
    case plume::RenderFormat::R32G32_FLOAT:
    case plume::RenderFormat::BC1_UNORM:
    case plume::RenderFormat::BC4_UNORM:
      return 8;
    case plume::RenderFormat::BC2_UNORM:
    case plume::RenderFormat::BC3_UNORM:
    case plume::RenderFormat::BC5_UNORM:
    case plume::RenderFormat::R32G32B32A32_FLOAT:
    case plume::RenderFormat::R32G32B32A32_UINT:
      return 16;
    default:
      return 0;  // unknown: do not police what we cannot measure
  }
}

uint32_t AlignRow(uint32_t bytes) { return (bytes + 255u) & ~255u; }

// The fetch swizzle is four 3-bit selectors: 0..3 a source channel, 4 constant 0, 5 constant 1.

// Expands an uncompressed 8-bit-per-channel surface to RGBA8 through the
// swizzle. `channels` is how many the source format actually stores.
void ApplySwizzleToRgba8(const uint8_t* src, uint8_t* dst, size_t texels, uint32_t channels,
                         uint32_t swizzle) {
  uint32_t select[4];
  for (uint32_t i = 0; i < 4; ++i) {
    select[i] = (swizzle >> (3 * i)) & 0x7;
  }
  for (size_t t = 0; t < texels; ++t) {
    const uint8_t* in = src + t * channels;
    uint8_t* out = dst + t * 4;
    for (uint32_t i = 0; i < 4; ++i) {
      switch (select[i]) {
        case 0: case 1: case 2: case 3:
          out[i] = (select[i] < channels) ? in[select[i]] : 0;
          break;
        case 4: out[i] = 0x00; break;
        case 5: out[i] = 0xFF; break;
        default: out[i] = 0x00; break;
      }
    }
  }
}


// BC1/BC2/BC3 decode for the texture dump and replacements only; the upload path hands compressed blocks straight to the GPU.
void DecodeBcBlockColour(const uint8_t* src, uint8_t* out_rgba, uint32_t out_pitch, uint32_t bx,
                         uint32_t by, uint32_t width, uint32_t height, bool one_bit_alpha) {
  const uint16_t c0 = uint16_t(src[0] | (src[1] << 8));
  const uint16_t c1 = uint16_t(src[2] | (src[3] << 8));
  auto unpack = [](uint16_t c, uint8_t* rgb) {
    rgb[0] = uint8_t(((c >> 11) & 0x1F) * 255 / 31);
    rgb[1] = uint8_t(((c >> 5) & 0x3F) * 255 / 63);
    rgb[2] = uint8_t((c & 0x1F) * 255 / 31);
  };
  uint8_t palette[4][3];
  unpack(c0, palette[0]);
  unpack(c1, palette[1]);
  const bool punchthrough = one_bit_alpha && c0 <= c1;
  for (int i = 0; i < 3; ++i) {
    if (punchthrough) {
      palette[2][i] = uint8_t((int(palette[0][i]) + palette[1][i]) / 2);
      palette[3][i] = 0;
    } else {
      palette[2][i] = uint8_t((2 * int(palette[0][i]) + palette[1][i]) / 3);
      palette[3][i] = uint8_t((int(palette[0][i]) + 2 * int(palette[1][i])) / 3);
    }
  }
  const uint32_t bits = uint32_t(src[4]) | (uint32_t(src[5]) << 8) | (uint32_t(src[6]) << 16) |
                        (uint32_t(src[7]) << 24);
  for (uint32_t y = 0; y < 4; ++y) {
    for (uint32_t x = 0; x < 4; ++x) {
      const uint32_t px = bx * 4 + x;
      const uint32_t py = by * 4 + y;
      if (px >= width || py >= height) {
        continue;
      }
      const uint32_t index = (bits >> (2 * (y * 4 + x))) & 3;
      uint8_t* dst = out_rgba + size_t(py) * out_pitch + size_t(px) * 4;
      dst[0] = palette[index][0];
      dst[1] = palette[index][1];
      dst[2] = palette[index][2];
      if (punchthrough && index == 3) {
        dst[3] = 0;
      } else if (one_bit_alpha) {
        dst[3] = 255;
      }
    }
  }
}

// One 8-byte BC4 sub-block into a single channel. BC5/DXN is two of these, and
// BC3's alpha is the same layout, which is why the interpolation lives here.
void DecodeBc4Block(const uint8_t* src, uint8_t* out_rgba, uint32_t out_pitch, uint32_t bx, uint32_t by, uint32_t width, uint32_t height, uint32_t channel) {
  const uint8_t a0 = src[0], a1 = src[1];
  uint8_t lut[8] = {a0, a1};
  if (a0 > a1) {
    for (int i = 0; i < 6; ++i) lut[2 + i] = uint8_t(((6 - i) * a0 + (1 + i) * a1) / 7);
  } else {
    for (int i = 0; i < 4; ++i) lut[2 + i] = uint8_t(((4 - i) * a0 + (1 + i) * a1) / 5);
    lut[6] = 0;
    lut[7] = 255;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= uint64_t(src[2 + i]) << (8 * i);
  for (uint32_t y = 0; y < 4; ++y) {
    for (uint32_t x = 0; x < 4; ++x) {
      const uint32_t px = bx * 4 + x, py = by * 4 + y;
      if (px >= width || py >= height) continue;
      out_rgba[size_t(py) * out_pitch + size_t(px) * 4 + channel] =
          lut[uint32_t((bits >> (3 * (y * 4 + x))) & 7)];
    }
  }
}

void DecodeBcToRgba(const uint8_t* src, uint32_t src_pitch, uint8_t* out_rgba, uint32_t out_pitch,
                    uint32_t width, uint32_t height, uint32_t xenos_format) {
  const bool bc1 = (xenos_format == kXenosFormat_DXT1 ||
                    xenos_format == kXenosFormat_DXT1_AS_16_16_16_16);
  const bool bc2 = (xenos_format == kXenosFormat_DXT2_3 ||
                    xenos_format == kXenosFormat_DXT2_3_AS_16_16_16_16);
  const bool bc4 = (xenos_format == kXenosFormat_DXT5A);
  const bool bc5 = (xenos_format == kXenosFormat_DXN);
  // DXN carries no colour block at all, so it takes its own path: the two BC4
  // halves are X and Y, and Z is reconstructed exactly as the shaders do it.
  if (bc4 || bc5) {
    const uint32_t stride = bc4 ? 8u : 16u;
    for (uint32_t by = 0; by < (height + 3) / 4; ++by) {
      for (uint32_t bx = 0; bx < (width + 3) / 4; ++bx) {
        const uint8_t* block = src + size_t(by) * src_pitch + size_t(bx) * stride;
        DecodeBc4Block(block, out_rgba, out_pitch, bx, by, width, height, 0);
        DecodeBc4Block(bc5 ? block + 8 : block, out_rgba, out_pitch, bx, by, width, height, 1);
      }
    }
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        uint8_t* t = out_rgba + size_t(y) * out_pitch + size_t(x) * 4;
        if (bc4) {
          t[1] = t[2] = t[0];
        } else {
          const float nx = t[0] / 127.5f - 1.0f, ny = t[1] / 127.5f - 1.0f;
          const float nz = 1.0f - nx * nx - ny * ny;
          t[2] = uint8_t((nz > 0.0f ? std::sqrt(nz) : 0.0f) * 127.5f + 127.5f);
        }
        t[3] = 255;
      }
    }
    return;
  }
  const uint32_t block_bytes = bc1 ? 8u : 16u;
  const uint32_t blocks_x = (width + 3) / 4;
  const uint32_t blocks_y = (height + 3) / 4;
  for (uint32_t by = 0; by < blocks_y; ++by) {
    for (uint32_t bx = 0; bx < blocks_x; ++bx) {
      const uint8_t* block = src + size_t(by) * src_pitch + size_t(bx) * block_bytes;
      const uint8_t* colour = bc1 ? block : block + 8;
      DecodeBcBlockColour(colour, out_rgba, out_pitch, bx, by, width, height, bc1);
      if (bc2) {
        for (uint32_t y = 0; y < 4; ++y) {
          for (uint32_t x = 0; x < 4; ++x) {
            const uint32_t px = bx * 4 + x, py = by * 4 + y;
            if (px >= width || py >= height) continue;
            const uint8_t nibble = block[y * 2 + x / 2];
            const uint8_t a = (x & 1) ? (nibble >> 4) : (nibble & 0xF);
            out_rgba[size_t(py) * out_pitch + size_t(px) * 4 + 3] = uint8_t(a * 17);
          }
        }
      } else if (!bc1) {  // BC3: 3-bit interpolated alpha
        const uint8_t a0 = block[0], a1 = block[1];
        uint8_t alpha[8] = {a0, a1};
        if (a0 > a1) {
          for (int i = 0; i < 6; ++i)
            alpha[2 + i] = uint8_t(((6 - i) * a0 + (1 + i) * a1) / 7);
        } else {
          for (int i = 0; i < 4; ++i)
            alpha[2 + i] = uint8_t(((4 - i) * a0 + (1 + i) * a1) / 5);
          alpha[6] = 0;
          alpha[7] = 255;
        }
        uint64_t abits = 0;
        for (int i = 0; i < 6; ++i) abits |= uint64_t(block[2 + i]) << (8 * i);
        for (uint32_t y = 0; y < 4; ++y) {
          for (uint32_t x = 0; x < 4; ++x) {
            const uint32_t px = bx * 4 + x, py = by * 4 + y;
            if (px >= width || py >= height) continue;
            const uint32_t idx = uint32_t((abits >> (3 * (y * 4 + x))) & 7);
            out_rgba[size_t(py) * out_pitch + size_t(px) * 4 + 3] = alpha[idx];
          }
        }
      }
    }
  }
}

}  // namespace kameo::gpu
