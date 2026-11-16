#include "ui/hud/achievement_icon_set.h"

#include <vector>

#include <rex/filesystem/xdbf.h>
#include <rex/kernel/title_runtime.h>
#include <rex/title.h>
#include <rex/xam/achievement_manager.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb/stb_image.h"

namespace {

std::unique_ptr<kameo::gpu::UiImage> LoadXdbfIcon(rex::kernel::TitleRuntime* runtime, uint32_t image_id) {
  const auto db = runtime->GetTitleXDBF();
  if (!db || !db->IsValid()) return nullptr;
  const rex::filesystem::XDBFBlock block = db->GetEntry(rex::filesystem::XDBFSection::kImage, uint64_t(image_id));
  if (!block) return nullptr;
  int width = 0, height = 0, components = 0;
  stbi_uc* pixels = stbi_load_from_memory(block.buffer, int(block.size), &width, &height, &components, 4);
  if (!pixels) return nullptr;
  auto image = kameo::gpu::UiImage::Create(uint32_t(width), uint32_t(height), pixels);
  stbi_image_free(pixels);
  return image;
}

}  // namespace

kameo::gpu::UiImage* KameoAchievementIconSet::Get(uint32_t achievement_id) {
  if (const auto found = icons_.find(achievement_id); found != icons_.end()) return found->second.get();
  rex::kernel::TitleRuntime* runtime = REX_RUNTIME();
  if (!runtime) return nullptr;
  if (image_ids_.empty()) {
    for (const rex::Achievement& a : runtime->GetAchievementManager().ListAchievements()) image_ids_[a.id] = a.image_id;
    if (image_ids_.empty()) return nullptr;
  }
  const auto id = image_ids_.find(achievement_id);
  if (id == image_ids_.end() || id->second == 0) return (icons_[achievement_id] = nullptr).get();
  std::unique_ptr<kameo::gpu::UiImage> image = LoadXdbfIcon(runtime, id->second);
  // The renderer not running yet is not a verdict on the art, so it is tried again at the next draw.
  if (!image) return nullptr;
  return (icons_[achievement_id] = std::move(image)).get();
}
