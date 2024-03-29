#pragma once

#include <forward_list>
#include <mutex>
#include "bml_includes.h"
#include "common.hpp"

class logger_wrapper {
private:
    ILogger* logger_;
    // we just need somewhere that guarantees to never move our data
    // to store our converted strings temporarily
    std::forward_list<std::string> converted_strings_;
    std::mutex mutex_;

    // @returns converted pointer, stored in `converted_strings_`.
    // must be freed manually.
    const char* ConvertArgument(const std::string& str) noexcept {
        return converted_strings_.emplace_front(bmmo::string_utils::utf8_to_ansi(str)).c_str();
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

    template <typename ... Args>
    void Info(const char* fmt, Args&& ... args) {
        std::lock_guard lk(mutex_);
        logger_->Info(ConvertArgument(fmt), ConvertArgument(args)...);
        converted_strings_.clear();
    }

    template <typename ... Args>
    void Warn(const char* fmt, Args&& ... args) {
        std::lock_guard lk(mutex_);
        logger_->Warn(ConvertArgument(fmt), ConvertArgument(args)...);
        converted_strings_.clear();
    }

    template <typename ... Args>
    void Error(const char* fmt, Args&& ... args) {
        std::lock_guard lk(mutex_);
        logger_->Error(ConvertArgument(fmt), ConvertArgument(args)...);
        converted_strings_.clear();
    }
};

class log_manager {
private:
    logger_wrapper logger_wrapper_;
    std::function<void (std::string, int)> ingame_msg_callback_;

public:
    log_manager(ILogger* logger, decltype(ingame_msg_callback_) ingame_msg_callback):
        logger_wrapper_(logger), ingame_msg_callback_(ingame_msg_callback) {}

    inline logger_wrapper* get_logger() { return &logger_wrapper_; }

    void send_ingame_message(const std::string& msg, int ansi_color = bmmo::ansi::Reset) {
        ingame_msg_callback_(msg, ansi_color);
    }
};
