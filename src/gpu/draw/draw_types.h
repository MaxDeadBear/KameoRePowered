#pragma once

// The capture/replay contract: the guest thread records draws into a DrawFrame and the render thread replays it; shared counters live in debug/census.h.

#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <plume_render_interface_types.h>

#include "gpu/guest_state.h"
#include "gpu/shaders/shader_cache.h"

namespace kameo::gpu {

// Arena offset meaning "this draw carries none of that data".
constexpr uint32_t kNoData = 0xFFFFFFFFu;
constexpr uint32_t kSamplerCount = 16;  // XenosRecomp's kSamplerRegisterCount

enum TranscodeOp : uint32_t {
  kSwap16,        // n 16-bit components, byte-swapped in place
  kSwap32,        // n 32-bit units (also packed dwords: DEC3N, D3DCOLOR, UBYTE4N)
  kShortToFloat,  // n int16 -> n float32
  kUShortToFloat,
  kByteToFloat,   // 4 uint8 (in one big-endian dword) -> 4 float32
  kDec3ToFloat,   // packed 10:10:10 -> 3 float32 (plume has no R10G10B10A2)
  kDec3ToR11G11B10,  // guest DEC3N repacked into the layout the shader decodes
  kFloat3ToDec3,  // float3 normal frame packed for the shader's DEC3N decoder
};

struct HostElement {
  uint32_t usage = 0;
  uint32_t usage_index = 0;
  uint32_t stream = 0;
  uint32_t src_offset = 0;
  uint32_t dst_offset = 0;
  uint32_t components = 0;
  uint32_t src_size = 0;
  uint32_t dst_size = 0;
  TranscodeOp op = kSwap32;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
};

struct HostLayout {
  std::vector<HostElement> elements;
  uint32_t dst_stride = 0;
  uint64_t key = 0;
  // Hash of the guest declaration's elements. A declaration is immutable, but its address is not: the guest recycles one for the next mesh, so a cached layout is only this declaration's while its elements still match.
  uint64_t decl_signature = 0;
  bool valid = false;
};

enum class CommandKind : uint32_t {
  Draw,
  Resolve,
  Bink,  // the decoded video frame, drawn at the point the guest blitted it
  DepthClear,
  QueryBegin,
  QueryEnd,
};

// A stage's constant block as the capture records it, followed by one 64-byte chunk per set mask bit in chunk order; the render thread builds the block from it.
struct ConstantRecord {
  uint64_t mask = 0;       // chunks carried: bit 63-k for chunk k
  uint32_t needed = 0;     // bytes of the block from register zero
  uint32_t arena = kNoData;  // the arena copy the render thread built, for the few readers on the CPU
  uint32_t ring = kNoData;   // the upload ring copy it built
  uint32_t applied = 0;      // its chunks are in the render thread's register file
};

// A bound vertex window the render thread reads when it records the frame; trivially copyable, since it lives in the frame arena.
constexpr uint32_t kDeferredStreams = 4;
struct DeferredVertexSource {
  const HostLayout* layout = nullptr;
  uint64_t reuse_key = 0;  // before the streams are folded in
  float viewport[4] = {};
  uint32_t first = 0;
  uint32_t count = 0;
  uint32_t stream_count = 0;
  struct Stream {
    uint32_t index = 0;
    uint32_t address = 0;  // guest address of the window's stream data, offset applied
    uint32_t stride = 0;
    uint32_t size = 0;
    uint32_t element_end = 0;
  } streams[kDeferredStreams];
};

struct DrawCall {
  CommandKind kind = CommandKind::Draw;
  // QueryBegin/QueryEnd: the guest D3DQuery, and for an END the sequence number GetData waits for.
  uint32_t query_object = 0;
  uint64_t query_seq = 0;
  float clear_depth = 1.0f;
  int32_t clear_rect[4] = {};
  // DepthClear records the whole D3DDevice_ClearF: which of its colour and depth halves were asked for.
  bool clear_colour = false;
  bool clear_depth_stencil = false;
  float clear_colour_value[4] = {};

  // Resolve only: where the result goes, and what part of the target to take.
  uint32_t resolve_address = 0;   // destination texture's guest address
  uint32_t resolve_texture_width = 0;
  uint32_t resolve_texture_height = 0;
  uint32_t resolve_width = 0;
  uint32_t resolve_height = 0;
  bool resolve_has_rect = false;  // the guest passed an explicit source rect
  bool resolve_clear_rt = false;  // D3DRESOLVE_CLEARRENDERTARGET
  bool resolve_clear_depth = false;
  uint32_t resolve_flags = 0;
  uint32_t resolve_format = 0;  // the destination texture's Xenos format

  float resolve_clear_color[4] = {};
  int32_t resolve_x = 0;
  int32_t resolve_y = 0;
  int32_t resolve_dest_x = 0;
  int32_t resolve_dest_y = 0;
  bool resolve_depth = false;
  // No pixel shader bound: a depth-only pass, as the shadow maps that resolve 768x768 / 1024x1024 out of surface 0.
  bool depth_only = false;
  // No pixel shader but a colour target bound and stencil on or an occlusion query open: it must land in that target's depth-stencil.
  bool stencil_only = false;

  uint32_t primitive = 0;
  uint32_t guest_caller = 0;
  uint32_t guest_command = 0;
  uint32_t guest_command_stream = 0;
  uint32_t guest_decl = 0;
  uint32_t vs_object = 0;
  uint32_t ps_object = 0;
  uint32_t diagnostic_data = kNoData;
  uint32_t diagnostic_bytes = 0;
  uint32_t command_data_offset = 0;
  uint32_t command_data_bytes = 0;
  uint32_t command_stream_data_offset = 0;
  uint32_t command_stream_data_bytes = 0;
  uint32_t command_stream_data_address = 0;
  uint32_t vs_object_data_offset = 0;
  uint32_t vs_object_data_bytes = 0;
  uint32_t vs_ucode_address = 0;
  uint32_t vs_ucode_data_offset = 0;
  uint32_t vs_ucode_data_bytes = 0;
  const ShaderCacheEntry* vs = nullptr;
  const ShaderCacheEntry* ps = nullptr;

  uint32_t vertex_data = kNoData;  // arena offset, host layout
  // Set instead of vertex_data when the vertices are shared with a capture cache, which a restream replaces rather than rewrites.
  std::shared_ptr<const std::vector<uint8_t>> vertex_blob;
  // Arena offset of a DeferredVertexSource: the render thread reads and transcodes the guest buffers into vertex_blob before recording.
  uint32_t vertex_source = kNoData;
  uint32_t vertex_bytes = 0;
  uint32_t vertex_stride = 0;
  uint32_t vertex_count = 0;
  uint32_t source_vertex_data = kNoData;
  uint32_t source_vertex_bytes = 0;
  uint32_t stream0_object = 0;
  uint32_t stream0_address = 0;
  uint32_t stream0_fetch1 = 0;
  uint32_t stream0_offset = 0;
  uint32_t stream0_stride = 0;
  // Every stream the layout reads, hashed: two draws can share stream 0 and still fetch different buffers on another.
  uint64_t stream_key = 0;

  // Set for a draw captured at D3DDevice_BeginVertices, before the guest wrote its vertices into the ring; zero for every ordinary draw.
  uint32_t deferred_vertex_addr = 0;
  uint32_t deferred_vertex_stride = 0;
  uint32_t deferred_vertex_count = 0;
  // Carried so streams other than 0 resolve against the right device rather than guest address ~0x1170.
  uint32_t deferred_device = 0;

  uint32_t index_data = kNoData;  // arena offset, always 32-bit host indices
  uint32_t index_count = 0;
  // Set instead of index_data when the expanded indices came from the cross-frame index cache; index_rebased says they were shifted to the draw's own vertex window rather than addressing the whole buffer.
  std::shared_ptr<const std::vector<uint32_t>> index_blob;
  bool index_rebased = false;

  const HostLayout* layout = nullptr;

  // A ConstantRecord's arena offset until the render thread builds the block; then the block's arena copy, or kNoData when only the upload ring holds it.
  uint32_t vs_constants = kNoData;
  uint32_t ps_constants = kNoData;
  uint32_t vs_ring = kNoData;  // the block's upload ring offset
  uint32_t ps_ring = kNoData;
  // How much of each block was copied: only up to the highest register the bound shader can read.
  uint32_t vs_constant_bytes = 0;
  uint32_t ps_constant_bytes = 0;
  uint32_t booleans = 0;

  GuestTextureFetch textures[kSamplerCount];
  uint32_t texture_objects[kSamplerCount] = {};  // guest D3DBaseTexture*, for BaseFlush
  // Each object's BaseFlush address, read at capture: the render thread runs a frame late, after the guest may have freed the object.
  uint32_t texture_addresses[kSamplerCount] = {};

  uint32_t depth_control = 0;
  uint32_t blend_control = 0;
  uint32_t color_control = 0;
  uint32_t mode_control = 0;
  uint32_t color_mask = 0xF;
  uint32_t guest_pass = 0;  // entry address of the effect pass that issued it
  // Drawn with one of the game's 2D vertex shaders (the fixed-function set or the font's), which transform by the WVP matrix UpdateMatrices or fontResetGraphics writes to c0-c3.
  bool hud_2d = false;
  // Nonzero for draws made while one text window rendered (g_hudGroup), which are placed together.
  uint32_t hud_group = 0;
  uint32_t half_pixel = 0;
  uint32_t stencil_enable = 0;
  uint32_t stencil_ref_mask = 0;  // ref | read mask << 8 | write mask << 16
  uint32_t alpha_to_mask = 0;
  uint32_t point_sprite = 0;
  uint32_t point_minmax = 0;
  uint32_t srgb_write = 0;
  uint32_t rt_surface = 0;   // D3DSurface* the draw renders into
  float alpha_ref = 0.0f;
  float poly_offset = 0.0f;        // guest depth bias, absolute normalised depth
  float poly_offset_scale = 0.0f;  // guest slope scale, already divided back by 16
  float viewport[6] = {0, 0, 0, 0, 0, 1};
  // PA_SC_WINDOW_SCISSOR in guest target pixels (left, top, right, bottom), only meaningful while scissor_enable is set.
  bool scissor_enable = false;
  int32_t scissor[4] = {0, 0, 0, 0};
};

struct DrawFrame {
  uint8_t* guest_base = nullptr;
  std::vector<DrawCall> draws;
  // The buffer's size is its high-water mark for the session and `used` how far this frame filled it, so the zero-fill of resize is paid only when it grows.
  std::vector<uint8_t> arena;
  size_t used = 0;

  void reset() {
    draws.clear();
    used = 0;
    guest_base = nullptr;
  }

  void swap(DrawFrame& other) noexcept {
    draws.swap(other.draws);
    arena.swap(other.arena);
    std::swap(used, other.used);
    std::swap(guest_base, other.guest_base);
  }

  uint32_t alloc(uint32_t bytes, uint32_t align = 4) {
    const size_t start = (used + align - 1) & ~size_t(align - 1);
    const size_t end = start + bytes;
    if (end > arena.size()) {
      arena.resize(end > arena.size() * 2 ? end : arena.size() * 2);
    }
    used = end;
    return uint32_t(start);
  }
  uint8_t* at(uint32_t offset) { return arena.data() + offset; }
};

class DrawFrameExchange {
 public:
  bool publish(DrawFrame& frame) {
    bool dropped;
    {
      std::lock_guard lock(mutex_);
      dropped = ready_valid_;
      ready_.swap(frame);
      ready_valid_ = true;
    }
    frame.reset();
    return dropped;
  }

  bool take(DrawFrame& frame) {
    frame.reset();
    std::lock_guard lock(mutex_);
    if (!ready_valid_) {
      return false;
    }
    ready_.swap(frame);
    ready_valid_ = false;
    return true;
  }

  void clear() {
    std::lock_guard lock(mutex_);
    ready_ = DrawFrame{};
    ready_valid_ = false;
  }

  bool waiting() {
    std::lock_guard lock(mutex_);
    return ready_valid_;
  }

 private:
  std::mutex mutex_;
  DrawFrame ready_;
  bool ready_valid_ = false;
};

}  // namespace kameo::gpu
