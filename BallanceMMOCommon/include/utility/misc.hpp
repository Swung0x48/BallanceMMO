#ifndef BALLANCEMMOSERVER_INTERNAL_HPP
#define BALLANCEMMOSERVER_INTERNAL_HPP
#include <cstdio>
#include <cstdarg>
#include <steam/steamnetworkingtypes.h>

namespace bmmo {
    void set_log_file(FILE* file);

    void LogFileOutput(const char* pMsg);

    void DebugOutput(ESteamNetworkingSocketsDebugOutputType eType, const char* pszMsg, int ansiColor);
    void DebugOutput(ESteamNetworkingSocketsDebugOutputType eType, const char* pszMsg);

    void RightTrim(char* text);

    template <typename T>
    inline const T& ConvertArgument(const T& arg) noexcept {
        return arg;
    }

    inline const char* ConvertArgument(const std::string& str) noexcept {
        return str.c_str();
    }


    // Formats into buf, growing it when the result doesn't fit.
    // snprintf returns the length the output *would* have had, so resizing
    // to it blindly would leave the tail filled with uninitialized bytes.
    template <typename ... Args>
    inline void Sprintf(std::string& buf, const char* fmt, Args&& ... args) {
        if (buf.empty()) buf.resize(256);
        const int needed = snprintf(buf.data(), buf.size(), fmt, ConvertArgument(args)...);
        if (needed < 0) { buf.clear(); return; }
        if (static_cast<size_t>(needed) >= buf.size()) {
            buf.resize(static_cast<size_t>(needed) + 1);
            snprintf(buf.data(), buf.size(), fmt, ConvertArgument(args)...);
        }
        buf.resize(static_cast<size_t>(needed));
    }

    // create a new string and format it
    template <typename ... Args>
    inline std::string Sprintf(const char* fmt, Args&& ... args) {
        std::string buf;
        Sprintf(buf, fmt, ConvertArgument(args)...);
        return buf;
    }

    template <typename ... Args>
    inline void Printf(const char* fmt, Args&& ... args) {
        char text[2048]{};
        std::snprintf(text, sizeof(text), fmt, ConvertArgument(args)...);
        // va_list ap;
        // va_start(ap, fmt);
        // vsprintf(text, fmt, ap);
        // va_end(ap);
        RightTrim(text);
        DebugOutput(k_ESteamNetworkingSocketsDebugOutputType_Important, text);
    }

    template<typename ... Args>
    inline void Printf(int ansiColor, const char* fmt, Args&& ... args) {
        char text[2048]{};
        std::snprintf(text, sizeof(text), fmt, ConvertArgument(args)...);
        RightTrim(text);
        DebugOutput(k_ESteamNetworkingSocketsDebugOutputType_Important, text, ansiColor);
    }

    inline void Printf(const char* fmt) {
        char text[2048]{};
        std::strncpy(text, fmt, sizeof(text) - 1);
        RightTrim(text);
        DebugOutput(k_ESteamNetworkingSocketsDebugOutputType_Important, text);
    }

    inline void Printf(int ansiColor, const char* fmt) {
        Printf(ansiColor, "%s", fmt);
    }

    inline void FatalError(const char* fmt, ...) {
        char text[2048];
        va_list ap;
        va_start(ap, fmt);
        vsprintf(text, fmt, ap);
        va_end(ap);
        RightTrim(text);
        DebugOutput(k_ESteamNetworkingSocketsDebugOutputType_Bug, text);
    }

    void set_auto_flush_log(bool flush);
    void flush_log();
    void close_log();
    // signal-handler safe(r) counterpart of close_log(); see misc.cpp
    void flush_log_on_exit();
}

#endif //BALLANCEMMOSERVER_INTERNAL_HPP
