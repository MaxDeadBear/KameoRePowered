#pragma once

#include <cstdint>

namespace kameo::gpu::detail {

// The guest's own render size: XGetVideoMode reports HiDef 720p and the game takes its kamVideoParams = 1280 branch, so every guest viewport is relative to this.
inline constexpr uint32_t kGuestWidth = 1280;
inline constexpr uint32_t kGuestHeight = 720;

}  // namespace kameo::gpu::detail
