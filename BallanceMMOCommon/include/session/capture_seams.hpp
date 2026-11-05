#pragma once

// The clock (C1) and the transport (C2) as a capture (C3) sees them
// (docs/canned-tests-plan.md, step K3): a recording twin of each writes every
// read into a capture_file as the live one answers it; a replaying twin
// answers from the file, and a read the live run did not make calls
// `diverged`.  A message and a state change are written before the host
// handles them, so the reads their handling makes follow them in the file.
// The capture is one thread's (step K4b-ii: the server's network thread; the
// headless client's only thread): a read made on any other thread - the
// server's runner and its callbacks - goes to the live clock or transport
// (the replay's own clock; a replayed transport answers it with nothing) and
// is not in the file.  CK-free and header-only.

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "../net/transport.hpp"
#include "capture.hpp"
#include "session_clock.hpp"

namespace bmmo::session {

    using capture_diverged = std::function<void(const std::string& why)>;

    // The thread whose reads the capture holds: the constructing one until
    // claim() hands it to the caller (the server's network loop, once it runs).
    class capture_owner {
    public:
        void claim() { owner_.store(std::this_thread::get_id()); }
        bool here() const { return owner_.load() == std::this_thread::get_id(); }

    private:
        std::atomic<std::thread::id> owner_{std::this_thread::get_id()};
    };

    class recording_clock final : public session_clock {
    public:
        recording_clock(session_clock& live, capture_file& file) : live_(live), file_(file) {}
        time_point now(clock_site site) override {
            const auto v = live_.now(site);
            if (owner.here()) file_.clock(site, static_cast<int64_t>(v.time_since_epoch().count()));
            return v;
        }
        int64_t utc_us(clock_site site) override {
            const int64_t v = live_.utc_us(site);
            if (owner.here()) file_.clock(site, v);
            return v;
        }
        uint64_t utc_ms(clock_site site) override {
            const uint64_t v = live_.utc_ms(site);
            if (owner.here()) file_.clock(site, static_cast<int64_t>(v));
            return v;
        }
        capture_owner owner;

    private:
        session_clock& live_;
        capture_file& file_;
    };

    // A free site (capture_file::set_free) is read from `own` and never
    // taken from the file.
    class replay_clock final : public session_clock {
    public:
        replay_clock(capture_file& file, session_clock& own, capture_diverged diverged)
            : file_(file), own_(own), diverged_(std::move(diverged)) {}
        time_point now(clock_site site) override {
            if (file_.is_free(site) || !owner.here()) return own_.now(site);
            return time_point(clock_type::duration(take(site)));
        }
        int64_t utc_us(clock_site site) override { return owner.here() ? take(site) : own_.utc_us(site); }
        uint64_t utc_ms(clock_site site) override {
            return owner.here() ? static_cast<uint64_t>(take(site)) : own_.utc_ms(site);
        }
        capture_owner owner;

    private:
        int64_t take(clock_site site) {
            int64_t value = 0;
            std::string error;
            if (!file_.take_clock(site, value, error)) diverged_(error);
            return value;
        }
        capture_file& file_;
        session_clock& own_;
        capture_diverged diverged_;
    };

    class recording_transport final : public net::transport {
    public:
        recording_transport(std::unique_ptr<net::transport> live, capture_file& file)
            : live_(std::move(live)), file_(file) {}

        bool listen(uint16_t port) override { return live_->listen(port); }
        accept_result accept(net::connection c) override { return live_->accept(c); }
        net::connection connect(const std::string& address) override { return live_->connect(address); }
        bool send(net::connection c, const void* data, size_t size, int flags) override {
            return live_->send(c, data, size, flags);
        }
        void flush(net::connection c) override { live_->flush(c); }
        void close(net::connection c, int reason, const std::string& debug, bool linger) override {
            live_->close(c, reason, debug, linger);
        }
        void set_name(net::connection c, const std::string& name) override { live_->set_name(c, name); }
        bool status(net::connection c, net::connection_status& out) override {
            const bool known = live_->status(c, out);
            if (owner.here()) file_.link_status(c, known, out);
            return known;
        }
        std::string detailed_status(net::connection c) override { return live_->detailed_status(c); }
        std::string remote_address(net::connection c) override { return live_->remote_address(c); }
        int receive(const std::function<void(const net::message&)>& each, int max) override {
            return live_->receive([&](const net::message& m) {
                file_.message(m);
                each(m);
            }, max);
        }
        void poll_status(const std::function<void(const net::status_change&)>& each) override {
            live_->poll_status([&](const net::status_change& c) {
                file_.status(c);
                each(c);
            });
        }
        net::microseconds local_timestamp() override {
            const auto v = live_->local_timestamp();
            if (owner.here()) file_.link_clock(v);
            return v;
        }
        bool set_fake_lag(direction d, int ms) override { return live_->set_fake_lag(d, ms); }
        bool set_fake_loss(direction d, float percent) override { return live_->set_fake_loss(d, percent); }
        int connection_timeout_ms() override { return live_->connection_timeout_ms(); }
        capture_owner owner;

    private:
        std::unique_ptr<net::transport> live_;
        capture_file& file_;
    };

    // Sends nothing and connects nowhere: what came in is the file's.
    class replay_transport final : public net::transport {
    public:
        replay_transport(capture_file& file, capture_diverged diverged)
            : file_(file), diverged_(std::move(diverged)) {}

        bool listen(uint16_t) override { return true; }
        accept_result accept(net::connection) override { return accept_result::ok; }
        net::connection connect(const std::string&) override { return net::kInvalidConnection; }
        bool send(net::connection, const void*, size_t, int) override { return true; }
        void flush(net::connection) override {}
        void close(net::connection, int, const std::string&, bool) override {}
        void set_name(net::connection, const std::string&) override {}
        bool status(net::connection c, net::connection_status& out) override {
            if (!owner.here()) return false;
            bool known = false;
            std::string error;
            if (!file_.take_link_status(c, known, out, error)) diverged_(error);
            return known;
        }
        std::string detailed_status(net::connection c) override { return "replayed connection " + std::to_string(c); }
        std::string remote_address(net::connection) override { return {}; }
        int receive(const std::function<void(const net::message&)>& each, int max) override {
            int count = 0;
            capture_file::message_record m;
            while (count < max && file_.take_message(m)) {
                each(net::message{m.conn, m.bytes.data(), static_cast<int>(m.bytes.size()), m.received});
                ++count;
            }
            return count;
        }
        void poll_status(const std::function<void(const net::status_change&)>& each) override {
            net::status_change c;
            while (file_.take_status(c)) each(c);
        }
        net::microseconds local_timestamp() override {
            if (!owner.here()) return 0;
            net::microseconds value = 0;
            std::string error;
            if (!file_.take_link_clock(value, error)) diverged_(error);
            return value;
        }
        bool set_fake_lag(direction, int) override { return true; }
        bool set_fake_loss(direction, float) override { return true; }
        int connection_timeout_ms() override { return 0; }
        capture_owner owner;

    private:
        capture_file& file_;
        capture_diverged diverged_;
    };

}
