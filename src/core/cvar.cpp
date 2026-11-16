#include "core/cvar.h"

#include <charconv>

namespace kameo::cvar {
namespace {

class KameoSettings final : public rex::SettingOwner {
 public:
  KameoSettings() : SettingOwner("kameo") {}
};

constexpr std::string_view kPrefix = "kameo_";

// The SDK's settings our code and our settings files name without a scope, as they were before SDK 0.11.
struct SdkName {
  std::string_view old_name;
  std::string_view name;
};
constexpr SdkName kSdkNames[] = {
    {"fullscreen", "ui.fullscreen"},
    {"user_language", "kernel.user_language"},
    {"license_mask", "kernel.license_mask"},
    {"guide_button", "hid.guide_button"},
    {"vibration", "hid.vibration"},
    {"game_data_root", "title.game_root"},
    {"update_data_root", "title.update_root"},
    {"user_data_root", "app.user_root"},
    {"cache_root", "app.cache_root"},
    {"log_level", "log.level"},
    // SentryWork's.
    {"crash_reports", "crash.reports"},
    {"crash_report_log_level", "crash.report_log_level"},
};

}  // namespace

rex::SettingOwner& Owner() {
  static KameoSettings owner;
  return owner;
}

std::string_view MemberName(std::string_view identifier) {
  return identifier.starts_with(kPrefix) ? identifier.substr(kPrefix.size()) : identifier;
}

std::string RegistryName(std::string_view name) {
  if (name.find('.') != std::string_view::npos) return std::string(name);
  for (const SdkName& sdk : kSdkNames)
    if (name == sdk.old_name) return std::string(sdk.name);
  // The keyboard and mouse settings kept their names inside the input scope.
  if (name.starts_with("mnk_") || name.starts_with("keybind_")) return "hid." + std::string(name);
  return "kameo." + std::string(MemberName(name));
}

rex::Result<void> RegisterAll(rex::SettingsRegistry& settings) {
  rex::SettingOwner& owner = Owner();
  REX_TRY_VOID(settings.Register(owner));
  for (rex::SettingBase* setting : owner.GetOwnedSettings()) {
    const std::string& name = setting->GetName();
    const std::string member = name.substr(name.find('.') + 1);
    // Settings declared without the prefix (padbind_a) had no prefix in the old name either.
    const bool unprefixed = member.starts_with("padbind_");
    settings.AddAlias(unprefixed ? member : std::string(kPrefix) + member, name);
  }
  // The folders have aliases of their own in the SDK only under paths.*, and our config files wrote them bare.
  settings.AddAlias("game_data_root", "title.game_root");
  settings.AddAlias("update_data_root", "title.update_root");
  settings.AddAlias("user_data_root", "app.user_root");
  settings.AddAlias("cache_root", "app.cache_root");
  settings.AddAlias("user_language", "kernel.user_language");
  settings.AddAlias("license_mask", "kernel.license_mask");
  return rex::Ok();
}

rex::SettingBase* Find(std::string_view name) {
  auto found = REX_SETTINGS().Find(RegistryName(name));
  return found ? *found : nullptr;
}

std::string GetText(std::string_view name) {
  const rex::SettingBase* setting = Find(name);
  return setting ? setting->GetText() : std::string();
}

bool SetText(std::string_view name, std::string_view text) {
  return static_cast<bool>(REX_SETTINGS().SetText(RegistryName(name), text));
}

bool SetText(rex::SettingLayer layer, std::string_view name, std::string_view text) {
  return static_cast<bool>(REX_SETTINGS().SetText(layer, RegistryName(name), text));
}

bool Reset(std::string_view name) { return static_cast<bool>(REX_SETTINGS().Reset(RegistryName(name))); }

rex::SettingLayer Layer(std::string_view name) {
  const rex::SettingBase* setting = Find(name);
  return setting ? setting->GetLayer() : rex::SettingLayer::kDefault;
}

void Store(rex::SettingBase& setting, std::string_view text) {
  (void)setting.SetText(rex::SettingLayer::kRuntime, text);
}

bool GetBool(std::string_view name, bool fallback) {
  const rex::SettingBase* setting = Find(name);
  if (!setting) return fallback;
  const std::string text = setting->GetText();
  return text == "true" || text == "1";
}

int32_t GetInt(std::string_view name, int32_t fallback) {
  const rex::SettingBase* setting = Find(name);
  if (!setting) return fallback;
  const std::string text = setting->GetText();
  int32_t value = fallback;
  std::from_chars(text.data(), text.data() + text.size(), value);
  return value;
}

double GetDouble(std::string_view name, double fallback) {
  const rex::SettingBase* setting = Find(name);
  if (!setting) return fallback;
  const std::string text = setting->GetText();
  double value = fallback;
  std::from_chars(text.data(), text.data() + text.size(), value);
  return value;
}

}  // namespace kameo::cvar
