// Game builds from the project's GitHub releases: listing them, downloading them into builds\<tag>\, and switching the game folder to one.
//
// The game keeps its settings, config, logs and assets beside its exe, and released builds cannot be taught otherwise, so a build is switched in place: its files replace the game's own beside the launcher, and nothing else in the folder is touched.
// The launcher runs on its own copy of the runtime DLL (runtime_loader.cpp), so it locks none of the files a switch replaces.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace kameo::launcher {

struct BuildRelease {
  std::string tag;
  std::string name;
  std::string published;  // the date part of published_at, YYYY-MM-DD
  std::string notes;      // the release body, as written on GitHub
  std::string url;        // the release page
  bool prerelease = false;
  std::string asset_url;  // the Windows zip, empty when the release has none
  uint64_t asset_size = 0;
};

// A build unpacked under builds\, from a release or saved from the game folder before the first switch.
struct InstalledBuild {
  std::string tag;
  std::filesystem::path folder;
  bool saved_local = false;  // the game's own files, kept so a switch can be undone
};

class BuildManager {
 public:
  // Where the releases come from; the repository moved from birabittoh to MaxDeadBear, and GitHub redirects the old name.
  static constexpr const char* kRepository = "MaxDeadBear/KameoRePowered";

  ~BuildManager();

  // `game_folder` is the launcher's own folder, which holds the game; `active` is the tag last switched to, from the launcher's preferences.
  void Init(const std::filesystem::path& game_folder, const std::string& active);

  // Background jobs, one at a time; each returns false when one is already running.
  bool Refresh();
  bool Install(const std::string& tag);
  bool Activate(const std::string& tag);
  bool Remove(const std::string& tag);

  bool busy() const { return busy_.load(); }
  void Cancel() { cancel_.store(true); }
  // What the running job is doing and how far it is, 0 to 1 (negative while unknown).
  std::string task() const;
  float progress() const { return progress_.load(); }

  // Snapshots for the UI; cheap enough to take every frame.
  std::vector<BuildRelease> releases() const;
  std::vector<InstalledBuild> installed() const;
  std::string active() const;
  bool fetched() const { return fetched_.load(); }
  // The last job's outcome, shown once and then cleared.
  std::string TakeMessage();
  // Set when a switch completed, so the launcher saves the active tag.
  bool TakeActiveChanged();

  // The newest release newer than the active build, honouring the pre-release choice; null when the game is up to date or the list is not in yet.
  const BuildRelease* Newer(const std::vector<BuildRelease>& list, bool include_prereleases) const;

 private:
  bool StartJob(const std::string& task, void (BuildManager::*job)(std::string), std::string tag);
  void Finish(const std::string& message);
  void RefreshJob(std::string);
  void InstallJob(std::string tag);
  void ActivateJob(std::string tag);
  void RemoveJob(std::string tag);
  void ScanInstalled();
  bool InstallLocked(const std::string& tag, std::string& error);
  std::filesystem::path BuildsFolder() const { return folder_ / "builds"; }

  std::filesystem::path folder_;
  std::thread thread_;
  std::atomic<bool> busy_{false};
  std::atomic<bool> cancel_{false};
  std::atomic<bool> fetched_{false};
  std::atomic<float> progress_{-1.0f};
  mutable std::mutex mutex_;
  std::string task_;
  std::string message_;
  std::string active_;
  bool active_changed_ = false;
  std::vector<BuildRelease> releases_;
  std::vector<InstalledBuild> installed_;
};

}  // namespace kameo::launcher
