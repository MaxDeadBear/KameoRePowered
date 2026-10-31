#pragma once

namespace kameo::gpu::detail {

// The single-sample scene copy stays current until the scene colour is written
// again. A missing state pointer keeps the uncached path safe for other callers.
template <typename Resolve>
bool ResolveSceneColorIfDirty(bool* dirty, Resolve&& resolve) {
  if (dirty && !*dirty) return false;
  resolve();
  if (dirty) *dirty = false;
  return true;
}

inline void InvalidateSceneColorResolve(bool* dirty) {
  if (dirty) *dirty = true;
}

}  // namespace kameo::gpu::detail
