// The runtime sets FZ+DAZ in MXCSR but leaves the exception mask bits clear, so an inexact, overflow or underflow result faults on the host; the handler masks them in the live MXCSR and resumes.

#include "core/fp_guard.h"

#ifdef _WIN32
#include <Windows.h>
#else
#include <csignal>
#include <ucontext.h>
#endif
#include <xmmintrin.h>

std::atomic<uint64_t> g_kameo_fp_exception_count{0};
std::atomic<uint32_t> g_kameo_fp_last_code{0};
std::atomic<uint32_t> g_kameo_fp_last_mxcsr{0};
std::atomic<uint64_t> g_kameo_fp_last_rip{0};

namespace {

#ifdef _WIN32
LONG WINAPI GuestFpExceptionHandler(EXCEPTION_POINTERS* ep) {
    switch (ep->ExceptionRecord->ExceptionCode) {
        case 0xC000008C:  // STATUS_FLOAT_DIVIDE_BY_ZERO
        case 0xC000008D:  // STATUS_FLOAT_OVERFLOW
        case 0xC000008E:  // STATUS_FLOAT_UNDERFLOW
        case 0xC000008F:  // STATUS_FLOAT_INEXACT_RESULT
        case 0xC0000090:  // STATUS_FLOAT_INVALID_OPERATION
        case 0xC0000091:  // STATUS_FLOAT_STACK_CHECK
            g_kameo_fp_exception_count.fetch_add(1, std::memory_order_relaxed);
            g_kameo_fp_last_code.store(ep->ExceptionRecord->ExceptionCode,
                                       std::memory_order_release);
            g_kameo_fp_last_mxcsr.store(ep->ContextRecord->MxCsr,
                                        std::memory_order_release);
            g_kameo_fp_last_rip.store(
                reinterpret_cast<uint64_t>(ep->ExceptionRecord->ExceptionAddress),
                std::memory_order_release);
            ep->ContextRecord->MxCsr |= _MM_MASK_MASK;
            return EXCEPTION_CONTINUE_EXECUTION;
        default:
            return EXCEPTION_CONTINUE_SEARCH;
    }
}
#else
void GuestFpExceptionHandler(int /*sig*/, siginfo_t* si, void* ctx) {
    switch (si->si_code) {
        case FPE_FLTDIV:
        case FPE_FLTOVF:
        case FPE_FLTUND:
        case FPE_FLTRES:
        case FPE_FLTINV:
        case FPE_FLTSUB: {
            auto* uc = static_cast<ucontext_t*>(ctx);
            const uint32_t mxcsr = uc->uc_mcontext.fpregs->mxcsr;
            g_kameo_fp_exception_count.fetch_add(1, std::memory_order_relaxed);
            g_kameo_fp_last_code.store(static_cast<uint32_t>(si->si_code),
                                       std::memory_order_release);
            g_kameo_fp_last_mxcsr.store(mxcsr, std::memory_order_release);
            g_kameo_fp_last_rip.store(
                static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RIP]),
                std::memory_order_release);
            // The status bits (0-5) are cleared too, or the next FP op faults again at once.
            uc->uc_mcontext.fpregs->mxcsr = (mxcsr & ~0x003Fu) | _MM_MASK_MASK;
            break;
        }
        default:
            break;
    }
}
#endif

}  // namespace

#ifdef _WIN32
void* InstallGuestFpExceptionHandlerWin() {
    return AddVectoredExceptionHandler(1, GuestFpExceptionHandler);
}
#else
void* InstallGuestFpExceptionHandlerPosix() {
    struct sigaction sa{};
    sa.sa_sigaction = GuestFpExceptionHandler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    auto* old_sa = new struct sigaction{};
    sigaction(SIGFPE, &sa, old_sa);
    return old_sa;
}
#endif

void RemoveGuestFpExceptionHandler(void* handle) {
    if (!handle) return;
#ifdef _WIN32
    RemoveVectoredExceptionHandler(handle);
#else
    auto* old_sa = static_cast<struct sigaction*>(handle);
    sigaction(SIGFPE, old_sa, nullptr);
    delete old_sa;
#endif
}
