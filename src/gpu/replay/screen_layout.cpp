#include "gpu/replay/screen_layout.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include <plume_render_interface.h>

#include "gpu/config.h"
#include "gpu/draw/draw_constants.h"
#include "gpu/frame_ring.h"
#include "gpu/graphics_system.h"
#include "gpu/guest_state.h"

namespace kameo::gpu::detail {
namespace {

std::atomic<bool> g_hudMenuOpen{false};

}  // namespace

// Widescreen already widened the projection to the display's aspect, so the guest frame covers the whole window; letterboxing too would squeeze the extra view back into 16:9.
bool LetterboxScene() {
  return REXCVAR_GET(kameo_gfx_preserve_aspect) && !REXCVAR_GET(kameo_gfx_widescreen);
}

GuestToHost GuestToHostTransform(uint32_t host_w, uint32_t host_h) {
  GuestToHost t;
  t.scale_x = float(host_w) / float(kGuestWidth);
  t.scale_y = float(host_h) / float(kGuestHeight);
  if (LetterboxScene()) {
    const float uniform = std::min(t.scale_x, t.scale_y);
    t.origin_x = (float(host_w) - float(kGuestWidth) * uniform) * 0.5f;
    t.origin_y = (float(host_h) - float(kGuestHeight) * uniform) * 0.5f;
    t.scale_x = uniform;
    t.scale_y = uniform;
  }
  return t;
}

bool HudDrawBox(const DrawCall& call, const uint8_t* vertices, const uint32_t* indices, const float* vs_constants, float* box) {
  if (!call.hud_2d || !call.layout || !vertices || !vs_constants || call.vs_constant_bytes < 64) return false;
  if (call.depth_control & (state::kDepthTestEnableBit | state::kDepthWriteEnableBit)) return false;
  const float* c = vs_constants;
  if (std::abs(c[12]) > 1e-4f || std::abs(c[13]) > 1e-4f || std::abs(c[14]) > 1e-4f || std::abs(c[15] - 1.0f) > 1e-4f) return false;
  const HostElement* position = nullptr;
  for (const HostElement& e : call.layout->elements) {
    if (e.usage == 0 && e.usage_index == 0) position = &e;
  }
  if (!position || position->dst_size < 8) return false;
  const bool is_float = position->op == kShortToFloat || position->op == kUShortToFloat || position->op == kByteToFloat || position->op == kDec3ToFloat || position->format == plume::RenderFormat::R32G32_FLOAT || position->format == plume::RenderFormat::R32G32B32_FLOAT || position->format == plume::RenderFormat::R32G32B32A32_FLOAT;
  if (!is_float || call.layout->dst_stride == 0) return false;
  const uint32_t stride = call.layout->dst_stride;
  const bool has_z = position->dst_size >= 12;
  float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
  auto add = [&](uint32_t v) {
    const size_t offset = size_t(v) * stride + position->dst_offset;
    if (offset + position->dst_size > call.vertex_bytes) return;
    float p[3] = {0.0f, 0.0f, 0.0f};
    std::memcpy(p, vertices + offset, has_z ? 12 : 8);
    const float x = c[0] * p[0] + c[1] * p[1] + c[2] * p[2] + c[3];
    const float y = c[4] * p[0] + c[5] * p[1] + c[6] * p[2] + c[7];
    x0 = std::min(x0, x);
    x1 = std::max(x1, x);
    y0 = std::min(y0, y);
    y1 = std::max(y1, y);
  };
  constexpr uint32_t kMaxSamples = 16384;
  if (indices && call.index_count) {
    for (uint32_t i = 0; i < std::min(call.index_count, kMaxSamples); ++i) add(indices[i]);
  } else {
    for (uint32_t v = 0; v < std::min(call.vertex_count, kMaxSamples); ++v) add(v);
  }
  if (x0 > x1 || y0 > y1) return false;
  box[0] = x0;
  box[1] = x1;
  box[2] = y0;
  box[3] = y1;
  return true;
}

bool PlaceHudBox(const float* box, float stretch, HudPlacement* out) {
  const int32_t layout = std::clamp(REXCVAR_GET(kameo_hud_layout), 0, 2);
  const bool menu = g_hudMenuOpen.load(std::memory_order_relaxed);
  const float scale = menu ? 1.0f : std::clamp(REXCVAR_GET(kameo_hud_scale), 50, 150) / 100.0f;
  const float unstretch = layout == 0 ? 1.0f : 1.0f / std::max(stretch, 1.0f);
  if (scale == 1.0f && unstretch == 1.0f) return false;
  if (box[1] - box[0] > 1.5f || box[3] - box[2] > 1.5f) return false;
  const float cx = (box[0] + box[1]) * 0.5f;
  const float cy = (box[2] + box[3]) * 0.5f;
  const float ax = menu ? 0.0f : cx < -1.0f / 3.0f ? -1.0f : cx > 1.0f / 3.0f ? 1.0f : 0.0f;
  // Anything beside the centre grows from the corner of its half, so elements stacked down one side keep their spacing; only the true centre scales about the middle.
  const float ay = menu || (ax == 0.0f && std::abs(cy) < 1.0f / 3.0f) ? 0.0f : cy < 0.0f ? -1.0f : 1.0f;
  const float target_ax = layout == 1 ? ax * unstretch : ax;
  out->mx = scale * unstretch;
  out->bx = target_ax - ax * out->mx;
  out->my = scale;
  out->by = ay - ay * scale;
  return true;
}

void GuestRectToHostUv(uint32_t host_w, uint32_t host_h, float gx, float gy, float gw, float gh, float* out) {
  const GuestToHost t = GuestToHostTransform(host_w, host_h);
  out[0] = (t.origin_x + gx * t.scale_x) / float(host_w);
  out[1] = (t.origin_y + gy * t.scale_y) / float(host_h);
  out[2] = (gw * t.scale_x) / float(host_w);
  out[3] = (gh * t.scale_y) / float(host_h);
}

void ClusterHudBoxes(const DrawFrame& frame, std::vector<std::array<float, 4>>& hud_boxes, std::vector<uint8_t>& hud_boxed) {
  if (REXCVAR_GET(kameo_hud_scale) != 100 || REXCVAR_GET(kameo_hud_layout) != 0) {
    hud_boxes.resize(frame.draws.size());
    hud_boxed.assign(frame.draws.size(), 0);
    std::vector<uint32_t> boxed;
    for (size_t i = 0; i < frame.draws.size(); ++i) {
      const DrawCall& c = frame.draws[i];
      if (c.kind != CommandKind::Draw || !c.hud_2d || c.vs_constants == kNoData) continue;
      std::array<float, 4>& box = hud_boxes[i];
      if (!HudDrawBox(c, VertexBytes(frame, c), reinterpret_cast<const uint32_t*>(IndexBytes(frame, c)), reinterpret_cast<const float*>(frame.arena.data() + c.vs_constants), box.data())) continue;
      if (box[1] - box[0] > 1.5f || box[3] - box[2] > 1.5f) continue;
      hud_boxed[i] = 1;
      boxed.push_back(uint32_t(i));
    }
    constexpr size_t kMaxClustered = 2048;
    std::vector<uint32_t> parent(boxed.size());
    for (uint32_t k = 0; k < parent.size(); ++k) parent[k] = k;
    auto find = [&parent](uint32_t k) {
      while (parent[k] != k) k = parent[k] = parent[parent[k]];
      return k;
    };
    std::unordered_map<uint32_t, uint32_t> group_first;
    for (uint32_t k = 0; k < boxed.size(); ++k) {
      const DrawCall& c = frame.draws[boxed[k]];
      if (!c.hud_group) continue;
      auto [it, fresh] = group_first.try_emplace(c.hud_group, k);
      if (!fresh) parent[find(k)] = find(it->second);
    }
    if (boxed.size() <= kMaxClustered) {
      for (uint32_t a = 0; a < boxed.size(); ++a) {
        const DrawCall& ca = frame.draws[boxed[a]];
        const std::array<float, 4>& ba = hud_boxes[boxed[a]];
        for (uint32_t b = a + 1; b < boxed.size(); ++b) {
          const DrawCall& cb = frame.draws[boxed[b]];
          const std::array<float, 4>& bb = hud_boxes[boxed[b]];
          if (ca.rt_surface != cb.rt_surface || std::memcmp(ca.viewport, cb.viewport, sizeof(float) * 4) != 0) continue;
          if (ba[0] >= bb[1] || bb[0] >= ba[1] || ba[2] >= bb[3] || bb[2] >= ba[3]) continue;
          parent[find(a)] = find(b);
        }
      }
    }
    std::unordered_map<uint32_t, std::array<float, 4>> cluster_boxes;
    for (uint32_t k = 0; k < boxed.size(); ++k) {
      const std::array<float, 4>& box = hud_boxes[boxed[k]];
      auto [it, fresh] = cluster_boxes.try_emplace(find(k), box);
      if (!fresh) it->second = {std::min(it->second[0], box[0]), std::max(it->second[1], box[1]), std::min(it->second[2], box[2]), std::max(it->second[3], box[3])};
    }
    for (uint32_t k = 0; k < boxed.size(); ++k) {
      const std::array<float, 4>& cluster = cluster_boxes[find(k)];
      if (cluster[1] - cluster[0] <= 1.5f && cluster[3] - cluster[2] <= 1.5f) hud_boxes[boxed[k]] = cluster;
    }
  }
}

}  // namespace kameo::gpu::detail

namespace kameo::gpu {
using namespace detail;

void SetHudMenuOpen(bool open) { g_hudMenuOpen.store(open, std::memory_order_relaxed); }

bool WindowPointToUiLayout(float window_x, float window_y, uint32_t window_width, uint32_t window_height, float* layout_x, float* layout_y) {
  if (window_width == 0 || window_height == 0) return false;
  const GuestToHost t = GuestToHostTransform(window_width, window_height);
  *layout_x = (window_x - t.origin_x) / t.scale_x * (640.0f / float(kGuestWidth));
  *layout_y = (window_y - t.origin_y) / t.scale_y * (480.0f / float(kGuestHeight));
  return true;
}

}  // namespace kameo::gpu
