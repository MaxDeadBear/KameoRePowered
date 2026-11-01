#include "shader_editor/model.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <unordered_map>

#include "launcher/kmod.h"

namespace fs = std::filesystem;

namespace kameo::shader_editor {

namespace {

constexpr size_t kHeaderSize = 0x180;
constexpr uint32_t kPrimitiveTriangleList = 4, kPrimitiveTriangleStrip = 6;
constexpr uint32_t kUsagePosition = 0, kUsageNormal = 3, kUsageTexcoord = 5;
constexpr uint32_t kFormatDxt1 = 18, kFormatDxt3 = 19, kFormatDxt5 = 20, kFormat8888 = 6, kFormat565 = 4, kFormat8 = 2, kFormat88 = 10;
constexpr uint32_t kTiledFlag = 0x100;
constexpr int kMaxDepth = 64;
constexpr uint32_t kPaletteDynamic = 0xFFFF;
constexpr uint32_t kKindSampler = 3;

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

struct Entry {
  uint32_t offset = 0;
  uint32_t size = 0;
  uint8_t section = 0;
};

struct Ref {
  uint32_t entry = 0;  // 1-based, as the relocations name them; 0 is none
  uint32_t offset = 0;
  explicit operator bool() const { return entry != 0; }
};

// A CAFF archive's sections unpacked, with its relocated pointers (caff.py, graph.py).
class Archive {
 public:
  bool Load(const fs::path& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), {});
    if (file.size() < kHeaderSize || std::memcmp(file.data(), "CAFF", 4) != 0) {
      error = std::format("{} is not a game archive", path.filename().string());
      return false;
    }
    const uint8_t* header = file.data();
    size_t cursor = Be32(header + 0x38);
    sections_.resize(header[0x3D]);
    for (size_t i = 0; i < sections_.size(); ++i) {
      const uint8_t* item = header + 0x40 + 40 * i;
      const uint32_t size = Be32(item + 8 + 8), packed = Be32(item + 8 + 28);
      if (packed == 0) continue;
      if (cursor + packed > file.size() || !kmod::inflate(file.data() + cursor, packed, size, sections_[i])) {
        error = std::format("{} could not be unpacked", path.filename().string());
        return false;
      }
      cursor += packed;
    }
    if (sections_.empty()) return false;
    const std::vector<uint8_t>& table = sections_[0];
    size_t at = Be32(header + 0x34);
    const auto need = [&](size_t bytes) { return at + bytes <= table.size(); };
    if (!need(4)) return false;
    const uint32_t name_bytes = Be32(&table[at]);
    at += 4;
    if (name_bytes) at += 4ull * Be32(header + 0x18) + name_bytes;
    entries_.resize(Be32(header + 0x1C));
    if (!need(entries_.size() * 14)) return false;
    for (Entry& entry : entries_) {
      entry = {Be32(&table[at + 4]), Be32(&table[at + 8]), table[at + 12]};
      at += 14;
    }
    at += 12ull * Be32(header + 0x20) + 4ull * Be32(header + 0x24);
    const uint32_t groups = Be32(header + 0x28);
    if (!need(12ull * groups)) return false;
    std::vector<std::array<uint32_t, 3>> relocations(groups);
    for (auto& group : relocations) {
      group = {Be32(&table[at]), Be32(&table[at + 4]), Be32(&table[at + 8])};
      at += 12;
    }
    for (const auto& [source, target, count] : relocations) {
      if (!need(4ull * count)) return false;
      for (uint32_t i = 0; i < count; ++i, at += 4) pointers_[uint64_t(source) << 32 | Be32(&table[at])] = target;
    }
    return true;
  }

  size_t entry_count() const { return entries_.size(); }

  // The bytes at a reference, or null when it would run past its entry.
  const uint8_t* At(Ref ref, size_t offset, size_t bytes) const {
    if (!ref || ref.entry > entries_.size()) return nullptr;
    const Entry& entry = entries_[ref.entry - 1];
    if (entry.section == 0 || entry.section > sections_.size()) return nullptr;
    const std::vector<uint8_t>& section = sections_[entry.section - 1];
    const uint64_t start = uint64_t(entry.offset) + ref.offset + offset;
    return ref.offset + offset + bytes <= entry.size && start + bytes <= section.size() ? section.data() + start : nullptr;
  }
  uint8_t U8(Ref ref, size_t offset = 0) const {
    const uint8_t* p = At(ref, offset, 1);
    return p ? *p : 0;
  }
  uint16_t U16(Ref ref, size_t offset = 0) const {
    const uint8_t* p = At(ref, offset, 2);
    return p ? Be16(p) : 0;
  }
  uint32_t U32(Ref ref, size_t offset = 0) const {
    const uint8_t* p = At(ref, offset, 4);
    return p ? Be32(p) : 0;
  }
  float F32(Ref ref, size_t offset = 0) const { return std::bit_cast<float>(U32(ref, offset)); }
  Ref Pointer(Ref ref, size_t offset = 0) const {
    const auto found = pointers_.find(uint64_t(ref.entry) << 32 | (ref.offset + offset));
    return found == pointers_.end() ? Ref{} : Ref{found->second, U32(ref, offset)};
  }
  std::string_view String(Ref ref) const {
    if (!ref || ref.entry > entries_.size()) return {};
    const Entry& entry = entries_[ref.entry - 1];
    const uint8_t* start = At(ref, 0, 1);
    if (!start) return {};
    const size_t limit = entry.size - ref.offset;
    const void* end = std::memchr(start, 0, limit);
    return {reinterpret_cast<const char*>(start), end ? size_t(static_cast<const uint8_t*>(end) - start) : limit};
  }
  std::string_view TypeName(uint32_t entry) const {
    const uint8_t* p = At({entry, 0}, 0, 1);
    if (!p) return {};
    const size_t limit = std::min<size_t>(entries_[entry - 1].size, 32);
    size_t length = 0;
    while (length < limit && p[length] >= 32 && p[length] < 127) ++length;
    return {reinterpret_cast<const char*>(p), length};
  }

 private:
  std::vector<std::vector<uint8_t>> sections_;
  std::vector<Entry> entries_;
  std::unordered_map<uint64_t, uint32_t> pointers_;
};

float Half(uint16_t value) {
  const float sign = value & 0x8000 ? -1.0f : 1.0f;
  const int exponent = (value >> 10) & 0x1F, fraction = value & 0x3FF;
  if (exponent == 0) return sign * float(fraction) * std::ldexp(1.0f, -20);
  if (exponent == 31) return 0.0f;
  return sign * (1.0f + float(fraction) / 1024.0f) * std::ldexp(1.0f, exponent - 15);
}

// One vertex element as floats (mesh.py's DECL_TYPES), for the types positions, normals and UVs come in; false for any other.
bool Decode(const uint8_t* p, uint32_t type, float out[4]) {
  switch (type) {
    case 0x2C23A5:
      for (int i = 0; i < 2; ++i) out[i] = std::bit_cast<float>(Be32(p + 4 * i));
      return true;
    case 0x2A23B9:
      for (int i = 0; i < 3; ++i) out[i] = std::bit_cast<float>(Be32(p + 4 * i));
      return true;
    case 0x1A23A6:
      for (int i = 0; i < 4; ++i) out[i] = std::bit_cast<float>(Be32(p + 4 * i));
      return true;
    case 0x2C235F:
      for (int i = 0; i < 2; ++i) out[i] = Half(Be16(p + 2 * i));
      return true;
    case 0x1A2360:
      for (int i = 0; i < 4; ++i) out[i] = Half(Be16(p + 2 * i));
      return true;
    case 0x2C2359:
      for (int i = 0; i < 2; ++i) out[i] = float(int16_t(Be16(p + 2 * i)));
      return true;
    case 0x1A235A:
      for (int i = 0; i < 4; ++i) out[i] = float(int16_t(Be16(p + 2 * i)));
      return true;
    case 0x2C2159:
      for (int i = 0; i < 2; ++i) out[i] = float(int16_t(Be16(p + 2 * i))) / 32767.0f;
      return true;
    case 0x1A215A:
      for (int i = 0; i < 4; ++i) out[i] = float(int16_t(Be16(p + 2 * i))) / 32767.0f;
      return true;
    case 0x2C2059:
      for (int i = 0; i < 2; ++i) out[i] = float(Be16(p + 2 * i)) / 65535.0f;
      return true;
    case 0x2A2187:
    case 0x2A2190:
    case 0x2A2390: {
      const uint32_t packed = Be32(p);
      for (int i = 0; i < 3; ++i) {
        int part = (packed >> (10 * i)) & 0x3FF;
        if (part & 0x200) part -= 0x400;
        out[i] = std::max(float(part) / 511.0f, -1.0f);
      }
      return true;
    }
    default: return false;
  }
}

// A texture's RGBA, from texture.py: untiled, swapped to little endian, then decoded.
class TextureReader {
 public:
  explicit TextureReader(const Archive& archive) : archive_(archive) {}

  std::optional<PreviewTexture> Read(Ref ref) const {
    const uint32_t format = archive_.U32(ref, 24);
    const uint32_t gpu = format & 0x3F, endian = (format >> 6) & 3;
    const uint32_t width = archive_.U16(ref, 36), height = archive_.U16(ref, 38);
    const Ref data = archive_.Pointer(ref, 40);
    uint32_t block = 1, block_bytes = 0;
    switch (gpu) {
      case kFormatDxt1: block = 4, block_bytes = 8; break;
      case kFormatDxt3:
      case kFormatDxt5: block = 4, block_bytes = 16; break;
      case kFormat8888: block_bytes = 4; break;
      case kFormat565:
      case kFormat88: block_bytes = 2; break;
      case kFormat8: block_bytes = 1; break;
      default: return std::nullopt;
    }
    if (archive_.U32(ref, 28) != 0 || !data || width == 0 || height == 0 || width > 4096 || height > 4096) return std::nullopt;
    const uint32_t blocks_x = (width + block - 1) / block, blocks_y = (height + block - 1) / block;
    std::vector<uint8_t> flat(size_t(blocks_x) * blocks_y * block_bytes);
    const uint32_t log2_bpp = uint32_t(std::countr_zero(block_bytes));
    const uint32_t pitch = (blocks_x + 31) / 32 * 32;
    const auto copy = [&](uint32_t x, uint32_t y, uint64_t source_block) {
      if (const uint8_t* p = archive_.At(data, size_t(source_block) * block_bytes, block_bytes)) std::memcpy(&flat[(size_t(y) * blocks_x + x) * block_bytes], p, block_bytes);
    };
    for (uint32_t y = 0; y < blocks_y; ++y)
      for (uint32_t x = 0; x < blocks_x; ++x) copy(x, y, format & kTiledFlag ? Tiled(x, y, blocks_x, log2_bpp) : uint64_t(y) * pitch + x);
    Swap(flat, endian);

    PreviewTexture out{width, height, std::vector<uint8_t>(size_t(width) * height * 4, 255)};
    const auto put = [&](uint32_t x, uint32_t y, const uint8_t rgba[4]) {
      if (x < width && y < height) std::memcpy(&out.rgba[(size_t(y) * width + x) * 4], rgba, 4);
    };
    if (block == 4) {
      for (uint32_t by = 0; by < blocks_y; ++by) {
        for (uint32_t bx = 0; bx < blocks_x; ++bx) {
          const uint8_t* b = &flat[(size_t(by) * blocks_x + bx) * block_bytes];
          uint8_t texels[16][4];
          Colours(gpu == kFormatDxt1 ? b : b + 8, gpu != kFormatDxt1, texels);
          if (gpu == kFormatDxt3) {
            for (int i = 0; i < 16; ++i) texels[i][3] = uint8_t(((b[i / 2] >> (i % 2 * 4)) & 15) * 17);
          } else if (gpu == kFormatDxt5) {
            Alphas(b, texels);
          }
          for (uint32_t i = 0; i < 16; ++i) put(bx * 4 + i % 4, by * 4 + i / 4, texels[i]);
        }
      }
      return out;
    }
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        const uint8_t* p = &flat[(size_t(y) * width + x) * block_bytes];
        uint8_t rgba[4] = {p[0], p[0], p[0], 255};
        if (gpu == kFormat8888) {
          rgba[0] = p[2], rgba[1] = p[1], rgba[2] = p[0], rgba[3] = p[3];
        } else if (gpu == kFormat565) {
          const uint32_t c = p[0] | p[1] << 8;
          rgba[0] = uint8_t((c & 31) * 255 / 31), rgba[1] = uint8_t(((c >> 5) & 63) * 255 / 63), rgba[2] = uint8_t(((c >> 11) & 31) * 255 / 31);
        } else if (gpu == kFormat88) {
          rgba[3] = p[1];
        }
        put(x, y, rgba);
      }
    }
    return out;
  }

 private:
  static uint64_t Tiled(uint64_t x, uint64_t y, uint64_t width, uint32_t log2_bpp) {
    const uint64_t aligned = (width + 31) & ~uint64_t(31);
    const uint64_t macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (log2_bpp + 7);
    const uint64_t micro = ((x & 7) + ((y & 6) << 2)) << log2_bpp;
    const uint64_t offset = macro + ((micro & ~uint64_t(15)) << 1) + (micro & 15) + ((y & 8) << (3 + log2_bpp)) + ((y & 1) << 4);
    return (((offset & ~uint64_t(511)) << 3) + ((offset & 448) << 2) + (offset & 63) + ((y & 16) << 7) + ((((y & 8) >> 2) + (x >> 3)) & 3) * 64) >> log2_bpp;
  }

  static void Swap(std::vector<uint8_t>& data, uint32_t endian) {
    if (endian == 1) {
      for (size_t i = 0; i + 1 < data.size(); i += 2) std::swap(data[i], data[i + 1]);
    } else if (endian == 2) {
      for (size_t i = 0; i + 3 < data.size(); i += 4) std::swap(data[i], data[i + 3]), std::swap(data[i + 1], data[i + 2]);
    }
  }

  static void Expand(uint32_t c, int out[3]) {
    const int r = (c >> 8) & 0xF8, g = (c >> 3) & 0xFC, b = (c << 3) & 0xF8;
    out[0] = r | r >> 5, out[1] = g | g >> 6, out[2] = b | b >> 5;
  }

  static void Colours(const uint8_t* b, bool four_colour, uint8_t texels[16][4]) {
    const uint32_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
    int p[4][4] = {};
    Expand(c0, p[0]);
    Expand(c1, p[1]);
    const bool four = four_colour || c0 > c1;
    for (int c = 0; c < 3; ++c) {
      p[2][c] = four ? (2 * p[0][c] + p[1][c]) / 3 : (p[0][c] + p[1][c]) / 2;
      p[3][c] = four ? (p[0][c] + 2 * p[1][c]) / 3 : 0;
    }
    p[0][3] = p[1][3] = p[2][3] = 255;
    p[3][3] = four ? 255 : 0;
    const uint32_t bits = Be32(b + 4);
    const uint32_t swapped = std::byteswap(bits);
    for (int i = 0; i < 16; ++i)
      for (int c = 0; c < 4; ++c) texels[i][c] = uint8_t(p[(swapped >> (2 * i)) & 3][c]);
  }

  static void Alphas(const uint8_t* b, uint8_t texels[16][4]) {
    const int a0 = b[0], a1 = b[1];
    int palette[8] = {a0, a1};
    if (a0 > a1) {
      for (int i = 1; i < 7; ++i) palette[1 + i] = ((7 - i) * a0 + i * a1) / 7;
    } else {
      for (int i = 1; i < 5; ++i) palette[1 + i] = ((5 - i) * a0 + i * a1) / 5;
      palette[6] = 0, palette[7] = 255;
    }
    uint64_t bits = 0;
    for (int k = 0; k < 6; ++k) bits |= uint64_t(b[2 + k]) << (8 * k);
    for (int i = 0; i < 16; ++i) texels[i][3] = uint8_t(palette[(bits >> (3 * i)) & 7]);
  }

  const Archive& archive_;
};

struct Node {
  Ref ref;
  std::string_view type;
  std::vector<Node> children;
};

std::vector<Node> Walk(const Archive& a, Ref first, int depth = 0) {
  std::vector<Node> nodes;
  std::set<std::pair<uint32_t, uint32_t>> seen;
  while (first && seen.insert({first.entry, first.offset}).second) {
    Node node{first, a.String(a.Pointer(first, 0)), {}};
    if (depth < kMaxDepth) node.children = Walk(a, a.Pointer(first, 20), depth + 1);
    nodes.push_back(std::move(node));
    first = a.Pointer(first, 24);
  }
  return nodes;
}

// The colour sampler and colour constant of an ndShaderParam2 (material.py).
struct Material {
  int palette = -1;
  float colour[3] = {1, 1, 1};
  float alpha_reference = 0;
};

Material ReadMaterial(const Archive& a, const Node& node) {
  Material material;
  const Ref pass_set = a.Pointer(node.ref, 48);
  const Ref passes = pass_set ? a.Pointer(pass_set, 4) : Ref{};
  const Ref params = passes ? a.Pointer(passes, 0) : Ref{};
  if (!params) return material;
  material.alpha_reference = float(a.U8(params, 24)) / 255.0f;
  const Ref samplers = a.Pointer(params, 16);
  const uint32_t sampler_count = samplers ? a.U8(params, 20) : 0;
  const Ref colours = a.Pointer(params, 0);
  const uint32_t colour_count = colours ? a.U16(params, 22) : 0;
  const Ref names = a.Pointer(params, 76);
  const uint32_t name_count = names ? a.U32(params, 80) : 0;
  bool found_colour = false;
  for (uint32_t i = 0; i < name_count; ++i) {
    const Ref item{names.entry, names.offset + 16 * i};
    const std::string_view name = a.String(a.Pointer(item, 0));
    const uint32_t slot = a.U32(item, 8), kind = a.U32(item, 12);
    if (!name.starts_with("colour") || name.find_first_not_of("0123456789", 6) != std::string_view::npos) continue;
    if (kind == kKindSampler && slot < sampler_count && material.palette < 0) {
      const uint32_t index = a.U32({samplers.entry, samplers.offset + 36 * slot}, 0);
      if (index <= kPaletteDynamic) material.palette = int(index);
    } else if (kind == 2 && slot < colour_count && !found_colour) {
      const Ref colour{colours.entry, colours.offset + 20 * slot};
      for (int c = 0; c < 3; ++c) material.colour[c] = a.F32(colour, 4 * c);
      found_colour = true;
    }
  }
  return material;
}

// One rendergraph's geometry, flattened.
class Builder {
 public:
  Builder(const Archive& archive, Ref graph) : a_(archive), graph_(graph) {}

  void Visit(const std::vector<Node>& nodes, std::optional<uint32_t> base, const Node* material) {
    for (const Node& node : nodes) {
      std::optional<uint32_t> owner = base;
      const Node* owner_material = material;
      if (node.type == "ndVertexBuffer2" || node.type == "ndBlendShape2") owner = ReadVertices(node);
      else if (node.type == "ndShaderParam2" || node.type == "ndShaderParam2WithVisibility") owner_material = &node;
      else if (node.type == "ndPushBuffer" && base) ReadDraws(node, *base, material);
      Visit(node.children, owner, owner_material);
    }
  }

  PreviewModel Take(const TextureReader& reader) {
    const Ref palette = a_.Pointer(graph_, 84);
    std::map<int, int> loaded;
    for (auto& [part, palette_index] : wanted_) {
      if (palette_index < 0 || !palette) continue;
      const auto found = loaded.find(palette_index);
      if (found != loaded.end()) {
        model_.parts[part].texture = found->second;
        continue;
      }
      std::optional<PreviewTexture> texture = reader.Read(a_.Pointer(palette, 4ull * palette_index));
      const int index = texture ? int(model_.textures.size()) : -1;
      if (texture) model_.textures.push_back(std::move(*texture));
      loaded[palette_index] = index;
      model_.parts[part].texture = index;
    }
    return std::move(model_);
  }

  size_t vertices() const { return model_.vertices.size(); }

 private:
  std::optional<uint32_t> ReadVertices(const Node& node) {
    const Ref variants = a_.Pointer(node.ref, 52);
    const Ref variant = variants ? a_.Pointer(a_.Pointer(variants, 4), 0) : Ref{};
    const Ref declaration = variant ? a_.Pointer(variant, 28) : Ref{};
    if (!declaration) return std::nullopt;
    const float position_scale = a_.U16(variant, 18) != 0xFFFF ? a_.F32(variant, 52) : 1.0f;
    const float uv_scale = a_.U16(variant, 20) != 0xFFFF ? a_.F32(variant, 56) : 1.0f;
    const Ref stream_indices = a_.Pointer(variant, 0);
    const Ref stream_table = a_.Pointer(node.ref, 48);

    const uint32_t base = uint32_t(model_.vertices.size());
    size_t count = 0;
    for (uint32_t i = 0; i < 64; ++i) {
      const Ref item{declaration.entry, declaration.offset + 12 * i};
      const uint32_t stream = a_.U16(item, 0);
      if (stream == 0xFF || stream == 0xFFFF) break;
      const uint32_t usage = a_.U8(item, 9), usage_index = a_.U8(item, 10);
      if (usage_index != 0 || (usage != kUsagePosition && usage != kUsageNormal && usage != kUsageTexcoord)) continue;
      const uint32_t record_index = stream_indices ? a_.U8(stream_indices, stream) : stream;
      const Ref record{stream_table.entry, stream_table.offset + 20 * record_index};
      const uint32_t stride = a_.U32(record, 0), size = a_.U32(record, 16);
      const Ref data = a_.Pointer(record, 12);
      if (!data || stride == 0) continue;
      const uint32_t offset = a_.U16(item, 2), type = a_.U32(item, 4);
      const size_t vertices = size / stride;
      if (model_.vertices.size() < base + vertices) model_.vertices.resize(base + vertices, PreviewVertex{{0, 0, 0}, {0, 0, 1}, {0, 0}});
      count = std::max(count, vertices);
      for (size_t v = 0; v < vertices; ++v) {
        const uint8_t* p = a_.At(data, v * stride + offset, 4);
        float out[4] = {};
        if (!p || !Decode(p, type, out)) continue;
        PreviewVertex& vertex = model_.vertices[base + v];
        if (usage == kUsagePosition) {
          for (int c = 0; c < 3; ++c) vertex.position[c] = out[c] * position_scale;
        } else if (usage == kUsageNormal) {
          const float length = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
          if (length > 1e-6f) {
            for (int c = 0; c < 3; ++c) vertex.normal[c] = out[c] / length;
          }
        } else {
          vertex.uv[0] = out[0] * uv_scale, vertex.uv[1] = out[1] * uv_scale;
        }
      }
    }
    if (count == 0) return std::nullopt;
    return base;
  }

  void ReadDraws(const Node& node, uint32_t base, const Node* material) {
    const Ref ref = node.ref;
    const Material m = material ? ReadMaterial(a_, *material) : Material{};
    const Ref types = a_.Pointer(ref, 64), counts = a_.Pointer(ref, 68), data = a_.Pointer(ref, 60);
    for (uint32_t i = 0, draws = a_.U32(ref, 48); i < draws; ++i) {
      const uint32_t primitive = a_.U32(types, 4 * i), primitives = a_.U32(counts, 4 * i);
      const Ref index_data = a_.Pointer(data, 4 * i);
      const uint32_t index_count = primitive == kPrimitiveTriangleList ? primitives * 3 : primitives + 2;
      if ((primitive != kPrimitiveTriangleList && primitive != kPrimitiveTriangleStrip) || !index_data) continue;
      PreviewPart part;
      part.first = uint32_t(model_.indices.size());
      std::memcpy(part.colour, m.colour, sizeof(part.colour));
      part.alpha_reference = m.alpha_reference;
      const auto index = [&](uint32_t n) { return a_.U16(index_data, 2ull * n); };
      if (primitive == kPrimitiveTriangleList) {
        for (uint32_t n = 0; n < index_count; ++n) model_.indices.push_back(base + index(n));
      } else {
        for (uint32_t n = 0; n + 2 < index_count; ++n) {
          const uint32_t x = index(n), y = index(n + 1), z = index(n + 2);
          if (x == y || y == z || x == z) continue;
          model_.indices.insert(model_.indices.end(), {base + x, base + (n % 2 ? z : y), base + (n % 2 ? y : z)});
        }
      }
      part.count = uint32_t(model_.indices.size()) - part.first;
      if (part.count == 0) continue;
      wanted_.emplace_back(model_.parts.size(), m.palette);
      model_.parts.push_back(part);
    }
  }

  const Archive& a_;
  Ref graph_;
  PreviewModel model_;
  std::vector<std::pair<size_t, int>> wanted_;
};

}  // namespace

bool LoadPreviewModel(const fs::path& file, int graph_index, PreviewModel& model, std::string& error) {
  Archive archive;
  if (!archive.Load(file, error)) {
    if (error.empty()) error = std::format("{} could not be read", file.filename().string());
    return false;
  }
  const TextureReader reader(archive);
  size_t best = 0;
  int position = -1;
  for (uint32_t entry = 1; entry <= archive.entry_count(); ++entry) {
    if (archive.TypeName(entry) != "rendergraph") continue;
    ++position;
    if (graph_index >= 0 && position != graph_index) continue;
    const Ref graph{entry, 0};
    std::vector<Node> roots;
    const Ref list = archive.Pointer(graph, 36);
    for (uint32_t i = 0, count = list ? archive.U32(graph, 40) : 0; i < count; ++i) {
      std::vector<Node> nodes = Walk(archive, archive.Pointer(list, 4ull * i));
      roots.insert(roots.end(), std::make_move_iterator(nodes.begin()), std::make_move_iterator(nodes.end()));
    }
    Builder builder(archive, graph);
    builder.Visit(roots, std::nullopt, nullptr);
    if (builder.vertices() <= best) continue;
    best = builder.vertices();
    model = builder.Take(reader);
  }
  if (model.vertices.empty()) {
    error = std::format("{} has no model in it", file.filename().string());
    return false;
  }
  for (int c = 0; c < 3; ++c) model.min[c] = 1e30f, model.max[c] = -1e30f;
  for (const uint32_t index : model.indices) {
    if (index >= model.vertices.size()) continue;
    for (int c = 0; c < 3; ++c) {
      model.min[c] = std::min(model.min[c], model.vertices[index].position[c]);
      model.max[c] = std::max(model.max[c], model.vertices[index].position[c]);
    }
  }
  return true;
}

}  // namespace kameo::shader_editor
