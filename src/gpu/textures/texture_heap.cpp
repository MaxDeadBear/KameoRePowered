#include "gpu/textures/texture_heap.h"

#include <algorithm>

#include "gpu/config.h"
#include "gpu/log.h"

namespace kameo::gpu::detail {

BindlessSlots g_textureSlots;
DeferredDestroyQueue<std::unique_ptr<plume::RenderBuffer>> g_uploadGraveyard;
uint32_t g_flatNormalSlot = 0;
uint32_t g_emptySlot = 0;

namespace {

// Heap slot 0 is a 1x1 magenta texel bound wherever a draw's texture could not be resolved: an empty heap entry samples zero, which through a modulate painted every missing texture as a flat black polygon.
std::unique_ptr<plume::RenderTexture> g_fallbackTexture;
std::unique_ptr<plume::RenderTextureView> g_fallbackView;
std::unique_ptr<plume::RenderBuffer> g_fallbackUpload;
// A 1x1 texel reading as a BC5 sample of the straight-up normal (x and y at the midpoint, blue 0, alpha 1), bound in place of normal maps when bump mapping is off.
std::unique_ptr<plume::RenderTexture> g_flatNormalTexture;
std::unique_ptr<plume::RenderTextureView> g_flatNormalView;
std::unique_ptr<plume::RenderBuffer> g_flatNormalUpload;
// A 1x1 transparent black texel for samplers the guest left without a texture object, which read as zero on the console and under Xenia; magenta stays for textures that exist but failed to load.
std::unique_ptr<plume::RenderTexture> g_emptySlotTexture;
std::unique_ptr<plume::RenderTextureView> g_emptySlotView;
std::unique_ptr<plume::RenderBuffer> g_emptySlotUpload;

}  // namespace

void EnsurePlaceholderTextures(GraphicsSystem::DrawPathContext& ctx, plume::RenderCommandList* list) {
  if (g_fallbackTexture) {
    return;
  }
  g_fallbackTexture = ctx.device->createTexture(
      plume::RenderTextureDesc::Texture2D(1, 1, 1, plume::RenderFormat::R8G8B8A8_UNORM));
  if (!g_fallbackTexture) {
    return;
  }
  g_fallbackView = g_fallbackTexture->createTextureView(
      plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R8G8B8A8_UNORM));
  g_fallbackUpload = ctx.device->createBuffer(plume::RenderBufferDesc::UploadBuffer(256));
  if (!g_fallbackUpload) {
    return;
  }
  if (auto* mapped = static_cast<uint8_t*>(g_fallbackUpload->map())) {
    // Magenta rather than white: it cannot be mistaken for anything the game draws.
    mapped[0] = 0xFF;  // R
    mapped[1] = 0x00;  // G
    mapped[2] = 0xFF;  // B
    mapped[3] = 0xFF;  // A
    g_fallbackUpload->unmap();
  }
  list->barriers(plume::RenderBarrierStage::COPY,
                 plume::RenderTextureBarrier(g_fallbackTexture.get(),
                                             plume::RenderTextureLayout::COPY_DEST));
  auto src = plume::RenderTextureCopyLocation::PlacedFootprint(
      g_fallbackUpload.get(), plume::RenderFormat::R8G8B8A8_UNORM, 1, 1, 1, 1, 0);
  auto dst = plume::RenderTextureCopyLocation::Subresource(g_fallbackTexture.get(), 0, 0);
  list->copyTextureRegion(dst, src, 0, 0, 0, nullptr);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(g_fallbackTexture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  ctx.texture_set->setTexture(0, g_fallbackTexture.get(), plume::RenderTextureLayout::SHADER_READ,
                              g_fallbackView.get());
  g_textureSlots.reset(kMaxBindlessTextures, 1);
  g_flatNormalTexture = ctx.device->createTexture(plume::RenderTextureDesc::Texture2D(1, 1, 1, plume::RenderFormat::R8G8B8A8_UNORM));
  g_flatNormalUpload = ctx.device->createBuffer(plume::RenderBufferDesc::UploadBuffer(256));
  if (g_flatNormalTexture && g_flatNormalUpload) {
    g_flatNormalView = g_flatNormalTexture->createTextureView(plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R8G8B8A8_UNORM));
    if (auto* mapped = static_cast<uint8_t*>(g_flatNormalUpload->map())) {
      mapped[0] = 0x80;
      mapped[1] = 0x80;
      mapped[2] = 0x00;
      mapped[3] = 0xFF;
      g_flatNormalUpload->unmap();
    }
    list->barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(g_flatNormalTexture.get(), plume::RenderTextureLayout::COPY_DEST));
    list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(g_flatNormalTexture.get(), 0, 0), plume::RenderTextureCopyLocation::PlacedFootprint(g_flatNormalUpload.get(), plume::RenderFormat::R8G8B8A8_UNORM, 1, 1, 1, 1, 0), 0, 0, 0, nullptr);
    list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(g_flatNormalTexture.get(), plume::RenderTextureLayout::SHADER_READ));
    const uint32_t slot = g_textureSlots.allocate();
    if (slot != kNoSlot) {
      g_flatNormalSlot = slot;
      ctx.texture_set->setTexture(slot, g_flatNormalTexture.get(), plume::RenderTextureLayout::SHADER_READ, g_flatNormalView.get());
    }
  }
  g_emptySlotTexture = ctx.device->createTexture(plume::RenderTextureDesc::Texture2D(1, 1, 1, plume::RenderFormat::R8G8B8A8_UNORM));
  g_emptySlotUpload = ctx.device->createBuffer(plume::RenderBufferDesc::UploadBuffer(256));
  if (g_emptySlotTexture && g_emptySlotUpload) {
    g_emptySlotView = g_emptySlotTexture->createTextureView(plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R8G8B8A8_UNORM));
    if (auto* mapped = static_cast<uint8_t*>(g_emptySlotUpload->map())) {
      mapped[0] = mapped[1] = mapped[2] = mapped[3] = 0x00;
      g_emptySlotUpload->unmap();
    }
    list->barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(g_emptySlotTexture.get(), plume::RenderTextureLayout::COPY_DEST));
    list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(g_emptySlotTexture.get(), 0, 0), plume::RenderTextureCopyLocation::PlacedFootprint(g_emptySlotUpload.get(), plume::RenderFormat::R8G8B8A8_UNORM, 1, 1, 1, 1, 0), 0, 0, 0, nullptr);
    list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(g_emptySlotTexture.get(), plume::RenderTextureLayout::SHADER_READ));
    const uint32_t slot = g_textureSlots.allocate();
    if (slot != kNoSlot) {
      g_emptySlot = slot;
      ctx.texture_set->setTexture(slot, g_emptySlotTexture.get(), plume::RenderTextureLayout::SHADER_READ, g_emptySlotView.get());
    }
  }
}

void ReleasePlaceholderTextures() {
  g_fallbackView.reset();
  g_fallbackTexture.reset();
  g_fallbackUpload.reset();
  g_flatNormalView.reset();
  g_flatNormalTexture.reset();
  g_flatNormalUpload.reset();
  g_flatNormalSlot = 0;
  g_emptySlotView.reset();
  g_emptySlotTexture.reset();
  g_emptySlotUpload.reset();
  g_emptySlot = 0;
}

}  // namespace kameo::gpu::detail
