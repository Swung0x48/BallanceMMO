// The shared session-client core (docs/session-client-core-design.md 1.1):
// the frame.  Step E5 moves the frame's two rewinds out of the headless
// client (session_client.cpp at 9029872), verbatim but for the platform calls
// (bind_world, keep_live_cells) and the legacy back-call recorded_input_at:
// replay_corrected_inputs (pre_step step 5) and force_rewind, the core half
// of the headless --force-rollback (step 7's hook; the cadence and the
// traces around it stay the host's).  pre_step and post_step follow at E9.
// Step E8 adds the sectors (session_client.cpp at 924092a), verbatim but for
// the platform calls (sector_manager_active, run_sector, record_activation,
// poll_sector): request_sector with the start sector pending until the
// assignment (pre_step step 1), hold_sector, the sector union's pass
// (step 9) and the own sector poll (post_step step 26).

#include "session/client_core.hpp"

#include <cstdint>
#include <set>
#include <string>

namespace bmmo::session {

    // One replay for every input correction this frame collected.
    void session_client_core::replay_corrected_inputs() {
        s.own_replay_pacer_.frame();
        if (!s.input_rewind_pending_) return;
        // Our own corrections alone are coalesced (own_replay_pacer): while
        // the server keeps substituting our ticks, one notice per server
        // tick would otherwise be one replay per frame across whatever the
        // last ~42 ticks hold - the sector activation on the lane - and
        // that cost kept the client late, which kept the notices coming
        // (lp4_a0/a1: five of seven runs ended in a schedule rebase).  The
        // records are amended already and the pending replay keeps the
        // earliest tick, so the one that runs carries them all; it runs
        // before that tick's anchor would leave the reach.  A remote's
        // correction is never held back, and takes ours along.
        // Only while the server still substitutes our ticks: once its
        // header says it has our frames again no notice will join the
        // waiting replay, which then runs at once.
        if (s.input_rewind_own_ && s.input_rewind_entities_.empty()
                && !s.own_replay_pacer_.due(current_tick(), s.input_rewind_tick_, s.rollback_.thresholds().max_resim_ticks,
                                          own_notices_coming())) {
            ++s.subst_deferred_;
            return;
        }
        const uint32_t anchor = s.input_rewind_tick_;
        const std::string entity = s.input_rewind_entity_;
        // The earliest correction names the replay: our own frame the
        // server substituted (late-player design 1.3) or a remote's
        // relayed frame.  One replay covers both kinds and is counted in
        // each class it carries: subst rew / the window's substrew when
        // an own correction is among this frame's, inrew when a remote's
        // is (both, when both are).
        const bool own = s.input_rewind_own_;
        const bool has_own = s.input_rewind_has_own_ || own;
        const uint32_t own_amends = s.input_rewind_own_amends_;
        std::set<std::string> corrected;
        corrected.swap(s.input_rewind_entities_);
        const bool has_remote = !corrected.empty() || !own;
        drop_pending_rewind();
        const uint32_t now = current_tick();
        if (static_cast<int32_t>(anchor - now) > 0) return;
        auto input_at = [this](const std::string& name, uint32_t tick, bmmo::session::input_frame& out) {
            return legacy.recorded_input_at(name, tick, out);
        };
        const scope_guard keep_cells = platform_.keep_live_cells(*this);
        const std::string reason = own ? "own input substituted at tick " + std::to_string(anchor)
                                       : entity + " input corrected at tick " + std::to_string(anchor);
        if (has_own) s.own_replay_pacer_.ran();
        if (s.rollback_.rewind_to(platform_.bind_world(), anchor - 1, now, input_at, reason)) {
            if (has_own) {
                ++s.subst_rewinds_;
                s.subst_rewind_depth_ += now - (anchor - 1);
                if (own_amends > 1) s.subst_saved_ += own_amends - 1;   // amendments that rode along
            }
            if (has_remote) {
                ++s.input_rewinds_;
                s.input_rewind_depth_ += now - (anchor - 1);
            }
            // Named by every source ball it corrects: the harness asks
            // whether rewinds naming a late player grow (late-player
            // design 5.2), and one replay covers all of this frame's.
            for (const auto& [id, remote]: s.remotes_)
                if (corrected.count(remote.entity)) ++s.window_inrew_src_[id];
            return;
        }
        if (has_remote) ++s.input_rewinds_missed_;
        if (has_own) ++s.subst_rewinds_missed_;
        if (s.input_rewinds_missed_ + s.subst_rewinds_missed_ < 8)
            logf("input correction for %s at tick %u is outside the rollback history at local tick %u: "
                 "a snapshot has to re-anchor it", entity.c_str(), anchor, now);
        // Our own ball ran on a frame the server did not apply (its
        // record is amended, never replayed), and with snapshots off
        // nothing else would ever repair it.  Not while a resync waits
        // for its snapshot: that snapshot is the repair.
        if (has_own && !s.resync_pending_) request_resync("a substituted own frame could not be replayed", true);
    }

    // Rewind for no reason at all (the headless --force-rollback, at the
    // host's cadence).  A solo session's world is a pure function of its
    // recorded inputs, so the hashes of a run with these must equal those
    // of a run without; any difference is state the replay failed to
    // restore.  The live cells stay kept until the caller drops the result
    // (the host's traces after the rewind ran inside that scope).
    // Nothing at all (not rolled, nothing counted, no cells kept) without
    // rollback or before `depth + 2` ticks have been played: the anchor
    // would wrap.  The headless host never calls it then (its own checks
    // in maybe_force_rollback); a retail verb may.
    session_client_core::forced_rewind session_client_core::force_rewind(uint32_t depth) {
        const uint32_t now = current_tick();
        if (!config.rollback || now < depth + 2) return {};
        auto input_at = [this](const std::string& name, uint32_t tick, bmmo::session::input_frame& out) {
            return legacy.recorded_input_at(name, tick, out);
        };
        const uint32_t anchor = now - depth;
        forced_rewind result{false, platform_.keep_live_cells(*this)};
        if (s.rollback_.rewind_to(platform_.bind_world(), anchor, now, input_at, "forced")) {
            ++s.forced_rollbacks_;
            s.forced_rollback_depth_ += now - anchor;
            drop_carried_own_replay(anchor, false);
            result.rolled = true;
        } else {
            ++s.forced_rollbacks_missed_;
        }
        return result;
    }

    // ---------------------------------------------------------- sectors
    // A sector this client enters, as an input: stamped for `due`, sent at
    // once, and handed to the union, which starts it before the step of
    // that tick (or the first one after it the pacer allows) - as the
    // server does with the event, and every other member with its relay.
    // Once per sector per session.
    void session_client_core::hold_sector(int sector, uint32_t due, const char* why) {
        if (s.sectors_requested_.count(sector)) return;
        s.sectors_requested_.insert(sector);
        bmmo::session_event_msg event;
        event.tick = due;
        event.type = bmmo::session::event_type::Sector;
        event.sector = sector;
        send_event(event);
        s.sectors_.stamp(s.own_id_, sector, due);
        logf("sector %d stamped for tick %u (%s, lead %u, input_delay %u), event sent ahead", sector, due, why,
             sector_lead_ticks(), static_cast<unsigned>(s.input_delay_));
    }

    // Before the assignment the request waits for the first assigned frame:
    // the anchor precedes the renumbering, so a stamp now would be
    // anchor-relative.  After it, the next tick to step plus the lead, the
    // stamp hold_pending_sector gives on that frame.  Every request made
    // before the assignment waits, in order (a repeat is dropped by
    // hold_sector, as after the assignment).
    void session_client_core::request_sector(int sector, const char* why) {
        if (!s.assigned_) {
            s.start_sector_pending_ = true;
            s.start_sectors_.emplace_back(sector, why ? why : "");
            return;
        }
        hold_sector(sector, current_tick() + 1 + sector_lead_ticks(), why);
    }

    // A held --start-sector: on the first frame the session is assigned
    // (tick base and lead known), stamp it `lead` ticks ahead like any
    // other sector this client enters (each request that waited, in order).
    void session_client_core::hold_pending_sector(uint32_t tick) {
        if (s.start_sector_pending_ && s.assigned_) {
            s.start_sector_pending_ = false;
            const auto waiting = std::move(s.start_sectors_);
            s.start_sectors_.clear();
            for (const auto& [sector, why]: waiting) hold_sector(sector, tick + sector_lead_ticks(), why.c_str());
        }
    }

    // The sector union (session/sector_union.hpp), the rule the server's
    // update_sectors() runs: one count of the pacer per step, before it,
    // from the manager's state after the previous step, and one pass per
    // idle manager for the lowest sector owed.  The two worlds run the same
    // scripts from the same anchor on the same stamps, so the tick a pass
    // starts at is the same on every side.
    void session_client_core::sector_union_pass(uint32_t tick) {
        const auto pass = s.sectors_.tick(tick, platform_.sector_manager_active());
        if (pass) {
            s.sector_cell_ = pass.sector;   // the detect block below must not take our own write for a crossing
            platform_.run_sector(pass.sector);
            platform_.record_activation(tick, true);
            logf("sector +%d at tick %u (player %u, stamped %u%s), running: %s", pass.sector, tick,
                 pass.player, pass.stamped_tick, tick > pass.stamped_tick ? ", held by the pacer" : "",
                 [this] { std::string text; for (int x: s.sectors_.active()) text += std::to_string(x) + " "; return text; }().c_str());
        }
    }

    // Sector: the sector the level's scripts last wrote.  With the
    // checkpoint chain held this is a crossing whose manager run did not
    // happen: it becomes an input, stamped ahead like the retail client's
    // cell poll (physics_session_frame).  This frame's step was `tick`, so
    // the earliest the manager can run is before tick + 1.  Without the hold
    // (a graph the hold did not understand) the chain already activated it:
    // report it after the fact, out of lockstep.
    //
    // A moved resetpoint is a crossing too, the own ball's alone (only the
    // checkpoint chain moves it, in the frame it writes the cell).  It is
    // what catches a trailing ball: a peer's crossing has the union pass
    // write the same sector into the cell first, so the chain's own write
    // changes nothing there - the retail client reports that crossing from
    // OnPostCheckpointReached.
    void session_client_core::poll_own_sector(uint32_t tick) {
        const sector_poll poll = platform_.poll_sector(tick);
        if (poll.cell_read && (poll.cell != s.sector_cell_ || poll.resetpoint_moved)) {
            const int sector = poll.cell;
            s.sector_cell_ = sector;
            if (sector > 0 && !s.sectors_requested_.count(sector)) {
                if (poll.checkpoint_held) {
                    hold_sector(sector, tick + 1 + sector_lead_ticks(), "checkpoint");
                } else {
                    s.sectors_requested_.insert(sector);
                    bmmo::session_event_msg event;
                    event.tick = tick;
                    event.type = bmmo::session::event_type::Sector;
                    event.sector = sector;
                    send_event(event);
                    logf("sector %d at tick %u", sector, tick);
                }
            }
        }
    }

}
