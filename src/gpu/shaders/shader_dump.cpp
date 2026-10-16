#include "gpu/shaders/shader_dump.h"

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_set>

#include <fmt/format.h>
#include <rex/cvar.h>

#include "gpu/log.h"

REXCVAR_DEFINE_BOOL(kameo_gfx_dump_shaders, false, "Kameo",
                    "Write every guest shader container to shader_dump/, and the decompressed guest image to guest_image.bin at the first present, "
                    "so the translated shader cache can be regenerated with tools/shaders/refresh_shader_cache.py. Most of Kameo's shaders are "
                    "decompressed out of packed game data at load time and never appear in the XEX, so play through the areas whose shaders are wanted.");

namespace kameo::gpu {

// The shipping XEX is compressed, but the runtime maps the decompressed module, so dumping that range gives the cache generator its input with byte order preserved.
void DumpGuestImageOnce(uint8_t* base) {
  if (!REXCVAR_GET(kameo_gfx_dump_shaders)) return;
  static std::once_flag once;
  std::call_once(once, [base] {
    constexpr uint32_t kImageBase = 0x82000000;
    constexpr uint32_t kImageEnd = 0x82BF0000;
    FILE* f = std::fopen("guest_image.bin", "wb");
    if (!f) {
      KGFX_WARN("could not open guest_image.bin for writing");
      return;
    }
    std::fwrite(base + kImageBase, 1, kImageEnd - kImageBase, f);
    std::fclose(f);
    KGFX_INFO("wrote guest_image.bin");
  });
}

void DumpShaderContainer(uint8_t* base, uint32_t function, uint64_t bytes, uint64_t hash, bool in_cache) {
  // Regenerating the cache needs the whole corpus, which is why the cvar writes every container rather than only the misses.
  if (in_cache && !REXCVAR_GET(kameo_gfx_dump_shaders)) return;
  static std::mutex mutex;
  static std::unordered_set<uint64_t> dumped;
  std::lock_guard<std::mutex> lock(mutex);
  if (!dumped.insert(hash).second) return;
  std::error_code error;
  std::filesystem::create_directory("shader_dump", error);
  if (FILE* f = std::fopen(fmt::format("shader_dump/{:016X}.bin", hash).c_str(), "wb")) {
    std::fwrite(base + function, 1, size_t(bytes), f);
    std::fclose(f);
  }
  if (!in_cache) KGFX_WARN("shader container {:08X} is not in the shader cache; written to shader_dump/{:016X}.bin", function, hash);
}

}  // namespace kameo::gpu
