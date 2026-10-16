#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <kameorepowered_init.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace kameo::gpu::detail {

// Always the raw guest address: REX_PHYS_HOST_OFFSET already applies the physical page bias, and applying it twice reads the wrong page.
inline const uint8_t* GuestData(uint8_t* base, uint32_t addr) { return REX_RAW_ADDR(addr); }

#ifndef _WIN32
// The runtime's own page tables answer this where the host has no cheap query; its Linux one rereads the whole process map.
inline bool GuestPagesReadable(const uint8_t* p, size_t bytes) {
  auto* memory = rex::system::kernel_state()->memory();
  if (p == nullptr || bytes == 0 || p < memory->virtual_membase()) return false;
  const uint64_t guest = uint64_t(p - memory->virtual_membase());
  if (guest < 0x10000 || guest + bytes > 0x100000000ull) return false;
  rex::memory::BaseHeap* heap = memory->LookupHeap(uint32_t(guest));
  return heap && heap->QueryRangeAccess(uint32_t(guest), uint32_t(guest + bytes - 1)) != rex::memory::PageAccess::kNoAccess;
}
#endif

// The guest address space is one flat reservation, so base + addr is always a valid pointer but not always committed memory; a mip chain into an uncommitted page faulted on the first read.
inline bool GuestRangeReadable(const uint8_t* p, size_t bytes) {
#ifdef _WIN32
  if (p == nullptr || bytes == 0) {
    return false;
  }
  const uint8_t* end = p + bytes;
  while (p < end) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(p, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT) {
      return false;
    }
    constexpr DWORD kNoRead = PAGE_NOACCESS | PAGE_GUARD;
    if ((info.Protect & kNoRead) != 0 || info.Protect == 0) {
      return false;
    }
    p = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
  }
  return true;
#else
  return GuestPagesReadable(p, bytes);
#endif
}

inline uint32_t Load32(uint8_t* base, uint32_t addr) { return REX_LOAD_U32(addr); }
inline uint16_t Load16(uint8_t* base, uint32_t addr) { return REX_LOAD_U16(addr); }
inline uint8_t Load8(uint8_t* base, uint32_t addr) { return REX_LOAD_U8(addr); }

inline float LoadFloat(uint8_t* base, uint32_t addr) {
  const uint32_t bits = REX_LOAD_U32(addr);
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

inline uint16_t Swap16(uint16_t v) { return __builtin_bswap16(v); }
inline uint32_t Swap32(uint32_t v) { return __builtin_bswap32(v); }

}  // namespace kameo::gpu::detail
