#include <cstdlib>
#include <ctime>
#include <cstring>
#include <chrono>
#ifdef _WIN32
# ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
# endif
#include <Windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif
#include <replxx.hxx>
#include "entity/globals.hpp"
#include "utility/misc.hpp"
#include "utility/ansi_colors.hpp"
#include "utility/string_utils.hpp"

namespace bmmo {

    const bool LOWER_THAN_WIN10 =
#ifdef _WIN32
    [] {
        typedef void (WINAPI* RtlGetVersionPtr) (PRTL_OSVERSIONINFOW);
        HMODULE hMod = GetModuleHandleW(L"ntdll.dll");
        if (hMod) {
            auto func = (RtlGetVersionPtr)GetProcAddress(hMod, "RtlGetVersion");
            if (func) {
                RTL_OSVERSIONINFOW VersionInformation{}; func(&VersionInformation);
                return VersionInformation.dwMajorVersion < 10;
            };
        }
        return true;
    }(); // no manifest; we cannot use GetVersion or IsWindowsVersionXXXorGreater
#else
    false;
#endif

    namespace {
        FILE* log_file = nullptr;
        bool auto_flush = false;
    }

    void set_log_file(FILE* file) { log_file = file; }

    void LogFileOutput(const char* pMsg) {
        if (!log_file)
            return;
        auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        char timeStr[15];
        std::strftime(timeStr, sizeof(timeStr), "%m-%d %T", std::localtime(&time));
        fprintf(log_file, "[%s] %s\n", timeStr, pMsg);
        // fflush(log);
    }

    void RightTrim(char* text) {
        char* el = strchr(text, '\0');
        if (el > text && el[-1] == '\n')
            text[el - text - 1] = '\0';
    }
 
    void DebugOutput(ESteamNetworkingSocketsDebugOutputType eType, const char* pszMsg, int ansiColor) {
        // SteamNetworkingMicroseconds time = SteamNetworkingUtils()->GetLocalTimestamp() - init_timestamp_;
        auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        char timeStr[15];
        std::strftime(timeStr, sizeof(timeStr), "%m-%d %T", std::localtime(&time));

        if (log_file) {
            fprintf(log_file, "[%s] %s\n", timeStr, pszMsg);
            if (auto_flush) fflush(log_file);
        }

        if (eType == k_ESteamNetworkingSocketsDebugOutputType_Bug) {
            fprintf(stderr, "\r[%s] %s\n> ", timeStr, pszMsg);
            fflush(stdout);
            fflush(stderr);
            if (log_file) fflush(log_file);
            return exit(2);
        }
        else if (!isatty(fileno(stdout))) {
            printf("[%s] %s\n", timeStr, pszMsg);
        }
#ifdef _WIN32
        else if (LOWER_THAN_WIN10) {
            printf("\r[%s] %s\n> ", timeStr, bmmo::string_utils::utf8_to_ansi(pszMsg).c_str());
        }
#endif
        else {
            // printf("\r%10.2f %s\n> ", time * 1e-6, pszMsg);
            // bmmo::replxx_instance.invoke(replxx::Replxx::ACTION::CLEAR_SELF, '\0');
            if (ansiColor == bmmo::ansi::Reset)
                replxx_instance.print("\r[%s] %s\n", timeStr, pszMsg);
            else
                replxx_instance.print("\r[%s] %s%s\033[m\n", timeStr,
                                      bmmo::ansi::get_escape_code(ansiColor).c_str(), pszMsg);
        }
        fflush(stdout);
        // bmmo::replxx_instance.invoke(replxx::Replxx::ACTION::REPAINT, '\0');
    }

    void DebugOutput(ESteamNetworkingSocketsDebugOutputType eType, const char* pszMsg) {
        DebugOutput(eType, pszMsg, bmmo::ansi::Reset);
    }

    void set_auto_flush_log(bool flush) {
        auto_flush = flush;
    }

    void flush_log() {
        if (!log_file) return;
        if (fflush(log_file) == 0)
            Printf("Log file flushed successfully.");
    }

    void close_log() {
        if (!log_file) return;
        fclose(log_file);
        log_file = nullptr;
    }
}
