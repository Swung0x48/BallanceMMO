#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif // !WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Dbghelp.h>
#include <iostream>
#include <vector>
#include <tchar.h>
#include <psapi.h>
#include <direct.h>
#include "entity/version.hpp"
#include "dumpfile.h"


#pragma comment(lib, "Dbghelp.lib")

namespace NSDumpFile {
    typedef BOOL(WINAPI* MINIDUMPWRITEDUMP)(HANDLE hProcess, DWORD dwPid, HANDLE hFile, MINIDUMP_TYPE DumpType, CONST PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam, CONST PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam, CONST PMINIDUMP_CALLBACK_INFORMATION CallbackParam);

    void CreateDumpFile(LPCSTR lpstrDumpFilePathName, EXCEPTION_POINTERS* pException)
    {
        // create dump file
        //
        HANDLE hDumpFile = CreateFile(lpstrDumpFilePathName, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        //HANDLE hTest = CreateFileA("C:\\Users\\Swung0x48\\Desktop\\dump\\test.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        

        // dump info
        //
        MINIDUMP_EXCEPTION_INFORMATION dumpInfo;
        dumpInfo.ExceptionPointers = pException;
        dumpInfo.ThreadId = GetCurrentThreadId();
        dumpInfo.ClientPointers = TRUE;

        HMODULE mhLib = ::LoadLibrary(_T("dbghelp.dll"));
        MINIDUMPWRITEDUMP pDump = (MINIDUMPWRITEDUMP)::GetProcAddress(mhLib, "MiniDumpWriteDump");

        // write in dump file
        //
        if (pDump(GetCurrentProcess(), GetCurrentProcessId(), hDumpFile, MiniDumpWithDataSegs, &dumpInfo, NULL, NULL)) {
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

    std::function<void(std::string&)> CrashCallback{};
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
        const TCHAR* szVersion = BMMO_VER_STRING;

        SYSTEMTIME stLocalTime;
        GetLocalTime(&stLocalTime);
        snprintf(szFileName, sizeof(szFileName), "%s\\BMMO_%s_%04d%02d%02d-%02d%02d%02d.dmp",
            DumpPath, szVersion, stLocalTime.wYear, stLocalTime.wMonth, stLocalTime.wDay,
            stLocalTime.wHour, stLocalTime.wMinute, stLocalTime.wSecond);
        CreateDumpFile(szFileName, pException);

        // TODO: MiniDumpWriteDump
        if (messageBoxTriggered)
            return EXCEPTION_CONTINUE_SEARCH;
        std::string extraText;
        CrashCallback(extraText);
        if (!extraText.empty())
            extraText = "--------\n" + extraText + "\n";
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
        srand(time(nullptr));
        extraText.append(Quotes[rand() % (sizeof(Quotes) / sizeof(char*))]);
        //FatalAppExit(-1, extraText.c_str());
        char basename[128];
        GetModuleBaseName(GetCurrentProcess(), NULL, basename, sizeof(basename));
        messageBoxTriggered = true;
        MessageBox(NULL, extraText.c_str(),
                        (basename + std::string{" - Fatal Application Exit"}).c_str(),
                        MB_OK | MB_ICONERROR | MB_SYSTEMMODAL | MB_SETFOREGROUND | MB_SERVICE_NOTIFICATION);
        FatalExit(-1);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void RunCrashHandler(std::function<void(std::string&)> Callback)
    {
        CrashCallback = Callback;
        SetUnhandledExceptionFilter(UnhandledExceptionFilterEx);
        PreventSetUnhandledExceptionFilter();
    }
}
