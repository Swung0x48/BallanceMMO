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
// Step E9 adds the frame (session_client.cpp at 0ea2e05), verbatim but for
// the platform calls of design 2.2 (the step's operations and hooks) and the
// configuration (config.rollback, config.correct, config.trace for the
// headless args_): pre_step and post_step (design 1.3's tables, in SC's
// order), recorded_input_at (over own_replica_name), the legacy blend's
// step, and the pacer's slew_step.

#include "session/client_core.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
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
        // Step A3: a correction inside the last exact resync's re-step
        // reaches as deep as that re-step did (window_rewind_reach).
        const uint32_t reach = window_rewind_reach(s.input_rewind_tick_);
        if (s.input_rewind_own_ && s.input_rewind_entities_.empty()
                && !s.own_replay_pacer_.due(current_tick(), s.input_rewind_tick_, reach, own_notices_coming())) {
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
        const bool trafo = s.input_rewind_trafo_;   // step T3: a trafo input among them
        drop_pending_rewind();
        const uint32_t now = current_tick();
        if (static_cast<int32_t>(anchor - now) > 0) return;
        auto input_at = [this](const std::string& name, uint32_t tick, bmmo::session::input_frame& out) {
            return recorded_input_at(name, tick, out);
        };
        const scope_guard keep_cells = platform_.keep_live_cells(*this);
        const std::string reason = own ? "own input substituted at tick " + std::to_string(anchor)
                                       : entity + " input corrected at tick " + std::to_string(anchor);
        if (has_own) s.own_replay_pacer_.ran();
        const uint32_t standard = s.rollback_.thresholds().max_resim_ticks;
        if (s.rollback_.rewind_to(world(), anchor - 1, now, input_at, reason, reach > standard ? reach : 0)) {
            if (reach > standard && now - (anchor - 1) > standard) ++s.resync_window_rewinds_;
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
        // Step T3 (trafo-replication design 4.7): a trafo input applied
        // nowhere - no snapshot can make, reset or blow pieces.
        if (trafo) {
            ++s.trafo_missed_;
            logf("trafo: the input rewind from tick %u was refused at local tick %u: resync (trafo_missed %llu)", anchor, now,
                 static_cast<unsigned long long>(s.trafo_missed_));
            if (!s.resync_pending_) request_resync("a trafo input could not be replayed", true);
        }
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
            return recorded_input_at(name, tick, out);
        };
        const uint32_t anchor = now - depth;
        forced_rewind result{false, platform_.keep_live_cells(*this)};
        // Deeper than max_resim_ticks only up to the resync reach, the depth
        // an exact resync re-steps (step A4: the retail 80-tick re-step
        // timing of resync design 9 Q2); a depth within max_resim_ticks is
        // the rewind it always was.
        const uint32_t reach = depth > s.rollback_.thresholds().max_resim_ticks ? depth : 0;
        if (s.rollback_.rewind_to(world(), anchor, now, input_at, "forced", reach)) {
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
        note_sector_stamp(s.own_id_, sector, due);
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
        const bool walk_open = s.sectors_.walk().seen;
        const auto pass = s.sectors_.tick(tick, platform_.sector_manager_active());
        // The level-walk gate (sector_union.hpp, step S4) opening: the tick
        // every side must log alike (sector-placement design 2.6).
        if (!walk_open && s.sectors_.walk().seen)
            logf("level walk %s at tick %u", s.sectors_.walk_open().timed_out ? "timeout" : "seen", s.sectors_.walk_open().tick);
        if (pass) {
            s.recent_passes_[1] = s.recent_passes_[0];
            s.recent_passes_[0] = {tick, pass.sector};
            s.sector_cell_ = pass.sector;   // the detect block below must not take our own write for a crossing
            platform_.run_sector(pass.sector);
            platform_.record_activation(tick, true);
            log_pass(tick, pass.sector);
            logf("sector +%d at tick %u (player %u, stamped %u%s), running: %s", pass.sector, tick,
                 pass.player, pass.stamped_tick, tick > pass.stamped_tick ? ", held by the pacer" : "",
                 [this] { std::string text; for (int x: s.sectors_.active()) text += std::to_string(x) + " "; return text; }().c_str());
        }
    }

    // ------------------------------------------- the union as rollback state
    // Step B1 (resync design 2.5, 2.7, 2.8).  The union's passes depend on
    // its stamps and on the manager's activity per tick, nothing else, so a
    // stamp that arrives late - a remote Sector event relayed past its tick,
    // an own one the server applied later - is applied where it belongs by
    // rewinding to the tick before it and re-deriving the passes inside the
    // replay from the union record of that tick and the stamp log.  Every
    // world then starts the sector at the tick the server did (I2).

    void session_client_core::log_pass(uint32_t tick, int sector) {
        s.pass_log_[tick] = sector;
        const uint32_t keep = static_cast<uint32_t>(s.rollback_.thresholds().history_ticks) + 128u;
        while (!s.pass_log_.empty() && static_cast<int32_t>(s.pass_log_.begin()->first + keep - tick) < 0)
            s.pass_log_.erase(s.pass_log_.begin());
    }

    void session_client_core::note_sector_stamp(uint32_t player, int sector, uint32_t tick, bool rewrite) {
        if (sector <= 0) return;
        auto [it, fresh] = s.sector_stamps_.try_emplace({player, sector});
        if (fresh) {
            it->second.tick = tick;
            it->second.seq = ++s.sector_stamp_seq_;
        } else if (rewrite) {
            it->second.tick = tick;
        }
    }

    std::vector<bmmo::session::sector_union::stamped> session_client_core::stamp_log() const {
        std::vector<bmmo::session::sector_union::stamped> out;
        out.reserve(s.sector_stamps_.size());
        for (const auto& [key, stamp]: s.sector_stamps_) out.push_back({stamp.tick, key.first, key.second});
        return out;
    }

    bool session_client_core::sector_rewind_reachable(uint32_t tick) const {
        if (tick == 0) return false;
        const uint32_t now = current_tick();
        return s.rollback_.knows_tick(tick - 1) && s.union_records_.count(tick - 1)
            && static_cast<int32_t>(now - (tick - 1)) >= 0
            && now - (tick - 1) <= s.rollback_.thresholds().max_resim_ticks;
    }

    // A MOVED own Sector echo (resync design 2.7): the stamp log takes the
    // server's tick; an own pass that has not run yet is stamped again at
    // it, one that has is moved by the frame's Sector rewind (anchor: the
    // origin).  Past the reach the live pass stands and the client resyncs.
    bool session_client_core::move_own_sector(int sector, uint32_t origin, uint32_t applied) {
        const auto it = s.sector_stamps_.find({s.own_id_, sector});
        if (it == s.sector_stamps_.end() || it->second.tick != origin) return false;
        note_sector_stamp(s.own_id_, sector, applied, true);
        if (static_cast<int32_t>(origin - current_tick()) > 0) {
            s.sectors_.unstamp(s.own_id_, sector);
            s.sectors_.stamp(s.own_id_, sector, applied);
            logf("own sector %d moved %u -> %u by the server before its pass: stamped again at %u", sector, origin, applied,
                 applied);
            return true;
        }
        if (sector_rewind_reachable(origin)) {
            client_core_state::late_sector late;
            late.tick = origin;
            late.player = s.own_id_;
            late.sector = sector;
            late.moved = true;
            late.applied = applied;
            s.sector_late_.push_back(late);
            return true;
        }
        ++s.sector_rewinds_missed_;
        logf("own sector %d moved %u -> %u by the server at local tick %u: outside the rollback history, resync", sector,
             origin, applied, current_tick());
        if (!s.resync_pending_) request_resync("an own Sector moved beyond the reach", true);
        return true;
    }

    // The frame's Sector rewind: one rewind to the lowest anchor of the
    // frame's late set, minus one, with the union re-derived inside the
    // replay (rederive_union_tick).  Before the input replay and the live
    // union pass, after the drain's lifecycle rewinds (which re-issued the
    // recorded passes) - the order a late Physicalize uses.
    void session_client_core::rewind_for_sectors() {
        if (s.sector_late_.empty()) return;
        std::vector<client_core_state::late_sector> late;
        late.swap(s.sector_late_);
        const uint32_t now = current_tick();
        uint32_t anchor_tick = late.front().tick;
        for (const auto& l: late)
            if (static_cast<int32_t>(l.tick - anchor_tick) < 0) anchor_tick = l.tick;
        const uint32_t anchor = anchor_tick - 1;
        bool ok = false;
        auto& rd = s.rederive_;
        if (sector_rewind_reachable(anchor_tick)) {
            rd.active = false;
            rd.live_tick = now;
            // The live passes of the window: replaced by the re-derived ones.
            rd.r.start(s.union_records_.at(anchor), stamp_log(), s.pass_log_, anchor_tick);
            auto input_at = [this](const std::string& name, uint32_t tick, bmmo::session::input_frame& out) {
                return recorded_input_at(name, tick, out);
            };
            std::string reason = "sector";
            for (const auto& l: late)
                reason += " " + std::to_string(l.sector) + (l.moved ? " moved " + std::to_string(l.tick) + "->" + std::to_string(l.applied)
                                                                    : " stamped " + std::to_string(l.tick) + " (late)");
            {
                const scope_guard keep_cells = platform_.keep_live_cells(*this);
                rd.active = true;
                const bool rolled = s.rollback_.rewind_to(world(), anchor, now, input_at, reason);
                rd.active = false;
                // Re-derived through the live tick (a frozen world rewinds
                // nothing and drops the history instead).
                ok = rolled && rd.r.through(now);
                // Adopt mode for the guard's end, only when the replay
                // re-derived the union (resync design 4.6).
                s.last_rewind_rederived_ = ok;
            }
            s.last_rewind_rederived_ = false;
            if (!ok) rd.r.abandon(s.pass_log_);
        }
        if (ok) {
            s.sectors_.restore(rd.r.u().state());
            // The cell as the replay left it is the one the own poll
            // compares with: the replayed pass's write is no crossing of ours.
            int cell = 0;
            if (platform_.sector_cell_adopted(cell)) s.sector_cell_ = cell;
            s.recent_passes_[0] = s.recent_passes_[1] = {};
            for (auto it = s.pass_log_.rbegin(); it != s.pass_log_.rend(); ++it) {
                if (!s.recent_passes_[0].first) s.recent_passes_[0] = *it;
                else if (!s.recent_passes_[1].first) { s.recent_passes_[1] = *it; break; }
            }
            ++s.sector_rewinds_;
            s.sector_rewind_depth_ += now - anchor;
            drop_carried_own_replay(anchor, false);
            for (const auto& l: late) {
                if (l.moved) {
                    logf("own sector %d moved %u -> %u by the server: the own pass moved (rewound to %u)", l.sector, l.tick,
                         l.applied, anchor);
                    journal_.note("sector " + std::to_string(l.sector) + " own stamp moved " + std::to_string(l.tick) + " -> "
                                  + std::to_string(l.applied) + ": rewound to " + std::to_string(anchor));
                } else {
                    logf("remote player %u entered sector %d, stamped for tick %u (late by %u: rewound to %u)", l.player,
                         l.sector, l.tick, now + 1 - l.tick, anchor);
                    journal_.note("sector " + std::to_string(l.sector) + " stamped " + std::to_string(l.tick)
                                  + ": rewound to " + std::to_string(anchor));
                }
            }
            return;
        }
        // Refused (outside the history, deeper than the reach, a frozen
        // world): every stamp of the set live, as before B1, and a resync -
        // its state brings the sector to the server's tick.
        for (const auto& l: late) {
            if (!l.moved) {
                s.sectors_.stamp(l.player, l.sector, l.tick);
                logf("remote player %u entered sector %d, stamped for tick %u (LATE: the server started it already)",
                     l.player, l.sector, l.tick);
            }
        }
        ++s.sector_rewinds_missed_;
        logf("late sector stamp for tick %u is outside the rollback history at local tick %u: stamped live, resync",
             anchor_tick, now);
        if (!s.resync_pending_) request_resync("a late Sector event beyond the reach", true);
    }

    // Before each tick a re-deriving replay steps (core_world_hooks::
    // before_replayed_tick, after the births): the union's tick with the
    // manager's state after the replayed step before, as the live frame's
    // pass (sector_union_pass) and the server's update_sectors take it; its
    // pass recorded in the tick's pass slot, which the host's
    // replay_activation re-issues next, or the live pass of that tick
    // forgotten.  Nothing outside a Sector rewind.
    void session_client_core::rederive_union_tick(uint32_t tick) {
        auto& rd = s.rederive_;
        if (!rd.active) return;
        // The decision is sector_rederivation's (shared with SimTool's
        // --late-sector-check); the platform's slot, the logs and the
        // records are this core's.
        const auto step = rd.r.tick(tick, platform_.sector_manager_active());
        const auto& pass = step.made;
        if (pass) {
            platform_.set_union_pass(tick, pass.sector);
            log_pass(tick, pass.sector);
            s.sector_cell_ = pass.sector;
            logf("sector +%d at tick %u (player %u, stamped %u%s), running: %s(re-derived in a rewind at live tick %u)",
                 pass.sector, tick, pass.player, pass.stamped_tick, tick > pass.stamped_tick ? ", held by the pacer" : "",
                 [&rd] { std::string text; for (int x: rd.r.u().active()) text += std::to_string(x) + " "; return text; }().c_str(),
                 rd.live_tick);
        } else if (step.clear_slot) {
            platform_.set_union_pass(tick, 0);
        }
        if (step.forgotten) {
            ++s.sector_passes_forgotten_;
            logf("sector +%d at tick %u of the live frames is not re-derived there: forgotten", step.forgotten, tick);
        }
        s.union_records_[tick] = rd.r.u().state();
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
        // A pass at M writes CurrentLevel[0][4] = 0 (M..M+3) and 1 (M+4);
        // the module scripts it reset branch on the cell in frame M+5, and
        // take their teardown branch on 0.  Anything else that writes the
        // cell then (the level's checkpoint chain did, at C+2 of a crossing
        // three ticks after the pass) leaves this world without the
        // sector's bodies.  A pass at M+5 writes its own 0 in that frame
        // and is not counted.  Ticks are the behaviour frames here: the
        // live frame steps exactly one tick (a frame that does not step
        // ends the session before this poll), and a replay never polls.
        if (poll.phase_read && poll.phase == 0)
            for (const auto& [m, sector]: s.recent_passes_) {
                if (!m || tick != m + 5 || s.recent_passes_[0].first == tick) continue;
                ++s.phase_misses_;
                logf("sector +%d at tick %u: its modules' first frame (tick %u) found CurrentLevel[0][4] = 0, "
                     "so they took their teardown branch here: this world lacks that pass's bodies (%llu so far)",
                     sector, m, tick, static_cast<unsigned long long>(s.phase_misses_));
            }
        // Step B2SX: a private cell (game/sector_cell_split.hpp) is the
        // chain's alone - no pass writes it, so it is compared with the last
        // poll, never with a pass's sector.
        int& seen = poll.cell_private ? s.chain_cell_ : s.sector_cell_;
        if (poll.cell_read && (poll.cell != seen || poll.resetpoint_moved)) {
            const int sector = poll.cell;
            seen = sector;
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

    // ---------------------------------------------------------- the frame
    // The input a navigated ball had at `tick`, for a re-simulation.  The
    // own ball: only an exact hit.  A remote: the relayed frame, or for a
    // hole between relayed frames the nearest earlier one (at most a
    // window old, where the recorded prediction is a round trip old), and
    // past the newest relayed frame that frame (hold-last, step RC1).
    // The own ball is told by the name its replica last had
    // (own_replica_name), whether or not the replica still exists.
    bool session_client_core::recorded_input_at(const std::string& entity, uint32_t tick,
                                                bmmo::session::input_frame& out) const {
        const std::map<uint32_t, bmmo::session::input_frame>* inputs = nullptr;
        if (entity == platform_.own_replica_name()) inputs = &s.own_inputs_;
        else
            for (const auto& [id, remote]: s.remotes_)
                if (remote.entity == entity) inputs = &remote.inputs;
        if (!inputs) return false;
        if (inputs == &s.own_inputs_) {
            auto it = inputs->find(tick);
            if (it == inputs->end()) return false;
            out = it->second;
            return true;
        }
        // A remote: the relayed frame, or for a hole the window did not
        // cover the nearest earlier one (session/input_relay.hpp); for the
        // tail the relay has not reached, its newest frame - the live
        // predictor's (live_drive), step RC1 - so a replay re-predicts the
        // tail instead of keeping the prediction recorded before the change
        // arrived; none before the first relayed frame, where the recorded
        // prediction stands.
        const auto* frame = bmmo::session::relay_frame_or_newest(*inputs, tick);
        if (!frame) return false;
        out = *frame;
        return true;
    }

    // Design 1.3 pre_step: T = next_tick(), the tick the engine is about
    // to simulate - the one current_tick() returns once
    // frames_since_anchor_ is advanced in post_step, and so the tick
    // report_physicalize sends for this very frame.  Where the server
    // applies them: after this tick's inputs and before its physics step
    // (session_runner::step).
    void session_client_core::pre_step() {
        frame_.prepared = true;
        frame_.f = s.frames_since_anchor_ + 1;
        const uint32_t frame_tick = s.tick_base_ + static_cast<uint32_t>(std::max<int64_t>(frame_.f - 1, 0));
        frame_.tick = frame_tick;
        // A held --start-sector: on the first frame the session is assigned
        // (tick base and lead known), stamp it `lead` ticks ahead like any
        // other sector this client enters.
        hold_pending_sector(frame_tick);
        // Step T3: the hold's own trafo reports that waited for the
        // assignment, likewise.
        hold_pending_trafo(frame_tick);
        // This tick's events first - a relayed Sector event stamped for it
        // among them - then the union's pass, then the step: the order the
        // server's session_runner::step keeps.
        drain_due_events(frame_tick);
        // Step A3: a mirror an exact resync made from its row at S whose
        // death the old numbering applied past the re-step (Delta > 0) dies
        // again at its tick here, live (the replay drains the others).
        drain_pending_deaths(frame_tick);
        // Our own events' echoes: a birth the server made later or never
        // is moved inside the history now, with the rewinds below still
        // to come in this frame.
        process_own_echoes();
        // The sector resets stay neutralized (the level-reset one once the
        // sector is up: this client has resolved a mechanism of its own).
        platform_.script_guards(frame_tick, s.mechanism_tracking_.resolved() > 0);
        // Step A3: a resync state waiting for its bodies (resync design
        // 2.6.4 step 2), tried again now that the drain made what was due -
        // before this frame's own rewinds, which then run on the re-stepped
        // history.
        if (s.have_resync_state_) try_apply_resync("retry");
        // Step B1: the Sector stamps that arrived late inside the reach, and
        // the own ones the server moved, re-derived from the tick before the
        // lowest of them - after the drain's lifecycle rewinds, before the
        // input replay and the union's live pass below.
        rewind_for_sectors();
        // After the late events, so a birth and an input correction in the
        // same frame leave the world replayed from the earlier of the two - and
        // BEFORE the union's pass below: the pass sets next-frame flags on
        // Gameplay_SectorManager, which the restore (the manager travels
        // with the records now) would write back to the anchor's and lose,
        // and the replay ends one tick short of re-issuing it.  The retail
        // client has this order already.
        replay_corrected_inputs();
        retry_owed_resync();
        // A mirror a restore took out for a remake that no replay made
        // again (step LB): made now, not left without a body.
        make_unmade_remakes();
        if (platform_.hooks.after_rewinds) platform_.hooks.after_rewinds(frame_tick);
        // The body changes stamped for this very tick, the own beam among
        // them: after the rewinds above, so none of them undoes one, and
        // before the union's pass and the step, where the server applies
        // them, in its order.
        apply_held_lifecycle(frame_tick);
        // Step T3 (trafo-replication design 2.5): the tick's trafo inputs,
        // before the union's pass, as the server applies its due events
        // before update_sectors().
        apply_trafo_inputs(frame_tick, false);
        // The sector union (session/sector_union.hpp), the rule the server's
        // update_sectors() runs: one count of the pacer per step, before
        // it, from the manager's state after the previous step, and one
        // pass per idle manager for the lowest sector owed.
        sector_union_pass(frame_tick);
        // Step T3: the trafo frame - every selected set's scripts by player
        // id - after the pass and before the behaviour pass and the step,
        // where the server's tick() runs it.
        if (s.trafo_active_) platform_.trafo_frame(frame_tick);
        // A rewind above replayed up to the previous tick and left every
        // mirror's navigation input at the frame it last fed there; this
        // tick's step is driven by the frame recorded as applied for it
        // (remote.applied, chosen at the end of the previous frame), or
        // the record and the step disagree and the relayed frame for this
        // tick compares equal to a prediction the step never ran on.
        for (auto& [id, remote]: s.remotes_) {
            if (!remote.physicalized || !remote.navigation) continue;
            // drive_from_newest_relay (R1): chosen now, from the relay the
            // messages since post_step left (the record of T stores it).
            if (config.drive_from_newest_relay) remote.applied = live_drive(remote, frame_tick);
            std::string drive_error;
            if (!platform_.drive(remote.entity, remote.applied, drive_error))
                logf("remote navigation input %s: %s", remote.entity.c_str(), drive_error.c_str());
        }
        // A tick a SessionLead forced (late-player design 2.4): the server
        // applies `held` here and drops our frame as stale, so the own
        // ball is driven with it the way a replay drives it - polling off,
        // the frame through nav_input - and our keys wait until E.  After
        // the rewinds above, whose replays switch the polling back on.
        frame_.forced_frame = client_core_state::forced_own{};
        frame_.forced = take_forced_own(frame_tick, frame_.forced_frame);
        frame_.forced_entity.clear();
        if (frame_.forced) {
            bool ok = true;
            std::string force_error;
            frame_.forced_entity = platform_.force_own(frame_.forced_frame.frame, ok, force_error);
            if (!frame_.forced_entity.empty() && !ok)
                if (s.lead_force_logs_++ < 8)   // the ball may be unphysicalized for the whole range
                    logf("lead: forced own input at tick %u: %s", frame_tick, force_error.c_str());
        }
        // The own ball's Physicalize is captured inside this tick's step,
        // before the PSIs (the headless PreSimulate capture).  Armed here,
        // after the rewinds above: a replayed step runs the PreSimulate
        // pass too, and only the live tick may capture.
        if (platform_.arm_own_capture) platform_.arm_own_capture(frame_tick);
    }

    // Design 1.3 post_step: the world has stepped T.
    bool session_client_core::post_step(bool stepped) {
        // A frame no pre_step prepared (R1): frame_ holds the previous
        // frame's number and tick, and taking it would count that frame
        // twice.  The headless frame always pairs the two; a retail anchor
        // path that skipped the pre_step must not reach the record.
        if (!frame_.prepared) {
            logf("post_step refused at tick %u: no pre_step prepared this frame", current_tick());
            return false;
        }
        frame_.prepared = false;
        const uint32_t frame_tick = frame_.tick;
        if (!frame_.forced_entity.empty()) {
            std::string force_error;
            if (!platform_.unforce_own(frame_.forced_entity, force_error))
                if (s.lead_force_logs_++ < 8)
                    logf("lead: own polling after the forced tick %u: %s", frame_tick, force_error.c_str());
            frame_.forced_entity.clear();
        }
        if (!stepped) return false;
        s.frames_since_anchor_ = frame_.f;
        // An activation of Gameplay_SectorManager this frame's scripts asked
        // for (Init Ingame's Execute Script at the level start, Event_handler):
        // pending as next-frame flags now, consumed by the next frame.  A
        // pass of ours never shows here - its flags were consumed by this
        // frame.  Recorded for the replay like a pass (plan doc 10).
        platform_.record_pending_activation(frame_tick + 1);
        // Step RO1: the order this live frame ran the replay's scripts in,
        // for the replays that re-run it (host-local).
        if (platform_.record_script_order) platform_.record_script_order(frame_tick);
        const uint32_t tick = current_tick();

        if (platform_.discover_navigation(tick)) {
            for (auto& [id, remote]: s.remotes_) {
                const bool navigated = remote.navigation;
                attach_remote_navigation(id, remote);
                // The ticks before this one ran it undriven: a replay
                // across them must not start driving it.
                if (!navigated && remote.navigation && static_cast<int32_t>(tick - remote.driven_from) > 0)
                    remote.driven_from = tick;
            }
        }

        // Own ball state after this tick.
        const own_ball_view own = platform_.own_ball();
        const std::string& ball_name = own.entity;
        const int ball_type = own.ball_type;
        const bool physicalized = own.physicalized;
        if (!ball_name.empty()) s.own_ball_entities_.insert(ball_name);   // is_ball (step P)
        platform_.own_body_upkeep(own);
        if (physicalized) {
            bmmo::session::ball_pose pose;
            pose.tick = tick;
            for (int k = 0; k < 3; ++k) { pose.position[k] = own.state.position[k]; pose.linear[k] = own.state.linear[k]; pose.angular[k] = own.state.angular[k]; }
            for (int k = 0; k < 4; ++k) pose.rotation[k] = own.state.rotation[k];
            s.corrector_.record(pose);
        }
        s.own_physicalized_ = physicalized;
        s.own_body_entity_ = physicalized ? ball_name : std::string();

        // Mechanism states after this tick (the legacy blending path;
        // with the rollback engine on, the engine keeps its own records).
        for (const auto& name: s.mechanism_tracking_.tracked_entities()) {
            bmmo_physics_body_state local{};
            if (!platform_.body_state(name, local)) continue;
            bmmo::session::ball_pose pose;
            pose.tick = tick;
            for (int k = 0; k < 3; ++k) { pose.position[k] = local.position[k]; pose.linear[k] = local.linear[k]; pose.angular[k] = local.angular[k]; }
            for (int k = 0; k < 4; ++k) pose.rotation[k] = local.rotation[k];
            s.mechanism_correctors_[name].record(pose);
        }
        for (auto& [id, remote]: s.remotes_) {
            if (!remote.physicalized || !remote.navigation) continue;
            bmmo_physics_body_state local{};
            if (!platform_.body_state(remote.entity, local)) continue;
            bmmo::session::ball_pose pose;
            pose.tick = tick;
            for (int k = 0; k < 3; ++k) { pose.position[k] = local.position[k]; pose.linear[k] = local.linear[k]; pose.angular[k] = local.angular[k]; }
            for (int k = 0; k < 4; ++k) pose.rotation[k] = local.rotation[k];
            remote.corrector.record(pose);
        }

        // Lifecycle events from the bridge's object log, read before the
        // rollback record below: a body created or deleted during this
        // frame is already in the world that record describes, so the new
        // lifetime generation belongs on THAT record (design 9.25).  The
        // birth recipes are the host's (its local birth log).
        std::set<std::string> revived_reported;
        platform_.drain_body_events(tick, [&](const body_event& event) {
            const std::string& name = event.name;
            if (event.what == body_event::kind::created || event.what == body_event::kind::deleted) {
                // The body behind a tracked name changed: a new lifetime for
                // it, and the mechanism registry has to ask the world again.
                if (!name.empty()) {
                    ++s.generations_[name];
                    s.mechanism_tracking_.mark_dirty();
                }
            }
            if (event.what == body_event::kind::created && !ball_name.empty() && name == ball_name) {
                // Sent and recorded by the core (E7), from the capture.
                if (caps_.own_lifecycle == own_lifecycle_source::event_log) report_physicalize(ball_name, tick);
                return true;
            } else if (event.what == body_event::kind::deleted && !ball_name.empty() && name == ball_name) {
                if (caps_.own_lifecycle == own_lifecycle_source::event_log) report_unphysicalize(tick);
                return true;
            } else if (event.what == body_event::kind::wakeup && !name.empty() && platform_.reportable_wakeup(name, ball_name)
                       && !revived_reported.count(name)) {
                revived_reported.insert(name);
                // Step A5: a late joiner's wakeups before its apply are its
                // catch-up passes' (the level's own walk, the owed passes,
                // physicalizing modules the server's world has had settled
                // for long): reported, the server would wake those bodies at
                // ticks where nothing in the shared world woke them, and no
                // other world would (a5_join_1: P_Ball_Paper_MF and the
                // P_Modul_29 plates 26 mm off in every member from 1650).
                if (s.resync_join_ && s.resync_pending_) {
                    if (s.join_wakes_withheld_++ == 0)
                        logf("late join: wakeups before the apply are not reported (%s at tick %u first)", name.c_str(),
                             tick);
                    return false;
                }
                bmmo::session_event_msg revived;
                revived.tick = tick;
                revived.type = bmmo::session::event_type::BodyRevived;
                revived.name = name;
                send_event(revived);
            }
            return false;
        });
        // The own body changes a host_hooks host reported in the behaviour
        // pass, committed right after the drain's generation bump (design
        // 1.3 step 19).
        if (caps_.own_lifecycle == own_lifecycle_source::host_hooks) commit_own_reports();
        // Which mechanism rows are about one of our bodies, before the
        // record: the tracked set of a tick has to name bodies it contains.
        if (s.mechanism_tracking_.due(tick))
            s.mechanism_tracking_.resolve(tick, [this](const char* entity, bmmo_physics_body_state& out) {
                return platform_.body_state(entity, out);
            });

        // Input for this tick.
        const input_observation seen = platform_.observe_input();
        bmmo::session::input_frame input{};
        input.keys = seen.keys;
        const float (*basis)[3] = s.previous_cam_valid_ ? s.previous_cam_ : seen.cam;
        for (int k = 0; k < 3; ++k) { input.cam_right[k] = basis[0][k]; input.cam_up[k] = basis[1][k]; input.cam_dir[k] = basis[2][k]; }
        if (seen.cam_valid) { std::memcpy(s.previous_cam_, seen.cam, sizeof(seen.cam)); s.previous_cam_valid_ = true; }
        input.ball_type = static_cast<uint8_t>(ball_type < 0 ? 0 : ball_type);
        const bool nav_active = seen.nav_active;
        input.flags = static_cast<uint8_t>((physicalized ? bmmo::session::INPUT_FLAG_PHYSICALIZED : 0)
                    | (seen.paused ? bmmo::session::INPUT_FLAG_PAUSED : 0)
                    | (nav_active ? bmmo::session::INPUT_FLAG_NAV_ACTIVE : 0));
        // A forced tick plays the server's frame (design 2.4): recorded,
        // journaled SUBSTITUTED and sent as such - the server drops it as
        // stale, and the record holds what the server applied, so its
        // notice, when it comes, changes nothing.
        const bool forced = frame_.forced;
        if (forced) play_forced_own(tick, frame_.forced_frame, input);
        if (config.trace && (input.keys != s.last_keys_ || nav_active != s.last_nav_active_))
            logf("input edge at tick %u keys=%u flags=%u", tick, input.keys, input.flags);
        s.last_keys_ = input.keys;
        s.last_nav_active_ = nav_active;
        platform_.own_camera_rows(basis);
        s.input_history_.emplace_back(tick, input);
        // The frame is finished: journal it before it goes on the wire, so
        // the box holds what this client played even when the send is lost.
        if (journal_.journalling())
            journal_.writer().input(tick, s.own_id_, input,
                           forced ? bmmo::session::JOURNAL_INPUT_SUBSTITUTED : bmmo::session::JOURNAL_INPUT_FRESH);
        if (config.rollback) {
            s.own_inputs_[tick] = input;
            while (s.own_inputs_.size() > 128) s.own_inputs_.erase(s.own_inputs_.begin());
            bmmo::session::rollback_tracked tracked;
            std::map<std::string, bmmo::session::input_frame> applied;
            const std::string own_entity = platform_.own_driven_entity();
            if (physicalized && !own_entity.empty()) {
                tracked.own_entity = own_entity;
                tracked.own_polls = true;
                applied[own_entity] = input;
            }
            for (const auto& [id, remote]: s.remotes_)
                if (remote.physicalized && remote.navigation) {
                    tracked.remote_entities.push_back(remote.entity);
                    applied[remote.entity] = remote.applied;
                }
            // Design 9.25: only the mechanism names this client has a body
            // for - a tracked name with no body is dropped by every
            // capture, which used to change the tracked set every tick.
            // Every movable body this client has, not only the ones the
            // server has named and the registry resolved.
            //
            // A body missing from the tracked set is not recorded and so
            // not restored - but the replay's step() still steps it, so
            // every rollback advances it by the replay's depth.  Measured
            // solo with forced rewinds: a P_Modul_29 plate, which hangs
            // from physics hinges and swings continuously, came out of a
            // depth-12 rewind at exactly the pose the un-rewound run had
            // 12 ticks LATER - not displaced, phase-shifted by the depth.
            // `mechanisms=22/12` had been in every run all along: ten of
            // the client's own bodies were being stepped by every replay
            // and restored by none.
            //
            // So the tracked set is the client's own world, which is what
            // the replay has to put back. A body the snapshot says nothing
            // about is restored from our own record, which is right.
            // Every movable body but the balls (step P: is_ball, not the
            // "Ball_" prefix the trafo explosion pieces share).
            tracked.mechanisms = movable_mechanisms();
            tracked.generations = s.generations_;
            platform_.note_script_resets(tick);
            // Captured once, by the amend at the end of this post_step (step
            // T3's fix-up: the amend re-captured the whole record anyway, and
            // nothing in between reads its world) - unless the host reads the
            // record before the amend (after_record: the headless client's
            // --dump-savestate / --compare-scripts).
            s.rollback_.record(world(), tick, tracked, applied, static_cast<bool>(platform_.hooks.after_record));
            // Step B1: the union after this tick's tick(), beside its record.
            while (!s.union_records_.empty() && static_cast<int32_t>(s.union_records_.rbegin()->first - tick) >= 0)
                s.union_records_.erase(std::prev(s.union_records_.end()));
            s.union_records_[tick] = s.sectors_.state();
            {
                const uint32_t keep = static_cast<uint32_t>(s.rollback_.thresholds().history_ticks);
                while (!s.union_records_.empty()
                       && static_cast<int32_t>(s.union_records_.begin()->first + keep - tick) < 0)
                    s.union_records_.erase(s.union_records_.begin());
            }
            if (platform_.hooks.after_record) platform_.hooks.after_record(tick);
        }
        // A notice that named this tick before it was simulated: the
        // record just made holds what we played, the server applied
        // something else (late-player design 1.3).
        take_held_ahead(tick);
        if (s.assigned_) {
            while (s.input_history_.size() > bmmo::session::MAX_INPUT_FRAMES) s.input_history_.pop_front();
            bmmo::session_input_msg msg;
            msg.session = s.session_;
            msg.first_tick = s.input_history_.front().first;
            for (const auto& [t, fr]: s.input_history_) msg.frames.push_back(fr);
            remember_sent(msg);
            msg.serialize();
            platform_.send(msg.raw.str(), send_mode::unreliable_no_delay);
            ++s.inputs_sent_;
            // The unreliable copies of our recent lifecycle events, one per
            // frame (send_event; protocol 6).
            s.own_event_repeats_.send_due([this](uint32_t, const std::string& bytes) {
                platform_.send(bytes, send_mode::unreliable_no_delay);
            });
        } else if (s.input_history_.size() > 660) {
            s.input_history_.pop_front();
        }

        // Sector: the own sector poll (a crossing stamped ahead with the
        // checkpoint chain held, else reported after the fact).
        poll_own_sector(tick);
        // Step T3 (trafo-replication design 2.3, 2.4): the hold of the own
        // trafo activations beside it, then retirement and the log's
        // pruning - after the live frame, outside any replay.
        poll_own_trafo(tick);
        retire_trafo(tick);

        // Continue running blends.
        if (config.correct) {
            if (physicalized) apply_blend(ball_name, s.corrector_);
            for (auto& [name, corrector]: s.mechanism_correctors_) apply_blend(name, corrector);
            for (auto& [id, remote]: s.remotes_)
                if (remote.physicalized && remote.navigation) apply_blend(remote.entity, remote.corrector);
        }
        // Next tick's input of every predicted remote ball: the frame the
        // server applied at that tick when the relay has it already (a
        // client running behind the relay), else the last relayed frame.
        for (auto& [id, remote]: s.remotes_) {
            if (!remote.physicalized || !remote.navigation) continue;
            remote.applied = live_drive(remote, tick + 1);
            std::string error;
            if (!platform_.drive(remote.entity, remote.applied, error))
                logf("remote navigation input %s: %s", remote.entity.c_str(), error.c_str());
        }
        // A requested beam (the headless --beam, the retail verb), stamped
        // tick + 1.
        apply_requested_beam(tick);
        // The record for this tick was captured before the corrections
        // above, so a rollback to it would restore the divergent pose and
        // lose the correction on the first re-simulated tick.  Re-capture
        // it now, exactly like the retail mod (design 9.25 item 4 of
        // findings/A5 section 3.1: the headless client never did).
        if (config.rollback && !s.rollback_.amend_record(world(), tick)) {
            if (s.amend_failures_++ == 0)
                logf("amend_record found no history entry for tick %u (the tick was dropped)", tick);
        }
        // The host's traces of the frame (step 30a), then the window: line
        // this tick closes, once assigned (step 31; E10).  The host's
        // counters are read only on a frame that closes a window.
        if (platform_.hooks.after_amend) platform_.hooks.after_amend(tick);
        if (s.assigned_ && window_due(tick)) {
            const std::string text = window_text(tick, host_values());
            if (!text.empty() && platform_.hooks.window) platform_.hooks.window(text);
        }
        // The box's own view of this frame, last: the fingerprint of the
        // world the corrections above have finished with, and the bodies
        // when a checkpoint is due.
        if (journal_.journalling()) {
            bmmo::physics::world_hash hash;
            if (platform_.world_hash(hash))
                journal_.writer().tick(tick, hash, journal_.ms());
            if (journal_.checkpoint_due(tick)) journal_.local_checkpoint(tick, platform_.own_ball().entity);
            if (platform_.hooks.after_journal) platform_.hooks.after_journal(tick);
        }
        return true;
    }

    // One step of a legacy blend in flight (the ladder's frame half): the
    // body moved by the step's share of the correction, awake.
    void session_client_core::apply_blend(const std::string& name, bmmo::session::body_corrector& corrector) {
        if (!corrector.blending()) return;
        const auto step = corrector.next_blend();
        if (step.action != bmmo::session::correction_step::kind::blend) return;
        std::string error;
        bmmo_physics_body_state current{};
        if (!platform_.body_state(name, current)) return;
        double position[3];
        float linear[3];
        for (int k = 0; k < 3; ++k) {
            position[k] = current.position[k] + step.delta_position[k];
            linear[k] = current.linear[k] + step.delta_linear[k];
        }
        if (!platform_.write_body(name, position, current.rotation, linear, current.angular, true, error))
            logf("blend %s: %s", name.c_str(), error.c_str());
    }

    // ----------------------------------------------------------- pacing
    // One step of the slew: the tick periods since the previous step and
    // how far the pacer is behind go to lead_slew::advance, and the host
    // moves its origin by what it returns (+1: a tick earlier, the clock
    // runs ahead).  The whole delta moved: acknowledged.
    int session_client_core::slew_step(std::chrono::steady_clock::time_point now, double tick_rate, double behind_ticks) {
        const double wall = std::chrono::duration<double>(now - s.lead_slew_clock_).count() * tick_rate;
        s.lead_slew_clock_ = now;
        const int moved = s.lead_slew_.advance(wall, behind_ticks);
        if (s.lead_slew_.done()) finish_lead_slew("slew complete");
        return moved;
    }

}
