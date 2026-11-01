#include "shader_editor/editor.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <string_view>

#include <imgui.h>

namespace fs = std::filesystem;

namespace kameo::shader_editor {

namespace {

constexpr char kGraphExtension[] = ".kgraph";
constexpr char kPackageExtension[] = ".kshader";
constexpr std::chrono::milliseconds kLiveDelay{600};
// Compiles past this are too slow to run after every edit.
constexpr size_t kLiveLimit = 400;
constexpr float kLeftWidth = 250, kRightWidth = 360;

const char kRawTemplate[] =
    "    // ShadeInputs s: colour, alpha, albedo, light, normal, view, up, light_dir, light_colour, uv, screen, time.\n"
    "    const float lit = ModStep(0.2, ModKeyLight(s, 0.8, 0.55, 0.45));\n"
    "    return s.albedo * lerp(float3(0.36, 0.31, 0.52), 1.0, lit);";

int ResizeString(ImGuiInputTextCallbackData* data) {
  if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
    auto* s = static_cast<std::string*>(data->UserData);
    s->resize(size_t(data->BufTextLen));
    data->Buf = s->data();
  }
  return 0;
}

bool InputMultiline(const char* label, std::string& text, ImVec2 size) {
  return ImGui::InputTextMultiline(label, text.data(), text.capacity() + 1, size, ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_AllowTabInput, ResizeString, &text);
}

bool InputLine(const char* label, std::string& text) { return ImGui::InputText(label, text.data(), text.capacity() + 1, ImGuiInputTextFlags_CallbackResize, ResizeString, &text); }

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// The body of the function a graph compiles to, for starting raw HLSL from it; empty when it calls Custom nodes' functions, which live outside it.
std::string GraphBody(const ShaderGraph& graph) {
  ShaderGraph g = graph;
  g.raw = false;
  if (std::any_of(g.nodes.begin(), g.nodes.end(), [](const Node& n) { return n.kind == NodeKind::kCustom; })) return {};
  const std::string source = GenerateHlsl(g);
  const size_t open = source.find('{'), close = source.rfind('}');
  return open == std::string::npos || close == std::string::npos ? std::string() : source.substr(open + 2, close - open - 2);
}

// An input's default: a colour picker for colours, one number while all three match, else three.
bool EditValue(const char* label, Float3& v, bool colour) {
  if (colour) return ImGui::ColorEdit3(label, v.data(), ImGuiColorEditFlags_Float);
  if (v[0] == v[1] && v[1] == v[2]) {
    if (!ImGui::DragFloat(label, &v[0], 0.01f)) return false;
    v[1] = v[2] = v[0];
    return true;
  }
  return ImGui::DragFloat3(label, v.data(), 0.01f);
}

// A file name from what the author typed.
std::string FileName(const std::string& name) {
  std::string out;
  for (const char c : name) out += std::string_view("<>:\"/\\|?*").find(c) == std::string_view::npos && static_cast<unsigned char>(c) >= 32 ? c : '_';
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  return out.empty() ? "Shader" : out;
}

}  // namespace

Editor::Editor() {
  New();
  preparing_ = std::async(std::launch::async, [this] {
    std::string error;
    source_.Prepare(error);
    return error;
  });
}

void Editor::New() {
  mod_ = {};
  mod_.graph = MakePreset(Preset::kCelShade);
  mod_.targets = {"kameo"};
  name_ = "My Shader";
  Replaced();
  last_source_.clear();
  live_pending_ = false;
  log_.clear();
  Changed();
}

void Editor::Open(const fs::path& path) {
  std::string error;
  std::optional<ShaderMod> mod = LoadMod(path, error);
  if (!mod) {
    log_ = error;
    log_ok_ = false;
    return;
  }
  mod_ = std::move(*mod);
  name_ = path.stem().string();
  Replaced();
  last_source_.clear();
  live_pending_ = false;
  log_ = std::format("{} opened", name_);
  log_ok_ = true;
  Changed();
}

// The graph was replaced whole: a load, a preset, a new mod; its history starts again from it.
void Editor::Replaced() {
  canvas_.Reset();
  history_ = History{{mod_.graph}, {GraphToText(mod_.graph)}, 0};
}

void Editor::Changed() {
  std::string source = GenerateHlsl(mod_.graph);
  for (const std::string& target : mod_.targets) source += target + "\n";
  if (source == last_source_) return;
  const bool first = last_source_.empty();
  last_source_ = std::move(source);
  if (!first) edited_at_ = std::chrono::steady_clock::now(), live_pending_ = true;
}

// Records the graph for undo once an edit has finished, not at every frame of a drag.
void Editor::Remember() {
  if (ImGui::IsAnyItemActive() || ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
  std::string key = GraphToText(mod_.graph);
  if (!history_.keys.empty() && history_.keys[history_.at] == key) return;
  const size_t keep = history_.keys.empty() ? 0 : history_.at + 1;
  history_.states.resize(keep);
  history_.keys.resize(keep);
  history_.states.push_back(mod_.graph);
  history_.keys.push_back(std::move(key));
  if (history_.states.size() > 200) history_.states.erase(history_.states.begin()), history_.keys.erase(history_.keys.begin());
  history_.at = history_.states.size() - 1;
}

void Editor::Step(int direction) {
  const long long at = (long long)history_.at + direction;
  if (at < 0 || at >= (long long)history_.states.size()) return;
  history_.at = size_t(at);
  mod_.graph = history_.states[history_.at];
}

void Editor::Build(bool full) {
  if (build_.valid() || !ready_) return;
  live_pending_ = false;
  const std::string file = FileName(name_);
  BuildRequest request;
  request.shade = GenerateHlsl(mod_.graph);
  request.shaders = source_.Shaders(mod_.targets);
  request.meshes = source_.Meshes(mod_.targets);
  request.out = ModsFolder() / (file + kPackageExtension);
  const bool vulkan = GameUsesVulkan();
  request.dxil = full || !vulkan;
  request.spirv = full || vulkan;
  if (full) {
    std::error_code ec;
    fs::create_directories(ModsFolder(), ec);
    std::string error;
    if (!SaveMod(mod_, ModsFolder() / (file + kGraphExtension), error)) {
      log_ = error;
      log_ok_ = false;
      return;
    }
  }
  build_done_ = 0;
  build_total_ = request.shaders.size() * (size_t(request.dxil) + size_t(request.spirv));
  build_started_ = std::chrono::steady_clock::now();
  build_ = std::async(std::launch::async, [this, request = std::move(request), full] {
    std::string log;
    const bool ok = BuildMod(source_, request, log, &build_done_);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - build_started_).count();
    if (ok) log += std::format(" in {:.1f} s{}", seconds, full ? ", and saved" : ", for this renderer only: Save before sharing it");
    return std::pair(ok, log);
  });
}

void Editor::Finish() {
  if (!build_.valid() || build_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  auto [ok, log] = build_.get();
  log_ok_ = ok;
  log_ = std::move(log);
}

void Editor::DrawToolbar() {
  ImGui::SetNextItemWidth(200);
  InputLine("##name", name_);
  ImGui::SetItemTooltip("The mod's name, which is also its file's");
  ImGui::SameLine();
  if (ImGui::Button("New")) New();
  ImGui::SameLine();
  if (ImGui::Button("Open...")) {
    saved_.clear();
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(ModsFolder(), ec))
      if (entry.path().extension() == kGraphExtension) saved_.push_back(entry.path());
    std::sort(saved_.begin(), saved_.end());
    ImGui::OpenPopup("open mod");
  }
  if (ImGui::BeginPopup("open mod")) {
    if (saved_.empty()) ImGui::TextDisabled("No saved mods yet.");
    for (const fs::path& path : saved_)
      if (ImGui::Selectable(path.stem().string().c_str())) Open(path);
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  if (ImGui::Button("Mods Folder")) {
    std::error_code ec;
    fs::create_directories(ModsFolder(), ec);
    ShellExecuteW(nullptr, L"open", ModsFolder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(170);
  if (ImGui::BeginCombo("##preset", "Start from...")) {
    for (int p = 0; p < int(Preset::kCount); ++p)
      if (ImGui::Selectable(PresetName(Preset(p)))) pending_preset_ = p;
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(history_.at == 0);
  if (ImGui::Button("Undo")) Step(-1);
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("Ctrl+Z");
  ImGui::SameLine();
  ImGui::BeginDisabled(history_.at + 1 >= history_.states.size());
  if (ImGui::Button("Redo")) Step(1);
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("Ctrl+Y");
  ShaderGraph& g = mod_.graph;
  ImGui::SameLine();
  ImGui::BeginDisabled(g.raw);
  if (ImGui::Button("Frame All")) canvas_.FrameAll();
  ImGui::SetItemTooltip("Home, or F with nothing selected");
  ImGui::SameLine();
  if (ImGui::Button("Comment")) {
    Comment c;
    c.id = g.next_id++;
    c.x = 0, c.y = -60;
    g.comments.push_back(c);
  }
  ImGui::SetItemTooltip("C on the canvas wraps the selection in a comment");
  ImGui::EndDisabled();
  ImGui::SameLine();
  bool raw = g.raw;
  if (ImGui::Checkbox("Raw HLSL", &raw)) {
    if (raw && g.raw_code.empty()) {
      g.raw_code = GraphBody(g);
      if (g.raw_code.empty()) g.raw_code = kRawTemplate;
    }
    g.raw = raw;
  }
  ImGui::SameLine();
  ImGui::Checkbox("Live Update", &live_);
  ImGui::SetItemTooltip("Compiles and applies to the running game a moment after each edit");
  ImGui::SameLine();
  const bool building = build_.valid();
  ImGui::BeginDisabled(building || !ready_ || mod_.targets.empty());
  if (ImGui::Button("Save")) Build(true);
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("Compiles for both renderers and saves the mod; a running game picks it up");

  if (pending_preset_ >= 0) ImGui::OpenPopup("replace graph");
  if (ImGui::BeginPopupModal("replace graph", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Replace the graph with %s?", PresetName(Preset(std::max(pending_preset_, 0))));
    ImGui::TextDisabled("Undo brings yours back.");
    if (ImGui::Button("Replace")) {
      mod_.graph = MakePreset(Preset(pending_preset_));
      canvas_.Reset();
      pending_preset_ = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Keep Mine")) {
      pending_preset_ = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

// Every node by category, searchable; drag one onto the graph or double-click it to add it in the middle.
void Editor::DrawPalette() {
  ImGui::SetNextItemWidth(-1);
  InputLine("##palette_search", palette_search_);
  if (palette_search_.empty()) {
    const ImVec2 at = ImGui::GetItemRectMin();
    ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + 6, at.y + 3), ImGui::GetColorU32(ImGuiCol_TextDisabled), "Search nodes");
  }
  const std::string needle = Lower(palette_search_);
  ImGui::BeginChild("##palette_list");
  for (const char* category : Categories()) {
    std::vector<int> kinds;
    for (int k = 0; k < int(NodeKind::kCount); ++k) {
      const NodeInfo& info = Info(NodeKind(k));
      if (NodeKind(k) == NodeKind::kOutput || std::string_view(info.category) != category) continue;
      if (!needle.empty() && Lower(info.name).find(needle) == std::string::npos && Lower(info.help).find(needle) == std::string::npos) continue;
      kinds.push_back(k);
    }
    if (kinds.empty()) continue;
    const uint32_t c = CategoryColour(category);
    ImGui::PushStyleColor(ImGuiCol_Header, (c & 0x00FFFFFF) | 0x90000000);
    ImGui::SetNextItemOpen(true, needle.empty() ? ImGuiCond_Once : ImGuiCond_Always);
    const bool open = ImGui::CollapsingHeader(category);
    ImGui::PopStyleColor();
    if (!open) continue;
    for (const int k : kinds) {
      const NodeInfo& info = Info(NodeKind(k));
      ImGui::PushID(k);
      if (ImGui::Selectable(info.name, false, ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) canvas_.AddAtCentre(mod_.graph, NodeKind(k));
      if (info.hotkey) {
        const char key[2] = {info.hotkey, 0};
        ImGui::GetWindowDrawList()->AddText(ImVec2(ImGui::GetItemRectMax().x - ImGui::CalcTextSize(key).x - 4, ImGui::GetItemRectMin().y), ImGui::GetColorU32(ImGuiCol_TextDisabled), key);
      }
      if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("KAMEO_NODE", &k, sizeof(k));
        ImGui::TextUnformatted(info.name);
        ImGui::EndDragDropSource();
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip(info.hotkey ? "%s\n\nHold %c and click the graph to place one." : "%s", info.help, info.hotkey);
      ImGui::PopID();
    }
  }
  ImGui::EndChild();
}

// The selected node's settings, input defaults, name and code, or the selected comment's text and colour.
void Editor::DrawDetails() {
  ShaderGraph& g = mod_.graph;
  if (canvas_.selected_comment()) {
    for (Comment& c : g.comments) {
      if (c.id != canvas_.selected_comment()) continue;
      ImGui::SeparatorText("Comment");
      ImGui::SetNextItemWidth(-1);
      InputLine("##comment_text", c.text);
      ImGui::ColorEdit3("Colour", c.colour.data());
      ImGui::DragFloat2("Size", &c.w, 1.0f, 120.0f, 4000.0f);
    }
    return;
  }
  if (canvas_.selected().size() != 1) {
    ImGui::SeparatorText("Details");
    ImGui::TextDisabled(canvas_.selected().empty() ? "Select a node to edit it." : "%d nodes selected.", int(canvas_.selected().size()));
    return;
  }
  Node* n = g.Find(canvas_.selected()[0]);
  if (!n) return;
  const NodeInfo& info = Info(n->kind);
  ImGui::SeparatorText(info.name);
  ImGui::PushTextWrapPos(0);
  ImGui::TextDisabled("%s", info.help);
  ImGui::PopTextWrapPos();
  ImGui::PushID(n->id);
  ImGui::PushItemWidth(-90);
  if (n->kind == NodeKind::kFloat || n->kind == NodeKind::kColor) {
    InputLine("Parameter", n->name);
    ImGui::SetItemTooltip("Named constants are listed under Parameters, to tune without finding them in the graph");
  }
  if (n->kind == NodeKind::kColor) {
    ImGui::ColorEdit3("Colour", n->settings.data(), ImGuiColorEditFlags_Float);
  } else if (n->kind == NodeKind::kComponentMask) {
    int channel = std::clamp(int(n->settings[0]), 0, 2);
    if (ImGui::Combo("Channel", &channel, "R (X)\0G (Y)\0B (Z)\0")) n->settings[0] = float(channel);
  } else {
    for (size_t s = 0; s < n->settings.size() && s < info.settings.size(); ++s) ImGui::DragFloat(info.settings[s], &n->settings[s], 0.01f);
  }
  bool header = false;
  for (size_t i = 0; i < n->inputs.size() && i < info.inputs.size(); ++i) {
    if (g.Into(n->id, int(i))) continue;
    if (!header) ImGui::SeparatorText("Unwired inputs"), header = true;
    ImGui::PushID(int(i));
    if (i == 0 && info.uv_input) {
      ImGui::TextDisabled("%s: the surface's UV", info.inputs[i]);
    } else {
      EditValue(info.inputs[i], n->inputs[i], std::string_view(info.inputs[i]) == "Colour");
    }
    ImGui::PopID();
  }
  ImGui::PopItemWidth();
  if (n->kind == NodeKind::kCustom) {
    ImGui::SeparatorText("HLSL");
    ImGui::TextDisabled("float3 f(ShadeInputs s, float3 a, b, c, d) { ... }");
    InputMultiline("##code", n->code, ImVec2(-1, 160));
  }
  ImGui::PopID();
}

// The named constants of the graph, to tune in one place; clicking a name finds it in the graph.
void Editor::DrawParameters() {
  ImGui::SeparatorText("Parameters");
  bool any = false;
  for (Node& n : mod_.graph.nodes) {
    if (n.name.empty() || (n.kind != NodeKind::kFloat && n.kind != NodeKind::kColor)) continue;
    any = true;
    ImGui::PushID(n.id);
    if (ImGui::SmallButton(n.name.c_str())) canvas_.Focus(n.id);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.5f);
    ImGui::SetNextItemWidth(-1);
    if (n.kind == NodeKind::kColor) ImGui::ColorEdit3("##v", n.settings.data(), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs);
    else ImGui::DragFloat("##v", &n.settings[0], 0.01f);
    ImGui::PopID();
  }
  if (!any) ImGui::TextDisabled("Name a Number or Colour node to list it here.");
}

bool Editor::Targeted(const std::string& key) const { return std::find(mod_.targets.begin(), mod_.targets.end(), key) != mod_.targets.end(); }

void Editor::TargetBox(const Target& target) {
  bool on = Targeted(target.key);
  if (!ImGui::Checkbox(target.label.c_str(), &on)) return;
  if (on) mod_.targets.push_back(target.key);
  else std::erase(mod_.targets, target.key);
}

// What the mod applies to, by what a player would call it.
void Editor::DrawTargets() {
  ImGui::SeparatorText("Applies to");
  if (!ready_) {
    ImGui::TextDisabled("Preparing the game's shaders...");
    return;
  }
  TargetBox(source_.all_characters());
  TargetBox(source_.everything());
  if (ImGui::TreeNodeEx("Characters", ImGuiTreeNodeFlags_DefaultOpen)) {
    for (const Target& target : source_.characters()) TargetBox(target);
    ImGui::TreePop();
  }
  if (ImGui::TreeNode("Levels")) {
    for (const Target& target : source_.levels()) TargetBox(target);
    ImGui::TreePop();
  }
  ImGui::Spacing();
  ImGui::PushTextWrapPos(0);
  const size_t shaders = source_.Shaders(mod_.targets).size();
  if (source_.Meshes(mod_.targets).empty()) ImGui::TextDisabled("%zu shaders. These choose the game's shaders, not objects, so anything else drawn with the same ones changes too.", shaders);
  else ImGui::TextDisabled("%zu shaders, on this model alone: anything else drawn with them keeps the game's look.", shaders);
  ImGui::PopTextWrapPos();
}

void Editor::DrawLog() {
  ImGui::SeparatorText("Compile log");
  ImGui::PushTextWrapPos(0);
  if (build_.valid()) ImGui::Text("Compiling... %zu of %zu", build_done_.load(), build_total_);
  else if (log_.empty()) ImGui::TextDisabled("Save, or Live Update, compiles the shaders. Run the game to see them.");
  ImGui::PushStyleColor(ImGuiCol_Text, log_ok_ ? IM_COL32(150, 220, 150, 255) : IM_COL32(255, 120, 110, 255));
  ImGui::TextUnformatted(log_.c_str());
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
}

void Editor::Draw(ID3D11Device* device, ID3D11DeviceContext* context) {
  if (!preview_attached_) {
    preview_.Attach(device, context);
    preview_attached_ = true;
  }
  if (preparing_.valid() && preparing_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    log_ = preparing_.get();
    log_ok_ = log_.empty();
    ready_ = log_.empty();
    if (ready_) {
      std::ifstream in(source_.folder() / "shader_mod.hlsli");
      preview_.SetTemplate(std::string(std::istreambuf_iterator<char>(in), {}));
    }
  }
  Finish();
  DrawToolbar();
  ShaderGraph& g = mod_.graph;
  const ImGuiIO& io = ImGui::GetIO();
  if (!io.WantTextInput && io.KeyCtrl) {
    if (ImGui::IsKeyPressed(ImGuiKey_Z)) Step(io.KeyShift ? 1 : -1);
    if (ImGui::IsKeyPressed(ImGuiKey_Y)) Step(1);
  }
  ImGui::BeginChild("left", ImVec2(kLeftWidth, 0));
  const float half = ImGui::GetContentRegionAvail().y * 0.5f;
  ImGui::BeginChild("palette", ImVec2(0, half), ImGuiChildFlags_Borders);
  DrawPalette();
  ImGui::EndChild();
  ImGui::BeginChild("details", ImVec2(0, 0), ImGuiChildFlags_Borders);
  DrawDetails();
  DrawParameters();
  ImGui::EndChild();
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("graph", ImVec2(ImGui::GetContentRegionAvail().x - kRightWidth, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  if (g.raw) {
    ImGui::TextDisabled("float3 Shade(ShadeInputs s) {  ... the body below ...  }   Helpers: ModKeyLight, ModStep, ModBands, ModFresnel, ModOutline, ModHalftone, ModLuminance, ModNoise (docs/modding.md).");
    InputMultiline("##hlsl", g.raw_code, ImVec2(-1, -1));
  } else {
    canvas_.Draw(g);
  }
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("right");
  const float height = ImGui::GetContentRegionAvail().y;
  ImGui::BeginChild("preview", ImVec2(0, height * 0.5f), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  preview_.SetShade(GenerateHlsl(g));
  preview_.Draw();
  ImGui::EndChild();
  ImGui::BeginChild("targets", ImVec2(0, height * 0.32f), ImGuiChildFlags_Borders);
  DrawTargets();
  ImGui::EndChild();
  ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders);
  DrawLog();
  ImGui::EndChild();
  ImGui::EndChild();
  Changed();
  Remember();
  if (live_ && live_pending_ && ready_ && !build_.valid() && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && std::chrono::steady_clock::now() - edited_at_ > kLiveDelay) {
    live_pending_ = false;
    if (source_.Shaders(mod_.targets).size() <= kLiveLimit) Build(false);
  }
}

}  // namespace kameo::shader_editor
