#pragma once

#include <cstdint>
#include <string>

#include <rex/cvar.h>

#include "gpu/movie/bink_frame.h"

REXCVAR_DECLARE(bool, kameo_movie_replacements);

namespace kameo::gpu {

enum class MovieDrive {
  // No usable replacement, or one without sound: the Bink plays as normal.
  Bink,
  // A replacement with sound is playing on its own clock, so the Bink must not advance.
  Replaced,
  // That replacement has reached its end.
  Finished,
};

// Called for each fmvRenderPlaying of a Bink: whether <kameo_movie_dir>/<name>.mp4 takes over, at `gain` volume.
MovieDrive DriveReplacementMovie(const std::string& name, uint32_t bink_handle, bool has_alpha, float gain);

// Swaps a decoded Bink frame's planes for the replacement's picture: at the replacement's own clock when it plays its sound, otherwise at `bink_seconds`.
bool ReplaceMovieFrame(const std::string& name, uint32_t bink_handle, double bink_seconds, BinkFrame* frame);

// The Bink closed, or looped back to its start: its replacement stops, or starts over on the next drive.
void EndReplacementMovie(uint32_t bink_handle);

}  // namespace kameo::gpu
