#pragma once

#include <cstdint>

#include "gpu/draw/draw_types.h"

namespace kameo::gpu::detail {

// Reads the per-draw device state the pipeline key and the shared constants need; offsets come from Kameo's own render-state setters (see gpu/guest_state.h).
void CaptureState(uint8_t* base, uint32_t device, DrawCall* call, bool tally = true);
// Identifies every vertex stream a layout reads, the way CaptureVertices resolves them: a draw that merges into another must read the same buffers, not just the same stream 0.
uint64_t StreamIdentity(uint8_t* base, uint32_t device, const HostLayout& layout);
// Every field CaptureState writes; a field added there must be added here.
bool SameCapturedState(const DrawCall& a, const DrawCall& b);

}  // namespace kameo::gpu::detail
