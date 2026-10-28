// The shared session-client core (docs/session-client-core-design.md 1.1):
// the messages.  Step E4 moves the snapshot path out of the headless client
// (session_client.cpp at aea9521): SessionSnapshot's count and apply_snapshot
// - the resync rebuild, the snapshot rollback and the legacy blend ladder -
// verbatim but for the platform calls (body_state, write_body,
// own_driven_entity, bind_world, keep_live_cells), the configuration
// (rollback, correct, trace) and the legacy back-calls (recorded_input_at,
// own_ball, describe_movable_objects, design 6.1; drop_carried_own_replay was
// one until E5).
// E11 deleted the back-calls: describe_movable_objects is the optional
// hooks.describe_movables.
// Step E5 adds the relay: SessionRemoteInput's intake and
// note_input_correction (session_client.cpp at 9029872), verbatim but for the
// configuration (rollback) and the journal; the held section it hands on and
// SessionLead are in client_core_held.cpp.

#include "session/client_core.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

namespace bmmo::session {

    void session_client_core::on_session_snapshot(const bmmo::session_snapshot_msg& msg) {
        ++s.snapshots_received_;
        if (msg.session != s.session_ || s.phase_ != client_core_state::phase::running) return;
        apply_snapshot(msg);
    }

    void session_client_core::apply_snapshot(const bmmo::session_snapshot_msg& snapshot) {
        // Everything the network delivered, stale rows included: what the
        // server said about a tick is authoritative whether or not this
        // client still had a use for it.
        if (journal_.journalling()) {
            // The full flag rides along: a full snapshot is the one that
            // carries the mechanism name dictionary, and a reader has no
            // other way to tell which received rows hold it.
            const uint8_t flags = static_cast<uint8_t>(bmmo::session::JOURNAL_CHECKPOINT_RECEIVED
                | (snapshot.full ? bmmo::session::JOURNAL_CHECKPOINT_FULL : 0));
            journal_.writer().checkpoint(snapshot.tick, flags, snapshot.bodies);
        }
        if (s.have_snapshot_ && snapshot.tick <= s.last_snapshot_tick_ && !snapshot.full) { ++s.snapshots_stale_; return; }
        // (Whether the server had our input for the tick it simulated is
        // read from the relay header's `acked` now, every tick and with
        // snapshots off too: note_held_section.)
        std::string error;
        const std::string ball_name = platform_.own_ball().entity;
        if (s.resync_pending_) {
            // Step A3: the pending resync is answered by its own state
            // (SessionResyncState); every snapshot meanwhile describes a
            // tick of another world - a broadcast full snapshot of a body-set
            // change, the legacy copy of the state's rows - and is dropped.
            if (s.resync_exact_) {
                ++s.snapshots_while_resync_;
                return;
            }
            if (!snapshot.full) return;
            s.have_snapshot_ = true;
            s.last_snapshot_tick_ = snapshot.tick;
            ++s.snapshots_applied_;
            // A hard world overwrite: the records collected while we waited
            // for this snapshot describe the world it is about to replace.
            s.rollback_.invalidate_history();
            s.union_records_.clear();   // step B1: beside the records
            // ...and so does a replay a correction of this poll asked
            // for: its anchor is gone with the history, and a refused own
            // replay would answer this resync with another.
            drop_pending_rewind();
            // The dictionary and the newest rows first, then one resolve
            // pass - this is the moment the registry knows the most about a
            // world that has just been rebuilt (design 9.25).
            for (const auto& body: snapshot.bodies)
                if (body.kind == bmmo::session::body_kind::Mechanism) {
                    if (!body.name.empty()) s.mechanism_tracking_.note_name(body.owner, body.name);
                    s.mechanism_tracking_.note_row(snapshot.tick, body);
                }
            s.mechanism_tracking_.mark_dirty();
            s.mechanism_tracking_.resolve(snapshot.tick, [this](const char* entity, bmmo_physics_body_state& out) {
                return platform_.body_state(entity, out);
            });
            std::string mechanism_entity;
            for (const auto& body: snapshot.bodies) {
                const bool wake = (body.flags & bmmo::session::BODY_FLAG_SIMULATED) != 0;
                const char* target = nullptr;
                if (body.kind == bmmo::session::body_kind::Ball) {
                    if (body.owner == s.own_id_) {
                        if (!s.own_physicalized_ || ball_name.empty()) continue;
                        target = ball_name.c_str();
                    } else {
                        auto it = s.remotes_.find(body.owner);
                        if (it == s.remotes_.end() || !it->second.physicalized) continue;
                        target = it->second.entity.c_str();
                    }
                } else {
                    mechanism_entity = s.mechanism_tracking_.entity_of(body.owner);
                    if (mechanism_entity.empty()) continue;   // another sector's instance
                    target = mechanism_entity.c_str();
                }
                platform_.write_body(target, body.position, body.rotation, body.linear, body.angular,
                                     body.owner == s.own_id_ ? true : wake, error);
            }
            s.resync_pending_ = false;
            s.resync_join_ = false;   // step A5: the world is the snapshot's
            // Every own tick up to the snapshot's is the server's now,
            // substituted ones included: no held notice of them is late.
            // Nor is one up to the next tick we step: the snapshot replaced
            // our world at the current tick, the history went with it,
            // and the first record after it is current + 1, so no rewind
            // can anchor before that.  Taking those for late (step 4 asks
            // for a resync on a late record) would answer a resync with
            // another one whenever the server held a frame of the ticks
            // we stepped while the snapshot was on its way.
            // (Before the first step of the new numbering there is no
            // current tick: the next record is the base's, which the
            // floor already is.)
            const uint32_t first_record = s.frames_since_anchor_ >= 1 ? current_tick() + 1 : s.tick_base_;
            if (static_cast<int32_t>(snapshot.tick - s.held_floor_) > 0) s.held_floor_ = snapshot.tick;
            if (static_cast<int32_t>(first_record - s.held_floor_) > 0) s.held_floor_ = first_record;
            ++s.resyncs_done_;
            logf("resync applied from the full snapshot of tick %u (%zu bodies)", snapshot.tick, snapshot.bodies.size());
            journal_.body_correction(4, {}, snapshot.tick, 0.0, nullptr);
            journal_.note("resync: rebuilt from the full snapshot of tick " + std::to_string(snapshot.tick)
                          + " (" + std::to_string(snapshot.bodies.size()) + " bodies)");
            return;
        }
        s.have_snapshot_ = true;
        s.last_snapshot_tick_ = snapshot.tick;
        ++s.snapshots_applied_;
        if (config.own_body_backstop) check_own_body(snapshot);
        if (config.rollback) {
            for (const auto& body: snapshot.bodies)
                if (body.kind == bmmo::session::body_kind::Mechanism) {
                    if (snapshot.full && !body.name.empty()) s.mechanism_tracking_.note_name(body.owner, body.name);
                    s.mechanism_tracking_.note_row(snapshot.tick, body);
                }
            // Step RC3 (rewind-cost design 3): with the session's snapshots
            // off, the full snapshot of a body-set change is no correction.
            // Its names and rows are the registry's (above) and the journal's
            // (the RECEIVED checkpoint); the world is what the inputs made of
            // it, and a drift it would have rewound is the drift the
            // snapshots-off judgement is there to show.
            if (!snapshots_run() && !config.body_set_corrections) {
                if (s.snapshots_uncorrected_++ < 4)
                    logf("snapshot of tick %u (%s, %zu rows) taken for its names only: the session's snapshots are off "
                         "(step RC3)", snapshot.tick, snapshot.full ? "full" : "delta", snapshot.bodies.size());
                return;
            }
            auto entity_of = [&](const bmmo::session::body_state& body) -> std::string {
                if (body.kind == bmmo::session::body_kind::Ball) {
                    // Step BALA: a ball that has left the sync in its airship
                    // zone takes no row (the own ball from its zone tick; a
                    // mirror while it is a ghost here).
                    if (body.owner == s.own_id_) {
                        if (own_out_of_sync_at(snapshot.tick)) { ++s.airship_rows_skipped_; return {}; }
                        return s.own_physicalized_ ? platform_.own_driven_entity() : std::string();
                    }
                    auto it = s.remotes_.find(body.owner);
                    if (it == s.remotes_.end() || !it->second.physicalized || !it->second.navigation) return {};
                    if (it->second.airship_tick != 0 && static_cast<int32_t>(snapshot.tick - it->second.airship_tick) >= 0) {
                        ++s.airship_rows_skipped_;
                        return {};
                    }
                    return it->second.entity;
                }
                // Design 9.25: the resolved local body, "" when this client
                // has none (another sector) or the identity guard refused
                // the pick - the mod does exactly the same.
                return s.mechanism_tracking_.entity_of(body.owner);
            };
            // Only an exact hit counts, like the mod: the engine feeds a
            // relayed frame instead of the recorded prediction when this
            // succeeds, and a frame from an earlier tick would replay a key
            // edge the tick did not have (findings/A5 section 3.1 item 5).
            auto input_at = [this](const std::string& entity, uint32_t tick, bmmo::session::input_frame& out) {
                return recorded_input_at(entity, tick, out);
            };
            const scope_guard keep_cells = platform_.keep_live_cells(*this);
            const uint64_t frozen_before = s.rollback_.stats().frozen;
            const uint64_t unmatched_before = s.rollback_.stats().unmatched;
            // Step T3: the trafo pieces are inputs' bodies here, exact by
            // the replay; a row of one is a restore source only when it
            // breaches (rollback_world::row_on_breach_only).
            rollback_world snapshot_world = world();
            if (s.trafo_active_)
                snapshot_world.row_on_breach_only = [this](const std::string& entity) { return s.trafo_pieces_.count(entity) != 0; };
            const bool rolled = s.rollback_.on_snapshot(snapshot_world, snapshot, current_tick(), entity_of, input_at);
            settle_unreplayed_lives(current_tick());   // step EL7: the reconcile's events no replay applied
            settle_own_due();                          // step EL8: the own ball's
            // A frozen clock set the bodies and dropped the history with no
            // replay (rollback.hpp): the world stays on the snapshot while
            // the tick counter runs on, and only a re-anchor brings the two
            // back together (design 4.1, 7 Q1: a core rule for both hosts
            // from R1; retail is the host whose clock can freeze).
            if (s.rollback_.stats().frozen != frozen_before) request_resync("local physics clock frozen");
            // Session-core design 7.1 decision 1 (step B2SW; the retail host's
            // transitional trigger until then, core_host.cpp): snapshots in a
            // row with no record of their tick are a numbering desync, which
            // only a re-anchor repairs.  Counted per snapshot - a matched one
            // ends the run, a tick without a snapshot does not - and only
            // here, past the resync-pending return above, as the host did.
            if (s.rollback_.stats().unmatched != unmatched_before) {
                if (++s.consecutive_unmatched_ >= kUnmatchedSnapshotRun) {
                    logf("%d snapshots in a row with no record of their tick (the last %u, local tick %u)",
                         s.consecutive_unmatched_, snapshot.tick, current_tick());
                    s.consecutive_unmatched_ = 0;
                    // Counted only when the request went out (the limiter
                    // may hold it back).
                    const uint64_t sent_before = s.resyncs_sent_;
                    request_resync("30 snapshots with no record of their tick");
                    if (s.resyncs_sent_ != sent_before) ++s.unmatched_resyncs_;
                }
            } else {
                s.consecutive_unmatched_ = 0;
            }
            if (rolled) {
                ++s.snapshot_rollbacks_;
                s.snapshot_rollback_depth_ += current_tick() - snapshot.tick;
                // The rows were the server's world at the anchor, our own
                // ball's among them when it has one here: that world ran on
                // the frames the server substituted, and the replay after it
                // fed our amended records.
                bool own_row = false;
                for (const auto& body: snapshot.bodies)
                    if (body.kind == bmmo::session::body_kind::Ball && body.owner == s.own_id_ && !entity_of(body).empty())
                        own_row = true;
                drop_carried_own_replay(snapshot.tick, own_row);
            }
            if (rolled && config.trace && s.rollback_diag_++ < 16 && platform_.hooks.describe_movables)
                logf("after rollback: movable=%s", platform_.hooks.describe_movables().c_str());
            return;
        }
        for (const auto& body: snapshot.bodies) {
            if (body.kind == bmmo::session::body_kind::Ball) {
                if (body.owner == s.own_id_) {
                    if (!s.own_physicalized_ || ball_name.empty()) continue;
                    bmmo::session::ball_pose pose;
                    pose.tick = snapshot.tick;
                    for (int k = 0; k < 3; ++k) { pose.position[k] = body.position[k]; pose.linear[k] = body.linear[k]; pose.angular[k] = body.angular[k]; }
                    for (int k = 0; k < 4; ++k) pose.rotation[k] = body.rotation[k];
                    const auto step = s.corrector_.compare(pose);
                    const double err = s.corrector_.stats().last_error;
                    if (step.action == bmmo::session::correction_step::kind::hard) {
                        if (++s.consecutive_hard_ >= 3) request_resync("3 hard corrections in a row");
                        logf("hard correction of own ball for tick %u (error %.4f m, local tick %u)", snapshot.tick, err, current_tick());
                        // Before the set, or the recorded local pose would
                        // be the server's own.
                        journal_.body_correction(2, ball_name, snapshot.tick, err, body.position);
                        if (config.correct && !platform_.write_body(ball_name, step.target.position,
                                step.target.rotation, step.target.linear, step.target.angular, true, error))
                            logf("hard set: %s", error.c_str());
                    } else if (step.action == bmmo::session::correction_step::kind::blend) {
                        s.consecutive_hard_ = 0;
                        logf("blend correction of own ball for tick %u (error %.4f m, local tick %u)", snapshot.tick, err, current_tick());
                        journal_.body_correction(3, ball_name, snapshot.tick, err, body.position);
                    } else if (config.trace && err > 0.002) {
                        logf("own ball error %.4f m at tick %u", err, snapshot.tick);
                    }
                    continue;
                }
                auto it = s.remotes_.find(body.owner);
                if (it == s.remotes_.end() || !it->second.physicalized) continue;
                const bool wake = (body.flags & bmmo::session::BODY_FLAG_SIMULATED) != 0;
                auto& remote = it->second;
                if (remote.navigation) {
                    bmmo::session::ball_pose pose;
                    pose.tick = snapshot.tick;
                    for (int k = 0; k < 3; ++k) { pose.position[k] = body.position[k]; pose.linear[k] = body.linear[k]; pose.angular[k] = body.angular[k]; }
                    for (int k = 0; k < 4; ++k) pose.rotation[k] = body.rotation[k];
                    const auto step = remote.corrector.compare(pose);
                    if (step.action == bmmo::session::correction_step::kind::hard) {
                        ++remote.hard_sets;
                        logf("hard correction of remote %s for tick %u (error %.4f m)", remote.entity.c_str(), snapshot.tick,
                             remote.corrector.stats().last_error);
                        journal_.body_correction(2, remote.entity, snapshot.tick,
                                                 remote.corrector.stats().last_error, body.position);
                        if (config.correct) platform_.write_body(remote.entity, step.target.position,
                                                                 step.target.rotation, step.target.linear, step.target.angular, wake, error);
                    } else if (step.action == bmmo::session::correction_step::kind::blend) {
                        ++remote.blends;
                        logf("blend correction of remote %s for tick %u (error %.4f m)", remote.entity.c_str(), snapshot.tick,
                             remote.corrector.stats().last_error);
                        journal_.body_correction(3, remote.entity, snapshot.tick,
                                                 remote.corrector.stats().last_error, body.position);
                    }
                    continue;
                }
                if (!platform_.write_body(it->second.entity, body.position, body.rotation,
                                          body.linear, body.angular, wake, error))
                    logf("remote %u: %s", body.owner, error.c_str());
                else ++s.remote_writes_;
                continue;
            }
            // The legacy blending path (--no-rollback): the row corrects the
            // locally simulated mechanism through a body_corrector instead.
            if (snapshot.full && !body.name.empty()) s.mechanism_tracking_.note_name(body.owner, body.name);
            s.mechanism_tracking_.note_row(snapshot.tick, body);
            const std::string entity = s.mechanism_tracking_.entity_of(body.owner);
            if (entity.empty()) continue;
            const bool wake = (body.flags & bmmo::session::BODY_FLAG_SIMULATED) != 0;
            bmmo_physics_body_state local{};
            if (!platform_.body_state(entity, local)) continue;
            bmmo::session::ball_pose pose;
            pose.tick = snapshot.tick;
            for (int k = 0; k < 3; ++k) { pose.position[k] = body.position[k]; pose.linear[k] = body.linear[k]; pose.angular[k] = body.angular[k]; }
            for (int k = 0; k < 4; ++k) pose.rotation[k] = body.rotation[k];
            auto& corrector = s.mechanism_correctors_[entity];
            const auto step = corrector.compare(pose);
            if (step.action == bmmo::session::correction_step::kind::hard) {
                ++s.mechanism_hard_;
                logf("hard correction of %s for tick %u (error %.4f m)", entity.c_str(), snapshot.tick, corrector.stats().last_error);
                journal_.body_correction(2, entity, snapshot.tick, corrector.stats().last_error, body.position);
                if (config.correct) platform_.write_body(entity, step.target.position, step.target.rotation,
                                                         step.target.linear, step.target.angular, wake, error);
            } else if (step.action == bmmo::session::correction_step::kind::blend) {
                ++s.mechanism_blends_;
                logf("blend correction of %s for tick %u (error %.4f m)", entity.c_str(), snapshot.tick, corrector.stats().last_error);
                journal_.body_correction(3, entity, snapshot.tick, corrector.stats().last_error, body.position);
            }
        }
    }

    // ------------------------------------------------- own_body_backstop
    // Every snapshot carries a ball row for each physicalized player, awake
    // or not.  Ours missing while our ball is physicalized here says the
    // server has no body for us: its copy of the last Physicalize never
    // arrived or was rejected, and no retail script reports it again.  After
    // about a second of snapshots it is reported once more, with the pose
    // the ball has now and without the spawn flag (the body was kicked, or
    // deliberately not, already): the retail mod's check_own_body (design
    // 4.1 own_body_backstop, R1).
    //
    // While the own echoes are acted on (R3), an own birth still waiting
    // for its echo is theirs: its answer is the echo, or the deadline
    // (expire_own_echoes: re-reported while current, then a resync).  A
    // backstop re-report beside it leaves that record `sent` (send_event
    // keeps no own_life), so the deadline would still give the birth up,
    // re-report it again and resync: two answers to one lost event.  So
    // the count waits until no birth is pending (the echoes' answer, made
    // or given up, ends the wait).
    void session_client_core::check_own_body(const bmmo::session_snapshot_msg& snapshot) {
        bool present = false;
        for (const auto& body: snapshot.bodies)
            if (body.kind == bmmo::session::body_kind::Ball && body.owner == s.own_id_) { present = true; break; }
        bool awaiting_echo = false;
        if (acting_on_echoes())
            for (const auto& life: s.own_life_)
                if (!life.retired && life.what == own_life::kind::birth && life.state == own_life::verdict::sent) {
                    awaiting_echo = true;
                    break;
                }
        if (present || !s.own_physicalized_ || !s.own_last_birth_.valid || s.resync_pending_ || awaiting_echo) {
            s.snapshots_without_own_ = 0;
            return;
        }
        if (++s.snapshots_without_own_ < 30) return;
        const auto now = platform_.now(clock_site::own_backstop);
        if (s.own_backstops_ > 0 && now - s.last_backstop_ < std::chrono::seconds(2)) return;
        const own_ball_view own = platform_.own_ball();
        if (own.entity.empty() || !own.physicalized) return;
        s.last_backstop_ = now;
        s.snapshots_without_own_ = 0;
        ++s.own_backstops_;
        bmmo::session_event_msg event;
        event.tick = current_tick() + 1;
        event.type = bmmo::session::event_type::Physicalize;
        event.ball_type = s.own_last_birth_.ball_type;
        event.flags = static_cast<uint8_t>(s.own_last_birth_.flags & ~bmmo::session::PHYSICALIZE_FLAG_SPAWN);
        for (int k = 0; k < 3; ++k) event.position[k] = static_cast<float>(own.state.position[k]);
        quaternion_rows(own.state.rotation, event.rotation);
        event.recipe = s.own_last_birth_.recipe;
        send_event(event);
        logf("the server has no body for our ball (%llu); Physicalize re-reported at tick %u",
             static_cast<unsigned long long>(s.own_backstops_), event.tick);
    }

    // ------------------------------------------------------------ the relay
    void session_client_core::on_session_remote_input(const bmmo::session_remote_input_msg& msg) {
        ++s.remote_inputs_received_;
        if (msg.session != s.session_) return;
        note_held_section(msg);
        for (const auto& entry: msg.entries) {
            // Step T3 (trafo-replication design 2.4): a member first seen in
            // the relay gets its piece set now, outside any replay.
            if (s.trafo_active_ && s.phase_ == client_core_state::phase::running && entry.player != s.own_id_)
                ensure_trafo_set(entry.player, "its first relayed input");
            auto it = s.remotes_.find(entry.player);
            // The journal's PLAYER record goes in at the oldest tick this
            // entry covers, so no INPUT record of the player can precede
            // it in tick order (a replay needs the player before its
            // inputs); after the first time it is a no-op.
            if (journal_.journalling()) journal_.announce_player(entry.player, msg.frame_tick(entry, 0));
            for (size_t i = 0; i < entry.frames.size(); ++i) {
                const uint32_t tick = msg.frame_tick(entry, i);
                const auto& frame = entry.frames[i];
                // The window re-delivers a frame up to relay_window
                // times: the first arrival is the truth, the rest change
                // nothing (no record, no correction, no predictor move).
                if (!s.relay_seen_.first(entry.player, tick)) { ++s.relay_dup_; continue; }
                ++s.relay_new_;
                // How late the first arrival is, in ticks already
                // simulated: what the rollback has to reach to apply it.
                if (s.assigned_ && static_cast<int32_t>(current_tick() - tick) > 0) {
                    const uint32_t lag = current_tick() - tick;
                    s.relay_lag_max_ = std::max(s.relay_lag_max_, lag);
                    ++s.relay_lag_hist_[std::min<uint32_t>(lag, client_core_state::kRelayLagBuckets - 1)];
                    ++s.window_lag_[entry.player][lag];
                }
                // What the server applied for that player at that tick,
                // whether or not this client mirrors the ball.
                if (journal_.journalling())
                    journal_.writer().input(tick, entry.player, frame, bmmo::session::JOURNAL_INPUT_RELAYED);
                if (it == s.remotes_.end()) {
                    // No mirror yet: the datagram beat the Physicalize
                    // here (the ordered stream used to mask this).  Parked
                    // by tick, moved into the ring when the mirror is made.
                    s.pending_relay_.store(entry.player, tick, frame);
                    ++s.relay_parked_;
                    continue;
                }
                auto& remote = it->second;
                // A reordered frame is still authoritative for its
                // historical tick: keep it for the rollback without
                // letting it replace the live predictor with an older
                // input.  The retail mod has always done it in this order
                // (physics_session_client.cpp); doing the guard first, as
                // this client used to, dropped a late frame on the floor
                // and left the rollback replaying a prediction the server
                // had already corrected.
                if (bmmo::session::relay_store(remote.inputs, tick, frame, bmmo::session::kRelayRing)
                        == bmmo::session::relay_store_result::stored)
                    note_input_correction(remote, tick, frame);
                if (remote.have_input && static_cast<int32_t>(tick - remote.input_tick) < 0) continue;   // out of order
                remote.input = frame;
                remote.input_tick = tick;
                remote.have_input = true;
            }
        }
    }

    // A relayed frame for a tick this client has already simulated: if it
    // differs from the frame the prediction used, that tick and every tick
    // after it ran on the wrong input.  Remember the earliest such tick and
    // let frame() replay from it once, before the next step - the frames of
    // several remotes arrive together, and one rewind covers them all.

    void session_client_core::note_input_correction(const client_core_state::remote_ball& remote, uint32_t tick,
                                                    const bmmo::session::input_frame& relayed) {
        const std::string& entity = remote.entity;
        // The input_rewind switch (design 1.2; E9): off, a relayed frame is
        // stored (by the caller) and no replay is asked for; this returns
        // before the checks below, so relay_late_, relay_bodiless_ and
        // relay_birth_frozen_ are not counted either (they stay 0).
        if (!config.rollback || !config.input_rewind || entity.empty() || tick == 0) return;
        if (static_cast<int32_t>(tick - current_tick()) > 0) return;   // not simulated yet
        // Deeper than the engine re-simulates (48 ticks), or past the
        // history: nothing to replay from.  Measured at the anchor the
        // rewind will use, the tick before the corrected one
        // (replay_corrected_inputs rewinds to tick - 1).  Counted - the
        // relay's pass criterion is that this never happens.
        // Inside the last exact resync's re-step window as deep as that
        // re-step (step A3: window_rewind_reach): the re-step ran on the
        // relayed frames it had, and these are the ones it lacked.
        const uint32_t anchor = tick - 1;
        if (current_tick() - anchor > window_rewind_reach(tick) || !s.rollback_.knows_tick(anchor)) {
            ++s.relay_late_;
            return;
        }
        // A frame also stands in for the holes after it (the nearest
        // earlier one, relay_frame_or_earlier): it can still change the
        // replay of tick + 1 when the ring has no frame there but has a
        // later one, and the body was there at tick + 1.
        const bool feeds_next = remote.inputs.find(tick + 1) == remote.inputs.end()
                             && remote.inputs.upper_bound(tick + 1) != remote.inputs.end()
                             && s.rollback_.recorded_body(entity, tick + 1);
        // No body behind the name at that tick - the player was dead there,
        // or the birth that makes the mirror has not arrived yet: the frame
        // drove nothing, and a replay from it reproduces what the records
        // have (measured: 154-200 rewinds per client in a lane run spent
        // on dead players' mirrors).  The frame is in the ring already, so
        // the replay of a late birth still feeds it.
        if (!s.rollback_.recorded_body(entity, tick) && !feeds_next) {
            ++s.relay_bodiless_;
            return;
        }
        // The birth tick of a body every rewind across it parks and re-poses
        // at that tick's record after the step: a different frame for it
        // changes nothing a replay can reach - a whole replay for the same
        // world.  (The one-tick error stays until a snapshot.)  Not a body
        // the restore makes again at its birth tick (step LB,
        // remakes_birth): the replay steps that tick with this frame.
        if (remote.physicalized && tick == remote.born_tick && !feeds_next && !remakes_birth(remote)) {
            ++s.relay_birth_frozen_;
            return;
        }
        // No record of an input for this body at this tick is NEWS, not a
        // reason to do nothing: we stepped that tick without driving the
        // body at all.  It is the normal state of a mirror that was just
        // rewound into existence - the replay could only feed it the frames
        // that had arrived by then, which for most of the birth window is
        // none - so skipping it here was stopping those balls from ever
        // converging.
        bmmo::session::input_frame predicted{};
        if (s.rollback_.recorded_input(entity, tick, predicted) && same_frame(predicted, relayed))
            return;   // the prediction was right
        if (!s.input_rewind_pending_ || static_cast<int32_t>(tick - s.input_rewind_tick_) < 0) {
            s.input_rewind_pending_ = true;
            s.input_rewind_tick_ = tick;
            s.input_rewind_entity_ = entity;
            s.input_rewind_own_ = false;
        }
        // Every ball corrected this frame, not only the earliest: the one
        // replay below re-runs them all, and the window: line credits it
        // to each source (late-player design 5.2, input rewinds naming the
        // late ball).
        s.input_rewind_entities_.insert(entity);
    }

}
