#pragma once

#include "bml_includes.h"
#include "common.hpp"

class log_manager {
private:
    ILogger* logger_;
    std::function<void (std::string, int)> ingame_msg_callback_;

public:
    log_manager(ILogger* logger, decltype(ingame_msg_callback_) ingame_msg_callback):
        logger_(logger), ingame_msg_callback_(ingame_msg_callback) {}

    inline ILogger* get_logger() { return logger_; }

    void send_ingame_message(const std::string& msg, int ansi_color = bmmo::ansi::Reset) {
        ingame_msg_callback_(msg, ansi_color);
    }
};
