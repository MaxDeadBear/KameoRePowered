// One function per launcher page, drawn into the content area by the shell.
#pragma once

#include "launcher/app.h"

namespace kameo::launcher {

void PagePlay(App& app);
void PageVideo(App& app);
void PageAudio(App& app);
void PageControls(App& app);
void PageBindings(App& app);
void PageMods(App& app);
void PageDlc(App& app);
void PageBuilds(App& app);

}  // namespace kameo::launcher
