// Kameo RePowered launcher: Xbox 360 DLC packages unpacked into the content folder the game reads.

#include "launcher/dlc_install.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <queue>
#include <span>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xam/content_manager.h>

#include "launcher/settings_file.h"

namespace kameo::launcher {

namespace {

// ContentManager::ResolvePackageRoot and ResolvePackageHeaderPath: DLC is shared by every profile, so it sits under the zero XUID.
constexpr const char* kSharedXuid = "0000000000000000";
constexpr const char* kTitleFolder = "4D5307D2";
constexpr const char* kMarketplaceFolder = "00000002";

std::filesystem::path PackageRoot(const std::filesystem::path& content_root) { return content_root / kSharedXuid / kTitleFolder / kMarketplaceFolder; }

std::filesystem::path HeaderRoot(const std::filesystem::path& content_root) { return content_root / kSharedXuid / kTitleFolder / "Headers" / kMarketplaceFolder; }

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int length = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
  std::wstring wide(size_t(length), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), wide.data(), length);
  return wide;
}

std::string Narrow(const std::u16string& text) {
  if (text.empty()) return {};
  const auto* wide = reinterpret_cast<const wchar_t*>(text.data());
  const int length = ::WideCharToMultiByte(CP_UTF8, 0, wide, int(text.size()), nullptr, 0, nullptr, nullptr);
  std::string utf8(size_t(length), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide, int(text.size()), utf8.data(), length, nullptr, nullptr);
  return utf8;
}

bool IsPackageFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::array<char, 4> magic{};
  if (!file.read(magic.data(), 4)) return false;
  const std::string text(magic.data(), 4);
  return text == "LIVE" || text == "PIRS" || text == "CON ";
}

uint64_t FolderSize(const std::filesystem::path& folder) {
  uint64_t size = 0;
  std::error_code ec;
  for (auto it = std::filesystem::recursive_directory_iterator(folder, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (it->is_regular_file(ec)) size += it->file_size(ec);
  }
  return size;
}

}  // namespace

std::filesystem::path ContentRoot(const std::filesystem::path& game_folder) {
  const std::string configured = ReadMainConfigUserDataRoot(game_folder / "kameorepowered.toml");
  if (!configured.empty()) return std::filesystem::path(Widen(configured));
  return rex::filesystem::GetUserFolder() / "kameorepowered";
}

std::vector<InstalledDlc> ListInstalledDlc(const std::filesystem::path& content_root) {
  std::vector<InstalledDlc> installed;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(PackageRoot(content_root), ec)) {
    if (!entry.is_directory(ec)) continue;
    InstalledDlc dlc;
    dlc.file_name = entry.path().filename().string();
    dlc.display_name = dlc.file_name;
    dlc.size_bytes = FolderSize(entry.path());
    std::ifstream header(HeaderRoot(content_root) / (dlc.file_name + ".header"), std::ios::binary);
    rex::system::xam::XCONTENT_AGGREGATE_DATA data;
    if (header.read(reinterpret_cast<char*>(&data), sizeof(data))) {
      const std::string name = Narrow(data.display_name());
      if (!name.empty()) dlc.display_name = name;
    }
    std::ifstream list(entry.path() / "KameoDLCList.txt");
    std::string line;
    int models = 0;
    bool levels = false;
    bool text = false;
    while (std::getline(list, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      const std::string extension = line.size() > 4 ? line.substr(line.size() - 4) : "";
      if (_stricmp(extension.c_str(), ".mdl") == 0) ++models;
      levels = levels || _stricmp(extension.c_str(), ".lvl") == 0;
      text = text || _stricmp(extension.c_str(), ".str") == 0;
    }
    if (levels) dlc.support = std::to_string(models) + " skins; its new levels need the title update";
    else if (text) dlc.support = std::to_string(models) + " skins; its new text needs the title update";
    else dlc.support = models > 0 ? std::to_string(models) + " skins" : "Nothing this version can use";
    installed.push_back(std::move(dlc));
  }
  std::sort(installed.begin(), installed.end(), [](const InstalledDlc& a, const InstalledDlc& b) { return a.display_name < b.display_name; });
  return installed;
}

bool RemoveDlc(const std::filesystem::path& content_root, const std::string& file_name, std::string& error) {
  std::error_code ec;
  std::filesystem::remove_all(PackageRoot(content_root) / file_name, ec);
  if (ec) {
    error = "Could not remove " + file_name + ": " + ec.message();
    return false;
  }
  std::filesystem::remove(HeaderRoot(content_root) / (file_name + ".header"), ec);
  return true;
}

std::vector<std::filesystem::path> FindPackages(const std::filesystem::path& folder_or_file) {
  std::vector<std::filesystem::path> packages;
  std::error_code ec;
  if (std::filesystem::is_regular_file(folder_or_file, ec)) {
    if (IsPackageFile(folder_or_file)) packages.push_back(folder_or_file);
    return packages;
  }
  for (auto it = std::filesystem::recursive_directory_iterator(folder_or_file, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (it->is_regular_file(ec) && IsPackageFile(it->path())) packages.push_back(it->path());
  }
  return packages;
}

DlcInstaller::~DlcInstaller() {
  if (thread_.joinable()) thread_.join();
}

void DlcInstaller::Start(std::vector<std::filesystem::path> packages, std::filesystem::path content_root) {
  if (running_.load()) return;
  if (thread_.joinable()) thread_.join();
  running_.store(true);
  done_bytes_.store(0);
  uint64_t total = 0;
  std::error_code ec;
  for (const auto& package : packages) total += std::filesystem::file_size(package, ec);
  total_bytes_.store(std::max<uint64_t>(total, 1));
  thread_ = std::thread([this, packages = std::move(packages), content_root = std::move(content_root)]() mutable { Run(std::move(packages), std::move(content_root)); });
}

float DlcInstaller::progress() const { return std::clamp(float(double(done_bytes_.load()) / double(total_bytes_.load())), 0.0f, 1.0f); }

std::string DlcInstaller::current() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return current_;
}

std::vector<std::string> DlcInstaller::TakeResults() {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::move(results_);
}

void DlcInstaller::Run(std::vector<std::filesystem::path> packages, std::filesystem::path content_root) {
  for (const auto& package : packages) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      current_ = package.filename().string();
    }
    std::string message;
    InstallOne(package, content_root, message);
    std::lock_guard<std::mutex> lock(mutex_);
    results_.push_back(message);
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    current_.clear();
  }
  done_bytes_.store(total_bytes_.load());
  running_.store(false);
}

// The same unpacking ContentManager::InstallContent does inside the game, which cannot run without a kernel: every entry of the package extracted under its file name, then the header the game lists DLC by.
bool DlcInstaller::InstallOne(const std::filesystem::path& package, const std::filesystem::path& content_root, std::string& message) {
  const std::string file_name = package.filename().string();
  std::error_code ec;
  const uint64_t package_size = std::filesystem::file_size(package, ec);
  const uint64_t done_before = done_bytes_.load();
  const auto finish = [&](bool ok, const std::string& text) {
    done_bytes_.store(done_before + package_size);
    message = text;
    return ok;
  };
  auto device = std::make_unique<rex::filesystem::StfsContainerDevice>("", package);
  if (!device->Initialize()) return finish(false, "Skipped " + file_name + ": not a readable Xbox 360 package.");
  const auto& header = device->header();
  const uint32_t title_id = header.metadata.execution_info.title_id;
  if (title_id != kKameoTitleId) {
    char text[16];
    std::snprintf(text, sizeof(text), "%08X", title_id);
    return finish(false, "Skipped " + file_name + ": it belongs to title " + text + ", not Kameo.");
  }
  if (header.metadata.content_type != rex::system::XContentType::kMarketplaceContent) return finish(false, "Skipped " + file_name + ": it is not downloadable content.");
  std::string display_name = Narrow(header.metadata.display_name(rex::system::XLanguage::kEnglish));
  if (display_name.empty()) display_name = file_name;

  const std::filesystem::path install_path = PackageRoot(content_root) / file_name;
  std::filesystem::create_directories(install_path, ec);
  if (ec) return finish(false, "Could not create " + install_path.string() + ": " + ec.message());

  rex::filesystem::Entry* root = device->ResolvePath("");
  if (!root) return finish(false, "Skipped " + file_name + ": the package has no files.");
  const auto buffer = std::make_unique<uint8_t[]>(4 * 1024 * 1024);
  uint64_t written = 0;
  std::queue<rex::filesystem::Entry*> queue;
  queue.push(root);
  while (!queue.empty()) {
    rex::filesystem::Entry* entry = queue.front();
    queue.pop();
    for (const auto& child : entry->children()) queue.push(child.get());
    std::string relative = entry->path();
    std::replace(relative.begin(), relative.end(), '\\', '/');
    const std::filesystem::path destination = install_path / std::filesystem::path(Widen(relative));
    if (entry->attributes() & rex::filesystem::kFileAttributeDirectory) {
      std::filesystem::create_directories(destination, ec);
      continue;
    }
    std::filesystem::create_directories(destination.parent_path(), ec);
    rex::filesystem::File* in_file = nullptr;
    if (entry->Open(rex::filesystem::FileAccess::kFileReadData, &in_file) != 0 || !in_file) return finish(false, "Could not read " + relative + " from " + file_name + ".");
    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    if (!out) {
      in_file->Destroy();
      return finish(false, "Could not write " + destination.string() + ".");
    }
    size_t offset = 0;
    size_t remaining = entry->size();
    while (remaining > 0) {
      size_t read = 0;
      in_file->ReadSync(std::span<uint8_t>(buffer.get(), std::min<size_t>(remaining, 4 * 1024 * 1024)), offset, &read);
      if (read == 0) break;
      out.write(reinterpret_cast<const char*>(buffer.get()), std::streamsize(read));
      offset += read;
      remaining -= read;
      written += read;
      done_bytes_.store(done_before + std::min<uint64_t>(written, package_size));
    }
    in_file->Destroy();
  }

  uint32_t license_mask = 0;
  for (const auto& license : header.header.licenses) {
    if (license.license_flags) license_mask |= license.license_bits;
  }
  rex::system::xam::XCONTENT_AGGREGATE_DATA data;
  data.device_id = static_cast<uint32_t>(rex::system::xam::DummyDeviceId::HDD);
  data.content_type = rex::system::XContentType::kMarketplaceContent;
  data.title_id = kKameoTitleId;
  data.xuid = 0;
  data.set_file_name(file_name);
  data.set_display_name(header.metadata.display_name(rex::system::XLanguage::kEnglish).empty() ? std::u16string(file_name.begin(), file_name.end()) : header.metadata.display_name(rex::system::XLanguage::kEnglish));
  std::filesystem::create_directories(HeaderRoot(content_root), ec);
  std::ofstream header_file(HeaderRoot(content_root) / (file_name + ".header"), std::ios::binary | std::ios::trunc);
  if (!header_file) return finish(false, "Unpacked " + display_name + " but could not write its header.");
  header_file.write(reinterpret_cast<const char*>(&data), sizeof(data));
  if (license_mask != 0) header_file.write(reinterpret_cast<const char*>(&license_mask), sizeof(license_mask));
  return finish(true, "Installed " + display_name + ".");
}

}  // namespace kameo::launcher
