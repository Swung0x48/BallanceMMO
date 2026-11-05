#ifndef BALLANCEMMOSERVER_ROLE_HPP
#define BALLANCEMMOSERVER_ROLE_HPP

#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <cassert>
#include <csignal>
#include <cstdlib>
#include "../entity/globals.hpp"
#include "../net/transport.hpp"
#include "../utility/ansi_colors.hpp"

#include "../utility/misc.hpp"

static constexpr inline size_t ONCE_RECV_MSG_COUNT = 1024;

// A process's network side: the server, a client.  It talks to the network
// through a bmmo::net::transport (step K2: the platform layer) its host
// chooses - GNS (net/gns.hpp, which the host starts and stops) or a
// coordinated run's - and gives it, or sets transport_ before the run.
class role {
public:
    // The process's console and start time: every host, first thing.
    static void init_process() {
#ifdef _WIN32
        DWORD mode;
        GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode);
        SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        // std::ignore = _setmode(_fileno(stdin), _O_U16TEXT);
#endif
        init_steady_ = std::chrono::steady_clock::now();
        init_time_t_ = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    }

    explicit role(std::unique_ptr<bmmo::net::transport> transport = nullptr)
            : transport_(std::move(transport)) {

        auto close_log_and_exit = [](int) { // signal handler
            // Keep this minimal: most functions (printf, locks, exit with
            // atexit handlers) are not async-signal-safe. Flushing the log is
            // the one thing we must attempt before dying - closing it would
            // pull the FILE out from under a thread still writing to it.
            bmmo::flush_log_on_exit();
            std::_Exit(0);
        };
        std::signal(SIGABRT, close_log_and_exit);
        std::signal(SIGTERM, close_log_and_exit);
        std::signal(SIGINT, close_log_and_exit);
    }

    virtual ~role() = default;

    virtual bool setup() { return true; };
    virtual void run() = 0;

    virtual bool running() {
        return running_;
    }

    // Blocks until mark_running() has been called on another thread.
    void wait_till_started() {
        std::unique_lock<std::mutex> lk(startup_mutex_);
        startup_cv_.wait(lk, [this] { return running_.load(); });
    }

    // Messages first, then the connection state changes (the order the
    // session code relies on).
    virtual bool update() {
        int msg_count = poll_incoming_messages();
        poll_connection_state_changes();
        return msg_count > 0;
    }

    bmmo::net::transport& transport() { return *transport_; }

    // The log's end (a GNS host stops the library first: gns::stop()).
    static void destroy() {
        bmmo::close_log();
    }

protected:
    // Sets running_ and wakes up any wait_till_started() callers.
    // The store happens under startup_mutex_ so the waiting thread cannot
    // miss the notification between its predicate check and the wait.
    void mark_running() {
        {
            std::lock_guard<std::mutex> lk(startup_mutex_);
            running_ = true;
        }
        startup_cv_.notify_all();
    }

    std::unique_ptr<bmmo::net::transport> transport_;
    static inline role* this_instance_ = nullptr;
    static inline std::chrono::steady_clock::time_point init_steady_;   // the uptime's (a diagnostic)
    static inline time_t init_time_t_;
    // The start on GNS's process clock, for the hosts whose streams are on it
    // (the mock client's record, the retail client): set by them.
    static inline bmmo::net::microseconds init_timestamp_;
    std::atomic_bool running_ = false;
    std::mutex startup_mutex_;
    std::condition_variable startup_cv_;

    virtual int poll_incoming_messages() = 0;

    virtual void poll_connection_state_changes() {
        this_instance_ = this;
        transport_->poll_status([this](const bmmo::net::status_change& change) { on_connection_status_changed(change); });
    }

    virtual void on_connection_status_changed(const bmmo::net::status_change& change) = 0;

    virtual void on_message(const bmmo::net::message& msg) = 0;

    // triggers an actual segmentation fault; I was too lazy to fake one
    static void trigger_fatal_error() {
        *(volatile int*) 0 = 0;
    }
};

#endif //BALLANCEMMOSERVER_ROLE_HPP
