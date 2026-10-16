#include <algorithm>
#include <mutex>

#include "gpu/draw/capture.h"
#include "gpu/draw_path.h"
#include "gpu/graphics_system.h"
#include "gpu/guest_device.h"
#include "gpu/guest_memory.h"

namespace kameo::gpu {
using namespace detail;

thread_local uint32_t g_guestPass = 0;
thread_local uint32_t g_hudGroup = 0;

void CaptureResolve(uint8_t* base, uint32_t device, uint32_t flags, uint32_t source_rect, uint32_t dest_texture, uint32_t dest_point, uint32_t clear_color, float clear_depth, uint32_t caller) {
  if (!dest_texture) {
    return;
  }
  // Flags & 7 selects the source: 0..3 are colour targets, 4 is depth.
  const uint32_t source = flags & 7;


  DrawCall call;
  call.kind = CommandKind::Resolve;
  call.resolve_flags = flags;
  call.resolve_clear_depth = (flags & 0x200u) != 0;
  call.clear_depth = clear_depth;
  call.guest_caller = caller;
  if (clear_color && (flags & 0x100u)) {
    for (uint32_t i = 0; i < 4; ++i) call.resolve_clear_color[i] = LoadFloat(base, clear_color + 4 * i);
  }
  // The surface this resolve reads from is the device's render target at the call; inferring it from the previous draw copied the swap chain into the storybook pages (run 099).
  call.rt_surface = Load32(base, device + dev::kRenderTarget0);
  // drawOrthoObj (0x823D0460) and imposterModelRender (0x82100270) render several passes into one target and move the viewport between them, so the viewport names the resolved sub-rectangle.
  // _D3DVIEWPORT9 is four DWORDs then two floats; reading the first four as floats made every resolve viewport 0x0.
  for (uint32_t i = 0; i < 4; ++i) {
    call.viewport[i] = float(Load32(base, device + dev::kViewport + i * 4));
  }
  call.viewport[4] = LoadFloat(base, device + dev::kViewport + 16);
  call.viewport[5] = LoadFloat(base, device + dev::kViewport + 20);
  // D3DRESOLVE_CLEARRENDERTARGET (0x100) / _CLEARDEPTHSTENCIL (0x200): imposterModelRender passes 0x300 after each imposter so the next gets a clean scratch pad.
  call.resolve_clear_rt = (flags & 0x100) != 0;
  call.resolve_depth = (source == 4);
  call.resolve_address = Load32(base, dest_texture + 20) & 0xFFFFF000u;
  if (!call.resolve_address) {
    return;
  }
  call.resolve_format = Load32(base, dest_texture + 20) & 0x3F;

  // MipFlush carries the destination's dimensions, in the two layouts D3DDevice_Resolve picks between on Format.dword[2] bit 10.
  const uint32_t mip_flush = Load32(base, dest_texture + 24);
  const uint32_t format2 = Load32(base, dest_texture + 36);
  if ((format2 & 0x600) == 0x400) {
    call.resolve_width = (mip_flush & 0x7FF) + 1;
    call.resolve_height = ((mip_flush >> 11) & 0x7FF) + 1;
  } else {
    call.resolve_width = (mip_flush & 0x1FFF) + 1;
    call.resolve_height = ((mip_flush >> 13) & 0x1FFF) + 1;
  }
  call.resolve_texture_width = call.resolve_width;
  call.resolve_texture_height = call.resolve_height;

  if (dest_point) {
    call.resolve_dest_x = int32_t(Load32(base, dest_point + 0));
    call.resolve_dest_y = int32_t(Load32(base, dest_point + 4));
  }

  if (source_rect) {
    call.resolve_has_rect = true;
    call.resolve_x = int32_t(Load32(base, source_rect + 0));
    call.resolve_y = int32_t(Load32(base, source_rect + 4));
    const int32_t x2 = int32_t(Load32(base, source_rect + 8));
    const int32_t y2 = int32_t(Load32(base, source_rect + 12));
    if (x2 > call.resolve_x && y2 > call.resolve_y) {
      call.resolve_width = uint32_t(x2 - call.resolve_x);
      call.resolve_height = uint32_t(y2 - call.resolve_y);
    }
  }
  if (call.resolve_width == 0 || call.resolve_height == 0 || call.resolve_texture_width == 0 || call.resolve_texture_height == 0) {
    return;
  }

  std::lock_guard<std::mutex> lock(g_captureMutex);
  g_capturing.guest_base = base;
  g_capturing.draws.push_back(call);
}

void CaptureDepthClear(uint8_t* base, uint32_t device, uint32_t flags, uint32_t rect, uint32_t colour, float depth, uint32_t caller) {
  const uint32_t depth_surface = Load32(base, device + dev::kDepthStencil);
  DrawCall call;
  call.kind = CommandKind::DepthClear;
  call.clear_depth = depth;
  call.guest_caller = caller;
  call.rt_surface = Load32(base, device + dev::kRenderTarget0);
  call.depth_only = call.rt_surface == 0;
  call.clear_depth_stencil = (flags & 0x10u) && depth_surface;
  call.clear_colour = (flags & 0xFu) && call.rt_surface;
  if (!call.clear_depth_stencil && !call.clear_colour) return;
  for (uint32_t i = 0; i < 4 && colour; ++i) call.clear_colour_value[i] = LoadFloat(base, colour + 4 * i);
  for (uint32_t i = 0; i < 4; ++i) {
    call.viewport[i] = float(Load32(base, device + dev::kViewport + 4 * i));
  }
  const uint32_t surface = call.rt_surface ? call.rt_surface : depth_surface;
  const uint32_t size = Load32(base, surface + 24);
  int32_t right = int32_t((size & 0x1FFFu) + 1);
  int32_t bottom = int32_t(((size >> 13) & 0x1FFFu) + 1);
  if ((base[device + 0x28C0] & 0x10u) != 0) {
    const uint32_t tiled_surface = Load32(base, device + 0x32A8);
    if (tiled_surface == call.rt_surface || tiled_surface == depth_surface) {
      right = int32_t(Load32(base, device + 0x3490));
      bottom = int32_t(Load32(base, device + 0x3494));
    }
  }
  call.clear_rect[0] = rect ? int32_t(Load32(base, rect)) : 0;
  call.clear_rect[1] = rect ? int32_t(Load32(base, rect + 4)) : 0;
  call.clear_rect[2] = rect ? int32_t(Load32(base, rect + 8)) : right;
  call.clear_rect[3] = rect ? int32_t(Load32(base, rect + 12)) : bottom;
  call.clear_rect[0] = std::max(call.clear_rect[0], int32_t(call.viewport[0]));
  call.clear_rect[1] = std::max(call.clear_rect[1], int32_t(call.viewport[1]));
  call.clear_rect[2] = std::min(call.clear_rect[2], int32_t(call.viewport[0] + call.viewport[2]));
  call.clear_rect[3] = std::min(call.clear_rect[3], int32_t(call.viewport[1] + call.viewport[3]));
  if (Load32(base, device + 0x2EB0)) {
    call.clear_rect[0] = std::max(call.clear_rect[0], int32_t(Load32(base, device + 0x3220)));
    call.clear_rect[1] = std::max(call.clear_rect[1], int32_t(Load32(base, device + 0x3224)));
    call.clear_rect[2] = std::min(call.clear_rect[2], int32_t(Load32(base, device + 0x3228)));
    call.clear_rect[3] = std::min(call.clear_rect[3], int32_t(Load32(base, device + 0x322C)));
  }
  if (call.clear_rect[2] <= call.clear_rect[0] || call.clear_rect[3] <= call.clear_rect[1]) return;
  std::lock_guard<std::mutex> lock(g_captureMutex);
  g_capturing.guest_base = base;
  g_capturing.draws.push_back(call);
}

void CaptureBinkMarker(uint8_t* base) {
  // The video composites in guest order: drawn first it sat under the opaque full-screen fade quad meant to cover it.
  DrawCall call;
  call.kind = CommandKind::Bink;
  std::lock_guard<std::mutex> lock(g_captureMutex);
  g_capturing.guest_base = base;
  g_capturing.draws.push_back(call);
}

}  // namespace kameo::gpu
