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
#include <optional>
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

    // Bytes of one datagram's fixed part (opcode, session, tick, count, and
    // since protocol 7 the recipient's acked, held_total and epoch, since
    // protocol 13 its wait) and of
    // one entry on the wire (session_remote_input_msg): the builder packs to a
    // budget of one GameNetworkingSockets packet, because an unreliable
    // message split over several packets is lost whole when any of them is,
    // and one over k_cbMaxUnreliableMsgSizeSend (16.5 KB) is quietly sent
    // reliable - the stall this relay exists to remove.
    constexpr size_t kRelayDatagramHeader = 4 + 4 + 4 + 1 + 4 + 4 + 4 + 1;   // opcode u32, session, tick, count, acked, held_total, epoch, wait
    constexpr size_t kRelayDatagramBudget = 1100;

    // ------------------------------------------------------------ held notices
    // Protocol 7 (late-player design 1.1): every datagram also tells its
    // recipient which of ITS OWN ticks the server substituted, because the
    // late player's client is the one side that otherwise never learns that
    // the world drove its ball with another frame.  The section trails the
    // entries: nheld u8, then one record per substituted tick within the last
    // kHeldNoticeTicks ticks.  It is longer than the relay window because the
    // late player's own downlink is the link most likely to be bad.
    constexpr size_t kHeldNoticeTicks = 32;
    static_assert(kHeldNoticeTicks <= 32, "a record's back offset is 5 bits");
    // The source of a substitute when nothing had arrived yet: the default frame.
    constexpr uint32_t kHeldSourceDefault = INPUT_SOURCE_DEFAULT;
    // A record names the tick whose frame, as the client sent it, the server
    // applied - 5 B and exact.  A source this far back or further is carried
    // inline instead (design 1.1).
    constexpr uint32_t kHeldInlineDistance = 96;
    // The ring of the frames a client sent, by tick, that resolves a
    // reference (plan step 4: the headless sent_inputs_; the retail part one).
    // A record of T is acted on while its anchor is inside the reach,
    // current - (T - 1) <= reach, and a reference names a source with
    // T - source <= kHeldInlineDistance - 1, so current - source can be
    // reach + kHeldInlineDistance - 2: the ring must reach that far back for
    // the deepest reach (design 2.5 clamps it at 120).  128 ticks, the
    // design's first figure, covered a reach of 33 only.
    constexpr uint32_t kHeldMaxReachTicks = 120;
    constexpr size_t kSentInputRing = 256;
    static_assert(kSentInputRing >= kHeldMaxReachTicks + kHeldInlineDistance - 1,
                  "every reference inside the deepest reach must still be in the sent ring");
    constexpr size_t kHeldCountBytes = 1;           // nheld, in every datagram
    constexpr size_t kHeldReferenceBytes = 1 + 4;   // back u8, source u32
    constexpr size_t kHeldInlineBytes = 1 + 40;     // back u8, the frame with its camera

    // One player's input as the world applied it at a tick: `held` when the
    // server had no fresh frame for the tick and substituted one, `source`
    // the tick of the substitute (kHeldSourceDefault for the default frame).
    struct applied_input {
        uint32_t player = 0;
        input_frame frame{};
        bool held = false;
        uint32_t source = kHeldSourceDefault;
        // Step R12 (protocol 13): the player's wait as its relay header
        // carries it (lateness.hpp server_wait_meter::value).
        uint8_t wait = 0;
    };

    // Whether a held record goes inline rather than by reference: the client
    // cannot resolve a source its sent ring (kSentInputRing) no longer
    // holds, nor one from before its current numbering (`epoch`, the base of
    // its last assignment: the ring is cleared with every re-anchor).
    inline bool held_inline(uint32_t tick, uint32_t source, uint32_t epoch) {
        if (source == kHeldSourceDefault) return false;
        return tick - source >= kHeldInlineDistance || static_cast<int32_t>(source - epoch) < 0;
    }

    // ------------------------------------------------------------ server
    // The last ticks' applied frames, and what a recipient gets: for every
    // other member present at the newest tick, its frames for the trailing
    // run of consecutive ticks it was present in, at most `window` of them,
    // oldest first; and the recipient's own substituted ticks within the
    // notice depth (kHeldNoticeTicks).  A member absent at the newest tick
    // (left) is not relayed.
    class relay_ring {
    public:
        struct window_entry {
            uint32_t player = 0;
            std::vector<input_frame> frames;   // ticks newest - frames.size() + 1 .. newest, oldest first
        };
        // One substituted tick of a player: what the world applied there and
        // the tick that frame came from.
        struct held_entry {
            uint32_t tick = 0;
            uint32_t source = kHeldSourceDefault;
            input_frame frame{};
        };
        // One datagram's worth: whole entries, and the held records that ride
        // in it (all of them in one datagram unless they cannot fit one).
        struct datagram {
            std::vector<window_entry> entries;
            std::vector<held_entry> held;
        };

        explicit relay_ring(size_t window = MAX_INPUT_FRAMES) { set_window(window); }

        void set_window(size_t window) {
            window_ = std::clamp<size_t>(window, 1, MAX_RELAY_FRAMES);
            trim();
        }
        size_t window() const { return window_; }
        // How many ticks the ring keeps: the others' window, or the notice
        // depth when that is longer.
        size_t depth() const { return std::max(window_, kHeldNoticeTicks); }
        bool empty() const { return ticks_.empty(); }
        uint32_t newest_tick() const { return ticks_.empty() ? 0 : ticks_.back().tick; }
        void clear() { ticks_.clear(); runs_.clear(); held_.clear(); }

        // What the world applied at `tick`.  A tick at or behind the newest
        // already held replaces the ring from there (the server never
        // re-simulates, but a re-anchored numbering must not leave two frames
        // for one tick).  The runs every recipient's window is cut from, and
        // every player's held ticks, are computed here, once per tick.
        void push(uint32_t tick, const std::vector<applied_input>& applied) {
            while (!ticks_.empty() && static_cast<int32_t>(ticks_.back().tick - tick) >= 0) ticks_.pop_back();
            ticks_.push_back(tick_entry{tick, applied});
            trim();
            rebuild_runs();
        }
        // Every frame fresh (the tests' and protocol 5's shape).
        void push(uint32_t tick, const std::vector<std::pair<uint32_t, input_frame>>& applied) {
            std::vector<applied_input> all;
            all.reserve(applied.size());
            for (const auto& [player, frame]: applied) all.push_back(applied_input{player, frame, false, kHeldSourceDefault});
            push(tick, all);
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

        // `player`'s substituted ticks within the notice depth of the newest
        // tick, oldest first, from `from` on (the recipient's numbering).
        std::vector<held_entry> held(uint32_t player, uint32_t from = 0) const {
            std::vector<held_entry> out;
            const auto it = held_.find(player);
            if (it == held_.end()) return out;
            for (const auto& h: it->second)
                if (static_cast<int32_t>(h.tick - from) >= 0) out.push_back(h);
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
        // The wire size of one held record for a recipient numbered from `epoch`.
        static size_t held_bytes(const held_entry& h, uint32_t epoch) {
            return held_inline(h.tick, h.source, epoch) ? kHeldInlineBytes : kHeldReferenceBytes;
        }

        // The recipient's window packed into datagrams of at most `budget`
        // bytes each, whole entries per datagram (every entry stands alone,
        // so a split costs one header).  An entry that does not fit a
        // datagram on its own loses its oldest frames until it does; `trimmed`
        // counts them.  `keep` says which players are still members: a leaver
        // the step still applied a frame for is not relayed after PlayerLeft.
        //
        // `held_from` is the recipient's epoch, the base of its last
        // assignment: its held ticks from there on go into the first datagram
        // with room for all of them, else into datagrams of their own (a
        // record stands alone as well, so the section can be split).  Without
        // it (not assigned yet) nothing is noticed.  There is always at least
        // one datagram, a header-only one when there is nothing else: the
        // header carries the recipient's ack and held_total, which a solo
        // player and one with nothing substituted need too.
        template <class Keep>
        std::vector<datagram> build_datagrams(uint32_t recipient, size_t budget, const Keep& keep,
                                              std::optional<uint32_t> held_from = std::nullopt,
                                              size_t* trimmed = nullptr) const {
            const size_t fixed = kRelayDatagramHeader + kHeldCountBytes;
            std::vector<datagram> out;
            std::vector<size_t> used;
            for (const auto& run: runs_) {
                if (run.player == recipient || !keep(run.player)) continue;
                window_entry entry = run;
                size_t bytes = entry_bytes(entry);
                while (bytes + fixed > budget && entry.frames.size() > 1) {
                    entry.frames.erase(entry.frames.begin());
                    if (trimmed) ++*trimmed;
                    bytes = entry_bytes(entry);
                }
                if (out.empty() || used.back() + bytes > budget) {
                    out.emplace_back();
                    used.push_back(fixed);
                }
                used.back() += bytes;
                out.back().entries.push_back(std::move(entry));
            }
            if (held_from) {
                const std::vector<held_entry> records = held(recipient, *held_from);
                size_t section = 0;
                for (const auto& h: records) section += held_bytes(h, *held_from);
                bool placed = records.empty();
                for (size_t i = 0; i < out.size() && !placed; ++i) {
                    if (used[i] + section > budget) continue;
                    out[i].held = records;
                    used[i] += section;
                    placed = true;
                }
                // Inline frames only (a source 96 or more ticks back): 32 of
                // them are 1312 B, past one packet, so they are split.
                for (size_t k = 0; !placed && k < records.size(); ++k) {
                    const size_t bytes = held_bytes(records[k], *held_from);
                    if (k == 0 || used.back() + bytes > budget) {
                        out.emplace_back();
                        used.push_back(fixed);
                    }
                    used.back() += bytes;
                    out.back().held.push_back(records[k]);
                }
            }
            if (out.empty()) out.emplace_back();
            return out;
        }

    private:
        struct tick_entry {
            uint32_t tick = 0;
            std::vector<applied_input> applied;
        };
        void trim() { while (ticks_.size() > depth()) ticks_.pop_front(); }
        void rebuild_runs() {
            runs_.clear();
            held_.clear();
            if (ticks_.empty()) return;
            const tick_entry& newest = ticks_.back();
            for (const auto& a: newest.applied) {
                window_entry entry;
                entry.player = a.player;
                // Walk back while the ticks stay consecutive and the member is
                // present, at most `window` ticks: the ring is deeper than the
                // window when the notices need it.
                std::vector<const input_frame*> run{&a.frame};
                uint32_t expect = newest.tick;
                for (auto it = ticks_.rbegin() + 1; it != ticks_.rend() && run.size() < window_; ++it) {
                    if (it->tick + 1 != expect) break;
                    const input_frame* found = nullptr;
                    for (const auto& b: it->applied)
                        if (b.player == a.player) { found = &b.frame; break; }
                    if (!found) break;
                    run.push_back(found);
                    expect = it->tick;
                }
                entry.frames.reserve(run.size());
                for (auto it = run.rbegin(); it != run.rend(); ++it) entry.frames.push_back(**it);
                runs_.push_back(std::move(entry));
            }
            // Held ticks need no run: a record names its own tick.
            for (const auto& t: ticks_) {
                if (newest.tick - t.tick >= kHeldNoticeTicks) continue;
                for (const auto& a: t.applied)
                    if (a.held) held_[a.player].push_back(held_entry{t.tick, a.source, a.frame});
            }
        }
        std::deque<tick_entry> ticks_;
        std::vector<window_entry> runs_;
        std::map<uint32_t, std::vector<held_entry>> held_;
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

    // Step RC1 (docs/rewind-cost-design.md §2): the frame the shared core's
    // replays drive a mirror with at `tick` (recorded_input_at, every rewind
    // kind).  relay_frame_or_earlier's frame (the relayed one, or a hole's
    // nearest earlier); for the tail past the newest relayed frame, that
    // newest frame - the frame the live predictor drives the tail with
    // (session_client_core::live_drive: remote.input), so the replay
    // re-predicts the tail with what it knows now and its re-captured record
    // holds it: a later arrival equal to it is no news.  Null before the
    // first relayed frame (the birth state or no input, as before) and for
    // an empty ring.  The legacy retail path keeps relay_frame_or_earlier.
    inline const input_frame* relay_frame_or_newest(const std::map<uint32_t, input_frame>& ring, uint32_t tick) {
        if (const input_frame* frame = relay_frame_or_earlier(ring, tick)) return frame;
        if (ring.empty() || tick < ring.rbegin()->first) return nullptr;
        return &ring.rbegin()->second;
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
