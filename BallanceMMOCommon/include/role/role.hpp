#ifndef BALLANCEMMOSERVER_ROLE_HPP
#define BALLANCEMMOSERVER_ROLE_HPP

#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <unordered_map>
#include <cassert>
#include <cstdarg>
#include <ctime>
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
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif
#include "../utility/ansi_colors.hpp"

static constexpr inline size_t ONCE_RECV_MSG_COUNT = 1024;

class role {
public:
    static void init_socket() {
#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
        SteamDatagramErrMsg err_msg;
        if (!GameNetworkingSockets_Init(nullptr, err_msg))
            FatalError("GameNetworkingSockets_Init failed.  %s", err_msg);
#else
        SteamDatagramClient_SetAppID(570); // Just set something, doesn't matter what
        //SteamDatagramClient_SetUniverse( k_EUniverseDev );

        SteamDatagramErrMsg errMsg;
        if (!SteamDatagramClient_Init(true, errMsg))
            FatalError("SteamDatagramClient_Init failed.  %s", errMsg);

        // Disable authentication when running with Steam, for this
        // example, since we're not a real app.
        //
        // Authentication is disabled automatically in the open-source
        // version since we don't have a trusted third party to issue
        // certs.
        SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);
#endif
#ifdef _WIN32
        DWORD mode;
        GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode);
        SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        std::ignore = _setmode(_fileno(stdin), _O_U16TEXT);
#endif
        init_timestamp_ = SteamNetworkingUtils()->GetLocalTimestamp();
        init_time_t_ = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Msg, DebugOutput);
    }

    static void set_log_file(FILE* file) {
        log_file_ = file;
    }

    role() {
        interface_ = SteamNetworkingSockets();
    }

    virtual bool setup() { return true; };
    virtual void run() = 0;

    virtual bool running() {
        return running_;
    }

    static SteamNetworkingConfigValue_t generate_opt() {
        SteamNetworkingConfigValue_t opt{};
        SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_TimeoutConnected, 7200);
        opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                   (void*)SteamNetConnectionStatusChangedCallbackWrapper);
        return opt;
    }

    virtual bool update() {
        int msg_count = poll_incoming_messages();
        poll_connection_state_changes();
//        poll_local_state_changes();
        return msg_count > 0;
    }

    virtual void poll_local_state_changes() = 0;

    static void destroy() {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
        GameNetworkingSockets_Kill();
#else
        SteamDatagramClient_Kill();
#endif
        if (log_file_) {
            fclose(log_file_);
            log_file_ = nullptr;
        }
    }

protected:
    ISteamNetworkingSockets* interface_ = nullptr;
    static inline role* this_instance_ = nullptr;
    static inline SteamNetworkingMicroseconds init_timestamp_;
    static inline time_t init_time_t_;
    std::atomic_bool running_ = false;
    ISteamNetworkingMessage* incoming_messages_[ONCE_RECV_MSG_COUNT];
    static inline FILE* log_file_ = nullptr;
#ifdef _WIN32
    static inline const bool LOWER_THAN_WIN10 = [] {
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
#endif

    virtual int poll_incoming_messages() = 0;

    virtual void poll_connection_state_changes() {
        this_instance_ = this;
        interface_->RunCallbacks();
    }

    virtual void on_connection_status_changed(SteamNetConnectionStatusChangedCallback_t* pInfo) = 0;

    virtual void on_message(ISteamNetworkingMessage* msg) = 0;

    static void SteamNetConnectionStatusChangedCallbackWrapper(SteamNetConnectionStatusChangedCallback_t* pInfo) {
        this_instance_->on_connection_status_changed(pInfo);
    }

    // triggers an actual segmentation fault; I was too lazy to fake one
    static void trigger_fatal_error() {
        *(int*)0 = 0;
    }

public:
    static void set_logging_level(ESteamNetworkingSocketsDebugOutputType eType) {
        SteamNetworkingUtils()->SetDebugOutputFunction(eType, DebugOutput);
    }

    static void LogFileOutput(const char* pMsg) {
        if (log_file_) {
            auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            char timeStr[15];
            std::strftime(timeStr, sizeof(timeStr), "%m-%d %T", std::localtime(&time));
            fprintf(log_file_, "[%s] %s\n", timeStr, pMsg);
            // fflush(log_file_);
        }
    }

    static void DebugOutput(ESteamNetworkingSocketsDebugOutputType eType, const char* pszMsg, int ansiColor) {
        // SteamNetworkingMicroseconds time = SteamNetworkingUtils()->GetLocalTimestamp() - init_timestamp_;
        auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        char timeStr[15];
        std::strftime(timeStr, sizeof(timeStr), "%m-%d %T", std::localtime(&time));

        if (log_file_) {
            fprintf(log_file_, "[%s] %s\n", timeStr, pszMsg);
        }

        if (eType == k_ESteamNetworkingSocketsDebugOutputType_Bug) {
            fprintf(stderr, "\r[%s] %s\n> ", timeStr, pszMsg);
            fflush(stdout);
            fflush(stderr);
            if (log_file_) fflush(log_file_);
            exit(2);
        } else {
            // printf("\r%10.2f %s\n> ", time * 1e-6, pszMsg);
            if (!isatty(fileno(stdout))
#ifdef _WIN32
                || LOWER_THAN_WIN10 // ansi sequences cannot be used on windows versions below 10
#endif
            ) {
                printf("\r[%s] %s\n> ", timeStr, pszMsg);
                fflush(stdout);
                return;
            }
#ifdef _WIN32
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
            short width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
#else
            struct winsize w;
            ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
            short width = w.ws_col;
            if (width <= 0)
                width = 80;
#endif
            unsigned short lines = ((short) strlen(pszMsg) + 17) / width + 1;
            if (ansiColor == bmmo::ansi::Reset)
                printf("\033[s\033[%uL\033[G[%s] %s\n> \033[u\033[%uB", lines, timeStr, pszMsg, lines);
            else
                printf("\033[s\033[%uL\033[G[%s] %s%s\033[0m\n> \033[u\033[%uB",
                        lines, timeStr, bmmo::ansi::get_escape_code(ansiColor).c_str(), pszMsg, lines);
            fflush(stdout);
        }
    }

    static void DebugOutput(ESteamNetworkingSocketsDebugOutputType eType, const char* pszMsg) {
        DebugOutput(eType, pszMsg, bmmo::ansi::Reset);
    }

    static void RightTrim(char* text) {
        char* el = strchr(text, '\0');
        if (el > text && el[-1] == '\n')
            text[el - text - 1] = '\0';
    }

    template <typename T>
    static inline const T& ConvertArgument(const T& arg) noexcept {
        return arg;
    }

    static inline const char* ConvertArgument(const std::string& str) noexcept {
        return str.c_str();
    }

    template <typename ... Args>
    static void Sprintf(std::string& buf, const char* fmt, Args&& ... args) {
        buf.resize(snprintf(buf.data(), buf.size(), fmt, ConvertArgument(args)...));
    }

    // create a new string and format it
    template <typename ... Args>
    static std::string Sprintf(const char* fmt, Args&& ... args) {
        std::string buf(2048, 0);
        buf.resize(snprintf(buf.data(), buf.size(), fmt, ConvertArgument(args)...));
        return buf;
    }

    static void Printf(const char* fmt) {
        char text[2048];
        strcpy(text, fmt);
        RightTrim(text);
        DebugOutput(k_ESteamNetworkingSocketsDebugOutputType_Important, text);
    }

    static void Printf(int ansiColor, const char* fmt) {
        Printf(ansiColor, "%s", fmt);
    }

    template <typename ... Args>
    static void Printf(const char* fmt, Args&& ... args) {
        char text[2048]{};
        snprintf(text, sizeof(text), fmt, ConvertArgument(args)...);
        // va_list ap;
        // va_start(ap, fmt);
        // vsprintf(text, fmt, ap);
        // va_end(ap);
        RightTrim(text);
        DebugOutput(k_ESteamNetworkingSocketsDebugOutputType_Important, text);
    }

    template<typename ... Args>
    static void Printf(int ansiColor, const char* fmt, Args&& ... args) {
        char text[2048]{};
        snprintf(text, sizeof(text), fmt, ConvertArgument(args)...);
        RightTrim(text);
        DebugOutput(k_ESteamNetworkingSocketsDebugOutputType_Important, text, ansiColor);
    }

    static void FatalError(const char* fmt, ...) {
        char text[2048];
        va_list ap;
        va_start(ap, fmt);
        vsprintf(text, fmt, ap);
        va_end(ap);
        RightTrim(text);
        DebugOutput(k_ESteamNetworkingSocketsDebugOutputType_Bug, text);
    }
};

#endif //BALLANCEMMOSERVER_ROLE_HPP
