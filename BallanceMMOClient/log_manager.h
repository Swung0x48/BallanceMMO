#pragma once

#include <chrono>
#include <cstdio>
#include <forward_list>
#include <mutex>
#include <string>
#include "bml_includes.h"
#include "common.hpp"
#include "session/io_queue.hpp"

// Step IOX (world-agreement-gaps-design.md, "As built (step IOX)"): the mod's
// log lines off the game thread.  BML's ILogger writes (and flushes) the
// ModLoader.log on the calling thread; once the mod starts this writer (its
// constructor, with a strand of the mod's thread_pool_), a line is formatted
// on the calling thread and the ILogger call itself runs on that strand, in
// call order.  Before the start and after the stop (OnExitGame, after the
// drain) a line is written on the calling thread as before.  File-scope state,
// never destroyed: BML's layout forbids a new BallanceMMOClient data member,
// and a crash drain may log late.
namespace bmmo::retail_log {
    struct writer_state {
        std::mutex mutex;
        bmmo::session::io_queue queue;
        ILogger* logger = nullptr;
        uint64_t dropped = 0;   // bytes of lines refused by a full queue
    };
    inline writer_state& state() {
        static writer_state* s = new writer_state;
        return *s;
    }
    // The headless client's and the server's log bound (step SIO).
    constexpr uint64_t kMaxQueued = 16ull << 20;

    // 'I' / 'W' / 'E': the ILogger level.
    inline void write_now(ILogger* logger, char level, const char* text) {
        switch (level) {
            case 'E': logger->Error("%s", text); break;
            case 'W': logger->Warn("%s", text); break;
            default: logger->Info("%s", text); break;
        }
    }

    inline void start(ILogger* logger, bmmo::session::io_poster poster) {
        auto& s = state();
        std::lock_guard lk(s.mutex);
        if (s.queue.is_open() || !logger || !poster) return;
        s.logger = logger;
        // A chunk is the level byte and the line; BML flushes its own file,
        // so the writer's flush mode adds nothing (none).
        s.queue.open_sink([logger](const std::string& chunk) {
            write_now(logger, chunk[0], chunk.c_str() + 1);
            return true;
        }, stdout, std::move(poster), kMaxQueued, bmmo::session::io_flush::none);
    }

    // Writes what is queued (bounded: the strand may be stalled) and goes
    // back to logging on the calling thread.
    inline void stop() {
        auto& s = state();
        std::lock_guard lk(s.mutex);
        s.queue.close_now(std::chrono::milliseconds(2000));
        s.logger = nullptr;
    }

    inline void submit(ILogger* logger, char level, std::string text) {
        auto& s = state();
        {
            std::lock_guard lk(s.mutex);
            if (s.queue.is_open() && s.logger == logger) {
                if (s.dropped != 0) {
                    const std::string notice = "W[log] " + std::to_string(s.dropped)
                                             + " bytes of log lines dropped: the log writer's queue was full";
                    if (s.queue.append(notice)) s.dropped = 0;
                }
                const size_t size = text.size();
                std::string chunk;
                chunk.reserve(size + 1);
                chunk.push_back(level);
                chunk += text;
                if (!s.queue.append(std::move(chunk))) s.dropped += size;
                return;
            }
        }
        write_now(logger, level, text.c_str());
    }

    // printf into a string (the arguments already converted).
    template <typename ... Args>
    inline std::string format(const char* fmt, Args ... args) {
        char stack[1024];
        const int n = std::snprintf(stack, sizeof(stack), fmt, args...);
        if (n < 0) return fmt;
        if (static_cast<size_t>(n) < sizeof(stack)) return std::string(stack, static_cast<size_t>(n));
        std::string out(static_cast<size_t>(n) + 1, '\0');
        std::snprintf(out.data(), out.size(), fmt, args...);
        out.resize(static_cast<size_t>(n));
        return out;
    }
}

class logger_wrapper {
private:
    ILogger* logger_;
    [[maybe_unused]] bool utf8_mode_ = false;
    // we just need somewhere that guarantees to never move our data
    // to store our converted strings temporarily
    std::forward_list<std::string> converted_strings_;
    std::mutex mutex_;

    // @returns converted pointer, stored in `converted_strings_`.
    // must be freed manually.
    const char* ConvertArgument(const std::string& str) noexcept {
        return converted_strings_.emplace_front(
#ifdef BMMO_USE_BML_PLUS
            utf8_mode_ ? str :
#endif
            bmmo::string_utils::utf8_to_ansi(str)
        ).c_str();
    }

    inline const char* ConvertArgument(const char* cstr) noexcept {
        return ConvertArgument(std::string{cstr});
    }

    template <typename T>
    inline const T& ConvertArgument(const T& arg) noexcept {
        return arg;
    }

public:
    logger_wrapper(ILogger* logger): logger_(logger) {}

    void set_utf8_mode(bool mode) { utf8_mode_ = mode; }

    // Step IOX: formatted here, written by bmmo::retail_log's strand once
    // the mod started it (the same text BML formatted from these arguments).
    template <typename ... Args>
    void Info(const char* fmt, Args&& ... args) { emit('I', fmt, std::forward<Args>(args)...); }

    template <typename ... Args>
    void Warn(const char* fmt, Args&& ... args) { emit('W', fmt, std::forward<Args>(args)...); }

    template <typename ... Args>
    void Error(const char* fmt, Args&& ... args) { emit('E', fmt, std::forward<Args>(args)...); }

    ILogger* raw() const { return logger_; }

private:
    template <typename ... Args>
    void emit(char level, const char* fmt, Args&& ... args) {
        std::lock_guard lk(mutex_);
        std::string text = bmmo::retail_log::format(ConvertArgument(fmt), ConvertArgument(args)...);
        converted_strings_.clear();
        bmmo::retail_log::submit(logger_, level, std::move(text));
    }

public:
};

class log_manager {
private:
    logger_wrapper logger_wrapper_;
    std::function<void (std::string, int)> ingame_msg_callback_;

public:
    log_manager(ILogger* logger, decltype(ingame_msg_callback_) ingame_msg_callback):
        logger_wrapper_(logger), ingame_msg_callback_(ingame_msg_callback) {}

    // UTF-8 support:
    // - BML: false
    // - BMLPlus: { "v < 0.3.4": false, "else": true }
    // Internal default: false
    void set_utf8_mode(bool mode) { logger_wrapper_.set_utf8_mode(mode); }

    inline logger_wrapper* get_logger() { return &logger_wrapper_; }

    void send_ingame_message(const std::string& msg, int ansi_color = bmmo::ansi::Reset) {
        ingame_msg_callback_(msg, ansi_color);
    }
};
