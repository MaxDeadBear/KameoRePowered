#include "gpu/ui/overlay_renderer.h"

#include <algorithm>

#include <imgui.h>

#include "gpu/ui/ui_image.h"

namespace kameo::gpu {

std::unique_ptr<UiImage> UiImage::Create(uint32_t width, uint32_t height, const uint8_t* rgba, bool repeated) {
  GraphicsSystem* graphics = GraphicsSystem::instance();
  const uint32_t id = graphics ? graphics->CreateUiTexture(width, height, rgba, repeated) : 0;
  if (id == 0) return nullptr;
  return std::unique_ptr<UiImage>(new UiImage(id, width, height));
}

UiImage::~UiImage() {
  if (GraphicsSystem* graphics = GraphicsSystem::instance()) graphics->DestroyUiTexture(id_);
}

// ImGui asks for its own textures (the font atlas, which grows as glyphs are first used) through the draw data.
void OverlayRenderer::UpdateTextures(ImDrawData& draw_data) {
  if (!draw_data.Textures) return;
  for (ImTextureData* texture : *draw_data.Textures) {
    if (texture->Status == ImTextureStatus_OK || texture->Status == ImTextureStatus_Destroyed) continue;
    if (texture->Status == ImTextureStatus_WantDestroy) {
      // Only once no frame of ImGui's still draws with it.
      if (texture->UnusedFrames <= 0) continue;
      graphics_->DestroyUiTexture(uint32_t(texture->GetTexID()));
      texture->SetTexID(ImTextureID_Invalid);
      texture->SetStatus(ImTextureStatus_Destroyed);
      continue;
    }
    const uint8_t* rgba = static_cast<const uint8_t*>(texture->GetPixels());
    if (texture->Format == ImTextureFormat_Alpha8) {
      const size_t pixels = size_t(texture->Width) * size_t(texture->Height);
      expanded_.resize(pixels * 4);
      for (size_t i = 0; i < pixels; ++i) {
        expanded_[i * 4 + 0] = expanded_[i * 4 + 1] = expanded_[i * 4 + 2] = 255;
        expanded_[i * 4 + 3] = rgba[i];
      }
      rgba = expanded_.data();
    }
    if (texture->Status == ImTextureStatus_WantCreate) {
      const uint32_t id = graphics_->CreateUiTexture(uint32_t(texture->Width), uint32_t(texture->Height), rgba);
      // Left wanting creation when there is no id, so the next frame tries again.
      if (id == 0) continue;
      texture->SetTexID(ImTextureID(id));
    } else {
      // The whole texture again: the atlas is small and its updates are rare once the glyphs in use are in it.
      graphics_->UpdateUiTexture(uint32_t(texture->GetTexID()), rgba);
    }
    texture->SetStatus(ImTextureStatus_OK);
  }
}

void OverlayRenderer::RenderOverlays(ImDrawData& draw_data) {
  if (!graphics_) return;
  UpdateTextures(draw_data);

  GraphicsSystem::UiFrame frame;
  const float space_width = draw_data.DisplaySize.x > 0.0f ? draw_data.DisplaySize.x : float(std::max(target_width_, 1u));
  const float space_height = draw_data.DisplaySize.y > 0.0f ? draw_data.DisplaySize.y : float(std::max(target_height_, 1u));
  // ImGui's coordinates to target pixels, which is what the scissor takes.
  const float sx = float(target_width_) / space_width;
  const float sy = float(target_height_) / space_height;
  const ImVec2 origin = draw_data.DisplayPos;

  for (const ImDrawList* list : draw_data.CmdLists) {
    for (const ImDrawCmd& command : list->CmdBuffer) {
      // The only callback ImGui itself issues resets render state, which every draw here sets anyway.
      if (command.UserCallback != nullptr || command.ElemCount == 0) continue;
      const uint32_t texture_id = uint32_t(command.GetTexID());
      if (texture_id == 0) continue;

      GraphicsSystem::UiDraw entry;
      entry.first_vertex = uint32_t(frame.vertices.size());
      entry.texture_id = texture_id;
      entry.scissor = true;
      const int32_t left = int32_t((command.ClipRect.x - origin.x) * sx);
      const int32_t top = int32_t((command.ClipRect.y - origin.y) * sy);
      entry.scissor_x = left;
      entry.scissor_y = top;
      entry.scissor_w = int32_t((command.ClipRect.z - origin.x) * sx) - left;
      entry.scissor_h = int32_t((command.ClipRect.w - origin.y) * sy) - top;
      if (entry.scissor_w <= 0 || entry.scissor_h <= 0) continue;

      // The overlay pipeline is non-indexed, so the index buffer is expanded here. ImGui's per-frame index counts are in the thousands, which costs nothing next to adding a second pipeline for the sake of an index buffer.
      frame.vertices.reserve(frame.vertices.size() + command.ElemCount);
      for (unsigned int i = 0; i < command.ElemCount; ++i) {
        const int index_at = int(command.IdxOffset + i);
        if (index_at >= list->IdxBuffer.Size) break;
        const int vertex_at = int(command.VtxOffset) + int(list->IdxBuffer[index_at]);
        if (vertex_at >= list->VtxBuffer.Size) break;
        const ImDrawVert& src = list->VtxBuffer[vertex_at];

        UiVertex out;
        // Pixels -> clip space, with Y flipped: the overlay vertex shader passes position straight through to SV_Position.
        out.x = ((src.pos.x - origin.x) / space_width) * 2.0f - 1.0f;
        out.y = 1.0f - ((src.pos.y - origin.y) / space_height) * 2.0f;
        out.u = src.uv.x;
        out.v = src.uv.y;
        // ImGui packs colour as ABGR little-endian (R in the low byte).
        out.r = float((src.col >> 0) & 0xFF) / 255.0f;
        out.g = float((src.col >> 8) & 0xFF) / 255.0f;
        out.b = float((src.col >> 16) & 0xFF) / 255.0f;
        out.a = float((src.col >> 24) & 0xFF) / 255.0f;
        frame.vertices.push_back(out);
      }

      entry.vertex_count = uint32_t(frame.vertices.size()) - entry.first_vertex;
      if (entry.vertex_count) frame.draws.push_back(entry);
    }
  }
  graphics_->SubmitUi(std::move(frame));
}

}  // namespace kameo::gpu
