// Spring bones: joints a modded model has beyond the game's own, swung by a spring each frame.
//
// A character's animations drive its joints by position: track i moves joint i, and the matrix build runs for as many
// joints as the animation has (animSetSkeleton takes the count from the clip). A model whose ndSkeleton has more
// joints than that loads, its matrix pool sized for all of them (modelGetMaxModelAnimJointsAndSkeleton, 64 bytes a
// joint at model+308), but nothing ever writes the extra joints' matrices. The modkit's importer adds such joints for
// bones named spring_... (tools/modkit/kameo_modkit/extend.py, add_bones); this file gives them their matrices.
//
// After each matrix build (animBuildMatrices) the pool holds skinning matrices: a joint's world matrix with its bind
// position taken out (skeletonSubtractPivots), row vectors, translation in the fourth row. An extra joint that moved
// rigidly with its parent would have its parent's matrix exactly. Here it has that matrix followed by a turn about
// the joint, from where the bone would point (its parent's pose) to where a damped spring has it pointing: the bone's
// tip is a point mass pulled to its posed place, with gravity, kept at the bone's length. Chains work link by link,
// each link's parent matrix being the one just made.
//
// A costume is a second model on the object, and the game points its skeleton at the body's pool
// (objectAlternativeModelCheck), which has no room for extra joints. A costume with spring bones is pointed back at
// its own pool instead, and the body's matrices are copied into it each frame before the springs run.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/logging/log_category.h>

#include "core/cvar.h"
#include "engine/guest_addresses.h"
#include "kameorepowered_init.h"

KCVAR_DEFINE_BOOL(kameo_spring_bones, true, "Kameo", "Swing the extra bones of modded models (bones named spring_... in the Blender add-on) on springs. Off leaves them rigid on the bone they hang from.");
KCVAR_DEFINE_DOUBLE(kameo_spring_stiffness, 90.0, "Kameo", "How hard a spring bone is pulled back to its pose. Higher is stiffer.");
KCVAR_DEFINE_DOUBLE(kameo_spring_damping, 9.0, "Kameo", "How quickly a spring bone's swing dies down.");
KCVAR_DEFINE_DOUBLE(kameo_spring_gravity, 12.0, "Kameo", "How strongly spring bones are pulled down, in game units per second squared. Kameo is about 5.6 units tall.");
KCVAR_DEFINE_DOUBLE(kameo_spring_limit, 55.0, "Kameo", "The furthest a spring bone swings from its pose, in degrees.");

namespace {

using kameo::engine::LoadGuestFloat;

constexpr uint32_t kModelSkeletonNode = 304;
constexpr uint32_t kModelPool = 308;
constexpr uint32_t kModelInstance = 68;
constexpr uint32_t kModelJointCount = 462;
constexpr uint32_t kNodeInstanceOffset = 12;
constexpr uint32_t kSkeletonCount = 48;
constexpr uint32_t kSkeletonJoints = 52;
constexpr uint32_t kJointBytes = 36;
constexpr uint32_t kJointPivot = 12;
constexpr uint32_t kJointParent = 24;
constexpr uint32_t kJointMark = 35;
constexpr uint8_t kSpringMark = 0x53;  // the importer's mark on a joint it added
constexpr uint32_t kMatrixBytes = 64;
// animBuildMatrices' state: the joint array, the pool being built and the animation's joint count.
constexpr uint32_t kStatePool = 12;
constexpr uint32_t kStateJointCount = 46;
constexpr uint32_t kMostJoints = 512;

struct Vec {
  float x = 0, y = 0, z = 0;
};
Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec operator*(Vec a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec Cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Length(Vec a) { return std::sqrt(Dot(a, a)); }

struct Matrix {
  float m[16];
};

struct Spring {
  Vec tip, velocity;
  bool live = false;
};

struct Model {
  uint32_t pool = 0;       // its own matrix pool
  uint32_t joints = 0;     // its ndSkeleton's joint array
  uint32_t count = 0;      // joints in it
  uint32_t body_pool = 0;  // the pool of the body it is a costume on, once known
  std::vector<Spring> springs;
  std::chrono::steady_clock::time_point swung_at{};  // when its springs last ran
};

std::mutex g_lock;
std::unordered_map<uint32_t, Model> g_models;  // by model address

void StoreGuestFloat(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  REX_STORE_U32(address, bits);
}

Matrix LoadMatrix(uint8_t* base, uint32_t address) {
  Matrix out;
  for (int i = 0; i < 16; ++i) out.m[i] = LoadGuestFloat(base, address + 4 * i);
  return out;
}

void StoreMatrix(uint8_t* base, uint32_t address, const Matrix& matrix) {
  for (int i = 0; i < 16; ++i) StoreGuestFloat(base, address + 4 * i, matrix.m[i]);
}

Vec LoadVec(uint8_t* base, uint32_t address) { return {LoadGuestFloat(base, address), LoadGuestFloat(base, address + 4), LoadGuestFloat(base, address + 8)}; }

// A point through a row-vector matrix.
Vec Transform(const Matrix& a, Vec p) {
  return {p.x * a.m[0] + p.y * a.m[4] + p.z * a.m[8] + a.m[12], p.x * a.m[1] + p.y * a.m[5] + p.z * a.m[9] + a.m[13], p.x * a.m[2] + p.y * a.m[6] + p.z * a.m[10] + a.m[14]};
}

// `parent` followed by the turn that takes direction `from` to direction `to` (both unit) about the point `about`.
Matrix TurnedAbout(const Matrix& parent, Vec about, Vec from, Vec to) {
  Vec axis = Cross(from, to);
  const float sine = Length(axis);
  const float cosine = std::clamp(Dot(from, to), -1.0f, 1.0f);
  if (sine < 1e-6f) return parent;
  axis = axis * (1.0f / sine);
  const float k = 1.0f - cosine;
  // The rotation for column vectors; a row vector takes its transpose.
  const float r[3][3] = {
      {cosine + axis.x * axis.x * k, axis.x * axis.y * k - axis.z * sine, axis.x * axis.z * k + axis.y * sine},
      {axis.y * axis.x * k + axis.z * sine, cosine + axis.y * axis.y * k, axis.y * axis.z * k - axis.x * sine},
      {axis.z * axis.x * k - axis.y * sine, axis.z * axis.y * k + axis.x * sine, cosine + axis.z * axis.z * k}};
  Matrix out = parent;
  for (int row = 0; row < 4; ++row) {
    Vec v{parent.m[4 * row], parent.m[4 * row + 1], parent.m[4 * row + 2]};
    if (row == 3) v = v - about;
    Vec turned{v.x * r[0][0] + v.y * r[0][1] + v.z * r[0][2], v.x * r[1][0] + v.y * r[1][1] + v.z * r[1][2], v.x * r[2][0] + v.y * r[2][1] + v.z * r[2][2]};
    if (row == 3) turned = turned + about;
    out.m[4 * row] = turned.x;
    out.m[4 * row + 1] = turned.y;
    out.m[4 * row + 2] = turned.z;
  }
  return out;
}

// Whether the model at this address is still the one registered: its pool and joint count are where they were.
bool StillThere(uint8_t* base, uint32_t address, const Model& model) {
  return REX_LOAD_U32(address + kModelPool) == model.pool && REX_LOAD_U16(address + kModelJointCount) >= model.count;
}

void Swing(uint8_t* base, Model& model, uint32_t first) {
  // Time by the clock, not the game's frame time: the matrices can be built more than once a frame (a second pass
  // after a look-at or a hold), and each build runs the springs. A build straight after another moves them by the
  // moment between the two, where a frame's worth each time would run them several times too fast.
  const auto now = std::chrono::steady_clock::now();
  const float since = model.swung_at == std::chrono::steady_clock::time_point{} ? 0.0f : std::chrono::duration<float>(now - model.swung_at).count();
  model.swung_at = now;
  // After a pause the springs pick up where they were, by one short step.
  const float frame = std::clamp(since, 0.0f, 1.0f / 20.0f);
  const bool on = KCVAR_GET(kameo_spring_bones);
  const float stiffness = float(std::clamp(KCVAR_GET(kameo_spring_stiffness), 0.0, 2000.0));
  const float damping = float(std::clamp(KCVAR_GET(kameo_spring_damping), 0.0, 200.0));
  const float gravity = float(std::clamp(KCVAR_GET(kameo_spring_gravity), -200.0, 200.0));
  const float limit = std::cos(float(std::clamp(KCVAR_GET(kameo_spring_limit), 1.0, 170.0)) * 3.14159265f / 180.0f);
  const int steps = std::max(1, int(std::ceil(frame * 120.0f)));
  const float step = frame / float(steps);
  model.springs.resize(model.count);

  for (uint32_t joint = first; joint < model.count; ++joint) {
    const uint32_t record = model.joints + kJointBytes * joint;
    const uint32_t parent = REX_LOAD_U16(record + kJointParent);
    if (parent >= joint) continue;  // a root, or out of order: nothing to hang from
    const Matrix above = LoadMatrix(base, model.pool + kMatrixBytes * parent);
    const Vec pivot = LoadVec(base, record + kJointPivot);
    // The bone runs to its first child among the extra joints, or as far again as it is from its parent.
    Vec end = pivot + (pivot - LoadVec(base, model.joints + kJointBytes * parent + kJointPivot));
    for (uint32_t child = joint + 1; child < model.count; ++child) {
      if (REX_LOAD_U16(model.joints + kJointBytes * child + kJointParent) == joint) {
        end = LoadVec(base, model.joints + kJointBytes * child + kJointPivot);
        break;
      }
    }
    const Vec root = Transform(above, pivot);
    const Vec rest = Transform(above, end);
    const float length = Length(rest - root);
    Spring& spring = model.springs[joint];
    if (!on || length < 1e-4f) {
      spring.live = false;
      StoreMatrix(base, model.pool + kMatrixBytes * joint, above);
      continue;
    }
    if (!spring.live || Length(spring.tip - rest) > 4.0f * length) {
      spring = {rest, {}, true};  // first sight of it, or the character was moved somewhere else
    }
    for (int i = 0; i < steps; ++i) {
      Vec pull = (rest - spring.tip) * stiffness - spring.velocity * damping;
      pull.y -= gravity;
      spring.velocity = spring.velocity + pull * step;
      spring.tip = spring.tip + spring.velocity * step;
      // The bone keeps its length: the tip goes back onto the sphere about the joint, and loses the speed along it.
      Vec out = spring.tip - root;
      const float reach = Length(out);
      out = reach > 1e-6f ? out * (1.0f / reach) : (rest - root) * (1.0f / length);
      spring.tip = root + out * length;
      spring.velocity = spring.velocity - out * Dot(spring.velocity, out);
    }
    const Vec posed = (rest - root) * (1.0f / length);
    Vec swung = (spring.tip - root) * (1.0f / length);
    if (Dot(posed, swung) < limit) {
      // Past the limit: back along the arc to it.
      Vec side = swung - posed * Dot(posed, swung);
      const float across = Length(side);
      side = across > 1e-6f ? side * (1.0f / across) : Vec{};
      swung = posed * limit + side * std::sqrt(std::max(0.0f, 1.0f - limit * limit));
      spring.tip = root + swung * length;
      spring.velocity = spring.velocity * 0.5f;
    }
    StoreMatrix(base, model.pool + kMatrixBytes * joint, TurnedAbout(above, root, posed, swung));
  }
}

// After a matrix build into `pool` for `animated` joints: the springs of every model drawn from those matrices.
void AfterBuild(uint8_t* base, uint32_t pool, uint32_t animated) {
  const std::lock_guard<std::mutex> guard(g_lock);
  for (auto it = g_models.begin(); it != g_models.end();) {
    const uint32_t address = it->first;
    Model& model = it->second;
    if (!StillThere(base, address, model)) {
      it = g_models.erase(it);
      continue;
    }
    if (model.pool == pool) {
      // A model of its own (not a costume) with extra joints: the build filled the first of them in place.
      Swing(base, model, std::min(animated, model.count));
    } else {
      const uint32_t node = REX_LOAD_U32(address + kModelSkeletonNode);
      const uint32_t slot = REX_LOAD_U32(node + kNodeInstanceOffset) + REX_LOAD_U32(address + kModelInstance) + 4;
      if (model.body_pool == 0 && REX_LOAD_U32(slot) == pool) {
        // A costume the game has just pointed at the body's pool: its own pool is the one with room.
        model.body_pool = pool;
        REX_STORE_U32(slot, model.pool);
        REXLOG_INFO("[kameo] spring bones: a costume with {} joints ({} animated) draws from its own matrices", model.count, animated);
      }
      if (model.body_pool == pool) {
        const uint32_t shared = std::min(animated, model.count);
        std::memcpy(base + model.pool, base + pool, size_t(kMatrixBytes) * shared);
        Swing(base, model, shared);
      }
    }
    ++it;
  }
}

}  // namespace

// modelInitByAsset(object type, model asset): a model whose skeleton ends in joints the importer added is kept track of.
extern "C" REX_FUNC(sub_8224B618) {
  __imp__sub_8224B618(ctx, base);
  const uint32_t address = ctx.r3.u32;
  if (!address) return;
  const uint32_t node = REX_LOAD_U32(address + kModelSkeletonNode);
  const uint32_t pool = REX_LOAD_U32(address + kModelPool);
  if (!node || !pool) return;
  const uint32_t count = REX_LOAD_U32(node + kSkeletonCount);
  const uint32_t joints = REX_LOAD_U32(node + kSkeletonJoints);
  if (!joints || count < 2 || count > kMostJoints || REX_LOAD_U16(address + kModelJointCount) < count) return;
  if (REX_LOAD_U8(joints + kJointBytes * (count - 1) + kJointMark) != kSpringMark) return;
  const std::lock_guard<std::mutex> guard(g_lock);
  Model model;
  model.pool = pool;
  model.joints = joints;
  model.count = count;
  g_models[address] = std::move(model);
  REXLOG_INFO("[kameo] spring bones: a model with {} joints was loaded", count);
}

// modelFree(model)
extern "C" REX_FUNC(sub_8224BC38) {
  {
    const std::lock_guard<std::mutex> guard(g_lock);
    g_models.erase(ctx.r3.u32);
  }
  __imp__sub_8224BC38(ctx, base);
}

// animBuildMatrices(state): the animated joints' matrices are built; the extra joints follow.
extern "C" REX_FUNC(sub_822F7EE0) {
  const uint32_t state = ctx.r3.u32;
  __imp__sub_822F7EE0(ctx, base);
  if (!state) return;
  {
    const std::lock_guard<std::mutex> guard(g_lock);
    if (g_models.empty()) return;
  }
  const uint32_t pool = REX_LOAD_U32(state + kStatePool);
  if (pool) AfterBuild(base, pool, REX_LOAD_U16(state + kStateJointCount));
}
