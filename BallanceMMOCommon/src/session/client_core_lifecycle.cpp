// The shared session-client core (docs/session-client-core-design.md 1.1):
// the lifecycle.  Step E6 moves the remote lifecycle out of the headless
// client (session_client.cpp at 8d933ad): SessionEvent's intake, the drain at
// the due tick (drain_due_events) and the held body changes
// (apply_held_lifecycle), apply_event, make_remote_birth,
// rewind_for_lifecycle, the births deferred into a replay
// (drain_pending_births), live_drive, and the core half of the mirrors'
// birth and death (physicalize_remote, unphysicalize_remote,
// attach_remote_navigation), verbatim but for the platform calls
// (mirror_entity, make_mirror, unmake_mirror, detach_mirror_navigation,
// attach_mirror_navigation, mute_event_log, bind_world, keep_live_cells), the
// configuration (rollback) and the legacy back-calls into the own lives,
// which move at E7 (live_own_lives, own_lives_due, apply_own_lives) and
// recorded_input_at (E9).  The own lives and echoes join this file at E7.

#include "session/client_core.hpp"
#include "session/bridge_recipe.hpp"
#include "session/spawn_impulse.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace bmmo::session {

    using pending_birth = client_core_state::pending_birth;
    using remote_ball = client_core_state::remote_ball;
    using applied_death = client_core_state::applied_death;

    // ------------------------------------------------- the event log's mute
    // The event log muted for a scope, nesting: a trafo inside a birth made
    // in a replay mutes inside the birth's mute, and must not end it.
    // (The bridge keeps one flag; the script replay's and destroy_body's
    // mutes never overlap these.)
    session_client_core::event_log_mute::event_log_mute(session_client_core& core)
        : owner(core), was(core.s.event_log_muted_) {
        owner.s.event_log_muted_ = true;
        owner.platform_.mute_event_log(true);
    }

    session_client_core::event_log_mute::~event_log_mute() {
        owner.s.event_log_muted_ = was;
        owner.platform_.mute_event_log(was);
    }

    // ------------------------------------------------------------ intake
    void session_client_core::on_session_event(bmmo::session_event_msg&& msg) {
        ++s.events_received_;
        if (msg.session != s.session_ || s.phase_ != client_core_state::phase::running) return;
        // The server sends every event several times (protocol 6): the
        // first arrival is the event, the rest are nothing.  Judged
        // only once this client runs: an event relayed while it loads
        // is dropped, and the server's catch-up after SessionReady
        // sends the same bytes again - those must not read as copies.
        if (!s.event_seen_.first(bmmo::session::lifecycle_key_of(msg, msg.player))) {
            ++s.event_dups_;
            return;
        }
        // Step T3 (trafo-replication design 2.4): a member first seen in a
        // message gets its piece set now, outside any replay.
        if (s.trafo_active_ && msg.player != 0 && msg.player != s.own_id_) ensure_trafo_set(msg.player, "its first event");
        // A Trafo event (protocol 11) is an input of the trafo log, not a
        // body change of the drain: our own (not server-stamped) is an echo
        // for the frame; another member's, and a server-stamped reset of
        // our own set, are logged at their applied tick now (design 4.6).
        if (msg.type == bmmo::session::event_type::Trafo) {
            if (msg.player == s.own_id_ && s.own_id_ != 0 && !(msg.flags & bmmo::session::TRAFO_SERVER)) {
                s.own_echoes_.push_back(std::move(msg));
                return;
            }
            on_trafo_event(msg);
            return;
        }
        // Protocol 9: the server relays every event to every member,
        // its owner included.  Our own is an echo: applied at its
        // origin it is a plain ack; MOVED, SUPERSEDED or REJECTED it
        // says the server applied it elsewhere or not at all, and an
        // own birth is then moved inside our history (late-player
        // design 3.3).  Handled in the frame, before its other
        // rewinds (process_own_echoes), not here between two frames.
        if (msg.player == s.own_id_ && s.own_id_ != 0) {
            s.own_echoes_.push_back(std::move(msg));
            return;
        }
        // Queued, not applied here: an event applied on arrival lands
        // at an arbitrary point relative to this client's physics step
        // and in network arrival order, while the server applies every
        // due event before its step in the members' join order.  The
        // drain in frame() matches the server; see drain_due_events.
        s.pending_events_.push_back(std::move(msg));
    }

    // ------------------------------------------------------------- drain
    // Every queued event stamped at or before `tick`, in the one order
    // the server also uses.  Called from frame() immediately before the
    // engine steps, which is where session_runner::step applies them:
    //
    //   * stamp <= tick, so an event that arrived after its own tick is
    //     applied now rather than dropped - the server does the same, and
    //     lateness is never a reason to drop a lifecycle event.
    //   * stable_sort by join order, because two balls created in one tick
    //     are created in that order and the order changes the world.  An
    //     unknown player sorts after every known one, as on the server.
    //
    // The event keeps its own stamped tick: the spawn impulse direction is
    // derived from that field (spawn_direction_index), and the server
    // derives it from the stamp too, so the two must not be re-stamped
    // with their differing consumption ticks.
    void session_client_core::drain_due_events(uint32_t tick) {
        if (s.pending_events_.empty()) return;
        std::vector<bmmo::session_event_msg> due;
        auto keep = s.pending_events_.begin();
        for (auto it = s.pending_events_.begin(); it != s.pending_events_.end(); ++it) {
            if (static_cast<int32_t>(it->tick - tick) <= 0) due.push_back(std::move(*it));
            else *keep++ = std::move(*it);
        }
        s.pending_events_.erase(keep, s.pending_events_.end());
        if (due.empty()) return;
        // (join, applied, origin, rank): the server applies each event at
        // its applied tick, and one player's events of one tick by origin
        // (protocol 9, late-player design 3.1).
        std::stable_sort(due.begin(), due.end(),
                         [this](const bmmo::session_event_msg& a, const bmmo::session_event_msg& b) {
                             const uint32_t ja = join_rank(a.player), jb = join_rank(b.player);
                             if (ja != jb) return ja < jb;
                             if (a.tick != b.tick) return static_cast<int32_t>(a.tick - b.tick) < 0;
                             return bmmo::session::lifecycle_apply_before(ja, a.origin_tick, a.type,
                                                                          jb, b.origin_tick, b.type);
                         });
        // A body change stamped for this very tick (on time) is held until
        // this frame's rewinds have run (apply_held_lifecycle): made now,
        // a rewind anchored before the stamp would restore the body's old
        // life onto the new core - no record carries the new generation
        // yet, so the anchor's record still counts as the same life - and
        // replay it to the tick before the stamp, undoing the change for
        // good, the way the own beam used to be undone.  Not late, it
        // asks for no rewind of its own, so holding it moves nothing else.
        for (auto& event: due) {
            if (event.tick == tick && (event.type == bmmo::session::event_type::Physicalize
                                       || event.type == bmmo::session::event_type::Unphysicalize)) {
                s.held_lifecycle_.push_back(std::move(event));   // the message is move-only
                continue;
            }
            apply_event(event);
        }
    }

    // The body changes held by drain_due_events, after this frame's rewinds
    // and before the union's pass and the step - where the server applies
    // them - in the server's order (they were sorted by join order): our
    // own changes due at this tick (a beam, a moved birth) go in at our
    // place.
    void session_client_core::apply_held_lifecycle(uint32_t frame_tick) {
        std::vector<bmmo::session_event_msg> held;
        held.swap(s.held_lifecycle_);
        if (!held.empty()) {
            const uint32_t own_rank = join_rank(s.own_id_);
            bool own_placed = false;
            for (const auto& event: held) {
                if (!own_placed && join_rank(event.player) > own_rank) {
                    live_own_lives(frame_tick);
                    own_placed = true;
                }
                apply_event(event);
            }
        }
        live_own_lives(frame_tick);   // applies once; afterwards only retires
    }

    // A member's place in the server's order of a tick's events: its join
    // order, an unknown player after every known one.
    uint32_t session_client_core::join_rank(uint32_t player) const {
        for (const auto& p: s.players_) if (p.id == player) return static_cast<uint32_t>(p.join_order);
        return 256u;
    }

    // ---------------------------------------------------------- rewinds
    // A ball enters or leaves the world at its owner's tick and is relayed,
    // so this client hears about it input_delay + a trip later.  Acting on
    // it at the tick we have reached would leave our body set disagreeing
    // with the owner's for that whole window, and with the snapshot path
    // demoted nothing would repair it: the clients' own per-tick hashes matched every tick before
    // the first ball and none after
    // (docs/lockstep-without-snapshots.md).
    //
    // So put the world back to the tick before the stamp and replay forward
    // with the body set we should have had from there.  The owner's change
    // took effect before that tick's step, so the anchor is stamp - 1: a
    // newborn is stepped at the stamp tick exactly as its owner stepped it,
    // and a body that went away is simply absent from the replay, which is
    // also what its owner had.  (A destroyed body cannot be restored by the
    // engine, and does not need to be - the replay runs without it.)
    //
    // A stamp older than the rollback history has nothing to rewind to, and
    // then the snapshot path takes over - which is what it is there for.
    void session_client_core::rewind_for_lifecycle(const bmmo::session_event_msg& event) {
        rewind_for_lifecycle(event.player, event.tick);
    }
    void session_client_core::rewind_for_lifecycle(uint32_t player, uint32_t event_tick) {
        // lifecycle_rewind off (R1, retail until R3): the change stays where
        // the drain made it, at the live tick.
        if (!config.rollback || !config.lifecycle_rewind || event_tick == 0) return;
        const uint32_t now = current_tick();
        if (static_cast<int32_t>(event_tick - now) > 0) return;   // not late: nothing to replay
        // Step A3 (resync design 2.6.2): a late change before the synthetic
        // anchor of a pending exact resync - the ticks (R, base - 1] the
        // stall never ran, drained at base - is covered by the server's
        // state of base - 1: applied live, no rewind, no miss.
        if (s.resync_pending_ && s.resync_exact_ && static_cast<int32_t>(event_tick - s.resync_base_) < 0) {
            logf("lifecycle event of %u stamped tick %u precedes the resync's anchor %u: the server's state covers it", player,
                 event_tick, s.resync_base_ - 1);
            return;
        }
        auto input_at = [this](const std::string& entity, uint32_t tick, bmmo::session::input_frame& out) {
            return recorded_input_at(entity, tick, out);
        };
        const std::string reason = "ball of " + std::to_string(player) + " born at tick "
                                 + std::to_string(event_tick);
        const scope_guard keep_cells = platform_.keep_live_cells(*this);
        if (s.rollback_.rewind_to(world(), event_tick - 1, now, input_at, reason)) {
            ++s.life_rewinds_;
            s.life_rewind_depth_ += now - (event_tick - 1);
            drop_carried_own_replay(event_tick - 1, false);
            return;
        }
        ++s.life_rewinds_missed_;
        logf("lifecycle event of %u stamped tick %u is outside the rollback history at local tick %u: "
             "the body set disagrees until a snapshot re-anchors it", player, event_tick, now);
    }

    // ------------------------------------------------------------ events
    // The remote's mirror goes (an Unphysicalize applied); false when
    // there is no such remote.
    bool session_client_core::unphysicalize_remote(uint32_t player, uint32_t tick) {
        auto it = s.remotes_.find(player);
        if (it == s.remotes_.end()) return false;
        if (it->second.navigation) platform_.detach_mirror_navigation(it->second.entity);
        it->second.navigation = false;
        if (it->second.physicalized) {
            ++s.generations_[it->second.entity];   // the body goes: a new lifetime
            unphysicalize_mirror(it->second.entity);
        }
        it->second.physicalized = false;
        logf("remote player %u unphysicalized at tick %u", player, tick);
        return true;
    }

    // A remote's birth.  One stamped in the past, with its tick still in
    // the rollback history, is not made here: the world is rewound to the
    // tick before it and the body is made inside the replay, at its tick,
    // in the world as it stood then (pre_step below).  Made here, at the
    // live tick, its collision bookkeeping is built against the live
    // world - a ball 1.9 m from the spawn it is then put back onto -
    // where the server built it coincident with the balls born the same
    // tick (a zero contact normal, no event; measured: the host's own
    // ball knocked 0.8 m back at the first replay and 11-17 m from the
    // server's through the start pile).
    void session_client_core::make_remote_birth(const bmmo::session_event_msg& event) {
        const uint32_t now = current_tick();
        if (config.rollback && config.lifecycle_rewind && event.tick != 0 && static_cast<int32_t>(event.tick - now) <= 0
                && s.rollback_.knows_tick(event.tick - 1)) {
            s.pending_births_.push_back(pending_birth::of(event));
            rewind_for_lifecycle(event);
            // The replay made it at its tick; if it did not get that
            // far, make it now.
            for (auto it = s.pending_births_.begin(); it != s.pending_births_.end();)
                if (it->player == event.player) {
                    logf("remote physicalize of %u: the replay did not reach tick %u, made live", event.player, event.tick);
                    physicalize_remote(*it, live_first_tick(event.tick));
                    it = s.pending_births_.erase(it);
                } else ++it;
            return;
        }
        // (Retail's mirror_start_from_row, the 9.17 workaround that started
        // a live birth from its player's newest snapshot row, was deleted at
        // R3: with the lifecycle rewind the birth is made at its stamp.)
        physicalize_remote(pending_birth::of(event), live_first_tick(event.tick));
        rewind_for_lifecycle(event);
    }

    void session_client_core::apply_event(const bmmo::session_event_msg& event) {
        if (event.player == 0 || event.player == s.own_id_) return;
        journal_.lifecycle_event(event.player, event);
        // Step A3: the remote lifecycle by applied tick (life_log_).
        if (event.type == bmmo::session::event_type::Physicalize || event.type == bmmo::session::event_type::Unphysicalize)
            log_life(event);
        switch (event.type) {
        case bmmo::session::event_type::Physicalize: {
            // A birth older by origin than a death of the same player this
            // client applied already (the death's relay overtook it): the
            // server applies the birth first and the death after it.  In
            // the death's own tick (both re-stamped into one, or the
            // birth's applied tick after it, which the server supersedes
            // and never relays) the two leave no ball: dropped.  Earlier,
            // the ball lived from the birth to the death: made as below,
            // inside a replay, and then the death applied again and
            // rewound for, so nothing is left of it after the death's
            // tick (review of step 16: applied alone, the mirror lived on
            // where the server and every other side had none).
            const auto death = s.remote_last_unphysicalize_.find(event.player);
            const bool overtaken = death != s.remote_last_unphysicalize_.end()
                                && static_cast<int32_t>(death->second.origin - event.origin_tick) > 0;
            if (overtaken && static_cast<int32_t>(event.tick - death->second.applied) >= 0) {
                ++s.births_overtaken_;
                logf("remote physicalize of %u stamped %u (applied %u) is older than its death stamped %u, applied"
                     " at %u already: no ball", event.player, event.origin_tick, event.tick,
                     death->second.origin, death->second.applied);
                break;
            }
            // The origin, which the guard below compares (design 3.1);
            // never backwards (an older birth relayed late).
            if (auto [last, fresh] = s.remote_last_physicalize_.try_emplace(event.player, event.origin_tick);
                    !fresh && static_cast<int32_t>(event.origin_tick - last->second) > 0)
                last->second = event.origin_tick;
            if (overtaken) {
                ++s.births_overtaken_;
                const applied_death later = death->second;
                logf("remote physicalize of %u stamped %u (applied %u) is older than its death stamped %u, applied"
                     " at %u already: made, and the death applied again", event.player, event.origin_tick,
                     event.tick, later.origin, later.applied);
                make_remote_birth(event);
                if (unphysicalize_remote(event.player, later.applied)) rewind_for_lifecycle(event.player, later.applied);
                break;
            }
            make_remote_birth(event);
            break;
        }
        case bmmo::session::event_type::Sector:
            // Another member entered a sector: an input to our world too.
            // Stamped into the union, which starts the sector before the
            // step of the stamped tick - this very frame when the relay was
            // on time (drain_due_events runs before the union's tick), the
            // next one when it was late, which the union counts.
            //
            // Step B1 (resync design 2.7): the stamp log first; then a late
            // stamp that changes the union (not covered by a request or
            // stamp at or before its tick) is re-derived where it belongs:
            // inside the reach it joins the frame's late set (one rewind to
            // the lowest tick - 1, rewind_for_sectors), beyond it the sector
            // starts live and a resync brings it to the server's tick.  A
            // stamp before this numbering's first tick (a late joiner's
            // catch-up: never reachable) and one beyond the reach while a
            // resync is pending start live and nothing else: the joiner's
            // owed passes, the resync state.  A reachable late stamp rewinds
            // even while a resync is pending (2.7 case 3); the resync's state
            // overwrites the world and the union records after it.  Without
            // the rollback, as before.
            {
                note_sector_stamp(event.player, event.sector, event.tick);
                const bool late = static_cast<int32_t>(event.tick - current_tick()) <= 0;
                if (late && config.rollback && event.sector > 0 && !s.sectors_.covers(event.sector, event.tick)) {
                    if (sector_rewind_reachable(event.tick)) {
                        client_core_state::late_sector entry;
                        entry.tick = event.tick;
                        entry.player = event.player;
                        entry.sector = event.sector;
                        s.sector_late_.push_back(entry);
                        break;
                    }
                    s.sectors_.stamp(event.player, event.sector, event.tick);
                    logf("remote player %u entered sector %d, stamped for tick %u (LATE: the server started it already)",
                         event.player, event.sector, event.tick);
                    if (s.resync_pending_ || static_cast<int32_t>(event.tick - s.tick_base_) <= 0) break;
                    ++s.sector_rewinds_missed_;
                    logf("late sector stamp for tick %u is outside the rollback history at local tick %u: stamped live, resync",
                         event.tick, current_tick());
                    request_resync("a late Sector event beyond the reach", true);
                    break;
                }
                s.sectors_.stamp(event.player, event.sector, event.tick);
                logf("remote player %u entered sector %d, stamped for tick %u%s", event.player, event.sector, event.tick,
                     late ? " (LATE: the server started it already)" : "");
            }
            break;
        case bmmo::session::event_type::Unphysicalize: {
            // A death stamped at or before the player's last applied birth
            // is a copy that lost its place (protocol 6); applied, it would
            // take the newer mirror out.  Stamped: compared on origins, as
            // the server does (design 3.1).
            if (const auto last = s.remote_last_physicalize_.find(event.player);
                    bmmo::session::lifecycle_unphysicalize_superseded(
                            event.origin_tick, last != s.remote_last_physicalize_.end(),
                            last == s.remote_last_physicalize_.end() ? 0 : last->second)) {
                ++s.events_superseded_;
                return;
            }
            // Newest by origin, with its applied tick: an older birth
            // relayed after it is put back in front of it (above).
            if (auto [death, fresh] = s.remote_last_unphysicalize_.try_emplace(
                        event.player, applied_death{event.origin_tick, event.tick});
                    !fresh && static_cast<int32_t>(event.origin_tick - death->second.origin) > 0)
                death->second = applied_death{event.origin_tick, event.tick};
            if (!unphysicalize_remote(event.player, event.tick)) return;
            rewind_for_lifecycle(event);
            break;
        }
        default:
            break;
        }
    }

    // ---------------------------------------------------- pending births
    // Births deferred into a replay (apply_event): made at their tick,
    // before its step.
    // The event log is muted around them: physicalize_remote bumps the
    // mirror's generation itself, and the replay labels the records it
    // re-captures from the birth tick on with that generation
    // (rollback_world::replay_remotes).  The "created" entry the live
    // frame would drain afterwards bumped it a second time, so the live
    // records after the replay named another lifetime than the replayed
    // ones and none of those could restore the body.
    // In the server's order (session_runner::step): join order, and our
    // own changes due at this tick (a beam's stamp, a moved birth's
    // applied tick: apply_own_lives) at ours - two bodies made in one tick
    // are made in that order and the order changes the world.  A beam's
    // stamp is applied here only once its live frame has run: before that
    // no replay reaches it.
    void session_client_core::drain_pending_births(uint32_t tick) {
        if (s.pending_births_.empty() && !own_lives_due(tick)) return;
        std::vector<pending_birth> due;
        for (auto it = s.pending_births_.begin(); it != s.pending_births_.end();)
            if (it->tick == tick) {
                due.push_back(*it);
                it = s.pending_births_.erase(it);
            } else ++it;
        std::stable_sort(due.begin(), due.end(), [this](const pending_birth& a, const pending_birth& b) {
            return bmmo::session::lifecycle_apply_before(join_rank(a.player), a.origin,
                                                         bmmo::session::event_type::Physicalize,
                                                         join_rank(b.player), b.origin,
                                                         bmmo::session::event_type::Physicalize);
        });
        bool own = own_lives_due(tick);
        const uint32_t own_rank = join_rank(s.own_id_);
        for (const auto& birth: due) {
            if (own && join_rank(birth.player) > own_rank) {
                apply_own_lives(tick, "replay", false);
                own = false;
            }
            bool made = false;
            {
                const event_log_mute mute(*this);
                made = physicalize_remote(birth, tick);
            }
            // A remake that failed (step LB): its body was taken out at the
            // restore, so it is queued again for make_unmade_remakes, which
            // tries once more, live, after the frame's rewinds.
            if (!made && birth.remake) s.pending_births_.push_back(birth);
            s.mechanism_tracking_.mark_dirty();
        }
        if (own) apply_own_lives(tick, "replay", false);
    }

    // ----------------------------------------------------------- mirrors
    // The first tick the live run steps a mirror made in this frame (before
    // the step of current_tick() + 1): that tick, or the stamp if it is
    // still ahead.
    uint32_t session_client_core::live_first_tick(uint32_t stamp) const {
        const uint32_t next = current_tick() + 1;
        return stamp != 0 && static_cast<int32_t>(stamp - next) > 0 ? stamp : next;
    }

    // A mirror's body goes, with the event log muted: the caller bumps the
    // generation itself, and the "deleted" entry the live frame drains
    // later bumped it a second time - after a replay had labelled the
    // records of a same-stamp rebirth with the generation in between
    // (beam, or a late death and respawn in one frame), so the live
    // records named another lifetime than the replayed ones.
    void session_client_core::unphysicalize_mirror(const std::string& entity) {
        {
            const event_log_mute mute(*this);
            platform_.unmake_mirror(entity);
        }
        s.mechanism_tracking_.mark_dirty();
    }

    // The frame a live step of `tick` drives a predicted mirror with: the
    // relayed frame for that tick when the ring has it, else the newest
    // relayed frame (the predictor), else no keys with the navigation off.
    bmmo::session::input_frame session_client_core::live_drive(const remote_ball& remote, uint32_t tick) {
        const auto exact = remote.inputs.find(tick);
        const bool have = exact != remote.inputs.end() || remote.have_input;
        bmmo::session::input_frame drive = exact != remote.inputs.end() ? exact->second : remote.input;
        if (!have) drive.keys = 0;
        if (!have || !(drive.flags & bmmo::session::INPUT_FLAG_NAV_ACTIVE))
            drive.flags &= static_cast<uint8_t>(~bmmo::session::INPUT_FLAG_NAV_ACTIVE);
        return drive;
    }

    // `first_tick`: the first tick this world steps the body (the stamp
    // inside a replay, live_first_tick otherwise).
    // A remake (event.remake, step LB: remake_mirror_birth took the body out
    // at a restore) makes the same body again: the entity it had (no
    // mirror_entity, which on the headless host clones a new one), its
    // lifetime kept (no generation bump: the records of the window name
    // it), and nothing of the remote's live state changed but its body and
    // navigation - the predictor, the frame the next live step drives it
    // with, the corrector and the birth stay as they are.
    bool session_client_core::physicalize_remote(const pending_birth& event, uint32_t first_tick) {
        std::string error;
        {
            const bool remake = event.remake;
            std::string entity;
            if (remake) {
                const auto found = s.remotes_.find(event.player);
                if (found != s.remotes_.end()) entity = found->second.entity;
                if (entity.empty()) { logf("remote physicalize: no mirror of %u to make again", event.player); return false; }
            } else {
                entity = platform_.mirror_entity(event.player, event.ball_type, error);
                if (entity.empty()) { logf("remote physicalize: %s", error.c_str()); return false; }
            }
            auto& remote = s.remotes_[event.player];
            if (remote.navigation) platform_.detach_mirror_navigation(remote.entity);
            remote.navigation = false;
            if (remote.physicalized && remote.entity != entity) {
                ++s.generations_[remote.entity];   // a trafo: the old mirror's lifetime ends here
                unphysicalize_mirror(remote.entity);
            }
            mirror_spec spec;
            spec.player = event.player;
            spec.ball_type = event.ball_type;
            spec.entity = entity;
            std::copy(std::begin(event.position), std::end(event.position), spec.position);
            std::copy(std::begin(event.rotation), std::end(event.rotation), spec.rotation);
            spec.recipe = to_bridge_recipe(event.recipe);
            int join_order = 63;
            for (const auto& p: s.players_) if (p.id == event.player) join_order = p.join_order;
            spec.group = "P#" + std::to_string(join_order);
            // The spawn kick goes with the body (design 2.2 make_mirror): its
            // direction from the stamp, as the server derives it.
            if ((event.flags & bmmo::session::PHYSICALIZE_FLAG_SPAWN) && s.spawn_impulse_ > 0.0f) {
                spec.kick = true;
                spec.kick_index = bmmo::session::spawn_direction_index(s.seed_, static_cast<uint8_t>(join_order), event.tick);
                spec.kick_strength = s.spawn_impulse_;
            }
            const mirror_made made = platform_.make_mirror(spec);
            if (!made.made) {
                logf("remote physicalize %s: %s", entity.c_str(), made.error.c_str());
                return false;
            }
            if (remake) {
                ++s.mirror_remakes_;
                remote.physicalized = true;
                attach_remote_navigation(event.player, remote, true);
                return true;
            }
            // The mirror's body is new: a new lifetime, so no record of the
            // previous one can restore it (design 9.25).
            ++s.generations_[entity];
            remote.entity = entity;
            remote.ball_type = event.ball_type;
            remote.physicalized = true;
            remote.born_tick = first_tick;
            remote.driven_from = first_tick;
            // Made at its stamp: every later restore anchored before it can
            // make it again the same way (step LB).
            remote.birth = event;
            remote.birth_known = first_tick == event.tick;
            remote.corrector.clear();
            // Frames relayed before this mirror existed move into its ring:
            // the newest becomes the live predictor, and a rewind that later
            // reads the ring finds them - the replay of a late birth too,
            // which drives the newborn from its stamp on
            // (rollback_world::replay_remotes).  Frames of an earlier
            // lifetime stay: a rewind across the death still drives that
            // body at those ticks.
            {
                bmmo::session::input_frame newest{};
                const uint32_t newest_tick = s.pending_relay_.take(event.player, remote.inputs, bmmo::session::kRelayRing, &newest);
                if (newest_tick && (!remote.have_input || static_cast<int32_t>(newest_tick - remote.input_tick) >= 0)) {
                    remote.input = newest;
                    remote.input_tick = newest_tick;
                    remote.have_input = true;
                }
            }
            if (spec.kick) {
                if (!made.kicked)
                    logf("remote spawn impulse for %u: %s", event.player, made.kick_error.c_str());
                else
                    logf("remote spawn impulse index=%u for player %u at tick %u", spec.kick_index, event.player, event.tick);
            }
            attach_remote_navigation(event.player, remote);
            // The next live step (current_tick() + 1: the birth tick when
            // made live, the tick after the replay when made inside one)
            // is driven by remote.applied, which still held the frame the
            // previous body had when it died - the end-of-frame loop skips
            // a dead mirror.  Driven as that loop would have.
            remote.applied = live_drive(remote, current_tick() + 1);
            logf("remote player %u physicalized (%s) at tick %u%s", event.player, remote.entity.c_str(), event.tick,
                 remote.navigation ? ", predicted" : ", mirrored");
            // (The rewind for a late birth happens in apply_event, once the
            // body is fully set up: the replay only drives and records a
            // remote that is physicalized AND navigated, and the spawn
            // impulse is part of the state the owner had at the stamp tick
            // - rewinding before either left the mirror outside the
            // rollback entirely, undriven and unkicked, and the balls ran
            // away instead of converging.)
        }
        return true;
    }

    // `remake`: the navigation of a mirror made again inside a replay (step
    // LB) - attached as at its birth, without the log line and with the
    // corrector left as the live frames have it.
    void session_client_core::attach_remote_navigation(uint32_t player, remote_ball& remote, bool remake) {
        if (!remote.physicalized || remote.navigation) return;
        std::string error;
        const mirror_navigation attached = platform_.attach_mirror_navigation(player, remote.entity, remote.ball_type, error);
        if (attached == mirror_navigation::unavailable) return;
        if (attached == mirror_navigation::attached) {
            remote.navigation = true;
            if (remake) return;
            remote.corrector.clear();
            logf("remote %s now predicted", remote.entity.c_str());
        } else {
            logf("remote navigation for %s: %s", remote.entity.c_str(), error.c_str());
        }
    }


    // Step LB: a mirror born inside a rewind's window, taken out at the
    // restore (the binding's destroy_body) and made again at its birth tick
    // by drain_pending_births, in join order with the other births of that
    // tick and our own changes - what the first replay, or the live frame,
    // did.  Parked instead (the path before LB), it was held out of
    // collision detection through its birth tick's step and re-posed after
    // it at its birth record, which a late birth's first replay had made
    // without the frame of that tick (the relay had not reached it, so the
    // navigation stood idle): every later replay kept that tick's missing
    // force and the collisions of the balls born with it (swing c0: the
    // mirrors born at 701 0.17 m/s behind the server at 702).  Only a body
    // made at its stamp with its navigation from the start (birth_known,
    // driven_from) and only with the lifecycle rewind, the switch under
    // which this host makes births inside a replay at all.
    bool session_client_core::remakes_birth(const remote_ball& remote) const {
        return config.rollback && config.lifecycle_rewind && remote.physicalized && remote.navigation && remote.birth_known
            && remote.born_tick != 0 && remote.birth.tick == remote.born_tick && remote.driven_from == remote.born_tick;
    }

    bool session_client_core::remake_mirror_birth(const std::string& entity, uint32_t birth_tick) {
        for (auto& [player, remote]: s.remotes_) {
            if (remote.entity != entity) continue;
            if (!remakes_birth(remote) || remote.born_tick != birth_tick) return false;
            for (const auto& queued: s.pending_births_)
                if (queued.player == player && queued.tick == birth_tick) return false;   // made there already
            platform_.detach_mirror_navigation(entity);
            remote.navigation = false;
            unphysicalize_mirror(entity);
            remote.physicalized = false;
            pending_birth again = remote.birth;
            again.remake = true;
            s.pending_births_.push_back(again);
            return true;
        }
        return false;
    }

    // A remake no replay made (the replay stopped before its birth tick, or
    // the make failed there): the mirror is made now, at its birth pose, as
    // make_remote_birth makes a late birth whose replay did not get that
    // far.  When this make fails too the mirror is left without a body until
    // the player's next Physicalize (its frames counted bodiless): counted
    // (remake=.../lost) and never remade again.
    void session_client_core::make_unmade_remakes() {
        for (auto it = s.pending_births_.begin(); it != s.pending_births_.end();) {
            if (!it->remake) { ++it; continue; }
            const pending_birth birth = *it;
            s.pending_births_.erase(it);
            logf("remote physicalize of %u: the replay did not make it again at tick %u, made live", birth.player, birth.tick);
            bool made = false;
            {
                const event_log_mute mute(*this);
                made = physicalize_remote(birth, birth.tick);
            }
            if (!made) {
                ++s.mirror_remakes_lost_;
                if (const auto remote = s.remotes_.find(birth.player); remote != s.remotes_.end())
                    remote->second.birth_known = false;
                logf("remote physicalize of %u: made again neither in the replay nor live; no body until its next Physicalize",
                     birth.player);
            }
            s.mechanism_tracking_.mark_dirty();
            it = s.pending_births_.begin();
        }
    }


    // ======================================================= own lives (E7)
    // Moved from the headless client (session_client.cpp at f2227b0): the
    // own ball's body changes the history keeps (struct own_life), the beam,
    // the own births reported and the echoes of our own events, verbatim but
    // for the platform calls (own_birth_capture, ball_recipe_of, make_own,
    // unmake_own, body_state, write_body, own_driven_entity, bind_world,
    // keep_live_cells), the configuration (rollback, own_echoes) and the
    // back-calls (own_ball, recorded_input_at).  Without own_body_control
    // (design 2.3) the own body is never made or taken out: the beam's stamp
    // is an in-place write, and an echo the history cannot carry is a
    // re-report or a resync.

    // ------------------------------------------------- own reports (step 19)
    // The own ball's Physicalize as the event log reported it for `tick`:
    // the pose the retail script physicalized at and the spawn flag (design
    // 9.10), both taken before the step moved the body (the host's capture).
    // Without a capture of this birth - no Ball Navigation yet to anchor the
    // callback to - the entity after the tick is all there is, and it is
    // never flagged: no kick was applied here, so none may be applied
    // anywhere else.
    void session_client_core::report_physicalize(const std::string& entity, uint32_t tick) {
        own_birth_report report;
        report.entity = entity;
        report.tick = tick;
        report.pose = platform_.own_birth_capture(entity, tick);
        report.ball_type = platform_.ball_recipe_of(entity, report.recipe);
        commit_own_birth(report);
    }

    void session_client_core::commit_own_birth(const own_birth_report& report) {
        bmmo::session_event_msg event;
        event.tick = report.tick;
        event.type = bmmo::session::event_type::Physicalize;
        event.ball_type = static_cast<uint8_t>(report.ball_type < 0 ? 0 : report.ball_type);
        if (!report.pose.captured) logf("warning: physicalize pose taken from the entity after the tick (no capture before the step)");
        std::copy(std::begin(report.pose.position), std::end(report.pose.position), event.position);
        std::copy(std::begin(report.pose.rotation), std::end(report.pose.rotation), event.rotation);
        const bool spawn = report.pose.captured && report.pose.spawn;
        if (spawn) event.flags |= bmmo::session::PHYSICALIZE_FLAG_SPAWN;
        event.recipe = report.recipe;
        send_event(event);
        // own_body_backstop (R1): what a re-report carries.
        s.own_last_birth_.valid = true;
        s.own_last_birth_.ball_type = event.ball_type;
        s.own_last_birth_.flags = event.flags;
        s.own_last_birth_.recipe = event.recipe;
        s.snapshots_without_own_ = 0;
        // The birth as the history keeps it: what the server makes when
        // its echo says it applied the event later (record_own_birth).
        record_own_birth(event, report.entity);
        logf("own ball %s physicalized at tick %u (type %d%s) pos=%a,%a,%a", report.entity.c_str(), report.tick,
             report.ball_type, spawn ? ", spawn" : "", static_cast<double>(event.position[0]),
             static_cast<double>(event.position[1]), static_cast<double>(event.position[2]));
    }

    void session_client_core::report_unphysicalize(uint32_t tick) {
        bmmo::session_event_msg event;
        event.tick = tick;
        event.type = bmmo::session::event_type::Unphysicalize;
        send_event(event);
        // The ball is meant to be gone now (own_body_backstop, R1).
        s.own_last_birth_.valid = false;
        s.snapshots_without_own_ = 0;
        logf("own ball unphysicalized at tick %u", tick);
    }

    void session_client_core::own_physicalized(const own_birth_report& report) {
        client_core_state::own_report queued;
        queued.report = report;
        s.own_reports_.push_back(std::move(queued));
    }

    void session_client_core::own_unphysicalized(uint32_t tick) {
        client_core_state::own_report queued;
        queued.birth = false;
        queued.report.tick = tick;
        s.own_reports_.push_back(std::move(queued));
    }

    // Committed where the event log's reports are (design 1.3): after the
    // drain bumped the generation of a body made this frame, so the life's
    // generation is the body's.
    void session_client_core::commit_own_reports() {
        std::vector<client_core_state::own_report> queued;
        queued.swap(s.own_reports_);
        for (const auto& r: queued) {
            if (r.birth) commit_own_birth(r.report);
            else report_unphysicalize(r.report.tick);
        }
    }

    // ------------------------------------------------------------- beam
    void session_client_core::request_beam(const double position[3]) {
        s.beam_requested_ = true;
        for (int k = 0; k < 3; ++k) s.beam_position_[k] = position[k];
    }

    void session_client_core::apply_requested_beam(uint32_t tick) {
        if (!s.beam_requested_) return;
        s.beam_requested_ = false;
        beam_own_ball(tick);
    }

    // The own ball beamed in place, upright and at rest (no rebuild: a
    // ball without the navigation replica, or a host without own body
    // control).
    bool session_client_core::write_beam(const std::string& name, const double position[3], uint32_t tick) {
        const double rotation[4] = {0.0, 0.0, 0.0, 1.0};
        const float still[3] = {0.0f, 0.0f, 0.0f};
        std::string error;
        if (!platform_.write_body(name, position, rotation, still, still, true, error)) {
            logf("--beam at tick %u: %s", tick, error.c_str());
            return false;
        }
        return true;
    }

    // The automation `beam` verb, headless: the own ball at `position`,
    // upright and at rest.  The server's copy is moved by re-reporting the
    // ball's life at that pose - a plain repeat Physicalize would not move
    // it, because physicalize() of an entity that already has a body is a
    // no-op (physics_state.cpp:741), so the pair the retail death sends is
    // used instead.
    //
    // This client does what the server does with that pair, at the pair's
    // stamp: the body is rebuilt at tick + 1, before that tick's step
    // (apply_own_lives), not moved now.  Moving it here, after the record of
    // `tick`, left the move outside every replay: a rewind anchored at or
    // before `tick` re-stepped the old path and nothing put the beam back
    // (measured: the own ball 540 m off on three of four clients), and
    // the moved core kept its floor contact, which the server's fresh one
    // does not have.  Applied at the stamp, live and in every replay that
    // crosses it, the record of `tick` stays the pre-beam world the
    // server's checkpoint of `tick` is, and the lifetime stays the same:
    // the body has the same name, recipe and group on every side.
    //
    // Without own body control (design 2.3) the record is kept the same
    // way, but its stamp is an in-place write (write_beam), live and in
    // every replay that crosses it, and the generation is bumped here, at
    // the request, as the in-place branch below does.
    void session_client_core::beam_own_ball(uint32_t tick) {
        const std::string name = platform_.own_ball().entity;
        if (name.empty() || !s.own_physicalized_) {
            logf("--beam at tick %u: no own body to beam", tick);
            return;
        }
        // Without the navigation replica (no --rollback) the ball is driven
        // by the retail force controllers, which die with the core and are
        // not re-made until the key is released: the body is moved in
        // place, as before.  There is no rewind to undo it then.
        const bool navigated = !platform_.own_driven_entity().empty();
        if (!navigated) {
            if (!write_beam(name, s.beam_position_, tick)) return;
            ++s.generations_[name];   // the server rebuilds its copy: a new lifetime
        } else if (!caps_.own_body_control) {
            ++s.generations_[name];   // design 2.3: the server's new lifetime, bumped at the request
        }
        own_life beam;
        beam.what = own_life::kind::beam;
        beam.origin = beam.applied = tick + 1;
        beam.entity = name;
        {
            const auto generation = s.generations_.find(name);
            beam.generation = generation == s.generations_.end() ? 0u : generation->second;
        }
        bmmo::session::ball_recipe recipe;
        const int ball_type = platform_.ball_recipe_of(name, recipe);
        beam.ball_type = static_cast<uint8_t>(std::max(ball_type, 0));
        for (int k = 0; k < 3; ++k) beam.position[k] = static_cast<float>(s.beam_position_[k]);
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k) beam.rotation[r * 3 + k] = r == k ? 1.0f : 0.0f;
        beam.recipe = recipe;
        beam.group = s.own_group_;
        bmmo::session_event_msg down;
        down.tick = beam.origin;
        down.type = bmmo::session::event_type::Unphysicalize;
        send_event(down);
        bmmo::session_event_msg up;
        up.tick = beam.origin;
        up.type = bmmo::session::event_type::Physicalize;
        up.ball_type = beam.ball_type;
        std::copy(std::begin(beam.position), std::end(beam.position), up.position);
        std::copy(std::begin(beam.rotation), std::end(beam.rotation), up.rotation);
        up.recipe = beam.recipe;
        send_event(up);
        // The server's copy is a fresh body now, not kicked (own_body_backstop,
        // R1: a re-report carries no spawn flag either).
        s.own_last_birth_.flags = static_cast<uint8_t>(s.own_last_birth_.flags & ~bmmo::session::PHYSICALIZE_FLAG_SPAWN);
        s.snapshots_without_own_ = 0;
        logf("--beam: %s to (%.3f,%.3f,%.3f), reported to the server for tick %u and %s", name.c_str(),
             s.beam_position_[0], s.beam_position_[1], s.beam_position_[2], beam.origin,
             !navigated ? "moved here now (no navigation replica)"
             : caps_.own_body_control ? "rebuilt here at that tick" : "moved here at that tick (no own body control)");
        if (navigated) s.own_life_.push_back(std::move(beam));
    }

    // ------------------------------------------------------ own lives
    // A birth the retail scripts made this frame and report_physicalize
    // sent, as an own_life: the body the scripts made stays (a replay
    // across it parks it as before), and the record is what moves it when
    // the server's echo says it applied the Physicalize later, or removes
    // it when the server never did (late-player design 3.3).
    void session_client_core::record_own_birth(const bmmo::session_event_msg& event, const std::string& entity) {
        if (!config.rollback) return;   // no history to move it in
        own_life birth;
        birth.what = own_life::kind::birth;
        birth.origin = birth.applied = event.tick;
        birth.entity = entity;
        {
            const auto generation = s.generations_.find(entity);
            birth.generation = generation == s.generations_.end() ? 0u : generation->second;
        }
        birth.ball_type = event.ball_type;
        std::copy(std::begin(event.position), std::end(event.position), birth.position);
        std::copy(std::begin(event.rotation), std::end(event.rotation), birth.rotation);
        birth.recipe = event.recipe;
        birth.group = s.own_group_;
        birth.spawn = (event.flags & bmmo::session::PHYSICALIZE_FLAG_SPAWN) != 0;
        birth.origin_live = birth.applied_live = true;
        s.own_life_.push_back(std::move(birth));
    }

    // Whether the ball's lifetime `life` belongs to is still the one the
    // world has: a death or a trafo since (the drain bumped the
    // generation) ended it, and a replay that made its body again would
    // put back a body the live run no longer has, with nothing in the
    // replay to take it out again.
    bool session_client_core::own_life_current(const own_life& life) const {
        const auto generation = s.generations_.find(life.entity);
        return (generation == s.generations_.end() ? 0u : generation->second) == life.generation;
    }

    // The own body of `life` taken out, with the event log muted: not a
    // lifetime change of the body the records describe (a beam's
    // Unphysicalize half, a moved birth before its applied tick).  The
    // entity is put at the pose its Physicalize carried, where the body
    // is made again.  (The host shuts down the navigation replica first:
    // its controllers would otherwise point into the core that goes.)
    bool session_client_core::unmake_own_body(const own_life& life, uint32_t tick, const char* where) {
        std::string error;
        bool ok = false;
        {
            const event_log_mute mute(*this);
            ok = platform_.unmake_own(life.entity, life.position, life.rotation, tick, error);
        }
        s.mechanism_tracking_.mark_dirty();
        if (!ok && s.own_lives_applied_ < 16)
            logf("own body at tick %u (%s): unphysicalize %s: %s", tick, where, life.entity.c_str(), error.c_str());
        return ok;
    }

    // The own body of `life` made at `tick` the way the server makes it
    // (physics_world::apply_event): the pose, recipe and group its
    // Physicalize carried, and a spawn's kick with the impulse index of
    // `tick` - the applied tick, which the server and every remote kick
    // with too.  A body still there is taken out first (a beam's pair in
    // one tick, or a body a restore parked instead of destroying).  The
    // event log is muted: the body is the same life to every side, so
    // nothing about it is reported, and its generation stays.
    bool session_client_core::remake_own_body(const own_life& life, uint32_t tick, const char* where) {
        own_body_spec spec;
        spec.entity = life.entity;
        std::copy(std::begin(life.position), std::end(life.position), spec.position);
        std::copy(std::begin(life.rotation), std::end(life.rotation), spec.rotation);
        spec.recipe = to_bridge_recipe(life.recipe);
        spec.group = life.group;
        spec.tick = tick;
        if (life.spawn && s.spawn_impulse_ > 0.0f) {
            const uint8_t join_order = static_cast<uint8_t>(s.own_join_order_ < 0 ? 63 : s.own_join_order_);
            spec.kick = true;
            spec.kick_index = bmmo::session::spawn_direction_index(s.seed_, join_order, tick);
            spec.kick_strength = s.spawn_impulse_;
        }
        own_body_made made;
        {
            const event_log_mute mute(*this);
            made = platform_.make_own(spec);
        }
        if (!made.found) {
            logf("own body at tick %u: no 3D entity named %s", tick, life.entity.c_str());
            return false;
        }
        if (made.took_out_failed)
            logf("own body at tick %u (%s): unphysicalize %s: %s", tick, where, life.entity.c_str(), made.took_out_error.c_str());
        s.mechanism_tracking_.mark_dirty();
        ++s.own_lives_applied_;
        if (!made.made) {
            logf("own body at tick %u (%s): physicalize %s: %s", tick, where, life.entity.c_str(), made.error.c_str());
            return false;
        }
        if (spec.kick && !made.kicked)
            logf("own body at tick %u: spawn impulse: %s", tick, made.kick_error.c_str());
        if (s.own_lives_applied_ <= 16)
            logf("own body: %s made at (%.3f,%.3f,%.3f) before tick %u (%s, %s%s)", life.entity.c_str(),
                 static_cast<double>(life.position[0]), static_cast<double>(life.position[1]),
                 static_cast<double>(life.position[2]), tick, where,
                 life.what == own_life::kind::beam ? "beam" : "moved birth", life.spawn ? ", spawn kick" : "");
        return true;
    }

    // The own body changes due before the step of `tick`, as the server
    // applies the pair or the Physicalize (physics_world::apply_event):
    //
    //   * a beam at its stamp: the body goes, and is made again at the
    //     beam pose when the server applied its Physicalize there too;
    //   * a moved birth or beam Physicalize at its applied tick: the body
    //     made again (remake_own_body).  A birth's own origin needs
    //     nothing: the scripts made the body live, a replay parks it, and
    //     a moved one was taken out at the restore (the destroy_body hook,
    //     or process_own_echoes before the first rewind).
    //
    // `live`: the frame of `tick` itself (live_own_lives), each change
    // once; otherwise a replay (drain_pending_births), where a beam's
    // stamp is redone only once its live frame has run - before that no
    // replay reaches it.  A lifetime that ended since is never made again
    // (own_life_current) and retires.
    //
    // Without own body control (design 2.3) a beam's stamp is the in-place
    // write instead, and nothing is ever moved (no remake is due).
    void session_client_core::apply_own_lives(uint32_t tick, const char* where, bool live) {
        for (auto& life: s.own_life_) {
            if (life.retired) continue;
            const bool beam_stamp = life.what == own_life::kind::beam && life.origin == tick
                                 && (live ? !life.origin_live : life.origin_live);
            const bool remake_due = life.moved() && life.applied == tick && (!live || !life.applied_live);
            if (!beam_stamp && !remake_due) continue;
            if (!own_life_current(life)) {
                const auto generation = s.generations_.find(life.entity);
                logf("own %s stamped %u retired at tick %u (%s): %s has had another life since (generation %u -> %u)",
                     life.what == own_life::kind::beam ? "beam" : "birth", life.origin, tick, where,
                     life.entity.c_str(), life.generation, generation == s.generations_.end() ? 0u : generation->second);
                life.retired = true;
                continue;
            }
            if (beam_stamp) {
                if (!caps_.own_body_control) {
                    if (life.made) {
                        const double position[3] = {life.position[0], life.position[1], life.position[2]};
                        write_beam(life.entity, position, tick);
                    }
                    ++s.own_lives_applied_;
                } else if (life.made && life.applied == life.origin) {
                    remake_own_body(life, tick, where);
                } else {
                    unmake_own_body(life, tick, where);
                    ++s.own_lives_applied_;
                }
                if (live) life.origin_live = true;
                if (live && life.applied == life.origin) life.applied_live = true;
            }
            if (remake_due) {
                if (remake_own_body(life, tick, where)) ++s.own_remakes_;
                if (live) life.applied_live = true;
            }
            if (live) {
                // The body this frame's step starts from: the capture
                // callback of this frame must not take it for a birth of
                // the scripts (capture_own_physicalize).
                bmmo_physics_body_state state{};
                if (platform_.body_state(life.entity, state)) s.own_body_entity_ = life.entity;
            }
        }
    }

    // Whether apply_own_lives has anything for a replayed `tick`.
    bool session_client_core::own_lives_due(uint32_t tick) const {
        for (const auto& life: s.own_life_) {
            if (life.retired) continue;
            if (life.what == own_life::kind::beam && life.origin == tick && life.origin_live) return true;
            if (life.moved() && life.applied == tick) return true;
        }
        return false;
    }

    // The live half, in the frame of the change, after every rewind of
    // that frame (they all end at the tick before it, on the old path)
    // and before the step, at our place among the body changes stamped
    // for the same tick (apply_held_lifecycle).  A change whose live frame
    // was skipped (a resync renumbered past it) is dropped.  The records
    // retire once no rewind can be anchored before them.
    void session_client_core::live_own_lives(uint32_t frame_tick) {
        for (auto& life: s.own_life_) {
            if (life.retired) continue;
            const bool beam_late = life.what == own_life::kind::beam && !life.origin_live
                                && static_cast<int32_t>(frame_tick - life.origin) > 0;
            const bool remake_late = life.moved() && !life.applied_live
                                  && static_cast<int32_t>(frame_tick - life.applied) > 0;
            if (beam_late || remake_late) {
                logf("own %s stamped %u (applied %u) never met its live frame (now %u): dropped",
                     life.what == own_life::kind::beam ? "beam" : "birth", life.origin, life.applied, frame_tick);
                life.retired = true;
            }
        }
        apply_own_lives(frame_tick, "live", true);
        retire_own_lives();
    }

    // A record retires once the live run has made all of it and the
    // history no longer holds the tick before its last change - the
    // origin, or the applied tick of a moved one: no rewind can cross it
    // any more.  (A moved birth has to outlive its origin's anchor: a
    // rewind anchored inside the gap still crosses the applied tick, and
    // the record is what makes the body there.)
    //
    // A birth the server has not answered yet stays, past its history
    // too, until its echo or the deadline (expire_own_echoes) says where
    // the server made it: the answer past the history is a resync, and
    // without the record it could not take a never-made body out.  It
    // makes nothing in a replay (it is not moved), so keeping it is free.
    void session_client_core::retire_own_lives() {
        const uint32_t now = current_tick();
        s.own_life_.erase(std::remove_if(s.own_life_.begin(), s.own_life_.end(), [this, now](const own_life& life) {
            if (life.retired) return true;
            if (life.what == own_life::kind::birth && life.state == own_life::verdict::sent
                    && static_cast<int32_t>(now - life.origin) < static_cast<int32_t>(client_core_state::kOwnRecordKeepTicks))
                return false;
            const bool done = life.origin_live && (life.applied_live || !life.made);
            const uint32_t last = life.moved() ? life.applied : life.origin;
            return done && !s.rollback_.knows_tick(last - 1);
        }), s.own_life_.end());
    }

    // A resync renumbers onto the server's tick and its full snapshot
    // puts the server's own ball in: the births go.  A beam the new
    // numbering has not reached yet is the server's to apply at its stamp,
    // and so ours: live once more.
    // A birth still unanswered is set aside (own_unanswered_): its echo,
    // when it comes, is not one a record of the new numbering can take.
    void session_client_core::rebase_own_lives() {
        set_aside_unanswered(own_life::aside::resync);
        s.own_life_.erase(std::remove_if(s.own_life_.begin(), s.own_life_.end(), [this](own_life& life) {
            if (life.what != own_life::kind::beam || static_cast<int32_t>(life.origin - s.tick_base_) < 0) return true;
            life.origin_live = false;
            life.applied_live = false;
            return false;
        }), s.own_life_.end());
    }

    // The births still waiting for their echo, kept aside (with the echoes
    // already queued: they answer the old numbering) when a SessionAssign
    // renumbers the history.  Bounded: the echo of each comes within a
    // round trip or two, and an old entry answers nothing any more.
    void session_client_core::set_aside_unanswered(own_life::aside why) {
        const uint32_t now = current_tick();
        for (const auto& life: s.own_life_) {
            if (life.retired || life.what != own_life::kind::birth || life.state != own_life::verdict::sent) continue;
            own_life aside = life;
            aside.why = why;
            aside.aside_at = now;
            s.own_unanswered_.push_back(std::move(aside));
        }
        for (auto& msg: s.own_echoes_) s.own_echoes_stale_.push_back(std::move(msg));
        s.own_echoes_.clear();
        while (s.own_unanswered_.size() > 16) s.own_unanswered_.erase(s.own_unanswered_.begin());
    }

    // The set-aside birth an echo answers, or null.
    client_core_state::own_life* session_client_core::find_unanswered(uint32_t origin, uint8_t ball_type) {
        for (auto& life: s.own_unanswered_)
            if (life.origin == origin && life.ball_type == ball_type) return &life;
        return nullptr;
    }

    // A moved own birth's body, asked for by a restore (rollback_world::
    // destroy_body) when the replay will cross its applied tick: the
    // birth record of that lifetime is the applied tick's, and the body
    // is made again there (apply_own_lives).
    const client_core_state::own_life* session_client_core::moved_own_life(const std::string& entity,
                                                                           uint32_t birth_tick) const {
        for (const auto& life: s.own_life_)
            if (!life.retired && life.moved() && life.entity == entity && life.applied == birth_tick
                    && own_life_current(life))
                return &life;
        return nullptr;
    }

    bool session_client_core::destroy_moved_own_birth(const std::string& entity, uint32_t birth_tick, bool& destroyed) {
        if (!caps_.own_body_control) return false;
        const own_life* life = moved_own_life(entity, birth_tick);
        if (!life) return false;
        destroyed = unmake_own_body(*life, current_tick(), "restore");
        return true;
    }

    // --------------------------------------------------------- echoes
    // The echoes of our own events that arrived since the last frame,
    // before this frame's other rewinds (process_own_echo): first those
    // queued before the last SessionAssign, which can only answer the
    // births it set aside, then the rest; then the births whose echo is
    // overdue (expire_own_echoes).
    void session_client_core::process_own_echoes() {
        if (!s.own_echoes_stale_.empty()) {
            std::vector<bmmo::session_event_msg> stale;
            stale.swap(s.own_echoes_stale_);
            for (const auto& msg: stale) process_own_echo(msg, true);
        }
        if (!s.own_echoes_.empty()) {
            std::vector<bmmo::session_event_msg> echoes;
            echoes.swap(s.own_echoes_);
            for (const auto& msg: echoes) process_own_echo(msg, false);
        }
        expire_own_echoes();
    }

    // An echo of one of our own events (protocol 9), counted by what the
    // server did with it: echo=acked/moved/superseded/rejected/missing on
    // the status line.  An ack changes nothing.  Any other verdict goes
    // into the box beside the event as it was sent: the same (player,
    // origin, type) with the server's applied tick and state, which a
    // replay of this file takes over the earlier record (sim_tool keeps
    // the last), so the box replays to the server's world.
    //
    // A Physicalize the server applied later, or never, is moved inside
    // our history (late-player design 3.3, move_own_birth).  A MOVED echo
    // of another type is counted and kept where we stamped it: the own
    // death and the own Sector move are plan steps 22 and 23.  One
    // exception: an own Unphysicalize moved together with the birth of
    // its origin (a beam, a trafo, a respawn's pair) is a pair the server
    // applied late as a whole; the old body lived on at the server until
    // T', which moving the birth alone cannot give, so this client
    // resyncs instead (resync_own_life), whichever echo comes first.
    //
    // `stale`: queued before the last SessionAssign.  It answers a birth
    // that assign set aside (answer_set_aside) and never a record of the
    // new numbering, and asks for no resync of its own: the assign was one.
    //
    // With the own_echoes switch off (design 1.2) nothing is acted on: no
    // record or set-aside birth is looked up, the moved-Unphysicalize list
    // is not kept, and the echo is counted, journaled and logged as kept
    // where we stamped it.
    void session_client_core::process_own_echo(const bmmo::session_event_msg& msg, bool stale) {
        // Step T3: a Trafo input's echo goes the input rewind's way (design
        // 4.6), never the own lives'.
        if (msg.type == bmmo::session::event_type::Trafo) {
            process_trafo_echo(msg, stale);
            return;
        }
        const bool acting = acting_on_echoes();
        const bool rejected = (msg.state & bmmo::session::EVENT_STATE_REJECTED) != 0;
        const bool superseded = !rejected && (msg.state & bmmo::session::EVENT_STATE_SUPERSEDED) != 0;
        const bool made = !rejected && !superseded;
        const bool moved = made && msg.tick != msg.origin_tick;
        const bool birth = msg.type == bmmo::session::event_type::Physicalize;
        own_life* life = nullptr;
        if (birth && !stale && acting)
            for (auto& candidate: s.own_life_)
                if (!candidate.retired && candidate.origin == msg.origin_tick && candidate.ball_type == msg.ball_type
                        && candidate.state == own_life::verdict::sent) {
                    life = &candidate;
                    break;
                }
        own_life* aside = birth && !life && acting ? find_unanswered(msg.origin_tick, msg.ball_type) : nullptr;
        if (made && !moved) {
            ++s.own_echo_acked_;
            if (life) life->state = own_life::verdict::acked;
            if (aside) answer_set_aside(*aside, true, false, msg.tick, "acked");
            return;
        }
        const char* verdict = rejected ? "rejected" : superseded ? "superseded" : "moved";
        if (rejected) ++s.own_echo_rejected_;
        else if (superseded) ++s.own_echo_superseded_;
        else ++s.own_echo_moved_;
        if (birth && moved) ++s.own_birth_echo_moved_;
        journal_.lifecycle_event(s.own_id_, msg);
        journal_.note(std::string("own echo: ") + event_type_name(static_cast<int>(msg.type)) + " stamped "
                      + std::to_string(msg.origin_tick) + " " + verdict + " (applied " + std::to_string(msg.tick) + ")");
        if (!birth) {
            // Step B1 (resync design 2.7, plan step 23): a moved own Sector
            // moves the own pass to the server's tick, like a late remote
            // stamp - when this world handed that stamp to its union.
            // (One reported after the fact never was: the level's chain ran
            // the manager, and it is kept where stamped as before.)
            if (msg.type == bmmo::session::event_type::Sector && moved && acting && !stale && config.rollback) {
                const auto own = s.sector_stamps_.find({s.own_id_, msg.sector});
                if (own != s.sector_stamps_.end() && own->second.tick == msg.origin_tick) {
                    logf("own %s stamped tick %u echoed %s by the server (applied %u, state %02x) at local tick %u; "
                         "the own pass moves to the server's tick",
                         event_type_name(static_cast<int>(msg.type)), msg.origin_tick, verdict, msg.tick,
                         static_cast<unsigned>(msg.state), current_tick());
                    move_own_sector(msg.sector, msg.origin_tick, msg.tick);
                    return;
                }
            }
            if (moved) ++s.own_moves_other_;
            logf("own %s stamped tick %u echoed %s by the server (applied %u, state %02x) at local tick %u; "
                 "kept where we stamped it (only an own Physicalize is moved here)",
                 event_type_name(static_cast<int>(msg.type)), msg.origin_tick, verdict, msg.tick,
                 static_cast<unsigned>(msg.state), current_tick());
            if (msg.type == bmmo::session::event_type::Unphysicalize && moved && acting && !stale) {
                s.own_moved_unphysicalize_.push_back(msg.origin_tick);
                while (s.own_moved_unphysicalize_.size() > 32) s.own_moved_unphysicalize_.erase(s.own_moved_unphysicalize_.begin());
                // The birth of the pair already moved inside the history:
                // the old body's late end is what it missed.
                for (auto& candidate: s.own_life_)
                    if (!candidate.retired && candidate.origin == msg.origin_tick
                            && candidate.state == own_life::verdict::moved) {
                        resync_own_life(candidate, candidate.made, candidate.applied,
                                        "its Unphysicalize was moved too (an own pair, plan step 22)");
                        break;
                    }
            }
            return;
        }
        if (life) {
            life->state = rejected ? own_life::verdict::rejected
                        : superseded ? own_life::verdict::superseded : own_life::verdict::moved;
            // Design 2.3: without own body control a superseded birth needs
            // nothing, its pair's moved Unphysicalize included (with the
            // control the pair resyncs as it always did).
            if (superseded && !caps_.own_body_control) {
                move_own_birth(*life, msg.tick, made);
                return;
            }
            if (std::find(s.own_moved_unphysicalize_.begin(), s.own_moved_unphysicalize_.end(), msg.origin_tick)
                    != s.own_moved_unphysicalize_.end()) {
                resync_own_life(*life, made, msg.tick, "its Unphysicalize was moved too (an own pair, plan step 22)");
                return;
            }
            move_own_birth(*life, msg.tick, made);
            return;
        }
        if (aside) {
            answer_set_aside(*aside, made, superseded, msg.tick, verdict);
            return;
        }
        // No record: never recorded (no rollback), sent before the last
        // resync and answered by it, or retired after kOwnRecordKeepTicks
        // with no echo.
        const bool before_base = static_cast<int32_t>(msg.origin_tick - s.tick_base_) < 0;
        if (!acting || stale || before_base) {
            logf("own physicalize stamped %u echoed %s by the server (applied %u) at local tick %u: no record of it, %s",
                 msg.origin_tick, verdict, msg.tick, current_tick(),
                 !config.rollback ? "no rollback: kept where we stamped it"
                 : !acting ? "own echoes off: kept where we stamped it"
                           : "sent before the last resync: nothing to do");
            return;
        }
        const std::string head = "own birth moved " + std::to_string(msg.origin_tick) + " -> "
                               + (made ? std::to_string(msg.tick) : std::string("never"));
        logf("%s at local tick %u: no record of it (past the history), resync", head.c_str(), current_tick());
        journal_.note(head + ": no record of it (past the history), resync");
        ++s.own_moves_resynced_;
        request_resync("own birth moved past the history", true);
    }

    // The answer to a birth set aside (own_unanswered_), which leaves the
    // list.  Given up as missing but made after all: this client took
    // its body out, so it is made again and the client resyncs.  Sent
    // before a resync and never made: the body the scripts made goes and
    // the client resyncs.  The other two need nothing: the body is where
    // the server has it, and the poses are the snapshots'.  (`superseded`
    // tells a never-made one's verdict to resync_own_life, which without
    // own body control re-reports a rejected birth and never a superseded
    // one.)
    void session_client_core::answer_set_aside(own_life& entry, bool made, bool superseded, uint32_t applied,
                                               const char* verdict) {
        own_life life = entry;
        s.own_unanswered_.erase(s.own_unanswered_.begin() + (&entry - s.own_unanswered_.data()));
        const bool missing = life.why == own_life::aside::missing;
        if (missing == made) {
            if (!made) life.state = superseded ? own_life::verdict::superseded : own_life::verdict::rejected;
            resync_own_life(life, made, applied, missing ? "made after its echo was given up as missing"
                                                         : "sent before the last resync and never made");
            return;
        }
        logf("own physicalize stamped %u echoed %s by the server (applied %u) at local tick %u: %s, nothing to do",
             life.origin, verdict, applied, current_tick(),
             missing ? "taken out already as missing" : "sent before the last resync");
    }

    // An own birth with no echo while the server's tick is
    // kOwnEchoDeadlineTicks past its stamp: the Physicalize never reached
    // the server (the A6 drop variant), or it will be applied so late
    // that the move is past the reach anyway.  Either way the answer is a
    // resync of this client alone, and since the server has no body for
    // it now, the body goes first (resync_own_life with `made` false).
    // The birth is set aside, so an echo that comes after all is still
    // answered (answer_set_aside), and the box gets the event as never
    // made (state REJECTED: sim_tool drops it), so the client journal
    // replays to the server's world; a later echo is journaled after it
    // and wins.
    //
    // The server's tick comes from the relay header, which every relayed
    // datagram carries every tick (plan step 20), and from the snapshots
    // when they run: the server is authoritative with snapshots off too,
    // and a birth it never made must not live on here for good.  The
    // server echoes an event when it accepts it, before it steps that
    // event's applied tick and so before it relays that tick, so an echo
    // that exists is sent ahead of the datagram that passes the deadline;
    // one that loses the race is answered as set aside.
    //
    // The deadline is gated on our uplink (bmmo::session::
    // own_echo_deadline): it counts only once the header's acked shows
    // the server applied a frame of ours at or past the origin, and only
    // kOwnEchoUplinkSettleTicks after that was first seen.  The server's
    // tick moves with the downlink; with the uplink out for longer than
    // the deadline the reliable copy is still queued here, and giving the
    // birth up would resync this client twice (the give-up, then the
    // late copy echoed as made).
    void session_client_core::expire_own_echoes() {
        if (!acting_on_echoes() || s.resync_pending_) return;
        uint32_t server_tick = 0;
        if (!bmmo::session::newest_server_tick(s.acked_header_known_, s.acked_header_tick_, s.have_snapshot_,
                                               s.last_snapshot_tick_, server_tick))
            return;
        for (auto& life: s.own_life_) {
            if (life.retired || life.what != own_life::kind::birth || life.state != own_life::verdict::sent) continue;
            if (!life.echo_deadline.missing(server_tick, s.acked_header_known_, s.header_acked_, life.origin,
                                            client_core_state::kOwnEchoDeadlineTicks,
                                            client_core_state::kOwnEchoUplinkSettleTicks))
                continue;
            ++s.own_echo_missing_;
            life.state = own_life::verdict::missing;
            own_life aside = life;
            aside.why = own_life::aside::missing;
            aside.aside_at = current_tick();
            s.own_unanswered_.push_back(aside);
            while (s.own_unanswered_.size() > 16) s.own_unanswered_.erase(s.own_unanswered_.begin());
            bmmo::session_event_msg never;
            never.tick = never.origin_tick = life.origin;
            never.type = bmmo::session::event_type::Physicalize;
            never.state = static_cast<uint8_t>(bmmo::session::EVENT_STATE_REJECTED);
            never.ball_type = life.ball_type;
            never.flags = static_cast<uint8_t>(life.spawn ? bmmo::session::PHYSICALIZE_FLAG_SPAWN : 0);
            std::copy(std::begin(life.position), std::end(life.position), never.position);
            std::copy(std::begin(life.rotation), std::end(life.rotation), never.rotation);
            never.recipe = life.recipe;
            journal_.lifecycle_event(s.own_id_, never);
            journal_.note("own echo: Physicalize stamped " + std::to_string(life.origin) + " missing at the server's tick "
                          + std::to_string(server_tick));
            resync_own_life(life, false, life.origin, "no echo by the server's tick " + std::to_string(server_tick));
        }
    }

    // The owner's answer when its history cannot carry what the server
    // did with an own birth (late-player design 3.3 step 1): a resync of
    // this client alone.  The full snapshot only sets the poses of the
    // bodies both sides have, so the own body set is put right first: a
    // birth the server never made goes, from the records too (no rewind
    // before the assign brings it back); one the server made but this
    // client has taken out is made again.  A lifetime that ended since is
    // left alone: the server's has ended too (its death came after T').
    //
    // Without own body control (design 2.3) the own body is never touched:
    // a birth the server never made, while its life is current, is
    // re-reported at the next tick without the spawn flag (LPD 3.5: the
    // REJECTED echo makes it immediate) - not a superseded one, which a
    // newer accepted event of ours overtook - and then the resync.
    void session_client_core::resync_own_life(own_life& life, bool made, uint32_t applied, const std::string& why) {
        const uint32_t now = current_tick();
        const char* what = life.what == own_life::kind::beam ? "beam" : "birth";
        const std::string head = std::string("own ") + what + " moved " + std::to_string(life.origin) + " -> "
                               + (made ? std::to_string(applied) : std::string("never"));
        if (life.what == own_life::kind::birth && own_life_current(life)) {
            if (!caps_.own_body_control) {
                if (!made && life.state != own_life::verdict::superseded) re_report_own_birth(life, now);
            } else {
                bmmo_physics_body_state state{};
                const bool there = platform_.body_state(life.entity, state);
                if (!made) {
                    s.rollback_.move_birth(life.entity, life.generation, life.origin, life.origin, false);
                    if (there) unmake_own_body(life, now, "never made");
                    s.own_physicalized_ = false;
                    s.own_body_entity_.clear();
                } else if (!there && remake_own_body(life, now, "resync")) {
                    ++s.own_remakes_;
                    s.own_body_entity_ = life.entity;
                }
            }
        }
        life.retired = true;
        ++s.own_moves_resynced_;
        logf("%s at local tick %u: %s, resync", head.c_str(), now, why.c_str());
        journal_.note(head + ": " + why + ", resync");
        request_resync("own birth: resync", true);
    }

    // Design 2.3, without own body control: the birth's Physicalize sent
    // again for the next tick, as it was but for the spawn flag (no kick:
    // the body is long made here).  Its echo answers nothing (no record):
    // the resync that follows renumbers past it.
    void session_client_core::re_report_own_birth(const own_life& life, uint32_t now) {
        bmmo::session_event_msg again;
        again.tick = now + 1;
        again.type = bmmo::session::event_type::Physicalize;
        again.ball_type = life.ball_type;
        std::copy(std::begin(life.position), std::end(life.position), again.position);
        std::copy(std::begin(life.rotation), std::end(life.rotation), again.rotation);
        again.recipe = life.recipe;
        journal_.note("own birth stamped " + std::to_string(life.origin) + " never made: re-reported for tick "
                      + std::to_string(now + 1) + " (no own body control)");
        send_event(again);
    }

    // The server made our own birth stamped T at T' > T (`made`), or never
    // (late-player design 3.3).  The history is put where the server's
    // world is: no own body from T to T'-1, the body made again at T' with
    // the impulse index of T', in this replay and in every later one that
    // crosses T' (apply_own_lives).  Steps:
    //
    //   1. a rewind to T-1 must still be possible (inside the reach, T-1
    //      recorded); otherwise this client resyncs itself, alone
    //      (resync_own_life);
    //   2. the live body of a birth goes now (a beam's body is the
    //      anchor's, and the replay takes it out at its stamp), with the
    //      navigation replica shut down first and the entity put at the
    //      captured pose;
    //   3. the records lose the body (rollback_engine::move_birth): from T
    //      to T'-1 there was none, from T' on the replay re-captures it;
    //   4. the rewind to T-1 replays with that body set, and the body is
    //      made at T' by drain_pending_births, at our join rank - or by
    //      the live frame of T' when the client has not reached it yet.
    //
    // A lifetime that ended since (a death or a trafo after the birth)
    // and that the server made: the server's ball lived T'..D, and
    // nothing in a replay could end a re-made one at D (the own death's
    // move is plan step 22), so the client resyncs.  One the server
    // never made is removed from the history from T on: the server had
    // no ball, and neither has the replay.
    //
    // Without own body control (design 2.3) nothing moves: a moved birth
    // is the snapshot rollback's to reconcile when snapshots run (the
    // server's ball exists from T'), a resync otherwise; a superseded one
    // needs nothing; a rejected one is re-reported while current, and the
    // client resyncs (resync_own_life).
    void session_client_core::move_own_birth(own_life& life, uint32_t applied, bool made) {
        if (!caps_.own_body_control) {
            const std::string head = std::string("own ") + (life.what == own_life::kind::beam ? "beam" : "birth")
                                   + " moved " + std::to_string(life.origin) + " -> "
                                   + (made ? std::to_string(applied) : std::string("never"));
            if (made && snapshots_run()) {
                journal_.note(head + ": no own body control, left to the snapshots");
                return;
            }
            if (!made && life.state == own_life::verdict::superseded) {
                journal_.note(head + ": superseded, no own body control: nothing to do");
                return;
            }
            resync_own_life(life, made, applied, made ? "no own body control, no snapshots" : "no own body control");
            return;
        }
        const uint32_t now = current_tick();
        const uint32_t anchor = life.origin - 1;
        const char* what = life.what == own_life::kind::beam ? "beam" : "birth";
        const std::string reason = std::string("own ") + what + " moved " + std::to_string(life.origin) + " -> "
                                 + (made ? std::to_string(applied) : std::string("never"));
        if (!config.rollback || static_cast<int32_t>(now - anchor) < 0
                || now - anchor > s.rollback_.thresholds().max_resim_ticks || !s.rollback_.knows_tick(anchor)) {
            resync_own_life(life, made, applied, "past the reach (depth " + std::to_string(static_cast<int>(now - anchor)) + ")");
            return;
        }
        const bool alive = own_life_current(life);
        if (made && !alive) {
            resync_own_life(life, made, applied, "its life has ended since (plan step 22)");
            return;
        }
        life.made = made;
        life.applied = made ? applied : life.origin;
        if (life.what == own_life::kind::birth && alive) {
            bmmo_physics_body_state state{};
            if (platform_.body_state(life.entity, state)) unmake_own_body(life, now, "moved");
        }
        const size_t changed = s.rollback_.move_birth(life.entity, life.generation, life.origin, life.applied, made);
        if (!made) {
            ++s.own_births_removed_;
            if (life.what == own_life::kind::birth) life.retired = true;   // a beam's stamp still takes the old body out
        }
        // The replay below makes it when it reaches T'; past the live
        // tick the live frame of T' does.
        life.applied_live = !made || static_cast<int32_t>(life.applied - now) <= 0;
        if (life.what == own_life::kind::birth && alive && !life.applied_live) {
            s.own_physicalized_ = false;
            s.own_body_entity_.clear();
        }
        auto input_at = [this](const std::string& entity, uint32_t tick, bmmo::session::input_frame& out) {
            return recorded_input_at(entity, tick, out);
        };
        const scope_guard keep_cells = platform_.keep_live_cells(*this);
        if (!s.rollback_.rewind_to(world(), anchor, now, input_at, reason)) {
            // The body set put back the way the server has it, then the
            // resync (a refusal leaves the world at the live tick).
            resync_own_life(life, made, applied, "the rewind to " + std::to_string(anchor) + " was refused");
            return;
        }
        ++s.life_rewinds_;
        s.life_rewind_depth_ += now - anchor;
        if (made) ++s.own_births_moved_;
        drop_carried_own_replay(anchor, false);
        if (life.what == own_life::kind::birth && !made) {
            s.own_physicalized_ = false;
            s.own_body_entity_.clear();
        }
        logf("own physicalize stamped %u %s by the server at local tick %u: rewound to %u (%zu records changed%s)",
             life.origin, made ? ("moved to " + std::to_string(applied)).c_str() : "never made", now, anchor, changed,
             made ? (life.applied_live ? ", made again in the replay" : ", made again live at that tick")
                  : alive ? ", removed from the history" : ", its life has ended since: removed from the history");
        journal_.note(reason + ": rewound to " + std::to_string(anchor));
    }

}
