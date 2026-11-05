#pragma once

// A process's transport in a coordinated run (docs/canned-tests-plan.md, steps
// T1b, T1e): the network is the coordinator's (coordinator.hpp, a sim_network),
// the time is a virtual_clock.  The transport calls go to the coordinator as
// ops, tagged with the sending thread's rank, a sequence and the virtual time.
//
// Rounds (T1e, the lookahead): the coordinator grants a time up to which this
// process runs on its own - nothing another process does can reach it sooner
// than a link's latency.  With the grant come the messages and the state
// changes that reach this process by then, each with its time; they wait here
// until the clock gets there (a receive or a poll takes what is due by now),
// as GNS holds what arrived until a receive.
//
// The link thread: when every participant of the process waits
// (virtual_clock::wait_idle), it advances the clock to the earliest wake if the
// grant reaches it; otherwise it sends the round's ops and "idle, next wake t"
// and takes the next grant.  On stop it calls the host's on_stop and lets the
// clock run free (the host's loops wind down on their own waits) until the
// transport goes.
//
// What this process did itself it learns at once: its connect's "connecting",
// its accept's "connected" (with the round trip the connect's notice brought).
// After its own close a connection has nothing more to say.
//
// Connection handles: this process's own connects get 1, 2, ... in the order it
// makes them; a connection the coordinator brings in gets the coordinator's
// number (0x10000 up).  Header-only.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <asio/connect.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "../session/virtual_clock.hpp"
#include "coordinator_protocol.hpp"
#include "transport.hpp"

namespace bmmo::net {

    class remote_transport final : public transport {
    public:
        explicit remote_transport(session::virtual_clock& clock) : clock_(clock), socket_(io_), reader_(socket_) {}
        ~remote_transport() override {
            closing_ = true;
            {
                std::lock_guard lk(write_);
                coord::body_out b;
                b.put<int64_t>(now_us());
                coord::append_frame(out_, coord::op::bye, b.bytes());
                coord::write_all(socket_, out_);
            }
            clock_.close();
            std::error_code ec;
            socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            socket_.close(ec);
            if (link_.joinable()) link_.join();
        }

        // Connects to the coordinator at "host:port" and says who this is.
        bool open(const std::string& address, uint8_t role, const std::string& name, std::string& error) {
            const auto colon = address.rfind(':');
            if (colon == std::string::npos) { error = "coordinator address " + address + ": no port"; return false; }
            std::error_code ec;
            asio::ip::tcp::resolver resolver(io_);
            const auto endpoints = resolver.resolve(address.substr(0, colon), address.substr(colon + 1), ec);
            if (!ec) asio::connect(socket_, endpoints, ec);
            if (ec) { error = "coordinator " + address + ": " + ec.message(); return false; }
            socket_.set_option(asio::ip::tcp::no_delay(true), ec);
            coord::body_out hello;
            hello.put<uint8_t>(role).str(name);
            coord::op o{};
            std::string body;
            if (!coord::write_frame(socket_, coord::op::hello, hello.bytes()) || !coord::read_frame(socket_, o, body)
                    || o != coord::op::welcome) {
                error = "coordinator " + address + ": no welcome";
                return false;
            }
            coord::body_in in(body);
            index_ = in.get<uint32_t>();
            return in.ok();
        }
        uint32_t index() const { return index_; }

        // The run starts: `participants` threads of this process wait through
        // the clock; `on_stop` is called (on the link thread) when the
        // coordinator ends the run.
        void start(size_t participants, std::function<void()> on_stop) {
            on_stop_ = std::move(on_stop);
            clock_.set_participants(participants);
            link_ = std::thread([this] { link(); });
        }
        bool stopped() const { return stopped_; }

        bool listen(uint16_t port) override {
            coord::body_out b;
            b.put<uint16_t>(port);
            tagged(coord::op::listen, b);
            return true;
        }
        accept_result accept(connection c) override {
            coord::body_out b;
            b.put<uint32_t>(c);
            tagged(coord::op::accept, b);
            std::lock_guard lk(state_);
            const auto it = links_.find(c);
            learn_locked(c, now_us(), connection_state::connected, kEndInvalid, {}, it == links_.end() ? -1 : it->second.round_trip);
            return accept_result::ok;
        }
        connection connect(const std::string& address) override {
            connection local;
            {
                std::lock_guard lk(state_);
                local = next_local_++;
            }
            coord::body_out b;
            b.put<uint32_t>(local).str(address);
            tagged(coord::op::connect, b);
            std::lock_guard lk(state_);
            learn_locked(local, now_us(), connection_state::connecting, kEndInvalid, {}, -1);
            return local;
        }
        bool send(connection c, const void* data, size_t size, int flags) override {
            coord::body_out b;
            b.put<uint32_t>(c).put<int32_t>(flags).str(std::string(static_cast<const char*>(data), size));
            tagged(coord::op::send, b);
            return true;
        }
        void flush(connection) override {}
        void close(connection c, int reason, const std::string& debug, bool linger) override {
            coord::body_out b;
            b.put<uint32_t>(c).put<int32_t>(reason).str(debug).put<uint8_t>(linger ? 1 : 0);
            tagged(coord::op::close, b);
            std::lock_guard lk(state_);
            links_.erase(c);
            const auto other = [c](const auto& e) { return e.conn == c; };
            inbox_.erase(std::remove_if(inbox_.begin(), inbox_.end(), other), inbox_.end());
            notices_.erase(std::remove_if(notices_.begin(), notices_.end(), other), notices_.end());
        }
        void set_name(connection c, const std::string& name) override {
            coord::body_out b;
            b.put<uint32_t>(c).str(name);
            tagged(coord::op::set_name, b);
            std::lock_guard lk(state_);
            links_[c].name = name;
        }
        bool status(connection c, connection_status& out) override {
            std::lock_guard lk(state_);
            const auto it = links_.find(c);
            if (it == links_.end() || it->second.state != connection_state::connected || it->second.ping < 0) return false;
            out = connection_status{};
            out.ping_ms = it->second.ping;
            out.quality_local = out.quality_remote = 1.0f;
            return true;
        }
        std::string detailed_status(connection c) override { return "coordinated connection " + std::to_string(c); }
        std::string remote_address(connection) override { return "127.0.0.1"; }
        int receive(const std::function<void(const message&)>& each, int max) override {
            std::deque<inbound> due;
            {
                std::lock_guard lk(state_);
                const microseconds now = now_us();
                while (!inbox_.empty() && inbox_.front().at <= now && static_cast<int>(due.size()) < max) {
                    due.push_back(std::move(inbox_.front()));
                    inbox_.pop_front();
                }
            }
            for (auto& m: due) each(message{m.conn, m.bytes.data(), static_cast<int>(m.bytes.size()), m.at});
            return static_cast<int>(due.size());
        }
        void poll_status(const std::function<void(const status_change&)>& each) override {
            std::deque<status_change> due;
            {
                std::lock_guard lk(state_);
                const microseconds now = now_us();
                while (!notices_.empty() && notices_.front().at <= now) {
                    const notice n = std::move(notices_.front());
                    notices_.pop_front();
                    auto& l = links_[n.conn];
                    if (n.ping_only) {
                        if (l.state == connection_state::connected) l.ping = n.ping;
                        continue;
                    }
                    status_change c;
                    c.conn = n.conn;
                    c.old_state = l.state;
                    c.state = n.state;
                    c.end_reason = n.reason;
                    c.end_debug = n.debug;
                    c.description = "#" + std::to_string(n.conn) + (l.name.empty() ? "" : " " + l.name);
                    l.state = n.state;
                    if (n.state == connection_state::connecting) l.round_trip = n.ping;
                    if (n.state == connection_state::connected) l.ping = n.ping;
                    due.push_back(std::move(c));
                }
            }
            for (const auto& c: due) each(c);
        }
        microseconds local_timestamp() override { return now_us(); }
        bool set_fake_lag(direction d, int ms) override {
            coord::body_out b;
            b.put<uint8_t>(d == direction::send ? 0 : 1).put<int32_t>(ms);
            tagged(coord::op::set_lag, b);
            return true;
        }
        bool set_fake_loss(direction d, float percent) override {
            coord::body_out b;
            b.put<uint8_t>(d == direction::send ? 0 : 1).put<float>(percent);
            tagged(coord::op::set_loss, b);
            return true;
        }
        int connection_timeout_ms() override { return 7200; }

        // Step LF (the fast LBM layer): the real-time harness's link events on
        // this process's end of its links, from its virtual time on - its
        // latency moved by `delta_ms` (a step and its end; with `dur_ms`, a
        // spike for what is sent in that long), a blackout of `ms`.
        void link_lag(direction d, int delta_ms, int dur_ms = 0) {
            coord::body_out b;
            b.put<uint8_t>(d == direction::send ? 0 : 1).put<int32_t>(delta_ms).put<int32_t>(dur_ms);
            tagged(coord::op::link_lag, b);
        }
        void link_blackout(direction d, int ms) {
            coord::body_out b;
            b.put<uint8_t>(d == direction::send ? 0 : 1).put<int32_t>(ms);
            tagged(coord::op::link_dark, b);
        }

    private:
        struct inbound {
            connection conn;
            microseconds at;
            std::string bytes;
        };
        struct notice {
            connection conn;
            microseconds at;
            connection_state state;
            int reason;
            std::string debug;
            int ping;
            bool ping_only = false;   // step LF: the ping changed, not the state
        };
        struct link_state {
            connection_state state = connection_state::none;
            std::string name;
            int round_trip = -1;   // from the connect's notice: an accept's ping
            int ping = -1;
        };

        microseconds now_us() {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                clock_.now(session::clock_site::server_poll).time_since_epoch()).count();
        }
        // A state change this process learns at `at` (its own: now), in time
        // order after what it already holds for then.
        void learn_locked(connection c, microseconds at, connection_state s, int reason, std::string debug, int ping,
                          bool ping_only = false) {
            notice n{c, at, s, reason, std::move(debug), ping, ping_only};
            notices_.insert(std::upper_bound(notices_.begin(), notices_.end(), n,
                                             [](const notice& a, const notice& b) { return a.at < b.at; }),
                            std::move(n));
        }

        void tagged(coord::op o, const coord::body_out& b) {
            const uint8_t rank = thread_rank();
            const microseconds at = now_us();
            std::lock_guard lk(write_);
            coord::body_out tag;
            tag.put<uint8_t>(rank).put<uint64_t>(++sequence_[rank]).put<int64_t>(at);
            coord::append_frame(out_, o, tag.bytes() + b.bytes());
        }

        void link() {
            bool granted = false;
            microseconds until = 0;
            while (!closing_) {
                // (a diagnostic for a hang: what the clock waits for, every 5 s
                // of real time without idle)
                const auto wake = clock_.wait_idle(std::chrono::seconds(5), [this](const std::string& what) {
                    std::fprintf(stderr, "coordinated process %u: not idle for 5 s of real time: %s\n", index_, what.c_str());
                    std::fflush(stderr);
                });
                if (closing_ || clock_.closed()) return;
                // The wake in whole microseconds, rounded up: an advance to it
                // must reach the wait (a tick's 15151.515 us rounded down is a
                // time short of it, and the run would step there for ever).
                const microseconds w = wake ? std::chrono::ceil<std::chrono::microseconds>(wake->time_since_epoch()).count() : 0;
                if (granted && wake && w <= until) {
                    clock_.advance_to(session::session_clock::time_point(std::chrono::microseconds(w)));
                    continue;
                }
                coord::body_out idle;
                idle.put<uint8_t>(wake ? 1 : 0).put<int64_t>(w);
                {
                    std::lock_guard lk(write_);
                    coord::append_frame(out_, coord::op::idle, idle.bytes());
                    if (!coord::write_all(socket_, out_)) return;
                }
                if (!take_grant(until)) return;
                granted = true;
            }
        }

        // The coordinator's answer to one idle: deliveries and changes, then
        // the grant (or the stop).
        bool take_grant(microseconds& until) {
            for (;;) {
                coord::op o{};
                std::string body;
                if (!reader_.next(o, body)) return false;
                coord::body_in in(body);
                switch (o) {
                case coord::op::deliver: {
                    inbound m;
                    m.conn = in.get<uint32_t>();
                    m.at = in.get<int64_t>();
                    m.bytes = in.str();
                    std::lock_guard lk(state_);
                    inbox_.push_back(std::move(m));
                    break;
                }
                case coord::op::status: {
                    const connection c = in.get<uint32_t>();
                    const microseconds at = in.get<int64_t>();
                    const auto s = static_cast<connection_state>(in.get<int32_t>());
                    const auto reason = in.get<int32_t>();
                    auto debug = in.str();
                    const auto ping = in.get<int32_t>();
                    std::lock_guard lk(state_);
                    learn_locked(c, at, s, reason, std::move(debug), ping);
                    break;
                }
                case coord::op::ping: {
                    const connection c = in.get<uint32_t>();
                    const microseconds at = in.get<int64_t>();
                    const auto ping = in.get<int32_t>();
                    std::lock_guard lk(state_);
                    learn_locked(c, at, connection_state::connected, 0, {}, ping, true);
                    break;
                }
                case coord::op::grant:
                    until = in.get<int64_t>();
                    return true;
                case coord::op::stop:
                    stopped_ = true;
                    if (on_stop_) on_stop_();
                    // The host's loops wind down on their own waits: the clock
                    // runs free from here.
                    while (!closing_) {
                        clock_.advance_to(clock_.now(session::clock_site::server_poll) + std::chrono::milliseconds(100));
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    return false;
                default:
                    return false;
                }
            }
        }

        session::virtual_clock& clock_;
        asio::io_context io_;
        asio::ip::tcp::socket socket_;
        coord::frame_reader reader_;              // the link thread's
        uint32_t index_ = 0;
        std::thread link_;
        std::function<void()> on_stop_;
        std::atomic<bool> closing_{false}, stopped_{false};
        std::mutex write_;
        std::string out_;                         // write_: the ops not yet sent
        std::map<uint8_t, uint64_t> sequence_;    // write_
        std::mutex state_;
        connection next_local_ = 1;               // state_
        std::deque<inbound> inbox_;               // state_, by time
        std::deque<notice> notices_;              // state_, by time
        std::map<connection, link_state> links_;  // state_
    };

}
