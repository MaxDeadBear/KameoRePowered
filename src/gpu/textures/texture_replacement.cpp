#include "gpu/textures/texture_replacement.h"

#include <chrono>
#include <cstring>
#include <unordered_set>
#include <vector>

#include <plume_render_interface.h>

#include "gpu/log.h"
#include "gpu/occlusion.h"
#include "gpu/textures/texture_codec.h"
#include "gpu/textures/texture_heap.h"
#include "gpu/textures/texture_pack.h"

namespace kameo::gpu::detail {

int64_t g_replacementUploadLeft = kReplacementUploadBytesPerFrame;
uint64_t g_replacementBytes = 0;
uint32_t g_replacementsLive = 0;
uint64_t g_nsReplacementUpload = 0;
PackSkips g_packSkips;

namespace {

// Builds a mipmapped RGBA8 texture from a decoded replacement image into `entry`, which keeps its guest identity.
bool UploadReplacementTexture(GraphicsSystem::DrawPathContext& ctx, plume::RenderCommandList* list, const DecodedReplacement& image, CachedTexture* entry) {
  const auto start = std::chrono::steady_clock::now();
  const plume::RenderFormat format = plume::RenderFormat::R8G8B8A8_UNORM;
  const ReplacementLevel& top = image.levels.front();
  auto texture = ctx.device->createTexture(plume::RenderTextureDesc::Texture2D(top.width, top.height, uint32_t(image.levels.size()), format));
  if (!texture) return false;
  auto view = texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(format));
  std::vector<uint32_t> offsets;
  uint32_t total = 0;
  for (const ReplacementLevel& level : image.levels) {
    offsets.push_back(total);
    total = (total + AlignRow(level.width * 4) * level.height + 511u) & ~511u;
  }
  auto upload = ctx.device->createBuffer(plume::RenderBufferDesc::UploadBuffer(total));
  auto* mapped = upload ? static_cast<uint8_t*>(upload->map()) : nullptr;
  if (!mapped) return false;
  for (size_t i = 0; i < image.levels.size(); ++i) {
    const ReplacementLevel& level = image.levels[i];
    const uint32_t pitch = AlignRow(level.width * 4);
    for (uint32_t y = 0; y < level.height; ++y) std::memcpy(mapped + offsets[i] + size_t(y) * pitch, level.rgba.data() + size_t(y) * level.width * 4, size_t(level.width) * 4);
  }
  upload->unmap();
  const uint32_t slot = g_textureSlots.allocate();
  if (slot == kNoSlot) return false;
  SuspendOpenQuery(list);
  list->barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(texture.get(), plume::RenderTextureLayout::COPY_DEST));
  for (size_t i = 0; i < image.levels.size(); ++i) {
    const ReplacementLevel& level = image.levels[i];
    auto src = plume::RenderTextureCopyLocation::PlacedFootprint(upload.get(), format, level.width, level.height, 1, AlignRow(level.width * 4) / 4, offsets[i]);
    auto dst = plume::RenderTextureCopyLocation::Subresource(texture.get(), uint32_t(i), 0);
    list->copyTextureRegion(dst, src, 0, 0, 0, nullptr);
  }
  list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(texture.get(), plume::RenderTextureLayout::SHADER_READ));
  g_uploadGraveyard.queue(std::move(upload), ctx.frame_index);
  ctx.texture_set->setTexture(slot, texture.get(), plume::RenderTextureLayout::SHADER_READ, view.get());
  entry->texture = std::move(texture);
  entry->view = std::move(view);
  entry->heap_index = slot;
  entry->volume = false;
  entry->replacement_pending = false;
  entry->replacement_bytes = image.bytes;
  g_replacementBytes += image.bytes;
  ++g_replacementsLive;
  g_replacementUploadLeft -= int64_t(image.bytes);
  g_nsReplacementUpload += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
  return true;
}

}  // namespace

void ApplyReadyReplacement(GraphicsSystem::DrawPathContext& ctx, plume::RenderCommandList* list, CachedTexture& cached) {
  if (!cached.replacement_pending || g_replacementUploadLeft <= 0) return;
  const auto image = RequestTextureReplacement(cached.content_hash, cached.replacement_channels);
  if (!image) return;
  CachedTexture replaced;
  replaced.guest_address = cached.guest_address;
  replaced.width = cached.width;
  replaced.height = cached.height;
  replaced.last_used_frame = cached.last_used_frame;
  replaced.content_hash = cached.content_hash;
  replaced.replacement_channels = cached.replacement_channels;
  replaced.replaceable = cached.replaceable;
  if (!UploadReplacementTexture(ctx, list, *image, &replaced)) {
    cached.replacement_pending = false;
    return;
  }
  if (cached.replacement_live) KGFX_INFO("texture pack: {:016X} ({}x{} at {:08X}) now draws a {}x{} image", cached.content_hash, cached.width, cached.height, cached.guest_address, image->levels.front().width, image->levels.front().height);
  std::swap(cached, replaced);
  g_textureGraveyard.queue(std::move(replaced), ctx.frame_index);
}

void ApplyTexturePackChanges(GraphicsSystem::DrawPathContext& ctx) {
  if (TakeDumpOfLoadedTexturesRequest() && REXCVAR_GET(kameo_gfx_dump_textures)) {
    uint32_t reloaded = 0;
    for (auto it = g_textures.begin(); it != g_textures.end();) {
      if (!it->second.replaceable || TextureWasDumped(it->second.content_hash)) {
        ++it;
        continue;
      }
      g_textureGraveyard.queue(std::move(it->second), ctx.frame_index);
      it = g_textures.erase(it);
      ++reloaded;
    }
    if (reloaded) KGFX_INFO("texture pack: reloading {} texture(s) loaded while dumping was off, so they dump", reloaded);
  }
  const std::vector<uint64_t> changed = TakeChangedReplacements();
  if (changed.empty()) return;
  const std::unordered_set<uint64_t> hashes(changed.begin(), changed.end());
  uint32_t refreshing = 0, reverted = 0;
  for (auto it = g_textures.begin(); it != g_textures.end();) {
    CachedTexture& cached = it->second;
    if (!cached.replaceable || !hashes.contains(cached.content_hash)) {
      ++it;
      continue;
    }
    if (HasTextureReplacement(cached.content_hash)) {
      cached.replacement_pending = true;
      cached.replacement_live = true;
      ++refreshing;
      ++it;
      continue;
    }
    cached.replacement_pending = false;
    if (cached.replacement_bytes == 0) {
      ++it;
      continue;
    }
    g_textureGraveyard.queue(std::move(cached), ctx.frame_index);
    it = g_textures.erase(it);
    ++reverted;
  }
  if (refreshing || reverted) KGFX_INFO("texture pack: {} texture(s) taking a new image, {} restored to the original", refreshing, reverted);
}

}  // namespace kameo::gpu::detail
