// Audio page: the volume sliders and the game language.

#include "launcher/ui/pages.h"
#include "launcher/ui/widgets.h"

namespace kameo::launcher {

void PageAudio(App& app) {
  PageTitle(app, "Audio", nullptr);
  RestoreButton(app, {"kameo_volume_master", "kameo_volume_music", "kameo_volume_sfx", "kameo_volume_ambience", "kameo_volume_speech", "kameo_volume_cutscenes", "kameo_language"});
  if (BeginSection("volume", "Volume")) {
    IntSliderRow(app, "Master Volume", "kameo_volume_master", 0, 100, "%d%%");
    IntSliderRow(app, "Music", "kameo_volume_music", 0, 100, "%d%%");
    IntSliderRow(app, "Sound Effects", "kameo_volume_sfx", 0, 100, "%d%%");
    IntSliderRow(app, "Ambience", "kameo_volume_ambience", 0, 100, "%d%%");
    IntSliderRow(app, "Speech", "kameo_volume_speech", 0, 100, "%d%%");
    IntSliderRow(app, "Cutscenes", "kameo_volume_cutscenes", 0, 100, "%d%%");
  }
  EndSection();
  if (BeginSection("language", "Language")) {
    ChoiceRow(app, "Language", "kameo_language", {{"Game Default", 0}, {"English", 1}, {"Japanese", 2}, {"German", 3}, {"French", 4}, {"Spanish", 5}, {"Italian", 6}, {"Korean", 7}, {"Chinese", 8}}, "Takes effect the next time the game starts.");
  }
  EndSection();
}

}  // namespace kameo::launcher
