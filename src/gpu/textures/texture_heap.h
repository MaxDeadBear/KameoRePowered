#pragma once

#include <cstdint>
#include <memory>

#include <plume_render_interface.h>

#include "gpu/bindless.h"
#include "gpu/graphics_system.h"

namespace kameo::gpu::detail {

// Must match EnsureDrawPathResources' kMaxTextures / kMaxSamplers.
inline constexpr uint32_t kMaxBindlessTextures = 16384;
inline constexpr uint32_t kMaxBindlessSamplers = 256;
// Slot 0 is the fallback texture and is never handed out.
extern BindlessSlots g_textureSlots;
// Upload buffers a frame in flight may still be copying from.
extern DeferredDestroyQueue<std::unique_ptr<plume::RenderBuffer>> g_uploadGraveyard;
// The flat normal bound for DXN normal maps while they are off, and the transparent black texel for samplers left without a texture object.
extern uint32_t g_flatNormalSlot;
extern uint32_t g_emptySlot;

// Creates the magenta fallback at slot 0 and the two placeholder textures, once per device.
void EnsurePlaceholderTextures(GraphicsSystem::DrawPathContext& ctx, plume::RenderCommandList* list);
void ReleasePlaceholderTextures();

}  // namespace kameo::gpu::detail
