// State and helpers shared by the story split-screen translation units.
#pragma once

#include <atomic>
#include <cstdint>

#include <rex/cvar.h>

#include "engine/guest_addresses.h"

REXCVAR_DECLARE(bool, kameo_story_splitscreen);

namespace kameo::engine {

// Collapses mainSetupMainRenderInfo's viewport count to one while player 2 is out, without dropping numWantedPlayers, which would unbind his pad.
extern std::atomic<uint32_t> g_collapse_view;

uint32_t AskObject(PPCContext& src, uint8_t* base, uint32_t obj, uint32_t msg, uint32_t arg);
uint32_t ActiveObjectFor(PPCContext& src, uint8_t* base, uint32_t player);
uint32_t WarpableObjectFor(PPCContext& src, uint8_t* base, uint32_t player, uint32_t scratch_out);
bool WarpPlayerToPlayer(PPCContext& ctx, uint8_t* base, uint32_t dst_idx, uint32_t src_idx);
void ServicePlayer2TripleTap(uint8_t* base);
void ServiceTeleportRequests(uint8_t* base);

bool ParkPlayerObject(uint8_t* base, uint32_t player2);
bool BenchPlayer2(uint8_t* base);
bool UnbenchPlayer2(PPCContext& ctx, uint8_t* base);
void TickBench(uint8_t* base);
bool Player2Benched();
void ForgetBenchedPlayer();

void SyncPlayer2Warriors(uint8_t* base);

}  // namespace kameo::engine
