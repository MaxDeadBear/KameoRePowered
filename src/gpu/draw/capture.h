#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "gpu/draw/draw_types.h"

namespace kameo::gpu::detail {

// The device fields D3D::LazyWriteShaders and SetVertexDeclaration keep: the bound shader objects and the declaration (guest_device.h's kVertexDeclaration 0x3010 is the separate FVF-built slot).
inline constexpr uint32_t kDeviceVertexShader = 0x4FD8;
inline constexpr uint32_t kDevicePixelShader = 0x3290;
inline constexpr uint32_t kDeviceVertexDeclaration = 0x2C90;

// Guards g_capturing, the deferred and merge state and the cross-frame vertex and index caches.
extern std::mutex g_captureMutex;
// The frame the guest thread is recording, the one the render thread is replaying, and the handoff between them.
extern DrawFrame g_capturing;
extern DrawFrame g_submitting;
extern DrawFrameExchange g_frameExchange;
extern std::atomic<uint64_t> g_framesTaken;
// Bumped once per captured frame, for cache eviction.
extern uint64_t g_captureFrame;
// Canonical guest addresses the render thread will read while recording this frame, gathered as its draws are captured.
extern std::vector<uint32_t> g_frameGuestReads;

// Empties the replayed frame however DrawCapturedScene returns.
struct SubmittedFrameReset {
  ~SubmittedFrameReset() { g_submitting.reset(); }
};

}  // namespace kameo::gpu::detail
