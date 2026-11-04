#pragma once

#include <optional>
#include <string>
#include <vector>

#include <imgui.h>

#include "shader_editor/shader_graph.h"

namespace kameo::shader_editor {

// The shader editor's graph canvas, modelled on UE's material editor (docs/modding.md): zoom about the cursor, pan, box and multiple selection, wires dragged from either end, a search menu on right-click or a wire dropped on nothing, hotkey clicks, reroutes, comments, copy and paste, and a minimap.
class NodeCanvas {
 public:
  // Draws `graph` over the rest of the current window and edits it; true when it changed.
  bool Draw(ShaderGraph& graph);
  // Forgets the selection and frames the whole graph on the next draw, for a graph just loaded.
  void Reset();
  // Adds a node of `kind` at the middle of the view, selected.
  void AddAtCentre(ShaderGraph& graph, NodeKind kind);
  void FrameAll() { frame_ = Frame::kAll; }
  // Selects one node and brings it into view, for the Parameters panel.
  void Focus(int id);

  const std::vector<int>& selected() const { return selected_; }
  int selected_comment() const { return selected_comment_; }

  // Whether a screen point is on the canvas, and where it is in the graph.
  bool Contains(ImVec2 screen) const;
  ImVec2 ToGraph(ImVec2 screen) const;

 private:
  enum class Drag { kNone, kNodes, kBox, kLink, kPan, kComment, kResize, kMinimap };
  enum class Frame { kNone, kAll, kSelection };
  struct Pending {
    int node = 0;
    // An output dragged from, or an input dragged from when `input` >= 0.
    int input = -1;
  };

  ImVec2 ToScreen(float x, float y) const;
  bool Selected(int id) const;
  void Select(int id, bool add);
  void Paste(ShaderGraph& graph, const ShaderGraph& clip, ImVec2 at);
  void Copy(const ShaderGraph& graph) const;
  void FrameBounds(float x0, float y0, float x1, float y1);
  Node& Place(ShaderGraph& graph, NodeKind kind, ImVec2 at);
  bool DrawMenu(ShaderGraph& graph);

  ImVec2 origin_{0, 0};
  ImVec2 size_{0, 0};
  ImVec2 last_size_{0, 0};
  bool fitted_ = false;
  ImVec2 pan_{40, 40};
  float zoom_ = 1.0f;
  Frame frame_ = Frame::kAll;
  std::vector<int> selected_;
  int selected_comment_ = 0;
  Drag drag_ = Drag::kNone;
  ImVec2 drag_from_{0, 0};
  // The nodes inside a comment being dragged, which move with it.
  std::vector<int> carried_;
  bool right_moved_ = false;
  Pending link_;
  // Where the search menu opened, in graph space, and the wire it will connect.
  ImVec2 menu_at_{0, 0};
  std::optional<Pending> menu_link_;
  bool open_menu_ = false;
  std::string search_;
  bool focus_search_ = false;
};

}  // namespace kameo::shader_editor
