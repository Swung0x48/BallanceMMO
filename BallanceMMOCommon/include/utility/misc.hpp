#ifndef BALLANCEMMOSERVER_INTERNAL_HPP
#define BALLANCEMMOSERVER_INTERNAL_HPP
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <functional>
#include "log_type.hpp"

namespace bmmo {
    void set_log_file(FILE* file);

    void LogFileOutput(const char* pMsg);

    void DebugOutput(bmmo::log_type eType, const char* pszMsg, int ansiColor);
    void DebugOutput(bmmo::log_type eType, const char* pszMsg);

    // Step SLQ: the least serious kind DebugOutput writes, process-wide
    // (everything by default: the client, SimTool and the tests write every
    // line; the server sets msg, or verbose under log_verbose).  A bug line
    // always passes.
    void set_log_threshold(bmmo::log_type least_serious);
    bool log_passes(bmmo::log_type type);

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
        DebugOutput(bmmo::log_type::important, text);
    }

    template<typename ... Args>
    inline void Printf(int ansiColor, const char* fmt, Args&& ... args) {
        char text[2048]{};
        std::snprintf(text, sizeof(text), fmt, ConvertArgument(args)...);
        RightTrim(text);
        DebugOutput(bmmo::log_type::important, text, ansiColor);
    }

    inline void Printf(const char* fmt) {
        char text[2048]{};
        std::strncpy(text, fmt, sizeof(text) - 1);
        RightTrim(text);
        DebugOutput(bmmo::log_type::important, text);
    }

    inline void Printf(int ansiColor, const char* fmt) {
        Printf(ansiColor, "%s", fmt);
    }

    // A line of the given kind (step SLQ).  A line under the threshold is not
    // formatted - but its arguments are always evaluated, being arguments: a
    // call site never guards a Printf on the level (some read a recorded
    // clock site, which a replay of a capture must see read).
    template <typename ... Args>
    inline void Printf(bmmo::log_type type, const char* fmt, Args&& ... args) {
        if (!log_passes(type)) return;
        char text[2048]{};
        std::snprintf(text, sizeof(text), fmt, ConvertArgument(args)...);
        RightTrim(text);
        DebugOutput(type, text);
    }

    inline void Printf(bmmo::log_type type, const char* fmt) {
        Printf(type, "%s", fmt);
    }

    inline void FatalError(const char* fmt, ...) {
        char text[2048];
        va_list ap;
        va_start(ap, fmt);
        vsprintf(text, fmt, ap);
        va_end(ap);
        RightTrim(text);
        DebugOutput(bmmo::log_type::bug, text);
    }

    void set_auto_flush_log(bool flush);
    // Step IOX: the log written by a writer strand (`poster` posts to one
    // strand of the host's I/O pool; session/io_queue.hpp io_poster), not by
    // the logging thread.  The server starts it in main; stop_log_writer
    // writes what is queued (bounded) and goes back to writing on the calling
    // thread, before the pool goes.  A host that never starts it logs as
    // before.  set_console_log_flush: the console stream's io_flush mode
    // (batch by default; the log file's follows auto_flush_log: record /
    // none).
    void start_log_writer(std::function<void(std::function<void()>)> poster);
    void stop_log_writer();
    void set_console_log_flush(uint8_t mode);
    void flush_log();
    void close_log();
    // signal-handler safe(r) counterpart of close_log(); see misc.cpp
    void flush_log_on_exit();
    // A crash handler's reason line, written to the --log file at once and
    // flushed (not through the writer strand, which the dying process will
    // not run again): call after the crash drain, so it is the file's last
    // line.  Nothing without a log file.
    void crash_log_line(const char* line);
}

#endif //BALLANCEMMOSERVER_INTERNAL_HPP
