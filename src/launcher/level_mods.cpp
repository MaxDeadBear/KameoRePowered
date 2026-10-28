#include "launcher/level_mods.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

#include "launcher/kmod.h"

namespace kameo::launcher {
namespace {

namespace fs = std::filesystem;

constexpr const char* kOrderFile = "load_order.txt";
constexpr const char* kMergedLanguageDir = ".rex_lang_merged";

std::vector<uint8_t> ReadAll(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

fs::path FindAssets(const fs::path& start) {
  std::error_code ec;
  for (fs::path dir = start; !dir.empty(); dir = dir.parent_path()) {
    if (fs::exists(dir / "assets" / "default.xex", ec)) return dir / "assets";
    if (dir == dir.parent_path()) break;
  }
  return {};
}

std::string Stamp(const std::vector<LevelMod>& mods, const fs::path& assets) {
  std::ostringstream out;
  out << assets.string() << "\n";
  std::error_code ec;
  for (const LevelMod& mod : mods) {
    if (!mod.enabled) continue;
    out << mod.file.filename().string() << " " << fs::file_size(mod.file, ec) << " " << fs::last_write_time(mod.file, ec).time_since_epoch().count() << "\n";
  }
  return out.str();
}

bool Mirror(const fs::path& assets, const fs::path& data, std::string& error) {
  std::error_code ec;
  fs::create_directories(data, ec);
  for (auto it = fs::recursive_directory_iterator(assets, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    const fs::path relative = fs::relative(it->path(), assets, ec);
    if (relative.begin() != relative.end() && *relative.begin() == kMergedLanguageDir) {
      if (it->is_directory()) it.disable_recursion_pending();
      continue;
    }
    const fs::path target = data / relative;
    if (it->is_directory()) {
      fs::create_directories(target, ec);
      continue;
    }
    if (!::CreateHardLinkW(target.c_str(), it->path().c_str(), nullptr)) {
      error = "could not link " + relative.string() + " (error " + std::to_string(::GetLastError()) + ")";
      return false;
    }
  }
  if (ec) error = "could not read the game folder: " + ec.message();
  return !ec;
}

}  // namespace

void LevelMods::Load(const fs::path& launcher_folder) {
  launcher_folder_ = launcher_folder;
  folder_ = launcher_folder / "mods";
  assets_ = FindAssets(launcher_folder);
  std::error_code ec;
  fs::create_directories(folder_, ec);
  std::vector<std::pair<std::string, bool>> order;
  std::ifstream in(folder_ / kOrderFile);
  std::string line;
  while (std::getline(in, line)) {
    if (line.size() > 1 && (line[0] == '+' || line[0] == '-')) order.emplace_back(line.substr(1), line[0] == '+');
  }
  std::map<std::string, LevelMod> found;
  for (const auto& entry : fs::directory_iterator(folder_, ec)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".kmod") continue;
    LevelMod mod;
    mod.file = entry.path();
    kmod::Package package;
    if (kmod::read(ReadAll(entry.path()), package, mod.error)) {
      mod.name = package.manifest.name.empty() ? entry.path().stem().string() : package.manifest.name;
      mod.author = package.manifest.author, mod.version = package.manifest.version, mod.description = package.manifest.description;
      for (const kmod::FilePatch& f : package.files) mod.levels.push_back(f.name);
    } else {
      mod.name = entry.path().stem().string();
    }
    found[entry.path().filename().string()] = std::move(mod);
  }
  mods_.clear();
  for (const auto& [file, enabled] : order) {
    auto it = found.find(file);
    if (it == found.end()) continue;
    it->second.enabled = enabled && it->second.error.empty();
    mods_.push_back(std::move(it->second));
    found.erase(it);
  }
  for (auto& [file, mod] : found) mods_.push_back(std::move(mod));
}

void LevelMods::Save() const {
  std::ofstream out(folder_ / kOrderFile, std::ios::trunc);
  for (const LevelMod& mod : mods_) out << (mod.enabled ? '+' : '-') << mod.file.filename().string() << "\n";
}

bool LevelMods::AnyEnabled() const {
  return std::any_of(mods_.begin(), mods_.end(), [](const LevelMod& m) { return m.enabled; });
}

std::vector<std::string> LevelMods::Conflicts() const {
  std::map<std::string, std::vector<std::string>> changers;
  for (const LevelMod& mod : mods_) {
    if (!mod.enabled) continue;
    for (const std::string& level : mod.levels) changers[level].push_back(mod.name);
  }
  std::vector<std::string> out;
  for (const auto& [level, names] : changers) {
    if (names.size() < 2) continue;
    std::string text = level + " is changed by ";
    for (size_t i = 0; i < names.size(); ++i) text += (i ? (i + 1 == names.size() ? " and " : ", ") : "") + names[i];
    out.push_back(text + "; only " + names.back() + " (lowest in the list) is used for it.");
  }
  return out;
}

void LevelMods::Move(size_t index, int step) {
  const size_t other = size_t(int(index) + step);
  if (index < mods_.size() && other < mods_.size()) std::swap(mods_[index], mods_[other]);
}

bool LevelMods::Build(fs::path& data_root, std::string& error) {
  if (assets_.empty()) return error = "the game folder (assets with default.xex) was not found", false;
  std::map<std::string, kmod::FilePatch> patches;
  for (const LevelMod& mod : mods_) {
    if (!mod.enabled) continue;
    kmod::Package package;
    if (!kmod::read(ReadAll(mod.file), package, error)) return error = mod.name + ": " + error, false;
    for (kmod::FilePatch& f : package.files) patches[f.name] = std::move(f);
  }
  std::error_code ec;
  const std::string stamp = Stamp(mods_, assets_);
  fs::path root = (launcher_folder_.root_name() == assets_.root_name() ? launcher_folder_ : assets_.parent_path()) / "mods_build";
  const fs::path data = root / "assets";
  std::ifstream previous(root / "stamp.txt");
  const std::string built((std::istreambuf_iterator<char>(previous)), std::istreambuf_iterator<char>());
  if (built == stamp && fs::exists(data / "default.xex", ec)) return data_root = data, true;
  previous.close();
  // The old build is moved aside before it is deleted: Windows refuses the rename while the game has files in it open,
  // which leaves the build a running game reads whole, where deleting file by file would empty it under the game.
  if (fs::exists(data, ec)) {
    const fs::path old = root / "assets.old";
    fs::remove_all(old, ec);
    fs::rename(data, old, ec);
    if (ec) return error = "the game is still running from the mod build; close it first (" + ec.message() + ")", false;
    fs::remove_all(old, ec);
  }
  fs::remove(root / "stamp.txt", ec);
  if (!Mirror(assets_, data, error)) return false;
  for (const auto& [name, patch] : patches) {
    std::vector<uint8_t> out;
    if (!kmod::apply(patch, ReadAll(assets_ / name), out, error)) return false;
    fs::remove(data / name, ec);
    std::ofstream file(data / name, std::ios::binary);
    file.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
    if (!file) return error = "could not write " + name, false;
  }
  std::ofstream(root / "stamp.txt", std::ios::trunc) << stamp;
  data_root = data;
  return true;
}

}  // namespace kameo::launcher
