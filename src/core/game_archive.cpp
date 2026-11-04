#include "core/game_archive.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <fstream>
#include <set>

#include "core/localization.h"
#include "launcher/kmod.h"

namespace kameo::game_archive {

namespace fs = std::filesystem;
using l10n::TrFormat;

namespace {

constexpr size_t kHeaderSize = 0x180;
constexpr uint32_t kFormatDxt1 = 18, kFormatDxt3 = 19, kFormatDxt5 = 20, kFormat8888 = 6, kFormat565 = 4, kFormat8 = 2, kFormat88 = 10;
constexpr uint32_t kTiledFlag = 0x100;

uint64_t Tiled(uint64_t x, uint64_t y, uint64_t width, uint32_t log2_bpp) {
  const uint64_t aligned = (width + 31) & ~uint64_t(31);
  const uint64_t macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (log2_bpp + 7);
  const uint64_t micro = ((x & 7) + ((y & 6) << 2)) << log2_bpp;
  const uint64_t offset = macro + ((micro & ~uint64_t(15)) << 1) + (micro & 15) + ((y & 8) << (3 + log2_bpp)) + ((y & 1) << 4);
  return (((offset & ~uint64_t(511)) << 3) + ((offset & 448) << 2) + (offset & 63) + ((y & 16) << 7) + ((((y & 8) >> 2) + (x >> 3)) & 3) * 64) >> log2_bpp;
}

void Swap(std::vector<uint8_t>& data, uint32_t endian) {
  if (endian == 1) {
    for (size_t i = 0; i + 1 < data.size(); i += 2) std::swap(data[i], data[i + 1]);
  } else if (endian == 2) {
    for (size_t i = 0; i + 3 < data.size(); i += 4) std::swap(data[i], data[i + 3]), std::swap(data[i + 1], data[i + 2]);
  }
}

void Expand(uint32_t c, int out[3]) {
  const int r = (c >> 8) & 0xF8, g = (c >> 3) & 0xFC, b = (c << 3) & 0xF8;
  out[0] = r | r >> 5, out[1] = g | g >> 6, out[2] = b | b >> 5;
}

void Colours(const uint8_t* b, bool four_colour, uint8_t texels[16][4]) {
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

void Alphas(const uint8_t* b, uint8_t texels[16][4]) {
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

}  // namespace

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

bool Archive::Load(const fs::path& path, std::string& error) {
  std::ifstream in(path, std::ios::binary);
  const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), {});
  if (file.size() < kHeaderSize || std::memcmp(file.data(), "CAFF", 4) != 0) {
    error = TrFormat("{0} is not a game archive", {path.filename().string()});
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
      error = TrFormat("{0} could not be unpacked", {path.filename().string()});
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

const uint8_t* Archive::At(Ref ref, size_t offset, size_t bytes) const {
  if (!ref || ref.entry > entries_.size()) return nullptr;
  const Entry& entry = entries_[ref.entry - 1];
  if (entry.section == 0 || entry.section > sections_.size()) return nullptr;
  const std::vector<uint8_t>& section = sections_[entry.section - 1];
  const uint64_t start = uint64_t(entry.offset) + ref.offset + offset;
  return ref.offset + offset + bytes <= entry.size && start + bytes <= section.size() ? section.data() + start : nullptr;
}

uint8_t Archive::U8(Ref ref, size_t offset) const {
  const uint8_t* p = At(ref, offset, 1);
  return p ? *p : 0;
}

uint16_t Archive::U16(Ref ref, size_t offset) const {
  const uint8_t* p = At(ref, offset, 2);
  return p ? Be16(p) : 0;
}

uint32_t Archive::U32(Ref ref, size_t offset) const {
  const uint8_t* p = At(ref, offset, 4);
  return p ? Be32(p) : 0;
}

float Archive::F32(Ref ref, size_t offset) const { return std::bit_cast<float>(U32(ref, offset)); }

Ref Archive::Pointer(Ref ref, size_t offset) const {
  const auto found = pointers_.find(uint64_t(ref.entry) << 32 | (ref.offset + offset));
  return found == pointers_.end() ? Ref{} : Ref{found->second, U32(ref, offset)};
}

std::string_view Archive::String(Ref ref) const {
  if (!ref || ref.entry > entries_.size()) return {};
  const Entry& entry = entries_[ref.entry - 1];
  const uint8_t* start = At(ref, 0, 1);
  if (!start) return {};
  const size_t limit = entry.size - ref.offset;
  const void* end = std::memchr(start, 0, limit);
  return {reinterpret_cast<const char*>(start), end ? size_t(static_cast<const uint8_t*>(end) - start) : limit};
}

std::string_view Archive::TypeName(uint32_t entry) const {
  const uint8_t* p = At({entry, 0}, 0, 1);
  if (!p) return {};
  const size_t limit = std::min<size_t>(entries_[entry - 1].size, 32);
  size_t length = 0;
  while (length < limit && p[length] >= 32 && p[length] < 127) ++length;
  return {reinterpret_cast<const char*>(p), length};
}

std::vector<uint32_t> Archive::Targets(uint32_t entry) const {
  if (targets_.empty()) {
    for (const auto& [key, target] : pointers_) {
      std::vector<uint32_t>& list = targets_[uint32_t(key >> 32)];
      if (std::find(list.begin(), list.end(), target) == list.end()) list.push_back(target);
    }
  }
  const auto found = targets_.find(entry);
  return found == targets_.end() ? std::vector<uint32_t>{} : found->second;
}

std::optional<Texture> ReadTexture(const Archive& archive, Ref ref) {
  const uint32_t format = archive.U32(ref, 24);
  const uint32_t gpu = format & 0x3F, endian = (format >> 6) & 3;
  const uint32_t width = archive.U16(ref, 36), height = archive.U16(ref, 38);
  const Ref data = archive.Pointer(ref, 40);
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
  if (archive.U32(ref, 28) != 0 || !data || width == 0 || height == 0 || width > 4096 || height > 4096) return std::nullopt;
  const uint32_t blocks_x = (width + block - 1) / block, blocks_y = (height + block - 1) / block;
  std::vector<uint8_t> flat(size_t(blocks_x) * blocks_y * block_bytes);
  const uint32_t log2_bpp = uint32_t(std::countr_zero(block_bytes));
  const uint32_t pitch = (blocks_x + 31) / 32 * 32;
  const auto copy = [&](uint32_t x, uint32_t y, uint64_t source_block) {
    if (const uint8_t* p = archive.At(data, size_t(source_block) * block_bytes, block_bytes)) std::memcpy(&flat[(size_t(y) * blocks_x + x) * block_bytes], p, block_bytes);
  };
  for (uint32_t y = 0; y < blocks_y; ++y)
    for (uint32_t x = 0; x < blocks_x; ++x) copy(x, y, format & kTiledFlag ? Tiled(x, y, blocks_x, log2_bpp) : uint64_t(y) * pitch + x);
  Swap(flat, endian);

  Texture out{width, height, std::vector<uint8_t>(size_t(width) * height * 4, 255)};
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

// The database is the first entry: +0x24 its type table (count, then items of name, 0, definition); a definition has its
// asset count at +68 and at +72 a list of asset records, each pointing at its entry from +8 (xmlblock.py).
std::vector<std::string> ObjectTypes(const Archive& archive) {
  std::vector<std::string> names;
  const Ref database{1, 0};
  if (archive.TypeName(1) != "kameoDatabase") return names;
  const Ref table = archive.Pointer(database, 0x24);
  const Ref items = table ? archive.Pointer(table, 4) : Ref{};
  for (uint32_t i = 0, count = items ? archive.U32(table) : 0; i < count; ++i) names.emplace_back(archive.String(archive.Pointer({items.entry, items.offset + 12 * i}, 0)));
  return names;
}

std::vector<uint32_t> TypeTextures(const Archive& archive, std::string_view type) {
  std::vector<uint32_t> textures;
  if (archive.TypeName(1) != "kameoDatabase") return textures;
  const Ref table = archive.Pointer({1, 0}, 0x24);
  const Ref items = table ? archive.Pointer(table, 4) : Ref{};
  std::vector<uint32_t> todo;
  for (uint32_t i = 0, count = items ? archive.U32(table) : 0; i < count; ++i) {
    const Ref item{items.entry, items.offset + 12 * i};
    if (archive.String(archive.Pointer(item, 0)) != type) continue;
    const Ref definition = archive.Pointer(item, 8);
    const Ref list = definition ? archive.Pointer(definition, 72) : Ref{};
    for (uint32_t a = 0, assets = list ? archive.U32(definition, 68) : 0; a < assets; ++a)
      if (const Ref asset = archive.Pointer(archive.Pointer(list, 4ull * a), 8)) todo.push_back(asset.entry);
  }
  // Everything the assets reach, short of the level's own database and level entries, which lead everywhere.
  std::set<uint32_t> seen;
  while (!todo.empty()) {
    const uint32_t entry = todo.back();
    todo.pop_back();
    if (!seen.insert(entry).second) continue;
    const std::string_view kind = archive.TypeName(entry);
    if (kind == "kameoDatabase" || kind == "kameolevel") continue;
    if (kind == "texture") {
      textures.push_back(entry);
      continue;
    }
    for (const uint32_t next : archive.Targets(entry)) todo.push_back(next);
  }
  std::sort(textures.begin(), textures.end());
  return textures;
}

}  // namespace kameo::game_archive
