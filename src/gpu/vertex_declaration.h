#pragma once

#include <cstdint>

#include "gpu/draw/draw_types.h"

namespace kameo::gpu::detail {

// The host layout for a guest vertex declaration, or null when the input assembler cannot express it.
const HostLayout* GetLayout(uint8_t* base, uint32_t decl);

}  // namespace kameo::gpu::detail
