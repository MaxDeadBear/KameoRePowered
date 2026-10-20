#include "gpu/textures/texture_upload.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <vector>

#include <plume_render_interface.h>

#include "gpu/config.h"
#include "gpu/draw/geometry_cache.h"
#include "gpu/guest_device.h"
#include "gpu/guest_memory.h"
#include "gpu/log.h"
#include "gpu/occlusion.h"
#include "gpu/resolve.h"
#include "gpu/resource_check.h"
#include "gpu/surface_pool.h"
#include "gpu/textures/texture_cache.h"
#include "gpu/textures/texture_codec.h"
#include "gpu/textures/texture_heap.h"
#include "gpu/textures/texture_pack.h"
#include "gpu/textures/texture_replacement.h"

namespace kameo::gpu::detail {

uint32_t g_mipLevelsUploaded = 0;
uint32_t g_mippedTextures = 0;
uint32_t g_mipChainsTruncated = 0;

namespace {

std::vector<uint8_t> g_untileScratch;

// One untiled mip level, staged for a placed-footprint copy.
struct MipUpload {
  uint32_t level = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t blocks_x = 0;
  uint32_t blocks_y = 0;
  uint32_t row_bytes = 0;     // tight rows in the staging vector
  uint32_t upload_pitch = 0;  // row pitch once copied into the upload buffer
  uint32_t upload_offset = 0;
  std::vector<uint8_t> bytes;
};

// The first level Xenos stores in the packed tail, where levels share one tile; only the unpacked levels are uploaded, so this is where the chain stops.
uint32_t FirstPackedMipLevel(uint32_t width, uint32_t height) {
  for (uint32_t level = 0; level < 16; ++level) {
    if (std::max(width >> level, 1u) <= 16 || std::max(height >> level, 1u) <= 16) {
      return level;
    }
  }
  return 16;
}

// Untiles levels 1..N out of the guest's own mip chain, still compressed: the exact blocks the console sampled, which is what makes this work for BC1, ~80% of the textures the game binds.
void CollectGuestMips(uint8_t* base, uint32_t object, const GuestTextureFetch& fetch, const HostTextureFormat& host, uint32_t width, uint32_t height, std::vector<MipUpload>* out) {
  // The fetch constant's base and mip addresses have both lost their high bits identically, so their distance applies to the real base the caller recovered from the texture object.
  const uint32_t real_base = Load32(base, object + 20) & 0xFFFFF000u;
  const uint32_t fetch_base = fetch.address();
  const uint32_t fetch_mip = fetch.mip_address();
  if (real_base == 0 || fetch_mip <= fetch_base) {
    return;
  }
  const uint32_t mip_address = real_base + (fetch_mip - fetch_base);
  if (mip_address == 0 || !fetch.tiled()) {
    return;
  }
  const uint32_t packed = FirstPackedMipLevel(width, height);
  if (packed <= 1) {
    return;
  }
  const uint32_t last = std::min(fetch.mip_max_level(), packed - 1);
  if (last < 1) {
    return;
  }
  const uint8_t* chain = GuestData(base, mip_address);
  if (chain == nullptr) {
    return;
  }
  const uint32_t log2_bpp = Log2(host.block_bytes);
  uint32_t level_offset = 0;
  for (uint32_t level = 1; level <= last; ++level) {
    const uint32_t lw = std::max(width >> level, 1u);
    const uint32_t lh = std::max(height >> level, 1u);
    const uint32_t bx = (lw + host.block - 1) / host.block;
    const uint32_t by = (lh + host.block - 1) / host.block;
    // Each unpacked level is tiled at its own pitch padded to the 32-block macro tile, and the next level starts on the following 4 KB page.
    const uint32_t pitch_blocks = (bx + 31u) & ~31u;
    const uint32_t rows_padded = (by + 31u) & ~31u;
    const uint32_t level_bytes = pitch_blocks * rows_padded * host.block_bytes;

    // Checked per level: the fetch constant advertises the full pyramid whether or not the game allocated it, and the tail is routinely reserved but not committed.
    if (!GuestRangeReadable(chain + level_offset, level_bytes)) {
      ++g_mipChainsTruncated;
      break;
    }

    MipUpload m;
    m.level = level;
    m.width = lw;
    m.height = lh;
    m.blocks_x = bx;
    m.blocks_y = by;
    m.row_bytes = bx * host.block_bytes;
    m.bytes.assign(size_t(m.row_bytes) * by, 0);
    const uint8_t* src = chain + level_offset;
    for (uint32_t y = 0; y < by; ++y) {
      uint8_t* dst = m.bytes.data() + size_t(y) * m.row_bytes;
      for (uint32_t x = 0; x < bx; ++x) {
        const uint32_t off = TiledOffset2D(x, y, pitch_blocks, log2_bpp) * host.block_bytes;
        if (off + host.block_bytes > level_bytes) {
          continue;
        }
        std::memcpy(dst + size_t(x) * host.block_bytes, src + off, host.block_bytes);
      }
    }
    ApplyEndian(m.bytes.data(), m.bytes.size(), fetch.endian());
    out->push_back(std::move(m));
    level_offset += (level_bytes + 4095u) & ~4095u;
  }
}

}  // namespace

// Untiles, converts and uploads one guest texture, returning its bindless heap index; cached on everything that describes the pixels, not on the sampler state sharing the fetch constant.
uint32_t EnsureTexture(uint8_t* base, GraphicsSystem::DrawPathContext& ctx, plume::RenderCommandList* list, const GuestTextureFetch& fetch, uint32_t object, uint32_t object_address, uint32_t want_dimension, bool* volume_out) {
  if (volume_out) {
    *volume_out = false;
  }
  if (!object || !fetch.valid()) {
    // Mostly harmless: a stale fetch constant in a slot the shader does not sample, which must not consume heap slots.
    return 0;
  }
  // The fetch constant's address has lost its high bits (SetTexture masks them), so the address comes from the texture object's BaseFlush, read at capture.
  const uint32_t address = object_address;
  if (!address) {
    return 0;
  }

  // Color and depth resolves can reuse the same address and shape, so prefer the fetch's format.
  const bool fetch_depth = fetch.format() == kXenosFormat_24_8 || fetch.format() == kXenosFormat_24_8_FLOAT;
  ResolveKey want{address, uint16_t(fetch.width()), uint16_t(fetch.height()), fetch_depth};
  auto resolved = g_resolveTargets.find(want);
  if (resolved == g_resolveTargets.end() || !resolved->second.written) {
    want.depth = !fetch_depth;
    resolved = g_resolveTargets.find(want);
  }
  // An entry no resolve has written is empty, and preferring it hid the storybook's warrior portraits (plain sprites at the same address) behind black.
  if (resolved != g_resolveTargets.end() && !resolved->second.written) {
    resolved = g_resolveTargets.end();
  }
  if (resolved != g_resolveTargets.end()) {
    if (WideGuestFormat(fetch.format())) g_wideResolveAddresses.insert(address);
    return resolved->second.heap_index;
  }

  const uint32_t width = fetch.width();
  const uint32_t height = fetch.height();
  const uint32_t format = fetch.format();

  const uint64_t texture_key = (uint64_t(address) << 32) ^ (uint64_t(width) << 20) ^
                       (uint64_t(height) << 6) ^ format ^ (fetch.tiled() ? 0x8000000000ull : 0) ^
                       (uint64_t(want_dimension) << 42);
  const uint64_t key = MixGeometryKey(texture_key, fetch.packed_mips() ? 1u : 0u);

  auto found = g_textures.find(key);
  if (found != g_textures.end()) {
    found->second.last_used_frame = g_drawFrame;
    ApplyReadyReplacement(ctx, list, found->second);
    if (volume_out) {
      *volume_out = found->second.volume;
    }
    return found->second.heap_index;
  }

  HostTextureFormat host = MapTextureFormat(format);
  if (host.format == plume::RenderFormat::UNKNOWN || width == 0 || height == 0) {
    return 0;
  }

  const uint32_t blocks_x = (width + host.block - 1) / host.block;
  uint32_t blocks_y = (height + host.block - 1) / host.block;
  const uint32_t pitch_texels = fetch.pitch() ? fetch.pitch() : width;
  const uint32_t pitch_blocks = std::max(1u, pitch_texels / host.block);
  const uint32_t src_row_bytes = blocks_x * host.block_bytes;
  uint32_t base_x, base_y;
  PackedTextureBaseOffset(width, height, host.block, fetch.packed_mips(), base_x, base_y);

  // The shader decides the dimension, not the fetch constant: XenosRecomp samples a stacked or 3D fetch through its Texture3D heap, where a 2D resource reads zero.
  bool volume = want_dimension == 1;
  // A cube's six faces are laid out like array slices, each padded to a 4 KB boundary.
  const bool cube = want_dimension == 2;
  uint32_t depth = volume ? std::max(1u, fetch.depth()) : cube ? 6u : 1u;
  // A one-slice volume stays a volume: the flying gremlin's shader declares its 256x256 DXT1 diffuse as Texture3D, and a 2D resource there drew its body black.
  depth = std::max(1u, depth);

  // Untile (or copy row by row) into scratch, then convert if needed.
  const size_t slice_bytes = size_t(src_row_bytes) * blocks_y;
  const size_t linear_bytes = slice_bytes * depth;
  g_untileScratch.resize(linear_bytes);
  const uint8_t* src = GuestData(base, address);
  // The render thread reads a frame after the guest drew it, by when the guest may have released this memory, so check the whole tiled footprint before touching it.
  const size_t cube_face_bytes = !cube ? 0 : fetch.tiled() ? (size_t((pitch_blocks + 31u) & ~31u) * ((base_y + blocks_y + 31u) & ~31u) * host.block_bytes + 4095u) & ~size_t(4095) : (size_t(pitch_blocks) * (base_y + blocks_y) * host.block_bytes + 4095u) & ~size_t(4095);
  const size_t footprint = cube ? cube_face_bytes * depth : size_t((pitch_blocks + 31u) & ~31u) * ((base_y + blocks_y + 31u) & ~31u) * host.block_bytes * (volume ? ((depth + 3u) & ~3u) : depth);
  if (!GuestRangeReadable(src, std::max(footprint, linear_bytes))) {
    return 0;
  }

  if (fetch.tiled()) {
    const uint32_t log2_bpp = Log2(host.block_bytes);
    // A stacked texture is an array of independently tiled 2D slices, not a TileVolume 32x32x4 volume; untiling the gremlins' skin array as a volume scrambled it.
    const bool array_slices = fetch.stacked();
    const uint32_t slice_blocks =
        ((pitch_blocks + 31u) & ~31u) * ((blocks_y + 31u) & ~31u);
    for (uint32_t z = 0; z < depth; ++z) {
      for (uint32_t y = 0; y < blocks_y; ++y) {
        uint8_t* dst = g_untileScratch.data() + size_t(z) * slice_bytes + size_t(y) * src_row_bytes;
        for (uint32_t x = 0; x < blocks_x; ++x) {
          const uint32_t offset =
              (volume && !array_slices)
                  ? TiledOffset3D(x + base_x, y + base_y, z, pitch_blocks, blocks_y, log2_bpp) * host.block_bytes
              : volume ? (TiledOffset2D(x + base_x, y + base_y, pitch_blocks, log2_bpp) + z * slice_blocks) *
                             host.block_bytes
                       : TiledOffset2D(x + base_x, y + base_y, pitch_blocks, log2_bpp) * host.block_bytes + z * cube_face_bytes;
          std::memcpy(dst + size_t(x) * host.block_bytes, src + offset, host.block_bytes);
        }
      }
    }
  } else {
    const uint32_t src_pitch_bytes = pitch_blocks * host.block_bytes;
    for (uint32_t z = 0; z < depth; ++z) {
      for (uint32_t y = 0; y < blocks_y; ++y) {
        const size_t slice_offset = cube ? z * cube_face_bytes : size_t(z) * blocks_y * src_pitch_bytes;
        std::memcpy(g_untileScratch.data() + size_t(z) * slice_bytes + size_t(y) * src_row_bytes,
                    src + slice_offset + size_t(y + base_y) * src_pitch_bytes + size_t(base_x) * host.block_bytes, src_row_bytes);
      }
    }
  }

  ApplyEndian(g_untileScratch.data(), linear_bytes, fetch.endian());

  // Hashed even with dumping and replacements both off, so either can be switched on later for textures already loaded.
  const bool pack_eligible = !volume && want_dimension == 0 && !fetch.unnormalized_coords() && fetch.sign(0) != kTextureSignSigned;
  const uint64_t content_hash = pack_eligible ? TextureContentHash(g_untileScratch.data(), slice_bytes, width, height, format, fetch.swizzle()) : 0;

  // An all-zero texture is usually a resolve target never rendered into, which needs a different fix from a missing one; it is also never dumped or replaced.
  bool all_zero = true;
  {
    for (size_t i = 0; i < linear_bytes; ++i) {
      if (g_untileScratch[i] != 0) {
        all_zero = false;
        break;
      }
    }
    if (all_zero) {
      ++g_emptyTextures;
      if (g_emptyTextures <= 12) {
        KGFX_WARN("texture {:08X} {}x{} format {} is entirely ZERO -- almost "
                    "certainly a resolve target we never rendered into",
                    address, width, height, format);
      }
    }
  }

  std::vector<uint8_t> expanded;
  const uint8_t* pixels = g_untileScratch.data();
  uint32_t dst_row_bytes = src_row_bytes;

  // A signed fetch holds two's complement texels, so the host format has to be SNORM; Kameo's only signed combination is fmt10/sign55 on the texbem maps.
  const bool data_signed = fetch.sign(0) == kTextureSignSigned;
  if (data_signed) {
    const plume::RenderFormat snorm = SnormTwin(host.format);
    if (snorm != plume::RenderFormat::UNKNOWN) {
      host.format = snorm;
    } else {
      static std::set<uint32_t> warned;
      if (warned.insert(format).second) {
        KGFX_WARN("xenos fmt {} is SIGNED (sign {:02X}) and has no SNORM host "
                    "format, so it is still sampled unsigned and every texel is wrong",
                    format, fetch.signs());
      }
    }
  }

  // 8-bit channel formats with a non-identity swizzle become RGBA8 with the swizzle baked in, since plume has no per-view component mapping; signed data is excluded, where the constant selectors would write 0 and -1/127.
  const bool byte_channels = host.block == 1 && (format == kXenosFormat_8 ||
                                                 format == kXenosFormat_8_8 ||
                                                 format == kXenosFormat_8_8_8_8 ||
                                                 format == kXenosFormat_8_8_8_8_AS_16_16_16_16);
  if (byte_channels && !data_signed && fetch.swizzle() != kSwizzleIdentity) {
    const uint32_t channels = host.block_bytes;
    expanded.resize(size_t(width) * height * 4 * depth);
    for (uint32_t z = 0; z < depth; ++z) {
      for (uint32_t y = 0; y < height; ++y) {
        ApplySwizzleToRgba8(g_untileScratch.data() + z * slice_bytes + size_t(y) * src_row_bytes, expanded.data() + (size_t(z) * height + y) * width * 4, width, channels, fetch.swizzle());
      }
    }
    pixels = expanded.data();
    dst_row_bytes = width * 4;
    host.format = plume::RenderFormat::R8G8B8A8_UNORM;
    host.block_bytes = 4;
  } else if (host.block == 4 && fetch.swizzle() != kSwizzleIdentity) {
    // A block-compressed view cannot carry a swizzle, so the constant selectors forcing a channel to 0 or 1 were dropped and every sample returned the stored alpha.
    expanded.resize(size_t(width) * height * 4 * depth);
    for (uint32_t z = 0; z < depth; ++z) DecodeBcToRgba(g_untileScratch.data() + z * slice_bytes, src_row_bytes, expanded.data() + size_t(z) * width * height * 4, width * 4, width, height, format);
    std::vector<uint8_t> swizzled(expanded.size());
    for (uint32_t y = 0; y < height * depth; ++y) {
      ApplySwizzleToRgba8(expanded.data() + size_t(y) * width * 4, swizzled.data() + size_t(y) * width * 4, width, 4, fetch.swizzle());
    }
    expanded.swap(swizzled);
    pixels = expanded.data();
    dst_row_bytes = width * 4;
    host.format = plume::RenderFormat::R8G8B8A8_UNORM;
    host.block = 1;
    host.block_bytes = 4;
    blocks_y = height;
  } else if (host.expand_to_rgba8) {
    expanded.resize(size_t(width) * height * 4 * depth);
    for (uint32_t z = 0; z < depth; ++z) {
      for (uint32_t y = 0; y < height; ++y) {
        ExpandPackedToRgba8(g_untileScratch.data() + z * slice_bytes + size_t(y) * src_row_bytes, expanded.data() + (size_t(z) * height + y) * width * 4, width, format, host.block_bytes);
      }
    }
    // The swizzle applies to the packed formats too; fmt54 is used with swizzle 0x60A, a BGRA swap.
    if (fetch.swizzle() != kSwizzleIdentity) {
      std::vector<uint8_t> swizzled(expanded.size());
      for (uint32_t y = 0; y < height * depth; ++y) {
        ApplySwizzleToRgba8(expanded.data() + size_t(y) * width * 4,
                            swizzled.data() + size_t(y) * width * 4, width, 4, fetch.swizzle());
      }
      expanded.swap(swizzled);
    }
    pixels = expanded.data();
    dst_row_bytes = width * 4;
    // block_bytes must follow the format, or the placed footprint claims twice the texels a row holds and D3D12 removes the device (the k_5_6_5 freeze).
    host.block_bytes = 4;
  }

  // Xenos DXN orders the two BC4 halves of each block the other way round from BC5, which transposes X and Y of every normal; swap them on the untiled copy.
  if (format == kXenosFormat_DXN && REXCVAR_GET(kameo_gfx_dxn_swap)) {
    uint8_t* blocks = g_untileScratch.data();
    for (size_t off = 0; off + 16 <= linear_bytes; off += 16) {
      uint8_t tmp[8];
      std::memcpy(tmp, blocks + off, 8);
      std::memcpy(blocks + off, blocks + off + 8, 8);
      std::memcpy(blocks + off + 8, tmp, 8);
    }
  }

  const bool dump_is_bc = host.format == plume::RenderFormat::BC1_UNORM || host.format == plume::RenderFormat::BC2_UNORM || host.format == plume::RenderFormat::BC3_UNORM || host.format == plume::RenderFormat::BC4_UNORM || host.format == plume::RenderFormat::BC5_UNORM;
  const bool pack_format = pack_eligible && !all_zero && (host.format == plume::RenderFormat::R8G8B8A8_UNORM || dump_is_bc);
  if (!pack_format) {
    if (volume || want_dimension != 0) ++g_packSkips.shape;
    else if (fetch.unnormalized_coords()) ++g_packSkips.unnormalized;
    else if (fetch.sign(0) == kTextureSignSigned) ++g_packSkips.signed_texels;
    else if (all_zero) ++g_packSkips.empty;
    else ++g_packSkips.format;
  }
  if (pack_format) {
    const uint32_t row_bytes = dump_is_bc ? src_row_bytes : dst_row_bytes;
    const uint32_t rows = dump_is_bc ? (height + 3) / 4 : height;
    const uint8_t* dump_source = dump_is_bc ? g_untileScratch.data() : pixels;
    QueueTextureDump(content_hash, object, width, height, format, std::vector<uint8_t>(dump_source, dump_source + size_t(row_bytes) * rows), row_bytes, dump_is_bc, REXCVAR_GET(kameo_gfx_dump_textures));
  }
  const bool has_replacement = pack_format && REXCVAR_GET(kameo_gfx_texture_replacements) && HasTextureReplacement(content_hash);
  const ReplacementChannels replacement_channels = host.format == plume::RenderFormat::BC4_UNORM ? ReplacementChannels::RedOnly : host.format == plume::RenderFormat::BC5_UNORM ? ReplacementChannels::RedGreen : ReplacementChannels::Rgba;

  // Mips come from the guest, untouched, and only when level 0 was not rewritten: the swizzle and expand branches change the block geometry the chain is laid out with.
  std::vector<MipUpload> mips;
  if (REXCVAR_GET(kameo_gfx_mipmaps) && pixels == g_untileScratch.data() && !cube) {
    CollectGuestMips(base, object, fetch, host, width, height, &mips);
  }

  // Those branches rewrite one 2D image, so a volume that took one falls back to its base level.
  if (volume && pixels != g_untileScratch.data()) {
    volume = false;
    depth = 1;
  }

  CachedTexture entry;
  // Checked: a refused texture once reached the copy below as a null resource and took the process down on a GTX 1060.
  entry.texture = CreateTextureChecked(ctx.device,
      volume ? plume::RenderTextureDesc::Texture3D(width, height, depth, 1, host.format)
      : cube ? plume::RenderTextureDesc::Texture(plume::RenderTextureDimension::TEXTURE_2D, width, height, 1, 1, 6, host.format, plume::RenderTextureFlag::CUBE)
             : plume::RenderTextureDesc::Texture2D(width, height, uint32_t(1 + mips.size()), host.format),
      "asset texture");
  if (!entry.texture) {
    return 0;
  }
  entry.view = entry.texture->createTextureView(
      volume ? plume::RenderTextureViewDesc::Texture3D(host.format)
      : cube ? plume::RenderTextureViewDesc::TextureCube(host.format)
             : plume::RenderTextureViewDesc::Texture2D(host.format));

  // Police the block_bytes/format invariant before anything is submitted; a mismatch removes the device silently without the debug layer.
  const uint32_t expected_block_bytes = FormatBytesPerBlock(host.format);
  if (expected_block_bytes != 0 && expected_block_bytes != host.block_bytes) {
    static std::set<uint32_t> reported_mismatch;
    if (reported_mismatch.insert(format).second) {
      KGFX_ERROR("texture upload BLOCKED for xenos format {} ({}x{}): host format "
                   "{} is {} bytes per block but block_bytes says {}. The placed footprint would "
                   "claim the wrong row pitch and D3D12 would REMOVE THE DEVICE (the game freezes "
                   "while the renderer keeps presenting). Fix MapTextureFormat -- see the k_5_6_5 "
                   "case, where expand_to_rgba8 changed the format and left block_bytes behind.",
                   format, width, height, uint32_t(host.format), expected_block_bytes,
                   host.block_bytes);
    }
    return 0;
  }

  const uint32_t upload_pitch = AlignRow(dst_row_bytes);
  // Each cube face is copied into its own layer, so its footprint has to start on a 512-byte boundary.
  const uint32_t upload_slice = cube ? (upload_pitch * blocks_y + 511u) & ~511u : upload_pitch * blocks_y;
  const uint32_t upload_bytes = upload_slice * depth;
  // Every level shares one upload buffer; placed-footprint offsets must be 512-byte aligned, coarser than the 256-byte row alignment.
  uint32_t total_upload = (upload_bytes + 511u) & ~511u;
  for (MipUpload& m : mips) {
    m.upload_pitch = AlignRow(m.row_bytes);
    m.upload_offset = total_upload;
    total_upload = (total_upload + m.upload_pitch * m.blocks_y + 511u) & ~511u;
  }
  entry.upload = CreateBufferChecked(ctx.device, plume::RenderBufferDesc::UploadBuffer(total_upload), "asset texture upload");
  if (!entry.upload) {
    return 0;
  }
  if (auto* mapped = static_cast<uint8_t*>(entry.upload->map())) {
    for (uint32_t z = 0; z < depth; ++z) {
      for (uint32_t y = 0; y < blocks_y; ++y) {
        std::memcpy(mapped + size_t(z) * upload_slice + size_t(y) * upload_pitch,
                    pixels + (size_t(z) * blocks_y + y) * dst_row_bytes, dst_row_bytes);
      }
    }
    for (const MipUpload& m : mips) {
      for (uint32_t y = 0; y < m.blocks_y; ++y) {
        std::memcpy(mapped + m.upload_offset + size_t(y) * m.upload_pitch,
                    m.bytes.data() + size_t(y) * m.row_bytes, m.row_bytes);
      }
    }
    entry.upload->unmap();
  } else {
    return 0;
  }

  SuspendOpenQuery(list);
  list->barriers(plume::RenderBarrierStage::COPY,
                 plume::RenderTextureBarrier(entry.texture.get(),
                                             plume::RenderTextureLayout::COPY_DEST));
  for (uint32_t face = 0; face < (cube ? depth : 1u); ++face) {
    auto src_loc = plume::RenderTextureCopyLocation::PlacedFootprint(entry.upload.get(), host.format, width, height, cube ? 1u : depth, upload_pitch / host.block_bytes * host.block, uint64_t(face) * upload_slice);
    auto dst_loc = plume::RenderTextureCopyLocation::Subresource(entry.texture.get(), 0, face);
    list->copyTextureRegion(dst_loc, src_loc, 0, 0, 0, nullptr);
  }
  for (const MipUpload& m : mips) {
    auto mip_src = plume::RenderTextureCopyLocation::PlacedFootprint(
        entry.upload.get(), host.format, m.width, m.height, 1,
        m.upload_pitch / host.block_bytes * host.block, m.upload_offset);
    auto mip_dst =
        plume::RenderTextureCopyLocation::Subresource(entry.texture.get(), m.level, 0);
    list->copyTextureRegion(mip_dst, mip_src, 0, 0, 0, nullptr);
  }
  if (!mips.empty()) {
    ++g_mippedTextures;
    g_mipLevelsUploaded += uint32_t(mips.size());
  }
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(entry.texture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  g_uploadGraveyard.queue(std::move(entry.upload), ctx.frame_index);

  // Writing past the heap's end corrupts other draws' descriptors, so exhaustion falls back to slot 0.
  const uint32_t slot = g_textureSlots.allocate();
  if (slot == kNoSlot) {
    if (!g_heapExhausted) {
      g_heapExhausted = true;
      KGFX_ERROR("bindless texture heap exhausted at {} slots in a single frame; "
                   "further textures fall back to the placeholder.",
                   g_textureSlots.capacity());
    }
    return 0;
  }
  entry.heap_index = slot;
  entry.guest_address = address;
  entry.last_used_frame = g_drawFrame;
  entry.width = width;
  entry.height = height;
  entry.volume = volume;
  entry.content_hash = content_hash;
  entry.replacement_channels = replacement_channels;
  entry.replaceable = pack_format;
  entry.replacement_pending = has_replacement;
  if (volume_out) {
    *volume_out = volume;
  }
  // Registered into the set matching the resource: XenosRecomp reads 3D and cube fetches from their own heaps, and a 2D-only registration made every 3D or cube sample read a null descriptor.
  plume::RenderDescriptorSet* set = ctx.texture_set;
  if (volume) {
    set = ctx.texture3d_set != nullptr ? ctx.texture3d_set : set;
  } else if (want_dimension == 2) {
    set = ctx.cube_set != nullptr ? ctx.cube_set : set;
  }
  set->setTexture(entry.heap_index, entry.texture.get(),
                  plume::RenderTextureLayout::SHADER_READ, entry.view.get());

  auto inserted = g_textures.emplace(key, std::move(entry)).first;
  ApplyReadyReplacement(ctx, list, inserted->second);
  return inserted->second.heap_index;
}

}  // namespace kameo::gpu::detail
