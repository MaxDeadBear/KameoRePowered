#include "shader_editor/editor.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <fstream>
#include <string_view>

#include <imgui.h>

#include "core/localization.h"

namespace fs = std::filesystem;

namespace kameo::shader_editor {

using l10n::Tr;
using l10n::TrFormat;

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
  log_ = TrFormat("{0} opened", {name_});
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
  request.model_type = source_.CostumeModel(mod_.targets);
  request.unlit = mod_.unlit || source_.NeedsUnlit(mod_.targets);
  request.textures = PackageTextures(GameTextures(mod_.graph));
  request.see_through = SeeThrough(mod_.graph);
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
    const std::string took = std::format("{:.1f}", seconds);
    if (ok) log = full ? TrFormat("{0} in {1} s, and saved", {log, took}) : TrFormat("{0} in {1} s, for this renderer only: Save before sharing it", {log, took});
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
  ImGui::SetItemTooltip("%s", Tr("The mod's name, which is also its file's"));
  ImGui::SameLine();
  if (ImGui::Button(Tr("New"))) New();
  ImGui::SameLine();
  if (ImGui::Button(Tr("Open..."))) {
    saved_.clear();
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(ModsFolder(), ec))
      if (entry.path().extension() == kGraphExtension) saved_.push_back(entry.path());
    std::sort(saved_.begin(), saved_.end());
    ImGui::OpenPopup("open mod");
  }
  if (ImGui::BeginPopup("open mod")) {
    if (saved_.empty()) ImGui::TextDisabled("%s", Tr("No saved mods yet."));
    for (const fs::path& path : saved_)
      if (ImGui::Selectable(path.stem().string().c_str())) Open(path);
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  if (ImGui::Button(Tr("Mods Folder"))) {
    std::error_code ec;
    fs::create_directories(ModsFolder(), ec);
    ShellExecuteW(nullptr, L"open", ModsFolder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(170);
  if (ImGui::BeginCombo("##preset", Tr("Start from..."))) {
    for (int p = 0; p < int(Preset::kCount); ++p)
      if (ImGui::Selectable(Tr(PresetName(Preset(p))))) pending_preset_ = p;
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(history_.at == 0);
  if (ImGui::Button(Tr("Undo"))) Step(-1);
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("%s", Tr("Ctrl+Z"));
  ImGui::SameLine();
  ImGui::BeginDisabled(history_.at + 1 >= history_.states.size());
  if (ImGui::Button(Tr("Redo"))) Step(1);
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("%s", Tr("Ctrl+Y"));
  ShaderGraph& g = mod_.graph;
  ImGui::SameLine();
  ImGui::BeginDisabled(g.raw);
  if (ImGui::Button(Tr("Frame All"))) canvas_.FrameAll();
  ImGui::SetItemTooltip("%s", Tr("Home, or F with nothing selected"));
  ImGui::SameLine();
  if (ImGui::Button(Tr("Comment"))) {
    Comment c;
    c.id = g.next_id++;
    c.text = Tr("Comment");
    c.x = 0, c.y = -60;
    g.comments.push_back(c);
  }
  ImGui::SetItemTooltip("%s", Tr("C on the canvas wraps the selection in a comment"));
  ImGui::EndDisabled();
  ImGui::SameLine();
  bool raw = g.raw;
  if (ImGui::Checkbox(Tr("Raw HLSL"), &raw)) {
    if (raw && g.raw_code.empty()) {
      g.raw_code = GraphBody(g);
      if (g.raw_code.empty()) g.raw_code = kRawTemplate;
    }
    g.raw = raw;
  }
  ImGui::SameLine();
  ImGui::Checkbox(Tr("Live Update"), &live_);
  ImGui::SetItemTooltip("%s", Tr("Compiles and applies to the running game a moment after each edit"));
  ImGui::SameLine();
  const bool building = build_.valid();
  ImGui::BeginDisabled(building || !ready_ || mod_.targets.empty());
  if (ImGui::Button(Tr("Save"))) Build(true);
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("%s", Tr("Compiles for both renderers and saves the mod; a running game picks it up"));

  const std::string replace_title = std::string(Tr("Replace graph")) + "###replace graph";
  if (pending_preset_ >= 0) ImGui::OpenPopup(replace_title.c_str());
  if (ImGui::BeginPopupModal(replace_title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted(TrFormat("Replace the graph with {0}?", {Tr(PresetName(Preset(std::max(pending_preset_, 0))))}).c_str());
    ImGui::TextDisabled("%s", Tr("Undo brings yours back."));
    if (ImGui::Button(Tr("Replace"))) {
      mod_.graph = MakePreset(Preset(pending_preset_));
      // The grass is for the levels' grass, not a character.
      if (Preset(pending_preset_) == Preset::kGrass) mod_.targets = {"grass"};
      canvas_.Reset();
      pending_preset_ = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr("Keep Mine"))) {
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
    ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + 6, at.y + 3), ImGui::GetColorU32(ImGuiCol_TextDisabled), Tr("Search nodes"));
  }
  const std::string needle = Lower(palette_search_);
  ImGui::BeginChild("##palette_list");
  for (const char* category : Categories()) {
    std::vector<int> kinds;
    for (int k = 0; k < int(NodeKind::kCount); ++k) {
      const NodeInfo& info = Info(NodeKind(k));
      if (NodeKind(k) == NodeKind::kOutput || std::string_view(info.category) != category) continue;
      if (!needle.empty() && Lower(Tr(info.name)).find(needle) == std::string::npos && Lower(Tr(info.help)).find(needle) == std::string::npos) continue;
      kinds.push_back(k);
    }
    if (kinds.empty()) continue;
    const uint32_t c = CategoryColour(category);
    ImGui::PushStyleColor(ImGuiCol_Header, (c & 0x00FFFFFF) | 0x90000000);
    ImGui::SetNextItemOpen(true, needle.empty() ? ImGuiCond_Once : ImGuiCond_Always);
    const bool open = ImGui::CollapsingHeader(Tr(category));
    ImGui::PopStyleColor();
    if (!open) continue;
    for (const int k : kinds) {
      const NodeInfo& info = Info(NodeKind(k));
      ImGui::PushID(k);
      if (ImGui::Selectable(Tr(info.name), false, ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) canvas_.AddAtCentre(mod_.graph, NodeKind(k));
      if (info.hotkey) {
        const char key[2] = {info.hotkey, 0};
        ImGui::GetWindowDrawList()->AddText(ImVec2(ImGui::GetItemRectMax().x - ImGui::CalcTextSize(key).x - 4, ImGui::GetItemRectMin().y), ImGui::GetColorU32(ImGuiCol_TextDisabled), key);
      }
      if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("KAMEO_NODE", &k, sizeof(k));
        ImGui::TextUnformatted(Tr(info.name));
        ImGui::EndDragDropSource();
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        std::string tip = Tr(info.help);
        if (info.hotkey) tip += "\n\n" + TrFormat("Hold {0} and click the graph to place one.", {std::string_view(&info.hotkey, 1)});
        ImGui::SetTooltip("%s", tip.c_str());
      }
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
      ImGui::SeparatorText(Tr("Comment"));
      ImGui::SetNextItemWidth(-1);
      InputLine("##comment_text", c.text);
      ImGui::ColorEdit3(Tr("Colour"), c.colour.data());
      ImGui::DragFloat2(Tr("Size"), &c.w, 1.0f, 120.0f, 4000.0f);
    }
    return;
  }
  if (canvas_.selected().size() != 1) {
    ImGui::SeparatorText(Tr("Details"));
    if (canvas_.selected().empty()) ImGui::TextDisabled("%s", Tr("Select a node to edit it."));
    else ImGui::TextDisabled("%s", TrFormat("{0} nodes selected.", {std::to_string(canvas_.selected().size())}).c_str());
    return;
  }
  Node* n = g.Find(canvas_.selected()[0]);
  if (!n) return;
  const NodeInfo& info = Info(n->kind);
  ImGui::SeparatorText(Tr(info.name));
  ImGui::PushTextWrapPos(0);
  ImGui::TextDisabled("%s", Tr(info.help));
  ImGui::PopTextWrapPos();
  ImGui::PushID(n->id);
  ImGui::PushItemWidth(-90);
  if (n->kind == NodeKind::kFloat || n->kind == NodeKind::kColor) {
    InputLine(Tr("Parameter"), n->name);
    ImGui::SetItemTooltip("%s", Tr("Named constants are listed under Parameters, to tune without finding them in the graph"));
  }
  if (n->kind == NodeKind::kColor) {
    ImGui::ColorEdit3(Tr("Colour"), n->settings.data(), ImGuiColorEditFlags_Float);
  } else if (n->kind == NodeKind::kComponentMask) {
    int channel = std::clamp(int(n->settings[0]), 0, 2);
    if (ImGui::Combo(Tr("Channel"), &channel, "R (X)\0G (Y)\0B (Z)\0")) n->settings[0] = float(channel);
  } else if (n->kind == NodeKind::kGameTexture) {
    const std::optional<GameTextureRef> texture = ParseGameTexture(n->code);
    if (texture) {
      // On a light square, as many of the game's textures are dark shapes cut out by their alpha.
      const ImVec2 at = ImGui::GetCursorScreenPos();
      ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + 96, at.y + 96), IM_COL32(190, 190, 196, 255), 4.0f);
      if (ID3D11ShaderResourceView* view = game_textures_.View(*texture)) ImGui::Image(ImTextureRef(ImTextureID(view)), ImVec2(96, 96));
      else ImGui::Dummy(ImVec2(96, 96));
      ImGui::SameLine();
      ImGui::BeginGroup();
      ImGui::TextUnformatted(texture->label.c_str());
      ImGui::TextDisabled("%s", TrFormat("{0}, {1} x {2}", {texture->archive, std::to_string(texture->width), std::to_string(texture->height)}).c_str());
      ImGui::EndGroup();
      const std::vector<GameTextureRef> used = GameTextures(g);
      if (std::find(used.begin(), used.end(), *texture) == used.end()) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", Tr("A mod can draw with four of the game's textures; this fifth one reads nothing."));
    } else {
      ImGui::TextDisabled("%s", Tr("No texture chosen yet."));
    }
    if (ImGui::Button(Tr("Choose a texture..."))) picking_node_ = n->id;
    int read = !n->settings.empty() && n->settings[0] >= 0.5f;
    const std::string reads = std::string(Tr("Its colour")) + '\0' + Tr("Its alpha") + '\0';
    if (ImGui::Combo(Tr("Reads"), &read, reads.c_str()) && !n->settings.empty()) n->settings[0] = float(read);
  } else {
    for (size_t s = 0; s < n->settings.size() && s < info.settings.size(); ++s) ImGui::DragFloat(Tr(info.settings[s]), &n->settings[s], 0.01f);
  }
  bool header = false;
  for (size_t i = 0; i < n->inputs.size() && i < info.inputs.size(); ++i) {
    if (g.Into(n->id, int(i))) continue;
    if (!header) ImGui::SeparatorText(Tr("Unwired inputs")), header = true;
    ImGui::PushID(int(i));
    if (i == 0 && info.uv_input) {
      ImGui::TextDisabled("%s", TrFormat("{0}: the surface's UV", {Tr(info.inputs[i])}).c_str());
    } else {
      EditValue(Tr(info.inputs[i]), n->inputs[i], std::string_view(info.inputs[i]) == "Colour");
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
// Any object of the game's levels, then one of its textures; the node takes it.
void Editor::DrawTexturePicker() {
  if (!picking_node_) return;
  Node* node = mod_.graph.Find(picking_node_);
  if (!node || node->kind != NodeKind::kGameTexture) {
    picking_node_ = 0;
    return;
  }
  game_textures_.StartIndex();
  ImGui::SetNextWindowSize(ImVec2(760, 540), ImGuiCond_FirstUseEver);
  bool open = true;
  if (ImGui::Begin(Tr("Choose a game texture"), &open, ImGuiWindowFlags_NoCollapse)) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", Tr("Find what wears it: an enemy, a character, a prop. The mod names the texture, and the game reads it from your own files, so the mod holds none of the game's pictures."));
    ImGui::PopTextWrapPos();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##search", Tr("Search, such as ShadowTroll"), picker_search_.data(), picker_search_.capacity() + 1, ImGuiInputTextFlags_CallbackResize, ResizeString, &picker_search_);
    if (game_textures_.Indexing()) ImGui::TextDisabled("%s", TrFormat("Reading the game's levels... {0} of {1} (once only)", {std::to_string(game_textures_.IndexedFiles()), std::to_string(game_textures_.FilesToIndex())}).c_str());
    ImGui::BeginChild("types", ImVec2(260, 0), ImGuiChildFlags_Borders);
    for (const GameTextureLibrary::Found& found : game_textures_.Find(picker_search_, 500)) {
      const std::string label = found.type + "##" + found.archive;
      if (ImGui::Selectable(label.c_str(), found.type == picker_type_ && found.archive == picker_archive_)) picker_type_ = found.type, picker_archive_ = found.archive;
      ImGui::SetItemTooltip("%s", found.archive.c_str());
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("textures", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (picker_type_.empty()) {
      ImGui::TextDisabled("%s", Tr("Choose something on the left."));
    } else if (const std::vector<GameTextureRef>* textures = game_textures_.TexturesOf(picker_archive_, picker_type_); !textures) {
      ImGui::TextDisabled("%s", Tr("Reading..."));
    } else if (textures->empty()) {
      ImGui::TextDisabled("%s", Tr("It has no textures a mod can draw with."));
    } else {
      constexpr float kThumb = 128.0f;
      const float room = ImGui::GetContentRegionAvail().x;
      float x = 0;
      for (const GameTextureRef& texture : *textures) {
        ImGui::PushID(int(texture.entry));
        if (x > 0 && x + kThumb + 8 <= room) ImGui::SameLine();
        else x = 0;
        bool chosen = false;
        if (ID3D11ShaderResourceView* view = game_textures_.View(texture)) chosen = ImGui::ImageButton("##texture", ImTextureRef(ImTextureID(view)), ImVec2(kThumb, kThumb), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0.75f, 0.75f, 0.77f, 1.0f));
        else chosen = ImGui::Button("...", ImVec2(kThumb + 8, kThumb + 8));
        ImGui::SetItemTooltip("%s", TrFormat("{0} x {1}, entry {2} of {3}", {std::to_string(texture.width), std::to_string(texture.height), std::to_string(texture.entry), texture.archive}).c_str());
        if (chosen) {
          node->code = FormatGameTexture(texture);
          picking_node_ = 0;
        }
        x += kThumb + 8 + ImGui::GetStyle().ItemSpacing.x;
        ImGui::PopID();
      }
    }
    ImGui::EndChild();
  }
  ImGui::End();
  if (!open) picking_node_ = 0;
}

void Editor::DrawParameters() {
  ImGui::SeparatorText(Tr("Parameters"));
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
  if (!any) ImGui::TextDisabled("%s", Tr("Name a Number or Colour node to list it here."));
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
  ImGui::SeparatorText(Tr("Applies to"));
  if (!ready_) {
    ImGui::TextDisabled("%s", Tr("Preparing the game's shaders..."));
    return;
  }
  TargetBox(source_.all_characters());
  TargetBox(source_.everything());
  if (ImGui::TreeNodeEx(Tr("Characters"), ImGuiTreeNodeFlags_DefaultOpen)) {
    for (const Target& target : source_.characters()) TargetBox(target);
    ImGui::TreePop();
  }
  if (!source_.surfaces().empty() && ImGui::TreeNodeEx(Tr("Surfaces"), ImGuiTreeNodeFlags_DefaultOpen)) {
    for (const Target& target : source_.surfaces()) TargetBox(target);
    ImGui::TreePop();
  }
  if (ImGui::TreeNode(Tr("Levels"))) {
    for (const Target& target : source_.levels()) TargetBox(target);
    ImGui::TreePop();
  }
  ImGui::Spacing();
  if (source_.NeedsUnlit(mod_.targets)) {
    ImGui::BeginDisabled();
    bool always = true;
    ImGui::Checkbox(Tr("Also surfaces the game doesn't light"), &always);
    ImGui::EndDisabled();
  } else {
    ImGui::Checkbox(Tr("Also surfaces the game doesn't light"), &mod_.unlit);
  }
  ImGui::SetItemTooltip("%s", Tr("Glows, effects and surfaces lit from within, such as lava. Their Game colour already has fog in it, and Base texture, Light and Normal aren't known."));
  ImGui::PushTextWrapPos(0);
  const std::string shaders = std::to_string(source_.Shaders(mod_.targets).size());
  if (source_.Meshes(mod_.targets).empty()) ImGui::TextDisabled("%s", TrFormat("{0} shaders. These choose the game's shaders, not objects, so anything else drawn with the same ones changes too.", {shaders}).c_str());
  else ImGui::TextDisabled("%s", TrFormat("{0} shaders, on this model alone: anything else drawn with them keeps the game's look.", {shaders}).c_str());
  ImGui::PopTextWrapPos();
}

void Editor::DrawLog() {
  ImGui::SeparatorText(Tr("Compile log"));
  ImGui::PushTextWrapPos(0);
  if (build_.valid()) ImGui::TextUnformatted(TrFormat("Compiling... {0} of {1}", {std::to_string(build_done_.load()), std::to_string(build_total_)}).c_str());
  else if (log_.empty()) ImGui::TextDisabled("%s", Tr("Save, or Live Update, compiles the shaders. Run the game to see them."));
  ImGui::PushStyleColor(ImGuiCol_Text, log_ok_ ? IM_COL32(150, 220, 150, 255) : IM_COL32(255, 120, 110, 255));
  ImGui::TextUnformatted(log_.c_str());
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
}

void Editor::Draw(ID3D11Device* device, ID3D11DeviceContext* context) {
  if (!preview_attached_) {
    preview_.Attach(device, context);
    game_textures_.Attach(device, context);
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
    ImGui::TextDisabled("%s", Tr("float3 Shade(ShadeInputs s) {  ... the body below ...  }   Helpers: ModKeyLight, ModStep, ModBands, ModFresnel, ModOutline, ModHalftone, ModLuminance, ModNoise (docs/modding.md)."));
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
  std::array<ID3D11ShaderResourceView*, kGameTextureSlots> views{};
  const std::vector<GameTextureRef> textures = GameTextures(g);
  for (size_t i = 0; i < textures.size() && i < views.size(); ++i) views[i] = game_textures_.View(textures[i]);
  preview_.SetGameTextures(views);
  preview_.Draw();
  ImGui::EndChild();
  ImGui::BeginChild("targets", ImVec2(0, height * 0.32f), ImGuiChildFlags_Borders);
  DrawTargets();
  ImGui::EndChild();
  ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders);
  DrawLog();
  ImGui::EndChild();
  ImGui::EndChild();
  DrawTexturePicker();
  Changed();
  Remember();
  if (live_ && live_pending_ && ready_ && !build_.valid() && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && std::chrono::steady_clock::now() - edited_at_ > kLiveDelay) {
    live_pending_ = false;
    if (source_.Shaders(mod_.targets).size() <= kLiveLimit) Build(false);
  }
}

}  // namespace kameo::shader_editor
