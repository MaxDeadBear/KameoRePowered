#pragma once

// The renderer's own log category, "kameo_gfx", so its level can be set apart from ReXGlue's and the verbose censuses silenced (log.categories.kameo_gfx).

#include <rex/logging/log_category.h>

REXLOG_CATEGORY(kameo_gfx, "Kameo: RePowered native renderer")
REXLOG_TABLE(kKameoLog, kameo_gfx)

#define KGFX_TRACE(...) REXLOG_CAT_TRACE(::rex::log::kameo_gfx, __VA_ARGS__)
#define KGFX_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::kameo_gfx, __VA_ARGS__)
#define KGFX_INFO(...)  REXLOG_CAT_INFO(::rex::log::kameo_gfx, __VA_ARGS__)
#define KGFX_WARN(...)  REXLOG_CAT_WARN(::rex::log::kameo_gfx, __VA_ARGS__)
#define KGFX_ERROR(...) REXLOG_CAT_ERROR(::rex::log::kameo_gfx, __VA_ARGS__)
