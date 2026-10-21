#ifndef BALLANCEMMOSERVER_INPUT_RELAY_HPP
#define BALLANCEMMOSERVER_INPUT_RELAY_HPP
// The server -> client relay of applied input frames, unreliable with a
// redundancy window, and its reassembly on the client (protocol 5).
//
// The relay is what corrects a client's prediction of the other players'
// balls when no snapshot does: for every tick the server simulated, the frame
// it actually applied for every other member.  Sent as a reliable ordered
// stream (2026-10-19 .. 10-21) a lost datagram stalled everything behind it,
// and under 10 % loss the corrections arrived past the rollback's reach (48
// ticks) and were applied as pose writes - the world came apart by 120-150 m.
// So the relay is unreliable again, like the uplink (session_input_msg,
// MAX_INPUT_FRAMES of history in every datagram, input_buffer::submit on the
// server), and every datagram carries, per other member, the frames of the
// last `window` ticks: a loss costs a gap only if `window` datagrams in a row
// are lost, and nothing waits for anything.
//
// Everything here is CK-free and shared by the server (relay_ring), the
// headless client and the retail mod (relay_store, relay_seen, pending_relay,
// relay_frame_or_earlier) so the two clients cannot drift apart on the rules.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "../entity/session.hpp"

namespace bmmo::session {
    // Bit for bit: == would call -0.0 and +0.0 the same and the frame a
    // client rebuilds from an omitted camera must be the bytes the server
    // applied.
    inline bool same_camera(const input_frame& a, const input_frame& b) {
        return std::memcmp(a.cam_right, b.cam_right, sizeof(a.cam_right)) == 0
            && std::memcmp(a.cam_up, b.cam_up, sizeof(a.cam_up)) == 0
            && std::memcmp(a.cam_dir, b.cam_dir, sizeof(a.cam_dir)) == 0;
    }

    // The per-player ring of relayed frames a client keeps, by tick: longer
    // than the rollback's history (96) so every tick a rewind can reach has
    // its frame, and longer than one window so a re-delivery is a duplicate,
    // never stale.
    constexpr size_t kRelayRing = 128;
    static_assert(kRelayRing > MAX_RELAY_FRAMES, "a window must fit the ring");

    // Bytes of one datagram's fixed part (opcode, session, tick, count) and of
    // one entry on the wire (session_remote_input_msg): the builder packs to a
    // budget of one GameNetworkingSockets packet, because an unreliable
    // message split over several packets is lost whole when any of them is,
    // and one over k_cbMaxUnreliableMsgSizeSend (16.5 KB) is quietly sent
    // reliable - the stall this relay exists to remove.
    constexpr size_t kRelayDatagramHeader = 4 + 4 + 4 + 1;   // opcode u32, session, tick, count
    constexpr size_t kRelayDatagramBudget = 1100;

    // ------------------------------------------------------------ server
    // The last `window` ticks' applied frames, and the window a recipient gets:
    // for every other member present at the newest tick, its frames for the
    // trailing run of consecutive ticks it was present in, oldest first.  A
    // member absent at the newest tick (left) is not relayed.
    class relay_ring {
    public:
        struct window_entry {
            uint32_t player = 0;
            std::vector<input_frame> frames;   // ticks newest - frames.size() + 1 .. newest, oldest first
        };

        explicit relay_ring(size_t window = MAX_INPUT_FRAMES) { set_window(window); }

        void set_window(size_t window) {
            window_ = std::clamp<size_t>(window, 1, MAX_RELAY_FRAMES);
            trim();
        }
        size_t window() const { return window_; }
        bool empty() const { return ticks_.empty(); }
        uint32_t newest_tick() const { return ticks_.empty() ? 0 : ticks_.back().tick; }
        void clear() { ticks_.clear(); runs_.clear(); }

        // What the world applied at `tick`.  A tick at or behind the newest
        // already held replaces the ring from there (the server never
        // re-simulates, but a re-anchored numbering must not leave two frames
        // for one tick).  The runs every recipient's window is cut from are
        // computed here, once per tick.
        void push(uint32_t tick, const std::vector<std::pair<uint32_t, input_frame>>& applied) {
            while (!ticks_.empty() && static_cast<int32_t>(ticks_.back().tick - tick) >= 0) ticks_.pop_back();
            ticks_.push_back(tick_entry{tick, applied});
            trim();
            rebuild_runs();
        }

        // Every member's run, oldest first; what a recipient gets is these
        // minus its own.
        const std::vector<window_entry>& runs() const { return runs_; }

        std::vector<window_entry> build(uint32_t recipient) const {
            std::vector<window_entry> out;
            for (const auto& run: runs_)
                if (run.player != recipient) out.push_back(run);
            return out;
        }

        // The wire size of an entry (player u32, nframes u8, per frame 4 B and
        // the camera where it differs from the frame before).
        static size_t entry_bytes(const window_entry& e) {
            size_t bytes = 4 + 1;
            for (size_t i = 0; i < e.frames.size(); ++i)
                bytes += 4 + ((i == 0 || !same_camera(e.frames[i - 1], e.frames[i])) ? 36 : 0);
            return bytes;
        }

        // The recipient's window packed into datagrams of at most `budget`
        // bytes each, whole entries per datagram (every entry stands alone,
        // so a split costs one header).  An entry that does not fit a
        // datagram on its own loses its oldest frames until it does; `trimmed`
        // counts them.  `keep` says which players are still members: a leaver
        // the step still applied a frame for is not relayed after PlayerLeft.
        template <class Keep>
        std::vector<std::vector<window_entry>> build_datagrams(uint32_t recipient, size_t budget, const Keep& keep,
                                                               size_t* trimmed = nullptr) const {
            std::vector<std::vector<window_entry>> out;
            size_t used = budget;   // forces a first datagram
            for (const auto& run: runs_) {
                if (run.player == recipient || !keep(run.player)) continue;
                window_entry entry = run;
                size_t bytes = entry_bytes(entry);
                while (bytes + kRelayDatagramHeader > budget && entry.frames.size() > 1) {
                    entry.frames.erase(entry.frames.begin());
                    if (trimmed) ++*trimmed;
                    bytes = entry_bytes(entry);
                }
                if (used + bytes > budget) {
                    out.emplace_back();
                    used = kRelayDatagramHeader;
                }
                used += bytes;
                out.back().push_back(std::move(entry));
            }
            return out;
        }

    private:
        struct tick_entry {
            uint32_t tick = 0;
            std::vector<std::pair<uint32_t, input_frame>> applied;
        };
        void trim() { while (ticks_.size() > window_) ticks_.pop_front(); }
        void rebuild_runs() {
            runs_.clear();
            if (ticks_.empty()) return;
            const tick_entry& newest = ticks_.back();
            for (const auto& [player, frame]: newest.applied) {
                window_entry entry;
                entry.player = player;
                // Walk back while the ticks stay consecutive and the member is present.
                std::vector<const input_frame*> run{&frame};
                uint32_t expect = newest.tick;
                for (auto it = ticks_.rbegin() + 1; it != ticks_.rend(); ++it) {
                    if (it->tick + 1 != expect) break;
                    const input_frame* found = nullptr;
                    for (const auto& [p, f]: it->applied)
                        if (p == player) { found = &f; break; }
                    if (!found) break;
                    run.push_back(found);
                    expect = it->tick;
                }
                entry.frames.reserve(run.size());
                for (auto it = run.rbegin(); it != run.rend(); ++it) entry.frames.push_back(**it);
                runs_.push_back(std::move(entry));
            }
        }
        std::deque<tick_entry> ticks_;
        std::vector<window_entry> runs_;
        size_t window_ = MAX_INPUT_FRAMES;
    };

    // ------------------------------------------------------------ client
    // The tick a frame of a window entry is for: the entry's frames end at the
    // message's tick.
    inline uint32_t relay_frame_tick(uint32_t newest_tick, size_t frames, size_t index) {
        return newest_tick - static_cast<uint32_t>(frames) + 1 + static_cast<uint32_t>(index);
    }

    // A relayed frame into a mirror's per-tick ring (remote.inputs).  A tick
    // already held is a duplicate from a later window: the first delivery is
    // the truth and nothing changes.  A tick older than what the ring would
    // keep is stale (it would be evicted at once, and the journal has it).
    enum class relay_store_result { stored, duplicate, stale };

    inline relay_store_result relay_store(std::map<uint32_t, input_frame>& ring, uint32_t tick,
                                          const input_frame& frame, size_t cap = kRelayRing) {
        if (ring.count(tick)) return relay_store_result::duplicate;
        if (!ring.empty() && static_cast<int32_t>(ring.rbegin()->first - tick) >= static_cast<int32_t>(cap))
            return relay_store_result::stale;
        ring.emplace(tick, frame);
        while (ring.size() > cap) ring.erase(ring.begin());
        return relay_store_result::stored;
    }

    // The frame a replay drives a mirror with at `tick`: the relayed one when
    // it arrived, else - for a hole the window did not cover, a tick with a
    // relayed frame on both sides - the nearest earlier relayed frame, which
    // is what the owner was most likely still holding.  Null when nothing
    // later has arrived either: the tick is not a hole but the tail the relay
    // has not reached, and the caller's recorded prediction stands.
    inline const input_frame* relay_frame_or_earlier(const std::map<uint32_t, input_frame>& ring, uint32_t tick) {
        auto it = ring.find(tick);
        if (it != ring.end()) return &it->second;
        auto later = ring.upper_bound(tick);
        if (later == ring.end() || later == ring.begin()) return nullptr;
        return &std::prev(later)->second;
    }

    // Which (player, tick) frames this client has already taken, for the
    // once-only rules (the journal's relayed-input record, the correction
    // note) and the duplicate count.  Bounded per player; a frame arriving
    // later than the bound would be taken again, which at 66 Hz means seconds
    // late through a window of a few ticks - never.  Cleared only with the
    // session: a leaver's straggling datagram must not re-announce it.
    class relay_seen {
    public:
        explicit relay_seen(size_t cap = 256) : cap_(cap) {}
        // True the first time (player, tick) is offered.
        bool first(uint32_t player, uint32_t tick) {
            auto& ticks = seen_[player];
            if (!ticks.insert(tick).second) return false;
            while (ticks.size() > cap_) ticks.erase(ticks.begin());
            return true;
        }
        void clear() { seen_.clear(); }
    private:
        std::map<uint32_t, std::set<uint32_t>> seen_;
        size_t cap_;
    };

    // Frames relayed for a player this client has no mirror for yet (its
    // Physicalize has not been processed here): kept by tick, bounded, and
    // moved into the mirror's ring when it is made - for the live predictor
    // (the newest one) and for the rewinds that read the ring later; the
    // replay of the birth itself drives only the mirrors the live record
    // tracked, so a frame older than the mirror's first live record is never
    // fed to a replay.  The ordered stream used to deliver the Physicalize
    // first, which hid this hole.
    class pending_relay {
    public:
        explicit pending_relay(size_t cap = kRelayRing) : cap_(cap) {}
        void store(uint32_t player, uint32_t tick, const input_frame& frame) {
            relay_store(frames_[player], tick, frame, cap_);
        }
        // Moves every pending frame of `player` into `ring` (the ring's own
        // frames win); returns the newest pending frame's tick, or 0 if none.
        uint32_t take(uint32_t player, std::map<uint32_t, input_frame>& ring, size_t ring_cap, input_frame* newest = nullptr) {
            auto it = frames_.find(player);
            if (it == frames_.end()) return 0;
            uint32_t newest_tick = 0;
            for (const auto& [tick, frame]: it->second) {
                if (relay_store(ring, tick, frame, ring_cap) == relay_store_result::stored
                        || ring.count(tick)) {
                    if (newest_tick == 0 || static_cast<int32_t>(tick - newest_tick) > 0) {
                        newest_tick = tick;
                        if (newest) *newest = ring.at(tick);
                    }
                }
            }
            frames_.erase(it);
            return newest_tick;
        }
        size_t players() const { return frames_.size(); }
        void clear() { frames_.clear(); }
    private:
        std::map<uint32_t, std::map<uint32_t, input_frame>> frames_;
        size_t cap_;
    };
}

#endif //BALLANCEMMOSERVER_INPUT_RELAY_HPP
