// Guest-memory helpers shared by the hook translation units, on top of the generated guest-function declarations.

#pragma once

#include "kameorepowered_init.h"

// Returns the host pointer to the start of the guest virtual address space,
// or nullptr if the memory subsystem is not yet initialised.
inline uint8_t* GuestBase() {
  auto* memory = rex::system::kernel_state()->memory();
  return memory ? memory->virtual_membase() : nullptr;
}

// Returns a pointer to the PPC register context of the current guest thread,
// or nullptr if the thread state or context is not available.
inline PPCContext* CurrentGuestContext() {
  auto* ts = rex::runtime::ThreadState::Get();
  if (!ts) return nullptr;
  return ts->context();
}

// Writes a NUL-terminated string into the guest virtual address space at
// `guest_addr`. The caller must ensure `guest_addr` points to sufficient space.
// `base` must be the result of GuestBase() — REX_STORE_U8 requires it in scope.
inline void WriteGuestStringAt(uint8_t* base, uint32_t guest_addr, const char* text) {
  (void)base;  // kept in scope for the REX_STORE_U8 macro expansion
  for (uint32_t i = 0;; ++i) {
    REX_STORE_U8(guest_addr + i, static_cast<uint8_t>(text[i]));
    if (text[i] == '\0') break;
  }
}
