// The open storybook's page meshes as captured, the fallback for pointing at page text before the GPU pick answers.

#include <cmath>
#include <iterator>

#include <rex/cvar.h>

#include "gpu/config.h"
#include "gpu/graphics_system.h"
#include "ui/menu_pointer/menu_ui_state.h"

namespace {

struct BookVertex {
  float x;
  float y;
  float inv_w;
  float u_w;
  float v_w;
};

// Three vertices per triangle.
const BookVertex kBookPageTriangles[] = {
#include "book/book_page_triangles.inc"
};

}  // namespace

// Maps a layout point onto the page texture through the open book's page meshes, nearest page first; widescreen widens the projection by the display aspect, which the 16:9 capture is scaled back to.
bool kameo::ui::BookPagePoint(uint8_t* base, float layout_x, float layout_y, float* page_x, float* page_y) {
  float x = layout_x;
  if (REXCVAR_GET(kameo_gfx_widescreen)) {
    if (auto* gfx = kameo::gpu::GraphicsSystem::instance()) x = 320.0f + (layout_x - 320.0f) * float(gfx->DisplayAspect() / (16.0 / 9.0));
  }
  float best_depth = 0.0f;
  bool found = false;
  for (size_t i = 0; i + 2 < std::size(kBookPageTriangles); i += 3) {
    const BookVertex& a = kBookPageTriangles[i];
    const BookVertex& b = kBookPageTriangles[i + 1];
    const BookVertex& c = kBookPageTriangles[i + 2];
    const float denominator = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
    if (std::abs(denominator) < 1e-6f) continue;
    const float l1 = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (layout_y - c.y)) / denominator;
    const float l2 = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (layout_y - c.y)) / denominator;
    const float l3 = 1.0f - l1 - l2;
    if (l1 < -1e-4f || l2 < -1e-4f || l3 < -1e-4f) continue;
    const float inv_w = l1 * a.inv_w + l2 * b.inv_w + l3 * c.inv_w;
    if (inv_w <= 0.0f) continue;
    const float depth = 1.0f / inv_w;
    if (found && depth >= best_depth) continue;
    found = true;
    best_depth = depth;
    *page_x = (l1 * a.u_w + l2 * b.u_w + l3 * c.u_w) / inv_w;
    *page_y = (l1 * a.v_w + l2 * b.v_w + l3 * c.v_w) / inv_w;
  }
  return found;
}
