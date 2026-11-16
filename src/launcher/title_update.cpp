// Installing and checking the title update for kameorepowered-tu.exe; see title_update.h.

#include "launcher/title_update.h"

#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <memory>
#include <span>
#include <vector>

#include "core/localization.h"
#include "launcher/container.h"

namespace kameo::launcher {

using l10n::Tr;
using l10n::TrFormat;

namespace {

std::filesystem::path UpdateFolder(const std::filesystem::path& game_folder) { return game_folder / "assets" / "TU"; }

uint32_t BigEndian32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}

// An XEX2 optional header's value (an offset into the header for structured ones), or 0.
uint32_t OptionalHeader(const std::vector<uint8_t>& xex, uint32_t key) {
  if (xex.size() < 0x18 || std::memcmp(xex.data(), "XEX2", 4) != 0) return 0;
  const uint32_t count = BigEndian32(xex.data() + 0x14);
  for (uint32_t i = 0; i < count && 0x18 + 8 * i + 8 <= xex.size(); ++i) {
    if (BigEndian32(xex.data() + 0x18 + 8 * i) == key) return BigEndian32(xex.data() + 0x18 + 8 * i + 4);
  }
  return 0;
}

bool Sha1(std::span<const uint8_t> data, std::array<uint8_t, 20>& digest) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0) return false;
  const bool ok = BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(data.data()), ULONG(data.size()), digest.data(), ULONG(digest.size())) == 0;
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return ok;
}

// The update's delta patch names the game it patches by the SHA-1 of that XEX's RSA signature (XexModule::ApplyPatch
// checks the same), which tells the regional variants apart.
bool PatchFitsGame(const std::vector<uint8_t>& xexp, const std::vector<uint8_t>& xex) {
  const uint32_t descriptor = OptionalHeader(xexp, 0x5FF);
  if (!descriptor || descriptor + 0xC + 20 > xexp.size() || xex.size() < 0x14) return false;
  const uint32_t security = BigEndian32(xex.data() + 0x10);
  if (security + 8 + 0x100 > xex.size()) return false;
  std::array<uint8_t, 20> digest{};
  if (!Sha1(std::span<const uint8_t>(xex.data() + security + 8, 0x100), digest)) return false;
  return std::memcmp(digest.data(), xexp.data() + descriptor + 0xC, 20) == 0;
}

std::string Hex(std::span<const uint8_t> bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text;
  for (const uint8_t byte : bytes) {
    text.push_back(kDigits[byte >> 4]);
    text.push_back(kDigits[byte & 15]);
  }
  return text;
}

// The SHA-1 of the patch kameorepowered-tu.exe was recompiled from, from its version info (res/windows/kameo.rc); empty
// when the build doesn't say, which only builds older than the check do.
std::string BuiltPatchSha1(const std::filesystem::path& exe) {
  DWORD handle = 0;
  const DWORD size = ::GetFileVersionInfoSizeW(exe.c_str(), &handle);
  if (!size) return {};
  std::vector<uint8_t> info(size);
  if (!::GetFileVersionInfoW(exe.c_str(), 0, size, info.data())) return {};
  wchar_t* value = nullptr;
  UINT length = 0;
  if (!::VerQueryValueW(info.data(), L"\\StringFileInfo\\040904B0\\TitleUpdatePatchSha1", reinterpret_cast<void**>(&value), &length) || !value) return {};
  std::string sha1;
  for (UINT i = 0; i < length && value[i]; ++i) sha1.push_back(char(::towlower(value[i])));
  return sha1;
}

bool ReadNodeFully(const rex::filesystem::IContainerImage& image, const rex::filesystem::ContainerNode& node, std::vector<uint8_t>& data) {
  data.resize(size_t(node.info.size));
  size_t done = 0;
  while (done < data.size()) {
    const size_t read = ReadNode(image, node, done, data.data() + done, data.size() - done);
    if (read == 0) break;
    done += read;
  }
  return done == data.size();
}

}  // namespace

bool TitleUpdateInstalled(const std::filesystem::path& game_folder) {
  std::error_code ec;
  const std::filesystem::path folder = UpdateFolder(game_folder);
  return std::filesystem::exists(folder / "default.xexp", ec) && std::filesystem::exists(folder / "default.xex", ec) &&
         std::filesystem::exists(game_folder / kTitleUpdateExe, ec);
}

bool TitleUpdateBuildPresent(const std::filesystem::path& game_folder) {
  std::error_code ec;
  return std::filesystem::exists(game_folder / kTitleUpdateExe, ec);
}

bool IsTitleUpdatePackage(const rex::filesystem::IContainerImage& image) {
  return image.Find("default.xexp") != nullptr;
}

bool InstallTitleUpdate(const rex::filesystem::IContainerImage& image, const std::filesystem::path& game_folder, std::string& message) {
  if (!TitleUpdateBuildPresent(game_folder)) return message = Tr("This version of Kameo: RePowered was released without its title-update build, so the update can't be played with it."), false;
  std::vector<uint8_t> patch;
  const rex::filesystem::ContainerNode* patch_node = image.Find("default.xexp");
  if (!patch_node || !ReadNodeFully(image, *patch_node, patch)) return message = Tr("The title update's patch could not be read."), false;
  const std::vector<uint8_t> game = ReadFile(game_folder / "assets" / "default.xex");
  if (game.empty()) return message = Tr("Install the game first: the title update patches its default.xex."), false;
  if (!PatchFitsGame(patch, game)) return message = Tr("This title update is for a different release of the game (another region's), so it can't patch yours."), false;
  // kameorepowered-tu.exe is the update's code recompiled ahead of time, so any other patch would run code it doesn't have.
  const std::string built = BuiltPatchSha1(game_folder / kTitleUpdateExe);
  std::array<uint8_t, 20> digest{};
  if (!built.empty() && (!Sha1(patch, digest) || Hex(digest) != built)) return message = Tr("This isn't the title update Kameo: RePowered was built for (version 0.0.2.1), so it can't be played."), false;

  const std::filesystem::path folder = UpdateFolder(game_folder);
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  if (ec) return message = TrFormat("Could not create {0}: {1}", {folder.string(), ec.message()}), false;
  std::string failed;
  switch (ExtractImage(image, folder, [](uint64_t) {}, failed)) {
    case ExtractResult::kOk: break;
    case ExtractResult::kBadName: return message = TrFormat("Could not read {0} from the title update.", {failed}), false;
    case ExtractResult::kUnreadable: return message = TrFormat("Could not read {0} from the title update.", {failed}), false;
    case ExtractResult::kUnwritable: return message = TrFormat("Could not write {0}.", {failed}), false;
  }
  // The title-update build loads assets\TU\default.xex, so the loader finds the patch beside it.
  std::filesystem::copy_file(game_folder / "assets" / "default.xex", folder / "default.xex", std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) return message = TrFormat("Could not copy default.xex into {0}: {1}", {folder.string(), ec.message()}), false;
  message = Tr("Installed the title update: Expert mode, Rune Battle and Time Attack are available.");
  return true;
}

bool RemoveTitleUpdate(const std::filesystem::path& game_folder, std::string& error) {
  std::error_code ec;
  std::filesystem::remove_all(UpdateFolder(game_folder), ec);
  if (ec) return error = TrFormat("Could not remove {0}: {1}", {UpdateFolder(game_folder).string(), ec.message()}), false;
  return true;
}

}  // namespace kameo::launcher
