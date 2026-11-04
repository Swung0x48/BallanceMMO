#ifndef BALLANCEMMOSERVER_INPUT_RELAY_HPP
#define BALLANCEMMOSERVER_INPUT_RELAY_HPP
// The server -> client relay of applied input frames and its reassembly on
// the client (protocol 29, docs/relay-protocol-design.md).
//
// The relay is what corrects a client's prediction of the other players'
// balls when no snapshot does: for every tick the server simulated, the frame
// it actually applied for every other member.  Sent as a reliable ordered
// stream (2026-10-19 .. 10-21) a lost datagram stalled everything behind it;
// sent unreliable with a fixed redundancy window (protocol 5) a loss longer
// than the window left a tick whose frame never arrived, and the client kept
// its guess for it for good (the LBM matrix's loss divergences).  So every
// datagram now carries each other member's frames for every tick after the
// newest tick the recipient acknowledged holding them all for
// (session_input_msg::relay_ack): any loss shorter than the server's history
// (kRelayHistory) is filled by the next datagram that arrives, and nothing
// waits for anything.  Repeats cost little: a run of equal frames is one
// run, and a camera is a 3-bit heading code against its player's base
// (camera_heading.hpp, protocol 30).
//
// Everything here is CK-free and shared by the server (relay_ring), the
// headless client and the retail mod (relay_store, relay_seen, pending_relay,
// relay_frame_or_earlier) so the two clients cannot drift apart on the rules.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "../entity/session.hpp"
#include "camera_heading.hpp"

namespace bmmo::session {
    // How many ticks of every member's applied frames the server keeps: a
    // recipient whose acknowledgement is older is beyond help and resyncs.
    // It covers the deepest rollback reach (design 2.5 clamps it at 120);
    // an older frame no rewind could use anyway.
    constexpr size_t kRelayHistory = MAX_RELAY_FRAMES;
    // The per-player ring of relayed frames a client keeps, by tick: longer
    // than the server's history, so a frame a datagram re-delivers is a
    // duplicate, never stale.
    constexpr size_t kRelayRing = 2 * kRelayHistory;

    // Bytes of one datagram's fixed part (opcode, session, tick, count, and
    // since protocol 7 the recipient's acked, held_total and epoch, since
    // protocol 13 its wait, since protocol 29 from, part and parts) and of
    // one entry on the wire (session_remote_input_msg):
    // the builder packs to a budget of one GameNetworkingSockets packet,
    // because an unreliable message split over several packets is lost whole
    // when any of them is, and one over k_cbMaxUnreliableMsgSizeSend (16.5 KB)
    // is quietly sent reliable - the stall this relay exists to remove.
    constexpr size_t kRelayDatagramHeader = 4 + 4 + 4 + 1 + 4 + 4 + 4 + 1 + 4 + 1 + 1;
    constexpr size_t kRelayDatagramBudget = 1100;
    // A recipient whose acknowledgement is older than the history has to
    // resync whatever it holds: its own frames were substituted for longer
    // than any rewind reaches, and the others' it lacks are gone.  It gets
    // the header alone (from = the tick before the oldest kept, its cue),
    // 1, 2, 4 .. this many ticks apart, so a client that crashed or froze
    // costs a small datagram a second instead of the whole history every
    // tick; the first one a live client receives starts its resync.
    constexpr uint32_t kPastHistoryNoticeMaxGap = 64;

    // ------------------------------------------------------------ held notices
    // Protocol 7 (late-player design 1.1): every datagram also tells its
    // recipient which of ITS OWN ticks the server substituted, because the
    // late player's client is the one side that otherwise never learns that
    // the world drove its ball with another frame.  The section trails the
    // entries: nheld u8, then one record per substituted tick within the last
    // kHeldNoticeTicks ticks, the late player's own downlink being the link
    // most likely to be bad.
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
    // The session's applied frames for the last kRelayHistory ticks with
    // their heading codes, every player's camera bases, and what every
    // recipient acknowledged.  What a
    // recipient gets: for every other member present at the newest tick, its
    // frames for every tick after the recipient's acknowledgement (its
    // trailing run of consecutive present ticks, so a member that joined
    // since starts later), and its own substituted ticks within the notice
    // depth (kHeldNoticeTicks).  A member absent at the newest tick (left)
    // is not relayed.
    class relay_ring {
    public:
        struct member_entry {
            uint32_t player = 0;
            std::vector<input_frame> frames;   // ticks newest - frames.size() + 1 .. newest, oldest first
            std::vector<uint8_t> headings;     // one per frame
            std::optional<camera_bases::base> base;   // attached: the base in force at the first frame
        };
        // One substituted tick of a player: what the world applied there and
        // the tick that frame came from.
        struct held_entry {
            uint32_t tick = 0;
            uint32_t source = kHeldSourceDefault;
            input_frame frame{};
        };
        // One datagram's worth: whole entries and the held records that ride
        // in it (all of them in one datagram unless they cannot fit one).
        // `from` is the same in every datagram of a tick.
        struct datagram {
            uint32_t from = 0;
            std::vector<member_entry> entries;
            std::vector<held_entry> held;
        };

        bool empty() const { return ticks_.empty(); }
        uint32_t newest_tick() const { return ticks_.empty() ? 0 : ticks_.back().tick; }
        void clear() { ticks_.clear(); held_.clear(); recipients_.clear(); bases_.clear(); }

        // What the world applied at `tick`.  A tick at or behind the newest
        // already held replaces the ring from there (the server never
        // re-simulates, but a re-anchored numbering must not leave two frames
        // for one tick).  Every player's held ticks and every frame's heading
        // code are computed here, once per tick.
        void push(uint32_t tick, const std::vector<applied_input>& applied) {
            if (!ticks_.empty() && static_cast<int32_t>(ticks_.back().tick - tick) >= 0)
                for (auto& [player, bases]: bases_) bases.forget_from(tick);
            while (!ticks_.empty() && static_cast<int32_t>(ticks_.back().tick - tick) >= 0) ticks_.pop_back();
            tick_entry entry{tick, applied, {}};
            for (const auto& a: applied) entry.headings.push_back(bases_[a.player].encode(tick, camera_of(a.frame)));
            ticks_.push_back(std::move(entry));
            while (ticks_.size() > kRelayHistory) ticks_.pop_front();
            for (auto& [player, bases]: bases_) bases.forget_before(ticks_.front().tick);
            rebuild_held();
        }
        // Every frame fresh (the tests' shape).
        void push(uint32_t tick, const std::vector<std::pair<uint32_t, input_frame>>& applied) {
            std::vector<applied_input> all;
            all.reserve(applied.size());
            for (const auto& [player, frame]: applied) all.push_back(applied_input{player, frame, false, kHeldSourceDefault});
            push(tick, all);
        }

        // The recipient's numbering starts at tick + 1 (an assignment: the
        // start, a resync, a late join): everything up to `tick` is the state
        // it was given, and it holds none of the camera bases.  Its
        // acknowledgements never move backwards and never past what was
        // relayed.
        void reanchor(uint32_t recipient, uint32_t tick) {
            auto& r = recipients_[recipient];
            r.acked = tick;
            r.carried.clear();
            r.notice_gap = 0;
        }
        void acknowledge(uint32_t recipient, uint32_t tick) {
            const auto it = recipients_.find(recipient);
            if (it == recipients_.end() || static_cast<int32_t>(tick - it->second.acked) <= 0) return;
            if (!ticks_.empty() && static_cast<int32_t>(tick - newest_tick()) > 0) tick = newest_tick();
            it->second.acked = tick;
        }
        // A leaver: as recipient, and its camera bases (a member that comes
        // back starts with an inline camera).
        void forget(uint32_t recipient) {
            recipients_.erase(recipient);
            bases_.erase(recipient);
            for (auto& [id, r]: recipients_) r.carried.erase(recipient);
        }
        uint32_t acked(uint32_t recipient) const {
            const auto it = recipients_.find(recipient);
            return it == recipients_.end() ? 0 : it->second.acked;
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

        // The wire size of an entry: player u32, nframes u8, nruns u8, an
        // attached base, per run 4 B and an inline camera's rows.
        static size_t entry_bytes(const member_entry& e) {
            size_t bytes = 4 + 1 + 1 + (e.base ? kCameraBaseBytes : 0);
            for (size_t i = 0; i < e.frames.size(); ++i)
                if (i == 0 || !same_run(e, i - 1, i))
                    bytes += 4 + (e.headings[i] == kHeadingInline ? sizeof(camera_rows) : 0);
            return bytes;
        }
        // The wire size of one held record for a recipient numbered from `epoch`.
        static size_t held_bytes(const held_entry& h, uint32_t epoch) {
            return held_inline(h.tick, h.source, epoch) ? kHeldInlineBytes : kHeldReferenceBytes;
        }

        // The recipient's relay packed into datagrams of at most `budget`
        // bytes each, whole entries per datagram (every entry stands alone,
        // so a split costs one header).  `keep` says which players are still
        // members: a leaver the step still applied a frame for is not relayed
        // after PlayerLeft.
        //
        // `from` is the recipient's acknowledgement.  When that is older than
        // the oldest tick kept, the recipient gets header-only notices with
        // `from` the tick before the oldest, at growing gaps (no datagram at
        // all between them: kPastHistoryNoticeMaxGap), until an
        // acknowledgement or a re-anchor brings it back.  An entry whose first camera is a
        // turn of a base the recipient may lack attaches the base, until the
        // recipient acknowledges the first datagram that carried it, inline
        // or attached (by then it held that datagram).
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
                                              std::optional<uint32_t> held_from = std::nullopt) {
            auto& r = recipients_[recipient];
            const uint32_t from = r.acked;
            if (!ticks_.empty() && static_cast<int32_t>(from + 1 - ticks_.front().tick) < 0) {
                if (r.notice_gap != 0 && static_cast<int32_t>(newest_tick() - r.next_notice) < 0) return {};
                r.notice_gap = r.notice_gap == 0 ? 1 : std::min(2 * r.notice_gap, kPastHistoryNoticeMaxGap);
                r.next_notice = newest_tick() + r.notice_gap;
                datagram notice;
                notice.from = ticks_.front().tick - 1;
                return {notice};
            }
            r.notice_gap = 0;
            const size_t fixed = kRelayDatagramHeader + kHeldCountBytes;
            std::vector<datagram> out;
            std::vector<size_t> used;
            const auto open = [&] {
                out.emplace_back();
                out.back().from = from;
                used.push_back(fixed);
            };
            for (member_entry& entry: entries_after(from, recipient, keep)) {
                carry_bases(r, entry);
                const size_t bytes = entry_bytes(entry);
                if (out.empty() || used.back() + bytes > budget) open();
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
                    if (k == 0 || used.back() + bytes > budget) open();
                    used.back() += bytes;
                    out.back().held.push_back(records[k]);
                }
            }
            if (out.empty()) open();
            return out;
        }

    private:
        struct tick_entry {
            uint32_t tick = 0;
            std::vector<applied_input> applied;
            std::vector<uint8_t> headings;   // one per applied frame
        };
        struct recipient_state {
            uint32_t acked = 0;
            uint32_t notice_gap = 0, next_notice = 0;   // past the history: the header-only notices' backoff
            // Per player, per base (its since), the newest tick of the first
            // datagram that carried it to this recipient, inline or attached.
            std::map<uint32_t, std::map<uint32_t, uint32_t>> carried;
        };

        // One run on the wire: the same keys, ball type, flags and heading.
        static bool same_run(const member_entry& e, size_t a, size_t b) {
            const auto& x = e.frames[a];
            const auto& y = e.frames[b];
            return x.keys == y.keys && x.ball_type == y.ball_type && x.flags == y.flags
                && e.headings[a] == e.headings[b] && (e.headings[a] != kHeadingInline || same_camera(x, y));
        }

        // Every member's frames for the ticks after `from`: walk back from the
        // newest tick while the ticks stay consecutive and the member present.
        template <class Keep>
        std::vector<member_entry> entries_after(uint32_t from, uint32_t recipient, const Keep& keep) {
            std::vector<member_entry> out;
            if (ticks_.empty()) return out;
            const tick_entry& newest = ticks_.back();
            for (const auto& a: newest.applied) {
                if (a.player == recipient || !keep(a.player)) continue;
                std::vector<std::pair<const input_frame*, uint8_t>> run;
                uint32_t expect = newest.tick;
                for (auto it = ticks_.rbegin(); it != ticks_.rend() && static_cast<int32_t>(it->tick - from) > 0; ++it) {
                    if (it->tick != expect) break;
                    const auto found = std::find_if(it->applied.begin(), it->applied.end(),
                                                    [&a](const applied_input& b) { return b.player == a.player; });
                    if (found == it->applied.end()) break;
                    run.emplace_back(&found->frame, it->headings[static_cast<size_t>(found - it->applied.begin())]);
                    --expect;
                }
                if (run.empty()) continue;
                member_entry entry;
                entry.player = a.player;
                for (auto it = run.rbegin(); it != run.rend(); ++it) {
                    entry.frames.push_back(*it->first);
                    entry.headings.push_back(it->second);
                }
                out.push_back(std::move(entry));
            }
            return out;
        }
        // The base the entry's first coded frame turns - with no frame coded,
        // the one in force at its end, which the next turn needs - is
        // attached while the recipient may lack it.  Every base the entry
        // carries (that one, the inline frames') is noted with this
        // datagram's tick, the first time: once the recipient acknowledges
        // that tick it held a datagram built before it did, which carried
        // the base or no longer needed it.
        void carry_bases(recipient_state& r, member_entry& entry) {
            const uint32_t first = newest_tick() - static_cast<uint32_t>(entry.frames.size()) + 1;
            auto& carried = r.carried[entry.player];
            const auto coded = std::find_if(entry.headings.begin(), entry.headings.end(),
                                            [](uint8_t code) { return code != kHeadingNone; });
            const auto bases = bases_.find(entry.player);
            if (bases != bases_.end() && (coded == entry.headings.end() || *coded < 4)) {
                const uint32_t tick = coded == entry.headings.end()
                                    ? newest_tick() : first + static_cast<uint32_t>(coded - entry.headings.begin());
                if (const camera_bases::base* b = bases->second.at(tick)) {
                    const auto it = carried.find(b->since);
                    if (it == carried.end() || static_cast<int32_t>(r.acked - it->second) < 0) entry.base = *b;
                    carried.emplace(b->since, newest_tick());
                    carried.erase(carried.begin(), carried.find(b->since));   // no later datagram needs those
                }
            }
            for (size_t i = 0; i < entry.headings.size(); ++i)
                if (entry.headings[i] == kHeadingInline) carried.emplace(first + static_cast<uint32_t>(i), newest_tick());
        }
        // Held ticks need no run: a record names its own tick.
        void rebuild_held() {
            held_.clear();
            const uint32_t newest = ticks_.back().tick;
            for (const auto& t: ticks_) {
                if (newest - t.tick >= kHeldNoticeTicks) continue;
                for (const auto& a: t.applied)
                    if (a.held) held_[a.player].push_back(held_entry{t.tick, a.source, a.frame});
            }
        }
        std::deque<tick_entry> ticks_;
        std::map<uint32_t, std::vector<held_entry>> held_;
        std::map<uint32_t, recipient_state> recipients_;
        std::map<uint32_t, camera_bases> bases_;   // per player, the codes' bases
    };

    // ------------------------------------------------------------ client
    // The tick a frame of a relayed entry is for: the entry's frames end at the
    // message's tick.
    inline uint32_t relay_frame_tick(uint32_t newest_tick, size_t frames, size_t index) {
        return newest_tick - static_cast<uint32_t>(frames) + 1 + static_cast<uint32_t>(index);
    }

    // A relayed frame into a mirror's per-tick ring (remote.inputs).  A tick
    // already held is a duplicate a later datagram re-delivered: the first delivery is
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
    // an empty ring.  relay_frame_or_earlier stays its building block (the
    // legacy retail path, its other caller, is gone since FULL step LGD).
    inline const input_frame* relay_frame_or_newest(const std::map<uint32_t, input_frame>& ring, uint32_t tick) {
        if (const input_frame* frame = relay_frame_or_earlier(ring, tick)) return frame;
        if (ring.empty() || tick < ring.rbegin()->first) return nullptr;
        return &ring.rbegin()->second;
    }

    // Which (player, tick) frames this client has already taken, for the
    // once-only rules (the journal's relayed-input record, the correction
    // note) and the duplicate count.  Bounded per player; a frame arriving
    // later than the bound would be taken again, which at 66 Hz means seconds
    // late through the server's history - never.  Cleared only with the
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
