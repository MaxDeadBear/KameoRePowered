#include "gpu/shaders/shader_mods.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
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
// Version 1 packages have no mesh keys, and their mesh_count is zero.
constexpr uint32_t kOldestVersion = 1;
constexpr uint32_t kVersion = 2;
constexpr const char* kFolder = "shader_mods";
constexpr const char* kExtension = ".kshader";
// How often the folder is looked at; at 60 fps, twice a second, so an edit shows without being looked for every frame.
constexpr uint32_t kPollFrames = 30;
constexpr double kTimeWrapSeconds = 3600.0;

std::atomic<float> g_time{0.0f};
std::atomic<bool> g_anyLoaded{false};
using MeshKeys = std::unordered_set<uint64_t>;
// The meshes each cache entry's stand-in is for, by index; null is every mesh. Kept for the session, as the game thread reads them.
std::unique_ptr<std::atomic<const MeshKeys*>[]> g_meshKeysByIndex;
std::vector<std::unique_ptr<MeshKeys>> g_meshKeys;

// A package with mesh keys is one of Kameo's costumes: its stand-ins are swapped in only while it's the one chosen.
struct Costume {
  std::u16string name;
  const MeshKeys* keys = nullptr;
  std::vector<std::pair<size_t, plume::RenderShader*>> standins;
};
std::vector<Costume> g_costumes;
// What each shader draws with when no costume covers it: the packages without mesh keys, or null for the game's own.
std::vector<plume::RenderShader*> g_baseStandins;
int g_appliedCostume = -1;

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

bool ShaderModSkipsDraw(const ShaderCacheEntry* ps, const uint8_t* vertices, uint32_t bytes) {
  if (!ps || !g_meshKeysByIndex) return false;
  const size_t index = size_t(ps - g_shaderCacheEntries);
  if (index >= g_shaderCacheEntryCount) return false;
  const MeshKeys* keys = g_meshKeysByIndex[index].load(std::memory_order_relaxed);
  if (!keys) return false;
  // A mod for her everyday outfit is a costume of its own, chosen in the Bonus pages. Her other costumes reuse parts of
  // that outfit, eyes and hands among them, so the meshes alone can't say which look this is.
  if (!engine::KameoWearsShaderSkin()) return true;
  return !vertices || bytes < kShaderModMeshKeyBytes || !keys->contains(XXH3_64bits(vertices, kShaderModMeshKeyBytes));
}

float ShaderModTime() { return g_anyLoaded.load(std::memory_order_relaxed) ? g_time.load(std::memory_order_relaxed) : 0.0f; }

void GraphicsSystem::UpdateShaderMods() {
  static const auto start = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  g_time.store(float(std::fmod(seconds, kTimeWrapSeconds)), std::memory_order_relaxed);

  if (!device_) return;
  if (!shader_mod_by_index_) {
    shader_mod_by_index_ = std::make_unique<std::atomic<plume::RenderShader*>[]>(g_shaderCacheEntryCount);
    g_meshKeysByIndex = std::make_unique<std::atomic<const MeshKeys*>[]>(g_shaderCacheEntryCount);
    g_baseStandins.assign(g_shaderCacheEntryCount, nullptr);
  }
  const auto apply_costume = [this](int chosen) {
    if (chosen >= int(g_costumes.size())) chosen = -1;
    if (g_appliedCostume >= 0) {
      for (const auto& [index, module] : g_costumes[g_appliedCostume].standins) {
        shader_mod_by_index_[index].store(g_baseStandins[index], std::memory_order_relaxed);
        g_meshKeysByIndex[index].store(nullptr, std::memory_order_relaxed);
      }
    }
    if (chosen >= 0) {
      const Costume& costume = g_costumes[chosen];
      for (const auto& [index, module] : costume.standins) {
        g_meshKeysByIndex[index].store(costume.keys, std::memory_order_relaxed);
        shader_mod_by_index_[index].store(module, std::memory_order_relaxed);
      }
    }
    g_appliedCostume = chosen;
  };
  if (const int chosen = engine::KameoShaderSkinChosen(); chosen != g_appliedCostume) apply_costume(chosen);
  if (shader_mod_frames_++ % kPollFrames != 0) return;
  const std::vector<fs::path> files = ListPackages();
  const uint64_t stamp = StampOf(files);
  if (stamp == shader_mod_stamp_) return;
  shader_mod_stamp_ = stamp;

  apply_costume(-1);
  g_costumes.clear();
  for (size_t i = 0; i < g_shaderCacheEntryCount; ++i) {
    shader_mod_by_index_[i].store(nullptr, std::memory_order_relaxed);
    g_baseStandins[i] = nullptr;
  }
  const bool spirv = interface_->getCapabilities().shaderFormat == plume::RenderShaderFormat::SPIRV;
  uint32_t standins = 0;
  uint32_t unknown = 0;
  for (const fs::path& path : files) {
    const std::vector<uint8_t> bytes = ReadFile(path);
    ShaderModHeader header;
    if (bytes.size() < sizeof(header)) continue;
    std::memcpy(&header, bytes.data(), sizeof(header));
    const uint64_t keys_at = sizeof(header) + uint64_t(header.count) * sizeof(ShaderModEntry);
    if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 || header.version < kOldestVersion || header.version > kVersion || keys_at + uint64_t(header.mesh_count) * sizeof(uint64_t) > bytes.size()) {
      KGFX_WARN("shader mod {} is not a package this build reads", path.filename().string());
      continue;
    }
    Costume* costume = nullptr;
    if (header.mesh_count) {
      auto set = std::make_unique<MeshKeys>();
      for (uint32_t i = 0; i < header.mesh_count; ++i) {
        uint64_t key;
        std::memcpy(&key, bytes.data() + keys_at + i * sizeof(key), sizeof(key));
        set->insert(key);
      }
      // The menu names the costume after its file.
      costume = &g_costumes.emplace_back(Costume{path.stem().u16string(), set.get(), {}});
      g_meshKeys.push_back(std::move(set));
    }
    for (uint32_t i = 0; i < header.count; ++i) {
      ShaderModEntry entry;
      std::memcpy(&entry, bytes.data() + sizeof(header) + i * sizeof(entry), sizeof(entry));
      const uint32_t offset = spirv ? entry.spirv_offset : entry.dxil_offset;
      const uint32_t size = spirv ? entry.spirv_size : entry.dxil_size;
      const ShaderCacheEntry* game = FindEntry(entry.game_hash);
      if (!game) ++unknown;
      if (!game || size == 0 || uint64_t(offset) + size > bytes.size()) continue;
      auto module = device_->createShader(bytes.data() + offset, size, "main", spirv ? plume::RenderShaderFormat::SPIRV : plume::RenderShaderFormat::DXIL);
      if (!module) continue;
      const size_t index = size_t(game - g_shaderCacheEntries);
      if (costume) {
        costume->standins.emplace_back(index, module.get());
      } else {
        g_baseStandins[index] = module.get();
        shader_mod_by_index_[index].store(module.get(), std::memory_order_relaxed);
      }
      // Pipelines are cached by module address, so a replaced stand-in is kept for the session: its address must never come back as another's.
      shader_mod_modules_.push_back(std::move(module));
      ++standins;
    }
    if (costume && costume->standins.empty()) g_costumes.pop_back();
  }
  std::vector<std::u16string> names;
  for (const Costume& costume : g_costumes) names.push_back(costume.name);
  engine::SetKameoShaderSkins(std::move(names));
  apply_costume(engine::KameoShaderSkinChosen());
  g_anyLoaded.store(standins != 0, std::memory_order_relaxed);
  KGFX_INFO("shader mods: {} stand-ins from {} packages, {} for shaders this build doesn't have", standins, files.size(), unknown);
}

}  // namespace kameo::gpu
