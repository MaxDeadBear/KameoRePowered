#include "engine/current_level.h"

#include <mutex>

#include <rex/logging.h>

#include "engine/guest_addresses.h"

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
    REXLOG_INFO("[kameo] level change -> '{}'", name);
  }
  __imp__sub_820B6B18(ctx, base);
}
