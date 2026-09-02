#pragma once

// Test automation channel: a Win32 named pipe that accepts one command per
// line and answers one line per command.  It is opt-in through the
// BMMO_COMMAND_PIPE environment variable and exists so that tooling can drive
// the game deterministically without simulating keyboard or mouse input.
//
// Threading: the pipe server runs on its own thread and only moves bytes.
// Commands are queued and must be drained on the game thread; responses may
// be written from any thread.

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bmmo::automation {
    struct pipe_command {
        uint64_t id = 0;
        std::string line;
    };

    class command_pipe {
    public:
        command_pipe() = default;
        ~command_pipe();
        command_pipe(const command_pipe&) = delete;
        command_pipe& operator=(const command_pipe&) = delete;

        // Creates \\.\pipe\<name> and starts accepting one client at a time.
        bool start(const std::string& name, std::string& error);
        void stop();
        bool running() const { return running_.load(std::memory_order_acquire); }

        // Game thread: take every queued command line.
        std::vector<pipe_command> drain();
        // Any thread: send one response line to the currently connected client.
        void respond(uint64_t command_id, const std::string& text);

    private:
        void serve();
        void close_client();

        std::string name_;
        std::atomic_bool running_ = false;
        std::atomic_bool stopping_ = false;
        std::thread thread_;
        std::mutex mutex_;
        std::deque<pipe_command> pending_;
        uint64_t next_id_ = 1;
        void* pipe_ = nullptr;        // HANDLE of the listening/connected pipe instance
        void* wake_event_ = nullptr;  // HANDLE used to abort blocking pipe waits
        std::mutex write_mutex_;
    };
}
