// The shared session-client core (docs/session-client-core-design.md 1.1):
// the lifecycle.  Step E6 moves the remote lifecycle out of the headless
// client (session_client.cpp at 8d933ad): SessionEvent's intake, the drain at
// the due tick (drain_due_events) and the held body changes
// (apply_held_lifecycle), apply_event, make_remote_birth,
// rewind_for_lifecycle, the births deferred into a replay
// (drain_pending_lives since step EL7), live_drive, and the core half of the mirrors'
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
#include <cstring>
#include <map>
#include <memory>
#include <set>
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
        if (msg.player != s.own_id_ && msg.type == bmmo::session::event_type::Sector)
            s.window_sector_margins_[msg.player].push_back(static_cast<int64_t>(msg.tick) - current_tick());
        // Step T3 (trafo-replication design 2.4): a member first seen in a
        // message gets its piece set now, outside any replay.  Step TRL: the
        // set of the life the event names, and only for a life this client
        // knows (an unknown one is parked below; its member message makes
        // the set) and for an event at or after this numbering's base (the
        // state names the sets before it: a catch-up of a life that left
        // before this client joined makes none).  A Trafo event's set is
        // on_trafo_event's.
        if (s.trafo_active_ && msg.player != 0 && msg.player != s.own_id_
                && msg.type != bmmo::session::event_type::Trafo
                && !(s.assigned_ && static_cast<int32_t>(msg.tick - s.tick_base_) < 0)) {
            if (msg.life == 0) {
                const uint16_t life = trafo_life_at(msg.player, msg.tick);
                ensure_trafo_set(msg.player, life ? life : uint16_t{1}, "its first event");
            } else if (bmmo::session::find_member_life(s.lives_, msg.player, msg.life)) {
                ensure_trafo_set(msg.player, msg.life, "its first event");
            }
        }
        // A Trafo event (protocol 11) is an input of the trafo log, not a
        // body change of the drain: our own (not server-stamped) is an echo
        // for the frame; another member's, and a server-stamped reset of
        // our own set, are logged at their applied tick now (design 4.6).
        // Step TRL: one of our own id's earlier life in this session (a
        // rejoin on the same connection) is no echo of ours - it acts on
        // that life's set, like another member's input.
        if (msg.type == bmmo::session::event_type::Trafo) {
            const bool other_life = msg.life != 0 && s.own_member_life_ != 0 && msg.life != s.own_member_life_;
            if (msg.player == s.own_id_ && s.own_id_ != 0 && !(msg.flags & bmmo::session::TRAFO_SERVER) && !other_life) {
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
            // Step M1: a repeat of an event of our previous life in this
            // session (a rejoin on the same connection) is no echo of ours.
            if (msg.life != 0 && s.own_member_life_ != 0 && msg.life != s.own_member_life_) {
                ++s.own_other_life_echoes_;
                return;
            }
            s.own_echoes_.push_back(std::move(msg));
            return;
        }
        // Step M1 (world-agreement-gaps-design.md 2.8, the refusal rule): an
        // event of a life this client has not been told of is parked until
        // its SessionMember arrives - no mirror is ever made with a
        // placeholder join order.  The one exception is a late joiner's
        // catch-up: a Sector stamp before this numbering's first tick of a
        // life that left before it joined (the server resends its whole
        // sector history; J4: its world never needs the life, only the
        // sectors the union grew by), which starts live as before.
        if (const auto* life = bmmo::session::find_member_life(s.lives_, msg.player, msg.life); !life) {
            if (msg.type == bmmo::session::event_type::Sector && s.assigned_
                    && static_cast<int32_t>(msg.tick - s.tick_base_) < 0) {
                ++s.catchup_unknown_lives_;
            } else {
                ++s.events_parked_;
                if (s.events_parked_ <= 50)
                    logf("member lives: event %d of p%u life %u for tick %u parked: its life was not announced yet",
                         static_cast<int>(msg.type), msg.player, static_cast<unsigned>(msg.life), msg.tick);
                s.parked_events_[{msg.player, msg.life}].push_back(std::move(msg));
                return;
            }
        } else if (!bmmo::session::member_life_covers(*life, msg.tick)) {
            // J3 makes it impossible; applied as stamped (the server did).
            ++s.member_life_outside_;
            logf("member lives: event %d of p%u life %u at tick %u is outside the life [%u, %u)", static_cast<int>(msg.type),
                 msg.player, static_cast<unsigned>(msg.life), msg.tick, life->from, life->until);
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
                             const uint32_t ja = join_rank(a.player, a.tick), jb = join_rank(b.player, b.tick);
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
                                       || event.type == bmmo::session::event_type::Unphysicalize
                                       || event.type == bmmo::session::event_type::AirshipRelease)) {
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
            const uint32_t own_rank = join_rank(s.own_id_, frame_tick);
            bool own_placed = false;
            for (const auto& event: held) {
                if (!own_placed && join_rank(event.player, event.tick) > own_rank) {
                    live_own_lives(frame_tick);
                    own_placed = true;
                }
                apply_event(event);
            }
        }
        live_own_lives(frame_tick);   // applies once; afterwards only retires
    }

    // A member's place in the server's order of a tick's events: the join
    // order of its life that `tick` names (step M1: J3, an event of a life
    // is applied inside it, and two lives of one id never overlap), an
    // unknown player after every known one - unreachable since events of
    // an unknown life are parked.
    uint32_t session_client_core::join_rank(uint32_t player, uint32_t tick) const {
        const auto* life = bmmo::session::member_life_at(s.lives_, player, tick);
        return life ? static_cast<uint32_t>(life->entry.join_order) : 256u;
    }

    int session_client_core::mirror_join_order(uint32_t player, uint32_t tick) {
        if (const auto* life = bmmo::session::member_life_at(s.lives_, player, tick)) return life->entry.join_order;
        ++s.mirrors_outside_lives_;
        logf("member lives: a mirror of p%u for tick %u has no life: grouped and kicked as 63", player, tick);
        return 63;
    }

    // ------------------------------------------------------ member lives
    // SessionMember (step M1, world-agreement-gaps-design.md 2.8).
    void session_client_core::on_session_member(const bmmo::session_member_msg& msg) {
        ++s.member_messages_;
        if (msg.session != s.session_ || s.phase_ == client_core_state::phase::idle) return;
        const auto& entry = msg.entry;
        const char* kind = msg.kind == bmmo::session::MEMBER_ADDED ? "added" : "removed";
        if (entry.id == s.own_id_) {
            logf("member lives: SessionMember %s names our own id (life %u): ignored", kind, static_cast<unsigned>(entry.life));
            return;
        }
        if (msg.kind == bmmo::session::MEMBER_ADDED) {
            if (auto* known = bmmo::session::find_member_life(s.lives_, entry.id, entry.life)) {
                if (known->entry.join_order == entry.join_order) {
                    ++s.member_copies_;
                    return;
                }
                const std::string why = "member " + std::to_string(entry.id) + " life " + std::to_string(entry.life)
                    + " re-announced with join order " + std::to_string(entry.join_order) + ", had "
                    + std::to_string(known->entry.join_order);
                logf("member lives: %s: the session ends here", why.c_str());
                end(why);
                return;
            }
            bmmo::session::member_life life;
            life.entry = entry;
            life.from = msg.tick;
            if (const auto early = s.early_removals_.find({entry.id, entry.life}); early != s.early_removals_.end()) {
                life.until = early->second;
                s.early_removals_.erase(early);
            }
            s.lives_.push_back(life);
            logf("member lives: member +%u life %u join %u (server tick %u)%s", entry.id, static_cast<unsigned>(entry.life),
                 static_cast<unsigned>(entry.join_order), msg.tick, life.until ? " (its removal came first)" : "");
            journal_.note("member +p" + std::to_string(entry.id) + " life " + std::to_string(entry.life) + " join "
                          + std::to_string(entry.join_order) + " (server tick " + std::to_string(msg.tick) + ")");
            journal_member_lives();
            // FULL step GHF: an early removal forgets its group only now,
            // with the life it closes known (its join order).
            if (life.until) queue_spawn_forget(life);
            if (s.phase_ != client_core_state::phase::running) return;
            // Step T3 (trafo-replication design 2.4): its piece set, outside
            // any replay ("a player first seen in a message").
            if (s.trafo_active_) ensure_trafo_set(entry.id, entry.life ? entry.life : uint16_t{1}, "its member message");
            // Its parked events go to the drain, each at its tick; one whose
            // tick has passed takes the late path there (a rewind to its
            // stamp, or a resync beyond the reach), now with its life's join
            // order, group and kick.
            if (const auto parked = s.parked_events_.find({entry.id, entry.life}); parked != s.parked_events_.end()) {
                auto events = std::move(parked->second);
                s.parked_events_.erase(parked);
                logf("member lives: %zu parked event(s) of p%u life %u released", events.size(), entry.id,
                     static_cast<unsigned>(entry.life));
                for (auto& event: events) {
                    ++s.parked_applied_;
                    if (!bmmo::session::member_life_covers(life, event.tick)) ++s.member_life_outside_;
                    s.pending_events_.push_back(std::move(event));
                }
            }
            return;
        }
        auto* known = bmmo::session::find_member_life(s.lives_, entry.id, entry.life);
        if (!known) {
            // Its `added` copies are all late: kept and applied when it comes.
            s.early_removals_[{entry.id, entry.life}] = msg.tick;
            logf("member lives: member -%u life %u (server tick %u) before its addition: kept", entry.id,
                 static_cast<unsigned>(entry.life), msg.tick);
            return;
        }
        if (known->until != 0) {
            ++s.member_copies_;
            return;
        }
        known->until = msg.tick;
        logf("member lives: member -%u life %u join %u (server tick %u)", entry.id, static_cast<unsigned>(entry.life),
             static_cast<unsigned>(known->entry.join_order), msg.tick);
        journal_.note("member -p" + std::to_string(entry.id) + " life " + std::to_string(entry.life) + " (server tick "
                      + std::to_string(msg.tick) + ")");
        journal_member_lives();
        queue_spawn_forget(*known);   // FULL step GHF
    }

    // ------------------------------------------- a leaver's spawn ghost group
    // FULL step GHF (host-platform audit D2; world-agreement-gaps design
    // "As built (step GHF)").  The server drops a leaver's group from its
    // spawn ghost records at remove_player (physics_world.cpp, step SPGH2):
    // in session_runner::step that is finish_leave at tick R - after the
    // proximity union's placement, before R's events and R's step - and the
    // SessionMember `removed` it sends carries R.  Every client's world
    // drops it at the same place of R: live when R is still ahead (the
    // message comes a lead and a trip after R, so live it is late in
    // practice), else live at the next frame's place, the records of
    // [R, that frame) captured without it; every replay that steps R drops
    // it again there (a restore before R put the leaver's record back), and
    // a replay that restores a record of [R, that frame) drops it before
    // its first replayed tick.  A resync's server state at an anchor at or
    // after R holds it already.  No journal line and no record change: the
    // journals move only where a world's collisions do (a member that takes
    // the freed order with a first birth that is not a spawn - a ghost in
    // every world now, as in the server's).
    //
    // The order this needs: no body of the group born at or after R before
    // the forget.  The members' messages and events are reliable and
    // ordered, the taker's `added` goes out after the leaver's `removed`
    // and its events are parked until its `added`, so its body never comes
    // first - except through an unreliable repeat that overtook the
    // reliable copy (or a held test message): then the entry is `taken`
    // and the world rewinds to R - 1 (rewind_for_spawn_forgets; a resync
    // beyond the reach), and the replay drops the group at R and makes the
    // taker's body again after it.
    void session_client_core::queue_spawn_forget(const bmmo::session::member_life& life) {
        if (!life.until) return;
        for (const auto& f: s.spawn_forgets_)
            if (f.player == life.entry.id && f.life == life.entry.life) return;   // never twice for a life
        client_core_state::spawn_group_forget f;
        f.player = life.entry.id;
        f.life = life.entry.life;
        f.group = "P#" + std::to_string(life.entry.join_order);
        f.until = life.until;
        ++s.spawn_forgets_queued_;
        // A server state (a resync's) at an anchor at or after R has the
        // record dropped already: nothing to do in the world as it stands.
        if (s.spawn_forget_server_state_ && static_cast<int32_t>(f.until - s.spawn_forget_server_anchor_) <= 0) {
            f.in_world = true;
            f.lack_end = s.spawn_forget_server_anchor_;
            ++s.spawn_forgets_server_;
        }
        // A body of the order born at or after R by another life: this world
        // stepped it with the leaver's record (see above).
        if (!f.in_world)
            for (const auto& [id, remote]: s.remotes_) {
                if (!remote.physicalized || static_cast<int32_t>(remote.born_tick - f.until) < 0) continue;
                const auto* owner = bmmo::session::member_life_at(s.lives_, id, remote.born_tick);
                if (!owner || owner->entry.join_order != life.entry.join_order) continue;
                if (owner->entry.id == f.player && owner->entry.life == f.life) continue;
                f.taken = true;
                logf("spawn ghost: %s of p%u life %u (removed at server tick %u) was re-taken by p%u life %u, born at tick "
                     "%u before the removal came: the world rewinds to tick %u", f.group.c_str(), f.player,
                     static_cast<unsigned>(f.life), f.until, id, static_cast<unsigned>(owner->entry.life), remote.born_tick,
                     f.until - 1);
                break;
            }
        s.spawn_forgets_.push_back(std::move(f));
    }

    // True when the operation ran (the rule on and the operation bound),
    // whether or not the world still had a record to drop.
    bool session_client_core::apply_spawn_forget(const client_core_state::spawn_group_forget& f, uint32_t tick,
                                                 const char* where) {
        if (!s.spawn_ghost_active_ || !platform_.spawn_ghost_forget) return false;
        const bool dropped = platform_.spawn_ghost_forget(f.group);
        if (dropped) ++s.spawn_forgets_dropped_;
        logf("spawn ghost: %s of p%u life %u forgotten at tick %u (removed at server tick %u, %s)%s", f.group.c_str(),
             f.player, static_cast<unsigned>(f.life), tick, f.until, where, dropped ? "" : ": no record");
        return true;
    }

    // The live frame's forgets, at the server's place of the tick: after
    // the rewinds and the proximity union's placement, before the tick's
    // body changes (pre_step).  An entry no restore can reach any more
    // goes.
    void session_client_core::forget_spawn_groups(uint32_t tick) {
        for (auto& f: s.spawn_forgets_) {
            if (f.in_world || static_cast<int32_t>(f.until - tick) > 0) continue;
            const bool on_time = f.until == tick;
            if (apply_spawn_forget(f, tick, on_time ? "live" : "live, late"))
                ++(on_time ? s.spawn_forgets_on_time_ : s.spawn_forgets_late_);
            f.in_world = true;
            f.lack_end = tick;
        }
        const uint32_t keep = config.rollback ? static_cast<uint32_t>(s.rollback_.thresholds().history_ticks) + 2u : 0u;
        s.spawn_forgets_.erase(std::remove_if(s.spawn_forgets_.begin(), s.spawn_forgets_.end(),
            [tick, keep](const client_core_state::spawn_group_forget& f) {
                return f.in_world && !f.taken && static_cast<int32_t>(tick - f.lack_end) > static_cast<int32_t>(keep);
            }), s.spawn_forgets_.end());
    }

    // A replayed tick's forgets, at the engine's point before its births
    // (core_world_hooks::before_replayed_births, after the union's
    // placement: the server's finish_leave place).  The tick after the last
    // one seen continues a replay; any other begins one (the cursor is
    // dropped at every live frame and before every restore), and its
    // anchor's record was just restored.
    void session_client_core::forget_spawn_groups_replayed(uint32_t tick) {
        const bool first = !s.spawn_forget_cursor_valid_ || tick != s.spawn_forget_cursor_ + 1;
        s.spawn_forget_cursor_ = tick;
        s.spawn_forget_cursor_valid_ = true;
        if (s.spawn_forgets_.empty()) return;
        const uint32_t anchor = tick - 1;
        for (auto& f: s.spawn_forgets_) {
            if (first) {
                if (static_cast<int32_t>(f.until - anchor) > 0) {
                    // Restored before R: the leaver's record is back, as it
                    // was then; dropped again when the replay steps R.
                    f.in_world = false;
                } else if (s.resync_restoring_) {
                    // The server's state at the anchor holds it.
                    if (!f.in_world || static_cast<int32_t>(f.lack_end - anchor) > 0) f.lack_end = anchor;
                    f.in_world = true;
                } else if (!f.in_world || static_cast<int32_t>(anchor - f.lack_end) < 0) {
                    // A record of [R, lack_end) came back: captured before
                    // the live forget.
                    if (apply_spawn_forget(f, tick, "replay, restored without it")) ++s.spawn_forgets_restored_;
                    f.in_world = true;
                    f.lack_end = tick;
                }
            }
            if (tick == f.until) {
                if (apply_spawn_forget(f, tick, "replay")) ++s.spawn_forgets_replayed_;
                f.in_world = true;
                f.lack_end = f.until;
                // A replay that stepped R made every later birth of the
                // window after the forget: a re-taken order is settled (an
                // earlier rewind of the same pre_step, flush_life_rewinds).
                f.taken = false;
            }
        }
    }

    // An entry whose order was re-taken before it came (queue_spawn_forget):
    // the world back to R - 1, like a late death; the replay drops the group
    // at R and makes the taker's birth again after it.  Beyond the reach a
    // resync brings the server's records.  In pre_step, after the drain's
    // lifecycle rewinds.
    void session_client_core::rewind_for_spawn_forgets() {
        for (auto& f: s.spawn_forgets_) {
            if (!f.taken) continue;
            f.taken = false;
            const uint32_t now = current_tick();
            ++s.spawn_forget_rewinds_;
            if (config.rollback && static_cast<int32_t>(f.until - now) <= 0) {
                auto input_at = [this](const std::string& entity, uint32_t tick, bmmo::session::input_frame& out) {
                    return recorded_input_at(entity, tick, out);
                };
                const scope_guard keep_cells = keep_live_cells();
                if (rewind_world(f.until - 1, now, input_at,
                                 "spawn ghost group " + f.group + " forgotten at tick " + std::to_string(f.until)))
                    continue;
            }
            if (!s.resync_pending_) {
                logf("spawn ghost: %s forgotten at tick %u is outside the rollback history at local tick %u: resync",
                     f.group.c_str(), f.until, now);
                request_resync("a re-taken order's spawn ghost record beyond the reach", true);
            }
        }
    }

    // After a resync applied the server's state of `anchor` (the replay's
    // first tick saw it too; this also covers a re-step of no tick): an
    // entry at or before the anchor is in that state.
    void session_client_core::spawn_forgets_after_resync(uint32_t anchor) {
        s.spawn_forget_server_anchor_ = anchor;
        s.spawn_forget_server_state_ = true;
        for (auto& f: s.spawn_forgets_) {
            if (static_cast<int32_t>(f.until - anchor) > 0) continue;
            if (!f.in_world || static_cast<int32_t>(f.lack_end - anchor) > 0) f.lack_end = anchor;
            f.in_world = true;
            f.taken = false;
        }
    }

    // The PLAYER records of the lives: an add waits until every older life
    // of its id has closed (its removal written first), so a rejoin's
    // PLAYER(-) and PLAYER(+) at one tick are in that order in the file
    // whatever order their messages came in.  Nothing while the journal is
    // not open; client_journal::open writes them then.
    void session_client_core::journal_member_lives() {
        if (!journal_.journalling()) return;
        bool progress = true;
        while (progress) {
            progress = false;
            for (auto& life: s.lives_) {
                if (life.journaled_add && life.until != 0 && !life.journaled_remove) {
                    journal_.leave_player(life.entry.id, life.until);
                    life.journaled_remove = true;
                    progress = true;
                }
            }
            for (auto& life: s.lives_) {
                if (life.journaled_add) continue;
                // A life told of after SessionStart waits for the numbering
                // (its J is a server tick; a record before the base would sit
                // in the hole a late join or a resync leaves in the file).
                if (life.from != 0 && !s.assigned_) continue;
                bool older_open = false;
                for (const auto& other: s.lives_)
                    if (other.entry.id == life.entry.id && other.entry.life < life.entry.life && !other.journaled_remove)
                        older_open = true;
                if (older_open) continue;
                journal_.add_player(life.entry, life.from);
                life.journaled_add = true;
                progress = true;
            }
        }
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
    // and a body that went away is removed in the replay's pre-step of the
    // stamp tick, which is also where its owner and the server removed it.
    // Step DX11 (J4): the death is not applied live before the rewind any
    // more (remove_remote): the anchor's record holds the body in its
    // friction systems with every body it touched, and a restore into a
    // world without it dropped those systems whole and re-created the
    // other bodies' contacts fresh, where the server's IVP deletion kept
    // them (P_Modul_01_Pusher 0.0085 m at the next tick, 0.025 m at rest).
    //
    // A stamp older than the rollback history has nothing to rewind to: the
    // change stays where the drain made it, at the live tick, and a resync
    // brings the world to the server's (step DX8, world-agreement design
    // 7.1 U5 (c); until DX8 it waited for a snapshot, which under "assume
    // no snapshots" never came).  As the late Sector path: not while a
    // resync is pending (its state covers it) and not for a stamp at or
    // before this numbering's first tick (a late joiner's catch-up).
    bool session_client_core::rewind_for_lifecycle(const bmmo::session_event_msg& event) {
        return rewind_for_lifecycle(event.player, event.tick);
    }
    bool session_client_core::rewind_for_lifecycle(uint32_t player, uint32_t event_tick) {
        // lifecycle_rewind off (R1, retail until R3): the change stays where
        // the drain made it, at the live tick.
        if (!config.rollback || !config.lifecycle_rewind || event_tick == 0) return false;
        const uint32_t now = current_tick();
        if (static_cast<int32_t>(event_tick - now) > 0) return false;   // not late: nothing to replay
        // Step A3 (resync design 2.6.2): a late change before the synthetic
        // anchor of a pending exact resync - the ticks (R, base - 1] the
        // stall never ran, drained at base - is covered by the server's
        // state of base - 1: applied live, no rewind, no miss.
        if (s.resync_pending_ && s.resync_exact_ && static_cast<int32_t>(event_tick - s.resync_base_) < 0) {
            logf("lifecycle event of %u stamped tick %u precedes the resync's anchor %u: the server's state covers it", player,
                 event_tick, s.resync_base_ - 1);
            return false;
        }
        const std::string reason = "ball of " + std::to_string(player) + " born at tick "
                                 + std::to_string(event_tick);
        // Step SRT3 (realtime-design 4 a1): inside the drain, an event the
        // rewind would take joins the frame's batch - one restore to the
        // lowest anchor and one replay for all of them (flush_life_rewinds).
        // The last of N sequential rewinds replayed with every event known
        // already (the reconcile queues the window's logged events at each
        // restore), so the N - 1 before it only cost.  One that would be
        // refused is never batched (apply_event flushed the batch before it:
        // settle_life_batch_before) and is refused below as before.
        if (joins_life_batch(event_tick)) {
            auto& b = s.life_batch_;
            const uint32_t anchor = event_tick - 1;
            if (!b.pending || static_cast<int32_t>(anchor - b.anchor) < 0) b.anchor = anchor;
            b.reason += (b.pending ? "; " : "") + reason;
            b.pending = true;
            ++b.events;
            b.players.insert(player);
            return true;
        }
        auto input_at = [this](const std::string& entity, uint32_t tick, bmmo::session::input_frame& out) {
            return recorded_input_at(entity, tick, out);
        };
        const scope_guard keep_cells = keep_live_cells();
        // A stamp inside the last exact resync's re-step reaches as deep as
        // that re-step did, as an input correction there does (step A3): the
        // events of the window arrive with its frames, behind the state.
        if (rewind_world(event_tick - 1, now, input_at, reason, life_rewind_reach(event_tick))) {
            ++s.life_rewinds_;
            s.life_rewind_depth_ += now - (event_tick - 1);
            drop_carried_own_replay(event_tick - 1, false);
            return false;
        }
        ++s.life_rewinds_missed_;
        if (s.resync_pending_ || static_cast<int32_t>(event_tick - s.tick_base_) <= 0) {
            logf("lifecycle event of %u stamped tick %u is outside the rollback history at local tick %u: "
                 "applied live, %s", player, event_tick, now,
                 s.resync_pending_ ? "the pending resync covers it" : "before this numbering");
            return false;
        }
        logf("lifecycle event of %u stamped tick %u is outside the rollback history at local tick %u: "
             "applied live, resync", player, event_tick, now);
        request_resync("a late lifecycle event beyond the reach", true);
        return false;
    }

    bool session_client_core::joins_life_batch(uint32_t event_tick) const {
        if (!s.life_batch_.collecting || !config.rollback || !config.lifecycle_rewind || event_tick == 0) return false;
        const uint32_t now = current_tick();
        if (static_cast<int32_t>(event_tick - now) > 0) return false;
        if (s.resync_pending_ && s.resync_exact_ && static_cast<int32_t>(event_tick - s.resync_base_) < 0) return false;
        const uint32_t anchor = event_tick - 1;
        return s.rollback_.knows_tick(anchor) && now - anchor <= window_rewind_reach(event_tick);
    }

    // rewind_world's reach for a lifecycle stamp: 0 (the standard reach)
    // outside a re-step window, the window's reach inside one.
    uint32_t session_client_core::life_rewind_reach(uint32_t event_tick) const {
        const uint32_t reach = window_rewind_reach(event_tick);
        return reach > s.rollback_.thresholds().max_resim_ticks ? reach : 0;
    }

    // Step SRT3: before the drain applies an event of `player` stamped
    // `stamp`, the batch rewinds when the sequential path would have had
    // its replay run first and the event reads what it left: an
    // Unphysicalize of a player with an event in the batch (it reads the
    // mirror's body and entity, which that replay changes), and any
    // lifecycle event that will not join the batch (applied live, or
    // refused and resynced, between rewinds that ran before and after it).
    // A Physicalize that joins reads nothing a replay changes (it is queued
    // for the replay), so a beam pair's Unphysicalize and Physicalize, and
    // every other player's events, stay in one rewind.
    void session_client_core::settle_life_batch_before(uint32_t player, uint32_t stamp, bool death) {
        const auto& b = s.life_batch_;
        if (!b.collecting || !b.pending) return;
        if ((death && b.players.count(player)) || !joins_life_batch(stamp)) flush_life_rewinds();
    }

    // Step SRT3: the batch's one rewind (rewind_for_lifecycle's, from the
    // lowest anchor), then the fallbacks make_remote_birth and
    // remove_remote ran after each event's own rewind: a birth or death the
    // replay did not apply (a frozen world sets the bodies and replays
    // nothing) is applied live now, in the drain's order.
    void session_client_core::flush_life_rewinds() {
        if (!s.life_batch_.pending) return;
        client_core_state::life_rewind_batch batch;
        std::swap(batch, s.life_batch_);
        s.life_batch_.collecting = batch.collecting;
        const uint32_t now = current_tick();
        auto input_at = [this](const std::string& entity, uint32_t tick, bmmo::session::input_frame& out) {
            return recorded_input_at(entity, tick, out);
        };
        bool rolled = false;
        {
            const scope_guard keep_cells = keep_live_cells();
            // Step SRT4 (review finding 4): the replay ran from `restored` -
            // the anchor, or the whole record before a light one - and read
            // every correction after it, so a pending replay from there on
            // is carried too.
            uint32_t restored = batch.anchor;
            rolled = rewind_world(batch.anchor, now, input_at, batch.reason, life_rewind_reach(batch.anchor + 1),
                                  &restored);
            if (rolled) {
                ++s.life_rewinds_;
                s.life_rewind_depth_ += now - batch.anchor;
                drop_carried_own_replay(restored, false);
                // A remote's input correction from at or after the anchor
                // + 1 was in the rings this replay read (the relay's frames
                // land there on arrival): replayed already, not again.
                drop_carried_input_replay(restored);
            }
        }
        if (batch.events > 1) {
            s.life_events_coalesced_ += batch.events - 1;
            logf("lifecycle: %u late events taken by one rewind to tick %u%s", batch.events, batch.anchor,
                 rolled ? "" : " (refused)");
        }
        for (const auto& c: batch.checks) {
            if (!c.death) {
                for (auto it = s.pending_births_.begin(); it != s.pending_births_.end();)
                    if (it->player == c.player) {
                        logf("remote physicalize of %u: the replay did not reach tick %u, made live", c.player, c.stamp);
                        physicalize_remote(*it, live_first_tick(c.stamp));
                        it = s.pending_births_.erase(it);
                    } else ++it;
                continue;
            }
            for (auto it = s.pending_deaths_.begin(); it != s.pending_deaths_.end(); ++it)
                if (it->player == c.player && it->tick == c.stamp && !it->reconciled) {
                    s.pending_deaths_.erase(it);
                    logf("remote unphysicalize of %u: the replay did not reach tick %u, removed live", c.player, c.stamp);
                    unphysicalize_remote(c.player, c.stamp);
                    if (c.had_body)
                        note_life_made(c.player, bmmo::session::event_type::Unphysicalize, c.stamp, c.origin, c.entity,
                                       live_first_tick(c.stamp));
                    break;
                }
        }
        if (rolled) return;
        // Not reachable after joins_life_batch (rewind_to refuses only the
        // two cases it tests), kept as rewind_for_lifecycle's miss path.
        s.life_rewinds_missed_ += batch.events;
        logf("lifecycle: the rewind to tick %u for %u late event(s) was refused at local tick %u: applied live, %s",
             batch.anchor, batch.events, now, s.resync_pending_ ? "the pending resync covers them" : "resync");
        if (!s.resync_pending_) request_resync("a late lifecycle event beyond the reach", true);
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

    // Step DX11 (J4, world-agreement gaps "As built (step H4)"; FULL plan
    // shape (b): the client world runs the server's sequence).  A death
    // stamped in the past whose anchor (stamp - 1) is still in the history,
    // of a mirror with a body: queued for the replay (pending_deaths_), not
    // applied here - the rewind restores the anchor with the body present
    // (its friction systems intact) and drain_pending_lives removes it in
    // the pre-step of the stamp tick, where the server's remove_player
    // does, so IVP deletes only its own contacts.  The gate is
    // make_remote_birth's.  If the replay did not get that far (the rewind
    // refused or missed, an exact resync's anchor covers the stamp, a
    // frozen clock), it is removed now, live.  Any other death - a stamp
    // outside the history (rewind_for_lifecycle's resync, U5 (c)), on
    // time, or of a bodiless mirror - is applied live and rewound for, as
    // before.
    bool session_client_core::remove_remote(uint32_t player, uint32_t stamp, uint32_t origin, bool& replayed) {
        replayed = false;
        // Step SRT3: it reads the mirror's body, which an earlier event of
        // this player in the batch changes in its replay.
        settle_life_batch_before(player, stamp, true);
        const auto remote = s.remotes_.find(player);
        if (remote == s.remotes_.end()) return false;
        const uint32_t now = current_tick();
        if (remote->second.physicalized && config.rollback && config.lifecycle_rewind && stamp != 0
                && static_cast<int32_t>(stamp - now) <= 0 && s.rollback_.knows_tick(stamp - 1)) {
            client_core_state::pending_death death;
            death.player = player;
            death.tick = stamp;
            death.origin = origin;
            s.pending_deaths_.push_back(death);
            if (rewind_for_lifecycle(player, stamp)) {
                // Step SRT3: in the frame's batch - the fallback below runs
                // after its one rewind (flush_life_rewinds); joined, the
                // rewind replays past the stamp (a frozen world aside).
                client_core_state::life_rewind_batch::check c;
                c.player = player;
                c.stamp = stamp;
                c.origin = origin;
                c.death = true;
                c.entity = remote->second.entity;
                s.life_batch_.checks.push_back(c);
                replayed = true;
                return true;
            }
            replayed = true;
            for (auto it = s.pending_deaths_.begin(); it != s.pending_deaths_.end(); ++it)
                if (it->player == player && it->tick == stamp && !it->reconciled) {
                    s.pending_deaths_.erase(it);
                    replayed = false;
                    logf("remote unphysicalize of %u: the replay did not reach tick %u, removed live", player, stamp);
                    unphysicalize_remote(player, stamp);
                    break;
                }
            return true;
        }
        unphysicalize_remote(player, stamp);
        rewind_for_lifecycle(player, stamp);
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
        // Step SRT3: a birth that will not join the frame's batch is made or
        // refused after the batch's rewind, as after the earlier events' own.
        settle_life_batch_before(event.player, event.tick, false);
        const uint32_t now = current_tick();
        if (config.rollback && config.lifecycle_rewind && event.tick != 0 && static_cast<int32_t>(event.tick - now) <= 0
                && s.rollback_.knows_tick(event.tick - 1)) {
            s.pending_births_.push_back(pending_birth::of(event));
            if (rewind_for_lifecycle(event)) {
                // Step SRT3: the fallback below runs after the batch's
                // rewind (flush_life_rewinds).
                client_core_state::life_rewind_batch::check c;
                c.player = event.player;
                c.stamp = event.tick;
                c.origin = event.origin_tick;
                s.life_batch_.checks.push_back(c);
                return;
            }
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
        // Step SRT3 (review finding 1): the batch's rewind, when this event
        // needs it run first (settle_life_batch_before), runs before the
        // event is logged - as the sequential path's rewinds of the earlier
        // events did.  After log_life, the flushing rewind's reconcile would
        // read this not-yet-applied event (no entity) in its player's window
        // and queue it.  The later calls in remove_remote / make_remote_birth
        // find the batch settled already (or the event joining).
        if (event.type == bmmo::session::event_type::Physicalize || event.type == bmmo::session::event_type::Unphysicalize)
            settle_life_batch_before(event.player, event.tick, event.type == bmmo::session::event_type::Unphysicalize);
        journal_.lifecycle_event(event.player, event);
        // Step A3: the remote lifecycle by applied tick (life_log_).
        if (event.type == bmmo::session::event_type::Physicalize || event.type == bmmo::session::event_type::Unphysicalize)
            log_life(event);
        switch (event.type) {
        case bmmo::session::event_type::AirshipRelease:
            s.airship_releases_.push_back({event.player, event.tick, event.release});
            rewind_for_lifecycle(event);
            if (event.tick == current_tick() + 1) apply_airship_releases(event.tick, event.player);
            break;
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
                note_life_skipped(event.player, event.type, event.tick, event.origin_tick);
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
                bool replayed = false;   // step DX11: removed in the replay at its tick, as any late death
                remove_remote(event.player, later.applied, later.origin, replayed);
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
                note_life_skipped(event.player, event.type, event.tick, event.origin_tick);
                return;
            }
            // Newest by origin, with its applied tick: an older birth
            // relayed after it is put back in front of it (above).
            if (auto [death, fresh] = s.remote_last_unphysicalize_.try_emplace(
                        event.player, applied_death{event.origin_tick, event.tick});
                    !fresh && static_cast<int32_t>(event.origin_tick - death->second.origin) > 0)
                death->second = applied_death{event.origin_tick, event.tick};
            {
                // Step SRT3: the body and entity read below are what an
                // earlier event of this player in the frame's batch leaves.
                settle_life_batch_before(event.player, event.tick, true);
                const auto remote = s.remotes_.find(event.player);
                if (remote == s.remotes_.end()) {
                    note_life_skipped(event.player, event.type, event.tick, event.origin_tick);
                    return;
                }
                const std::string entity = remote->second.entity;
                const bool had_body = remote->second.physicalized;
                // Gap L, L2 (step EL7): what the death removed, for the
                // reconcile (a death of a mirror with no body changed none).
                // Step DX11: noted before its rewind, as when the death was
                // applied live first - the reconcile of that rewind reads
                // the entity (a window that begins with a death) - and
                // again after it, with the generation the removal gave and
                // the tick it was made at (the stamp when the replay made it).
                if (!had_body) note_life_skipped(event.player, event.type, event.tick, event.origin_tick);
                else note_life_made(event.player, event.type, event.tick, event.origin_tick, entity, live_first_tick(event.tick));
                bool replayed = false;
                remove_remote(event.player, event.tick, event.origin_tick, replayed);
                if (had_body) {
                    // Step SRT3: in the frame's batch `replayed` is the
                    // promise of its rewind (the stamp is inside it); its
                    // fallback notes the live tick again if the replay did
                    // not get there.  (The entry's generation is read for
                    // own lives only; a remote's is the value now.)
                    note_life_made(event.player, event.type, event.tick, event.origin_tick, entity,
                                   replayed ? event.tick : live_first_tick(event.tick));
                    auto& checks = s.life_batch_.checks;
                    if (replayed && s.life_batch_.pending && !checks.empty() && checks.back().death
                            && checks.back().player == event.player && checks.back().stamp == event.tick) {
                        checks.back().had_body = true;
                        checks.back().entity = entity;
                    }
                }
            }
            break;
        }
        default:
            break;
        }
    }

    // ---------------------------------------------------- pending lives
    // Births (and, since step EL7, deaths) deferred into a replay
    // (apply_event, the reconcile of an ended life, a resync's deaths):
    // applied at their tick, before its step.
    // The event log is muted around them: physicalize_remote bumps the
    // mirror's generation itself, and the replay labels the records it
    // re-captures from the birth tick on with that generation
    // (rollback_world::replay_remotes).  The "created" entry the live
    // frame would drain afterwards bumped it a second time, so the live
    // records after the replay named another lifetime than the replayed
    // ones and none of those could restore the body.
    // In the server's order (session_runner::step, drain_due_events): join
    // order, then origin, then lifecycle_event_rank (a beam pair's
    // Unphysicalize before its Physicalize), and our own changes due at
    // this tick (a beam's stamp, a moved birth's applied tick:
    // apply_own_lives) at ours - two bodies made in one tick are made in
    // that order and the order changes the world.  A beam's stamp is
    // applied here only once its live frame has run: before that no replay
    // reaches it.  (Before step EL7 the deaths, then only a resync's, came
    // after every birth of the tick.)
    void session_client_core::drain_pending_lives(uint32_t tick) {
        // Step SPGH2: an own spawn birth of this tick (its stamp, not moved:
        // a moved one is remade at its applied tick, remake_own_body) begins
        // the spawn ghost life again in the replay - the live frame's host
        // birth (own_birth_seen) does not run in a replay, and the
        // restore put the state back to the anchor's.
        if (s.spawn_ghost_active_)
            for (const auto& life: s.own_life_)
                if (!life.retired && life.made && life.what == own_life::kind::birth && life.spawn && !life.moved()
                    && life.origin == tick)
                    note_spawn_birth(life.group.empty() ? s.own_group_ : life.group, tick, "replayed own birth");
        const bool own_release = std::any_of(s.airship_releases_.begin(), s.airship_releases_.end(),
                [this, tick](const auto& release) { return release.tick == tick && release.player == s.own_id_; });
        const bool remote_release = std::any_of(s.airship_releases_.begin(), s.airship_releases_.end(),
                [this, tick](const auto& release) { return release.tick == tick && release.player != s.own_id_; });
        if (s.pending_births_.empty() && s.pending_deaths_.empty() && !own_lives_due(tick) && !own_release && !remote_release) {
            apply_own_due(tick);
            return;
        }
        struct due_life {
            uint32_t player = 0, origin = 0;
            bmmo::session::event_type type = bmmo::session::event_type::Physicalize;
            pending_birth birth;
        };
        std::vector<due_life> due;
        for (const auto& release: s.airship_releases_)
            if (release.tick == tick && release.player != s.own_id_) {
                due_life life;
                life.player = release.player;
                life.origin = tick;
                life.type = bmmo::session::event_type::AirshipRelease;
                due.push_back(std::move(life));
            }
        for (auto it = s.pending_births_.begin(); it != s.pending_births_.end();)
            if (it->tick == tick) {
                due_life life;
                life.player = it->player;
                life.origin = it->origin;
                life.birth = *it;
                due.push_back(std::move(life));
                it = s.pending_births_.erase(it);
            } else ++it;
        for (auto it = s.pending_deaths_.begin(); it != s.pending_deaths_.end();)
            if (it->tick == tick) {
                due_life life;
                life.player = it->player;
                life.origin = it->origin;
                life.type = bmmo::session::event_type::Unphysicalize;
                due.push_back(std::move(life));
                it = s.pending_deaths_.erase(it);
            } else ++it;
        std::stable_sort(due.begin(), due.end(), [this, tick](const due_life& a, const due_life& b) {
            return bmmo::session::lifecycle_apply_before(join_rank(a.player, tick), a.origin, a.type,
                                                         join_rank(b.player, tick), b.origin, b.type);
        });
        bool own = own_lives_due(tick) || own_release;
        const uint32_t own_rank = join_rank(s.own_id_, tick);
        for (const auto& life: due) {
            if (own && join_rank(life.player, tick) > own_rank) {
                apply_moved_own_deaths(tick);   // step ODM: the server's order, U before P
                apply_own_lives(tick, "replay", false);
                apply_airship_releases(tick, s.own_id_);
                own = false;
            }
            if (life.type == bmmo::session::event_type::AirshipRelease) {
                apply_airship_releases(tick, life.player);
                continue;
            }
            if (life.type == bmmo::session::event_type::Unphysicalize) {
                const event_log_mute mute(*this);
                unphysicalize_remote(life.player, tick);
                continue;
            }
            const pending_birth& birth = life.birth;
            bool made = false;
            {
                const event_log_mute mute(*this);
                made = physicalize_remote(birth, tick);
            }
            // A remake that failed (step LB), or a window birth the
            // reconcile queued (step EL7): its body was taken out at the
            // restore, so it is queued again - make_unmade_remakes, or
            // settle_unreplayed_lives after the rewind, tries once more,
            // live.
            if (!made && (birth.remake || birth.reconciled)) s.pending_births_.push_back(birth);
            s.mechanism_tracking_.mark_dirty();
        }
        if (own) {
            apply_moved_own_deaths(tick);   // step ODM
            apply_own_lives(tick, "replay", false);
            apply_airship_releases(tick, s.own_id_);
        }
        // Step EL8 (design 2.3.4 "In the replay at D"): the own ball's
        // queued changes last - after the remote events of the tick, before
        // its scripts (live, the retail scripts removed the ball in the
        // frame's behaviour pass, after the drain).
        apply_own_due(tick);
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
    // mirror_entity; every host gives one entity per player and ball type
    // since step BMX, game/mirror_clones.hpp, so it would name the same), its
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
            } else if (!event.entity.empty()) {
                // Gap L, L2 (step EL7): the entity the live event made (the
                // life log's), never a new one minted inside a replay.
                entity = event.entity;
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
            } else if (remote.physicalized && !remake) {
                // Step HLX: a Physicalize of the same entity while its body
                // lives - a beam whose Physicalize was applied before its
                // same-origin Unphysicalize (the copies of the pair arrived
                // in two drains: drain_due_events orders U before P only
                // within one).  A Physicalize always makes the body at its
                // pose, so the old life ends here as a trafo's does and the
                // death the pair carries would (unphysicalize_remote: the
                // navigation went above, the generation bumped, the body
                // out); the backend's physicalize of a live body is a
                // no-op, so without this the old body rolled on, in every
                // replay too.  The late Unphysicalize is then superseded
                // (equal origin) correctly.  Receivers must not depend on
                // copy order (lifecycle.hpp, protocol 6); the server's
                // world does the same (physics_world::apply_event).
                ++s.generations_[remote.entity];
                unphysicalize_mirror(remote.entity);
                remote.physicalized = false;
                if (!event.reconciled) {
                    ++s.births_replaced_live_;
                    logf("remote physicalize of %u stamped %u: its mirror %s was alive, the old body replaced",
                         event.player, event.tick, remote.entity.c_str());
                }
            }
            mirror_spec spec;
            spec.player = event.player;
            spec.ball_type = event.ball_type;
            spec.entity = entity;
            std::copy(std::begin(event.position), std::end(event.position), spec.position);
            std::copy(std::begin(event.rotation), std::end(event.rotation), spec.rotation);
            spec.recipe = to_bridge_recipe(event.recipe);
            const int join_order = mirror_join_order(event.player, event.tick);   // step M1: the life's
            spec.group = "P#" + std::to_string(join_order);
            // The spawn kick goes with the body (design 2.2 make_mirror): its
            // direction from the stamp, as the server derives it.
            if ((event.flags & bmmo::session::PHYSICALIZE_FLAG_SPAWN) && s.spawn_impulse_ > 0.0f) {
                spec.kick = true;
                spec.kick_index = bmmo::session::spawn_direction_index(s.seed_, static_cast<uint8_t>(join_order), event.tick);
                spec.kick_strength = s.spawn_impulse_;
            }
            // Step SPGH2: a spawn birth begins the ball's spawn ghost life (a
            // trafo's or a beam's Physicalize keeps it), as the server's
            // apply_event does at this tick.
            if (event.flags & bmmo::session::PHYSICALIZE_FLAG_SPAWN) note_spawn_birth(spec.group, event.tick, "mirror birth");
            const mirror_made made = platform_.make_mirror(spec);
            if (!made.made) {
                logf("remote physicalize %s: %s", entity.c_str(), made.error.c_str());
                return false;
            }
            s.mirror_entities_.insert(entity);
            s.mirror_owners_[entity] = {event.player, event.ball_type};   // step G3
            if (remake) {
                ++s.mirror_remakes_;
                remote.physicalized = true;
                attach_remote_navigation(event.player, remote, true);
                return true;
            }
            // The mirror's body is new: a new lifetime, so no record of the
            // previous one can restore it (design 9.25).
            ++s.generations_[entity];
            // Gap L, L2 (step EL7): the life log entry of this birth names
            // the entity it made, its generation and where.
            note_life_made(event.player, bmmo::session::event_type::Physicalize, event.tick, event.origin, entity, first_tick);
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
    mirror_navigation session_client_core::attach_remote_navigation(uint32_t player, remote_ball& remote, bool remake) {
        if (!remote.physicalized) return mirror_navigation::unavailable;
        if (remote.navigation) return mirror_navigation::attached;
        std::string error;
        const mirror_navigation attached = platform_.attach_mirror_navigation(player, remote.entity, remote.ball_type, error);
        if (attached == mirror_navigation::unavailable) return attached;
        if (attached == mirror_navigation::attached) {
            remote.navigation = true;
            if (remake) return attached;
            remote.corrector.clear();
            logf("remote %s now predicted", remote.entity.c_str());
        } else {
            logf("remote navigation for %s: %s", remote.entity.c_str(), error.c_str());
        }
        return attached;
    }


    // Step LB: a mirror born inside a rewind's window, taken out at the
    // restore (the binding's destroy_body) and made again at its birth tick
    // by drain_pending_lives, in join order with the other births of that
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
            // L-H6's control arm: every window birth parked.
            if (config.remote_births_parked) return false;
            // Gap L, L2 (rollback-engine gaps design 2.3.3, step EL7): a body
            // made live after its stamp keeps the parking path.  Counted
            // (late_births_kept) only for the RC stamp rule's documented
            // class - stamped at or before the anchor (lifecycle_rewind
            // off, or a refused rewind's fallback) - and once per birth: one
            // stamped inside the window is made again at its stamp by the
            // reconcile (its window lives) and never reaches this test.
            if (remote.physicalized && remote.born_tick == birth_tick && !remote.birth_known && remote.birth.tick != 0
                    && static_cast<int32_t>(remote.birth.tick - birth_tick) < 0
                    && static_cast<int32_t>(remote.birth.tick - s.lives_anchor_) <= 0
                    && s.late_births_counted_.emplace(player, remote.birth.tick).second)
                ++s.late_births_kept_;
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


    // ==================================================== gap L, L2 (EL7)
    // Rollback-engine gaps design 2.3.3: the mirror lives that end inside a
    // rewind's window.

    // The life log entry of a remote event just applied here (the newest
    // matching one): the entity it made or removed, the generation it gave
    // it and the tick it was made at.
    void session_client_core::note_life_made(uint32_t player, bmmo::session::event_type type, uint32_t stamp,
                                             uint32_t origin, const std::string& entity, uint32_t made_at) {
        for (auto it = s.life_log_.rbegin(); it != s.life_log_.rend(); ++it) {
            if (it->player != player || it->type != type || it->applied != stamp || it->origin != origin) continue;
            it->entity = entity;
            const auto generation = s.generations_.find(entity);
            it->generation = generation == s.generations_.end() ? 0u : generation->second;
            it->made_at = made_at;
            it->skipped = false;
            if (type == bmmo::session::event_type::Physicalize) it->birth.entity = entity;
            return;
        }
    }

    void session_client_core::note_life_skipped(uint32_t player, bmmo::session::event_type type, uint32_t stamp,
                                                uint32_t origin) {
        for (auto it = s.life_log_.rbegin(); it != s.life_log_.rend(); ++it)
            if (it->player == player && it->type == type && it->applied == stamp && it->origin == origin) {
                if (it->entity.empty()) it->skipped = true;
                return;
            }
    }

    // The player whose mirror `entity` is now, or was (its newest logged
    // Physicalize): recorded_input_at reads that player's ring for a life
    // whose name is not the current one (H names one clone per life).
    uint32_t session_client_core::mirror_player_of(const std::string& entity) const {
        if (entity.empty()) return 0;
        for (const auto& [id, remote]: s.remotes_)
            if (remote.entity == entity) return id;
        for (auto it = s.life_log_.rbegin(); it != s.life_log_.rend(); ++it)
            if (it->type == bmmo::session::event_type::Physicalize && it->entity == entity) return it->player;
        return 0;
    }

    // The core's half of rollback_world::restore_lives for the mirrors
    // (2.3.3 "The reconcile").  The engine asks it at every restore a
    // replay follows, with the lives alive at `anchor` (A) that end inside
    // (A, now] (possibly none).
    //
    // First, per player whose mirror life alive at A ends in the window:
    //   the A-life is the player's newest logged Physicalize of that entity
    //   stamped <= A with no event of the player stamped in (birth, A];
    //   the window events are the player's logged events stamped in
    //   (A, now] that changed a body, in the server's order;
    //   1. the player's body now (a later life: the A-life ended) is taken
    //      out, muted, its navigation detached;
    //   2. the A-life is made again from its Physicalize at the A record's
    //      pose, no kick (the engine then writes its A record, or the blob
    //      covers it), navigated, and the remote is that life again;
    //      reported revived until its first window event;
    //   3. every window event is queued at its stamp (births with the
    //      entity the live event made); each born entity is reported
    //      remade at its last window birth (the life its birth item, if
    //      any, is about).
    // The generations of the A-life and of every entity the window made
    // are put back to A's (absent there: erased), so the replayed events
    // bump them to the live values; a life whose revive failed is put one
    // past A's (its end bumps nothing without a body).  A player with no
    // log entry, an inconsistent log, no window event, or (the parked arm)
    // a body now is left unanswered: the engine counts it unrevivable.
    //
    // Then (review finding 1), per other player with logged events in the
    // window that step LB's remake and a late birth's own queue cannot
    // replay - a life that began AND ended inside the window (no record at
    // A, no body now: no restore item names it), two births in it (a trafo
    // of a window life), or the body now made after a stamp inside the
    // window (the RC stamp rule: replayed at the stamp) - steps 1 and 3
    // with no revive: the body now (born in the window) taken out, every
    // window event queued, the generations of their entities put back to
    // A's.  Counted (mlives=.../window).  A player whose only window event
    // is the birth of its body now, made at its stamp, stays on LB's path;
    // one whose birth is not made yet (its own rewind) on make_remote_birth's.
    void session_client_core::reconcile_remote_lives(uint32_t anchor, uint32_t now, const std::vector<ended_life>& ended,
                                                     std::map<std::string, life_outcome>& out) {
        using bmmo::session::event_type;
        using entry_t = client_core_state::life_entry;
        s.lives_anchor_ = anchor;
        // An exact resync's apply reconciles its mirrors itself (resync
        // design 2.6.3); the one implementation RSD 2.6.3 asks for is not
        // built yet.
        if (s.resync_restoring_) return;
        const rollback_tracked* at_tracked = s.rollback_.recorded_tracked(anchor);
        const auto stamp_in = [](uint32_t stamp, uint32_t after, uint32_t upto) {
            return static_cast<int32_t>(stamp - after) > 0 && static_cast<int32_t>(stamp - upto) <= 0;
        };
        const auto generation_at_anchor = [&](const std::string& entity, uint32_t& out_generation) {
            if (!at_tracked) return false;
            const auto it = at_tracked->generations.find(entity);
            if (it == at_tracked->generations.end()) return false;
            out_generation = it->second;
            return true;
        };
        const auto put_back_generation = [&](const std::string& entity) {
            uint32_t generation = 0;
            if (generation_at_anchor(entity, generation)) s.generations_[entity] = generation;
            else s.generations_.erase(entity);
        };
        const auto fail = [this](const std::string& name, const std::string& why) {
            ++s.mirror_revive_failures_;
            if (s.mirror_revive_failures_ <= 8)
                logf("lives: mirror %s not revived: %s (%llu so far)", name.c_str(), why.c_str(),
                     static_cast<unsigned long long>(s.mirror_revive_failures_));
        };
        const auto in_server_order = [](std::vector<const entry_t*>& window) {
            std::stable_sort(window.begin(), window.end(), [](const entry_t* a, const entry_t* b) {
                if (a->applied != b->applied) return static_cast<int32_t>(a->applied - b->applied) < 0;
                return bmmo::session::lifecycle_apply_before(0, a->origin, a->type, 0, b->origin, b->type);
            });
        };
        // 1. The player's body now out, muted, its navigation detached.
        const auto take_out = [this](remote_ball& remote) {
            if (remote.navigation) platform_.detach_mirror_navigation(remote.entity);
            remote.navigation = false;
            if (remote.physicalized) {
                unphysicalize_mirror(remote.entity);
                remote.physicalized = false;
            }
        };
        // 3. The window's events at their stamps.
        const auto queue_window = [&](uint32_t player, const std::vector<const entry_t*>& window) {
            for (const auto* entry: window) {
                if (entry->type == event_type::Unphysicalize) {
                    bool queued = false;
                    for (const auto& d: s.pending_deaths_) queued = queued || (d.player == player && d.tick == entry->applied);
                    if (!queued) {
                        client_core_state::pending_death death;
                        death.player = player;
                        death.tick = entry->applied;
                        death.origin = entry->origin;
                        death.reconciled = true;
                        s.pending_deaths_.push_back(death);
                    }
                    continue;
                }
                if (entry->type != event_type::Physicalize) continue;
                bool queued = false;
                for (auto& b: s.pending_births_)
                    if (b.player == player && b.tick == entry->applied && !b.remake) {
                        if (b.entity.empty()) b.entity = entry->entity;
                        queued = true;
                    }
                if (!queued) {
                    pending_birth birth = entry->birth;
                    birth.entity = entry->entity;
                    birth.remake = false;
                    birth.reconciled = true;
                    s.pending_births_.push_back(birth);
                }
                // The window is in stamp order: the last birth of a name
                // wins - the life its restore item (its first record at its
                // current generation) is about.
                if (!entry->entity.empty()) out[entry->entity].remade_at = entry->applied;
            }
        };
        std::set<uint32_t> done, asked;
        for (const ended_life& life: ended) {
            if (life.kind != life_kind::mirror || out.count(life.name)) continue;
            // The A-life's birth: the newest logged Physicalize of the
            // entity stamped at or before A.
            const entry_t* born = nullptr;
            for (const auto& entry: s.life_log_) {
                if (entry.type != event_type::Physicalize || entry.skipped || entry.entity != life.name) continue;
                if (static_cast<int32_t>(entry.applied - anchor) > 0) continue;
                if (!born || static_cast<int32_t>(entry.applied - born->applied) >= 0) born = &entry;
            }
            if (!born) {
                if (const uint32_t owner = mirror_player_of(life.name)) asked.insert(owner);
                fail(life.name, "no logged birth at or before tick " + std::to_string(anchor));
                continue;
            }
            const uint32_t player = born->player;
            asked.insert(player);
            if (done.count(player)) continue;
            const auto remote_it = s.remotes_.find(player);
            if (remote_it == s.remotes_.end()) {
                fail(life.name, "no remote " + std::to_string(player));
                continue;
            }
            bool consistent = true;
            std::vector<const entry_t*> window;
            for (const auto& entry: s.life_log_) {
                if (entry.player != player || entry.skipped || &entry == born) continue;
                if (stamp_in(entry.applied, born->applied, anchor)) consistent = false;
                else if (stamp_in(entry.applied, anchor, now)) window.push_back(&entry);
            }
            if (!consistent) {
                fail(life.name, "the log has an event of player " + std::to_string(player) + " between its birth at "
                                    + std::to_string(born->applied) + " and tick " + std::to_string(anchor));
                continue;
            }
            if (window.empty()) {
                fail(life.name, "no logged event of player " + std::to_string(player) + " in (" + std::to_string(anchor)
                                    + ", " + std::to_string(now) + "]");
                continue;
            }
            in_server_order(window);
            auto& remote = remote_it->second;
            if (config.remote_births_parked && remote.physicalized) {
                fail(life.name, "its later life " + remote.entity + " stays parked (remote_births_parked)");
                continue;
            }
            done.insert(player);
            const uint32_t until = window.front()->applied;
            // 1. The later life out.
            take_out(remote);
            // The generations the window's events will bump again.
            std::set<std::string> entities{life.name};
            for (const auto* entry: window)
                if (entry->type == event_type::Physicalize && !entry->entity.empty()) entities.insert(entry->entity);
            for (const auto& entity: entities) put_back_generation(entity);
            // 2. The A-life again.
            bmmo_physics_body_state state{};
            mirror_made made;
            std::string why;
            if (!s.rollback_.recorded_body_state(life.name, anchor, state)) {
                why = "no body in the record of tick " + std::to_string(anchor);
            } else {
                mirror_spec spec;
                spec.player = player;
                spec.ball_type = born->birth.ball_type;
                spec.entity = life.name;
                for (int k = 0; k < 3; ++k) spec.position[k] = static_cast<float>(state.position[k]);
                quaternion_rows(state.rotation, spec.rotation);
                spec.recipe = to_bridge_recipe(born->birth.recipe);
                const int join_order = mirror_join_order(player, born->applied);   // step M1: the life's
                spec.group = "P#" + std::to_string(join_order);
                {
                    const event_log_mute mute(*this);
                    made = platform_.make_mirror(spec);
                }
                if (!made.made) why = "make_mirror: " + made.error;
            }
            if (made.made) {
                ++s.mirror_revives_;
                s.mirror_entities_.insert(life.name);
                s.mirror_owners_[life.name] = {player, born->birth.ball_type};   // step G3
                remote.entity = life.name;
                remote.ball_type = born->birth.ball_type;
                remote.physicalized = true;
                remote.born_tick = born->made_at ? born->made_at : born->applied;
                remote.driven_from = static_cast<int32_t>(remote.born_tick - anchor) <= 0 ? remote.born_tick : anchor;
                remote.birth = born->birth;
                remote.birth.remake = false;
                remote.birth.reconciled = false;
                remote.birth_known = remote.born_tick == born->applied;
                remote.corrector.clear();
                attach_remote_navigation(player, remote, true);
                out[life.name].revived_until = until;
                logf("lives: mirror %s of %u (alive at tick %u) revived until tick %u; %zu window event(s) queued",
                     life.name.c_str(), player, anchor, until, window.size());
            } else {
                // Its end bumps nothing without a body: the live value.
                uint32_t generation = 0;
                generation_at_anchor(life.name, generation);
                s.generations_[life.name] = generation + 1;
                fail(life.name, why);
            }
            s.mechanism_tracking_.mark_dirty();
            queue_window(player, window);
        }
        // The window lives of the players no ended A-life brought here (in
        // player order: s.remotes_ is ordered).
        for (auto& [player, remote]: s.remotes_) {
            if (asked.count(player)) continue;
            std::vector<const entry_t*> window;
            for (const auto& entry: s.life_log_)
                if (entry.player == player && !entry.skipped && stamp_in(entry.applied, anchor, now)) window.push_back(&entry);
            if (window.empty()) continue;
            in_server_order(window);
            // A window that begins with a death took a life the anchor had:
            // one the engine did not hand over as ended, because no record
            // of the window shows its end yet - a late death's own rewind
            // (A = D - 1: since step DX11 the body is still there at the
            // restore and the replay removes it at D, remove_remote queued
            // it).  Nothing to replay here.  (Since step GL1 that replay
            // records the end - rollback_world::remote_generation - so a
            // later rewind of the same frame, the beam pair's birth
            // rewind included, gets the life handed over as ended above.)
            {
                const auto first = std::find_if(window.begin(), window.end(),
                                                [](const entry_t* e) { return !e->entity.empty(); });
                if (first == window.end() || (*first)->type != event_type::Physicalize) continue;
            }
            // What the window did that LB's remake cannot replay: a life it
            // removed, a second birth, or the body now made after its stamp
            // (an event not applied yet - its own rewind - names no entity).
            bool removed = false;
            size_t births = 0;
            const entry_t* current = nullptr;
            for (const auto* entry: window) {
                if (entry->entity.empty()) continue;
                if (entry->type == event_type::Unphysicalize) {
                    removed = true;
                } else if (entry->type == event_type::Physicalize) {
                    ++births;
                    if (remote.physicalized && entry->entity == remote.entity) current = entry;
                }
            }
            const bool made_late = current && current->made_at != 0 && current->made_at != current->applied;
            if (!removed && births < 2 && !made_late) continue;
            const std::string name = remote.physicalized ? remote.entity : window.front()->entity;
            if (remote.physicalized && !current) {
                fail(name, "player " + std::to_string(player) + "'s body now was not born in (" + std::to_string(anchor) + ", "
                               + std::to_string(now) + "] although its log has events there");
                continue;
            }
            if (config.remote_births_parked && remote.physicalized) {
                fail(name, "its life " + remote.entity + " stays parked (remote_births_parked)");
                continue;
            }
            ++s.window_lives_;
            // 1. The body now (born in the window) out.
            take_out(remote);
            // The generations the window's events will bump again.
            std::set<std::string> entities;
            for (const auto* entry: window)
                if (!entry->entity.empty()) entities.insert(entry->entity);
            for (const auto& entity: entities) put_back_generation(entity);
            s.mechanism_tracking_.mark_dirty();
            // 3. Every window event at its stamp.
            queue_window(player, window);
            logf("lives: the window lives of %u in (%u, %u] replayed: %zu event(s) queued%s", player, anchor, now,
                 window.size(), made_late ? ", its body now made after its stamp" : "");
        }
    }

    // The reconcile's queued events that no replay applied (it stopped
    // before their tick, or a make failed there): applied now, live, in the
    // server's order, so the world has the body set it had before the
    // rewind.  Counted (mlives=.../settled).
    void session_client_core::settle_unreplayed_lives(uint32_t now) {
        struct left {
            uint32_t tick = 0, player = 0, origin = 0;
            bmmo::session::event_type type = bmmo::session::event_type::Physicalize;
            pending_birth birth;
        };
        std::vector<left> due;
        for (auto it = s.pending_births_.begin(); it != s.pending_births_.end();)
            if (it->reconciled && static_cast<int32_t>(it->tick - now) <= 0) {
                due.push_back({it->tick, it->player, it->origin, bmmo::session::event_type::Physicalize, *it});
                it = s.pending_births_.erase(it);
            } else ++it;
        for (auto it = s.pending_deaths_.begin(); it != s.pending_deaths_.end();)
            if (it->reconciled && static_cast<int32_t>(it->tick - now) <= 0) {
                due.push_back({it->tick, it->player, it->origin, bmmo::session::event_type::Unphysicalize, {}});
                it = s.pending_deaths_.erase(it);
            } else ++it;
        if (due.empty()) return;
        std::stable_sort(due.begin(), due.end(), [this](const left& a, const left& b) {
            if (a.tick != b.tick) return static_cast<int32_t>(a.tick - b.tick) < 0;
            return bmmo::session::lifecycle_apply_before(join_rank(a.player, a.tick), a.origin, a.type,
                                                         join_rank(b.player, b.tick), b.origin, b.type);
        });
        for (const auto& l: due) {
            ++s.lives_settled_live_;
            logf("lives: the %s of %u stamped %u was not replayed: applied live", l.type == bmmo::session::event_type::Physicalize
                 ? "birth" : "death", l.player, l.tick);
            const event_log_mute mute(*this);
            if (l.type == bmmo::session::event_type::Unphysicalize) {
                unphysicalize_remote(l.player, l.tick);
            } else {
                pending_birth birth = l.birth;
                birth.reconciled = false;
                if (!physicalize_remote(birth, live_first_tick(birth.tick)))
                    logf("lives: the birth of %u stamped %u could not be made live either", l.player, l.tick);
            }
            s.mechanism_tracking_.mark_dirty();
        }
    }


    // ================================================ own lives in windows
    // Gap L, L3 (rollback-engine gaps design 2.3.4, 9.7; step EL8).

    // The pose a matrix carries, in the layout own_body_spec and
    // unmake_own take (rotation rows, then the translation row).
    namespace {
        void pose_of_matrix(const float matrix[16], float position[3], float rotation[9]) {
            for (int r = 0; r < 3; ++r)
                for (int k = 0; k < 3; ++k) rotation[r * 3 + k] = matrix[r * 4 + k];
            for (int k = 0; k < 3; ++k) position[k] = matrix[12 + k];
        }
    }

    // An own entity's body gone in a live frame (the drain's "deleted"
    // entry: a death, or the old ball of a trafo), with the life's
    // generation and the entity's matrix after the frame (design 2.3.4
    // "Own deaths in the log"; review 1 #7).  Nothing is sent: the death's
    // own report (report_unphysicalize) is unchanged.  Adapted: recorded at
    // the drain, the one site both own-lifecycle sources and the trafo's
    // old ball reach, not in report_unphysicalize (which knows no entity
    // and never sees the trafo's old ball).
    void session_client_core::note_own_death(const std::string& entity, uint32_t tick, uint32_t generation) {
        if (!config.rollback) return;
        client_core_state::own_death death;
        death.tick = tick;
        death.entity = entity;
        death.generation = generation;
        if (platform_.entity_matrix) death.has_matrix = platform_.entity_matrix(entity, death.matrix);
        s.own_deaths_.push_back(std::move(death));
        while (s.own_deaths_.size() > 64) s.own_deaths_.erase(s.own_deaths_.begin());
    }

    // Kept while a rewind can be anchored before the death (the rule of the
    // other own records, retire_own_lives) - before its end, for one the
    // server moved (step ODM: a rewind anchored inside [D, end) revives it
    // too).
    void session_client_core::retire_own_deaths() {
        const uint32_t now = current_tick();
        s.own_deaths_.erase(std::remove_if(s.own_deaths_.begin(), s.own_deaths_.end(),
                                           [this, now](const client_core_state::own_death& d) {
                                               const uint32_t end = own_death_end(d);
                                               return !s.rollback_.knows_tick(end - 1)
                                                   && static_cast<int32_t>(now - end) > 0;
                                           }),
                            s.own_deaths_.end());
    }

    // ================================================ own deaths moved
    // FULL step ODM (plan step 22; late-player design 3.3 "The
    // Unphysicalize side", 6.2.3 (d)).
    //
    // The server applies a late own Unphysicalize at D' > D (MOVED), but a
    // Physicalize of ours stamped at or after D that it applies first ends
    // the body there instead: physics_world::apply_event takes the player's
    // old body out at a Physicalize, and the runner's guard
    // (lifecycle_unphysicalize_superseded) then drops the late
    // Unphysicalize unechoed.  So the owner learns the end from its own
    // records: the earliest of D' and those births' applied ticks (a birth
    // still unanswered counts at its stamp; its echo, if MOVED, moves the
    // end with it, move_own_birth).  Adapted from design 3.3, which has the
    // server echo such a death SUPERSEDED: no server path does.
    uint32_t session_client_core::own_death_end(const client_core_state::own_death& d) const {
        if (!d.moved) return d.tick;
        uint32_t end = d.applied;
        for (const auto& life: s.own_life_) {
            if (life.what != own_life::kind::birth || !life.made || life.retired) continue;
            if (static_cast<int32_t>(life.origin - d.tick) < 0) continue;   // older: the life that died
            if (static_cast<int32_t>(life.applied - end) < 0) end = life.applied;
        }
        return static_cast<int32_t>(end - d.tick) < 0 ? d.tick : end;
    }

    // A beam's old body: until the stamp, or - its Unphysicalize moved -
    // the earlier of that and the Physicalize's applied tick (the
    // Physicalize found the body alive and replaced it: step HLX), and
    // never past a death of that life after the stamp (a beam keeps the
    // generation; the death ends the body at the server too, and the late
    // Unphysicalize then finds no body: measured, a beam into the arena's
    // death zone died 2 ticks after its stamp, odm/pair1).
    uint32_t session_client_core::own_beam_end(const own_life& beam) const {
        if (beam.what != own_life::kind::beam || beam.death_applied == 0) return beam.origin;
        uint32_t end = beam.death_applied;
        if (beam.made && static_cast<int32_t>(beam.applied - end) < 0) end = beam.applied;
        for (const auto& d: s.own_deaths_)
            if (d.entity == beam.entity && d.generation == beam.generation && static_cast<int32_t>(d.tick - beam.origin) > 0
                    && static_cast<int32_t>(d.tick - end) < 0)
                end = d.tick;
        return static_cast<int32_t>(end - beam.origin) < 0 ? beam.origin : end;
    }

    // Every moved own death and moved beam whose end lies past what its
    // records name: the records of [named, end) name the old body again
    // (rollback_engine::extend_life), and `from` the lowest tick touched
    // (unchanged when none).  False: a record in a span names another own
    // entity (nothing is changed for that one; the caller resyncs).
    // An end a birth gave that the server has answered is final: kept in the
    // death's record, so the birth's record retiring first (retire_own_lives
    // erases a retired one at once) cannot widen it again.  Run where the
    // records are extended and where a birth is acked without a move (review
    // finding 2: such a birth never reaches extend_moved_deaths).
    void session_client_core::freeze_moved_death_ends() {
        for (auto& d: s.own_deaths_) {
            if (!d.moved) continue;
            const uint32_t end = own_death_end(d);
            if (end == d.applied) continue;
            for (const auto& life: s.own_life_)
                if (life.what == own_life::kind::birth && life.made && !life.retired && life.applied == end
                        && life.state != own_life::verdict::sent) {
                    d.applied = end;
                    break;
                }
        }
    }

    bool session_client_core::extend_moved_deaths(uint32_t& from) {
        const auto input_of = [this](uint32_t tick, bmmo::session::input_frame& frame) {
            const auto it = s.own_inputs_.find(tick);
            if (it == s.own_inputs_.end()) return false;
            frame = it->second;
            return true;
        };
        const auto lower = [&from](uint32_t tick) {
            if (from == 0 || static_cast<int32_t>(tick - from) < 0) from = tick;
        };
        bool ok = true;
        freeze_moved_death_ends();
        for (auto& d: s.own_deaths_) {
            if (!d.moved) continue;
            const uint32_t end = own_death_end(d);
            const uint32_t named = d.extended == 0 ? d.tick : d.extended;
            if (static_cast<int32_t>(end - named) <= 0) continue;
            size_t changed = 0;
            if (!s.rollback_.extend_life(d.entity, d.generation, named, end, s.rollback_.recorded_own_polls(d.tick - 1),
                                         input_of, changed)) {
                ok = false;
                continue;
            }
            d.extended = end;
            lower(named);
        }
        for (auto& beam: s.own_life_) {
            if (beam.what != own_life::kind::beam || beam.retired || beam.death_applied == 0) continue;
            const uint32_t end = own_beam_end(beam);
            // A death of the life capped it: final (the death's record may
            // retire before the beam's).
            if (static_cast<int32_t>(end - beam.death_applied) < 0
                    && (!beam.made || static_cast<int32_t>(end - beam.applied) < 0))
                beam.death_applied = end;
            const uint32_t named = beam.death_extended == 0 ? beam.origin : beam.death_extended;
            if (static_cast<int32_t>(end - named) <= 0) continue;
            size_t changed = 0;
            if (!s.rollback_.extend_life(beam.entity, beam.generation, named, end,
                                         s.rollback_.recorded_own_polls(beam.origin - 1), input_of, changed)) {
                ok = false;
                continue;
            }
            beam.death_extended = end;
            lower(named);
        }
        return ok;
    }

    // An own correction joins the frame's lifecycle batch (late-player
    // design 6.2.3 (d), the rule SRT3 left to ODM): one restore to the
    // lowest anchor and one replay with the frame's late remote events,
    // never a rewind_world of its own.  Outside pre_step's collecting
    // window (a test, a caller outside the frame) the batch is opened and
    // run here.  False: past the reach or outside the history.
    bool session_client_core::join_own_correction(uint32_t anchor, const std::string& reason) {
        if (!config.rollback) return false;
        const uint32_t now = current_tick();
        if (static_cast<int32_t>(now - anchor) < 0 || now - anchor > s.rollback_.thresholds().max_resim_ticks
                || !s.rollback_.knows_tick(anchor))
            return false;
        // As rewind_for_lifecycle: a pending exact resync's state of base - 1
        // covers a change before its base (no rewind, no resync of its own).
        if (s.resync_pending_ && s.resync_exact_ && static_cast<int32_t>(anchor + 1 - s.resync_base_) < 0) {
            logf("%s precedes the pending resync's anchor %u: the server's state covers it", reason.c_str(),
                 s.resync_base_ - 1);
            return true;
        }
        auto& b = s.life_batch_;
        const bool outside = !b.collecting;
        if (outside) b.collecting = true;
        if (!b.pending || static_cast<int32_t>(anchor - b.anchor) < 0) b.anchor = anchor;
        b.reason += (b.pending ? "; " : "") + reason;
        b.pending = true;
        ++b.events;
        if (outside) {
            flush_life_rewinds();
            s.life_batch_.collecting = false;
        }
        return true;
    }

    // The echo of an own Unphysicalize stamped D that the server applied at
    // D' (MOVED), with own deaths moving (own_deaths_move).  Three cases
    // (plan step 22):
    //   (1) lone: no birth of ours ends the body first - the body lives in
    //       our history until D', the records name it there, and the frame's
    //       batch rewinds to D - 1: the restore revives the life at the
    //       record of D - 1 (reconcile_own_lives), the replay drives it from
    //       own_inputs_ and removes it at D' with the matrix of the live
    //       death (apply_moved_own_deaths, before the tick's own lives);
    //   (2) superseded: a birth of ours stamped after D that the server
    //       applied before D' (its Physicalize replaced the body): the death
    //       moves to that birth's applied tick instead (own_death_end), or
    //       nowhere when it was on time;
    //   (3) a pair: the birth of the same origin (a trafo, a respawn's
    //       pair) moved too - its echo, MOVED, came first and its body is
    //       made at T' already (move_own_birth), so the death goes to the
    //       earlier of D' and T' in the same correction; one not answered
    //       yet counts at its stamp, so nothing moves until its echo, whose
    //       move_own_birth then carries the death (extend_moved_deaths).  A
    //       beam is its own record: the old body lives to own_beam_end.
    // Past the reach or outside the history: the resync it always was.
    void session_client_core::move_own_death(const bmmo::session_event_msg& msg) {
        const uint32_t now = current_tick();
        const uint32_t stamp = msg.origin_tick;
        const std::string head = "own death moved " + std::to_string(stamp) + " -> " + std::to_string(msg.tick);
        const auto resync = [&](const std::string& why) {
            ++s.own_death_moves_resynced_;
            logf("%s at local tick %u: %s, resync", head.c_str(), now, why.c_str());
            journal_.note(head + ": " + why + ", resync");
            request_resync("own death moved: resync", true);
        };
        // Review finding 3: the refusals that need no record changed come
        // before any is - a rewind of another event before the resync
        // applies must not revive the body from a half-made correction.
        const auto unreachable = [&](uint32_t anchor) -> std::string {
            if (!config.rollback || static_cast<int32_t>(now - anchor) < 0
                    || now - anchor > s.rollback_.thresholds().max_resim_ticks || !s.rollback_.knows_tick(anchor))
                return "past the reach (depth " + std::to_string(static_cast<int>(now - anchor)) + ")";
            return {};
        };
        // A beam: its own record carries both halves.
        for (auto& beam: s.own_life_) {
            if (beam.what != own_life::kind::beam || beam.retired || beam.origin != stamp) continue;
            const uint32_t before = own_beam_end(beam);
            const uint32_t was = beam.death_applied;
            beam.death_applied = msg.tick;
            const uint32_t end = own_beam_end(beam);
            if (end == before) {
                ++s.own_deaths_kept_;
                logf("%s (a beam) at local tick %u: its Physicalize (applied %u) replaced the body first: nothing to move",
                     head.c_str(), now, beam.applied);
                return;
            }
            std::string refused = static_cast<int32_t>(end - now) > 0 ? "the beam's old body ends after the live tick"
                                                                       : unreachable(before - 1);
            if (!refused.empty()) {
                beam.death_applied = was;
                return resync(refused);
            }
            uint32_t from = 0;
            if (!extend_moved_deaths(from)) return resync("a record of the span names another own body");
            const std::string why = head + " (a beam pair: the old body until " + std::to_string(end) + ", the new one from "
                                  + std::to_string(beam.applied) + ")";
            if (!join_own_correction(before - 1, why)) return resync("past the reach (depth "
                                                                     + std::to_string(static_cast<int>(now - (before - 1))) + ")");
            ++s.own_deaths_moved_;
            ++s.own_pairs_moved_;
            logf("%s at local tick %u: rewound to %u in the frame's batch", why.c_str(), now, before - 1);
            journal_.note(why);
            return;
        }
        client_core_state::own_death* death = nullptr;
        for (auto& d: s.own_deaths_)
            if (d.tick == stamp) {
                death = &d;
                break;
            }
        if (!death) {
            return resync(s.rollback_.knows_tick(stamp - 1) ? "no own death logged at its stamp"
                                                             : "no record of it (past the history)");
        }
        const uint32_t before = own_death_end(*death);
        const bool was_moved = death->moved;
        const uint32_t was_applied = death->applied;
        death->moved = true;
        death->applied = msg.tick;
        const uint32_t end = own_death_end(*death);
        // The pair: a birth of the same origin.
        const own_life* pair = nullptr;
        for (const auto& life: s.own_life_)
            if (life.what == own_life::kind::birth && !life.retired && life.origin == stamp) pair = &life;
        if (end == before) {
            ++s.own_deaths_kept_;
            if (pair && pair->state == own_life::verdict::sent)
                logf("%s at local tick %u: its pair's birth is unanswered (counted at its stamp): moved with that "
                     "birth's echo", head.c_str(), now);
            else
                logf("%s at local tick %u: a birth of ours ended the body at the server first (applied %u): nothing to move",
                     head.c_str(), now, end);
            return;
        }
        {
            const std::string refused = static_cast<int32_t>(end - now) > 0 ? std::string("the body ends after the live tick")
                                                                             : unreachable(before - 1);
            if (!refused.empty()) {
                death->moved = was_moved;
                death->applied = was_applied;
                return resync(refused);
            }
        }
        uint32_t from = 0;
        if (!extend_moved_deaths(from)) return resync("a record of the span names another own body");
        const bool superseded = end != msg.tick;
        const std::string why = head + (pair ? " (a pair" : superseded ? " (superseded" : " (lone")
                              + ": the body until " + std::to_string(end) + ")";
        if (!join_own_correction(before - 1, why))
            return resync("past the reach (depth " + std::to_string(static_cast<int>(now - (before - 1))) + ")");
        ++s.own_deaths_moved_;
        if (pair && pair->moved()) ++s.own_pairs_moved_;
        logf("%s at local tick %u: rewound to %u in the frame's batch", why.c_str(), now, before - 1);
        journal_.note(why);
    }

    // The moved own deaths queued for a replayed `tick` (own_due::moved):
    // at our place among the tick's body changes, before the own lives of
    // that tick - the server applies an Unphysicalize before the
    // Physicalize of the same origin, and the Physicalize that replaced a
    // body ends it first.
    void session_client_core::apply_moved_own_deaths(uint32_t tick) {
        std::vector<client_core_state::own_due> due;
        for (auto it = s.own_due_.begin(); it != s.own_due_.end();)
            if (it->tick == tick && it->death && it->moved) {
                due.push_back(std::move(*it));
                it = s.own_due_.erase(it);
            } else ++it;
        for (const auto& d: due) {
            float position[3], rotation[9];
            pose_of_matrix(d.matrix, position, rotation);
            std::string error;
            bool ok = false;
            {
                const event_log_mute mute(*this);
                ok = platform_.unmake_own(d.entity, position, rotation, tick, true, error);
            }
            s.mechanism_tracking_.mark_dirty();
            if (!ok) logf("lives: own %s: the moved death at tick %u: %s", d.entity.c_str(), tick, error.c_str());
        }
    }

    // A renumbering (resync, SessionAssign): the ticks these name are gone.
    void session_client_core::clear_own_window_state() {
        s.own_deaths_.clear();
        s.own_due_.clear();
        s.own_matrices_.clear();
    }

    // Design 9.7 (a): after the live record of `tick`, the CK matrix of
    // every own entity without a body (the scripts no replay runs move it:
    // the death sequence to the resetpoint at D+1, the trafo animation).
    void session_client_core::record_own_matrices(uint32_t tick) {
        if (!config.rollback || !platform_.entity_matrix) return;
        while (!s.own_matrices_.empty() && static_cast<int32_t>(s.own_matrices_.rbegin()->first - tick) >= 0)
            s.own_matrices_.erase(std::prev(s.own_matrices_.end()));
        std::vector<client_core_state::own_matrix> bodiless;
        for (const auto& entity: s.own_ball_entities_) {
            if (entity == s.own_body_entity_) continue;
            bmmo_physics_body_state state{};
            if (platform_.body_state(entity, state)) continue;
            client_core_state::own_matrix m;
            m.entity = entity;
            if (platform_.entity_matrix(entity, m.matrix)) bodiless.push_back(std::move(m));
        }
        if (!bodiless.empty()) s.own_matrices_[tick] = std::move(bodiless);
        const uint32_t keep = static_cast<uint32_t>(s.rollback_.thresholds().history_ticks);
        while (!s.own_matrices_.empty() && static_cast<int32_t>(s.own_matrices_.begin()->first + keep - tick) < 0)
            s.own_matrices_.erase(s.own_matrices_.begin());
    }

    // Before the scripts of a replayed `tick`: the live matrix after the
    // frame of that same tick, for each own entity that has no body in the
    // replay either (a body is the replay's: the restore or the step wrote
    // it).  The tick's own record, not the one before it: the scripts that
    // move a dead ball (Gameplay_Ingame's death sequence, which puts it on
    // the resetpoint at D+1) are not replayed and, live, run before the
    // modules whose proximities read it - so the modules of tick t read the
    // matrix the frame of t ends with.  Measured on core_ref_s8
    // resync_snapoff c0 (death 2452, resetpoint at 2453): with the record
    // of t-1 the replayed 2453 left the ball at its death pose, P_Modul_18's
    // TT Scaleable Proximity sampled it there and held that distance to the
    // window's end (2459: "Current Distance" differs from the live record);
    // with the record of t it does not.  Nothing is written where the
    // entity already has the value bit for bit (gap P: SetWorldMatrix
    // re-derives the local matrix, and with KeepChildren the children's,
    // with rounding).
    void session_client_core::write_own_matrices(uint32_t tick) {
        if (!platform_.set_entity_matrix || !platform_.entity_matrix) return;
        const auto it = s.own_matrices_.find(tick);
        if (it == s.own_matrices_.end()) return;
        for (const auto& m: it->second) {
            bmmo_physics_body_state state{};
            if (platform_.body_state(m.entity, state)) continue;
            float current[16];
            if (platform_.entity_matrix(m.entity, current) && std::memcmp(current, m.matrix, sizeof(current)) == 0)
                continue;
            if (platform_.set_entity_matrix(m.entity, m.matrix)) ++s.own_matrix_writes_;
        }
    }

    // The core's scope around a rewind: the host's guard, and the own
    // entities without a body before the rewind put back where the live
    // scripts had them if they have none after it either (a revived life
    // removed again at its death, the matrices written into the replay).
    scope_guard session_client_core::keep_live_cells() {
        auto host = std::make_shared<scope_guard>(platform_.keep_live_cells(*this));
        if (!config.rollback || !platform_.entity_matrix || !platform_.set_entity_matrix)
            return scope_guard([host]() mutable { host.reset(); });
        std::vector<client_core_state::own_matrix> saved;
        for (const auto& entity: s.own_ball_entities_) {
            if (entity == s.own_body_entity_) continue;
            bmmo_physics_body_state state{};
            if (platform_.body_state(entity, state)) continue;
            client_core_state::own_matrix m;
            m.entity = entity;
            if (platform_.entity_matrix(entity, m.matrix)) saved.push_back(std::move(m));
        }
        return scope_guard([this, host, saved]() mutable {
            for (const auto& m: saved) {
                bmmo_physics_body_state state{};
                if (platform_.body_state(m.entity, state)) continue;
                float current[16];   // gap P: no write of the value it has
                if (platform_.entity_matrix(m.entity, current) && std::memcmp(current, m.matrix, sizeof(current)) == 0)
                    continue;
                platform_.set_entity_matrix(m.entity, m.matrix);
            }
            host.reset();
        });
    }

    // The core's half of rollback_world::restore_lives for the own ball
    // (2.3.4 "Revive", "Own events inside the window"), before the mirrors'.
    //
    // Per own life alive at `anchor` (A) that ends inside (A, now]:
    //   its logged death D (own_deaths_: that entity and generation);
    //   a later life of the same name with a body now (a respawn of that
    //   ball in the window) is taken out (unmake_own, muted) and remade at
    //   its birth stamp E (answered remade_at = E);
    //   the A-life is made (make_own, muted) at the A record's pose, with
    //   the ball table's recipe in the own group, no kick, with_replica;
    //   its death is queued at D with its matrix (answered revived_until =
    //   D).  The pre-rewind matrix is the core's scope's (keep_live_cells).
    // Then per own life born AND ended inside the window (no body now, no
    // record at A: L-U9): remade at its stamp and removed at its death.
    // Own births in the window whose life is alive now and whose name was
    // not revived stay on the parking path, as before.
    // Generations are not touched: the replay re-captures its records with
    // the live records' generations, and the muted changes bump nothing.
    void session_client_core::reconcile_own_lives(uint32_t anchor, uint32_t now, const std::vector<ended_life>& ended,
                                                  std::map<std::string, life_outcome>& out) {
        if (!caps_.own_body_in_replay) return;
        // Step RSX: under an exact resync's restore the own half answers for
        // one life only - the A-life the apply mapped the server's own row
        // onto (resync_own_revive_entity: no body now, its death logged in
        // the window).  The row outranks the A record's pose (rollback.hpp,
        // revive_ended_lives keeps a from_row item's state), its death is
        // queued at its stamp like a rewind's.  The rest stays as before the
        // step: a same-name life born again inside the window, and an own
        // life born and ended inside it, are not reconciled by a resync
        // (the resync design's table gives them no rule beyond "applied at
        // their stamps", and no run has shown one).  The mirrors keep EL7's
        // guard (reconcile_remote_lives): they are the apply's own reconcile.
        const std::string resync_only = s.resync_restoring_ ? resync_own_revive_entity(anchor, now) : std::string();
        if (s.resync_restoring_ && resync_only.empty()) return;
        const auto stamp_in = [](uint32_t stamp, uint32_t after, uint32_t upto) {
            return static_cast<int32_t>(stamp - after) > 0 && static_cast<int32_t>(stamp - upto) <= 0;
        };
        const auto fail = [this](const std::string& name, const std::string& why) {
            ++s.own_revive_failures_;
            if (s.own_revive_failures_ <= 8)
                logf("lives: own %s not revived: %s (%llu so far)", name.c_str(), why.c_str(),
                     static_cast<unsigned long long>(s.own_revive_failures_));
        };
        // The first logged death of `entity`'s life `generation` after `after`
        // - at its end: the tick the server ended it, for one it moved
        // (step ODM, own_death_end).
        const auto death_of = [&](const std::string& entity, uint32_t generation, uint32_t after)
                -> const client_core_state::own_death* {
            const client_core_state::own_death* found = nullptr;
            for (const auto& d: s.own_deaths_)
                if (d.entity == entity && d.generation == generation && stamp_in(own_death_end(d), after, now)
                        && (!found || static_cast<int32_t>(own_death_end(d) - own_death_end(*found)) < 0))
                    found = &d;
            return found;
        };
        const auto queue_death = [this](const client_core_state::own_death& d) {
            client_core_state::own_due due;
            due.tick = own_death_end(d);
            due.moved = due.tick != d.tick;   // step ODM: before the tick's own lives
            due.death = true;
            due.entity = d.entity;
            std::copy(std::begin(d.matrix), std::end(d.matrix), due.matrix);
            if (!d.has_matrix) {
                // No matrix read (a host without entity_matrix): the record's
                // pose before the death stands in (the scope puts the live
                // one back at the end).
                bmmo_physics_body_state state{};
                if (s.rollback_.recorded_body_state(d.entity, d.tick - 1, state)) {
                    float rotation[9];
                    quaternion_rows(state.rotation, rotation);
                    for (int r = 0; r < 3; ++r)
                        for (int k = 0; k < 3; ++k) due.matrix[r * 4 + k] = rotation[r * 3 + k];
                    for (int k = 0; k < 3; ++k) due.matrix[12 + k] = static_cast<float>(state.position[k]);
                    due.matrix[15] = 1.0f;
                }
            }
            s.own_due_.push_back(std::move(due));
        };
        const auto queue_remake = [this](const own_life& life) {
            client_core_state::own_due due;
            due.tick = life.applied;
            due.entity = life.entity;
            due.life = life;
            s.own_due_.push_back(std::move(due));
        };
        // The newest own birth of `entity` made at its stamp in (after, now].
        // A moved birth is not one: rollback_world calls restore_lives
        // BEFORE destroy_body, so a moved own birth (own_body_control only,
        // the headless client) still has its body here, and the moved path
        // (destroy_moved_own_birth, then its re-make at the server's tick)
        // owns it.  Such a life fails below ("its body now was not born
        // after its death"), counted in olives' failed, and keeps today's
        // behaviour (not revived); L-H4 covers it.
        //
        // FULL step ODM: after a death the server moved, the birth that ended
        // it at the server may be applied at that very tick (a same-name
        // respawn the late Unphysicalize was superseded by, or the pair's
        // own birth), and a moved one is that pair's: both count then.  A
        // moved one is taken out here and made again by apply_own_lives at
        // its applied tick, after the death (apply_moved_own_deaths).
        const auto birth_of = [&](const std::string& entity, uint32_t after, bool moved_death) -> const own_life* {
            const own_life* found = nullptr;
            for (const auto& life: s.own_life_)
                if (life.what == own_life::kind::birth && life.made && (!life.moved() || moved_death)
                        && life.entity == entity && stamp_in(life.applied, moved_death ? after - 1 : after, now)
                        && (!found || static_cast<int32_t>(life.applied - found->applied) > 0))
                    found = &life;
            return found;
        };
        for (const ended_life& life: ended) {
            if (life.kind != life_kind::own || out.count(life.name)) continue;
            if (s.resync_restoring_ && life.name != resync_only) continue;   // step RSX: as before the step
            const auto* death = death_of(life.name, life.generation, anchor);
            if (!death) {
                fail(life.name, "no own death of generation " + std::to_string(life.generation) + " logged in ("
                                    + std::to_string(anchor) + ", " + std::to_string(now) + "]");
                continue;
            }
            bmmo_physics_body_state state{};
            if (!s.rollback_.recorded_body_state(life.name, anchor, state)) {
                fail(life.name, "no body in the record of tick " + std::to_string(anchor));
                continue;
            }
            bmmo_physics_body_state there{};
            const bool body_now = platform_.body_state(life.name, there);
            const uint32_t death_end = own_death_end(*death);
            const own_life* later = body_now ? birth_of(life.name, death_end, death_end != death->tick) : nullptr;
            // FULL step ODM: the pair's same-name birth moved_own_birth took
            // out before this rewind (no body now): answered remade at its
            // applied tick, so the restore does not destroy it again (it
            // would take the revived old body out, the same name) - the
            // replay makes it there (apply_own_lives).
            const own_life* taken_out = nullptr;
            if (!body_now && death_end != death->tick)
                if (const own_life* moved = birth_of(life.name, death_end, true); moved && moved->moved()) taken_out = moved;
            const uint32_t taken_out_at = taken_out ? taken_out->applied : 0;
            if (body_now && !later) {
                fail(life.name, "its body now was not born after its death at tick " + std::to_string(death_end));
                continue;
            }
            const own_life later_copy = later ? *later : own_life{};
            const client_core_state::own_death death_copy = *death;
            if (later) {
                // The same-name step: the later life out, remade at E.
                std::string error;
                bool ok = false;
                {
                    const event_log_mute mute(*this);
                    ok = platform_.unmake_own(life.name, later_copy.position, later_copy.rotation, anchor, false, error);
                }
                if (!ok) {
                    fail(life.name, "its later life could not be taken out: " + error);
                    continue;
                }
            }
            own_body_spec spec;
            spec.entity = life.name;
            for (int k = 0; k < 3; ++k) spec.position[k] = static_cast<float>(state.position[k]);
            quaternion_rows(state.rotation, spec.rotation);
            bmmo::session::ball_recipe recipe;
            platform_.ball_recipe_of(life.name, recipe);
            spec.recipe = to_bridge_recipe(recipe);
            spec.group = s.own_group_;
            spec.tick = anchor;
            spec.with_replica = true;
            own_body_made made;
            {
                const event_log_mute mute(*this);
                made = platform_.make_own(spec);
            }
            s.mechanism_tracking_.mark_dirty();
            if (made.took_out_failed)
                logf("lives: own %s at the restore of tick %u: unphysicalize: %s", life.name.c_str(), anchor,
                     made.took_out_error.c_str());
            if (!made.found || !made.made) {
                fail(life.name, made.found ? "make_own: " + made.error : std::string("no 3D entity of that name"));
                if (later) {
                    // The later life still comes back at its stamp.
                    if (!later_copy.moved()) queue_remake(later_copy);   // step ODM: else apply_own_lives'
                    out[life.name].remade_at = later_copy.applied;
                    ++s.own_window_remakes_;
                }
                continue;
            }
            ++s.own_revives_;
            queue_death(death_copy);
            out[life.name].revived_until = death_end;
            if (taken_out_at) out[life.name].remade_at = taken_out_at;   // step ODM
            if (later) {
                // Step ODM: a moved birth is apply_own_lives' to make again.
                if (!later_copy.moved()) queue_remake(later_copy);
                out[life.name].remade_at = later_copy.applied;
                ++s.own_window_remakes_;
            }
            if (s.own_revives_ <= 16) {
                const std::string remade = later ? ", its later life remade at tick " + std::to_string(later_copy.applied)
                                                 : std::string();
                logf("lives: own %s (alive at tick %u, generation %u) revived until tick %u%s%s", life.name.c_str(), anchor,
                     life.generation, death_end, remade.c_str(),
                     death_copy.has_matrix ? "" : " (no death matrix: the record's pose)");
            }
        }
        // Own lives born and ended inside the window (no body now).  Not
        // under a resync's restore (step RSX: above).
        for (const auto& life: s.own_life_) {
            if (s.resync_restoring_) break;
            if (life.what != own_life::kind::birth || !life.made || life.moved() || out.count(life.entity)) continue;
            if (!stamp_in(life.applied, anchor, now)) continue;
            const auto* death = death_of(life.entity, life.generation, life.applied);
            if (!death) continue;   // alive now: parked, as before
            bmmo_physics_body_state there{};
            if (platform_.body_state(life.entity, there)) continue;   // a later life of the name lives: left as before
            const own_life copy = life;
            const client_core_state::own_death death_copy = *death;
            queue_remake(copy);
            queue_death(death_copy);
            out[copy.entity].remade_at = copy.applied;
            ++s.own_window_remakes_;
            logf("lives: own %s born at tick %u and dead at tick %u inside (%u, %u]: remade and removed in the replay",
                 copy.entity.c_str(), copy.applied, death_copy.tick, anchor, now);
        }
        // The queue in tick order (a death before a birth of one tick).
        std::stable_sort(s.own_due_.begin(), s.own_due_.end(),
                         [](const client_core_state::own_due& a, const client_core_state::own_due& b) {
                             if (a.tick != b.tick) return static_cast<int32_t>(a.tick - b.tick) < 0;
                             return a.death && !b.death;
                         });
    }

    // Step RSX (resync design 2.6.3: "A body exists at S if and only if the
    // server has a row for it at S"; the own ball's row "always" when the
    // server has one; its death at its stamp like every own event inside a
    // window).  The caller has the server's own row at S; this names the
    // body it lands on when the own ball has none now: the life the record
    // of S tracked, revived by reconcile_own_lives under the restore.  The
    // own deaths of the old numbering are gone at the re-assign
    // (clear_own_window_state): a death before it is not found here, and
    // the row stays unmapped as before the step.
    std::string session_client_core::resync_own_revive_entity(uint32_t anchor, uint32_t now) {
        if (!caps_.own_body_in_replay || !config.rollback || own_out_of_sync_at(anchor)) return {};
        std::string entity = s.rollback_.recorded_own_entity(anchor);
        if (entity.empty()) return {};
        bmmo_physics_body_state there{};
        if (platform_.body_state(entity, there)) return {};
        // The death must be the S-life's (its generation in the record of S,
        // the one revive_ended_lives hands reconcile_own_lives): a death of
        // a later life of the name (the S-life's own death cleared at the
        // re-assign, a respawn dead before the apply) would map the row onto
        // a life the reconcile then refuses, and the row would be skipped
        // silently instead of counted unmapped.
        uint32_t generation = 0;
        if (!s.rollback_.generation_at(entity, anchor, generation)) return {};
        for (const auto& d: s.own_deaths_)
            if (d.entity == entity && d.generation == generation && static_cast<int32_t>(own_death_end(d) - anchor) > 0
                    && static_cast<int32_t>(own_death_end(d) - now) <= 0)
                return entity;
        return {};
    }

    // The own changes queued for a replayed `tick` (the end of
    // drain_pending_lives): a revived life's death with its matrix, muted,
    // the navigation left as the live death left it; a window life made
    // the way the server makes it (remake_own_body).
    void session_client_core::apply_own_due(uint32_t tick) {
        if (s.own_due_.empty()) return;
        std::vector<client_core_state::own_due> due;
        for (auto it = s.own_due_.begin(); it != s.own_due_.end();)
            if (it->tick == tick) {
                due.push_back(std::move(*it));
                it = s.own_due_.erase(it);
            } else ++it;
        for (const auto& d: due) {
            if (d.death) {
                float position[3], rotation[9];
                pose_of_matrix(d.matrix, position, rotation);
                std::string error;
                bool ok = false;
                {
                    const event_log_mute mute(*this);
                    ok = platform_.unmake_own(d.entity, position, rotation, tick, true, error);
                }
                s.mechanism_tracking_.mark_dirty();
                if (!ok) logf("lives: own %s: the replayed death at tick %u: %s", d.entity.c_str(), tick, error.c_str());
            } else {
                remake_own_body(d.life, tick, "replay (window life)");
            }
        }
    }

    // What no replay applied (it stopped before the tick): made live now,
    // so the world keeps the body set it had before the rewind.  (Step
    // OWNB: a birth the scripts made, queued by destroy_moved_own_birth,
    // is made here at the current tick, its spawn kick indexed by that
    // tick, not by its birth tick as the server's clone was - the same as
    // EL8's window lives; only an aborted replay leaves one.)
    void session_client_core::settle_own_due() {
        if (s.own_due_.empty()) return;
        std::vector<client_core_state::own_due> due;
        due.swap(s.own_due_);
        for (const auto& d: due) {
            ++s.own_due_settled_;
            logf("lives: own %s %s queued for tick %u was not replayed: applied live", d.entity.c_str(),
                 d.death ? "death" : "birth", d.tick);
            if (d.death) {
                float position[3], rotation[9];
                pose_of_matrix(d.matrix, position, rotation);
                std::string error;
                const event_log_mute mute(*this);
                platform_.unmake_own(d.entity, position, rotation, current_tick(), true, error);
            } else {
                remake_own_body(d.life, current_tick(), "settled live");
            }
            s.mechanism_tracking_.mark_dirty();
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
    // (design 2.3) an own birth is never made or taken out live, and an echo
    // the history cannot carry is a re-report or a resync.  The beam's stamp
    // is the fresh body on every host that can make the own body (FULL step
    // BEAMF, beam_remakes_body).

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
        const bool spawn = own_birth_is_spawn(report.pose);
        if (spawn) event.flags |= bmmo::session::PHYSICALIZE_FLAG_SPAWN;
        event.recipe = report.recipe;
        send_event(event);
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
        logf("own ball unphysicalized at tick %u", tick);
    }

    bool session_client_core::own_physicalized(const own_birth_report& report) {
        // FULL step RPA: a Physicalize the block makes no body for is no
        // birth - no spawn ghost life, nothing queued, nothing sent or
        // recorded (a host asks first and does none of a birth's work).
        if (own_physicalize_keeps_body(report.entity, report.tick)) return false;
        own_birth_seen(report.pose, report.tick);
        client_core_state::own_report queued;
        queued.report = report;
        s.own_reports_.push_back(std::move(queued));
        return true;
    }

    // FULL step RPA (the user's decision 62 = R6B's option (c); roadmap
    // section 4 item 5).  physics_RT's Physicalize block keeps a body its
    // target already has: GetPhysicsObject(ent) non-null -> Out1, no
    // CreatePhysicsObjectOnParameters (Behaviors/Physicalize.cpp 105-116;
    // the body guard of engine change #6 only re-syncs a guarded entity's
    // matrix there).  BMLPlus broadcasts OnPhysicalize before the block
    // runs, so a body alive now is the body the block will keep: the
    // bridge's answer is platform body_state (get_body_state,
    // physics_state.cpp, the same GetPhysicsObject - an entry is only ever
    // inserted with its IVP object, CKIpionManager.cpp, so the two tests
    // agree).  RTR's stall recipe: the paper trafo's script physicalized
    // the own Ball_Paper, alive since its trafo at 767, again at 843; sent,
    // the server (HLX) replaced its clone with a fresh body at rest while
    // the owner kept its rolling one (0.2309 m at 856).  Not sent, every
    // world keeps the 767 body; not recorded, no replay remakes it.
    bool session_client_core::own_physicalize_keeps_body(const std::string& entity, uint32_t tick) {
        if (entity.empty()) return false;
        bmmo_physics_body_state alive{};
        if (!platform_.body_state(entity, alive)) return false;
        logf("own ball %s physicalized again at tick %u with its body alive: the block keeps the body, not reported "
             "(no event, no own birth)", entity.c_str(), tick);
        return true;
    }

    void session_client_core::own_birth_seen(const own_birth_pose& pose, uint32_t tick) {
        if (own_birth_is_spawn(pose)) note_spawn_birth(s.own_group_, tick, "own birth");
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
    // ball without the navigation replica, or a host that cannot make the
    // own body - beam_remakes_body; no host of ours since FULL step BEAMF).
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
    // FULL step BEAMF (the user's Q-R6-BEAM (a), decision 49.4: "a, and
    // in the core"): this is the one rule on every host that can make the
    // own body (beam_remakes_body: own_body_control, or the retail host's
    // own_body_in_replay) - the stamp takes the body out and makes it
    // fresh at the beam pose, live and in every replay that crosses it.
    // The in-place write it replaced (write_beam through the bridge's
    // optimized beam) left the retail own ball an overlap element sized by
    // the moved distance per PSI (1255.5 m against the server's 4 m: the
    // blob window of realtime-design "As built (step SRT2)").  Only a host
    // with neither capability still writes the stamp in place.  On every
    // capability setting a
    // navigated beam is a move within one life and keeps the generation
    // (rollback-engine gaps design 1.3 invariant B, 2.4; step B8EG item
    // EGB): bumped at the request, after the record of `tick`, it made
    // every anchor before the stamp fail record_at's generation test, so
    // a replay took the ball for one born at the stamp, parked it at its
    // post-beam pose and fed it the pre-beam inputs.  Only the in-place
    // move without the navigation replica, which no replay redoes, bumps.
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
        logf("--beam: %s to (%.3f,%.3f,%.3f), reported to the server for tick %u and %s", name.c_str(),
             s.beam_position_[0], s.beam_position_[1], s.beam_position_[2], beam.origin,
             !navigated ? "moved here now (no navigation replica)"
             : beam_remakes_body() ? "rebuilt here at that tick" : "moved here at that tick (no own body control)");
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
        // FULL step R6H: an armed forced move takes this birth.
        if (s.force_own_move_.armed) {
            birth.force_pending = true;
            birth.force_n = s.force_own_move_.n;
            birth.force_due = birth.origin + s.force_own_move_.after;
            s.force_own_move_ = client_core_state::forced_own_move_arm{};
            logf("forced own move: own birth of %s stamped %u will be moved to %u at local tick %u", entity.c_str(),
                 birth.origin, birth.origin + birth.force_n, birth.force_due);
        }
        s.own_life_.push_back(std::move(birth));
    }

    // Whether the ball's lifetime `life` belongs to is still the one the
    // world has: a death or a trafo since (the drain bumped the
    // generation) ended it, and a replay that made its body again would
    // put back a body the live run no longer has, with nothing in the
    // replay to take it out again.
    //
    // Step EL8 (rollback-engine gaps design 2.3.4, review 1 #5): a replayed
    // tick asks the world the change acts on - the generation of the entity
    // in the live record of tick - 1 (a replay re-captures its records with
    // the live records' generations) - not the live generation, which a
    // death after the change has bumped already: a beam inside a life that
    // dies later in the same window is applied in the replay.  Without a
    // record of tick - 1, the live generation, as before.
    bool session_client_core::own_life_current(const own_life& life, uint32_t tick, bool live) const {
        uint32_t recorded = 0;
        if (!live && s.rollback_.generation_at(life.entity, tick - 1, recorded)) return recorded == life.generation;
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
            ok = platform_.unmake_own(life.entity, life.position, life.rotation, tick, false, error);
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
        // Step SPGH2: a spawn birth begins the spawn ghost life (before the
        // body is made), as the server's copy's does at this tick.
        if (life.spawn) note_spawn_birth(life.group.empty() ? s.own_group_ : life.group, tick, where);
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
                 life.what == own_life::kind::beam ? "beam" : "moved birth",
                 spec.kick ? ", spawn kick" : "");   // step F0 (SPX nit): the label only with a kick
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
    // once; otherwise a replay (drain_pending_lives), where a beam's
    // stamp is redone only once its live frame has run - before that no
    // replay reaches it.  A lifetime that ended since is never made again
    // (own_life_current) and retires.
    //
    // A beam's stamp is the same on every host that can make the own body
    // (FULL step BEAMF, beam_remakes_body: the retail host too, which
    // declares own_body_in_replay, not own_body_control); only a host with
    // neither writes it in place (write_beam).  Without own body control
    // nothing is ever moved (no remake is due: the echo of a moved beam
    // stays a resync, move_own_birth - Stage C's R6).
    void session_client_core::apply_own_lives(uint32_t tick, const char* where, bool live) {
        for (auto& life: s.own_life_) {
            if (life.retired) continue;
            const bool beam_stamp = life.what == own_life::kind::beam && life.origin == tick
                                 && (live ? !life.origin_live : life.origin_live);
            const bool remake_due = life.moved() && life.applied == tick && (!live || !life.applied_live);
            // FULL step ODM: a beam whose Unphysicalize the server moved
            // keeps its old body past the stamp, to own_beam_end - where it
            // goes in a replay, unless the Physicalize remakes it there.
            const uint32_t beam_end = own_beam_end(life);
            const bool beam_kept = life.what == own_life::kind::beam && beam_end != life.origin;
            // (Not where a death of that life ends it: the restore queued
            // that death, reconcile_own_lives.)
            const bool beam_death = !live && beam_kept && beam_end == tick && life.origin_live
                                 && (!life.made || life.applied != beam_end)
                                 && std::none_of(s.own_deaths_.begin(), s.own_deaths_.end(),
                                                 [&life, beam_end](const client_core_state::own_death& d) {
                                                     return d.entity == life.entity && d.generation == life.generation
                                                         && d.tick == beam_end;
                                                 });
            if (!beam_stamp && !remake_due && !beam_death) continue;
            // FULL step ODM: a same-name moved birth made at the end of a
            // moved own death - the records of [D, end) carry the old life
            // (extend_life), so the record of tick - 1 names its generation,
            // not this birth's: the birth is current there all the same.
            const bool after_moved_death = !live && remake_due && life.what == own_life::kind::birth
                && std::any_of(s.own_deaths_.begin(), s.own_deaths_.end(), [&](const client_core_state::own_death& d) {
                       uint32_t recorded = 0;
                       return d.moved && d.entity == life.entity && own_death_end(d) == tick
                           && s.rollback_.generation_at(life.entity, tick - 1, recorded) && recorded == d.generation;
                   });
            if (!after_moved_death && !own_life_current(life, tick, live)) {
                const auto generation = s.generations_.find(life.entity);
                // Step EL8: a replay never retires (only the live frames
                // do, live_own_lives / retire_own_lives): the next replay
                // may revive the life it belongs to.
                if (live) {
                    logf("own %s stamped %u retired at tick %u (%s): %s has had another life since (generation %u -> %u)",
                         life.what == own_life::kind::beam ? "beam" : "birth", life.origin, tick, where,
                         life.entity.c_str(), life.generation, generation == s.generations_.end() ? 0u : generation->second);
                    life.retired = true;
                } else if (s.own_lives_applied_ < 16) {
                    uint32_t recorded = 0;
                    s.rollback_.generation_at(life.entity, tick - 1, recorded);
                    logf("own %s stamped %u not applied at tick %u (%s): %s is in another life there (generation %u, the "
                         "record of tick %u: %u)", life.what == own_life::kind::beam ? "beam" : "birth", life.origin, tick,
                         where, life.entity.c_str(), life.generation, tick - 1, recorded);
                }
                continue;
            }
            if (beam_stamp) {
                if (!beam_remakes_body()) {
                    if (life.made) {
                        const double position[3] = {life.position[0], life.position[1], life.position[2]};
                        write_beam(life.entity, position, tick);
                    }
                    ++s.own_lives_applied_;
                } else if (beam_kept) {
                    // FULL step ODM: the old body lives on (beam_death, or
                    // the remake at the applied tick, ends it).
                } else if (life.made && life.applied == life.origin) {
                    remake_own_body(life, tick, where);
                } else {
                    unmake_own_body(life, tick, where);
                    ++s.own_lives_applied_;
                }
                if (live) life.origin_live = true;
                if (live && life.applied == life.origin) life.applied_live = true;
            }
            if (beam_death) {
                unmake_own_body(life, tick, where);
                ++s.own_lives_applied_;
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
            // FULL step ODM: a moved beam's old body ends here.
            if (life.what == own_life::kind::beam && life.origin_live && own_beam_end(life) == tick
                    && own_beam_end(life) != life.origin)
                return true;   // (over-reports one capped by a death: apply_own_lives then does nothing)
        }
        // FULL step ODM (review finding 1): a moved own death alone at its
        // tick still goes at our join rank among the tick's remote body
        // changes (drain_pending_lives), never after all of them as
        // apply_own_due's deaths in place do.
        for (const auto& d: s.own_due_)
            if (d.tick == tick && d.death && d.moved) return true;
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
        if (s.own_airship_release_ == frame_tick) {
            prepare_airship_release(frame_tick);
            apply_airship_releases(frame_tick, s.own_id_);
        }
    }

    // A record retires once the live run has made all of it and the
    // history no longer holds the tick before its last change - the
    // origin, or the applied tick of a moved one: no rewind can cross it
    // any more.  (A moved birth has to outlive its origin's anchor: a
    // rewind anchored inside the gap still crosses the applied tick, and
    // the record is what makes the body there.)
    //
    // An own event the server has not answered yet stays, past its history
    // too, until its echo or the deadline (expire_own_echoes) says where
    // the server made it: the answer past the history is a resync, and
    // without the record it could not take a never-made body out.  It
    // makes nothing in a replay (it is not moved), so keeping it is free.
    void session_client_core::retire_own_lives() {
        const uint32_t now = current_tick();
        for (auto& life: s.own_life_)
            if (life.retired && life.force_pending) refuse_forced_own_move(life, "its life ended first");
        s.own_life_.erase(std::remove_if(s.own_life_.begin(), s.own_life_.end(), [this, now](const own_life& life) {
            if (life.retired) return true;
            if (life.force_pending) return false;   // FULL step R6H: kept until its tick
            if (life.rereported) return false;      // FULL step R6P: kept until its re-report is answered
            if (s.resync_pending_ && s.resync_exact_ && life.what == own_life::kind::birth
                    && static_cast<int32_t>(life.applied - s.tick_base_) >= 0) return false;
            if (life.state == own_life::verdict::sent
                    && static_cast<int32_t>(now - life.origin) < static_cast<int32_t>(client_core_state::kOwnRecordKeepTicks))
                return false;
            const bool done = life.origin_live && (life.applied_live || !life.made);
            const uint32_t last = life.moved() ? life.applied : life.origin;
            return done && !s.rollback_.knows_tick(last - 1);
        }), s.own_life_.end());
        retire_own_deaths();
    }

    // A resync renumbers onto the server's tick and its full snapshot
    // puts the server's own ball in: the births go.  A beam the new
    // numbering has not reached yet is the server's to apply at its stamp,
    // and so ours: live once more.
    // Step RSO: an exact state before a confirmed own birth has no row
    // for it: keep its server-applied tick for the ordinary birth remake.
    // An unanswered one stays aside until its echo supplies that tick.
    void session_client_core::rebase_own_lives(bool exact) {
        clear_own_window_state();
        set_aside_unanswered(own_life::aside::resync);
        s.own_rereports_.clear();   // FULL step R6P: set aside above with the births
        s.own_life_.erase(std::remove_if(s.own_life_.begin(), s.own_life_.end(), [this, exact](own_life& life) {
            if (exact && life.what == own_life::kind::birth && life.made && !life.retired
                    && (life.state == own_life::verdict::acked || life.state == own_life::verdict::moved)
                    && static_cast<int32_t>(life.applied - s.tick_base_) >= 0
                    && own_life_current(life, current_tick(), true)) return false;
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
        // FULL step R6H (review finding 4): a forced move still pending is
        // refused aloud - the records it would move go with the old
        // numbering (both callers drop the births next).
        for (auto& life: s.own_life_)
            if (life.force_pending) refuse_forced_own_move(life, "a SessionAssign renumbered the history first");
        for (const auto& life: s.own_life_) {
            if (life.retired || life.what != own_life::kind::birth || life.state != own_life::verdict::sent) continue;
            own_life aside = life;
            aside.why = why;
            aside.aside_at = now;
            s.own_unanswered_.push_back(std::move(aside));
        }
        // FULL step R6P: an unanswered re-report is a birth of ours the
        // server may still make: set aside like one (answer_set_aside).
        for (const auto& again: s.own_rereports_) {
            if (again.state != own_life::verdict::sent) continue;
            own_life aside = again;
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
            if (life.origin == origin && life.ball_type == ball_type && life.why != own_life::aside::replaced) return &life;
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
                    && own_life_current(life, current_tick(), true))
                return &life;
        return nullptr;
    }

    // Step OWNB (SFD (A)): an own birth the scripts made live at
    // `birth_tick`, where the server applied it too (not moved), whose
    // lifetime is still the body's.  Parked, such a body kept the live
    // body's IVP collision bookkeeping (hull manager clock and values, hull
    // entries, overlap element) from ticks after its birth, beamed back to
    // its birth pose, into the replayed birth: every own ball a rewind
    // crossed differed there from every copy of it (resync_snapoff c0 at
    // 231: time_of_next_reset 16 against the server clone's 0).
    const client_core_state::own_life* session_client_core::scripted_own_life(const std::string& entity,
                                                                              uint32_t birth_tick) const {
        for (const auto& life: s.own_life_)
            if (!life.retired && life.what == own_life::kind::birth && life.made && !life.moved() && life.origin_live
                    && life.entity == entity && life.origin == birth_tick && own_life_current(life, current_tick(), true))
                return &life;
        return nullptr;
    }

    // The own birth destroy_moved_own_birth takes out, or null: none
    // without own body control (design 2.3: the branch is inert).  The one
    // test behind both destroy_own_birth and own_birth_destroyable
    // (rollback-engine gaps design 2.4, step B8EG item EGB).  A moved one
    // (late-player design 3.3), or (step OWNB) one the scripts made at its
    // stamp unless own_births_parked keeps it on the parking path.
    const client_core_state::own_life* session_client_core::destroyable_own_birth(const std::string& entity,
                                                                                  uint32_t birth_tick) const {
        if (!caps_.own_body_control) {
            // FULL step R6H: a forced move ran the capability path; a later
            // restore across its applied tick takes its body out the same way.
            const own_life* moved = moved_own_life(entity, birth_tick);
            return moved && moved->forced ? moved : nullptr;
        }
        if (const own_life* moved = moved_own_life(entity, birth_tick)) return moved;
        if (config.own_births_parked) return nullptr;
        return scripted_own_life(entity, birth_tick);
    }

    bool session_client_core::destroy_moved_own_birth(const std::string& entity, uint32_t birth_tick, bool& destroyed) {
        const own_life* life = destroyable_own_birth(entity, birth_tick);
        if (!life) return false;
        const own_life copy = *life;
        destroyed = unmake_own_body(copy, current_tick(), "restore");
        // Step OWNB: a birth the scripts made is made again at its birth
        // tick by the replay's drain, after that tick's remote events -
        // where the live frame's scripts made it (apply_own_due) - the way
        // the Physicalize made it and the server makes its clone
        // (remake_own_body).  What no replay reaches is made live after
        // the rewind (settle_own_due).  A moved birth's remake is
        // apply_own_lives' at its applied tick.
        if (destroyed && !copy.moved()) {
            client_core_state::own_due due;
            due.tick = copy.origin;
            due.entity = copy.entity;
            due.life = copy;
            due.scripted = true;
            s.own_due_.push_back(std::move(due));
            std::stable_sort(s.own_due_.begin(), s.own_due_.end(),
                             [](const client_core_state::own_due& a, const client_core_state::own_due& b) {
                                 if (a.tick != b.tick) return static_cast<int32_t>(a.tick - b.tick) < 0;
                                 return a.death && !b.death;
                             });
            ++s.own_births_remade_;
            if (s.own_births_remade_ <= 8)
                logf("own birth: %s made by the scripts at tick %u taken out at the restore, made again at that tick in the"
                     " replay (%llu so far)", copy.entity.c_str(), copy.origin,
                     static_cast<unsigned long long>(s.own_births_remade_));
        }
        return true;
    }

    // --------------------------------------------------------- echoes
    // The echoes of our own events that arrived since the last frame,
    // before this frame's other rewinds (process_own_echo): first those
    // queued before the last SessionAssign, which can only answer the
    // births it set aside, then the rest; then the births whose echo is
    // overdue (expire_own_echoes).
    //
    // FULL step ODM (late-player design 6.2.3 (d)): the frame's lifecycle
    // batch stays open over the echoes (pre_step opened it for the drain),
    // so a moved own death and the drain's late remote events share one
    // restore and one replay; it is run (flush_life_rewinds) after the
    // echoes and before the rest, which acted after the batch's rewind
    // before the step too.  Called outside pre_step, the echoes open it.
    void session_client_core::process_own_echoes() {
        s.life_batch_.collecting = true;
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
        s.life_batch_.collecting = false;
        flush_life_rewinds();
        settle_replaced_rereports();   // FULL step R6P (review finding 1)
        expire_own_echoes();
        apply_forced_own_moves();
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
        if (msg.type == bmmo::session::event_type::AirshipRelease) {
            if (msg.origin_tick != s.own_airship_inflight_) return;
            s.own_airship_inflight_ = 0;
            // Answered before the last SessionAssign: that resync's state is
            // the server's world after the answer, so nothing is undone - but
            // the release is no longer in flight (it used to stay so, and no
            // release was proposed again for the rest of the life).
            if (stale) return;
            if (msg.state & bmmo::session::EVENT_STATE_REJECTED) {
                flush_life_rewinds();
                s.airship_releases_.erase(std::remove_if(s.airship_releases_.begin(), s.airship_releases_.end(),
                        [&msg, this](const auto& release) { return release.player == s.own_id_ && release.tick == msg.origin_tick; }),
                        s.airship_releases_.end());
                if (platform_.airship_zone_release) platform_.airship_zone_release(s.own_body_entity_, msg.origin_tick, true);
                s.own_airship_tick_ = msg.origin_tick;
                journal_.lifecycle_event(s.own_id_, msg);
                logf("airship: release at %u refused: rewind to ghost and re-propose", msg.origin_tick);
                rewind_for_lifecycle(msg);
                ++s.own_echo_rejected_;
            } else ++s.own_echo_acked_;
            return;
        }
        // Step T3: a Trafo input's echo goes the input rewind's way (design
        // 4.6), never the own lives'.
        // FULL step ODM (late-player design 6.2.3 (d)): the frame's
        // lifecycle batch is still open here (pre_step), and only a moved
        // own death joins it.  Every other echo that may act runs after the
        // batch's rewind, as it did when the batch was flushed right after
        // the drain: flushed first.  A plain ack with no set-aside birth or
        // re-report to answer changes a record's state only.
        const bool rejected = (msg.state & bmmo::session::EVENT_STATE_REJECTED) != 0;
        const bool superseded = !rejected && (msg.state & bmmo::session::EVENT_STATE_SUPERSEDED) != 0;
        const bool made = !rejected && !superseded;
        const bool moved = made && msg.tick != msg.origin_tick;
        const bool joins = msg.type == bmmo::session::event_type::Unphysicalize && moved && !stale && own_deaths_move();
        const bool plain_ack = msg.type != bmmo::session::event_type::Trafo && made && !moved
                            && s.own_unanswered_.empty() && s.own_rereports_.empty();
        if (!joins && !plain_ack) flush_life_rewinds();
        if (msg.type == bmmo::session::event_type::Trafo) {
            process_trafo_echo(msg, stale);
            return;
        }
        const bool acting = acting_on_echoes();
        const bool birth = msg.type == bmmo::session::event_type::Physicalize;
        // FULL step R6P (Q-R6-NEVER (a)): the echo of a never-made birth's
        // re-report answers the original - its ack too, not only a MOVED
        // echo, so this comes before the ack branch below.
        if (birth && !stale && acting)
            for (size_t i = 0; i < s.own_rereports_.size(); ++i)
                if (s.own_rereports_[i].origin == msg.origin_tick && s.own_rereports_[i].ball_type == msg.ball_type
                        && s.own_rereports_[i].state == own_life::verdict::sent) {
                    answer_rereport(i, msg, made, superseded);
                    return;
                }
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
            if (life) {
                life->state = own_life::verdict::acked;
                freeze_moved_death_ends();   // FULL step ODM (review finding 2)
            }
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
            // FULL step ODM: a moved own death moves inside the history.
            if (joins) {
                logf("own %s stamped tick %u echoed %s by the server (applied %u, state %02x) at local tick %u",
                     event_type_name(static_cast<int>(msg.type)), msg.origin_tick, verdict, msg.tick,
                     static_cast<unsigned>(msg.state), current_tick());
                move_own_death(msg);
                return;
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
            // control the pair resyncs as it always did; with the control
            // and own_birth_move off, FULL step R6S, a lone one resyncs in
            // move_own_birth).
            if (superseded && !caps_.own_body_control) {
                move_own_birth(*life, msg.tick, made);
                return;
            }
            // FULL step ODM: with own deaths moving, the pair's death moves
            // with the birth (move_own_birth, extend_moved_deaths).
            if (!own_deaths_move()
                    && std::find(s.own_moved_unphysicalize_.begin(), s.own_moved_unphysicalize_.end(), msg.origin_tick)
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
        // FULL step R6P (review finding 1): a re-report (link: its original's
        // T) set aside by a resync and made after all.  The server replaced
        // its ball at `applied` with the re-report's pose; the resync's state
        // is of its base - 1, so one made at or past that base is in no state
        // this client took: kept, and a resync of its own once none is in
        // flight or pending (settle_replaced_rereports).
        if (made && life.link != 0 && static_cast<int32_t>(applied - s.tick_base_) >= 0) {
            keep_replaced_rereport(life, applied, "set aside by a resync");
            return;
        }
        // Step RSO: this echo answers the old numbering's birth, but its
        // applied tick is still the server's in the new numbering.  An
        // exact state before it must take the live body out and remake it
        // there, as an ordinary rewind does; never infer it from origin.
        if (made && !missing && s.resync_pending_ && s.resync_exact_ && !life.retired
                && static_cast<int32_t>(applied - s.tick_base_) >= 0
                && own_life_current(life, current_tick(), true)) {
            life.applied = applied;
            life.state = applied == life.origin ? own_life::verdict::acked : own_life::verdict::moved;
            life.why = own_life::aside::none;
            s.own_life_.push_back(std::move(life));
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
            if (life.retired || life.state != own_life::verdict::sent) continue;
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
            // FULL step R6P (Q-R6-NEVER (a)): while its life is current, the
            // birth is re-reported at the ball's current pose and that
            // re-report's echo answers it (re_report_current); no resync
            // against a world that has no ball of ours yet.
            if (life.what == own_life::kind::birth && own_births_move()
                && own_life_current(life, current_tick(), true) && !life.rereported) {
                re_report_current(life, current_tick(), "no echo by the server's tick " + std::to_string(server_tick));
                continue;
            }
            resync_own_life(life, false, life.origin, "no echo by the server's tick " + std::to_string(server_tick));
        }
        expire_rereports(server_tick);
    }

    // FULL step R6P (Q-R6-NEVER (a)): a re-report with no echo past the
    // same deadline as any birth.  One re-report per life, no loop: the
    // original is resynced as never made (resync_own_life: the body taken
    // out only on a host that declares own_life_take_out), and the
    // re-report is set aside as missing like any birth, so an echo that
    // comes after all is still answered (answer_set_aside).
    void session_client_core::expire_rereports(uint32_t server_tick) {
        for (size_t i = 0; i < s.own_rereports_.size();) {
            own_life& again = s.own_rereports_[i];
            if (again.state != own_life::verdict::sent
                    || !again.echo_deadline.missing(server_tick, s.acked_header_known_, s.header_acked_, again.origin,
                                                    client_core_state::kOwnEchoDeadlineTicks,
                                                    client_core_state::kOwnEchoUplinkSettleTicks)) {
                ++i;
                continue;
            }
            ++s.own_echo_missing_;
            own_life copy = again;
            s.own_rereports_.erase(s.own_rereports_.begin() + static_cast<std::ptrdiff_t>(i));
            copy.state = own_life::verdict::missing;
            own_life aside = copy;
            aside.why = own_life::aside::missing;
            aside.aside_at = current_tick();
            s.own_unanswered_.push_back(aside);
            while (s.own_unanswered_.size() > 16) s.own_unanswered_.erase(s.own_unanswered_.begin());
            bmmo::session_event_msg never;
            never.tick = never.origin_tick = copy.origin;
            never.type = bmmo::session::event_type::Physicalize;
            never.state = static_cast<uint8_t>(bmmo::session::EVENT_STATE_REJECTED);
            never.ball_type = copy.ball_type;
            std::copy(std::begin(copy.position), std::end(copy.position), never.position);
            std::copy(std::begin(copy.rotation), std::end(copy.rotation), never.rotation);
            never.recipe = copy.recipe;
            journal_.lifecycle_event(s.own_id_, never);
            journal_.note("own echo: Physicalize stamped " + std::to_string(copy.origin) + " (the re-report of "
                          + std::to_string(copy.link) + ") missing at the server's tick " + std::to_string(server_tick));
            if (own_life* original = rereported_original(copy)) {
                original->rereported = false;
                resync_own_life(*original, false, original->origin,
                                "its re-report for tick " + std::to_string(copy.origin) + " has no echo by the server's tick "
                                    + std::to_string(server_tick) + " either");
            } else {
                logf("own birth stamped %u: its re-report for tick %u has no echo by the server's tick %u; the original's "
                     "record is gone, resync", copy.link, copy.origin, server_tick);
                request_resync("own birth: resync", true);
            }
        }
    }

    // FULL step R6P: the original a re-report speaks for, while it waits.
    client_core_state::own_life* session_client_core::rereported_original(const own_life& rereport) {
        for (auto& life: s.own_life_)
            if (!life.retired && life.rereported && life.origin == rereport.link && life.link == rereport.origin
                    && life.entity == rereport.entity && life.generation == rereport.generation)
                return &life;
        return nullptr;
    }

    // FULL step R6P (review finding 1): a re-report the server made at
    // `applied` (it replaced its ball there with the re-report's pose,
    // physics_world's "found its body alive") where the state of a resync -
    // in flight, pending, or the one that set it aside - may not hold it.
    // Kept in own_unanswered_ as aside::replaced (find_unanswered skips it:
    // it is answered) until settle_replaced_rereports.
    void session_client_core::keep_replaced_rereport(const own_life& rereport, uint32_t applied, const char* when) {
        own_life kept = rereport;
        kept.state = own_life::verdict::moved;
        kept.applied = applied;
        kept.why = own_life::aside::replaced;
        kept.aside_at = current_tick();
        s.own_unanswered_.push_back(kept);
        while (s.own_unanswered_.size() > 16) s.own_unanswered_.erase(s.own_unanswered_.begin());
        const std::string head = "own birth stamped " + std::to_string(rereport.link) + ": its re-report for tick "
                               + std::to_string(rereport.origin) + " made at " + std::to_string(applied) + " (" + when
                               + ")";
        logf("%s at local tick %u: kept until no resync is in flight or pending", head.c_str(), current_tick());
        journal_.note(head + ": kept until the resync is done");
    }

    // FULL step R6P (review finding 1): the kept re-reports, once no resync
    // is in flight or pending.  The newest base past `applied`: that
    // resync's state (of base - 1) holds the server's replaced ball, nothing
    // to do.  Otherwise no state this client took holds it: a resync (owed
    // to the limiter if it holds it back).
    void session_client_core::settle_replaced_rereports() {
        if (s.resync_in_flight_ || s.resync_pending_) return;
        bool owed = false;
        uint32_t newest = 0;
        for (size_t i = 0; i < s.own_unanswered_.size();) {
            const own_life& kept = s.own_unanswered_[i];
            if (kept.why != own_life::aside::replaced) {
                ++i;
                continue;
            }
            const bool covered = static_cast<int32_t>(kept.applied - s.tick_base_) < 0;
            logf("own birth stamped %u: its re-report for tick %u made at %u, the tick base %u: %s", kept.link,
                 kept.origin, kept.applied, s.tick_base_,
                 covered ? "the resync's state holds it, nothing to do" : "no state holds it, resync");
            if (!covered) {
                owed = true;
                newest = kept.applied;
                ++s.own_moves_resynced_;
            }
            s.own_unanswered_.erase(s.own_unanswered_.begin() + static_cast<std::ptrdiff_t>(i));
        }
        if (!owed) return;
        journal_.note("own birth: a re-report made at " + std::to_string(newest) + ", past the tick base "
                      + std::to_string(s.tick_base_) + ": resync");
        request_resync("own birth: a re-report made past the resync base", true);
    }

    // FULL step R6P (the user's Q-R6-NEVER (a); core design "As built (step
    // R6D)" item 3.1): a birth the server never made (REJECTED, MISSING)
    // while its life is current.  Its Physicalize goes out again for now + 1
    // at the ball's CURRENT pose (the live body's, as the deleted
    // own_body_backstop's check_own_body did -
    // not the stamp's, so the server's ball is made where ours is, with no
    // pull-back), without the spawn flag (the body was kicked, or not,
    // long ago), with the life's recipe; recorded in own_rereports_ (origin
    // R, linked to the original's T), the original kept with its verdict
    // and the link back.  No resync yet: the re-report's echo answers the
    // original (answer_rereport) - made at R'', a move T -> R'' with the
    // re-report's pose (a resync only past the reach); never made, the
    // resync.  Once per life: the original's `rereported` stays set until
    // the answer, and the answers never re-report again.
    void session_client_core::re_report_current(own_life& life, uint32_t now, const std::string& why) {
        own_life again = life;   // entity, generation, ball type, recipe, group
        again.what = own_life::kind::birth;
        again.state = own_life::verdict::sent;
        again.why = own_life::aside::none;
        again.aside_at = 0;
        again.origin = again.applied = now + 1;
        again.made = true;
        again.spawn = false;
        again.origin_live = again.applied_live = true;
        again.retired = false;
        again.force_pending = again.forced = false;
        again.echo_deadline = bmmo::session::own_echo_deadline{};
        again.rereported = false;
        again.link = life.origin;
        bmmo_physics_body_state state{};
        const bool there = platform_.body_state(life.entity, state);
        if (there) {
            for (int k = 0; k < 3; ++k) again.position[k] = static_cast<float>(state.position[k]);
            quaternion_rows(state.rotation, again.rotation);
        }
        life.rereported = true;
        life.link = again.origin;
        bmmo::session_event_msg event;
        event.tick = again.origin;
        event.type = bmmo::session::event_type::Physicalize;
        event.ball_type = again.ball_type;
        std::copy(std::begin(again.position), std::end(again.position), event.position);
        std::copy(std::begin(again.rotation), std::end(again.rotation), event.rotation);
        event.recipe = again.recipe;
        s.own_rereports_.push_back(again);
        while (s.own_rereports_.size() > 16) s.own_rereports_.erase(s.own_rereports_.begin());
        const std::string head = "own birth stamped " + std::to_string(life.origin) + " never made ("
                               + why + "): re-reported for tick " + std::to_string(again.origin);
        logf("%s at local tick %u at %s (%.3f,%.3f,%.3f); its echo answers it", head.c_str(), now,
             there ? "the ball's current pose" : "its stamp's pose (no body)", static_cast<double>(again.position[0]),
             static_cast<double>(again.position[1]), static_cast<double>(again.position[2]));
        journal_.note(head + (there ? " at the ball's current pose" : " at its stamp's pose (no body)"));
        send_event(event);
    }

    // FULL step R6P (Q-R6-NEVER (a), core design "As built (step R6D)" item
    // 3.2): the echo of a re-report answers its original.  Made at R''
    // (acked or moved): the original is a move T -> R'' with the
    // re-report's pose and flags - no spawn flag, so the body is made at
    // rest and unkicked as the server made it - through move_own_birth
    // (the history without the body T..R''-1, the rewind to T-1, the body
    // made at R''; past the reach, a resync once the server's ball
    // exists).  Superseded (a newer accepted P / U of ours ended the life):
    // the original goes the superseded way (move_own_birth, never made).
    // Rejected: one re-report per life, so a resync (resync_own_life; the
    // body out only with own_life_take_out).
    void session_client_core::answer_rereport(size_t index, const bmmo::session_event_msg& msg, bool made, bool superseded) {
        const own_life again = s.own_rereports_[index];
        s.own_rereports_.erase(s.own_rereports_.begin() + static_cast<std::ptrdiff_t>(index));
        const bool moved = made && msg.tick != msg.origin_tick;
        const char* verdict = !made ? (superseded ? "superseded" : "rejected") : moved ? "moved" : "acked";
        if (made && !moved) {
            ++s.own_echo_acked_;
        } else {
            if (!made && !superseded) ++s.own_echo_rejected_;
            else if (superseded) ++s.own_echo_superseded_;
            else ++s.own_echo_moved_;
            if (moved) ++s.own_birth_echo_moved_;
            journal_.lifecycle_event(s.own_id_, msg);
        }
        const std::string head = "own birth stamped " + std::to_string(again.link) + ": its re-report for tick "
                               + std::to_string(again.origin) + " echoed " + verdict + " (applied "
                               + std::to_string(msg.tick) + ")";
        journal_.note(head);
        // FULL step R6P (review finding 1): made while a resync is in flight
        // (the original's late made echo asked for it, or anything else did).
        // That resync replaces the history, so no move; but its base may not
        // have reached the tick the server replaced its ball at, and a second
        // request now would be dropped (request_resync while one is in
        // flight): kept, settled once that resync is done.
        if (made && (s.resync_in_flight_ || s.resync_pending_)) {
            if (own_life* original = rereported_original(again)) {
                original->rereported = false;
                original->retired = true;
            }
            keep_replaced_rereport(again, msg.tick, "a resync in flight");
            return;
        }
        own_life* original = rereported_original(again);
        if (!original) {
            logf("%s at local tick %u: the original's record is gone, nothing to do", head.c_str(), current_tick());
            return;
        }
        logf("%s at local tick %u", head.c_str(), current_tick());
        original->rereported = false;
        if (made) {
            std::copy(std::begin(again.position), std::end(again.position), original->position);
            std::copy(std::begin(again.rotation), std::end(again.rotation), original->rotation);
            original->spawn = false;
            original->state = own_life::verdict::moved;
            move_own_birth(*original, msg.tick, true);
            return;
        }
        if (superseded) {
            original->state = own_life::verdict::superseded;
            move_own_birth(*original, msg.tick, false);
            return;
        }
        resync_own_life(*original, false, original->origin,
                        "its re-report for tick " + std::to_string(again.origin) + " was rejected too");
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
        if ((life.what == own_life::kind::birth || life.state == own_life::verdict::missing)
            && own_life_current(life, now, true)) {
            // FULL step R6S: with the control but own_birth_move off, the
            // no-capability answer (the body kept, a re-report).
            if (life.what == own_life::kind::birth && !own_births_move()) {
                if (!made && life.state != own_life::verdict::superseded) re_report_own_birth(life, now);
            } else {
                bmmo_physics_body_state state{};
                const bool there = platform_.body_state(life.entity, state);
                if (!made && caps_.own_life_take_out) {
                    if (life.what == own_life::kind::birth)
                        s.rollback_.move_birth(life.entity, life.generation, life.origin, life.origin, false);
                    if (there) unmake_own_body(life, now, "never made");
                    s.own_physicalized_ = false;
                    s.own_body_entity_.clear();
                } else if (!made) {
                    // FULL step R6P (Q-R6-NEVER (a) item 3.2): the host's
                    // game owns the life (no own_life_take_out): the body
                    // stays, live and in the history, and only the resync
                    // goes out.
                    logf("own %s stamped %u: never made, the body kept (the host cannot take a current life out)",
                         what, life.origin);
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
                      + std::to_string(now + 1)
                      + (caps_.own_body_control ? " (own_birth_move off)" : " (no own body control)"));
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
    //      made at T' by drain_pending_lives, at our join rank - or by
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
    // client resyncs (resync_own_life).  With the control and
    // core_config::own_birth_move off (FULL step R6S) the same, but for a
    // superseded birth: a resync (R6H-SUP (b), decision 57).
    void session_client_core::move_own_birth(own_life& life, uint32_t applied, bool made) {
        if (!own_births_move() && !life.forced) {
            const std::string head = std::string("own ") + (life.what == own_life::kind::beam ? "beam" : "birth")
                                   + " moved " + std::to_string(life.origin) + " -> "
                                   + (made ? std::to_string(applied) : std::string("never"));
            // FULL step R6S: the capability with the switch off answers as
            // without it, the journal naming the switch - but a superseded
            // birth resyncs (R6H-SUP (b), decision 57: the body this client
            // made stays, the server has none).
            const char* why = caps_.own_body_control ? "own_birth_move off" : "no own body control";
            if (made && snapshots_run()) {
                journal_.note(head + ": " + why + ", left to the snapshots");
                return;
            }
            if (!made && life.state == own_life::verdict::superseded && !caps_.own_body_control) {
                journal_.note(head + ": superseded, no own body control: nothing to do");
                return;
            }
            resync_own_life(life, made, applied, std::string(why) + (made ? ", no snapshots"
                                                                     : life.state == own_life::verdict::superseded
                                                                           ? ", superseded"
                                                                           : ""));
            return;
        }
        const uint32_t now = current_tick();
        const uint32_t anchor = life.origin - 1;
        // FULL step R6P (the user's Q-R6-NEVER (a)): a birth the server never
        // made (REJECTED; a MISSING one comes through expire_own_echoes)
        // while its life is current is not taken out of the history: it is
        // re-reported once at the ball's current pose, and that re-report's
        // echo answers it (answer_rereport).  A superseded birth is not (a
        // newer accepted event of ours overtook it), nor a life that has
        // ended since, nor a forced move.
        if (!made && life.what == own_life::kind::birth && life.state != own_life::verdict::superseded && !life.forced
                && !life.rereported && own_life_current(life, now, true)) {
            re_report_current(life, now, "rejected");
            return;
        }
        const char* what = life.what == own_life::kind::beam ? "beam" : "birth";
        const std::string reason = std::string("own ") + what + " moved " + std::to_string(life.origin) + " -> "
                                 + (made ? std::to_string(applied) : std::string("never"));
        if (!config.rollback || static_cast<int32_t>(now - anchor) < 0
                || now - anchor > s.rollback_.thresholds().max_resim_ticks || !s.rollback_.knows_tick(anchor)) {
            resync_own_life(life, made, applied, "past the reach (depth " + std::to_string(static_cast<int>(now - anchor)) + ")");
            return;
        }
        const bool alive = own_life_current(life, now, true);
        if (made && !alive) {
            resync_own_life(life, made, applied, "its life has ended since (plan step 22)");
            return;
        }
        // FULL step ODM: kept apart from the frame's batch (late-player
        // design 6.2.3 (d) lets it join): its refusal is resync_own_life's,
        // which puts the body set back the way the server has it, and its
        // live take-out below must come after the batch's replay, as it
        // did.  The batch runs first, so a frame still has one rewind per
        // correction in the order it had.
        flush_life_rewinds();
        life.made = made;
        life.applied = made ? applied : life.origin;
        if (life.what == own_life::kind::birth && alive) {
            bmmo_physics_body_state state{};
            if (platform_.body_state(life.entity, state)) unmake_own_body(life, now, "moved");
        }
        // FULL step ODM: a beam whose Unphysicalize was moved keeps its old
        // body to own_beam_end - the records lose the body from there on,
        // not from the stamp.  Then a death of ours this birth ends at the
        // server (its pair, or one it superseded) ends there here too
        // (own_death_end reads the applied tick just set): its records name
        // it again, after move_birth took the new body's name out of them.
        const uint32_t gap_from = life.what == own_life::kind::beam ? own_beam_end(life) : life.origin;
        const size_t changed = s.rollback_.move_birth(life.entity, life.generation, gap_from, life.applied, made);
        uint32_t extended_from = 0;
        if (own_deaths_move() && !extend_moved_deaths(extended_from)) {
            resync_own_life(life, made, applied, "a record of its death's span names another own body");
            return;
        }
        if (extended_from != 0) {
            ++s.own_deaths_moved_;
            ++s.own_pairs_moved_;
        }
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
        if (!rewind_world(anchor, now, input_at, reason)) {
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

    // ------------------------------------------------- forced own move
    // FULL step R6H (a test tool, core design 6.2 R6 row): the next own
    // birth record moved as if its echo said "made at origin + n", `after`
    // ticks after the birth (record_own_birth attaches it, process_own_echoes
    // applies it through apply_forced_own_moves).
    bool session_client_core::force_own_move(uint32_t n, uint32_t after) {
        if (!config.rollback || n == 0) {
            ++s.forced_own_moves_refused_;
            logf("forced own move: not armed (%s)", !config.rollback ? "no rollback: no history to move a birth in" : "n is 0");
            return false;
        }
        s.force_own_move_.armed = true;
        s.force_own_move_.n = n;
        s.force_own_move_.after = after;
        logf("forced own move: armed at local tick %u for the next own birth: moved %u ticks late, %u ticks after the birth%s",
             current_tick(), n, after, caps_.own_body_control ? "" : " (no own body control: the capability path forced)");
        return true;
    }

    void session_client_core::refuse_forced_own_move(own_life& life, const std::string& why) {
        life.force_pending = false;
        ++s.forced_own_moves_refused_;
        const std::string head = "forced own move: own birth stamped " + std::to_string(life.origin) + " -> "
                               + std::to_string(life.origin + life.force_n) + " refused at local tick "
                               + std::to_string(current_tick()) + ": " + why;
        logf("%s", head.c_str());
        journal_.note(head);
    }

    // Each armed birth whose tick has come: refused past the reach, while a
    // resync is pending, when its life has ended or the server has moved it
    // (each its own path: a resync, never this test's); otherwise moved
    // through move_own_birth's capability path (life.forced), the same
    // records, take-out and remake a moved own birth takes with own body
    // control.  The server's echo of the birth (an ack) finds no record left
    // to answer (the state is `moved`), so nothing else changes.
    void session_client_core::apply_forced_own_moves() {
        const uint32_t now = current_tick();
        for (size_t i = 0; i < s.own_life_.size(); ++i) {
            own_life& life = s.own_life_[i];
            if (!life.force_pending || static_cast<int32_t>(now - life.force_due) < 0) continue;
            const uint32_t anchor = life.origin - 1;
            const uint32_t applied = life.origin + life.force_n;
            if (life.retired || life.what != own_life::kind::birth || !own_life_current(life, now, true)) {
                refuse_forced_own_move(life, "its life has ended since");
                continue;
            }
            if (life.state != own_life::verdict::sent && life.state != own_life::verdict::acked) {
                refuse_forced_own_move(life, "the server has answered it otherwise (not an ack)");
                continue;
            }
            if (s.resync_pending_) {
                refuse_forced_own_move(life, "a resync is pending");
                continue;
            }
            if (!config.rollback || static_cast<int32_t>(now - anchor) < 0
                    || now - anchor > s.rollback_.thresholds().max_resim_ticks || !s.rollback_.knows_tick(anchor)) {
                refuse_forced_own_move(life, "past the reach (depth " + std::to_string(static_cast<int>(now - anchor)) + ")");
                continue;
            }
            life.force_pending = false;
            life.forced = true;
            life.state = own_life::verdict::moved;
            ++s.forced_own_moves_;
            const std::string head = "forced own move: own birth stamped " + std::to_string(life.origin) + " moved to "
                                   + std::to_string(applied) + " at local tick " + std::to_string(now) + " ("
                                   + std::to_string(now - life.origin) + " ticks after the birth; test override"
                                   + (caps_.own_body_control ? "" : ", no own body control") + ")";
            logf("%s", head.c_str());
            journal_.note(head);
            move_own_birth(life, applied, true);
        }
    }

}
