#include "gpu/shaders/shader_mods.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <plume_render_interface.h>
#define XXH_INLINE_ALL
#include <xxhash.h>

#include <rex/filesystem.h>

#include "engine/dlc/dlc.h"
#include "gpu/graphics_system.h"
#include "gpu/log.h"
#include "gpu/shaders/shader_cache.h"

namespace kameo::gpu {

namespace fs = std::filesystem;

namespace {

constexpr char kMagic[4] = {'K', 'S', 'H', 'D'};
// Version 1 packages have no mesh keys, and their mesh_count is zero; version 2 ones have no model_type.
constexpr uint32_t kOldestVersion = 1;
constexpr uint32_t kVersion = 3;
constexpr uint32_t kKameoModelType = 0x007183BF;
constexpr const char* kFolder = "shader_mods";
constexpr const char* kExtension = ".kshader";
// How often the folder is looked at; at 60 fps, twice a second, so an edit shows without being looked for every frame.
constexpr uint32_t kPollFrames = 30;
constexpr double kTimeWrapSeconds = 3600.0;

std::atomic<float> g_time{0.0f};
std::atomic<bool> g_anyLoaded{false};
using MeshKeys = std::unordered_set<uint64_t>;

// A package with mesh keys is a costume for one character: its stand-ins draw only those meshes, only while the
// character wears it. Characters share shaders, so one shader can have stand-ins from several costumes.
struct CostumeStandIn {
  const MeshKeys* keys;
  plume::RenderShader* module;
  uint32_t model_type;
  // Its place among that character's costumes, as the engine counts them.
  int costume;
};
using CostumeStandIns = std::vector<CostumeStandIn>;
// Each cache entry's costume stand-ins, by index, or null. The game thread reads them as it records draws, so every
// list, key set and module stays for the session and a reload only swaps the pointers.
std::unique_ptr<std::atomic<const CostumeStandIns*>[]> g_costumesByIndex;
std::vector<std::unique_ptr<MeshKeys>> g_meshKeys;
std::vector<std::unique_ptr<CostumeStandIns>> g_costumeLists;

std::vector<fs::path> ListPackages() {
  std::vector<fs::path> files;
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(rex::filesystem::GetExecutableFolder() / kFolder, error)) {
    if (entry.is_regular_file(error) && entry.path().extension() == kExtension) files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());
  return files;
}

// Changes when a package is added, removed or written. The editor renames a finished file into place, so a change is never half a file.
uint64_t StampOf(const std::vector<fs::path>& files) {
  uint64_t stamp = 1469598103934665603ull;
  auto mix = [&stamp](uint64_t v) { stamp = (stamp ^ v) * 1099511628211ull; };
  for (const fs::path& file : files) {
    std::error_code error;
    mix(std::hash<std::string>{}(file.string()));
    mix(fs::file_size(file, error));
    mix(uint64_t(fs::last_write_time(file, error).time_since_epoch().count()));
  }
  return stamp;
}

std::vector<uint8_t> ReadFile(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}

const ShaderCacheEntry* FindEntry(uint64_t hash) {
  const ShaderCacheEntry* begin = g_shaderCacheEntries;
  const ShaderCacheEntry* end = g_shaderCacheEntries + g_shaderCacheEntryCount;
  const ShaderCacheEntry* found = std::lower_bound(begin, end, hash, [](const ShaderCacheEntry& lhs, uint64_t rhs) { return lhs.hash < rhs; });
  return found != end && found->hash == hash ? found : nullptr;
}

}  // namespace

plume::RenderShader* ShaderModCostumeStandIn(const ShaderCacheEntry* ps, const uint8_t* vertices, uint32_t bytes) {
  if (!ps || !g_costumesByIndex || !vertices || bytes < kShaderModMeshKeyBytes) return nullptr;
  const size_t index = size_t(ps - g_shaderCacheEntries);
  if (index >= g_shaderCacheEntryCount) return nullptr;
  const CostumeStandIns* costumes = g_costumesByIndex[index].load(std::memory_order_acquire);
  if (!costumes) return nullptr;
  uint64_t key = 0;
  for (const CostumeStandIn& costume : *costumes) {
    // The character's other skins reuse parts of its standard look (Kameo's costumes, her eyes and hands), so the
    // meshes alone can't say which look this is.
    if (engine::ShaderSkinChosen(costume.model_type) != costume.costume || !engine::WearsShaderSkin(costume.model_type)) continue;
    if (!key) key = XXH3_64bits(vertices, kShaderModMeshKeyBytes);
    if (costume.keys->contains(key)) return costume.module;
  }
  return nullptr;
}

float ShaderModTime() { return g_anyLoaded.load(std::memory_order_relaxed) ? g_time.load(std::memory_order_relaxed) : 0.0f; }

void GraphicsSystem::UpdateShaderMods() {
  static const auto start = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  g_time.store(float(std::fmod(seconds, kTimeWrapSeconds)), std::memory_order_relaxed);

  if (!device_) return;
  if (!shader_mod_by_index_) {
    shader_mod_by_index_ = std::make_unique<std::atomic<plume::RenderShader*>[]>(g_shaderCacheEntryCount);
    g_costumesByIndex = std::make_unique<std::atomic<const CostumeStandIns*>[]>(g_shaderCacheEntryCount);
  }
  if (shader_mod_frames_++ % kPollFrames != 0) return;
  const std::vector<fs::path> files = ListPackages();
  const uint64_t stamp = StampOf(files);
  if (stamp == shader_mod_stamp_) return;
  shader_mod_stamp_ = stamp;

  for (size_t i = 0; i < g_shaderCacheEntryCount; ++i) shader_mod_by_index_[i].store(nullptr, std::memory_order_relaxed);
  std::unordered_map<size_t, CostumeStandIns> costumes_by_index;
  std::vector<std::pair<uint32_t, std::u16string>> costumes;
  std::unordered_map<uint32_t, int> costumes_per_model;
  const bool spirv = interface_->getCapabilities().shaderFormat == plume::RenderShaderFormat::SPIRV;
  uint32_t standins = 0;
  uint32_t unknown = 0;
  for (const fs::path& path : files) {
    const std::vector<uint8_t> bytes = ReadFile(path);
    ShaderModHeader header{};
    if (bytes.size() < kShaderModHeaderV2Bytes) continue;
    std::memcpy(&header, bytes.data(), kShaderModHeaderV2Bytes);
    const uint64_t header_bytes = header.version >= 3 ? sizeof(header) : kShaderModHeaderV2Bytes;
    if (bytes.size() < header_bytes) continue;
    std::memcpy(&header, bytes.data(), size_t(header_bytes));
    if (header.version < 3) header.model_type = kKameoModelType;
    const uint64_t keys_at = header_bytes + uint64_t(header.count) * sizeof(ShaderModEntry);
    if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 || header.version < kOldestVersion || header.version > kVersion || keys_at + uint64_t(header.mesh_count) * sizeof(uint64_t) > bytes.size()) {
      KGFX_WARN("shader mod {} is not a package this build reads", path.filename().string());
      continue;
    }
    const MeshKeys* keys = nullptr;
    if (header.mesh_count) {
      auto set = std::make_unique<MeshKeys>();
      for (uint32_t i = 0; i < header.mesh_count; ++i) {
        uint64_t key;
        std::memcpy(&key, bytes.data() + keys_at + i * sizeof(key), sizeof(key));
        set->insert(key);
      }
      keys = set.get();
      g_meshKeys.push_back(std::move(set));
    }
    const int costume = keys ? costumes_per_model[header.model_type] : -1;
    uint32_t made = 0;
    for (uint32_t i = 0; i < header.count; ++i) {
      ShaderModEntry entry;
      std::memcpy(&entry, bytes.data() + header_bytes + i * sizeof(entry), sizeof(entry));
      const uint32_t offset = spirv ? entry.spirv_offset : entry.dxil_offset;
      const uint32_t size = spirv ? entry.spirv_size : entry.dxil_size;
      const ShaderCacheEntry* game = FindEntry(entry.game_hash);
      if (!game) ++unknown;
      if (!game || size == 0 || uint64_t(offset) + size > bytes.size()) continue;
      auto module = device_->createShader(bytes.data() + offset, size, "main", spirv ? plume::RenderShaderFormat::SPIRV : plume::RenderShaderFormat::DXIL);
      if (!module) continue;
      const size_t index = size_t(game - g_shaderCacheEntries);
      if (keys) {
        costumes_by_index[index].push_back({keys, module.get(), header.model_type, costume});
      } else {
        shader_mod_by_index_[index].store(module.get(), std::memory_order_relaxed);
      }
      // Pipelines are cached by module address, so a replaced stand-in is kept for the session: its address must never come back as another's.
      shader_mod_modules_.push_back(std::move(module));
      ++standins;
      ++made;
    }
    // The menu names a costume after its file.
    if (keys && made) {
      costumes.emplace_back(header.model_type, path.stem().u16string());
      ++costumes_per_model[header.model_type];
    }
  }
  for (size_t i = 0; i < g_shaderCacheEntryCount; ++i) {
    const auto found = costumes_by_index.find(i);
    const CostumeStandIns* list = nullptr;
    if (found != costumes_by_index.end()) list = g_costumeLists.emplace_back(std::make_unique<CostumeStandIns>(std::move(found->second))).get();
    g_costumesByIndex[i].store(list, std::memory_order_release);
  }
  engine::SetShaderSkins(std::move(costumes));
  g_anyLoaded.store(standins != 0, std::memory_order_relaxed);
  KGFX_INFO("shader mods: {} stand-ins from {} packages, {} for shaders this build doesn't have", standins, files.size(), unknown);
}

}  // namespace kameo::gpu
