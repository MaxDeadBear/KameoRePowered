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

#include "core/game_archive.h"
#include "core/localization.h"

namespace fs = std::filesystem;

namespace kameo::shader_editor {

using l10n::TrFormat;

namespace {

constexpr uint32_t kPrimitiveTriangleList = 4, kPrimitiveTriangleStrip = 6;
constexpr uint32_t kUsagePosition = 0, kUsageNormal = 3, kUsageTexcoord = 5;
constexpr int kMaxDepth = 64;
constexpr uint32_t kPaletteDynamic = 0xFFFF;
constexpr uint32_t kKindSampler = 3;

using game_archive::Archive;
using game_archive::Be16;
using game_archive::Be32;
using game_archive::Ref;

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

  PreviewModel Take() {
    const Ref palette = a_.Pointer(graph_, 84);
    std::map<int, int> loaded;
    for (auto& [part, palette_index] : wanted_) {
      if (palette_index < 0 || !palette) continue;
      const auto found = loaded.find(palette_index);
      if (found != loaded.end()) {
        model_.parts[part].texture = found->second;
        continue;
      }
      std::optional<game_archive::Texture> texture = game_archive::ReadTexture(a_, a_.Pointer(palette, 4ull * palette_index));
      const int index = texture ? int(model_.textures.size()) : -1;
      if (texture) model_.textures.push_back({texture->width, texture->height, std::move(texture->rgba)});
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
    if (error.empty()) error = TrFormat("{0} could not be read", {file.filename().string()});
    return false;
  }
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
    model = builder.Take();
  }
  if (model.vertices.empty()) {
    error = TrFormat("{0} has no model in it", {file.filename().string()});
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
