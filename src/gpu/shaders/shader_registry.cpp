#include "gpu/shaders/shader_registry.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <kameorepowered_init.h>

#include <rex/logging.h>

#include "gpu/graphics_system.h"
#include "gpu/log.h"
#include "gpu/shaders/shader_cache.h"
#include "gpu/shaders/shader_dump.h"
#include "gpu/shaders/ucode_scan.h"

// Shaders are bound to the cache when the guest creates them, not when it draws: XGRegisterVertexShader patches vertex fetch into the bound microcode per declaration, so only the authored container hashes to a cache entry.

namespace kameo::gpu {

namespace {

std::mutex g_shaderMutex;
// Guest shader object -> cache entry; a map because shader objects are freed and their addresses reused, and only the newest binding is right.
std::unordered_map<uint32_t, const ShaderCacheEntry*> g_shaderBindings;

// Entries are sorted by hash, exactly as UnleashedRecomp's FindShaderCacheEntry assumes.
const ShaderCacheEntry* FindShaderCacheEntry(uint64_t hash) {
  const ShaderCacheEntry* begin = g_shaderCacheEntries;
  const ShaderCacheEntry* end = g_shaderCacheEntries + g_shaderCacheEntryCount;
  const ShaderCacheEntry* found = std::lower_bound(
      begin, end, hash,
      [](const ShaderCacheEntry& lhs, uint64_t rhs) { return lhs.hash < rhs; });
  return (found != end && found->hash == hash) ? found : nullptr;
}

// Per cache entry, indexed the same way, so the capture path can read a range without taking g_shaderMutex.
std::atomic<ShaderConstantRange*> g_shaderConstantRanges{nullptr};
std::atomic<ShaderMaterialRegisters*> g_shaderMaterialRegisters{nullptr};

}  // namespace

// The container header is big-endian in guest memory, but the hash is over the raw bytes as stored, because the cache was generated from a dump of that same memory.
void RegisterShader(uint8_t* base, uint32_t function, uint32_t object, const char* what) {
  if (!function || !object) {
    return;
  }
  const uint32_t virtual_size = REX_LOAD_U32(function + 4);
  const uint32_t physical_size = REX_LOAD_U32(function + 8);
  const uint64_t total = uint64_t(virtual_size) + physical_size;
  if (total == 0 || total > 0x100000) {
    KGFX_WARN("{} container at {:08X} has implausible size {}", what, function,
                total);
    return;
  }

  const uint64_t hash = XXH3_64bits(base + function, size_t(total));
  const ShaderCacheEntry* entry = FindShaderCacheEntry(hash);

  std::lock_guard<std::mutex> lock(g_shaderMutex);
  if (entry) {
    g_shaderBindings.insert_or_assign(object, entry);
    ShaderConstantRange* ranges = g_shaderConstantRanges.load(std::memory_order_acquire);
    if (!ranges) {
      ranges = new ShaderConstantRange[g_shaderCacheEntryCount];
      g_shaderConstantRanges.store(ranges, std::memory_order_release);
    }
    const ShaderConstantRange range = ScanShaderConstantRange(base + function + virtual_size, physical_size);
    ranges[size_t(entry - g_shaderCacheEntries)] = range;
    ShaderMaterialRegisters* materials = g_shaderMaterialRegisters.load(std::memory_order_acquire);
    if (!materials) {
      materials = new ShaderMaterialRegisters[g_shaderCacheEntryCount];
      g_shaderMaterialRegisters.store(materials, std::memory_order_release);
    }
    if ((REX_LOAD_U32(function) & 0xFF) == 0) materials[size_t(entry - g_shaderCacheEntries)] = ScanMaterialRegisters(base, function, virtual_size);
    static uint32_t range_logs = 0;
    if (range_logs < 16) {
      ++range_logs;
      KGFX_INFO("{} {:08X} reads constants c{}..c{}", what, function, range.lo, range.hi);
    }
    if (auto* gfx = GraphicsSystem::instance()) {
      gfx->QueueShaderModule(entry);
    }
  }
  DumpShaderContainer(base, function, total, hash, entry != nullptr);
}

ShaderConstantRange ShaderConstantRangeOf(const ShaderCacheEntry* entry) {
  const ShaderConstantRange* ranges = g_shaderConstantRanges.load(std::memory_order_acquire);
  if (!entry || !ranges) return {};
  const size_t index = size_t(entry - g_shaderCacheEntries);
  return index < g_shaderCacheEntryCount ? ranges[index] : ShaderConstantRange{};
}

ShaderMaterialRegisters ShaderMaterialRegistersOf(const ShaderCacheEntry* entry) {
  const ShaderMaterialRegisters* materials = g_shaderMaterialRegisters.load(std::memory_order_acquire);
  if (!entry || !materials) return {};
  const size_t index = size_t(entry - g_shaderCacheEntries);
  return index < g_shaderCacheEntryCount ? materials[index] : ShaderMaterialRegisters{};
}

const ShaderCacheEntry* LookupShaderBinding(uint32_t guest_object) {
  if (!guest_object) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(g_shaderMutex);
  auto found = g_shaderBindings.find(guest_object);
  return found != g_shaderBindings.end() ? found->second : nullptr;
}

}  // namespace kameo::gpu
