#include "shader_editor/node_canvas.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <map>
#include <string_view>

#include "core/localization.h"

namespace kameo::shader_editor {

using l10n::Tr;
using l10n::TrFormat;

namespace {

// Node layout in graph units, which zoom 1 draws as pixels.
constexpr float kTitle = 26, kRow = 22, kPad = 9, kPin = 5.5f, kText = 15, kMinWidth = 150;
constexpr float kMinZoom = 0.15f, kMaxZoom = 2.5f;
constexpr ImU32 kBackground = IM_COL32(34, 32, 36, 255), kGridMinor = IM_COL32(46, 44, 50, 255), kGridMajor = IM_COL32(60, 57, 64, 255);
constexpr ImU32 kBody = IM_COL32(24, 23, 26, 236), kBorder = IM_COL32(12, 11, 13, 255), kSelected = IM_COL32(255, 160, 50, 255);
constexpr ImU32 kLabel = IM_COL32(220, 214, 222, 255), kFaint = IM_COL32(150, 144, 152, 255), kWire = IM_COL32(230, 226, 232, 255);

struct Layout {
  float x, y, w, h;
  int rows;
};

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string Number(float v) {
  std::string s = std::format("{:.3g}", v);
  return s;
}

std::string ValueText(const Float3& v) {
  if (v[0] == v[1] && v[1] == v[2]) return Number(v[0]);
  return std::format("{}, {}, {}", Number(v[0]), Number(v[1]), Number(v[2]));
}

// What a node's title shows: its parameter name or its value for constants, else its kind.
std::string Title(const Node& n) {
  if (n.kind == NodeKind::kFloat) return n.name.empty() ? Number(n.settings[0]) : std::format("{} ({})", n.name, Number(n.settings[0]));
  if (n.kind == NodeKind::kColor && !n.name.empty()) return n.name;
  if (n.kind == NodeKind::kReroute) return "";
  return Tr(Info(n.kind).name);
}

// The lines under the pins: settings and their values, which the Details panel edits.
std::vector<std::string> Summary(const Node& n) {
  std::vector<std::string> lines;
  const NodeInfo& info = Info(n.kind);
  if (n.kind == NodeKind::kFloat || n.kind == NodeKind::kColor) return lines;
  if (n.kind == NodeKind::kComponentMask) return {TrFormat("Channel {0}", {std::string(1, "RGB"[std::clamp(int(n.settings[0]), 0, 2)])})};
  if (n.kind == NodeKind::kCustom) return {Tr("HLSL (Details panel)")};
  for (size_t i = 0; i < info.settings.size() && i < n.settings.size(); ++i) lines.push_back(std::format("{}  {}", Tr(info.settings[i]), Number(n.settings[i])));
  return lines;
}

float TextWidth(float size, const std::string& text) {
  return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
}

Layout Measure(const Node& n) {
  const NodeInfo& info = Info(n.kind);
  if (n.kind == NodeKind::kReroute) return {n.x, n.y, 24, 18, 0};
  float w = std::max(kMinWidth, TextWidth(kText, Title(n)) + 40);
  for (size_t i = 0; i < info.inputs.size(); ++i) {
    const std::string value = n.inputs.size() > i ? ValueText(n.inputs[i]) : std::string();
    w = std::max(w, TextWidth(kText, Tr(info.inputs[i])) + TextWidth(kText - 2, value) + 70);
  }
  const std::vector<std::string> summary = Summary(n);
  for (const std::string& s : summary) w = std::max(w, TextWidth(kText - 2, s) + 30);
  int rows = int(info.inputs.size() + summary.size()) + (n.kind == NodeKind::kColor ? 1 : 0);
  rows = std::max(rows, 1);
  return {n.x, n.y, std::min(w, 340.0f), kTitle + kPad + rows * kRow + kPad * 0.5f, rows};
}

ImVec2 InputPin(const Layout& l, int input) {
  if (l.rows == 0) return ImVec2(l.x, l.y + l.h * 0.5f);
  return ImVec2(l.x, l.y + kTitle + kPad + input * kRow + kRow * 0.5f);
}

ImVec2 OutputPin(const Layout& l) {
  if (l.rows == 0) return ImVec2(l.x + l.w, l.y + l.h * 0.5f);
  return ImVec2(l.x + l.w, l.y + kTitle + kPad + kRow * 0.5f);
}

ImU32 Lighter(ImU32 c, float f) {
  const int r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
  const auto up = [&](int v) { return std::min(255, int(v + (255 - v) * f)); };
  return IM_COL32(up(r), up(g), up(b), 255);
}

ImU32 ColourOf(const Float3& c, float alpha) {
  const auto to = [](float v) { return int(std::clamp(std::pow(std::clamp(v, 0.0f, 1.0f), 1 / 2.2f), 0.0f, 1.0f) * 255); };
  return IM_COL32(to(c[0]), to(c[1]), to(c[2]), int(alpha * 255));
}

float BezierDistance(ImVec2 a, ImVec2 b, float bend, ImVec2 p) {
  float best = 1e9f;
  ImVec2 prev = a;
  for (int i = 1; i <= 24; ++i) {
    const float t = i / 24.0f, u = 1 - t;
    const ImVec2 c1(a.x + bend, a.y), c2(b.x - bend, b.y);
    const ImVec2 q(u * u * u * a.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * b.x, u * u * u * a.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * b.y);
    const ImVec2 d(q.x - prev.x, q.y - prev.y);
    const float len2 = d.x * d.x + d.y * d.y;
    const float s = len2 > 0 ? std::clamp(((p.x - prev.x) * d.x + (p.y - prev.y) * d.y) / len2, 0.0f, 1.0f) : 0.0f;
    const float dx = prev.x + d.x * s - p.x, dy = prev.y + d.y * s - p.y;
    best = std::min(best, std::sqrt(dx * dx + dy * dy));
    prev = q;
  }
  return best;
}

// The keys held while clicking the canvas that place a node, as in UE.
std::optional<NodeKind> HeldHotkey() {
  for (int k = 0; k < int(NodeKind::kCount); ++k) {
    const char key = Info(NodeKind(k)).hotkey;
    if (!key) continue;
    const ImGuiKey imkey = key >= '0' && key <= '9' ? ImGuiKey(ImGuiKey_0 + (key - '0')) : ImGuiKey(ImGuiKey_A + (key - 'A'));
    if (ImGui::IsKeyDown(imkey)) return NodeKind(k);
  }
  return std::nullopt;
}

}  // namespace

ImVec2 NodeCanvas::ToScreen(float x, float y) const { return ImVec2(origin_.x + (x + pan_.x) * zoom_, origin_.y + (y + pan_.y) * zoom_); }

bool NodeCanvas::Contains(ImVec2 s) const { return s.x >= origin_.x && s.y >= origin_.y && s.x < origin_.x + size_.x && s.y < origin_.y + size_.y; }

int NodeCanvas::NodeAt(const ShaderGraph& graph, ImVec2 screen) const {
  if (!Contains(screen)) return 0;
  for (auto it = graph.nodes.rbegin(); it != graph.nodes.rend(); ++it) {
    const Layout l = Measure(*it);
    const ImVec2 a = ToScreen(l.x, l.y), b = ToScreen(l.x + l.w, l.y + l.h);
    if (screen.x >= a.x && screen.x < b.x && screen.y >= a.y && screen.y < b.y) return it->id;
  }
  return 0;
}

ImVec2 NodeCanvas::ToGraph(ImVec2 s) const { return ImVec2((s.x - origin_.x) / zoom_ - pan_.x, (s.y - origin_.y) / zoom_ - pan_.y); }

bool NodeCanvas::Selected(int id) const { return std::find(selected_.begin(), selected_.end(), id) != selected_.end(); }

void NodeCanvas::Select(int id, bool add) {
  if (!add) selected_.clear();
  if (!Selected(id)) selected_.push_back(id);
  selected_comment_ = 0;
}

void NodeCanvas::Reset() {
  selected_.clear();
  selected_comment_ = 0;
  drag_ = Drag::kNone;
  frame_ = Frame::kAll;
}

void NodeCanvas::Focus(int id) {
  Select(id, false);
  frame_ = Frame::kSelection;
}

void NodeCanvas::FrameBounds(float x0, float y0, float x1, float y1) {
  if (size_.x <= 0 || size_.y <= 0) return;
  const float margin = 30;
  zoom_ = std::clamp(std::min(size_.x / (x1 - x0 + margin * 2), size_.y / (y1 - y0 + margin * 2)), kMinZoom, 1.25f);
  pan_ = ImVec2(size_.x / zoom_ * 0.5f - (x0 + x1) * 0.5f, size_.y / zoom_ * 0.5f - (y0 + y1) * 0.5f);
}

Node& NodeCanvas::Place(ShaderGraph& graph, NodeKind kind, ImVec2 at) {
  Node& n = graph.Add(kind, at.x, at.y);
  Select(n.id, false);
  return n;
}

void NodeCanvas::AddAtCentre(ShaderGraph& graph, NodeKind kind) {
  const ImVec2 c = ToGraph(ImVec2(origin_.x + size_.x * 0.5f, origin_.y + size_.y * 0.5f));
  Place(graph, kind, ImVec2(c.x - 75, c.y - 30));
}

void NodeCanvas::Copy(const ShaderGraph& graph) const {
  ShaderGraph clip;
  for (const Node& n : graph.nodes)
    if (Selected(n.id) && n.kind != NodeKind::kOutput) clip.nodes.push_back(n);
  for (const Link& l : graph.links)
    if (clip.Find(l.from) && clip.Find(l.to)) clip.links.push_back(l);
  for (const Comment& c : graph.comments)
    if (c.id == selected_comment_) clip.comments.push_back(c);
  if (clip.nodes.empty() && clip.comments.empty()) return;
  ImGui::SetClipboardText(GraphToText(clip).c_str());
}

void NodeCanvas::Paste(ShaderGraph& graph, const ShaderGraph& clip, ImVec2 at) {
  float x0 = 1e9f, y0 = 1e9f;
  for (const Node& n : clip.nodes) x0 = std::min(x0, n.x), y0 = std::min(y0, n.y);
  for (const Comment& c : clip.comments) x0 = std::min(x0, c.x), y0 = std::min(y0, c.y);
  std::map<int, int> ids;
  selected_.clear();
  for (Node n : clip.nodes) {
    if (n.kind == NodeKind::kOutput) continue;
    ids[n.id] = graph.next_id;
    n.id = graph.next_id++;
    n.x = n.x - x0 + at.x, n.y = n.y - y0 + at.y;
    selected_.push_back(n.id);
    graph.nodes.push_back(std::move(n));
  }
  for (const Link& l : clip.links)
    if (ids.count(l.from) && ids.count(l.to)) graph.links.push_back({graph.next_id++, ids[l.from], ids[l.to], l.input});
  for (Comment c : clip.comments) {
    c.id = graph.next_id++;
    c.x = c.x - x0 + at.x, c.y = c.y - y0 + at.y;
    graph.comments.push_back(std::move(c));
  }
  selected_comment_ = 0;
}

bool NodeCanvas::DrawMenu(ShaderGraph& graph) {
  bool changed = false;
  if (open_menu_) {
    ImGui::OpenPopup("##graph_menu");
    open_menu_ = false;
    search_.clear();
    focus_search_ = true;
  }
  ImGui::SetNextWindowSizeConstraints(ImVec2(260, 0), ImVec2(320, 460));
  if (!ImGui::BeginPopup("##graph_menu")) {
    menu_link_.reset();
    return false;
  }
  if (focus_search_) ImGui::SetKeyboardFocusHere(), focus_search_ = false;
  char buffer[128] = {};
  std::snprintf(buffer, sizeof(buffer), "%s", search_.c_str());
  ImGui::SetNextItemWidth(-1);
  const bool enter = ImGui::InputTextWithHint("##search", Tr("Search nodes..."), buffer, sizeof(buffer), ImGuiInputTextFlags_EnterReturnsTrue);
  search_ = buffer;
  const std::string needle = Lower(search_);
  std::optional<NodeKind> chosen;
  std::optional<NodeKind> first;
  ImGui::BeginChild("##menu_list", ImVec2(0, 380));
  for (const char* category : Categories()) {
    bool header = false;
    for (int k = 0; k < int(NodeKind::kCount); ++k) {
      const NodeInfo& info = Info(NodeKind(k));
      if (std::string_view(info.category) != category) continue;
      if (!needle.empty() && Lower(Tr(info.name)).find(needle) == std::string::npos && Lower(Tr(info.category)).find(needle) == std::string::npos) continue;
      // A wire from an input wants a node with an output; every node but Output has one.
      if (menu_link_ && menu_link_->input >= 0 && NodeKind(k) == NodeKind::kOutput) continue;
      if (!header) ImGui::TextDisabled("%s", Tr(category)), header = true;
      if (!first) first = NodeKind(k);
      const std::string label = info.hotkey ? std::format("  {}   {}", Tr(info.name), TrFormat("[{0} + click]", {std::string_view(&info.hotkey, 1)})) : std::format("  {}", Tr(info.name));
      if (ImGui::Selectable(label.c_str())) chosen = NodeKind(k);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr(info.help));
    }
  }
  ImGui::EndChild();
  if (enter && first) chosen = first;
  if (chosen) {
    Node& n = Place(graph, *chosen, menu_at_);
    const int id = n.id;
    const bool has_inputs = !Info(*chosen).inputs.empty();
    if (menu_link_) {
      if (menu_link_->input < 0 && has_inputs) graph.Connect(menu_link_->node, id, 0);
      if (menu_link_->input >= 0) graph.Connect(id, menu_link_->node, menu_link_->input);
    }
    menu_link_.reset();
    changed = true;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
  return changed;
}

bool NodeCanvas::Draw(ShaderGraph& graph) {
  bool changed = false;
  ImGuiIO& io = ImGui::GetIO();
  origin_ = ImGui::GetCursorScreenPos();
  size_ = ImGui::GetContentRegionAvail();
  if (size_.x < 50 || size_.y < 50) return false;
  ImGui::InvisibleButton("##canvas", size_, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
  const bool hovered = ImGui::IsItemHovered();
  const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput;
  // Nodes dragged in from the palette.
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("KAMEO_NODE")) {
      const ImVec2 at = ToGraph(io.MousePos);
      Place(graph, NodeKind(*static_cast<const int*>(payload->Data)), ImVec2(at.x - 20, at.y - 12));
      changed = true;
    }
    ImGui::EndDragDropTarget();
  }

  std::map<int, Layout> layouts;
  for (const Node& n : graph.nodes) layouts[n.id] = Measure(n);
  // A graph framed whole stays framed as the canvas resizes, until the view is moved by hand.
  if (fitted_ && (size_.x != last_size_.x || size_.y != last_size_.y)) frame_ = Frame::kAll;
  last_size_ = size_;
  if (frame_ != Frame::kNone) {
    fitted_ = frame_ == Frame::kAll;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (const auto& [id, l] : layouts) {
      if (frame_ == Frame::kSelection && !Selected(id)) continue;
      x0 = std::min(x0, l.x), y0 = std::min(y0, l.y), x1 = std::max(x1, l.x + l.w), y1 = std::max(y1, l.y + l.h);
    }
    if (x0 < x1) FrameBounds(x0, y0, x1, y1);
    frame_ = Frame::kNone;
  }

  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(origin_, ImVec2(origin_.x + size_.x, origin_.y + size_.y), true);
  dl->AddRectFilled(origin_, ImVec2(origin_.x + size_.x, origin_.y + size_.y), kBackground);
  // The grid: lines every 32 units, stronger every 160, the fine ones dropped when zoomed far out.
  for (const float step : {32.0f, 160.0f}) {
    if (step * zoom_ < 8) continue;
    const ImU32 colour = step > 100 ? kGridMajor : kGridMinor;
    const ImVec2 top = ToGraph(origin_);
    for (float x = std::floor(top.x / step) * step; ToScreen(x, 0).x < origin_.x + size_.x; x += step) dl->AddLine(ImVec2(ToScreen(x, 0).x, origin_.y), ImVec2(ToScreen(x, 0).x, origin_.y + size_.y), colour);
    for (float y = std::floor(top.y / step) * step; ToScreen(0, y).y < origin_.y + size_.y; y += step) dl->AddLine(ImVec2(origin_.x, ToScreen(0, y).y), ImVec2(origin_.x + size_.x, ToScreen(0, y).y), colour);
  }
  const bool text = zoom_ >= 0.45f;
  const float font = kText * zoom_;
  ImFont* imfont = ImGui::GetFont();

  // What the mouse is over, topmost first: pins, then nodes, then comments, then wires.
  const ImVec2 mouse = io.MousePos;
  int over_node = 0, over_input = -1, over_output = 0, over_comment = 0, over_link = 0;
  bool over_corner = false;
  const float pin_reach = std::max(kPin * 2.2f * zoom_, 7.0f);
  for (auto it = graph.nodes.rbegin(); it != graph.nodes.rend() && !over_node && !over_output && over_input < 0; ++it) {
    const Layout& l = layouts[it->id];
    const NodeInfo& info = Info(it->kind);
    if (it->kind != NodeKind::kOutput) {
      const ImVec2 p = ToScreen(OutputPin(l).x, OutputPin(l).y);
      if (std::hypot(mouse.x - p.x, mouse.y - p.y) < pin_reach) over_output = it->id;
    }
    for (int i = 0; i < int(info.inputs.size()) && !over_output; ++i) {
      const ImVec2 p = ToScreen(InputPin(l, i).x, InputPin(l, i).y);
      if (std::hypot(mouse.x - p.x, mouse.y - p.y) < pin_reach) over_input = i, over_node = it->id;
    }
    if (over_input >= 0 || over_output) break;
    const ImVec2 a = ToScreen(l.x, l.y), b = ToScreen(l.x + l.w, l.y + l.h);
    if (mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y) over_node = it->id;
  }
  if (!over_node && !over_output) {
    for (auto it = graph.comments.rbegin(); it != graph.comments.rend(); ++it) {
      const ImVec2 a = ToScreen(it->x, it->y), b = ToScreen(it->x + it->w, it->y + it->h);
      if (mouse.x >= b.x - 14 && mouse.x < b.x + 4 && mouse.y >= b.y - 14 && mouse.y < b.y + 4) {
        over_comment = it->id, over_corner = true;
        break;
      }
      if (mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < a.y + 28 * zoom_) {
        over_comment = it->id;
        break;
      }
    }
  }
  if (!over_node && !over_output && !over_comment) {
    for (const Link& link : graph.links) {
      if (!layouts.count(link.from) || !layouts.count(link.to)) continue;
      const ImVec2 a = ToScreen(OutputPin(layouts[link.from]).x, OutputPin(layouts[link.from]).y), b = ToScreen(InputPin(layouts[link.to], link.input).x, InputPin(layouts[link.to], link.input).y);
      if (BezierDistance(a, b, std::max(40.0f, std::fabs(b.x - a.x) * 0.5f), mouse) < 6) {
        over_link = link.id;
        break;
      }
    }
  }

  // Comments, behind everything.
  for (const Comment& c : graph.comments) {
    const ImVec2 a = ToScreen(c.x, c.y), b = ToScreen(c.x + c.w, c.y + c.h);
    dl->AddRectFilled(a, b, ColourOf(c.colour, 0.16f), 6 * zoom_);
    dl->AddRectFilled(a, ImVec2(b.x, a.y + 28 * zoom_), ColourOf(c.colour, 0.55f), 6 * zoom_, ImDrawFlags_RoundCornersTop);
    dl->AddRect(a, b, c.id == selected_comment_ ? kSelected : ColourOf(c.colour, 0.8f), 6 * zoom_, 0, c.id == selected_comment_ ? 2.0f : 1.0f);
    dl->AddTriangleFilled(ImVec2(b.x - 12 * zoom_, b.y), b, ImVec2(b.x, b.y - 12 * zoom_), ColourOf(c.colour, 0.8f));
    if (zoom_ >= 0.2f) dl->AddText(imfont, std::max(18.0f * zoom_, 11.0f), ImVec2(a.x + 8 * zoom_, a.y + 4 * zoom_), IM_COL32_WHITE, c.text.c_str());
  }

  // Wires.
  const auto wire = [&](ImVec2 a, ImVec2 b, ImU32 colour, float thickness) {
    const float bend = std::max(40.0f * zoom_, std::fabs(b.x - a.x) * 0.5f);
    dl->AddBezierCubic(a, ImVec2(a.x + bend, a.y), ImVec2(b.x - bend, b.y), b, colour, thickness);
  };
  for (const Link& link : graph.links) {
    if (!layouts.count(link.from) || !layouts.count(link.to)) continue;
    const Node* from = graph.Find(link.from);
    const ImVec2 a = ToScreen(OutputPin(layouts[link.from]).x, OutputPin(layouts[link.from]).y), b = ToScreen(InputPin(layouts[link.to], link.input).x, InputPin(layouts[link.to], link.input).y);
    const bool lit = link.id == over_link || Selected(link.from) || Selected(link.to);
    wire(a, b, lit ? kSelected : Lighter(CategoryColour(Info(from->kind).category), 0.45f), (lit ? 3.0f : 2.2f) * std::max(zoom_, 0.6f));
  }

  // Nodes.
  for (const Node& n : graph.nodes) {
    const Layout& l = layouts[n.id];
    const NodeInfo& info = Info(n.kind);
    const ImVec2 a = ToScreen(l.x, l.y), b = ToScreen(l.x + l.w, l.y + l.h);
    if (b.x < origin_.x || a.x > origin_.x + size_.x || b.y < origin_.y || a.y > origin_.y + size_.y) continue;
    const ImU32 head = CategoryColour(info.category);
    const bool selected = Selected(n.id);
    if (n.kind == NodeKind::kReroute) {
      dl->AddCircleFilled(ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), 7 * zoom_, selected ? kSelected : kWire);
      continue;
    }
    const float r = 6 * zoom_;
    dl->AddRectFilled(ImVec2(a.x + 3 * zoom_, a.y + 4 * zoom_), ImVec2(b.x + 3 * zoom_, b.y + 4 * zoom_), IM_COL32(0, 0, 0, 90), r);
    dl->AddRectFilled(a, b, kBody, r);
    dl->AddRectFilled(a, ImVec2(b.x, a.y + std::max(kTitle * zoom_, zoom_ >= 0.2f && !text ? 15.0f : 0.0f)), head, r, ImDrawFlags_RoundCornersTop);
    dl->AddRect(a, b, selected ? kSelected : kBorder, r, 0, selected ? 2.5f : 1.0f);
    // Titles stay readable zoomed out, as UE's do, clipped to their node.
    if (zoom_ >= 0.2f) {
      const float title = std::max(font, 11.0f);
      const ImVec4 clip(a.x, a.y, b.x - 4 * zoom_, a.y + std::max(kTitle * zoom_, title + 4));
      dl->AddText(imfont, title, ImVec2(a.x + 9 * zoom_, a.y + std::max(5 * zoom_, 1.0f)), IM_COL32_WHITE, Title(n).c_str(), nullptr, 0.0f, &clip);
    }
    for (int i = 0; i < int(info.inputs.size()); ++i) {
      const ImVec2 p = ToScreen(InputPin(l, i).x, InputPin(l, i).y);
      const bool wired = graph.Into(n.id, i) != nullptr;
      const bool hot = over_node == n.id && over_input == i;
      dl->AddCircleFilled(p, kPin * zoom_, wired ? kWire : kBody);
      dl->AddCircle(p, kPin * zoom_, hot ? kSelected : kWire, 12, 1.5f);
      if (!text) continue;
      dl->AddText(imfont, font, ImVec2(p.x + 10 * zoom_, p.y - font * 0.55f), kLabel, Tr(info.inputs[i]));
      if (!wired) {
        const std::string value = i == 0 && info.uv_input ? std::string(Tr("(surface UV)")) : "(" + ValueText(n.inputs[i]) + ")";
        dl->AddText(imfont, font * 0.9f, ImVec2(p.x + 16 * zoom_ + TextWidth(font, Tr(info.inputs[i])), p.y - font * 0.5f), kFaint, value.c_str());
      }
    }
    int row = int(info.inputs.size());
    if (n.kind == NodeKind::kColor) {
      const ImVec2 s = ToScreen(l.x + 10, l.y + kTitle + kPad + row * kRow + 2);
      dl->AddRectFilled(s, ImVec2(s.x + (l.w - 20) * zoom_, s.y + (kRow - 4) * zoom_), ColourOf({n.settings[0], n.settings[1], n.settings[2]}, 1.0f), 3 * zoom_);
      ++row;
    }
    if (text) {
      for (const std::string& line : Summary(n)) {
        const ImVec2 s = ToScreen(l.x + 10, l.y + kTitle + kPad + row * kRow + 3);
        dl->AddText(imfont, font * 0.9f, s, kFaint, line.c_str());
        ++row;
      }
    }
    if (n.kind != NodeKind::kOutput) {
      const ImVec2 p = ToScreen(OutputPin(l).x, OutputPin(l).y);
      const bool wired = std::any_of(graph.links.begin(), graph.links.end(), [&](const Link& k) { return k.from == n.id; });
      dl->AddCircleFilled(p, kPin * zoom_, wired ? kWire : kBody);
      dl->AddCircle(p, kPin * zoom_, over_output == n.id ? kSelected : kWire, 12, 1.5f);
    }
  }

  // The wire being dragged.
  if (drag_ == Drag::kLink && layouts.count(link_.node)) {
    const Layout& l = layouts[link_.node];
    if (link_.input < 0) wire(ToScreen(OutputPin(l).x, OutputPin(l).y), mouse, kSelected, 2.5f);
    else wire(mouse, ToScreen(InputPin(l, link_.input).x, InputPin(l, link_.input).y), kSelected, 2.5f);
  }
  if (drag_ == Drag::kBox) {
    dl->AddRectFilled(drag_from_, mouse, IM_COL32(255, 160, 50, 30));
    dl->AddRect(drag_from_, mouse, kSelected);
  }

  // The minimap, bottom right: the nodes and the view, click or drag to move there.
  const ImVec2 map_size(150, 95), map_at(origin_.x + size_.x - map_size.x - 10, origin_.y + size_.y - map_size.y - 10);
  float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
  for (const auto& [id, l] : layouts) bx0 = std::min(bx0, l.x), by0 = std::min(by0, l.y), bx1 = std::max(bx1, l.x + l.w), by1 = std::max(by1, l.y + l.h);
  const ImVec2 view0 = ToGraph(origin_), view1 = ToGraph(ImVec2(origin_.x + size_.x, origin_.y + size_.y));
  bx0 = std::min(bx0, view0.x), by0 = std::min(by0, view0.y), bx1 = std::max(bx1, view1.x), by1 = std::max(by1, view1.y);
  const float map_scale = std::min(map_size.x / std::max(bx1 - bx0, 1.0f), map_size.y / std::max(by1 - by0, 1.0f));
  const auto to_map = [&](float x, float y) { return ImVec2(map_at.x + (x - bx0) * map_scale, map_at.y + (y - by0) * map_scale); };
  const bool over_map = mouse.x >= map_at.x && mouse.x < map_at.x + map_size.x && mouse.y >= map_at.y && mouse.y < map_at.y + map_size.y;
  dl->AddRectFilled(map_at, ImVec2(map_at.x + map_size.x, map_at.y + map_size.y), IM_COL32(16, 15, 18, 200), 4);
  for (const Node& n : graph.nodes) {
    const Layout& l = layouts[n.id];
    dl->AddRectFilled(to_map(l.x, l.y), to_map(l.x + l.w, l.y + l.h), Selected(n.id) ? kSelected : CategoryColour(Info(n.kind).category));
  }
  dl->AddRect(to_map(view0.x, view0.y), to_map(view1.x, view1.y), IM_COL32_WHITE);
  dl->AddText(ImVec2(origin_.x + 10, origin_.y + size_.y - 24), kFaint, std::format("{}%   {}", int(std::round(zoom_ * 100)), Tr("right-click to add a node")).c_str());
  dl->PopClipRect();

  // Mouse.
  const bool ctrl = io.KeyCtrl, shift = io.KeyShift, alt = io.KeyAlt;
  if (hovered && io.MouseWheel != 0) {
    fitted_ = false;
    const ImVec2 before = ToGraph(mouse);
    zoom_ = std::clamp(zoom_ * std::pow(1.12f, io.MouseWheel), kMinZoom, kMaxZoom);
    const ImVec2 after = ToGraph(mouse);
    pan_.x += after.x - before.x, pan_.y += after.y - before.y;
  }
  if (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle))) drag_ = Drag::kPan, right_moved_ = false;
  if (drag_ == Drag::kPan) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
      if (std::fabs(io.MouseDelta.x) + std::fabs(io.MouseDelta.y) > 0) right_moved_ = true, fitted_ = false;
      pan_.x += io.MouseDelta.x / zoom_, pan_.y += io.MouseDelta.y / zoom_;
    } else {
      // A right click that did not pan opens the search menu where it was.
      if (!right_moved_ && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) menu_at_ = ToGraph(mouse), menu_link_.reset(), open_menu_ = true;
      drag_ = Drag::kNone;
    }
  }
  if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && over_link) {
    // A reroute dropped onto the wire, in UE's way.
    const auto it = std::find_if(graph.links.begin(), graph.links.end(), [&](const Link& l) { return l.id == over_link; });
    const Link link = *it;
    graph.links.erase(it);
    const ImVec2 at = ToGraph(mouse);
    const int reroute = graph.Add(NodeKind::kReroute, at.x - 12, at.y - 9).id;
    graph.Connect(link.from, reroute, 0);
    graph.Connect(reroute, link.to, link.input);
    changed = true;
  } else if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && drag_ == Drag::kNone) {
    const std::optional<NodeKind> hotkey = HeldHotkey();
    if (over_map) {
      drag_ = Drag::kMinimap;
    } else if (alt && (over_input >= 0 || over_output)) {
      if (over_output) std::erase_if(graph.links, [&](const Link& l) { return l.from == over_output; });
      else std::erase_if(graph.links, [&](const Link& l) { return l.to == over_node && l.input == over_input; });
      changed = true;
    } else if (over_output) {
      drag_ = Drag::kLink, link_ = {over_output, -1};
    } else if (over_input >= 0) {
      // Pulling a wired input picks up its wire from the far end, to drop elsewhere.
      if (const Link* into = graph.Into(over_node, over_input)) {
        const int from = into->from;
        std::erase_if(graph.links, [&](const Link& l) { return l.to == over_node && l.input == over_input; });
        drag_ = Drag::kLink, link_ = {from, -1};
        changed = true;
      } else {
        drag_ = Drag::kLink, link_ = {over_node, over_input};
      }
    } else if (over_node) {
      if (ctrl && Selected(over_node)) std::erase(selected_, over_node);
      else if (!Selected(over_node)) Select(over_node, ctrl || shift);
      selected_comment_ = 0;
      drag_ = Drag::kNodes;
    } else if (over_comment) {
      selected_comment_ = over_comment;
      if (!ctrl && !shift) selected_.clear();
      drag_ = over_corner ? Drag::kResize : Drag::kComment;
      carried_.clear();
      if (const auto c = std::find_if(graph.comments.begin(), graph.comments.end(), [&](const Comment& k) { return k.id == over_comment; }); c != graph.comments.end()) {
        for (const auto& [id, l] : layouts)
          if (l.x >= c->x && l.y >= c->y && l.x + l.w <= c->x + c->w && l.y + l.h <= c->y + c->h) carried_.push_back(id);
      }
    } else if (hotkey) {
      const ImVec2 at = ToGraph(mouse);
      Place(graph, *hotkey, ImVec2(at.x - 20, at.y - 12));
      changed = true;
    } else {
      if (!ctrl && !shift) selected_.clear(), selected_comment_ = 0;
      drag_ = Drag::kBox, drag_from_ = mouse;
    }
  }
  if (drag_ == Drag::kNodes || drag_ == Drag::kComment || drag_ == Drag::kResize || drag_ == Drag::kMinimap) {
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      drag_ = Drag::kNone;
    } else if (drag_ == Drag::kMinimap) {
      fitted_ = false;
      const ImVec2 target((mouse.x - map_at.x) / map_scale + bx0, (mouse.y - map_at.y) / map_scale + by0);
      pan_ = ImVec2(size_.x / zoom_ * 0.5f - target.x, size_.y / zoom_ * 0.5f - target.y);
    } else if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) {
      const float dx = io.MouseDelta.x / zoom_, dy = io.MouseDelta.y / zoom_;
      if (drag_ == Drag::kNodes) {
        for (Node& n : graph.nodes)
          if (Selected(n.id)) n.x += dx, n.y += dy;
      }
      for (Comment& c : graph.comments) {
        if (c.id != selected_comment_) continue;
        if (drag_ == Drag::kResize) {
          c.w = std::max(120.0f, c.w + dx), c.h = std::max(80.0f, c.h + dy);
        } else if (drag_ == Drag::kComment) {
          c.x += dx, c.y += dy;
          for (Node& n : graph.nodes)
            if (std::find(carried_.begin(), carried_.end(), n.id) != carried_.end()) n.x += dx, n.y += dy;
        }
      }
      changed = true;
    }
  }
  if (drag_ == Drag::kBox && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const ImVec2 a(std::min(drag_from_.x, mouse.x), std::min(drag_from_.y, mouse.y)), b(std::max(drag_from_.x, mouse.x), std::max(drag_from_.y, mouse.y));
    for (const auto& [id, l] : layouts) {
      const ImVec2 p = ToScreen(l.x, l.y), q = ToScreen(l.x + l.w, l.y + l.h);
      if (q.x >= a.x && p.x <= b.x && q.y >= a.y && p.y <= b.y && !Selected(id)) selected_.push_back(id);
    }
    drag_ = Drag::kNone;
  }
  if (drag_ == Drag::kLink && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    drag_ = Drag::kNone;
    if (link_.input < 0 && over_input >= 0) {
      changed |= graph.Connect(link_.node, over_node, over_input);
    } else if (link_.input >= 0 && over_output) {
      changed |= graph.Connect(over_output, link_.node, link_.input);
    } else if (!over_node && !over_output) {
      // Dropped on nothing: the search menu, wiring what is picked to this end.
      menu_at_ = ToGraph(mouse), menu_link_ = link_, open_menu_ = true;
    }
  }

  // Keys, while the graph has focus.
  if (focused && hovered) {
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) || (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !io.WantTextInput)) {
      for (const int id : selected_)
        if (const Node* n = graph.Find(id); n && n->kind != NodeKind::kOutput) graph.Remove(id);
      std::erase_if(graph.comments, [&](const Comment& c) { return c.id == selected_comment_; });
      selected_.clear(), selected_comment_ = 0;
      changed = true;
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A)) {
      selected_.clear();
      for (const Node& n : graph.nodes) selected_.push_back(n.id);
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C)) Copy(graph);
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_X)) {
      Copy(graph);
      for (const int id : selected_)
        if (const Node* n = graph.Find(id); n && n->kind != NodeKind::kOutput) graph.Remove(id);
      selected_.clear();
      changed = true;
    }
    if (ctrl && (ImGui::IsKeyPressed(ImGuiKey_V) || ImGui::IsKeyPressed(ImGuiKey_D))) {
      const bool duplicate = ImGui::IsKeyPressed(ImGuiKey_D);
      if (duplicate) Copy(graph);
      if (const char* clip = ImGui::GetClipboardText()) {
        if (const std::optional<ShaderGraph> pasted = GraphFromText(clip)) {
          ImVec2 at = ToGraph(mouse);
          if (duplicate && !pasted->nodes.empty()) at = ImVec2(pasted->nodes[0].x + 30, pasted->nodes[0].y + 30);
          Paste(graph, *pasted, at);
          changed = true;
        }
      }
    }
    if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_F)) frame_ = selected_.empty() ? Frame::kAll : Frame::kSelection;
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) frame_ = Frame::kAll;
    if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_C)) {
      // A comment around the selection, or at the mouse.
      Comment c;
      c.id = graph.next_id++;
      c.text = Tr("Comment");
      float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
      for (const int id : selected_)
        if (layouts.count(id)) x0 = std::min(x0, layouts[id].x), y0 = std::min(y0, layouts[id].y), x1 = std::max(x1, layouts[id].x + layouts[id].w), y1 = std::max(y1, layouts[id].y + layouts[id].h);
      if (x0 < x1) c.x = x0 - 20, c.y = y0 - 44, c.w = x1 - x0 + 40, c.h = y1 - y0 + 64;
      else c.x = ToGraph(mouse).x, c.y = ToGraph(mouse).y;
      graph.comments.insert(graph.comments.begin(), c);
      selected_comment_ = c.id;
      changed = true;
    }
  }
  changed |= DrawMenu(graph);
  return changed;
}

}  // namespace kameo::shader_editor
