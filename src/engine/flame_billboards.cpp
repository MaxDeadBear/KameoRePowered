// The flames off lava pits and fire traps (the game's plite flame effect) as cards that turn to face the camera,
// rather than each standing at its own fixed angle, where from the side or above they show as flat sheets.

#include <cmath>

#include <rex/cvar.h>

#include "engine/guest_addresses.h"

REXCVAR_DEFINE_BOOL(kameo_gfx_flame_billboards, true, "Kameo", "Turn the flames off lava and fire traps to face the camera, so they don't show as flat sheets from the side or above. Off keeps each at the fixed angle the game gives it.");

using kameo::engine::LoadGuestFloat;

namespace {

// __viewCurrentView: the view being drawn, whose world-to-view matrix is at +16.
constexpr uint32_t kCurrentView = 0x82B7124C;
constexpr uint32_t kViewMatrix = 16;
// objectRenderThreadGetBufferData's id for an object's world matrix, which the flame renderer asks for its object.
constexpr uint32_t kWorldMatrixData = 129389752;

// Set while the flame renderer runs, with the angle that turns its cards to the camera.
thread_local bool t_facing = false;
thread_local float t_facingAngle = 0.0f;

struct Matrix {
  float m[16];
};

Matrix LoadMatrix(uint8_t* base, uint32_t address) {
  Matrix out;
  for (int i = 0; i < 16; ++i) out.m[i] = LoadGuestFloat(base, address + 4 * i);
  return out;
}

// Whether a matrix keeps its translation in its last row, as row vectors times the matrix do (the D3DX way); the
// other way keeps it in the last column.
bool RowVectors(const Matrix& view) {
  const float last_row = std::abs(view.m[12]) + std::abs(view.m[13]) + std::abs(view.m[14]);
  const float last_column = std::abs(view.m[3]) + std::abs(view.m[7]) + std::abs(view.m[11]);
  return last_row >= last_column;
}

// The angle about Y that turns a card, whose corners lie flat in X and Y and face along Z, towards the camera, in the
// flame object's own space; false when there's no view or object to work it out from.
bool AngleToCamera(PPCContext& ctx, uint8_t* base, uint32_t render_object, float& angle) {
  const uint32_t view = REX_LOAD_U32(kCurrentView);
  const uint32_t object = render_object ? REX_LOAD_U32(render_object + 4) : 0;
  if (!view || !object) return false;
  PPCContext call = ctx;
  call.r3.u64 = 0;
  call.r4.u64 = kWorldMatrixData;
  call.r5.u64 = REX_LOAD_U32(object + 76);
  sub_822562C0(call, base);
  if (!call.r3.u32) return false;
  const Matrix v = LoadMatrix(base, view + kViewMatrix);
  const Matrix w = LoadMatrix(base, call.r3.u32);
  const bool rows = RowVectors(v);
  // The direction the camera looks, in the world: where view space's +Z comes from.
  const float forward[3] = {rows ? v.m[2] : v.m[8], rows ? v.m[6] : v.m[9], v.m[10]};
  // ...and in the object's own space, along each of its axes.
  float local[3];
  for (int axis = 0; axis < 3; ++axis) {
    const float a[3] = {rows ? w.m[4 * axis] : w.m[axis], rows ? w.m[4 * axis + 1] : w.m[4 + axis], rows ? w.m[4 * axis + 2] : w.m[8 + axis]};
    const float length_squared = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
    if (length_squared < 1e-12f) return false;
    local[axis] = (forward[0] * a[0] + forward[1] * a[1] + forward[2] * a[2]) / length_squared;
  }
  if (std::abs(local[0]) + std::abs(local[2]) < 1e-6f) return false;
  // RotationY turns a card's facing (0, 0, 1) to (sin, 0, cos); it should face back along the look.
  angle = std::atan2(-local[0], -local[2]);
  return true;
}

}  // namespace

// _pliteFlame_coreRender(renderObject, frustum, pass) -- draws each flame as a card turned by its own angle with
// D3DXMatrixRotationY; while it runs, that angle is the one facing the camera.
extern "C" REX_FUNC(sub_824D27E0) {
  float angle = 0.0f;
  t_facing = REXCVAR_GET(kameo_gfx_flame_billboards) && AngleToCamera(ctx, base, ctx.r3.u32, angle);
  t_facingAngle = angle;
  __imp__sub_824D27E0(ctx, base);
  t_facing = false;
}

// D3DXMatrixRotationY(out, angle) -- the flame renderer also adds the matrix's translation row to each card corner,
// so a facing card is lifted there, out of the lava it would otherwise cut through in a hard line; Living Fire fades
// the card's foot away, so it doesn't float.
extern "C" REX_FUNC(sub_8251CDD0) {
  // In the card's own units, before the flame's scale: the card is 3 tall.
  constexpr float kLift = 0.35f;
  // D3DXMATRIX::_42, the translation row's Y.
  constexpr uint32_t kTranslationY = 52;
  const uint32_t out = ctx.r3.u32;
  if (t_facing) ctx.f1.f64 = t_facingAngle;
  __imp__sub_8251CDD0(ctx, base);
  if (t_facing && out) kameo::engine::StoreGuestFloat(base, out + kTranslationY, kLift);
}
