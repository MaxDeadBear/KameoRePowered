// FP-exception guard: masks the SSE FP exceptions recompiled guest code raises, as the Xenon CPU runs with all of them masked.
#pragma once

#include <atomic>
#include <cstdint>

// Written by the handler on any thread, read by the debug UI.
extern std::atomic<uint64_t> g_kameo_fp_exception_count;
extern std::atomic<uint32_t> g_kameo_fp_last_code;
extern std::atomic<uint32_t> g_kameo_fp_last_mxcsr;
extern std::atomic<uint64_t> g_kameo_fp_last_rip;

#ifdef _WIN32
// Installs the vectored handler; call from OnPreSetup, before SDK setup.
void* InstallGuestFpExceptionHandlerWin();
#else
// Installs the SIGFPE handler; call from OnPostSetup so it replaces any SDK-installed one.
void* InstallGuestFpExceptionHandlerPosix();
#endif

void RemoveGuestFpExceptionHandler(void* handle);
