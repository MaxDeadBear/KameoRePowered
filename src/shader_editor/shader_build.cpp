#include "shader_editor/shader_build.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <future>
#include <map>
#include <set>
#include <string_view>
#include <thread>

#include <nlohmann/json.hpp>

#include "gpu/shaders/shader_mods.h"
#include "launcher/zip_extract.h"

namespace fs = std::filesystem;

namespace kameo::shader_editor {

namespace {

constexpr char kToolsFolder[] = "editor_shaders";
constexpr char kSourcePack[] = "source.zip";
constexpr char kDoneMark[] = "unpacked";
constexpr char kShadeFile[] = "shade.hlsl";

// The model files, by the names they are hashed from, as players know them.
struct Character {
  const char* key;
  const char* label;
};
constexpr Character kCharacters[] = {{"kameo", "Kameo"},           {"flowerboxer", "Pummel Weed"}, {"rubble", "Rubble"},     {"armadillo", "Major Ruin"},
                                     {"firedragon", "Ash"},        {"fire_ant", "Thermite"},       {"spitter", "Snare"},     {"watercannon", "Flex"},
                                     {"watercreature_small", "Deep Blue"}, {"ice_b_yeti", "Chilla"}, {"snow_man", "40 Below"}};

std::vector<uint8_t> ReadAll(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}

// Runs a command line with no console window, its output to `log`; the exit code, or -1 when it could not start.
int RunHidden(const std::wstring& command, const fs::path& log) {
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE out = CreateFileW(log.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = out;
  si.hStdError = out;
  PROCESS_INFORMATION pi{};
  std::wstring line = command;
  const BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
  if (!started) return -1;
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return int(code);
}

}  // namespace

fs::path EditorFolder() {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  return fs::path(exe).parent_path();
}

fs::path ModsFolder() { return EditorFolder() / "shader_mods"; }

bool GameUsesVulkan() {
  std::ifstream in(EditorFolder() / "kameo_settings.toml");
  std::string line;
  while (std::getline(in, line)) {
    if (line.starts_with("kameo_gfx_backend")) return line.find("vulkan") != std::string::npos;
  }
  return false;
}

bool ShaderSource::Prepare(std::string& error) {
  const fs::path pack = EditorFolder() / kToolsFolder / kSourcePack;
  std::error_code ec;
  const auto size = fs::file_size(pack, ec);
  if (ec) {
    error = std::format("The game's shaders are missing from {}; reinstall the game.", pack.parent_path().string());
    return false;
  }
  // By the pack's size and time, so a new build unpacks its own.
  const uint64_t stamp = uint64_t(fs::last_write_time(pack, ec).time_since_epoch().count());
  folder_ = fs::temp_directory_path() / std::format("KameoShaderSource_{:x}_{:x}", size, stamp);
  if (!fs::exists(folder_ / kDoneMark)) {
    fs::remove_all(folder_, ec);
    if (!launcher::ExtractZip(pack, folder_, error)) return false;
    std::ofstream(folder_ / kDoneMark) << "done";
  }

  std::ifstream in(folder_ / "targets.json");
  const nlohmann::json table = nlohmann::json::parse(in, nullptr, false);
  if (table.is_discarded()) {
    error = "The game's shader list could not be read.";
    return false;
  }
  std::map<std::string, std::vector<std::string>> shaders = table.get<std::map<std::string, std::vector<std::string>>>();
  meshes_.clear();
  std::ifstream mesh_file(folder_ / "meshes.json");
  const nlohmann::json mesh_table = nlohmann::json::parse(mesh_file, nullptr, false);
  if (!mesh_table.is_discarded()) {
    for (const auto& [key, list] : mesh_table.items()) {
      for (const std::string& mesh : list) meshes_[key].push_back(std::stoull(mesh, nullptr, 16));
    }
  }
  // A character's standard look alone, which the game shows as a costume of its own, so that look stays as it was.
  costume_models_.clear();
  std::ifstream costume_file(folder_ / "costumes.json");
  const nlohmann::json costume_table = nlohmann::json::parse(costume_file, nullptr, false);
  if (!costume_table.is_discarded()) {
    for (const auto& [key, model] : costume_table.items()) costume_models_[key] = uint32_t(std::stoul(model.get<std::string>(), nullptr, 16));
  }
  // Older packs listed only Kameo's.
  constexpr char kEveryday[] = "kameo_everyday";
  if (costume_models_.empty() && meshes_.contains(kEveryday)) costume_models_[kEveryday] = 0x007183BF;
  std::set<std::string> models, all;
  for (const auto& [key, list] : shaders) all.insert(list.begin(), list.end());
  characters_.clear();
  levels_.clear();
  for (const Character& c : kCharacters) {
    const auto found = shaders.find(c.key);
    if (found == shaders.end()) continue;
    characters_.push_back({c.key, c.label, found->second});
    models.insert(found->second.begin(), found->second.end());
    shaders.erase(found);
    const std::string costume = c.key == std::string_view("kameo") ? kEveryday : std::string(c.key) + "_standard";
    if (const auto look = shaders.find(costume); look != shaders.end()) {
      if (costume_models_.contains(costume)) {
        characters_.push_back({costume, std::format("{}: new costume ({})", c.label, c.key == std::string_view("kameo") ? "everyday outfit" : "standard look"), look->second});
      }
      shaders.erase(look);
    }
  }
  for (const auto& [key, list] : shaders) levels_.push_back({key, key == "LevelRequiredObjects" ? "Shared objects" : key, list});
  all_characters_ = {"characters", "All characters", {models.begin(), models.end()}};
  everything_ = {"everything", "Everything in the game", {all.begin(), all.end()}};
  return true;
}

std::vector<std::string> ShaderSource::Shaders(const std::vector<std::string>& keys) const {
  std::set<std::string> shaders;
  const auto add = [&](const Target& target) {
    if (std::find(keys.begin(), keys.end(), target.key) != keys.end()) shaders.insert(target.shaders.begin(), target.shaders.end());
  };
  add(everything_);
  add(all_characters_);
  for (const Target& target : characters_) add(target);
  for (const Target& target : levels_) add(target);
  return {shaders.begin(), shaders.end()};
}

std::vector<uint64_t> ShaderSource::Meshes(const std::vector<std::string>& keys) const {
  if (keys.size() != 1 || !costume_models_.contains(keys[0])) return {};
  const auto found = meshes_.find(keys[0]);
  return found == meshes_.end() ? std::vector<uint64_t>{} : found->second;
}

uint32_t ShaderSource::CostumeModel(const std::vector<std::string>& keys) const {
  if (keys.size() != 1) return 0;
  const auto found = costume_models_.find(keys[0]);
  return found == costume_models_.end() ? 0 : found->second;
}

bool BuildMod(const ShaderSource& source, const BuildRequest& request, std::string& log, std::atomic<size_t>* done) {
  const fs::path tools = EditorFolder() / kToolsFolder;
  const fs::path dxc = tools / "dxc.exe";
  if (!fs::exists(dxc)) {
    log = std::format("The shader compiler is missing from {}; reinstall the game.", tools.string());
    return false;
  }
  if (request.shaders.empty()) {
    log = "Choose something for the mod to apply to.";
    return false;
  }
  const fs::path work = fs::temp_directory_path() / std::format("KameoShaderBuild_{}", GetCurrentProcessId());
  std::error_code ec;
  fs::create_directories(work, ec);
  // The work folder goes however the build ends.
  struct Cleanup {
    fs::path dir;
    ~Cleanup() {
      std::error_code e;
      fs::remove_all(dir, e);
    }
  } cleanup{work};
  const fs::path shade = work / kShadeFile;
  std::ofstream(shade, std::ios::trunc) << request.shade;

  struct Job {
    size_t shader;
    bool spirv;
  };
  std::vector<Job> jobs;
  for (size_t s = 0; s < request.shaders.size(); ++s) {
    if (request.dxil) jobs.push_back({s, false});
    if (request.spirv) jobs.push_back({s, true});
  }
  const auto output = [&](const Job& j) { return work / std::format("{}.{}", request.shaders[j.shader], j.spirv ? "spv" : "dxil"); };
  const auto log_of = [&](const Job& j) { return fs::path(output(j)) += ".log"; };
  // With the flags the game's own shaders are built with, so they share its pipeline layout.
  const auto run = [&](const Job& j) {
    const std::wstring input = (source.folder() / (request.shaders[j.shader] + ".hlsl")).wstring();
    const std::wstring command = std::format(L"\"{}\" -T ps_6_0 -Fo \"{}\" \"{}\" -I \"{}\" -HV 2021 -all-resources-bound -Qstrip_debug {}", dxc.wstring(), output(j).wstring(), input, work.wstring(), j.spirv ? L"-spirv -fvk-use-dx-layout" : L"-Wno-ignored-attributes -Qstrip_reflect");
    const int code = RunHidden(command, log_of(j));
    if (done) ++*done;
    return code;
  };
  // DXC's messages name the files, and a mod's author only needs to see their own code's lines.
  const auto tidy = [&](const std::vector<uint8_t>& bytes) {
    std::string text(bytes.begin(), bytes.end());
    const std::string path = shade.string();
    for (size_t at = text.find(path); at != std::string::npos; at = text.find(path)) text.replace(at, path.size(), "shader");
    return text;
  };

  // The first one alone, so a mistake in the graph or the HLSL is reported once, not for every shader.
  if (run(jobs[0]) != 0) {
    log = tidy(ReadAll(log_of(jobs[0])));
    if (log.empty()) log = "The shader compiler did not start.";
    return false;
  }
  std::atomic<size_t> next = 1;
  std::atomic<bool> failed = false;
  std::vector<std::future<void>> workers;
  const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 4u, 14u) - 2;
  for (unsigned t = 0; t < threads; ++t) {
    workers.push_back(std::async(std::launch::async, [&] {
      for (size_t i = next++; i < jobs.size() && !failed; i = next++)
        if (run(jobs[i]) != 0) failed = true;
    }));
  }
  for (auto& w : workers) w.wait();
  if (failed) {
    for (const Job& j : jobs) {
      if (fs::exists(output(j))) continue;
      const std::vector<uint8_t> text = ReadAll(log_of(j));
      if (text.empty()) continue;
      log = std::format("{} ({}): {}", request.shaders[j.shader], j.spirv ? "Vulkan" : "Direct3D 12", tidy(text));
      break;
    }
    if (log.empty()) log = "The shader compiler failed.";
    return false;
  }

  const size_t count = request.shaders.size();
  std::vector<gpu::ShaderModEntry> entries(count);
  std::vector<uint8_t> blobs;
  const size_t base = sizeof(gpu::ShaderModHeader) + count * sizeof(gpu::ShaderModEntry) + request.meshes.size() * sizeof(uint64_t);
  const auto append = [&](const Job& j, uint32_t& offset, uint32_t& size) {
    const std::vector<uint8_t> blob = ReadAll(output(j));
    offset = uint32_t(base + blobs.size());
    size = uint32_t(blob.size());
    blobs.insert(blobs.end(), blob.begin(), blob.end());
  };
  for (size_t s = 0; s < count; ++s) {
    entries[s].game_hash = std::stoull(request.shaders[s], nullptr, 16);
    if (request.dxil) append({s, false}, entries[s].dxil_offset, entries[s].dxil_size);
    if (request.spirv) append({s, true}, entries[s].spirv_offset, entries[s].spirv_size);
  }
  gpu::ShaderModHeader header{};
  std::memcpy(header.magic, "KSHD", sizeof(header.magic));
  header.version = 3;
  header.count = uint32_t(count);
  header.mesh_count = uint32_t(request.meshes.size());
  header.model_type = request.meshes.empty() ? 0 : request.model_type;

  fs::create_directories(request.out.parent_path(), ec);
  // Written beside and moved over, so the game's reload never reads half a file.
  fs::path partial = request.out;
  partial += ".partial";
  {
    std::ofstream f(partial, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(&header), sizeof(header));
    f.write(reinterpret_cast<const char*>(entries.data()), std::streamsize(count * sizeof(gpu::ShaderModEntry)));
    f.write(reinterpret_cast<const char*>(request.meshes.data()), std::streamsize(request.meshes.size() * sizeof(uint64_t)));
    f.write(reinterpret_cast<const char*>(blobs.data()), std::streamsize(blobs.size()));
    if (!f) {
      log = std::format("could not write {}", partial.string());
      return false;
    }
  }
  fs::rename(partial, request.out, ec);
  if (ec) {
    log = std::format("could not replace {}: {}", request.out.string(), ec.message());
    return false;
  }
  log = std::format("{} shaders written to {}", count, request.out.filename().string());
  return true;
}

}  // namespace kameo::shader_editor
