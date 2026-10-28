// zlib for .kmod sections, borrowed from stb: the launcher's only copy of stb_image and stb_image_write is compiled here.

#include <cstdint>
#include <vector>

#include "launcher/kmod.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb/stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

namespace kmod {

bool inflate(const uint8_t* source, size_t size, size_t expected, std::vector<uint8_t>& out) {
  int length = 0;
  char* data = stbi_zlib_decode_malloc_guesssize(reinterpret_cast<const char*>(source), int(size), int(expected ? expected : 16384), &length);
  if (!data) return false;
  out.assign(data, data + length);
  STBI_FREE(data);
  return true;
}

bool inflate_raw(const uint8_t* source, size_t size, size_t expected, std::vector<uint8_t>& out) {
  if (size > size_t(INT32_MAX) || expected > size_t(INT32_MAX)) return false;
  out.resize(expected);
  if (expected == 0) return true;
  const int length = stbi_zlib_decode_noheader_buffer(reinterpret_cast<char*>(out.data()), int(expected), reinterpret_cast<const char*>(source), int(size));
  return length == int(expected);
}

std::vector<uint8_t> deflate(const std::vector<uint8_t>& data) {
  int length = 0;
  unsigned char* packed = stbi_zlib_compress(const_cast<unsigned char*>(data.data()), int(data.size()), &length, 8);
  if (!packed) return {};
  std::vector<uint8_t> out(packed, packed + length);
  STBIW_FREE(packed);
  return out;
}

}  // namespace kmod
