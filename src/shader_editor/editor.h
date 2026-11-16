// The shader editor's window: a palette and details on the left, the graph in the middle, what the mod applies to and
// the compile log on the right. The running game is the preview (docs/modding.md).
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "shader_editor/game_textures.h"
#include "shader_editor/node_canvas.h"
#include "shader_editor/shader_build.h"
#include "shader_editor/shader_graph.h"

namespace kameo::shader_editor {

// What differs between the programs that run the editor: the standalone one has Direct3D 11 thumbnails and a model
// preview; the game draws thumbnails as native-renderer images and has no preview (the game is the preview).
class EditorHost {
 public:
  virtual ~EditorHost() = default;
  // A texture of the given RGBA pixels, to draw with ImGui; 0 when it can't be made. Called from the drawing thread.
  virtual ImTextureID CreateTexture(uint32_t width, uint32_t height, const uint8_t* rgba) = 0;
  virtual void FreeTexture(ImTextureID texture) = 0;
  // The preview panel, when the host has one; when not, the editor leaves its place to the other panels.
  virtual bool HasPreview() const { return false; }
  // The template the mod's code goes with (shader_mod.hlsli), once the shader source is ready.
  virtual void SetPreviewTemplate(std::string /*text*/) {}
  // Fills the current ImGui child window with the preview of `shade` (the generated Shade function), with the textures
  // (as CreateTexture returned them, 0 for none) the mod's Game texture nodes use.
  virtual void DrawPreview(const std::string& /*shade*/, const std::array<ImTextureID, kGameTextureSlots>& /*textures*/) {}
};

class Editor {
 public:
  // `host` must outlive the editor.
  explicit Editor(EditorHost& host);
  void Draw();
  // The editor's work when its window is not drawn (hidden in the game): a build under way finishes, and an edit still waiting for its live update gets it.
  void Background();

 private:
  // The graph's past states, with the text that tells them apart.
  struct History {
    std::vector<ShaderGraph> states;
    std::vector<std::string> keys;
    size_t at = 0;
  };

  void DrawToolbar();
  void DrawPalette();
  void DrawDetails();
  void DrawParameters();
  void DrawTargets();
  void DrawLog();
  // The window a Game texture node's texture is chosen in, while picking_node_ names the node.
  void DrawTexturePicker();
  void TargetBox(const Target& target);
  bool Targeted(const std::string& key) const;

  void New();
  void Open(const std::filesystem::path& path);
  void Replaced();
  void Changed();
  void Remember();
  void Step(int direction);
  // `full` builds for both renderers and saves the graph; otherwise only for the renderer the game is on, as a quick look.
  void Build(bool full);
  // Takes this mod's shaders out of the game, which goes back to its own look. The saved graph stays.
  void RemoveFromGame();
  // Starts the live build an edit is waiting for, once edits have stopped for a moment.
  void LiveUpdate(bool mouse_held);
  void Finish();

  ShaderSource source_;
  std::future<std::string> preparing_;
  bool ready_ = false;
  std::string name_ = "My Shader";
  ShaderMod mod_;
  NodeCanvas canvas_;
  EditorHost& host_;
  GameTextureLibrary game_textures_;
  int picking_node_ = 0;
  std::string picker_search_;
  std::string picker_archive_;
  std::string picker_type_;
  History history_;
  std::vector<std::filesystem::path> saved_;
  std::string palette_search_;
  int pending_preset_ = -1;
  std::string last_source_;
  // Live update applies the shaders once edits have stopped for a moment.
  bool live_ = true;
  bool live_pending_ = false;
  std::chrono::steady_clock::time_point edited_at_;
  std::string log_;
  bool log_ok_ = true;
  std::future<std::pair<bool, std::string>> build_;
  std::atomic<size_t> build_done_ = 0;
  size_t build_total_ = 0;
  std::chrono::steady_clock::time_point build_started_;
};

}  // namespace kameo::shader_editor
