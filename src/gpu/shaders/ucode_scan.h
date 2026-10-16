#pragma once

#include <cstdint>

#include "gpu/shaders/shader_registry.h"

namespace kameo::gpu {

// Reads a shader container's D3DX constant table in guest memory for the material constants; offsets inside the table are relative to its own start.
ShaderMaterialRegisters ScanMaterialRegisters(uint8_t* base, uint32_t function, uint32_t virtual_size);

// Which float constant registers a shader's ALU instructions read, from big-endian Xenos microcode; [0, 256) whenever relative addressing or a malformed stream defeats the scan.
ShaderConstantRange ScanShaderConstantRange(const uint8_t* ucode, uint32_t bytes);

}  // namespace kameo::gpu
