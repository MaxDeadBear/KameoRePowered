// Kameo RePowered shader editor: builds shader mods from a node graph or HLSL, for the running game to pick up.
#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <imgui.h>

#include "launcher/window.h"
#include "shader_editor/editor.h"
#include "shader_editor/shader_build.h"

using namespace kameo::launcher;
using namespace kameo::shader_editor;

namespace {

std::string Narrow(const std::wstring& text) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string out(size_t(std::max(size, 1)) - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), size, nullptr, nullptr);
  return out;
}

std::optional<ShaderGraph> GraphNamed(const std::filesystem::path& what, std::string& error) {
  for (int p = 0; p < int(Preset::kCount); ++p)
    if (what.string() == PresetName(Preset(p))) return MakePreset(Preset(p));
  std::optional<ShaderMod> mod = LoadMod(what, error);
  return mod ? std::optional<ShaderGraph>(std::move(mod->graph)) : std::nullopt;
}

// KameoShaderEditor --build <preset name or .kgraph> <targets, comma-separated> <out.kshader>; its result goes to <out.kshader>.log.
int BuildFromCommandLine(const std::vector<std::wstring>& arguments) {
  const std::filesystem::path out = arguments[3];
  std::string log;
  bool ok = false;
  ShaderSource source;
  if (std::optional<ShaderGraph> graph = GraphNamed(arguments[1], log); graph && source.Prepare(log)) {
    std::vector<std::string> targets;
    std::stringstream list(Narrow(arguments[2]));
    for (std::string target; std::getline(list, target, ',');) targets.push_back(target);
    ok = BuildMod(source, {.shade = GenerateHlsl(*graph), .shaders = source.Shaders(targets), .meshes = source.Meshes(targets), .model_type = source.CostumeModel(targets), .out = out}, log);
  }
  std::filesystem::path log_file = out;
  log_file += ".log";
  std::ofstream(log_file, std::ios::trunc) << log << "\n";
  return ok ? 0 : 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  int count = 0;
  wchar_t** raw = CommandLineToArgvW(GetCommandLineW(), &count);
  const std::vector<std::wstring> arguments(raw + std::min(count, 1), raw + count);
  LocalFree(raw);
  if (arguments.size() == 4 && arguments[0] == L"--build") return BuildFromCommandLine(arguments);

  WindowState window;
  const WindowOptions options = {.title = L"Kameo Shader Editor", .width = 1500, .height = 900, .min_width = 1100, .min_height = 640};
  Editor editor;
  const bool ran = RunWindow(instance, show, options, window, [&] {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("editor", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    editor.Draw(window.device, window.context);
    ImGui::End();
  });
  return ran ? 0 : 1;
}
