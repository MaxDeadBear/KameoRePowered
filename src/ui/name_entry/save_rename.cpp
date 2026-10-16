#include "ui/name_entry/save_rename.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include <rex/logging/macros.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xam/user_profile.h>

namespace {

// saveGameGenerateFileName: an ELF-style hash over the name's big-endian UTF-16 bytes, taken as signed chars.
std::string SaveFileNameFor(const std::u16string& name) {
  uint32_t hash = 0;
  for (const char16_t c : name) {
    for (const uint8_t byte : {uint8_t(c >> 8), uint8_t(c)}) {
      hash = uint32_t(int32_t(int8_t(byte))) + (hash << 4);
      if (const uint32_t top = hash & 0xF0000000) hash ^= (top >> 24) | top;
    }
  }
  char text[16];
  std::snprintf(text, sizeof(text), "%08X", hash);
  return text;
}

// The save data copied to 0x8275B5A0 keeps the player name at +4 (34 characters to +0x48) and its file name at +0x490, which later saves reuse, so both are rewritten in every save file of the folder.
bool RewriteSaveNames(const std::filesystem::path& folder, const std::u16string& name, const std::string& file_name) {
  constexpr uint32_t kSaveMagic = 0x056EC3CE;
  std::error_code error;
  bool rewritten = false;
  for (const auto& file : std::filesystem::directory_iterator(folder, error)) {
    if (!file.is_regular_file(error) || file.file_size(error) < 0x490 + 16) continue;
    std::fstream stream(file.path(), std::ios::in | std::ios::out | std::ios::binary);
    std::vector<uint8_t> bytes(0x490 + 16);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) continue;
    if ((uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3]) != kSaveMagic) continue;
    std::fill(bytes.begin() + 4, bytes.begin() + 0x48, 0);
    for (size_t i = 0; i < name.size() && i < 33; ++i) {
      bytes[4 + i * 2] = uint8_t(name[i] >> 8);
      bytes[5 + i * 2] = uint8_t(name[i]);
    }
    std::fill(bytes.begin() + 0x490, bytes.begin() + 0x490 + 9, 0);
    std::copy_n(file_name.begin(), std::min<size_t>(file_name.size(), 8), bytes.begin() + 0x490);
    stream.seekp(0);
    rewritten = bool(stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  }
  if (!rewritten) REXLOG_WARN("[kameo] rename save: no save data found in {}", folder.string());
  return rewritten;
}

}  // namespace

bool kameo::ui::RenameSaveGame(const std::string& file_name, const std::u16string& name) {
  auto* state = rex::system::kernel_state();
  auto* content = state ? state->content_manager() : nullptr;
  if (!content || !state->user_profile() || file_name.empty()) return false;
  const std::string new_file_name = SaveFileNameFor(name);
  const uint64_t user = state->user_profile()->xuid();
  // ResolveGameUserContentPath is root/title/profile/user, and saves live under root/xuid/title/00000001 with headers in root/xuid/title/Headers/00000001.
  const std::filesystem::path root = content->ResolveGameUserContentPath().parent_path().parent_path().parent_path();
  char title[16];
  std::snprintf(title, sizeof(title), "%08X", state->title_id());
  for (const uint64_t xuid : {user, uint64_t(0)}) {
    for (auto data : content->ListContent(1, xuid, rex::system::XContentType::kSavedGame)) {
      if (data.file_name() != file_name) continue;
      if (content->IsContentOpen(data)) return false;
      char owner[24];
      std::snprintf(owner, sizeof(owner), "%016llX", static_cast<unsigned long long>(data.xuid != 0 && data.xuid != uint64_t(-1) ? uint64_t(data.xuid) : xuid));
      const std::filesystem::path saves = root / owner / title / "00000001";
      const std::filesystem::path headers = root / owner / title / "Headers" / "00000001";
      std::error_code error;
      if (new_file_name != file_name && (std::filesystem::exists(saves / new_file_name, error) || std::filesystem::exists(headers / (new_file_name + ".header"), error))) {
        REXLOG_WARN("[kameo] rename save {}: a save named {} already exists", file_name, new_file_name);
        return false;
      }
      if (!RewriteSaveNames(saves / file_name, name, new_file_name)) return false;
      if (new_file_name != file_name) {
        std::filesystem::rename(saves / file_name, saves / new_file_name, error);
        if (error) {
          REXLOG_WARN("[kameo] rename save {}: moving the folder failed ({})", file_name, error.message());
          RewriteSaveNames(saves / file_name, data.display_name(), file_name);
          return false;
        }
      }
      data.set_display_name(name);
      data.set_file_name(new_file_name);
      if (XFAILED(content->WriteContentHeaderFile(xuid, data))) return false;
      if (new_file_name != file_name) std::filesystem::remove(headers / (file_name + ".header"), error);
      REXLOG_INFO("[kameo] renamed save {} to {}", file_name, new_file_name);
      return true;
    }
    if (xuid == 0) break;
  }
  REXLOG_WARN("[kameo] rename save {}: not found", file_name);
  return false;
}
