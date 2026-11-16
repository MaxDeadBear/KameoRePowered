// An RGBA image drawn through ImGui (ImGui::Image, ImDrawList::AddImage) by the native renderer.
#pragma once

#include <cstdint>
#include <memory>

#include <imgui.h>

namespace kameo::gpu {

class UiImage {
 public:
  // Null while the native renderer is not running. The pixels are copied, and uploaded at the next present.
  // `repeated` wraps coordinates outside 0 to 1 around the image, for patterns drawn scrolling.
  static std::unique_ptr<UiImage> Create(uint32_t width, uint32_t height, const uint8_t* rgba, bool repeated = false);
  ~UiImage();
  UiImage(const UiImage&) = delete;
  UiImage& operator=(const UiImage&) = delete;

  ImTextureID texture_id() const { return ImTextureID(id_); }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }

 private:
  UiImage(uint32_t id, uint32_t width, uint32_t height) : id_(id), width_(width), height_(height) {}

  uint32_t id_ = 0;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

}  // namespace kameo::gpu
