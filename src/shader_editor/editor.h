// The shader editor's window: a palette and details on the left, the graph in the middle, what the mod applies to and
// the compile log on the right. The running game is the preview (docs/modding.md).
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "shader_editor/node_canvas.h"
#include "shader_editor/preview.h"
#include "shader_editor/shader_build.h"
#include "shader_editor/shader_graph.h"

namespace kameo::shader_editor {

class Editor {
 public:
  Editor();
  void Draw(ID3D11Device* device, ID3D11DeviceContext* context);

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
  void Finish();

  ShaderSource source_;
  std::future<std::string> preparing_;
  bool ready_ = false;
  std::string name_ = "My Shader";
  ShaderMod mod_;
  NodeCanvas canvas_;
  Preview preview_;
  bool preview_attached_ = false;
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
