// The shared session-client core (docs/session-client-core-design.md 1.1):
// the messages.  Step E4 moves the snapshot path out of the headless client
// (session_client.cpp at aea9521): SessionSnapshot's count and apply_snapshot
// - the resync rebuild, the snapshot rollback and the legacy blend ladder -
// verbatim but for the platform calls (body_state, write_body,
// own_driven_entity, bind_world, keep_live_cells), the configuration
// (rollback, correct, trace) and the legacy back-calls (recorded_input_at,
// own_ball, describe_movable_objects, drop_carried_own_replay, design 6.1).
// The relay intake and the SessionLead follow at E5.

#include "session/client_core.hpp"

#include <cstdint>
#include <string>

namespace bmmo::session {

    void session_client_core::on_session_snapshot(const bmmo::session_snapshot_msg& msg) {
        ++s.snapshots_received_;
        if (msg.session == s.session_ && s.phase_ == client_core_state::phase::running) apply_snapshot(msg);
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
        const std::string ball_name = legacy.own_ball();
        if (s.resync_pending_) {
            if (!snapshot.full) return;
            s.have_snapshot_ = true;
            s.last_snapshot_tick_ = snapshot.tick;
            ++s.snapshots_applied_;
            // A hard world overwrite: the records collected while we waited
            // for this snapshot describe the world it is about to replace.
            s.rollback_.invalidate_history();
            // ...and so does a replay a correction of this poll asked
            // for: its anchor is gone with the history, and a refused own
            // replay would answer this resync with another.
            legacy.drop_pending_rewind();
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
        if (config.rollback) {
            for (const auto& body: snapshot.bodies)
                if (body.kind == bmmo::session::body_kind::Mechanism) {
                    if (snapshot.full && !body.name.empty()) s.mechanism_tracking_.note_name(body.owner, body.name);
                    s.mechanism_tracking_.note_row(snapshot.tick, body);
                }
            auto entity_of = [&](const bmmo::session::body_state& body) -> std::string {
                if (body.kind == bmmo::session::body_kind::Ball) {
                    if (body.owner == s.own_id_) return s.own_physicalized_ ? platform_.own_driven_entity() : std::string();
                    auto it = s.remotes_.find(body.owner);
                    if (it == s.remotes_.end() || !it->second.physicalized || !it->second.navigation) return {};
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
                return legacy.recorded_input_at(entity, tick, out);
            };
            const scope_guard keep_cells = platform_.keep_live_cells(*this);
            const bool rolled = s.rollback_.on_snapshot(platform_.bind_world(), snapshot, current_tick(), entity_of, input_at);
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
                legacy.drop_carried_own_replay(snapshot.tick, own_row);
            }
            if (rolled && config.trace && s.rollback_diag_++ < 16)
                logf("after rollback: movable=%s", legacy.describe_movable_objects().c_str());
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

}
