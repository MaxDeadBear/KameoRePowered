#include "launcher/kmod.h"

#include <algorithm>
#include <cstring>

namespace kmod {
namespace {

constexpr uint32_t kHeaderSize = 0x180;
constexpr uint32_t kVersion = 1;
constexpr uint32_t kRunGap = 64;

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void putBe32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }

uint32_t headerChecksum(const uint8_t* header) {
  uint32_t value = 0;
  for (uint32_t i = 0; i < kHeaderSize; i++) {
    const int8_t byte = (i >= 0x14 && i < 0x18) ? 0 : int8_t(header[i]);
    value = (value << 4) + uint32_t(int32_t(byte));
    if (const uint32_t top = value & 0xF0000000) value ^= (top >> 24) | top;
  }
  return value;
}

// A CAFF archive's header and its sections unpacked.
bool unpack(const std::vector<uint8_t>& file, std::vector<std::vector<uint8_t>>& sections, std::string& error) {
  if (file.size() < kHeaderSize || memcmp(file.data(), "CAFF", 4) != 0) return error = "not a CAFF archive", false;
  const uint8_t* h = file.data();
  size_t cursor = be32(h + 0x38);
  const uint32_t count = h[0x3D];
  if (cursor < kHeaderSize || count < 1 || count > (kHeaderSize - 0x40) / 40) return error = "a CAFF header out of range", false;
  sections.assign(count, {});
  for (uint32_t i = 0; i < count; i++) {
    const uint8_t* s = h + 0x40 + 40 * i;
    const uint32_t size = be32(s + 16), packed = be32(s + 36);
    if (cursor + packed > file.size()) return error = "a section runs past the end of the file", false;
    if (size && !inflate(file.data() + cursor, packed, size, sections[i])) return error = "a section does not unpack", false;
    if (sections[i].size() != size) return error = "a section unpacks to the wrong size", false;
    cursor += packed;
  }
  return true;
}

uint64_t hashSections(const std::vector<std::vector<uint8_t>>& sections) {
  uint64_t value = 1469598103934665603ull;
  for (const auto& s : sections) {
    for (uint8_t b : s) value = (value ^ b) * 1099511628211ull;
    value = (value ^ 0xFF) * 1099511628211ull;
  }
  return value;
}

struct Writer {
  std::vector<uint8_t> out;
  void u32(uint32_t v) { for (int i = 0; i < 4; i++) out.push_back(uint8_t(v >> (8 * i))); }
  void u64(uint64_t v) { for (int i = 0; i < 8; i++) out.push_back(uint8_t(v >> (8 * i))); }
  void bytes(const std::vector<uint8_t>& b) { u32(uint32_t(b.size())), out.insert(out.end(), b.begin(), b.end()); }
  void text(const std::string& s) { u32(uint32_t(s.size())), out.insert(out.end(), s.begin(), s.end()); }
};

struct Reader {
  const uint8_t* data;
  size_t size, at = 0;
  bool ok = true;
  bool need(size_t n) { return ok = ok && at + n <= size; }
  uint32_t u32() {
    if (!need(4)) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= uint32_t(data[at + size_t(i)]) << (8 * i);
    at += 4;
    return v;
  }
  uint64_t u64() {
    const uint64_t lo = u32();
    return lo | uint64_t(u32()) << 32;
  }
  std::vector<uint8_t> bytes() {
    const uint32_t n = u32();
    if (!need(n)) return {};
    std::vector<uint8_t> v(data + at, data + at + n);
    at += n;
    return v;
  }
  std::string text() {
    const std::vector<uint8_t> b = bytes();
    return std::string(b.begin(), b.end());
  }
};

}  // namespace

size_t FilePatch::bytes() const {
  size_t total = header.size();
  for (const SectionPatch& s : sections)
    for (const Run& r : s.runs) total += r.bytes.size() + 8;
  return total;
}

uint64_t hash(const uint8_t* data, size_t size) {
  uint64_t value = 1469598103934665603ull;
  for (size_t i = 0; i < size; i++) value = (value ^ data[i]) * 1099511628211ull;
  return value;
}

bool diff(const std::string& name, const std::vector<uint8_t>& original, const std::vector<uint8_t>& edited, FilePatch& out, std::string& error) {
  std::vector<std::vector<uint8_t>> before, after;
  if (!unpack(original, before, error) || !unpack(edited, after, error)) return false;
  if (before.size() != after.size()) return error = "the archives have different sections", false;
  out = {};
  out.name = name, out.baseSize = original.size(), out.baseHash = hash(original.data(), original.size()), out.resultHash = hashSections(after);
  out.header.assign(edited.begin(), edited.begin() + kHeaderSize);
  for (size_t s = 0; s < after.size(); s++) {
    const std::vector<uint8_t>& a = before[s];
    const std::vector<uint8_t>& b = after[s];
    SectionPatch patch;
    patch.size = uint32_t(b.size());
    // Runs of changed bytes, those closer than kRunGap joined.
    size_t i = 0;
    while (i < b.size()) {
      if (i < a.size() && a[i] == b[i]) {
        i++;
        continue;
      }
      size_t end = i + 1, same = 0;
      while (end < b.size() && same < kRunGap) {
        same = end < a.size() && a[end] == b[end] ? same + 1 : 0;
        end++;
      }
      end -= same;
      patch.runs.push_back({uint32_t(i), std::vector<uint8_t>(b.begin() + ptrdiff_t(i), b.begin() + ptrdiff_t(end))});
      i = end;
    }
    out.sections.push_back(std::move(patch));
  }
  return true;
}

bool apply(const FilePatch& patch, const std::vector<uint8_t>& original, std::vector<uint8_t>& out, std::string& error) {
  if (original.size() != patch.baseSize || hash(original.data(), original.size()) != patch.baseHash) return error = patch.name + " is not the file the mod was made from", false;
  if (patch.header.size() != kHeaderSize) return error = "the mod's header is damaged", false;
  std::vector<std::vector<uint8_t>> sections;
  if (!unpack(original, sections, error)) return false;
  if (sections.size() != patch.sections.size() || patch.header[0x3D] != sections.size()) return error = "the mod does not match the archive's sections", false;
  for (size_t s = 0; s < sections.size(); s++) {
    sections[s].resize(patch.sections[s].size, 0);
    for (const Run& r : patch.sections[s].runs) {
      if (size_t(r.offset) + r.bytes.size() > sections[s].size()) return error = "the mod writes past a section", false;
      memcpy(sections[s].data() + r.offset, r.bytes.data(), r.bytes.size());
    }
  }
  if (hashSections(sections) != patch.resultHash) return error = "the patched archive is not what the mod's author saved", false;
  out.assign(patch.header.begin(), patch.header.end());
  out.resize(be32(out.data() + 0x38), 0);
  for (size_t s = 0; s < sections.size(); s++) {
    const std::vector<uint8_t> packed = sections[s].empty() ? std::vector<uint8_t>() : deflate(sections[s]);
    putBe32(out.data() + 0x40 + 40 * s + 16, uint32_t(sections[s].size()));
    putBe32(out.data() + 0x40 + 40 * s + 36, uint32_t(packed.size()));
    out.insert(out.end(), packed.begin(), packed.end());
  }
  putBe32(out.data() + 0x14, headerChecksum(out.data()));
  return true;
}

std::vector<uint8_t> write(const Package& package) {
  Writer body;
  body.text(package.manifest.name), body.text(package.manifest.author), body.text(package.manifest.version), body.text(package.manifest.description);
  body.u32(uint32_t(package.files.size()));
  for (const FilePatch& f : package.files) {
    body.text(f.name), body.u64(f.baseSize), body.u64(f.baseHash), body.u64(f.resultHash), body.bytes(f.header);
    body.u32(uint32_t(f.sections.size()));
    for (const SectionPatch& s : f.sections) {
      body.u32(s.size), body.u32(uint32_t(s.runs.size()));
      for (const Run& r : s.runs) body.u32(r.offset), body.bytes(r.bytes);
    }
  }
  Writer out;
  out.out = {'K', 'M', 'O', 'D'};
  out.u32(kVersion), out.u32(uint32_t(body.out.size()));
  const std::vector<uint8_t> packed = deflate(body.out);
  out.out.insert(out.out.end(), packed.begin(), packed.end());
  return out.out;
}

bool read(const std::vector<uint8_t>& bytes, Package& out, std::string& error) {
  if (bytes.size() < 12 || memcmp(bytes.data(), "KMOD", 4) != 0) return error = "not a Kameo mod package", false;
  Reader head{bytes.data(), bytes.size(), 4};
  if (head.u32() != kVersion) return error = "a mod package of another version", false;
  const uint32_t size = head.u32();
  std::vector<uint8_t> body;
  if (!inflate(bytes.data() + 12, bytes.size() - 12, size, body) || body.size() != size) return error = "the mod package is damaged", false;
  Reader r{body.data(), body.size()};
  out = {};
  out.manifest.name = r.text(), out.manifest.author = r.text(), out.manifest.version = r.text(), out.manifest.description = r.text();
  const uint32_t files = r.u32();
  for (uint32_t i = 0; r.ok && i < files; i++) {
    FilePatch f;
    f.name = r.text(), f.baseSize = r.u64(), f.baseHash = r.u64(), f.resultHash = r.u64(), f.header = r.bytes();
    const uint32_t sections = r.u32();
    for (uint32_t s = 0; r.ok && s < sections && s < 16; s++) {
      SectionPatch patch;
      patch.size = r.u32();
      const uint32_t runs = r.u32();
      for (uint32_t k = 0; r.ok && k < runs; k++) {
        Run run;
        run.offset = r.u32(), run.bytes = r.bytes();
        patch.runs.push_back(std::move(run));
      }
      f.sections.push_back(std::move(patch));
    }
    out.files.push_back(std::move(f));
  }
  if (!r.ok) return error = "the mod package is damaged", false;
  for (const FilePatch& f : out.files)
    if (f.name.empty() || f.name.find_first_of("/\\:") != std::string::npos || f.name.find("..") != std::string::npos) return error = "the mod package names a file outside the game folder", false;
  return true;
}

}  // namespace kmod
