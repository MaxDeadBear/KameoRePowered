#include "gpu/vertex_transcode.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gpu/draw/draw_constants.h"
#include "gpu/draw/geometry_cache.h"
#include "gpu/guest_memory.h"

namespace kameo::gpu::detail {

SpanHash g_spanHashes[kSpanHashSlots];
SpanHash g_renderSpanHashes[kSpanHashSlots];

// A costume swap rewrites a buffer in place and sampling windows misses a rewrite between them, so the whole span is hashed.
uint64_t FrameSpanHashIn(SpanHash* table, const uint8_t* src, size_t len, uint64_t frame) {
  const uint64_t key = MixGeometryKey(reinterpret_cast<uintptr_t>(src), len);
  const uint32_t home = uint32_t(key * 0x9E3779B97F4A7C15ull >> 51);
  for (uint32_t probe = 0; probe < 8; ++probe) {
    SpanHash& slot = table[(home + probe) & (kSpanHashSlots - 1)];
    if (slot.frame == frame && slot.key == key) return slot.hash;
    if (slot.frame != frame) {
      slot = SpanHash{key, GeometryContentHash(src, len), frame};
      return slot.hash;
    }
  }
  return GeometryContentHash(src, len);
}

uint64_t FrameSpanHash(const uint8_t* src, size_t len, uint64_t frame) { return FrameSpanHashIn(g_spanHashes, src, len, frame); }

uint64_t HashVertexWindow(const HostLayout& layout, const VertexStream* streams, uint32_t first, uint32_t count, SpanHash* table, uint64_t frame, uint64_t* reuse_key) {
  uint64_t source_hash = 0;
  uint32_t hashed_streams = 0;
  for (const HostElement& e : layout.elements) {
    const VertexStream& stream = streams[e.stream];
    if (hashed_streams & (1u << e.stream)) {
      continue;
    }
    hashed_streams |= 1u << e.stream;
    *reuse_key = MixGeometryKey(*reuse_key, e.stream);
    *reuse_key = MixGeometryKey(*reuse_key, reinterpret_cast<uintptr_t>(stream.data));
    *reuse_key = MixGeometryKey(*reuse_key, stream.stride);
    *reuse_key = MixGeometryKey(*reuse_key, stream.size);
    const size_t span = size_t(count - 1) * stream.stride + stream.element_end;
    const size_t available = size_t(stream.size) > size_t(first) * stream.stride
                                 ? stream.size - size_t(first) * stream.stride
                                 : 0;
    const size_t len = std::min(span, available);
    if (len == 0) {
      continue;
    }
    const uint8_t* src = stream.data + size_t(first) * stream.stride;
    source_hash = MixGeometryKey(source_hash, e.stream);
    source_hash = MixGeometryKey(source_hash, FrameSpanHashIn(table, src, len, frame));
    source_hash = MixGeometryKey(source_hash, len);
  }
  return source_hash;
}

bool TranscodeVertexWindow(const HostLayout& layout, const VertexStream* streams, uint32_t first, uint32_t count, uint8_t* dst_base, const float* viewport) {
  for (const HostElement& e : layout.elements) {
    const VertexStream& stream = streams[e.stream];
    if (!stream.data) {
      return false;
    }
    // The op switch sits outside the per-vertex loop so each case vectorises; this is the hottest loop in the renderer (15.1 ms a frame for 9.7 MB when it branched per vertex).
    const uint8_t* const src0 = stream.data + size_t(first) * stream.stride + e.src_offset;
    uint8_t* const dst0 = dst_base + e.dst_offset;
    const size_t sstride = stream.stride;
    const size_t dstride = layout.dst_stride;
    const bool screen_space_position = e.usage == kUsagePosition && e.components == 2 && e.dst_size >= 8;

    switch (e.op) {
      case kSwap16: {
        const uint32_t components = e.components;
        for (uint32_t v = 0; v < count; ++v) {
          const uint8_t* src = src0 + size_t(v) * sstride;
          uint8_t* dst = dst0 + size_t(v) * dstride;
          for (uint32_t c = 0; c < components; ++c) {
            uint16_t value;
            std::memcpy(&value, src + 2 * c, 2);
            value = Swap16(value);
            std::memcpy(dst + 2 * c, &value, 2);
          }
        }
        break;
      }
      case kSwap32: {
        // Usually one dword (D3DCOLOR, DEC3N, UBYTE4N) or a float3 position, so the 1- and 3-unit shapes get straight-line loops.
        const uint32_t units = e.src_size / 4;
        if (units == 1) {
          for (uint32_t v = 0; v < count; ++v) {
            uint32_t value;
            std::memcpy(&value, src0 + size_t(v) * sstride, 4);
            value = Swap32(value);
            std::memcpy(dst0 + size_t(v) * dstride, &value, 4);
          }
        } else if (units == 3) {
          for (uint32_t v = 0; v < count; ++v) {
            const uint8_t* src = src0 + size_t(v) * sstride;
            uint8_t* dst = dst0 + size_t(v) * dstride;
            uint32_t a, b, c;
            std::memcpy(&a, src + 0, 4);
            std::memcpy(&b, src + 4, 4);
            std::memcpy(&c, src + 8, 4);
            a = Swap32(a);
            b = Swap32(b);
            c = Swap32(c);
            std::memcpy(dst + 0, &a, 4);
            std::memcpy(dst + 4, &b, 4);
            std::memcpy(dst + 8, &c, 4);
          }
        } else {
          for (uint32_t v = 0; v < count; ++v) {
            const uint8_t* src = src0 + size_t(v) * sstride;
            uint8_t* dst = dst0 + size_t(v) * dstride;
            for (uint32_t c = 0; c < units; ++c) {
              uint32_t value;
              std::memcpy(&value, src + 4 * c, 4);
              value = Swap32(value);
              std::memcpy(dst + 4 * c, &value, 4);
            }
          }
        }
        break;
      }
      case kShortToFloat:
      case kUShortToFloat: {
        const bool is_signed = (e.op == kShortToFloat);
        const uint32_t components = e.components;
        for (uint32_t v = 0; v < count; ++v) {
          const uint8_t* src = src0 + size_t(v) * sstride;
          uint8_t* dst = dst0 + size_t(v) * dstride;
          for (uint32_t c = 0; c < components; ++c) {
            uint16_t raw;
            std::memcpy(&raw, src + 2 * c, 2);
            raw = Swap16(raw);
            const float value = is_signed ? float(int16_t(raw)) : float(raw);
            std::memcpy(dst + 4 * c, &value, 4);
          }
        }
        break;
      }
      case kByteToFloat: {
        for (uint32_t v = 0; v < count; ++v) {
          uint32_t packed;
          std::memcpy(&packed, src0 + size_t(v) * sstride, 4);
          packed = Swap32(packed);
          uint8_t* dst = dst0 + size_t(v) * dstride;
          for (uint32_t c = 0; c < 4; ++c) {
            const float value = float((packed >> (8 * c)) & 0xFF);
            std::memcpy(dst + 4 * c, &value, 4);
          }
        }
        break;
      }
      case kDec3ToR11G11B10: {
        // tfetchR11G11B10 decodes (sign ? -1 : 0) + magnitude/scale, so encode exactly that; BuildHostLayout no longer emits this op, which is kept for reverting the DEC3N decode.
        auto encode = [](float value, uint32_t magnitude_bits) {
          const float scale = float(1u << magnitude_bits);
          const float c = std::clamp(value, -1.0f, 1.0f - 1.0f / scale);
          const uint32_t sign = (c < 0.0f) ? 1u : 0u;
          const uint32_t mag =
              uint32_t(std::lround((c + float(sign)) * scale)) & ((1u << magnitude_bits) - 1u);
          return (sign << magnitude_bits) | mag;
        };
        for (uint32_t v = 0; v < count; ++v) {
          uint32_t packed;
          std::memcpy(&packed, src0 + size_t(v) * sstride, 4);
          packed = Swap32(packed);
          // Guest DEC3N: three signed 10-bit fields at 0..9, 10..19, 20..29.
          auto decode = [packed](uint32_t shift) {
            const int32_t raw = int32_t(packed << (22 - shift)) >> 22;
            return float(raw) / 511.0f;
          };
          // x: magnitude bits 0..9, sign bit 10; y: the same shifted 11; z: magnitude bits 22..30, sign bit 31.
          const uint32_t out = encode(decode(0), 10) | (encode(decode(10), 10) << 11) |
                               (encode(decode(20), 9) << 22);
          std::memcpy(dst0 + size_t(v) * dstride, &out, 4);
        }
        break;
      }
      case kDec3ToFloat: {
        for (uint32_t v = 0; v < count; ++v) {
          uint32_t packed;
          std::memcpy(&packed, src0 + size_t(v) * sstride, 4);
          packed = Swap32(packed);
          uint8_t* dst = dst0 + size_t(v) * dstride;
          for (uint32_t c = 0; c < 3; ++c) {
            const uint32_t field = (packed >> (10 * c)) & 0x3FF;
            // Sign extend from 10 bits, then normalise the way DEC3N does.
            const int32_t signed_field = int32_t(field << 22) >> 22;
            const float value = float(signed_field) / 511.0f;
            std::memcpy(dst + 4 * c, &value, 4);
          }
        }
        break;
      }
      case kFloat3ToDec3: {
        for (uint32_t v = 0; v < count; ++v) {
          const uint8_t* src = src0 + size_t(v) * sstride;
          uint32_t packed = 0;
          for (uint32_t c = 0; c < 3; ++c) {
            uint32_t bits;
            std::memcpy(&bits, src + 4 * c, 4);
            bits = Swap32(bits);
            float value;
            std::memcpy(&value, &bits, 4);
            const int32_t field = std::lround(std::clamp(std::isfinite(value) ? value : 0.0f, -1.0f, 1.0f) * 511.0f);
            packed |= (uint32_t(field) & 0x3FFu) << (10 * c);
          }
          std::memcpy(dst0 + size_t(v) * dstride, &packed, 4);
        }
        break;
      }
    }

    // Pre-transformed geometry uses window coordinates inside the active guest viewport.
    if (screen_space_position) {
      float max_extent = 0.0f;
      for (uint32_t v = 0; v < count; ++v) {
        float xy[2];
        std::memcpy(xy, dst0 + size_t(v) * dstride, sizeof(xy));
        max_extent = std::max(max_extent, std::max(std::fabs(xy[0]), std::fabs(xy[1])));
      }
      if (max_extent > 2.0f) {
        const float vp_x = viewport ? viewport[0] : 0.0f;
        const float vp_y = viewport ? viewport[1] : 0.0f;
        const float vp_w = viewport && viewport[2] > 0.0f ? viewport[2] : float(kGuestWidth);
        const float vp_h = viewport && viewport[3] > 0.0f ? viewport[3] : float(kGuestHeight);
        for (uint32_t v = 0; v < count; ++v) {
          uint8_t* dst = dst0 + size_t(v) * dstride;
          float xy[2];
          std::memcpy(xy, dst, sizeof(xy));
          xy[0] = (xy[0] - vp_x) * (2.0f / vp_w) - 1.0f;
          xy[1] = 1.0f - (xy[1] - vp_y) * (2.0f / vp_h);
          std::memcpy(dst, xy, sizeof(xy));
        }
      }
    }
  }

  return true;
}

}  // namespace kameo::gpu::detail
