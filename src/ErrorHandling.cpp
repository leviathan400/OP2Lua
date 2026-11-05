// ErrorHandling.cpp - a lightweight crash reporter for OP2LuaCore.
//
// Installs a vectored exception handler (VEH) and a std::terminate handler that, on a fatal
// fault, log the exception code, CPU registers, and a symbolized native stack trace (via dbghelp)
// to the OP2Lua log. The actual dumping runs on a dedicated thread (driven by an APC) so the
// faulting thread does as little as possible. This is best-effort diagnostics, not recovery.

#include "ErrorHandling.h"

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <type_traits>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include "Log.h"

#include "Tethys/Common/Types.h"

namespace OP2Lua::ErrorHandling {

// Configuration.
// Maximum number of stack entries to show (in case of infinite recursion).
static constexpr const size_t kMaxStackFrames = 200;

// Global shared state about the current exception being handled by the logging thread, and associated mutex protecting it.
static HANDLE ghExcThread;
static DWORD ghExcThreadId;
static EXCEPTION_RECORD gExcRecord;
static CONTEXT gExcContext;
static std::mutex gExcMutex;

// Exception reporter/logging thread and associated mutex.
static std::thread gReporterThread;

// Convert a scalar value into a fixed-width hex string (no leading "0x").
template <typename T,
          typename = std::enable_if_t<std::is_scalar_v<T>>>
std::string ToHexString(T value) {
  std::ostringstream ss;
  ss << std::right << std::hex << std::setfill('0') << std::setw(sizeof(T) * 2) << value;
  return ss.str();
}

// Dumps a thread context.
// @param ctx If null, use the shared state in ghExcContext, otherwise use this context.
// ctx is intended to be used from things such as std::terminate handlers, if it's null, we assume we are being called via an APC from our VEH handler.
// This is STDCALL because it's used as an APC that runs on the error handling thread.
static void STDCALL DumpContextOrException(CONTEXT* ctx) {
  // Synchronize access to shared state (and additionally ensure we don't interleave the logs if multiple exceptions were to occur at once).
  const std::lock_guard<std::mutex> lock(gExcMutex);

  // Map exception codes to strings so we don't have to guess what went wrong.
  static std::map<DWORD, const char*> exceptionCodes = {
#define X(v) { v, #v },
    X(EXCEPTION_ACCESS_VIOLATION) X(EXCEPTION_ARRAY_BOUNDS_EXCEEDED) X(EXCEPTION_BREAKPOINT)
    X(EXCEPTION_DATATYPE_MISALIGNMENT) X(EXCEPTION_FLT_DENORMAL_OPERAND) X(EXCEPTION_FLT_DIVIDE_BY_ZERO)
    X(EXCEPTION_FLT_INEXACT_RESULT) X(EXCEPTION_FLT_INVALID_OPERATION) X(EXCEPTION_FLT_OVERFLOW)
    X(EXCEPTION_FLT_STACK_CHECK) X(EXCEPTION_FLT_UNDERFLOW) X(EXCEPTION_ILLEGAL_INSTRUCTION)
    X(EXCEPTION_IN_PAGE_ERROR) X(EXCEPTION_INT_DIVIDE_BY_ZERO) X(EXCEPTION_INT_OVERFLOW)
    X(EXCEPTION_INVALID_DISPOSITION) X(EXCEPTION_NONCONTINUABLE_EXCEPTION) X(EXCEPTION_PRIV_INSTRUCTION)
    X(EXCEPTION_SINGLE_STEP) X(EXCEPTION_STACK_OVERFLOW)
#undef X
  };
  try {
    if (ctx == nullptr) {
      // No context was passed in, so an exception occurred and this is an APC.
      DWORD code = gExcRecord.ExceptionCode;
      const auto it = exceptionCodes.find(code);
      const std::string name = (it != exceptionCodes.end()) ? it->second : ToHexString(code);
      const std::string addr = ToHexString(gExcRecord.ExceptionAddress);
      // Main log: a one-line summary so the framework record notes the crash without the noise.
      LOG(ERROR) << "Mission CRASHED: " << name << " at " << addr
                 << " (thread " << ghExcThreadId << ") - full dump in the mission log";
      // Mission log: the full dump (exception, registers, stack) - all on the mission's own log.
      LOG_MISSION(ERROR) << "** Caught Windows exception " << name
                << " at address " << addr << " (thread " << ghExcThreadId << ") **";
      if (gExcRecord.NumberParameters > 0 &&
          (code == EXCEPTION_ACCESS_VIOLATION ||
          code == EXCEPTION_IN_PAGE_ERROR)) {
        // Log the type of operation and VA where it occurred, for those exceptions that have this information.
        // The rw constants come from the MS docs for EXCEPTION_RECORD
        ULONG_PTR rw = gExcRecord.ExceptionInformation[0];
        ULONG_PTR va = gExcRecord.ExceptionInformation[1];
        LOG_MISSION(ERROR) << "Couldn't " << (rw == 0 ? "read" : (rw == 1 ? "write" : (rw == 8 ? "execute" : "access" )))
                  << " virtual address " << ToHexString(va);
      }
      ctx = &gExcContext;
    } else {
      // A context was passed in explicitly (e.g. from the std::terminate handler). Adopt it so the
      // register/stack dump below reflects it, rather than stale VEH state left in gExcContext.
      gExcContext = *ctx;
    }
    // Log as much useful info from the context as we can.
    // Log registers
    // TODO: FPU registers, etc?
    LOG_MISSION(ERROR) << "CPU registers:";
    if (gExcContext.ContextFlags & CONTEXT_INTEGER) {
      LOG_MISSION(ERROR) << "  eax=" << ToHexString(gExcContext.Eax)
                << "  ebx=" << ToHexString(gExcContext.Ebx)
                << "  ecx=" << ToHexString(gExcContext.Ecx)
                << "  edx=" << ToHexString(gExcContext.Edx)
                << "  esi=" << ToHexString(gExcContext.Esi)
                << "  edi=" << ToHexString(gExcContext.Edi);
    }
    if (gExcContext.ContextFlags & CONTEXT_CONTROL) {
      LOG_MISSION(ERROR) << "  esp=" << ToHexString(gExcContext.Esp)
                << "  ebp=" << ToHexString(gExcContext.Ebp)
                << "  eip=" << ToHexString(gExcContext.Eip)
                << "  eflags=" << ToHexString(gExcContext.EFlags)
                << "  cs=" << ToHexString(gExcContext.SegCs)
                << "  ss=" << ToHexString(gExcContext.SegSs);
    }
    if (gExcContext.ContextFlags & CONTEXT_SEGMENTS) {
      LOG_MISSION(ERROR) << "  ds=" << ToHexString(gExcContext.SegDs)
                << "  es=" << ToHexString(gExcContext.SegEs)
                << "  fs=" << ToHexString(gExcContext.SegFs)
                << "  gs=" << ToHexString(gExcContext.SegGs);
    }
    // Attempt to generate and print a stacktrace.
    if (gExcContext.ContextFlags & CONTEXT_CONTROL) {
      SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_LOAD_ANYTHING | SYMOPT_DEBUG);
      HANDLE hProcess = GetCurrentProcess();
      if (SymInitialize(hProcess, nullptr, TRUE) == TRUE) {
        STACKFRAME frame;
        frame.AddrPC.Offset = gExcContext.Eip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = gExcContext.Ebp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = gExcContext.Esp;
        frame.AddrStack.Mode = AddrModeFlat;
        size_t numFrames = 0;
        LOG_MISSION(ERROR) << "Native code stacktrace:";
        while (StackWalk(IMAGE_FILE_MACHINE_I386, hProcess, ghExcThread, &frame, &gExcContext, nullptr, SymFunctionTableAccess, SymGetModuleBase, nullptr) == TRUE) {
          std::ostringstream ss;
          // Symbol info
          // TODO resizable buffer?
          char buffer[sizeof(IMAGEHLP_SYMBOL) + 255];
          IMAGEHLP_SYMBOL* symbol = reinterpret_cast<IMAGEHLP_SYMBOL*>(buffer);
          symbol->SizeOfStruct = sizeof(IMAGEHLP_SYMBOL) + 255;
          symbol->MaxNameLength = 254;
          DWORD offset = 0;
          ss << std::hex;
          if (SymGetSymFromAddr(hProcess, frame.AddrPC.Offset, &offset, symbol) == TRUE) {
            ss << symbol->Name << '+' << offset;
          } else {
            ss << "";
          }

          // Line info
          IMAGEHLP_LINE line;
          line.SizeOfStruct = sizeof(IMAGEHLP_LINE);
          if (SymGetLineFromAddr(hProcess, frame.AddrPC.Offset, &offset, &line) == TRUE) {
            ss << "  (" << line.FileName << ':' << std::dec << line.LineNumber << ')';
          } else {
            ss << "";
          }
          LOG_MISSION(ERROR) << "  " << ToHexString(frame.AddrPC.Offset) << ": " << ss.str();

          // In rare situations the stack/SEH records can get corrupted, leading to the stack walk never terminating.
          if (numFrames++ >= kMaxStackFrames) {
            LOG_MISSION(ERROR) << "  <recursion limit of " << kMaxStackFrames << " stack frames reached>";
            break;
          }
        }
        SymCleanup(hProcess);
      }
      // TODO: python backtrace?
    }
  } catch (std::exception& e) {
    LOG(ERROR) << "Caught C++ exception: " << e.what();
  }
}

// The actual exception handler, which hands off the details to the reporter thread.
// This should ideally do very little to avoid using much stack space (in the event of stack overflows).
static LONG WINAPI ExceptionHandler(EXCEPTION_POINTERS* exc) {
  try {
    const DWORD code = exc->ExceptionRecord->ExceptionCode;
    // We only care about genuine hardware/structured faults with the "error" bit set.
    // IMPORTANT: several common codes also have the high bit set but are NOT crashes and must be
    // ignored, or we would log a full stack dump + Sleep() on every one of them:
    //   0xE06D7363 = MSVC C++ exception (a normal `throw`)
    //   0x406D1388 = SetThreadName signal used by debuggers
    //   0xE0434352 / 0xE0434F4D = .NET/CLR exceptions
    // (Lua itself uses longjmp, not C++ exceptions, but our own C++ code may still throw.)
    const bool isRealFault =
        (code & 0x80000000) &&
        code != 0xE06D7363 && code != 0x406D1388 &&
        code != 0xE0434352 && code != 0xE0434F4D;
    if (isRealFault) {
      {
        // Update the exception info in memory and queue an APC to handle it in the other thread.
        const std::lock_guard<std::mutex> lock(gExcMutex);
        ghExcThread = GetCurrentThread();
        ghExcThreadId = GetCurrentThreadId();
        memcpy(&gExcRecord, exc->ExceptionRecord, sizeof(EXCEPTION_RECORD));
        memcpy(&gExcContext, exc->ContextRecord, sizeof(CONTEXT));
        QueueUserAPC(reinterpret_cast<PAPCFUNC>(DumpContextOrException), gReporterThread.native_handle(), 0);
      }
      // Chances are the program will terminate after this (unless we are being debugged) so we need to stay alive for a short period of time to allow log files to be flushed, etc.
      // TODO: pop up a message box instead?
      Sleep(500);
    }
  } catch (std::exception& e) {
    LOG(ERROR) << "Caught C++ exception: " << e.what();
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

void Enable(bool enable) {
  static void* exceptionHandler = nullptr;
  // Used to notify the thread to stop running when we shut down.
  static HANDLE hStopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  auto TerminationFunc = [] {
    std::set_terminate(nullptr);
    RemoveVectoredExceptionHandler(exceptionHandler);
    exceptionHandler = nullptr;
    LOG(INFO) << "Signaling crash reporter to stop";
    SetEvent(hStopEvent);
    if (gReporterThread.joinable()) {
      gReporterThread.join();
    }
    CloseHandle(hStopEvent);
    hStopEvent = nullptr;
    LOG(INFO) << "Crash reporter thread stopped";
  };

  if (enable) {
    // Install only once.
    if (exceptionHandler == nullptr) {
      gReporterThread = std::thread([] {
        // Alertable wait for the stop event forever (exception logging is done via an APC)
        while (WaitForSingleObjectEx(hStopEvent, 1000, TRUE) != WAIT_OBJECT_0) {}
      });
      LOG(INFO) << "Crash reporter thread started";
      // Set up C++ termination handler.
      // TODO: Figure out how to handle C abort() as well? That seems to happen (and cause a dialog in debug mode) if we intentionally crash ourselves.
      std::set_terminate([] {
        CONTEXT ctx;
        ctx.ContextFlags = CONTEXT_ALL;
        GetThreadContext(GetCurrentThread(), &ctx);
        LOG(ERROR) << "Mission CRASHED: std::terminate called - full dump in the mission log";
        LOG_MISSION(ERROR) << "** std::terminate called! **";
        DumpContextOrException(&ctx);
      });
      exceptionHandler = AddVectoredExceptionHandler(TRUE, ExceptionHandler);
    }
    // Set up an atexit() catch-all to ensure the thread gets stopped if we don't explicitly shut it down.
    std::atexit(TerminationFunc);
  } else if (exceptionHandler != nullptr) {
    TerminationFunc();
  }
}

}  // namespace OP2Lua::ErrorHandling
