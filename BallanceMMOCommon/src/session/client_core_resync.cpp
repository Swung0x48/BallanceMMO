// The shared session-client core (docs/session-client-core-design.md 1.1):
// the exact resync (docs/resync-and-sector-timing-design.md 2.6, step A3).
//
// A resync's SessionAssign with ASSIGN_FLAG_STATE_FOLLOWS (protocol 10)
// renumbers the client from `base` as every re-assign does, and anchors the
// history at base - 1 = S with a synthetic record of the client's world as it
// stands (anchor_resync).  The server's state of S follows on the reliable
// channel, to this member alone (SessionResyncState: rows, the union, the
// portable script record and the server's IVP savestate).  It is applied as
// tick S (try_apply_resync): the scripts localized, the union merged with the
// stamp log, the anchor's structure reconciled to S's (what the anchor world
// built after S destroyed, the replay makes it again at its shared tick), and
// the server's blob restored as a travelling one under its rows; then S+1 ..
// now are re-stepped from the recorded inputs with the union re-derived, as a
// late Sector stamp's rewind does (step B1).  While it waits, the resync hold
// (poll_resync_hold) keeps the world from stepping its anchor out of the
// history.

#include "session/client_core.hpp"
#include "session/resync_rules.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace bmmo::session {

    namespace {
        // Step DX10 (review 3): the frames a resync state waits for a
        // mirror's navigation before it goes on with that mirror undriven
        // (the TRL naming point of a wait).
        constexpr uint32_t kNavigationWaitFrames = 60;
    }

    // post_step's tracked set, for a record made outside a frame (the
    // synthetic anchor): the own ball when it has a body, every navigated
    // mirror, every movable mechanism, the generations.
    rollback_tracked session_client_core::tracked_now(bool physicalized) const {
        rollback_tracked tracked;
        const std::string own_entity = platform_.own_driven_entity();
        if (physicalized && !own_entity.empty()) {
            tracked.own_entity = own_entity;
            tracked.own_polls = true;
        }
        for (const auto& [id, remote]: s.remotes_)
            if (remote.physicalized && remote.navigation) tracked.remote_entities.push_back(remote.entity);
        tracked.mechanisms = movable_mechanisms();
        tracked.generations = s.generations_;
        return tracked;
    }

    // 2.6.1 steps 2-5, after the re-assign's renumbering and invalidation:
    // the world as it stands is what the frames numbered base, base + 1, ...
    // step from, so it is recorded as base - 1 (its structure is that of the
    // old numbering's tick R, delta ticks off; the apply reconciles it).
    void session_client_core::anchor_resync(uint32_t base, uint32_t old_tick) {
        const uint32_t anchor = base - 1;
        s.resync_base_ = base;
        s.state_anchored_ = true;   // step RLT: relayed frames for ticks <= anchor are the state's
        s.resync_old_tick_ = old_tick;
        s.resync_delta_ = static_cast<int64_t>(old_tick) - static_cast<int64_t>(anchor);
        s.resync_wait_frames_ = 0;
        s.have_resync_state_ = false;
        s.resync_state_ = bmmo::session_resync_state_msg{};
        s.pending_deaths_.clear();
        s.resync_hold_spent_ = false;
        platform_.note_script_resets(anchor);
        s.rollback_.record(world(), anchor, tracked_now(s.own_physicalized_), {});
        s.union_records_[anchor] = s.sectors_.state();
        // G-a: the host's activation records and script reset ticks of the
        // old numbering's frames from base on (the birth log stays).
        platform_.forget_activations_from(base);
        logf("resynced: tick base %u (old tick %u, delta %+lld): anchored at %u, the server's state of it follows", base,
             old_tick, static_cast<long long>(s.resync_delta_), anchor);
        journal_.note("resync: tick base " + std::to_string(base) + " (old tick " + std::to_string(old_tick) + ", delta "
                      + std::to_string(s.resync_delta_) + "): anchored at " + std::to_string(anchor));
    }

    void session_client_core::on_session_resync_state(bmmo::session_resync_state_msg&& msg) {
        ++s.resync_states_;
        if (msg.session != s.session_ || s.phase_ != client_core_state::phase::running) return;
        // The rows as received, like every snapshot's (flag RESYNC).
        if (journal_.journalling())
            journal_.writer().checkpoint(msg.tick, static_cast<uint8_t>(bmmo::session::JOURNAL_CHECKPOINT_RECEIVED
                                                                        | bmmo::session::JOURNAL_CHECKPOINT_FULL
                                                                        | bmmo::session::JOURNAL_CHECKPOINT_RESYNC),
                                         msg.rows);
        // Only the state of the pending exact resync's anchor: a stale one is
        // for a base this client has left (a newer assignment came).
        if (!s.resync_pending_ || !s.resync_exact_ || msg.base != s.resync_base_ || msg.tick + 1 != msg.base) {
            ++s.resync_states_stale_;
            logf("resync: a state of tick %u (base %u) is not the pending resync's (%s base %u): dropped", msg.tick, msg.base,
                 s.resync_pending_ && s.resync_exact_ ? "pending" : "none pending, last", s.resync_base_);
            return;
        }
        // The packed sections back to their bytes (the server packs the
        // record and the blob: see session_resync_state_msg).
        for (auto [bytes, size, what]: {std::tuple<std::vector<uint8_t>*, uint32_t, const char*>{&msg.scripts, msg.scripts_size, "script record"},
                                        {&msg.savestate, msg.savestate_size, "savestate"}}) {
            if (!size) continue;
            std::vector<uint8_t> out;
            if (!platform_.unpack_bytes || !platform_.unpack_bytes(*bytes, size, out) || out.size() != size) {
                ++s.resync_states_refused_;
                logf("resync: the state of tick %u: its %s (%zu bytes packed, %u unpacked) does not unpack%s: asking again",
                     msg.tick, what, bytes->size(), size, platform_.unpack_bytes ? "" : " (no unpack here)");
                s.resync_in_flight_ = false;
                request_resync("the resync state does not unpack", true);
                return;
            }
            bytes->swap(out);
        }
        msg.scripts_size = msg.savestate_size = 0;
        s.resync_state_ = std::move(msg);
        s.have_resync_state_ = true;
        s.resync_state_arrived_ = current_tick();
        s.resync_wait_frames_ = 0;
        // Applied after the drain this message came in (poll_resync_hold,
        // which every host calls before pre_step), not here: the held notices
        // queued behind the state in the same drain - the server's
        // substitutions of the new epoch's first ticks - are stored as base
        // while the resync is pending, so the re-step replays them instead of
        // each asking for a rewind past max_resim_ticks after it.
    }

    // Gap L, L2 (rollback-engine gaps design 2.3.3, step EL7): the entry
    // carries the event (a Physicalize's pose, recipe, ball type, flags and
    // origin) and later the entity it made or removed (note_life_made); it
    // is made before the event's own rewind.  The trim keeps every entry of
    // the last history_ticks + 128 ticks and, of the older ones, each
    // player's newest Physicalize: the A-life of any anchor in the history
    // is the player's newest Physicalize stamped at or before it, and when
    // that is older than the horizon no Physicalize of the player lies
    // between it and the horizon, so it is the kept entry.  Each player's
    // newest older entry of any kind stays too (review finding 2): a death
    // after that birth, trimmed, would let the reconcile's consistency test
    // pass and revive a life that is over (an exact resync makes a mirror
    // from its row with no log entry, which a stale birth would then
    // stand in for).
    void session_client_core::log_life(const bmmo::session_event_msg& event) {
        client_core_state::life_entry entry;
        entry.applied = event.tick;
        entry.origin = event.origin_tick;
        entry.player = event.player;
        entry.type = event.type;
        if (event.type == bmmo::session::event_type::Physicalize) entry.birth = client_core_state::pending_birth::of(event);
        s.life_log_.push_back(std::move(entry));
        const uint32_t keep = static_cast<uint32_t>(s.rollback_.thresholds().history_ticks) + 128u;
        const auto old = [&](const client_core_state::life_entry& e) {
            return static_cast<int32_t>(e.applied + keep - event.tick) < 0;
        };
        if (s.life_log_.empty() || !old(s.life_log_.front())) return;
        // Of the old entries, each player's newest Physicalize stays, and
        // its newest entry of any kind (a death after that birth).
        std::map<uint32_t, size_t> newest_old_birth, newest_old_entry;
        const auto newer = [&](std::map<uint32_t, size_t>& newest, const client_core_state::life_entry& e, size_t i) {
            const auto [it, fresh] = newest.try_emplace(e.player, i);
            if (!fresh && static_cast<int32_t>(e.applied - s.life_log_[it->second].applied) >= 0) it->second = i;
        };
        for (size_t i = 0; i < s.life_log_.size(); ++i) {
            const auto& e = s.life_log_[i];
            if (!old(e) || e.skipped) continue;
            newer(newest_old_entry, e, i);
            if (e.type == bmmo::session::event_type::Physicalize) newer(newest_old_birth, e, i);
        }
        const auto is_kept = [](const std::map<uint32_t, size_t>& newest, uint32_t player, size_t i) {
            const auto it = newest.find(player);
            return it != newest.end() && it->second == i;
        };
        std::deque<client_core_state::life_entry> kept;
        for (size_t i = 0; i < s.life_log_.size(); ++i) {
            auto& e = s.life_log_[i];
            if (old(e) && !is_kept(newest_old_birth, e.player, i) && !is_kept(newest_old_entry, e.player, i)) continue;
            kept.push_back(std::move(e));
        }
        s.life_log_.swap(kept);
    }

    void session_client_core::drain_pending_deaths(uint32_t tick) {
        if (s.pending_deaths_.empty()) return;
        for (auto it = s.pending_deaths_.begin(); it != s.pending_deaths_.end();) {
            if (it->tick != tick) { ++it; continue; }
            const uint32_t player = it->player;
            it = s.pending_deaths_.erase(it);
            const event_log_mute mute(*this);
            unphysicalize_remote(player, tick);
        }
    }

    // 2.6.4: the apply.  False while it waits (the state is kept and tried
    // again the next frame); true once it ran, applied or refused for good.
    bool session_client_core::try_apply_resync(const char* why) {
        if (!s.have_resync_state_ || !s.resync_pending_ || !s.resync_exact_) return false;
        auto& st = s.resync_state_;
        const uint32_t anchor = s.resync_base_ - 1;
        const bool stepped = s.frames_since_anchor_ >= 1;
        const uint32_t now = stepped ? current_tick() : anchor;
        const uint32_t depth = now - anchor;
        // 1. The anchor still recorded, and the re-step inside the reach.
        if (!s.rollback_.knows_tick(anchor) || depth > s.rollback_.resync_reach()) {
            ++s.resync_states_refused_;
            logf("resync: the state of tick %u cannot be applied at tick %u (%s): the anchor left the history (depth %u, "
                 "reach %u); asking again", anchor, now, why, depth, s.rollback_.resync_reach());
            if (journal_.journalling()) {
                journal_resync r;
                r.tick = anchor;
                r.base = s.resync_base_;
                r.delta = static_cast<int32_t>(s.resync_delta_);
                r.applied_at = now;
                r.outcome = JOURNAL_RESYNC_TIMED_OUT;
                journal_.writer().resync(r);
            }
            s.have_resync_state_ = false;
            s.resync_state_ = bmmo::session_resync_state_msg{};
            s.resync_in_flight_ = false;
            request_resync("the resync state's anchor left the history", true);
            return true;
        }
        // 1b. Step T3 (trafo-replication design 4.7): a set for every player
        // the server's ledger names, and the rows of the pieces it has bodies
        // for at S - the reconcile below makes them when this world lacks
        // them, so they wait for nothing.
        std::set<std::string> trafo_rows;
        if (s.trafo_active_)
            for (const auto& e: st.trafo_ledger) {
                const uint16_t life = e.life ? e.life : uint16_t{1};   // step TRL: the set is (player, life)'s
                if (e.type >= kTrafoTypes || !ensure_trafo_set(e.player, life, "the resync ledger", true)) continue;
                // Step TRL: the final marks with the set, so a set made for
                // a state that is then not applied (a wait, a timeout) still
                // retires by the rule (the reconcile marks them again).
                // final_applied is in the numbering of the assign before
                // this state; a state dropped after a re-assign with a
                // non-zero delta leaves the mark that far off until the next
                // state's reconcile marks the named sets again and retires
                // the ones it does not name (review 7).
                if (e.final) platform_.trafo_final(e.player, life, e.type, e.final_applied);
                if (!e.has_bodies) continue;
                for (auto& piece: platform_.trafo_type_set(e.player, life, e.type).pieces) trafo_rows.insert(std::move(piece));
            }
        // 2. The rows onto this world's bodies.  The registry starts over
        // from the state's dictionary (the re-assign cleared it).
        for (const auto& body: st.rows)
            if (body.kind == body_kind::Mechanism && !keep_local_body(body.name)) {
                if (!body.name.empty()) s.mechanism_tracking_.note_name(body.owner, body.name);
                s.mechanism_tracking_.note_row(anchor, body);
            }
        s.mechanism_tracking_.mark_dirty();
        s.mechanism_tracking_.resolve(anchor, [this](const char* entity, bmmo_physics_body_state& out) {
            return platform_.body_state(entity, out);
        });
        const std::string own_entity = s.own_physicalized_ ? platform_.own_driven_entity() : std::string();
        // Step BALA (the airship rule: no correction, compare, resync or
        // rollback acts on a ball that left the sync): the own ball in its
        // airship zone at S is the server's frozen ghost there, not ours.  It
        // keeps this world's own state - no row, no navigation, out of the
        // server's blob by its exact name, so the restore takes it from the
        // anchor's own record like a keep-local body - and the re-step drives
        // it from its records (never amended from the zone tick on).  An
        // entry after S is re-stepped from the server's state of S, where the
        // ball was still in the sync.
        const bool own_kept = !own_entity.empty() && own_out_of_sync_at(anchor);
        // Step RSX (2.6.3, the own ball's row "always"): an own life alive at
        // S that ended inside the window has no body now; the server's row
        // at S is its, and lands on it once reconcile_own_lives has made it
        // again under the restore (its death queued at its stamp).  Before
        // the step the row was unmapped and the re-step ran without the
        // ball until its death (fan_dr client 0, WG3-dr).
        const std::string own_revive = own_entity.empty() ? resync_own_revive_entity(anchor, now) : std::string();
        // The deaths after S this world applied already (2.6.3: a mirror
        // row at S whose Unphysicalize came after S): made from the row,
        // killed again at its tick in the replay.
        const auto death_after = [&](uint32_t player, uint32_t& tick, uint32_t& origin) {
            for (auto it = s.life_log_.rbegin(); it != s.life_log_.rend(); ++it)
                if (it->player == player && it->type == event_type::Unphysicalize
                        && static_cast<int32_t>(it->applied - anchor) > 0) {
                    tick = it->applied;
                    origin = it->origin;
                    return true;
                }
            return false;
        };
        std::map<std::string, const body_state*> rows;
        std::vector<std::string> waiting;
        std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> deaths;   // player, tick, origin: mirrors made from their row
        uint32_t unmapped = 0;
        for (const auto& body: st.rows) {
            if (body.kind == body_kind::Ball) {
                if (body.owner == s.own_id_) {
                    if (own_kept) continue;
                    if (!own_entity.empty()) rows[own_entity] = &body;
                    else if (!own_revive.empty()) rows[own_revive] = &body;   // step RSX
                    else ++unmapped;   // the own lifecycle is ours: the anchor's own ball stands
                    continue;
                }
                const auto it = s.remotes_.find(body.owner);
                if (it != s.remotes_.end() && it->second.physicalized) {
                    // Step DX10 (J1b): the server's state at S drives this
                    // mirror from S + 1, and a replay drives only a navigated
                    // one (replay_remotes): a mirror made before the host
                    // knew the navigation graph (the attach answered
                    // `unavailable`) waits for it here like a missing body.
                    // Only a ball the server navigated at S; never a failed
                    // attach (no later frame would make it succeed).
                    if (!it->second.navigation
                            && std::any_of(st.navs.begin(), st.navs.end(),
                                           [&](const auto& nav) { return nav.first == body.owner; })) {
                        auto& remote = it->second;
                        const mirror_navigation answer = attach_remote_navigation(body.owner, remote);
                        // An attach made here is the discovery's
                        // (post_step): the ticks before this one ran the
                        // mirror undriven, so a rewind before this state
                        // applies (the wait goes on for another body, or the
                        // state is dropped) must not drive it across them.
                        // An apply that proceeds lowers it to S + 1 below.
                        const uint32_t tick = current_tick();
                        if (remote.navigation && static_cast<int32_t>(tick - remote.driven_from) > 0)
                            remote.driven_from = tick;
                        if (answer == mirror_navigation::unavailable) {
                            // Bounded (review 3): a host that never learns
                            // the graph (retail's leaf rule: fewer than four
                            // keyed leaves) would wait for good; after the
                            // TRL naming point the state goes on with the
                            // mirror undriven, as before DX10.
                            if (s.resync_wait_frames_ < kNavigationWaitFrames) {
                                waiting.push_back("the navigation of the ball of " + std::to_string(body.owner));
                                continue;
                            }
                            logf("resync: the state of tick %u goes on at tick %u without the navigation of the ball of %u "
                                 "(not known after %u frames): its mirror stays undriven", anchor, now, body.owner,
                                 s.resync_wait_frames_);
                        }
                    }
                    rows[it->second.entity] = &body;
                    continue;
                }
                // Step F2 (DRJ): any recorded birth makes it, not only one
                // made at its stamp (birth_known).  A late joiner's or a
                // rejoiner's mirrors born before its anchor are made live at
                // base (birth_known false); one whose owner left inside the
                // wait was never found again, and the state waited until the
                // hold timed out (trl_live_B / B2 client 1, ball of the 5 s
                // leaver, its death at 1301 after S 1296).  The blob puts
                // the body at S whatever pose it was made at.  The
                // birth.player term is a defensive check only: remotes_ is
                // keyed by player and every write of remote.birth comes from
                // that player's own event, so it cannot fail today.
                uint32_t died = 0, died_origin = 0;
                if (it != s.remotes_.end() && it->second.birth.tick != 0 && it->second.birth.player == body.owner
                        && death_after(body.owner, died, died_origin)) {
                    deaths.emplace_back(body.owner, died, died_origin);
                    continue;
                }
                waiting.push_back("ball of " + std::to_string(body.owner));
                continue;
            }
            if (keep_local_body(body.name)) continue;
            if (trafo_rows.count(body.name)) continue;   // step T3: after the reconcile
            std::string entity = s.mechanism_tracking_.entity_of(body.owner);
            // The registry's identity guard refuses a pick whose pose is far
            // from the row's (a twin of another sector); the resync takes the
            // server's state by name (its blob does too), so a body of that
            // name is the row's however far this world has taken it.
            bmmo_physics_body_state probe{};
            if (entity.empty() && !body.name.empty() && platform_.body_state(body.name, probe)) entity = body.name;
            if (entity.empty()) {
                waiting.push_back(body.name.empty() ? "#" + std::to_string(body.owner) : body.name);
                continue;
            }
            rows[entity] = &body;
        }
        // The server's body set is not all here yet: a pass the server ran
        // before S that this world runs live now (its bodies in frame M+5),
        // a birth drained late at base.  The state waits (2.6.4 step 2).
        if (!waiting.empty()) {
            if (s.resync_wait_frames_++ == 0) {
                ++s.resync_deferred_;
                logf("resync: the state of tick %u waits at tick %u for %zu bod%s this world does not have yet (%s first)",
                     anchor, now, waiting.size(), waiting.size() == 1 ? "y" : "ies", waiting.front().c_str());
            }
            // Step TRL (review 4): a wait still open after 60 frames names
            // what it waits for (run B's 1 s rejoiner waited until the hold
            // timed out, and only the first body was logged).
            if (s.resync_wait_frames_ == 60) {
                std::string names;
                for (size_t i = 0; i < waiting.size() && i < 24; ++i) names += (i ? ", " : "") + waiting[i];
                logf("resync: the state of tick %u still waits at tick %u for %zu bod%s: %s%s", anchor, now, waiting.size(),
                     waiting.size() == 1 ? "y" : "ies", names.c_str(), waiting.size() > 24 ? ", ..." : "");
            }
            ++s.resync_deferred_ticks_;
            return false;
        }
        if (s.resync_wait_frames_)
            logf("resync: the state of tick %u waited %u frame(s) for its bodies", anchor, s.resync_wait_frames_);
        // 2b. Step T3: the trafo reconcile (design 4.7), outside the replay
        // and before the restore, then the pieces' rows onto the bodies it
        // left (the names are equal in every world).
        std::set<std::string> trafo_gone;
        std::vector<journal_trafo_entry> trafo_journal;
        if (s.trafo_active_) {
            reconcile_trafo(st, anchor, trafo_gone, trafo_journal);
            for (const auto& body: st.rows) {
                if (body.kind != body_kind::Mechanism || !trafo_rows.count(body.name)) continue;
                bmmo_physics_body_state probe{};
                if (platform_.body_state(body.name, probe)) rows[body.name] = &body;
                else ++unmapped;
            }
        }
        s.resync_rows_unmapped_ += unmapped;
        // 3. The scripts: the server's record, localized, the keep-local ones
        // from the anchor's own (2.4).  Refused: the anchor's own scripts,
        // and the rest of the apply as designed (never silent).
        // The server's clone of each player -> this world's ball of that
        // player (the own ball as the anchor recorded it, every mirror):
        // the blob's renames (5.) and, step G3, the scripts' references to a
        // clone (a latched Get Cell output, world-agreement design 2.9).
        const auto clone_renames = [&]() {
            std::map<uint32_t, std::string> local;
            std::string local_own = s.rollback_.recorded_own_entity(anchor);
            if (local_own.empty()) local_own = own_entity;
            if (local_own.empty()) local_own = platform_.own_ball().entity;
            local[s.own_id_] = local_own;
            for (const auto& [player, remote]: s.remotes_)
                if (remote.physicalized) local[player] = remote.entity;
            return resync_renames(st.clones, local);
        };
        std::vector<uint8_t> localized;
        resync_script_report script_report;
        // Under the union only (the bit-clear clients keep today's own-ball
        // sensors, and their localize stays as it was).
        if (s.probes_active_) script_report.renames = clone_renames();
        std::string script_error;
        uint8_t outcome = JOURNAL_RESYNC_APPLIED;
        const bool scripts_ok = !st.scripts.empty()
            && platform_.localize_scripts(st.scripts, s.rollback_.recorded_scripts(anchor), localized, script_error,
                                          script_report);
        if (!scripts_ok) {
            ++s.resync_scripts_refused_;
            outcome = JOURNAL_RESYNC_REFUSED_SCRIPTS;
            localized.clear();
            logf("resync: the server's script state was refused at %s", st.scripts.empty() ? "an empty record" : script_error.c_str());
        }
        s.resync_unportable_ += script_report.unportable;
        // Step G3: the server's proximity hold at S (portable version 3).
        if (scripts_ok && script_report.hold)
            logf("resync: the server's proximity hold at %u %s; %u clone reference(s) renamed", anchor,
                 script_report.hold == 1 ? "localized into the record" : "left out (no localizer here: the anchor's own kept)",
                 script_report.objects_renamed);
        // 4. The union: the server's at S, its stamps not applied yet from
        // the stamp log (2.5 "the merge").
        sector_union::state_type server_union;
        if (!sector_union::deserialize(st.union_state.data(), st.union_state.size(), server_union)) {
            logf("resync: the server's union state does not read (%zu bytes): the anchor's own kept", st.union_state.size());
            server_union = s.union_records_.count(anchor) ? s.union_records_.at(anchor) : s.sectors_.state();
        }
        const auto merged = sector_union::with_pending(server_union, stamp_log());
        s.union_records_[anchor] = merged;
        // The mirrors dead after S: made now from their last birth, the
        // blob puts them at the server's state of S, and their death is
        // applied again at its tick - in the replay, or live when it lies
        // past `now` (Delta > 0: the old numbering ran further than the new
        // one has; pre_step drains it then).  Made before the renames and the
        // navigation below, which name every mirror the blob must find.
        for (const auto& [player, died, died_origin]: deaths) {
            auto& remote = s.remotes_[player];
            bool made = false;
            {
                const event_log_mute mute(*this);
                made = physicalize_remote(remote.birth, anchor);
            }
            if (made) {
                attach_remote_navigation(player, remote, true);
                for (const auto& body: st.rows)
                    if (body.kind == body_kind::Ball && body.owner == player) rows[remote.entity] = &body;
                client_core_state::pending_death death;
                death.player = player;
                death.tick = died;
                death.origin = died_origin;
                s.pending_deaths_.push_back(death);
                logf("resync: mirror of %u made from its row at S %u, its death at %u applied again %s", player, anchor,
                     died, static_cast<int32_t>(died - now) <= 0 ? "in the replay" : "live");
            } else {
                ++s.resync_rows_unmapped_;
            }
        }
        // Step DX10 (J1b, world-agreement gaps "As built (step H4)"): the
        // server's state at S drives every mirror alive at S.  A replay
        // drives a mirror from its driven_from on (replay_remotes), which a
        // late live birth (the catch-up birth made at the tick it was
        // drained, seed 3: 1669 for S 1655) or the navigation's discovery
        // (seeds 2 and j1: S + 3) put after S + 1: the re-step left the
        // mirror undriven on the ticks between, on the live prediction's
        // hold-last keys.  Keyed on the row, not on the local birth.  Its
        // born_tick goes with it: the body this history has is the
        // server's of S (the restore writes the row), so no rewind parks it
        // at a later birth record any more (the relay's birth-frozen rule
        // and remakes_birth read born_tick), and it is no longer the body
        // its Physicalize made (birth_known).
        uint32_t driven_moved = 0;
        // What the block below changed, put back if the re-step is refused
        // (review 1): the world is then still the live one, whose births
        // and driven ranges these are.
        struct moved_mirror { uint32_t player, driven_from, born_tick; bool birth_known; };
        std::vector<moved_mirror> moved_mirrors;
        for (auto& [player, remote]: s.remotes_) {
            if (!remote.physicalized || !rows.count(remote.entity)) continue;
            const moved_mirror before{player, remote.driven_from, remote.born_tick, remote.birth_known};
            bool moved = false;
            if (static_cast<int32_t>(remote.driven_from - (anchor + 1)) > 0) {
                if (config.trace)
                    logf("resync: mirror %s of %u driven from %u (was %u, born %u)", remote.entity.c_str(), player, anchor + 1,
                         remote.driven_from, remote.born_tick);
                remote.driven_from = anchor + 1;
                moved = true;
            }
            if (static_cast<int32_t>(remote.born_tick - anchor) > 0) {
                remote.born_tick = anchor;
                remote.birth_known = false;
                moved = true;
            }
            if (moved) {
                ++driven_moved;
                moved_mirrors.push_back(before);
            }
        }
        // 5. The reconcile (2.6.3): the anchor's bodies the server's state at
        // S has no row for.
        resync_spec spec;
        spec.scripts = std::move(localized);
        spec.rows = rows;
        spec.gone = trafo_gone;   // step T3
        // Step LJ1 (world-agreement gaps "As built (step H6)", shape (i)):
        // the mirrors the server's state drives from S + 1 (the block
        // above: physicalized, a row at S) that a replay drives
        // (replay_remotes: navigated) go into the re-captured anchor
        // record, so a later rewind to S restores them from it.
        for (const auto& [player, remote]: s.remotes_)
            if (remote.physicalized && remote.navigation && rows.count(remote.entity)) spec.mirrors.push_back(remote.entity);
        spec.blob.blob = &st.savestate;
        spec.blob.excluded_prefixes = keep_local_body_prefixes();
        {
            std::string local_own = s.rollback_.recorded_own_entity(anchor);
            if (local_own.empty()) local_own = own_entity;
            if (local_own.empty()) local_own = platform_.own_ball().entity;
            if (own_kept && local_own != own_entity) spec.blob.excluded_prefixes.push_back("=" + local_own);
            spec.blob.renames = clone_renames();
        }
        if (own_kept) {
            spec.blob.excluded_prefixes.push_back("=" + own_entity);
            ++s.resyncs_own_kept_;
            logf("resync: the own ball %s left the sync in its airship zone at tick %u (S %u): kept as this world has it",
                 own_entity.c_str(), s.own_airship_tick_, anchor);
        }
        // The server's navigation of every ball at S onto this world's
        // entity of that player (the own replica, a driven mirror).
        uint32_t navs = 0;
        for (const auto& [player, nav]: st.navs) {
            std::string entity;
            if (player == s.own_id_) entity = s.own_physicalized_ && !own_kept ? platform_.own_driven_entity() : own_revive;
            else if (const auto it = s.remotes_.find(player); it != s.remotes_.end() && it->second.navigation)
                entity = it->second.entity;
            if (entity.empty()) continue;
            spec.navs[entity] = nav;
            ++navs;
        }
        uint32_t keep_local = 0, local_only = 0, destroy = 0, unattributed = 0, unexplained = 0;
        std::string unexplained_names;
        if (const rollback_tracked* tracked = s.rollback_.recorded_tracked(anchor)) {
            for (const std::string& name: tracked->mechanisms) {
                if (rows.count(name) || trafo_gone.count(name)) continue;
                const bool only_here = platform_.local_only_body && platform_.local_only_body(name);
                const uint32_t birth = platform_.birth_tick_of ? platform_.birth_tick_of(name) : 0;
                int sector = 0;
                switch (classify_unrowed(name, only_here, birth, anchor, s.pass_log_, server_union.active, &sector)) {
                case resync_unrowed::keep_local: ++keep_local; break;
                case resync_unrowed::local_only: ++local_only; break;
                case resync_unrowed::destroy:
                    spec.destroy[name] = birth;
                    ++destroy;
                    break;
                case resync_unrowed::unattributed:
                    ++unattributed;
                    if (unattributed <= 4) logf("resync: %s born at %u after S %u is no union pass's: kept", name.c_str(), birth, anchor);
                    break;
                case resync_unrowed::unexplained:
                    ++unexplained;
                    if (unexplained <= 6) unexplained_names += " " + name + "@" + std::to_string(birth);
                    break;
                }
            }
            // The mirrors the anchor world made after S (the server has no
            // row for them at S): made again by the replay at their birth
            // tick (step LB's remake), as a window birth is.
            for (const auto& [player, remote]: s.remotes_) {
                if (!remote.physicalized || rows.count(remote.entity)) continue;
                if (std::find(tracked->remote_entities.begin(), tracked->remote_entities.end(), remote.entity)
                        == tracked->remote_entities.end())
                    continue;
                if (static_cast<int32_t>(remote.born_tick - anchor) > 0 && remakes_birth(remote)) {
                    spec.destroy[remote.entity] = remote.born_tick;
                    ++destroy;
                } else {
                    ++unexplained;
                    if (unexplained <= 6) unexplained_names += " " + remote.entity;
                }
            }
        }
        s.resync_unexplained_ += unexplained;
        s.resync_unattributed_ += unattributed;
        s.resync_destroyed_ += destroy;
        if (unexplained) logf("resync: %u bod%s of the anchor world have no row at S %u and no reason to:%s (kept)", unexplained,
                              unexplained == 1 ? "y" : "ies", anchor, unexplained_names.c_str());
        // --trace: what the re-step will drive each mirror with - the relayed
        // frames it has in (S, now].
        if (config.trace)
            for (const auto& [player, remote]: s.remotes_) {
                uint32_t have = 0, first = 0, last = 0;
                for (auto it = remote.inputs.upper_bound(anchor); it != remote.inputs.end() && static_cast<int32_t>(it->first - now) <= 0; ++it) {
                    if (!have++) first = it->first;
                    last = it->first;
                }
                logf("resync: mirror %s of %u: %u relayed frame(s) in (%u, %u], %u..%u%s", remote.entity.c_str(), player, have,
                     anchor, now, first, last, remote.physicalized ? "" : " (no body)");
            }
        // The level's own walk inside the window (a late joiner, step A5:
        // its anchor is the level start).  Init Ingame's activation of the
        // manager is a pending activation of the live frames, which every
        // replay re-issues at its tick; the server's state at S has run that
        // walk long before, so re-issued after the restore it would walk
        // sector 1 a second time over the server's mid-run modules (a Set
        // World Matrix and a module reset).  Every pending activation of the
        // manager from the base through the tick the walk was seen is the
        // level's own (the gate lets no union pass start before it), and is
        // dropped.  A walk opened by the gate's timeout was never seen.
        // Only the late joiner's resync (a running member's A3 resync whose
        // S lies before the server's walk would drop the activation the
        // server's state has not run yet), and only a walk seen in the
        // current numbering (a superseded join's re-assign forgot the old
        // numbering's activations from its base, anchor_resync).
        if (const auto& walk = s.sectors_.walk_open(); s.resync_join_ && walk.known && s.sectors_.walk().seen && !walk.timed_out
                && static_cast<int32_t>(walk.tick - anchor) > 0 && static_cast<int32_t>(walk.tick - now) <= 0) {
            const size_t dropped = platform_.forget_pending_activations(s.resync_base_, walk.tick);
            logf("resync: the level's own walk (seen at tick %u) is in the server's state of tick %u: %zu manager "
                 "activation(s) of %u..%u not re-issued", walk.tick, anchor, dropped, s.resync_base_, walk.tick);
        }
        // 6. The restore and the re-step, the union re-derived in the replay
        // (2.8), the live cells adopted from it (4.6).
        auto& rd = s.rederive_;
        rd.active = false;
        rd.live_tick = now;
        rd.r.start(server_union, stamp_log(), s.pass_log_, s.resync_base_);
        auto input_at = [this](const std::string& name, uint32_t tick, bmmo::session::input_frame& out) {
            return recorded_input_at(name, tick, out);
        };
        resync_outcome restored;
        bool ok = false;
        {
            // Step RSX: the core's scope, as every rewind's (step EL8): the
            // host's, and the matrices of the own entities without a body
            // put back at the end - an own life revived under the restore
            // is removed at its replayed death with the death's matrix.
            const scope_guard keep_cells = keep_live_cells();
            rd.active = true;
            s.last_rewind_rederived_ = true;
            // Step EL7: the mirrors are this apply's own reconcile (2.6.3,
            // the deaths after S above), not the ended-life reconcile's.
            s.resync_restoring_ = true;
            const uint64_t globals_before = s.rollback_.stats().globals_failures;
            // Step LJ2: the bodies the server's blob knew whose births after
            // S the live frames logged (rollback_world::forget_births_after)
            // are forgotten between the restore and the replay - the platform
            // contract's "bodies the server's state has at S", not only the
            // rowed ones below.
            rollback_world resync_world = world();
            resync_world.forget_births_after = [this](uint32_t tick, const std::vector<std::string>& entities) {
                platform_.forget_births_after(tick, entities);
            };
            ok = s.rollback_.resync(resync_world, anchor, now, spec, input_at, restored,
                                    "resync from the server's tick " + std::to_string(anchor));
            // C3 (record-format-gaps design 2.3 step 6, step RK1): a refused
            // write of the physics globals dropped the history: not applied.
            if (s.rollback_.stats().globals_failures != globals_before) ok = false;
            s.resync_restoring_ = false;
            // Step RSX: an own death the revive queued that no replay applied
            // (a re-step stopped before it), made live as after a rewind.
            settle_own_due();
            rd.active = false;
            if (!ok) s.last_rewind_rederived_ = false;
        }
        s.last_rewind_rederived_ = false;
        if (!ok) {
            rd.r.abandon(s.pass_log_);
            // The mirrors the block before step 5 moved to S + 1 keep the
            // live history's ranges: a later rewind remakes a live birth at
            // its tick (remakes_birth) and drives no tick it ran undriven.
            // Only a field the block wrote and nothing changed since.
            for (const auto& was: moved_mirrors) {
                const auto it = s.remotes_.find(was.player);
                if (it == s.remotes_.end()) continue;
                auto& remote = it->second;
                if (remote.driven_from == anchor + 1) remote.driven_from = was.driven_from;
                if (remote.born_tick == anchor && !remote.birth_known) {
                    remote.born_tick = was.born_tick;
                    remote.birth_known = was.birth_known;
                }
            }
            ++s.resync_states_refused_;
            logf("resync: the re-step from tick %u to %u was refused: asking again", anchor, now);
            s.have_resync_state_ = false;
            s.resync_state_ = bmmo::session_resync_state_msg{};
            s.resync_in_flight_ = false;
            request_resync("the resync state could not be applied", true);
            return true;
        }
        // The union at the live tick: the re-derived one (the merged state
        // itself when nothing was re-stepped).
        s.sectors_.restore(depth > 0 && rd.r.through(now) ? rd.r.u().state() : merged);
        {
            int cell = 0;
            if (platform_.sector_cell_adopted(cell)) s.sector_cell_ = cell;
        }
        s.recent_passes_[0] = s.recent_passes_[1] = {};
        for (auto it = s.pass_log_.rbegin(); it != s.pass_log_.rend(); ++it) {
            if (!s.recent_passes_[0].first) s.recent_passes_[0] = *it;
            else if (!s.recent_passes_[1].first) { s.recent_passes_[1] = *it; break; }
        }
        // The old numbering's births after S of the bodies the server's state
        // has at S are no births of this history (2.6.3 "the host logs").
        // The rowed ones here, after the re-step (as before step LJ2); the
        // others the server's blob knew went before the replay (above).
        {
            std::vector<std::string> present;
            for (const auto& [entity, row]: spec.rows) present.push_back(entity);
            platform_.forget_births_after(anchor, present);
        }
        // 7. After it.
        if (!restored.blob_applied) ++s.resync_blobs_refused_;
        s.resync_window_ = true;
        s.resync_window_anchor_ = anchor;
        s.resync_window_end_ = now;
        // The history reaches back to S now: a held notice for base itself
        // has its anchor (S) and is replayed like every later one (2.6.4: a
        // notice for a tick >= base finds its record and amends it).  Left at
        // base, the notice for base that came after the apply was stored and
        // never replayed, and the rewinds of the later ones anchored on a
        // record of base stepped with the frame the server did not apply
        // (a5u_lead: 0.17 m on the own ball for good).
        s.held_floor_ = anchor;
        s.resync_pending_ = false;
        s.resync_exact_ = false;
        s.resync_join_ = false;   // step A5: the world is the shared one again
        s.have_resync_state_ = false;
        ++s.resyncs_done_;
        ++s.resyncs_restepped_;
        s.resync_restep_depth_ += depth;
        bool own_row = false;
        for (const auto& body: st.rows)
            if (body.kind == body_kind::Ball && body.owner == s.own_id_ && (!own_entity.empty() || !own_revive.empty()) && !own_kept)
                own_row = true;   // step RSX: a revived own life's row too
        drop_carried_own_replay(anchor, own_row);
        // Every correction noted before the apply - a relayed frame or an
        // own amendment stored while the resync was pending - is in the
        // ring or the records the re-step just replayed from S: its rewind
        // is subsumed, and running it would only re-step the window again.
        if (s.input_rewind_pending_) {
            ++s.resync_rewinds_subsumed_;
            drop_pending_rewind();
        }
        std::string active, renamed;
        for (int sector: s.sectors_.active()) active += (active.empty() ? "" : ",") + std::to_string(sector);
        for (const auto& [from, to]: spec.blob.renames) renamed += " " + from + "->" + to;
        logf("resync: re-stepped from the server's tick %u to %u (%u ticks, delta %+lld, waited %u frame(s); %u navs; %zu rows: %u "
             "written, %u equal, %u unmapped; %zu script bytes (%s, %u unportable, %u kept local); blob %zu bytes %s (%s); "
             "keep-local %u, local-only %u, destroyed %u/%u, remade %u, from record %u, rechecked %u+%u deferred, "
             "unexplained %u, unattributed %u; %u mirror(s) driven from S + 1, %u added to its record; %u known birth(s) forgotten; "
             "union {%s}; renames:%s) [%s]",
             anchor, now, depth, static_cast<long long>(s.resync_delta_), s.resync_wait_frames_, navs, st.rows.size(),
             restored.rows_written, restored.rows_equal, unmapped, st.scripts.size(), scripts_ok ? "localized" : "REFUSED",
             script_report.unportable, script_report.kept_local, st.savestate.size(),
             restored.blob_applied ? "restored" : restored.anchor_blob ? "REFUSED, the anchor's own" : "REFUSED",
             restored.blob_summary.c_str(), keep_local, local_only, restored.destroyed, destroy, restored.remade,
             restored.from_record, restored.rechecked, restored.deferred, unexplained, unattributed, driven_moved, restored.mirrors_tracked,
             restored.births_forgotten, active.c_str(),
             renamed.empty() ? " none" : renamed.c_str(), why);
        if (journal_.journalling()) {
            journal_resync r;
            r.tick = anchor;
            r.base = s.resync_base_;
            r.delta = static_cast<int32_t>(s.resync_delta_);
            r.applied_at = now;
            r.outcome = outcome;
            r.union_state.assign(st.union_state.begin(), st.union_state.end());
            r.scripts.assign(st.scripts.begin(), st.scripts.end());
            r.savestate.assign(st.savestate.begin(), st.savestate.end());
            r.has_trafo = s.trafo_active_;   // step T3: the reconcile's actions
            r.trafo = std::move(trafo_journal);
            journal_.writer().resync(r);
            journal_.note("resync: re-stepped from the server's tick " + std::to_string(anchor) + " to "
                          + std::to_string(now) + " (" + std::to_string(depth) + " ticks, delta "
                          + std::to_string(s.resync_delta_) + ")");
        }
        s.resync_state_ = bmmo::session_resync_state_msg{};
        if (s.resync_held_) {
            // The hold ends with the apply.
            s.resync_held_ = false;
            const auto t = platform_.now(clock_site::resync_release);
            const auto held = t - s.resync_hold_started_;
            const double ms = std::chrono::duration<double, std::milli>(held).count();
            s.resync_hold_ms_max_ = std::max(s.resync_hold_ms_max_, ms);
            logf("resync hold released after %.0f ms: the state was applied", ms);
            journal_.note("resync hold " + std::to_string(static_cast<long long>(ms)) + " ms");
            if (platform_.hooks.resync_hold_ended) platform_.hooks.resync_hold_ended(held);
        }
        return true;
    }

    // Late-player plan step 12 (design 2.5): the per-client reach.  Every
    // first arrival of a server tick's datagram is one sample of the relay
    // lag; the reach is clamp(p99 over the last 4 windows + 8 + w, 48, cap)
    // (step R12, the user's decisions: w the server's wait in the newest
    // header, cap this client's own rewind cost's, rollback.hpp
    // rewind_cost_meter, at most 120),
    // re-derived at every sample (from the first one: the design's starting
    // value, L_assigned + ceil(rtt/2) + 8, is that lag measured, which the
    // core has and the assignment does not carry), so it rises within the
    // window a lag rises in and falls as the windows that held it age out.
    // Every gate that reads max_resim_ticks follows it (input, own and
    // lifecycle rewinds, a late Sector stamp, a snapshot's depth).
    static_assert(bmmo::session::kReachCapTicks == bmmo::session::kHeldMaxReachTicks,
                  "the deepest reach is the one the sent ring was sized for");
    void session_client_core::sample_reach(uint32_t lag) {
        s.reach_meter_.sample(window_index_of(current_tick()), lag);
        if (config.reach_ticks != 0 || !config.rollback) return;
        apply_reach(s.reach_meter_.reach(s.server_wait_, s.rollback_.rewind_cost().cap()), "relay lag");
    }

    void session_client_core::apply_reach(uint32_t reach, const char* why) {
        const size_t history_before = s.rollback_.thresholds().history_ticks;
        if (!s.rollback_.set_reach(reach)) return;
        ++s.reach_changes_;
        s.reach_max_ = std::max(s.reach_max_, reach);
        if (s.reach_logs_ < 32) {
            ++s.reach_logs_;
            const auto& cost = s.rollback_.rewind_cost();
            logf("reach: %u ticks at tick %u (%s: p99 %u over %llu samples of the last %zu windows + %u + wait %u, "
                 "cap %u from %llu rewinds: restore %.2f ms, replay %.3f ms/tick; history %zu%s)", reach,
                 current_tick(), why, s.reach_meter_.p99(), static_cast<unsigned long long>(s.reach_meter_.samples()),
                 bmmo::session::reach_meter::kWindows, bmmo::session::kReachMarginTicks, s.server_wait_, cost.cap(),
                 static_cast<unsigned long long>(cost.samples()), cost.restore_ms(), cost.replay_ms_per_tick(),
                 s.rollback_.thresholds().history_ticks,
                 s.rollback_.thresholds().history_ticks != history_before ? ", grown" : "");
        }
    }

    uint32_t session_client_core::window_rewind_reach(uint32_t tick) const {
        const uint32_t standard = s.rollback_.thresholds().max_resim_ticks;
        if (!s.resync_window_ || static_cast<int32_t>(tick - 1 - s.resync_window_anchor_) < 0
                || static_cast<int32_t>(tick - s.resync_window_end_) > 0)
            return standard;
        return std::max(standard, s.rollback_.resync_reach());
    }

    // 2.6.6: while a pending exact resync's state has not been applied and
    // the next step would take the anchor S out of the history (D would pass
    // the reach), the world steps nothing.  Bounded by 2 s: then the hold
    // releases, the resync stays pending, and a new resync is asked for at
    // once (every other trigger waits while one is pending).  First, a state
    // that arrived in this frame's drain is applied (on_session_resync_state).
    hold_state session_client_core::poll_resync_hold() {
        hold_state state;
        if (s.have_resync_state_ && s.resync_pending_ && s.resync_exact_ && s.resync_wait_frames_ == 0)
            try_apply_resync("arrival");
        const bool needed = s.resync_pending_ && s.resync_exact_ && !s.resync_hold_spent_ && s.frames_since_anchor_ >= 1
                         && current_tick() - (s.resync_base_ - 1) >= s.rollback_.resync_reach();
        if (!s.resync_held_) {
            if (!needed) return state;
            s.resync_held_ = true;
            ++s.resync_holds_;
            s.resync_hold_started_ = platform_.now(clock_site::resync_hold);
            s.resync_hold_deadline_ = s.resync_hold_started_ + std::chrono::seconds(2);
            logf("resync hold at tick %u: the state of tick %u has not been applied and the next step would leave the "
                 "anchor outside the history", current_tick(), s.resync_base_ - 1);
            journal_.note("resync hold at tick " + std::to_string(current_tick()));
            state.held = true;
            return state;
        }
        const auto now = platform_.now(clock_site::resync_hold_timeout);
        if (needed && now < s.resync_hold_deadline_) {
            state.held = true;
            return state;
        }
        s.resync_held_ = false;
        const auto held = now - s.resync_hold_started_;
        const double ms = std::chrono::duration<double, std::milli>(held).count();
        s.resync_hold_ms_max_ = std::max(s.resync_hold_ms_max_, ms);
        if (needed) {
            ++s.resync_hold_timeouts_;
            s.resync_in_flight_ = false;
            s.resync_hold_spent_ = true;
            state.timed_out = true;
            logf("resync hold timed out after %.0f ms at tick %u: running on, asking again", ms, current_tick());
            journal_.note("resync hold timed out after " + std::to_string(static_cast<long long>(ms)) + " ms");
            request_resync("the resync state did not come within the hold", true);
        } else {
            logf("resync hold released after %.0f ms", ms);
            journal_.note("resync hold " + std::to_string(static_cast<long long>(ms)) + " ms");
        }
        if (platform_.hooks.resync_hold_ended) platform_.hooks.resync_hold_ended(held);
        return state;
    }

}
