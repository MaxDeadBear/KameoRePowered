// Standalone regression: clang++ -std=c++23 -Isrc tools/tests/scene_resolve_state_test.cpp -o scene_resolve_state_test.exe
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#include "gpu/scene_resolve_state.h"

using kameo::gpu::detail::InvalidateSceneColorResolve;
using kameo::gpu::detail::ResolveSceneColorIfDirty;

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}

// Models actual resolve output, so reusing a stale copy fails the pixel checks.
struct Scene {
  bool dirty = true;
  unsigned samples = 8;
  int color = 0;
  int resolved = -1;
  unsigned resolves = 0;

  void WriteColor(int value) {
    color = value;
    InvalidateSceneColorResolve(&dirty);
  }

  int ReadColor() {
    if (samples <= 1) return color;
    ResolveSceneColorIfDirty(&dirty, [&] {
      resolved = color;
      ++resolves;
    });
    return resolved;
  }
};

void ConsecutiveCopiesAndFinalComposite() {
  Scene scene;
  scene.WriteColor(17);
  const int first_copy = scene.ReadColor();
  const int second_copy = scene.ReadColor();
  // Offscreen and depth-only work does not change the scene's color.
  const int final_composite = scene.ReadColor();
  Require(first_copy == 17 && second_copy == 17 && final_composite == 17,
          "copies and final composite must read the rendered color");
  Require(scene.resolves == 1, "unchanged scene must be resolved only once");

  scene.WriteColor(29);  // An intervening scene draw or color clear.
  Require(scene.ReadColor() == 29, "scene write must invalidate the previous copy");
  Require(scene.ReadColor() == 29 && scene.resolves == 2,
          "multiple reads after a new write must share the new resolve");
}

void FrameAndTargetReset() {
  Scene scene;
  scene.WriteColor(63);
  Require(scene.ReadColor() == 63, "first frame resolve failed");
  scene.WriteColor(0);  // Next frame's clear, even when no guest frame is ready.
  Require(scene.ReadColor() == 0, "empty frame must not reuse the preceding frame");
  Require(scene.resolves == 2, "each frame clear must require a new resolve");

  // Resizing/recreating the target invalidates even if its address is reused.
  scene.resolved = -1;
  InvalidateSceneColorResolve(&scene.dirty);
  Require(scene.ReadColor() == 0 && scene.resolves == 3,
          "new resolve target must be populated before it is sampled");
}

void StereoEyes() {
  Scene scene;
  scene.WriteColor(71);
  Require(scene.ReadColor() == 71, "left eye resolve failed");
  const int stashed_left_eye = scene.color;
  scene.WriteColor(0);  // StashStereoEye clears the scene for the right eye.
  Require(scene.ReadColor() == 0, "right eye clear must invalidate left eye copy");
  scene.WriteColor(83);
  Require(scene.ReadColor() == 83 && scene.ReadColor() == 83,
          "right eye copies and stereo composite must see right eye color");
  Require(stashed_left_eye == 71 && scene.resolves == 3,
          "stashed left eye and current right eye must remain independent");
}

void SingleSampleAndMissingState() {
  Scene scene;
  scene.samples = 1;
  scene.WriteColor(97);
  Require(scene.ReadColor() == 97 && scene.resolves == 0,
          "single-sample rendering must not perform an MSAA resolve");
  unsigned uncached_resolves = 0;
  for (unsigned i = 0; i < 3; ++i) {
    ResolveSceneColorIfDirty(nullptr, [&] { ++uncached_resolves; });
  }
  Require(uncached_resolves == 3, "callers without shared state must always resolve");
  InvalidateSceneColorResolve(nullptr);
}

void FailedResolveStaysDirty() {
  bool dirty = true;
  try {
    ResolveSceneColorIfDirty(&dirty, [] { throw std::runtime_error("recording failed"); });
  } catch (const std::runtime_error&) {
  }
  Require(dirty, "a failed resolve must not validate the cached copy");
}

}  // namespace

int main() {
  ConsecutiveCopiesAndFinalComposite();
  FrameAndTargetReset();
  StereoEyes();
  SingleSampleAndMissingState();
  FailedResolveStaysDirty();
  std::puts("scene resolve state tests passed");
}
