#pragma once

// Binds guest shader objects to their translated cache entries when the guest creates them.

#include <cstdint>

// Declared in the generated shader_cache.h, at global scope.
struct ShaderCacheEntry;

namespace kameo::gpu {

// The half-open float constant register range a translated shader can read, scanned out of its microcode at creation; [0, 256) whenever the scan cannot prove a narrower one.
struct ShaderConstantRange {
  uint16_t lo = 0;
  uint16_t hi = 256;
};

// Registers of the material constants a pixel shader's constant table names: c_power, the specular exponent, and the texbem bump-environment scales.
struct ShaderMaterialRegisters {
  int16_t specular_power = -1;
  uint8_t bump_count = 0;
  uint8_t bump[4] = {};
};

// Hashes the authored container the guest is creating a shader from and binds the returned shader object to its cache entry.
void RegisterShader(uint8_t* base, uint32_t function, uint32_t object, const char* what);

// Resolves a guest shader object to its translated cache entry, or null when its container was not in the cache.
const ShaderCacheEntry* LookupShaderBinding(uint32_t guest_object);
ShaderConstantRange ShaderConstantRangeOf(const ShaderCacheEntry* entry);
ShaderMaterialRegisters ShaderMaterialRegistersOf(const ShaderCacheEntry* entry);

}  // namespace kameo::gpu
