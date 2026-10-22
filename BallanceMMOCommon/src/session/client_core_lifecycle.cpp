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
                    legacy.live_own_lives(frame_tick);
                    own_placed = true;
                }
                apply_event(event);
            }
        }
        legacy.live_own_lives(frame_tick);   // applies once; afterwards only retires
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
        if (!config.rollback || event_tick == 0) return;
        const uint32_t now = current_tick();
        if (static_cast<int32_t>(event_tick - now) > 0) return;   // not late: nothing to replay
        auto input_at = [this](const std::string& entity, uint32_t tick, bmmo::session::input_frame& out) {
            return legacy.recorded_input_at(entity, tick, out);
        };
        const std::string reason = "ball of " + std::to_string(player) + " born at tick "
                                 + std::to_string(event_tick);
        const scope_guard keep_cells = platform_.keep_live_cells(*this);
        if (s.rollback_.rewind_to(platform_.bind_world(), event_tick - 1, now, input_at, reason)) {
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
        if (config.rollback && event.tick != 0 && static_cast<int32_t>(event.tick - now) <= 0
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
        physicalize_remote(pending_birth::of(event), live_first_tick(event.tick));
        rewind_for_lifecycle(event);
    }

    void session_client_core::apply_event(const bmmo::session_event_msg& event) {
        if (event.player == 0 || event.player == s.own_id_) return;
        journal_.lifecycle_event(event.player, event);
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
            s.sectors_.stamp(event.player, event.sector, event.tick);
            logf("remote player %u entered sector %d, stamped for tick %u%s", event.player, event.sector, event.tick,
                 static_cast<int32_t>(event.tick - current_tick()) <= 0 ? " (LATE: the server started it already)" : "");
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
        if (s.pending_births_.empty() && !legacy.own_lives_due(tick)) return;
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
        bool own = legacy.own_lives_due(tick);
        const uint32_t own_rank = join_rank(s.own_id_);
        for (const auto& birth: due) {
            if (own && join_rank(birth.player) > own_rank) {
                legacy.apply_own_lives(tick, "replay", false);
                own = false;
            }
            {
                const event_log_mute mute(*this);
                physicalize_remote(birth, tick);
            }
            s.mechanism_tracking_.mark_dirty();
        }
        if (own) legacy.apply_own_lives(tick, "replay", false);
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
    void session_client_core::physicalize_remote(const pending_birth& event, uint32_t first_tick) {
        std::string error;
        {
            const std::string entity = platform_.mirror_entity(event.player, event.ball_type, error);
            if (entity.empty()) { logf("remote physicalize: %s", error.c_str()); return; }
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
                return;
            }
            // The mirror's body is new: a new lifetime, so no record of the
            // previous one can restore it (design 9.25).
            ++s.generations_[entity];
            remote.entity = entity;
            remote.ball_type = event.ball_type;
            remote.physicalized = true;
            remote.born_tick = first_tick;
            remote.driven_from = first_tick;
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
    }

    void session_client_core::attach_remote_navigation(uint32_t player, remote_ball& remote) {
        if (!remote.physicalized || remote.navigation) return;
        std::string error;
        const mirror_navigation attached = platform_.attach_mirror_navigation(player, remote.entity, remote.ball_type, error);
        if (attached == mirror_navigation::unavailable) return;
        if (attached == mirror_navigation::attached) {
            remote.navigation = true;
            remote.corrector.clear();
            logf("remote %s now predicted", remote.entity.c_str());
        } else {
            logf("remote navigation for %s: %s", remote.entity.c_str(), error.c_str());
        }
    }

}
