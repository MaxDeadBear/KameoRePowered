#pragma once

// The renderer's own log category, "kameo_gfx", so its level can be set apart from ReXGlue's and the verbose censuses silenced.

#include <rex/logging.h>

namespace kameo::gpu::log {
inline const rex::LogCategoryId Gfx = rex::RegisterLogCategory("kameo_gfx");
}

#define KGFX_TRACE(...) REXLOG_CAT_TRACE(::kameo::gpu::log::Gfx, __VA_ARGS__)
#define KGFX_DEBUG(...) REXLOG_CAT_DEBUG(::kameo::gpu::log::Gfx, __VA_ARGS__)
#define KGFX_INFO(...)  REXLOG_CAT_INFO(::kameo::gpu::log::Gfx, __VA_ARGS__)
#define KGFX_WARN(...)  REXLOG_CAT_WARN(::kameo::gpu::log::Gfx, __VA_ARGS__)
#define KGFX_ERROR(...) REXLOG_CAT_ERROR(::kameo::gpu::log::Gfx, __VA_ARGS__)
