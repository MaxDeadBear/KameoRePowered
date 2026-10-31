// The copy of a player's saves taken just before a cheat first takes effect, shared by the game, which makes it, and the launcher, which puts it back.
#pragma once

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace kameo::save_backup {

namespace detail {

namespace fs = std::filesystem;

inline constexpr const char* kTitle = "4D5307D2";
inline constexpr const char* kSavedGames = "00000001";

inline fs::path BackupRoot(const fs::path& content_root) { return content_root / "save_backups"; }
inline fs::path BeforeCheats(const fs::path& content_root) { return BackupRoot(content_root) / "before_cheats"; }

// Saves live under <xuid>\<title>\00000001, with their headers under <xuid>\<title>\Headers\00000001; the all-zero xuid holds DLC, not saves.
inline std::vector<fs::path> ProfileFolders(const fs::path& content_root) {
  std::vector<fs::path> profiles;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(content_root, ec)) {
    const std::string name = entry.path().filename().string();
    if (name.size() == 16 && name != std::string(16, '0') && fs::is_directory(entry.path() / kTitle, ec)) profiles.push_back(entry.path());
  }
  return profiles;
}

// The two folders of a profile's saves, relative to the profile folder.
inline std::vector<fs::path> SaveFolders() { return {fs::path(kTitle) / kSavedGames, fs::path(kTitle) / "Headers" / kSavedGames}; }

}  // namespace detail

inline bool Exists(const std::filesystem::path& content_root) {
  std::error_code ec;
  return std::filesystem::is_directory(detail::BeforeCheats(content_root), ec);
}

// Copies every profile's saves, into a folder that takes the backup's name only once all of it is there.
inline bool BackUp(const std::filesystem::path& content_root, std::string& error) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path destination = detail::BeforeCheats(content_root);
  const fs::path partial = destination.string() + ".partial";
  fs::remove_all(partial, ec);
  fs::create_directories(partial, ec);
  if (ec) return error = ec.message(), false;
  for (const fs::path& profile : detail::ProfileFolders(content_root)) {
    for (const fs::path& folder : detail::SaveFolders()) {
      if (!fs::is_directory(profile / folder, ec)) continue;
      const fs::path target = partial / profile.filename() / folder;
      fs::create_directories(target.parent_path(), ec);
      fs::copy(profile / folder, target, fs::copy_options::recursive, ec);
      if (ec) return error = ec.message(), false;
    }
  }
  fs::remove_all(destination, ec);
  fs::rename(partial, destination, ec);
  if (ec) return error = ec.message(), false;
  return true;
}

// Puts the backup back. The saves it replaces go to a replaced_N folder rather than being deleted, so a mistake here loses nothing.
inline bool Restore(const std::filesystem::path& content_root, std::string& error) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path backup = detail::BeforeCheats(content_root);
  if (!fs::is_directory(backup, ec)) return error = "there is no backup to restore", false;
  fs::path replaced;
  for (int n = 1;; ++n) {
    replaced = detail::BackupRoot(content_root) / ("replaced_" + std::to_string(n));
    if (!fs::exists(replaced, ec)) break;
  }
  for (const fs::path& profile : detail::ProfileFolders(content_root)) {
    for (const fs::path& folder : detail::SaveFolders()) {
      if (!fs::is_directory(profile / folder, ec)) continue;
      const fs::path target = replaced / profile.filename() / folder;
      fs::create_directories(target.parent_path(), ec);
      fs::rename(profile / folder, target, ec);
      if (ec) return error = ec.message(), false;
    }
  }
  for (const auto& profile : fs::directory_iterator(backup, ec)) {
    fs::copy(profile.path(), content_root / profile.path().filename(), fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) return error = ec.message(), false;
  }
  fs::remove_all(backup, ec);
  return true;
}

}  // namespace kameo::save_backup
