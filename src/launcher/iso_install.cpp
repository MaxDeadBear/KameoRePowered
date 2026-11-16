// A disc image or Games on Demand package unpacked through the runtime's own readers for them, into a folder beside assets that takes its place only once every file is out, so a failed or cancelled run leaves nothing half done.

#include "launcher/iso_install.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "core/localization.h"
#include "launcher/container.h"
#include "launcher/dlc_install.h"
#include "launcher/settings_file.h"

namespace kameo::launcher {

using l10n::Tr;
using l10n::TrFormat;

namespace {

namespace fs = std::filesystem;
using rex::filesystem::ContainerNode;

constexpr size_t kChunkBytes = 4 * 1024 * 1024;
// Room left over once the game is out, so the unpack does not fill the drive to the last byte.
constexpr uint64_t kSpareBytes = 256ull * 1024 * 1024;
// Where a disc keeps the console's own update, which nothing here reads.
constexpr const char* kSystemUpdate = "$SystemUpdate";
// The XEX header key of the execution info, which holds the title ID.
constexpr uint32_t kXexExecutionInfo = 0x00040006;

uint32_t BigEndian32(const uint8_t* bytes) { return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3]; }

std::string SizeText(uint64_t bytes) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f GB", double(bytes) / (1024.0 * 1024.0 * 1024.0));
  return text;
}

// A disc image, or a Games on Demand package.
std::unique_ptr<rex::filesystem::IContainerImage> OpenGame(const fs::path& source) {
  auto image = OpenImage(source);
  if (!image) return nullptr;
  if (image->GetType() == rex::filesystem::ContainerType::kGDFX) return image;
  const rex::filesystem::XContentPackageHeader* header = image->GetHeader();
  if (!header || !header->header.IsMagicValid()) return nullptr;
  if (static_cast<rex::kernel::XContentType>(header->metadata.content_type) != rex::kernel::XContentType::kGamesOnDemand) return nullptr;
  return image;
}

// The title ID in a default.xex's headers, or 0 when it is not one.
uint32_t XexTitleId(const rex::filesystem::IContainerImage& image, const ContainerNode& xex, uint8_t* buffer) {
  const size_t read = ReadNode(image, xex, 0, buffer, size_t(std::min<uint64_t>(xex.info.size, kChunkBytes)));
  if (read < 0x18 || std::memcmp(buffer, "XEX2", 4) != 0) return 0;
  const uint32_t headers = BigEndian32(buffer + 0x14);
  for (uint32_t i = 0; i < headers && 0x20 + (i + 1) * 8ull <= read; ++i) {
    const uint8_t* header = buffer + 0x18 + i * 8;
    if (BigEndian32(header) != kXexExecutionInfo) continue;
    const uint32_t info = BigEndian32(header + 4);
    return info + 0x10ull <= read ? BigEndian32(buffer + info + 0x0C) : 0;
  }
  return 0;
}

bool Collect(const rex::filesystem::IContainerImage& image, const ContainerNode& folder, const fs::path& relative, bool top, std::vector<std::pair<fs::path, const ContainerNode*>>& files, uint64_t& total) {
  const auto nodes = image.GetNodes();
  for (const uint32_t index : folder.children) {
    const ContainerNode& child = nodes[index];
    if (!IsSafeName(child.info.name)) return false;
    if (top && _stricmp(child.info.name.c_str(), kSystemUpdate) == 0) continue;
    const fs::path path = relative / Widen(child.info.name);
    if (IsDirectory(child)) {
      if (!Collect(image, child, path, false, files, total)) return false;
    } else {
      files.emplace_back(path, &child);
      total += child.info.size;
    }
  }
  return true;
}

bool HoldsGame(const fs::path& folder) {
  std::error_code ec;
  if (fs::exists(folder / "default.xex", ec)) return true;
  for (const auto& entry : fs::directory_iterator(folder, ec)) {
    if (entry.is_directory(ec) && fs::exists(entry.path() / "default.xex", ec)) return true;
  }
  return false;
}

}  // namespace

bool HasGameFiles(const fs::path& game_folder) {
  const std::string configured = ReadMainConfigGameDataRoot(game_folder / "kameorepowered.toml");
  if (!configured.empty()) return HoldsGame(fs::path(Widen(configured)));
  std::error_code ec;
  // Build folders are given theirs further up, as the level mods look for it.
  for (fs::path dir = game_folder; !dir.empty(); dir = dir.parent_path()) {
    if (fs::exists(dir / "assets", ec) && HoldsGame(dir / "assets")) return true;
    if (dir == dir.parent_path()) break;
  }
  return false;
}

IsoInstaller::~IsoInstaller() {
  cancel_.store(true);
  if (thread_.joinable()) thread_.join();
}

void IsoInstaller::Start(fs::path source, fs::path assets) {
  if (running_.load()) return;
  if (thread_.joinable()) thread_.join();
  done_bytes_.store(0);
  total_bytes_.store(0);
  cancel_.store(false);
  installed_.store(false);
  running_.store(true);
  thread_ = std::thread(&IsoInstaller::Run, this, std::move(source), std::move(assets));
}

float IsoInstaller::progress() const {
  const uint64_t total = total_bytes_.load();
  return total == 0 ? 0.0f : float(double(done_bytes_.load()) / double(total));
}

std::string IsoInstaller::current() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return current_;
}

std::string IsoInstaller::TakeResult() {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::exchange(result_, {});
}

void IsoInstaller::Run(fs::path source, fs::path assets) {
  std::string message;
  const fs::path staging = assets.parent_path() / L"assets.unpacking";
  const bool ok = Unpack(source, assets, message);
  std::error_code ec;
  if (!ok) fs::remove_all(staging, ec);
  installed_.store(ok);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    current_.clear();
    result_ = std::move(message);
  }
  running_.store(false);
}

bool IsoInstaller::Unpack(const fs::path& source, const fs::path& assets, std::string& message) {
  std::error_code ec;
  const std::string file_name = source.filename().string();
  if (fs::exists(assets / "default.xex", ec)) return message = TrFormat("The game is already unpacked in {0}.", {assets.string()}), false;
  if (fs::exists(assets, ec) && !fs::is_empty(assets, ec)) return message = Tr("The assets folder already has files in it. Move them somewhere else, then try again."), false;

  const std::unique_ptr<rex::filesystem::IContainerImage> image = OpenGame(source);
  if (!image) return message = TrFormat("{0} could not be read. It may be incomplete, or not an Xbox 360 disc image or Games on Demand package.", {file_name}), false;
  const ContainerNode& root = image->GetRoot();
  const ContainerNode* xex = image->Find("default.xex");
  if (!xex) return message = TrFormat("{0} has no default.xex, so it is not a game disc.", {file_name}), false;
  const auto buffer = std::make_unique<uint8_t[]>(kChunkBytes);
  const uint32_t title_id = XexTitleId(*image, *xex, buffer.get());
  if (title_id != kKameoTitleId) {
    char text[16];
    std::snprintf(text, sizeof(text), "%08X", title_id);
    return message = title_id == 0 ? TrFormat("{0} is incomplete or damaged: its game program could not be read.", {file_name}) : TrFormat("{0} is a different game (title {1}), not Kameo: Elements of Power.", {file_name, text}), false;
  }

  std::vector<std::pair<fs::path, const ContainerNode*>> files;
  uint64_t total = 0;
  if (!Collect(*image, root, {}, true, files, total)) return message = TrFormat("{0} looks damaged: it holds a file with an invalid name.", {file_name}), false;
  total_bytes_.store(total);

  const fs::path parent = assets.parent_path();
  const fs::space_info space = fs::space(parent, ec);
  if (!ec && space.available < total + kSpareBytes) return message = TrFormat("Not enough free space: the game needs {0} on this drive, and it has {1}.", {SizeText(total + kSpareBytes), SizeText(space.available)}), false;

  const fs::path staging = parent / L"assets.unpacking";
  fs::remove_all(staging, ec);
  uint64_t written = 0;
  for (const auto& [relative, entry] : files) {
    if (cancel_.load()) return message = Tr("Unpacking was cancelled."), false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      current_ = relative.filename().string();
    }
    const fs::path destination = staging / relative;
    fs::create_directories(destination.parent_path(), ec);
    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    uint64_t offset = 0;
    uint64_t remaining = entry->info.size;
    while (out && remaining > 0 && !cancel_.load()) {
      const size_t read = ReadNode(*image, *entry, offset, buffer.get(), size_t(std::min<uint64_t>(remaining, kChunkBytes)));
      if (read == 0) break;
      out.write(reinterpret_cast<const char*>(buffer.get()), std::streamsize(read));
      offset += read;
      remaining -= read;
      written += read;
      done_bytes_.store(written);
    }
    out.close();
    if (cancel_.load()) return message = Tr("Unpacking was cancelled."), false;
    if (remaining > 0 || !out) return message = TrFormat("Could not unpack {0}. The image may be incomplete or damaged, or the drive may be full.", {relative.string()}), false;
  }

  // An empty assets folder is in the way of the rename, and is all that can be there by now.
  fs::remove(assets, ec);
  fs::rename(staging, assets, ec);
  if (ec) return message = TrFormat("Could not move the game into {0}: {1}", {assets.string(), ec.message()}), false;
  message = Tr("Your game is ready to play.");
  return true;
}

}  // namespace kameo::launcher
