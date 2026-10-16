#include "gpu/draw/vertex_capture.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <unordered_map>

#include "gpu/config.h"
#include "gpu/draw/capture.h"
#include "gpu/draw/geometry_cache.h"
#include "gpu/graphics_system.h"
#include "gpu/guest_device.h"
#include "gpu/guest_memory.h"
#include "gpu/vertex_transcode.h"

namespace kameo::gpu::detail {
namespace {

// A copy made earlier this frame, keyed on source buffer, window and layout and validated by the hash of the source bytes it was made from.
struct VertexReuse {
  uint32_t offset = kNoData;  // arena offset, when the copy went to the arena
  std::shared_ptr<const std::vector<uint8_t>> blob;  // ...or the shared buffer it came from
  uint64_t hash = 0;
};
std::unordered_map<uint64_t, VertexReuse> g_vertexReuse;

// The same transcoded bytes kept across frames, validated by a content hash, so level geometry is not re-transcoded every frame.
struct TranscodeCacheEntry {
  // Shared with every frame that drew from it; a restream replaces the pointer rather than writing into the vector.
  std::shared_ptr<const std::vector<uint8_t>> bytes;
  uint64_t hash = 0;
  uint64_t last_frame = 0;
  size_t size() const { return bytes ? bytes->size() : 0; }
};
std::unordered_map<uint64_t, TranscodeCacheEntry> g_transcodeCache;

// The render thread's half of kameo_gfx_defer_vertices: the same per-frame reuse and cross-frame cache, owned by it alone.
std::unordered_map<uint64_t, VertexReuse> g_renderVertexReuse;
std::unordered_map<uint64_t, TranscodeCacheEntry> g_renderTranscodeCache;
uint64_t g_renderVertexFrame = 0;

}  // namespace

bool CaptureVertices(uint8_t* base, uint32_t device, const HostLayout& layout, uint32_t min_vertex, uint32_t max_vertex, DrawFrame* frame, uint32_t* out_offset, uint32_t* out_bytes, uint32_t* out_base, uint32_t* out_count, std::shared_ptr<const std::vector<uint8_t>>* out_blob, const float* viewport, const StreamOverride& override_stream, uint32_t* out_source) {
  out_blob->reset();  // only the two reuse paths set it; a transcode uses the arena
  // Streams are the same for every element, so they are resolved once.
  VertexStream streams[16];
  uint32_t stream_objects[16] = {};
  uint32_t stream_addresses[16] = {};

  for (const HostElement& e : layout.elements) {
    if (e.stream >= 16) {
      return false;
    }
    if (streams[e.stream].data != nullptr) {
      continue;
    }
    if (override_stream.active() && e.stream == 0) {
      if (uint64_t(override_stream.stride) * override_stream.count > UINT32_MAX) {
        return false;
      }
      streams[0].data = override_stream.data;
      streams[0].stride = override_stream.stride;
      streams[0].size = override_stream.stride * override_stream.count;
      continue;
    }
    const uint32_t buffer = Load32(base, device + dev::kStreamBuffer + 8 * e.stream);
    if (!buffer) {
      return false;
    }
    // A vertex buffer's address is in its fetch constant at +12 (the low two bits are the fetch type) with the size in dwords at +16, not in BaseFlush as for textures.
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
    const uint32_t buffer_bytes = ((fetch1 >> 2) & 0xFFFFFFu) * 4;
    if (byte_offset >= buffer_bytes || uint64_t(address) + buffer_bytes > (uint64_t(1) << 32)) {
      return false;
    }
    streams[e.stream].data = GuestData(base, address + byte_offset);
    stream_objects[e.stream] = buffer;
    stream_addresses[e.stream] = address + byte_offset;
    streams[e.stream].stride = stride;
    streams[e.stream].size = buffer_bytes - byte_offset;
  }

  // How many vertices every bound element can supply, so a whole-buffer copy stays inside all of them.
  uint32_t whole = 0xFFFFFFFFu;
  for (const HostElement& e : layout.elements) {
    VertexStream& stream = streams[e.stream];
    const uint32_t capacity = VertexStreamCapacity(stream.size, stream.stride, e.src_offset, e.src_size);
    if (!stream.data || capacity <= max_vertex || min_vertex > max_vertex) {
      return false;
    }
    whole = std::min(whole, capacity);
    stream.element_end = std::max(stream.element_end, e.src_offset + e.src_size);
  }

  // One copy per buffer per frame, not one per draw.
  constexpr uint32_t kWholeBufferByteCap = 16u << 20;
  uint32_t first = min_vertex;
  uint32_t count = max_vertex - min_vertex + 1;
  if (whole != 0xFFFFFFFFu && whole > max_vertex && layout.dst_stride != 0 &&
      uint64_t(whole) * layout.dst_stride <= kWholeBufferByteCap) {
    first = 0;
    count = whole;
  }
  *out_base = first;
  *out_count = count;

  // Reuse an identical copy made earlier this frame, keyed on the source rather than the draw; never for DrawVerticesUP, whose scratch buffer is rewritten before every draw (reusing it collapsed ~148 glyphs into one quad).
  const uint32_t bytes = count * layout.dst_stride;
  const bool reusable = !override_stream.active();
  uint64_t reuse_key = MixGeometryKey(MixGeometryKey(layout.key, first), count);
  bool screen_space_layout = false;
  for (const HostElement& e : layout.elements) {
    screen_space_layout = screen_space_layout ||
                          (e.usage == kUsagePosition && e.components == 2 && e.dst_size >= 8);
  }
  if (reusable && screen_space_layout && viewport) {
    for (uint32_t i = 0; i < 4; ++i) {
      reuse_key = MixGeometryKey(reuse_key, uint32_t(std::lround(viewport[i])));
    }
  }
  if (reusable && out_source && REXCVAR_GET(kameo_gfx_defer_vertices)) {
    DeferredVertexSource source;
    source.layout = &layout;
    source.reuse_key = reuse_key;
    for (uint32_t i = 0; i < 4; ++i) source.viewport[i] = viewport ? viewport[i] : 0.0f;
    source.first = first;
    source.count = count;
    uint32_t seen = 0;
    bool fits = true;
    for (const HostElement& e : layout.elements) {
      if (seen & (1u << e.stream)) continue;
      seen |= 1u << e.stream;
      if (source.stream_count == kDeferredStreams) {
        fits = false;
        break;
      }
      const VertexStream& stream = streams[e.stream];
      source.streams[source.stream_count++] = {e.stream, stream_addresses[e.stream], stream.stride, stream.size, stream.element_end};
    }
    if (fits) {
      for (uint32_t i = 0; i < source.stream_count; ++i) {
        g_frameGuestReads.push_back(CanonicalGuestAddress(stream_objects[source.streams[i].index]));
        g_frameGuestReads.push_back(CanonicalGuestAddress(source.streams[i].address));
      }
      *out_source = frame->alloc(sizeof(DeferredVertexSource), 8);
      std::memcpy(frame->at(*out_source), &source, sizeof(source));
      *out_offset = kNoData;
      *out_bytes = bytes;
      return true;
    }
  }
  uint64_t source_hash = 0;
  if (reusable) {
    // Hash what would be copied: a pointer only says where the vertices came from, and streaming puts a different mesh at the same address.
    source_hash = HashVertexWindow(layout, streams, first, count, g_spanHashes, g_captureFrame, &reuse_key);
    auto reused = g_vertexReuse.find(reuse_key);
    if (reused != g_vertexReuse.end()) {
      if (reused->second.hash == source_hash) {
        *out_offset = reused->second.offset;
        *out_blob = reused->second.blob;
        *out_bytes = bytes;
        return true;
      }
      // Same buffer, different contents: the guest restreamed it.
    }

    // Not seen yet THIS frame -- but very likely transcoded in an earlier one.
    auto cached = g_transcodeCache.find(reuse_key);
    if (cached != g_transcodeCache.end() && cached->second.hash == source_hash &&
        cached->second.size() == bytes) {
      cached->second.last_frame = g_captureFrame;
      g_vertexReuse.insert_or_assign(reuse_key,
                                     VertexReuse{kNoData, cached->second.bytes, source_hash});
      *out_offset = kNoData;
      *out_blob = cached->second.bytes;
      *out_bytes = bytes;
      return true;
    }
  }

  const uint32_t offset = frame->alloc(bytes, 16);
  uint8_t* dst_base = frame->at(offset);

  if (!TranscodeVertexWindow(layout, streams, first, count, dst_base, viewport)) {
    return false;
  }

  if (reusable) {
    g_vertexReuse.insert_or_assign(reuse_key, VertexReuse{offset, nullptr, source_hash});
    TranscodeCacheEntry& entry = g_transcodeCache[reuse_key];
    // A new buffer every time: a published frame may still be drawing from the old one.
    entry.bytes = std::make_shared<const std::vector<uint8_t>>(dst_base, dst_base + bytes);
    entry.hash = source_hash;
    entry.last_frame = g_captureFrame;
  }
  *out_offset = offset;
  *out_bytes = bytes;
  return true;
}

size_t EvictTranscodeCache(uint64_t frame) {
  size_t live = 0;
  for (auto it = g_transcodeCache.begin(); it != g_transcodeCache.end();) {
    if (frame - it->second.last_frame > 240) {
      it = g_transcodeCache.erase(it);
    } else {
      live += it->second.size();
      ++it;
    }
  }
  return live;
}

void ClearVertexReuse() { g_vertexReuse.clear(); }

void ReleaseVertexCaptureCaches() {
  g_vertexReuse.clear();
  g_transcodeCache.clear();
}

void ResolveDeferredVertexSources(DrawFrame& frame) {
  uint8_t* base = frame.guest_base;
  if (base == nullptr) return;
  ++g_renderVertexFrame;
  g_renderVertexReuse.clear();
  for (DrawCall& call : frame.draws) {
    if (call.vertex_source == kNoData) continue;
    DeferredVertexSource source;
    std::memcpy(&source, frame.at(call.vertex_source), sizeof(source));
    call.vertex_source = kNoData;
    VertexStream streams[16];
    for (uint32_t i = 0; i < source.stream_count; ++i) {
      const DeferredVertexSource::Stream& s = source.streams[i];
      streams[s.index] = {GuestData(base, s.address), s.stride, s.size, s.element_end};
    }
    const uint32_t bytes = source.count * source.layout->dst_stride;
    uint64_t reuse_key = source.reuse_key;
    const uint64_t hash = HashVertexWindow(*source.layout, streams, source.first, source.count, g_renderSpanHashes, g_renderVertexFrame, &reuse_key);
    auto reused = g_renderVertexReuse.find(reuse_key);
    if (reused != g_renderVertexReuse.end()) {
      if (reused->second.hash == hash) {
        call.vertex_blob = reused->second.blob;
        continue;
      }
    }
    auto cached = g_renderTranscodeCache.find(reuse_key);
    if (cached != g_renderTranscodeCache.end() && cached->second.hash == hash && cached->second.size() == bytes) {
      cached->second.last_frame = g_renderVertexFrame;
      g_renderVertexReuse.insert_or_assign(reuse_key, VertexReuse{kNoData, cached->second.bytes, hash});
      call.vertex_blob = cached->second.bytes;
      continue;
    }
    auto bytes_out = std::make_shared<std::vector<uint8_t>>(bytes);
    if (!TranscodeVertexWindow(*source.layout, streams, source.first, source.count, bytes_out->data(), source.viewport)) std::fill(bytes_out->begin(), bytes_out->end(), uint8_t(0));
    std::shared_ptr<const std::vector<uint8_t>> blob = std::move(bytes_out);
    g_renderVertexReuse.insert_or_assign(reuse_key, VertexReuse{kNoData, blob, hash});
    TranscodeCacheEntry& entry = g_renderTranscodeCache[reuse_key];
    entry.bytes = blob;
    entry.hash = hash;
    entry.last_frame = g_renderVertexFrame;
    call.vertex_blob = std::move(blob);
  }
  // Same rule as the guest thread's cache: anything not drawn for a few seconds goes.
  if ((g_renderVertexFrame % 240) == 0) {
    size_t live = 0;
    for (auto it = g_renderTranscodeCache.begin(); it != g_renderTranscodeCache.end();) {
      if (g_renderVertexFrame - it->second.last_frame > 240) {
        it = g_renderTranscodeCache.erase(it);
      } else {
        live += it->second.size();
        ++it;
      }
    }
  }
}

}  // namespace kameo::gpu::detail
