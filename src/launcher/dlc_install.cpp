// Kameo RePowered launcher: Xbox 360 DLC packages unpacked into the content folder the game reads.

#include "launcher/dlc_install.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

#include <rex/kernel/types/content.h>
#include <rex/platform/file.h>

#include "core/localization.h"
#include "launcher/container.h"
#include "launcher/settings_file.h"
#include "launcher/title_update.h"

namespace kameo::launcher {

using l10n::Tr;
using l10n::TrFormat;

namespace {

// ContentManager::ResolvePackageRoot and ResolvePackageHeaderPath: DLC is shared by every profile, so it sits under the zero XUID. A package's own files are in <content type>\\<file name>, and its header (the XCONTENT_AGGREGATE_DATA the game lists it by) and manifest are in Headers\\<content type>.
constexpr const char* kSharedXuid = "0000000000000000";
constexpr const char* kTitleFolder = "4D5307D2";
constexpr const char* kMarketplaceFolder = "00000002";
constexpr const char* kManifestSuffix = ".package.toml";
constexpr const char* kThumbnailFile = "__thumbnail.png";
// rex::xam::DummyDeviceID::kHDD
constexpr uint32_t kHddDeviceId = 1;

std::filesystem::path PackageRoot(const std::filesystem::path& content_root) { return content_root / kSharedXuid / kTitleFolder / kMarketplaceFolder; }

std::filesystem::path HeaderRoot(const std::filesystem::path& content_root) { return content_root / kSharedXuid / kTitleFolder / "Headers" / kMarketplaceFolder; }

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
  return rex::platform::GetUserFolder() / "kameorepowered";
}

std::vector<InstalledDlc> ListInstalledDlc(const std::filesystem::path& content_root, bool title_update) {
  std::vector<InstalledDlc> installed;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(PackageRoot(content_root), ec)) {
    if (!entry.is_directory(ec)) continue;
    InstalledDlc dlc;
    dlc.file_name = entry.path().filename().string();
    dlc.display_name = dlc.file_name;
    dlc.size_bytes = FolderSize(entry.path());
    std::ifstream header(HeaderRoot(content_root) / (dlc.file_name + ".header"), std::ios::binary);
    rex::kernel::XCONTENT_AGGREGATE_DATA data;
    if (header.read(reinterpret_cast<char*>(&data), sizeof(data))) {
      const std::string name = Narrow(data.GetDisplayName());
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
    const std::string count = std::to_string(models);
    if (levels && title_update) dlc.support = TrFormat("{0} skins, Expert mode, Rune Battle and Time Attack", {count});
    else if (levels) dlc.support = TrFormat("{0} skins; its new levels need the title update", {count});
    else if (text && !title_update) dlc.support = TrFormat("{0} skins; its new text needs the title update", {count});
    else if (text) dlc.support = TrFormat("{0} skins", {count});
    else dlc.support = models > 0 ? TrFormat("{0} skins", {count}) : std::string(Tr("Nothing this version can use"));
    installed.push_back(std::move(dlc));
  }
  std::sort(installed.begin(), installed.end(), [](const InstalledDlc& a, const InstalledDlc& b) { return a.display_name < b.display_name; });
  return installed;
}

bool RemoveDlc(const std::filesystem::path& content_root, const std::string& file_name, std::string& error) {
  std::error_code ec;
  std::filesystem::remove_all(PackageRoot(content_root) / file_name, ec);
  if (ec) {
    error = TrFormat("Could not remove {0}: {1}", {file_name, ec.message()});
    return false;
  }
  std::filesystem::remove(HeaderRoot(content_root) / (file_name + ".header"), ec);
  std::filesystem::remove(HeaderRoot(content_root) / (file_name + kManifestSuffix), ec);
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

void DlcInstaller::Start(std::vector<std::filesystem::path> packages, std::filesystem::path content_root, std::filesystem::path game_folder) {
  if (running_.load()) return;
  if (thread_.joinable()) thread_.join();
  game_folder_ = std::move(game_folder);
  running_.store(true);
  failures_.store(0);
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
    if (!InstallOne(package, content_root, message)) failures_.fetch_add(1);
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

// The same unpacking ContentManager::InstallContent does inside the game, which cannot run without a title: every file of the package extracted under its file name, then the header the game lists DLC by and the manifest that lists it among the installed packages.
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
  const auto image = OpenImage(package);
  const rex::filesystem::XContentPackageHeader* header = image ? image->GetHeader() : nullptr;
  if (!header || !header->header.IsMagicValid()) return finish(false, TrFormat("Skipped {0}: not a readable Xbox 360 package.", {file_name}));
  const uint32_t title_id = header->metadata.execution_info.title_id;
  if (title_id != kKameoTitleId) {
    char text[16];
    std::snprintf(text, sizeof(text), "%08X", title_id);
    return finish(false, TrFormat("Skipped {0}: it belongs to title {1}, not Kameo.", {file_name, text}));
  }
  if (IsTitleUpdatePackage(*image)) {
    std::string text;
    const bool ok = InstallTitleUpdate(*image, game_folder_, text);
    return finish(ok, text);
  }
  const rex::filesystem::XContentMetadata& metadata = header->metadata;
  if (static_cast<rex::kernel::XContentType>(metadata.content_type) != rex::kernel::XContentType::kMarketplaceContent) return finish(false, TrFormat("Skipped {0}: it is not downloadable content.", {file_name}));
  const std::u16string english = metadata.GetDisplayName(rex::kernel::XLanguage::kEnglish);
  std::string display_name = Narrow(english);
  if (display_name.empty()) display_name = file_name;

  const std::filesystem::path install_path = PackageRoot(content_root) / file_name;
  std::filesystem::create_directories(install_path, ec);
  if (ec) return finish(false, TrFormat("Could not create {0}: {1}", {install_path.string(), ec.message()}));

  std::string failed;
  switch (ExtractImage(*image, install_path, [&](uint64_t written) { done_bytes_.store(done_before + std::min<uint64_t>(written, package_size)); }, failed)) {
    case ExtractResult::kOk: break;
    case ExtractResult::kBadName: return finish(false, TrFormat("Could not read {0} from {1}.", {failed, file_name}));
    case ExtractResult::kUnreadable: return finish(false, TrFormat("Could not read {0} from {1}.", {failed, file_name}));
    case ExtractResult::kUnwritable: return finish(false, TrFormat("Could not write {0}.", {failed}));
  }
  if (metadata.thumbnail_size > 0) {
    const uint32_t size = std::min<uint32_t>(metadata.thumbnail_size, sizeof(metadata.thumbnail));
    std::ofstream thumbnail(install_path / kThumbnailFile, std::ios::binary | std::ios::trunc);
    thumbnail.write(reinterpret_cast<const char*>(metadata.thumbnail), size);
  }

  uint32_t license_mask = 0;
  for (const auto& license : header->header.licenses) {
    if (license.license_flags) license_mask |= license.license_bits;
  }
  rex::kernel::XCONTENT_AGGREGATE_DATA data{};
  data.device_id = kHddDeviceId;
  data.content_type = rex::kernel::XContentType::kMarketplaceContent;
  data.title_id = kKameoTitleId;
  data.xuid = 0;
  data.SetFileName(file_name);
  data.SetDisplayName(english.empty() ? std::u16string(file_name.begin(), file_name.end()) : english);
  std::filesystem::create_directories(HeaderRoot(content_root), ec);
  std::vector<uint8_t> bytes(sizeof(data) + (license_mask != 0 ? sizeof(license_mask) : 0));
  std::memcpy(bytes.data(), &data, sizeof(data));
  if (license_mask != 0) std::memcpy(bytes.data() + sizeof(data), &license_mask, sizeof(license_mask));
  std::ofstream header_file(HeaderRoot(content_root) / (file_name + ".header"), std::ios::binary | std::ios::trunc);
  if (!header_file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()))) return finish(false, TrFormat("Unpacked {0} but could not write its header.", {display_name}));
  header_file.close();

  toml::table manifest;
  manifest.insert("title_id", int64_t(kKameoTitleId));
  manifest.insert("content_type", int64_t(rex::kernel::XContentType::kMarketplaceContent));
  manifest.insert("license_mask", int64_t(license_mask));
  std::string content_id;
  for (const uint8_t byte : header->header.content_id) {
    char hex[3];
    std::snprintf(hex, sizeof(hex), "%02X", byte);
    content_id += hex;
  }
  manifest.insert("content_id", content_id);
  manifest.insert("description", Narrow(metadata.GetDescription(rex::kernel::XLanguage::kEnglish)));
  manifest.insert("publisher", Narrow(metadata.GetPublisher()));
  toml::table names;
  for (uint32_t language = 1; language <= rex::filesystem::XContentMetadata::kNumLanguagesV2; ++language) {
    const auto id = static_cast<rex::kernel::XLanguage>(language);
    if (!rex::filesystem::detail::FindLanguageSlot(id)) continue;
    const std::string name = Narrow(metadata.GetDisplayName(id));
    if (!name.empty()) names.insert(std::to_string(language), name);
  }
  manifest.insert("display_names", std::move(names));
  std::ostringstream manifest_text;
  manifest_text << manifest;
  std::ofstream manifest_file(HeaderRoot(content_root) / (file_name + kManifestSuffix), std::ios::trunc);
  manifest_file << manifest_text.str();
  return finish(true, TrFormat("Installed {0}.", {display_name}));
}

}  // namespace kameo::launcher
