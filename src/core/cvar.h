// The game's own settings, on the SDK's settings registry.
//
// A setting is declared at namespace scope with KCVAR_DEFINE_*, under the name it has always had in code
// (kameo_gfx_shadows). In the registry it is kameo.gfx_shadows: the scope is "kameo" and the kameo_ prefix is dropped.
// The name from before SDK 0.11, kameo_gfx_shadows with no scope, stays an alias, so older config files and command
// lines still apply.
//
// The by-name functions take either spelling, for our settings and for the SDK's (fullscreen is ui.fullscreen).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>

#include <rex/console_command.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>

namespace kameo::cvar {

// The owner every KCVAR_DEFINE_* setting belongs to, scope "kameo".
rex::SettingOwner& Owner();
// kameo_gfx_shadows -> gfx_shadows: the member name of a setting declared under that identifier.
std::string_view MemberName(std::string_view identifier);
// The registry name of a setting given by either spelling: kameo_gfx_shadows -> kameo.gfx_shadows, fullscreen ->
// ui.fullscreen. A name with a dot is returned as it is.
std::string RegistryName(std::string_view name);

// Registers the owner and the old names. Called once from the app's OnConfigure, before any source is read.
rex::Result<void> RegisterAll(rex::SettingsRegistry& settings);

// The setting, or null when no registered owner declares it.
rex::SettingBase* Find(std::string_view name);
inline bool Exists(std::string_view name) { return Find(name) != nullptr; }
// The value as text, empty when there is no such setting.
std::string GetText(std::string_view name);
// Stores a value from text; false when the setting is missing, frozen, or refuses the text.
bool SetText(std::string_view name, std::string_view text);
// Stores a value at a layer, for a value read from one of our own files before the SDK reads the others.
bool SetText(rex::SettingLayer layer, std::string_view name, std::string_view text);
bool Reset(std::string_view name);
// The layer the current value came from, kDefault when there is no such setting.
rex::SettingLayer Layer(std::string_view name);
inline bool IsDefault(std::string_view name) { return Layer(name) == rex::SettingLayer::kDefault; }
inline bool FromCommandLine(std::string_view name) { return Layer(name) == rex::SettingLayer::kCommandLine; }

// Stores a value from code so that it holds whatever source set the setting before: Setting::Set writes at the title
// layer until the game launches, which a value from one of our settings files outranks.
void Store(rex::SettingBase& setting, std::string_view text);
template <typename T, typename Value>
void Store(rex::Setting<T>& setting, const Value& value) {
  rex::SettingBase& base = setting;
  if constexpr (std::is_same_v<T, bool>) Store(base, std::string_view(value ? "true" : "false"));
  else if constexpr (std::is_same_v<T, std::string>) Store(base, std::string_view(value));
  else Store(base, std::string_view(std::to_string(static_cast<T>(value))));
}

bool GetBool(std::string_view name, bool fallback = false);
int32_t GetInt(std::string_view name, int32_t fallback = 0);
double GetDouble(std::string_view name, double fallback = 0.0);

}  // namespace kameo::cvar

#define KCVAR_DEFINE(Type, name, default_value, help) \
  ::rex::Setting<Type> name { &::kameo::cvar::Owner(), ::kameo::cvar::MemberName(#name), default_value, help }
// The group argument is kept for the reader: every setting of ours is in the "kameo" scope.
#define KCVAR_DEFINE_BOOL(name, default_value, group, help) KCVAR_DEFINE(bool, name, default_value, help)
#define KCVAR_DEFINE_INT32(name, default_value, group, help) KCVAR_DEFINE(::rex::i32, name, default_value, help)
#define KCVAR_DEFINE_UINT32(name, default_value, group, help) KCVAR_DEFINE(::rex::u32, name, default_value, help)
#define KCVAR_DEFINE_DOUBLE(name, default_value, group, help) KCVAR_DEFINE(::rex::f64, name, default_value, help)
#define KCVAR_DEFINE_STRING(name, default_value, group, help) KCVAR_DEFINE(std::string, name, default_value, help)
// A console command (kameo.gfx_rescan_textures), run from the console overlay.
#define KCVAR_DEFINE_COMMAND(name, function, group, help)   ::rex::ConsoleCommand name { &::kameo::cvar::Owner(), ::kameo::cvar::MemberName(#name), help, [](std::string_view) -> ::rex::Result<void> { (function)(); return ::rex::Ok(); } }
#define KCVAR_DECLARE(Type, name) extern ::rex::Setting<Type> name
#define KCVAR_GET(name) (name.Get())
#define KCVAR_SET(name, value) (::kameo::cvar::Store(name, value))
