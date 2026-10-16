#include "gpu/ui/imgui_drawer.h"

#include <algorithm>

namespace kameo::gpu {

namespace {

// The texture handle ImGui hands back in ImmediateDraw::texture. It only has to
// carry the id the graphics system registered.
class PlumeImmediateTexture final : public rex::ui::ImmediateTexture {
 public:
  PlumeImmediateTexture(uint32_t width, uint32_t height, uint32_t id)
      : rex::ui::ImmediateTexture(width, height), id(id) {}
  uint32_t id = 0;
};

}  // namespace

std::unique_ptr<rex::ui::ImmediateTexture> PlumeImmediateDrawer::CreateTexture(
    uint32_t width, uint32_t height, rex::ui::ImmediateTextureFilter filter, bool is_repeated,
    const uint8_t* data) {
  (void)filter;
  (void)is_repeated;
  const uint32_t id = graphics_ ? graphics_->CreateUiTexture(width, height, data) : 0;
  // The font atlas is uploaded lazily on the first Draw, which can precede the
  // guest creating the device; nullptr makes that a retry rather than a texture
  // permanently registered as undrawable.
  if (id == 0) {
    return nullptr;
  }
  return std::make_unique<PlumeImmediateTexture>(width, height, id);
}

void PlumeImmediateDrawer::Begin(rex::ui::UIDrawContext& ui_draw_context,
                                 float coordinate_space_width, float coordinate_space_height) {
  ImmediateDrawer::Begin(ui_draw_context, coordinate_space_width, coordinate_space_height);
  target_width_ = ui_draw_context.render_target_width();
  target_height_ = ui_draw_context.render_target_height();
  space_width_ = this->coordinate_space_width() > 0.0f ? this->coordinate_space_width()
                                                       : float(std::max(target_width_, 1u));
  space_height_ = this->coordinate_space_height() > 0.0f ? this->coordinate_space_height()
                                                         : float(std::max(target_height_, 1u));
  frame_ = GraphicsSystem::UiFrame();
}

void PlumeImmediateDrawer::BeginDrawBatch(const rex::ui::ImmediateDrawBatch& batch) {
  batch_vertices_.assign(batch.vertices, batch.vertices + std::max(batch.vertex_count, 0));
  batch_indices_.assign(batch.indices, batch.indices + std::max(batch.index_count, 0));
}

void PlumeImmediateDrawer::Draw(const rex::ui::ImmediateDraw& draw) {
  // Lines would need their own topology; ImGui only ever emits triangles.
  if (draw.primitive_type != rex::ui::ImmediatePrimitiveType::kTriangles || draw.count <= 0) {
    return;
  }
  const auto* texture = static_cast<const PlumeImmediateTexture*>(draw.texture);
  if (!texture || texture->id == 0) {
    return;
  }

  GraphicsSystem::UiDraw entry;
  entry.first_vertex = uint32_t(frame_.vertices.size());
  entry.texture_id = texture->id;
  entry.scissor = draw.scissor;
  if (draw.scissor) {
    // Coordinate space -> render target pixels, which is what setScissors takes.
    const float sx = float(target_width_) / space_width_;
    const float sy = float(target_height_) / space_height_;
    const int32_t left = int32_t(draw.scissor_left * sx);
    const int32_t top = int32_t(draw.scissor_top * sy);
    entry.scissor_x = left;
    entry.scissor_y = top;
    entry.scissor_w = int32_t(draw.scissor_right * sx) - left;
    entry.scissor_h = int32_t(draw.scissor_bottom * sy) - top;
    if (entry.scissor_w <= 0 || entry.scissor_h <= 0) {
      return;  // the interface defines an inverted rect as "do not draw"
    }
  }

  // The overlay pipeline is non-indexed, so the index buffer is expanded here.
  // ImGui's per-frame index counts are in the thousands, which costs nothing
  // next to adding a second pipeline for the sake of an index buffer.
  frame_.vertices.reserve(frame_.vertices.size() + size_t(draw.count));
  for (int i = 0; i < draw.count; ++i) {
    size_t vertex_index;
    if (!batch_indices_.empty()) {
      const size_t at = size_t(draw.index_offset) + size_t(i);
      if (at >= batch_indices_.size()) break;
      vertex_index = size_t(draw.base_vertex) + batch_indices_[at];
    } else {
      vertex_index = size_t(draw.base_vertex) + size_t(i);
    }
    if (vertex_index >= batch_vertices_.size()) break;
    const rex::ui::ImmediateVertex& src = batch_vertices_[vertex_index];

    UiVertex out;
    // Pixels -> clip space, with Y flipped: the overlay vertex shader passes
    // position straight through to SV_Position.
    out.x = (src.x / space_width_) * 2.0f - 1.0f;
    out.y = 1.0f - (src.y / space_height_) * 2.0f;
    out.u = src.u;
    out.v = src.v;
    // ImGui packs colour as ABGR little-endian (R in the low byte).
    out.r = float((src.color >> 0) & 0xFF) / 255.0f;
    out.g = float((src.color >> 8) & 0xFF) / 255.0f;
    out.b = float((src.color >> 16) & 0xFF) / 255.0f;
    out.a = float((src.color >> 24) & 0xFF) / 255.0f;
    frame_.vertices.push_back(out);
  }

  entry.vertex_count = uint32_t(frame_.vertices.size()) - entry.first_vertex;
  if (entry.vertex_count) {
    frame_.draws.push_back(entry);
  }
}

void PlumeImmediateDrawer::EndDrawBatch() {
  batch_vertices_.clear();
  batch_indices_.clear();
}

void PlumeImmediateDrawer::End() {
  if (graphics_) {
    graphics_->SubmitUi(std::move(frame_));
  }
  frame_ = GraphicsSystem::UiFrame();
  ImmediateDrawer::End();
}

}  // namespace kameo::gpu
