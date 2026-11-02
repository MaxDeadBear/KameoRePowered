// Audio page: the volume sliders and the game language.

#include "core/localization.h"
#include "launcher/ui/pages.h"
#include "launcher/ui/widgets.h"

namespace kameo::launcher {

using l10n::Tr;

void PageAudio(App& app) {
  PageTitle(app, Tr("Audio"), nullptr);
  RestoreButton(app, {"kameo_volume_master", "kameo_volume_music", "kameo_volume_sfx", "kameo_volume_ambience", "kameo_volume_speech", "kameo_volume_cutscenes", "kameo_language"});
  if (BeginSection("volume", Tr("Volume"))) {
    IntSliderRow(app, Tr("Master Volume"), "kameo_volume_master", 0, 100, "%d%%");
    IntSliderRow(app, Tr("Music"), "kameo_volume_music", 0, 100, "%d%%");
    IntSliderRow(app, Tr("Sound Effects"), "kameo_volume_sfx", 0, 100, "%d%%");
    IntSliderRow(app, Tr("Ambience"), "kameo_volume_ambience", 0, 100, "%d%%");
    IntSliderRow(app, Tr("Speech"), "kameo_volume_speech", 0, 100, "%d%%");
    IntSliderRow(app, Tr("Cutscenes"), "kameo_volume_cutscenes", 0, 100, "%d%%");
  }
  EndSection();
  if (BeginSection("language", Tr("Language"))) {
    // Each language is named in itself, so a player can find their own whatever the launcher is showing.
    ChoiceRow(app, Tr("Language"), "kameo_language", {{Tr("Game Default"), 0}, {"English", 1}, {"日本語", 2}, {"Deutsch", 3}, {"Français", 4}, {"Español", 5}, {"Italiano", 6}, {"한국어", 7}, {"繁體中文", 8}}, Tr("Takes effect the next time the game starts."));
  }
  EndSection();
}

}  // namespace kameo::launcher
