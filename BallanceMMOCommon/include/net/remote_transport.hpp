#pragma once

// A process's transport in a coordinated run (docs/canned-tests-plan.md, step
// T1b): the network is the coordinator's (coordinator.hpp, a sim_network), the
// time is a virtual_clock the coordinator advances.  The transport calls go to
// the coordinator as ops, tagged with the sending thread's rank and a sequence;
// what the network delivers comes back between two steps and waits here until
// the host polls, as GNS holds what arrived until a receive.
//
// The link thread: when every participant of the process waits
// (virtual_clock::wait_idle), it reports "idle, next wake t" and takes the
// coordinator's answer - the deliveries, the connection state changes, then the
// time to advance to.  On stop it calls the host's on_stop and lets the clock run
// free (the host's loops wind down on their own waits) until the transport goes.
//
// Connection handles: this process's own connects get 1, 2, ... in the order it
// makes them; a connection the coordinator brings in gets the coordinator's
// number (0x10000 up).  Header-only.

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
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
        explicit remote_transport(session::virtual_clock& clock) : clock_(clock), socket_(io_) {}
        ~remote_transport() override {
            closing_ = true;
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
            return tagged(coord::op::listen, b);
        }
        accept_result accept(connection c) override {
            coord::body_out b;
            b.put<uint32_t>(c);
            return tagged(coord::op::accept, b) ? accept_result::ok : accept_result::refused;
        }
        connection connect(const std::string& address) override {
            connection local;
            {
                std::lock_guard lk(state_);
                local = next_local_++;
            }
            coord::body_out b;
            b.put<uint32_t>(local).str(address);
            return tagged(coord::op::connect, b) ? local : kInvalidConnection;
        }
        bool send(connection c, const void* data, size_t size, int flags) override {
            coord::body_out b;
            b.put<uint32_t>(c).put<int32_t>(flags).str(std::string(static_cast<const char*>(data), size));
            return tagged(coord::op::send, b);
        }
        void flush(connection) override {}
        void close(connection c, int reason, const std::string& debug, bool linger) override {
            coord::body_out b;
            b.put<uint32_t>(c).put<int32_t>(reason).str(debug).put<uint8_t>(linger ? 1 : 0);
            tagged(coord::op::close, b);
            std::lock_guard lk(state_);
            pings_.erase(c);
        }
        void set_name(connection c, const std::string& name) override {
            coord::body_out b;
            b.put<uint32_t>(c).str(name);
            tagged(coord::op::set_name, b);
        }
        bool status(connection c, connection_status& out) override {
            std::lock_guard lk(state_);
            const auto it = pings_.find(c);
            if (it == pings_.end()) return false;
            out = connection_status{};
            out.ping_ms = it->second;
            out.quality_local = out.quality_remote = 1.0f;
            return true;
        }
        std::string detailed_status(connection c) override { return "coordinated connection " + std::to_string(c); }
        std::string remote_address(connection) override { return "127.0.0.1"; }
        int receive(const std::function<void(const message&)>& each, int max) override {
            std::deque<inbound> due;
            {
                std::lock_guard lk(state_);
                while (!inbox_.empty() && static_cast<int>(due.size()) < max) {
                    due.push_back(std::move(inbox_.front()));
                    inbox_.pop_front();
                }
            }
            for (auto& m: due) each(message{m.conn, m.bytes.data(), static_cast<int>(m.bytes.size()), m.received});
            return static_cast<int>(due.size());
        }
        void poll_status(const std::function<void(const status_change&)>& each) override {
            std::deque<status_change> due;
            {
                std::lock_guard lk(state_);
                due.swap(changes_);
            }
            for (const auto& c: due) each(c);
        }
        microseconds local_timestamp() override {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                clock_.now(session::clock_site::server_poll).time_since_epoch()).count();
        }
        bool set_fake_lag(direction d, int ms) override {
            coord::body_out b;
            b.put<uint8_t>(d == direction::send ? 0 : 1).put<int32_t>(ms);
            return tagged(coord::op::set_lag, b);
        }
        bool set_fake_loss(direction d, float percent) override {
            coord::body_out b;
            b.put<uint8_t>(d == direction::send ? 0 : 1).put<float>(percent);
            return tagged(coord::op::set_loss, b);
        }
        int connection_timeout_ms() override { return 7200; }

    private:
        struct inbound {
            connection conn;
            microseconds received;
            std::string bytes;
        };

        bool tagged(coord::op o, const coord::body_out& b) {
            std::lock_guard lk(write_);
            coord::body_out framed;
            framed.put<uint8_t>(thread_rank()).put<uint64_t>(++sequence_[thread_rank()]);
            return coord::write_frame(socket_, o, framed.bytes() + b.bytes());
        }

        void link() {
            while (!closing_) {
                const auto wake = clock_.wait_idle();
                if (closing_ || clock_.closed()) return;
                // The wake in whole microseconds, rounded up: an advance to it
                // must reach the wait (a tick's 15151.515 us rounded down is a
                // time short of it, and the run would step there for ever).
                coord::body_out idle;
                idle.put<uint8_t>(wake ? 1 : 0)
                    .put<int64_t>(wake ? std::chrono::ceil<std::chrono::microseconds>(wake->time_since_epoch()).count() : 0);
                {
                    std::lock_guard lk(write_);
                    if (!coord::write_frame(socket_, coord::op::idle, idle.bytes())) return;
                }
                if (!take_step()) return;
            }
        }

        // The coordinator's answer to one idle: deliveries and changes, then
        // the time (or the stop).
        bool take_step() {
            for (;;) {
                coord::op o{};
                std::string body;
                if (!coord::read_frame(socket_, o, body)) return false;
                coord::body_in in(body);
                switch (o) {
                case coord::op::deliver: {
                    inbound m;
                    m.conn = in.get<uint32_t>();
                    m.received = in.get<int64_t>();
                    m.bytes = in.str();
                    std::lock_guard lk(state_);
                    inbox_.push_back(std::move(m));
                    break;
                }
                case coord::op::status: {
                    status_change c;
                    c.conn = in.get<uint32_t>();
                    c.state = static_cast<connection_state>(in.get<int32_t>());
                    c.old_state = static_cast<connection_state>(in.get<int32_t>());
                    c.end_reason = in.get<int32_t>();
                    c.end_debug = in.str();
                    c.description = in.str();
                    std::lock_guard lk(state_);
                    if (c.state != connection_state::connected && c.state != connection_state::connecting) pings_.erase(c.conn);
                    changes_.push_back(std::move(c));
                    break;
                }
                case coord::op::link: {
                    const auto c = in.get<uint32_t>();
                    const auto ping = in.get<int32_t>();
                    std::lock_guard lk(state_);
                    pings_[c] = ping;
                    break;
                }
                case coord::op::advance:
                    clock_.advance_to(session::session_clock::time_point(std::chrono::microseconds(in.get<int64_t>())));
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
        uint32_t index_ = 0;
        std::thread link_;
        std::function<void()> on_stop_;
        std::atomic<bool> closing_{false}, stopped_{false};
        std::mutex write_;
        std::map<uint8_t, uint64_t> sequence_;   // write_
        std::mutex state_;
        connection next_local_ = 1;              // state_
        std::deque<inbound> inbox_;              // state_
        std::deque<status_change> changes_;      // state_
        std::map<connection, int> pings_;        // state_
    };

}
