// Ability energy at any frame rate.
//
// The elemental warriors' energy meter is a player status float at +16 (maximum +48, 1000). These abilities spend it by a fixed amount on every game tick while held:
//   Pummel Weed's underground travel (_ProActInGround) 3.0 and shell attack (_ProActShellAttack) 3.0, the armadillo's super spin (superSpinAttackState) 2.0,
//   the freeze breath (freezeBreathState) 2.5, the flamethrower (flameThrowerTick, once its 0.15 s start-up timer runs out) 3.3,
//   the water cannon (WaterCannonTick) 2.5 or 3.0, the electrocute (_ProActElectrocute) 6.0, the suck (suckState_0) 5.0,
//   and the snare hold (holdState) and the spitter's carry (dropTest, from its stand, move and strafe carry states) 0.2 - 1.2 by what is held.
// Every other caller of ObjPlaySetValue spends a one-off cost of 30 or more when an attack fires, which is right at any frame rate.
// The console ticked at 30 Hz (videoInit sets D3DPRESENT_INTERVAL_TWO on a 60 Hz display), so those amounts were tuned for 30 ticks a second, and every tick above that empties the meter faster: twice as fast at 60 fps, 5.5 times at 165.
// Each spend goes through ObjPlaySetValue(status, 0, 1, current - amount); inside these four abilities the amount is scaled by frameTime * 30, the console's rate at any frame rate. One-off costs (the grab-bury's 250, the shell attack) and the wrecking spin, which already spends 37 a second by frame time, are left alone.

#include <algorithm>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "engine/ability_energy.h"
#include "engine/guest_addresses.h"

REXCVAR_DEFINE_BOOL(kameo_fix_ability_energy, true, "Kameo", "Spend the elemental warriors' energy by time rather than per frame in the abilities that drain it while held (Pummel Weed underground and shell attack, super spin, freeze breath, flamethrower, water cannon, electrocute, suck, snare hold, spitter carry), so the meter lasts as long as on the console at any frame rate. Off restores the per-frame drain, which empties it faster the higher the frame rate.");

namespace {

using kameo::engine::LoadGuestFloat;

// The measured frame time (frameTime), capped by kamVideoFrameTimer at 0.1 s.
constexpr uint32_t kFrameTime = 0x82755760;
constexpr float kConsoleTicksPerSecond = 30.0f;
constexpr uint32_t kStatusEnergy = 16;
// Per-tick drains are at most 6; one-off costs are 30 or more. A held ability can fire a one-off inside its tick (the water cannon's torpedoes, 70), which must not be scaled.
constexpr float kLargestPerTickSpend = 20.0f;

thread_local bool t_perTickDrain = false;

using PerTickDrainScope = kameo::engine::PerTickEnergyDrainScope;

}  // namespace

kameo::engine::PerTickEnergyDrainScope::PerTickEnergyDrainScope() : previous_(t_perTickDrain) { t_perTickDrain = true; }
kameo::engine::PerTickEnergyDrainScope::~PerTickEnergyDrainScope() { t_perTickDrain = previous_; }

// ObjPlaySetValue(status, kind, mode, value): kind 0 mode 1 sets the energy.
extern "C" REX_FUNC(sub_8217EBA8) {
  const uint32_t status = ctx.r3.u32;
  if (t_perTickDrain && status && ctx.r4.u32 == 0 && ctx.r5.u32 == 1 && REXCVAR_GET(kameo_fix_ability_energy)) {
    const float current = LoadGuestFloat(base, status + kStatusEnergy);
    const float spent = current - float(ctx.f1.f64);
    if (spent > 0.0f && spent < kLargestPerTickSpend) {
      const float scale = std::clamp(LoadGuestFloat(base, kFrameTime), 0.0f, 0.1f) * kConsoleTicksPerSecond;
      ctx.f1.f64 = double(current - spent * scale);
      static bool logged = false;
      if (!logged) {
        logged = true;
        REXLOG_INFO("[kameo] ability energy spent by time: {:.2f} per tick scaled by {:.3f}", spent, scale);
      }
    }
  }
  __imp__sub_8217EBA8(ctx, base);
}

// _ProActInGround: Pummel Weed travelling underground.
extern "C" REX_FUNC(sub_823A0990) {
  const PerTickDrainScope scope;
  __imp__sub_823A0990(ctx, base);
}

// superSpinAttackState: the armadillo's super spin.
extern "C" REX_FUNC(sub_8220E4B0) {
  const PerTickDrainScope scope;
  __imp__sub_8220E4B0(ctx, base);
}

// freezeBreathState
extern "C" REX_FUNC(sub_822240C0) {
  const PerTickDrainScope scope;
  __imp__sub_822240C0(ctx, base);
}

// flameThrowerTick
extern "C" REX_FUNC(sub_8223D7A0) {
  const PerTickDrainScope scope;
  __imp__sub_8223D7A0(ctx, base);
}

// _ProActShellAttack
extern "C" REX_FUNC(sub_823A57C8) {
  const PerTickDrainScope scope;
  __imp__sub_823A57C8(ctx, base);
}

// WaterCannonTick is hooked in input/mouse_aim.cpp, which opens a PerTickEnergyDrainScope around it.

// _ProActElectrocute
extern "C" REX_FUNC(sub_824F8200) {
  const PerTickDrainScope scope;
  __imp__sub_824F8200(ctx, base);
}

// suckState_0
extern "C" REX_FUNC(sub_8251B560) {
  const PerTickDrainScope scope;
  __imp__sub_8251B560(ctx, base);
}

// holdState: the snare holding a troll.
extern "C" REX_FUNC(sub_823B0510) {
  const PerTickDrainScope scope;
  __imp__sub_823B0510(ctx, base);
}

// dropTest: the spitter carrying something, called from its stand, move and strafe carry states.
extern "C" REX_FUNC(sub_823AEA58) {
  const PerTickDrainScope scope;
  __imp__sub_823AEA58(ctx, base);
}
