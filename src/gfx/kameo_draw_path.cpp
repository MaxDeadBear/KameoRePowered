// The general draw path: guest DrawIndexedVertices / DrawVertices rendered
// natively with the translated shaders.
//
// Unlike the Bink blit and the 2D overlay, these draws carry no data in their
// arguments -- D3DDevice_DrawIndexedVertices emits PM4 directly and reads
// everything out of the device -- so the state map in kameo_guest_device.h and
// kameo_gfx_state.h *is* the interface. See src/gfx/README.md for how each
// offset was derived.
//
// Two threads are involved and the split matters:
//
//   capture     runs on the guest thread inside the draw hook. Copies every
//               byte the draw depends on (vertices, indices, both constant
//               blocks, the fetch constants) because the guest is free to
//               overwrite all of it later in the same frame.
//   submission  runs on the presenting thread, inside PresentClear's command
//               list. Owns the plume caches: textures, samplers, pipelines.
//
// SCOPE: this is the DXIL/D3D12 path. The SPIR-V variants of the translated
// shaders take their constants through push constants holding buffer device
// addresses, which is a different binding model altogether; the Vulkan backend
// is refused up front rather than silently mis-binding.

#include "kameo_graphics_system.h"

#include <plume_render_interface.h>

#include "kameo_gfx_state.h"
#include "kameo_guest_device.h"
#include "shader_cache.h"

#include <kameorepowered_init.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

// Header-only: this project does not enable the C language, so xxhash.c
// cannot be added as a source.
#define XXH_INLINE_ALL
#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/logging.h>

// Defined in kameo_gfx_hooks.cpp alongside the other gfx cvars.
REXCVAR_DECLARE(bool, kameo_gfx_dump_textures);
REXCVAR_DECLARE(int32_t, kameo_gfx_skip_primitive);
REXCVAR_DECLARE(bool, kameo_gfx_force_vertex_color);
REXCVAR_DECLARE(int32_t, kameo_gfx_only_primitive);
REXCVAR_DECLARE(bool, kameo_gfx_dxn_swap);
REXCVAR_DECLARE(bool, kameo_gfx_preserve_aspect);

#ifdef _WIN32
// plume's own header, for two things its public interface does not expose: the
// native ID3D12Device (to attach an info queue to) and whether a graphics
// pipeline actually got created.
//
// TRAP, and the reason this include exists: plume IGNORES the HRESULT from
// CreateGraphicsPipelineState. createGraphicsPipeline therefore returns a
// non-null wrapper whose `d3d` is null when D3D12 rejected the PSO, and the
// next setPipeline calls SetPipelineState(nullptr) and dies inside d3d12.dll
// with no message. A null check on the returned pointer does NOT catch this.
#include <plume_d3d12.h>
#endif

namespace kameo::gfx {
namespace {

constexpr uint32_t kNoData = 0xFFFFFFFFu;
constexpr uint32_t kSamplerCount = 16;   // XenosRecomp's kSamplerRegisterCount
constexpr uint32_t kConstantRegisters = 256;
constexpr uint32_t kConstantBytes = kConstantRegisters * 16;  // 0x1000, both stages
constexpr uint32_t kSharedConstantBytes = 512;
constexpr uint32_t kRootConstantAlign = 256;  // D3D12 root CBV alignment
// Thresholds for `clip(alpha - threshold)`: below any possible alpha, and
// above any possible alpha.
constexpr float kAlphaPassAll = -1.0f;
constexpr float kAlphaPassNone = 2.0f;

// The guest's own render size. XGetVideoMode reports a HiDef 720p mode and the
// game takes its kamVideoParams = 1280 branch, so every viewport it sets is
// relative to this; the host scales them to whatever the window actually is.
// preShadowRender (0x82260a30) sets this to 1, postShadowRender (0x82260cb8)
// clears it. The authoritative "these draws are a shadow map" signal.
constexpr uint32_t kGeneratingShadowMap = 0x82B70B8Fu;

constexpr uint32_t kGuestWidth = 1280;
constexpr uint32_t kGuestHeight = 720;

// -- guest memory ------------------------------------------------------------
//
// The SDK maps guest physical memory (>= 0xE0000000) one page up on Windows,
// which REX_PHYS_HOST_OFFSET applies for you. That page is the SAME +0x1000
// the older Bink and overlay code adds by hand as "D3D's page bias": for every
// address in the ranges these resources use, `(((addr >> 20) + 512) & 0x1000)`
// and REX_PHYS_HOST_OFFSET produce identical results. Only one of them may be
// applied. This file always uses the raw guest address and lets the SDK macros
// do it, so nothing here adds the bias a second time.

inline const uint8_t* GuestData(uint8_t* base, uint32_t addr) { return REX_RAW_ADDR(addr); }

inline uint32_t Load32(uint8_t* base, uint32_t addr) { return REX_LOAD_U32(addr); }
inline uint16_t Load16(uint8_t* base, uint32_t addr) { return REX_LOAD_U16(addr); }
inline uint8_t Load8(uint8_t* base, uint32_t addr) { return REX_LOAD_U8(addr); }

inline float LoadFloat(uint8_t* base, uint32_t addr) {
  const uint32_t bits = REX_LOAD_U32(addr);
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

inline uint16_t Swap16(uint16_t v) { return __builtin_bswap16(v); }
inline uint32_t Swap32(uint32_t v) { return __builtin_bswap32(v); }

// -- host vertex layout ------------------------------------------------------
//
// The guest declaration says how vertices are stored; the TRANSLATED SHADER
// says what the input assembler has to deliver. XenosRecomp declares
// BLENDINDICES / NORMAL / TANGENT / BINORMAL as uint4 and everything else as
// float4, and D3D12 requires the input element's component type to match.
//
// Most guest types map straight onto a DXGI format. The exception is a
// NON-NORMALISED integer feeding a float input -- SHORT2 texcoords and SHORT4
// positions, which is exactly what Kameo's main declaration uses. DXGI has no
// SSCALED/USCALED formats, so those are converted to float during the capture
// copy. Everything else is copied with only an endian swap.

enum TranscodeOp : uint32_t {
  kSwap16,        // n 16-bit components, byte-swapped in place
  kSwap32,        // n 32-bit units (also packed dwords: DEC3N, D3DCOLOR, UBYTE4N)
  kShortToFloat,  // n int16 -> n float32
  kUShortToFloat,
  kByteToFloat,   // 4 uint8 (in one big-endian dword) -> 4 float32
  kDec3ToFloat,   // packed 10:10:10 -> 3 float32 (plume has no R10G10B10A2)
  kDec3ToR11G11B10,  // guest DEC3N repacked into the layout the shader decodes
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
  bool valid = false;
};

plume::RenderFormat FloatFormat(uint32_t components) {
  switch (components) {
    case 1: return plume::RenderFormat::R32_FLOAT;
    case 2: return plume::RenderFormat::R32G32_FLOAT;
    case 3: return plume::RenderFormat::R32G32B32_FLOAT;
    default: return plume::RenderFormat::R32G32B32A32_FLOAT;
  }
}

// Builds the host layout for one guest vertex declaration. Cached on the
// declaration's guest address -- declarations are immutable once created.
bool BuildHostLayout(uint8_t* base, uint32_t decl, HostLayout* out) {
  const uint32_t count = Load32(base, decl + 8);
  if (count == 0 || count > 32) {
    return false;
  }

  uint32_t offset = 0;
  uint64_t key = 1469598103934665603ull;
  auto mix = [&key](uint32_t v) {
    key = (key ^ v) * 1099511628211ull;
  };

  for (uint32_t e = 0; e < count; ++e) {
    // Xbox's _D3DVERTEXELEMENT9 is 12 bytes, not the 8 PC D3D9 uses: Type is a
    // 32-bit XGVERTEXFORMAT rather than a WORD.
    const uint32_t el = decl + 36 + 12 * e;
    const uint32_t stream = Load16(base, el + 0);
    if (stream == 0xFF) {
      break;  // D3DDECL_END
    }
    HostElement h;
    h.stream = stream;
    h.src_offset = Load16(base, el + 2);
    const uint32_t type = Load32(base, el + 4);
    h.usage = Load8(base, el + 9);
    h.usage_index = Load8(base, el + 10);

    const GuestVertexType t = DecodeVertexType(type);
    if (t.size == 0) {
      REXLOG_WARN("[kameo-gfx] unhandled vertex element type {:08X} (format {}) in decl {:08X}",
                  type, t.data_format, decl);
      return false;
    }
    h.src_size = t.size;
    h.components = t.components;

    const bool integer_input = DeclUsageIsInteger(h.usage);
    if (integer_input) {
      // uint4 in the shader: keep the bits, pick a UINT format of the same shape.
      if (t.packed && t.data_format == 7 &&
          (h.usage == kUsageNormal || h.usage == kUsageTangent || h.usage == kUsageBinormal)) {
        // The translated shader runs tfetchR11G11B10 on these -- XenosRecomp
        // picks that decode from the USAGE, not from the declared format,
        // because Sonic Unleashed's normals really are 11:11:10. Kameo's are
        // DEC3N (2:10:10:10 signed), so the shader would read the wrong field
        // widths and produce a garbage tangent frame. Repacking here is exact
        // and leaves the shader untouched.
        h.op = kDec3ToR11G11B10;
        h.format = plume::RenderFormat::R32_UINT;
        h.dst_size = 4;
      } else if (t.packed || t.component_bits == 32) {
        h.op = kSwap32;
        h.format = t.components == 1   ? plume::RenderFormat::R32_UINT
                   : t.components == 2 ? plume::RenderFormat::R32G32_UINT
                   : t.components == 3 ? plume::RenderFormat::R32G32B32_UINT
                                       : plume::RenderFormat::R32G32B32A32_UINT;
        h.dst_size = t.size;
      } else if (t.component_bits == 16) {
        h.op = kSwap16;
        h.format = t.components <= 2 ? plume::RenderFormat::R16G16_UINT
                                     : plume::RenderFormat::R16G16B16A16_UINT;
        h.dst_size = t.size;
      } else {
        h.op = kSwap32;
        h.format = plume::RenderFormat::R8G8B8A8_UINT;
        h.dst_size = 4;
      }
    } else if (t.is_float) {
      h.op = (t.component_bits == 16) ? kSwap16 : kSwap32;
      if (t.component_bits == 16) {
        h.format = t.components <= 2 ? plume::RenderFormat::R16G16_FLOAT
                                     : plume::RenderFormat::R16G16B16A16_FLOAT;
      } else {
        h.format = FloatFormat(t.components);
      }
      h.dst_size = t.size;
    } else if (t.num_format == kVertexColor) {
      // D3DCOLOR is an ARGB dword: swapped it reads B,G,R,A in memory order.
      h.op = kSwap32;
      h.format = plume::RenderFormat::B8G8R8A8_UNORM;
      h.dst_size = 4;
    } else if (t.num_format == kVertexUNorm || t.num_format == kVertexSNorm) {
      const bool sn = (t.num_format == kVertexSNorm);
      if (t.packed) {
        // DEC3N / UDEC3 feeding a float input. plume exposes no packed 10:10:10
        // format, so it is unpacked during the capture copy like the other
        // conversions.
        h.op = kDec3ToFloat;
        h.format = FloatFormat(3);
        h.dst_size = 12;
        h.components = 3;
      } else if (t.component_bits == 16) {
        h.op = kSwap16;
        h.format = t.components <= 2
                       ? (sn ? plume::RenderFormat::R16G16_SNORM : plume::RenderFormat::R16G16_UNORM)
                       : (sn ? plume::RenderFormat::R16G16B16A16_SNORM
                             : plume::RenderFormat::R16G16B16A16_UNORM);
        h.dst_size = t.size;
      } else {
        h.op = kSwap32;
        h.format = sn ? plume::RenderFormat::R8G8B8A8_SNORM : plume::RenderFormat::R8G8B8A8_UNORM;
        h.dst_size = 4;
      }
    } else {
      // Non-normalised integer feeding a float input. No DXGI format converts
      // these, so convert on the way in.
      if (t.component_bits == 16) {
        h.op = (t.num_format == kVertexUInt) ? kUShortToFloat : kShortToFloat;
        h.format = FloatFormat(t.components);
        h.dst_size = t.components * 4;
      } else if (t.component_bits == 8) {
        h.op = kByteToFloat;
        h.format = FloatFormat(4);
        h.dst_size = 16;
      } else {
        h.op = kSwap32;
        h.format = FloatFormat(t.components);
        h.dst_size = t.size;
      }
    }

    // 4-byte alignment keeps every element naturally aligned for the copy.
    offset = (offset + 3u) & ~3u;
    h.dst_offset = offset;
    offset += h.dst_size;

    mix(h.usage);
    mix(h.usage_index);
    mix(uint32_t(h.format));
    mix(h.dst_offset);
    out->elements.push_back(h);
  }

  if (out->elements.empty()) {
    return false;
  }
  out->dst_stride = offset;
  out->key = key;
  out->valid = true;
  return true;
}

// -- captured draws ----------------------------------------------------------

// A captured frame is an ordered command stream, not just a bag of draws: a
// Resolve has to happen at the point in the frame the guest issued it, because
// what it copies is whatever has been drawn so far.
enum class CommandKind : uint32_t {
  Draw,
  Resolve,
  Bink,  // the decoded video frame, drawn at the point the guest blitted it
};

struct DrawCall {
  CommandKind kind = CommandKind::Draw;

  // Resolve only: where the result goes, and what part of the target to take.
  uint32_t resolve_address = 0;   // destination texture's guest address
  uint32_t resolve_width = 0;
  uint32_t resolve_height = 0;
  int32_t resolve_x = 0;
  int32_t resolve_y = 0;
  bool resolve_depth = false;
  // No pixel shader bound: a depth-only pass. The guest renders its shadow maps
  // this way, which is why they resolve 768x768 / 1024x1024 out of surface 0.
  bool depth_only = false;

  uint32_t primitive = 0;
  const ShaderCacheEntry* vs = nullptr;
  const ShaderCacheEntry* ps = nullptr;

  uint32_t vertex_data = kNoData;  // arena offset, host layout
  uint32_t vertex_bytes = 0;
  uint32_t vertex_stride = 0;
  uint32_t vertex_count = 0;

  // Set for a draw captured at D3DDevice_BeginVertices, where the guest has NOT
  // written its vertices yet. The state is right at that moment and wrong a
  // moment later (the next icon binds its texture before its own BeginVertices),
  // so the state is taken now and the vertices are read at frame end, by which
  // time the guest has filled the ring. Zero for every ordinary draw.
  uint32_t deferred_vertex_addr = 0;
  uint32_t deferred_vertex_stride = 0;
  uint32_t deferred_vertex_count = 0;
  // The device must be carried too. Resolving these at frame end with a
  // device of 0 makes any element on a stream OTHER than 0 read its buffer
  // pointer out of guest address ~0x1170 -- arbitrary memory reinterpreted as
  // a vertex fetch constant.
  uint32_t deferred_device = 0;

  uint32_t index_data = kNoData;  // arena offset, always 32-bit host indices
  uint32_t index_count = 0;

  const HostLayout* layout = nullptr;

  uint32_t vs_constants = kNoData;  // arena offset, host endian
  uint32_t ps_constants = kNoData;
  uint32_t booleans = 0;

  GuestTextureFetch textures[kSamplerCount];
  uint32_t texture_objects[kSamplerCount] = {};  // guest D3DBaseTexture*, for BaseFlush

  uint32_t depth_control = 0;
  uint32_t blend_control = 0;
  uint32_t color_control = 0;
  uint32_t mode_control = 0;
  uint32_t color_mask = 0xF;
  uint32_t rt_surface = 0;   // D3DSurface* the draw renders into
  float alpha_ref = 0.0f;
  float viewport[6] = {0, 0, 0, 0, 0, 1};
};

struct DrawFrame {
  std::vector<DrawCall> draws;
  std::vector<uint8_t> arena;

  void reset() {
    draws.clear();
    arena.clear();
  }

  uint32_t alloc(uint32_t bytes, uint32_t align = 4) {
    const size_t start = (arena.size() + align - 1) & ~size_t(align - 1);
    arena.resize(start + bytes);
    return uint32_t(start);
  }
  uint8_t* at(uint32_t offset) { return arena.data() + offset; }
};

std::mutex g_captureMutex;
// The guest memory base, recorded by the capture so submission can resolve
// texture data without a hook context of its own.
uint8_t* g_guestBase = nullptr;
DrawFrame g_capturing;
DrawFrame g_ready;
bool g_readyValid = false;

// Declaration -> host layout. Never invalidated: D3DVertexDeclaration objects
// are immutable, and the map is keyed on the guest object address.
std::map<uint32_t, HostLayout> g_layouts;

// Constant-block dedup, per frame. Consecutive draws overwhelmingly share their
// constants, and a 4 KB memcmp against guest memory is far cheaper than the
// swap-and-copy it avoids.
struct ConstantDedup {
  std::vector<uint8_t> raw;  // last snapshot, still big-endian
  uint32_t offset = kNoData;
  void reset() {
    raw.clear();
    offset = kNoData;
  }
};
ConstantDedup g_vsDedup;
ConstantDedup g_psDedup;

// Vertex reuse, per frame. The same mesh is redrawn several times a frame (a
// shadow pass, then the main pass, then whatever else is predicated on it), and
// each of those draws was transcoding and copying the identical vertex window
// again. Keyed on what fully determines the copy: the source buffer, the window
// inside it, and the host layout.
// The offset of the copy, plus a hash of the SOURCE bytes it was made from.
// The key describes where the vertices came from, which is not enough on its
// own: the guest streams assets in and out, so the same address can hold a
// different mesh later in the same frame. Reusing on the key alone then draws
// the earlier mesh's vertices through this draw's indices, which is what makes
// geometry explode across the screen as things pop in and out.
struct VertexReuse {
  uint32_t offset = 0;
  uint64_t hash = 0;
};
std::unordered_map<uint64_t, VertexReuse> g_vertexReuse;

// The SAME transcoded bytes, kept ACROSS frames. g_vertexReuse above is cleared
// every frame with the arena, so until now every vertex in the scene was
// re-transcoded from guest memory 60 times a second even though level geometry
// never changes -- measured at 24 MB and 33 ms a frame, far and away the
// largest CPU cost in the renderer and a hard ceiling on frame rate before the
// GPU is reached at all.
//
// A hit still copies into the frame arena rather than handing the submit path a
// pointer into this map. That is deliberate: the arena is moved wholesale to
// the render thread while the guest thread is already capturing the next frame,
// so a DrawCall pointing in here could be read while this map is being written.
// A memcpy is roughly ten times cheaper than the transcode and keeps the frame
// self-contained.
//
// Validity is the same sampled content hash the per-frame path uses, so a
// restreamed buffer is caught the same way it always was.
struct TranscodeCacheEntry {
  std::vector<uint8_t> bytes;
  uint64_t hash = 0;
  uint64_t last_frame = 0;
};
std::unordered_map<uint64_t, TranscodeCacheEntry> g_transcodeCache;
uint64_t g_captureFrame = 0;      // bumped once per captured frame, for eviction
uint32_t g_transcodeCacheHits = 0;
uint32_t g_transcodeCacheMiss = 0;
size_t g_transcodeCacheBytes = 0;
uint32_t g_vertexReuseStale = 0;  // hits rejected because the source changed

// Where the capture's CPU time actually goes. The capture-only measurement
// (--kameo_gfx_only_primitive=99, which captures everything and submits
// nothing) put the CPU side at ~28.6 ms a frame on its own -- a hard 35 fps
// ceiling before the GPU does anything at all. These split that number so the
// optimisation goes where the time is instead of where it is assumed to be.
std::atomic<uint64_t> g_nsVertex{0};    // transcoding vertices
std::atomic<uint64_t> g_nsConstants{0}; // copying + byte-swapping constant blocks
std::atomic<uint64_t> g_nsIndices{0};   // topology expansion and index rebasing
std::atomic<uint64_t> g_nsHash{0};      // the restream content check
uint64_t g_captureBytes = 0;            // transcoded vertex bytes per 120 frames

uint32_t g_captureSkippedNoShader = 0;
uint32_t g_captureDepthOnly = 0;           // depth-only draws CAPTURED (no pixel shader)
uint32_t g_captureSkippedTranslation = 0;  // shader bound but absent from the cache
uint32_t g_captureSkippedNullPS = 0;       // colour draw with NO pixel shader bound
uint32_t g_captureSkippedNoVSObject = 0;   // no vertex shader bound at all
uint32_t g_captureSkippedNoLayout = 0;
uint32_t g_captureSkippedOther = 0;

const HostLayout* GetLayout(uint8_t* base, uint32_t decl) {
  auto found = g_layouts.find(decl);
  if (found != g_layouts.end()) {
    return found->second.valid ? &found->second : nullptr;
  }
  HostLayout layout;
  BuildHostLayout(base, decl, &layout);
  auto inserted = g_layouts.emplace(decl, std::move(layout));
  return inserted.first->second.valid ? &inserted.first->second : nullptr;
}

// Copies a constant block out of guest memory, swapping to host endian, and
// reuses the previous copy when the guest bytes have not changed.
uint32_t CaptureConstants(uint8_t* base, uint32_t address, ConstantDedup* dedup, DrawFrame* frame) {
  struct Timer {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~Timer() {
      g_nsConstants += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - t0)
                                    .count());
    }
  } timer;
  const uint8_t* src = GuestData(base, address);
  if (dedup->offset != kNoData && dedup->raw.size() == kConstantBytes &&
      std::memcmp(dedup->raw.data(), src, kConstantBytes) == 0) {
    return dedup->offset;
  }

  dedup->raw.assign(src, src + kConstantBytes);
  const uint32_t offset = frame->alloc(kConstantBytes, 16);
  auto* dst = reinterpret_cast<uint32_t*>(frame->at(offset));
  const auto* words = reinterpret_cast<const uint32_t*>(src);
  for (uint32_t i = 0; i < kConstantBytes / 4; ++i) {
    dst[i] = Swap32(words[i]);
  }
  dedup->offset = offset;
  return offset;
}

// Transcodes vertices into the frame arena, covering at least [min_vertex,
// max_vertex].
//
// It prefers to transcode the WHOLE source buffer and cache that, rather than
// the window this draw happens to touch. Kameo puts many meshes in one buffer
// and draws them as chunks whose index ranges overlap heavily, so per-draw
// windows re-copied the same buffer over and over -- 64 MB of vertex uploads a
// frame, which exhausted the ring and dropped ~1600 draws. One copy per buffer
// per frame is both smaller and cheaper, and it leaves indices usable as-is.
//
// `out_base` is what the caller must subtract from each index: 0 for a whole
// buffer, min_vertex for the fallback window.
// DrawVerticesUP hands the vertex data over as a pointer instead of binding a
// stream, so stream 0 can be overridden rather than resolved from the device.
struct StreamOverride {
  const uint8_t* data = nullptr;
  uint32_t stride = 0;
  uint32_t count = 0;
  bool active() const { return data != nullptr; }
};

bool CaptureVertices(uint8_t* base, uint32_t device, const HostLayout& layout, uint32_t min_vertex,
                     uint32_t max_vertex, DrawFrame* frame, uint32_t* out_offset,
                     uint32_t* out_bytes, uint32_t* out_base, uint32_t* out_count,
                     const StreamOverride& override_stream = {}) {
  // Per-stream source pointers. Streams are the same for every element, so they
  // are resolved once rather than per vertex.
  struct StreamInfo {
    const uint8_t* data = nullptr;
    uint32_t stride = 0;
    uint32_t size = 0;  // bytes available from `data`
  };
  StreamInfo streams[16];

  for (const HostElement& e : layout.elements) {
    if (e.stream >= 16 || streams[e.stream].data != nullptr) {
      continue;
    }
    if (override_stream.active() && e.stream == 0) {
      streams[0].data = override_stream.data;
      streams[0].stride = override_stream.stride;
      streams[0].size = override_stream.stride * override_stream.count;
      continue;
    }
    const uint32_t buffer = Load32(base, device + dev::kStreamBuffer + 8 * e.stream);
    if (!buffer) {
      return false;
    }
    // Buffers do NOT carry their address in BaseFlush the way textures do; it
    // is in a Xenos vertex fetch constant at +12, with the size in dwords at
    // +16. The low two bits of +12 are the fetch type, not address bits.
    const uint32_t fetch0 = Load32(base, buffer + 12);
    const uint32_t fetch1 = Load32(base, buffer + 16);
    const uint32_t address = fetch0 & 0xFFFFFFFCu;
    if (!address) {
      return false;
    }
    const uint32_t byte_offset = Load32(base, device + dev::kStreamOffset + 8 * e.stream);
    // The stride is stored as a BYTE holding (stride >> 2).
    const uint32_t stride = Load8(base, device + dev::kStreamStride + e.stream)
                            << dev::kStreamStrideShift;
    if (stride == 0) {
      return false;
    }
    streams[e.stream].data = GuestData(base, address + byte_offset);
    streams[e.stream].stride = stride;
    // +16 is `endian:2 | size:24`, the size in DWORDS.
    streams[e.stream].size = ((fetch1 >> 2) & 0xFFFFFFu) * 4;
  }

  // How many vertices the smallest bound stream can supply. A whole-buffer
  // copy is only safe when every stream actually covers the draw's range.
  uint32_t whole = 0xFFFFFFFFu;
  for (const HostElement& e : layout.elements) {
    const StreamInfo& stream = streams[e.stream];
    if (stream.data && stream.stride) {
      whole = std::min(whole, stream.size / stream.stride);
    }
  }

  constexpr uint32_t kWholeBufferByteCap = 16u << 20;
  uint32_t first = min_vertex;
  uint32_t count = max_vertex - min_vertex + 1;
  uint32_t index_base = min_vertex;
  if (whole != 0xFFFFFFFFu && whole > max_vertex &&
      uint64_t(whole) * layout.dst_stride <= kWholeBufferByteCap) {
    first = 0;
    count = whole;
    index_base = 0;
  }
  *out_base = index_base;
  *out_count = count;

  // Reuse an identical copy made earlier this frame. The key has to describe
  // the SOURCE, not the draw: two draws of the same mesh differ in state and
  // constants but share every vertex byte.
  //
  // NOT for DrawVerticesUP. Its data arrives in a scratch buffer the guest
  // rewrites before every draw, so the same pointer, count and layout describe
  // completely different vertices each time. Reusing on that key collapsed all
  // ~148 glyphs of a screen's text into one quad -- 148 draws submitted, and
  // the vertex ring reporting 0 KB uploaded.
  const uint32_t bytes = count * layout.dst_stride;
  const bool reusable = !override_stream.active();
  uint64_t reuse_key = layout.key ^ (uint64_t(first) << 32) ^ (uint64_t(count) << 8);
  uint64_t source_hash = 0;
  if (reusable) {
    for (const HostElement& e : layout.elements) {
      if (e.stream < 16 && streams[e.stream].data) {
        reuse_key ^= reinterpret_cast<uintptr_t>(streams[e.stream].data) * 1099511628211ull;
      }
    }
    // Hash what would be copied. A pointer only says WHERE the vertices came
    // from; streaming means the same address can hold a different mesh a few
    // draws later. Hashing is a sequential read of bytes the transcode would
    // read anyway -- no write and no per-element conversion -- so it is far
    // cheaper than the copy it still avoids in the common case.
    uint32_t hashed_streams = 0;
    for (const HostElement& e : layout.elements) {
      const StreamInfo& stream = streams[e.stream];
      if (e.stream >= 16 || !stream.data || !stream.stride) {
        continue;
      }
      if (hashed_streams & (1u << e.stream)) {
        continue;
      }
      hashed_streams |= 1u << e.stream;
      const auto hash_t0 = std::chrono::steady_clock::now();
      const size_t span = size_t(count) * stream.stride;
      const size_t available = size_t(stream.size) > size_t(first) * stream.stride
                                   ? stream.size - size_t(first) * stream.stride
                                   : 0;
      const size_t len = std::min(span, available);
      if (len == 0) {
        continue;
      }
      const uint8_t* src = stream.data + size_t(first) * stream.stride;
      // SAMPLE rather than hash the whole buffer. `count` is the entire source
      // buffer for a whole-buffer transcode, so hashing all of it once per
      // reusing draw re-reads exactly the data the reuse cache exists to avoid
      // touching -- measured as a large part of a 99 -> 34 fps drop. Streaming
      // replaces a whole buffer, so three windows plus the length identify a
      // restream just as well at a fraction of the cost.
      constexpr size_t kHashWindow = 2048;
      auto mix_at = [&](size_t off, size_t n) {
        if (off < len) {
          source_hash ^= XXH3_64bits(src + off, std::min(n, len - off)) + 0x9E3779B97F4A7C15ull +
                         (uint64_t(e.stream) << 1) + off;
        }
      };
      if (len <= kHashWindow * 3) {
        source_hash ^= XXH3_64bits(src, len) + 0x9E3779B97F4A7C15ull + (uint64_t(e.stream) << 1);
      g_nsHash += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - hash_t0)
                               .count());
      } else {
        mix_at(0, kHashWindow);
        mix_at(len / 2, kHashWindow);
        mix_at(len - kHashWindow, kHashWindow);
      }
      source_hash ^= uint64_t(len) * 1099511628211ull;
    }
    auto reused = g_vertexReuse.find(reuse_key);
    if (reused != g_vertexReuse.end()) {
      if (reused->second.hash == source_hash) {
        *out_offset = reused->second.offset;
        *out_bytes = bytes;
        return true;
      }
      // Same buffer, different contents: the guest restreamed it. Fall through
      // and transcode the new data rather than drawing the old mesh.
      ++g_vertexReuseStale;
    }

    // Not seen yet THIS frame -- but very likely transcoded in an earlier one.
    auto cached = g_transcodeCache.find(reuse_key);
    if (cached != g_transcodeCache.end() && cached->second.hash == source_hash &&
        cached->second.bytes.size() == bytes) {
      const uint32_t offset = frame->alloc(bytes, 16);
      std::memcpy(frame->at(offset), cached->second.bytes.data(), bytes);
      cached->second.last_frame = g_captureFrame;
      g_vertexReuse.insert_or_assign(reuse_key, VertexReuse{offset, source_hash});
      ++g_transcodeCacheHits;
      *out_offset = offset;
      *out_bytes = bytes;
      return true;
    }
    ++g_transcodeCacheMiss;
  }

  const uint32_t offset = frame->alloc(bytes, 16);
  uint8_t* dst_base = frame->at(offset);
  const auto transcode_start = std::chrono::steady_clock::now();
  g_captureBytes += bytes;

  const bool force_color = REXCVAR_GET(kameo_gfx_force_vertex_color);
  for (const HostElement& e : layout.elements) {
    const StreamInfo& stream = streams[e.stream];
    if (!stream.data) {
      return false;
    }
    if (force_color && e.usage == kUsageColor) {
      for (uint32_t v = 0; v < count; ++v) {
        std::memset(dst_base + size_t(v) * layout.dst_stride + e.dst_offset, 0xFF, e.dst_size);
      }
      continue;
    }
    // The op switch is hoisted OUT of the per-vertex loop. It used to sit
    // inside it, so every vertex of every element paid a branch and the body
    // could not be vectorised -- and this is the hottest loop in the renderer:
    // measured at 15.1 ms a frame transcoding 9.7 MB, the single largest CPU
    // cost in the capture and a hard ceiling on frame rate before the GPU is
    // even reached. Each case now runs its own tight loop over a fixed stride.
    const uint8_t* const src0 = stream.data + size_t(first) * stream.stride + e.src_offset;
    uint8_t* const dst0 = dst_base + e.dst_offset;
    const size_t sstride = stream.stride;
    const size_t dstride = layout.dst_stride;

    switch (e.op) {
      case kSwap16: {
        const uint32_t components = e.components;
        for (uint32_t v = 0; v < count; ++v) {
          const uint8_t* src = src0 + size_t(v) * sstride;
          uint8_t* dst = dst0 + size_t(v) * dstride;
          for (uint32_t c = 0; c < components; ++c) {
            uint16_t value;
            std::memcpy(&value, src + 2 * c, 2);
            value = Swap16(value);
            std::memcpy(dst + 2 * c, &value, 2);
          }
        }
        break;
      }
      case kSwap32: {
        // The overwhelmingly common case, and almost always a single dword
        // (D3DCOLOR, DEC3N, UBYTE4N) or a float3 position. Peeling the 1- and
        // 3-unit shapes out keeps the inner loop a straight-line copy.
        const uint32_t units = e.src_size / 4;
        if (units == 1) {
          for (uint32_t v = 0; v < count; ++v) {
            uint32_t value;
            std::memcpy(&value, src0 + size_t(v) * sstride, 4);
            value = Swap32(value);
            std::memcpy(dst0 + size_t(v) * dstride, &value, 4);
          }
        } else if (units == 3) {
          for (uint32_t v = 0; v < count; ++v) {
            const uint8_t* src = src0 + size_t(v) * sstride;
            uint8_t* dst = dst0 + size_t(v) * dstride;
            uint32_t a, b, c;
            std::memcpy(&a, src + 0, 4);
            std::memcpy(&b, src + 4, 4);
            std::memcpy(&c, src + 8, 4);
            a = Swap32(a);
            b = Swap32(b);
            c = Swap32(c);
            std::memcpy(dst + 0, &a, 4);
            std::memcpy(dst + 4, &b, 4);
            std::memcpy(dst + 8, &c, 4);
          }
        } else {
          for (uint32_t v = 0; v < count; ++v) {
            const uint8_t* src = src0 + size_t(v) * sstride;
            uint8_t* dst = dst0 + size_t(v) * dstride;
            for (uint32_t c = 0; c < units; ++c) {
              uint32_t value;
              std::memcpy(&value, src + 4 * c, 4);
              value = Swap32(value);
              std::memcpy(dst + 4 * c, &value, 4);
            }
          }
        }
        break;
      }
      case kShortToFloat:
      case kUShortToFloat: {
        const bool is_signed = (e.op == kShortToFloat);
        const uint32_t components = e.components;
        for (uint32_t v = 0; v < count; ++v) {
          const uint8_t* src = src0 + size_t(v) * sstride;
          uint8_t* dst = dst0 + size_t(v) * dstride;
          for (uint32_t c = 0; c < components; ++c) {
            uint16_t raw;
            std::memcpy(&raw, src + 2 * c, 2);
            raw = Swap16(raw);
            const float value = is_signed ? float(int16_t(raw)) : float(raw);
            std::memcpy(dst + 4 * c, &value, 4);
          }
        }
        break;
      }
      case kByteToFloat: {
        for (uint32_t v = 0; v < count; ++v) {
          uint32_t packed;
          std::memcpy(&packed, src0 + size_t(v) * sstride, 4);
          packed = Swap32(packed);
          uint8_t* dst = dst0 + size_t(v) * dstride;
          for (uint32_t c = 0; c < 4; ++c) {
            const float value = float((packed >> (8 * c)) & 0xFF);
            std::memcpy(dst + 4 * c, &value, 4);
          }
        }
        break;
      }
      case kDec3ToR11G11B10: {
        // tfetchR11G11B10 reconstructs each component as (sign ? -1 : 0) +
        // magnitude/scale, so encode to exactly that: an unsigned magnitude
        // plus a bit that subtracts one.
        auto encode = [](float value, uint32_t magnitude_bits) {
          const float scale = float(1u << magnitude_bits);
          const float c = std::clamp(value, -1.0f, 1.0f - 1.0f / scale);
          const uint32_t sign = (c < 0.0f) ? 1u : 0u;
          const uint32_t mag =
              uint32_t(std::lround((c + float(sign)) * scale)) & ((1u << magnitude_bits) - 1u);
          return (sign << magnitude_bits) | mag;
        };
        for (uint32_t v = 0; v < count; ++v) {
          uint32_t packed;
          std::memcpy(&packed, src0 + size_t(v) * sstride, 4);
          packed = Swap32(packed);
          // Guest DEC3N: three signed 10-bit fields at 0..9, 10..19, 20..29.
          auto decode = [packed](uint32_t shift) {
            const int32_t raw = int32_t(packed << (22 - shift)) >> 22;
            return float(raw) / 511.0f;
          };
          // x: magnitude bits 0..9, sign bit 10. y: the same, shifted 11.
          // z: magnitude bits 22..30, sign bit 31.
          const uint32_t out = encode(decode(0), 10) | (encode(decode(10), 10) << 11) |
                               (encode(decode(20), 9) << 22);
          std::memcpy(dst0 + size_t(v) * dstride, &out, 4);
        }
        break;
      }
      case kDec3ToFloat: {
        for (uint32_t v = 0; v < count; ++v) {
          uint32_t packed;
          std::memcpy(&packed, src0 + size_t(v) * sstride, 4);
          packed = Swap32(packed);
          uint8_t* dst = dst0 + size_t(v) * dstride;
          for (uint32_t c = 0; c < 3; ++c) {
            const uint32_t field = (packed >> (10 * c)) & 0x3FF;
            // Sign extend from 10 bits, then normalise the way DEC3N does.
            const int32_t signed_field = int32_t(field << 22) >> 22;
            const float value = float(signed_field) / 511.0f;
            std::memcpy(dst + 4 * c, &value, 4);
          }
        }
        break;
      }
    }
  }

  g_nsVertex += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - transcode_start)
                             .count());
  if (reusable) {
    g_vertexReuse.insert_or_assign(reuse_key, VertexReuse{offset, source_hash});
    TranscodeCacheEntry& entry = g_transcodeCache[reuse_key];
    entry.bytes.assign(dst_base, dst_base + bytes);
    entry.hash = source_hash;
    entry.last_frame = g_captureFrame;
  }
  *out_offset = offset;
  *out_bytes = bytes;
  return true;
}

// Fans and quad lists have no D3D12 topology, so they are expanded into a
// triangle list here. Returns false when the primitive is one we do not draw.
bool ExpandTopology(uint32_t primitive, uint32_t vertex_count, const uint32_t* indices,
                    std::vector<uint32_t>* out) {
  auto index_at = [&](uint32_t i) { return indices ? indices[i] : i; };

  switch (primitive) {
    case kPrimTriangleList:
    case kPrimTriangleStrip:
    case kPrimLineList:
    case kPrimLineStrip:
    case kPrimPointList:
      out->resize(vertex_count);
      for (uint32_t i = 0; i < vertex_count; ++i) {
        (*out)[i] = index_at(i);
      }
      return true;

    case kPrimTriangleFan: {
      if (vertex_count < 3) return false;
      const uint32_t triangles = vertex_count - 2;
      out->reserve(triangles * 3);
      for (uint32_t t = 0; t < triangles; ++t) {
        out->push_back(index_at(0));
        out->push_back(index_at(t + 1));
        out->push_back(index_at(t + 2));
      }
      return true;
    }

    case kPrimQuadList: {
      if (vertex_count < 4) return false;
      const uint32_t quads = vertex_count / 4;
      out->reserve(quads * 6);
      for (uint32_t q = 0; q < quads; ++q) {
        const uint32_t v = q * 4;
        out->push_back(index_at(v + 0));
        out->push_back(index_at(v + 1));
        out->push_back(index_at(v + 2));
        out->push_back(index_at(v + 0));
        out->push_back(index_at(v + 2));
        out->push_back(index_at(v + 3));
      }
      return true;
    }

    default:
      return false;
  }
}

plume::RenderPrimitiveTopology HostTopology(uint32_t primitive) {
  switch (primitive) {
    case kPrimPointList: return plume::RenderPrimitiveTopology::POINT_LIST;
    case kPrimLineList: return plume::RenderPrimitiveTopology::LINE_LIST;
    case kPrimLineStrip: return plume::RenderPrimitiveTopology::LINE_STRIP;
    case kPrimTriangleStrip: return plume::RenderPrimitiveTopology::TRIANGLE_STRIP;
    default: return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  }
}

// Reads the per-draw device state the pipeline key and the shared constants
// need. Offsets and bit positions come from Kameo's own render-state setters --
// see the provenance block in kameo_gfx_state.h.
void CaptureState(uint8_t* base, uint32_t device, DrawCall* call) {
  call->depth_control = Load32(base, device + state::kDepthControl);
  call->blend_control = Load32(base, device + state::kBlendControl0);
  call->color_control = Load32(base, device + state::kColorControl);
  call->mode_control = Load32(base, device + state::kModeControl);
  call->color_mask = Load32(base, device + state::kColorMask) & 0xF;
  call->rt_surface = Load32(base, device + dev::kRenderTarget0);
  // Alpha test. The translated shaders implement exactly ONE comparison --
  // `clip(oC0.w - g_AlphaThreshold)`, which discards when alpha < threshold,
  // i.e. GEQUAL -- so the guest's alpha FUNC has to be folded into the
  // threshold. It used to be ignored entirely, and that is what drew the black
  // quads over the levels.
  //
  // Kameo's foliage uses the standard cut-out setup: enabled, func GREATER,
  // ref 0 ("keep only texels with alpha strictly above zero"). Read as GEQUAL
  // that becomes "keep alpha >= 0", which is every texel -- so the 62% of a
  // leaf texture that is fully transparent was drawn as well. With blending
  // off (those draws are ONE/ZERO) the card came out as a solid rectangle, and
  // since the transparent texels are black the colour landed on
  // lerp(FogColour, 0, fog) -- measured on screen as R4 G8 B10 against a
  // FogColour of (0.384, 0.800, 1.000). That is the whole bug.
  //
  // GREATER is turned into GEQUAL by nudging the threshold up by half a texel
  // step. Alpha arrives from 8-bit UNORM textures, so it moves in 1/255ths and
  // 1/512 sits safely between "exactly zero" and "the smallest nonzero alpha".
  const uint32_t alpha_func = call->color_control & state::kAlphaFuncMask;
  const bool alpha_test = (call->color_control & state::kAlphaTestEnableBit) != 0;
  const float ref = alpha_test ? LoadFloat(base, device + state::kAlphaRef) : 0.0f;
  if (!alpha_test) {
    call->alpha_ref = kAlphaPassAll;
  } else {
    switch (alpha_func) {
      case state::kAlphaFuncNever:   call->alpha_ref = kAlphaPassNone; break;
      case state::kAlphaFuncGreater: call->alpha_ref = ref + (1.0f / 512.0f); break;
      case state::kAlphaFuncGEqual:  call->alpha_ref = ref; break;
      case state::kAlphaFuncAlways:  call->alpha_ref = kAlphaPassAll; break;
      default:
        // LESS / LEQUAL / EQUAL / NOTEQUAL cannot be expressed as a single
        // one-sided clip, so they pass everything rather than cut wrongly.
        // Not seen in Kameo so far; if something draws with a halo, look here.
        call->alpha_ref = kAlphaPassAll;
        break;
    }
  }

  for (uint32_t i = 0; i < 4; ++i) {
    call->viewport[i] = float(Load32(base, device + dev::kViewport + 4 * i));
  }
  call->viewport[4] = LoadFloat(base, device + dev::kViewport + 16);
  call->viewport[5] = LoadFloat(base, device + dev::kViewport + 20);
  // Census of the distinct (viewport, surface) combinations the guest draws
  // with. The shadow passes resolve 768x768 and 1024x1024 out of surface 0
  // (depth-only, no colour target), and nothing of that size is being
  // redirected -- this says whether those draws arrive with a matching
  // viewport, arrive full-size, or never arrive at all.
  {
    const uint32_t vw = uint32_t(call->viewport[2]);
    const uint32_t vh = uint32_t(call->viewport[3]);
    static std::set<uint64_t> seen;
    const uint64_t combo = (uint64_t(vw) << 48) ^ (uint64_t(vh) << 32) ^ call->rt_surface;
    if (seen.size() < 64 && seen.insert(combo).second) {
      REXLOG_INFO("[kameo-gfx] draw viewport census: {}x{} surface {:08X}", vw, vh,
                  call->rt_surface);
    }
  }

  for (uint32_t s = 0; s < kSamplerCount; ++s) {
    const uint32_t fetch = device + dev::kSamplerFetchConstants + dev::kSamplerFetchStride * s;
    for (uint32_t w = 0; w < 6; ++w) {
      call->textures[s].words[w] = Load32(base, fetch + 4 * w);
    }
    call->texture_objects[s] = Load32(base, device + dev::kTextureSlots + 4 * s);
  }

  // Bool constants: the vertex block in bits 0..15, the pixel block in 16..31,
  // which is the packing XenosRecomp's `b{N}` defines assume.
  const uint32_t vs_bools = Load32(base, device + dev::kVertexShaderBoolConstants);
  const uint32_t ps_bools = Load32(base, device + dev::kPixelShaderBoolConstants);
  call->booleans = (vs_bools & 0xFFFF) | ((ps_bools & 0xFFFF) << 16);
}

// Shared by the indexed and non-indexed capture entry points.
void CaptureCommon(uint8_t* base, uint32_t device, uint32_t primitive, uint32_t first_vertex,
                   uint32_t vertex_count, const uint32_t* indices, uint32_t index_count,
                   const StreamOverride& override_stream = {},
                   uint32_t deferred_vertex_addr = 0, uint32_t deferred_vertex_stride = 0) {
  const uint32_t vs_object = Load32(base, device + 0x4FD8);
  const uint32_t ps_object = Load32(base, device + 0x3290);
  const ShaderCacheEntry* vs = LookupShaderBinding(vs_object);
  const ShaderCacheEntry* ps = LookupShaderBinding(ps_object);
  if (!vs || !ps) {
    // "No shader" was hiding two unrelated problems behind one counter:
    //
    //   * ps_object == 0 -- a DEPTH-ONLY draw. The guest binds no pixel shader
    //     for shadow passes, which is why the shadow maps resolve 768x768 and
    //     1024x1024 out of surface 0 while no draw of that size is ever seen.
    //     These are not missing translations; they need a depth-only pipeline.
    //   * ps_object != 0 but not in the cache -- a genuinely missing
    //     translation, fixed by dumping the container and rebuilding.
    //
    // They need opposite fixes, so they get separate counters.
    // A null pixel shader does NOT mean "shadow pass". grass_initRender
    // (0x824dc600) releases the pixel shader and writes 0 to device+12944,
    // which is 0x3290 -- the very slot read above -- so grass draws arrive with
    // ps_object == 0 while being an ordinary COLOUR pass. Routing those to a
    // depth-only target with no colour output is why the grass went wrong while
    // the trees, which never take this path, stayed fine.
    //
    // The game says outright when it is building a shadow map: preShadowRender
    // sets GeneratingShadowMap = 1 and postShadowRender clears it. Use that
    // rather than inferring from a shader slot.
    const bool generating_shadow = Load8(base, kGeneratingShadowMap) != 0;
    if (vs && ps_object == 0 && generating_shadow) {
      // Depth-only: keep it. Measured at ~206 draws/frame in a level.
      ++g_captureDepthOnly;
    } else {
      ++g_captureSkippedNoShader;
      if (vs_object == 0) {
        ++g_captureSkippedNoVSObject;
      } else if (ps_object == 0) {
        // A COLOUR draw carrying no pixel shader, outside a shadow pass. This
        // is a different failure from a missing translation and needs the
        // opposite fix, but both used to land in the "untranslated" bucket --
        // which reads as "rebuild the shader cache" and sends you to the wrong
        // place. It is exactly the grass_initRender shape described above, so
        // it is counted on its own.
        ++g_captureSkippedNullPS;
      } else {
        // Both objects exist but at least one is absent from the cache. Name
        // the guilty stage and its guest container: the container address is
        // what identifies the shader to dump and rebuild from, and without it
        // the counter says only "something is missing".
        ++g_captureSkippedTranslation;
        static std::set<uint32_t> reported_missing;
        if (reported_missing.size() < 16) {
          const uint32_t culprit = vs ? ps_object : vs_object;
          if (reported_missing.insert(culprit).second) {
            REXLOG_WARN("[kameo-gfx] draw dropped: {} object {:08X} is not in the shader cache "
                        "(vs {:08X} {}, ps {:08X} {}) -- dump it and rebuild",
                        vs ? "pixel shader" : "vertex shader", culprit, vs_object,
                        vs ? "ok" : "MISSING", ps_object, ps ? "ok" : "MISSING");
          }
        }
      }
      return;
    }
  }

  // SetVertexDeclaration stores the declaration at 0x2C90 -- confirmed in the
  // disassembly. (kameo_guest_device.h's kVertexDeclaration 0x3010 is the
  // separate FVF-built slot, not this one.)
  const uint32_t decl = Load32(base, device + 0x2C90);
  if (!decl) {
    ++g_captureSkippedOther;
    return;
  }
  const HostLayout* layout = GetLayout(base, decl);
  if (!layout) {
    ++g_captureSkippedNoLayout;
    return;
  }

  std::vector<uint32_t> expanded;
  if (!ExpandTopology(primitive, indices ? index_count : vertex_count, indices, &expanded)) {
    ++g_captureSkippedOther;
    return;
  }
  if (expanded.empty()) {
    return;
  }

  // Copy only the vertices this draw actually references, and rebase the
  // indices onto that window so the host draw needs no base vertex.
  uint32_t min_vertex = expanded[0];
  uint32_t max_vertex = expanded[0];
  for (uint32_t i : expanded) {
    min_vertex = std::min(min_vertex, i);
    max_vertex = std::max(max_vertex, i);
  }
  (void)vertex_count;
  // Indices are relative to the stream; StartVertex shifts them for the
  // non-indexed path, where `expanded` holds 0..count-1.
  min_vertex += first_vertex;
  max_vertex += first_vertex;
  if (max_vertex - min_vertex + 1 > 1u << 20) {
    ++g_captureSkippedOther;
    return;
  }

  std::lock_guard<std::mutex> lock(g_captureMutex);
  g_guestBase = base;
  DrawFrame& frame = g_capturing;

  DrawCall call;
  call.primitive = primitive;
  call.vs = vs;
  call.ps = ps;
  call.depth_only = (ps == nullptr);
  call.layout = layout;
  call.vertex_stride = layout->dst_stride;

  uint32_t index_base = 0;
  if (deferred_vertex_addr) {
    // Everything except the vertices, which are read at frame end.
    call.deferred_vertex_addr = deferred_vertex_addr;
    call.deferred_vertex_stride = deferred_vertex_stride;
    call.deferred_device = device;
    call.deferred_vertex_count = vertex_count;
    call.vertex_count = vertex_count;
    call.vertex_bytes = vertex_count * layout->dst_stride;
  } else if (!CaptureVertices(base, device, *layout, min_vertex, max_vertex, &frame,
                              &call.vertex_data, &call.vertex_bytes, &index_base,
                              &call.vertex_count, override_stream)) {
    ++g_captureSkippedOther;
    return;
  }

  call.index_count = uint32_t(expanded.size());
  call.index_data = frame.alloc(call.index_count * 4, 4);
  auto* dst = reinterpret_cast<uint32_t*>(frame.at(call.index_data));
  for (uint32_t i = 0; i < call.index_count; ++i) {
    dst[i] = expanded[i] + first_vertex - index_base;
  }

  call.vs_constants = CaptureConstants(base, device + dev::kVertexShaderFloatConstants, &g_vsDedup,
                                       &frame);
  call.ps_constants = CaptureConstants(base, device + dev::kPixelShaderFloatConstants, &g_psDedup,
                                       &frame);
  CaptureState(base, device, &call);
  frame.draws.push_back(call);
}

}  // namespace

void CaptureIndexedDraw(uint8_t* base, uint32_t device, uint32_t primitive, int32_t base_vertex,
                        uint32_t start_index, uint32_t index_count) {
  const uint32_t ib = Load32(base, device + dev::kIndexBuffer);
  if (!ib || index_count == 0) {
    return;
  }
  // Index width is bit 31 of the buffer's Common word: DrawIndexedVertices
  // tests exactly that, taking StartIndex*4 when it is set and StartIndex*2
  // when it is not.
  const bool index32 = (Load32(base, ib) & 0x80000000u) != 0;
  const uint32_t address = Load32(base, ib + 12) & 0xFFFFFFFCu;
  if (!address) {
    return;
  }

  static thread_local std::vector<uint32_t> indices;
  indices.resize(index_count);
  const uint32_t element = index32 ? 4 : 2;
  const uint8_t* src = GuestData(base, address + start_index * element);
  for (uint32_t i = 0; i < index_count; ++i) {
    if (index32) {
      uint32_t value;
      std::memcpy(&value, src + 4 * i, 4);
      indices[i] = Swap32(value);
    } else {
      uint16_t value;
      std::memcpy(&value, src + 2 * i, 2);
      indices[i] = Swap16(value);
    }
  }

  // BaseVertexIndex is added to every index by the GPU; folding it in here
  // keeps the host draw's base vertex at zero.
  if (base_vertex != 0) {
    for (uint32_t& i : indices) {
      i = uint32_t(int32_t(i) + base_vertex);
    }
  }

  CaptureCommon(base, device, primitive, 0, 0, indices.data(), index_count);
}

void CaptureDraw(uint8_t* base, uint32_t device, uint32_t primitive, uint32_t start_vertex,
                 uint32_t vertex_count) {
  if (vertex_count == 0) {
    return;
  }
  CaptureCommon(base, device, primitive, start_vertex, vertex_count, nullptr, 0);
}

void CaptureResolve(uint8_t* base, uint32_t device, uint32_t flags, uint32_t source_rect,
                    uint32_t dest_texture) {
  if (!dest_texture) {
    return;
  }
  // Flags & 7 selects the source: 0..3 are colour targets, 4 is depth. Only the
  // colour path is reproduced here -- a depth resolve would have to come out of
  // the depth attachment in a sampleable format, which is its own piece of work.
  const uint32_t source = flags & 7;

  // Every resolve the guest issues, counted by (source, destination address).
  // RenderDoc proved the 768x768 and 1024x1024 shadow maps receive 94 draws
  // each yet are never copied out, so the question is whether the guest asks
  // for those resolves at all -- and if it does, whether we reject them before
  // the depth path. Counting by `flags & 7` also shows whether a shadow map is
  // resolved as DEPTH (4) or as COLOUR, which would send it down the wrong
  // path entirely.
  {
    static std::map<uint64_t, uint32_t> seen;
    static uint32_t ticks = 0;
    const uint32_t dest_addr = dest_texture ? Load32(base, dest_texture + 20) & 0xFFFFF000u : 0;
    seen[(uint64_t(source) << 32) | dest_addr]++;
    if (++ticks % 600 == 0) {
      std::string all;
      for (const auto& [k, n] : seen) {
        all += fmt::format("src{}@{:08X}x{} ", uint32_t(k >> 32), uint32_t(k), n);
      }
      REXLOG_INFO("[kameo-gfx] guest resolves so far: {}", all);
    }
  }

  DrawCall call;
  call.kind = CommandKind::Resolve;
  // WHICH SURFACE this resolve reads from -- the device's render target at the
  // moment of the call. Without it the submission loop has to infer the source
  // from whatever the previous DRAW happened to set, which is wrong whenever
  // the page pass never reached submission: the copy then takes the swap chain
  // and the storybook pages come out as the purple background behind them.
  // Measured in run 099, where only the 480x640 page got an offscreen target
  // and the 640x480 / 240x320 pages were blitted from the frame buffer.
  call.rt_surface = Load32(base, device + dev::kRenderTarget0);
  call.resolve_depth = (source == 4);
  call.resolve_address = Load32(base, dest_texture + 20) & 0xFFFFF000u;
  if (!call.resolve_address) {
    return;
  }

  // MipFlush carries the destination's dimensions, in the same two layouts
  // D3DDevice_Resolve itself picks between on Format.dword[2] bit 10.
  const uint32_t mip_flush = Load32(base, dest_texture + 24);
  const uint32_t format2 = Load32(base, dest_texture + 36);
  if ((format2 & 0x600) == 0x400) {
    call.resolve_width = (mip_flush & 0x7FF) + 1;
    call.resolve_height = ((mip_flush >> 11) & 0x7FF) + 1;
  } else {
    call.resolve_width = (mip_flush & 0x1FFF) + 1;
    call.resolve_height = ((mip_flush >> 13) & 0x1FFF) + 1;
  }

  if (source_rect) {
    call.resolve_x = int32_t(Load32(base, source_rect + 0));
    call.resolve_y = int32_t(Load32(base, source_rect + 4));
    const int32_t x2 = int32_t(Load32(base, source_rect + 8));
    const int32_t y2 = int32_t(Load32(base, source_rect + 12));
    if (x2 > call.resolve_x && y2 > call.resolve_y) {
      call.resolve_width = uint32_t(x2 - call.resolve_x);
      call.resolve_height = uint32_t(y2 - call.resolve_y);
    }
  }
  if (call.resolve_width == 0 || call.resolve_height == 0) {
    return;
  }

  std::lock_guard<std::mutex> lock(g_captureMutex);
  g_guestBase = base;
  g_capturing.draws.push_back(call);
}

void CaptureBinkMarker() {
  // The video is a draw like any other as far as ORDER is concerned. Drawing it
  // before the scene unconditionally put it underneath geometry the guest had
  // already covered it with -- most visibly the full-screen fade quad, which is
  // opaque black and is meant to be painted over.
  DrawCall call;
  call.kind = CommandKind::Bink;
  std::lock_guard<std::mutex> lock(g_captureMutex);
  g_capturing.draws.push_back(call);
}

// D3DDevice_BeginVertices hands the guest a pointer into a ring and returns;
// the vertices appear only afterwards, and the inlined EndVertices is not
// hookable. So the state is captured here, where it is still this draw's, and
// the geometry is picked up at frame end. Roughly 250 of these a frame were
// being dropped outright -- the whole 2D icon system: health and energy bars,
// the warrior button prompts, boss bars.
void CaptureDrawBeginVertices(uint8_t* base, uint32_t device, uint32_t primitive,
                              uint32_t vertex_count, uint32_t ring, uint32_t stride) {
  if (!ring || !stride || !vertex_count) {
    return;
  }
  CaptureCommon(base, device, primitive, 0, vertex_count, nullptr, 0, {}, ring, stride);
}

void CaptureDrawUP(uint8_t* base, uint32_t device, uint32_t primitive, uint32_t vertex_count,
                   uint32_t data, uint32_t stride) {
  if (!data || !stride || !vertex_count) {
    return;
  }
  StreamOverride override_stream;
  override_stream.data = GuestData(base, data);
  override_stream.stride = stride;
  override_stream.count = vertex_count;
  CaptureCommon(base, device, primitive, 0, vertex_count, nullptr, 0, override_stream);
}

void SubmitCapturedFrame() {
  std::lock_guard<std::mutex> lock(g_captureMutex);
  // The guest has finished writing every BeginVertices ring by now, so the
  // geometry those draws deferred can finally be transcoded. Done before the
  // frame is handed to the render thread, so the arena stays self-contained.
  if (g_guestBase != nullptr) {
    for (DrawCall& call : g_capturing.draws) {
      if (!call.deferred_vertex_addr || !call.layout) {
        continue;
      }
      StreamOverride ring;
      ring.data = GuestData(g_guestBase, call.deferred_vertex_addr);
      ring.stride = call.deferred_vertex_stride;
      ring.count = call.deferred_vertex_count;
      uint32_t index_base = 0;
      uint32_t count = 0;
      if (!CaptureVertices(g_guestBase, call.deferred_device, *call.layout, 0,
                           call.deferred_vertex_count - 1,
                           &g_capturing, &call.vertex_data, &call.vertex_bytes, &index_base,
                           &count, ring)) {
        call.vertex_data = kNoData;
      }
    }
  }
  // Drop anything the guest has not drawn for a few seconds. Without this the
  // cache keeps every buffer the session has ever seen, and streaming a large
  // level would grow it without limit.
  ++g_captureFrame;
  if ((g_captureFrame % 240) == 0) {
    size_t live = 0;
    for (auto it = g_transcodeCache.begin(); it != g_transcodeCache.end();) {
      if (g_captureFrame - it->second.last_frame > 240) {
        it = g_transcodeCache.erase(it);
      } else {
        live += it->second.bytes.size();
        ++it;
      }
    }
    g_transcodeCacheBytes = live;
  }
  g_ready = std::move(g_capturing);
  g_readyValid = true;
  g_capturing.reset();
  g_vsDedup.reset();
  g_psDedup.reset();
  g_vertexReuse.clear();
}

}  // namespace kameo::gfx

// ---------------------------------------------------------------------------
// Submission
// ---------------------------------------------------------------------------

namespace kameo::gfx {
namespace {

// -- upload rings ------------------------------------------------------------
//
// PresentClear waits for the GPU at the end of every frame, so each ring can
// simply restart at zero next frame; nothing in flight can still be reading it.

struct UploadRing {
  std::unique_ptr<plume::RenderBuffer> buffer;
  uint8_t* mapped = nullptr;
  uint32_t capacity = 0;
  uint32_t used = 0;
  uint32_t exhausted = 0;   // draws dropped this frame for want of room
  uint32_t peak = 0;        // high-water mark, so the sizes can be tuned
  const char* name = "";
  // Arena offset -> ring offset, so a block the capture already deduplicated is
  // uploaded once instead of once per draw. Without this a frame re-uploaded
  // the same 4 KB constant blocks thousands of times and ran the ring dry.
  std::unordered_map<uint32_t, uint32_t> uploaded;

  bool ensure(plume::RenderDevice* device, uint32_t bytes, plume::RenderBufferFlags flags) {
    if (buffer && capacity >= bytes) {
      return true;
    }
    unmap();
    buffer.reset();
    capacity = std::max(bytes, capacity ? capacity * 2 : 4u << 20);
    buffer = device->createBuffer(plume::RenderBufferDesc::UploadBuffer(capacity, flags));
    return buffer != nullptr;
  }

  uint32_t map_failures = 0;  // frames this ring could not be mapped at all

  void begin() {
    peak = std::max(peak, used);
    used = 0;
    exhausted = 0;
    uploaded.clear();
    if (buffer && !mapped) {
      mapped = static_cast<uint8_t*>(buffer->map());
      if (!mapped) {
        // Every write this frame will now fail, and `write` cannot tell the
        // caller why: it returns kNoData for "full" and for "not mapped"
        // alike. That produced frames submitting ZERO draws with the ring
        // reporting 0 KB used and no exhaustion -- a frozen picture with the
        // renderer still happily presenting. Say so instead of guessing.
        ++map_failures;
        REXLOG_ERROR("[kameo-gfx] {} ring FAILED TO MAP ({} KB); every draw this frame will be "
                     "dropped for want of space",
                     name, capacity >> 10);
      }
    }
  }

  // Uploads a block identified by its arena offset, reusing the ring copy if
  // this block already went up this frame.
  uint32_t writeOnce(uint32_t arena_offset, const void* data, uint32_t bytes, uint32_t align) {
    auto found = uploaded.find(arena_offset);
    if (found != uploaded.end()) {
      return found->second;
    }
    const uint32_t offset = write(data, bytes, align);
    if (offset != kNoData) {
      uploaded.emplace(arena_offset, offset);
    }
    return offset;
  }

  void unmap() {
    if (buffer && mapped) {
      buffer->unmap();
      mapped = nullptr;
    }
  }

  // Returns kNoData when the ring is full; the caller drops the draw rather
  // than growing mid-frame, which would invalidate offsets already recorded.
  uint32_t write(const void* data, uint32_t bytes, uint32_t align) {
    if (!mapped) {
      return kNoData;  // not mapped -- see begin(), which logs it
    }
    const uint32_t offset = (used + align - 1) & ~(align - 1);
    if (offset + bytes > capacity) {
      ++exhausted;
      return kNoData;
    }
    std::memcpy(mapped + offset, data, bytes);
    used = offset + bytes;
    return offset;
  }
};

UploadRing g_vertexRing;
UploadRing g_indexRing;
UploadRing g_constantRing;

// -- texture cache -----------------------------------------------------------

struct CachedTexture {
  std::unique_ptr<plume::RenderTexture> texture;
  std::unique_ptr<plume::RenderTextureView> view;
  std::unique_ptr<plume::RenderBuffer> upload;
  uint32_t heap_index = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  // Resolve targets are keyed on guest ADDRESS alone, and the guest uses one
  // address for BOTH a colour resolve and a depth resolve -- the census caught
  // F018F000 arriving as src0 (colour) 14 times and src4 (depth) 87 times. The
  // colour path makes it B8G8R8A8 and the depth path copies R32_FLOAT into it,
  // which is an incompatible-format copy: DXGI_ERROR_INVALID_CALL, device
  // removed, freeze. Size alone is not enough to tell them apart.
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
};

struct CachedSampler {
  std::unique_ptr<plume::RenderSampler> sampler;
  uint32_t heap_index = 0;
};

std::unordered_map<uint64_t, CachedTexture> g_textures;
// Guest address -> host texture the guest resolved into. Checked BEFORE guest
// memory: once a target has been resolved, the rendered result lives here and
// the guest-side bytes are stale (all zeros, in practice, which is exactly the
// flat black the scene was drawing).
// Keyed on address AND SHAPE, not address alone. The guest reuses one address
// for completely different surfaces, and keying on the address by itself has
// now caused five separate bugs: wrong content (the purple storybook pages),
// wrong size (a freeze), wrong format (colour vs depth at F018F000), stale
// entries, and finally EFDEC000 thrashing between 1280x720 and a parade of
// small sizes many times a frame. Each of the first four was patched where it
// surfaced; this is the class fix the earlier ones kept pointing at.
//
// The rebuild-on-resize patch that preceded this is what made the last one
// fatal rather than merely wrong: it destroyed and recreated the texture behind
// a heap slot mid-frame, so draws that had already sampled that slot earlier in
// the same frame were left referring to a resource that no longer existed, and
// D3D12 answered with DXGI_ERROR_INVALID_CALL and removed the device -- which
// presents as the picture freezing while the renderer keeps on presenting.
//
// With shape in the key, incompatible uses simply cannot collide, so nothing is
// ever rebuilt in place and no descriptor is pulled out from under a draw.
struct ResolveKey {
  uint32_t address = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  bool depth = false;

  bool operator==(const ResolveKey& o) const {
    return address == o.address && width == o.width && height == o.height && depth == o.depth;
  }
};

struct ResolveKeyHash {
  size_t operator()(const ResolveKey& k) const {
    uint64_t h = uint64_t(k.address) * 1099511628211ull;
    h ^= (uint64_t(k.width) << 32) | (uint64_t(k.height) << 48) | (k.depth ? 1u : 0u);
    h *= 1099511628211ull;
    return size_t(h ^ (h >> 29));
  }
};

std::unordered_map<ResolveKey, CachedTexture, ResolveKeyHash> g_resolveTargets;
std::unordered_map<uint64_t, CachedSampler> g_samplers;
std::unordered_map<uint64_t, std::unique_ptr<plume::RenderPipeline>> g_pipelines;
// Must match EnsureDrawPathResources' kMaxTextures / kMaxSamplers.
constexpr uint32_t kMaxBindlessTextures = 4096;
constexpr uint32_t kMaxBindlessSamplers = 256;
uint32_t g_nextTextureSlot = 1;  // slot 0 is the fallback texture
uint32_t g_nextSamplerSlot = 0;
bool g_heapExhausted = false;
std::vector<uint8_t> g_untileScratch;

struct HostTextureFormat {
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  uint32_t block = 1;       // texels per block edge
  uint32_t block_bytes = 0;  // bytes per block (or per texel when block == 1)
  bool expand_to_rgba8 = false;
};

HostTextureFormat MapTextureFormat(uint32_t xenos_format) {
  HostTextureFormat f;
  switch (xenos_format) {
    case kXenosFormat_8:
      f.format = plume::RenderFormat::R8_UNORM;
      f.block_bytes = 1;
      break;
    case kXenosFormat_8_8:
      f.format = plume::RenderFormat::R8G8_UNORM;
      f.block_bytes = 2;
      break;
    case kXenosFormat_8_8_8_8:
    case kXenosFormat_8_8_8_8_AS_16_16_16_16:
      f.format = plume::RenderFormat::R8G8B8A8_UNORM;
      f.block_bytes = 4;
      break;
    case kXenosFormat_1_5_5_5:
    case kXenosFormat_5_6_5:
    case kXenosFormat_4_4_4_4:
      // Expanded on the CPU: the DXGI equivalents are either missing or have
      // the channels in a different order, and these are rare enough that a
      // conversion costs nothing measurable.
      f.format = plume::RenderFormat::R8G8B8A8_UNORM;
      f.block_bytes = 2;
      f.expand_to_rgba8 = true;
      break;
    case kXenosFormat_2_10_10_10:
    case kXenosFormat_2_10_10_10_AS_16_16_16_16:
      f.format = plume::RenderFormat::R8G8B8A8_UNORM;
      f.block_bytes = 4;
      f.expand_to_rgba8 = true;
      break;
    case kXenosFormat_16_16_16_16_FLOAT:
      f.format = plume::RenderFormat::R16G16B16A16_FLOAT;
      f.block_bytes = 8;
      break;
    case kXenosFormat_16_16_FLOAT:
      f.format = plume::RenderFormat::R16G16_FLOAT;
      f.block_bytes = 4;
      break;
    case kXenosFormat_DXT1:
    case kXenosFormat_DXT1_AS_16_16_16_16:
      f.format = plume::RenderFormat::BC1_UNORM;
      f.block = 4;
      f.block_bytes = 8;
      break;
    case kXenosFormat_DXT2_3:
    case kXenosFormat_DXT2_3_AS_16_16_16_16:
      f.format = plume::RenderFormat::BC2_UNORM;
      f.block = 4;
      f.block_bytes = 16;
      break;
    case kXenosFormat_DXT4_5:
    case kXenosFormat_DXT4_5_AS_16_16_16_16:
      f.format = plume::RenderFormat::BC3_UNORM;
      f.block = 4;
      f.block_bytes = 16;
      break;
    case kXenosFormat_DXT5A:
      f.format = plume::RenderFormat::BC4_UNORM;
      f.block = 4;
      f.block_bytes = 8;
      break;
    case kXenosFormat_DXN:
      f.format = plume::RenderFormat::BC5_UNORM;
      f.block = 4;
      f.block_bytes = 16;
      break;
    default:
      break;
  }
  return f;
}

// Xbox 360 2D tiling, the standard XGAddress2DTiledOffset swizzle. Returns a
// BLOCK index: for compressed formats the caller works in 4x4 blocks and passes
// the block pitch, exactly as the console stores them.
uint32_t TiledOffset2D(uint32_t x, uint32_t y, uint32_t width, uint32_t log2_bpp) {
  const uint32_t aligned_width = (width + 31u) & ~31u;
  const uint32_t macro = ((x >> 5) + (y >> 5) * (aligned_width >> 5)) << (log2_bpp + 7);
  const uint32_t micro = ((x & 7u) + ((y & 6u) << 2)) << log2_bpp;
  const uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15u) +
                          ((y & 8u) << (3 + log2_bpp)) + ((y & 1u) << 4);
  return ((((offset & ~511u) << 3) + ((offset & 448u) << 2) + (offset & 63u) +
           ((y & 16u) << 7) + (((((y & 8u) >> 2) + (x >> 3)) & 3u) << 6)) >>
          log2_bpp);
}

uint32_t Log2(uint32_t value) {
  uint32_t result = 0;
  while ((1u << result) < value) {
    ++result;
  }
  return result;
}

// Undoes the fetch constant's endian field on a block of pixel data.
void ApplyEndian(uint8_t* data, size_t bytes, uint32_t endian) {
  switch (endian) {
    case 1: {  // 8in16
      for (size_t i = 0; i + 1 < bytes; i += 2) {
        std::swap(data[i], data[i + 1]);
      }
      break;
    }
    case 2: {  // 8in32
      for (size_t i = 0; i + 3 < bytes; i += 4) {
        std::swap(data[i], data[i + 3]);
        std::swap(data[i + 1], data[i + 2]);
      }
      break;
    }
    case 3: {  // 16in32
      for (size_t i = 0; i + 3 < bytes; i += 4) {
        std::swap(data[i], data[i + 2]);
        std::swap(data[i + 1], data[i + 3]);
      }
      break;
    }
    default:
      break;
  }
}

void ExpandPackedToRgba8(const uint8_t* src, uint8_t* dst, uint32_t texels, uint32_t format,
                         uint32_t src_bytes) {
  for (uint32_t i = 0; i < texels; ++i) {
    uint32_t value = 0;
    std::memcpy(&value, src + src_bytes * i, src_bytes);
    uint8_t r = 0, g = 0, b = 0, a = 255;
    switch (format) {
      case kXenosFormat_2_10_10_10:
      case kXenosFormat_2_10_10_10_AS_16_16_16_16:
        r = uint8_t(((value >> 20) & 0x3FF) >> 2);
        g = uint8_t(((value >> 10) & 0x3FF) >> 2);
        b = uint8_t((value & 0x3FF) >> 2);
        a = uint8_t(((value >> 30) & 0x3) * 85);
        break;
      case kXenosFormat_5_6_5:
        r = uint8_t(((value >> 11) & 0x1F) * 255 / 31);
        g = uint8_t(((value >> 5) & 0x3F) * 255 / 63);
        b = uint8_t((value & 0x1F) * 255 / 31);
        break;
      case kXenosFormat_1_5_5_5:
        a = (value & 0x8000) ? 255 : 0;
        r = uint8_t(((value >> 10) & 0x1F) * 255 / 31);
        g = uint8_t(((value >> 5) & 0x1F) * 255 / 31);
        b = uint8_t((value & 0x1F) * 255 / 31);
        break;
      case kXenosFormat_4_4_4_4:
        a = uint8_t(((value >> 12) & 0xF) * 17);
        r = uint8_t(((value >> 8) & 0xF) * 17);
        g = uint8_t(((value >> 4) & 0xF) * 17);
        b = uint8_t((value & 0xF) * 17);
        break;
      default:
        break;
    }
    dst[4 * i + 0] = r;
    dst[4 * i + 1] = g;
    dst[4 * i + 2] = b;
    dst[4 * i + 3] = a;
  }
}

uint32_t AlignRow(uint32_t bytes) { return (bytes + 255u) & ~255u; }

// The fetch constant's swizzle says where each output component comes from:
// four 3-bit selectors, 0..3 pick a source channel, 4 is constant 0, 5 is
// constant 1. Ignoring it is why a k_8 font atlas drew black text -- one stored
// channel that the swizzle fans out to the components the shader multiplies by,
// mapped to R8_UNORM instead, whose .gba read as 0,0,1.
constexpr uint32_t kSwizzleIdentity = 0u | (1u << 3) | (2u << 6) | (3u << 9);

// Expands an uncompressed 8-bit-per-channel surface to RGBA8 through the
// swizzle. `channels` is how many the source format actually stores.
void ApplySwizzleToRgba8(const uint8_t* src, uint8_t* dst, size_t texels, uint32_t channels,
                         uint32_t swizzle) {
  uint32_t select[4];
  for (uint32_t i = 0; i < 4; ++i) {
    select[i] = (swizzle >> (3 * i)) & 0x7;
  }
  for (size_t t = 0; t < texels; ++t) {
    const uint8_t* in = src + t * channels;
    uint8_t* out = dst + t * 4;
    for (uint32_t i = 0; i < 4; ++i) {
      switch (select[i]) {
        case 0: case 1: case 2: case 3:
          out[i] = (select[i] < channels) ? in[select[i]] : 0;
          break;
        case 4: out[i] = 0x00; break;
        case 5: out[i] = 0xFF; break;
        default: out[i] = 0x00; break;
      }
    }
  }
}


// --- BC1/BC2/BC3 decode, for the texture DUMP only ---------------------------
// The upload path hands compressed blocks straight to the GPU, which is right --
// but it also means the dump never saw them, and the census says DXT1 is ~90%
// of the textures this game uses. Decoding here (and only here) is what makes
// the sprites, foliage and inventory art actually inspectable.
void DecodeBcBlockColour(const uint8_t* src, uint8_t* out_rgba, uint32_t out_pitch, uint32_t bx,
                         uint32_t by, uint32_t width, uint32_t height, bool one_bit_alpha) {
  const uint16_t c0 = uint16_t(src[0] | (src[1] << 8));
  const uint16_t c1 = uint16_t(src[2] | (src[3] << 8));
  auto unpack = [](uint16_t c, uint8_t* rgb) {
    rgb[0] = uint8_t(((c >> 11) & 0x1F) * 255 / 31);
    rgb[1] = uint8_t(((c >> 5) & 0x3F) * 255 / 63);
    rgb[2] = uint8_t((c & 0x1F) * 255 / 31);
  };
  uint8_t palette[4][3];
  unpack(c0, palette[0]);
  unpack(c1, palette[1]);
  const bool punchthrough = one_bit_alpha && c0 <= c1;
  for (int i = 0; i < 3; ++i) {
    if (punchthrough) {
      palette[2][i] = uint8_t((int(palette[0][i]) + palette[1][i]) / 2);
      palette[3][i] = 0;
    } else {
      palette[2][i] = uint8_t((2 * int(palette[0][i]) + palette[1][i]) / 3);
      palette[3][i] = uint8_t((int(palette[0][i]) + 2 * int(palette[1][i])) / 3);
    }
  }
  const uint32_t bits = uint32_t(src[4]) | (uint32_t(src[5]) << 8) | (uint32_t(src[6]) << 16) |
                        (uint32_t(src[7]) << 24);
  for (uint32_t y = 0; y < 4; ++y) {
    for (uint32_t x = 0; x < 4; ++x) {
      const uint32_t px = bx * 4 + x;
      const uint32_t py = by * 4 + y;
      if (px >= width || py >= height) {
        continue;
      }
      const uint32_t index = (bits >> (2 * (y * 4 + x))) & 3;
      uint8_t* dst = out_rgba + size_t(py) * out_pitch + size_t(px) * 4;
      dst[0] = palette[index][0];
      dst[1] = palette[index][1];
      dst[2] = palette[index][2];
      if (punchthrough && index == 3) {
        dst[3] = 0;
      } else if (one_bit_alpha) {
        dst[3] = 255;
      }
    }
  }
}

void DecodeBcToRgba(const uint8_t* src, uint32_t src_pitch, uint8_t* out_rgba, uint32_t out_pitch,
                    uint32_t width, uint32_t height, uint32_t xenos_format) {
  const bool bc1 = (xenos_format == kXenosFormat_DXT1 ||
                    xenos_format == kXenosFormat_DXT1_AS_16_16_16_16);
  const bool bc2 = (xenos_format == kXenosFormat_DXT2_3 ||
                    xenos_format == kXenosFormat_DXT2_3_AS_16_16_16_16);
  const uint32_t block_bytes = bc1 ? 8u : 16u;
  const uint32_t blocks_x = (width + 3) / 4;
  const uint32_t blocks_y = (height + 3) / 4;
  for (uint32_t by = 0; by < blocks_y; ++by) {
    for (uint32_t bx = 0; bx < blocks_x; ++bx) {
      const uint8_t* block = src + size_t(by) * src_pitch + size_t(bx) * block_bytes;
      const uint8_t* colour = bc1 ? block : block + 8;
      DecodeBcBlockColour(colour, out_rgba, out_pitch, bx, by, width, height, bc1);
      if (bc2) {
        for (uint32_t y = 0; y < 4; ++y) {
          for (uint32_t x = 0; x < 4; ++x) {
            const uint32_t px = bx * 4 + x, py = by * 4 + y;
            if (px >= width || py >= height) continue;
            const uint8_t nibble = block[y * 2 + x / 2];
            const uint8_t a = (x & 1) ? (nibble >> 4) : (nibble & 0xF);
            out_rgba[size_t(py) * out_pitch + size_t(px) * 4 + 3] = uint8_t(a * 17);
          }
        }
      } else if (!bc1) {  // BC3: 3-bit interpolated alpha
        const uint8_t a0 = block[0], a1 = block[1];
        uint8_t alpha[8] = {a0, a1};
        if (a0 > a1) {
          for (int i = 0; i < 6; ++i)
            alpha[2 + i] = uint8_t(((6 - i) * a0 + (1 + i) * a1) / 7);
        } else {
          for (int i = 0; i < 4; ++i)
            alpha[2 + i] = uint8_t(((4 - i) * a0 + (1 + i) * a1) / 5);
          alpha[6] = 0;
          alpha[7] = 255;
        }
        uint64_t abits = 0;
        for (int i = 0; i < 6; ++i) abits |= uint64_t(block[2 + i]) << (8 * i);
        for (uint32_t y = 0; y < 4; ++y) {
          for (uint32_t x = 0; x < 4; ++x) {
            const uint32_t px = bx * 4 + x, py = by * 4 + y;
            if (px >= width || py >= height) continue;
            const uint32_t idx = uint32_t((abits >> (3 * (y * 4 + x))) & 7);
            out_rgba[size_t(py) * out_pitch + size_t(px) * 4 + 3] = alpha[idx];
          }
        }
      }
    }
  }
}

uint32_t g_textureFailures = 0;

// Why textures fail to resolve, counted so a black surface can be attributed
// rather than guessed at.
struct TextureMissReasons {
  uint32_t no_object = 0;
  uint32_t no_address = 0;
  uint32_t bad_format = 0;
  uint32_t create_failed = 0;
  void reset() { *this = {}; }
  uint32_t total() const { return no_object + no_address + bad_format + create_failed; }
};
TextureMissReasons g_textureMiss;
std::map<uint32_t, uint32_t> g_unsupportedFormats;  // xenos format -> lookups
std::map<uint32_t, uint32_t> g_formatCensus;       // xenos format -> distinct textures created
std::set<uint64_t> g_unsupportedTextures;           // distinct address+format
uint32_t g_emptyTextures = 0;                      // uploaded but all-zero

// Heap slot 0 is a 1x1 opaque white texel, bound wherever a draw's texture
// could not be resolved.
//
// This is a diagnostic as much as a fallback: an unresolved slot used to leave
// the shader indexing an EMPTY heap entry, which samples as zero -- and zero
// through a modulate is black, with an alpha of zero that the alpha test does
// not clip (clip() only discards on a negative argument). So every missing
// texture painted a flat black polygon that looked like a shading bug. White
// makes the same failure look like a missing texture, which is what it is.
std::unique_ptr<plume::RenderTexture> g_fallbackTexture;
std::unique_ptr<plume::RenderTextureView> g_fallbackView;
std::unique_ptr<plume::RenderBuffer> g_fallbackUpload;

void EnsureFallbackTexture(KameoGraphicsSystem::DrawPathContext& ctx,
                           plume::RenderCommandList* list) {
  if (g_fallbackTexture) {
    return;
  }
  g_fallbackTexture = ctx.device->createTexture(
      plume::RenderTextureDesc::Texture2D(1, 1, 1, plume::RenderFormat::R8G8B8A8_UNORM));
  if (!g_fallbackTexture) {
    return;
  }
  g_fallbackView = g_fallbackTexture->createTextureView(
      plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R8G8B8A8_UNORM));
  g_fallbackUpload = ctx.device->createBuffer(plume::RenderBufferDesc::UploadBuffer(256));
  if (!g_fallbackUpload) {
    return;
  }
  if (auto* mapped = static_cast<uint8_t*>(g_fallbackUpload->map())) {
    // MAGENTA, not white. White is a plausible-looking result, so a surface
    // sampling it is still a judgement call; magenta cannot be mistaken for
    // anything the game would draw, which turns "is this texture missing?"
    // into a yes/no question answerable from one screenshot.
    mapped[0] = 0xFF;  // R
    mapped[1] = 0x00;  // G
    mapped[2] = 0xFF;  // B
    mapped[3] = 0xFF;  // A
    g_fallbackUpload->unmap();
  }
  list->barriers(plume::RenderBarrierStage::COPY,
                 plume::RenderTextureBarrier(g_fallbackTexture.get(),
                                             plume::RenderTextureLayout::COPY_DEST));
  auto src = plume::RenderTextureCopyLocation::PlacedFootprint(
      g_fallbackUpload.get(), plume::RenderFormat::R8G8B8A8_UNORM, 1, 1, 1, 1, 0);
  auto dst = plume::RenderTextureCopyLocation::Subresource(g_fallbackTexture.get(), 0, 0);
  list->copyTextureRegion(dst, src, 0, 0, 0, nullptr);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(g_fallbackTexture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  ctx.texture_set->setTexture(0, g_fallbackTexture.get(), plume::RenderTextureLayout::SHADER_READ,
                              g_fallbackView.get());
  REXLOG_INFO("[kameo-gfx] fallback magenta texture bound at heap slot 0");
}

// Untiles, converts and uploads one guest texture, returning its bindless heap
// index. Cached on everything that describes the pixels; the sampler state that
// shares the fetch constant is deliberately NOT part of the key.
uint32_t EnsureTexture(uint8_t* base, KameoGraphicsSystem::DrawPathContext& ctx,
                       plume::RenderCommandList* list, const GuestTextureFetch& fetch,
                       uint32_t object) {
  if (!object || !fetch.valid()) {
    ++g_textureMiss.no_object;
    return 0;
  }
  // The fetch constant's address has lost its high bits (SetTexture masks them
  // when it builds the GPU address), so the guest-memory address comes from the
  // texture object's own BaseFlush instead.
  const uint32_t address = Load32(base, object + 20) & 0xFFFFF000u;
  if (!address) {
    ++g_textureMiss.no_address;
    return 0;
  }

  // A resolved target is only the right answer for a texture of the SAME shape.
  // Keying on the address alone aliases: the guest reuses that memory for
  // ordinary textures, and handing those a copy of the framebuffer replaced the
  // storybook's parchment pages with the purple background behind them.
  // The shape is part of the key, so a hit is already the right shape -- there
  // is no post-check to get wrong. Colour is tried first, then the depth copy.
  ResolveKey want{address, uint16_t(fetch.width()), uint16_t(fetch.height()), false};
  auto resolved = g_resolveTargets.find(want);
  if (resolved == g_resolveTargets.end()) {
    want.depth = true;
    resolved = g_resolveTargets.find(want);
  }
  if (resolved != g_resolveTargets.end()) {
    static std::set<uint32_t> reported;
    if (reported.insert(address).second) {
      REXLOG_INFO("[kameo-gfx] draw sampled RESOLVE TARGET {:08X} {}x{} fmt {}", address,
                  fetch.width(), fetch.height(), fetch.format());
    }
    return resolved->second.heap_index;
  }

  const uint32_t width = fetch.width();
  const uint32_t height = fetch.height();
  const uint32_t format = fetch.format();
  // Census of every texture format actually used, compressed ones included.
  // The TGA dump only sees textures converted to RGBA, so DXT never appears
  // there -- and that is most of the game's art. This counts them all, which is
  // what answers "what format are the sprites and the foliage".
  g_formatCensus[format]++;

  const uint64_t key = (uint64_t(address) << 32) ^ (uint64_t(width) << 20) ^
                       (uint64_t(height) << 6) ^ format ^ (fetch.tiled() ? 0x8000000000ull : 0);

  auto found = g_textures.find(key);
  if (found != g_textures.end()) {
    return found->second.heap_index;
  }

  HostTextureFormat host = MapTextureFormat(format);
  if (host.format == plume::RenderFormat::UNKNOWN || width == 0 || height == 0) {
    ++g_textureMiss.bad_format;
    // Count every distinct format rather than logging the first dozen hits: the
    // capped log showed only k_24_8 and hid whatever else is failing.
    g_unsupportedFormats[format]++;
    g_unsupportedTextures.insert(uint64_t(address) << 8 | format);
    return 0;
  }

  const uint32_t blocks_x = (width + host.block - 1) / host.block;
  const uint32_t blocks_y = (height + host.block - 1) / host.block;
  const uint32_t pitch_texels = fetch.pitch() ? fetch.pitch() : width;
  const uint32_t pitch_blocks = std::max(1u, pitch_texels / host.block);
  const uint32_t src_row_bytes = blocks_x * host.block_bytes;

  // Untile (or copy row by row) into scratch, then convert if needed.
  const size_t linear_bytes = size_t(src_row_bytes) * blocks_y;
  g_untileScratch.resize(linear_bytes);
  const uint8_t* src = GuestData(base, address);

  if (fetch.tiled()) {
    const uint32_t log2_bpp = Log2(host.block_bytes);
    for (uint32_t y = 0; y < blocks_y; ++y) {
      uint8_t* dst = g_untileScratch.data() + size_t(y) * src_row_bytes;
      for (uint32_t x = 0; x < blocks_x; ++x) {
        const uint32_t offset = TiledOffset2D(x, y, pitch_blocks, log2_bpp) * host.block_bytes;
        std::memcpy(dst + size_t(x) * host.block_bytes, src + offset, host.block_bytes);
      }
    }
  } else {
    const uint32_t src_pitch_bytes = pitch_blocks * host.block_bytes;
    for (uint32_t y = 0; y < blocks_y; ++y) {
      std::memcpy(g_untileScratch.data() + size_t(y) * src_row_bytes,
                  src + size_t(y) * src_pitch_bytes, src_row_bytes);
    }
  }

  ApplyEndian(g_untileScratch.data(), linear_bytes, fetch.endian());

  // Is this texture actually EMPTY? A surface that samples a resolve target we
  // never wrote gets a texture that resolves perfectly well and contains
  // nothing but zeros -- which draws as flat black and looks exactly like a
  // shading bug. Counting them separates "texture missing" from "texture
  // present but never rendered into", which need completely different fixes.
  {
    bool all_zero = true;
    for (size_t i = 0; i < linear_bytes; ++i) {
      if (g_untileScratch[i] != 0) {
        all_zero = false;
        break;
      }
    }
    if (all_zero) {
      ++g_emptyTextures;
      if (g_emptyTextures <= 12) {
        REXLOG_WARN("[kameo-gfx] texture {:08X} {}x{} format {} is entirely ZERO -- almost "
                    "certainly a resolve target we never rendered into",
                    address, width, height, format);
      }
    }
  }

  std::vector<uint8_t> expanded;
  const uint8_t* pixels = g_untileScratch.data();
  uint32_t dst_row_bytes = src_row_bytes;

  // 8-bit channel formats with a non-identity swizzle become RGBA8 so the
  // swizzle can be baked in; plume has no per-view component mapping.
  const bool byte_channels = host.block == 1 && (format == kXenosFormat_8 ||
                                                 format == kXenosFormat_8_8 ||
                                                 format == kXenosFormat_8_8_8_8 ||
                                                 format == kXenosFormat_8_8_8_8_AS_16_16_16_16);
  if (byte_channels && fetch.swizzle() != kSwizzleIdentity) {
    const uint32_t channels = host.block_bytes;
    expanded.resize(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
      ApplySwizzleToRgba8(g_untileScratch.data() + size_t(y) * src_row_bytes,
                          expanded.data() + size_t(y) * width * 4, width, channels,
                          fetch.swizzle());
    }
    pixels = expanded.data();
    dst_row_bytes = width * 4;
    host.format = plume::RenderFormat::R8G8B8A8_UNORM;
    host.block_bytes = 4;
  } else if (host.expand_to_rgba8) {
    expanded.resize(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
      ExpandPackedToRgba8(g_untileScratch.data() + size_t(y) * src_row_bytes,
                          expanded.data() + size_t(y) * width * 4, width, format,
                          host.block_bytes);
    }
    pixels = expanded.data();
    dst_row_bytes = width * 4;
    // block_bytes MUST follow the format. The data is RGBA8 from here on, and
    // the copy below turns the upload pitch back into TEXELS by dividing by
    // block_bytes -- so leaving it at the packed source's 2 makes the footprint
    // claim twice as many texels per row as were allocated. D3D12 catches that
    // as "PlacedFootprint extends past the end of the buffer" and REMOVES THE
    // DEVICE, which presents as the game freezing mid-play while the renderer
    // carries on presenting the last good frame.
    //
    // It bites every 16-bit packed format (k_5_6_5, k_1_5_5_5, k_4_4_4_4) and
    // always by exactly 2x. It stayed hidden this long because those formats
    // are rare here -- a whole session's texture census contains a single
    // k_5_6_5 -- so it only fired when that one texture was first uploaded,
    // which looks like a crash tied to a point in the game rather than to a
    // format. The 2_10_10_10 case was already correct by luck: its block_bytes
    // is 4 to begin with.
    host.block_bytes = 4;
  }

  // DXN is not BC5, quite. Both pack two BC4 sub-blocks into each 16-byte
  // 4x4 block, but Xenos orders them the opposite way round, so uploading the
  // bytes verbatim as BC5_UNORM gives the shader a normal map with X and Y
  // transposed. A transposed tangent-space normal still has a plausible length,
  // so nothing downstream complains -- it just lights every texel from the
  // wrong direction, which reads as hard, metallic, cel-shaded shading rather
  // than as an obviously broken texture.
  //
  // Swapping the 8-byte halves in place is the whole correction. Done here, on
  // the untiled copy, so the guest's memory is untouched.
  if (format == kXenosFormat_DXN && REXCVAR_GET(kameo_gfx_dxn_swap)) {
    uint8_t* blocks = g_untileScratch.data();
    for (size_t off = 0; off + 16 <= linear_bytes; off += 16) {
      uint8_t tmp[8];
      std::memcpy(tmp, blocks + off, 8);
      std::memcpy(blocks + off, blocks + off + 8, 8);
      std::memcpy(blocks + off + 8, tmp, 8);
    }
  }

  // Texture report / dump. The black inventory slots fire NEITHER the magenta
  // "missing" fallback NOR the all-zero "never rendered" check, so their pixels
  // exist and the fault is in how they are drawn -- most likely alpha. This
  // reports what is actually in the texels (including how much of the alpha is
  // zero) and, with the cvar on, writes a TGA so the image can just be looked
  // at. TGA because it is a dozen lines to write and preserves alpha, which is
  // the whole question here.
  const bool dump_is_bc = (host.format == plume::RenderFormat::BC1_UNORM ||
                          host.format == plume::RenderFormat::BC2_UNORM ||
                          host.format == plume::RenderFormat::BC3_UNORM);
  if (REXCVAR_GET(kameo_gfx_dump_textures) &&
      (host.format == plume::RenderFormat::R8G8B8A8_UNORM || dump_is_bc)) {
    static std::set<uint64_t> dumped;
    if (dumped.insert(uint64_t(address) ^ (uint64_t(width) << 32) ^ (uint64_t(height) << 48))
            .second) {
      // Compressed textures are uploaded as-is, so decode a copy purely for the
      // dump. DXT1 alone is ~90% of this game's textures; without this the dump
      // only ever showed the small uncompressed minority.
      std::vector<uint8_t> decoded;
      const uint8_t* dump_pixels = pixels;
      uint32_t dump_pitch = dst_row_bytes;
      if (dump_is_bc) {
        decoded.assign(size_t(width) * height * 4, 0xFF);
        DecodeBcToRgba(g_untileScratch.data(), src_row_bytes, decoded.data(), width * 4, width,
                       height, format);
        dump_pixels = decoded.data();
        dump_pitch = width * 4;
      }
      uint64_t a_zero = 0, a_full = 0, rgb_zero = 0;
      const size_t texels = size_t(width) * height;
      for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
          const uint8_t* t = dump_pixels + size_t(y) * dump_pitch + size_t(x) * 4;
          if (t[3] == 0) ++a_zero;
          if (t[3] == 255) ++a_full;
          if (t[0] == 0 && t[1] == 0 && t[2] == 0) ++rgb_zero;
        }
      }
      REXLOG_INFO("[kameo-gfx] texdump {:08X} {}x{} xenos-fmt {} swizzle {:06X}: alpha 0 in "
                  "{}% of texels, alpha 255 in {}%, rgb black in {}%",
                  address, width, height, format, fetch.swizzle(),
                  texels ? a_zero * 100 / texels : 0, texels ? a_full * 100 / texels : 0,
                  texels ? rgb_zero * 100 / texels : 0);
      std::error_code ec;
      std::filesystem::create_directories("texdump", ec);
      const std::string name = fmt::format("texdump/{:08X}_{}x{}_fmt{}.tga", address, width,
                                           height, format);
      if (FILE* f = std::fopen(name.c_str(), "wb")) {
        uint8_t header[18] = {};
        header[2] = 2;  // uncompressed true-colour
        header[12] = uint8_t(width & 0xFF);
        header[13] = uint8_t((width >> 8) & 0xFF);
        header[14] = uint8_t(height & 0xFF);
        header[15] = uint8_t((height >> 8) & 0xFF);
        header[16] = 32;    // bits per pixel
        header[17] = 0x28;  // top-down, 8 bits of alpha
        std::fwrite(header, 1, sizeof(header), f);
        std::vector<uint8_t> row(size_t(width) * 4);
        for (uint32_t y = 0; y < height; ++y) {
          const uint8_t* src = dump_pixels + size_t(y) * dump_pitch;
          for (uint32_t x = 0; x < width; ++x) {
            row[x * 4 + 0] = src[x * 4 + 2];  // TGA is BGRA
            row[x * 4 + 1] = src[x * 4 + 1];
            row[x * 4 + 2] = src[x * 4 + 0];
            row[x * 4 + 3] = src[x * 4 + 3];
          }
          std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
      }
    }
  }

  CachedTexture entry;
  entry.texture = ctx.device->createTexture(
      plume::RenderTextureDesc::Texture2D(width, height, 1, host.format));
  if (!entry.texture) {
    ++g_textureMiss.create_failed;
    return 0;
  }
  entry.view =
      entry.texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(host.format));

  const uint32_t upload_pitch = AlignRow(dst_row_bytes);
  const uint32_t upload_bytes = upload_pitch * blocks_y;
  entry.upload = ctx.device->createBuffer(plume::RenderBufferDesc::UploadBuffer(upload_bytes));
  if (!entry.upload) {
    return 0;
  }
  if (auto* mapped = static_cast<uint8_t*>(entry.upload->map())) {
    for (uint32_t y = 0; y < blocks_y; ++y) {
      std::memcpy(mapped + size_t(y) * upload_pitch, pixels + size_t(y) * dst_row_bytes,
                  dst_row_bytes);
    }
    entry.upload->unmap();
  } else {
    return 0;
  }

  list->barriers(plume::RenderBarrierStage::COPY,
                 plume::RenderTextureBarrier(entry.texture.get(),
                                             plume::RenderTextureLayout::COPY_DEST));
  auto src_loc = plume::RenderTextureCopyLocation::PlacedFootprint(
      entry.upload.get(), host.format, width, height, 1, upload_pitch / host.block_bytes * host.block, 0);
  auto dst_loc = plume::RenderTextureCopyLocation::Subresource(entry.texture.get(), 0, 0);
  list->copyTextureRegion(dst_loc, src_loc, 0, 0, 0, nullptr);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(entry.texture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));

  // The bindless heap is a fixed 4096 slots with no eviction, so a long enough
  // session streaming through enough textures walks off the end. Writing past
  // it corrupts other draws' descriptors rather than failing cleanly, so stop
  // at the boundary and fall back to the placeholder.
  if (g_nextTextureSlot >= kMaxBindlessTextures) {
    if (!g_heapExhausted) {
      g_heapExhausted = true;
      REXLOG_ERROR("[kameo-gfx] bindless texture heap exhausted at {} slots; further textures "
                   "fall back to the placeholder. Eviction is needed.",
                   kMaxBindlessTextures);
    }
    return 0;
  }
  entry.heap_index = g_nextTextureSlot++;
  entry.width = width;
  entry.height = height;
  ctx.texture_set->setTexture(entry.heap_index, entry.texture.get(),
                              plume::RenderTextureLayout::SHADER_READ, entry.view.get());

  const uint32_t index = entry.heap_index;
  g_textures.emplace(key, std::move(entry));
  return index;
}

plume::RenderTextureAddressMode AddressMode(uint32_t clamp) {
  switch (clamp) {
    case 0: return plume::RenderTextureAddressMode::WRAP;
    case 1: return plume::RenderTextureAddressMode::MIRROR;
    case 2: return plume::RenderTextureAddressMode::CLAMP;
    case 3: return plume::RenderTextureAddressMode::MIRROR_ONCE;
    default: return plume::RenderTextureAddressMode::CLAMP;
  }
}

uint32_t EnsureSampler(KameoGraphicsSystem::DrawPathContext& ctx, const GuestTextureFetch& fetch) {
  const uint64_t key = (uint64_t(fetch.words[0] >> 10) & 0xFFF) |
                       (uint64_t(fetch.words[3] >> 19) << 12);
  auto found = g_samplers.find(key);
  if (found != g_samplers.end()) {
    return found->second.heap_index;
  }

  plume::RenderSamplerDesc desc;
  // Xenos filter fields: 0 point, 1 linear, 2 "base map" -- treat anything that
  // is not point as linear.
  desc.magFilter = fetch.mag_filter() == 0 ? plume::RenderFilter::NEAREST : plume::RenderFilter::LINEAR;
  desc.minFilter = fetch.min_filter() == 0 ? plume::RenderFilter::NEAREST : plume::RenderFilter::LINEAR;
  desc.mipmapMode = plume::RenderMipmapMode::NEAREST;
  desc.addressU = AddressMode(fetch.clamp_x());
  desc.addressV = AddressMode(fetch.clamp_y());
  desc.addressW = AddressMode(fetch.clamp_z());

  CachedSampler entry;
  entry.sampler = ctx.device->createSampler(desc);
  if (!entry.sampler) {
    return 0;
  }
  if (g_nextSamplerSlot >= kMaxBindlessSamplers) {
    return 0;
  }
  entry.heap_index = g_nextSamplerSlot++;
  ctx.sampler_set->setSampler(entry.heap_index, entry.sampler.get());
  const uint32_t index = entry.heap_index;
  g_samplers.emplace(key, std::move(entry));
  return index;
}

// -- pipeline cache ----------------------------------------------------------

plume::RenderComparisonFunction HostCompare(uint32_t func) {
  switch (func) {
    case kCompareNever: return plume::RenderComparisonFunction::NEVER;
    case kCompareLess: return plume::RenderComparisonFunction::LESS;
    case kCompareEqual: return plume::RenderComparisonFunction::EQUAL;
    case kCompareLessEqual: return plume::RenderComparisonFunction::LESS_EQUAL;
    case kCompareGreater: return plume::RenderComparisonFunction::GREATER;
    case kCompareNotEqual: return plume::RenderComparisonFunction::NOT_EQUAL;
    case kCompareGreaterEqual: return plume::RenderComparisonFunction::GREATER_EQUAL;
    default: return plume::RenderComparisonFunction::ALWAYS;
  }
}

plume::RenderBlend HostBlend(uint32_t factor) {
  switch (factor) {
    case kBlendZero: return plume::RenderBlend::ZERO;
    case kBlendOne: return plume::RenderBlend::ONE;
    case kBlendSrcColor: return plume::RenderBlend::SRC_COLOR;
    case kBlendInvSrcColor: return plume::RenderBlend::INV_SRC_COLOR;
    case kBlendSrcAlpha: return plume::RenderBlend::SRC_ALPHA;
    case kBlendInvSrcAlpha: return plume::RenderBlend::INV_SRC_ALPHA;
    case kBlendDestColor: return plume::RenderBlend::DEST_COLOR;
    case kBlendInvDestColor: return plume::RenderBlend::INV_DEST_COLOR;
    case kBlendDestAlpha: return plume::RenderBlend::DEST_ALPHA;
    case kBlendInvDestAlpha: return plume::RenderBlend::INV_DEST_ALPHA;
    case kBlendConstColor: return plume::RenderBlend::BLEND_FACTOR;
    case kBlendInvConstColor: return plume::RenderBlend::INV_BLEND_FACTOR;
    case kBlendConstAlpha: return plume::RenderBlend::BLEND_FACTOR;
    case kBlendInvConstAlpha: return plume::RenderBlend::INV_BLEND_FACTOR;
    case kBlendSrcAlphaSat: return plume::RenderBlend::SRC_ALPHA_SAT;
    default: return plume::RenderBlend::ONE;
  }
}

// The ALPHA blend slots take a narrower set of factors than the colour ones.
// Xenos lets the alpha fields hold a "colour" enum and uses the alpha channel of
// whatever that names; D3D12 forbids it outright -- the debug layer rejects the
// whole blend state with "DestBlendAlpha[0] is trying to use a D3D11_BLEND value
// (0x3) that manipulates color, which is invalid", and a rejected blend state is
// silent without the debug layer attached. Folding each colour factor onto its
// alpha counterpart is what the hardware already did.
plume::RenderBlend HostBlendAlpha(uint32_t factor) {
  switch (HostBlend(factor)) {
    case plume::RenderBlend::SRC_COLOR: return plume::RenderBlend::SRC_ALPHA;
    case plume::RenderBlend::INV_SRC_COLOR: return plume::RenderBlend::INV_SRC_ALPHA;
    case plume::RenderBlend::DEST_COLOR: return plume::RenderBlend::DEST_ALPHA;
    case plume::RenderBlend::INV_DEST_COLOR: return plume::RenderBlend::INV_DEST_ALPHA;
    default: return HostBlend(factor);
  }
}

plume::RenderBlendOperation HostBlendOp(uint32_t op) {
  switch (op) {
    case kBlendOpAdd: return plume::RenderBlendOperation::ADD;
    case kBlendOpSrcMinusDest: return plume::RenderBlendOperation::SUBTRACT;
    case kBlendOpMin: return plume::RenderBlendOperation::MIN;
    case kBlendOpMax: return plume::RenderBlendOperation::MAX;
    case kBlendOpDestMinusSrc: return plume::RenderBlendOperation::REV_SUBTRACT;
    default: return plume::RenderBlendOperation::ADD;
  }
}

uint32_t g_pipelineFailures = 0;

// See the plume_d3d12.h include note: a returned pipeline is only real if the
// backend object actually holds a PSO.
bool PipelineCreated(plume::RenderPipeline* pipeline) {
#ifdef _WIN32
  if (!pipeline) {
    return false;
  }
  auto* d3d12 = static_cast<plume::D3D12GraphicsPipeline*>(pipeline);
  return d3d12->d3d != nullptr;
#else
  return pipeline != nullptr;
#endif
}

plume::RenderPipeline* EnsurePipeline(KameoGraphicsSystem::DrawPathContext& ctx,
                                      const DrawCall& call, plume::RenderShader* vs,
                                      plume::RenderShader* ps) {
  const plume::RenderPrimitiveTopology topology = HostTopology(call.primitive);

  uint64_t key = 1469598103934665603ull;
  auto mix = [&key](uint64_t v) { key = (key ^ v) * 1099511628211ull; };
  mix(reinterpret_cast<uintptr_t>(vs));
  mix(reinterpret_cast<uintptr_t>(ps));
  mix(call.layout->key);
  mix(uint32_t(topology));
  mix(call.depth_control);
  mix(call.blend_control);
  mix(call.color_control & 0xF);
  mix(call.mode_control & 0x7);
  mix(call.color_mask);
  mix(call.depth_only ? 0x9E3779B97F4A7C15ull : 0ull);

  auto found = g_pipelines.find(key);
  if (found != g_pipelines.end()) {
    return found->second.get();
  }

  std::vector<plume::RenderInputElement> elements;
  elements.reserve(call.layout->elements.size());
  uint32_t location = 0;
  for (const HostElement& e : call.layout->elements) {
    elements.emplace_back(DeclUsageSemantic(e.usage), e.usage_index, location++, e.format, 0,
                          e.dst_offset);
  }
  const plume::RenderInputSlot slot(0, call.layout->dst_stride);

  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = ctx.layout;
  desc.vertexShader = vs;
  desc.pixelShader = ps;
  desc.inputSlots = &slot;
  desc.inputSlotsCount = 1;
  desc.inputElements = elements.data();
  desc.inputElementsCount = uint32_t(elements.size());
  desc.primitiveTopology = topology;
  if (call.depth_only) {
    // No pixel shader and no colour attachment. plume's D3D12 backend already
    // null-guards psoDesc.PS and accepts colorAttachmentsCount == 0, so this
    // needs no backend change -- the PSO simply has to agree with the
    // framebuffer it will be used against, which is depth-only too.
    desc.renderTargetCount = 0;
  } else {
    desc.renderTargetFormat[0] = plume::RenderFormat(ctx.color_format);
    desc.renderTargetCount = 1;
  }
  desc.depthTargetFormat = plume::RenderFormat(ctx.depth_format);

  desc.depthEnabled = (call.depth_control & state::kDepthTestEnableBit) != 0;
  desc.depthWriteEnabled = (call.depth_control & state::kDepthWriteEnableBit) != 0;
  desc.depthFunction =
      HostCompare((call.depth_control >> state::kDepthFuncShift) & state::kDepthFuncMask);

  // PA_SU_SC_MODE_CNTL: bit 0 culls front faces, bit 1 culls back faces, bit 2
  // says which winding is front. Both bits set means nothing is drawn, which
  // the guest does use -- map it to FRONT and let the depth state decide.
  const bool cull_front = (call.mode_control & state::kCullFrontBit) != 0;
  const bool cull_back = (call.mode_control & state::kCullBackBit) != 0;
  desc.cullMode = cull_front ? plume::RenderCullMode::FRONT
                  : cull_back ? plume::RenderCullMode::BACK
                              : plume::RenderCullMode::NONE;
  desc.frontFace = (call.mode_control & state::kFrontFaceCwBit)
                       ? plume::RenderFrontFace::CLOCKWISE
                       : plume::RenderFrontFace::COUNTER_CLOCKWISE;

  // RB_BLENDCONTROL0 describes the effective blend on its own: turning blending
  // off writes ONE/ZERO/ADD into it, and the factor setters return early while
  // it is off. So there is no separate enable to consult.
  plume::RenderBlendDesc blend;
  const uint32_t src = call.blend_control & 0x1F;
  const uint32_t op = (call.blend_control >> 5) & 0x7;
  const uint32_t dst = (call.blend_control >> 8) & 0x1F;
  const uint32_t src_a = (call.blend_control >> 16) & 0x1F;
  const uint32_t op_a = (call.blend_control >> 21) & 0x7;
  const uint32_t dst_a = (call.blend_control >> 24) & 0x1F;
  blend.blendEnabled = !(src == kBlendOne && dst == kBlendZero && op == kBlendOpAdd);
  blend.srcBlend = HostBlend(src);
  blend.dstBlend = HostBlend(dst);
  blend.blendOp = HostBlendOp(op);
  blend.srcBlendAlpha = HostBlendAlpha(src_a);
  blend.dstBlendAlpha = HostBlendAlpha(dst_a);
  blend.blendOpAlpha = HostBlendOp(op_a);
  blend.renderTargetWriteMask = uint8_t(call.color_mask);
  if (!call.depth_only) {
    desc.renderTargetBlend[0] = blend;
  }

  auto pipeline = ctx.device->createGraphicsPipeline(desc);
  if (!PipelineCreated(pipeline.get())) {
    if (++g_pipelineFailures <= 12) {
      REXLOG_WARN("[kameo-gfx] pipeline creation failed ({} inputs, stride {}, prim {})",
                  elements.size(), call.layout->dst_stride, call.primitive);
      for (const plume::RenderInputElement& e : elements) {
        REXLOG_WARN("[kameo-gfx]   {}{} format {} at {}", e.semanticName, e.semanticIndex,
                    uint32_t(e.format), e.alignedByteOffset);
      }
    }
    g_pipelines.emplace(key, nullptr);
    return nullptr;
  }
  auto* raw = pipeline.get();
  g_pipelines.emplace(key, std::move(pipeline));
  return raw;
}

uint32_t g_sceneFrames = 0;
uint32_t g_resolveCount = 0;
uint32_t g_resolveSkipped = 0;  // offscreen resolves with no pass rendered this frame
uint32_t g_postProcessHeldBack = 0;  // full-screen opaque composites, held until bloom works
std::map<uint64_t, std::pair<uint32_t, uint32_t>> g_quadCensus;  // ps hash -> draws, tris
uint32_t g_grassCaptured = 0;   // QUADLIST draws seen at submission
uint32_t g_grassSubmitted = 0;  // ...and actually drawn
uint32_t g_resolveFromScene = 0;    // EDRAM resolves of the scene itself (bloom chain)
uint32_t g_depthResolveDone = 0;    // shadow-map depth copies performed
uint32_t g_depthResolveMissed = 0;  // depth resolves with no depth-only target of that size

// Offscreen render targets.
//
// The storybook renders each page's content into its own target (640x480,
// 240x320, 480x640), resolves it to a texture and maps that onto the curved
// page mesh. Rendering those passes into the swap chain instead put the page
// content on the screen AND, via the resolve, on the page -- the text appeared
// twice, once flat and once wrapped.
//
// A draw is redirected when its viewport is not the guest's full 1280x720
// frame, which is what distinguishes an offscreen pass from the main scene
// without having to reverse the D3DSurface layout.
struct OffscreenTarget {
  uint32_t surface = 0;  // the guest D3DSurface this pass renders into
  std::unique_ptr<plume::RenderTexture> color;
  std::unique_ptr<plume::RenderTextureView> color_view;
  std::unique_ptr<plume::RenderTexture> depth;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t cleared_frame = ~0ull;
};
std::unordered_map<uint64_t, OffscreenTarget> g_offscreen;

// Depth-only passes (the shadow maps) render with NO colour target, so they
// need a framebuffer with none either -- the PSO declares renderTargetCount 0
// and D3D12 requires the bound targets to agree. Sized from the draw's own
// viewport and keyed on that size, because these passes carry no surface
// pointer to key on (rt_surface is 0).
struct DepthOnlyTarget {
  std::unique_ptr<plume::RenderTexture> depth;
  std::unique_ptr<plume::RenderTextureView> depth_view;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t cleared_frame = ~0ull;
};
std::unordered_map<uint64_t, DepthOnlyTarget> g_depthOnly;

DepthOnlyTarget* EnsureDepthOnly(KameoGraphicsSystem::DrawPathContext& ctx, uint32_t w,
                                 uint32_t h) {
  const uint64_t key = (uint64_t(w) << 32) | h;
  auto found = g_depthOnly.find(key);
  if (found != g_depthOnly.end()) {
    return &found->second;
  }
  DepthOnlyTarget target;
  target.width = w;
  target.height = h;
  target.depth = ctx.device->createTexture(
      plume::RenderTextureDesc::DepthTarget(w, h, plume::RenderFormat(ctx.depth_format)));
  if (!target.depth) {
    return nullptr;
  }
  target.depth_view = target.depth->createTextureView(
      plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat(ctx.depth_format)));
  plume::RenderFramebufferDesc desc;
  desc.colorAttachments = nullptr;
  desc.colorAttachmentsCount = 0;
  desc.depthAttachment = target.depth.get();
  desc.depthAttachmentView = target.depth_view.get();
  target.framebuffer = ctx.device->createFramebuffer(desc);
  if (!target.framebuffer) {
    REXLOG_ERROR("[kameo-gfx] depth-only framebuffer {}x{} FAILED", w, h);
    return nullptr;
  }
  REXLOG_INFO("[kameo-gfx] depth-only render target {}x{} created", w, h);
  return &g_depthOnly.emplace(key, std::move(target)).first->second;
}

OffscreenTarget* EnsureOffscreen(KameoGraphicsSystem::DrawPathContext& ctx, uint64_t key,
                                 uint32_t w, uint32_t h) {
  auto found = g_offscreen.find(key);
  if (found != g_offscreen.end()) {
    return &found->second;
  }
  OffscreenTarget target;
  target.surface = uint32_t(key >> 32);
  target.width = w;
  target.height = h;
  target.color = ctx.device->createTexture(plume::RenderTextureDesc::ColorTarget(
      w, h, plume::RenderFormat(ctx.color_format)));
  target.depth = ctx.device->createTexture(
      plume::RenderTextureDesc::DepthTarget(w, h, plume::RenderFormat(ctx.depth_format)));
  if (!target.color || !target.depth) {
    return nullptr;
  }
  target.color_view = target.color->createTextureView(
      plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat(ctx.color_format)));

  const plume::RenderTexture* attachments[] = {target.color.get()};
  plume::RenderFramebufferDesc desc;
  desc.colorAttachments = attachments;
  desc.colorAttachmentsCount = 1;
  desc.depthAttachment = target.depth.get();
  target.framebuffer = ctx.device->createFramebuffer(desc);
  if (!target.framebuffer) {
    return nullptr;
  }
  REXLOG_INFO("[kameo-gfx] offscreen render target {}x{} created", w, h);
  return &g_offscreen.emplace(key, std::move(target)).first->second;
}

// Copies what has been rendered so far into a host texture the guest can then
// sample. The scene renders straight into the swap chain image, so that is the
// copy source; it has to leave COLOR_WRITE and come back, and the framebuffer
// has to be rebound afterwards.
void PerformResolve(KameoGraphicsSystem::DrawPathContext& ctx, plume::RenderCommandList* list,
                    plume::RenderTexture* source, const DrawCall& call, uint32_t width,
                    uint32_t height) {
  // The depth path is handled BEFORE the null-source guard, because a depth
  // resolve does not use `source` at all -- and for a shadow pass it is always
  // null. The game sets render target 0 to NULL for the whole shadow pass
  // (confirmed in preShadowRender: D3DDevice_SetRenderTarget(dev, 0, 0)), so
  // the depth-only routing sets current_texture = nullptr, and this guard was
  // silently discarding every shadow resolve -- 1017 of them per census
  // window, against 108 for the scene depth that did work.
  if (call.resolve_depth) {
    // The shadow maps. The guest renders them depth-only (no colour target, so
    // rt_surface is 0) and resolves them to k_24_8 textures, which the scene
    // then samples. Match the depth-only target by SIZE -- that is the only
    // identity these passes have, and it was measured to line up exactly:
    // 768x768, 1024x1024 and 1280x720 targets against resolves of the same.
    auto depth_src = g_depthOnly.find((uint64_t(call.resolve_width) << 32) | call.resolve_height);
    if (depth_src == g_depthOnly.end() || !depth_src->second.depth) {
      // COUNTED, not logged once per address: the first miss happens at boot,
      // 20+ seconds before any level target exists, and first-sight-only
      // logging let that harmless miss mask the steady state completely.
      ++g_depthResolveMissed;
      // CREATE the target now rather than just dropping this resolve. Measured
      // ordering: the shadow maps are resolved ~2/frame BEFORE any depth-only
      // draw of that size has created a target, and once they stop being
      // re-resolved (they are baked, then sampled) a dropped window never comes
      // back -- which is why the 768x768 and 1024x1024 maps stayed magenta for
      // the whole session. Creating it here means the next shadow pass renders
      // into it and the following resolve succeeds; this frame still has
      // nothing to copy, so it is skipped.
      EnsureDepthOnly(ctx, call.resolve_width, call.resolve_height);
      return;
    }
    ++g_depthResolveDone;

    const ResolveKey depth_key{call.resolve_address, uint16_t(call.resolve_width),
                               uint16_t(call.resolve_height), true};
    auto found_depth = g_resolveTargets.find(depth_key);
    if (found_depth == g_resolveTargets.end()) {
      CachedTexture entry;
      // R32_FLOAT, not a depth format: this is the SAMPLEABLE copy. D32_FLOAT
      // and R32_FLOAT share the R32 typeless group, which is what makes the
      // copy legal in D3D12.
      entry.texture = ctx.device->createTexture(plume::RenderTextureDesc::Texture2D(
          call.resolve_width, call.resolve_height, 1, plume::RenderFormat::R32_FLOAT));
      if (!entry.texture) {
        return;
      }
      entry.view = entry.texture->createTextureView(
          plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R32_FLOAT));
      if (g_nextTextureSlot >= kMaxBindlessTextures) {
        return;
      }
      entry.heap_index = g_nextTextureSlot++;
      entry.width = call.resolve_width;
      entry.height = call.resolve_height;
      entry.format = plume::RenderFormat::R32_FLOAT;
      ctx.texture_set->setTexture(entry.heap_index, entry.texture.get(),
                                  plume::RenderTextureLayout::SHADER_READ, entry.view.get());
      REXLOG_INFO("[kameo-gfx] DEPTH resolve target {:08X} {}x{} registered at heap slot {}",
                  call.resolve_address, call.resolve_width, call.resolve_height, entry.heap_index);
      found_depth = g_resolveTargets.emplace(depth_key, std::move(entry)).first;
    }

    // Belt and braces: the source depth target must match too, or the
    // full-resource copy is out of bounds from the other end.
    if (depth_src->second.width != call.resolve_width ||
        depth_src->second.height != call.resolve_height) {
      ++g_depthResolveMissed;
      return;
    }

    plume::RenderTexture* depth_tex = depth_src->second.depth.get();
    plume::RenderTexture* depth_dst = found_depth->second.texture.get();
    const plume::RenderTextureBarrier to_copy[] = {
        plume::RenderTextureBarrier(depth_tex, plume::RenderTextureLayout::COPY_SOURCE),
        plume::RenderTextureBarrier(depth_dst, plume::RenderTextureLayout::COPY_DEST),
    };
    list->barriers(plume::RenderBarrierStage::COPY, to_copy, uint32_t(std::size(to_copy)));
    list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(depth_dst, 0, 0),
                            plume::RenderTextureCopyLocation::Subresource(depth_tex, 0, 0), 0, 0,
                            0, nullptr);
    const plume::RenderTextureBarrier back[] = {
        plume::RenderTextureBarrier(depth_tex, plume::RenderTextureLayout::DEPTH_WRITE),
        plume::RenderTextureBarrier(depth_dst, plume::RenderTextureLayout::SHADER_READ),
    };
    list->barriers(plume::RenderBarrierStage::GRAPHICS, back, uint32_t(std::size(back)));
    ++g_resolveCount;
    return;
  }

  if (!source) {
    return;  // colour resolve with nothing bound to copy from
  }

  const ResolveKey colour_key{call.resolve_address, uint16_t(call.resolve_width),
                              uint16_t(call.resolve_height), false};
  auto found = g_resolveTargets.find(colour_key);
  if (found == g_resolveTargets.end()) {
    CachedTexture entry;
    // Matching the swap chain's format keeps this a straight copy. Channel
    // ORDER is not a problem: a shader sampling B8G8R8A8_UNORM still gets red
    // in .r, because DXGI formats describe memory layout, not swizzle.
    entry.texture = ctx.device->createTexture(plume::RenderTextureDesc::Texture2D(
        call.resolve_width, call.resolve_height, 1, plume::RenderFormat::B8G8R8A8_UNORM));
    if (!entry.texture) {
      return;
    }
    entry.view = entry.texture->createTextureView(
        plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::B8G8R8A8_UNORM));
    if (g_nextTextureSlot >= kMaxBindlessTextures) {
      return;
    }
    entry.heap_index = g_nextTextureSlot++;
    entry.width = call.resolve_width;
    entry.height = call.resolve_height;
    entry.format = plume::RenderFormat::B8G8R8A8_UNORM;
    ctx.texture_set->setTexture(entry.heap_index, entry.texture.get(),
                                plume::RenderTextureLayout::SHADER_READ, entry.view.get());
    REXLOG_INFO("[kameo-gfx] resolve target {:08X} {}x{} registered at heap slot {}",
                call.resolve_address, call.resolve_width, call.resolve_height, entry.heap_index);
    found = g_resolveTargets.emplace(colour_key, std::move(entry)).first;
  }

  plume::RenderTexture* dest = found->second.texture.get();

  // Clamp to what BOTH ends can actually hold, offset included. `left + copy_w`
  // has to stay inside the source as well as the destination; clamping only the
  // width let a non-zero source rect run off the end of the source.
  const int32_t left_pre = std::max(0, call.resolve_x);
  const int32_t top_pre = std::max(0, call.resolve_y);
  const uint32_t src_avail_w = uint32_t(left_pre) < width ? width - uint32_t(left_pre) : 0;
  const uint32_t src_avail_h = uint32_t(top_pre) < height ? height - uint32_t(top_pre) : 0;
  const uint32_t copy_w = std::min({call.resolve_width, src_avail_w, found->second.width});
  const uint32_t copy_h = std::min({call.resolve_height, src_avail_h, found->second.height});
  if (copy_w == 0 || copy_h == 0) {
    return;
  }

  const plume::RenderTextureBarrier to_copy[] = {
      plume::RenderTextureBarrier(source, plume::RenderTextureLayout::COPY_SOURCE),
      plume::RenderTextureBarrier(dest, plume::RenderTextureLayout::COPY_DEST),
  };
  list->barriers(plume::RenderBarrierStage::COPY, to_copy, uint32_t(std::size(to_copy)));

  // RenderBox is (left, top, right, bottom, front, back) -- not a pair of 3D
  // points, which is what the argument order invites you to assume.
  const int32_t left = left_pre;
  const int32_t top = top_pre;
  const plume::RenderBox box(left, top, left + int32_t(copy_w), top + int32_t(copy_h), 0, 1);
  list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(dest, 0, 0),
                          plume::RenderTextureCopyLocation::Subresource(source, 0, 0), 0, 0, 0,
                          &box);

  const plume::RenderTextureBarrier back[] = {
      plume::RenderTextureBarrier(source, plume::RenderTextureLayout::COLOR_WRITE),
      plume::RenderTextureBarrier(dest, plume::RenderTextureLayout::SHADER_READ),
  };
  list->barriers(plume::RenderBarrierStage::GRAPHICS, back, uint32_t(std::size(back)));
  ++g_resolveCount;
}

}  // namespace

void EnableD3D12DebugLayer() {
#ifdef _WIN32
  ID3D12Debug* debug = nullptr;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))) && debug != nullptr) {
    debug->EnableDebugLayer();
    debug->Release();
    REXLOG_INFO("[kameo-gfx] D3D12 debug layer enabled");
  } else {
    // Not an error: the layer ships with the optional "Graphics Tools" feature
    // and is simply absent on most machines.
    REXLOG_WARN("[kameo-gfx] D3D12 debug layer unavailable (install the Graphics Tools feature)");
  }
#endif
}

// Drains whatever the debug layer has to say. plume only wires an info queue up
// in its own debug builds, and PSO creation failures are otherwise completely
// silent -- this is the only way to learn WHY a pipeline was rejected.
// Reports device loss once. GetDeviceRemovedReason is the only thing that
// separates "the GPU died" from "we have a bug in our own submission": both
// present as every draw silently vanishing.
bool CheckDeviceRemoved(plume::RenderDevice* device) {
#ifdef _WIN32
  if (!device) {
    return false;
  }
  static bool reported = false;
  if (reported) {
    return true;
  }
  auto* d3d12_device = static_cast<plume::D3D12Device*>(device)->d3d;
  if (d3d12_device == nullptr) {
    return false;
  }
  const HRESULT reason = d3d12_device->GetDeviceRemovedReason();
  if (FAILED(reason)) {
    reported = true;
    REXLOG_ERROR("[kameo-gfx] D3D12 DEVICE REMOVED, reason 0x{:08X} -- every draw from here on "
                 "silently disappears. 0x887A0001 INVALID_CALL (our bug: an out-of-bounds copy "
                 "or a bad parameter), 0x887A0005 DEVICE_REMOVED, 0x887A0006 DEVICE_HUNG (TDR), "
                 "0x887A0007 DEVICE_RESET, 0x887A0020 DRIVER_INTERNAL_ERROR.",
                 uint32_t(reason));
    return true;
  }
#endif
  return false;
}

void DrainD3D12Messages(plume::RenderDevice* device) {
#ifdef _WIN32
  if (!device) {
    return;
  }
  static plume::RenderDevice* bound = nullptr;
  static ID3D12InfoQueue* queue = nullptr;
  if (bound != device) {
    bound = device;
    if (queue) {
      queue->Release();
      queue = nullptr;
    }
    auto* d3d12_device = static_cast<plume::D3D12Device*>(device)->d3d;
    if (d3d12_device != nullptr) {
      d3d12_device->QueryInterface(IID_PPV_ARGS(&queue));
    }
  }
  if (!queue) {
    return;
  }

  const uint64_t count = queue->GetNumStoredMessages();
  std::vector<uint8_t> storage;
  for (uint64_t i = 0; i < count; ++i) {
    SIZE_T length = 0;
    if (FAILED(queue->GetMessage(i, nullptr, &length)) || length == 0) {
      continue;
    }
    storage.resize(length);
    auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
    if (FAILED(queue->GetMessage(i, message, &length))) {
      continue;
    }
    if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
      REXLOG_ERROR("[kameo-gfx] D3D12: {}", std::string(message->pDescription, message->DescriptionByteLength));
    } else if (message->Severity == D3D12_MESSAGE_SEVERITY_WARNING) {
      REXLOG_WARN("[kameo-gfx] D3D12: {}", std::string(message->pDescription, message->DescriptionByteLength));
    }
  }
  queue->ClearStoredMessages();
#endif
}

void ReleaseDrawPathCaches() {
  g_vertexRing.unmap();
  g_indexRing.unmap();
  g_constantRing.unmap();
  g_vertexRing.buffer.reset();
  g_indexRing.buffer.reset();
  g_constantRing.buffer.reset();
  g_pipelines.clear();
  g_samplers.clear();
  g_textures.clear();
  g_resolveTargets.clear();
  g_offscreen.clear();
  g_fallbackView.reset();
  g_fallbackTexture.reset();
  g_fallbackUpload.reset();
  g_nextTextureSlot = 1;
  g_nextSamplerSlot = 0;
  g_heapExhausted = false;
}

void DrawCapturedScene(KameoGraphicsSystem& gfx, plume::RenderCommandList* list,
                       plume::RenderTexture* target, const plume::RenderFramebuffer* framebuffer,
                       uint32_t width, uint32_t height) {
  DrawFrame frame;
  {
    std::lock_guard<std::mutex> lock(g_captureMutex);
    if (!g_readyValid) {
      return;
    }
    frame = std::move(g_ready);
    g_readyValid = false;
  }
  if (frame.draws.empty()) {
    return;
  }

  KameoGraphicsSystem::DrawPathContext ctx;
  if (!gfx.GetDrawPathContext(&ctx)) {
    return;
  }
  if (ctx.spirv) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXLOG_ERROR("[kameo-gfx] the general draw path is DXIL-only; the SPIR-V shaders bind their "
                   "constants through push constants and need a separate path");
    }
    return;
  }

  g_vertexRing.name = "vertex";
  g_indexRing.name = "index";
  g_constantRing.name = "constant";
  // A map failure that never clears usually means the device is gone: a GPU
  // hang / TDR makes every subsequent map fail forever, which looks exactly
  // like a freeze with the renderer still presenting. Distinguish the two here
  // rather than inferring it from the symptom.
  if (CheckDeviceRemoved(ctx.device)) {
    // Once the device is gone every map returns null and plume's D3D12Buffer
    // ::map dereferences it, turning a diagnosable stall into a hard crash in
    // someone else's code. Stop here instead: the process stays alive and the
    // log keeps the reason.
    return;
  }

  if (!g_vertexRing.ensure(ctx.device, 64u << 20, plume::RenderBufferFlag::VERTEX) ||
      !g_indexRing.ensure(ctx.device, 32u << 20, plume::RenderBufferFlag::INDEX) ||
      !g_constantRing.ensure(ctx.device, 96u << 20, plume::RenderBufferFlag::CONSTANT)) {
    return;
  }
  g_vertexRing.begin();
  g_indexRing.begin();
  g_constantRing.begin();

  EnsureFallbackTexture(ctx, list);

  list->setGraphicsPipelineLayout(ctx.layout);
  list->setGraphicsDescriptorSet(ctx.texture_set, 0);
  list->setGraphicsDescriptorSet(ctx.texture3d_set, 1);
  list->setGraphicsDescriptorSet(ctx.cube_set, 2);
  list->setGraphicsDescriptorSet(ctx.sampler_set, 3);

  uint8_t* base = g_guestBase;
  if (base == nullptr) {
    return;
  }

  // An offscreen pass is a draw that renders into a surface OTHER than the
  // scene's. That is a direct read of guest state; the viewport test below only
  // recognises a page pass when the guest also shrank the viewport, and run 099
  // measured two of the storybook's three pages keeping the full 1280x720
  // viewport. Those two were never redirected, so their resolves copied the
  // swap chain and the pages came out as the purple background.
  //
  // An offscreen surface's dimensions come from the resolve that consumes it,
  // so the frame is scanned once up front to learn them.
  uint32_t main_surface = 0;
  for (const DrawCall& call : frame.draws) {
    if (call.kind != CommandKind::Draw) {
      continue;
    }
    if (uint32_t(call.viewport[2]) == kGuestWidth && uint32_t(call.viewport[3]) == kGuestHeight) {
      main_surface = call.rt_surface;
      break;
    }
  }
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> surface_dims;
  for (const DrawCall& call : frame.draws) {
    if (call.kind != CommandKind::Resolve || call.resolve_depth) {
      continue;
    }
    if (call.rt_surface == 0 || call.rt_surface == main_surface) {
      continue;
    }
    surface_dims[call.rt_surface] = {call.resolve_width, call.resolve_height};
  }

  // How many DRAWS each surface actually receives this frame. A resolve whose
  // surface got zero draws means the content pass is not reaching us at all;
  // a resolve whose surface got draws that were never redirected means the
  // detection is wrong. Those need completely different fixes, and the skipped
  // post-process chain (512x512, 426x240, 160x90, 32x32) and the book's black
  // slots cannot be told apart without this.
  std::unordered_map<uint32_t, uint32_t> draws_per_surface;
  for (const DrawCall& call : frame.draws) {
    if (call.kind == CommandKind::Draw) {
      draws_per_surface[call.rt_surface]++;
    }
  }

  // Which surface the command stream is currently rendering into. Starts as the
  // swap chain; page passes redirect it and Resolve copies from whichever is
  // active.
  plume::RenderTexture* current_texture = target;
  const plume::RenderFramebuffer* current_framebuffer = framebuffer;
  uint64_t current_key = 0;
  uint32_t drawn = 0;
  uint32_t skipped_shader = 0;
  uint32_t skipped_pipeline = 0;
  uint32_t skipped_space = 0;

  // The shared block is rebuilt per draw: it carries the descriptor indices the
  // translated shaders look their textures up by, so binding a texture without
  // writing its index here draws nothing.
  uint8_t shared[kSharedConstantBytes];

  for (const DrawCall& call : frame.draws) {
    if (call.kind == CommandKind::Bink) {
      gfx.DrawPendingBink(list);
      list->setGraphicsPipelineLayout(ctx.layout);
      list->setGraphicsDescriptorSet(ctx.texture_set, 0);
      list->setGraphicsDescriptorSet(ctx.texture3d_set, 1);
      list->setGraphicsDescriptorSet(ctx.cube_set, 2);
      list->setGraphicsDescriptorSet(ctx.sampler_set, 3);
      continue;
    }
    if (call.kind == CommandKind::Resolve) {
      // The source comes from the resolve's OWN surface, not from whatever the
      // last draw left active. Inferring it from the active surface is only
      // right when the page pass immediately precedes its resolve; when those
      // draws went somewhere else -- or were dropped for a missing shader --
      // the copy silently took the swap chain instead.
      plume::RenderTexture* source = current_texture;
      uint32_t src_w = current_key == 0 ? width : uint32_t(-1);
      uint32_t src_h = current_key == 0 ? height : uint32_t(-1);
      if (call.rt_surface != 0 && main_surface != 0 && call.rt_surface != main_surface) {
        // Find the pass by SURFACE ALONE, not surface+resolve size. The two are
        // different things: imposterModelRender draws each inventory icon into
        // edRamManagerScratchPadColour at the scratch pad's own size, then
        // resolves it into an icon texture of a DIFFERENT size. Keying on the
        // resolve's dimensions missed the pass that had just rendered, and the
        // icon was skipped and left black. RenderDoc showed 481 draws landing
        // in that 256x256 target and being thrown away at the resolve.
        OffscreenTarget* off_match = nullptr;
        for (auto& [k, t] : g_offscreen) {
          if (t.surface == call.rt_surface && t.cleared_frame == g_sceneFrames) {
            off_match = &t;
            break;
          }
        }
        if (off_match != nullptr) {
          source = off_match->color.get();
          src_w = off_match->width;
          src_h = off_match->height;
        } else if (false && draws_per_surface.count(call.rt_surface) == 0) {
          // DISABLED. "Zero draws targeted this surface" looked like a clean
          // way to spot the bloom chain, but the storybook page targets have
          // zero draws too, so this branch copied the purple background over
          // the finished pages -- the exact bug the skip below was added to
          // fix, reintroduced from the other side. The bloom textures stay
          // black until there is a discriminator that actually separates the
          // two cases.
          // ZERO draws targeted this surface, so there is no pass to have
          // missed: the guest is resolving the SCENE out of EDRAM. Kameo's
          // bloom chain does exactly this -- 512x512, 426x240, 160x90, 32x32
          // resolved straight from the frame buffer with no geometry of their
          // own. Copying the frame buffer is the RIGHT answer here, and
          // skipping it is what turned those textures black.
          //
          // This is the opposite of the storybook pages below, which DO have a
          // pass and must never take the frame buffer. Whether the surface
          // received any draws is what separates them.
          //
          // Name the scene explicitly: `current_texture` may be an offscreen
          // target at this point in the stream, and the guest is resolving out
          // of EDRAM, not out of whatever pass happened to run last.
          source = target;
          src_w = width;
          src_h = height;
          ++g_resolveFromScene;
        } else {
          // The surface has draws but no offscreen target -- a real pass we
          // failed to redirect. Copying the frame buffer here is what painted
          // the storybook pages with the purple background behind them, so
          // leave the texture holding whatever was last resolved into it.
          ++g_resolveSkipped;
          static std::set<uint32_t> reported;
          if (reported.insert(call.resolve_address).second) {
            auto dc = draws_per_surface.find(call.rt_surface);
            const uint32_t n = dc == draws_per_surface.end() ? 0 : dc->second;
            REXLOG_INFO("[kameo-gfx] resolve {:08X} {}x{} SKIPPED: surface {:08X} had no "
                        "offscreen pass this frame ({} draws targeted that surface; "
                        "main_surface {:08X})",
                        call.resolve_address, call.resolve_width, call.resolve_height,
                        call.rt_surface, n, main_surface);
          }
          continue;
        }
      }
      PerformResolve(ctx, list, source, call, src_w, src_h);
      // The copy took the render target out of COLOR_WRITE, so the framebuffer
      // and its state have to be re-established before the next draw.
      list->setFramebuffer(current_framebuffer);
      list->setGraphicsPipelineLayout(ctx.layout);
      list->setGraphicsDescriptorSet(ctx.texture_set, 0);
      list->setGraphicsDescriptorSet(ctx.texture3d_set, 1);
      list->setGraphicsDescriptorSet(ctx.cube_set, 2);
      list->setGraphicsDescriptorSet(ctx.sampler_set, 3);
      continue;
    }

    // Pick this draw's target. A viewport smaller than the guest's own frame
    // means an offscreen pass; anything full-size is the scene itself.
    const uint32_t vp_w = uint32_t(call.viewport[2]);
    const uint32_t vp_h = uint32_t(call.viewport[3]);
    bool offscreen = vp_w != 0 && vp_h != 0 && (vp_w != kGuestWidth || vp_h != kGuestHeight);
    uint32_t off_w = vp_w;
    uint32_t off_h = vp_h;
    // A surface that is not the scene's is an offscreen pass even when the
    // guest left the viewport at full size.
    if (!offscreen && main_surface != 0 && call.rt_surface != main_surface) {
      auto dims = surface_dims.find(call.rt_surface);
      if (dims != surface_dims.end()) {
        offscreen = true;
        off_w = dims->second.first;
        off_h = dims->second.second;
      }
    }
    const uint64_t want_key =
        offscreen ? ((uint64_t(call.rt_surface) << 32) ^ (uint64_t(off_w) << 16) ^ off_h) : 0;

    // A depth-only pass goes to a depth-only framebuffer regardless of what the
    // colour routing above decided: its PSO has no render targets.
    if (call.depth_only) {
      DepthOnlyTarget* d = EnsureDepthOnly(ctx, vp_w ? vp_w : kGuestWidth,
                                           vp_h ? vp_h : kGuestHeight);
      if (d == nullptr) {
        ++skipped_pipeline;
        continue;
      }
      if (current_framebuffer != d->framebuffer.get()) {
        // Without this the resource sits in COMMON and ClearDepthStencilView is
        // rejected -- the colour offscreen path does the same transition.
        list->barriers(plume::RenderBarrierStage::GRAPHICS,
                       plume::RenderTextureBarrier(d->depth.get(),
                                                   plume::RenderTextureLayout::DEPTH_WRITE));
        current_texture = nullptr;
        current_framebuffer = d->framebuffer.get();
        current_key = ~0ull;  // never matches a colour key, so the next colour
                              // draw re-establishes its own framebuffer
        list->setFramebuffer(current_framebuffer);
        list->setGraphicsPipelineLayout(ctx.layout);
        list->setGraphicsDescriptorSet(ctx.texture_set, 0);
        list->setGraphicsDescriptorSet(ctx.texture3d_set, 1);
        list->setGraphicsDescriptorSet(ctx.cube_set, 2);
        list->setGraphicsDescriptorSet(ctx.sampler_set, 3);
      }
      if (d->cleared_frame != g_sceneFrames) {
        d->cleared_frame = g_sceneFrames;
        list->clearDepth(true, 1.0f);
      }
    } else if (want_key != current_key) {
      if (want_key == 0) {
        current_texture = target;
        current_framebuffer = framebuffer;
      } else if (OffscreenTarget* off = EnsureOffscreen(ctx, want_key, off_w, off_h)) {
        list->barriers(plume::RenderBarrierStage::GRAPHICS,
                       plume::RenderTextureBarrier(off->color.get(),
                                                   plume::RenderTextureLayout::COLOR_WRITE));
        current_texture = off->color.get();
        current_framebuffer = off->framebuffer.get();
        list->setFramebuffer(current_framebuffer);
        // Each offscreen target is cleared once per frame, the first time the
        // stream lands on it.
        if (off->cleared_frame != g_sceneFrames) {
          off->cleared_frame = g_sceneFrames;
          list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
          list->clearDepth(true, 1.0f);
        }
      } else {
        current_texture = target;
        current_framebuffer = framebuffer;
      }
      if (want_key == 0) {
        list->setFramebuffer(current_framebuffer);
      }
      current_key = want_key;
      list->setGraphicsPipelineLayout(ctx.layout);
      list->setGraphicsDescriptorSet(ctx.texture_set, 0);
      list->setGraphicsDescriptorSet(ctx.texture3d_set, 1);
      list->setGraphicsDescriptorSet(ctx.cube_set, 2);
      list->setGraphicsDescriptorSet(ctx.sampler_set, 3);
    }

    // Grass is identifiable: grass_coreRender draws QUADLIST with a 20-byte
    // stride via DrawVertices. Count them submitted vs captured so "the grass
    // is black" can be split into "never drawn" and "drawn but shaded wrong"
    // without guessing.
    const bool is_grass = (call.primitive == kPrimQuadList);
    if (is_grass) {
      ++g_grassCaptured;
    }
    // Bisection: remove one primitive type from the frame to find out what a
    // block of black polygons actually belongs to.
    if (REXCVAR_GET(kameo_gfx_skip_primitive) >= 0 &&
        call.primitive == uint32_t(REXCVAR_GET(kameo_gfx_skip_primitive))) {
      continue;
    }
    if (REXCVAR_GET(kameo_gfx_only_primitive) >= 0 &&
        call.primitive != uint32_t(REXCVAR_GET(kameo_gfx_only_primitive))) {
      continue;
    }
    // Skinned-character draws, identified by BLENDINDICES in the declaration --
    // that is what a bone-weighted mesh has and terrain/foliage does not. Their
    // lighting is what makes Kameo look metallic, so report the shader pairing
    // and the light constants together: c0/c1/c2 are SunLightColour /
    // AmbientLightColour / FogColour, pinned by grass_coreRender writing them
    // straight to device+0x1780 (= kPixelShaderFloatConstants).
    if (call.layout && call.ps && call.ps_constants != kNoData) {
      bool skinned = false;
      for (const HostElement& e : call.layout->elements) {
        if (e.usage == kUsageBlendIndices) {
          skinned = true;
          break;
        }
      }
      if (skinned) {
        static std::set<uint64_t> skinned_reported;
        if (skinned_reported.size() < 8 && skinned_reported.insert(call.ps->hash).second) {
          const auto* c = reinterpret_cast<const float*>(frame.arena.data() + call.ps_constants);
          std::string tex;
          for (uint32_t t = 0; t < kSamplerCount; ++t) {
            if (call.textures[t].valid()) {
              tex += fmt::format("s{}={:08X} fmt{} ", t, call.textures[t].address(),
                                 call.textures[t].format());
            }
          }
          REXLOG_INFO("[kameo-gfx] SKINNED draw: vs {:016X} ps {:016X} stride {} blend {:08X}",
                      call.vs ? call.vs->hash : 0ull, call.ps->hash, call.layout->dst_stride,
                      call.blend_control);
          REXLOG_INFO("[kameo-gfx]   Sun=({:.3f},{:.3f},{:.3f},{:.3f}) "
                      "Ambient=({:.3f},{:.3f},{:.3f},{:.3f}) Fog=({:.3f},{:.3f},{:.3f},{:.3f}) "
                      "c3=({:.3f},{:.3f},{:.3f},{:.3f})",
                      c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11],
                      c[12], c[13], c[14], c[15]);
          REXLOG_INFO("[kameo-gfx]   textures [{}]", tex);
          // The character shader's real lighting registers, from its own
          // constant table (NOT the foliage shader's c0/c1/c2 = Sun/Ambient/Fog
          // layout -- a different shader names its constants differently, and
          // assuming otherwise is how you end up reading the wrong numbers):
          //   c14 ambientHigh  c19 light0dir  c20 light0colour
          //   c21 light1colour c22 light2colour c26 light1atten
          //   c27 fillColour   c28 light1pos  c29 light2pos  c30 ambient
          auto reg = [c](uint32_t n) {
            return fmt::format("({:.3f},{:.3f},{:.3f},{:.3f})", c[4 * n], c[4 * n + 1],
                               c[4 * n + 2], c[4 * n + 3]);
          };
          REXLOG_INFO("[kameo-gfx]   light0 dir{} col{} | light1 col{} pos{} atten{} | "
                      "light2 col{} pos{}",
                      reg(19), reg(20), reg(21), reg(28), reg(26), reg(22), reg(29));
          REXLOG_INFO("[kameo-gfx]   ambient{} ambientHigh{} fill{} specExp{}",
                      reg(30), reg(14), reg(27), reg(3));
        }
      }
    }

    // A full-screen OPAQUE BeginVertices quad is not HUD -- it is the
    // post-process composite, which samples the bloom chain. That chain is
    // still black (see "Still to do"), so before these draws were captured at
    // all it was invisible, and now it paints a black rectangle over the scene.
    // Neither is right, but invisible is closer, so hold these back until bloom
    // works. Deliberately narrow: every HUD element is a small quad, and the
    // downsample steps have their own smaller viewports and route to offscreen
    // targets normally.
    if (call.deferred_vertex_addr && call.color_mask != 0 &&
        (call.blend_control & 0xFFFF) == 0x0001 &&
        uint32_t(call.viewport[2]) == kGuestWidth && uint32_t(call.viewport[3]) == kGuestHeight) {
      ++g_postProcessHeldBack;
      continue;
    }

    // What the newly-captured BeginVertices draws actually are. One of them
    // paints a large dark rectangle over the scene, and the fade overlays are
    // known to be untextured stride-16 quads, so report shape and blend.
    if (call.deferred_vertex_addr) {
      static std::set<uint64_t> seen_def;
      const uint64_t k = (uint64_t(call.layout ? call.layout->dst_stride : 0) << 40) ^
                         (uint64_t(call.blend_control) << 8) ^ call.primitive;
      if (seen_def.size() < 14 && seen_def.insert(k).second) {
        std::string tex;
        for (uint32_t t = 0; t < kSamplerCount; ++t) {
          if (call.textures[t].valid()) {
            tex += fmt::format("s{}={:08X} ", t, call.textures[t].address());
          }
        }
        std::string col;
        if (call.layout && call.vertex_data != kNoData) {
          const uint8_t* v = frame.arena.data() + call.vertex_data;
          for (const HostElement& e : call.layout->elements) {
            if (e.usage == kUsageColor) {
              for (uint32_t i = 0; i < std::min<uint32_t>(2, call.vertex_count); ++i) {
                const size_t off = size_t(i) * call.layout->dst_stride + e.dst_offset;
                col += fmt::format("[{:02X}{:02X}{:02X}{:02X}]", v[off], v[off + 1], v[off + 2],
                                   v[off + 3]);
              }
            }
          }
        }
        REXLOG_INFO("[kameo-gfx] BEGINVERTICES draw: prim {} count {} stride {} blend {:08X} "
                    "colormask {} depth {:08X} rt {:08X} vp {}x{} colour {} textures [{}]",
                    call.primitive, call.vertex_count,
                    call.layout ? call.layout->dst_stride : 0, call.blend_control, call.color_mask,
                    call.depth_control, call.rt_surface, uint32_t(call.viewport[2]),
                    uint32_t(call.viewport[3]), col.empty() ? "none" : col, tex);
      }
    }

    plume::RenderShader* vs = gfx.ShaderModule(call.vs);
    plume::RenderShader* ps = call.depth_only ? nullptr : gfx.ShaderModule(call.ps);
    if (!vs || (!ps && !call.depth_only)) {
      ++skipped_shader;
      continue;
    }
    plume::RenderPipeline* pipeline = EnsurePipeline(ctx, call, vs, ps);
    if (!pipeline) {
      ++skipped_pipeline;
      continue;
    }

    std::memset(shared, 0, sizeof(shared));
    auto* shared_words = reinterpret_cast<uint32_t*>(shared);
    for (uint32_t s = 0; s < kSamplerCount; ++s) {
      const GuestTextureFetch& fetch = call.textures[s];
      if (!fetch.valid()) {
        continue;
      }
      const uint32_t texture_index = EnsureTexture(base, ctx, list, fetch, call.texture_objects[s]);
      if (texture_index == 0) {
        continue;
      }
      // Layout fixed by XenosRecomp: 16 Texture2D indices, then 16 Texture3D,
      // then 16 TextureCube, then 16 sampler indices, then the shared block
      // proper at c16.
      shared_words[s] = texture_index;
      shared_words[48 + s] = EnsureSampler(ctx, fetch);
    }
    shared_words[64] = call.booleans;                       // c16.x g_Booleans
    shared_words[65] = 0;                                   // c16.y g_SwappedTexcoords
    const float half_pixel[2] = {-1.0f / float(width), 1.0f / float(height)};
    std::memcpy(&shared_words[66], half_pixel, sizeof(half_pixel));  // c16.zw
    std::memcpy(&shared_words[68], &call.alpha_ref, sizeof(float));  // c17.x g_AlphaThreshold

    const uint32_t vertex_offset = g_vertexRing.writeOnce(
        call.vertex_data, frame.arena.data() + call.vertex_data, call.vertex_bytes, 16);
    const uint32_t index_offset = g_indexRing.writeOnce(
        call.index_data, frame.arena.data() + call.index_data, call.index_count * 4, 4);
    const uint32_t vs_offset = g_constantRing.writeOnce(
        call.vs_constants, frame.arena.data() + call.vs_constants, kConstantBytes,
        kRootConstantAlign);
    const uint32_t ps_offset = g_constantRing.writeOnce(
        call.ps_constants, frame.arena.data() + call.ps_constants, kConstantBytes,
        kRootConstantAlign);
    // The shared block is built fresh per draw (it carries this draw's texture
    // descriptor indices), so it has no arena identity to deduplicate on.
    const uint32_t shared_offset =
        g_constantRing.write(shared, sizeof(shared), kRootConstantAlign);
    if (vertex_offset == kNoData || index_offset == kNoData || vs_offset == kNoData ||
        ps_offset == kNoData || shared_offset == kNoData) {
      ++skipped_space;
      continue;
    }

    if (is_grass) {
      ++g_grassSubmitted;
      // WHICH quadlist shader is actually covering the screen? The per-hash
      // dump below only reports the first 12 distinct shaders it happens to
      // see, which is not the same as the one drawing the most, and analysing
      // the wrong one costs a whole session. Rank them by triangles submitted.
      if (call.ps) {
        auto& stat = g_quadCensus[call.ps->hash];
        stat.first += 1;
        stat.second += call.index_count / 3;
      }
      static std::set<uint64_t> reported;
      if (reported.size() < 12 && call.ps && reported.insert(call.ps->hash).second) {
        std::string tex;
        for (uint32_t t = 0; t < kSamplerCount; ++t) {
          if (call.textures[t].valid()) {
            // Address, size and swizzle too: the format alone cannot be matched
            // against texdump/<addr>_WxH_fmtN.tga, and the swizzle decides
            // whether a two-channel k_8_8 fans out to RGB or leaves .b at 0.
            tex += fmt::format("s{}={:08X} {}x{} fmt{} swz{:03X} ", t,
                               call.textures[t].address(), call.textures[t].width(),
                               call.textures[t].height(), call.textures[t].format(),
                               call.textures[t].swizzle());
          }
        }
        // The GUEST hash is the container filename in shader_dump/, which is
        // what lets the translated HLSL be inspected offline. A host pointer
        // says nothing once the process exits.
        // Does the declaration even PROVIDE a colour? On Xenos a shader reading
        // an undeclared attribute gets a hardware default; in a D3D12 input
        // layout a missing element reads as ZERO -- and both shaders behind the
        // black surfaces multiply by iColor0.
        std::string usages;
        bool has_color = false;
        for (const HostElement& e : call.layout->elements) {
          usages += fmt::format("u{}.{} ", e.usage, e.usage_index);
          if (e.usage == 10) {  // D3DDECLUSAGE_COLOR
            has_color = true;
          }
        }
        REXLOG_INFO("[kameo-gfx] QUADLIST draw: vs-hash {:016X} ps-hash {:016X} blend {:08X} "
                    "depth {:08X} colormask {} stride {} COLOR-in-decl={} usages [{}] textures [{}]",
                    call.vs ? call.vs->hash : 0ull, call.ps ? call.ps->hash : 0ull,
                    call.blend_control, call.depth_control, call.color_mask,
                    call.layout->dst_stride, has_color ? "YES" : "NO", usages, tex);

        // THE measurement for "black grass". Both black shaders multiply by
        // iColor0, the shaders translate correctly, the geometry is drawn and
        // COLOR is declared -- so the fault is in the VALUES. Three sources can
        // supply a zero, and this separates them in one run.
        //
        // Note what the transcode can and cannot do: D3DCOLOR uses kSwap32,
        // which is a pure byte PERMUTATION. It cannot turn a nonzero colour
        // into zero -- only into the wrong channel order, which reads as a
        // wrong hue, not black. So all-zero bytes here prove the GUEST vertex
        // data is zero, and the colour must be arriving some other way (the
        // constant-colour override at 0x82B718A8, or a shader constant).
        // Every element, decoded the way the input assembler will hand it to
        // the shader -- not just COLOR. A UV that transcodes to a raw SHORT
        // (4096 rather than 0.25) samples far outside the texture and comes
        // back black, which is indistinguishable from a black texture unless
        // the numbers are actually looked at.
        const uint8_t* vtx = frame.arena.data() + call.vertex_data;
        static const char* kUsageNames[] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES",
                                            "NORMAL",   "PSIZE",       "TEXCOORD",
                                            "TANGENT",  "BINORMAL",    "TESSFACTOR",
                                            "POSITIONT", "COLOR",      "FOG",
                                            "DEPTH",    "SAMPLE"};
        for (const HostElement& e : call.layout->elements) {
          std::string verts;
          const uint32_t n = std::min<uint32_t>(3, call.vertex_count);
          for (uint32_t v = 0; v < n; ++v) {
            const size_t off = size_t(v) * call.layout->dst_stride + e.dst_offset;
            if (off + e.dst_size > call.vertex_bytes) {
              break;
            }
            const uint8_t* p = vtx + off;
            verts += "(";
            const bool as_float = (e.op == kShortToFloat || e.op == kUShortToFloat ||
                                   e.op == kByteToFloat || e.op == kDec3ToFloat ||
                                   e.format == plume::RenderFormat::R32_FLOAT ||
                                   e.format == plume::RenderFormat::R32G32_FLOAT ||
                                   e.format == plume::RenderFormat::R32G32B32_FLOAT ||
                                   e.format == plume::RenderFormat::R32G32B32A32_FLOAT);
            if (as_float) {
              for (uint32_t c = 0; c * 4 + 4 <= e.dst_size; ++c) {
                float f;
                std::memcpy(&f, p + 4 * c, 4);
                verts += fmt::format("{}{:.4f}", c ? "," : "", f);
              }
            } else {
              for (uint32_t b = 0; b < e.dst_size; ++b) {
                verts += fmt::format("{:02X}", p[b]);
              }
            }
            verts += ") ";
          }
          const char* name = e.usage < std::size(kUsageNames) ? kUsageNames[e.usage] : "?";
          REXLOG_INFO("[kameo-gfx]   {}{} (fmt {}, op {}, +{} size {}): {}", name, e.usage_index,
                      uint32_t(e.format), uint32_t(e.op), e.dst_offset, e.dst_size, verts);
        }

        // The other half of the fork: the constants the shaders blend against.
        // c0 is what the second black shader mixes toward; the VS registers are
        // dumped too because the PS only ever sees what the VERTEX shader wrote
        // to oColor0, so a VS constant can zero the colour just as easily as
        // the attribute being zero.
        auto dump_consts = [&](const char* what, uint32_t arena_off, uint32_t first,
                               uint32_t last) {
          if (arena_off == kNoData) {
            return;
          }
          const auto* c = reinterpret_cast<const float*>(frame.arena.data() + arena_off);
          std::string text;
          for (uint32_t r = first; r <= last && (r + 1) * 16 <= kConstantBytes; ++r) {
            text += fmt::format("c{}=({:.3f},{:.3f},{:.3f},{:.3f}) ", r, c[r * 4 + 0],
                                c[r * 4 + 1], c[r * 4 + 2], c[r * 4 + 3]);
          }
          REXLOG_INFO("[kameo-gfx]   {} {}", what, text);
        };
        dump_consts("PS", call.ps_constants, 0, 3);
        // c8 and c10 are the ones that matter for grass: the vertex shader
        // builds oColor0 (the PS lerp factor) from c8 and oTexCoord3 (the
        // tex0 multiplier) from c10, so a zero in either turns the pixel
        // shader's result negative or flat and the blade goes black.
        dump_consts("VS", call.vs_constants, 0, 11);
      }
    }
    list->setPipeline(pipeline);
    list->setGraphicsRootDescriptor(g_constantRing.buffer->at(vs_offset), 0);
    list->setGraphicsRootDescriptor(g_constantRing.buffer->at(ps_offset), 1);
    list->setGraphicsRootDescriptor(g_constantRing.buffer->at(shared_offset), 2);

    const plume::RenderVertexBufferView vertex_view(g_vertexRing.buffer->at(vertex_offset),
                                                    call.vertex_bytes);
    const plume::RenderInputSlot slot(0, call.vertex_stride);
    list->setVertexBuffers(0, &vertex_view, 1, &slot);

    const plume::RenderIndexBufferView index_view(g_indexRing.buffer->at(index_offset),
                                                  call.index_count * 4,
                                                  plume::RenderFormat::R32_UINT);
    list->setIndexBuffer(&index_view);

    // The guest's viewport, SCALED to the swap chain.
    //
    // The guest renders at its own video mode (720p) and the translated shaders
    // emit clip space, so the viewport transform is the host's to apply -- but
    // applying the guest's numbers verbatim pins every draw inside a 1280x720
    // rectangle in the corner of a larger window. That is invisible while the
    // client area happens to be 720p and glaring the moment it is not, because
    // the Bink quad is full-screen clip space and covers the whole window.
    //
    // Scale X and Y BY THE SAME FACTOR and centre the result, or the picture is
    // stretched to whatever shape the window happens to be. The guest's
    // projection is built for 16:9 and nothing downstream corrects for it, so a
    // 2576x1048 window widens everything by ~1.4x -- which looks like the camera
    // being wrong rather than the window being the wrong shape.
    float scale_x = offscreen ? 1.0f : float(width) / float(kGuestWidth);
    float scale_y = offscreen ? 1.0f : float(height) / float(kGuestHeight);
    float origin_x = 0.0f;
    float origin_y = 0.0f;
    if (!offscreen && REXCVAR_GET(kameo_gfx_preserve_aspect)) {
      const float uniform = std::min(scale_x, scale_y);
      origin_x = (float(width) - float(kGuestWidth) * uniform) * 0.5f;
      origin_y = (float(height) - float(kGuestHeight) * uniform) * 0.5f;
      scale_x = uniform;
      scale_y = uniform;
    }
    const float vp_width = (call.viewport[2] > 0 ? call.viewport[2] : float(kGuestWidth)) * scale_x;
    const float vp_height =
        (call.viewport[3] > 0 ? call.viewport[3] : float(kGuestHeight)) * scale_y;
    list->setViewports(plume::RenderViewport(origin_x + call.viewport[0] * scale_x,
                                             origin_y + call.viewport[1] * scale_y, vp_width,
                                             vp_height, call.viewport[4], call.viewport[5]));
    list->setScissors(offscreen ? plume::RenderRect(0, 0, vp_w, vp_h)
                                : plume::RenderRect(0, 0, width, height));

    list->drawIndexedInstanced(call.index_count, 1, 0, 0, 0);
    ++drawn;
  }

  if (current_key != 0) {
    list->setFramebuffer(framebuffer);
    list->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
    list->setScissors(plume::RenderRect(0, 0, width, height));
  }

  g_vertexRing.unmap();
  g_indexRing.unmap();
  g_constantRing.unmap();
  DrainD3D12Messages(ctx.device);

  if ((g_sceneFrames++ % 120) == 0) {
    REXLOG_INFO(
        "[kameo-gfx] scene: {} draws submitted of {} captured (skipped {} shader, {} pipeline, "
        "{} space; {} quadlist/{} drawn; {} depth-only ({} depth resolves, {} unmatched, "
        "{} scene resolves); "
        "capture dropped {} shader "
        "[{} untranslated, {} null-ps, {} no-vs], "
        "{} layout, {} other); {} restreamed vertex "
        "buffers; {} textures, {} samplers, "
        "{} pipelines; rings vtx {}/{} KB idx {}/{} KB cb {}/{} KB",
        drawn, frame.draws.size(), skipped_shader, skipped_pipeline, skipped_space,
        g_grassCaptured, g_grassSubmitted, g_captureDepthOnly, g_depthResolveDone,
        g_depthResolveMissed, g_resolveFromScene,
        g_captureSkippedNoShader,
        g_captureSkippedTranslation, g_captureSkippedNullPS,
        g_captureSkippedNoVSObject, g_captureSkippedNoLayout, g_captureSkippedOther,
        g_vertexReuseStale, g_textures.size() + g_resolveTargets.size(), g_samplers.size(),
        g_pipelines.size(),
        g_vertexRing.used >> 10,
        g_vertexRing.capacity >> 10, g_indexRing.used >> 10, g_indexRing.capacity >> 10,
        g_constantRing.used >> 10, g_constantRing.capacity >> 10);
    {
      static uint32_t reported_census = 0;
      if (g_formatCensus.size() != reported_census) {
        reported_census = uint32_t(g_formatCensus.size());
        std::string census;
        for (const auto& [fmt, hits] : g_formatCensus) {
          census += fmt::format("{}x{} ", fmt, hits);
        }
        REXLOG_INFO("[kameo-gfx] texture format census (xenos fmt x textures): {}", census);
      }
    }
    if (g_textureMiss.total()) {
      std::string formats;
      for (const auto& [fmt, hits] : g_unsupportedFormats) {
        formats += fmt::format(" fmt{}x{}", fmt, hits);
      }
      REXLOG_WARN("[kameo-gfx] texture slots unresolved: {} no object, {} no address, {} format, "
                  "{} create failed (fell back to magenta); unsupported:{} across {} distinct "
                  "textures",
                  g_textureMiss.no_object, g_textureMiss.no_address, g_textureMiss.bad_format,
                  g_textureMiss.create_failed, formats, g_unsupportedTextures.size());
      REXLOG_WARN("[kameo-gfx] {} cached textures are entirely zero (unrendered resolve targets)",
                  g_emptyTextures);
      g_textureMiss.reset();
      g_unsupportedFormats.clear();
    }
    if (g_vertexRing.exhausted || g_indexRing.exhausted || g_constantRing.exhausted) {
      REXLOG_WARN("[kameo-gfx] ring exhausted this frame: vtx {} idx {} cb {} draws dropped",
                  g_vertexRing.exhausted, g_indexRing.exhausted, g_constantRing.exhausted);
    }
    g_captureSkippedNoShader = 0;
    g_captureDepthOnly = 0;
    g_depthResolveDone = 0;
    g_depthResolveMissed = 0;
    g_resolveFromScene = 0;
    REXLOG_INFO("[kameo-gfx] capture cpu per 120 frames: vertices {} ms ({} MB), constants {} ms, "
                "indices {} ms, restream-hash {} ms; transcode cache {} hit / {} miss, {} MB live",
                g_nsVertex.load() / 1000000, g_captureBytes >> 20, g_nsConstants.load() / 1000000,
                g_nsIndices.load() / 1000000, g_nsHash.load() / 1000000, g_transcodeCacheHits,
                g_transcodeCacheMiss, g_transcodeCacheBytes >> 20);
    g_transcodeCacheHits = 0;
    g_transcodeCacheMiss = 0;
    g_nsVertex = 0;
    g_nsConstants = 0;
    g_nsIndices = 0;
    g_nsHash = 0;
    g_captureBytes = 0;
    {
      std::string census;
      for (const auto& [hash, stat] : g_quadCensus) {
        census += fmt::format("{:016X}={}draws/{}tris ", hash, stat.first, stat.second);
      }
      if (!census.empty()) {
        REXLOG_INFO("[kameo-gfx] quadlist census by pixel shader (120 frames): {}", census);
      }
      g_quadCensus.clear();
    }
    g_grassCaptured = 0;
    g_grassSubmitted = 0;
    g_captureSkippedTranslation = 0;
    g_captureSkippedNullPS = 0;
    g_captureSkippedNoVSObject = 0;
    g_captureSkippedNoLayout = 0;
    g_captureSkippedOther = 0;
    g_vertexReuseStale = 0;
  }
}

}  // namespace kameo::gfx
