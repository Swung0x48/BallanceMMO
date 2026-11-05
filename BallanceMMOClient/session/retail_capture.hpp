#pragma once

// K7d / K7e (docs/k7-retail-capture-design.md): a session's capture in the
// retail mod, and its replay in Player.exe.
//
// The capture holds everything from outside the session reads on the game
// thread, in the order it read it (session/capture.hpp):
//   * the clock, by site (the host clock, session/host_clock.hpp);
//   * each take from a queue - the core host's mailbox, the inbox, the
//     command sources - every take, an empty one too (take_site);
//   * each frame's keyboard as the scripts read it (only a frame whose keys
//     changed);
//   * a few game-thread reads of the connection's state (retail_connected);
//   * what the session sends, so a replay names the first send that differs.
// It starts at the take holding a SessionStart and ends when the session
// does.  A replay installs the capture's clock and answers every take, key
// read and send from the file; the network, the command sources and the
// player's keyboard are not read.  The first read the replay makes that the
// capture does not hold stops the replay ("diverged") and says where.
//
// Game thread only.  One per mod (instance()).

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <session/capture.hpp>
#include <session/capture_seams.hpp>

#include "host_clock.hpp"

namespace bmmo::retail {

    enum class take_site : uint8_t { mailbox = 1, inbox = 2, command_file = 3, command_pipe = 4, console = 5 };

    class session_capture {
    public:
        enum class mode { off, recording, armed, replaying, ended };

        static session_capture& instance() {
            static session_capture capture;
            return capture;
        }

        mode state() const { return mode_; }
        bool recording() const { return mode_ == mode::recording; }
        bool replaying() const { return mode_ == mode::replaying; }
        const std::string& path() const { return path_; }
        const std::string& header() const { return file_->replaying() ? file_->command_line() : header_; }
        // Why a replay stopped (empty: it has not, or it reached the end marker).
        const std::string& divergence() const { return divergence_; }
        uint64_t frames() const { return frame_; }

        // ------------------------------------------------------------ capture
        // `prepare`: run on the writer strand before the file is made (the prune).
        bool start_recording(const std::filesystem::path& path, const std::string& header,
                             bmmo::session::io_poster poster, std::function<void()> prepare, std::string& error) {
            if (mode_ != mode::off && mode_ != mode::ended) { error = "a capture or a replay is running"; return false; }
            reset();
            mark_free();
            if (!file_->open_capture_deferred(path.string(), header, clock_den(), std::move(poster), std::move(prepare), error))
                return false;
            recording_clock_ = std::make_unique<bmmo::session::recording_clock>(bmmo::session::real_session_clock(), *file_);
            recording_clock_->owner.claim();
            set_host_clock(recording_clock_.get());
            path_ = path.string();
            header_ = header;
            mode_ = mode::recording;
            return true;
        }

        // The session is over: the capture's end marker, the real clock again.
        void stop_recording() {
            if (mode_ != mode::recording) return;
            set_host_clock(nullptr);
            file_->close();
            mode_ = mode::ended;
        }
        bool write_failed() const { return file_->write_failed(); }

        // ------------------------------------------------------------- replay
        // Opens the file; the replay begins at the first take (begin_replay,
        // the core host's drain), as the capture did.
        // `on_diverged`: told once, when a read the capture does not hold
        // stops the replay.
        bool arm_replay(const std::string& path, std::string& error,
                        std::function<void(const std::string&)> on_diverged = {}) {
            if (mode_ == mode::recording || mode_ == mode::replaying) { error = "a capture or a replay is running"; return false; }
            reset();
            mark_free();
            if (!file_->open_replay(path, clock_den(), error)) return false;
            if (file_->version() < 4) { error = path + ": not a retail capture (version " + std::to_string(file_->version()) + ")"; return false; }
            path_ = path;
            on_diverged_ = std::move(on_diverged);
            mode_ = mode::armed;
            return true;
        }
        bool armed() const { return mode_ == mode::armed; }
        void begin_replay() {
            if (mode_ != mode::armed) return;
            replay_clock_ = std::make_unique<bmmo::session::replay_clock>(*file_, bmmo::session::real_session_clock(),
                                                                          [this](const std::string& why) { diverge(why); });
            replay_clock_->owner.claim();
            set_host_clock(replay_clock_.get());
            mode_ = mode::replaying;
        }
        // The replay stops (its end, or a divergence): the real clock again.
        // The replay clock stays alive until the next start: a divergence is
        // raised from inside one of its reads.
        void end_replay() {
            if (mode_ != mode::replaying && mode_ != mode::armed) return;
            set_host_clock(nullptr);
            mode_ = mode::ended;
        }
        bool replay_complete() const { return file_->complete(); }
        std::string where() const { return file_->where(); }

        // ------------------------------------------------------------- reads
        // A take: written as taken while recording; the capture's while
        // replaying (`entries` replaced).  Other modes: nothing.
        void take(take_site site, std::vector<std::string>& entries) {
            if (mode_ == mode::recording) {
                file_->take(static_cast<uint8_t>(site), entries);
            } else if (mode_ == mode::replaying) {
                std::string error;
                if (!file_->take_take(static_cast<uint8_t>(site), entries, error)) {
                    entries.clear();
                    diverge(error);
                }
            }
        }

        // A frame's keyboard (the input manager's state, after the poll):
        // written when it changed while recording; set from the capture while
        // replaying.  Counts the frames of either.
        void keyboard(unsigned char* state, size_t size) {
            if (mode_ == mode::recording) {
                if (last_keys_.size() != size || std::memcmp(last_keys_.data(), state, size) != 0) {
                    last_keys_.assign(reinterpret_cast<const char*>(state), size);
                    file_->keys(frame_, state, size);
                }
                ++frame_;
            } else if (mode_ == mode::replaying) {
                std::string keys;
                if (file_->take_keys(frame_, keys)) last_keys_ = std::move(keys);
                if (last_keys_.size() == size) std::memcpy(state, last_keys_.data(), size);
                ++frame_;
            }
        }

        // A game-thread read of a value from outside (retail_connected):
        // recorded, or the capture's.
        bool flag(bmmo::session::clock_site site, bool live) {
            if (mode_ == mode::recording) {
                file_->clock(site, live ? 1 : 0);
                return live;
            }
            if (mode_ == mode::replaying) {
                int64_t value = 0;
                std::string error;
                if (!file_->take_clock(site, value, error)) { diverge(error); return live; }
                return value != 0;
            }
            return live;
        }

        // A session send: recorded (true: send it), or compared with the
        // capture's (false: a replay sends nothing).
        bool send(uint8_t send_mode, const std::string& bytes) {
            if (mode_ == mode::recording) {
                file_->sent(send_mode, bytes);
                return true;
            }
            if (mode_ == mode::replaying) {
                std::string error;
                if (!file_->take_sent(send_mode, bytes, error)) diverge(error);
                return false;
            }
            return true;
        }

        // Deletes every session_*.bmic in `directory` beyond the newest `keep`
        // (the writer strand, before a capture's file is made).  Throws nothing.
        static void prune(const std::filesystem::path& directory, size_t keep) {
            std::error_code ec;
            std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
            std::filesystem::directory_iterator it(directory, ec), end;
            for (; !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec) || ec) { ec.clear(); continue; }
                if (it->path().filename().string().rfind("session_", 0) != 0 || it->path().extension() != ".bmic") continue;
                files.emplace_back(it->last_write_time(ec), it->path());
                ec.clear();
            }
            if (files.size() <= keep) return;
            std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            for (size_t i = keep; i < files.size(); ++i) std::filesystem::remove(files[i].second, ec);
        }

    private:
        session_capture() = default;

        static int64_t clock_den() {
            using period = bmmo::session::session_clock::clock_type::period;
            return static_cast<int64_t>(period::den / period::num);
        }
        void mark_free() {
            file_->set_free(bmmo::session::clock_site::retail_display);
            // The inbox's countdown wait decides when a take happens, and the
            // replay has the take itself.
            file_->set_free(bmmo::session::clock_site::retail_inbox);
        }
        void reset() {
            set_host_clock(nullptr);
            file_ = std::make_unique<bmmo::session::capture_file>();
            recording_clock_.reset();
            replay_clock_.reset();
            frame_ = 0;
            last_keys_.clear();
            divergence_.clear();
            path_.clear();
            header_.clear();
        }
        void diverge(const std::string& why) {
            if (!divergence_.empty()) return;
            divergence_ = why + " (" + file_->where() + ", frame " + std::to_string(frame_) + ")";
            end_replay();
            if (on_diverged_) on_diverged_(divergence_);
        }

        mode mode_ = mode::off;
        std::unique_ptr<bmmo::session::capture_file> file_ = std::make_unique<bmmo::session::capture_file>();
        std::unique_ptr<bmmo::session::recording_clock> recording_clock_;
        std::unique_ptr<bmmo::session::replay_clock> replay_clock_;
        uint64_t frame_ = 0;
        std::string last_keys_;
        std::string divergence_;
        std::function<void(const std::string&)> on_diverged_;
        std::string path_;
        std::string header_;
    };

}
