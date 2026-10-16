#pragma once

#include <cstdint>

#include <rex/ppc.h>

// Keeps the in-game FPS counter's text box alive and up to date; called once per game tick.
void KameoTickFpsCounter(PPCContext& ctx, uint8_t* base);
// True while the FPS counter's box is being drawn in its flat-black shadow pass.
bool KameoSkipTextBoxShadow();

namespace kameo::ui {

// Whether `text_object` is the FPS counter's window, which xfix_textWindow_render then draws without its drop shadow.
bool IsFpsCounterText(uint8_t* base, uint32_t text_object);
void MarkFpsCounterRender(bool rendering);

}  // namespace kameo::ui
