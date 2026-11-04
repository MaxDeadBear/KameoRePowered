#include "engine/current_level.h"

#include <mutex>

#include <rex/logging.h>

#include "engine/guest_addresses.h"
#include "gpu/vertex_transcode.h"

namespace {

// A level name has to fit the guest buffer LevelChange copies into.
constexpr size_t kMaxLevelName = 31;

std::mutex g_levelMutex;
std::string g_currentLevel;

}  // namespace

std::string KameoCurrentLevelName() {
  std::lock_guard<std::mutex> lock(g_levelMutex);
  return g_currentLevel;
}

// LevelChange(char* levelname) -- copies the name into requestedlevelname, raises the pending flag and starts the fade; every level transition funnels through here.
extern "C" REX_FUNC(sub_820B6B18) {
  std::string name;
  for (uint32_t i = 0; i <= kMaxLevelName; ++i) {
    const char c = char(REX_LOAD_U8(ctx.r3.u32 + i));
    if (c == 0) break;
    name.push_back(c);
  }
  if (!name.empty()) {
    {
      std::lock_guard<std::mutex> lock(g_levelMutex);
      g_currentLevel = name;
    }
    kameo::gpu::ForgetStableVertexSpans();
    REXLOG_INFO("[kameo] level change -> '{}'", name);
  }
  __imp__sub_820B6B18(ctx, base);
}

namespace {

std::string GuestText(uint8_t* base, uint32_t address, size_t limit) {
  std::string text;
  for (size_t i = 0; address && i < limit; ++i) {
    const char c = char(REX_LOAD_U8(address + uint32_t(i)));
    if (c == 0) break;
    text.push_back(c);
  }
  return text;
}

}  // namespace

// databaseLoadLevel(path, levelName, extension, kind) -- queues a level archive's load: the file is path, the ELF hash of the name in hex, and the
// extension (databaseMakeLevelFilename). Logged so a level that never finishes loading shows which file it waited on.
extern "C" REX_FUNC(sub_822C5FE0) {
  REXLOG_INFO("[kameo] level archive load: '{}' '{}' '{}' kind {}", GuestText(base, ctx.r3.u32, 64), GuestText(base, ctx.r4.u32, 64), GuestText(base, ctx.r5.u32, 16), int32_t(ctx.r6.u32));
  __imp__sub_822C5FE0(ctx, base);
}
