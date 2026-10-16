#include "gpu/vertex_declaration.h"

#include <deque>
#include <map>
#include <set>

#include <plume_render_interface.h>

#include "gpu/draw/geometry_cache.h"
#include "gpu/guest_memory.h"
#include "gpu/log.h"
#include "gpu/pipeline/pso_precache.h"

namespace kameo::gpu::detail {
namespace {

// XenosRecomp declares BLENDINDICES/NORMAL/TANGENT/BINORMAL as uint4 and the rest as float4, and DXGI has no SSCALED/USCALED formats, so non-normalised integers feeding a float input are converted during the capture copy.
plume::RenderFormat FloatFormat(uint32_t components) {
  switch (components) {
    case 1: return plume::RenderFormat::R32_FLOAT;
    case 2: return plume::RenderFormat::R32G32_FLOAT;
    case 3: return plume::RenderFormat::R32G32B32_FLOAT;
    default: return plume::RenderFormat::R32G32B32A32_FLOAT;
  }
}

// Builds the host layout for one guest vertex declaration.
bool BuildHostLayout(uint8_t* base, uint32_t decl, HostLayout* out) {
  const uint32_t count = Load32(base, decl + 8);
  if (count == 0 || count > 32) {
    return false;
  }

  uint32_t offset = 0;
  for (uint32_t e = 0; e < count; ++e) {
    // Xbox's _D3DVERTEXELEMENT9 is 12 bytes, not PC D3D9's 8: Type is a 32-bit XGVERTEXFORMAT rather than a WORD.
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
      KGFX_WARN("unhandled vertex element type {:08X} (format {}) in decl {:08X}",
                  type, t.data_format, decl);
      return false;
    }
    h.src_size = t.size;
    h.components = t.components;

    // XenosRecomp emits tfetchR11G11B10 for every NORMAL/TANGENT/BINORMAL from the USAGE alone, and the spec constant fixes that decode to DEC3N, so any other declared format silently becomes a garbage tangent frame.
    const bool normal_frame = h.usage == kUsageNormal || h.usage == kUsageTangent || h.usage == kUsageBinormal;
    const bool normal_frame_supported = (t.packed && t.data_format == 7) ||
                                        (t.is_float && t.component_bits == 32 && t.components >= 3);
    if (normal_frame && !normal_frame_supported) {
      static std::set<uint32_t> reported_normal_format;
      if (reported_normal_format.insert(type).second) {
        KGFX_WARN("{} declared as unsupported vertex type {:08X} (format {}) for the shader's DEC3N input",
                    DeclUsageSemantic(h.usage), type, t.data_format);
      }
    }

    const bool integer_input = DeclUsageIsInteger(h.usage);
    if (integer_input) {
      // uint4 in the shader: keep the bits, pick a UINT format of the same shape.
      if (normal_frame && t.is_float && t.component_bits == 32 && t.components >= 3) {
        h.op = kFloat3ToDec3;
        h.format = plume::RenderFormat::R32_UINT;
        h.dst_size = 4;
        h.components = 1;
      } else if (t.packed && t.data_format == 7 &&
          (h.usage == kUsageNormal || h.usage == kUsageTangent || h.usage == kUsageBinormal)) {
        // XenosRecomp decodes DEC3N natively (SPEC_CONSTANT_DEC3N_NORMAL), so the raw dword passes through; the decode and this must change together.
        h.op = kSwap32;
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
        // DEC3N / UDEC3 feeding a float input: plume has no packed 10:10:10 format, so it is unpacked during the copy.
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
        // Measured: dropping this swap made the storybook page pinker (+15 -> +92); UBYTE4N arrives as a packed big-endian dword like D3DCOLOR.
        h.op = kSwap32;
        h.format = sn ? plume::RenderFormat::R8G8B8A8_SNORM : plume::RenderFormat::R8G8B8A8_UNORM;
        h.dst_size = 4;
      }
    } else {
      // Non-normalised integer feeding a float input: no DXGI format converts these, so convert on the way in.
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

    out->elements.push_back(h);
  }

  if (out->elements.empty()) {
    return false;
  }
  out->dst_stride = offset;
  out->key = PipelineLayoutKey(*out);
  out->valid = true;
  return true;
}

// Keyed on the guest address and verified against the declaration's contents, because the guest recycles a declaration's address for the next mesh.
// Superseded layouts are kept: a published frame's draws point at one and the render thread reads it a frame later.
std::deque<HostLayout> g_layoutStorage;
std::map<uint32_t, HostLayout*> g_layouts;

}  // namespace

const HostLayout* GetLayout(uint8_t* base, uint32_t decl) {
  // Consecutive draws overwhelmingly share a declaration, and std::map nodes never move.
  const uint32_t count = Load32(base, decl + 8);
  const uint64_t signature = (count && count <= 32) ? MixGeometryKey(GeometryContentHash(GuestData(base, decl + 36), size_t(count) * 12), count) : 0;
  static uint32_t last_decl = 0;
  static uint64_t last_signature = 0;
  static const HostLayout* last_layout = nullptr;
  if (decl == last_decl && signature == last_signature && last_layout) return last_layout->valid ? last_layout : nullptr;
  auto found = g_layouts.find(decl);
  if (found == g_layouts.end() || found->second->decl_signature != signature) {
    HostLayout& layout = g_layoutStorage.emplace_back();
    BuildHostLayout(base, decl, &layout);
    layout.decl_signature = signature;
    found = g_layouts.insert_or_assign(decl, &layout).first;
  }
  last_decl = decl;
  last_signature = signature;
  last_layout = found->second;
  return found->second->valid ? found->second : nullptr;
}

}  // namespace kameo::gpu::detail
