#include "crash_report.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <typeinfo>
#if defined(__GNUG__)
#include <cxxabi.h>
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#elif defined(__has_include)
// bionic declares backtrace() only from API 33 on.
#if __has_include(<execinfo.h>) && (!defined(__ANDROID__) || __ANDROID_API__ >= 33)
#include <execinfo.h>
#include <unistd.h>
#define BMMO_HAVE_EXECINFO 1
#endif
#endif

namespace bmmo::sim {
    namespace {
        // Step SIO: the host's drain of its session writers (set_crash_drain).
        void (*g_crash_drain)() = nullptr;
        void run_crash_drain() {
            if (auto drain = g_crash_drain) {
                g_crash_drain = nullptr;   // once, even if the drain itself faults
                drain();
            }
        }
        // The host's log line (set_crash_log): the reason, after the drain,
        // so it is the log's last line.
        void (*g_crash_log)(const char*) = nullptr;
        void finish_report(const char* reason) {
            std::fflush(stderr);
            run_crash_drain();
            if (auto log = g_crash_log) {
                g_crash_log = nullptr;
                log(reason);
            }
        }
        // std::terminate's reason: the uncaught exception's type and what().
        void describe_terminate(char* reason, size_t size) {
            if (auto e = std::current_exception()) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& x) {
                    const char* type = typeid(x).name();
#if defined(__GNUG__)
                    int status = 0;
                    char* readable = abi::__cxa_demangle(type, nullptr, nullptr, &status);
                    if (status == 0 && readable) type = readable;   // not freed: the process ends
#endif
                    std::snprintf(reason, size, "[crash] std::terminate: uncaught %s: %s", type, x.what());
                } catch (...) {
                    std::snprintf(reason, size, "[crash] std::terminate: uncaught exception of a non-std type");
                }
            } else {
                std::snprintf(reason, size, "[crash] std::terminate called without an active exception");
            }
        }
#if defined(_WIN32)
        void print_frame(HANDLE process, DWORD64 address, int index) {
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] = {};
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 511;
            DWORD64 displacement = 0;
            const char* name = SymFromAddr(process, address, &displacement, symbol) ? symbol->Name : "?";
            IMAGEHLP_LINE64 line = {};
            line.SizeOfStruct = sizeof(line);
            DWORD line_displacement = 0;
            char module_name[MAX_PATH] = "?";
            if (HMODULE module = nullptr;
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(address)), &module)) {
                GetModuleFileNameA(module, module_name, sizeof module_name);
            }
            if (SymGetLineFromAddr64(process, address, &line_displacement, &line)) {
                std::fprintf(stderr, "  #%-2d %p %s+0x%llx  %s:%lu  [%s]\n", index, reinterpret_cast<void*>(address), name,
                             static_cast<unsigned long long>(displacement), line.FileName, line.LineNumber, module_name);
            } else {
                std::fprintf(stderr, "  #%-2d %p %s+0x%llx  [%s]\n", index, reinterpret_cast<void*>(address), name,
                             static_cast<unsigned long long>(displacement), module_name);
            }
        }

        void print_backtrace_from_context(CONTEXT* context) {
            HANDLE process = GetCurrentProcess();
            HANDLE thread = GetCurrentThread();
            STACKFRAME64 frame = {};
            DWORD machine;
#if defined(_M_X64)
            machine = IMAGE_FILE_MACHINE_AMD64;
            frame.AddrPC.Offset = context->Rip;
            frame.AddrFrame.Offset = context->Rbp;
            frame.AddrStack.Offset = context->Rsp;
#elif defined(_M_IX86)
            machine = IMAGE_FILE_MACHINE_I386;
            frame.AddrPC.Offset = context->Eip;
            frame.AddrFrame.Offset = context->Ebp;
            frame.AddrStack.Offset = context->Esp;
#elif defined(_M_ARM64)
            machine = IMAGE_FILE_MACHINE_ARM64;
            frame.AddrPC.Offset = context->Pc;
            frame.AddrFrame.Offset = context->Fp;
            frame.AddrStack.Offset = context->Sp;
#else
#error unsupported Windows architecture
#endif
            frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
            for (int i = 0; i < 64; ++i) {
                if (!StackWalk64(machine, process, thread, &frame, context, nullptr, SymFunctionTableAccess64,
                                 SymGetModuleBase64, nullptr) || frame.AddrPC.Offset == 0)
                    break;
                print_frame(process, frame.AddrPC.Offset, i);
            }
        }

        void print_backtrace_here() {
            void* frames[64];
            const USHORT count = CaptureStackBackTrace(0, 64, frames, nullptr);
            HANDLE process = GetCurrentProcess();
            for (USHORT i = 0; i < count; ++i)
                print_frame(process, reinterpret_cast<DWORD64>(frames[i]), i);
        }

        // A minidump of the crashing process in the working directory
        // (crash-<pid>.dmp): by default a small one (every thread's stack and
        // the memory they point at, a few MB); with BMMO_CRASH_DUMP set in the
        // environment a full-memory one, so the state behind a backtrace can be
        // inspected afterwards (a debugger, or a minidump reader walking the
        // structures by their layouts; hundreds of MB for a client);
        // BMMO_CRASH_DUMP=0: none.  `info` null (abort, terminate): the dump
        // has the calling thread's context at the call.
        void write_minidump(EXCEPTION_POINTERS* info) {
            char enabled[8] = "";
            const bool full = GetEnvironmentVariableA("BMMO_CRASH_DUMP", enabled, sizeof enabled) != 0;
            if (full && enabled[0] == '0' && enabled[1] == '\0') return;
            char path[64];
            std::snprintf(path, sizeof path, "crash-%lu.dmp", GetCurrentProcessId());
            HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                std::fprintf(stderr, "[crash] could not create %s\n", path);
                return;
            }
            MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
            const auto type = static_cast<MINIDUMP_TYPE>(
                    full ? MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo
                         : MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory);
            const BOOL written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                                   info ? &exception : nullptr, nullptr, nullptr);
            CloseHandle(file);
            std::fprintf(stderr, written ? "[crash] minidump written to %s\n" : "[crash] minidump %s failed (%lu)\n", path,
                         GetLastError());
        }

        LONG WINAPI on_unhandled_exception(EXCEPTION_POINTERS* info) {
            const auto* record = info->ExceptionRecord;
            char reason[160];
            int n = std::snprintf(reason, sizeof reason, "[crash] unhandled exception 0x%08lx at %p",
                                  record->ExceptionCode, record->ExceptionAddress);
            if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2 && n > 0
                && n < static_cast<int>(sizeof reason)) {
                std::snprintf(reason + n, sizeof reason - n, " (%s address %p)",
                              record->ExceptionInformation[0] ? "writing" : "reading",
                              reinterpret_cast<void*>(record->ExceptionInformation[1]));
            }
            std::fprintf(stderr, "\n%s\n", reason);
            // StackWalk64 unwinds the context it is given in place; the dump
            // must see the registers as they were at the fault.
            CONTEXT walked = *info->ContextRecord;
            print_backtrace_from_context(&walked);
            write_minidump(info);
            finish_report(reason);
            return EXCEPTION_EXECUTE_HANDLER;
        }

        [[noreturn]] void die_here(const char* reason) {
            std::fprintf(stderr, "\n%s\n", reason);
            print_backtrace_here();
            write_minidump(nullptr);
            finish_report(reason);
            _exit(134);
        }

        // abort(): the default terminate's too, on a thread that never set
        // the per-thread handler (MSVC keeps terminate handlers per thread) -
        // raised synchronously inside the catch(...) that rethrew, so the
        // uncaught exception is still the current one and is named here.
        void on_abort(int) {
            char reason[512];
            if (std::current_exception()) describe_terminate(reason, sizeof reason);
            else std::snprintf(reason, sizeof reason, "[crash] abort() called");
            die_here(reason);
        }

        // std::terminate (an exception nothing caught): before the default
        // handler's abort(), so the reason names the exception.  The stack
        // is still the throw's (terminate runs in the search phase).
        void on_terminate() {
            char reason[512];
            describe_terminate(reason, sizeof reason);
            die_here(reason);
        }

        // A CRT function given an invalid argument (release builds pass no
        // details): would otherwise end the process by __fastfail, which no
        // filter sees.
        void on_invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
            die_here("[crash] invalid parameter passed to a C runtime function");
        }
#elif defined(BMMO_HAVE_EXECINFO)
        void on_fatal_signal(int signal_number) {
            const char* name = signal_number == SIGSEGV ? "[crash] SIGSEGV" : signal_number == SIGABRT ? "[crash] SIGABRT"
                             : signal_number == SIGBUS  ? "[crash] SIGBUS"  : signal_number == SIGFPE  ? "[crash] SIGFPE"
                             : "[crash] fatal signal";
            std::fprintf(stderr, "\n%s\n", name);
            std::fflush(stderr);
            void* frames[64];
            const int count = backtrace(frames, 64);
            backtrace_symbols_fd(frames, count, STDERR_FILENO);
            finish_report(name);
            _exit(128 + signal_number);
        }

        void on_terminate() {
            char reason[512];
            describe_terminate(reason, sizeof reason);
            std::fprintf(stderr, "\n%s\n", reason);
            void* frames[64];
            const int count = backtrace(frames, 64);
            backtrace_symbols_fd(frames, count, STDERR_FILENO);
            finish_report(reason);
            _exit(134);
        }
#endif
    }

    void set_crash_drain(void (*drain)()) { g_crash_drain = drain; }
    void set_crash_log(void (*log)(const char*)) { g_crash_log = log; }

    void install_thread_crash_reporter() {
#if defined(_WIN32) || defined(BMMO_HAVE_EXECINFO)
        std::set_terminate(on_terminate);
#endif
    }

    void install_crash_reporter() {
#if defined(_WIN32)
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        SetUnhandledExceptionFilter(on_unhandled_exception);
        // Assertion failures and abort() would otherwise open a dialog box or
        // silently exit with code 3.
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        std::signal(SIGABRT, on_abort);
        _set_invalid_parameter_handler(on_invalid_parameter);
#elif defined(BMMO_HAVE_EXECINFO)
        for (int signal_number: {SIGSEGV, SIGABRT, SIGBUS, SIGFPE})
            std::signal(signal_number, on_fatal_signal);
#endif
        install_thread_crash_reporter();
    }
}
