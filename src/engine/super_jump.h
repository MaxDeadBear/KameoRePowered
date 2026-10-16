// Super jump: holding LB floats the player up and lets them glide down; switched by the model dialog.
#pragma once

#include <atomic>
#include <cstdint>

#include <rex/ppc.h>

extern std::atomic<uint32_t> g_kameo_super_jump_enabled;
// Called from the per-frame game tick.
void KameoUpdateSuperJumpFloat(PPCContext& ctx, uint8_t* base);
