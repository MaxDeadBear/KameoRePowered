#pragma once

#include <cstdint>
#include <string>

// Widens a low prompt bar window when a line with {button} tags is added to it, so keyboard key names fit where glyphs did.
void KameoWidenPromptBar(uint8_t* base, uint32_t window, uint32_t text);
// The moveset HUD's move name with its trigger key added while IconLRCheckContext builds it, or 0 to leave the text alone.
uint32_t KameoIconLabelText(uint8_t* base, uint32_t window, uint32_t text);

namespace kameo::ui {

// The first key of a binding as a prompt shows it: bare and short, since it takes the place of a one-character glyph.
std::u16string KeyLabel(const std::string& binding);
// Whether `player`'s HUD (or, with -1, every HUD) names keyboard keys instead of controller glyphs.
bool KeyboardLabelsFor(int player);

}  // namespace kameo::ui
