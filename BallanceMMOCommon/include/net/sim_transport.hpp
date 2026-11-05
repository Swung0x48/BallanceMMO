#pragma once

// A simulated network for bmmo::net::transport (step K2): any number of
// transports in one process, on one virtual clock the owner advances.  For
// unit tests of the session code over a network, and the shape the stage 1
// coordinator's network takes (docs/canned-tests-plan.md).
//
// The model, deliberately small: a link's latency per direction; reliable
// messages arrive in order after it (a loss costs nothing but its latency:
// the retransmit is not modelled); unreliable ones are lost with the link's
// loss rate (a seeded generator: the same seed loses the same messages).
// Handles count from 1 in the order connections are made.  Header-only.

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "transport.hpp"

namespace bmmo::net {

    class sim_network {
    public:
        explicit sim_network(uint64_t seed = 1) : random_(seed) {}

        microseconds now() const { return now_; }
        void advance(microseconds by) { now_ += by; }

        std::unique_ptr<transport> make_transport();

    private:
        class endpoint;
        struct packet {
            microseconds at = 0;
            uint64_t seq = 0;
            connection to = kInvalidConnection;
            std::string bytes;
        };
        struct pending_close {   // a close the peer learns once its link has carried it
            microseconds at = 0;
            connection to = kInvalidConnection;
            int reason = kEndInvalid;
            std::string debug;
        };
        struct half {   // one side of a connection
            endpoint* owner = nullptr;
            connection peer = kInvalidConnection;
            connection_state state = connection_state::none;
            microseconds last_reliable = 0;   // the in-order floor of its reliable sends
            std::string name;
        };

        half* find(connection c) {
            const auto it = halves_.find(c);
            return it == halves_.end() ? nullptr : &it->second;
        }
        void change(connection c, connection_state to, int reason = kEndInvalid, std::string debug = {});
        bool deliver(connection from, const void* data, size_t size, int flags);
        void close(connection c, int reason, const std::string& debug, bool linger);
        void settle_closes();

        microseconds now_ = 0;
        uint64_t seq_ = 0;
        connection next_ = 1;
        std::mt19937_64 random_;
        std::map<connection, half> halves_;
        std::map<uint16_t, endpoint*> listeners_;
        std::vector<packet> packets_;   // in flight
        std::vector<pending_close> closes_;

        friend class endpoint;
    };

    class sim_network::endpoint final : public transport {
    public:
        explicit endpoint(sim_network& net) : net_(net) {}
        ~endpoint() override {
            for (auto it = net_.listeners_.begin(); it != net_.listeners_.end();)
                it = it->second == this ? net_.listeners_.erase(it) : std::next(it);
            for (auto& [c, h]: net_.halves_)
                if (h.owner == this) h.owner = nullptr;
        }

        bool listen(uint16_t port) override {
            if (net_.listeners_.count(port)) return false;
            net_.listeners_[port] = this;
            return true;
        }

        accept_result accept(connection c) override {
            half* h = net_.find(c);
            if (!h || h->owner != this || h->state != connection_state::connecting) return accept_result::refused;
            net_.change(c, connection_state::connected);
            net_.change(h->peer, connection_state::connected);
            return accept_result::ok;
        }

        connection connect(const std::string& address) override {
            const auto colon = address.rfind(':');
            if (colon == std::string::npos) return kInvalidConnection;
            const auto port = static_cast<uint16_t>(std::atoi(address.c_str() + colon + 1));
            const auto server = net_.listeners_.find(port);
            if (server == net_.listeners_.end()) return kInvalidConnection;
            const connection mine = net_.next_++, theirs = net_.next_++;
            net_.halves_[mine] = half{this, theirs, connection_state::none, 0, {}};
            net_.halves_[theirs] = half{server->second, mine, connection_state::none, 0, {}};
            net_.change(mine, connection_state::connecting);
            net_.change(theirs, connection_state::connecting);
            return mine;
        }

        bool send(connection c, const void* data, size_t size, int flags) override {
            const half* h = net_.find(c);
            return h && h->owner == this && net_.deliver(c, data, size, flags);
        }
        void flush(connection) override {}
        void close(connection c, int reason, const std::string& debug, bool linger) override {
            net_.close(c, reason, debug, linger);
        }
        void set_name(connection c, const std::string& name) override {
            if (half* h = net_.find(c)) h->name = name;
        }

        // The link's round trip: this side's and the peer's latencies, both
        // ways (step T1b: a coordinated run gives the latency to the clients'
        // ends, and the server must see it too).
        bool status(connection c, connection_status& out) override {
            const half* h = net_.find(c);
            if (!h) return false;
            const half* p = net_.find(h->peer);
            const endpoint* peer = p ? p->owner : nullptr;
            out = connection_status{};
            out.ping_ms = static_cast<int>((lag_us_[0] + lag_us_[1] + (peer ? peer->lag_us_[0] + peer->lag_us_[1] : 0)) / 1000);
            out.quality_local = out.quality_remote = 1.0f - loss_[0] / 100.0f;
            return true;
        }
        std::string detailed_status(connection c) override {
            return "sim connection " + std::to_string(c);
        }
        std::string remote_address(connection c) override {
            return net_.find(c) ? "127.0.0.1" : "";
        }

        int receive(const std::function<void(const message&)>& each, int max) override {
            std::vector<packet> due;
            for (auto it = net_.packets_.begin(); it != net_.packets_.end() && static_cast<int>(due.size()) < max;) {
                const half* h = net_.find(it->to);
                if (h && h->owner == this && h->state == connection_state::connected && it->at <= net_.now_) {
                    due.push_back(std::move(*it));
                    it = net_.packets_.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto& p: due)
                each(message{p.to, p.bytes.data(), static_cast<int>(p.bytes.size()), p.at});
            return static_cast<int>(due.size());
        }

        void poll_status(const std::function<void(const status_change&)>& each) override {
            net_.settle_closes();
            while (!changes_.empty()) {
                const status_change change = std::move(changes_.front());
                changes_.pop_front();
                each(change);
            }
        }

        microseconds local_timestamp() override { return net_.now_; }

        bool set_fake_lag(direction d, int ms) override {
            lag_us_[d == direction::send ? 0 : 1] = static_cast<microseconds>(ms) * 1000;
            return true;
        }
        bool set_fake_loss(direction d, float percent) override {
            loss_[d == direction::send ? 0 : 1] = percent;
            return true;
        }
        int connection_timeout_ms() override { return 7200; }

    private:
        friend class sim_network;
        sim_network& net_;
        std::deque<status_change> changes_;
        microseconds lag_us_[2] = {0, 0};   // send, recv
        float loss_[2] = {0, 0};             // percent
    };

    inline std::unique_ptr<transport> sim_network::make_transport() { return std::make_unique<endpoint>(*this); }

    inline void sim_network::change(connection c, connection_state to, int reason, std::string debug) {
        half* h = find(c);
        if (!h) return;
        status_change ch;
        ch.conn = c;
        ch.old_state = h->state;
        ch.state = to;
        ch.end_reason = reason;
        ch.end_debug = std::move(debug);
        ch.description = "#" + std::to_string(c) + (h->name.empty() ? "" : " " + h->name);
        h->state = to;
        if (h->owner) h->owner->changes_.push_back(std::move(ch));
    }

    inline bool sim_network::deliver(connection from, const void* data, size_t size, int flags) {
        half* h = find(from);
        if (!h || h->state != connection_state::connected) return false;
        half* peer = find(h->peer);
        if (!peer) return false;
        const auto* sender = h->owner;
        const auto* receiver = peer->owner;
        microseconds at = now_ + sender->lag_us_[0] + (receiver ? receiver->lag_us_[1] : 0);
        if (flags & kSendReliable) {
            at = std::max(at, h->last_reliable);
            h->last_reliable = at;
        } else {
            const float loss = sender->loss_[0] + (receiver ? receiver->loss_[1] : 0.0f);
            if (std::uniform_real_distribution<float>(0.0f, 100.0f)(random_) < loss) return true;   // sent, lost
        }
        packets_.push_back(packet{at, seq_++, h->peer, std::string(static_cast<const char*>(data), size)});
        std::stable_sort(packets_.begin(), packets_.end(),
                         [](const packet& a, const packet& b) { return a.at != b.at ? a.at < b.at : a.seq < b.seq; });
        return true;
    }

    // The peer learns of a close after the link's latency; with `linger`, only
    // once the reliable data still in flight has arrived (receive() hands that
    // over first, poll_status the close after it).  Without it the data is lost.
    inline void sim_network::close(connection c, int reason, const std::string& debug, bool linger) {
        half* h = find(c);
        if (!h) return;
        const connection peer = h->peer;
        microseconds at = now_ + (h->owner ? h->owner->lag_us_[0] : 0);
        if (linger) at = std::max(at, h->last_reliable);
        else
            packets_.erase(std::remove_if(packets_.begin(), packets_.end(),
                                          [peer](const packet& p) { return p.to == peer; }), packets_.end());
        const bool was_open = h->state == connection_state::connected || h->state == connection_state::connecting;
        halves_.erase(c);
        if (was_open && find(peer)) closes_.push_back(pending_close{at, peer, reason, debug});
    }

    inline void sim_network::settle_closes() {
        for (auto it = closes_.begin(); it != closes_.end();) {
            if (it->at > now_) { ++it; continue; }
            change(it->to, connection_state::closed_by_peer, it->reason, it->debug);
            it = closes_.erase(it);
        }
    }

}
