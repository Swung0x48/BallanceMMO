#include "crash_report.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

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

        LONG WINAPI on_unhandled_exception(EXCEPTION_POINTERS* info) {
            const auto* record = info->ExceptionRecord;
            std::fprintf(stderr, "\n[crash] unhandled exception 0x%08lx at %p", record->ExceptionCode, record->ExceptionAddress);
            if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
                std::fprintf(stderr, " (%s address %p)", record->ExceptionInformation[0] ? "writing" : "reading",
                             reinterpret_cast<void*>(record->ExceptionInformation[1]));
            }
            std::fprintf(stderr, "\n");
            print_backtrace_from_context(info->ContextRecord);
            std::fflush(stderr);
            return EXCEPTION_EXECUTE_HANDLER;
        }

        void on_abort(int) {
            std::fprintf(stderr, "\n[crash] abort() called\n");
            print_backtrace_here();
            std::fflush(stderr);
            _exit(134);
        }
#elif defined(BMMO_HAVE_EXECINFO)
        void on_fatal_signal(int signal_number) {
            const char* name = signal_number == SIGSEGV ? "SIGSEGV" : signal_number == SIGABRT ? "SIGABRT"
                             : signal_number == SIGBUS  ? "SIGBUS"  : signal_number == SIGFPE  ? "SIGFPE" : "signal";
            std::fprintf(stderr, "\n[crash] %s\n", name);
            std::fflush(stderr);
            void* frames[64];
            const int count = backtrace(frames, 64);
            backtrace_symbols_fd(frames, count, STDERR_FILENO);
            _exit(128 + signal_number);
        }
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
#elif defined(BMMO_HAVE_EXECINFO)
        for (int signal_number: {SIGSEGV, SIGABRT, SIGBUS, SIGFPE})
            std::signal(signal_number, on_fatal_signal);
#endif
    }
}
