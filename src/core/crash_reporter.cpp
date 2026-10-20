#include "core/crash_reporter.h"

#ifdef _WIN32

#include <Windows.h>

#include <DbgHelp.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(kameo_crash_test, false, "Kameo", "Crash on purpose once the game is set up, to check that the crash report is written.");

namespace {

// Everything in the filter runs inside a crashed process, so it keeps to fixed buffers and raw Win32 calls, and touches the logger last.
wchar_t g_crash_dir[MAX_PATH] = {};
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;
volatile LONG g_reporting = 0;

constexpr size_t kReportBytes = 8192;
constexpr int kMaxFrames = 48;

struct Report {
  char text[kReportBytes];
  size_t length = 0;

  void Add(const char* format, ...) {
    if (length >= sizeof(text) - 1) return;
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(text + length, sizeof(text) - length, format, args);
    va_end(args);
    if (written > 0) length += size_t(written) < sizeof(text) - length ? size_t(written) : sizeof(text) - length - 1;
  }
};

const char* ExceptionName(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_BREAKPOINT: return "breakpoint";
    case 0xE06D7363: return "unhandled C++ exception";
    default: return "exception";
  }
}

// A frame inside recompiled game code is named by its guest function, the same in every build and what the disassembly uses, so it needs no symbols at all. The function holding the address comes from the unwind tables, and it is guest code exactly when its start is a mapping entry; host code that follows the last recompiled function in memory is not mistaken for it.
bool DescribeGuestFunction(uint64_t address, char* out, size_t out_size) {
  DWORD64 image_base = 0;
  const RUNTIME_FUNCTION* function = RtlLookupFunctionEntry(address, &image_base, nullptr);
  if (function == nullptr) return false;
  const uint64_t start = image_base + function->BeginAddress;
  for (const PPCFuncMapping* mapping = PPCFuncMappings; mapping->host != nullptr; ++mapping) {
    if (reinterpret_cast<uint64_t>(mapping->host) != start) continue;
    snprintf(out, out_size, "sub_%08llX+0x%llX", static_cast<unsigned long long>(mapping->guest), static_cast<unsigned long long>(address - start));
    return true;
  }
  return false;
}

// module+offset, which the matching PDB resolves to a function, plus the guest function for a frame in recompiled code.
void DescribeAddress(uint64_t address, char* out, size_t out_size) {
  HMODULE module = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address), &module) && module) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    char guest[64];
    const bool in_guest_code = module == GetModuleHandleW(nullptr) && DescribeGuestFunction(address, guest, sizeof(guest));
    snprintf(out, out_size, "%ls+0x%llX%s%s", name, static_cast<unsigned long long>(address - reinterpret_cast<uint64_t>(module)), in_guest_code ? "  guest " : "", in_guest_code ? guest : "");
  } else {
    snprintf(out, out_size, "0x%016llX (no module)", static_cast<unsigned long long>(address));
  }
}

// The x64 unwinder, not CaptureStackBackTrace, so the walk starts at the faulting frame rather than inside this filter. A corrupt stack can fault the walk itself, which SEH turns into a short list instead of a lost report.
void WalkStack(const CONTEXT* crash_context, Report* report) {
  CONTEXT context = *crash_context;
  __try {
    for (int frame = 0; frame < kMaxFrames && context.Rip != 0; ++frame) {
      char where[MAX_PATH + 32];
      DescribeAddress(context.Rip, where, sizeof(where));
      report->Add("  #%-2d %s\n", frame, where);
      DWORD64 image_base = 0;
      RUNTIME_FUNCTION* function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
      if (function == nullptr) {
        // A leaf function has no unwind data; its return address is at the top of the stack.
        uint64_t return_address = 0;
        SIZE_T read = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(context.Rsp), &return_address, sizeof(return_address), &read) || read != sizeof(return_address)) break;
        context.Rip = return_address;
        context.Rsp += sizeof(return_address);
        continue;
      }
      void* handler_data = nullptr;
      DWORD64 establisher_frame = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data, &establisher_frame, nullptr);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    report->Add("  (stack walk stopped: the stack is damaged)\n");
  }
}

void WriteFileText(const wchar_t* path, const char* text, size_t length) {
  HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  WriteFile(file, text, DWORD(length), &written, nullptr);
  CloseHandle(file);
}

bool WriteMinidump(const wchar_t* path, EXCEPTION_POINTERS* exception) {
  HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  MINIDUMP_EXCEPTION_INFORMATION info = {};
  info.ThreadId = GetCurrentThreadId();
  info.ExceptionPointers = exception;
  info.ClientPointers = FALSE;
  // Stacks, what they point at, globals and thread state: enough to debug from, without the guest memory arena that a full dump would drag in.
  const MINIDUMP_TYPE type = MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithHandleData);
  BOOL ok = FALSE;
  __try {
    ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, &info, nullptr, nullptr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = FALSE;
  }
  CloseHandle(file);
  return ok == TRUE;
}

LONG WINAPI ReportCrash(EXCEPTION_POINTERS* exception) {
  // A second thread crashing while the first reports waits for the process to end rather than writing over it.
  if (InterlockedCompareExchange(&g_reporting, 1, 0) != 0) {
    Sleep(INFINITE);
  }
  const EXCEPTION_RECORD& record = *exception->ExceptionRecord;

  static Report report;  // static: the crashing thread's stack may be nearly gone
  char where[MAX_PATH + 32];
  DescribeAddress(reinterpret_cast<uint64_t>(record.ExceptionAddress), where, sizeof(where));
  report.Add("kameorepowered crashed: %s (0x%08lX) at %s on thread %lu\n", ExceptionName(record.ExceptionCode), static_cast<unsigned long>(record.ExceptionCode), where, static_cast<unsigned long>(GetCurrentThreadId()));
  if ((record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION || record.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) && record.NumberParameters >= 2) {
    const ULONG_PTR operation = record.ExceptionInformation[0];
    report.Add("  %s address 0x%016llX\n", operation == 0 ? "reading" : operation == 1 ? "writing" : operation == 8 ? "executing" : "touching", static_cast<unsigned long long>(record.ExceptionInformation[1]));
  }
  report.Add("  stack (module+offset; resolve with the build's kameorepowered.pdb):\n");
  WalkStack(exception->ContextRecord, &report);

  SYSTEMTIME now = {};
  GetLocalTime(&now);
  wchar_t base[MAX_PATH + 40];
  _snwprintf_s(base, _TRUNCATE, L"%s\\crash_%04u%02u%02u_%02u%02u%02u", g_crash_dir, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
  CreateDirectoryW(g_crash_dir, nullptr);
  wchar_t text_path[MAX_PATH + 48];
  wchar_t dump_path[MAX_PATH + 48];
  _snwprintf_s(text_path, _TRUNCATE, L"%s.txt", base);
  _snwprintf_s(dump_path, _TRUNCATE, L"%s.dmp", base);
  // The text goes first: it is the part a report needs, and the dump and the logger can both still fail.
  WriteFileText(text_path, report.text, report.length);
  const bool dumped = WriteMinidump(dump_path, exception);

  // The log is what gets sent, so the report is repeated there; every line flushes, so it reaches the file before the process ends.
  char base_utf8[MAX_PATH * 3] = {};
  WideCharToMultiByte(CP_UTF8, 0, base, -1, base_utf8, sizeof(base_utf8), nullptr, nullptr);
  REXLOG_ERROR("{}", report.text);
  REXLOG_ERROR("crash report written to {}.txt{}", base_utf8, dumped ? " and .dmp (send both with this log)" : " (the minidump failed)");

  return g_previous_filter ? g_previous_filter(exception) : EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void InstallCrashReporter() {
  // Beside the log, which the SDK keeps in logs/ next to the executable.
  const std::wstring dir = (rex::filesystem::GetExecutableFolder() / "logs").wstring();
  wcsncpy_s(g_crash_dir, dir.c_str(), _TRUNCATE);
  g_previous_filter = SetUnhandledExceptionFilter(ReportCrash);
}

void CrashForTestIfAsked() {
  if (!REXCVAR_GET(kameo_crash_test)) return;
  // The guest lookup is checked on a known entry, since the test crash itself is in host code.
  char where[MAX_PATH + 96];
  DescribeAddress(reinterpret_cast<uint64_t>(PPCFuncMappings[1].host) + 0x10, where, sizeof(where));
  REXLOG_WARN("kameo_crash_test: guest lookup self-check (expect sub_{:08X}+0x10): {}", static_cast<unsigned long long>(PPCFuncMappings[1].guest), where);
  REXLOG_WARN("kameo_crash_test: crashing on purpose");
  volatile int* nowhere = nullptr;
  *nowhere = 1;
}

#else

void InstallCrashReporter() {}
void CrashForTestIfAsked() {}

#endif
