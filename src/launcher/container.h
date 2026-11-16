// Kameo RePowered launcher: the runtime's package and disc image readers (the rexfilesystem module), and the unpacking the installers share.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include <rex/filesystem/file_system_factory.h>
#include <rex/filesystem/stfs.h>

namespace kameo::launcher {

// The runtime's factory for container readers. The first call loads the rexfilesystem module from the folder rexbase was loaded from (launcher\ for the launcher, see runtime_loader.cpp); null when it does not load.
rex::filesystem::IFileSystemFactory* FileSystemFactory();
// Stops and unloads the module; call it once, when the program is about to exit.
void ShutdownFileSystem();

// The runtime maps an image into memory, so a file cut short by a failed download, or a drive pulled out, faults in a read instead of returning an error. These return false or 0 for that.
// The container the file or folder holds, or null when it is not a readable package or disc image.
std::unique_ptr<rex::filesystem::IContainerImage> OpenImage(const std::filesystem::path& path);
// The bytes of a file node from offset on, 0 when unreadable.
size_t ReadNode(const rex::filesystem::IContainerImage& image, const rex::filesystem::ContainerNode& node, uint64_t offset, uint8_t* buffer, size_t size);

bool IsDirectory(const rex::filesystem::ContainerNode& node);
// A name comes from the image, so one that would leave the folder it is written to is treated as a damaged image.
bool IsSafeName(const std::string& name);
std::wstring Widen(const std::string& utf8);

enum class ExtractResult { kOk, kBadName, kUnreadable, kUnwritable };

// Writes every file of the image under destination, reporting the bytes written so far. failed names the entry (or the host path) that stopped it.
ExtractResult ExtractImage(const rex::filesystem::IContainerImage& image, const std::filesystem::path& destination, const std::function<void(uint64_t)>& on_bytes, std::string& failed);

}  // namespace kameo::launcher
