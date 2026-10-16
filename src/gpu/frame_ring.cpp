#include "gpu/frame_ring.h"

#include <unordered_map>

#include "gpu/textures/texture_heap.h"

namespace kameo::gpu::detail {

UploadRing g_vertexRing;
UploadRing g_indexRing;
UploadRing g_constantRing;

uint64_t g_residentBytes = 0;
uint32_t g_residentCreated = 0;
uint32_t g_residentCreatesLeft = kResidentCreatesPerFrame;

namespace {

struct ResidentGeometry {
  std::weak_ptr<const void> owner;
  std::unique_ptr<plume::RenderBuffer> buffer;
  uint32_t bytes = 0;
};
std::unordered_map<const void*, ResidentGeometry> g_residentGeometry;

}  // namespace

plume::RenderBuffer* ResidentBufferFor(GraphicsSystem::DrawPathContext& ctx, std::shared_ptr<const void> owner, const void* data, uint32_t bytes, plume::RenderBufferFlags flags) {
  auto existing = g_residentGeometry.find(data);
  if (existing != g_residentGeometry.end() && existing->second.buffer && existing->second.bytes >= bytes && !existing->second.owner.owner_before(owner) && !owner.owner_before(existing->second.owner)) return existing->second.buffer.get();
  if (g_residentCreatesLeft == 0) return nullptr;
  --g_residentCreatesLeft;
  ResidentGeometry& entry = existing != g_residentGeometry.end() ? existing->second : g_residentGeometry[data];
  if (entry.buffer) {
    g_residentBytes -= entry.bytes;
    g_uploadGraveyard.queue(std::move(entry.buffer), ctx.frame_index);
  }
  entry.buffer = ctx.device->createBuffer(plume::RenderBufferDesc::UploadBuffer(bytes, flags));
  ++g_residentCreated;
  void* mapped = entry.buffer ? entry.buffer->map() : nullptr;
  if (!mapped) {
    if (entry.buffer) g_uploadGraveyard.queue(std::move(entry.buffer), ctx.frame_index);
    g_residentGeometry.erase(data);
    return nullptr;
  }
  std::memcpy(mapped, data, bytes);
  entry.buffer->unmap();
  entry.owner = owner;
  entry.bytes = bytes;
  g_residentBytes += bytes;
  return entry.buffer.get();
}

void SweepResidentGeometry(GraphicsSystem::DrawPathContext& ctx) {
  for (auto it = g_residentGeometry.begin(); it != g_residentGeometry.end();) {
    if (!it->second.owner.expired()) {
      ++it;
      continue;
    }
    g_residentBytes -= it->second.bytes;
    if (it->second.buffer) g_uploadGraveyard.queue(std::move(it->second.buffer), ctx.frame_index);
    it = g_residentGeometry.erase(it);
  }
}

bool BeginFrameRings(const GraphicsSystem::DrawPathContext& ctx) {
  g_vertexRing.name = "vertex";
  g_indexRing.name = "index";
  g_constantRing.name = "constant";
  // One slice must clear the observed high-water marks: a heavy frame writes ~20 MB of vertices, ~6 MB of indices and ~13 MB of constants, and split-screen filled a 24 MB constant slice.
  if (!g_vertexRing.ensure(ctx.device, 96u << 20, plume::RenderBufferFlag::VERTEX) ||
      !g_indexRing.ensure(ctx.device, 48u << 20, plume::RenderBufferFlag::INDEX) ||
      !g_constantRing.ensure(ctx.device, 96u << 20, plume::RenderBufferFlag::CONSTANT)) {
    return false;
  }
  g_vertexRing.begin(ctx.frame_slot);
  g_indexRing.begin(ctx.frame_slot);
  g_constantRing.begin(ctx.frame_slot);
  return true;
}

void EndFrameRings() {
  g_vertexRing.unmap();
  g_indexRing.unmap();
  g_constantRing.unmap();
}

void ReleaseFrameRings() {
  g_vertexRing.unmap();
  g_indexRing.unmap();
  g_constantRing.unmap();
  g_vertexRing.buffer.reset();
  g_indexRing.buffer.reset();
  g_constantRing.buffer.reset();
  g_residentGeometry.clear();
  g_residentBytes = 0;
}

}  // namespace kameo::gpu::detail
