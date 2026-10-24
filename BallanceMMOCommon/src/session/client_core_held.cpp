// The shared session-client core (docs/session-client-core-design.md 1.1):
// late-player design part one (the held section of the relay, the own
// correction, held_floor_) and part two (SessionLead, the forced own frames,
// the lead slew's bookkeeping).  Step E5 moves them out of the headless
// client (session_client.cpp at 9029872, "section J" of the design), verbatim
// but for the platform calls (send, now) and the configuration (rollback),
// with two additions in on_session_lead: SC's message filter (this running
// session's, assigned, our own player) and the refusal without the `slew`
// capability (design 2.3), which the headless client never takes.

#include "session/client_core.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

namespace bmmo::session {

    bool session_client_core::same_frame(const bmmo::session::input_frame& a, const bmmo::session::input_frame& b) {
        if (a.keys != b.keys || a.ball_type != b.ball_type || a.flags != b.flags) return false;
        for (int k = 0; k < 3; ++k)
            if (a.cam_right[k] != b.cam_right[k] || a.cam_up[k] != b.cam_up[k] || a.cam_dir[k] != b.cam_dir[k])
                return false;
        return true;
    }

    // Protocol 7 (late-player design 1.3): the held section of a relayed
    // datagram names our own ticks the server substituted and the frame
    // it applied there, and this is where our ball is put back on the
    // server's input.  A datagram numbered from another base (a
    // SessionAssign processed late here, or notices from before it) says
    // nothing about this numbering: its records are only counted as such,
    // once per (epoch, tick).
    //
    // Of our own epoch's records, each first arrival of a tick is
    // resolved to the frame the server applied (inline, the default
    // frame, or the bytes we sent for `source`, sent_inputs_), stored as
    // our input for that tick and journaled as SUBSTITUTED.  Then:
    //  - base: no rewind could ever reach it - without --rollback, while
    //    a resync waits for its full snapshot, or at or below held_floor_
    //    (the base tick, whose anchor precedes the numbering, and every
    //    tick a resync's full snapshot overwrote: the server's frame is in
    //    that snapshot already).  Never a resync: a resync's own first
    //    ticks held again would answer it with another one, for ever.
    //  - late: the anchor a rewind would use (tick - 1) is past the reach
    //    or out of the history, or the source is no longer in the sent
    //    ring.  The ball cannot be replayed onto the server's input, so
    //    this player (and only this player) resyncs.
    //  - recv: everything else.  A tick not simulated yet waits for its
    //    live frame (take_held_ahead); one simulated is compared with the
    //    frame its record holds (correct_own_tick).
    // recv + late + base is what the server's S2 lines count for this
    // epoch, and the header's held_total is checked against it
    // (check_held_complete).
    void session_client_core::note_held_section(const bmmo::session_remote_input_msg& msg) {
        if (!s.assigned_) return;
        // Did the server have our frame for the tick it just simulated, or
        // substitute one?  Once per server tick, from the header, so it
        // works with snapshots off (design 1.3; it used to need a
        // snapshot's acked_input_tick).  From a datagram of any epoch -
        // its tick and acked are the server's own numbers, the one thing
        // another epoch's datagram still says - but only for the ticks of
        // our numbering.  `lead` is how far our tick runs ahead of the
        // server's newest.
        const bool newer_ack = !s.acked_header_known_ || static_cast<int32_t>(msg.tick - s.acked_header_tick_) > 0;
        if (newer_ack) {
            s.acked_header_known_ = true;
            s.acked_header_tick_ = msg.tick;
            s.header_acked_ = msg.acked;
        }
        if (newer_ack && static_cast<int32_t>(msg.tick - s.tick_base_) >= 0) {
            ++s.input_reports_;
            if (static_cast<int32_t>(msg.tick - msg.acked) > 0) {
                const uint32_t behind = msg.tick - msg.acked;
                ++s.input_starved_;
                if (behind > s.worst_starved_) s.worst_starved_ = behind;
            }
            s.lead_sum_ += static_cast<int32_t>(current_tick() - msg.tick) > 0 ? current_tick() - msg.tick : 0;
            ++s.lead_count_;
        }
        if (msg.epoch != s.tick_base_) {
            for (const auto& record: msg.held)
                if (s.held_stale_seen_.first(msg.epoch, msg.held_tick(record))) ++s.subst_stale_epoch_;
            return;
        }
        if (!held_header_current() || static_cast<int32_t>(msg.tick - s.held_header_tick_) >= 0) {
            s.held_header_known_ = true;
            s.held_header_epoch_ = msg.epoch;
            s.held_header_tick_ = msg.tick;
            s.held_header_acked_ = msg.acked;
            s.held_header_total_ = msg.held_total;
        }
        for (const auto& record: msg.held) {
            const uint32_t tick = msg.held_tick(record);
            if (static_cast<int32_t>(tick - s.tick_base_) < 0) continue;   // before our numbering: never noticed
            if (!s.held_seen_.first(s.own_id_, tick)) { ++s.subst_dup_; continue; }
            learn_held_tick(tick);
            bmmo::session::input_frame frame{};
            if (!resolve_held_frame(record, frame)) {
                if (held_tick_is_base(tick)) { ++s.subst_base_; continue; }
                ++s.subst_late_;
                if (s.subst_unresolved_logs_++ < 8)
                    logf("held notice for own tick %u names source %u, which the sent ring no longer holds", tick,
                         record.source);
                request_resync("a substituted own frame could not be resolved", true);
                continue;
            }
            settle_held_tick(tick, frame);
        }
        check_held_complete(msg);
    }

    // A held tick no rewind could ever reach (the base rule of
    // note_held_section): without --rollback, while a resync waits for
    // its full snapshot, or at or below held_floor_.
    bool session_client_core::held_tick_is_base(uint32_t tick) const {
        return !config.rollback || s.resync_pending_ || static_cast<int32_t>(tick - s.held_floor_) <= 0;
    }

    // One own tick the server applied `frame` at, resolved: base, held
    // for its live frame, late, or recv (note_held_section's rules; a
    // SessionLead's simulated range takes the same path).
    void session_client_core::settle_held_tick(uint32_t tick, const bmmo::session::input_frame& frame) {
        if (held_tick_is_base(tick)) {
            ++s.subst_base_;
            // Step A3: a tick of a pending exact resync's epoch has a record
            // the apply's re-step replays from S: amended now, replayed by it.
            const bool exact = resync_amends(tick);
            // Kept all the same (design 1.3: stored with or without a
            // way to replay it) - a tick not simulated yet when its live
            // frame is (an exact resync's corrected once its live frame is
            // recorded: amended while the resync is pending, replayed after).
            if (static_cast<int32_t>(tick - current_tick()) > 0) hold_ahead(tick, frame, exact);
            else {
                store_held_frame(tick, frame);
                if (exact) amend_for_resync(tick, frame);
            }
            return;
        }
        if (static_cast<int32_t>(tick - current_tick()) > 0) {   // not simulated yet
            ++s.subst_recv_;
            hold_ahead(tick, frame, true);
            return;
        }
        store_held_frame(tick, frame);
        // The gate note_input_correction applies: the anchor of the
        // rewind, tick - 1, still re-simulable and in the history - as deep
        // as the re-step of an exact resync for a tick inside it (step A3:
        // window_rewind_reach; resync design 2.6.4, a notice for a tick >= base
        // finds its record and amends it).
        const uint32_t anchor = tick - 1;
        const uint32_t reach = window_rewind_reach(tick);
        const bool reachable = current_tick() - anchor <= reach && s.rollback_.knows_tick(anchor);
        if (!reachable) {
            ++s.subst_late_;
            if (s.subst_late_logs_++ < 8)
                logf("held notice for own tick %u arrived at tick %u, past the reach of %u ticks", tick,
                     current_tick(), reach);
            request_resync("a substituted own frame arrived past the reach", true);
            return;
        }
        ++s.subst_recv_;
        correct_own_tick(tick, frame);
    }

    // SessionLead (late-player design 2.4): the server moves this
    // client's lead by msg.delta and, for a raise, applied `held` at
    // every tick of [held_from, effective) - its buffer for us was reset
    // there, and our frames below E are stale to it.  At our current
    // tick c (bmmo::session::lead_split_range):
    //  1. the range's ticks at or before c ran on our own input: each is
    //     a substituted tick like a held notice's (settle_held_tick -
    //     stored, journaled SUBSTITUTED, the record amended and one
    //     replay from the earliest), counted once with the notices;
    //  2. the ticks of (c, E) are forced: their live frames drive the
    //     own ball with `held` (frame()), so we play what the server
    //     applies and their notices, when they come, change nothing;
    //  3. every tick of the range counts as learned for the completeness
    //     check: the server's held_total counts them, and a notice that
    //     comes later is a re-delivery;
    //  4. the clock slews by delta (pace_and_frame), and the ack goes out
    //     when the slew ends, with what it moved - or, when a resync
    //     request ends it first, with the whole delta, which the
    //     re-anchor at L_eff applies (request_resync).
    // Nothing is renumbered: the first tick played on our own input
    // again is E, stamped E, exactly where the server's buffer takes our
    // frames from.  Copies are dropped by seq; a decision of another
    // base by epoch (a resync crossed it: the server dropped it too).
    void session_client_core::on_session_lead(const bmmo::session_lead_msg& msg) {
        if (msg.session != s.session_ || s.phase_ != client_core_state::phase::running) return;
        if (!s.assigned_ || msg.player != s.own_id_) return;
        // Design 2.3: a host that cannot slew sent no CAP_LEAD, and the
        // server decides no lead for it; one that comes anyway is a
        // protocol error - counted and journaled, never applied (no range
        // amended or forced) and never acknowledged.  Once per decision:
        // its seq is remembered, so the server's copies (reliable plus
        // unreliable) are dropped as copies below, not refused again.
        if (s.lead_seen_.count(msg.seq)) { ++s.lead_dups_; return; }
        if (!caps_.slew) {
            s.lead_seen_.insert(msg.seq);
            ++s.lead_refused_;
            journal_.note("lead: SessionLead " + std::to_string(msg.seq) + " refused: this host cannot slew (no CAP_LEAD)");
            return;
        }
        if (msg.epoch != s.tick_base_) {
            // An older base: void.  A newer one: an unreliable copy that
            // overtook its assignment; the reliable one follows it.
            ++s.lead_stale_;
            logf("lead: SessionLead %u of base %u dropped (our base %u)", msg.seq, msg.epoch, s.tick_base_);
            return;
        }
        s.lead_seen_.insert(msg.seq);
        // A slew still running (the server never overlaps two, so this is
        // defensive): it ends here with what it moved, acknowledged.
        if (s.lead_slew_.active()) finish_lead_slew("superseded by SessionLead " + std::to_string(msg.seq));
        // A resync of ours in flight: the server answers it with an
        // assignment at L_eff that voids this decision (send_session_assign
        // clears the outstanding one, and our ack would come after the
        // request on the same channel and be ignored).  The range is
        // still applied - the server plays `held` there until it
        // re-anchors us - but the clock is not slewed for an assignment
        // that replaces it, and nothing is acknowledged or counted.
        const bool voided = s.resync_in_flight_;
        // The slew starts before the range is settled: an amended tick
        // past the reach asks for a resync (settle_held_tick), and
        // request_resync then acknowledges this decision in full ahead of
        // the request, so the re-anchor at L_eff carries it.
        const uint64_t resyncs_before = s.resyncs_sent_;
        if (!voided) {
            s.lead_slew_.start(msg.seq, msg.delta);
            s.lead_slew_clock_ = platform_.now(clock_site::slew_start);
        }
        const uint32_t next_unsimulated = s.frames_since_anchor_ >= 1 ? current_tick() + 1 : s.tick_base_;
        const auto ranges = bmmo::session::lead_cap_forced(
            bmmo::session::lead_split_range(msg.held_from, msg.effective, next_unsimulated),
            static_cast<uint32_t>(bmmo::session::kSentInputRing));
        uint32_t amended = 0, forced = 0, known = 0;
        if (msg.held_from != 0) {
            // Up to the forced range's end, capped: the ticks past the cap
            // are not ours to force or learn (lead_cap_forced), their
            // notices settle them.
            for (uint32_t t = msg.held_from; static_cast<int32_t>(t - ranges.force_to) < 0; ++t) {
                const bool first = s.held_seen_.first(s.own_id_, t);
                if (first) learn_held_tick(t);
                else ++known;
                if (static_cast<int32_t>(t - ranges.force_from) < 0) {
                    // Simulated: a notice that came first settled it already.
                    if (first) { settle_held_tick(t, msg.held); ++amended; }
                    continue;
                }
                // Ahead: the live frame plays `held`.  A notice that named
                // the tick first waits in held_ahead_ to compare the record
                // with what we play, which is now the same frame (the
                // server froze it for the range): dropped, and a recv one
                // counted as the same frame here, since the forced tick
                // does not count it again.
                if (const auto ahead = s.held_ahead_.find(t); ahead != s.held_ahead_.end()) {
                    if (ahead->second.second) ++s.subst_same_;
                    s.held_ahead_.erase(ahead);
                }
                s.forced_own_[t] = client_core_state::forced_own{msg.held, first};
                ++forced;
            }
        }
        const bool reanchored = s.resyncs_sent_ != resyncs_before;
        if (!voided) ++s.lead_adjustments_;
        else ++s.lead_void_;
        logf("lead: SessionLead %u %s: k=%+d N=%u E=%u at tick %u (base %u): amended %u [%u,%u) forced %u [%u,%u) "
             "already noticed %u, held-source=%s; %s",
             msg.seq, voided ? "void (a resync of ours is in flight), range applied" : "applied",
             static_cast<int>(msg.delta), msg.held_from, msg.effective,
             s.frames_since_anchor_ >= 1 ? current_tick() : s.tick_base_, s.tick_base_, amended, ranges.amend_from,
             ranges.amend_to, forced, ranges.force_from, ranges.force_to, known,
             msg.held_source == bmmo::session::INPUT_SOURCE_DEFAULT ? "default" : std::to_string(msg.held_source).c_str(),
             voided ? "no slew" : reanchored ? "acknowledged in full ahead of the resync it asked for"
                                : msg.delta > 0 ? "slewing at 2x" : "slewing at 0.9x");
        journal_.note("lead: SessionLead " + std::to_string(msg.seq) + " " + (msg.delta > 0 ? "+" : "")
                     + std::to_string(msg.delta) + " held [" + std::to_string(msg.held_from) + ", "
                     + std::to_string(msg.effective) + "), forced from " + std::to_string(ranges.force_from)
                     + (voided ? " (void: resync in flight)" : ""));
        // A zero delta: nothing to slew, acknowledged at once (unless a
        // resync request did it, or the decision is void).
        if (!voided && !reanchored && !s.lead_slew_.active()) finish_lead_slew("nothing to slew");
    }

    // The slew ends - moved in full, superseded, or ended by a resync
    // request (`reanchored`: acknowledged in full, the re-anchor at
    // L_eff applies all of it, lead_slew::finish_reanchored) - and the
    // server is told what it applied (its lead_offset and a resync's
    // L_eff follow that, bounded by the delta it sent).  Reliable: it is
    // ordered before the resync request request_resync sends right after
    // it, so the re-anchor already counts it.
    void session_client_core::finish_lead_slew(const std::string& why, bool reanchored) {
        const uint32_t seq = s.lead_slew_.seq();
        const int delta = s.lead_slew_.delta();
        const int moved = s.lead_slew_.applied();
        const int applied = reanchored ? s.lead_slew_.finish_reanchored() : s.lead_slew_.finish();
        s.lead_offset_ += applied;
        bmmo::session_lead_ack_msg ack;
        ack.session = s.session_;
        ack.seq = seq;
        ack.applied = static_cast<int16_t>(std::clamp(applied, -32768, 32767));
        ack.serialize();
        platform_.send(ack.raw.str(), send_mode::reliable);
        logf("lead: SessionLead %u acknowledged at tick %u: applied %+d of %+d, the slew moved %+d (%s); lead offset %+d",
             seq, current_tick(), applied, delta, moved, why.c_str(), s.lead_offset_);
        journal_.note("lead: SessionLead " + std::to_string(seq) + " applied " + std::to_string(applied) + " of "
                     + std::to_string(delta) + " (" + why + ")");
    }

    // A new assignment voids the adjustment in flight and the forced
    // ticks (numbered from the old base): the server dropped the decision
    // with its base and ignores an ack of it.  `why`: a new base, or the
    // session's end.
    void session_client_core::drop_lead_state(const char* why) {
        if (s.lead_slew_.active()) {
            logf("lead: SessionLead %u voided by %s after %+d of %+d ticks", s.lead_slew_.seq(), why,
                 s.lead_slew_.applied(), s.lead_slew_.delta());
            s.lead_slew_.finish();
        }
        s.forced_own_.clear();
    }

    // The frame a held record says the server applied.  False when it
    // names a source our sent ring no longer holds (the server writes the
    // frame inline when the source is 96 ticks back or before our epoch,
    // so inside the reach this does not happen).
    bool session_client_core::resolve_held_frame(const bmmo::session_remote_input_msg::held_record& record,
                                                 bmmo::session::input_frame& out) const {
        if (record.has_frame) { out = record.frame; return true; }
        if (record.source == bmmo::session::kHeldSourceDefault) { out = bmmo::session::input_frame{}; return true; }
        const auto it = s.sent_inputs_.find(record.source);
        if (it == s.sent_inputs_.end()) return false;
        out = it->second;
        return true;
    }

    // The server's frame becomes our input for `tick`, with or without a
    // body there: a replay that re-makes the own body (design 3.3) drives
    // it from own_inputs_, and recorded_input_at must agree with the
    // record.  The journal's SUBSTITUTED record follows the tick's FRESH
    // one, and a reader takes the later.
    void session_client_core::store_held_frame(uint32_t tick, const bmmo::session::input_frame& frame) {
        if (config.rollback) {
            s.own_inputs_[tick] = frame;
            while (s.own_inputs_.size() > 128) s.own_inputs_.erase(s.own_inputs_.begin());
        }
        if (journal_.journalling()) journal_.writer().input(tick, s.own_id_, frame, bmmo::session::JOURNAL_INPUT_SUBSTITUTED);
    }

    // A notice for a tick this client has not simulated yet (it runs
    // behind the server's cursor: a stall, a slow start).  Its live frame
    // would overwrite our input and journal FRESH after SUBSTITUTED, so it
    // waits for that frame (take_held_ahead).  `correct`: a recv record,
    // compared and replayed then; a base one is only stored.
    void session_client_core::hold_ahead(uint32_t tick, const bmmo::session::input_frame& frame, bool correct) {
        s.held_ahead_[tick] = {frame, correct};
        while (s.held_ahead_.size() > bmmo::session::kSentInputRing) s.held_ahead_.erase(s.held_ahead_.begin());
    }

    // After the live frame of `tick` is recorded: a notice that named it
    // earlier is applied now, the record compared with the frame the
    // server applied and the tick replayed on the next frame when they
    // differ.  Notices the numbering moved past are dropped.
    void session_client_core::take_held_ahead(uint32_t tick) {
        while (!s.held_ahead_.empty() && static_cast<int32_t>(s.held_ahead_.begin()->first - tick) < 0)
            s.held_ahead_.erase(s.held_ahead_.begin());
        const auto it = s.held_ahead_.find(tick);
        if (it == s.held_ahead_.end()) return;
        const bmmo::session::input_frame frame = it->second.first;
        const bool correct = it->second.second;
        s.held_ahead_.erase(it);
        store_held_frame(tick, frame);
        // Step A3: a pending exact resync re-steps this record from S.
        if (correct && resync_amends(tick)) {
            amend_for_resync(tick, frame);
            return;
        }
        // A resync since the notice arrived: its snapshot is the server's
        // world, our record of this tick is not.
        if (!correct || !config.rollback || s.resync_pending_ || static_cast<int32_t>(tick - s.held_floor_) <= 0) return;
        correct_own_tick(tick, frame);
    }

    // Step A3 (resync design 2.2: "the server's held notices name every
    // substituted tick, so the records are amended before or after the apply
    // as usual"): while an exact resync is pending, a tick of its epoch
    // (base and after) has a record - the live frames record from base on -
    // and the apply's re-step feeds the own ball from the RECORD of each
    // tick.  A notice stored in own_inputs_ alone (the base rule) left the
    // record with the frame we played: a5u_lead, own ticks 2779-2788 played
    // with keys 0 where the server applied 5, 0.17 m on the own ball for
    // good after the apply.
    bool session_client_core::resync_amends(uint32_t tick) const {
        return config.rollback && s.resync_pending_ && s.resync_exact_ && static_cast<int32_t>(tick - s.resync_base_) >= 0;
    }

    // The record takes the server's frame; no rewind of its own (the apply's
    // re-step is the replay).  The own_corrections switch holds here too.
    void session_client_core::amend_for_resync(uint32_t tick, const bmmo::session::input_frame& frame) {
        if (!config.own_corrections) return;
        const std::string entity = s.rollback_.recorded_own_entity(tick);
        if (entity.empty()) return;
        bmmo::session::input_frame played{};
        if (s.rollback_.recorded_input(entity, tick, played) && same_frame(played, frame)) return;
        if (s.rollback_.amend_recorded_input(entity, tick, frame)) ++s.resync_amends_;
    }

    // The own branch for a substituted tick inside the reach (design
    // 1.3).  A replay feeds the own ball from the RECORD of each tick,
    // not from own_inputs_, so the record is what has to change; its own
    // entity is the one to amend, since a trafo may have renamed the
    // ball since.  No ball at the tick: the frame drove nothing.  The
    // frame we played: the server's ball and ours ran on the same input,
    // nothing to replay.  Otherwise the record takes the server's frame
    // and a later frame replays from tick - 1 (replay_corrected_inputs:
    // one replay from the earliest tick for every correction pending,
    // own and remote; an own-only one may wait a few frames for the
    // notices still coming, own_replay_pacer).
    void session_client_core::correct_own_tick(uint32_t tick, const bmmo::session::input_frame& frame) {
        // The own_corrections switch (design 1.2; E9): off, the record of
        // the tick keeps what we played and no own replay is asked for.
        if (!config.own_corrections) return;
        const std::string entity = s.rollback_.recorded_own_entity(tick);
        if (entity.empty()) { ++s.subst_bodiless_; return; }
        bmmo::session::input_frame played{};
        const bool have_played = s.rollback_.recorded_input(entity, tick, played);
        if (have_played && same_frame(played, frame)) { ++s.subst_same_; return; }
        ++s.subst_diff_;
        if (!s.rollback_.amend_recorded_input(entity, tick, frame)) return;   // the tick was dropped meanwhile
        // A difference no replay can see (a camera turned with no leaf
        // key down, Shift or Space, navigation off in both): the record
        // holds the server's frame now, which every later replay feeds,
        // and a replay of its own would step the same world again.
        if (have_played && !bmmo::session::replay_sees_difference(played, frame)) { ++s.subst_unseen_; return; }
        // Earliest or not, the replay carries an own correction: counted
        // under subst rew, and a refusal resyncs (replay_corrected_inputs).
        s.input_rewind_has_own_ = true;
        ++s.input_rewind_own_amends_;
        if (!s.input_rewind_pending_ || static_cast<int32_t>(tick - s.input_rewind_tick_) < 0) {
            s.input_rewind_pending_ = true;
            s.input_rewind_tick_ = tick;
            s.input_rewind_entity_ = entity;
            s.input_rewind_own_ = true;
        }
    }

    // The ticks whose notice has arrived, for the completeness check: a
    // total since the epoch and the recent ticks themselves, enough to
    // count those at or below a datagram's tick (a notice rides at most
    // kHeldNoticeTicks datagrams, and a datagram is judged 64 ticks on).
    void session_client_core::learn_held_tick(uint32_t tick) {
        ++s.held_learned_total_;
        s.held_learned_recent_.insert(tick);
        while (s.held_learned_recent_.size() > 256) s.held_learned_recent_.erase(s.held_learned_recent_.begin());
    }

    uint64_t session_client_core::held_learned_upto(uint32_t tick) const {
        uint64_t later = 0;
        for (auto it = s.held_learned_recent_.upper_bound(tick); it != s.held_learned_recent_.end(); ++it) ++later;
        return s.held_learned_total_ - later;
    }

    // Completeness (design 1.3): `held_total` counts our substituted
    // ticks at or after the epoch up to the datagram's tick, and every one
    // of them is noticed in 32 consecutive datagrams.  A datagram whose
    // count is above the ticks we learned up to its tick may only be
    // ahead of a sibling datagram still on its way; if it is still above
    // 64 ticks later, every copy of a notice was lost, and that tick ran
    // on a frame we will never know.  Counted late (the server counted
    // it) and answered with a resync, unless one is on its way already -
    // its snapshot covers the tick or the next resync's will.  The
    // shortfall is credited so the datagrams after it do not count it
    // again.
    void session_client_core::check_held_complete(const bmmo::session_remote_input_msg& msg) {
        if (msg.held_total > held_learned_upto(msg.tick) + s.held_gap_credit_) {
            auto& total = s.held_gap_watch_[msg.tick];
            total = std::max(total, msg.held_total);
            while (s.held_gap_watch_.size() > 256) s.held_gap_watch_.erase(s.held_gap_watch_.begin());
        }
        for (auto it = s.held_gap_watch_.begin(); it != s.held_gap_watch_.end();) {
            if (static_cast<int32_t>(msg.tick - it->first) <= static_cast<int32_t>(kHeldGapTicks)) break;
            const uint64_t have = held_learned_upto(it->first) + s.held_gap_credit_;
            if (it->second > have) {
                const uint64_t missing = it->second - have;
                s.subst_late_ += missing;
                s.held_gap_credit_ += missing;
                logf("held notices lost: held_total %u at tick %u, %llu of them never arrived", it->second, it->first,
                     static_cast<unsigned long long>(missing));
                // The base rule a received record follows: no resync for
                // what no rewind could have corrected anyway - without
                // --rollback every record is base, and a datagram at or
                // below the floor names only ticks a resync's snapshot
                // covered.
                if (!s.resync_pending_ && config.rollback && static_cast<int32_t>(it->first - s.held_floor_) > 0)
                    request_resync("substituted own frames whose every notice was lost", true);
            }
            it = s.held_gap_watch_.erase(it);
        }
    }

    // Everything above is numbered from our base: a new assignment (start
    // or resync) starts it over.  The sent ring goes too - the server
    // writes a source from before the epoch inline.
    // A replay asked for by a correction under the previous numbering
    // has no anchor in the new one (the history went with it): dropped,
    // not run into a refusal - a refused own replay asks for a resync.
    void session_client_core::reset_held_epoch(const char* why) {
        drop_pending_rewind();
        drop_lead_state(why);
        s.own_replay_pacer_.reset();
        s.held_seen_.clear();
        s.held_ahead_.clear();
        s.sent_inputs_.clear();
        s.held_learned_recent_.clear();
        s.held_learned_total_ = 0;
        s.held_gap_credit_ = 0;
        s.held_gap_watch_.clear();
    }

    void session_client_core::drop_pending_rewind() {
        s.input_rewind_pending_ = false;
        s.input_rewind_own_ = false;
        s.input_rewind_has_own_ = false;
        s.input_rewind_own_amends_ = 0;
        s.input_rewind_entities_.clear();
    }

    // Whether the server is still substituting our ticks: its newest
    // header of our epoch says it lacked our frame for the tick it
    // simulated (acked < tick).  Unknown (no header of this epoch yet)
    // counts as yes - the pacer's reach deadline still bounds the wait.
    bool session_client_core::own_notices_coming() const {
        return !held_header_current() || static_cast<int32_t>(s.held_header_tick_ - s.held_header_acked_) > 0;
    }

    // Another rewind that replayed from `anchor` already carried a
    // waiting own-only replay (bmmo::session::replay_subsumes; any
    // anchor when `server_rows`: a snapshot rollback that wrote the
    // server's own-ball row at its anchor): dropped instead of run.  Run
    // after it, a replay from an earlier anchor would also restore our
    // prediction over the snapshot's rows.  A pending replay that carries
    // a remote's correction is left to replay_corrected_inputs as before.
    void session_client_core::drop_carried_own_replay(uint32_t anchor, bool server_rows) {
        if (!s.input_rewind_pending_ || !s.input_rewind_own_ || !s.input_rewind_entities_.empty()) return;
        if (!server_rows && !bmmo::session::replay_subsumes(anchor, s.input_rewind_tick_)) return;
        ++s.subst_subsumed_;
        s.subst_saved_ += s.input_rewind_own_amends_;
        s.own_replay_pacer_.ran();
        drop_pending_rewind();
    }

    // ------------------------------------------------- the forced own frames
    // A tick a SessionLead forced (late-player design 2.4): the server
    // applies `held` here and drops our frame as stale, so the own ball is
    // driven with it the way a replay drives it (the host's half: polling
    // off, the frame through nav_input, polling on after the step).  The
    // core's half: which frame (taken here, the older ones dropped), and
    // the record of the tick (play_forced_own).
    bool session_client_core::take_forced_own(uint32_t tick, client_core_state::forced_own& out) {
        while (!s.forced_own_.empty() && static_cast<int32_t>(s.forced_own_.begin()->first - tick) < 0)
            s.forced_own_.erase(s.forced_own_.begin());
        const auto it = s.forced_own_.find(tick);
        if (it == s.forced_own_.end()) return false;
        out = it->second;
        s.forced_own_.erase(it);
        return true;
    }

    // A forced tick plays the server's frame (design 2.4): recorded,
    // journaled SUBSTITUTED and sent as such - the server drops it as
    // stale, and the record holds what the server applied, so its
    // notice, when it comes, changes nothing.
    void session_client_core::play_forced_own(uint32_t tick, const client_core_state::forced_own& forced,
                                              bmmo::session::input_frame& input) {
        input = forced.frame;
        ++s.lead_forced_played_;
        if (forced.count) {
            if (held_tick_is_base(tick)) ++s.subst_base_;
            else { ++s.subst_recv_; ++s.subst_same_; }
        }
    }

}
