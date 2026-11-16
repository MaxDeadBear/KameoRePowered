#include "ui/hud/shader_editor_overlay.h"

#ifdef _WIN32

#include <mutex>
#include <unordered_map>

#include "core/localization.h"
#include "gpu/ui/ui_image.h"
#include "shader_editor/editor.h"

namespace {

// The editor's host in the game: thumbnails are native-renderer images, kept here until the editor frees them. There is
// no preview panel.
class GameEditorHost final : public kameo::shader_editor::EditorHost {
 public:
  ImTextureID CreateTexture(uint32_t width, uint32_t height, const uint8_t* rgba) override {
    std::unique_ptr<kameo::gpu::UiImage> image = kameo::gpu::UiImage::Create(width, height, rgba);
    if (!image) return 0;
    const ImTextureID id = image->texture_id();
    std::lock_guard lock(mutex_);
    images_[id] = std::move(image);
    return id;
  }
  void FreeTexture(ImTextureID texture) override {
    std::lock_guard lock(mutex_);
    images_.erase(texture);
  }

 private:
  std::mutex mutex_;
  std::unordered_map<ImTextureID, std::unique_ptr<kameo::gpu::UiImage>> images_;
};

}  // namespace

struct KameoShaderEditorOverlay::State {
  GameEditorHost host;
  kameo::shader_editor::Editor editor{host};
};

KameoShaderEditorOverlay::KameoShaderEditorOverlay() : state_(std::make_unique<State>()) {}

KameoShaderEditorOverlay::~KameoShaderEditorOverlay() = default;

void KameoShaderEditorOverlay::OnDraw(ImGuiIO& io) {
  if (!visible_) {
    state_->editor.Background();
    return;
  }
  const float margin = 24.0f;
  ImGui::SetNextWindowPos(ImVec2(margin, margin), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x - 2 * margin, io.DisplaySize.y - 2 * margin), ImGuiCond_Always);
  // The ### id keeps the window's place the same whatever language titles it.
  const std::string title = std::string(kameo::l10n::Tr("Shader editor (F1)")) + "###kameo_shader_editor";
  bool open = true;
  if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) state_->editor.Draw();
  ImGui::End();
  if (!open) visible_ = false;
}

#endif
