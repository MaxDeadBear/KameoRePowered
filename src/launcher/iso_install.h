// Kameo RePowered launcher: the player's disc image or Games on Demand package unpacked into the assets folder the game reads.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace kameo::launcher {

// Whether the game will find its files: the assets folder near the launcher, or the game_data_root kameorepowered.toml names, either of them holding default.xex directly or one folder down.
bool HasGameFiles(const std::filesystem::path& game_folder);

// Unpacks on a thread of its own and reports progress for the UI to poll.
class IsoInstaller {
 public:
  ~IsoInstaller();

  void Start(std::filesystem::path source, std::filesystem::path assets);
  void Cancel() { cancel_.store(true); }
  bool running() const { return running_.load(); }
  // Fraction of the game's bytes unpacked so far, 0 to 1.
  float progress() const;
  std::string current() const;
  // What came of the run that just finished, once; empty otherwise.
  std::string TakeResult();
  // The run's outcome: true once the game's files are in place.
  bool installed() const { return installed_.load(); }

 private:
  void Run(std::filesystem::path source, std::filesystem::path assets);
  bool Unpack(const std::filesystem::path& source, const std::filesystem::path& assets, std::string& message);

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> cancel_{false};
  std::atomic<bool> installed_{false};
  std::atomic<uint64_t> done_bytes_{0};
  std::atomic<uint64_t> total_bytes_{0};
  mutable std::mutex mutex_;
  std::string current_;
  std::string result_;
};

}  // namespace kameo::launcher
