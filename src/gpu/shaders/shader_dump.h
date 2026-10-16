#pragma once

// The inputs tools/shaders/build_shader_cache.py regenerates the translated shader cache from.

#include <cstdint>

namespace kameo::gpu {

// With --kameo_gfx_dump_shaders, writes the decompressed guest image (82000000-82BF0000) to guest_image.bin once per run.
void DumpGuestImageOnce(uint8_t* base);

// Writes a shader container to shader_dump/<hash>.bin when the cache lacks it, or always with --kameo_gfx_dump_shaders.
void DumpShaderContainer(uint8_t* base, uint32_t function, uint64_t bytes, uint64_t hash, bool in_cache);

}  // namespace kameo::gpu
