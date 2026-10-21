#include "gpu/hooks/hooks_internal.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <kameorepowered_init.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "gpu/config.h"
#include "gpu/draw_path.h"
#include "gpu/frame_profile.h"
#include "gpu/guest_device.h"
#include "gpu/log.h"
#include "gpu/shaders/shader_dump.h"

using kameo::gpu::hooks::Active;

namespace {

using GuestImport = void (*)(PPCContext&, uint8_t*);
GuestImport g_realNtFreeVirtualMemory = nullptr;
GuestImport g_realMmFreePhysicalMemory = nullptr;

// The render thread reads a handed-over frame's textures from guest memory, so a free waits for it when that frame reads the region being released.
void WaitForRenderReadsOfRegion(uint32_t address, uint32_t bytes, const char* reason) {
  auto* gfx = Active();
  if (!gfx || !address) return;
  // A sized free decommits exactly that range; only a size of zero releases the whole region around the address.
  uint32_t region = address;
  uint32_t region_bytes = bytes;
  rex::memory::BaseHeap* heap = rex::system::kernel_state()->memory()->LookupHeap(address);
  if (bytes == 0 && (!heap || !heap->QueryBaseAndSize(&region, &region_bytes))) {
    gfx->WaitForRenderIdle(reason);
    return;
  }
  const uint32_t begin = region;
  const uint64_t end = uint64_t(region) + region_bytes;
  const uint64_t canonical_end = uint64_t(kameo::gpu::CanonicalGuestAddress(begin)) + (end - begin);
  gfx->WaitForRenderReads(kameo::gpu::CanonicalGuestAddress(begin), uint32_t(std::min<uint64_t>(canonical_end, 0xFFFFFFFFu)), reason);
}

void NtFreeVirtualMemoryAfterRender(PPCContext& ctx, uint8_t* base) {
  if (ctx.r3.u32 && ctx.r4.u32) WaitForRenderReadsOfRegion(REX_LOAD_U32(ctx.r3.u32), REX_LOAD_U32(ctx.r4.u32), "NtFreeVirtualMemory");
  g_realNtFreeVirtualMemory(ctx, base);
}

void MmFreePhysicalMemoryAfterRender(PPCContext& ctx, uint8_t* base) {
  WaitForRenderReadsOfRegion(ctx.r4.u32, 0, "MmFreePhysicalMemory");
  g_realMmFreePhysicalMemory(ctx, base);
}

#ifdef _WIN32
// The recompiled code calls these kernel exports straight through the exe's import table, so that is the one place they can be wrapped without touching rexglue.
void RedirectGuestMemoryFrees() {
  auto* image = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(image + reinterpret_cast<IMAGE_DOS_HEADER*>(image)->e_lfanew);
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  uint32_t redirected = 0;
  for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(image + dir.VirtualAddress); dir.VirtualAddress && desc->Name; ++desc) {
    if (_stricmp(reinterpret_cast<const char*>(image + desc->Name), "rexruntimerd.dll") != 0 && _stricmp(reinterpret_cast<const char*>(image + desc->Name), "rexruntime.dll") != 0) continue;
    auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(image + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
    auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(image + desc->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++slots) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
      const char* name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(image + names->u1.AddressOfData)->Name;
      GuestImport* real = nullptr;
      GuestImport hook = nullptr;
      if (!std::strcmp(name, "__imp__NtFreeVirtualMemory")) { real = &g_realNtFreeVirtualMemory; hook = NtFreeVirtualMemoryAfterRender; }
      else if (!std::strcmp(name, "__imp__MmFreePhysicalMemory")) { real = &g_realMmFreePhysicalMemory; hook = MmFreePhysicalMemoryAfterRender; }
      if (!real) continue;
      DWORD old = 0;
      VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
      *real = reinterpret_cast<GuestImport>(slots->u1.Function);
      slots->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
      VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
      ++redirected;
    }
  }
  if (redirected == 2) {
    KGFX_INFO("guest memory frees now wait for the render thread when a frame in flight reads them");
  } else {
    KGFX_ERROR("only {} of 2 guest memory-free imports were found; a free can race the render thread", redirected);
  }
}
#else
// An ELF executable's own definition of a symbol is bound ahead of the runtime library's, so the wrappers below the namespace replace these exports at link time and only the real ones need finding.
void ResolveGuestMemoryFrees() {
  static std::once_flag resolved;
  std::call_once(resolved, [] {
    g_realNtFreeVirtualMemory = reinterpret_cast<GuestImport>(dlsym(RTLD_NEXT, "__imp__NtFreeVirtualMemory"));
    g_realMmFreePhysicalMemory = reinterpret_cast<GuestImport>(dlsym(RTLD_NEXT, "__imp__MmFreePhysicalMemory"));
    if (!g_realNtFreeVirtualMemory || !g_realMmFreePhysicalMemory) {
      KGFX_ERROR("the runtime's guest memory-free exports were not found; the game cannot free memory");
      std::abort();
    }
  });
}

void RedirectGuestMemoryFrees() {
  ResolveGuestMemoryFrees();
  KGFX_INFO("guest memory frees now wait for the render thread when a frame in flight reads them");
}
#endif

void HandOffFrame(uint8_t* base, kameo::gpu::GraphicsSystem* gfx) {
  static std::once_flag frees_redirected;
  std::call_once(frees_redirected, RedirectGuestMemoryFrees);
  // Present and D3D's worker thread can both reach here, and the handoff below must stay in order.
  static std::mutex drive_mutex;
  std::lock_guard<std::mutex> drive_lock(drive_mutex);
  // The game thread's own frame time: from one handoff to the next, whatever it spent the time on.
  static std::chrono::steady_clock::time_point last_handoff;
  const auto now = std::chrono::steady_clock::now();
  if (last_handoff != std::chrono::steady_clock::time_point{}) {
    kameo::gpu::detail::g_frameProfile.ns_guest_frame.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_handoff).count()), std::memory_order_relaxed);
    kameo::gpu::detail::g_frameProfile.guest_frames.fetch_add(1, std::memory_order_relaxed);
  }
  last_handoff = now;
  kameo::gpu::detail::g_captureDetailed = kameo::gpu::detail::FrameProfileDetailed();
  // The dialogs write guest state, so they run here on the guest thread rather than on the render thread.
  gfx->RunUiCallback();

  // Same handoff for the scene geometry captured since the last swap, once the render thread has taken the previous frame.
  gfx->WaitForRenderSlot();
  std::vector<uint32_t> guest_reads;
  {
    kameo::gpu::detail::ScopedAtomicNs timed(&kameo::gpu::detail::g_frameProfile.ns_guest_handoff);
    kameo::gpu::SubmitCapturedFrame(&guest_reads);
    kameo::gpu::hooks::ResetVertexSink();
  }
  uint32_t fence = 0;
  uint32_t* readback_word = nullptr;
  if (const uint32_t device = REX_LOAD_U32(kameo::gpu::kD3DDevicePtr)) {
    fence = REX_LOAD_U32(device + kameo::gpu::dev::kCpuFence);
    const uint32_t readback = REX_LOAD_U32(device + kameo::gpu::dev::kGpuReadback);
    if (readback) readback_word = reinterpret_cast<uint32_t*>(base + readback + REX_PHYS_HOST_OFFSET(readback));
  }
  gfx->KickRenderThread(fence, readback_word, std::move(guest_reads));
}

}  // namespace

#ifndef _WIN32
extern "C" REX_FUNC(__imp__NtFreeVirtualMemory) {
  ResolveGuestMemoryFrees();
  NtFreeVirtualMemoryAfterRender(ctx, base);
}

extern "C" REX_FUNC(__imp__MmFreePhysicalMemory) {
  ResolveGuestMemoryFrees();
  MmFreePhysicalMemoryAfterRender(ctx, base);
}
#endif

// D3DDevice_Present(D3DDevice*) -- drives the frame from the guest's own present call.
extern "C" REX_FUNC(sub_820D0048) {
  auto* gfx = Active();
  if (gfx == nullptr) {
    __imp__sub_820D0048(ctx, base);
    return;
  }
  kameo::gpu::DumpGuestImageOnce(base);
  HandOffFrame(base, gfx);
}

// D3DDevice_Swap(D3DDevice*) -- D3D's worker thread calls it directly; the original is not called, since it would push a present packet into a ring nothing consumes.
extern "C" REX_FUNC(sub_820CF9D8) {
  auto* gfx = Active();
  if (gfx == nullptr) {
    __imp__sub_820CF9D8(ctx, base);
    return;
  }
  kameo::gpu::DumpGuestImageOnce(base);
  HandOffFrame(base, gfx);
}
