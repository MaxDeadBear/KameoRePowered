#include "launcher/builds.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <utility>

#include "launcher/http.h"
#include "launcher/json.h"
#include "launcher/zip_extract.h"

namespace kameo::launcher {
namespace {

namespace fs = std::filesystem;

constexpr const wchar_t* kGameExe = L"kameorepowered.exe";
// Beside each downloaded build: the tag it came from, when the folder name had to be cleaned up.
constexpr const wchar_t* kTagFile = L"build_tag.txt";

// A tag as a folder name: anything but letters, digits, '.', '-' and '_' becomes '_'.
std::string FolderName(const std::string& tag) {
  std::string out;
  for (const char c : tag) out.push_back((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' ? c : '_');
  return out.empty() ? "build" : out;
}

// The files a build consists of: the files at the top of its folder, without the launcher, which updates itself separately.
std::vector<fs::path> BuildFiles(const fs::path& folder) {
  std::vector<fs::path> files;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(folder, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    const std::wstring name = entry.path().filename().wstring();
    if (_wcsnicmp(name.c_str(), L"KameoLauncher", 13) == 0 || _wcsicmp(name.c_str(), kTagFile) == 0) continue;
    files.push_back(entry.path().filename());
  }
  return files;
}

// The game's own files in the game folder: its exe and the DLLs beside it (the runtime and the GPU plugin), which is what a release ships.
std::vector<fs::path> GameFolderFiles(const fs::path& folder) {
  std::vector<fs::path> files;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(folder, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    const fs::path name = entry.path().filename();
    std::wstring extension = name.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    if (_wcsicmp(name.c_str(), kGameExe) == 0 || extension == L".dll") files.push_back(name);
  }
  return files;
}

bool FilesEqual(const fs::path& a, const fs::path& b) {
  std::error_code ec;
  const auto size_a = fs::file_size(a, ec);
  if (ec) return false;
  const auto size_b = fs::file_size(b, ec);
  if (ec || size_a != size_b) return false;
  std::ifstream fa(a, std::ios::binary);
  std::ifstream fb(b, std::ios::binary);
  std::vector<char> ba(1 << 20), bb(1 << 20);
  while (fa && fb) {
    fa.read(ba.data(), std::streamsize(ba.size()));
    fb.read(bb.data(), std::streamsize(bb.size()));
    if (fa.gcount() != fb.gcount() || std::memcmp(ba.data(), bb.data(), size_t(fa.gcount())) != 0) return false;
  }
  return fa.eof() && fb.eof();
}

// True while something has the game's exe open, which is the game running.
bool GameRunning(const fs::path& exe) {
  const HANDLE file = ::CreateFileW(exe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return ::GetLastError() == ERROR_SHARING_VIOLATION;
  ::CloseHandle(file);
  return false;
}

std::string Today() {
  const std::time_t now = std::time(nullptr);
  std::tm local = {};
  localtime_s(&local, &now);
  char text[16];
  std::strftime(text, sizeof(text), "%Y-%m-%d", &local);
  return text;
}

}  // namespace

BuildManager::~BuildManager() {
  cancel_.store(true);
  if (thread_.joinable()) thread_.join();
}

void BuildManager::Init(const fs::path& game_folder, const std::string& active) {
  folder_ = game_folder;
  active_ = active;
  // Files a switch moved aside last time; one still in use by a running game is left for next time.
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(BuildsFolder() / ".trash", ec)) fs::remove_all(entry.path(), ec);
  ScanInstalled();
}

void BuildManager::ScanInstalled() {
  std::vector<InstalledBuild> found;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(BuildsFolder(), ec)) {
    if (!entry.is_directory(ec)) continue;
    const std::string name = entry.path().filename().string();
    if (name.empty() || name[0] == '.' || name.ends_with(".unpacking")) continue;
    if (!fs::exists(entry.path() / kGameExe, ec)) continue;
    InstalledBuild build;
    build.folder = entry.path();
    build.saved_local = name.rfind("local-", 0) == 0;
    // A release's folder is named after its tag; the tag file keeps the original when the name had to be cleaned up.
    std::ifstream tag_file(entry.path() / kTagFile);
    if (!(tag_file && std::getline(tag_file, build.tag)) || build.tag.empty()) build.tag = name;
    found.push_back(std::move(build));
  }
  std::string active;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    active = active_;
  }
  // With nothing remembered, the build the game folder holds is whichever installed one has the same exe.
  if (active.empty()) {
    for (const InstalledBuild& build : found) {
      if (FilesEqual(build.folder / kGameExe, folder_ / kGameExe)) {
        active = build.tag;
        break;
      }
    }
  }
  std::lock_guard<std::mutex> lock(mutex_);
  installed_ = std::move(found);
  if (active_.empty() && !active.empty()) {
    active_ = active;
    active_changed_ = true;
  }
}

std::string BuildManager::task() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return task_;
}

std::vector<BuildRelease> BuildManager::releases() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return releases_;
}

std::vector<InstalledBuild> BuildManager::installed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return installed_;
}

std::string BuildManager::active() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_;
}

std::string BuildManager::TakeMessage() {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::exchange(message_, std::string());
}

bool BuildManager::TakeActiveChanged() {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::exchange(active_changed_, false);
}

const BuildRelease* BuildManager::Newer(const std::vector<BuildRelease>& list, bool include_prereleases) const {
  const std::string current = active();
  std::string current_date;
  for (const BuildRelease& release : list) {
    if (release.tag == current) current_date = release.published;
  }
  // A build installed by hand or saved from the game folder has no release date, so the game exe's own date stands in; without it an older release would be offered as an update.
  if (current_date.empty()) {
    std::error_code ec;
    const auto written = fs::last_write_time(folder_ / kGameExe, ec);
    if (ec) return nullptr;
    const std::time_t seconds = std::chrono::system_clock::to_time_t(std::chrono::clock_cast<std::chrono::system_clock>(written));
    std::tm local = {};
    localtime_s(&local, &seconds);
    char text[16];
    std::strftime(text, sizeof(text), "%Y-%m-%d", &local);
    current_date = text;
  }
  for (const BuildRelease& release : list) {
    if (release.asset_url.empty() || (release.prerelease && !include_prereleases)) continue;
    // The list is newest first, so the first candidate decides.
    if (release.tag == current) return nullptr;
    return release.published > current_date ? &release : nullptr;
  }
  return nullptr;
}

bool BuildManager::StartJob(const std::string& task, void (BuildManager::*job)(std::string), std::string tag) {
  if (busy_.exchange(true)) return false;
  if (thread_.joinable()) thread_.join();
  cancel_.store(false);
  progress_.store(-1.0f);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    task_ = task;
  }
  thread_ = std::thread([this, job, tag = std::move(tag)]() mutable {
    (this->*job)(std::move(tag));
    busy_.store(false);
  });
  return true;
}

void BuildManager::Finish(const std::string& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  message_ = message;
  task_.clear();
}

bool BuildManager::Refresh() { return StartJob("Checking GitHub for builds...", &BuildManager::RefreshJob, {}); }
bool BuildManager::Install(const std::string& tag) { return StartJob("Downloading " + tag + "...", &BuildManager::InstallJob, tag); }
bool BuildManager::Activate(const std::string& tag) { return StartJob("Switching to " + tag + "...", &BuildManager::ActivateJob, tag); }
bool BuildManager::Remove(const std::string& tag) { return StartJob("Removing " + tag + "...", &BuildManager::RemoveJob, tag); }

void BuildManager::RefreshJob(std::string) {
  std::string body, error;
  if (!http::Get(std::string("https://api.github.com/repos/") + kRepository + "/releases?per_page=40", body, error, "application/vnd.github+json")) {
    Finish("Could not check for builds: " + error);
    return;
  }
  json::Value document;
  if (!json::Parse(body, document, error) || document.type != json::Value::Type::Array) {
    Finish("GitHub's answer could not be read: " + (error.empty() ? std::string("not a release list") : error));
    return;
  }
  std::vector<BuildRelease> list;
  for (const json::Value& item : document.array) {
    if (item["draft"].Bool()) continue;
    BuildRelease release;
    release.tag = item["tag_name"].Str();
    release.name = item["name"].Str();
    release.notes = item["body"].Str();
    release.url = item["html_url"].Str();
    release.prerelease = item["prerelease"].Bool();
    release.published = item["published_at"].Str().substr(0, 10);
    for (const json::Value& asset : item["assets"].array) {
      const std::string name = asset["name"].Str();
      if (name.ends_with("-windows-x64.zip")) {
        release.asset_url = asset["browser_download_url"].Str();
        release.asset_size = uint64_t(asset["size"].Num());
      }
    }
    if (!release.tag.empty()) list.push_back(std::move(release));
  }
  std::stable_sort(list.begin(), list.end(), [](const BuildRelease& a, const BuildRelease& b) { return a.published > b.published; });
  {
    std::lock_guard<std::mutex> lock(mutex_);
    releases_ = std::move(list);
  }
  fetched_.store(true);
  ScanInstalled();
  Finish("");
}

bool BuildManager::InstallLocked(const std::string& tag, std::string& error) {
  BuildRelease release;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = std::find_if(releases_.begin(), releases_.end(), [&](const BuildRelease& r) { return r.tag == tag; });
    if (found == releases_.end() || found->asset_url.empty()) {
      error = tag + " has no Windows build to download";
      return false;
    }
    release = *found;
  }
  std::error_code ec;
  const fs::path builds = BuildsFolder();
  fs::create_directories(builds, ec);
  const std::string folder_name = FolderName(tag);
  const fs::path zip = builds / (folder_name + ".zip.part");
  const fs::path unpacking = builds / (folder_name + ".unpacking");
  const fs::path target = builds / folder_name;
  const bool downloaded = http::Download(release.asset_url, zip, [this, size = release.asset_size](uint64_t done, uint64_t total) {
    const uint64_t whole = total ? total : size;
    progress_.store(whole ? float(double(done) / double(whole)) : -1.0f);
    return !cancel_.load();
  }, error);
  if (!downloaded) return false;
  progress_.store(-1.0f);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    task_ = "Unpacking " + tag + "...";
  }
  fs::remove_all(unpacking, ec);
  const bool extracted = ExtractZip(zip, unpacking, error);
  fs::remove(zip, ec);
  if (!extracted) {
    fs::remove_all(unpacking, ec);
    return false;
  }
  if (!fs::exists(unpacking / kGameExe, ec)) {
    fs::remove_all(unpacking, ec);
    error = "the download holds no kameorepowered.exe";
    return false;
  }
  std::ofstream(unpacking / kTagFile) << tag;
  fs::remove_all(target, ec);
  fs::rename(unpacking, target, ec);
  if (ec) {
    error = "could not move the build into place (" + ec.message() + ")";
    return false;
  }
  return true;
}

void BuildManager::InstallJob(std::string tag) {
  std::string error;
  const bool ok = InstallLocked(tag, error);
  ScanInstalled();
  Finish(ok ? "Downloaded " + tag + "." : "Could not download " + tag + ": " + error);
}

void BuildManager::ActivateJob(std::string tag) {
  std::error_code ec;
  // Downloads it first when it is not here yet.
  fs::path source;
  for (const InstalledBuild& build : installed()) {
    if (build.tag == tag) source = build.folder;
  }
  if (source.empty()) {
    std::string error;
    if (!InstallLocked(tag, error)) {
      ScanInstalled();
      Finish("Could not download " + tag + ": " + error);
      return;
    }
    ScanInstalled();
    for (const InstalledBuild& build : installed()) {
      if (build.tag == tag) source = build.folder;
    }
    if (source.empty()) {
      Finish("Could not find " + tag + " after downloading it.");
      return;
    }
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    task_ = "Switching to " + tag + "...";
  }
  progress_.store(-1.0f);
  const fs::path game_exe = folder_ / kGameExe;
  if (GameRunning(game_exe)) {
    Finish("Close Kameo before switching builds.");
    return;
  }

  // The game folder's own build is kept before it is first replaced, unless it is one of the downloaded builds.
  bool known = !fs::exists(game_exe, ec);
  for (const InstalledBuild& build : installed()) {
    if (!known && FilesEqual(build.folder / kGameExe, game_exe)) known = true;
  }
  if (!known) {
    const fs::path saved = BuildsFolder() / ("local-" + Today());
    fs::remove_all(saved, ec);
    fs::create_directories(saved, ec);
    bool saved_all = true;
    for (const fs::path& name : GameFolderFiles(folder_)) {
      fs::copy_file(folder_ / name, saved / name, fs::copy_options::overwrite_existing, ec);
      if (ec) saved_all = false;
    }
    if (!saved_all) {
      fs::remove_all(saved, ec);
      Finish("Could not keep a copy of the current build before switching, so nothing was changed.");
      return;
    }
    std::ofstream(saved / kTagFile) << ("local-" + Today());
  }

  // Each file is copied in beside its target, the old one is moved aside, then the new one takes its name; any failure puts the old files back.
  const fs::path trash = BuildsFolder() / ".trash" / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  fs::create_directories(trash, ec);
  struct Replaced {
    fs::path target;
    fs::path aside;  // empty when there was no file to replace
  };
  std::vector<Replaced> replaced;
  std::string failure;
  for (const fs::path& name : BuildFiles(source)) {
    const fs::path target = folder_ / name;
    const fs::path incoming = folder_ / (name.wstring() + L".incoming");
    fs::copy_file(source / name, incoming, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      failure = "could not copy " + name.string() + " (" + ec.message() + ")";
      break;
    }
    Replaced step{target, {}};
    if (fs::exists(target, ec)) {
      step.aside = trash / name;
      if (!::MoveFileExW(target.c_str(), step.aside.c_str(), MOVEFILE_COPY_ALLOWED)) {
        fs::remove(incoming, ec);
        failure = "could not move the old " + name.string() + " aside (error " + std::to_string(::GetLastError()) + ")";
        break;
      }
    }
    if (!::MoveFileExW(incoming.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
      failure = "could not put the new " + name.string() + " in place (error " + std::to_string(::GetLastError()) + ")";
      if (!step.aside.empty()) ::MoveFileExW(step.aside.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
      fs::remove(incoming, ec);
      break;
    }
    replaced.push_back(std::move(step));
  }
  if (!failure.empty()) {
    for (auto it = replaced.rbegin(); it != replaced.rend(); ++it) {
      if (it->aside.empty()) fs::remove(it->target, ec);
      else ::MoveFileExW(it->aside.c_str(), it->target.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    ScanInstalled();
    Finish("Could not switch to " + tag + ": " + failure + ". The previous build is still in place.");
    return;
  }
  fs::remove_all(trash, ec);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = tag;
    active_changed_ = true;
  }
  ScanInstalled();
  Finish("Switched to " + tag + ".");
}

void BuildManager::RemoveJob(std::string tag) {
  if (tag == active()) {
    Finish("The build in use cannot be removed; switch to another first.");
    return;
  }
  std::error_code ec;
  for (const InstalledBuild& build : installed()) {
    if (build.tag == tag) fs::remove_all(build.folder, ec);
  }
  ScanInstalled();
  Finish(ec ? "Could not remove " + tag + ": " + ec.message() : "Removed " + tag + ".");
}

}  // namespace kameo::launcher
