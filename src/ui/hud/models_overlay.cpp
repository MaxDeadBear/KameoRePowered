#include "ui/hud/models_overlay.h"

#ifdef _WIN32

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <rex/logging/log_category.h>
#include <rex/platform/file.h>
#include <rex/ui/dialog.h>
#include <rex/ui/ui_system.h>
#include <rex/utility.h>

#include "engine/dlc/dlc_swap.h"
#include "engine/dlc/mod_costumes.h"

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kKameoModelType = 0x007183BF;

std::string Utf8(const fs::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

fs::path FromUtf8(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }

std::string Lower(std::string text) {
  for (char& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

std::wstring Quoted(const fs::path& path) { return L"\"" + path.wstring() + L"\""; }

// Runs a command with no console window and gathers what it prints. The exit code, or -1 when it could not start.
int RunProcess(std::wstring command, const fs::path& folder, std::string& output) {
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  HANDLE read = nullptr, write = nullptr;
  if (!CreatePipe(&read, &write, &security, 0)) return -1;
  SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write;
  startup.hStdError = write;
  PROCESS_INFORMATION process{};
  const std::wstring working = folder.wstring();
  const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, working.empty() ? nullptr : working.c_str(), &startup, &process);
  CloseHandle(write);
  if (!started) {
    CloseHandle(read);
    return -1;
  }
  char buffer[4096];
  DWORD got = 0;
  while (ReadFile(read, buffer, sizeof(buffer), &got, nullptr) && got) output.append(buffer, got);
  CloseHandle(read);
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hProcess);
  CloseHandle(process.hThread);
  return int(code);
}

// The last few lines of what a tool printed, for the window.
std::string Tail(const std::string& text, size_t max = 700) {
  std::string out = text;
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
  if (out.size() > max) out = "..." + out.substr(out.size() - max);
  return out;
}

// The folder that holds kameo_modkit: modkit\ beside the executable (a release), else tools\modkit of the repository
// the executable was built in.
fs::path FindModkit() {
  std::error_code ec;
  const fs::path exe = rex::platform::GetExecutableFolder();
  if (fs::is_directory(exe / "modkit" / "kameo_modkit", ec)) return exe / "modkit";
  for (fs::path at = exe; !at.empty(); at = at.parent_path()) {
    if (fs::is_directory(at / "tools" / "modkit" / "kameo_modkit", ec)) return at / "tools" / "modkit";
    if (at == at.parent_path()) break;
  }
  return {};
}

}  // namespace

struct KameoModelsOverlay::State {
  struct Entry {
    std::string status;
    bool error = false;
    bool busy = false;
    fs::path imported;  // the file last imported this session
    bool watch = false;
    fs::file_time_type stamp{};
  };

  fs::path game_root;

  std::mutex mutex;  // everything below, shared with the worker threads
  std::map<std::string, Entry> entries;
  std::wstring python;  // "py -3" or "python" once one has been found to work
  std::string tool_problem;

  // UI thread only.
  char new_name[64] = {};
  std::vector<std::string> bases;
  int base = 0;
  bool bases_read = false;
  std::string new_status;
  bool new_error = false;
  double last_watch = 0;
  fs::path last_folder;

  void Set(const std::string& model, std::string text, bool error, bool busy) {
    std::lock_guard lock(mutex);
    Entry& entry = entries[model];
    entry.status = std::move(text);
    entry.error = error;
    entry.busy = busy;
  }

  // Finds Python 3.10 or newer with NumPy, once; the reason when there is none.
  bool FindPython(std::wstring& python_out, std::string& why) {
    {
      std::lock_guard lock(mutex);
      if (!python.empty()) {
        python_out = python;
        return true;
      }
    }
    bool too_old = false, no_numpy = false;
    for (const wchar_t* launcher : {L"py -3", L"python"}) {
      std::string text;
      const std::wstring base_command = launcher;
      const int version = RunProcess(base_command + L" -c \"import sys; sys.exit(0 if sys.version_info >= (3, 10) else 3)\"", {}, text);
      if (version == 3) too_old = true;
      if (version != 0) continue;
      text.clear();
      if (RunProcess(base_command + L" -c \"import numpy\"", {}, text) != 0) {
        no_numpy = true;
        continue;
      }
      std::lock_guard lock(mutex);
      python = base_command;
      python_out = python;
      return true;
    }
    if (no_numpy) why = "NumPy is not installed for Python. Open a command prompt and run: py -m pip install numpy";
    else if (too_old) why = "Python is older than 3.10. Install Python 3.10 or newer from python.org, then run: py -m pip install numpy";
    else why = "Python was not found. glTF files need Python 3.10 or newer (python.org, tick \"Add python.exe to PATH\") and NumPy (run: py -m pip install numpy). Importing an .mdl works without them.";
    return false;
  }

  // python -m kameo_modkit <args>; true on success. `message` is what it printed, or why it could not run.
  bool RunModkit(const std::vector<fs::path>& args, std::string& message) {
    std::wstring python_command;
    if (!FindPython(python_command, message)) return false;
    const fs::path modkit = FindModkit();
    if (modkit.empty()) {
      message = "The modkit was not found. It should be the folder modkit\\kameo_modkit beside kameorepowered.exe (or tools\\modkit\\kameo_modkit in the source tree).";
      return false;
    }
    std::wstring command = python_command + L" -m kameo_modkit";
    for (const fs::path& arg : args) command += L" " + Quoted(arg);
    std::string output;
    const int code = RunProcess(command, modkit, output);
    if (code != 0) {
      message = code < 0 ? "Python could not be started." : "The modkit failed: " + Tail(output);
      return false;
    }
    message = Tail(output);
    return true;
  }
};

namespace {

using State = KameoModelsOverlay::State;

// Puts a model file on a costume: an .mdl is copied over its file, a glTF is converted with the costume's current model
// as the template. It is written beside the costume first and then moved over it, so the watcher never reads half a file.
void DoImport(State& st, const std::string& model, const fs::path& source) {
  const fs::path folder = kameo::engine::ModCostumeFolder();
  const fs::path dest = folder / (model + ".mdl");
  const fs::path temp = folder / (model + ".import.mdl");
  std::error_code ec;
  if (!fs::exists(source, ec)) {
    st.Set(model, "The file is gone: " + Utf8(source), true, false);
    return;
  }
  const std::string ext = Lower(Utf8(source.extension()));
  if (ext == ".mdl") {
    if (fs::equivalent(source, dest, ec)) {
      st.Set(model, "That is the costume's own file already.", true, false);
      return;
    }
    fs::copy_file(source, temp, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      st.Set(model, "Could not copy the file: " + ec.message(), true, false);
      return;
    }
  } else if (ext == ".gltf" || ext == ".glb") {
    std::string message;
    fs::remove(temp, ec);
    // A file made from another of the character's models is built on that model, found among the game's own.
    if (!st.RunModkit({"import", "--try-models", st.game_root, dest, source, temp}, message)) {
      fs::remove(temp, ec);
      st.Set(model, message, true, false);
      return;
    }
  } else {
    st.Set(model, "Choose an .mdl, .gltf or .glb file.", true, false);
    return;
  }
  const auto size = fs::file_size(temp, ec);
  if (ec || size == 0) {
    fs::remove(temp, ec);
    st.Set(model, "The conversion wrote no model.", true, false);
    return;
  }
  fs::rename(temp, dest, ec);
  if (ec) {
    fs::remove(temp, ec);
    st.Set(model, "Could not replace the costume's file: " + ec.message(), true, false);
    return;
  }
  REXLOG_INFO("[kameo] models: {} imported from {}", model, Utf8(source));
  st.Set(model, "Imported " + Utf8(source.filename()) + ". The game reloads it on the character within a couple of seconds.", false, false);
}

void StartImport(const std::shared_ptr<State>& st, const std::string& model, const fs::path& source) {
  {
    std::lock_guard lock(st->mutex);
    State::Entry& entry = st->entries[model];
    entry.busy = true;
    entry.status = "Importing...";
    entry.error = false;
    entry.imported = source;
    std::error_code ec;
    entry.stamp = fs::last_write_time(source, ec);
  }
  std::thread([st, model, source] { DoImport(*st, model, source); }).detach();
}

// The file dialog, blocking; false when it is not available. `chosen` stays empty when the player cancels.
bool Pick(const rex::ui::FileDialogConfig& config, fs::path& chosen) {
  rex::ui::IUISystem* ui = REX_INTERFACE(rex::ui::IUISystem);
  if (!ui) return false;
  auto picked = ui->ShowFileDialog(config);
  if (!picked) return false;
  if (!picked->empty()) chosen = picked->front();
  return true;
}

void StartPickImport(const std::shared_ptr<State>& st, const std::string& model, fs::path start) {
  st->Set(model, "Choose a file...", false, true);
  std::thread([st, model, start] {
    rex::ui::FileDialogConfig config;
    config.mode = rex::ui::FileDialogConfig::Mode::kOpenFile;
    config.title = "Import a model";
    config.filters = {{"Model files (.mdl, .gltf, .glb)", "mdl;gltf;glb"}, {"All files", "*"}};
    config.default_location = start;
    fs::path chosen;
    const bool available = Pick(config, chosen);
    if (chosen.empty()) {
      st->Set(model, available ? "Canceled." : "The file dialog is not available.", !available, false);
      return;
    }
    {
      std::lock_guard lock(st->mutex);
      State::Entry& entry = st->entries[model];
      entry.imported = chosen;
      entry.busy = true;
      entry.status = "Importing...";
      entry.error = false;
      std::error_code ec;
      entry.stamp = fs::last_write_time(chosen, ec);
    }
    DoImport(*st, model, chosen);
  }).detach();
}

void StartExport(const std::shared_ptr<State>& st, const std::string& model, const std::string& name) {
  st->Set(model, "Choose where to save...", false, true);
  std::thread([st, model, name] {
    rex::ui::FileDialogConfig config;
    config.mode = rex::ui::FileDialogConfig::Mode::kSaveFile;
    config.title = "Export \"" + name + "\" to glTF (also writes a .bin and a textures folder beside it: pick an empty folder)";
    config.filters = {{"glTF (.gltf)", "gltf"}};
    fs::path out;
    const bool available = Pick(config, out);
    if (out.empty()) {
      st->Set(model, available ? "Canceled." : "The file dialog is not available.", !available, false);
      return;
    }
    if (Lower(Utf8(out.extension())) != ".gltf") out += ".gltf";
    std::string message;
    if (st->RunModkit({"gltf", kameo::engine::ModCostumeFolder() / (model + ".mdl"), out}, message)) {
      REXLOG_INFO("[kameo] models: {} exported to {}", model, Utf8(out));
      st->Set(model, "Exported to " + Utf8(out), false, false);
    } else {
      st->Set(model, message, true, false);
    }
  }).detach();
}

void ReadBases(State& st) {
  st.bases.clear();
  st.base = 0;
  std::error_code ec;
  char prefix[12];
  std::snprintf(prefix, sizeof(prefix), "%08X_", kKameoModelType);
  for (auto it = fs::directory_iterator(st.game_root, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
    const std::string name = Utf8(it->path().filename());
    if (it->is_regular_file(ec) && Lower(name).rfind(Lower(prefix), 0) == 0 && Lower(name).ends_with(".mdl")) st.bases.push_back(name);
  }
  std::sort(st.bases.begin(), st.bases.end());
}

std::string CharacterName(uint32_t model_type) {
  if (model_type == kKameoModelType) return "Kameo";
  char text[24];
  std::snprintf(text, sizeof(text), "model %08X", model_type);
  return text;
}

}  // namespace

KameoModelsOverlay::KameoModelsOverlay(fs::path game_root) : state_(std::make_shared<State>()) { state_->game_root = std::move(game_root); }

KameoModelsOverlay::~KameoModelsOverlay() = default;

void KameoModelsOverlay::OnDraw(ImGuiIO&) {
  State& st = *state_;
  const auto costumes = kameo::engine::ListModCostumes();

  // A file imported earlier is imported again when it is saved again, while the costume's box is ticked; hidden or not.
  const double now = ImGui::GetTime();
  if (now - st.last_watch >= 1.0) {
    st.last_watch = now;
    std::vector<std::pair<std::string, fs::path>> due;
    {
      std::lock_guard lock(st.mutex);
      for (auto& [model, entry] : st.entries) {
        if (!entry.watch || entry.busy || entry.imported.empty()) continue;
        std::error_code ec;
        const auto stamp = fs::last_write_time(entry.imported, ec);
        if (!ec && stamp != entry.stamp) due.emplace_back(model, entry.imported);
      }
    }
    for (const auto& [model, file] : due) StartImport(state_, model, file);
  }
  if (!visible_) return;

  ImGui::SetNextWindowSize(ImVec2(780, 560), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);
  bool open = true;
  if (ImGui::Begin("Models (F11)###kameo_models", &open, ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::TextWrapped("Put a model file on a costume and see it on the character. Costumes are the files in costumes\\ beside the game.");
    if (ImGui::Button("Standard look")) g_kameo_pending_standard_swap.store(1, std::memory_order_release);
    ImGui::SameLine();
    ImGui::TextDisabled("Takes Kameo out of any costume.");
    ImGui::Separator();

    if (costumes.empty()) ImGui::TextDisabled("No costumes yet. Make one below, then restart the game.");
    for (const auto& costume : costumes) {
      ImGui::PushID(costume.model.c_str());
      State::Entry entry;
      {
        std::lock_guard lock(st.mutex);
        entry = st.entries[costume.model];
      }
      ImGui::TextUnformatted(costume.name.c_str());
      ImGui::SameLine();
      ImGui::TextDisabled("%s   %s", CharacterName(costume.model_type).c_str(), costume.file.c_str());
      if (costume.worn) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "[worn now]");
      }
      if (!costume.listed) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.3f, 1), "[not listed by the game yet]");
      }

      ImGui::BeginDisabled(entry.busy);
      if (ImGui::Button("Wear")) QueueKameoDlcSuffix(std::string_view(costume.model).substr(9));
      ImGui::SameLine();
      if (ImGui::Button("Reload")) kameo::engine::RequestModCostumeReload(costume.model);
      ImGui::SameLine();
      if (ImGui::Button("Import...")) StartPickImport(state_, costume.model, entry.imported.empty() ? st.last_folder : entry.imported.parent_path());
      ImGui::SameLine();
      if (ImGui::Button("Export glTF...")) StartExport(state_, costume.model, costume.name);
      ImGui::EndDisabled();

      if (!entry.imported.empty()) {
        bool watch = entry.watch;
        if (ImGui::Checkbox("Reload when the file changes", &watch)) {
          std::lock_guard lock(st.mutex);
          st.entries[costume.model].watch = watch;
          std::error_code ec;
          if (watch) st.entries[costume.model].stamp = fs::last_write_time(entry.imported, ec);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", Utf8(entry.imported).c_str());
        if (!entry.imported.parent_path().empty()) st.last_folder = entry.imported.parent_path();
      }
      if (!entry.status.empty()) {
        if (entry.error) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.45f, 1));
        ImGui::TextWrapped("%s", entry.status.c_str());
        if (entry.error) ImGui::PopStyleColor();
      }
      ImGui::Separator();
      ImGui::PopID();
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("New costume...", ImGuiTreeNodeFlags_DefaultOpen)) {
      if (!st.bases_read) {
        st.bases_read = true;
        ReadBases(st);
      }
      ImGui::SetNextItemWidth(260);
      ImGui::InputText("Name", st.new_name, sizeof(st.new_name));
      ImGui::SetNextItemWidth(260);
      const char* shown = st.bases.empty() ? "(no Kameo skins found)" : st.bases[size_t(st.base)].c_str();
      if (ImGui::BeginCombo("Based on", shown)) {
        for (size_t i = 0; i < st.bases.size(); ++i) {
          if (ImGui::Selectable(st.bases[i].c_str(), int(i) == st.base)) st.base = int(i);
        }
        ImGui::EndCombo();
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Refresh")) ReadBases(st);
      ImGui::BeginDisabled(st.bases.empty() || st.new_name[0] == 0);
      if (ImGui::Button("Create")) {
        std::string file;
        const std::string error = kameo::engine::CreateModCostume(st.game_root / FromUtf8(st.bases[size_t(st.base)]), st.new_name, &file);
        st.new_error = !error.empty();
        if (error.empty()) {
          st.new_status = "Made " + file + ". The game lists a new costume only after a restart: close it and start it again, then it shows here and on the Bonus pages.";
          st.new_name[0] = 0;
          REXLOG_INFO("[kameo] models: new costume {}", file);
        } else {
          st.new_status = error;
        }
      }
      ImGui::EndDisabled();
      if (!st.new_status.empty()) {
        if (st.new_error) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.45f, 1));
        ImGui::TextWrapped("%s", st.new_status.c_str());
        if (st.new_error) ImGui::PopStyleColor();
      }
    }
  }
  ImGui::End();
  if (!open) visible_ = false;
}

#endif
