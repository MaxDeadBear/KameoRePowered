// Kameo mod packages (.kmod): level archive edits stored as differences from the shipped files, so a package holds none of the game's data.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace kmod {

struct Manifest {
  std::string name, author, version, description;
};

struct Run {
  uint32_t offset = 0;
  std::vector<uint8_t> bytes;
};

struct SectionPatch {
  uint32_t size = 0;
  std::vector<Run> runs;
};

struct FilePatch {
  std::string name;
  uint64_t baseSize = 0, baseHash = 0, resultHash = 0;
  std::vector<uint8_t> header;
  std::vector<SectionPatch> sections;
  size_t bytes() const;
};

struct Package {
  Manifest manifest;
  std::vector<FilePatch> files;
};

uint64_t hash(const uint8_t* data, size_t size);
bool diff(const std::string& name, const std::vector<uint8_t>& original, const std::vector<uint8_t>& edited, FilePatch& out, std::string& error);
bool apply(const FilePatch& patch, const std::vector<uint8_t>& original, std::vector<uint8_t>& out, std::string& error);
std::vector<uint8_t> write(const Package& package);
bool read(const std::vector<uint8_t>& bytes, Package& out, std::string& error);

// The launcher provides these in launcher/kmod_zlib.cpp.
bool inflate(const uint8_t* source, size_t size, size_t expected, std::vector<uint8_t>& out);
// A raw deflate stream (no zlib header), as a zip entry stores it, into exactly `expected` bytes.
bool inflate_raw(const uint8_t* source, size_t size, size_t expected, std::vector<uint8_t>& out);
std::vector<uint8_t> deflate(const std::vector<uint8_t>& data);

}  // namespace kmod
