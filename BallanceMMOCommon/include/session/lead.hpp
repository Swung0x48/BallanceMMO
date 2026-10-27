#pragma once

// The server's half of the per-player lead (late-player design 2.2, 2.3,
// 2.5): what the runner does with a lateness_meter decision once
// physics.lead_adjust is on.  Pure bookkeeping, like lateness.hpp, so the
// gates and the arithmetic are unit-tested and the runner only wires them.
//
// A raise of k for player p, decided at the session's next tick N:
//   - E, the tick from which the server expects p's new schedule, covers the
//     slew: p's client gains one tick per tick period at 2x, so it produces
//     2k ticks before it has gained k, each still late.  With c_est, p's
//     tick when the message reaches it,
//         c_est = t_newest + (N - n_arr) + ceil(rtt_p),   E = c_est + 2k + 2
//     where t_newest is p's newest uplink tick received and n_arr the
//     session tick at its arrival.
//   - p's input buffer is reset over [N, E) (input_buffer::reset_hold): every
//     tick of it gets the substitute the buffer holds now, frozen, so the
//     message can carry that frame as final.
//   - k is capped by the total lead cap and by p's Sector bound: p's Sector
//     margin less a tick, plus - for a client that rewinds a late remote
//     Sector (plan step 14, resync design step B2) - its rewind reach.  A
//     capped raise is sent as far as the cap allows.  When the total cap
//     binds, the meter takes no further raise this assignment (one capped
//     to nothing is declined); when the Sector bound binds it does not
//     latch, since the margin moves with the path and the other members'
//     crossings: one capped to nothing is deferred for kSectorRetryTicks,
//     and the next raise is capped anew.
// A lower moves no range: held_from 0, E = N.
//
// Nothing is sent to a client that did not declare CAP_LEAD, or with the
// flag off: the meter's decision then stays pending, unacknowledged, so the
// player gets exactly one logged decision per assignment and nothing else -
// what the runner did before the flag existed.

#include <algorithm>
#include <cstdint>
#include <deque>

#include "../entity/session.hpp"
#include "lateness.hpp"
#include "timeline.hpp"

namespace bmmo::session {
    // The total lead cap (design 2.5): the lead of a player's assignment plus
    // every acknowledged offset never goes past this.  sim/late_tick.hpp's
    // per-player sanity cap is the same number.
    inline constexpr uint32_t kLeadCapTicks = 80;

    // How long a round trip of `rtt_ms` is in whole ticks, rounded up.
    inline uint32_t lead_rtt_ticks(uint32_t rtt_ms) {
        return static_cast<uint32_t>((static_cast<uint64_t>(rtt_ms) * 66 + 999) / 1000);
    }

    // p's tick when something the server sends now reaches it (design 2.3's
    // c_est): its newest received tick, the session ticks since that
    // datagram arrived, and one round trip (the uplink trip that datagram
    // spent, which p's clock ran on, plus the downlink trip ahead).
    inline int64_t lead_client_tick_estimate(uint32_t newest_tick, uint32_t newest_at, uint32_t now_tick,
                                             uint32_t rtt_ms) {
        return static_cast<int64_t>(newest_tick) + (static_cast<int64_t>(now_tick) - newest_at)
             + lead_rtt_ticks(rtt_ms);
    }

    // p's Sector margin as the server can see it (design 2.9): how many ticks
    // before its stamp another member's Sector event reaches p.  The server
    // relays a Sector event the moment it takes it, so at that moment the
    // event reaches p at about c_est; the margin is the stamp less that.  A
    // raise of k takes k off it.  A client that does not rewind a late
    // remote Sector needs the event at least a tick ahead, so k <= margin -
    // 1; one that does (lead_player::rewinds_sectors) re-derives the pass at
    // its stamp as long as the rewind, to stamp - 1, stays within its reach,
    // so k <= margin - 1 + kLeadSectorReachTicks (lead_sector_bound).  The
    // smallest of the last kSamples is what binds: the margin is a property
    // of the path, and the worst recent one is the one a raise must not
    // break.
    class lead_sector_margin {
    public:
        static constexpr size_t kSamples = 8;
        void push(int64_t margin) {
            samples_.push_back(margin);
            if (samples_.size() > kSamples) samples_.pop_front();
        }
        bool known() const { return !samples_.empty(); }
        int64_t worst() const { return samples_.empty() ? 0 : *std::min_element(samples_.begin(), samples_.end()); }
        void clear() { samples_.clear(); }
        // p's clock moved by `ticks` (an acknowledged lead delta): every
        // sample was taken against the old clock, and the same events would
        // now reach p that much closer to their stamp.  Without this a second
        // raise would be checked against margins the first already spent.
        void shift(int64_t ticks) {
            for (auto& m: samples_) m -= ticks;
        }
    private:
        std::deque<int64_t> samples_;
    };

    // How long a raise the Sector margin left nothing of waits before the
    // meter may decide again (lateness_meter::defer): ten seconds.  Sector
    // crossings are tens of seconds apart, so the margin rarely changes
    // faster, and a retry every streak would only repeat the same log line.
    inline constexpr uint32_t kSectorRetryTicks = 660;

    // The depth to which a session-core client rewinds for a late remote
    // Sector stamp (late-player design 2.9 and plan step 14, resync design
    // 2.7 case 3 and step B2): the floor of the client's reach,
    // rollback.hpp's kReachFloorTicks.  client_core's
    // sector_rewind_reachable compares the rewind to stamp - 1 against the
    // client's runtime max_resim_ticks, which since late-player plan step
    // 12 is a per-client value never below that floor; the server cannot
    // know a client's current reach, so it bounds with the floor every
    // client has (lead_test checks the two agree).  A stamp that reaches p
    // d ticks late rewinds d + 1 ticks.  Past this the client starts the
    // sector live and resyncs: still exact (protocol 10), but a state
    // transfer, which a raise must not make routine; so the bound stays,
    // as a reach bound that costs p only (the others' worlds are not
    // touched by p's late arrival of their events).
    inline constexpr int64_t kLeadSectorReachTicks = 48;

    // What the server knows of one player for the lead decision.
    struct lead_player {
        bool cap_lead = false;          // its SessionReady declared CAP_LEAD
        // Its SessionReady declared CAP_RESYNC_STATE: a session core with the
        // rollback on, which since resync step B1 rewinds a late remote Sector
        // to its stamp - 1 and re-derives the pass there (a resync beyond its
        // reach).  Its raises are bounded by the reach, not by the margin
        // alone (step B2); a client without it (a core with the rollback
        // off) starts a late sector late, and keeps the margin bound.
        bool rewinds_sectors = false;
        // The lead of the player's first assignment (the start base, or the
        // late join's lead): with the meter's offset, its total lead.
        uint32_t assigned_lead = 0;
        uint32_t rtt_ms = 0;            // its current round trip (set_player_rtt), not the peak
        bool have_rtt = false;
        // Its newest uplink tick received, and the session tick then.
        uint32_t newest_tick = 0, newest_at = 0;
        bool have_newest = false;
        lead_sector_margin sector_margin;
        // The decision sent and not yet acknowledged.
        bool outstanding = false;
        uint32_t id = 0;
        int k = 0;
        uint32_t held_from = 0, effective = 0;
        // Counters for the S1 lead line.
        uint64_t sent = 0, observed = 0, declined = 0, capped = 0, acked = 0;

        // p's tick when something sent now reaches it; false without a
        // newest tick (nothing arrived since the assignment).
        bool client_tick_estimate(uint32_t now_tick, int64_t& out) const {
            if (!have_newest) return false;
            out = lead_client_tick_estimate(newest_tick, newest_at, now_tick, rtt_ms);
            return true;
        }
        // A new datagram newer than any before: what c_est is measured from.
        void received(uint32_t newest, uint32_t now_tick) {
            if (have_newest && newest <= newest_tick) return;
            have_newest = true;
            newest_tick = newest;
            newest_at = now_tick;
        }
        // A new assignment (late join, resync): the outstanding decision is
        // dropped with the meter's pending one (a client on the new base
        // drops the message by its epoch, and an ack of it after the
        // re-anchor matches nothing), and the newest tick of the old
        // numbering means nothing for the new one.
        void reassigned() {
            outstanding = false;
            have_newest = false;
            sector_margin.clear();
        }
    };

    // Another member's Sector event stamped `stamp`, taken by the server
    // when its next tick is `now_tick`: a sample of p's Sector margin (the
    // stamp less p's c_est).  Only an event that reached the server on time
    // (stamp >= now_tick) says anything about p: a later one is late for
    // every member whatever their leads - the sender's problem, not p's path
    // - and taking it would cap p's raises at nothing for a lateness that is
    // not p's (design 0: one player's problem must not cost the others).
    // False when no sample was taken (late at the server, or p has no round
    // trip or newest tick to estimate its clock with).
    inline bool lead_sector_sample(lead_player& p, uint32_t stamp, uint32_t now_tick) {
        if (stamp < now_tick || !p.have_rtt) return false;
        int64_t client_tick = 0;
        if (!p.client_tick_estimate(now_tick, client_tick)) return false;
        p.sector_margin.push(static_cast<int64_t>(stamp) - client_tick);
        return true;
    }

    // How far a raise of p may go before another member's Sector event
    // would reach p past what p can take (design 2.9): the worst margin less
    // a tick, plus the rewind reach for a client that rewinds a late remote
    // Sector; never below 0.  Without a margin sample, `k` (no bound).
    inline int64_t lead_sector_bound(const lead_player& p, int64_t k) {
        if (!p.sector_margin.known()) return k;
        const int64_t reach = p.rewinds_sectors ? kLeadSectorReachTicks : 0;
        return std::max<int64_t>(p.sector_margin.worst() - 1 + reach, 0);
    }

    // What the runner does with one meter decision.
    enum class lead_route : uint8_t {
        none,       // no decision
        observe,    // flag off, or no CAP_LEAD: logged, nothing sent, the meter stays pending
        declined,   // the total cap left nothing of the raise: no further raise this assignment
        deferred,   // the Sector bound left nothing of it: the meter may decide again after kSectorRetryTicks
        send,       // a SessionLead goes out
    };

    struct lead_plan {
        lead_route route = lead_route::none;
        int k = 0;                       // the delta sent (capped)
        int decided_k = 0;               // the meter's k
        uint32_t held_from = 0;          // N for a raise (the buffer's cursor at the reset), 0 for a lower
        uint32_t effective = 0;          // E
        int64_t client_tick = 0;         // c_est, when known
        bool have_client_tick = false;
        // When k was capped: "total", "sector", or "total+sector" (both cut
        // it), and which of them bound it ("total" latches the meter at the
        // cap; "sector" does not, see lateness_meter::defer).
        const char* cap = nullptr;
        const char* binding = nullptr;
        int dropped = 0;                 // queued frames reset_hold dropped
        input_frame held{};              // the frozen substitute for [held_from, effective)
        uint32_t held_source = INPUT_SOURCE_DEFAULT;
    };

    // The lead-adjust path for one decision of `meter`, taken after the step
    // of tick N - 1 (`next_tick` = N, the session's next tick).  `buffer` is
    // the player's input buffer, reset over [N, E) for a sent raise.  With
    // `lead_adjust` off or no CAP_LEAD the route is observe and nothing is
    // touched.
    inline lead_plan lead_decide(bool lead_adjust, lead_player& p, lateness_meter& meter,
                                 const lateness_decision& d, uint32_t next_tick, input_buffer& buffer) {
        lead_plan plan;
        if (d.action != lateness_action::raise && d.action != lateness_action::lower) return plan;
        plan.decided_k = d.k;
        if (!lead_adjust || !p.cap_lead) {
            plan.route = lead_route::observe;
            ++p.observed;
            return plan;
        }
        plan.have_client_tick = p.client_tick_estimate(next_tick, plan.client_tick);
        int k = d.k;
        if (d.action == lateness_action::raise) {
            // Both caps are bounds on k; the smaller binds.  On a tie the
            // total cap is the binding one: the player is then at the cap.
            const int64_t total = static_cast<int64_t>(p.assigned_lead) + meter.offset();
            const int64_t total_bound = std::max<int64_t>(static_cast<int64_t>(kLeadCapTicks) - total, 0);
            const bool cut_total = k > total_bound;
            const int64_t sector_bound = lead_sector_bound(p, k);
            const bool cut_sector = k > sector_bound;
            const bool total_binds = cut_total && total_bound <= sector_bound;
            if (cut_total || cut_sector) {
                plan.cap = cut_total && cut_sector ? "total+sector" : (cut_total ? "total" : "sector");
                plan.binding = total_binds ? "total" : "sector";
                k = static_cast<int>(total_binds ? total_bound : sector_bound);
            }
            plan.k = k;
            const bool latch = total_binds;
            if (k <= 0) {
                if (latch) {
                    meter.decline(d.id);
                    plan.route = lead_route::declined;
                } else {
                    meter.defer(d.id, next_tick + kSectorRetryTicks);
                    plan.route = lead_route::deferred;
                }
                ++p.declined;
                return plan;
            }
            if (k != d.k) {
                meter.cap_pending(d.id, k, latch);
                ++p.capped;
            }
            // E covers the slew; never at or before N, so the range is never
            // empty.  Without a newest tick (nothing arrived since the
            // assignment - and then no sample could have raised it) E is N
            // plus the slew alone.
            const int64_t c = plan.have_client_tick ? plan.client_tick : static_cast<int64_t>(next_tick);
            int64_t effective = c + 2 * static_cast<int64_t>(k) + 2;
            if (effective <= next_tick) effective = static_cast<int64_t>(next_tick) + 1;
            plan.effective = static_cast<uint32_t>(effective);
            // The substitute is final from here: taken now, and reset_hold
            // freezes it for the range.  A cursor already at or past E (a
            // re-anchor ahead of the session) holds nothing: the message then
            // carries no range and the client only slews.
            plan.held = buffer.last();
            plan.held_source = buffer.substitute_tick();
            plan.dropped = buffer.reset_hold(plan.effective);
            if (plan.dropped < 0) {
                plan.dropped = 0;
                plan.held_from = 0;
            } else {
                plan.held_from = buffer.hold_from();
            }
        } else {
            plan.k = k;
            plan.held_from = 0;
            plan.effective = next_tick;
        }
        plan.route = lead_route::send;
        p.outstanding = true;
        p.id = d.id;
        p.k = plan.k;
        p.held_from = plan.held_from;
        p.effective = plan.effective;
        ++p.sent;
        return plan;
    }

    // p's client acknowledged decision `id` with `applied` ticks.  An ack of
    // anything but the outstanding decision is ignored and returns false.
    // `applied` is bounded by the k that was sent (lateness_bound_applied:
    // the client's word moves the offset by no more than the server sent,
    // and never the other way); `bounded`, when given, gets what counted.
    // The meter takes the ack against the E the decision was sent with; when
    // the meter no longer has it pending (cannot happen while outstanding is
    // kept in step with the meter, but an offset must never be lost) the
    // offset still moves, since the client did move its clock.  p's Sector
    // margin samples move with its clock.
    inline bool lead_acknowledge(lead_player& p, lateness_meter& meter, uint32_t id, int applied,
                                 int* bounded = nullptr) {
        if (!p.outstanding || id != p.id) return false;
        p.outstanding = false;
        ++p.acked;
        applied = lateness_bound_applied(applied, p.k);
        if (bounded) *bounded = applied;
        if (!meter.acknowledge(id, applied, p.effective)) meter.set_offset(meter.offset() + applied);
        p.sector_margin.shift(applied);
        return true;
    }
}
