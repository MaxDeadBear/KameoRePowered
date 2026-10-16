#pragma once

#include <cstdint>

#include "gpu/draw/draw_types.h"
#include "gpu/graphics_system.h"
#include "gpu/guest_device.h"

namespace kameo::gpu::detail {

inline constexpr uint32_t kConstantRegisters = 256;
inline constexpr uint32_t kConstantBytes = kConstantRegisters * 16;  // 0x1000, both stages
inline constexpr uint32_t kSharedConstantBytes = 512;
inline constexpr uint32_t kRootConstantAlign = 256;  // D3D12 root CBV alignment

// Per-stage capture state: the host register file and the frame's current block, so consecutive draws that change no register they read share one record.
struct ConstantDedup {
  explicit ConstantDedup(uint32_t dirty, uint32_t file) : dirty_offset(dirty), file_offset(file) {}
  uint32_t dirty_offset;  // the stage's dirty mask in the device
  uint32_t file_offset;   // the stage's register file in the device
  // The stage's register file as the host reads it, byte-swapped and NaN-flushed, kept current chunk by chunk from the guest's dirty mask.
  alignas(32) uint32_t host[kConstantBytes / 4] = {};
  bool primed = false;
  // Chunks the guest wrote since the block at `offset` was built.
  uint64_t changed = 0;
  uint32_t offset = kNoData;
  // How many bytes of the block, from register zero, were copied: a shader reaching past that cannot reuse it.
  uint32_t covered = 0;
  uint32_t hits = 0;
  uint32_t misses = 0;
  void reset() {
    offset = kNoData;
    covered = 0;
    changed = 0;
  }
};
extern ConstantDedup g_vsDedup;
extern ConstantDedup g_psDedup;

// The byte span of a shader's constant range, widened to whole 128-byte blocks so the compare needs no tail.
struct ConstantSpan {
  uint32_t offset = 0;
  uint32_t bytes = kConstantBytes;
};

ConstantSpan SpanOf(ShaderConstantRange range);
// Swaps and NaN-flushes 16 guest dwords into the host copy.
void SwapConstantChunk(const uint32_t* words, uint32_t* host);
// True when no chunk of the span changed since the stage's current block was built, counting what the guest has written since the last capture.
bool ConstantsUnchanged(uint8_t* base, uint32_t device, const ConstantDedup& dedup, ConstantSpan span);
// The stage's constant block for a draw: the previous record while no register the shader reads has been written since, otherwise a new one.
uint32_t CaptureConstants(uint8_t* base, uint32_t device, ConstantDedup* dedup, DrawFrame* frame, ShaderConstantRange range, uint32_t* out_bytes);
// Replays the frame's constant records in capture order into the upload ring, keeping an arena copy only where the CPU reads the block.
void MaterializeConstants(DrawFrame& frame);

}  // namespace kameo::gpu::detail
