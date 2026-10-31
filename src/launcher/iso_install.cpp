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

#include <rex/filesystem.h>
#include <rex/filesystem/device.h>
#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/system/xcontent.h>

#include "launcher/dlc_install.h"
#include "launcher/settings_file.h"

namespace kameo::launcher {
namespace {

namespace fs = std::filesystem;
using rex::filesystem::Entry;

constexpr size_t kChunkBytes = 4 * 1024 * 1024;
// Room left over once the game is out, so the unpack does not fill the drive to the last byte.
constexpr uint64_t kSpareBytes = 256ull * 1024 * 1024;
// Where a disc keeps the console's own update, which nothing here reads.
constexpr const char* kSystemUpdate = "$SystemUpdate";
// The XEX header key of the execution info, which holds the title ID.
constexpr uint32_t kXexExecutionInfo = 0x00040006;

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) return {};
  std::wstring out(size_t(::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0)), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), int(out.size()));
  return out;
}

uint32_t BigEndian32(const uint8_t* bytes) { return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3]; }

std::string SizeText(uint64_t bytes) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f GB", double(bytes) / (1024.0 * 1024.0 * 1024.0));
  return text;
}

// A name comes from the image, so one that would leave the folder it is written to is treated as a damaged image.
bool IsSafeName(const std::string& name) {
  if (name.empty() || name == "." || name == "..") return false;
  return std::none_of(name.begin(), name.end(), [](char c) { return c == '/' || c == '\\' || c == ':' || uint8_t(c) < 0x20; });
}

// The runtime maps an image into memory, so a file cut short by a failed download, or a drive pulled out, faults in the read instead of returning an error.
bool InitializeDevice(rex::filesystem::Device& device) {
  __try {
    return device.Initialize();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

size_t ReadChunk(rex::filesystem::File& file, uint8_t* buffer, size_t size, size_t offset) {
  size_t read = 0;
  __try {
    file.ReadSync(std::span<uint8_t>(buffer, size), offset, &read);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
  return read;
}

std::unique_ptr<rex::filesystem::Device> OpenGame(const fs::path& source) {
  auto disc = std::make_unique<rex::filesystem::DiscImageDevice>("", source);
  if (InitializeDevice(*disc)) return disc;
  const auto header = rex::filesystem::StfsContainerDevice::ReadPackageHeader(source);
  if (!header || !header->header.is_magic_valid()) return nullptr;
  if (rex::system::XContentType(header->metadata.content_type) != rex::system::XContentType::kGamesOnDemand) return nullptr;
  auto package = std::make_unique<rex::filesystem::StfsContainerDevice>("", source);
  if (!InitializeDevice(*package)) return nullptr;
  return package;
}

// The title ID in a default.xex's headers, or 0 when it is not one.
uint32_t XexTitleId(Entry& xex, uint8_t* buffer) {
  rex::filesystem::File* file = nullptr;
  if (xex.Open(rex::filesystem::FileAccess::kFileReadData, &file) != 0 || !file) return 0;
  const size_t read = ReadChunk(*file, buffer, std::min<size_t>(xex.size(), kChunkBytes), 0);
  file->Destroy();
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

bool Collect(Entry& folder, const fs::path& relative, bool top, std::vector<std::pair<fs::path, Entry*>>& files, uint64_t& total) {
  for (const auto& child : folder.children()) {
    if (!IsSafeName(child->name())) return false;
    if (top && _stricmp(child->name().c_str(), kSystemUpdate) == 0) continue;
    const fs::path path = relative / Widen(child->name());
    if (child->attributes() & rex::filesystem::kFileAttributeDirectory) {
      if (!Collect(*child, path, false, files, total)) return false;
    } else {
      files.emplace_back(path, child.get());
      total += child->size();
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
  if (fs::exists(assets / "default.xex", ec)) return message = "The game is already unpacked in " + assets.string() + ".", false;
  if (fs::exists(assets, ec) && !fs::is_empty(assets, ec)) return message = "The assets folder already has files in it. Move them somewhere else, then try again.", false;

  const std::unique_ptr<rex::filesystem::Device> device = OpenGame(source);
  if (!device) return message = file_name + " could not be read. It may be incomplete, or not an Xbox 360 disc image or Games on Demand package.", false;
  Entry* root = device->ResolvePath("");
  Entry* xex = root ? root->ResolvePath("default.xex") : nullptr;
  if (!xex) return message = file_name + " has no default.xex, so it is not a game disc.", false;
  const auto buffer = std::make_unique<uint8_t[]>(kChunkBytes);
  const uint32_t title_id = XexTitleId(*xex, buffer.get());
  if (title_id != kKameoTitleId) {
    char text[16];
    std::snprintf(text, sizeof(text), "%08X", title_id);
    return message = title_id == 0 ? file_name + " is incomplete or damaged: its game program could not be read." : file_name + " is a different game (title " + text + "), not Kameo: Elements of Power.", false;
  }

  std::vector<std::pair<fs::path, Entry*>> files;
  uint64_t total = 0;
  if (!Collect(*root, {}, true, files, total)) return message = file_name + " looks damaged: it holds a file with an invalid name.", false;
  total_bytes_.store(total);

  const fs::path parent = assets.parent_path();
  const fs::space_info space = fs::space(parent, ec);
  if (!ec && space.available < total + kSpareBytes) return message = "Not enough free space: the game needs " + SizeText(total + kSpareBytes) + " on this drive, and it has " + SizeText(space.available) + ".", false;

  const fs::path staging = parent / L"assets.unpacking";
  fs::remove_all(staging, ec);
  uint64_t written = 0;
  for (const auto& [relative, entry] : files) {
    if (cancel_.load()) return message = "Unpacking was cancelled.", false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      current_ = relative.filename().string();
    }
    const fs::path destination = staging / relative;
    fs::create_directories(destination.parent_path(), ec);
    rex::filesystem::File* in_file = nullptr;
    if (entry->Open(rex::filesystem::FileAccess::kFileReadData, &in_file) != 0 || !in_file) return message = "Could not read " + relative.string() + " from " + file_name + ".", false;
    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    size_t offset = 0;
    size_t remaining = entry->size();
    while (out && remaining > 0 && !cancel_.load()) {
      const size_t read = ReadChunk(*in_file, buffer.get(), std::min(remaining, kChunkBytes), offset);
      if (read == 0) break;
      out.write(reinterpret_cast<const char*>(buffer.get()), std::streamsize(read));
      offset += read;
      remaining -= read;
      written += read;
      done_bytes_.store(written);
    }
    in_file->Destroy();
    out.close();
    if (cancel_.load()) return message = "Unpacking was cancelled.", false;
    if (remaining > 0 || !out) return message = "Could not unpack " + relative.string() + ". The image may be incomplete or damaged, or the drive may be full.", false;
  }

  // An empty assets folder is in the way of the rename, and is all that can be there by now.
  fs::remove(assets, ec);
  fs::rename(staging, assets, ec);
  if (ec) return message = "Could not move the game into " + assets.string() + ": " + ec.message(), false;
  message = "Your game is ready to play.";
  return true;
}

}  // namespace kameo::launcher
