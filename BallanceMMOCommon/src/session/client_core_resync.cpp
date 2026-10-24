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

    void session_client_core::log_life(const bmmo::session_event_msg& event) {
        client_core_state::life_entry entry;
        entry.applied = event.tick;
        entry.origin = event.origin_tick;
        entry.player = event.player;
        entry.type = event.type;
        s.life_log_.push_back(std::move(entry));
        const uint32_t keep = static_cast<uint32_t>(s.rollback_.thresholds().history_ticks) + 128u;
        while (!s.life_log_.empty() && static_cast<int32_t>(s.life_log_.front().applied + keep - event.tick) < 0)
            s.life_log_.pop_front();
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
        // The deaths after S this world applied already (2.6.3: a mirror
        // row at S whose Unphysicalize came after S): made from the row,
        // killed again at its tick in the replay.
        const auto death_after = [&](uint32_t player, uint32_t& tick) {
            for (auto it = s.life_log_.rbegin(); it != s.life_log_.rend(); ++it)
                if (it->player == player && it->type == event_type::Unphysicalize
                        && static_cast<int32_t>(it->applied - anchor) > 0) {
                    tick = it->applied;
                    return true;
                }
            return false;
        };
        std::map<std::string, const body_state*> rows;
        std::vector<std::string> waiting;
        std::vector<std::pair<uint32_t, uint32_t>> deaths;   // player, tick: mirrors made from their row
        uint32_t unmapped = 0;
        for (const auto& body: st.rows) {
            if (body.kind == body_kind::Ball) {
                if (body.owner == s.own_id_) {
                    if (!own_entity.empty()) rows[own_entity] = &body;
                    else ++unmapped;   // the own lifecycle is ours: the anchor's own ball stands
                    continue;
                }
                const auto it = s.remotes_.find(body.owner);
                if (it != s.remotes_.end() && it->second.physicalized) {
                    rows[it->second.entity] = &body;
                    continue;
                }
                uint32_t died = 0;
                if (it != s.remotes_.end() && it->second.birth_known && death_after(body.owner, died)) {
                    deaths.emplace_back(body.owner, died);
                    continue;
                }
                waiting.push_back("ball of " + std::to_string(body.owner));
                continue;
            }
            if (keep_local_body(body.name)) continue;
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
            ++s.resync_deferred_ticks_;
            return false;
        }
        if (s.resync_wait_frames_)
            logf("resync: the state of tick %u waited %u frame(s) for its bodies", anchor, s.resync_wait_frames_);
        s.resync_rows_unmapped_ += unmapped;
        // 3. The scripts: the server's record, localized, the keep-local ones
        // from the anchor's own (2.4).  Refused: the anchor's own scripts,
        // and the rest of the apply as designed (never silent).
        std::vector<uint8_t> localized;
        resync_script_report script_report;
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
        for (const auto& [player, died]: deaths) {
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
                s.pending_deaths_.push_back({player, died});
                logf("resync: mirror of %u made from its row at S %u, its death at %u applied again %s", player, anchor,
                     died, static_cast<int32_t>(died - now) <= 0 ? "in the replay" : "live");
            } else {
                ++s.resync_rows_unmapped_;
            }
        }
        // 5. The reconcile (2.6.3): the anchor's bodies the server's state at
        // S has no row for.
        resync_spec spec;
        spec.scripts = std::move(localized);
        spec.rows = rows;
        spec.blob.blob = &st.savestate;
        {
            std::map<uint32_t, std::string> local;
            std::string local_own = s.rollback_.recorded_own_entity(anchor);
            if (local_own.empty()) local_own = own_entity;
            if (local_own.empty()) local_own = platform_.own_ball().entity;
            local[s.own_id_] = local_own;
            for (const auto& [player, remote]: s.remotes_)
                if (remote.physicalized) local[player] = remote.entity;
            spec.blob.renames = resync_renames(st.clones, local);
        }
        spec.blob.excluded_prefixes = keep_local_body_prefixes();
        // The server's navigation of every ball at S onto this world's
        // entity of that player (the own replica, a driven mirror).
        uint32_t navs = 0;
        for (const auto& [player, nav]: st.navs) {
            std::string entity;
            if (player == s.own_id_) entity = s.own_physicalized_ ? platform_.own_driven_entity() : std::string();
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
                if (rows.count(name)) continue;
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
            const scope_guard keep_cells = platform_.keep_live_cells(*this);
            rd.active = true;
            s.last_rewind_rederived_ = true;
            ok = s.rollback_.resync(world(), anchor, now, spec, input_at, restored,
                                    "resync from the server's tick " + std::to_string(anchor));
            rd.active = false;
            if (!ok) s.last_rewind_rederived_ = false;
        }
        s.last_rewind_rederived_ = false;
        if (!ok) {
            rd.r.abandon(s.pass_log_);
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
        s.have_resync_state_ = false;
        ++s.resyncs_done_;
        ++s.resyncs_restepped_;
        s.resync_restep_depth_ += depth;
        bool own_row = false;
        for (const auto& body: st.rows)
            if (body.kind == body_kind::Ball && body.owner == s.own_id_ && !own_entity.empty()) own_row = true;
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
             "unexplained %u, unattributed %u; union {%s}; renames:%s) [%s]",
             anchor, now, depth, static_cast<long long>(s.resync_delta_), s.resync_wait_frames_, navs, st.rows.size(),
             restored.rows_written, restored.rows_equal, unmapped, st.scripts.size(), scripts_ok ? "localized" : "REFUSED",
             script_report.unportable, script_report.kept_local, st.savestate.size(),
             restored.blob_applied ? "restored" : restored.anchor_blob ? "REFUSED, the anchor's own" : "REFUSED",
             restored.blob_summary.c_str(), keep_local, local_only, restored.destroyed, destroy, restored.remade,
             restored.from_record, restored.rechecked, restored.deferred, unexplained, unattributed, active.c_str(),
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
