#include "gpu/graphics_system.h"

#include <plume_render_interface.h>
#include <smolv.h>

#include <rex/logging.h>

#include "gpu/log.h"
#include "gpu/shaders/shader_cache.h"

namespace kameo::gpu {

plume::RenderShader* GraphicsSystem::ShaderModule(const ShaderCacheEntry* entry) {
  if (!entry) {
    return nullptr;
  }
  const size_t index = size_t(entry - g_shaderCacheEntries);
  if (index < shader_module_by_index_.size()) return shader_module_by_index_[index];
  auto found = shader_modules_.find(entry);
  return found != shader_modules_.end() ? found->second.get() : nullptr;
}

void GraphicsSystem::QueueShaderModule(const ShaderCacheEntry* entry) {
  if (!entry) {
    return;
  }
  std::lock_guard<std::mutex> lock(shader_mutex_);
  shader_pending_.push_back(entry);
}

void GraphicsSystem::EnsureShaderModules() {
  std::vector<const ShaderCacheEntry*> pending;
  {
    std::lock_guard<std::mutex> lock(shader_mutex_);
    pending.swap(shader_pending_);
  }
  if (pending.empty() || !device_) {
    return;
  }

  const bool spirv = interface_->getCapabilities().shaderFormat == plume::RenderShaderFormat::SPIRV;
  std::vector<uint8_t> spirv_scratch;
  uint32_t& empty = shader_module_empty_;
  uint32_t& undecodable = shader_module_undecodable_;
  uint32_t& rejected = shader_module_rejected_;
  for (const ShaderCacheEntry* entry : pending) {
    if (shader_modules_.count(entry)) {
      continue;
    }
    // The cache is generated uncompressed, so the blobs are used directly.
    const uint8_t* blob =
        spirv ? g_compressedSpirvCache + entry->spirvOffset : g_compressedDxilCache + entry->dxilOffset;
    uint32_t size = spirv ? entry->spirvSize : entry->dxilSize;
    if (size == 0) {
      ++shader_module_failed_;
      ++empty;
      continue;
    }
    // XenosRecomp smol-v encodes the SPIR-V half of the cache, which Vulkan rejects verbatim, so it is decoded first.
    if (spirv) {
      const size_t decoded = smolv::GetDecodedBufferSize(blob, size);
      if (decoded == 0) {
        ++shader_module_failed_;
        ++undecodable;
        continue;
      }
      spirv_scratch.resize(decoded);
      if (!smolv::Decode(blob, size, spirv_scratch.data(), decoded)) {
        ++shader_module_failed_;
        ++undecodable;
        continue;
      }
      blob = spirv_scratch.data();
      size = uint32_t(decoded);
    }
    // XenosRecomp emits "main" for both stages.
    auto module = device_->createShader(blob, size, "main",
                                        spirv ? plume::RenderShaderFormat::SPIRV
                                              : plume::RenderShaderFormat::DXIL);
    if (module) {
      ++shader_module_ok_;
      const size_t index = size_t(entry - g_shaderCacheEntries);
      if (index < g_shaderCacheEntryCount) {
        if (shader_module_by_index_.size() < g_shaderCacheEntryCount) shader_module_by_index_.resize(g_shaderCacheEntryCount, nullptr);
        shader_module_by_index_[index] = module.get();
      }
      shader_modules_.emplace(entry, std::move(module));
    } else {
      ++shader_module_failed_;
      ++rejected;
    }
  }

  static uint32_t reported = 0;
  if (shader_module_ok_ + shader_module_failed_ >= reported + 200) {
    reported = shader_module_ok_ + shader_module_failed_;
    KGFX_INFO("shader modules: {} created, {} failed ({}) -- {} absent from the "
                "cache, {} undecodable, {} rejected by the backend",
                shader_module_ok_, shader_module_failed_, spirv ? "spirv" : "dxil",
                shader_module_empty_, shader_module_undecodable_, shader_module_rejected_);
  }
}

}  // namespace kameo::gpu
