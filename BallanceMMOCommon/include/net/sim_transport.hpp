#pragma once

// A simulated network for bmmo::net::transport (step K2): any number of
// transports in one process, on one virtual clock the owner advances.  For
// unit tests of the session code over a network, and the coordinator's
// network (docs/canned-tests-plan.md, T1).
//
// The model, deliberately small: a link's latency per direction; reliable
// messages arrive in order after it (a loss costs nothing but its latency:
// the retransmit is not modelled); unreliable ones are lost with the link's
// loss rate (a seeded generator: the same seed loses the same messages), in
// bursts when the link says so (scripts/arena_map_test.py DelayProxy's rule:
// a loss takes the next burst - 1 with it, the start rate solved so the loss
// rate holds).  Every message of a connection's direction arrives in order
// (the proxy keeps its order across a latency change too).  The link events
// of the real-time harness (step LF, the fast LBM layer): a latency changed
// mid-run (a step, a spike), a blackout (unreliable messages dropped, reliable
// ones arriving once it ends - GNS's retransmit, roughly).
//
// Nothing crosses a link faster than its latency (T1e, the coordinator's
// lookahead): a connect reaches the listener after the link's latency, an
// accept the connector, a close the peer.  What an endpoint did itself (its own
// connect's "connecting", its own accept's "connected") it learns at once - a
// local notice.  A close drops nothing in flight: the data on the wire arrives
// before the close does (with `linger`, also the reliable data still queued).
// Handles count from 1 in the order connections are made.  Header-only.

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "transport.hpp"

namespace bmmo::net {

    class sim_network {
    public:
        explicit sim_network(uint64_t seed = 1) : random_(seed) {}

        microseconds now() const { return now_; }
        void advance(microseconds by) { advance_to(now_ + by); }
        // Moves the time forward (never back); a state change due by then
        // takes effect.
        void advance_to(microseconds t) {
            now_ = std::max(now_, t);
            settle();
        }

        std::unique_ptr<transport> make_transport();

        // The coordinator's link events on `t`'s end (step LF): a loss burst,
        // a latency moved by `ms` (never below the one set_fake_lag gave: the
        // lookahead counts on that one), `ms` more for what is sent before
        // `until` (a spike), a blackout until `until`.
        static void set_loss_burst(transport& t, transport::direction d, int burst);
        static void add_lag(transport& t, transport::direction d, int ms);
        static void spike(transport& t, transport::direction d, int ms, microseconds until);
        static void blackout(transport& t, transport::direction d, microseconds until);

        // The coordinator's side (T1e).  What `t` is due by `until` - the
        // messages to it and the state changes it learns from its peers, each
        // with its time - taken now, before the network gets there (a
        // coordinated process holds them until its own clock does).  Its local
        // notices are dropped: the process makes its own.
        void take(transport& t, microseconds until,
                  const std::function<void(connection, microseconds at, const std::string& bytes)>& message,
                  const std::function<void(connection, microseconds at, connection_state, int reason,
                                           const std::string& debug)>& notice);
        // The fastest any endpoint can reach another: the least of a sender's
        // send latency plus a receiver's receive latency over every pair (with
        // fewer than two endpoints, kNoLink).
        static constexpr microseconds kNoLink = std::numeric_limits<microseconds>::max() / 4;
        microseconds lowest_latency() const;

    private:
        class endpoint;
        struct packet {
            microseconds at = 0;
            uint64_t seq = 0;
            connection to = kInvalidConnection;
            std::string bytes;
        };
        struct notice {   // a state change its endpoint learns at `at`
            microseconds at = 0;
            uint64_t seq = 0;
            connection conn = kInvalidConnection;
            connection_state state = connection_state::none;
            int reason = kEndInvalid;
            std::string debug;
            bool local = false;
        };
        struct half {   // one side of a connection
            endpoint* owner = nullptr;
            connection peer = kInvalidConnection;
            connection_state state = connection_state::none;
            microseconds last_reliable = 0;   // the in-order floor of its reliable sends
            microseconds last_any = 0;        // and of all its sends
            std::string name;
        };

        half* find(connection c) {
            const auto it = halves_.find(c);
            return it == halves_.end() ? nullptr : &it->second;
        }
        microseconds latency(const half& from, const half* to) const;
        bool lost(endpoint& e, int d);   // this message, on e's end in direction d (0 send, 1 recv)
        // `c` changes to `to` at `at`: the state then, its owner's notice for then.
        void schedule(connection c, connection_state to, microseconds at, bool local, int reason = kEndInvalid,
                      std::string debug = {});
        void settle();
        bool deliver(connection from, const void* data, size_t size, int flags);
        void close(connection c, int reason, const std::string& debug, bool linger);

        microseconds now_ = 0;
        uint64_t seq_ = 0;
        connection next_ = 1;
        std::mt19937_64 random_;
        std::map<connection, half> halves_;
        std::map<uint16_t, endpoint*> listeners_;
        std::set<endpoint*> endpoints_;
        std::vector<packet> packets_;   // in flight, by (at, seq)
        std::vector<notice> flips_;     // the state changes still to come, by (at, seq)

        friend class endpoint;
    };

    class sim_network::endpoint final : public transport {
    public:
        explicit endpoint(sim_network& net) : net_(net) { net_.endpoints_.insert(this); }
        ~endpoint() override {
            net_.endpoints_.erase(this);
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
            const microseconds at = net_.now_ + net_.latency(*h, net_.find(h->peer));
            const connection peer = h->peer;
            net_.schedule(c, connection_state::connected, net_.now_, true);
            net_.schedule(peer, connection_state::connected, at, false);
            return accept_result::ok;
        }

        connection connect(const std::string& address) override {
            const auto colon = address.rfind(':');
            if (colon == std::string::npos) return kInvalidConnection;
            const auto port = static_cast<uint16_t>(std::atoi(address.c_str() + colon + 1));
            const auto server = net_.listeners_.find(port);
            if (server == net_.listeners_.end()) return kInvalidConnection;
            const connection mine = net_.next_++, theirs = net_.next_++;
            net_.halves_[mine] = half{this, theirs, connection_state::none, 0, 0, {}};
            net_.halves_[theirs] = half{server->second, mine, connection_state::none, 0, 0, {}};
            const microseconds at = net_.now_ + net_.latency(net_.halves_[mine], &net_.halves_[theirs]);
            net_.schedule(mine, connection_state::connecting, net_.now_, true);
            net_.schedule(theirs, connection_state::connecting, at, false);
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
            out.ping_ms = static_cast<int>((lag_us_[0] + extra_us_[0] + lag_us_[1] + extra_us_[1]
                                            + (peer ? peer->lag_us_[0] + peer->extra_us_[0] + peer->lag_us_[1] + peer->extra_us_[1] : 0)) / 1000);
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
                // (a peer's close does not stop what was sent before it)
                if (h && h->owner == this && it->at <= net_.now_) {
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
            net_.settle();
            while (!notices_.empty() && notices_.front().at <= net_.now_) {
                const notice n = std::move(notices_.front());
                notices_.pop_front();
                const half* h = net_.find(n.conn);
                if (!h) continue;   // closed here meanwhile
                status_change change;
                change.conn = n.conn;
                change.old_state = told_[n.conn];
                change.state = n.state;
                change.end_reason = n.reason;
                change.end_debug = n.debug;
                change.description = "#" + std::to_string(n.conn) + (h->name.empty() ? "" : " " + h->name);
                told_[n.conn] = n.state;
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
        std::deque<notice> notices_;                 // by (at, seq)
        std::map<connection, connection_state> told_;
        microseconds lag_us_[2] = {0, 0};   // send, recv
        float loss_[2] = {0, 0};             // percent
        int burst_[2] = {1, 1};              // a loss takes the next burst - 1 with it
        int burst_left_[2] = {0, 0};
        microseconds blackout_until_[2] = {0, 0};
        microseconds extra_us_[2] = {0, 0};     // add_lag's, on top of lag_us_
        microseconds spike_us_[2] = {0, 0};
        microseconds spike_until_[2] = {0, 0};
    };

    inline std::unique_ptr<transport> sim_network::make_transport() { return std::make_unique<endpoint>(*this); }

    inline void sim_network::set_loss_burst(transport& t, transport::direction d, int burst) {
        static_cast<endpoint&>(t).burst_[d == transport::direction::send ? 0 : 1] = std::max(1, burst);
    }
    inline void sim_network::add_lag(transport& t, transport::direction d, int ms) {
        auto& extra = static_cast<endpoint&>(t).extra_us_[d == transport::direction::send ? 0 : 1];
        extra = std::max<microseconds>(0, extra + static_cast<microseconds>(ms) * 1000);
    }
    inline void sim_network::spike(transport& t, transport::direction d, int ms, microseconds until) {
        auto& e = static_cast<endpoint&>(t);
        const int i = d == transport::direction::send ? 0 : 1;
        e.spike_us_[i] = std::max<microseconds>(0, static_cast<microseconds>(ms) * 1000);
        e.spike_until_[i] = until;
    }
    inline void sim_network::blackout(transport& t, transport::direction d, microseconds until) {
        static_cast<endpoint&>(t).blackout_until_[d == transport::direction::send ? 0 : 1] = until;
    }

    inline bool sim_network::lost(endpoint& e, int d) {
        if (e.loss_[d] <= 0.0f) return false;
        if (e.burst_left_[d] > 0) {
            --e.burst_left_[d];
            return true;
        }
        // A burst of b started with probability q drops q*b of every 1 + q*(b-1): q solved for the loss rate.
        const double loss = e.loss_[d] / 100.0, b = e.burst_[d];
        const double start = loss / (b - loss * (b - 1));
        if (std::uniform_real_distribution<double>(0.0, 1.0)(random_) >= start) return false;
        e.burst_left_[d] = e.burst_[d] - 1;
        return true;
    }

    inline microseconds sim_network::latency(const half& from, const half* to) const {
        const auto lag = [this](const endpoint* e, int i) -> microseconds {
            if (!e) return 0;
            return e->lag_us_[i] + e->extra_us_[i] + (now_ < e->spike_until_[i] ? e->spike_us_[i] : 0);
        };
        return lag(from.owner, 0) + lag(to ? to->owner : nullptr, 1);
    }

    inline microseconds sim_network::lowest_latency() const {
        microseconds lowest = kNoLink;
        for (const auto* a: endpoints_)
            for (const auto* b: endpoints_)
                if (a != b) lowest = std::min(lowest, a->lag_us_[0] + b->lag_us_[1]);
        return lowest;
    }

    inline void sim_network::schedule(connection c, connection_state to, microseconds at, bool local, int reason,
                                      std::string debug) {
        half* h = find(c);
        if (!h) return;
        const auto by_time = [](const notice& a, const notice& b) { return a.at != b.at ? a.at < b.at : a.seq < b.seq; };
        notice n{at, seq_++, c, to, reason, std::move(debug), local};
        if (h->owner) {
            auto& q = h->owner->notices_;
            q.insert(std::upper_bound(q.begin(), q.end(), n, by_time), n);
        }
        flips_.insert(std::upper_bound(flips_.begin(), flips_.end(), n, by_time), std::move(n));
        settle();
    }

    inline void sim_network::settle() {
        size_t done = 0;
        for (; done < flips_.size() && flips_[done].at <= now_; ++done)
            if (half* h = find(flips_[done].conn)) h->state = flips_[done].state;
        flips_.erase(flips_.begin(), flips_.begin() + static_cast<std::ptrdiff_t>(done));
    }

    inline bool sim_network::deliver(connection from, const void* data, size_t size, int flags) {
        half* h = find(from);
        if (!h || h->state != connection_state::connected) return false;
        half* peer = find(h->peer);
        if (!peer) return false;
        auto* sender = h->owner;
        auto* receiver = peer->owner;
        microseconds at = now_ + latency(*h, peer);
        microseconds dark = 0;   // a blackout on either end, this direction
        if (sender && sender->blackout_until_[0] > now_) dark = sender->blackout_until_[0];
        if (receiver && receiver->blackout_until_[1] > now_) dark = std::max(dark, receiver->blackout_until_[1]);
        if (flags & kSendReliable) {
            if (dark) at = std::max(at, dark + latency(*h, peer));
            at = std::max(at, h->last_reliable);
            h->last_reliable = at;
        } else {
            // Both ends' loss draws, every time (the seed's pattern does not
            // depend on what the other end dropped).
            const bool lost_up = sender && lost(*sender, 0);
            const bool lost_down = receiver && lost(*receiver, 1);
            if (dark || lost_up || lost_down) return true;   // sent, lost
        }
        at = std::max(at, h->last_any);
        h->last_any = at;
        packet p{at, seq_++, h->peer, std::string(static_cast<const char*>(data), size)};
        packets_.insert(std::upper_bound(packets_.begin(), packets_.end(), p,
                                         [](const packet& a, const packet& b) { return a.at != b.at ? a.at < b.at : a.seq < b.seq; }),
                        std::move(p));
        return true;
    }

    // The peer learns of a close after the link's latency; with `linger`, only
    // once the reliable data still in flight has arrived (receive() hands that
    // over first, poll_status the close after it).
    inline void sim_network::close(connection c, int reason, const std::string& debug, bool linger) {
        half* h = find(c);
        if (!h) return;
        const connection peer = h->peer;
        microseconds at = now_ + latency(*h, find(peer));
        if (linger) at = std::max(at, h->last_reliable);
        const bool was_open = h->state == connection_state::connected || h->state == connection_state::connecting;
        halves_.erase(c);
        if (was_open && find(peer)) schedule(peer, connection_state::closed_by_peer, at, false, reason, debug);
    }

    inline void sim_network::take(transport& t, microseconds until,
                                  const std::function<void(connection, microseconds, const std::string&)>& message,
                                  const std::function<void(connection, microseconds, connection_state, int,
                                                           const std::string&)>& notice_of) {
        auto& e = static_cast<endpoint&>(t);
        while (!e.notices_.empty() && e.notices_.front().at <= until) {
            const notice n = std::move(e.notices_.front());
            e.notices_.pop_front();
            if (!n.local) notice_of(n.conn, n.at, n.state, n.reason, n.debug);
        }
        for (auto it = packets_.begin(); it != packets_.end() && it->at <= until;) {
            const half* h = find(it->to);
            if (!h || h->owner == &e) {   // to a connection closed at its end: gone
                if (h) message(it->to, it->at, it->bytes);
                it = packets_.erase(it);
            } else {
                ++it;
            }
        }
    }

}
