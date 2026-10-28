// Kameo RePowered launcher: Xbox 360 DLC packages unpacked into the content folder the game reads.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace kameo::launcher {

constexpr uint32_t kKameoTitleId = 0x4D5307D2;

struct InstalledDlc {
  std::string file_name;
  std::string display_name;
  uint64_t size_bytes = 0;
  // What this build can use from the pack: the vanilla game registers its skins, while levels and new text in title-update packs go unused.
  std::string support;
};

// Where the runtime keeps saves and DLC: user_data_root from kameorepowered.toml, or Documents\kameorepowered.
std::filesystem::path ContentRoot(const std::filesystem::path& game_folder);
std::vector<InstalledDlc> ListInstalledDlc(const std::filesystem::path& content_root);
bool RemoveDlc(const std::filesystem::path& content_root, const std::string& file_name, std::string& error);

// Every LIVE, PIRS or CON package in a folder (searched through its subfolders) or the file itself.
std::vector<std::filesystem::path> FindPackages(const std::filesystem::path& folder_or_file);

// Unpacks packages on a thread of its own and reports progress for the UI to poll.
class DlcInstaller {
 public:
  ~DlcInstaller();

  void Start(std::vector<std::filesystem::path> packages, std::filesystem::path content_root);
  bool running() const { return running_.load(); }
  // Fraction of all bytes unpacked so far, 0 to 1.
  float progress() const;
  std::string current() const;
  // One line per package once the run has finished: what was installed and what was skipped and why.
  std::vector<std::string> TakeResults();

 private:
  void Run(std::vector<std::filesystem::path> packages, std::filesystem::path content_root);
  bool InstallOne(const std::filesystem::path& package, const std::filesystem::path& content_root, std::string& message);

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> done_bytes_{0};
  std::atomic<uint64_t> total_bytes_{0};
  mutable std::mutex mutex_;
  std::string current_;
  std::vector<std::string> results_;
};

}  // namespace kameo::launcher
