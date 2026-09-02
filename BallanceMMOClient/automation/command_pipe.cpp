#include "command_pipe.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>

namespace bmmo::automation {
    namespace {
        constexpr DWORD kBufferBytes = 64 * 1024;
        constexpr size_t kMaximumPending = 4096;
        constexpr size_t kMaximumLineBytes = 64 * 1024;
    }

    command_pipe::~command_pipe() { stop(); }

    bool command_pipe::start(const std::string& name, std::string& error) {
        error.clear();
        if (running()) {
            error = "command pipe is already running";
            return false;
        }
        if (name.empty() || name.find_first_of("\\/") != std::string::npos) {
            error = "command pipe name must be a plain identifier";
            return false;
        }
        wake_event_ = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!wake_event_) {
            error = "CreateEvent failed: " + std::to_string(GetLastError());
            return false;
        }
        name_ = name;
        stopping_.store(false, std::memory_order_release);
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { serve(); });
        return true;
    }

    void command_pipe::stop() {
        if (!running()) return;
        stopping_.store(true, std::memory_order_release);
        if (wake_event_) SetEvent(static_cast<HANDLE>(wake_event_));
        close_client();
        if (thread_.joinable()) thread_.join();
        running_.store(false, std::memory_order_release);
        if (wake_event_) {
            CloseHandle(static_cast<HANDLE>(wake_event_));
            wake_event_ = nullptr;
        }
    }

    void command_pipe::close_client() {
        std::lock_guard lk(write_mutex_);
        if (pipe_) {
            // Cancelling outstanding overlapped I/O unblocks the server thread.
            CancelIoEx(static_cast<HANDLE>(pipe_), nullptr);
            DisconnectNamedPipe(static_cast<HANDLE>(pipe_));
            CloseHandle(static_cast<HANDLE>(pipe_));
            pipe_ = nullptr;
        }
    }

    std::vector<pipe_command> command_pipe::drain() {
        std::vector<pipe_command> out;
        std::lock_guard lk(mutex_);
        out.assign(pending_.begin(), pending_.end());
        pending_.clear();
        return out;
    }

    void command_pipe::respond(uint64_t command_id, const std::string& text) {
        std::lock_guard lk(write_mutex_);
        if (!pipe_) return;
        std::string line = std::to_string(command_id) + " " + text;
        for (auto& ch: line)
            if (ch == '\n' || ch == '\r') ch = ' ';
        line.push_back('\n');
        DWORD written = 0;
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) return;
        if (!WriteFile(static_cast<HANDLE>(pipe_), line.data(),
                       static_cast<DWORD>(line.size()), &written, &overlapped)
                && GetLastError() == ERROR_IO_PENDING) {
            GetOverlappedResult(static_cast<HANDLE>(pipe_), &overlapped, &written, TRUE);
        }
        CloseHandle(overlapped.hEvent);
    }

    void command_pipe::serve() {
        const std::string path = "\\\\.\\pipe\\" + name_;
        std::string partial;
        while (!stopping_.load(std::memory_order_acquire)) {
            HANDLE pipe = CreateNamedPipeA(
                path.c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT
                    | PIPE_REJECT_REMOTE_CLIENTS,
                1, kBufferBytes, kBufferBytes, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE) {
                Sleep(200);
                continue;
            }
            {
                std::lock_guard lk(write_mutex_);
                pipe_ = pipe;
            }

            OVERLAPPED connect{};
            connect.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
            bool connected = false;
            if (connect.hEvent) {
                if (ConnectNamedPipe(pipe, &connect)) {
                    connected = true;
                } else {
                    const DWORD last = GetLastError();
                    if (last == ERROR_PIPE_CONNECTED) {
                        connected = true;
                    } else if (last == ERROR_IO_PENDING) {
                        std::array<HANDLE, 2> handles{
                            connect.hEvent, static_cast<HANDLE>(wake_event_)};
                        const DWORD waited = WaitForMultipleObjects(
                            2, handles.data(), FALSE, INFINITE);
                        connected = waited == WAIT_OBJECT_0;
                    }
                }
                CloseHandle(connect.hEvent);
            }
            if (!connected || stopping_.load(std::memory_order_acquire)) {
                close_client();
                continue;
            }

            partial.clear();
            std::array<char, 4096> buffer{};
            while (!stopping_.load(std::memory_order_acquire)) {
                OVERLAPPED read{};
                read.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
                if (!read.hEvent) break;
                DWORD bytes = 0;
                BOOL ok = ReadFile(pipe, buffer.data(),
                                   static_cast<DWORD>(buffer.size()), &bytes, &read);
                if (!ok && GetLastError() == ERROR_IO_PENDING) {
                    std::array<HANDLE, 2> handles{
                        read.hEvent, static_cast<HANDLE>(wake_event_)};
                    const DWORD waited = WaitForMultipleObjects(
                        2, handles.data(), FALSE, INFINITE);
                    ok = waited == WAIT_OBJECT_0
                        && GetOverlappedResult(pipe, &read, &bytes, FALSE);
                }
                CloseHandle(read.hEvent);
                if (!ok || bytes == 0) break;

                partial.append(buffer.data(), bytes);
                if (partial.size() > kMaximumLineBytes) {
                    partial.clear();
                    break;
                }
                size_t start = 0;
                for (;;) {
                    const size_t end = partial.find('\n', start);
                    if (end == std::string::npos) break;
                    std::string line = partial.substr(start, end - start);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    start = end + 1;
                    if (line.empty()) continue;
                    std::lock_guard lk(mutex_);
                    if (pending_.size() >= kMaximumPending) pending_.pop_front();
                    pending_.push_back({next_id_++, std::move(line)});
                }
                partial.erase(0, start);
            }
            close_client();
        }
    }
}
