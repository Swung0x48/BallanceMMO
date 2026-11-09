#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif // !WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Dbghelp.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include <tchar.h>
#include <psapi.h>
#include <direct.h>
#include "entity/version.hpp"
#include "utility/string_utils.hpp"
#include "dumpfile.h"


#pragma comment(lib, "Dbghelp.lib")

namespace NSDumpFile {
    typedef BOOL(WINAPI* MINIDUMPWRITEDUMP)(HANDLE hProcess, DWORD dwPid, HANDLE hFile, MINIDUMP_TYPE DumpType, CONST PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam, CONST PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam, CONST PMINIDUMP_CALLBACK_INFORMATION CallbackParam);

    // <DumpPath>\BMMO_<version>_<local yyyymmdd-hhmmss><suffix>.dmp (utils::cleanup_old_crash_dumps
    // reads the time back and removes the file after 7 days).
    void DumpFileName(TCHAR* out, size_t size, const char* suffix)
    {
        SYSTEMTIME stLocalTime;
        GetLocalTime(&stLocalTime);
        snprintf(out, size, "%s\\BMMO_%s_%04d%02d%02d-%02d%02d%02d%s.dmp",
            DumpPath, BMMO_VER_STRING, stLocalTime.wYear, stLocalTime.wMonth, stLocalTime.wDay,
            stLocalTime.wHour, stLocalTime.wMinute, stLocalTime.wSecond, suffix);
    }

    void CreateDumpFile(LPCSTR lpstrDumpFilePathName, EXCEPTION_POINTERS* pException,
                        MINIDUMP_TYPE type = MiniDumpWithDataSegs)
    {
        // create dump file
        //
        HANDLE hDumpFile = CreateFile(lpstrDumpFilePathName, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        //HANDLE hTest = CreateFileA("C:\\Users\\Swung0x48\\Desktop\\dump\\test.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        

        // dump info
        //
        MINIDUMP_EXCEPTION_INFORMATION dumpInfo{};
        dumpInfo.ExceptionPointers = pException;
        dumpInfo.ThreadId = GetCurrentThreadId();
        dumpInfo.ClientPointers = TRUE;

        HMODULE mhLib = ::LoadLibrary(_T("dbghelp.dll"));
        MINIDUMPWRITEDUMP pDump = (MINIDUMPWRITEDUMP)::GetProcAddress(mhLib, "MiniDumpWriteDump");

        // write in dump file
        //
        if (pDump && pDump(GetCurrentProcess(), GetCurrentProcessId(), hDumpFile, type,
                           pException ? &dumpInfo : NULL, NULL, NULL)) {
        //    //DWORD bytesWritten;
        //    //WriteFile(
        //    //    hTest,            // Handle to the file
        //    //    "1",  // Buffer to write
        //    //    1,   // Buffer size
        //    //    &bytesWritten,    // Bytes written
        //    //    nullptr);         // Overlapped
        //    //std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }


        CloseHandle(hDumpFile);
        //CloseHandle(hTest);
    }

    LPTOP_LEVEL_EXCEPTION_FILTER __stdcall MyDummySetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER lpTopLevelExceptionFilter)
    {
        return NULL;
    }

    BOOL PreventSetUnhandledExceptionFilter()
    {
        HMODULE hKernel32 = LoadLibrary(_T("kernel32.dll"));
        if (hKernel32 == NULL)
            return FALSE;


        void* pOrgEntry = GetProcAddress(hKernel32, "SetUnhandledExceptionFilter");
        if (pOrgEntry == NULL)
            return FALSE;


        unsigned char newJump[100];
        DWORD dwOrgEntryAddr = (DWORD)pOrgEntry;
        dwOrgEntryAddr += 5; // add 5 for 5 op-codes for jmp far


        void* pNewFunc = &MyDummySetUnhandledExceptionFilter;
        DWORD dwNewEntryAddr = (DWORD)pNewFunc;
        DWORD dwRelativeAddr = dwNewEntryAddr - dwOrgEntryAddr;


        newJump[0] = 0xE9;  // JMP absolute
        memcpy(&newJump[1], &dwRelativeAddr, sizeof(pNewFunc));
        SIZE_T bytesWritten;
        BOOL bRet = WriteProcessMemory(GetCurrentProcess(), pOrgEntry, newJump, sizeof(pNewFunc) + 1, &bytesWritten);
        return bRet;
    }

    std::function<void(char*)> CrashCallback{};
    static bool messageBoxTriggered = false;

    LONG WINAPI UnhandledExceptionFilterEx(struct ::_EXCEPTION_POINTERS* pException)
    {
        /*TCHAR szMbsFile[MAX_PATH] = { 0 };
        ::GetModuleFileName(NULL, szMbsFile, MAX_PATH);
        TCHAR* pFind = _tcsrchr(szMbsFile, '\\');
        if (pFind)
        {
            *(pFind + 1) = 0;
            _tcscat(szMbsFile, _T("CrashDumpFile.dmp"));
            CreateDumpFile(szMbsFile, pException);
        }*/
        int retval = _mkdir(DumpPath);
        TCHAR szFileName[MAX_PATH] = { 0 };
        DumpFileName(szFileName, sizeof(szFileName), "");
        CreateDumpFile(szFileName, pException);

        // TODO: MiniDumpWriteDump
        if (messageBoxTriggered)
            return EXCEPTION_CONTINUE_SEARCH;
        std::string extraText;
        char callbackText[128]{};
        CrashCallback(callbackText);
        if (std::strlen(callbackText) != 0)
            extraText.append("--------\n").append(callbackText).append("\n");
        extraText = "Fatal Error\n" + extraText + "========\n"
                + std::string{ szFileName }.erase(0, strlen(DumpPath) + 1);
        EXCEPTION_RECORD* record{};
        do {
            if (record) {
                record = record->ExceptionRecord;
                extraText.append("\n--------");
            } else
                record = pException->ExceptionRecord;
            if (!record) break;
            char desc[128];
            snprintf(desc, sizeof(desc), "\nCode: %08X | Flags: %08X\nAddress: %p", record->ExceptionCode, record->ExceptionFlags, record->ExceptionAddress);
            extraText.append(desc);
            if (record->NumberParameters == 0) continue;
            extraText.append("\nExtraInfo");
            for (DWORD i = 0; i < record->NumberParameters; ++i) {
                snprintf(desc, sizeof(desc), " | 0x%08X", record->ExceptionInformation[i]);
                extraText.append(desc);
            }
        } while (record->ExceptionRecord);
        extraText.append("\n========\n");
        srand((unsigned int) time(nullptr));
        extraText.append(Quotes[rand() % (sizeof(Quotes) / sizeof(char*))]);
        //FatalAppExit(-1, extraText.c_str());
        wchar_t basename[128];
        GetModuleBaseNameW(GetCurrentProcess(), NULL, basename, sizeof(basename) / sizeof(wchar_t));
        messageBoxTriggered = true;
        MessageBoxW(NULL, bmmo::string_utils::ConvertUtf8ToWide(extraText).c_str(),
                    (basename + std::wstring{ L" - Fatal Application Exit" }).c_str(),
                    MB_OK | MB_ICONERROR | MB_SYSTEMMODAL | MB_SETFOREGROUND | MB_SERVICE_NOTIFICATION);
        FatalExit(-1);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    namespace {
        std::atomic<ULONGLONG> hang_beat{0};      // GetTickCount64 of the last beat
        std::atomic<bool> hang_armed{false};
        std::atomic<bool> hang_dumped{false};     // a dump was written for the current stall
        std::atomic<ULONGLONG> hang_dumped_beat{0};
        TCHAR hang_file[MAX_PATH]{};              // written by the watch before hang_dumped
        std::mutex hang_mutex;
        std::condition_variable hang_wake;
        bool hang_stop = false;
        std::thread hang_thread;

        // Windows' own "Not Responding": a visible window of this process whose
        // thread has not pumped messages for 5 s.  False for an alt-tab, a
        // window drag or a slow frame that still pumps - so a dump (which
        // suspends every thread, the network's included) is written only for
        // what a player sees as frozen.
        bool own_window_hung() {
            BOOL hung = FALSE;
            EnumWindows([](HWND hwnd, LPARAM out) -> BOOL {
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && IsHungAppWindow(hwnd)) {
                    *reinterpret_cast<BOOL*>(out) = TRUE;
                    return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&hung));
            return hung != FALSE;
        }

        void hang_watch_loop() {
            std::unique_lock lk(hang_mutex);
            while (!hang_wake.wait_for(lk, std::chrono::milliseconds(500), [] { return hang_stop; })) {
                const ULONGLONG beat = hang_beat.load();
                if (!hang_armed.load() || hang_dumped.load() || GetTickCount64() - beat < kHangMs) continue;
                if (!own_window_hung()) continue;
                _mkdir(DumpPath);
                DumpFileName(hang_file, sizeof(hang_file), "_hang");
                // No exception: every thread's stack, the game thread's where it stalls.
                CreateDumpFile(hang_file, nullptr,
                               static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithThreadInfo |
                                                          MiniDumpWithHandleData));
                hang_dumped_beat = beat;
                hang_dumped = true;
            }
        }
    }

    void StartHangWatch()
    {
        std::lock_guard lk(hang_mutex);
        if (hang_thread.joinable()) return;
        hang_stop = false;
        hang_beat = GetTickCount64();
        ::LoadLibrary(_T("dbghelp.dll"));   // loaded now: a hang inside the loader lock must not block the dump
        hang_thread = std::thread(hang_watch_loop);
    }

    void StopHangWatch()
    {
        {
            std::lock_guard lk(hang_mutex);
            hang_stop = true;
        }
        hang_wake.notify_all();
        if (hang_thread.joinable()) hang_thread.join();
    }

    bool HangWatchBeat(bool armed, char* report, size_t size)
    {
        const ULONGLONG now = GetTickCount64();
        bool reported = false;
        if (hang_dumped.load()) {
            snprintf(report, size, "the game thread stalled for %.1f s; a hang dump was written: %s",
                     (now - hang_dumped_beat.load()) / 1000.0, hang_file);
            hang_dumped = false;
            reported = true;
        }
        hang_beat = now;
        hang_armed = armed;
        return reported;
    }

    void RunCrashHandler(std::function<void(char*)> Callback)
    {
        CrashCallback = Callback;
        SetUnhandledExceptionFilter(UnhandledExceptionFilterEx);
        PreventSetUnhandledExceptionFilter();
    }
}
