#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "gpu/draw/indices.h"

namespace kameo::gpu::detail {

// Draws whose vertices have not been read out of the guest's ring yet.
extern std::vector<uint32_t> g_pendingDeferred;
// Draws folded into the one before them since the last report.
extern std::atomic<uint64_t> g_mergedDraws;
extern std::atomic<uint64_t> g_mergedIndexedDraws;

// Reads the vertices of every BeginVertices draw still waiting on the guest; called with g_captureMutex held.
void ResolveDeferredVertices(uint8_t* base);
// Writes an indexed merge run's combined list into the arena, ending the run.
void FinishIndexMerge();
// Folds an indexed draw into the one before it when both read the same transcoded vertex buffer and everything else captured would be identical.
bool TryMergeIndexedDraw(uint8_t* base, uint32_t device, uint32_t primitive, const ExpandedIndices& pre);
// Folds a BeginVertices draw into the pending one before it when everything but the vertices would be captured identically, as the particle systems issue one quad per call.
bool TryMergeBeginVertices(uint8_t* base, uint32_t device, uint32_t primitive, uint32_t vertex_count, uint32_t ring, uint32_t stride);
void ResetDrawMerges();

}  // namespace kameo::gpu::detail
