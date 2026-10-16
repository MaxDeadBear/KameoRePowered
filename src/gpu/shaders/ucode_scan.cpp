#include "gpu/shaders/ucode_scan.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <string_view>

#include <kameorepowered_init.h>

namespace kameo::gpu {

ShaderMaterialRegisters ScanMaterialRegisters(uint8_t* base, uint32_t function, uint32_t virtual_size) {
  ShaderMaterialRegisters found;
  const uint32_t table_offset = REX_LOAD_U32(function + 16);
  if (!table_offset || table_offset + 32 > virtual_size) return found;
  const uint32_t table = function + table_offset + 4;
  const uint32_t table_size = REX_LOAD_U32(table);
  const uint32_t constants = REX_LOAD_U32(table + 12);
  const uint32_t infos = REX_LOAD_U32(table + 16);
  if (constants > 512 || infos + constants * 20 > table_size) return found;
  for (uint32_t i = 0; i < constants; ++i) {
    const uint32_t info = table + infos + i * 20;
    const uint32_t name_offset = REX_LOAD_U32(info);
    if (name_offset >= table_size || REX_LOAD_U16(info + 4) != 2) continue;  // RegisterSet::Float4
    const uint16_t reg = REX_LOAD_U16(info + 6);
    const char* name = reinterpret_cast<const char*>(base + table + name_offset);
    const std::string_view view(name, strnlen(name, std::min<size_t>(64, table_size - name_offset)));
    if (view == "c_power") {
      found.specular_power = int16_t(reg);
    } else if ((view.starts_with("c_texbem_sampler") || view == "c_icetexbem" || view == "c_oiltexbem") && found.bump_count < 4 && reg < 256) {
      found.bump[found.bump_count++] = uint8_t(reg);
    }
  }
  return found;
}

// Field positions are XenosRecomp's ControlFlowExecInstruction and AluInstruction (thirdparty/XenosRecomp/XenosRecomp/shader_code.h), which translated these shaders.
ShaderConstantRange ScanShaderConstantRange(const uint8_t* ucode, uint32_t bytes) {
  const ShaderConstantRange full;
  if (bytes < 12 || (bytes % 4) != 0) return full;
  const uint32_t dwords = bytes / 4;
  auto dword = [&](uint32_t index) -> uint32_t { uint32_t v; std::memcpy(&v, ucode + index * 4, 4); return std::byteswap(v); };
  // A control flow pair is 3 dwords, and the instruction stream begins at the lowest address any exec jumps to.
  uint32_t instr_bytes = bytes;
  for (uint32_t pass = 0; pass < 2; ++pass) {
    uint32_t lo = 256;
    uint32_t hi = 0;
    for (uint32_t at = 0; at + 12 <= instr_bytes; at += 12) {
      const uint32_t base = at / 4;
      if (base + 3 > dwords) return full;
      const uint32_t c0 = dword(base);
      const uint32_t c1 = dword(base + 1);
      const uint32_t c2 = dword(base + 2);
      const uint32_t pair[2][2] = {{c0, c1 & 0xFFFF}, {(c1 >> 16) | (c2 << 16), c2 >> 16}};
      for (const auto& cf : pair) {
        const uint32_t opcode = (cf[1] >> 12) & 0xF;
        if (!((opcode >= 1 && opcode <= 6) || opcode == 13 || opcode == 14)) continue;
        const uint32_t address = cf[0] & 0xFFF;
        if (address == 0) continue;
        if (pass == 0) {
          instr_bytes = std::min(instr_bytes, address * 12);
          continue;
        }
        const uint32_t count = (cf[0] >> 12) & 0x7;
        uint32_t sequence = (cf[0] >> 16) & 0xFFF;
        for (uint32_t i = 0; i < count; ++i, sequence >>= 2) {
          const uint32_t instr = (address + i) * 3;
          if (instr + 3 > dwords) return full;
          if ((sequence & 1) != 0) continue;  // a fetch instruction reads fetch constants, not these
          const uint32_t w1 = dword(instr + 1);
          const uint32_t w2 = dword(instr + 2);
          if ((w1 & 0xC0000000u) != 0) return full;  // relative addressing can reach any register
          for (uint32_t src = 0; src < 3; ++src) {
            if ((w2 & (1u << (31 - src))) != 0) continue;  // a temp register, not a constant
            const uint32_t reg = (w2 >> (16 - src * 8)) & 0xFF;
            lo = std::min(lo, reg);
            hi = std::max(hi, reg + 1);
          }
        }
      }
    }
    if (pass == 1) return hi > lo ? ShaderConstantRange{uint16_t(lo), uint16_t(hi)} : ShaderConstantRange{0, 0};
  }
  return full;
}

}  // namespace kameo::gpu
