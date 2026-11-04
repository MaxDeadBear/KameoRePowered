#include "gpu/shaders/shader_mods.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <plume_render_interface.h>
#define XXH_INLINE_ALL
#include <xxhash.h>

#include <rex/filesystem.h>

#include "core/game_archive.h"
#include "engine/dlc/dlc.h"
#include "gpu/graphics_system.h"
#include "gpu/log.h"
#include "gpu/sampler_cache.h"
#include "gpu/shaders/shader_cache.h"
#include "gpu/textures/texture_heap.h"

namespace kameo::gpu {

namespace fs = std::filesystem;

// A package's game textures: read from the archives on a thread of their own, then made on the render thread, each slot
// filled in as its texture is ready.
struct ShaderModTextureSet {
  std::array<std::atomic<uint32_t>, kShaderModTextureSlots> slots{};
};

namespace {

constexpr char kMagic[4] = {'K', 'S', 'H', 'D'};
// Version 1 packages have no mesh keys, and their mesh_count is zero; version 2 ones have no model_type; version 3 ones
// draw with none of the game's textures; version 4 ones have no flags.
constexpr uint32_t kOldestVersion = 1;
constexpr uint32_t kVersion = 5;
// MOD_SHOW_* in shader_mod.hlsli.
constexpr uint32_t kShowDither = 0, kShowAlpha = 1, kShowCoverage = 2, kShowAdditive = 3;
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
  const ShaderModTextureSet* textures;
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
// The textures of each cache entry's every-draw stand-in, by index, or null.
std::unique_ptr<std::atomic<const ShaderModTextureSet*>[]> g_texturesByIndex;

// The stand-ins of see-through mods. Modules stay for the session, so the set only grows.
std::mutex g_seeThroughMutex;
std::unordered_set<const plume::RenderShader*> g_seeThrough;

std::mutex g_dataRootMutex;
fs::path g_dataRoot;
std::vector<std::unique_ptr<ShaderModTextureSet>> g_textureSets;
struct PendingTexture {
  ShaderModTextureSet* set;
  uint32_t slot;
  game_archive::Texture pixels;
};
std::mutex g_pendingMutex;
std::vector<PendingTexture> g_pendingTextures;

// Reads a package's textures out of the player's own copies of the archives they name, each archive once; a slot whose
// texture isn't found stays empty and draws transparent black.
void ReadTextures(ShaderModTextureSet* set, std::vector<ShaderModTexture> wanted, std::string package) {
  fs::path root;
  {
    std::lock_guard lock(g_dataRootMutex);
    root = g_dataRoot;
  }
  std::thread([set, wanted = std::move(wanted), package = std::move(package), root = std::move(root)] {
    std::map<std::string, std::unique_ptr<game_archive::Archive>> archives;
    for (uint32_t slot = 0; slot < wanted.size() && slot < kShaderModTextureSlots; ++slot) {
      const ShaderModTexture& want = wanted[slot];
      const std::string name(want.archive, strnlen(want.archive, sizeof(want.archive)));
      if (!archives.contains(name)) {
        auto archive = std::make_unique<game_archive::Archive>();
        std::string error;
        if (!archive->Load(root / name, error)) {
          KGFX_WARN("shader mod {}: could not read {} for its textures: {}", package, name, error);
          archive.reset();
        }
        archives[name] = std::move(archive);
      }
      const game_archive::Archive* archive = archives[name].get();
      if (!archive) continue;
      std::optional<game_archive::Texture> texture;
      if (want.entry && want.entry <= archive->entry_count() && archive->TypeName(want.entry) == "texture") texture = game_archive::ReadTexture(*archive, {want.entry, 0});
      if (!texture || texture->width != want.width || texture->height != want.height) {
        KGFX_WARN("shader mod {}: {} has no {}x{} texture at entry {}", package, name, want.width, want.height, want.entry);
        continue;
      }
      std::lock_guard lock(g_pendingMutex);
      g_pendingTextures.push_back({set, slot, std::move(*texture)});
    }
  }).detach();
}

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

plume::RenderShader* ShaderModCostumeStandIn(const ShaderCacheEntry* ps, const uint8_t* vertices, uint32_t bytes, const ShaderModTextureSet** textures) {
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
    if (costume.keys->contains(key)) {
      if (textures) *textures = costume.textures;
      return costume.module;
    }
  }
  return nullptr;
}

const ShaderModTextureSet* ShaderModTexturesFor(const ShaderCacheEntry* ps) {
  if (!ps || !g_texturesByIndex) return nullptr;
  const size_t index = size_t(ps - g_shaderCacheEntries);
  return index < g_shaderCacheEntryCount ? g_texturesByIndex[index].load(std::memory_order_acquire) : nullptr;
}

void ShaderModTextureSlots(const ShaderModTextureSet* set, uint32_t out[kShaderModTextureSlots]) {
  for (uint32_t i = 0; i < kShaderModTextureSlots; ++i) out[i] = set ? set->slots[i].load(std::memory_order_acquire) : 0;
}

bool ShaderModSeeThrough(const plume::RenderShader* module) {
  if (!module) return false;
  std::lock_guard lock(g_seeThroughMutex);
  return g_seeThrough.contains(module);
}

uint32_t ShaderModShow(bool see_through, uint32_t blend_control, bool alpha_to_mask, bool multisampled) {
  // Every stand-in reads it, and one whose mod shows all of itself is unchanged by the draw's own alpha.
  if (!see_through) return kShowAlpha;
  const uint32_t src = blend_control & 0x1F, op = (blend_control >> 5) & 0x7, dst = (blend_control >> 8) & 0x1F;
  const bool blended = !(src == 1 && dst == 0 && op == 0);
  if (blended) return src == 1 ? kShowAdditive : kShowAlpha;  // ONE: additive or premultiplied, so colour fades too
  if (alpha_to_mask) return kShowAlpha;  // the game's own alpha already covers samples
  return multisampled ? kShowCoverage : kShowDither;
}

void SetShaderModDataRoot(const fs::path& root) {
  std::lock_guard lock(g_dataRootMutex);
  g_dataRoot = root;
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
    g_texturesByIndex = std::make_unique<std::atomic<const ShaderModTextureSet*>[]>(g_shaderCacheEntryCount);
  }
  if (shader_mod_frames_++ % kPollFrames != 0) return;
  const std::vector<fs::path> files = ListPackages();
  const uint64_t stamp = StampOf(files);
  if (stamp == shader_mod_stamp_) return;
  shader_mod_stamp_ = stamp;

  for (size_t i = 0; i < g_shaderCacheEntryCount; ++i) {
    shader_mod_by_index_[i].store(nullptr, std::memory_order_relaxed);
    g_texturesByIndex[i].store(nullptr, std::memory_order_relaxed);
  }
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
    const uint64_t header_bytes = header.version >= 5 ? sizeof(header) : header.version >= 4 ? kShaderModHeaderV4Bytes : header.version >= 3 ? kShaderModHeaderV3Bytes : kShaderModHeaderV2Bytes;
    if (bytes.size() < header_bytes) continue;
    std::memcpy(&header, bytes.data(), size_t(header_bytes));
    if (header.version < 3) header.model_type = kKameoModelType;
    if (header.version < 4) header.texture_count = 0;
    if (header.version < 5) header.flags = 0;
    const uint64_t keys_at = header_bytes + uint64_t(header.count) * sizeof(ShaderModEntry);
    const uint64_t textures_at = keys_at + uint64_t(header.mesh_count) * sizeof(uint64_t);
    if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 || header.version < kOldestVersion || header.version > kVersion || header.texture_count > kShaderModTextureSlots || textures_at + uint64_t(header.texture_count) * sizeof(ShaderModTexture) > bytes.size()) {
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
    ShaderModTextureSet* textures = nullptr;
    if (header.texture_count) {
      std::vector<ShaderModTexture> wanted(header.texture_count);
      std::memcpy(wanted.data(), bytes.data() + textures_at, wanted.size() * sizeof(ShaderModTexture));
      textures = g_textureSets.emplace_back(std::make_unique<ShaderModTextureSet>()).get();
      ReadTextures(textures, std::move(wanted), path.filename().string());
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
        costumes_by_index[index].push_back({keys, module.get(), textures, header.model_type, costume});
      } else {
        shader_mod_by_index_[index].store(module.get(), std::memory_order_relaxed);
        g_texturesByIndex[index].store(textures, std::memory_order_relaxed);
      }
      if (header.flags & kShaderModSeeThrough) {
        std::lock_guard lock(g_seeThroughMutex);
        g_seeThrough.insert(module.get());
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

namespace kameo::gpu::detail {

namespace {

// The textures shader mods draw with, made on the render thread and kept for the session.
struct HeapTexture {
  std::unique_ptr<plume::RenderTexture> texture;
  std::unique_ptr<plume::RenderTextureView> view;
  std::unique_ptr<plume::RenderBuffer> upload;
};
std::vector<HeapTexture> g_heapTextures;

}  // namespace

void UploadShaderModTextures(GraphicsSystem::DrawPathContext& ctx, plume::RenderCommandList* list) {
  std::vector<PendingTexture> ready;
  {
    std::lock_guard lock(g_pendingMutex);
    if (g_pendingTextures.empty()) return;
    ready.swap(g_pendingTextures);
  }
  constexpr plume::RenderFormat kFormat = plume::RenderFormat::R8G8B8A8_UNORM;
  for (PendingTexture& pending : ready) {
    const uint32_t width = pending.pixels.width, height = pending.pixels.height;
    // Rows padded to 256 bytes, as a copy from a buffer wants them.
    const uint32_t row = (width + 63) / 64 * 64;
    HeapTexture made;
    made.texture = ctx.device->createTexture(plume::RenderTextureDesc::Texture2D(width, height, 1, kFormat));
    made.upload = ctx.device->createBuffer(plume::RenderBufferDesc::UploadBuffer(uint64_t(row) * 4 * height));
    if (!made.texture || !made.upload) continue;
    made.view = made.texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(kFormat));
    auto* mapped = static_cast<uint8_t*>(made.upload->map());
    if (!mapped || !made.view) continue;
    for (uint32_t y = 0; y < height; ++y) std::memcpy(mapped + size_t(y) * row * 4, pending.pixels.rgba.data() + size_t(y) * width * 4, size_t(width) * 4);
    made.upload->unmap();
    const uint32_t slot = g_textureSlots.allocate();
    if (slot == kNoSlot) continue;
    list->barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(made.texture.get(), plume::RenderTextureLayout::COPY_DEST));
    list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(made.texture.get(), 0, 0), plume::RenderTextureCopyLocation::PlacedFootprint(made.upload.get(), kFormat, width, height, 1, row, 0), 0, 0, 0, nullptr);
    list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(made.texture.get(), plume::RenderTextureLayout::SHADER_READ));
    ctx.texture_set->setTexture(slot, made.texture.get(), plume::RenderTextureLayout::SHADER_READ, made.view.get());
    pending.set->slots[pending.slot].store(slot, std::memory_order_release);
    g_heapTextures.push_back(std::move(made));
  }
}

uint32_t ShaderModSampler(GraphicsSystem::DrawPathContext& ctx) {
  // Linear and repeating, for patterns laid over a surface.
  static const GuestTextureFetch fetch = [] {
    GuestTextureFetch f;
    f.xe.mag_filter = decltype(f.xe.mag_filter)(1);
    f.xe.min_filter = decltype(f.xe.min_filter)(1);
    return f;
  }();
  return EnsureSampler(ctx, fetch, false);
}

}  // namespace kameo::gpu::detail
