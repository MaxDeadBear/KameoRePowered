#include "gpu/constant_buffers.h"

#include <algorithm>
#include <cstring>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

#include "gpu/config.h"
#include "gpu/frame_ring.h"
#include "gpu/guest_memory.h"

namespace kameo::gpu::detail {

ConstantDedup g_vsDedup{dev::kDirtyVertexConstants, dev::kVertexShaderFloatConstants};
ConstantDedup g_psDedup{dev::kDirtyPixelConstants, dev::kPixelShaderFloatConstants};

namespace {

// Chunk k of a register file is registers 4k..4k+3 and bit 63-k of its dirty mask, as D3DDevice_SetVertexShaderConstantFN and SetPixelShaderConstantFN set it. The chunks covering bytes [begin, end):
uint64_t ConstantChunkBits(uint32_t begin, uint32_t end) {
  if (end <= begin) return 0;
  const uint32_t first = begin / 64;
  const uint32_t last = (end - 1) / 64;
  const uint64_t through_last = last >= 63 ? ~0ull : ~(~0ull >> (last + 1));
  const uint64_t before_first = first == 0 ? 0 : ~(~0ull >> first);
  return through_last & ~before_first;
}

// Brings the host copy up to date with every chunk the guest marked since the last capture, and clears the mask as the draw's own lazy flush did.
void SyncConstantFile(uint8_t* base, uint32_t device, ConstantDedup* dedup) {
  uint64_t mask = REX_LOAD_U64(device + dedup->dirty_offset);
  if (!dedup->primed) {
    mask = ~0ull;
    dedup->primed = true;
  }
  if (mask == 0) return;
  REX_STORE_U64(device + dedup->dirty_offset, 0);
  dedup->changed |= mask;
  const auto* words = reinterpret_cast<const uint32_t*>(GuestData(base, device + dedup->file_offset));
  while (mask) {
    const uint32_t chunk = uint32_t(__builtin_clzll(mask));
    mask &= ~(1ull << (63 - chunk));
    SwapConstantChunk(words + chunk * 16, dedup->host + chunk * 16);
  }
}

// The render thread's copies of the two register files, rebuilt from each frame's constant records.
alignas(32) uint32_t g_renderConstants[2][kConstantBytes / 4];

// Draws sharing a record are consecutive for their stage, so the register file still holds its state whenever a later one asks for another copy.
void MaterializeConstantRecord(DrawFrame& frame, uint32_t record, uint32_t* file, bool want_arena, uint32_t* arena, uint32_t* ring) {
  *arena = kNoData;
  *ring = kNoData;
  if (record == kNoData) return;
  ConstantRecord header;
  std::memcpy(&header, frame.at(record), sizeof(header));
  if (!header.applied) {
    const uint8_t* in = frame.at(record) + sizeof(ConstantRecord);
    for (uint64_t mask = header.mask; mask; in += 64) {
      const uint32_t chunk = uint32_t(__builtin_clzll(mask));
      mask &= ~(1ull << (63 - chunk));
      std::memcpy(file + chunk * 16, in, 64);
    }
    header.applied = 1;
    header.ring = g_constantRing.write(file, header.needed, kRootConstantAlign);
  }
  if ((want_arena || header.ring == kNoData) && header.arena == kNoData) {
    header.arena = frame.alloc(header.needed, 16);
    std::memcpy(frame.at(header.arena), file, header.needed);
  }
  std::memcpy(frame.at(record), &header, sizeof(header));
  *arena = header.arena;
  *ring = header.ring;
}

}  // namespace

ConstantSpan SpanOf(ShaderConstantRange range) {
  if (!REXCVAR_GET(kameo_gfx_constant_range)) return {0, kConstantBytes};
  if (range.hi <= range.lo) return {0, 0};
  const uint32_t offset = (uint32_t(range.lo) * 16) & ~127u;
  const uint32_t end = std::min<uint32_t>((uint32_t(range.hi) * 16 + 127) & ~127u, kConstantBytes);
  return {offset, end - offset};
}

// Xenos flushed a multiply by zero to zero; DXIL keeps NaN, so registers the guest never wrote as floats would poison any shader reading them with a zero weight.
void SwapConstantChunk(const uint32_t* words, uint32_t* host) {
#if defined(__AVX2__)
  const __m256i swap = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
  const __m256i exponent = _mm256_set1_epi32(0x7F800000);
  for (uint32_t i = 0; i < 16; i += 8) {
    const __m256i value = _mm256_shuffle_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(words + i)), swap);
    const __m256i not_finite = _mm256_cmpeq_epi32(_mm256_and_si256(value, exponent), exponent);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(host + i), _mm256_andnot_si256(not_finite, value));
  }
#else
  for (uint32_t i = 0; i < 16; ++i) {
    const uint32_t value = Swap32(words[i]);
    host[i] = (value & 0x7F800000u) == 0x7F800000u ? 0u : value;
  }
#endif
}

bool ConstantsUnchanged(uint8_t* base, uint32_t device, const ConstantDedup& dedup, ConstantSpan span) {
  const uint64_t pending = dedup.primed ? REX_LOAD_U64(device + dedup.dirty_offset) : ~0ull;
  return ((dedup.changed | pending) & ConstantChunkBits(span.offset, span.offset + span.bytes)) == 0;
}

uint32_t CaptureConstants(uint8_t* base, uint32_t device, ConstantDedup* dedup, DrawFrame* frame, ShaderConstantRange range, uint32_t* out_bytes) {
  SyncConstantFile(base, device, dedup);
  const ConstantSpan span = SpanOf(range);
  const uint32_t needed = std::min<uint32_t>((span.offset + span.bytes + kRootConstantAlign - 1) & ~(kRootConstantAlign - 1), kConstantBytes);
  uint32_t offset = dedup->offset;
  if (offset != kNoData && dedup->covered >= needed && (dedup->changed & ConstantChunkBits(span.offset, span.offset + span.bytes)) == 0) {
    ++dedup->hits;
    *out_bytes = dedup->covered;
  } else {
    // Only chunks written since the stage's previous record go into the frame; a frame's first record carries the whole file, since frames can be dropped between.
    ++dedup->misses;
    const uint64_t carried = offset == kNoData ? ~0ull : dedup->changed;
    const uint32_t chunks = uint32_t(__builtin_popcountll(carried));
    offset = frame->alloc(uint32_t(sizeof(ConstantRecord)) + chunks * 64, 16);
    const ConstantRecord header{carried, needed, kNoData, kNoData, 0};
    uint8_t* record = frame->at(offset);
    std::memcpy(record, &header, sizeof(header));
    uint8_t* out = record + sizeof(ConstantRecord);
    for (uint64_t mask = carried; mask; out += 64) {
      const uint32_t chunk = uint32_t(__builtin_clzll(mask));
      mask &= ~(1ull << (63 - chunk));
      std::memcpy(out, dedup->host + chunk * 16, 64);
    }
    dedup->offset = offset;
    dedup->covered = needed;
    dedup->changed = 0;
    *out_bytes = needed;
  }
  return offset;
}

void MaterializeConstants(DrawFrame& frame) {
  const bool strip = !REXCVAR_GET(kameo_gfx_specular) || !REXCVAR_GET(kameo_gfx_bump_maps);
  for (DrawCall& call : frame.draws) {
    MaterializeConstantRecord(frame, call.vs_constants, g_renderConstants[0], call.hud_2d, &call.vs_constants, &call.vs_ring);
    MaterializeConstantRecord(frame, call.ps_constants, g_renderConstants[1], strip, &call.ps_constants, &call.ps_ring);
  }
}

}  // namespace kameo::gpu::detail
