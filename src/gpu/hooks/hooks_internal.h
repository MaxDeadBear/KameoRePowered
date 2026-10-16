#pragma once

// Shared by the guest-function hooks in gpu/hooks/.
// Each `extern "C" REX_FUNC(sub_<addr>)` here is a strong definition that wins over the recompiled weak alias, and `__imp__sub_<addr>` still reaches the original.
// A hook delegates to the original whenever Active() is null, so --kameo_native_renderer=false runs the xenos plugin as a reference.

#include <cstdint>

#include "gpu/draw_path.h"
#include "gpu/graphics_system.h"

namespace kameo::gpu::hooks {

// The native renderer, or null when the reference renderer runs.
inline GraphicsSystem* Active() { return GraphicsSystem::instance(); }

// The stubbed guest D3D ring (hooks/ring_buffer.cpp): plain guest memory handed out per frame and never read.
bool RingSinksReady();
uint32_t VertexSinkAlloc(uint32_t bytes);
void ResetVertexSink();

// Tags every draw captured during its scope with a guest pass's entry address; scoped because the passes nest and the outer one must get its tag back.
class ScopedGuestPass {
 public:
  explicit ScopedGuestPass(uint32_t pass) : previous_(g_guestPass) {
    g_guestPass = pass;
  }
  ~ScopedGuestPass() { g_guestPass = previous_; }
  ScopedGuestPass(const ScopedGuestPass&) = delete;
  ScopedGuestPass& operator=(const ScopedGuestPass&) = delete;

 private:
  uint32_t previous_;
};

}  // namespace kameo::gpu::hooks
