#pragma once
#include <atomic>
#include <thread>
#include <functional>
#include <utility>
#include <mutex>
#include <boost/circular_buffer.hpp>
#include "bml_includes.h"
#include "log_manager.h"

class console_window {
private:
    IBML* bml_;
    log_manager* log_manager_;
    std::function<void(IBML*, const std::vector<std::string>&)> command_callback_;
    std::thread console_thread_;
    bool owned_console_ = false;
    std::atomic_bool running_ = false;
    std::mutex mutex_;

    // pair <text, color>
    boost::circular_buffer<std::pair<std::string, int>> previous_msg_ = decltype(previous_msg_)(12);

    bool cleanup();

public:
    console_window(IBML* bml, log_manager* log_manager, decltype(command_callback_) command_callback):
        bml_(bml), log_manager_(log_manager), command_callback_(command_callback) {}

    void print_text(const char* text, int ansi_color = bmmo::ansi::Reset);

    inline bool running() { return running_; }
    void run();

    bool show();
    bool hide();

    void free_thread();
};
