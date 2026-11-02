#include "ui/hud/shader_list_overlay.h"

#include <algorithm>
#include <cctype>
#include <format>

#include "core/localization.h"
#include "gpu/shader_list.h"

namespace {

// Frames since a shader was last drawn for it to still count as on screen.
constexpr uint32_t kOnScreenFrames = 30;

std::string Hex(uint64_t value) { return std::format("{:016X}", value); }

// Whether the filter, as hex, appears in the shader's hash or any of its textures'.
bool Matches(const kameo::gpu::ShaderListRow& row, const std::string& filter) {
  if (filter.empty()) return true;
  if (Hex(row.hash).find(filter) != std::string::npos) return true;
  return std::any_of(row.textures.begin(), row.textures.end(), [&](uint64_t texture) { return Hex(texture).find(filter) != std::string::npos; });
}

}  // namespace

KameoShaderListOverlay::KameoShaderListOverlay(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) { kameo::gpu::ShaderListSetRecording(true); }

KameoShaderListOverlay::~KameoShaderListOverlay() { kameo::gpu::ShaderListSetRecording(false); }

void KameoShaderListOverlay::OnDraw(ImGuiIO& /*io*/) {
  using namespace kameo::gpu;
  ImGui::SetNextWindowPos(ImVec2(20, 60), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(620, 560), ImGuiCond_FirstUseEver);
  // The ### id keeps the window's place the same whatever language titles it.
  const std::string title = std::string(kameo::l10n::Tr("Shaders (F7)")) + "###kameo_shader_list";
  if (!ImGui::Begin(title.c_str())) {
    ImGui::End();
    return;
  }
  std::vector<ShaderListRow> rows = ShaderListSnapshot();
  std::string filter = filter_;
  for (char& c : filter) c = char(std::toupper(static_cast<unsigned char>(c)));
  std::erase_if(rows, [&](const ShaderListRow& row) { return (on_screen_only_ && !row.hidden && row.frames_ago > kOnScreenFrames) || !Matches(row, filter); });
  std::sort(rows.begin(), rows.end(), [](const ShaderListRow& a, const ShaderListRow& b) { return a.draws != b.draws ? a.draws > b.draws : a.hash < b.hash; });

  ImGui::TextWrapped("%s", kameo::l10n::Tr("Switch a shader off to see what it draws vanish. Only this switches off everything else listed. A shader's hash is what a shader mod targets."));
  ImGui::SetNextItemWidth(220);
  ImGui::InputTextWithHint("##filter", kameo::l10n::Tr("Filter by shader or texture hash"), filter_, sizeof(filter_));
  ImGui::SameLine();
  ImGui::Checkbox(kameo::l10n::Tr("On screen only"), &on_screen_only_);
  if (ImGui::Button(kameo::l10n::Tr("Show all"))) ShaderListShowAll();
  ImGui::SameLine();
  if (ImGui::Button(kameo::l10n::Tr("Switch off all listed"))) {
    for (const ShaderListRow& row : rows) ShaderListSetHidden(row.hash, true);
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%s", kameo::l10n::TrFormat("{0} shaders", {std::to_string(rows.size())}).c_str());

  constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
  if (ImGui::BeginTable("shaders", 5, kFlags)) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(kameo::l10n::Tr("On"));
    ImGui::TableSetupColumn(kameo::l10n::Tr("Pixel shader"));
    ImGui::TableSetupColumn(kameo::l10n::Tr("Draws"));
    ImGui::TableSetupColumn(kameo::l10n::Tr("Textures"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("");
    ImGui::TableHeadersRow();
    for (const ShaderListRow& row : rows) {
      ImGui::PushID(Hex(row.hash).c_str());
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      bool on = !row.hidden;
      if (ImGui::Checkbox("##on", &on)) ShaderListSetHidden(row.hash, !on);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(Hex(row.hash).c_str());
      ImGui::TableNextColumn();
      if (row.hidden) ImGui::TextDisabled("%s", kameo::l10n::Tr("off"));
      else ImGui::Text("%u", row.draws);
      ImGui::TableNextColumn();
      if (row.textures.empty()) {
        ImGui::TextDisabled("%s", kameo::l10n::Tr("none seen"));
      } else {
        std::string text = Hex(row.textures[0]);
        if (row.textures.size() > 1) text += std::format(" +{}", row.textures.size() - 1);
        ImGui::TextUnformatted(text.c_str());
        if (ImGui::IsItemHovered() && row.textures.size() > 1) {
          std::string all;
          for (uint64_t texture : row.textures) all += Hex(texture) + "\n";
          ImGui::SetTooltip("%s", all.c_str());
        }
      }
      ImGui::TableNextColumn();
      if (ImGui::SmallButton(kameo::l10n::Tr("Only this"))) {
        for (const ShaderListRow& other : rows) ShaderListSetHidden(other.hash, other.hash != row.hash);
      }
      ImGui::SameLine();
      if (ImGui::SmallButton(kameo::l10n::Tr("Copy"))) ImGui::SetClipboardText(Hex(row.hash).c_str());
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::End();
}
