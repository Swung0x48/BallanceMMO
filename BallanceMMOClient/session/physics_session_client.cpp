// Client side of a physics session (design section 8.5): restart-and-anchor
// on SessionStart, one input per tick, own-ball lifecycle events from the BML
// physicalize hooks, mirrored remote balls and mechanism bodies from the
// server's snapshots, and correction of the predicted own ball.
//
// Network-thread entry points (handle_session_*) only queue or post to the
// game thread; everything that touches the engine runs from OnProcess.
#include "../BallanceMMOClient.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <memory>
#include <sstream>

#include <session/spawn_impulse.hpp>

#include "session_journal_client.hpp"

namespace {
    // Diagnostic: how often the server had to simulate a tick without our input
    // for it (session_snapshot_msg.acked_input_tick).  A file-scope object, not
    // a member: the mod class does not survive having its layout moved.
    struct input_freshness_stats {
        uint64_t reports = 0, starved = 0, lag_sum = 0;
        uint32_t last_acked = 0;
    } input_freshness;

    using bmmo::session::physics_session_state;
    using phase_type = physics_session_state::phase_type;

    // Option A (findings/mechanism-strategy-decision.md): the client renders
    // the script-constrained mechanism bodies from the newest authoritative
    // poses instead of predicting them.  A pair further apart than this in one
    // snapshot interval is a teleport (sector change, level reset, re-entry),
    // and a body further than this from the pose the applier itself last
    // commanded for it is somewhere else entirely (a rebuild, a re-entry, a
    // level reset moved it): both are followed by a snap, never by a skipped
    // write, so the body can never be left behind at the old pose.
    constexpr double kMechanismSnapTravel = 0.5;   // metres per snapshot pair
    // The reference is the commanded pose, not the live body: since 9.20 the
    // applier dead-reckons its write up to kMechanismMaxExtrapolation past the
    // newest row, so the live body is our own command and is legitimately more
    // than this far from the row - measuring the row against it made the
    // applier snap to the raw row and dead-reckon again on the next frame,
    // forever.
    constexpr double kMechanismSnapJump = 1.0;     // metres from the last commanded pose
    // The newest row is stale by the input delay plus half the round trip,
    // measured at ~24 ticks (0.36 s) in the session journals, so rendering it
    // as-is puts a moving mechanism that far behind the predicted ball - the
    // ball then passes through it and the next server correction separates the
    // two.  The render therefore dead-reckons the newest row ahead to the
    // client's own tick along the row's authoritative velocity, bounded so a
    // stalled or reset snapshot stream cannot run away; a snap (teleport) row
    // is never extrapolated, its velocity across the jump means nothing.
    constexpr double kMechanismMaxLead = 0.6;               // seconds past the newest row
    constexpr double kMechanismMaxExtrapolation = 1.5;      // metres of extrapolated travel
    constexpr double kMechanismMaxExtrapolationAngle = 0.5; // radians of extrapolated rotation
    // Below this the body already sits on the target pose: writing it again
    // would only disturb the core's sleep state.
    constexpr double kMechanismWriteEpsilon = 1e-4;

    // The pose one mechanism has to be written with at a given tick, computed
    // from the stored authoritative rows.  One computation for both writers -
    // the live applier (the render tick) and the re-simulation poser (every
    // replayed tick, rollback_world::pre_step) - so a replayed tick renders a
    // mechanism exactly as the live path would have rendered it.
    struct mechanism_target {
        double position[3] = {};
        double rotation[4] = {0.0, 0.0, 0.0, 1.0};
        float linear[3] = {};
        float angular[3] = {};
        bool simulated = false;   // the governing row's wake flag
        bool snap = false;        // write as a snap: teleport row or a body elsewhere
    };

    // `elsewhere` is the applier's kMechanismSnapJump test: our body is further
    // than that from the pose the applier itself last commanded for it - a
    // rebuild, a sector re-entry or a level reset moved it, so the exact newer
    // pose is written instead of a lerp or a dead-reckon, and the write
    // rechecks the contacts.  The live path passes it, a re-simulation does not:
    // there the live pose is this hook's own write for the previous tick and is
    // refreshed as the command for exactly that reason, so the distance says
    // nothing about identity - see the poser below.
    mechanism_target mechanism_target_at(const physics_session_state::mechanism_pose_history& history, uint32_t tick,
                                         bool elsewhere) {
        const auto& rows = history.rows;
        const auto& newest = rows.back();
        mechanism_target out;
        const auto copy_pose = [](const auto& row, mechanism_target& target) {
            for (int k = 0; k < 3; ++k) {
                target.position[k] = row.position[k];
                target.linear[k] = row.linear[k];
                target.angular[k] = row.angular[k];
            }
            for (int k = 0; k < 4; ++k) target.rotation[k] = row.rotation[k];
            target.simulated = row.simulated;
        };
        // A pair further apart than kMechanismSnapTravel is a teleport (sector
        // change, level reset, re-entry): its two poses are not two ends of one
        // motion, so the newer one is used exactly, never interpolated across.
        const auto teleport = [](const auto& older, const auto& newer) {
            if (older.tick >= newer.tick) return false;
            double travel = 0.0;
            for (int k = 0; k < 3; ++k) {
                const double d = newer.position[k] - older.position[k];
                travel += d * d;
            }
            return std::sqrt(travel) > kMechanismSnapTravel;
        };
        if (tick >= newest.tick) {
            // The normal case: the newest row is one input delay plus half the
            // round trip old, so dead-reckon it along its own velocity up to the
            // render tick.  The session steps 1000/66 ms per tick (world.step
            // below), hence the lead in ticks.
            copy_pose(newest, out);
            const bool jumped = rows.size() >= 2 && teleport(rows[rows.size() - 2], newest);
            if (jumped) {
                // The velocity of a row that teleported says nothing about the
                // jump: write the pose exactly, with a contact recheck.
                out.snap = true;
                return out;
            }
            double dt = 0.0;
            if (!elsewhere && newest.simulated) {
                dt = std::min(static_cast<double>(tick - newest.tick) / 66.0, kMechanismMaxLead);
                double speed = 0.0, spin = 0.0;
                for (int k = 0; k < 3; ++k) {
                    speed += static_cast<double>(newest.linear[k]) * newest.linear[k];
                    spin += static_cast<double>(newest.angular[k]) * newest.angular[k];
                }
                speed = std::sqrt(speed);
                spin = std::sqrt(spin);
                if (speed > 0.0) dt = std::min(dt, kMechanismMaxExtrapolation / speed);
                if (spin > 0.0) dt = std::min(dt, kMechanismMaxExtrapolationAngle / spin);
            }
            // A frozen row (simulated == false) holds its pose: dt stays 0.
            out.snap = elsewhere;
            for (int k = 0; k < 3; ++k) out.position[k] = newest.position[k] + newest.linear[k] * dt;
            if (dt > 0.0) {
                // rot_speed is core space (ivp_core.hxx:177; the core carries
                // the object's orientation for the physicalized props), so it
                // is rotated into the world frame by the row's own quaternion
                // (v' = v + 2w (q x v) + 2 q x (q x v)) before the derivative
                // q' = normalize(q + 1/2 dt (w (x) q)) - the same rotation the
                // engine integrates with q_new = q (x) q_core_f_core
                // (ivp_calc_next_psi_solver.cxx:180).
                const double qx = newest.rotation[0], qy = newest.rotation[1];
                const double qz = newest.rotation[2], qw = newest.rotation[3];
                const double tx = 2.0 * (qy * newest.angular[2] - qz * newest.angular[1]);
                const double ty = 2.0 * (qz * newest.angular[0] - qx * newest.angular[2]);
                const double tz = 2.0 * (qx * newest.angular[1] - qy * newest.angular[0]);
                const double wx = newest.angular[0] + qw * tx + (qy * tz - qz * ty);
                const double wy = newest.angular[1] + qw * ty + (qz * tx - qx * tz);
                const double wz = newest.angular[2] + qw * tz + (qx * ty - qy * tx);
                const double h = 0.5 * dt;
                const double nx = qx + h * (wx * qw + wy * qz - wz * qy);
                const double ny = qy + h * (-wx * qz + wy * qw + wz * qx);
                const double nz = qz + h * (wx * qy - wy * qx + wz * qw);
                const double nw = qw - h * (wx * qx + wy * qy + wz * qz);
                const double norm = std::sqrt(nx * nx + ny * ny + nz * nz + nw * nw);
                // A degenerate (non-unit / zero) row keeps its own rotation.
                if (norm > 1e-9) {
                    out.rotation[0] = nx / norm;
                    out.rotation[1] = ny / norm;
                    out.rotation[2] = nz / norm;
                    out.rotation[3] = nw / norm;
                }
            }
            return out;
        }
        // The newest row with a tick <= `tick`, and the one after it: `tick`
        // sits inside that pair (or before every retained row).  Rows are
        // strictly ascending in tick (physics_session_note_mechanism).
        size_t upper = rows.size() - 1;
        while (upper > 0 && rows[upper - 1].tick > tick) --upper;
        if (upper == 0) {
            // Older than every retained row (a resync that jumped back further
            // than the history reaches): hold the oldest row's pose rather than
            // run the body backwards.  Not a snap, exactly as before the
            // history existed: this is a held pose, not a beam into a contact.
            copy_pose(rows.front(), out);
            return out;
        }
        const auto& older = rows[upper - 1];
        const auto& newer = rows[upper];
        copy_pose(newer, out);   // rotation and velocities are the newer row's
        out.snap = teleport(older, newer) || elsewhere;
        if (out.snap) return out;   // teleport / body elsewhere: the exact newer pose
        const double span = static_cast<double>(newer.tick - older.tick);
        const double alpha = std::clamp((static_cast<double>(tick) - older.tick) / span, 0.0, 1.0);
        for (int k = 0; k < 3; ++k)
            out.position[k] = older.position[k] + alpha * (newer.position[k] - older.position[k]);
        return out;
    }

    // Already there: leave the core (and its sleep state) alone instead of
    // rewriting the same pose.  Shared by the applier (once per frame) and the
    // re-simulation poser, where a stationary mechanism would otherwise be
    // rewritten - and woken - on every replayed tick.
    bool mechanism_pose_unchanged(const bmmo_physics_body_state& local, const mechanism_target& target) {
        double dq = 0.0, dp = 0.0, dv = 0.0;
        for (int k = 0; k < 4; ++k) dq += target.rotation[k] * local.rotation[k];
        for (int k = 0; k < 3; ++k) {
            dp += (target.position[k] - local.position[k]) * (target.position[k] - local.position[k]);
            dv += (target.linear[k] - local.linear[k]) * (target.linear[k] - local.linear[k]);
        }
        return std::sqrt(dp) < kMechanismWriteEpsilon && std::sqrt(dv) < kMechanismWriteEpsilon
            && std::fabs(std::fabs(dq) - 1.0) < kMechanismWriteEpsilon;
    }

    // The mechanism bodies a pose write can target, resolved once per writer.
    // A dictionary name can be carried by several server bodies (Level 8 has
    // two same-named P_Modul_30_Wippe instances 332 m apart in different
    // sectors), so the owners sharing a name are narrowed to the one whose
    // authoritative pose is nearest our own local body - and a name this client
    // has no physicalized body for is another sector's instance and is skipped.
    // `history` points into mechanism_authority: nothing removes one mechanism's
    // entry while the session runs, so it stays valid for the write and for
    // recording the pose that write commanded (mechanism_note_command).
    struct mechanism_candidate {
        std::string name;
        bmmo_physics_body_state local{};
        physics_session_state::mechanism_pose_history* history = nullptr;
        double distance = 0.0;   // squared, the chosen history's newest row to `local`
        // Squared distance the applier's `elsewhere` test uses: the live body to
        // the pose the applier last commanded for it, or - on a history no write
        // has reached yet - to the newest authoritative row.  `distance` above
        // has to keep measuring the live pose against the newest row: picking
        // which of two same-named instances we drive is about where the body
        // actually is, not about what we last commanded.
        double elsewhere_distance = 0.0;
    };

    std::vector<mechanism_candidate> mechanism_candidates(physics_session_state& s,
                                                          const bmmo::physics::physics_view& view) {
        std::vector<mechanism_candidate> out;
        if (s.mechanism_names.empty() || s.mechanism_authority.empty()) return out;
        std::map<std::string, std::vector<uint32_t>> owners_by_name;
        for (const auto& [owner, name]: s.mechanism_names) {
            const auto it = s.mechanism_authority.find(owner);
            if (it != s.mechanism_authority.end() && !it->second.rows.empty()) owners_by_name[name].push_back(owner);
        }
        std::string error;
        for (const auto& [name, owners]: owners_by_name) {
            mechanism_candidate candidate;
            candidate.name = name;
            if (!view.get_body_state(name.c_str(), candidate.local, error)) continue;   // not physicalized here
            for (uint32_t owner: owners) {
                auto& history = s.mechanism_authority.at(owner);
                double distance = 0.0;
                for (int k = 0; k < 3; ++k) {
                    const double d = history.rows.back().position[k] - candidate.local.position[k];
                    distance += d * d;
                }
                if (!candidate.history || distance < candidate.distance) {
                    candidate.history = &history;
                    candidate.distance = distance;
                }
            }
            // The reference of the `elsewhere` test is the applier's own last
            // command once it has written one.  The live body cannot be it: the
            // applier's dead-reckoned write IS the live body (a body up to
            // kMechanismMaxExtrapolation past the newest row, by design), so
            // reading it back as a body "somewhere else" made the applier snap
            // to the raw row and dead-reckon again on the very next frame - the
            // 9.22 journals show the local mechanism poses split between exactly
            // a raw row and exactly the extrapolated target, mech_snaps ~2 per
            // tick, above ~2.8 m/s.  A body is somewhere else only when it is no
            // longer where the applier put it: a rebuild, a re-entry, a level
            // reset.  With no command recorded yet (a fresh history, which is
            // what rebase_tick leaves) the newest row is used instead, i.e. the
            // test this applier had before it ever dead-reckoned.
            const double* reference = candidate.history->have_last_target
                    ? candidate.history->last_target : candidate.history->rows.back().position;
            for (int k = 0; k < 3; ++k) {
                const double d = reference[k] - candidate.local.position[k];
                candidate.elsewhere_distance += d * d;
            }
            out.push_back(std::move(candidate));
        }
        return out;
    }

    // Record the pose a writer commanded for one mechanism body.  Both writers
    // call it - the live applier and the re-simulation poser - so the next live
    // frame's `elsewhere` test asks the applier's own question ("is the body
    // still where we put it?") and never mistakes the dead-reckoned write it
    // left behind for a foreign pose.
    void mechanism_note_command(physics_session_state::mechanism_pose_history& history,
                                const mechanism_target& target) {
        for (int k = 0; k < 3; ++k) history.last_target[k] = target.position[k];
        history.have_last_target = true;
    }

    void copy_name(char* out, size_t size, const std::string& text) {
        std::snprintf(out, size, "%s", text.c_str());
    }

    // Wire recipe (strings/vectors) -> bridge recipe (fixed arrays).
    bmmo_physics_ball_recipe to_bridge_recipe(const bmmo::session::ball_recipe& r) {
        bmmo_physics_ball_recipe p{};
        p.fixed = r.fixed;
        p.start_frozen = r.start_frozen;
        p.enable_collision = r.enable_collision;
        p.calc_mass_center = r.calc_mass_center;
        p.friction = r.friction;
        p.elasticity = r.elasticity;
        p.mass = r.mass;
        p.linear_damp = r.linear_damp;
        p.rot_damp = r.rot_damp;
        for (int k = 0; k < 3; ++k) p.mass_center[k] = r.mass_center[k];
        copy_name(p.collision_surface, sizeof(p.collision_surface), r.collision_surface);
        p.convex_count = static_cast<int32_t>(std::min<size_t>(r.convex_meshes.size(), BMMO_PHYSICS_MAX_CONVEX));
        for (int i = 0; i < p.convex_count; ++i) copy_name(p.convex[i], sizeof(p.convex[i]), r.convex_meshes[i]);
        p.ball_count = static_cast<int32_t>(std::min<size_t>(r.balls.size(), BMMO_PHYSICS_MAX_BALLS));
        for (int i = 0; i < p.ball_count; ++i) {
            for (int k = 0; k < 3; ++k) p.ball_center[i][k] = r.balls[i].center[k];
            p.ball_radius[i] = r.balls[i].radius;
        }
        p.concave_count = static_cast<int32_t>(std::min<size_t>(r.concave_meshes.size(), BMMO_PHYSICS_MAX_CONCAVE));
        for (int i = 0; i < p.concave_count; ++i) copy_name(p.concave[i], sizeof(p.concave[i]), r.concave_meshes[i]);
        return p;
    }

    // A lifecycle event in journal shape (session/journal.hpp EVENT records):
    // the same field copy on the way out and on the way in, so both sides of a
    // session read alike in the black box.
    bmmo::session::journal_event to_journal_event(const bmmo::session_event_msg& msg, uint32_t id) {
        bmmo::session::journal_event e;
        // Both ticks are the stamped one: only the server sees the tick its
        // world finally dequeued the event at, and a client never learns it.
        e.tick = msg.tick;
        e.event_tick = msg.tick;
        e.id = id;
        e.type = msg.type;
        e.ball_type = msg.ball_type;
        e.flags = msg.flags;
        for (int k = 0; k < 3; ++k) e.position[k] = msg.position[k];
        for (int k = 0; k < 9; ++k) e.rotation[k] = msg.rotation[k];
        e.sector = msg.sector;
        e.name = msg.name;
        e.recipe = to_bridge_recipe(msg.recipe);
        return e;
    }

    VxMatrix matrix_from_pose(const float position[3], const float rotation[9]) {
        VxMatrix matrix;
        matrix.SetIdentity();
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k) matrix[r][k] = rotation[r * 3 + k];
        matrix[3][0] = position[0];
        matrix[3][1] = position[1];
        matrix[3][2] = position[2];
        matrix[3][3] = 1.0f;
        return matrix;
    }

    // Snapshot rows carry a quaternion (body_state::rotation), the wire event a
    // world matrix; a mirror started from a row needs the quaternion form.
    VxMatrix matrix_from_quaternion(const double position[3], const double rotation[4]) {
        VxQuaternion quaternion;
        quaternion.x = static_cast<float>(rotation[0]);
        quaternion.y = static_cast<float>(rotation[1]);
        quaternion.z = static_cast<float>(rotation[2]);
        quaternion.w = static_cast<float>(rotation[3]);
        VxMatrix matrix;
        quaternion.ToMatrix(matrix);
        for (int k = 0; k < 3; ++k) matrix[3][k] = static_cast<float>(position[k]);
        matrix[3][3] = 1.0f;
        return matrix;
    }

    // The join order this client mirrors a player with: the roster SessionStart
    // brought, or the last slot for someone who joined after it - the server
    // announces a join to the joiner alone, so nothing on the wire carries a
    // late joiner's order.  The black box records the same guess.
    uint8_t mirror_join_order(const physics_session_state& s, uint32_t id) {
        for (const auto& p: s.players) if (p.id == id) return static_cast<uint8_t>(p.join_order);
        return 63;
    }

    // The behavior the PreSimulate clock guard hangs its callback on: the
    // navigation replica while the session has one, otherwise the level script
    // itself.  A destroyed behavior takes its callbacks with it
    // (ClearBehaviorCallbacks), so the id is resolved again on every frame the
    // guard is re-attached.
    uint32_t clock_guard_behavior_id(IBML* bml, const physics_session_state& s) {
        if (s.navigation.ball_navigation) return s.navigation.ball_navigation;
        CKBehavior* script = bml->GetScriptByName("Gameplay_Ingame");
        return script ? script->GetID() : 0;
    }

    // The behavior whose deactivation *is* the retail pause: the Event_handler's
    // "Pause Level" chain stops Gameplay_Ingame.  The guard reads it inside the
    // engine's PreSimulate pass, because the mod's own frame hook runs after
    // the physics step and would see the pause one frame too late.
    uint32_t pause_sensor_behavior_id(IBML* bml) {
        CKBehavior* script = bml->GetScriptByName("Gameplay_Ingame");
        return script ? script->GetID() : 0;
    }
}

// ---------------------------------------------------------------- network thread

void BallanceMMOClient::handle_session_start(bmmo::session_start_msg msg) {
    // std::function needs a copyable target; the message owns a stringstream
    auto shared = std::make_shared<bmmo::session_start_msg>(std::move(msg));
    utils_.run_on_game_thread([this, shared] { physics_session_begin(*shared); });
}

void BallanceMMOClient::handle_session_assign(const bmmo::session_assign_msg& msg) {
    utils_.run_on_game_thread([this, session = msg.session, first_tick = msg.first_tick] {
        auto& s = physics_session_;
        if (s.session != session || s.phase != phase_type::running) return;
        // Both branches below renumber, and a renumber invalidates everything
        // the server acked under the old numbering: the first snapshot of the
        // new one sets the starvation detector's baseline again.
        s.acked_input.reset();
        if (s.assigned) {
            // Resync (design 9.2): tick numbering restarts from the server's
            // current tick, histories are dropped, the next full snapshot
            // rebuilds every body.
            s.rebase_tick(first_tick);
            s.resync_pending = true;
            s.have_snapshot = false;
            bmmo::session::client_journal::instance().note(
                    s.tick_base, std::format("resync: reassigned, tick base {}", s.tick_base));
            logger_->Info("Physics session %u: resynced, tick base %u", s.session, s.tick_base);
            return;
        }
        s.assigned = true;
        if (first_tick != 0) {
            // A numbered base.  The server hands one out at a session start
            // (the session's start lead, protocol 2.2) as well as to a late join
            // or a resync (its current tick plus the same lead), so this is the
            // ordinary path.  The frames recorded before this assignment carry
            // OUR numbers - the anchor-relative ones, which name ticks the
            // server never asked us for - while the server's read cursor starts
            // at the base.  Keeping them would leave the base's own frames
            // missing for as long as the anchor ran ahead (the server fills
            // those ticks with a repeated frame), plus a permanent k-tick extra
            // lead on top of the input delay that every snapshot arrives that
            // much further behind.  Renumber here, exactly like the headless
            // session client (sim/session_client.cpp).
            const int64_t renumbered = s.frames_since_anchor;
            s.rebase_tick(first_tick);
            logger_->Info("Physics session %u: tick base %u assigned (renumbered, %lld anchor-relative frames dropped)",
                          s.session, s.tick_base, static_cast<long long>(renumbered));
        } else {
            // Base 0: a server that still numbers the members present at the
            // start from their own anchor.  The backlog frames stamped 0..k-1
            // are valid inputs it is waiting for, and renumbering would relabel
            // them.
            s.tick_base = first_tick;
            logger_->Info("Physics session %u: tick base %u assigned (%lld frames since anchor)", s.session,
                          s.tick_base, static_cast<long long>(s.frames_since_anchor));
        }
        s.last_rebases = fixed_tick_.rebases();
        bmmo::session::client_journal::instance().note(
                s.tick_base, std::format("assigned: tick base {}", s.tick_base));
        physics_session_flush_inputs();
    });
}

void BallanceMMOClient::handle_session_snapshot(bmmo::session_snapshot_msg msg) {
    auto& s = physics_session_;
    std::lock_guard lk(s.queue_mutex);
    ++s.snapshots_received;
    if (s.snapshot_queue.size() >= physics_session_state::kMaxQueuedSnapshots) s.snapshot_queue.pop_front();
    s.snapshot_queue.push_back(std::move(msg));
}

void BallanceMMOClient::handle_session_event(bmmo::session_event_msg msg) {
    auto& s = physics_session_;
    std::lock_guard lk(s.queue_mutex);
    ++s.events_received;
    s.event_queue.push_back(std::move(msg));
}

void BallanceMMOClient::handle_session_remote_input(bmmo::session_remote_input_msg msg) {
    auto& s = physics_session_;
    std::lock_guard lk(s.queue_mutex);
    ++s.remote_inputs_received;
    if (s.remote_input_queue.size() >= physics_session_state::kMaxQueuedSnapshots) s.remote_input_queue.pop_front();
    s.remote_input_queue.push_back(std::move(msg));
}

void BallanceMMOClient::handle_session_end(const bmmo::session_end_msg& msg) {
    utils_.run_on_game_thread([this, session = msg.session, reason = msg.reason] {
        if (physics_session_.session != session && physics_session_.session != 0) return;
        physics_session_end_local("ended by the server: " + reason);
    });
}

// ---------------------------------------------------------------- game thread

void BallanceMMOClient::physics_session_begin(const bmmo::session_start_msg& msg) {
    auto& s = physics_session_;
    if (s.phase != phase_type::idle) physics_session_end_local("replaced by a new session");
    s.reset_runtime();
    // The black box is armed once per session, so switching it off in the BML
    // menu takes effect from the next one (and never mid-recording).
    bmmo::session::client_journal::instance().set_enabled(config_manager_["session_journal"]->GetBoolean());
    s.session = msg.session;
    s.room = msg.room;
    s.snapshot_interval = msg.snapshot_interval;
    s.input_delay = msg.input_delay;
    s.seed = msg.seed;
    s.spawn_impulse = msg.spawn_impulse;
    s.map = msg.map;
    s.players = msg.players;
    s.own_join_order = -1;
    s.spawn_known = false;
    const auto own = db_.get_client_id();
    for (const auto& p: s.players) {
        if (p.id != own) continue;
        s.own_join_order = p.join_order;
        for (int k = 0; k < 3; ++k) s.spawn_position[k] = p.spawn_position[k];
        for (int k = 0; k < 4; ++k) s.spawn_rotation[k] = p.spawn_rotation[k];
        s.spawn_known = true;
    }
    s.own_group = "P#" + std::to_string(s.own_join_order < 0 ? 63 : s.own_join_order);
    std::string error;
    if (!physics_view_.available() && !physics_view_.initialize(m_bml->GetCKContext(), error)) {
        SendIngameMessage("Physics session unavailable: " + error, bmmo::ansi::BrightRed);
        s.phase = phase_type::idle;
        return;
    }
    if (!m_bml->IsIngame()) {
        SendIngameMessage("Physics session: you must be in the level to take part.", bmmo::ansi::BrightRed);
        // Say it out loud instead of going silent: the server's start barrier
        // waits for this member's SessionReady, which will never come.  Leaving
        // the room removes us from the session and re-runs the barrier for the
        // members that are actually here.
        bmmo::room_request_msg leave;
        leave.action = bmmo::room::action::Leave;
        leave.room = s.room;
        leave.serialize();
        send(leave.raw.str().data(), leave.size(), k_nSteamNetworkingSend_Reliable);
        logger_->Info("Physics session %u: not in the level, left room %u", s.session, s.room);
        s.phase = phase_type::idle;
        return;
    }
    s.phase = phase_type::counting_down;
    s.saw_ingame_inactive = false;
    s.restart_deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(PHYSICS_SESSION_COUNTDOWN + 5);
    SendIngameMessage(std::format("Physics session {} starting: the level restarts on \"Go!\".", s.session),
                      bmmo::ansi::BrightGreen);
    logger_->Info("Physics session %u: countdown started (players=%zu, join order %d)", s.session, s.players.size(),
                  s.own_join_order);
    physics_session_countdown();
}

// The classic "3 - 2 - 1 - Go!" lead-in (the same cue as /mmo countdown) in
// front of the restart: everyone in the room gets their SessionStart within a
// few milliseconds of each other, so the local timers stay in step. Nothing
// here is part of the deterministic run - the restart, and with it the anchor
// frame, happens on "Go!".
void BallanceMMOClient::physics_session_countdown() {
    const uint32_t session = physics_session_.session;
    // Stale timers of a session that ended in the meantime must do nothing.
    const auto still_starting = [this, session] {
        return physics_session_.session == session && physics_session_.phase == phase_type::counting_down;
    };
    float delay = 0.0f;
    for (int i = PHYSICS_SESSION_COUNTDOWN; i >= 1; --i) {
        m_bml->AddTimer(delay, [this, session, i, still_starting] {
            if (!still_starting()) return;
            SendIngameMessage(std::format("Physics session {} - {}", session, i), bmmo::ansi::BrightGreen);
            play_countdown_sound(bmmo::countdown_type::Countdown_1);   // one dong per number
        });
        delay += 1000.0f;
    }
    m_bml->AddTimer(delay, [this, session, still_starting] {
        if (!still_starting()) return;
        auto& s = physics_session_;
        SendIngameMessage(std::format("Physics session {} - Go!", session), bmmo::ansi::BrightGreen);
        play_countdown_sound(bmmo::countdown_type::Go);
        s.phase = phase_type::restarting;
        s.saw_ingame_inactive = false;
        s.restart_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        logger_->Info("Physics session %u: restart requested", session);
        // Fixed 1/66 s behaviour delta from here on: the retail intro timers start
        // in the anchor frame (Gameplay_Ingame's first frame), and with the retail
        // limits that frame's delta is whatever the restart took (4.5 ms .. 200 ms
        // measured), which moved the intro's end by a tick between the two sides.
        if (!fixed_tick_.enabled()) fixed_tick_.enable(m_bml);
        restart_current_level();
    });
}

void BallanceMMOClient::physics_session_end_local(const std::string& reason) {
    auto& s = physics_session_;
    std::string error;
    pause_clock_restore();
    // The clock guard is scoped to the session and must not outlive it, so it
    // is dropped even when there is nothing left to tear down.
    physics_view_.set_clock_guard(false, 0.001f, 0, 0, error);
    if (s.phase == phase_type::idle) return;
    for (auto& [id, remote]: s.remotes) {
        if (remote.navigation && physics_view_.available()) physics_view_.navigation_destroy(remote.entity.c_str(), error);
        if (remote.physicalized && physics_view_.available()) physics_view_.unphysicalize(remote.entity.c_str(), error);
        objects_.set_physicalized(id, false);
    }
    if (s.own_navigation && physics_view_.available()) physics_view_.navigation_destroy(s.own_nav_entity.c_str(), error);
    if (s.body_guard && physics_view_.available()) physics_view_.set_body_guard(false, nullptr, error);
    if (fixed_tick_.enabled()) fixed_tick_.disable(m_bml);
    // The black box closes here, whatever ended the session; the file is the
    // only thing that survives reset_runtime().
    bmmo::session::client_journal::instance().end(s.current_tick(), reason);
    const uint32_t session = s.session;
    s.reset_runtime();
    s.phase = phase_type::idle;
    s.session = 0;
    SendIngameMessage(std::format("Physics session {} over ({}).", session, reason), bmmo::ansi::BrightYellow);
    logger_->Info("Physics session %u ended: %s", session, reason.c_str());
}

// Called at the start of every OnProcess, after the fixed-tick pacing.
void BallanceMMOClient::process_physics_session() {
    auto& s = physics_session_;
    switch (s.phase) {
    case phase_type::idle:
    case phase_type::ended:
        return;
    case phase_type::counting_down:
        // The lead-in only waits; leaving the level during it (a quit to the
        // menu, a death that ends the run) cancels the session instead of
        // restarting something else on "Go!".
        if (!m_bml->IsIngame())
            physics_session_end_local("the level was left during the countdown");
        else if (std::chrono::steady_clock::now() > s.restart_deadline)
            physics_session_end_local("the countdown did not finish");
        return;
    case phase_type::restarting: {
        const bool active = gameplay_ingame_script_active();
        if (!active) {
            s.saw_ingame_inactive = true;
        } else if (s.saw_ingame_inactive) {
            physics_session_anchor();
        } else if (std::chrono::steady_clock::now() > s.restart_deadline) {
            physics_session_end_local("the level did not restart");
        }
        return;
    }
    case phase_type::running:
        physics_session_frame();
        return;
    }
}

// The two retail blocks that write the physics time factor from the pause menu
// (design: the session clock).  Resolved from the level's Event_handler, which
// exists by the time the session anchors.
//
// Unit conversion: the block feeds its "Physic Time Factor" input straight to
// CKIpionManager::SetTimeFactor, which scales it by 0.001
// (CKIpionManager.cpp:848) -- the script value is 2 during normal play, while
// get_clock() reports the scaled engine value (0.002).
static constexpr float kTimeFactorScriptScale = 1000.0f;

bool BallanceMMOClient::pause_clock_resolve() {
    CKBehavior* event_handler = m_bml->GetScriptByName("Event_handler");
    if (!event_handler) return false;
    static const char* kChains[2] = {"Pause Level", "Unpause Level"};
    bool resolved = true;
    for (int i = 0; i < 2; ++i) {
        pause_clock_[i] = {};
        CKBehavior* chain = ScriptHelper::FindFirstBB(event_handler, kChains[i]);
        CKBehavior* block = chain ? ScriptHelper::FindFirstBB(chain, "Set Physics Globals", true) : nullptr;
        auto* input = block ? block->GetInputParameter(1) : nullptr;
        CKParameter* param = input ? input->GetRealSource() : nullptr;
        if (!param) {
            resolved = false;
            continue;
        }
        pause_clock_[i].behavior = block->GetID();
        pause_clock_[i].retail = ScriptHelper::GetParamValue<float>(param);
        pause_clock_[i].applied = pause_clock_[i].retail;
    }
    if (resolved)
        logger_->Info("Physics session: pause chain time factors: pause=%.4f unpause=%.4f",
                      pause_clock_[0].retail, pause_clock_[1].retail);
    return resolved;
}

void BallanceMMOClient::pause_clock_apply(float factor) {
    const float script = factor * kTimeFactorScriptScale;
    for (auto& write: pause_clock_) {
        if (!write.behavior || write.applied == script) continue;
        auto* block = static_cast<CKBehavior*>(m_bml->GetCKContext()->GetObject(write.behavior));
        auto* input = block ? block->GetInputParameter(1) : nullptr;
        CKParameter* param = input ? input->GetRealSource() : nullptr;
        if (!param) continue;
        float value = script;
        param->SetValue(&value, sizeof(value));
        write.applied = script;
    }
}

void BallanceMMOClient::pause_clock_restore() {
    for (auto& write: pause_clock_) {
        if (!write.behavior) continue;
        auto* block = static_cast<CKBehavior*>(m_bml->GetCKContext()->GetObject(write.behavior));
        auto* input = block ? block->GetInputParameter(1) : nullptr;
        CKParameter* param = input ? input->GetRealSource() : nullptr;
        if (param && write.applied != write.retail) {
            float value = write.retail;
            param->SetValue(&value, sizeof(value));
        }
        write = {};
    }
}

// The anchor frame: session clock reset, world hash, SessionReady.
void BallanceMMOClient::physics_session_anchor() {
    auto& s = physics_session_;
    std::string error;
    {
        auto* time_manager = m_bml->GetTimeManager();
        logger_->Info("Physics session anchor timing: driver_enabled=%d last_delta=%.4f cktime=%.3f frames=%d",
                      fixed_tick_.enabled() ? 1 : 0, time_manager->GetLastDeltaTime(), time_manager->GetTime(),
                      time_manager->GetMainTickCount());
    }
    if (fixed_tick_.enabled()) fixed_tick_.disable(m_bml);
    fixed_tick_.enable(m_bml);
    if (!physics_view_.reset_session_clock(s.seed, error)) {
        physics_session_end_local("session clock reset failed: " + error);
        return;
    }
    // The retail pause menu stops Gameplay_Ingame and writes the time factor 0;
    // the guard pins the factor to the value the run had before the menu opened
    // for as long as it is open, and samples it otherwise, so the level scripts
    // keep driving the clock exactly as they do on the server.
    const uint32_t guard_id = clock_guard_behavior_id(m_bml, s);
    const uint32_t pause_id = pause_sensor_behavior_id(m_bml);
    if (!guard_id)
        logger_->Warn("Physics session: no behavior to anchor the clock guard on");
    else if (!pause_id)
        logger_->Warn("Physics session: clock guard has no pause sensor (Gameplay_Ingame)");
    else if (!physics_view_.set_clock_guard(true, 0.001f, guard_id, pause_id, error))
        logger_->Warn("Physics session: clock guard: %s", error.c_str());
    // The pause menu's own time-factor write is turned into a no-op for the
    // session (see pause_clock_resolve): the retail chain would otherwise stop
    // the clock, which the server never does.
    if (!pause_clock_resolve())
        logger_->Warn("Physics session: the pause chain's time factor block was not found");
    else {
        float factor = 0.0f, delta = 0.0f;
        if (physics_view_.get_clock(factor, delta, error)) pause_clock_apply(factor);
    }
    bmmo::physics::world_hash hash;
    if (!physics_view_.capture(hash, error)) {
        physics_session_end_local("world hash failed: " + error);
        return;
    }
    if (!physics_view_.install_player_collision_filter("P#", error))
        logger_->Warn("Physics session: player collision filter: %s", error.c_str());
    s.anchor_hash = hash.pose;   // movable-core pose hash (see physics_world::anchor)
    s.anchor_surfaces = hash.surfaces;
    s.anchored = true;
    s.frames_since_anchor = 0;
    s.phase = phase_type::running;
    s.corrector.clear();
    physics_view_.drain_event_log();   // install the listener, discard history
    s.ball_forces.clear();
    if (CKDataArray* balls = m_bml->GetArrayByName("Physicalize_GameBall")) {
        const int rows = balls->GetRowCount();
        for (int row = 0; row < rows; ++row) {
            float force = 0.0f;
            balls->GetElementValue(row, 7, &force);
            s.ball_forces.push_back(force);
        }
    }
    bmmo::session_ready_msg ready;
    ready.session = s.session;
    ready.first_tick = 0;
    ready.anchor_hash = s.anchor_hash;
    ready.anchor_surfaces = s.anchor_surfaces;
    ready.physics_sha256 = physics_view_.dll_sha256();
    ready.build_id = physics_view_.build_id();
    ready.serialize();
    send(ready.raw.str().data(), ready.size(), k_nSteamNetworkingSend_Reliable);
    logger_->Info("Physics session %u: anchored (hash %016llx surfaces %016llx), waiting for the tick assignment",
                  s.session, static_cast<unsigned long long>(s.anchor_hash),
                  static_cast<unsigned long long>(s.anchor_surfaces));
    logger_->Info("Physics session anchor state: cores=%d ivp_time=%.6f seed=%d mc=%d psi=%.6f/%.6f delta=%.4f pdelta=%.6f factor=%.6f movable=%s",
                  hash.cores, hash.ivp_time, hash.ivp_seed, static_cast<int>(hash.next_movement_check),
                  hash.time_of_last_psi, hash.time_of_next_psi, hash.delta_time_ms, hash.physics_delta_time,
                  hash.time_factor, physics_view_.describe_movable_objects().c_str());
    logger_->Info("Physics session anchor bodies: %s", physics_view_.describe_physics_objects().substr(0, 900).c_str());
    physics_session_journal_begin();
    SendIngameMessage("Physics session: level synchronized, waiting for the server.", bmmo::ansi::BrightGreen);
}

// Opens this session's black box (session/session_journal_client.hpp) at the
// anchor, where the world the server will hash is finally known.  The tick base
// is not: it arrives with SessionAssign and goes into a NOTE, so the header's
// first_tick stays 0 - the records before the assignment are numbered from the
// anchor, and the renumbering (a start member's base included) shows up in the
// file as a jump, which is what sim_tool's --continue-after-jump is for.  A
// journal that cannot be opened is logged once and the session runs without one.
void BallanceMMOClient::physics_session_journal_begin() {
    auto& s = physics_session_;
    auto& journal = bmmo::session::client_journal::instance();
    if (!journal.enabled()) return;
    bmmo::session::journal_header header;
    header.session = s.session;
    header.level = s.map.level;
    header.seed = s.seed;
    header.spawn_impulse = s.spawn_impulse;
    header.input_delay = s.input_delay;
    header.first_tick = 0;
    header.anchor_hash = s.anchor_hash;
    header.anchor_surfaces = s.anchor_surfaces;
    header.build_id = physics_view_.build_id();
    header.own_player = db_.get_client_id();
    // The same fallback s.own_group and the own spawn impulse use: a roster
    // that does not name us still puts our ball in slot 63, so the file has to
    // say 63 too or a replay would spawn it in another group, with another
    // impulse direction.  255 is reserved for a server journal, which has no
    // own player at all.
    header.own_join_order = static_cast<uint8_t>(s.own_join_order < 0 ? 63 : s.own_join_order);
    std::vector<bmmo::session::journal_player> members;
    std::string start = std::format("start: session {} room {} level {} players", s.session, s.room, s.map.level);
    for (const auto& p: s.players) {
        bmmo::session::journal_player member;
        member.tick = 0;
        member.id = p.id;
        member.join_order = p.join_order;
        member.added = true;
        if (p.id == header.own_player)
            member.name = get_display_nickname();
        else if (const auto state = db_.get(p.id))
            member.name = state->name;
        members.push_back(member);
        start += std::format(" {}({}, join {})", member.name, member.id, member.join_order);
    }
    std::string error;
    if (!journal.begin(header, bmmo::session::client_journal::directory(),
                       bmmo::session::client_journal::kMaxBytes, members, error)) {
        logger_->Warn("Physics session: no journal for this session: %s", error.c_str());
        return;
    }
    journal.note(0, start);
    journal.note(0, std::format("anchor: hash={:016x} surfaces={:016x} build={}", s.anchor_hash, s.anchor_surfaces,
                                physics_view_.build_id()));
    logger_->Info("Physics session %u: journal at %s", s.session, journal.path().string().c_str());
}

// Sends every buffered input frame the server does not have yet, newest
// last, in chunks of kInputHistory.
void BallanceMMOClient::physics_session_flush_inputs() {
    auto& s = physics_session_;
    if (!s.assigned || s.input_history.empty()) return;
    size_t index = 0;
    while (index < s.input_history.size()) {
        bmmo::session_input_msg msg;
        msg.session = s.session;
        msg.first_tick = s.input_history[index].first;
        while (index < s.input_history.size() && msg.frames.size() < physics_session_state::kInputHistory) {
            msg.frames.push_back(s.input_history[index].second);
            ++index;
        }
        msg.serialize();
        send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_UnreliableNoDelay);
        ++s.inputs_sent;
    }
}

void BallanceMMOClient::physics_session_frame() {
    auto& s = physics_session_;
    ++s.frames_since_anchor;
    const uint32_t tick = s.current_tick();
    std::string error;
    CK3dObject* ball = get_current_ball();
    const std::string ball_name = ball && ball->GetName() ? ball->GetName() : "";

    // Re-attach the clock guard every frame: a sector change or a level reset
    // destroys the behavior it hung on (and with it the callback), while the
    // session keeps running.  Idempotent while the callback is still there.
    const uint32_t guard_id = clock_guard_behavior_id(m_bml, s);
    const uint32_t pause_id = pause_sensor_behavior_id(m_bml);
    if (guard_id && pause_id && !physics_view_.set_clock_guard(true, 0.001f, guard_id, pause_id, error))
        s.last_error = error;
    // Keep the pause chain's write equal to the factor in use: it changes only
    // when the level scripts change it (the tutorial), so this is a compare
    // most frames.
    {
        float factor = 0.0f, delta = 0.0f;
        if (physics_view_.get_clock(factor, delta, error)) pause_clock_apply(factor);
    }

    // The tick driver restarted its schedule (pause, long stall): our tick
    // numbers no longer line up with the server's.
    if (s.assigned && fixed_tick_.rebases() != s.last_rebases) {
        s.last_rebases = fixed_tick_.rebases();
        physics_session_request_resync("tick driver rebased");
    }

    // Key bindings arrive from Gameplay_Refresh a few frames after the anchor.
    const bool keys_were_known = s.navigation_keys_known;
    if (!s.navigation_keys_known) {
        auto graph = bmmo::game::read_navigation_graph(m_bml->GetCKContext());
        // Other mods graft SetPhysicsForce leaves of their own onto Ball
        // Navigation (NewBallType's sticky ball, fly-up/fly-down helpers),
        // driven by Key Events with no key bound.  They would keep "every leaf
        // has a key" false for the whole session, and a session that never
        // learns its keys never sends any: the server's copy of the ball then
        // sits at the spawn while the player drives the local one (found by
        // the session black box, design 9.15).  The server runs the unmodded
        // script with four keyed leaves, so only those count: unbound leaves
        // are dropped and the survivors renumbered in retail order, and the
        // wait for Gameplay_Refresh to bind the retail four stays.
        if (graph.valid()) {
            graph.leaves.erase(std::remove_if(graph.leaves.begin(), graph.leaves.end(),
                                              [](const bmmo::game::navigation_leaf& leaf) { return leaf.key == 0; }),
                               graph.leaves.end());
            for (size_t i = 0; i < graph.leaves.size(); ++i) graph.leaves[i].index = static_cast<int>(i);
        }
        const bool complete = graph.valid() && graph.leaves.size() >= 4;
        if (complete) {
            s.navigation = graph;
            s.navigation_keys_known = true;
            logger_->Info("Physics session: %zu navigation leaves, keys %d/%d/%d/%d", graph.leaves.size(),
                          graph.leaves.size() > 0 ? graph.leaves[0].key : 0, graph.leaves.size() > 1 ? graph.leaves[1].key : 0,
                          graph.leaves.size() > 2 ? graph.leaves[2].key : 0, graph.leaves.size() > 3 ? graph.leaves[3].key : 0);
        }
    }

    if (!keys_were_known && s.navigation_keys_known)
        for (auto& [id, remote]: s.remotes) physics_session_attach_remote_navigation(id);

    // Engine change #6: keep the level bodies through the retail sector
    // reset (death); only the current ball may be unphysicalized.
    if (!ball_name.empty() && (!s.body_guard || s.body_guard_entity != ball_name)) {
        if (physics_view_.set_body_guard(true, ball_name.c_str(), error)) {
            s.body_guard = true;
            s.body_guard_entity = ball_name;
        } else
            s.last_error = error;
    }

    // Own ball state after this tick.
    bmmo_physics_body_state own{};
    s.own_physicalized = !ball_name.empty() && physics_view_.get_body_state(ball_name.c_str(), own, error);
    if (s.own_physicalized) {
        if (!s.own_group_set) {
            if (physics_view_.set_body_group(ball_name.c_str(), s.own_group.c_str(), error)) s.own_group_set = true;
            else logger_->Warn("Physics session: own ball group: %s", error.c_str());
        }
        bmmo::session::ball_pose pose;
        pose.tick = tick;
        for (int k = 0; k < 3; ++k) {
            pose.position[k] = own.position[k];
            pose.linear[k] = own.linear[k];
            pose.angular[k] = own.angular[k];
        }
        for (int k = 0; k < 4; ++k) pose.rotation[k] = own.rotation[k];
        s.corrector.record(pose);
        if (s.navigation_keys_known && (!s.own_navigation || s.own_nav_entity != ball_name))
            physics_session_attach_own_navigation(ball_name, static_cast<uint8_t>(db_.get_ball_id(ball_name)));
    } else {
        s.own_group_set = false;
        // The ball stays where the retail script puts it (the resetpoint);
        // no ring slot to move it to (design 9.10 replaces the ring with a
        // spawn impulse, applied once the body exists in OnPhysicalize).
    }

    // Remote balls after this tick (design 9.1): the snapshot for tick T is
    // compared with the state recorded at T.
    for (auto& [id, remote]: s.remotes) {
        if (!remote.physicalized || !remote.navigation) continue;
        bmmo_physics_body_state local{};
        if (!physics_view_.get_body_state(remote.entity.c_str(), local, error)) continue;
        bmmo::session::ball_pose pose;
        pose.tick = tick;
        for (int k = 0; k < 3; ++k) {
            pose.position[k] = local.position[k];
            pose.linear[k] = local.linear[k];
            pose.angular[k] = local.angular[k];
        }
        for (int k = 0; k < 4; ++k) pose.rotation[k] = local.rotation[k];
        remote.corrector.record(pose);
    }

    // Input for this tick: keys polled at this frame's PreProcess, camera
    // basis from the END of the previous frame (the retail Ball Navigation
    // executes before the camera scripts of a frame), nav state after this
    // frame's scripts.

    // Pause menu (ESC): the retail scripts stop Gameplay_Ingame, which also
    // stops the keyboard poll they drive, while the session keeps stepping
    // (PreSimulate clock guard).  Report zero keys and stop the replica from
    // polling them, so the ball is not driven by the arrow keys the player
    // holds while the menu is open.
    const bool muted = m_bml->IsIngame() && !gameplay_ingame_script_active();
    if (muted != s.input_muted) {
        logger_->Info("Physics session: input %s (ingame script %d, paused %d)", muted ? "muted" : "live",
                      gameplay_ingame_script_active() ? 1 : 0, m_bml->IsPaused() ? 1 : 0);
        if (s.own_navigation
                && !physics_view_.navigation_poll(s.own_nav_entity.c_str(), !muted, s.own_key_codes, s.own_key_blocks,
                                                  s.own_key_count, error))
            s.last_error = error;
        s.input_muted = muted;
    }
    bmmo::session::input_frame frame{};
    if (!muted && s.navigation_keys_known && input_hook_installed_)
        frame.keys = s.navigation.keys_from_state(frame_keys_.data());
    float cam[3][3] = {};
    bool cam_valid = false;
    if (auto* orient = m_bml->Get3dEntityByName("Cam_OrientRef")) {
        const VxMatrix& m = orient->GetWorldMatrix();
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k) cam[r][k] = m[r][k];
        cam_valid = true;
    }
    const float (*basis)[3] = s.previous_cam_valid ? s.previous_cam : cam;
    for (int k = 0; k < 3; ++k) {
        frame.cam_right[k] = basis[0][k];
        frame.cam_up[k] = basis[1][k];
        frame.cam_dir[k] = basis[2][k];
    }
    if (cam_valid) {
        std::memcpy(s.previous_cam, cam, sizeof(cam));
        s.previous_cam_valid = true;
    }
    if (s.own_navigation) {
        // The replica polls keys and BallNav activity itself at PreSimulate;
        // only the camera rows (previous frame, like the server) come from here.
        if (!physics_view_.navigation_input(s.own_nav_entity.c_str(), 0, basis[0], basis[1], basis[2], false, error))
            s.last_error = error;
        physics_session_zero_retail_forces();
    }
    frame.ball_type = ball_name.empty() ? 0 : static_cast<uint8_t>(db_.get_ball_id(ball_name));
    frame.flags = static_cast<uint8_t>((s.own_physicalized ? bmmo::session::INPUT_FLAG_PHYSICALIZED : 0)
                | (muted ? bmmo::session::INPUT_FLAG_PAUSED : 0)
                | (!muted && ball_nav_active_ ? bmmo::session::INPUT_FLAG_NAV_ACTIVE : 0));
    {
        const uint8_t nav_mask = bmmo::session::INPUT_FLAG_NAV_ACTIVE;
        if (s.trace && (frame.keys != s.last_input_keys || (frame.flags & nav_mask) != (s.last_input_flags & nav_mask))) {
            s.exact_log_frames = 24;   // debug: exact dumps around an input edge
            logger_->Info("Physics session: input edge at tick %u keys=%u flags=%u cam=%a,%a,%a|%a,%a,%a", tick, frame.keys,
                          frame.flags, static_cast<double>(frame.cam_right[0]), static_cast<double>(frame.cam_right[1]),
                          static_cast<double>(frame.cam_right[2]), static_cast<double>(frame.cam_dir[0]),
                          static_cast<double>(frame.cam_dir[1]), static_cast<double>(frame.cam_dir[2]));
        }
        s.last_input_keys = frame.keys;
        s.last_input_flags = frame.flags;
    }
    // The black box: our own frame for this tick, exactly as it is about to go
    // to the server and into the rollback history.
    bmmo::session::client_journal::instance().own_input(tick, frame);
    s.input_history.emplace_back(tick, frame);
    if (s.rollback_enabled) {
        s.own_inputs[tick] = frame;
        while (s.own_inputs.size() > physics_session_state::kInputRing) s.own_inputs.erase(s.own_inputs.begin());
        bmmo::session::rollback_tracked tracked;
        std::map<std::string, bmmo::session::input_frame> applied;
        if (s.own_physicalized && s.own_navigation) {
            tracked.own_entity = s.own_nav_entity;
            tracked.own_polls = true;
            applied[s.own_nav_entity] = frame;
        }
        for (const auto& [id, remote]: s.remotes)
            if (remote.physicalized && remote.navigation) {
                tracked.remote_entities.push_back(remote.entity);
                applied[remote.entity] = remote.applied;
            }
        s.rollback.record(physics_session_rollback_world(), tick, tracked, applied);
    }
    if (s.assigned) {
        while (s.input_history.size() > physics_session_state::kInputHistory) s.input_history.pop_front();
        bmmo::session_input_msg msg;
        msg.session = s.session;
        msg.first_tick = s.input_history.front().first;
        for (const auto& [t, f]: s.input_history) msg.frames.push_back(f);
        msg.serialize();
        send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_UnreliableNoDelay);
        ++s.inputs_sent;
    } else if (s.input_history.size() > 660) {
        s.input_history.pop_front();
    }

    const bool exact_window = s.trace && (tick >= 4 && tick <= 12);
    if (s.trace) {
        bmmo::physics::world_hash probe;
        if (physics_view_.capture(probe, error)) {
            if (s.rng_last_pdelta == 0.0f && probe.physics_delta_time > 0.0f) s.exact_log_frames = 12;   // physics resumed
            s.rng_last_pdelta = probe.physics_delta_time;
        }
    }
    if (s.exact_log_frames > 0 || exact_window) {
        if (s.exact_log_frames > 0) --s.exact_log_frames;
        const std::string exact = physics_view_.describe_cores_exact();
        std::istringstream lines(exact);
        std::string line;
        while (std::getline(lines, line))
            logger_->Info("exact t=%u %s", tick, line.c_str());
        bmmo::physics::world_hash h;
        if (physics_view_.capture(h, error))
            logger_->Info("exact t=%u env seed=%d mc=%d cores=%d pose=%016llx time=%a psi=%a/%a pdelta=%a factor=%a", tick,
                          h.ivp_seed, static_cast<int>(h.next_movement_check), h.cores, static_cast<unsigned long long>(h.pose),
                          h.ivp_time, h.time_of_last_psi, h.time_of_next_psi, static_cast<double>(h.physics_delta_time),
                          static_cast<double>(h.time_factor));
    }
    if (s.trace) {
        bmmo::physics::world_hash h;
        if (physics_view_.capture(h, error) && (h.ivp_seed != s.rng_last_seed || h.cores != s.rng_last_cores)) {
            s.rng_last_seed = h.ivp_seed;
            s.rng_last_cores = h.cores;
            auto* time_manager = m_bml->GetTimeManager();
            logger_->Info("rng t=%u seed=%d mc=%d cores=%d cktime=%.3f dt=%.4f frames=%d movable=%s", tick, h.ivp_seed,
                          static_cast<int>(h.next_movement_check), h.cores, time_manager->GetTime(),
                          time_manager->GetLastDeltaTime(), time_manager->GetMainTickCount(),
                          physics_view_.describe_movable_objects().c_str());
        }
    }

    // Only explicit script wake-ups belong on the authoritative timeline.
    // IVP's generic revived events also include predicted collisions and
    // rollback restores: sending those back would wake the server's bodies
    // again and feed the next correction.
    s.revived_reported_this_frame.clear();
    const std::string events = physics_view_.drain_event_log();
    size_t pos = 0;
    while ((pos = events.find("script_wakeup ", pos)) != std::string::npos) {
        pos += 14;
        const size_t end = events.find(';', pos);
        if (end == std::string::npos) break;
        const std::string name = events.substr(pos, end - pos);
        pos = end + 1;
        if (name.empty() || name.rfind("Ball_", 0) == 0 || name == ball_name || name.find("_Peer_") != std::string::npos
                || s.revived_reported_this_frame.count(name))
            continue;
        s.revived_reported_this_frame.insert(name);
        bmmo::session_event_msg event;
        event.session = s.session;
        event.tick = tick;
        event.type = bmmo::session::event_type::BodyRevived;
        event.name = name;
        physics_session_send_event(event);
    }

    // A too_far or frozen decision sets the bodies and truncates the history
    // without re-simulating, so the world stays on the snapshot while our tick
    // counter runs on: the two timelines no longer line up.  Re-anchor instead
    // of drifting, once - request_resync itself rate-limits, and a request
    // already in flight will bring the full snapshot this needs.
    const uint64_t too_far_before = s.rollback.stats().too_far;
    const uint64_t frozen_before = s.rollback.stats().frozen;
    const uint64_t unmatched_before = s.rollback.stats().unmatched;
    physics_session_apply_queues();
    // Option A: after the queue was drained, every mechanism has this
    // snapshot's authority row; render it before the rollback decision below
    // (which may restore the world) and before the blends continue.
    physics_session_apply_mechanism_authority();
    if (s.rollback_enabled && !s.resync_pending) {
        const auto& rollback = s.rollback.stats();
        // The rollback path returns before the correction ladder of the
        // non-rollback path, so that ladder's resync triggers never run here.
        //
        // Design 9.2 escalates to a resync for a tick-NUMBERING desync, and
        // the one engine outcome that can mean that is `unmatched` (journal
        // kind 7): the snapshot's tick was not in our history, or a tracked
        // body was missing from it, so there was nothing to compare.  A
        // mismatch (kind 0) or a rollback (kind 1) proves the opposite - the
        // tick's record was found and every tracked body was compared, which
        // is exactly what aligned numbering looks like - so a divergence that
        // keeps being corrected, a mechanism fight or a bad spawn, must not
        // count towards the escalation.  Counting it did: 30 non-matching
        // snapshots is 60 ticks of sustained divergence, and every one of them
        // wiped the history, the inputs and the ball's correction state.
        const uint64_t unmatched = rollback.unmatched - unmatched_before;
        if (unmatched == 0) s.consecutive_unmatched = 0;
        else s.consecutive_unmatched += static_cast<int>(unmatched);
        if (rollback.too_far != too_far_before)
            physics_session_request_resync("rollback lag beyond the re-simulation window");
        else if (rollback.frozen != frozen_before)
            physics_session_request_resync("local physics clock frozen");
        else if (s.consecutive_unmatched >= 30)
            physics_session_request_resync("30 snapshots with no record of their tick");
    }
    // Input starvation (design 9.2 follow-up): separate from the tick-numbering
    // desync above, and the one failure that hides behind a healthy session.
    physics_session_check_input_starvation();

    // Continue running blends (own ball and remote balls).
    auto apply_blend = [&](const std::string& name, bmmo::session::body_corrector& corrector) {
        if (!corrector.blending()) return;
        const auto step = corrector.next_blend();
        if (step.action != bmmo::session::correction_step::kind::blend) return;
        bmmo_physics_body_state current{};
        if (!physics_view_.get_body_state(name.c_str(), current, error)) return;
        double position[3];
        float linear[3];
        for (int k = 0; k < 3; ++k) {
            position[k] = current.position[k] + step.delta_position[k];
            linear[k] = current.linear[k] + step.delta_linear[k];
        }
        if (physics_view_.set_body_state(name.c_str(), position, current.rotation, linear, current.angular, true, error))
            ++s.body_writes;
        else { ++s.body_write_errors; s.last_error = error; }
    };
    if (s.own_physicalized) apply_blend(ball_name, s.corrector);
    for (auto& [id, remote]: s.remotes)
        if (remote.physicalized && remote.navigation) apply_blend(remote.entity, remote.corrector);
    physics_session_drive_remotes();

    // The record for this tick was captured before the snapshot corrections
    // above, so a rollback to it would restore the divergent pose and lose the
    // correction on the first re-simulated tick.  Re-capture it now, keeping
    // its tick label, tracked set and inputs: this is the state the next
    // step will start from, the same one the journal hash below describes.
    if (s.rollback_enabled && !s.rollback.amend_record(physics_session_rollback_world(), tick)) {
        // False means this tick is no longer in the history - invalidate_history
        // or the history_ticks trim dropped it - so the record still holds the
        // pre-correction pose.  Log it once per session instead of every frame.
        if (s.amend_failures++ == 0)
            logger_->Warn("Physics session %u: amend_record found no history entry for tick %u (the tick was dropped)",
                          s.session, tick);
    }

    // The black box, last thing in the frame: the fingerprint of our own world
    // as the next step will find it (every correction applied above included),
    // and the bodies themselves every 10 s or after a correction moved them.
    auto& journal = bmmo::session::client_journal::instance();
    if (journal.recording()) {
        bmmo::physics::world_hash hash;
        if (physics_view_.capture(hash, error)) journal.tick(tick, hash);
        if (journal.checkpoint_due(tick)) journal.local_checkpoint(tick, physics_view_.list_bodies());
    }
}

void BallanceMMOClient::physics_session_send_event(bmmo::session_event_msg& event) {
    auto& s = physics_session_;
    event.session = s.session;
    event.serialize();
    send(event.raw.str().data(), event.size(), k_nSteamNetworkingSend_Reliable);
    ++s.events_sent;
    // The black box: our own events carry player 0 on the wire; in the file
    // they are ours by id, like the relayed ones are their sender's.
    bmmo::session::client_journal::instance().event(to_journal_event(event, db_.get_client_id()));
}

// Queued snapshots and relayed lifecycle events, in arrival order.
void BallanceMMOClient::physics_session_apply_queues() {
    auto& s = physics_session_;
    std::deque<bmmo::session_snapshot_msg> snapshots;
    std::deque<bmmo::session_event_msg> events;
    std::deque<bmmo::session_remote_input_msg> inputs;
    {
        std::lock_guard lk(s.queue_mutex);
        snapshots.swap(s.snapshot_queue);
        events.swap(s.event_queue);
        inputs.swap(s.remote_input_queue);
    }
    // The freshest authority rows are in this frame's snapshots: cache them
    // before the events run so a Physicalize starts its mirror on the row the
    // rollback is about to compare it against, not on last frame's.
    for (const auto& snapshot: snapshots)
        for (const auto& body: snapshot.bodies)
            if (body.kind == bmmo::session::body_kind::Ball)
                physics_session_cache_ball_row(snapshot.tick, body);
    for (auto& event: events) physics_session_apply_event(event);
    auto& journal = bmmo::session::client_journal::instance();
    for (const auto& msg: inputs) {
        if (msg.session != s.session) continue;
        for (const auto& entry: msg.entries) {
            // The black box: what the server says it applied for that player at
            // that tick, whether or not we mirror them here.  A frame for a
            // player the roster never had is how this client learns of a late
            // join, and the file needs that player before its inputs.
            if (journal.needs_player(entry.player)) {
                const auto state = db_.get(entry.player);
                journal.late_player(msg.tick, entry.player, mirror_join_order(s, entry.player),
                                    state ? state->name : std::string{});
            }
            journal.relayed_input(msg.tick, entry.player, entry.frame);
            auto it = s.remotes.find(entry.player);
            if (it == s.remotes.end()) continue;
            auto& remote = it->second;
            // A reordered frame is still authoritative for its historical
            // tick. Keep it for rollback without replacing the live predictor
            // with an older input.
            remote.inputs[msg.tick] = entry.frame;
            while (remote.inputs.size() > physics_session_state::kInputRing) remote.inputs.erase(remote.inputs.begin());
            if (remote.have_input && msg.tick < remote.input_tick) continue;   // out of order
            remote.input = entry.frame;
            remote.input_tick = msg.tick;
            remote.have_input = true;
        }
    }
    for (auto& snapshot: snapshots) physics_session_apply_snapshot(snapshot);
}

void BallanceMMOClient::physics_session_apply_event(const bmmo::session_event_msg& event) {
    auto& s = physics_session_;
    if (event.session != s.session) return;
    if (event.player == 0 || event.player == db_.get_client_id()) return;
    // The black box: a relayed event of another member (ours went in when it
    // was sent, so an echo of it must not be written twice).  A member the
    // roster never had gets its PLAYER record first, in the same tick group:
    // a replay builds its member list from those records and would drop this
    // event as "unknown player" without one.
    auto& journal = bmmo::session::client_journal::instance();
    if (journal.needs_player(event.player)) {
        const auto state = db_.get(event.player);
        journal.late_player(event.tick, event.player, mirror_join_order(s, event.player),
                            state ? state->name : std::string{});
    }
    journal.event(to_journal_event(event, event.player));
    std::string error;
    switch (event.type) {
    case bmmo::session::event_type::Physicalize: {
        objects_.ensure_player(event.player);
        CK3dObject* entity = objects_.get_ball_entity(event.player, event.ball_type);
        if (!entity) {
            logger_->Warn("Physics session: no spirit ball for player %u type %u", event.player, event.ball_type);
            return;
        }
        auto& remote = s.remotes[event.player];
        // A trafo: the peer's ball is a different entity now, so the mirror of
        // the old one has to go (body, navigation and the relayed inputs that
        // were meant for it) and its spirit ball has to be hidden.
        const uint32_t previous_type = remote.physicalized && remote.entity != entity->GetName()
            ? remote.ball_type : std::numeric_limits<uint32_t>::max();
        if (remote.physicalized && remote.entity != entity->GetName()) {
            s.rollback.invalidate_history();
            if (remote.navigation) physics_view_.navigation_destroy(remote.entity.c_str(), error);
            physics_view_.unphysicalize(remote.entity.c_str(), error);
        } else if (remote.navigation) {
            physics_view_.navigation_destroy(remote.entity.c_str(), error);
        }
        remote.navigation = false;
        // The event carries the pose from the tick it was stamped for; the rows
        // for that player are newer than that by the relay delay and are what
        // the mirror has to start from (9.17 spawn twitch).
        const auto cached = s.latest_ball_rows.find(event.player);
        const bool have_cached = cached != s.latest_ball_rows.end() && cached->second.have
            && cached->second.tick > event.tick;
        if (have_cached)
            entity->SetWorldMatrix(matrix_from_quaternion(cached->second.position, cached->second.rotation));
        else
            entity->SetWorldMatrix(matrix_from_pose(event.position, event.rotation));
        const auto recipe = to_bridge_recipe(event.recipe);
        int join_order = 63;
        for (const auto& p: s.players) if (p.id == event.player) join_order = p.join_order;
        const std::string group = "P#" + std::to_string(join_order);
        if (!physics_view_.physicalize(entity->GetName(), recipe, group.c_str(), error)) {
            logger_->Warn("Physics session: physicalize %s: %s", entity->GetName(), error.c_str());
            s.last_error = error;
            return;
        }
        if (have_cached) {
            // Start the mirror on the server's row instead of the spawn pose:
            // position, rotation and the velocity the server already gave it.
            if (!physics_view_.set_body_state(entity->GetName(), cached->second.position, cached->second.rotation,
                                              cached->second.linear, cached->second.angular,
                                              cached->second.simulated, error))
                logger_->Warn("Physics session: physicalize %s: %s", entity->GetName(), error.c_str());
        }
        // Queues run after this frame's history capture. The new body (also a
        // same-name respawn) must first be recorded on the NEXT live frame;
        // older snapshots cannot safely rewind the current world through it.
        s.rollback.invalidate_history();
        remote.entity = entity->GetName();
        remote.ball_type = event.ball_type;
        remote.physicalized = true;
        remote.corrector.clear();
        // Design 9.10: the same spawn kick as our own ball, applied at once -
        // the body exists here already.  Skipped when the row above already
        // carries the server's kick: adding it again would double the speed.
        if (!have_cached && (event.flags & bmmo::session::PHYSICALIZE_FLAG_SPAWN) && s.spawn_impulse > 0.0f) {
            const uint32_t index = bmmo::session::spawn_direction_index(s.seed, static_cast<uint8_t>(join_order), event.tick);
            std::string impulse_error;
            if (!physics_view_.push_impulse(entity->GetName(), bmmo::session::kSpawnDirectionTable[index],
                                            s.spawn_impulse, 0, impulse_error))
                logger_->Warn("Physics session: spawn impulse for %s: %s", entity->GetName(), impulse_error.c_str());
        }
        if (previous_type != std::numeric_limits<uint32_t>::max()) {
            // Inputs relayed for the old entity must not drive the new one.
            remote.inputs.clear();
            remote.have_input = false;
            remote.applied = {};
        }
        objects_.set_physicalized(event.player, true);
        objects_.on_trafo(event.player, previous_type, event.ball_type);
        // Design 9.1: drive the mirror with the retail navigation replica from
        // the relayed inputs; the snapshots then only correct it.
        physics_session_attach_remote_navigation(event.player);
        break;
    }
    case bmmo::session::event_type::Unphysicalize: {
        auto it = s.remotes.find(event.player);
        if (it == s.remotes.end()) return;
        if (it->second.physicalized || it->second.navigation) s.rollback.invalidate_history();
        if (it->second.navigation) physics_view_.navigation_destroy(it->second.entity.c_str(), error);
        it->second.navigation = false;
        if (it->second.physicalized) physics_view_.unphysicalize(it->second.entity.c_str(), error);
        it->second.physicalized = false;
        objects_.set_physicalized(event.player, false);
        break;
    }
    default:
        break;   // Sector / Finish / BodyRevived of others: server-side only
    }
}

void BallanceMMOClient::physics_session_apply_snapshot(const bmmo::session_snapshot_msg& snapshot) {
    auto& s = physics_session_;
    if (snapshot.session != s.session) return;
    // The black box gets the snapshot as it arrived, before the staleness and
    // resync filters below decide what to do with it: those decisions are what
    // an offline replay has to be able to second-guess.
    bmmo::session::client_journal::instance().received_snapshot(snapshot);
    if (s.have_snapshot && snapshot.tick <= s.last_snapshot_tick && !snapshot.full) {
        ++s.snapshots_stale;
        return;
    }
    // The snapshot stream is alive as of this frame: the starvation detector
    // needs to know that a silent acked tick is a numbering hole and not a
    // stream that stopped (a network outage, a different failure entirely).
    s.acked_input.snapshot_frame = s.frames_since_anchor;
    for (const auto& body: snapshot.bodies)
        if (body.kind == bmmo::session::body_kind::Ball)
            physics_session_cache_ball_row(snapshot.tick, body);
    if (s.resync_pending) {
        if (!snapshot.full) return;   // wait for the full snapshot the server forced
        s.have_snapshot = true;
        s.last_snapshot_tick = snapshot.tick;
        ++s.snapshots_applied;
        physics_session_apply_resync(snapshot);
        s.resync_pending = false;
        ++s.resyncs_done;
        return;
    }
    s.have_snapshot = true;
    s.last_snapshot_tick = std::max(s.last_snapshot_tick, snapshot.tick);
    ++s.snapshots_applied;
    // Did the server have our input for the tick it just simulated, or did it
    // fall back on the last one it had?  The reported counters are file-scope
    // so the mod class's layout does not move (see the F3 panel commit); the
    // starvation detector's own state is a session member, because it has to be
    // dropped together with the numbering it was measured under.
    if (s.assigned) {
        ++input_freshness.reports;
        input_freshness.last_acked = snapshot.acked_input_tick;
        if (snapshot.acked_input_tick < snapshot.tick) ++input_freshness.starved;
        const uint32_t lag = s.current_tick() > snapshot.tick ? s.current_tick() - snapshot.tick : 0;
        input_freshness.lag_sum += lag;
        // Input starvation detector: acked_input_tick is the server's read
        // cursor for us.  The first one of a numbering is the baseline, and
        // every later change moves it; the frame it last moved at is what
        // physics_session_check_input_starvation measures against.
        if (!s.acked_input.have || snapshot.acked_input_tick != s.acked_input.tick) {
            s.acked_input.have = true;
            s.acked_input.tick = snapshot.acked_input_tick;
            s.acked_input.frame = s.frames_since_anchor;
        }
    }
    const auto own_id = db_.get_client_id();
    CK3dObject* ball = get_current_ball();
    const std::string ball_name = ball && ball->GetName() ? ball->GetName() : "";
    std::string error;
    physics_session_check_own_body(snapshot, own_id);
    if (s.rollback_enabled) {
        for (const auto& body: snapshot.bodies)
            if (body.kind == bmmo::session::body_kind::Mechanism) {
                if (snapshot.full && !body.name.empty()) s.mechanism_names[body.owner] = body.name;
                // Option A: the row is not compared or restored, it is stored
                // as authority for the frame's applier.
                physics_session_note_mechanism(snapshot.tick, body);
            }
        physics_session_rollback(snapshot);
        return;
    }
    for (const auto& body: snapshot.bodies) {
        if (body.kind == bmmo::session::body_kind::Ball) {
            if (body.owner == own_id) {
                if (!s.own_physicalized || ball_name.empty()) continue;
                bmmo::session::ball_pose pose;
                pose.tick = snapshot.tick;
                for (int k = 0; k < 3; ++k) {
                    pose.position[k] = body.position[k];
                    pose.linear[k] = body.linear[k];
                    pose.angular[k] = body.angular[k];
                }
                for (int k = 0; k < 4; ++k) pose.rotation[k] = body.rotation[k];
                const auto step = s.corrector.compare(pose);
                {
                    const auto& st = s.corrector.stats();
                    if (st.unmatched != s.last_unmatched) {
                        s.last_unmatched = st.unmatched;
                        ++s.consecutive_unmatched;
                    } else if (step.action != bmmo::session::correction_step::kind::none) {
                        s.consecutive_unmatched = 0;
                    } else {
                        s.consecutive_unmatched = 0;
                    }
                    if (step.action == bmmo::session::correction_step::kind::hard) ++s.consecutive_hard;
                    else if (step.action == bmmo::session::correction_step::kind::none && st.unmatched == s.last_unmatched)
                        s.consecutive_hard = 0;
                    if (s.consecutive_hard >= 3) physics_session_request_resync("3 hard corrections in a row");
                    else if (s.consecutive_unmatched >= 30) physics_session_request_resync("30 unmatched snapshots");
                }
                if (s.trace && s.corrector.stats().last_error > 0.002 && s.corrections_logged < 200) {
                    ++s.corrections_logged;
                    logger_->Info("Physics session: own ball error %.4f m at tick %u (server pos %.4f,%.4f,%.4f v %.4f,%.4f,%.4f)",
                                  s.corrector.stats().last_error, snapshot.tick, pose.position[0], pose.position[1],
                                  pose.position[2], static_cast<double>(pose.linear[0]), static_cast<double>(pose.linear[1]),
                                  static_cast<double>(pose.linear[2]));
                }
                if (step.action == bmmo::session::correction_step::kind::hard) {
                    ++s.hard_sets;
                    physics_session_log_correction(ball_name, snapshot.tick, s.corrector.stats().last_error, "hard");
                    if (physics_view_.set_body_state(ball_name.c_str(), step.target.position, step.target.rotation,
                                                     step.target.linear, step.target.angular, true, error))
                        ++s.body_writes;
                    else { ++s.body_write_errors; s.last_error = error; }
                } else if (step.action == bmmo::session::correction_step::kind::blend) {
                    ++s.blends;
                    physics_session_log_correction(ball_name, snapshot.tick, s.corrector.stats().last_error, "blend");
                }
                continue;
            }
            auto it = s.remotes.find(body.owner);
            if (it == s.remotes.end() || !it->second.physicalized) continue;
            const bool wake = (body.flags & bmmo::session::BODY_FLAG_SIMULATED) != 0;
            auto& remote = it->second;
            if (remote.navigation) {
                bmmo::session::ball_pose pose;
                pose.tick = snapshot.tick;
                for (int k = 0; k < 3; ++k) {
                    pose.position[k] = body.position[k];
                    pose.linear[k] = body.linear[k];
                    pose.angular[k] = body.angular[k];
                }
                for (int k = 0; k < 4; ++k) pose.rotation[k] = body.rotation[k];
                const auto step = remote.corrector.compare(pose);
                if (step.action == bmmo::session::correction_step::kind::hard) {
                    ++remote.hard_sets;
                    physics_session_log_correction(remote.entity, snapshot.tick, remote.corrector.stats().last_error, "hard");
                    if (physics_view_.set_body_state(remote.entity.c_str(), step.target.position, step.target.rotation,
                                                     step.target.linear, step.target.angular, wake, error))
                        ++s.body_writes;
                    else { ++s.body_write_errors; s.last_error = error; }
                } else if (step.action == bmmo::session::correction_step::kind::blend) {
                    ++remote.blends;
                    physics_session_log_correction(remote.entity, snapshot.tick, remote.corrector.stats().last_error, "blend");
                }
                continue;
            }
            if (physics_view_.set_body_state(remote.entity.c_str(), body.position, body.rotation, body.linear,
                                             body.angular, wake, error))
                ++s.body_writes;
            else { ++s.body_write_errors; s.last_error = error; }
            continue;
        }
        // Mechanism (Option A): no local prediction, no comparison - the row
        // is stored as authority and rendered by the frame's applier.
        if (snapshot.full && !body.name.empty()) s.mechanism_names[body.owner] = body.name;
        physics_session_note_mechanism(snapshot.tick, body);
    }
}

// Every snapshot carries a ball row for each physicalized player, awake or
// not.  If ours is missing while our ball is physicalized here, the server has
// no body for us: its copy of the last Physicalize never arrived or was
// rejected, and no retail script will ever report it again (the ball would
// stay out of the simulation and stop touching the mechanisms until death).
// Report it once more, with the pose the ball has now.
void BallanceMMOClient::physics_session_check_own_body(const bmmo::session_snapshot_msg& snapshot, uint32_t own_id) {
    auto& s = physics_session_;
    bool present = false;
    for (const auto& body: snapshot.bodies)
        if (body.kind == bmmo::session::body_kind::Ball && body.owner == own_id) { present = true; break; }
    if (present || !s.own_physicalized || !s.last_physicalize.valid || s.resync_pending) {
        s.snapshots_without_own = 0;
        return;
    }
    // About a second of snapshots: well past the server's lag behind us.
    if (++s.snapshots_without_own < 30) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - s.last_physicalize_resend < std::chrono::seconds(2)) return;
    CK3dObject* ball = get_current_ball();
    if (!ball) return;
    s.last_physicalize_resend = now;
    s.snapshots_without_own = 0;
    ++s.physicalize_resends;

    bmmo::session_event_msg event;
    event.tick = s.current_tick() + 1;
    event.type = bmmo::session::event_type::Physicalize;
    event.ball_type = s.last_physicalize.ball_type;
    // The spawn bit is what makes a Physicalize kick the ball, and this one is
    // a resend of a body that was already kicked (or deliberately was not):
    // leaving the bit set had the server kick the re-created body while the
    // client, correctly, did not.  Everything else about the event is resent
    // unchanged.
    event.flags = static_cast<uint8_t>(s.last_physicalize.flags & ~bmmo::session::PHYSICALIZE_FLAG_SPAWN);
    const VxMatrix& world = ball->GetWorldMatrix();
    for (int k = 0; k < 3; ++k) event.position[k] = world[3][k];
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) event.rotation[r * 3 + k] = world[r][k];
    event.recipe = s.last_physicalize.recipe;
    physics_session_send_event(event);
    logger_->Warn("Physics session: the server has no body for our ball (%llu snapshots); Physicalize re-reported at tick %u",
                  static_cast<unsigned long long>(s.physicalize_resends), event.tick);
    if (s.physicalize_resends == 1)
        SendIngameMessage("Physics session: the server did not take our ball; reporting it again.", bmmo::ansi::BrightYellow);
}

// ---------------------------------------------------------------- BML hooks

void BallanceMMOClient::OnPhysicalize(CK3dEntity* target, CKBOOL fixed, float friction, float elasticity, float mass,
                                      const char* collGroup, CKBOOL startFrozen, CKBOOL enableColl, CKBOOL calcMassCenter,
                                      float linearDamp, float rotDamp, const char* collSurface, VxVector massCenter,
                                      int convexCnt, CKMesh** convexMesh, int ballCnt, VxVector* ballCenter,
                                      float* ballRadius, int concaveCnt, CKMesh** concaveMesh) {
    auto& s = physics_session_;
    if (s.phase != phase_type::running || !target) return;
    CK3dObject* ball = get_current_ball();
    if (!ball || target != static_cast<CK3dEntity*>(ball)) {
        // retail Physicalize of a level object during the session (the
        // sector reset after a death re-runs it; the body guard keeps the
        // existing body, engine change #6)
        if (s.trace)
            logger_->Info("Physics session: retail physicalize of %s at tick %u (%d convex, %d balls, %d concave)",
                          target->GetName() ? target->GetName() : "?", s.current_tick() + 1, convexCnt, ballCnt, concaveCnt);
        return;
    }
    // BML calls this before body creation/PreSimulate. Discard old lifetimes
    // now; this frame's post-physics record captures the newly created body.
    // In particular, a same-name respawn is not the body in the old history.
    s.rollback.invalidate_history();
    bmmo::session_event_msg event;
    event.tick = s.current_tick() + 1;   // this frame's physics step is still ahead
    event.type = bmmo::session::event_type::Physicalize;
    event.ball_type = static_cast<uint8_t>(db_.get_ball_id(ball->GetName() ? ball->GetName() : ""));
    const VxMatrix& world = target->GetWorldMatrix();
    for (int k = 0; k < 3; ++k) event.position[k] = world[3][k];
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) event.rotation[r * 3 + k] = world[r][k];
    // Design 9.10: a Physicalize at the level's current resetpoint is a spawn
    // or a respawn (a trafo never happens there), flagged so every side
    // - including this one's own kick below - applies the same impulse.
    bool spawn = false;
    if (CKDataArray* level = current_level_array()) {
        VxMatrix reset;
        if (level->GetElementValue(0, 3, &reset)) {
            const float dx = event.position[0] - reset[3][0];
            const float dy = event.position[1] - reset[3][1];
            const float dz = event.position[2] - reset[3][2];
            spawn = (dx * dx + dy * dy + dz * dz) < 1e-6f;
        }
    }
    if (spawn) event.flags |= bmmo::session::PHYSICALIZE_FLAG_SPAWN;
    auto& r = event.recipe;
    r.fixed = fixed != 0;
    r.friction = friction;
    r.elasticity = elasticity;
    r.mass = mass;
    r.start_frozen = startFrozen != 0;
    r.enable_collision = enableColl != 0;
    r.calc_mass_center = calcMassCenter != 0;
    r.linear_damp = linearDamp;
    r.rot_damp = rotDamp;
    r.mass_center[0] = massCenter.x; r.mass_center[1] = massCenter.y; r.mass_center[2] = massCenter.z;
    r.collision_surface = collSurface ? collSurface : "";
    for (int i = 0; i < convexCnt && convexMesh; ++i)
        r.convex_meshes.push_back(convexMesh[i] && convexMesh[i]->GetName() ? convexMesh[i]->GetName() : "");
    for (int i = 0; i < ballCnt; ++i) {
        bmmo::session::ball_recipe::sphere sphere;
        if (ballCenter) { sphere.center[0] = ballCenter[i].x; sphere.center[1] = ballCenter[i].y; sphere.center[2] = ballCenter[i].z; }
        sphere.radius = ballRadius ? ballRadius[i] : 0.0f;
        r.balls.push_back(sphere);
    }
    for (int i = 0; i < concaveCnt && concaveMesh; ++i)
        r.concave_meshes.push_back(concaveMesh[i] && concaveMesh[i]->GetName() ? concaveMesh[i]->GetName() : "");
    (void)collGroup;
    physics_session_send_event(event);
    // Keep it: a server that never applied this event has to be told again
    // (physics_session_check_own_body), same flags, no second impulse.
    s.last_physicalize.valid = true;
    s.last_physicalize.ball_type = event.ball_type;
    s.last_physicalize.flags = event.flags;
    for (int k = 0; k < 3; ++k) s.last_physicalize.position[k] = event.position[k];
    for (int k = 0; k < 9; ++k) s.last_physicalize.rotation[k] = event.rotation[k];
    s.last_physicalize.recipe = event.recipe;
    s.snapshots_without_own = 0;
    s.own_group_set = false;
    if (spawn && s.spawn_impulse > 0.0f) {
        // BMLPlus broadcasts OnPhysicalize before the block creates the body,
        // so the bridge queues this into the frame's PreSimulate pass,
        // anchored to a live behavior (the Ball Navigation script).
        uint32_t behavior_id = s.navigation.ball_navigation;
        if (behavior_id == 0) behavior_id = bmmo::game::read_navigation_graph(m_bml->GetCKContext()).ball_navigation;
        const uint8_t join_order = static_cast<uint8_t>(s.own_join_order < 0 ? 63 : s.own_join_order);
        const uint32_t index = bmmo::session::spawn_direction_index(s.seed, join_order, event.tick);
        std::string impulse_error;
        if (physics_view_.push_impulse(ball->GetName(), bmmo::session::kSpawnDirectionTable[index], s.spawn_impulse,
                                       behavior_id, impulse_error))
            logger_->Info("Physics session: spawn impulse index=%u speed=%.3f at tick %u", index, s.spawn_impulse,
                          event.tick);
        else
            logger_->Warn("Physics session: spawn impulse: %s", impulse_error.c_str());
    }
    // Design 9.6: the replica drives the ball, the retail leaves push zero.
    // The replica only moves to the new ball in the next frame, and a
    // SetPhysicsForce.Create that ran while the ball had no body is retried at
    // this frame's PreSimulate: zero the leaves now, or the trafo frame gets
    // the retail force on top of the replica's.
    if (s.own_navigation) physics_session_zero_retail_forces();
    if (s.trace) s.exact_log_frames = 12;
    logger_->Info("Physics session: own ball %s physicalized at tick %u (type %u, %d convex, %d balls, %d concave, "
                  "friction %.4f elasticity %.4f mass %.4f linear damp %.4f rot damp %.4f surface '%s') "
                  "pos=%a,%a,%a rows=%a,%a,%a|%a,%a,%a|%a,%a,%a",
                  ball->GetName(), event.tick, event.ball_type, convexCnt, ballCnt, concaveCnt,
                  friction, elasticity, mass, linearDamp, rotDamp, r.collision_surface.c_str(),
                  event.position[0], event.position[1], event.position[2],
                  event.rotation[0], event.rotation[1], event.rotation[2], event.rotation[3], event.rotation[4],
                  event.rotation[5], event.rotation[6], event.rotation[7], event.rotation[8]);
}

void BallanceMMOClient::OnUnphysicalize(CK3dEntity* target) {
    auto& s = physics_session_;
    if (s.phase != phase_type::running || !target) return;
    CK3dObject* ball = get_current_ball();
    if (!ball || target != static_cast<CK3dEntity*>(ball)) return;
    s.rollback.invalidate_history();
    bmmo::session_event_msg event;
    event.tick = s.current_tick() + 1;
    event.type = bmmo::session::event_type::Unphysicalize;
    physics_session_send_event(event);
    s.corrector.clear();
    s.last_physicalize.valid = false;   // the ball is meant to be gone now
    s.snapshots_without_own = 0;
    s.own_group_set = false;
    logger_->Info("Physics session: own ball unphysicalized at tick %u", event.tick);
}

void BallanceMMOClient::physics_session_on_sector(int sector) {
    auto& s = physics_session_;
    if (s.phase != phase_type::running) return;
    bmmo::session_event_msg event;
    event.tick = s.current_tick();
    event.type = bmmo::session::event_type::Sector;
    event.sector = sector;
    physics_session_send_event(event);
}

void BallanceMMOClient::physics_session_on_finish() {
    auto& s = physics_session_;
    if (s.phase != phase_type::running) return;
    bmmo::session_event_msg event;
    event.tick = s.current_tick();
    event.type = bmmo::session::event_type::Finish;
    physics_session_send_event(event);
}

void BallanceMMOClient::physics_session_log_correction(const std::string& name, uint32_t tick, double error, const char* action) {
    auto& s = physics_session_;
    // The black box records every one of these, not only the first 200 the log
    // gets (kind 2 hard, 3 blend; the rollback engine reports its own).
    auto& journal = bmmo::session::client_journal::instance();
    if (journal.recording()) {
        bmmo::session::journal_correction record;
        record.tick = tick;
        record.local_tick = s.current_tick();
        record.kind = std::strcmp(action, "hard") == 0 ? 2 : 3;
        record.entity = name;
        record.error_m = static_cast<float>(error);
        journal.correction(record);
        if (record.kind == 2) journal.request_checkpoint();   // a hard set moved a body
    }
    if (++s.corrections_logged > 200) return;   // enough to diagnose, not enough to flood
    logger_->Info("Physics session: %s correction of %s for tick %u (error %.4f m, local tick %u)",
                  action, name.c_str(), tick, error, s.current_tick());
}

// Design 9.6: the world adapter of the rollback engine over the physics bridge.
bmmo::session::rollback_world BallanceMMOClient::physics_session_rollback_world() {
    bmmo::session::rollback_world world;
    world.get_body = [this](const std::string& entity, bmmo_physics_body_state& out) {
        std::string error;
        return physics_view_.get_body_state(entity.c_str(), out, error);
    };
    world.set_body = [this](const std::string& entity, const bmmo_physics_body_state& state, bool wake) {
        std::string error;
        if (physics_view_.set_body_state(entity.c_str(), state.position, state.rotation, state.linear, state.angular, wake, error))
            return true;
        physics_session_.last_error = error;
        return false;
    };
    world.get_nav = [this](const std::string& entity, bmmo_physics_nav_state& out) {
        std::string error;
        return physics_view_.navigation_get_state(entity.c_str(), out, error);
    };
    world.set_nav = [this](const std::string& entity, const bmmo_physics_nav_state& state) {
        std::string error;
        return physics_view_.navigation_set_state(entity.c_str(), state, error);
    };
    world.nav_input = [this](const std::string& entity, const bmmo::session::input_frame& frame) {
        std::string error;
        const bool active = (frame.flags & bmmo::session::INPUT_FLAG_NAV_ACTIVE) != 0;
        return physics_view_.navigation_input(entity.c_str(), frame.keys, frame.cam_right, frame.cam_up, frame.cam_dir, active, error);
    };
    world.nav_poll = [this](const std::string& entity, bool enable) {
        auto& s = physics_session_;
        std::string error;
        // A replay must not switch the polling back on while the pause menu
        // has it off: the ball would follow the held arrow keys again.
        return physics_view_.navigation_poll(entity.c_str(), enable && !s.input_muted, s.own_key_codes,
                                            s.own_key_blocks, s.own_key_count, error);
    };
    world.step = [this]() {
        std::string error;
        return physics_view_.step_physics(1000.0f / 66.0f, error);
    };
    // Option A: the mechanisms are not in the tracked set, so the re-simulation
    // would replay the ball against the one pose the frame's applier left them
    // at - the applier runs once per frame, not once per replayed tick.
    world.pre_step = [this](uint32_t tick) { physics_session_pose_mechanisms(tick); };
    world.simulating = [this]() {
        float factor = 0.0f, delta = 0.0f;
        std::string error;
        return !physics_view_.get_clock(factor, delta, error) || (factor > 0.0f && delta > 0.0f);
    };
    world.log = [this](const std::string& text) {
        auto& s = physics_session_;
        if (++s.corrections_logged <= 200) logger_->Info("Physics session: %s", text.c_str());
    };
    // The black box: every decision the engine takes about a snapshot (kinds 0
    // mismatch, 1 rollback, 5 too far, 6 frozen, 7 unmatched), structured, so
    // the timeline does not have to be parsed back out of the log lines.
    world.on_correction = [](const bmmo::session::rollback_correction& c) {
        auto& journal = bmmo::session::client_journal::instance();
        if (!journal.recording()) return;
        bmmo::session::journal_correction record;
        record.tick = c.tick;
        record.local_tick = c.local_tick;
        record.kind = c.kind;
        record.entity = c.entity;
        record.error_m = static_cast<float>(c.error_m);
        record.velocity_error = static_cast<float>(c.velocity_error);
        for (int k = 0; k < 3; ++k) {
            record.local_position[k] = c.local_position[k];
            record.server_position[k] = c.server_position[k];
        }
        journal.correction(record);
        if (c.kind == 1) journal.request_checkpoint();   // a rollback re-simulated every body
    };
    return world;
}

// One authoritative snapshot through the rollback engine (design 9.6).
bool BallanceMMOClient::physics_session_rollback(const bmmo::session_snapshot_msg& snapshot) {
    auto& s = physics_session_;
    const auto own_id = db_.get_client_id();
    auto entity_of = [&](const bmmo::session::body_state& body) -> std::string {
        if (body.kind == bmmo::session::body_kind::Ball) {
            if (body.owner == own_id) return s.own_physicalized && s.own_navigation ? s.own_nav_entity : std::string();
            auto it = s.remotes.find(body.owner);
            if (it == s.remotes.end() || !it->second.physicalized || !it->second.navigation) return {};
            return it->second.entity;
        }
        // Mechanisms are not tracked by the rollback engine (Option A): they
        // are server-authoritative, so no snapshot row may restore or
        // re-simulate them.  An empty entity makes on_snapshot skip the row
        // (rollback.hpp), and rollback_tracked no longer lists them either.
        return std::string();
    };
    // Only an exact hit counts: the engine feeds a relayed frame instead of the
    // recorded prediction when this succeeds, and a frame from an earlier tick
    // would replay a key edge the tick did not have.
    auto input_at = [&](const std::string& entity, uint32_t tick, bmmo::session::input_frame& out) {
        const std::map<uint32_t, bmmo::session::input_frame>* inputs = nullptr;
        if (entity == s.own_nav_entity) inputs = &s.own_inputs;
        else
            for (const auto& [id, remote]: s.remotes)
                if (remote.entity == entity) inputs = &remote.inputs;
        if (!inputs) return false;
        auto it = inputs->find(tick);
        if (it == inputs->end()) return false;
        out = it->second;
        return true;
    };
    const bool rolled = s.rollback.on_snapshot(physics_session_rollback_world(), snapshot, s.current_tick(), entity_of, input_at);
    if (rolled && s.own_navigation && s.previous_cam_valid) {
        // the live input for the next frame was consumed by the re-simulation
        std::string error;
        physics_view_.navigation_input(s.own_nav_entity.c_str(), 0, s.previous_cam[0], s.previous_cam[1], s.previous_cam[2],
                                       false, error);
    }
    return rolled;
}

// Input starvation (design 9.2 follow-up).  Every snapshot carries the tick the
// server last consumed a FRESH input frame from us for.  If our numbering ever
// falls behind that per-player read cursor, the server drops every later frame
// as stale and simulates our ball from the last frame it had - for the rest of
// the session, because input_buffer::reset() never moves that cursor back and
// nothing else notices: our snapshot stream keeps arriving and keeps
// correcting us, it is just correcting a ball steered by stale input (the
// failure in docs/multiplayer-rollback-retest-20260909.md).  Healing it needs
// the same re-anchor a tick-numbering desync needs, and the client is the right
// side to ask for it: it is the only member that knows both its own numbering
// and what the server acknowledged, and asking costs the server no determinism
// (design 9.2).
void BallanceMMOClient::physics_session_check_input_starvation() {
    auto& s = physics_session_;
    if (s.phase != phase_type::running || !s.assigned || s.resync_pending) return;
    // No baseline yet, or no snapshot to judge it against: nothing to fire on.
    if (!s.have_snapshot || !s.acked_input.have) return;
    // A stream that has stopped is a network outage, not a numbering hole:
    // a resync cannot bring the server back, and the reconnect path owns it.
    constexpr int64_t kSnapshotFreshFrames = 66;   // one full-snapshot cadence
    if (s.frames_since_anchor - s.acked_input.snapshot_frame > kSnapshotFreshFrames) return;
    // Two seconds (~132 frames) without the read cursor moving, while the
    // stream is alive.  acked_input_tick rides EVERY snapshot, delta ones
    // included (server.cpp on_session_snapshot fills it from the runner's acked
    // map), so a healthy numbering moves it about every other tick and two
    // seconds of silence is ~130 snapshots with no fresh frame in any of them.
    // Had it been carried on the 66-tick full snapshots only, this would have
    // to be 200 so that two consecutive fulls must both show no advance.
    constexpr int64_t kStarvedFrames = 132;
    const int64_t starved = s.frames_since_anchor - s.acked_input.frame;
    if (starved <= kStarvedFrames) return;
    logger_->Warn("Physics session %u: server has not consumed a fresh input for %lld ticks (acked %u, our tick %u); "
                  "tick numbering hole suspected", s.session, static_cast<long long>(starved), s.acked_input.tick,
                  s.current_tick());
    physics_session_request_resync("no fresh input consumed for 2 s");
    // Measure the next window from here: request_resync rate-limits itself, and
    // a request already in flight brings the re-anchor this needs.
    s.acked_input.frame = s.frames_since_anchor;
}

// Resync request (design 9.2), at most one every two seconds.
void BallanceMMOClient::physics_session_request_resync(const char* reason) {
    auto& s = physics_session_;
    if (s.phase != phase_type::running || !s.assigned) return;
    const auto now = std::chrono::steady_clock::now();
    if (s.resyncs_sent > 0 && now - s.last_resync_request < std::chrono::seconds(2)) return;
    s.last_resync_request = now;
    s.consecutive_hard = s.consecutive_unmatched = 0;
    bmmo::session_resync_msg msg;
    msg.session = s.session;
    msg.last_full_tick = s.last_snapshot_tick;
    msg.serialize();
    send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
    ++s.resyncs_sent;
    bmmo::session::client_journal::instance().note(
            s.current_tick(), std::format("resync: requested ({}) at tick {}", reason, s.current_tick()));
    logger_->Info("Physics session %u: resync requested (%s) at tick %u", s.session, reason, s.current_tick());
}

// The full snapshot after a resync: every body is written outright (no
// history to compare with yet).
void BallanceMMOClient::physics_session_apply_resync(const bmmo::session_snapshot_msg& snapshot) {
    auto& s = physics_session_;
    // A hard world overwrite also invalidates records collected while we
    // waited for this full snapshot, not just those dropped on reassignment.
    s.rollback.invalidate_history();
    std::string error;
    const auto own_id = db_.get_client_id();
    CK3dObject* ball = get_current_ball();
    const std::string ball_name = ball && ball->GetName() ? ball->GetName() : "";
    for (const auto& body: snapshot.bodies) {
        const bool wake = (body.flags & bmmo::session::BODY_FLAG_SIMULATED) != 0;
        const char* target = nullptr;
        if (body.kind == bmmo::session::body_kind::Ball) {
            if (body.owner == own_id) {
                if (!s.own_physicalized || ball_name.empty()) continue;
                target = ball_name.c_str();
            } else {
                auto it = s.remotes.find(body.owner);
                if (it == s.remotes.end() || !it->second.physicalized) continue;
                target = it->second.entity.c_str();
            }
        } else {
            if (!body.name.empty()) s.mechanism_names[body.owner] = body.name;
            auto name = s.mechanism_names.find(body.owner);
            if (name == s.mechanism_names.end()) continue;
            // Option A: the applier renders from the stored rows, so the hard
            // set below is also the newest row of the history.
            physics_session_note_mechanism(snapshot.tick, body);
            target = name->second.c_str();
        }
        if (physics_view_.set_body_state(target, body.position, body.rotation, body.linear, body.angular,
                                         body.owner == own_id ? true : wake, error))
            ++s.body_writes;
        else { ++s.body_write_errors; s.last_error = error; }
    }
    // The black box: the whole world was rebuilt from this snapshot (kind 4),
    // so the next checkpoint is worth having whatever the rate limit says.
    auto& journal = bmmo::session::client_journal::instance();
    if (journal.recording()) {
        bmmo::session::journal_correction record;
        record.tick = snapshot.tick;
        record.local_tick = s.current_tick();
        record.kind = 4;
        journal.correction(record);
        journal.note(s.current_tick(), std::format("resync: applied from tick {}", snapshot.tick));
        journal.request_checkpoint();
    }
    logger_->Info("Physics session %u: resync applied from the full snapshot of tick %u (%zu bodies)", s.session,
                  snapshot.tick, snapshot.bodies.size());
}

// Keeps the freshest ball row of every player, physicalized here or not: the
// Physicalize event for a peer arrives about an input delay after the rows
// that already show the server's spawned balls separated, and the mirror has
// to start where the server has it, not where the event's spawn pose was.
void BallanceMMOClient::physics_session_cache_ball_row(uint32_t tick, const bmmo::session::body_state& body) {
    auto& row = physics_session_.latest_ball_rows[body.owner];
    if (row.have && row.tick > tick) return;   // a queued older snapshot
    row.have = true;
    row.tick = tick;
    for (int k = 0; k < 3; ++k) {
        row.position[k] = body.position[k];
        row.linear[k] = body.linear[k];
        row.angular[k] = body.angular[k];
    }
    for (int k = 0; k < 4; ++k) row.rotation[k] = body.rotation[k];
    row.simulated = (body.flags & bmmo::session::BODY_FLAG_SIMULATED) != 0;
}

// Option A: store one authoritative mechanism row.  Rows arrive with every
// snapshot, full or not (the non-full ones carry exactly the simulated bodies,
// which are the ones that move), and are keyed by the server's owner index -
// so a row is kept even before its dictionary name is known, and two instances
// that share one name never mix.  The history is bounded (kMechanismRows): a
// re-simulation re-poses the body from these rows once per replayed tick.
void BallanceMMOClient::physics_session_note_mechanism(uint32_t tick, const bmmo::session::body_state& body) {
    auto& rows = physics_session_.mechanism_authority[body.owner].rows;
    if (!rows.empty()) {
        if (rows.back().tick > tick) return;   // a queued older snapshot
        // Equal tick: a re-sent row of the same tick replaces the newest one,
        // so the history keeps ascending unique ticks instead of two rows the
        // interpolation would see as a zero-length interval.
        if (rows.back().tick == tick) rows.pop_back();
    }
    auto& pose = rows.emplace_back();
    pose.tick = tick;
    for (int k = 0; k < 3; ++k) {
        pose.position[k] = body.position[k];
        pose.linear[k] = body.linear[k];
        pose.angular[k] = body.angular[k];
    }
    for (int k = 0; k < 4; ++k) pose.rotation[k] = body.rotation[k];
    pose.simulated = (body.flags & bmmo::session::BODY_FLAG_SIMULATED) != 0;
    while (rows.size() > physics_session_state::kMechanismRows) rows.pop_front();
}

// Option A: render the stored authoritative mechanism poses, once per frame
// after the snapshot queue was drained.  Only bodies this client actually has
// are written (a mechanism of another sector is not physicalized here), and a
// dictionary name carried by several server bodies is driven by the one whose
// authoritative pose is nearest to ours.  The pose itself comes from the shared
// helper: a render tick inside a snapshot pair interpolates the pair, past the
// newest row it is dead-reckoned along that row's authoritative velocity, and a
// teleport (or a body somewhere else entirely) is written as a snap, so a
// mechanism that lands on a sleeping ball still builds the contact pair instead
// of resting inside it until the next server correction.
void BallanceMMOClient::physics_session_apply_mechanism_authority() {
    auto& s = physics_session_;
    const uint32_t tick = s.current_tick();
    std::string error;
    for (const auto& candidate: mechanism_candidates(s, physics_view_)) {
        // `elsewhere` is measured against the pose this applier last commanded,
        // never against the live body: the live body IS that command (the
        // dead-reckon puts it up to kMechanismMaxExtrapolation past the newest
        // row on purpose), so measuring the row against it made the applier snap
        // to the raw row and dead-reckon again on the next frame, for as long as
        // the mechanism moved fast enough to be dead-reckoned at all.
        const bool elsewhere = std::sqrt(candidate.elsewhere_distance) > kMechanismSnapJump;
        const mechanism_target target = mechanism_target_at(*candidate.history, tick, elsewhere);
        if (target.snap) ++s.mechanism_snaps;
        if (mechanism_pose_unchanged(candidate.local, target)) {
            // Already there.  The target is the command all the same, and
            // recording it keeps the reference from drifting stale behind a body
            // the engine nudged onto the target after the previous write.
            mechanism_note_command(*candidate.history, target);
            continue;
        }
        if (physics_view_.set_body_state(candidate.name.c_str(), target.position, target.rotation, target.linear,
                                         target.angular, target.simulated, error, target.snap)) {
            ++s.body_writes;
            mechanism_note_command(*candidate.history, target);
        } else {
            // The body is not where the write wanted it, so the previous command
            // stands and the next frame still asks the applier's own question.
            ++s.body_write_errors;
            s.last_error = error;
        }
    }
}

// Option A: re-pose the untracked mechanism bodies for one re-simulated tick
// (rollback_world::pre_step).  The re-simulation restores the tracked bodies and
// the navigation replicas, but the mechanisms are server-authoritative and were
// left at the single pose the live applier wrote at the end of the previous
// frame: the replayed ball met each of them at the wrong place on every tick,
// so a rollback near a mechanism could not converge on the server trajectory.
// The target is the one the live path would render for that tick (the same
// helper, the same rows); the write has no snap recheck, because this walks the
// world forward tick by tick rather than beaming a body into a contact, and no
// mechanism_snaps count, which measures the live applier's snaps.
void BallanceMMOClient::physics_session_pose_mechanisms(uint32_t tick) {
    auto& s = physics_session_;
    std::string error;
    for (const auto& candidate: mechanism_candidates(s, physics_view_)) {
        // `elsewhere` is the live applier's test and is deliberately not passed
        // here: during a re-simulation the live pose is this hook's own write for
        // the previous tick, not a body somebody else moved.
        const mechanism_target target = mechanism_target_at(*candidate.history, tick, false);
        if (mechanism_pose_unchanged(candidate.local, target)) {
            mechanism_note_command(*candidate.history, target);
            continue;
        }
        if (physics_view_.set_body_state(candidate.name.c_str(), target.position, target.rotation, target.linear,
                                         target.angular, target.simulated, error)) {
            ++s.mechanism_resim_writes;
            // This hook stops at the current tick, so its last write is where the
            // live applier left the body before the rollback.  Recording it as
            // the command is what keeps the next live frame's `elsewhere` test
            // comparing the body against the applier's own prior state instead of
            // against a pose the re-simulation has overwritten since - which
            // would read as "somewhere else" and snap a body that is exactly
            // where the applier put it.
            mechanism_note_command(*candidate.history, target);
        } else {
            ++s.body_write_errors;
            s.last_error = error;
        }
    }
}

// Design 9.6: the own ball's forces come from the shared replica (polling the
// keyboard and the retail Key Event blocks at PreSimulate, camera rows from
// the previous frame), so a rollback can replay the recorded key edges.  The
// retail SetPhysicsForce leaves keep running but push zero.
void BallanceMMOClient::physics_session_attach_own_navigation(const std::string& ball_name, uint8_t ball_type) {
    auto& s = physics_session_;
    if (!s.navigation_keys_known || !s.navigation.valid() || ball_name.empty()) return;
    std::string error;
    if (s.own_navigation && s.own_nav_entity != ball_name) {
        // transformation: move the replica to the new ball entity
        if (physics_view_.navigation_set_ball(s.own_nav_entity.c_str(), ball_name.c_str(),
                                              physics_session_ball_force(ball_type), error)) {
            s.own_nav_entity = ball_name;
            return;
        }
        physics_view_.navigation_destroy(s.own_nav_entity.c_str(), error);
        s.own_navigation = false;
    }
    float directions[8][3] = {};
    int key_codes[8] = {};
    uint32_t key_blocks[8] = {};
    int count = 0;
    for (const auto& leaf: s.navigation.leaves) {
        if (leaf.index < 0 || leaf.index >= 8) continue;
        directions[leaf.index][0] = leaf.direction.x;
        directions[leaf.index][1] = leaf.direction.y;
        directions[leaf.index][2] = leaf.direction.z;
        key_codes[leaf.index] = leaf.key;
        key_blocks[leaf.index] = leaf.key_block;
        count = std::max(count, leaf.index + 1);
    }
    if (!physics_view_.navigation_create(ball_name.c_str(), "CamRef_BMMO_self", s.navigation.ball_navigation, directions,
                                         count, physics_session_ball_force(ball_type), error)
            || !physics_view_.navigation_poll(ball_name.c_str(), !s.input_muted, key_codes, key_blocks, count, error)) {
        logger_->Warn("Physics session: own ball navigation replica: %s", error.c_str());
        s.last_error = error;
        return;
    }
    s.own_navigation = true;
    s.own_nav_entity = ball_name;
    std::memcpy(s.own_key_codes, key_codes, sizeof(key_codes));
    std::memcpy(s.own_key_blocks, key_blocks, sizeof(key_blocks));
    s.own_key_count = count;
    logger_->Info("Physics session: own ball %s driven by the navigation replica (type %u, force %.3f)", ball_name.c_str(),
                  ball_type, physics_session_ball_force(ball_type));
}

// The retail SetPhysicsForce leaves push zero while the replica drives the
// ball: their "Force Value" input parameter is overwritten every frame (the
// scripts rewrite it only when the ball type changes).
void BallanceMMOClient::physics_session_zero_retail_forces() {
    auto& s = physics_session_;
    CKContext* context = m_bml->GetCKContext();
    const float zero = 0.0f;
    for (const auto& leaf: s.navigation.leaves) {
        auto* block = CKBehavior::Cast(context->GetObject(leaf.force_block));
        if (!block) continue;
        if (CKParameterIn* in = block->GetInputParameter(4))
            if (CKParameter* source = in->GetRealSource()) source->SetValue(&zero, sizeof(zero));
    }
}

// Navigation for a mirrored remote ball (design 9.1); a no-op until the
// navigation graph is known, then also called for remotes created before.
void BallanceMMOClient::physics_session_attach_remote_navigation(uint32_t player) {
    auto& s = physics_session_;
    auto it = s.remotes.find(player);
    if (it == s.remotes.end()) return;
    auto& remote = it->second;
    if (!remote.physicalized || remote.navigation || !s.navigation_keys_known || !s.navigation.valid()) return;
    std::string error;
    float directions[8][3] = {};
    int count = 0;
    for (const auto& leaf: s.navigation.leaves) {
        if (leaf.index < 0 || leaf.index >= 8) continue;
        directions[leaf.index][0] = leaf.direction.x;
        directions[leaf.index][1] = leaf.direction.y;
        directions[leaf.index][2] = leaf.direction.z;
        count = std::max(count, leaf.index + 1);
    }
    const std::string cam_ref = "CamRef_BMMO_" + std::to_string(player);
    if (physics_view_.navigation_create(remote.entity.c_str(), cam_ref.c_str(), s.navigation.ball_navigation, directions,
                                        count, physics_session_ball_force(remote.ball_type), error)) {
        remote.navigation = true;
        remote.corrector.clear();
    } else {
        logger_->Warn("Physics session: remote navigation for %s: %s", remote.entity.c_str(), error.c_str());
    }
}

// Next tick's input for every predicted remote ball: the last relayed frame
// (the relay is ~input_delay + latency behind, so key edges show up late and
// the corrector absorbs the difference).
void BallanceMMOClient::physics_session_drive_remotes() {
    auto& s = physics_session_;
    std::string error;
    for (auto& [id, remote]: s.remotes) {
        if (!remote.physicalized || !remote.navigation) continue;
        const uint8_t keys = remote.have_input ? remote.input.keys : 0;
        const bool active = remote.have_input && (remote.input.flags & bmmo::session::INPUT_FLAG_NAV_ACTIVE) != 0;
        remote.applied = remote.input;
        remote.applied.keys = keys;
        if (!active) remote.applied.flags &= static_cast<uint8_t>(~bmmo::session::INPUT_FLAG_NAV_ACTIVE);
        if (!physics_view_.navigation_input(remote.entity.c_str(), keys, remote.input.cam_right, remote.input.cam_up,
                                            remote.input.cam_dir, active, error))
            s.last_error = error;
    }
}

float BallanceMMOClient::physics_session_ball_force(uint8_t ball_type) const {
    const auto& s = physics_session_;
    if (ball_type < s.ball_forces.size()) return s.ball_forces[ball_type];
    return s.navigation.leaves.empty() ? 0.0f : s.navigation.leaves.front().force_value;
}

// The F3 panel's physics block (game thread).  Empty outside a session, so
// the panel looks exactly as it did before for everyone not in one.  What is
// worth watching live rather than in a log: how far the relay is behind, and
// whether the corrections are converging.
std::string BallanceMMOClient::physics_session_overlay_text() {
    auto& s = physics_session_;
    if (s.phase == phase_type::idle) return {};
    const char* phase = "idle";
    switch (s.phase) {
    case phase_type::counting_down: phase = "counting down"; break;
    case phase_type::restarting: phase = "restarting"; break;
    case phase_type::running: phase = "running"; break;
    case phase_type::ended: phase = "ended"; break;
    default: break;
    }
    const auto& rs = s.rollback.stats();
    const auto& st = s.corrector.stats();
    std::string text = std::format("\nPhysics session {} ({})\n", s.session, phase);
    if (s.phase != phase_type::running) return text;
    // The relay is input_delay + one trip behind us (design 9.1), which is
    // what every correction below has to travel.
    text += std::format("Tick: {} (relay {} behind)\n", s.current_tick(),
                        s.current_tick() > s.last_snapshot_tick ? s.current_tick() - s.last_snapshot_tick : 0);
    text += std::format("Input delay: {} ticks ({} ms)\n", s.input_delay,
                        static_cast<int>(s.input_delay * 1000 / 66));
    text += std::format("Snapshots: {} ok, {} corrected\n", rs.matched, rs.mismatched);
    if (input_freshness.reports)
        text += std::format("Input late: {}/{} ({:.0f}%), acked {}, lag {:.0f}\n",
                            input_freshness.starved, input_freshness.reports,
                            100.0 * static_cast<double>(input_freshness.starved) / static_cast<double>(input_freshness.reports),
                            input_freshness.last_acked,
                            static_cast<double>(input_freshness.lag_sum) / static_cast<double>(input_freshness.reports));
    if (rs.snapshots)
        text += std::format("Rollbacks: {} ({:.0f}%), {} ticks resim\n", rs.rollbacks,
                            100.0 * rs.mismatched / rs.snapshots, rs.resim_ticks);
    text += std::format("Max error: {:.3f} m\n", std::max(rs.max_error, st.max_error));
    if (rs.too_far || rs.frozen || s.resyncs_sent)
        text += std::format("Too far: {}  Frozen: {}  Resyncs: {}\n", rs.too_far, rs.frozen, s.resyncs_sent);
    if (!s.remotes.empty()) text += std::format("Remote balls: {}\n", s.remotes.size());
    if (!s.last_error.empty()) text += "Last error: " + s.last_error + "\n";
    return text;
}

std::string BallanceMMOClient::physics_session_status_text() {
    auto& s = physics_session_;
    const char* phase = "idle";
    switch (s.phase) {
    case phase_type::counting_down: phase = "counting down"; break;
    case phase_type::restarting: phase = "restarting"; break;
    case phase_type::running: phase = "running"; break;
    case phase_type::ended: phase = "ended"; break;
    default: break;
    }
    const auto& st = s.corrector.stats();
    const auto& rs = s.rollback.stats();
    uint64_t remote_compared = 0, remote_ignored = 0, remote_blended = 0, remote_hard = 0;
    for (const auto& [id, remote]: s.remotes) {
        const auto& rs = remote.corrector.stats();
        remote_compared += rs.compared;
        remote_ignored += rs.ignored;
        remote_blended += rs.blended;
        remote_hard += rs.hard;
    }
    return std::format(
        "session={} phase={} impulse={:.3f} tick={} base={} assigned={} frames={} inputs_sent={} keys_known={} own_phys={} group_set={} "
        "snapshots={}/{}/{} last_snapshot={} remotes={} remote_inputs={} remote_corr={}/{}/{}/{} mechanisms={}/{} writes={}/{} mech_resim_writes={} mech_snaps={} amend_failures={} "
        "events={}/{} phys_resends={} "
        "resyncs={}/{} rollback: {} snaps={} ok={} mism={} rb={} resim={} unmatched={} far={} frozen={} max_err={:.4f} last={} "
        "corrections: compared={} ignored={} blended={} hard={} unmatched={} last_err={:.4f} max_err={:.4f} last_error='{}'",
        s.session, phase, s.spawn_impulse, s.current_tick(), s.tick_base, s.assigned ? 1 : 0, static_cast<long long>(s.frames_since_anchor),
        s.inputs_sent, s.navigation_keys_known ? 1 : 0, s.own_physicalized ? 1 : 0, s.own_group_set ? 1 : 0,
        s.snapshots_received, s.snapshots_applied, s.snapshots_stale, s.last_snapshot_tick, s.remotes.size(),
        s.remote_inputs_received, remote_compared, remote_ignored, remote_blended, remote_hard,
        s.mechanism_names.size(), s.mechanism_authority.size(), s.body_writes, s.body_write_errors,
        s.mechanism_resim_writes, s.mechanism_snaps,
        s.amend_failures,
        s.events_sent, s.events_received, s.physicalize_resends,
        s.resyncs_sent, s.resyncs_done,
        s.rollback_enabled ? "on" : "off", rs.snapshots, rs.matched, rs.mismatched, rs.rollbacks, rs.resim_ticks, rs.unmatched,
        rs.too_far, rs.frozen, rs.max_error, rs.last_mismatch,
        st.compared, st.ignored, st.blended, st.hard, st.unmatched, st.last_error, st.max_error, s.last_error);
}
