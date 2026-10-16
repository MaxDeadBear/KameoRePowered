// kameo_settings.toml next to the exe: loaded once after the main config, rewritten whenever a menu option changes.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

void KameoLoadSettings();
void KameoSaveSettings();
// Puts the named settings back to their built-in defaults, applies what needs applying, and saves.
void KameoRestoreDefaultSettings(const std::vector<std::string>& names);
// The language this session started with; the Language option only takes effect at the next launch.
uint32_t KameoRunningLanguage();
