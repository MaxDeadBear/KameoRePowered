// Guest-function hooks that drive the native renderer.
//
// Only compiled when KAMEO_NATIVE_RENDERER=ON. Even then every hook delegates
// to the original body unless the renderer is actually active at runtime, so a
// build with the renderer compiled in but the cvar off behaves exactly like a
// stock build.
//
// The delegation relies on codegen emitting each recompiled function as a weak
// alias to `__imp__sub_<addr>` (DEFINE_REX_FUNC), with both names declared. A
// strong definition here wins at link time, and `__imp__` still reaches the
// original -- so we can observe or replace, per call site, without touching the
// SDK or the generated sources.

#include "kameo_graphics_system.h"

#include <kameorepowered_init.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>

#include <fmt/format.h>

#include "shader_cache.h"

// Header-only: this project does not enable the C language, so xxhash.c
// cannot be added as a source.
#define XXH_INLINE_ALL
#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(kameo_gfx_legacy_overlay, false, "Kameo",
                    "Draw DrawVerticesUP geometry through the old hand-written 2D overlay path "
                    "instead of the general draw path. The overlay path predates the general "
                    "one and cannot render into the storybook's page render targets, which is "
                    "exactly what it got wrong: with the overlay path the menu's text was "
                    "drawn to the SCREEN in 640x480 UI space and scaled up, so the button "
                    "plates came out empty, the A/B glyphs were missing, and the page text was "
                    "oversized. Through the general path all three page passes (640x480, "
                    "240x320, 480x640) render into their own targets and the menu matches the "
                    "console. Now defaults OFF; set it true to get the old path back. Note the "
                    "declaration is NOT a "
                    "difference between them: D3D::ProcessLazyStreams reads it from 0x2C90, "
                    "which is what the capture uses, and nothing reads the FVF slot at 0x3010.");

REXCVAR_DEFINE_BOOL(kameo_gfx_dump_textures, false, "Kameo",
                    "Write every converted RGBA texture to texdump/<addr>_WxH_fmtN.tga and "
                    "report what is in its texels -- how much of the alpha is zero, how much "
                    "is opaque, how much of the RGB is black. Answers whether a surface that "
                    "draws black has no picture in it or has a picture we are drawing wrong; "
                    "the magenta fallback and the all-zero check together cannot tell those "
                    "apart, because a texture with real pixels trips neither.");

REXCVAR_DEFINE_BOOL(kameo_gfx_dump_shaders, false, "Kameo",
                    "Write every guest shader container to shader_dump/ so the translated "
                    "shader cache can be regenerated. Most of Kameo's shaders are decompressed "
                    "out of packed game data at load time and never appear in the XEX, so a "
                    "capture run is the only way to collect them.");

REXCVAR_DEFINE_BOOL(kameo_gfx_preserve_aspect, false, "Kameo",
                    "Letterbox rather than stretch. The guest renders 16:9 and its "
                    "projection assumes it, so scaling X and Y independently to fill the "
                    "window distorts everything the moment the client area is not 16:9 -- "
                    "a 2576x1048 window stretches the picture horizontally by about 1.4x, "
                    "which reads as the camera being wrong rather than as the window being "
                    "the wrong shape. "
                    "DEFAULT OFF, because it is only half done: it letterboxes the 3D scene but "
                    "the HUD keeps its own placement, so at a maximised size the two no longer "
                    "agree and the bars and button prompts sit outside the picture. Windowed at "
                    "roughly 16:9 it is fine. Finishing it means putting the HUD through the same "
                    "transform -- those draws do not all carry the scene's viewport, which is why "
                    "offsetting by the guest 1280x720 rectangle alone does not place them.");

REXCVAR_DEFINE_BOOL(kameo_gfx_capture_begin_vertices, true, "Kameo",
                    "Capture standalone D3DDevice_BeginVertices draws. This is the 2D icon "
                    "system -- health and energy bars, warrior button prompts, boss bars -- "
                    "and about 250 of them a frame used to be counted and then thrown away, "
                    "which is why the HUD had text but no bars and no button glyphs. Set "
                    "false to get the old behaviour back.");

REXCVAR_DEFINE_BOOL(kameo_gfx_dxn_swap, true, "Kameo",
                    "Swap the two BC4 sub-blocks of every DXN (xenos-fmt 49) texture on "
                    "upload. Xbox 360 DXN and DXGI BC5_UNORM both store two BC4 blocks per "
                    "4x4, but not in the same order, so mapping DXN straight onto BC5 hands "
                    "the shader a normal map with X and Y transposed -- light arrives from "
                    "the wrong direction per texel and skin reads as hard and metallic. Set "
                    "false to get the untransposed upload back for comparison.");

REXCVAR_DEFINE_INT32(kameo_gfx_only_primitive, -1, "Kameo",
                     "Draw ONLY draws of this guest primitive type and drop everything "
                     "else. The inverse of kameo_gfx_skip_primitive, and the more useful "
                     "of the two: seeing a suspect primitive alone against the cleared "
                     "background shows its real shape and coverage, which is invisible "
                     "when it is buried under the rest of the scene. -1 disables it.");

REXCVAR_DEFINE_BOOL(kameo_gfx_force_vertex_color, false, "Kameo",
                    "Overwrite every COLOR vertex element with opaque white after the "
                    "transcode. Answers one question the vertex bytes cannot: whether the "
                    "COLOR attribute REACHES the shader at all. If a surface that draws "
                    "black is unchanged by this, the attribute is not arriving and the "
                    "input layout is at fault; if it changes, the attribute arrives and "
                    "the values or what the shader does with them are.");

REXCVAR_DEFINE_INT32(kameo_gfx_skip_primitive, -1, "Kameo",
                   "Drop every draw whose guest primitive type equals this value "
                   "(13 = QUADLIST, the type grass and the 2D overlay use). A bisection "
                   "tool, not a fix: when a screenful of black polygons has no obvious "
                   "owner, removing one primitive type and re-screenshotting answers "
                   "\"is this the grass?\" outright, where reasoning from the vertex "
                   "data cannot. -1 disables it.");

REXCVAR_DEFINE_BOOL(kameo_native_renderer, false, "Kameo",
                    "Use the native plume renderer instead of the xenos GPU plugin. "
                    "Work in progress: currently clears the screen and presents, "
                    "so the game will not be visible.");

namespace kameo::gfx {

bool NativeRendererEnabled() { return REXCVAR_GET(kameo_native_renderer); }

void GfxTrace(const char* what);  // defined below, once Trace() is in scope

}  // namespace kameo::gfx

namespace {

// Log the first time each hook is reached, so a run tells us which of these the
// game actually depends on rather than us guessing.
void LogOnce(const char* what) {
  static std::atomic<uint32_t> seen{0};
  static const char* names[32] = {};
  const uint32_t n = seen.load(std::memory_order_relaxed);
  for (uint32_t i = 0; i < n && i < 32; ++i) {
    if (names[i] == what) return;
  }
  if (n < 32) {
    names[n] = what;
    seen.store(n + 1, std::memory_order_relaxed);
  }
  REXLOG_INFO("[kameo-gfx] first call: {}", what);
}

// --- Stall watchdog --------------------------------------------------------
//
// The guest wedges roughly ten seconds in, and both the guest thread and the
// D3D worker stop together -- so anything that logs from those threads stops
// too, which is exactly when we most want information. This runs on a HOST
// thread so it keeps reporting through a guest deadlock, and prints the last
// hook each guest thread entered. Whatever is named last is where it is stuck.

std::atomic<const char*> g_last_hook{"<none>"};
std::atomic<uint32_t> g_last_thread{0};
std::atomic<uint64_t> g_hook_seq{0};

void Trace(const char* what) {
  g_last_hook.store(what, std::memory_order_relaxed);
  g_last_thread.store(::GetCurrentThreadId(), std::memory_order_relaxed);
  g_hook_seq.fetch_add(1, std::memory_order_relaxed);
}

void StartWatchdog();

// Every hook funnels through here: records the call for the watchdog and logs
// the first occurrence.
void Enter(const char* what) {
  StartWatchdog();
  Trace(what);
  LogOnce(what);
}

// Walk every other thread in the process and log its stack. The recompiled
// guest functions are ordinary x86 functions named sub_<guest address>, so a
// symbolised stack names the exact guest function that is wedged -- which beats
// decompiling candidates and guessing, as the last few rounds showed.
void DumpAllThreadStacks() {
  const DWORD self_pid = ::GetCurrentProcessId();
  const DWORD self_tid = ::GetCurrentThreadId();
  HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return;
  }

  static std::once_flag sym_once;
  std::call_once(sym_once, [] {
    ::SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    ::SymInitialize(::GetCurrentProcess(), nullptr, TRUE);
  });

  THREADENTRY32 te{};
  te.dwSize = sizeof(te);
  if (::Thread32First(snapshot, &te)) {
    do {
      if (te.th32OwnerProcessID != self_pid || te.th32ThreadID == self_tid) {
        continue;
      }
      HANDLE thread = ::OpenThread(
          THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME, FALSE,
          te.th32ThreadID);
      if (!thread) {
        continue;
      }

      // Capture under suspension, then resume BEFORE symbolising: dbghelp takes
      // locks, and holding a thread suspended across that risks deadlocking on
      // whatever it was holding.
      CONTEXT context{};
      context.ContextFlags = CONTEXT_FULL;
      DWORD64 frames[24]{};
      int frame_count = 0;
      if (::SuspendThread(thread) != (DWORD)-1) {
        if (::GetThreadContext(thread, &context)) {
          STACKFRAME64 sf{};
          sf.AddrPC.Offset = context.Rip;
          sf.AddrPC.Mode = AddrModeFlat;
          sf.AddrFrame.Offset = context.Rbp;
          sf.AddrFrame.Mode = AddrModeFlat;
          sf.AddrStack.Offset = context.Rsp;
          sf.AddrStack.Mode = AddrModeFlat;
          while (frame_count < 24 &&
                 ::StackWalk64(IMAGE_FILE_MACHINE_AMD64, ::GetCurrentProcess(), thread, &sf,
                               &context, nullptr, ::SymFunctionTableAccess64, ::SymGetModuleBase64,
                               nullptr) &&
                 sf.AddrPC.Offset) {
            frames[frame_count++] = sf.AddrPC.Offset;
          }
        }
        ::ResumeThread(thread);
      }
      ::CloseHandle(thread);

      if (frame_count == 0) {
        continue;
      }

      std::string text;
      alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256]{};
      auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
      symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
      symbol->MaxNameLen = 255;
      for (int i = 0; i < frame_count; ++i) {
        DWORD64 displacement = 0;
        if (::SymFromAddr(::GetCurrentProcess(), frames[i], &displacement, symbol)) {
          text += fmt::format("\n    {}", symbol->Name);
        } else {
          text += fmt::format("\n    {:016X}", frames[i]);
        }
      }
      REXLOG_WARN("[kameo-gfx] thread {} stack:{}", te.th32ThreadID, text);
    } while (::Thread32Next(snapshot, &te));
  }
  ::CloseHandle(snapshot);
}

// Access violations were killing the process with nothing in the log -- the
// watchdog only reports STALLS, and a hard fault is the opposite of a stall.
// This vectored handler symbolises the faulting frame and lets the exception
// continue to its normal fate, so a crash names the function that caused it.
//
// It deliberately only reports ACCESS_VIOLATION. The guest raises FP exceptions
// as a matter of course (that is what InstallGuestFpExceptionHandlerWin is for)
// and logging those would bury the log.
LONG CALLBACK GfxCrashHandler(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  static std::atomic<bool> reported{false};
  if (reported.exchange(true)) {
    return EXCEPTION_CONTINUE_SEARCH;  // one report; the first is the useful one
  }

  static std::once_flag sym_once;
  std::call_once(sym_once, [] {
    ::SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    ::SymInitialize(::GetCurrentProcess(), nullptr, TRUE);
  });

  const auto* record = info->ExceptionRecord;
  const char* what = record->ExceptionInformation[0] == 1 ? "write" : "read";
  REXLOG_ERROR("[kameo-gfx] ACCESS VIOLATION: {} at {:016X}, faulting address {:016X}", what,
               uint64_t(record->ExceptionAddress), uint64_t(record->ExceptionInformation[1]));

  CONTEXT context = *info->ContextRecord;
  STACKFRAME64 sf{};
  sf.AddrPC.Offset = context.Rip;
  sf.AddrPC.Mode = AddrModeFlat;
  sf.AddrFrame.Offset = context.Rbp;
  sf.AddrFrame.Mode = AddrModeFlat;
  sf.AddrStack.Offset = context.Rsp;
  sf.AddrStack.Mode = AddrModeFlat;

  std::string text;
  alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256]{};
  auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = 255;
  for (int i = 0; i < 24; ++i) {
    if (!::StackWalk64(IMAGE_FILE_MACHINE_AMD64, ::GetCurrentProcess(), ::GetCurrentThread(), &sf,
                       &context, nullptr, ::SymFunctionTableAccess64, ::SymGetModuleBase64,
                       nullptr) ||
        !sf.AddrPC.Offset) {
      break;
    }
    DWORD64 displacement = 0;
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD line_displacement = 0;
    if (::SymFromAddr(::GetCurrentProcess(), sf.AddrPC.Offset, &displacement, symbol)) {
      if (::SymGetLineFromAddr64(::GetCurrentProcess(), sf.AddrPC.Offset, &line_displacement,
                                 &line)) {
        text += fmt::format("\n    {} ({}:{})", symbol->Name, line.FileName, line.LineNumber);
      } else {
        text += fmt::format("\n    {}+{}", symbol->Name, displacement);
      }
    } else {
      text += fmt::format("\n    {:016X}", sf.AddrPC.Offset);
    }
  }
  REXLOG_ERROR("[kameo-gfx] faulting stack:{}", text);
  return EXCEPTION_CONTINUE_SEARCH;
}

void StartWatchdog() {
  static std::once_flag once;
  ::AddVectoredExceptionHandler(0, GfxCrashHandler);
  std::call_once(once, [] {
    std::thread([] {
      uint64_t previous = 0;
      uint32_t quiet = 0;
      bool dumped = false;
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        const uint64_t seq = g_hook_seq.load(std::memory_order_relaxed);
        if (seq == previous) {
          ++quiet;
          REXLOG_WARN("[kameo-gfx] WATCHDOG: no D3D hook for {}s; last was '{}' on thread {} (seq {})",
                      quiet * 2, g_last_hook.load(std::memory_order_relaxed),
                      g_last_thread.load(std::memory_order_relaxed), seq);
          if (quiet >= 2 && !dumped) {
            dumped = true;  // once per run; this is noisy and not cheap
            DumpAllThreadStacks();
          }
        } else {
          quiet = 0;
          dumped = false;
          REXLOG_INFO("[kameo-gfx] watchdog: {} hook calls, last '{}'", seq - previous,
                      g_last_hook.load(std::memory_order_relaxed));
        }
        previous = seq;
      }
    }).detach();
  });
}

}  // namespace

namespace kameo::gfx {
void GfxTrace(const char* what) { Trace(what); }
}  // namespace kameo::gfx

// --- Synchronisation: replaced, not satisfied ------------------------------
//
// These are the functions that wait for the GPU. With no GPU behind them they
// would wait forever, which is what left the window black.
//
// The alternative -- running a vsync worker and dispatching guest interrupts so
// the real D3D keeps making progress -- means rebuilding the emulator we are
// trying to replace. Removing the waits is both smaller and truer to the
// design: once draws are serviced natively, there is genuinely nothing for the
// game to wait on.

// D3DDevice_BlockUntilIdle(D3DDevice*) -> void
extern "C" REX_FUNC(sub_820CE0A0) {
  if (kameo::gfx::KameoGraphicsSystem::instance() == nullptr) {
    __imp__sub_820CE0A0(ctx, base);
    return;
  }
  Enter("BlockUntilIdle");
  // Already idle: we submit and wait inside PresentClear.
}

// D3DDevice_IsFencePending(unsigned Fence) -> int
extern "C" REX_FUNC(sub_820CE128) {
  if (kameo::gfx::KameoGraphicsSystem::instance() == nullptr) {
    __imp__sub_820CE128(ctx, base);
    return;
  }
  Enter("IsFencePending");
  ctx.r3.u64 = 0;  // nothing is ever pending
}

// D3DDevice_SynchronizeToPresentationInterval(D3DDevice*) -> void
extern "C" REX_FUNC(sub_820CF868) {
  if (kameo::gfx::KameoGraphicsSystem::instance() == nullptr) {
    __imp__sub_820CF868(ctx, base);
    return;
  }
  Enter("SynchronizeToPresentationInterval");
  // Pacing comes from the swap chain's own present, not from waiting on vblank.
}

// D3DResource_BlockUntilNotBusy(D3DResource*) -> void
extern "C" REX_FUNC(sub_820CE1B8) {
  if (kameo::gfx::KameoGraphicsSystem::instance() == nullptr) {
    __imp__sub_820CE1B8(ctx, base);
    return;
  }
  Enter("BlockUntilNotBusy");
}

// D3D::CDevice::BlockOnFence(CDevice*, uint* target) -> void
//
// This is what wedged the first working run: it spins
//     while (current - target < current - *readback) DeadlockDetector::Check();
// waiting for the GPU's read-back pointer at **(device + 10384) to catch up to
// the CPU fence at *(device + 10396). With no GPU nothing ever writes it, so the
// loop never exits -- the game played the Microsoft Bink logo, then hung.
//
// Rather than stub the wait, publish the truth: we never consume the ring
// buffer, so everything the guest has queued is already "done". Writing the
// current fence into the read-back slot makes this loop -- and any other fence
// comparison, including inlined ones we cannot hook -- resolve as complete.
extern "C" REX_FUNC(sub_820CDD80) {
  if (kameo::gfx::KameoGraphicsSystem::instance() == nullptr) {
    __imp__sub_820CDD80(ctx, base);
    return;
  }
  Enter("BlockOnFence");

  const uint32_t device = ctx.r3.u32;
  if (device) {
    const uint32_t readback_ptr = REX_LOAD_U32(device + 10384);
    const uint32_t fence = REX_LOAD_U32(device + 10396);
    if (readback_ptr) {
      REX_STORE_U32(readback_ptr, fence);
    }
  }
}

// D3D::CDevice::BlockOnSecondaryPosition(CDevice*, uint position, byte segment)
//
// THIS is the real stall, found by walking the wedged thread's stack rather than
// by guessing:
//
//     mainRender -> ... -> D3DDevice_Resolve -> BeginRingAlloc
//                       -> RingBufferDeviceAllocate -> BlockOnSecondaryPosition
//
// It spins until the GPU's read-back block reports having consumed past
// `position` in ring segment `segment`. The read-back word lives at
// *(device + 10384) + 4, packed as (position & ~3) | (segment & 3). With no GPU
// consuming the ring buffer it never advances, the allocator can never wrap, and
// the render thread spins forever -- about ten seconds in, once the buffer has
// filled for the first time.
//
// As with the fence, the honest fix is not to fake progress but to stop lying:
// we never read the ring buffer at all, so everything in it IS consumed.
// Publishing that makes the wait fall straight through.
extern "C" REX_FUNC(sub_820CD0C8) {
  if (kameo::gfx::KameoGraphicsSystem::instance() == nullptr) {
    __imp__sub_820CD0C8(ctx, base);
    return;
  }
  Enter("BlockOnSecondaryPosition");

  const uint32_t device = ctx.r3.u32;
  const uint32_t position = ctx.r4.u32;
  const uint32_t segment = ctx.r5.u32 & 3;
  if (device) {
    const uint32_t readback = REX_LOAD_U32(device + 10384);
    if (readback) {
      REX_STORE_U32(readback + 4, (position & 0xFFFFFFFCu) | segment);
    }
  }
}

// --- Draw-path census -------------------------------------------------------
//
// Before implementing the title-screen overlay, find out which submission path
// the game actually uses for it. Guessing here is expensive: the 2D helpers
// (drawTextureQuad*) are resolve/post blits, while the HUD may well go through
// DrawVerticesUP or BeginVertices instead, and each implies a different amount
// of work (a vertex scratch buffer, a texture untiler, or both).
//
// Every hook below tees -- it counts and then runs the original -- so this is
// observation only and the build behaves exactly as it did without it.

namespace {

enum DrawPath {
  kDrawTextureQuadNoSetup,
  kDrawVertices,
  kDrawIndexedVertices,
  kDrawVerticesUP,
  kBeginVertices,
  kSetTexture,
  kDrawPathCount,
};

const char* const kDrawPathNames[kDrawPathCount] = {
    "drawTextureQuadNoSetup", "DrawVertices",  "DrawIndexedVertices",
    "DrawVerticesUP",         "BeginVertices", "SetTexture",
};

std::atomic<uint32_t> g_drawCounts[kDrawPathCount];

void CountDraw(DrawPath path) { g_drawCounts[path].fetch_add(1, std::memory_order_relaxed); }

void ReportDrawCensus() {
  std::string line;
  for (int i = 0; i < kDrawPathCount; ++i) {
    const uint32_t n = g_drawCounts[i].exchange(0, std::memory_order_relaxed);
    if (n) {
      line += fmt::format(" {}={}", kDrawPathNames[i], n);
    }
  }
  REXLOG_INFO("[kameo-gfx] draw census (per 300 frames):{}", line.empty() ? " nothing" : line);
}

// --- Overlay capture --------------------------------------------------------
//
// The title screen emits one pre-transformed quad per glyph. They accumulate
// here through the frame and go out as a single batched draw at present time.
//
// Pre-transformed vertices are in the guest's VIEWPORT space, so the conversion
// to clip space has to use the viewport the game actually set -- assuming 720p
// put the text at roughly half scale in the top-left corner, because the guest
// viewport is not the full render target.
//
// D3DDevice_SetViewport (0x820C24D0) stores the clipped rect as four dwords:
//   device + 12808  X
//   device + 12812  Y
//   device + 12816  Width
//   device + 12820  Height
constexpr uint32_t kViewportX = 12808;
constexpr uint32_t kViewportY = 12812;
constexpr uint32_t kViewportWidth = 12816;
constexpr uint32_t kViewportHeight = 12820;

// Xenos texture format 2 = k_8: a single 8-bit channel. That is what a font
// atlas is, and it is the only format the overlay path handles so far.
constexpr uint32_t kXenosFormat_8 = 2;

// Set around DrawVerticesUP's delegation so the BeginVertices hook can tell its
// internal calls from standalone ones.
thread_local bool g_inDrawVerticesUP = false;

// Which vertex shader constant registers the game actually writes.
//
// Reading c0..c3 at the logo draw produced a clean pixel-to-clip matrix
// (c0.x = 2/1280, c1.y = -2/720 exactly) but with translate -0.6719 instead of
// -1.0, placing the quad off the top-left. SHADERS.md independently confirms the
// vertex constant block is at device + 0x780 = 1920, which is the offset used,
// so the block is right and the REGISTERS are wrong -- the UI shader reads some
// other range.
//
// Rather than disassemble the shader, record what the game sets: the transform
// has to be written shortly before the draw that uses it.
struct ConstantWrite {
  uint32_t start;
  uint32_t count;
};
std::mutex g_constMutex;
ConstantWrite g_recentConstants[12];
uint32_t g_recentConstantCount = 0;

std::mutex g_overlayMutex;
// One entry per RUN of consecutive glyphs sharing an atlas. Runs rather than
// one-batch-per-texture because the glyphs alpha-blend, so submission order has
// to survive.
std::vector<kameo::gfx::OverlayBatch> g_overlayBatches;

// Both D3DDevice_Present and D3DDevice_Swap can reach us: Present is the guest's
// own per-frame call, Swap is what D3D's worker thread uses. Whichever arrives
// first drives the frame.
std::atomic<uint32_t> g_binkCallsThisFrame{0};

// One-shot dump of the loaded guest image.
//
// scripts/build_shader_cache.py needs the DECOMPRESSED image to scan for shader
// containers, and the shipping assets/default.xex is compressed -- a raw scan of
// it finds zero containers. rexglue has no decompress subcommand and XenosRecomp
// takes an image, not a XEX.
//
// But the runtime has already done the work: it maps the decompressed module at
// 82000000-82BF0000 (the range the function table reports at startup). Dumping
// that range gives exactly the input the script wants, with no XEX2/LZX
// decompressor to write. Byte order is preserved -- SHADERS.md records that the
// ucode blobs matched the XEX blobs with no swapping.
void DumpGuestImageOnce(uint8_t* base) {
  static std::once_flag once;
  std::call_once(once, [base] {
    constexpr uint32_t kImageBase = 0x82000000;
    constexpr uint32_t kImageEnd = 0x82BF0000;
    constexpr uint32_t kImageSize = kImageEnd - kImageBase;
    if (FILE* f = fopen("guest_image.bin", "wb")) {
      const size_t written = fwrite(base + kImageBase, 1, kImageSize, f);
      fclose(f);
      REXLOG_INFO("[kameo-gfx] wrote guest_image.bin ({} of {} bytes from {:08X})", written,
                  kImageSize, kImageBase);
    } else {
      REXLOG_WARN("[kameo-gfx] could not open guest_image.bin for writing");
    }
  });
}

void DriveFrame(kameo::gfx::KameoGraphicsSystem* gfx) {
  const uint32_t bink_calls = g_binkCallsThisFrame.exchange(0, std::memory_order_relaxed);
  static uint32_t reported = 0;
  if (bink_calls > 1 && reported < 5) {
    ++reported;
    REXLOG_WARN("[kameo-gfx] Draw_Bink_textures ran {}x this frame -- one full-screen quad is the wrong model",
                bink_calls);
  }
  {
    // Hand off whatever glyphs accumulated this frame and start a fresh batch.
    std::lock_guard<std::mutex> lock(g_overlayMutex);
    if (!g_overlayBatches.empty()) {
      gfx->SubmitOverlay(std::move(g_overlayBatches));
    }
    g_overlayBatches.clear();
  }

  // Same handoff for the scene geometry captured since the last swap.
  kameo::gfx::SubmitCapturedFrame();

  static std::atomic<uint32_t> frame{0};
  const uint32_t n = frame.fetch_add(1, std::memory_order_relaxed);
  if ((n % 300) == 0) {
    ReportDrawCensus();
  }
  const float t = float(n) * 0.01f;
  gfx->PresentClear(0.05f, 0.05f + 0.05f * (0.5f + 0.5f * __builtin_sinf(t)), 0.15f);
}

}  // namespace

// D3D::CDevice::BeginRingAlloc(CDevice*, dwords, alignment)
// Traced only. An earlier version forced allocation from the ring base to dodge
// exhaustion; that was papering over the read positions never advancing, which
// is what the BlockOn* hooks now fix properly.
extern "C" REX_FUNC(sub_820CD390) {
  if (kameo::gfx::KameoGraphicsSystem::instance() == nullptr) {
    __imp__sub_820CD390(ctx, base);
    return;
  }
  Enter("BeginRingAlloc");
  __imp__sub_820CD390(ctx, base);
}

// D3D::CDevice::KickOff(CDevice*) -> command write pointer
//
// NOT stubbed, deliberately. An earlier attempt returned the write pointer
// unchanged and skipped submission; the caller loops
// `if (write > limit) write = KickOff(device)`, saw no progress, and spun
// 170 MILLION times per two seconds.
//
// KickOff recycles the command buffer, and the game's PM4 writers are INLINED
// into its own code, so they cannot be hooked -- meaning the buffer really does
// get written and really does need recycling. Let the original do that; the
// waits inside it are handled by the BlockOn* hooks below.
extern "C" REX_FUNC(sub_820CDC88) {
  if (kameo::gfx::KameoGraphicsSystem::instance() == nullptr) {
    __imp__sub_820CDC88(ctx, base);
    return;
  }
  Enter("KickOff");
  __imp__sub_820CDC88(ctx, base);
}

// D3D::CDevice::BlockOnPrimaryRange(CDevice*, pos, size) -> wrapped end pos
//
// The third and last of the ring waits. Spins until *(readback + 60), the GPU's
// primary read position, passes the requested range. Publish it and return the
// wrapped end, exactly as the original would once the GPU had caught up.
extern "C" REX_FUNC(sub_820CD578) {
  if (kameo::gfx::KameoGraphicsSystem::instance() == nullptr) {
    __imp__sub_820CD578(ctx, base);
    return;
  }
  Enter("BlockOnPrimaryRange");

  const uint32_t device = ctx.r3.u32;
  const uint32_t pos = ctx.r4.u32;
  const uint32_t size = ctx.r5.u32;
  uint32_t end = pos + size;
  if (device) {
    end &= REX_LOAD_U32(device + 13968);  // ring size mask
    const uint32_t readback = REX_LOAD_U32(device + 10384);
    if (readback) {
      REX_STORE_U32(readback + 60, end);
    }
  }
  ctx.r3.u64 = end;
}

// D3DDevice_Present(D3DDevice*). Drives the frame from the guest's own present
// call. Swap is hooked too because D3D's worker thread calls it directly.
extern "C" REX_FUNC(sub_820D0048) {
  auto* gfx = kameo::gfx::KameoGraphicsSystem::instance();
  if (gfx == nullptr) {
    __imp__sub_820D0048(ctx, base);
    return;
  }
  Enter("Present");
  DumpGuestImageOnce(base);
  DriveFrame(gfx);
}

// D3DDevice_Swap (0x820CF9D8). The game calls this once per frame from
// D3DDevice_Present and from D3D's own worker thread, which makes it the
// natural place to drive presentation while the guest GPU is not emulated.
extern "C" REX_FUNC(sub_820CF9D8) {
  auto* gfx = kameo::gfx::KameoGraphicsSystem::instance();
  if (gfx == nullptr) {
    __imp__sub_820CF9D8(ctx, base);
    return;
  }

  Enter("Swap");
  DumpGuestImageOnce(base);
  DriveFrame(gfx);

  // Deliberately NOT calling the original: it would push a present packet into
  // a ring buffer that nothing is consuming.
}

// XGetVideoMode(PXVIDEO_MODE) -- 0x826CED3C, an import thunk into the host.
//
// This is why the overlay was laid out in a 640x480 space. x_g_kamVideoParams
// (0x820BECA8) sets the game's UI resolution from the reported mode:
//
//     kamVideoParams = 640; word_8275D24A = 480;      // default
//     XGetVideoMode(&mode);
//     if (mode.fIsHiDef && mode.dwDisplayHeight >= 0x2D0)
//         { kamVideoParams = 1280; word_8275D24A = 720; ... }
//
// Whatever the runtime reports here does not satisfy that, so the game stays at
// 640x480 and every pre-transformed UI vertex comes out at 4:3 half scale.
// Scaling those in the renderer was compensating for the wrong video mode and
// could not preserve glyph aspect; reporting the mode the console actually had
// makes the guest lay the UI out at 1280x720 itself.
//
// NOTE, so this is not re-attempted: the UI is NOT laid out at 640x480 because
// of a bad video mode. A hook on x_g_kamVideoParams (0x820BECA8) that logged the
// value before overriding it reported the guest UI resolution was ALREADY
// 1280x720 -- XGetVideoMode reports a HiDef 720p mode and the game takes its
// own `kamVideoParams = 1280; height = 720` branch. Forcing those values changes
// nothing, and the overlay vertices still arrive in a ~640x480 space.
//
// So the scale lives somewhere else, almost certainly the UI vertex shader's
// constants -- meaning these vertices are not truly pre-transformed and `rhw=1.0`
// at offset 12 does not prove a pass-through shader. Resolving it properly needs
// the shader path; see src/gfx/README.md.

// --- Shader cache binding ---------------------------------------------------
//
// Associates each guest shader object with its translated DXIL/SPIR-V, captured
// at CREATION rather than at draw time.
//
// Hashing the bound shader's ucode cannot work: XGRegisterVertexShader patches
// vertex fetch into the microcode per vertex declaration, so no vertex shader
// reaches the GPU as authored. Dumping a bound object confirms it -- BaseFlush
// is zero (shaders do not carry their data address there the way textures do)
// and the pointer at +40 is patched ucode with no container magic.
//
// D3DDevice_CreateVertexShader / CreatePixelShader receive the AUTHORED
// container, which is exactly what the cache was built from, and return the
// object later seen bound at device + 20440.

namespace {

std::mutex g_shaderMutex;
// Guest shader object -> translated cache entry, recorded at CREATION. The
// draw path resolves the bound objects through this every draw, so it is a map
// rather than the append-only list this started as: shader objects are freed
// and their addresses reused, and only the newest binding for an address is
// correct.
std::unordered_map<uint32_t, const ShaderCacheEntry*> g_shaderBindings;
uint32_t g_shaderHits = 0;
uint32_t g_shaderMisses = 0;

// Entries are sorted by hash, exactly as UnleashedRecomp's FindShaderCacheEntry
// assumes.
const ShaderCacheEntry* FindShaderCacheEntry(uint64_t hash) {
  const ShaderCacheEntry* begin = g_shaderCacheEntries;
  const ShaderCacheEntry* end = g_shaderCacheEntries + g_shaderCacheEntryCount;
  const ShaderCacheEntry* found = std::lower_bound(
      begin, end, hash,
      [](const ShaderCacheEntry& lhs, uint64_t rhs) { return lhs.hash < rhs; });
  return (found != end && found->hash == hash) ? found : nullptr;
}

// The container header is big-endian in guest memory, so the two sizes are read
// byte-swapped -- but the HASH is taken over the raw bytes as stored, because
// the cache was generated from a dump of that same memory.
void RegisterShader(uint8_t* base, uint32_t function, uint32_t object, const char* what) {
  if (!function || !object) {
    return;
  }
  const uint32_t virtual_size = REX_LOAD_U32(function + 4);
  const uint32_t physical_size = REX_LOAD_U32(function + 8);
  const uint64_t total = uint64_t(virtual_size) + physical_size;
  if (total == 0 || total > 0x100000) {
    REXLOG_WARN("[kameo-gfx] {} container at {:08X} has implausible size {}", what, function,
                total);
    return;
  }

  const uint64_t hash = XXH3_64bits(base + function, size_t(total));
  const ShaderCacheEntry* entry = FindShaderCacheEntry(hash);

  std::lock_guard<std::mutex> lock(g_shaderMutex);
  if (entry) {
    ++g_shaderHits;
    g_shaderBindings.insert_or_assign(object, entry);
    if (auto* gfx = kameo::gfx::KameoGraphicsSystem::instance()) {
      gfx->QueueShaderModule(entry);
    }
  } else {
    ++g_shaderMisses;
  }
  // Where the container LIVES is the question that matters for the miss rate.
  // The static cache was built by scanning the XEX image (82000000-82BF0000).
  // If missing containers sit outside that range they came from game data at
  // load time, which SHADERS.md's raw scan of assets/ could not have seen
  // because that data is packed -- and it would mean a static cache can never
  // be complete.
  const bool in_image = function >= 0x82000000u && function < 0x82BF0000u;

  // Capture every container we cannot resolve. Scanning the XEX can never find
  // these -- they are decompressed out of packed game data into the heap at load
  // time -- so the only way to build a complete cache is to collect them here
  // and translate them offline. XenosRecomp's --blobs mode takes exactly this:
  // one container per file.
  //
  // With kameo_gfx_dump_shaders on, EVERY container is written, not only the
  // unresolved ones. Regenerating the cache needs the whole corpus, and once a
  // cache exists almost nothing misses -- so "dump the misses" collects a dozen
  // files instead of the thousand the cache was built from.
  if (!entry || REXCVAR_GET(kameo_gfx_dump_shaders)) {
    static std::mutex dump_mutex;
    static std::vector<uint64_t> dumped;
    std::lock_guard<std::mutex> dump_lock(dump_mutex);
    if (std::find(dumped.begin(), dumped.end(), hash) == dumped.end()) {
      dumped.push_back(hash);
      ::CreateDirectoryA("shader_dump", nullptr);
      const std::string path = fmt::format("shader_dump/{:016X}.bin", hash);
      if (FILE* f = fopen(path.c_str(), "wb")) {
        fwrite(base + function, 1, size_t(total), f);
        fclose(f);
      }
      if (dumped.size() % 100 == 0) {
        REXLOG_INFO("[kameo-gfx] captured {} unique unresolved shader containers", dumped.size());
      }
    }
  }
  if (!entry && g_shaderMisses <= 6) {
    REXLOG_WARN("[kameo-gfx] MISS: container {:08X} ({}) magic {:08X} vsize {} psize {} hash {:016X}",
                function, in_image ? "in XEX image" : "OUTSIDE image (loaded from data)",
                REX_LOAD_U32(function), virtual_size, physical_size, hash);
  }
  if ((g_shaderHits + g_shaderMisses) <= 8 || (g_shaderHits + g_shaderMisses) % 200 == 0) {
    REXLOG_INFO("[kameo-gfx] {} {:08X} container {:08X} {} -> {} (cache {} hit / {} miss)", what,
                object, function, in_image ? "in-image" : "out-of-image",
                entry ? "FOUND" : "missing", g_shaderHits, g_shaderMisses);
  }
}

}  // namespace

namespace kameo::gfx {

// Declared in kameo_graphics_system.h; the draw path calls this once per draw
// for each of the two bound shader objects.
const ShaderCacheEntry* LookupShaderBinding(uint32_t guest_object) {
  if (!guest_object) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(g_shaderMutex);
  auto found = g_shaderBindings.find(guest_object);
  return found != g_shaderBindings.end() ? found->second : nullptr;
}

}  // namespace kameo::gfx

// D3DDevice_CreateVertexShader(const unsigned int *pFunction) -> D3DVertexShader*
extern "C" REX_FUNC(sub_820C86D0) {
  const uint32_t function = ctx.r3.u32;
  __imp__sub_820C86D0(ctx, base);
  if (kameo::gfx::KameoGraphicsSystem::instance() != nullptr) {
    RegisterShader(base, function, ctx.r3.u32, "vertex shader");
  }
}

// D3DDevice_CreatePixelShader(const unsigned int *pFunction) -> D3DPixelShader*
extern "C" REX_FUNC(sub_820C8360) {
  const uint32_t function = ctx.r3.u32;
  __imp__sub_820C8360(ctx, base);
  if (kameo::gfx::KameoGraphicsSystem::instance() != nullptr) {
    RegisterShader(base, function, ctx.r3.u32, "pixel shader");
  }
}

// D3DDevice_SetVertexShaderConstantFN(pDevice, StartRegister, pData, Vector4fCount)
// Records the register ranges written, so the logo draw can report which ones
// were set immediately before it.
extern "C" REX_FUNC(sub_820C7DC0) {
  const uint32_t start = ctx.r4.u32;
  const uint32_t count = ctx.r6.u32;
  __imp__sub_820C7DC0(ctx, base);

  if (kameo::gfx::KameoGraphicsSystem::instance() != nullptr) {
    std::lock_guard<std::mutex> lock(g_constMutex);
    const uint32_t slot = g_recentConstantCount % 12;
    g_recentConstants[slot] = {start, count};
    ++g_recentConstantCount;

    // Report each DISTINCT range once. On the title screen the game only ever
    // writes c0+4, which would make every draw's transform a single 4x4 matrix
    // -- a big simplification for the general path. The storybook menu runs
    // ~4100 indexed draws/frame with skinning and lighting, which would normally
    // want far more than four registers, so this has to be re-confirmed there
    // before anything is built on it. Anything new prints the moment it appears.
    static std::vector<uint64_t> seen;
    static uint32_t max_register = 0;
    const uint64_t key = (uint64_t(start) << 32) | count;
    if (std::find(seen.begin(), seen.end(), key) == seen.end() && seen.size() < 64) {
      seen.push_back(key);
      max_register = std::max(max_register, start + count);
      REXLOG_INFO("[kameo-gfx] NEW VS constant range: c{}+{} (distinct ranges so far {}, highest register {})",
                  start, count, seen.size(), max_register);
    }
  }
}

// --- Census hooks: count and pass through, nothing else ---------------------

#define KAMEO_CENSUS_HOOK(addr, path)                                  \
  extern "C" REX_FUNC(sub_##addr) {                                    \
    if (kameo::gfx::KameoGraphicsSystem::instance() != nullptr) {      \
      CountDraw(path);                                                 \
    }                                                                  \
    __imp__sub_##addr(ctx, base);                                      \
  }

KAMEO_CENSUS_HOOK(820DB8C8, kDrawTextureQuadNoSetup)  // drawTextureQuadNoSetup

// D3DDevice_Resolve(pDevice, Flags, pSourceRect, pDestTexture, pDestPoint, ...)
//
// The guest renders a pass, resolves it into a texture, then samples that
// texture. With no resolve implemented, those destination textures stayed at
// whatever guest memory held -- zeros -- and every surface that sampled one
// drew flat black. Recorded in frame order; the copy happens at the matching
// point of the replayed command stream.
extern "C" REX_FUNC(sub_820C3460) {
  if (kameo::gfx::KameoGraphicsSystem::instance() != nullptr) {
    kameo::gfx::CaptureResolve(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  }
  __imp__sub_820C3460(ctx, base);
}

// D3DDevice_DrawVertices(pDevice, PrimitiveType, StartVertex, VertexCount)
//
// Non-indexed geometry: 36 draws a frame on the storybook menu against ~4100
// indexed ones. Same state, same capture -- only the index buffer is absent.
extern "C" REX_FUNC(sub_820CED48) {
  if (kameo::gfx::KameoGraphicsSystem::instance() != nullptr) {
    CountDraw(kDrawVertices);
    kameo::gfx::CaptureDraw(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  }
  __imp__sub_820CED48(ctx, base);
}
// D3DDevice_DrawIndexedVertices(pDevice, PrimitiveType, BaseVertexIndex,
//                               StartIndex, VertexCount)
//
// The storybook menu's main draw path (~4100/frame). It takes no buffers -- it
// emits PM4 and reads everything from device state -- so this validates the
// state map in src/gfx/README.md against real draws before any pipeline work is
// built on it. Resource data addresses need the same BaseFlush + page-bias
// treatment as the Bink planes.
extern "C" REX_FUNC(sub_820CEF88) {
  if (kameo::gfx::KameoGraphicsSystem::instance() != nullptr) {
    CountDraw(kDrawIndexedVertices);

    static uint32_t logged = 0;
    if (logged < 6) {
      ++logged;
      const uint32_t device = ctx.r3.u32;
      const uint32_t prim = ctx.r4.u32;
      const uint32_t base_vertex = ctx.r5.u32;
      const uint32_t start_index = ctx.r6.u32;
      const uint32_t vertex_count = ctx.r7.u32;

      auto resource_address = [&](uint32_t resource) -> uint32_t {
        if (!resource) return 0;
        const uint32_t base_flush = REX_LOAD_U32(resource + 20);
        return (base_flush & 0xFFFFF000u) + ((((base_flush >> 20) + 512) & 0x1000));
      };

      const uint32_t index_buffer = REX_LOAD_U32(device + 12532);
      const uint32_t decl = REX_LOAD_U32(device + 11408);
      const uint32_t vshader = REX_LOAD_U32(device + 20440);
      const uint32_t stream0 = REX_LOAD_U32(device + 12560);
      const uint32_t stream0_offset = REX_LOAD_U32(device + 12556);
      const uint32_t stream0_stride = REX_LOAD_U8(device + 12688) * 4u;

      REXLOG_INFO("[kameo-gfx] DrawIndexed prim={} baseVtx={} startIdx={} count={}", prim,
                  base_vertex, start_index, vertex_count);
      REXLOG_INFO("[kameo-gfx]   ib={:08X} data={:08X} | vb0={:08X} data={:08X} off={} stride={}",
                  index_buffer, resource_address(index_buffer), stream0,
                  resource_address(stream0), stream0_offset, stream0_stride);
      REXLOG_INFO("[kameo-gfx]   decl={:08X} vshader={:08X}", decl, vshader);

      // Vertex declaration -> input layout. Derived from
      // D3DDevice_CreateVertexDeclaration: it allocates 12*count + 40, stores
      // the element count at +8 and the max stream index at +12, then memcpys
      // the caller's array to +36 at 12 bytes per element. Note Xbox's
      // _D3DVERTEXELEMENT9 is 12 bytes, NOT the 8 PC D3D9 uses -- Type is a
      // 32-bit XGVERTEXFORMAT rather than a WORD.
      if (decl) {
        const uint32_t count = REX_LOAD_U32(decl + 8);
        REXLOG_INFO("[kameo-gfx]   decl elements={} maxStream={}", count,
                    REX_LOAD_U32(decl + 12));
        for (uint32_t e = 0; e < count && e < 10; ++e) {
          const uint32_t el = decl + 36 + 12 * e;
          REXLOG_INFO(
              "[kameo-gfx]     [{}] stream={} offset={} type={:08X} method={} usage={} idx={}", e,
              REX_LOAD_U16(el + 0), REX_LOAD_U16(el + 2), REX_LOAD_U32(el + 4),
              REX_LOAD_U8(el + 8), REX_LOAD_U8(el + 9), REX_LOAD_U8(el + 10));
        }
      }

      // Read the actual geometry. Positions are SHORT4 (packed int16), which
      // usually implies a per-mesh scale/bias somewhere -- dumping the raw
      // values decides whether they are already world-space integers or
      // normalised and needing a scale, which changes the whole vertex path.
      if (stream0 && decl) {
        const uint32_t fetch0 = REX_LOAD_U32(stream0 + 12);
        const uint32_t fetch1 = REX_LOAD_U32(stream0 + 16);
        const uint32_t vb_addr = fetch0 & 0xFFFFFFFCu;
        const uint32_t vb_dwords = (fetch1 >> 2) & 0xFFFFFFu;

        // Find the POSITION element (D3DDECLUSAGE_POSITION == 0).
        uint32_t pos_offset = 0xFFFFFFFFu, pos_type = 0;
        const uint32_t count_el = REX_LOAD_U32(decl + 8);
        for (uint32_t e = 0; e < count_el; ++e) {
          const uint32_t el = decl + 36 + 12 * e;
          if (REX_LOAD_U8(el + 9) == 0) {
            pos_offset = REX_LOAD_U16(el + 2);
            pos_type = REX_LOAD_U32(el + 4);
            break;
          }
        }
        REXLOG_INFO("[kameo-gfx]   vb addr={:08X} sizeDwords={} stride={} posOff={} posType={:08X}",
                    vb_addr, vb_dwords, stream0_stride, pos_offset, pos_type);

        if (vb_addr && pos_offset != 0xFFFFFFFFu && stream0_stride) {
          for (uint32_t v = 0; v < 4; ++v) {
            const uint32_t at = vb_addr + stream0_offset + v * stream0_stride + pos_offset;
            REXLOG_INFO("[kameo-gfx]     v{} pos shorts = {} {} {} {}", v,
                        int16_t(REX_LOAD_U16(at + 0)), int16_t(REX_LOAD_U16(at + 2)),
                        int16_t(REX_LOAD_U16(at + 4)), int16_t(REX_LOAD_U16(at + 6)));
          }
        }

        const uint32_t ib_fetch = index_buffer ? REX_LOAD_U32(index_buffer + 12) : 0;
        if (ib_fetch) {
          const uint32_t ib_addr = ib_fetch & 0xFFFFFFFCu;
          REXLOG_INFO("[kameo-gfx]   ib addr={:08X} first indices = {} {} {} {} {} {}", ib_addr,
                      REX_LOAD_U16(ib_addr + 0), REX_LOAD_U16(ib_addr + 2),
                      REX_LOAD_U16(ib_addr + 4), REX_LOAD_U16(ib_addr + 6),
                      REX_LOAD_U16(ib_addr + 8), REX_LOAD_U16(ib_addr + 10));
        }
      }

      // BaseFlush (+20) comes back ZERO for vertex buffers, exactly as it did
      // for shader objects -- so buffers do not carry their data address there
      // either. Dump the object and chase every field that looks like a guest
      // pointer, the same way the texture descriptor and shader ucode were
      // located.
      if (stream0) {
        std::string raw;
        for (uint32_t i = 0; i < 12; ++i) {
          raw += fmt::format(" {:02d}:{:08X}", i * 4, REX_LOAD_U32(stream0 + 4 * i));
        }
        REXLOG_INFO("[kameo-gfx]   vb0 raw:{}", raw);
      }
    }

    // VertexCount is the number of INDICES, matching the PM4 packet the
    // original builds; BaseVertexIndex is added to every index by the GPU.
    kameo::gfx::CaptureIndexedDraw(base, ctx.r3.u32, ctx.r4.u32, int32_t(ctx.r5.u32), ctx.r6.u32,
                                   ctx.r7.u32);
  }
  __imp__sub_820CEF88(ctx, base);
}
// D3DDevice_DrawVerticesUP(pDevice, PrimitiveType, VertexCount,
//                          pVertexStreamZeroData, VertexStreamZeroStride)
//
// The overlay path. Unlike the standalone BeginVertices pattern, everything
// needed is already in the arguments -- including a guest pointer to the vertex
// data -- so implementing this natively does NOT need the vertex scratch buffer
// that BeginVertices would require. Census says ~148 of these per frame on the
// title screen against ~150 BeginVertices, so essentially all overlay geometry
// arrives here.
//
// Logging distinct (primitive, stride) shapes first: the stride is what tells
// us the vertex layout to declare, and guessing it would mean writing the input
// assembler twice.
extern "C" REX_FUNC(sub_820CE9D8) {
  if (kameo::gfx::KameoGraphicsSystem::instance() != nullptr) {
    CountDraw(kDrawVerticesUP);

    const uint32_t prim = ctx.r4.u32;
    const uint32_t count = ctx.r5.u32;
    const uint32_t data = ctx.r6.u32;
    const uint32_t stride = ctx.r7.u32;

    // One line per distinct (primitive, stride) pair, with the first vertex's
    // raw dwords so the component layout can be read off directly.
    static std::mutex shapes_mutex;
    static std::vector<uint64_t> seen_shapes;
    const uint64_t shape = (uint64_t(prim) << 32) | stride;
    bool is_new = false;
    {
      std::lock_guard<std::mutex> lock(shapes_mutex);
      if (std::find(seen_shapes.begin(), seen_shapes.end(), shape) == seen_shapes.end() &&
          seen_shapes.size() < 24) {
        seen_shapes.push_back(shape);
        is_new = true;
      }
    }
    if (is_new && data && stride && stride <= 128) {
      std::string words;
      for (uint32_t i = 0; i < stride / 4 && i < 16; ++i) {
        const uint32_t bits = REX_LOAD_U32(data + 4 * i);
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        words += fmt::format(" [{}]{:08X}({:.3f})", i, bits, f);
      }
      REXLOG_INFO("[kameo-gfx] DrawVerticesUP shape: prim={} stride={} count={} v0:{}", prim,
                  stride, count, words);

      // Whatever texture is bound on sampler 0 for this shape. D3DDevice_SetTexture
      // stores the pointer at 4*(Sampler + 3176) = device + 12704 + 4*Sampler,
      // which is the same slot set Draw_Bink_textures releases at +12704..12716.
      //
      // The D3DBaseTexture header is D3DResource {Common, ReferenceCount, Fence,
      // ReadFence, Identifier, BaseFlush} then MipFlush, then the 6-dword Xenos
      // fetch constant at +28. SetTexture merges Format.dword[0] into the fetch
      // constant with mask 0x8007FFFF and takes pitch from sampler state, which
      // places pitch at bits 19..30 and leaves **bit 31 as the tiled flag** --
      // the bit that decides whether an untiler has to exist at all.
      // The D3DBaseTexture `Format` field is NOT the GPU descriptor -- reading it
      // returns the resource name string. D3DDevice_SetTexture *assembles* the
      // real Xenos fetch constant into the device's shadow at
      // 24 * (Sampler + 48) = device + 1152 + 24*Sampler, out of Identifier,
      // BaseFlush, MipFlush and Format[0..2].
      //
      // So read the assembled block: it is exactly what the console's GPU
      // sampled, with address, size, format and tiling all in one place, and it
      // needs no reassembly on our side.
      //
      // Word 0 is Identifier-derived: pitch in bits 22..30 and **tiled in bit
      // 31** (SetTexture takes 0xFFC003FF from Identifier and fills bits 10..21
      // from sampler state, which is what pins those fields).
      const uint32_t device = ctx.r3.u32;
      for (uint32_t s = 0; s < 4; ++s) {
        const uint32_t tex = REX_LOAD_U32(device + 12704 + 4 * s);
        if (!tex) continue;
        const uint32_t fc = device + 1152 + 24 * s;
        std::string words;
        for (uint32_t i = 0; i < 6; ++i) {
          words += fmt::format(" w{}={:08X}", i, REX_LOAD_U32(fc + 4 * i));
        }
        const uint32_t w0 = REX_LOAD_U32(fc);
        REXLOG_INFO("[kameo-gfx]   sampler{} tex={:08X} TILED={} pitch={} type={}{}", s, tex,
                    (w0 >> 31) & 1, ((w0 >> 22) & 0x1FF) * 32, w0 & 3, words);
      }
    }

    // The general draw path handles these like any other geometry: same shaders,
    // same declaration, same state -- the vertex data just arrives as a pointer.
    // That is what lets them render into the storybook's page render targets,
    // which the hand-written overlay path below could never do.
    if (!REXCVAR_GET(kameo_gfx_legacy_overlay)) {
      kameo::gfx::CaptureDrawUP(base, ctx.r3.u32, prim, count, data, stride);
    }

    // Capture the glyph. Layout confirmed by measurement (see src/gfx/README.md):
    //   +0  float4 position, x/y in PIXELS with rhw=1.0 (pre-transformed)
    //   +16 D3DCOLOR
    //   +20 float2 uv
    //   +28 32 bytes unused
    if (REXCVAR_GET(kameo_gfx_legacy_overlay) && stride == 60 && prim == 5 && count == 4 && data) {
      const uint32_t device = ctx.r3.u32;
      const uint32_t tex = REX_LOAD_U32(device + 12704);
      const uint32_t fc = device + 1152;
      const uint32_t w0 = REX_LOAD_U32(fc);
      const uint32_t w1 = REX_LOAD_U32(fc + 4);
      const uint32_t w2 = REX_LOAD_U32(fc + 8);

      if (tex && (w1 & 0x3F) == kXenosFormat_8) {
        const uint32_t base_flush = REX_LOAD_U32(tex + 20);
        // Same page adjustment D3D applies when it builds the fetch address.
        const uint32_t addr =
            (base_flush & 0xFFFFF000u) + ((((base_flush >> 20) + 512) & 0x1000));

        auto read_float = [&](uint32_t at) {
          const uint32_t bits = REX_LOAD_U32(at);
          float f;
          std::memcpy(&f, &bits, sizeof(f));
          return f;
        };

        // Use the guest's OWN transform, read from vertex shader constants
        // c0..c3 (device + 1920 + 16*reg). This replaces a hardcoded 640x480,
        // which could never have been right everywhere: the title screen lays UI
        // out in 640x480 but the storybook menu uses 1280x720.
        //
        // Measured at a title-screen text draw, the matrix is exactly a
        // pixel-to-clip transform for that screen's own resolution:
        //
        //   c0 = (0.003125, 0, 0, -1.0)      0.003125 =  2/640
        //   c1 = (0, -0.004167, 0,  1.0)    -0.004167 = -2/480
        //
        // so applying it handles both screens with no constant to pick.
        float m[4][4];
        for (uint32_t reg = 0; reg < 4; ++reg) {
          const uint32_t at = device + 1920 + 16 * reg;
          for (uint32_t c = 0; c < 4; ++c) {
            m[reg][c] = read_float(at + 4 * c);
          }
        }

        kameo::gfx::OverlayVertex quad[4];
        for (uint32_t v = 0; v < 4; ++v) {
          const uint32_t at = data + v * stride;
          // Position is float4 (x, y, z, rhw); transform it by the guest matrix
          // row-wise, exactly as the vertex shader does.
          const float px = read_float(at + 0);
          const float py = read_float(at + 4);
          const float pz = read_float(at + 8);
          const float pw = read_float(at + 12);
          const uint32_t colour = REX_LOAD_U32(at + 16);
          const float cx = px * m[0][0] + py * m[0][1] + pz * m[0][2] + pw * m[0][3];
          const float cy = px * m[1][0] + py * m[1][1] + pz * m[1][2] + pw * m[1][3];
          const float cw = px * m[3][0] + py * m[3][1] + pz * m[3][2] + pw * m[3][3];
          const float inv_w = (cw != 0.0f) ? (1.0f / cw) : 1.0f;
          quad[v].x = cx * inv_w;
          quad[v].y = cy * inv_w;
          quad[v].u = read_float(at + 20);
          quad[v].v = read_float(at + 24);
          quad[v].a = float((colour >> 24) & 0xFF) / 255.0f;
          quad[v].r = float((colour >> 16) & 0xFF) / 255.0f;
          quad[v].g = float((colour >> 8) & 0xFF) / 255.0f;
          quad[v].b = float(colour & 0xFF) / 255.0f;
        }

        // Text placement: is the hardcoded 640x480 avoidable?
        //
        // The title screen lays UI out in 640x480 but the menu reaches 1280x720,
        // so no single constant can be right for both. If these draws are
        // transformed by c0..c3 like the logo quad is, then applying that matrix
        // instead of dividing by a fixed resolution fixes BOTH automatically.
        // The logo's matrix scaled by 2/1280 and -2/720; if the title screen's
        // text matrix scales by 2/640 = 0.003125 and -2/480 = -0.004167, that
        // confirms it and the constant can go.
        {
          static bool logged_text_matrix = false;
          if (!logged_text_matrix) {
            logged_text_matrix = true;
            for (uint32_t reg = 0; reg < 4; ++reg) {
              const uint32_t at = device + 1920 + 16 * reg;
              REXLOG_INFO("[kameo-gfx] TEXT c{} = ({:.6f}, {:.6f}, {:.6f}, {:.6f})", reg,
                          read_float(at + 0), read_float(at + 4), read_float(at + 8),
                          read_float(at + 12));
            }
          }
        }

        // Wiring the shader cache needs a route from the bound shader object to
        // its ucode, so it can be hashed and looked up in g_shaderCacheEntries.
        // IDA types D3DVertexShader as a bare 24-byte D3DResource, so the ucode
        // lives elsewhere. For textures the data address turned out to be in
        // BaseFlush (+20); test whether shaders follow the same pattern by
        // dumping the object and chasing each plausible pointer for the shader
        // container magic (0x102A0E00 pixel / vertex variant).
        {
          static bool dumped_shader = false;
          if (!dumped_shader) {
            dumped_shader = true;
            const uint32_t vs = REX_LOAD_U32(device + 20440);
            REXLOG_INFO("[kameo-gfx] bound vertex shader object = {:08X}", vs);
            if (vs) {
              std::string raw;
              for (uint32_t i = 0; i < 16; ++i) {
                raw += fmt::format(" {:02d}:{:08X}", i * 4, REX_LOAD_U32(vs + 4 * i));
              }
              REXLOG_INFO("[kameo-gfx]   raw:{}", raw);
              // Chase every field that looks like a guest pointer and report
              // what the first dword there is -- the container magic identifies
              // the ucode immediately.
              for (uint32_t i = 0; i < 16; ++i) {
                const uint32_t cand = REX_LOAD_U32(vs + 4 * i);
                if (cand >= 0x82000000u && cand < 0xF0000000u) {
                  REXLOG_INFO("[kameo-gfx]   +{} -> {:08X} first dwords {:08X} {:08X} {:08X}",
                              i * 4, cand, REX_LOAD_U32(cand), REX_LOAD_U32(cand + 4),
                              REX_LOAD_U32(cand + 8));
                }
              }
            }
          }
        }

        // The rendered glyphs have a diagonal seam with the two halves
        // mismatched, which is what a wrong strip->list expansion looks like
        // (cull mode is NONE, so it is not a winding drop). That expansion
        // assumes the guest emits TL,TR,BL,BR like Draw_Bink_textures does --
        // log the real per-vertex order and UVs instead of assuming it carries
        // over to the UI.
        {
          static uint32_t order_logs = 0;
          if (order_logs < 4) {
            ++order_logs;
            std::string vs;
            for (uint32_t v = 0; v < 4; ++v) {
              const uint32_t at = data + v * stride;
              vs += fmt::format("  v{}=({:.1f},{:.1f}) uv=({:.4f},{:.4f})", v, read_float(at + 0),
                                read_float(at + 4), read_float(at + 20), read_float(at + 24));
            }
            REXLOG_INFO("[kameo-gfx] quad order:{}", vs);
          }
        }

        // Is the red "PRESS START" our D3DCOLOR decode or the colour the game
        // actually emits? The copyright line proves nothing -- white is
        // 0xFFFFFFFF and survives any channel swap -- so log the raw dwords
        // against the quad's y. PRESS START sits near y=287, the copyright near
        // y=390..418, so the two groups are separable.
        {
          static uint32_t colour_logs = 0;
          if (colour_logs < 20) {
            ++colour_logs;
            const float py0 = read_float(data + 4);
            std::string cols;
            for (uint32_t v = 0; v < 4; ++v) {
              cols += fmt::format(" {:08X}", REX_LOAD_U32(data + v * stride + 16));
            }
            REXLOG_INFO("[kameo-gfx] glyph y={:.1f} colours:{}", py0, cols);
          }
        }

        // What coordinate space is the UI actually laid out in? "PRESS START"
        // centres on x=321, which is 640/2, not 1280/2 -- so the pre-transformed
        // positions may not be in viewport space at all. Track the extent over a
        // whole frame: if it tops out near 640x480 the guest is using a virtual
        // UI resolution and something else applies the scale.
        {
          static float ext_minx = 1e9f, ext_miny = 1e9f, ext_maxx = -1e9f, ext_maxy = -1e9f;
          static uint32_t ext_frames = 0;
          for (uint32_t v = 0; v < 4; ++v) {
            const uint32_t at = data + v * stride;
            const float px = read_float(at + 0);
            const float py = read_float(at + 4);
            ext_minx = std::min(ext_minx, px);
            ext_maxx = std::max(ext_maxx, px);
            ext_miny = std::min(ext_miny, py);
            ext_maxy = std::max(ext_maxy, py);
          }
          if ((ext_frames++ % 2000) == 0) {
            REXLOG_INFO("[kameo-gfx] overlay coord extent: x {:.1f}..{:.1f}  y {:.1f}..{:.1f}",
                        ext_minx, ext_maxx, ext_miny, ext_maxy);
          }
        }

        std::lock_guard<std::mutex> lock(g_overlayMutex);
        // Start a new run when the atlas changes. Appending everything to one
        // batch made every glyph sample whichever atlas happened to be bound
        // last, which rendered the copyright line correctly and shredded
        // "PRESS START".
        if (g_overlayBatches.empty() || g_overlayBatches.back().atlas_address != addr) {
          kameo::gfx::OverlayBatch fresh;
          fresh.atlas = base + addr;
          fresh.atlas_address = addr;
          fresh.atlas_tiled = ((w0 >> 31) & 1) != 0;
          fresh.atlas_pitch = ((w0 >> 22) & 0x1FF) * 32;
          fresh.atlas_width = (w2 & 0x1FFF) + 1;
          fresh.atlas_height = ((w2 >> 13) & 0x1FFF) + 1;
          fresh.valid = true;
          g_overlayBatches.push_back(std::move(fresh));
        }
        // The UI emits its 4 vertices in PERIMETER order -- measured:
        //   v0=(237.7,288.1) TL  v1=(251.4,288.1) TR
        //   v2=(251.4,307.2) BR  v3=(237.7,307.2) BL
        // which is a fan, NOT the strip order Draw_Bink_textures uses
        // (TL,TR,BL,BR). Expanding it as a strip ({0,1,2, 2,1,3}) yields a
        // correct first triangle and a bogus second one (BR,TR,BL) -- that was
        // the diagonal seam through every glyph, with half of each letter
        // sampling the wrong texels.
        const int fan_to_list[6] = {0, 1, 2, 0, 2, 3};
        for (int i = 0; i < 6; ++i) {
          g_overlayBatches.back().vertices.push_back(quad[fan_to_list[i]]);
        }
      }
    }

    // The fading logo is not a stride-60 draw. Identify which shape carries it
    // and what texture it binds, before assuming it needs the full shader path:
    // if its texture is logo-sized rather than the font atlas, that pins it.
    if ((stride == 24 || stride == 16) && data && count == 4) {
      static uint32_t other_logs = 0;
      if (other_logs < 12) {
        ++other_logs;
        const uint32_t device = ctx.r3.u32;
        const uint32_t tex = REX_LOAD_U32(device + 12704);
        auto rf = [&](uint32_t at) {
          const uint32_t bits = REX_LOAD_U32(at);
          float f;
          std::memcpy(&f, &bits, sizeof(f));
          return f;
        };
        std::string vs;
        for (uint32_t v = 0; v < 4; ++v) {
          const uint32_t at = data + v * stride;
          vs += fmt::format("  ({:.1f},{:.1f},{:.1f})c={:08X}", rf(at + 0), rf(at + 4), rf(at + 8),
                            REX_LOAD_U32(at + 12));
          if (stride == 24) {
            vs += fmt::format("uv=({:.3f},{:.3f})", rf(at + 16), rf(at + 20));
          }
        }
        std::string texinfo = "none";
        if (tex) {
          const uint32_t fc = device + 1152;
          const uint32_t w0 = REX_LOAD_U32(fc);
          const uint32_t w1 = REX_LOAD_U32(fc + 4);
          const uint32_t w2 = REX_LOAD_U32(fc + 8);
          texinfo = fmt::format("{:08X} {}x{} fmt={} tiled={}", tex, (w2 & 0x1FFF) + 1,
                                ((w2 >> 13) & 0x1FFF) + 1, w1 & 0x3F, (w0 >> 31) & 1);
        }
        REXLOG_INFO("[kameo-gfx] stride{} prim={} tex[{}]{}", stride, prim, texinfo, vs);
      }
    }

    // Confirm WHICH shape is the overlay before building anything for it.
    // Knowing that stride-60 is cheap to draw (rhw=1.0, pixel coords) is not the
    // same as knowing it is the text -- building the untiler and the 2D pipeline
    // only to find they render the wrong geometry is the expensive mistake here.
    //
    // Log the screen-space bounding box and bound texture for the first handful
    // of draws of each shape. Many small scattered boxes on one texture is text;
    // a few large ones is background or a full-screen effect.
    static std::atomic<uint32_t> logged_quads{0};
    if (stride == 60 && data && count == 4 && logged_quads.fetch_add(1) < 14) {
      float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
      for (uint32_t v = 0; v < count; ++v) {
        const uint32_t xb = REX_LOAD_U32(data + v * stride + 0);
        const uint32_t yb = REX_LOAD_U32(data + v * stride + 4);
        float x, y;
        std::memcpy(&x, &xb, sizeof(x));
        std::memcpy(&y, &yb, sizeof(y));
        minx = std::min(minx, x);
        maxx = std::max(maxx, x);
        miny = std::min(miny, y);
        maxy = std::max(maxy, y);
      }
      const uint32_t tex0 = REX_LOAD_U32(ctx.r3.u32 + 12704);
      REXLOG_INFO("[kameo-gfx] quad60: x={:.1f}..{:.1f} y={:.1f}..{:.1f} ({:.0f}x{:.0f}) tex={:08X}",
                  minx, maxx, miny, maxy, maxx - minx, maxy - miny, tex0);
    }
  }
  // Marks the nested BeginVertices so it can tell internal calls from standalone
  // ones (the Bink blit and, we think, the logo).
  g_inDrawVerticesUP = true;
  __imp__sub_820CE9D8(ctx, base);
  g_inDrawVerticesUP = false;
}
// D3DDevice_BeginVertices(pDevice, PrimitiveType, VertexCount, Stride)
//
// DrawVerticesUP calls this internally, so the interesting calls are the
// STANDALONE ones -- census says ~2 per frame against 148 UP draws, and the
// Bink blit is one of them. The fading logo is not a stride-60 UP draw and not
// one of the untextured stride-16 fade quads, so it is very likely the other.
//
// Log only standalone calls, using a flag set around the UP hook's delegation.
extern "C" REX_FUNC(sub_820CE738) {
  const bool standalone = !g_inDrawVerticesUP;
  // Capture the arguments BEFORE delegating: r3 comes back holding the ring
  // pointer, so reading the device out of it afterwards would be garbage.
  const uint32_t device = ctx.r3.u32;
  const uint32_t prim = ctx.r4.u32;
  const uint32_t count = ctx.r5.u32;
  const uint32_t stride = ctx.r6.u32;

  if (kameo::gfx::KameoGraphicsSystem::instance() != nullptr) {
    CountDraw(kBeginVertices);
  }
  __imp__sub_820CE738(ctx, base);

  // Capture the STANDALONE ones. The nested calls belong to DrawVerticesUP,
  // which captures itself once the data is already in hand. Standalone
  // BeginVertices is the 2D icon system -- health and energy bars, the warrior
  // button prompts -- and ~250 of them a frame were being counted here and then
  // thrown away, which is why the HUD had text but no bars and no button
  // glyphs. r3 now holds the ring the guest is about to write through.
  if (standalone && kameo::gfx::KameoGraphicsSystem::instance() != nullptr) {
    if (REXCVAR_GET(kameo_gfx_capture_begin_vertices)) {
      kameo::gfx::CaptureDrawBeginVertices(base, device, prim, count, ctx.r3.u32, stride);
    }
  }

  if (standalone && kameo::gfx::KameoGraphicsSystem::instance() != nullptr) {
    // Only TEXTURED standalone draws: the untextured stride-16 quads are the
    // black fade overlays and there are enough of them during boot to bury the
    // interesting ones.
    const uint32_t tex = REX_LOAD_U32(device + 12704);
    static uint32_t standalone_logs = 0;
    if (tex && standalone_logs < 16) {
      ++standalone_logs;
      std::string texinfo = "none";
      if (tex) {
        const uint32_t fc = device + 1152;
        const uint32_t w0 = REX_LOAD_U32(fc);
        const uint32_t w1 = REX_LOAD_U32(fc + 4);
        const uint32_t w2 = REX_LOAD_U32(fc + 8);
        texinfo = fmt::format("{:08X} {}x{} fmt={} tiled={}", tex, (w2 & 0x1FFF) + 1,
                              ((w2 >> 13) & 0x1FFF) + 1, w1 & 0x3F, (w0 >> 31) & 1);
      }
      // r3 now holds the ring pointer the guest will write its vertices through.
      REXLOG_INFO("[kameo-gfx] standalone BeginVertices prim={} count={} stride={} ring={:08X} tex[{}]",
                  prim, count, stride, ctx.r3.u32, texinfo);

      // The guest has not written the vertices yet, so dump the PREVIOUS call's
      // ring contents now that they are populated. That is what says whether the
      // logo quad is in the same 640x480 UI space as the text or in model space
      // needing the shader transform.
      static uint32_t prev_ring = 0, prev_stride = 0, prev_count = 0;
      if (prev_ring && prev_stride == 24 && prev_count == 4) {
        auto rf = [&](uint32_t at) {
          const uint32_t bits = REX_LOAD_U32(at);
          float f;
          std::memcpy(&f, &bits, sizeof(f));
          return f;
        };
        std::string vs;
        for (uint32_t v = 0; v < 4; ++v) {
          const uint32_t at = prev_ring + v * prev_stride;
          vs += fmt::format("  ({:.1f},{:.1f},{:.1f})c={:08X}uv=({:.3f},{:.3f})", rf(at + 0),
                            rf(at + 4), rf(at + 8), REX_LOAD_U32(at + 12), rf(at + 16),
                            rf(at + 20));
        }
        REXLOG_INFO("[kameo-gfx]   previous quad verts:{}", vs);

        // The logo quad is model space (-256..256 centred on origin), so the
        // placement lives in the vertex shader constants.
        // D3DDevice_SetVertexShaderConstantFN stores them at
        // pDevice->raw[16*StartRegister + 1920], i.e. device + 1920 + 16*reg.
        std::string ranges;
        {
          std::lock_guard<std::mutex> lock(g_constMutex);
          const uint32_t n = std::min<uint32_t>(g_recentConstantCount, 12);
          for (uint32_t i = 0; i < n; ++i) {
            ranges += fmt::format(" c{}+{}", g_recentConstants[i].start,
                                  g_recentConstants[i].count);
          }
        }
        REXLOG_INFO("[kameo-gfx]   recent VS constant writes:{}", ranges);

        // Dump every register the game recently wrote, not just c0..c7, and look
        // for the one whose translate is -1.0 (a real pixel-to-clip matrix).
        for (uint32_t reg = 0; reg < 8; ++reg) {
          const uint32_t at = device + 1920 + 16 * reg;
          REXLOG_INFO("[kameo-gfx]     c{} = ({:.4f}, {:.4f}, {:.4f}, {:.4f})", reg, rf(at + 0),
                      rf(at + 4), rf(at + 8), rf(at + 12));
        }
        for (uint32_t reg = 96; reg < 104; ++reg) {
          const uint32_t at = device + 1920 + 16 * reg;
          REXLOG_INFO("[kameo-gfx]     c{} = ({:.4f}, {:.4f}, {:.4f}, {:.4f})", reg, rf(at + 0),
                      rf(at + 4), rf(at + 8), rf(at + 12));
        }
      }
      prev_ring = ctx.r3.u32;
      prev_stride = stride;
      prev_count = count;
    }
  }
}
KAMEO_CENSUS_HOOK(820C76F0, kSetTexture)              // D3DDevice_SetTexture

#undef KAMEO_CENSUS_HOOK

// Draw_Bink_textures(0x82265558) -- the Bink video blit.
//
// Everything is derived from the guest struct rather than from the decompiled
// argument list: Hex-Rays mis-numbers this function's arguments across PPC's
// float/int register split (it invents an `a5` the caller never passes), so the
// struct pointer in r4 plus the offsets below are the trustworthy source.
//
// Layout, with idx = struct[84] (see src/gfx/README.md):
//   textures   struct + 32*idx + {0,4,8,12}     (unused here -- we read planes)
//   data ptrs  struct + {92,104,116,128} + 48*idx
//   pitches    struct + {96,108,120,132} + 48*idx
//   luma  dims struct+68 x struct+72
//   chroma dims struct+76 x struct+80
extern "C" REX_FUNC(sub_82265558) {
  auto* gfx = kameo::gfx::KameoGraphicsSystem::instance();
  if (gfx == nullptr) {
    __imp__sub_82265558(ctx, base);
    return;
  }
  Enter("Draw_Bink_textures");
  g_binkCallsThisFrame.fetch_add(1, std::memory_order_relaxed);

  const uint32_t st = ctx.r4.u32;
  if (!st) {
    return;
  }

  kameo::gfx::BinkFrame frame;
  frame.luma_width = REX_LOAD_U32(st + 68);
  frame.luma_height = REX_LOAD_U32(st + 72);
  frame.chroma_width = REX_LOAD_U32(st + 76);
  frame.chroma_height = REX_LOAD_U32(st + 80);
  const uint32_t idx = REX_LOAD_U32(st + 84);

  // Reject implausible geometry rather than trusting the register: a wrong r4
  // would otherwise be read as a huge texture and hang the upload.
  if (frame.luma_width == 0 || frame.luma_height == 0 || frame.luma_width > 4096 ||
      frame.luma_height > 4096 || idx > 8) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXLOG_WARN("[kameo-gfx] bink struct looks wrong (r4={:08X} {}x{} idx={}); skipping", st,
                  frame.luma_width, frame.luma_height, idx);
    }
    return;
  }

  // Gate each plane on the guest's OWN existence test: the D3DTexture pointer at
  // st + 32*idx + 4*i. Create_Bink_textures (0x822653F0) zeroes those four
  // pointers every iteration and only fills them in when make_texture succeeds,
  // so they are the authoritative "does this plane exist" flag.
  //
  // The data pointers at +92/104/116/128 are NOT zeroed -- make_texture only
  // writes them on success -- so an absent plane leaves stale garbage there.
  // Testing those instead (as this did) made every video look like it had an
  // alpha plane and sent UploadBinkPlanes reading 1280x720 from a junk pointer
  // with a junk pitch.
  // Bink's pixel data starts ONE 4KB PAGE after the pointer the game stores at
  // +92/104/116/128. That pointer is make_texture's `v16` -- the raw
  // memXMemAlloc base -- and make_texture takes its *pitch* from
  // D3D::LockSurface while discarding the surface pointer LockSurface hands
  // back, so the stored base and the stored pitch do not describe the same
  // origin.
  //
  // Measured, not guessed, and it accounts for every artifact that was open:
  //
  //   * luma read at the stored pointer has a hard vertical seam at exactly
  //     x=256 -- mean column-to-column delta 39.7 against a 3.87 background,
  //     the only outlier in 1280 columns
  //   * chroma (pitch 768, width 640) puts its 128 padding bytes per row at
  //     columns 128..255 instead of 640..767, which is exactly the 256px green
  //     band at screen x=256..511. Through the guest's own matrix cr=cb=0 is
  //     precisely pure green
  //   * the image appears doubled 256px right, because chroma skews by 256
  //     chroma texels (=512 screen px) while luma skews 256 screen px, so the
  //     colour ghost trails the luma by 256px
  //   * the leading rows are garbage
  //
  // Beware the trap here: 4096 mod 1280 == 4096 mod 768 == 256, so ALL of the
  // horizontal evidence points at 256 and a 256-byte correction genuinely
  // removes the seam, the green band and the doubling. What it leaves behind is
  // a whole number of junk rows -- 3 for luma (3*1280) and 5 for chroma
  // (5*768), i.e. 3840 bytes in every plane. 3840 + 256 = 4096. Correcting by
  // the page is what removes the top strip as well.
  // WHY one page: this is D3D's own address adjustment, not a fudge factor.
  // D3DDevice_SetTexture builds the GPU fetch address as
  //
  //     ((((BaseFlush >> 20) + 512) & 0x1000) + BaseFlush) & 0x1FFFF7FF
  //
  // and that first term adds exactly 0x1000 whenever the address is >= 0xE0000000
  // (below that, (addr>>20)+512 never reaches bit 12). Verified live: a texture
  // at BaseFlush=F3D13002 comes out as fetch address 13D14002 -- the top nibble
  // masked away and one page added. The Bink planes sit at 0xED70F000, in that
  // same range, so they take the same page.
  //
  // Applying the guest's own formula rather than a constant means this stays
  // correct for planes allocated outside that range, and it is the same
  // adjustment the general texture path will need.
  const auto guest_page_bias = [](uint32_t addr) -> uint32_t {
    return (((addr >> 20) + 512) & 0x1000);
  };

  static bool logged_base = false;
  const uint32_t data_off[4] = {92, 104, 116, 128};
  const uint32_t pitch_off[4] = {96, 108, 120, 132};
  for (int i = 0; i < 4; ++i) {
    const uint32_t texture = REX_LOAD_U32(st + 32 * idx + 4 * i);
    const uint32_t ptr = texture ? REX_LOAD_U32(st + data_off[i] + 48 * idx) : 0;
    frame.pitches[i] = texture ? REX_LOAD_U32(st + pitch_off[i] + 48 * idx) : 0;
    frame.planes[i] = ptr ? (base + ptr + guest_page_bias(ptr)) : nullptr;

    // The texture header carries the address the console's GPU actually fetched
    // from (D3DResource::BaseFlush, address in the top 20 bits). Logging it
    // against the stored pointer is what will finally explain the 256 below --
    // replace the constant with (BaseFlush & ~0xFFF) - ptr once a run confirms
    // the two differ by exactly that.
    if (texture && !logged_base) {
      REXLOG_INFO("[kameo-gfx] bink plane {}: stored ptr {:08X}, texture {:08X} BaseFlush {:08X}",
                  i, ptr, texture, REX_LOAD_U32(texture + 20));
    }
  }
  logged_base = true;
  frame.has_alpha = frame.planes[3] != nullptr;

  // yuvtorgb: four float4 at 0x8273A5B0, big-endian in guest memory.
  for (int i = 0; i < 16; ++i) {
    const uint32_t bits = REX_LOAD_U32(0x8273A5B0 + 4 * i);
    std::memcpy(&frame.yuv_to_rgb[i], &bits, sizeof(float));
  }

  // Full-screen quad for now. The game computes letterbox offsets in
  // x_g_kamVideoParams_1 and passes them as floats; wiring those up is a
  // refinement once pixels are confirmed.
  const float quad[4][5] = {
      {-1.0f, 1.0f, 0.0f, 0.0f, 0.0f},
      {1.0f, 1.0f, 0.0f, 1.0f, 0.0f},
      {-1.0f, -1.0f, 0.0f, 0.0f, 1.0f},
      {1.0f, -1.0f, 0.0f, 1.0f, 1.0f},
  };
  std::memcpy(frame.vertices, quad, sizeof(quad));
  frame.valid = true;

  // Log once per distinct frame-buffer index, not once overall. Kameo cycles
  // through several buffers and each has its OWN pointer/pitch record at
  // +48*idx, so a single log only ever described whichever index happened to
  // arrive first -- which would hide a per-buffer layout difference completely.
  static uint32_t logged_mask = 0;
  if (!(logged_mask & (1u << idx))) {
    logged_mask |= 1u << idx;
    REXLOG_INFO("[kameo-gfx] bink idx={} {}x{} chroma {}x{} alpha={} pitches={}/{}/{}/{} "
                "ptrs={:08X}/{:08X}/{:08X}/{:08X}",
                idx, frame.luma_width, frame.luma_height, frame.chroma_width, frame.chroma_height,
                frame.has_alpha, frame.pitches[0], frame.pitches[1], frame.pitches[2],
                frame.pitches[3], REX_LOAD_U32(st + data_off[0] + 48 * idx),
                REX_LOAD_U32(st + data_off[1] + 48 * idx),
                REX_LOAD_U32(st + data_off[2] + 48 * idx),
                REX_LOAD_U32(st + data_off[3] + 48 * idx));
  }

  gfx->SubmitBinkFrame(frame);
    kameo::gfx::CaptureBinkMarker();
}
