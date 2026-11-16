#include "mods/mods.h"

#if KAMEO_MODS

#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include <rex/hid/input_system.h>
#include <rex/logging/log_category.h>
#include <rex/module.h>
#include <rex/platform/file.h>
#include <rex/ui/ui_system.h>

#include <rexfabric/game_api.h>
#include <rexfabric/host_input.h>
#include <rexfabric/rexfabric.h>

#include "core/cheat_guard.h"
#include "engine/cheats.h"
#include "engine/super_jump.h"
#include "core/cvar.h"
#include "core/guest.h"
#include "core/settings.h"
#include "engine/current_level.h"
#include "engine/dlc/dlc_swap.h"
#include "engine/guest_addresses.h"
#include "gpu/ui/ui_image.h"
#include "input/input.h"
#include "ui/pause_menu/pause_menu_internal.h"

#ifndef KAMEO_BUILD_NAME
#define KAMEO_BUILD_NAME "Kameo: RePowered dev"
#endif

namespace {

// What a mod asked for from a thread that may not be the game's, done at the next game tick.
struct Requests {
  std::mutex mutex;
  std::optional<std::pair<std::string, uint32_t>> level;
  int32_t task = -1;
};
Requests g_requests;

std::optional<rexfabric::Event> g_levelChanged;

// How many times as far player one moves along the ground each tick; 1 is the game's own pace.
std::atomic<float> g_runSpeed{1.0f};

// A player's actor (the object the game moves), or 0: player_list[n] is a wrapper whose +0x10 is the actor.
uint32_t PlayerActor(uint8_t* base, int64_t player) {
  if (!base || player < 0 || player > 3) return 0;
  const uint32_t wrapper = REX_LOAD_U32(kameo::engine::kPlayerList + 4 * uint32_t(player));
  return wrapper ? REX_LOAD_U32(wrapper + 0x10) : 0;
}

// The actor's place and facing, as the game keeps them: x, y (up) and z at +512, +516 and +520, the yaw at +568.
constexpr uint32_t kActorX = 512;
constexpr uint32_t kActorY = 516;
constexpr uint32_t kActorZ = 520;
constexpr uint32_t kActorYaw = 568;

float LoadFloat(uint8_t* base, uint32_t address) {
  const uint32_t bits = REX_LOAD_U32(address);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void StoreFloat(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  REX_STORE_U32(address, bits);
}

double ActorField(int64_t player, uint32_t offset) {
  uint8_t* base = GuestBase();
  const uint32_t actor = PlayerActor(base, player);
  return actor ? double(LoadFloat(base, actor + offset)) : 0.0;
}

// The run speed: what player one moved along the ground since the last tick is added again, scaled. The game's own
// movement has already been checked against walls and this extra step is not, so a large factor can carry the player
// through a thin wall; a step too long to be walking (a warp, a level change) is left alone.
void ApplyRunSpeed(uint8_t* base) {
  static uint32_t last_actor = 0;
  static float last_x = 0.0f, last_z = 0.0f;
  const float factor = g_runSpeed.load(std::memory_order_relaxed);
  const uint32_t actor = PlayerActor(base, 0);
  if (!actor) {
    last_actor = 0;
    return;
  }
  float x = LoadFloat(base, actor + kActorX);
  float z = LoadFloat(base, actor + kActorZ);
  if (actor == last_actor && factor != 1.0f) {
    const float dx = x - last_x;
    const float dz = z - last_z;
    constexpr float kLongestStep = 2.0f;
    if (dx * dx + dz * dz < kLongestStep * kLongestStep) {
      x += dx * (factor - 1.0f);
      z += dz * (factor - 1.0f);
      StoreFloat(base, actor + kActorX, x);
      StoreFloat(base, actor + kActorZ, z);
    }
  }
  last_actor = actor;
  last_x = x;
  last_z = z;
}

// The pictures RexFabric's menus show (mod icons), kept until it frees them.
std::mutex g_imagesMutex;
std::map<ImTextureID, std::unique_ptr<kameo::gpu::UiImage>> g_images;

rex::hid::IInputSystem* Input() { return REX_INTERFACE(rex::hid::IInputSystem); }

// rex.input reads the window's keys and mouse through the SDK's input system, as our own key bindings do.
void InstallModInput() {
  rexfabric::HostInput input;
  input.resolve_key = [](std::string_view name) -> uint16_t {
    rex::hid::IInputSystem* system = Input();
    return system ? uint16_t(system->ParseVirtualKey(name)) : 0;
  };
  input.key_down = [](uint16_t key) {
    rex::hid::IInputSystem* system = Input();
    if (system && system->IsKeyDown(rex::VirtualKey(key))) return true;
#ifdef _WIN32
    // The key codes are Windows' own, so the keyboard itself answers too while the game's window has the focus: a key the input system was not shown (one a dialog took) still reaches a mod's hotkey.
    rex::ui::IUISystem* ui = REX_INTERFACE(rex::ui::IUISystem);
    return ui && ui->IsFocused() && (::GetAsyncKeyState(key) & 0x8000) != 0;
#else
    return false;
#endif
  };
  input.mouse_button_down = [](int button) {
    rex::hid::IInputSystem* system = Input();
    return system && button >= 0 && button < 5 && system->IsButtonDown(rex::MouseButton(button));
  };
  input.accepts_input = [] {
    rex::ui::IUISystem* ui = REX_INTERFACE(rex::ui::IUISystem);
    return ui && ui->IsFocused() && !ui->IsAnyOverlayVisible() && !rexfabric::MenuShown();
  };
  rexfabric::SetHostInput(std::move(input));
}

// The `kameo` table mods see. Every function may be called from any thread a mod runs on; what needs the game thread
// is queued for the next tick.
void DeclareApi() {
  rexfabric::Namespace kameo("kameo");

  kameo.Function("build", [] { return std::string(KAMEO_BUILD_NAME); });
#if KAMEO_TU
  kameo.Constant("title_update", true);
#else
  kameo.Constant("title_update", false);
#endif

  // Levels. A level's name is the one the game passes to LevelChange ("FrontEndLevel", "CastleIntro"); its file is the
  // ELF hash of that name.
  kameo.Function("level.name", [] { return KameoCurrentLevelName(); });
  kameo.Function("level.change", [](std::string name, int64_t setup_point) {
    std::lock_guard<std::mutex> lock(g_requests.mutex);
    g_requests.level = {std::move(name), uint32_t(setup_point < 0 ? 0 : setup_point)};
  });
  // A story task, as the pause menu's level select starts one.
  kameo.Function("level.start_task", [](int64_t task) {
    std::lock_guard<std::mutex> lock(g_requests.mutex);
    g_requests.task = int32_t(task);
  });
  g_levelChanged = kameo.Event("level_changed");

  // Players. The object is the player's character, for rex.mem and rex.call; 0 when that player is not in the game.
  kameo.Function("player.count", []() -> int64_t {
    uint8_t* base = GuestBase();
    return base ? int64_t(REX_LOAD_U32(kameo::engine::kNumPlayers)) : 0;
  });
  kameo.Function("player.object", [](int64_t player) -> int64_t {
    uint8_t* base = GuestBase();
    if (!base || player < 0 || player > 3) return 0;
    const uint32_t wrapper = REX_LOAD_U32(kameo::engine::kPlayerList + 4 * uint32_t(player));
    return wrapper ? int64_t(REX_LOAD_U32(wrapper + 0x200)) : 0;
  });

  // Where a player's actor is and which way it faces (radians). 0 when that player is not in the game.
  kameo.Function("player.x", [](int64_t player) { return ActorField(player, kActorX); });
  kameo.Function("player.y", [](int64_t player) { return ActorField(player, kActorY); });
  kameo.Function("player.z", [](int64_t player) { return ActorField(player, kActorZ); });
  kameo.Function("player.yaw", [](int64_t player) { return ActorField(player, kActorYaw); });
  // Puts a player's actor somewhere else in the level. Nothing checks the place is one the game can stand in.
  kameo.Function("player.set_position", [](int64_t player, double x, double y, double z) {
    uint8_t* base = GuestBase();
    const uint32_t actor = PlayerActor(base, player);
    if (!actor) return false;
    StoreFloat(base, actor + kActorX, float(x));
    StoreFloat(base, actor + kActorY, float(y));
    StoreFloat(base, actor + kActorZ, float(z));
    return true;
  });
  // Player one's pace along the ground, as a multiple of the game's own (1 is unchanged, 2 twice as fast, 0.5 half).
  kameo.Function("player.set_run_speed", [](double factor) { g_runSpeed.store(float(factor < 0.0 ? 0.0 : factor > 10.0 ? 10.0 : factor), std::memory_order_relaxed); });
  kameo.Function("player.run_speed", [] { return double(g_runSpeed.load(std::memory_order_relaxed)); });

  // The switches behind the game's cheats, for a mod to set itself.
  kameo.Function("cheat.infinite_health", [](bool on) { g_kameo_infinite_health_enabled.store(on ? 1 : 0, std::memory_order_release); });
  kameo.Function("cheat.infinite_energy", [](bool on) { g_kameo_infinite_energy_enabled.store(on ? 1 : 0, std::memory_order_release); });
  kameo.Function("cheat.one_shot", [](bool on) { g_kameo_kameo_oneshot_enabled.store(on ? 1 : 0, std::memory_order_release); });
  kameo.Function("cheat.super_jump", [](bool on) { g_kameo_super_jump_enabled.store(on ? 1 : 0, std::memory_order_release); });
  kameo.Function("score.add", [](int64_t points) { g_kameo_add_score_pending.fetch_add(points, std::memory_order_acq_rel); });

  // Kameo's costume, by file suffix ("00" is Classic); empty puts the standard look back.
  kameo.Function("costume.set", [](std::string suffix) {
    if (suffix.empty()) g_kameo_pending_standard_swap.store(1, std::memory_order_release);
    else QueueKameoDlcSuffix(suffix);
  });

  // The game's own settings (those in the pause menu and kameo_settings.toml), by name: "kameo.camera_fov",
  // "kameo.gfx_bloom", or the older spelling "kameo_camera_fov". A change is saved, and one that switches a cheat on is
  // recorded as the pause menu's would be.
  kameo.Function("setting.get", [](std::string name) { return kameo::cvar::GetText(name); });
  kameo.Function("setting.exists", [](std::string name) { return kameo::cvar::Exists(name); });
  kameo.Function("setting.set", [](std::string name, std::string value) {
    if (!kameo::cvar::SetText(name, value)) return false;
    KameoSaveSettings();
    return true;
  });

  // Friendlier forms over the functions above.
  kameo.Lua(R"LUA(
    function kameo.on(event, fn) return rex.on("kameo." .. event, fn) end
    function kameo.setting.number(name) return tonumber(kameo.setting.get(name)) end
    function kameo.setting.boolean(name) return kameo.setting.get(name) == "true" end
  )LUA");
}

}  // namespace

rex::Result<void> KameoModsConfigure(rex::SettingsRegistry& settings) { return rexfabric::Configure(settings); }

rex::Result<void> KameoModsInit(rex::ui::IUISystem& ui) {
  rexfabric::Config config;
  config.mods_dir = rex::platform::GetExecutableFolder() / "mods";
  // F3, F4 and F6 to F10 are the game's own (the debug overlay, its menu pages, the shader list, texture packs, the Guide).
  config.menu_key = "F5";
  config.node_editor_key = "F2";
  // The game tick calls Tick itself, from its per-frame hook.
  config.frame_function = 0;
  rexfabric::Setup(std::move(config));
  InstallModInput();
  DeclareApi();
  rexfabric::Images images;
  images.create = [](uint32_t width, uint32_t height, const uint8_t* rgba) -> ImTextureID {
    std::unique_ptr<kameo::gpu::UiImage> image = kameo::gpu::UiImage::Create(width, height, rgba);
    if (!image) return ImTextureID_Invalid;
    const ImTextureID id = image->texture_id();
    std::lock_guard<std::mutex> lock(g_imagesMutex);
    g_images[id] = std::move(image);
    return id;
  };
  images.destroy = [](ImTextureID texture) {
    std::lock_guard<std::mutex> lock(g_imagesMutex);
    g_images.erase(texture);
  };
  return rexfabric::CreateMenus(ui, std::move(images));
}

void KameoModsStart() {
  rexfabric::Start();
  REXLOG_INFO("[kameo] mods: {} enabled in {}", rexfabric::EnabledModDirs().size(), rexfabric::ModsDir().string());
}

void KameoModsStop() {
  rexfabric::Stop();
  std::lock_guard<std::mutex> lock(g_imagesMutex);
  g_images.clear();
}

void KameoModsTick(PPCContext& ctx, uint8_t* base) {
  std::optional<std::pair<std::string, uint32_t>> level;
  int32_t task = -1;
  {
    std::lock_guard<std::mutex> lock(g_requests.mutex);
    level.swap(g_requests.level);
    std::swap(task, g_requests.task);
  }
  if (task >= 0) kameo::ui::JumpToTask(ctx, base, task);
  if (level) kameo::ui::ChangeToLevel(ctx, base, level->first, level->second);

  ApplyRunSpeed(base);

  static std::string last_level;
  if (std::string now = KameoCurrentLevelName(); now != last_level) {
    last_level = now;
    if (g_levelChanged) g_levelChanged->Fire(std::move(now));
  }
  rexfabric::Tick();
}

bool KameoModsMenuShown() { return rexfabric::MenuShown(); }

bool KameoModsEnabled() { return !rexfabric::EnabledModDirs().empty(); }

#else  // KAMEO_MODS

rex::Result<void> KameoModsConfigure(rex::SettingsRegistry&) { return rex::Ok(); }
rex::Result<void> KameoModsInit(rex::ui::IUISystem&) { return rex::Ok(); }
void KameoModsStart() {}
void KameoModsStop() {}
void KameoModsTick(PPCContext&, uint8_t*) {}
bool KameoModsMenuShown() { return false; }
bool KameoModsEnabled() { return false; }

#endif  // KAMEO_MODS
