// The achievements' art as ImGui images, made once each and kept: the PNGs come out of the title's XDBF.
#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

#include "gpu/ui/ui_image.h"

class KameoAchievementIconSet {
 public:
  // The icon of an achievement by its id; null until the native renderer and the title's achievement data exist, or when the title has no art for it.
  kameo::gpu::UiImage* Get(uint32_t achievement_id);

 private:
  std::unordered_map<uint32_t, uint32_t> image_ids_;
  std::unordered_map<uint32_t, std::unique_ptr<kameo::gpu::UiImage>> icons_;
};
