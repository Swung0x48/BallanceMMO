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

namespace {
    using bmmo::session::physics_session_state;
    using phase_type = physics_session_state::phase_type;

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
        s.assigned = true;
        s.tick_base = first_tick;
        logger_->Info("Physics session %u: tick base %u assigned (%lld frames since anchor)", s.session,
                      s.tick_base, static_cast<long long>(s.frames_since_anchor));
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
    s.session = msg.session;
    s.room = msg.room;
    s.snapshot_interval = msg.snapshot_interval;
    s.input_delay = msg.input_delay;
    s.seed = msg.seed;
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
        s.phase = phase_type::idle;
        return;
    }
    s.phase = phase_type::restarting;
    s.saw_ingame_inactive = false;
    s.restart_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    SendIngameMessage(std::format("Physics session {} starting: restarting the level to synchronize.", s.session),
                      bmmo::ansi::BrightGreen);
    logger_->Info("Physics session %u: restart requested (players=%zu, join order %d)", s.session, s.players.size(),
                  s.own_join_order);
    // Fixed 1/66 s behaviour delta from here on: the retail intro timers start
    // in the anchor frame (Gameplay_Ingame's first frame), and with the retail
    // limits that frame's delta is whatever the restart took (4.5 ms .. 200 ms
    // measured), which moved the intro's end by a tick between the two sides.
    if (!fixed_tick_.enabled()) fixed_tick_.enable(m_bml);
    restart_current_level();
}

void BallanceMMOClient::physics_session_end_local(const std::string& reason) {
    auto& s = physics_session_;
    if (s.phase == phase_type::idle) return;
    std::string error;
    for (auto& [id, remote]: s.remotes) {
        if (remote.navigation && physics_view_.available()) physics_view_.navigation_destroy(remote.entity.c_str(), error);
        if (remote.physicalized && physics_view_.available()) physics_view_.unphysicalize(remote.entity.c_str(), error);
        objects_.set_physicalized(id, false);
    }
    if (fixed_tick_.enabled()) fixed_tick_.disable(m_bml);
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
    if (CKDataArray* level = current_level_array()) {
        VxMatrix reset;
        if (level->GetElementValue(0, 3, &reset) && s.spawn_known) {
            for (int k = 0; k < 3; ++k) s.spawn_offset[k] = s.spawn_position[k] - reset[3][k];
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
    SendIngameMessage("Physics session: level synchronized, waiting for the server.", bmmo::ansi::BrightGreen);
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

    // Key bindings arrive from Gameplay_Refresh a few frames after the anchor.
    if (!s.navigation_keys_known) {
        auto graph = bmmo::game::read_navigation_graph(m_bml->GetCKContext());
        bool complete = graph.valid();
        for (const auto& leaf: graph.leaves) complete = complete && leaf.key != 0;
        if (complete) {
            s.navigation = graph;
            s.navigation_keys_known = true;
            logger_->Info("Physics session: %zu navigation leaves, keys %d/%d/%d/%d", graph.leaves.size(),
                          graph.leaves.size() > 0 ? graph.leaves[0].key : 0, graph.leaves.size() > 1 ? graph.leaves[1].key : 0,
                          graph.leaves.size() > 2 ? graph.leaves[2].key : 0, graph.leaves.size() > 3 ? graph.leaves[3].key : 0);
        }
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
    } else {
        s.own_group_set = false;
        // Ring offset (design 3.4): while the retail script holds the ball at
        // the resetpoint before physicalizing it, move it to our slot.
        CKDataArray* level = current_level_array();
        if (ball && s.spawn_known && level && s.players.size() > 1) {
            VxMatrix reset;
            VxVector position;
            ball->GetPosition(&position);
            if (level->GetElementValue(0, 3, &reset)) {
                const VxVector reset_position(reset[3][0], reset[3][1], reset[3][2]);
                if ((position - reset_position).SquareMagnitude() < 1e-6f) {
                    VxVector target(reset_position.x + s.spawn_offset[0], reset_position.y + s.spawn_offset[1],
                                    reset_position.z + s.spawn_offset[2]);
                    if ((target - position).SquareMagnitude() > 1e-8f) ball->SetPosition(&target);
                }
            }
        }
    }

    // Mechanism states after this tick (design 8.5 step 5): the snapshot for
    // tick T is compared with these, never with a later state of the body.
    for (const auto& [index, name]: s.mechanism_names) {
        bmmo_physics_body_state local{};
        if (!physics_view_.get_body_state(name.c_str(), local, error)) continue;
        bmmo::session::ball_pose pose;
        pose.tick = tick;
        for (int k = 0; k < 3; ++k) {
            pose.position[k] = local.position[k];
            pose.linear[k] = local.linear[k];
            pose.angular[k] = local.angular[k];
        }
        for (int k = 0; k < 4; ++k) pose.rotation[k] = local.rotation[k];
        s.mechanism_correctors[name].record(pose);
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
    bmmo::session::input_frame frame{};
    if (s.navigation_keys_known && input_hook_installed_) frame.keys = s.navigation.keys_from_state(frame_keys_.data());
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
    frame.ball_type = ball_name.empty() ? 0 : static_cast<uint8_t>(db_.get_ball_id(ball_name));
    frame.flags = static_cast<uint8_t>((s.own_physicalized ? bmmo::session::INPUT_FLAG_PHYSICALIZED : 0)
                | (m_bml->IsPaused() ? bmmo::session::INPUT_FLAG_PAUSED : 0)
                | (ball_nav_active_ ? bmmo::session::INPUT_FLAG_NAV_ACTIVE : 0));
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
    s.input_history.emplace_back(tick, frame);
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

    // Mechanism wake-ups the retail scripts did this frame.
    s.revived_reported_this_frame.clear();
    const std::string events = physics_view_.drain_event_log();
    size_t pos = 0;
    while ((pos = events.find("revived ", pos)) != std::string::npos) {
        pos += 8;
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

    physics_session_apply_queues();

    // Continue running blends (own ball and mechanisms).
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
    for (auto& [name, corrector]: s.mechanism_correctors) apply_blend(name, corrector);
    for (auto& [id, remote]: s.remotes)
        if (remote.physicalized && remote.navigation) apply_blend(remote.entity, remote.corrector);
    physics_session_drive_remotes();
}

void BallanceMMOClient::physics_session_send_event(bmmo::session_event_msg& event) {
    auto& s = physics_session_;
    event.session = s.session;
    event.serialize();
    send(event.raw.str().data(), event.size(), k_nSteamNetworkingSend_Reliable);
    ++s.events_sent;
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
    for (auto& event: events) physics_session_apply_event(event);
    for (const auto& msg: inputs) {
        if (msg.session != s.session) continue;
        for (const auto& entry: msg.entries) {
            auto it = s.remotes.find(entry.player);
            if (it == s.remotes.end()) continue;
            auto& remote = it->second;
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
        if (remote.physicalized && remote.entity != entity->GetName()) {
            if (remote.navigation) physics_view_.navigation_destroy(remote.entity.c_str(), error);
            physics_view_.unphysicalize(remote.entity.c_str(), error);
        } else if (remote.navigation) {
            physics_view_.navigation_destroy(remote.entity.c_str(), error);
        }
        remote.navigation = false;
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
        remote.entity = entity->GetName();
        remote.ball_type = event.ball_type;
        remote.physicalized = true;
        remote.corrector.clear();
        objects_.set_physicalized(event.player, true);
        objects_.on_trafo(event.player, std::numeric_limits<uint32_t>::max(), event.ball_type);
        // Design 9.1: drive the mirror with the retail navigation replica from
        // the relayed inputs; the snapshots then only correct it.
        if (s.navigation_keys_known && s.navigation.valid()) {
            float directions[8][3] = {};
            int count = 0;
            for (const auto& leaf: s.navigation.leaves) {
                if (leaf.index < 0 || leaf.index >= 8) continue;
                directions[leaf.index][0] = leaf.direction.x;
                directions[leaf.index][1] = leaf.direction.y;
                directions[leaf.index][2] = leaf.direction.z;
                count = std::max(count, leaf.index + 1);
            }
            const std::string cam_ref = "CamRef_BMMO_" + std::to_string(event.player);
            if (physics_view_.navigation_create(remote.entity.c_str(), cam_ref.c_str(), s.navigation.ball_navigation,
                                                directions, count, physics_session_ball_force(event.ball_type), error))
                remote.navigation = true;
            else
                logger_->Warn("Physics session: remote navigation for %s: %s", remote.entity.c_str(), error.c_str());
        }
        break;
    }
    case bmmo::session::event_type::Unphysicalize: {
        auto it = s.remotes.find(event.player);
        if (it == s.remotes.end()) return;
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
    if (s.have_snapshot && snapshot.tick <= s.last_snapshot_tick && !snapshot.full) {
        ++s.snapshots_stale;
        return;
    }
    s.have_snapshot = true;
    s.last_snapshot_tick = std::max(s.last_snapshot_tick, snapshot.tick);
    ++s.snapshots_applied;
    const auto own_id = db_.get_client_id();
    CK3dObject* ball = get_current_ball();
    const std::string ball_name = ball && ball->GetName() ? ball->GetName() : "";
    std::string error;
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
        // Mechanism: resolve the dictionary index, skip bodies this client
        // does not have (other sectors).
        if (snapshot.full && !body.name.empty()) s.mechanism_names[body.owner] = body.name;
        auto name = s.mechanism_names.find(body.owner);
        if (name == s.mechanism_names.end()) continue;
        const bool wake = (body.flags & bmmo::session::BODY_FLAG_SIMULATED) != 0;
        bmmo_physics_body_state local{};
        if (!physics_view_.get_body_state(name->second.c_str(), local, error)) continue;   // not physicalized here
        bmmo::session::ball_pose pose;
        pose.tick = snapshot.tick;
        for (int k = 0; k < 3; ++k) {
            pose.position[k] = body.position[k];
            pose.linear[k] = body.linear[k];
            pose.angular[k] = body.angular[k];
        }
        for (int k = 0; k < 4; ++k) pose.rotation[k] = body.rotation[k];
        auto& corrector = s.mechanism_correctors[name->second];
        const auto step = corrector.compare(pose);
        if (step.action == bmmo::session::correction_step::kind::hard) {
            ++s.mechanism_hard;
            physics_session_log_correction(name->second, snapshot.tick, corrector.stats().last_error, "hard");
            if (physics_view_.set_body_state(name->second.c_str(), step.target.position, step.target.rotation,
                                             step.target.linear, step.target.angular, wake, error))
                ++s.body_writes;
            else { ++s.body_write_errors; s.last_error = error; }
        } else if (step.action == bmmo::session::correction_step::kind::blend) {
            ++s.mechanism_blends;
            physics_session_log_correction(name->second, snapshot.tick, corrector.stats().last_error, "blend");
        } else {
            ++s.mechanism_matches;
        }
    }
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
    if (!ball || target != static_cast<CK3dEntity*>(ball)) return;
    bmmo::session_event_msg event;
    event.tick = s.current_tick() + 1;   // this frame's physics step is still ahead
    event.type = bmmo::session::event_type::Physicalize;
    event.ball_type = static_cast<uint8_t>(db_.get_ball_id(ball->GetName() ? ball->GetName() : ""));
    const VxMatrix& world = target->GetWorldMatrix();
    for (int k = 0; k < 3; ++k) event.position[k] = world[3][k];
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) event.rotation[r * 3 + k] = world[r][k];
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
    s.own_group_set = false;
    if (s.trace) s.exact_log_frames = 12;
    logger_->Info("Physics session: own ball %s physicalized at tick %u (type %u, %d convex, %d balls) pos=%a,%a,%a rows=%a,%a,%a|%a,%a,%a|%a,%a,%a",
                  ball->GetName(), event.tick, event.ball_type, convexCnt, ballCnt,
                  event.position[0], event.position[1], event.position[2],
                  event.rotation[0], event.rotation[1], event.rotation[2], event.rotation[3], event.rotation[4],
                  event.rotation[5], event.rotation[6], event.rotation[7], event.rotation[8]);
}

void BallanceMMOClient::OnUnphysicalize(CK3dEntity* target) {
    auto& s = physics_session_;
    if (s.phase != phase_type::running || !target) return;
    CK3dObject* ball = get_current_ball();
    if (!ball || target != static_cast<CK3dEntity*>(ball)) return;
    bmmo::session_event_msg event;
    event.tick = s.current_tick() + 1;
    event.type = bmmo::session::event_type::Unphysicalize;
    physics_session_send_event(event);
    s.corrector.clear();
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
    if (++s.corrections_logged > 200) return;   // enough to diagnose, not enough to flood
    logger_->Info("Physics session: %s correction of %s for tick %u (error %.4f m, local tick %u)",
                  action, name.c_str(), tick, error, s.current_tick());
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

std::string BallanceMMOClient::physics_session_status_text() {
    auto& s = physics_session_;
    const char* phase = "idle";
    switch (s.phase) {
    case phase_type::restarting: phase = "restarting"; break;
    case phase_type::running: phase = "running"; break;
    case phase_type::ended: phase = "ended"; break;
    default: break;
    }
    const auto& st = s.corrector.stats();
    uint64_t remote_compared = 0, remote_ignored = 0, remote_blended = 0, remote_hard = 0;
    for (const auto& [id, remote]: s.remotes) {
        const auto& rs = remote.corrector.stats();
        remote_compared += rs.compared;
        remote_ignored += rs.ignored;
        remote_blended += rs.blended;
        remote_hard += rs.hard;
    }
    double mechanism_max_error = 0.0;
    for (const auto& [name, corrector]: s.mechanism_correctors)
        if (corrector.stats().max_error > mechanism_max_error) mechanism_max_error = corrector.stats().max_error;
    return std::format(
        "session={} phase={} tick={} base={} assigned={} frames={} inputs_sent={} keys_known={} own_phys={} group_set={} "
        "snapshots={}/{}/{} last_snapshot={} remotes={} remote_inputs={} remote_corr={}/{}/{}/{} mechanisms={} writes={}/{} mech_same={} mech_blend={} mech_hard={} "
        "mech_max_err={:.4f} events={}/{} "
        "corrections: compared={} ignored={} blended={} hard={} unmatched={} last_err={:.4f} max_err={:.4f} last_error='{}'",
        s.session, phase, s.current_tick(), s.tick_base, s.assigned ? 1 : 0, static_cast<long long>(s.frames_since_anchor),
        s.inputs_sent, s.navigation_keys_known ? 1 : 0, s.own_physicalized ? 1 : 0, s.own_group_set ? 1 : 0,
        s.snapshots_received, s.snapshots_applied, s.snapshots_stale, s.last_snapshot_tick, s.remotes.size(),
        s.remote_inputs_received, remote_compared, remote_ignored, remote_blended, remote_hard,
        s.mechanism_names.size(), s.body_writes, s.body_write_errors, s.mechanism_matches, s.mechanism_blends, s.mechanism_hard,
        mechanism_max_error, s.events_sent, s.events_received,
        st.compared, st.ignored, st.blended, st.hard, st.unmatched, st.last_error, st.max_error, s.last_error);
}
