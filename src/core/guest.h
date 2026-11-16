// Guest-memory helpers shared by the hook translation units, on top of the generated guest-function declarations.

#pragma once

#include "kameorepowered_init.h"

#include <rex/kernel/thread_state.h>
#include <rex/kernel/title_runtime.h>

// Returns the host pointer to the start of the guest virtual address space,
// or nullptr while no title is loaded.
inline uint8_t* GuestBase() {
  rex::kernel::TitleRuntime* runtime = REX_RUNTIME();
  return runtime && runtime->IsLoaded() ? runtime->GetMembase().GetBase() : nullptr;
}

// Returns a pointer to the PPC register context of the current guest thread,
// or nullptr on a host thread.
inline PPCContext* CurrentGuestContext() {
  auto* ts = rex::kernel::ThreadState::Get();
  if (!ts) return nullptr;
  return ts->GetContext();
}

// The address space of the calling guest thread, for the heap queries the checked GuestMemory does not offer; null on
// a host thread.
inline rex::kernel::GuestAddressSpace* CurrentGuestAddressSpace() {
  auto* ts = rex::kernel::ThreadState::Get();
  return ts ? ts->GetAddressSpace() : nullptr;
}

// Zeroed guest memory from the runtime's system heap, which is never freed; 0 when there is none.
inline uint32_t GuestSystemAlloc(uint32_t bytes) {
  rex::kernel::GuestMemory* memory = REX_GUEST_MEMORY();
  if (!memory) return 0;
  const auto address = memory->Allocate(bytes, 16, rex::kernel::GuestProtect::kReadWrite, rex::kernel::GuestHeap::kSystem);
  return address ? *address : 0;
}

// Guest physical memory (the 0xA0000000 view, 64 KiB pages) from the runtime rather than the game's own pools; 0 when
// there is none.
inline uint32_t GuestPhysicalAlloc(uint32_t bytes) {
  rex::kernel::GuestMemory* memory = REX_GUEST_MEMORY();
  if (!memory) return 0;
  const auto address = memory->Allocate(bytes, 4096, rex::kernel::GuestProtect::kReadWrite, rex::kernel::GuestHeap::kPhysical);
  return address ? *address : 0;
}

// Whether every byte of a guest range is mapped and readable, by the runtime's page tables.
inline bool GuestRangeMapped(uint32_t address, uint32_t bytes) {
  rex::kernel::GuestMemory* memory = REX_GUEST_MEMORY();
  if (!memory || bytes == 0 || address < 0x10000 || address > 0xFFFFFFFFu - bytes) return false;
  return static_cast<bool>(memory->GetReadOnlySpan(address, bytes));
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
