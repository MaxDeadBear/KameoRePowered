// The runtime's container readers for the launcher; see container.h.

#include "launcher/container.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include <rex/init.h>
#include <rex/module.h>

namespace kameo::launcher {

namespace {

namespace fs = std::filesystem;

constexpr size_t kChunkBytes = 4 * 1024 * 1024;

bool g_started = false;

bool GuardedCall(void (*call)(void*), void* argument) {
  __try {
    call(argument);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

template <class F>
bool Guarded(F&& function) {
  return GuardedCall([](void* argument) { (*static_cast<F*>(argument))(); }, &function);
}

}  // namespace

rex::filesystem::IFileSystemFactory* FileSystemFactory() {
  static std::once_flag once;
  std::call_once(once, [] {
    rex::Config config;
    config.modules = {"rexfilesystem"};
    if (rex::Init(config)) g_started = true;
  });
  return g_started ? REX_INTERFACE(rex::filesystem::IFileSystemFactory) : nullptr;
}

void ShutdownFileSystem() {
  if (!g_started) return;
  g_started = false;
  rex::Stop();
  rex::Shutdown();
}

std::unique_ptr<rex::filesystem::IContainerImage> OpenImage(const fs::path& path) {
  rex::filesystem::IFileSystemFactory* factory = FileSystemFactory();
  if (!factory) return nullptr;
  std::unique_ptr<rex::filesystem::IContainerImage> image;
  Guarded([&] {
    auto opened = factory->OpenContainer(path);
    if (opened) image = std::move(*opened);
  });
  return image;
}

size_t ReadNode(const rex::filesystem::IContainerImage& image, const rex::filesystem::ContainerNode& node, uint64_t offset, uint8_t* buffer, size_t size) {
  size_t read = 0;
  Guarded([&] {
    auto result = image.Read(node, offset, std::span<uint8_t>(buffer, size));
    if (result) read = *result;
  });
  return read;
}

bool IsDirectory(const rex::filesystem::ContainerNode& node) { return rex::HasFlags(node.info.attributes, rex::filesystem::FileAttributes::kDirectory); }

bool IsSafeName(const std::string& name) {
  if (name.empty() || name == "." || name == "..") return false;
  return std::none_of(name.begin(), name.end(), [](char c) { return c == '/' || c == '\\' || c == ':' || uint8_t(c) < 0x20; });
}

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) return {};
  std::wstring out(size_t(::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0)), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), int(out.size()));
  return out;
}

ExtractResult ExtractImage(const rex::filesystem::IContainerImage& image, const fs::path& destination, const std::function<void(uint64_t)>& on_bytes, std::string& failed) {
  const auto nodes = image.GetNodes();
  std::error_code ec;
  const auto buffer = std::make_unique<uint8_t[]>(kChunkBytes);
  uint64_t written = 0;
  for (size_t i = 1; i < nodes.size(); ++i) {
    const rex::filesystem::ContainerNode& node = nodes[i];
    if (!IsSafeName(node.info.name)) return failed = node.info.name, ExtractResult::kBadName;
    std::string relative = image.GetNodePath(node);
    std::replace(relative.begin(), relative.end(), '\\', '/');
    const fs::path target = destination / fs::path(Widen(relative));
    if (IsDirectory(node)) {
      fs::create_directories(target, ec);
      continue;
    }
    fs::create_directories(target.parent_path(), ec);
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) return failed = target.string(), ExtractResult::kUnwritable;
    uint64_t offset = 0;
    while (offset < node.info.size) {
      const size_t read = ReadNode(image, node, offset, buffer.get(), size_t(std::min<uint64_t>(node.info.size - offset, kChunkBytes)));
      if (read == 0) return failed = relative, ExtractResult::kUnreadable;
      out.write(reinterpret_cast<const char*>(buffer.get()), std::streamsize(read));
      if (!out) return failed = target.string(), ExtractResult::kUnwritable;
      offset += read;
      written += read;
      on_bytes(written);
    }
  }
  return ExtractResult::kOk;
}

}  // namespace kameo::launcher
