#include "launcher/zip_extract.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

#include "launcher/kmod.h"

namespace kameo::launcher {
namespace {

uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t U32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

constexpr uint32_t kEndOfCentralDirectory = 0x06054B50;
constexpr uint32_t kCentralFileHeader = 0x02014B50;
constexpr uint32_t kLocalFileHeader = 0x04034B50;

// A relative path that stays inside the destination: no drive, no root, no "..".
bool SafeEntryPath(const std::string& name, std::filesystem::path& out) {
  if (name.empty() || name.find(':') != std::string::npos || name[0] == '/' || name[0] == '\\') return false;
  std::filesystem::path path;
  for (const auto& part : std::filesystem::path(std::u8string(name.begin(), name.end()))) {
    const std::u8string text = part.u8string();
    if (text.empty() || text == u8".") continue;
    if (text == u8"..") return false;
    path /= part;
  }
  if (path.empty()) return false;
  out = path;
  return true;
}

}  // namespace

bool ExtractZip(const std::filesystem::path& zip, const std::filesystem::path& folder, std::string& error) {
  std::ifstream in(zip, std::ios::binary);
  if (!in) {
    error = "could not open " + zip.string();
    return false;
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (data.size() < 22) {
    error = "the download is not a zip file";
    return false;
  }
  // The end-of-central-directory record is the last thing in the file, before a comment of up to 64 KB.
  size_t end = std::string::npos;
  for (size_t at = data.size() - 22 + 1; at-- > 0 && data.size() - at <= 22 + 0xFFFF;) {
    if (U32(&data[at]) == kEndOfCentralDirectory) {
      end = at;
      break;
    }
  }
  if (end == std::string::npos) {
    error = "the download is not a zip file";
    return false;
  }
  const uint16_t count = U16(&data[end + 10]);
  size_t at = U32(&data[end + 16]);
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  for (uint16_t i = 0; i < count; ++i) {
    if (at + 46 > data.size() || U32(&data[at]) != kCentralFileHeader) {
      error = "the zip's directory is damaged";
      return false;
    }
    const uint16_t method = U16(&data[at + 10]);
    const uint32_t packed_size = U32(&data[at + 20]);
    const uint32_t size = U32(&data[at + 24]);
    const uint16_t name_length = U16(&data[at + 28]);
    const uint16_t extra_length = U16(&data[at + 30]);
    const uint16_t comment_length = U16(&data[at + 32]);
    const uint32_t local = U32(&data[at + 42]);
    if (at + 46 + name_length > data.size()) {
      error = "the zip's directory is damaged";
      return false;
    }
    const std::string name(reinterpret_cast<const char*>(&data[at + 46]), name_length);
    at += 46 + size_t(name_length) + extra_length + comment_length;
    if (packed_size == 0xFFFFFFFFu || size == 0xFFFFFFFFu) {
      error = name + " is stored as ZIP64, which this launcher does not read";
      return false;
    }
    std::filesystem::path relative;
    if (!SafeEntryPath(name, relative)) {
      error = "the zip names a file outside its folder (" + name + ")";
      return false;
    }
    const std::filesystem::path target = folder / relative;
    if (!name.empty() && (name.back() == '/' || name.back() == '\\')) {
      std::filesystem::create_directories(target, ec);
      continue;
    }
    if (size_t(local) + 30 > data.size() || U32(&data[local]) != kLocalFileHeader) {
      error = "the zip entry " + name + " is damaged";
      return false;
    }
    const size_t start = size_t(local) + 30 + U16(&data[local + 26]) + U16(&data[local + 28]);
    if (start + packed_size > data.size()) {
      error = "the zip entry " + name + " runs past the end of the file";
      return false;
    }
    std::vector<uint8_t> contents;
    if (method == 0) {
      contents.assign(data.begin() + std::ptrdiff_t(start), data.begin() + std::ptrdiff_t(start + packed_size));
    } else if (method == 8) {
      if (!kmod::inflate_raw(&data[start], packed_size, size, contents)) {
        error = "the zip entry " + name + " could not be unpacked";
        return false;
      }
    } else {
      error = "the zip entry " + name + " uses compression method " + std::to_string(method) + ", which this launcher does not read";
      return false;
    }
    std::filesystem::create_directories(target.parent_path(), ec);
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(contents.data()), std::streamsize(contents.size()));
    if (!out) {
      error = "could not write " + target.string();
      return false;
    }
  }
  return true;
}

}  // namespace kameo::launcher
