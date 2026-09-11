// Client side of a physics session (design section 8.5): restart-and-anchor
// on SessionStart, one input per tick, own-ball lifecycle events from the BML
// physicalize hooks, mirrored remote balls, locally simulated shared
// mechanisms (design 9.25) and correction of every predicted body through the
// rollback engine.
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
#include <thread>

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

    // How long the world waits at the anchor for SessionAssign before it starts
    // stepping anyway (design 9.25 phase alignment).  The assignment is one
    // reliable round trip behind SessionReady plus the slowest member's
    // remaining level load, which the start barrier already caps at 8 s
    // (sim/late_tick.hpp kStartBarrierTimeout); this is the fault timeout
    // behind that, so a server that never answers costs a phase offset and a
    // warning instead of a frozen game.
    constexpr std::chrono::seconds kAnchorHoldTimeout{10};

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

    // The behavior the guard used to read as its pause sensor: the Event_handler's
    // "Pause Level" chain deactivated Gameplay_Ingame, and the guard holds the
    // time factor whenever the sensor is inactive.  A session no longer lets
    // that chain stop the script (see pause_scripts_resolve), so the sensor is
    // never inactive and the guard's hold never fires - it only samples the
    // factor the level scripts wrote, which is what the server honours too, and
    // writes nothing.  It stays attached because the mod may not see a pause
    // inside its own frame hook: the sensor is read in the engine's PreSimulate
    // pass, and dropping the attachment is an engine-side change with no
    // benefit while the actor that triggers it has been neutralized.
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

// Network thread: only queued.  The game thread applies it at its next frame
// boundary - or inside the anchor hold's own polling loop, which is why this
// does not go through run_on_game_thread(): that defers through BML's timer
// queue, which runs between frames, and the hold waits inside one.
void BallanceMMOClient::handle_session_assign(const bmmo::session_assign_msg& msg) {
    auto& s = physics_session_;
    std::lock_guard lk(s.queue_mutex);
    s.assign_queue.push_back({msg.session, msg.first_tick});
}

// Game thread: every queued assignment, in arrival order.
void BallanceMMOClient::physics_session_drain_assignments() {
    auto& s = physics_session_;
    std::deque<physics_session_state::assign_notice> pending;
    {
        std::lock_guard lk(s.queue_mutex);
        pending.swap(s.assign_queue);
    }
    for (const auto& notice: pending) physics_session_apply_assign(notice.session, notice.first_tick);
}

void BallanceMMOClient::physics_session_apply_assign(uint32_t session, uint32_t first_tick) {
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
        // Design 9.25 phase alignment: with the hold in place `renumbered`
        // is 0 - the world has not stepped since the anchor, and the step
        // it takes next is the one the server's world takes under this very
        // number.  A non-zero count here means the hold timed out.
        physics_session_release_hold("tick base assigned");
        logger_->Info("Physics session %u: tick base %u assigned (renumbered, %lld anchor-relative frames dropped)",
                      s.session, s.tick_base, static_cast<long long>(renumbered));
    } else {
        // Base 0: a server that still numbers the members present at the
        // start from their own anchor.  The backlog frames stamped 0..k-1
        // are valid inputs it is waiting for, and renumbering would relabel
        // them.
        s.tick_base = first_tick;
        // Base 0 leaves the numbering alone, but the world still has to
        // start: an old server that numbers from the anchor gets the world
        // running here rather than at the hold's timeout.
        physics_session_release_hold("tick base 0 assigned (anchor numbering)");
        logger_->Info("Physics session %u: tick base %u assigned (%lld frames since anchor)", s.session,
                      s.tick_base, static_cast<long long>(s.frames_since_anchor));
    }
    s.last_rebases = fixed_tick_.rebases();
    bmmo::session::client_journal::instance().note(
            s.tick_base, std::format("assigned: tick base {}", s.tick_base));
    physics_session_flush_inputs();
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
    // The pause chains' edits go back too: outside a session the menu stops and
    // starts the world scripts as retail intends, a death resets the sector
    // again and a checkpoint deactivates the one behind it (design 9.25).
    pause_scripts_restore();
    death_reset_restore();
    sector_deactivate_restore();
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
    // The tick assignment is applied here, at the frame boundary, whatever the
    // phase (one that arrives outside `running` is dropped, as before).
    physics_session_drain_assignments();
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

// The level scripts a session must keep running while the menu is open.
// Gameplay_Ingame carries the world's per-frame work - BallManager's
// out-of-bounds test, which kills a ball that leaves the level, is one of its
// blocks - and Gameplay_Events the level's event scripts.  Gameplay_Tutorial is
// not in this set on purpose: the menu stopping the tutorial is retail
// behaviour with no consequence for the world, and the unpause chain resumes it
// without a reset.
namespace {
    const char* const kSessionWorldScripts[] = {"Gameplay_Events", "Gameplay_Ingame"};
    constexpr int kSessionWorldScriptCount = 2;

    bool is_session_world_script(const char* name) {
        if (!name) return false;
        for (const char* candidate: kSessionWorldScripts)
            if (std::strcmp(name, candidate) == 0) return true;
        return false;
    }

    // The Deactivate Script / Activate Script blocks of one pause chain, in
    // graph order.  They are leaves; a group below a chain (the Unpause Level
    // chain has one) is walked into.
    struct pause_script_block {
        const char* chain;
        CKBehavior* block;
    };

    void collect_pause_script_blocks(const char* chain, CKBehavior* behavior,
                                     std::vector<pause_script_block>& out) {
        const int count = behavior ? behavior->GetSubBehaviorCount() : 0;
        for (int i = 0; i < count; ++i) {
            CKBehavior* sub = behavior->GetSubBehavior(i);
            if (!sub) continue;
            const char* prototype = sub->IsUsingFunction() ? sub->GetPrototypeName() : nullptr;
            if (prototype
                    && (std::strcmp(prototype, "Deactivate Script") == 0
                        || std::strcmp(prototype, "Activate Script") == 0)) {
                out.push_back({chain, sub});
                continue;
            }
            collect_pause_script_blocks(chain, sub, out);
        }
    }

    // A recorded entry, resolved back into the live graph for the applier and
    // the restorer.  The context recycles object ids when a level is torn down
    // (CKObjectManager::RegisterObject), so an id alone is not enough to write
    // through: the block has to still be a Deactivate/Activate Script block
    // inside the chain it was found in, holding its "Script" parameter in the
    // state the session left it in.  A level change that leaves a stale table
    // behind fails those checks and is skipped until the next anchor resolves
    // the new graph.
    CKParameter* pause_script_parameter(CKContext* context, CK_ID block_id, const char* chain, int input_index) {
        auto* block = CKBehavior::Cast(context->GetObject(block_id));
        if (!block || !block->IsUsingFunction()) return nullptr;
        const char* prototype = block->GetPrototypeName();
        if (!prototype
                || (std::strcmp(prototype, "Deactivate Script") != 0
                    && std::strcmp(prototype, "Activate Script") != 0))
            return nullptr;
        CKBehavior* parent = block->GetParent();
        const char* parent_name = parent ? parent->GetName() : nullptr;
        if (!chain || !parent_name || std::strcmp(parent_name, chain) != 0) return nullptr;
        CKParameterIn* input = block->GetInputParameter(input_index);
        if (!input || input->GetGUID() != CKPGUID_SCRIPT) return nullptr;
        CKParameter* param = input->GetRealSource();
        if (!param || param->GetDataSize() != static_cast<int>(sizeof(CK_ID))) return nullptr;
        return param;
    }

    CK_ID pause_script_value(CKParameter* param) {
        CK_ID value = 0;
        if (param) param->GetValue(&value);
        return value;
    }

    const char* pause_script_prototype(CKContext* context, CK_ID block_id) {
        auto* block = CKBehavior::Cast(context->GetObject(block_id));
        return block && block->IsUsingFunction() ? block->GetPrototypeName() : "?";
    }

    // How many behavior inputs read one parameter, and which ones: the
    // shared-reader safety check every chain neutralization below makes -
    // emptying a parameter that something else reads would take its value away
    // too.
    std::pair<int, std::string> parameter_readers(CKContext* context, CKParameter* param) {
        std::string text;
        int readers = 0;
        if (!context || !param) return {0, text};
        const int behaviors = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
        CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
        for (int b = 0; b < behaviors; ++b) {
            auto* other = CKBehavior::Cast(context->GetObject(ids[b]));
            const int inputs = other ? other->GetInputParameterCount() : 0;
            for (int k = 0; k < inputs; ++k) {
                CKParameterIn* in = other->GetInputParameter(k);
                if (!in || in->GetRealSource() != param) continue;
                ++readers;
                if (readers > 8) continue;
                CKBehavior* owner = other->GetParent();
                text += std::format("{}{}/{} input {}", text.empty() ? "" : ", ",
                                    owner && owner->GetName() ? owner->GetName() : "?",
                                    other->GetName() ? other->GetName() : "?", k);
            }
        }
        return {readers, text};
    }

    // A recorded block, resolved back into the live graph: the context recycles
    // object ids when a level is torn down, so the id alone is not enough to
    // write through (see pause_script_parameter).
    CKBehavior* session_block(CKContext* context, CK_ID id, const char* prototype) {
        auto* block = CKBehavior::Cast(context->GetObject(id));
        if (!block || !block->IsUsingFunction()) return nullptr;
        const char* name = block->GetPrototypeName();
        return name && std::strcmp(name, prototype) == 0 ? block : nullptr;
    }
}

// The retail pause chain also stops and restarts the level's gameplay scripts:
// "Pause Level" deactivates Gameplay_Events and Gameplay_Ingame, "Unpause
// Level" re-activates both.  A session must not let the menu do that.  The
// world it stops is exactly the thing the session keeps simulating, and the
// death of a ball that leaves the level happens inside Gameplay_Ingame: with
// the script stopped, the ball crossed the depth it had died at earlier and was
// never killed (build/manual-test-922-20260910, pause window ticks 7917-14925).
//
// The neutralization is the block's target, not the block: the "Script" input's
// real source is written with a null CK_ID, and both blocks act on nothing when
// that is what they read.  Deactivate Script hands null to
// CKScene::DeActivate, which returns without doing anything
// (submodule/Ballanced/Source/CK2/src/CKScene.cpp:214), and Activate Script
// skips a null target outright (ActivateScript.cpp:124).  The block still
// fires, its Out still carries the chain along the same links with the same
// delays, and the chain's own work - End Music, Mouse On/Off, Start Music, the
// cursor chain - is untouched, so the menu behaves exactly as it did.
//
// A graph that does not match - a missing chain, a target input with no source
// or a source of another type or size, a target parameter an input other than
// ours also reads, more targets than the table holds, no target at all - is
// refused as a whole and logged as an error, because half a fix leaves the world
// pausing in a way that is far harder to see than the failure itself.  Every
// level (re)entry brings a new graph, so this runs at each anchor.
bool BallanceMMOClient::pause_scripts_resolve() {
    pause_scripts_count_ = 0;
    const auto refuse = [this](const std::string& why) {
        pause_scripts_count_ = 0;
        pause_scripts_failed_ = true;
        logger_->Error("Physics session: the pause menu's script edits were NOT neutralized (%s): the world "
                       "will stop while the menu is open", why.c_str());
        return false;
    };
    CKContext* context = m_bml->GetCKContext();
    CKBehavior* event_handler = m_bml->GetScriptByName("Event_handler");
    if (!event_handler) return refuse("Event_handler is missing");
    static const char* kChains[2] = {"Pause Level", "Unpause Level"};
    std::vector<pause_script_block> blocks;
    for (const char* chain_name: kChains) {
        CKBehavior* chain = ScriptHelper::FindFirstBB(event_handler, chain_name);
        if (!chain) return refuse(std::string(chain_name) + " is missing");
        collect_pause_script_blocks(chain_name, chain, blocks);
    }
    // A target parameter can be shared: the retail level feeds Gameplay_Events
    // to the Pause chain's Deactivate Script and to the Unpause chain's
    // Activate Script through ONE parameter object.  Emptying it is then safe -
    // both readers are inputs this fix is neutralizing - but only then: a reader
    // that is not one of ours would lose its target with ours.  So the pass
    // below first collects every candidate input and then checks that each
    // target parameter's readers are exactly the candidates that read it.
    struct script_target {
        CKBehavior* block;
        const char* chain;
        int input;
        CKParameter* param;
        CK_ID retail;
        const char* name;
    };
    std::vector<script_target> candidates;
    for (const auto& candidate: blocks) {
        CKBehavior* block = candidate.block;
        const int inputs = block->GetInputParameterCount();
        for (int i = 0; i < inputs; ++i) {
            CKParameterIn* input = block->GetInputParameter(i);
            // Activate Script's input 0 is its "Reset ?" flag, so an input is a
            // target when its type says so - the same test the block makes.
            if (!input || input->GetGUID() != CKPGUID_SCRIPT) continue;
            CKParameter* param = input->GetRealSource();
            if (!param || param->GetDataSize() != static_cast<int>(sizeof(CK_ID)))
                return refuse(std::format("{}/{} input {} has no usable target parameter", candidate.chain,
                                          block->GetName() ? block->GetName() : "?", i));
            CK_ID retail = 0;
            param->GetValue(&retail);
            auto* target = CKBehavior::Cast(block->GetInputParameterObject(i));
            if (!target || !is_session_world_script(target->GetName())) continue;
            candidates.push_back({block, candidate.chain, i, param, retail, target->GetName()});
        }
    }
    if (candidates.empty()) return refuse("no pause chain block targets the world scripts");
    bool seen[kSessionWorldScriptCount] = {};
    std::string list;
    for (const auto& candidate: candidates) {
        const auto [readers, readers_text] = parameter_readers(context, candidate.param);
        int mine = 0;
        for (const auto& other: candidates)
            if (other.param == candidate.param) ++mine;
        if (readers != mine)
            return refuse(std::format("{}/{}'s target {} is read by {} inputs ({})", candidate.chain,
                                      candidate.block->GetName() ? candidate.block->GetName() : "?",
                                      candidate.name, readers, readers_text));
        if (pause_scripts_count_ >= PAUSE_SCRIPT_WRITES)
            return refuse(std::format("more than {} target inputs", PAUSE_SCRIPT_WRITES));
        pause_script_write& write = pause_scripts_[pause_scripts_count_++];
        write.block = candidate.block->GetID();
        write.chain = candidate.chain;
        write.input = candidate.input;
        write.retail = candidate.retail;
        write.applied = false;
        for (int s = 0; s < kSessionWorldScriptCount; ++s)
            if (std::strcmp(kSessionWorldScripts[s], candidate.name) == 0) seen[s] = true;
        list += std::format("{}{}/{} -> {}", list.empty() ? "" : ", ", candidate.chain,
                            candidate.block->GetName() ? candidate.block->GetName() : "?", candidate.name);
    }
    for (int s = 0; s < kSessionWorldScriptCount; ++s)
        if (!seen[s]) logger_->Warn("Physics session: no pause chain block targets %s", kSessionWorldScripts[s]);
    logger_->Info("Physics session: pause menu script edits neutralized: %s", list.c_str());
    pause_scripts_apply();
    return true;
}

// Keeps the edits applied: the resolve neutralizes every target once, and a
// frame that finds one back in place writes it out again, so nothing can hand
// the pause back to the retail chain behind the session's back.  Only the exact
// retail target of a recorded input is ever moved, and only to null: anything
// else in that parameter is somebody else's value and is left alone.
void BallanceMMOClient::pause_scripts_apply() {
    if (!pause_scripts_count_) return;
    CKContext* context = m_bml->GetCKContext();
    const CK_ID zero = 0;
    for (int i = 0; i < pause_scripts_count_; ++i) {
        pause_script_write& write = pause_scripts_[i];
        CKParameter* param = pause_script_parameter(context, write.block, write.chain, write.input);
        const CK_ID current = pause_script_value(param);
        if (current == 0) {
            write.applied = true;   // already neutral (this session, or the same graph reloaded)
            continue;
        }
        if (current != write.retail) continue;
        param->SetValue(&zero, sizeof(CK_ID));
        if (!write.applied) {
            write.applied = true;
            logger_->Info("Physics session: pause chain target %u emptied (input %d)", write.retail, write.input);
        }
    }
}

// Hands the retail targets back and drops the table, so the menu stops and
// starts the world scripts again exactly as it did before the session.
void BallanceMMOClient::pause_scripts_restore() {
    CKContext* context = m_bml->GetCKContext();
    for (int i = 0; i < pause_scripts_count_; ++i) {
        pause_script_write& write = pause_scripts_[i];
        CKParameter* param = pause_script_parameter(context, write.block, write.chain, write.input);
        if (pause_script_value(param) == 0 && write.retail) {
            const CK_ID retail = write.retail;
            param->SetValue(&retail, sizeof(CK_ID));
        }
        write = {};
    }
    pause_scripts_count_ = 0;
    pause_scripts_failed_ = false;
}

// Design 9.25, the two retail chains a session must not let run.  Both are
// per-player world edits the server never makes, and both tear down bodies the
// server keeps simulating - which is what the body guard (engine change #6) was
// already fighting, one level deeper than it can reach: it keeps the body, but
// the module's script rebuilds the CONSTRAINTS around it from the referential
// entities' initial poses (PhysicsBallJoint.cpp:105-149 anchors at the
// referential's current world position, and the module's TT Restore IC has just
// put that back at its IC), so the sandbag's joint is violated by however far it
// had swung and IVP removes the violation with an impulse - the convulsion of
// the user's third symptom (findings/A2 section 5.5).
//
// N1, the sector resets Gameplay_Ingame runs.  Two of its groups call
// "Execute Script" on Gameplay_SectorManager with Reset? = TRUE, i.e. the whole
// sector is deactivated and re-activated in one frame:
//
//   * BallManager / Deactivate Ball, for one player's death (9.25);
//   * Init Ingame / activate Scripts, for a level reset (9.28) - the path a
//     lost last life and the ESC menu's "restart level" both take, through
//     "Reset Level" -> Event_handler / reset Level -> Activate Script
//     (Gameplay_Ingame) -> Gameplay_Ingame.Start -> Init Ingame.
//
// The server's world never does either (design section 2: it activates the
// union of the players' sectors and never resets), so neither may we.  Both are
// found by walking Gameplay_Ingame for the target's name rather than by group
// path, so a third one would be caught as well; the sibling call sites outside
// this script are deliberately left alone (Gameplay_Events / activate Sektor is
// N2's, and Event_handler's two only ever deactivate - they write the sector
// cell to 0 first, which makes the SectorManager's own Test skip its iterator).
//
// Each block's "Script" target is emptied for the session, exactly like the
// pause chains': Execute Script activates its Out immediately when the script
// is null (ExecuteScript.cpp:66-70), so "Set Cell.Found -> Execute Script.In ->
// Deactivate Ball.Ball OFF" keeps its shape, its delays and the respawn behind
// it, and Init Ingame's four Activate Scripts (Gameplay_Events, Sky, Energy and
// the tutorial test) still run.  Skipping the re-activation cannot leave the
// sector dead, because a reset never stopped it: "deactivate Scripts" iterates
// Logic_Scripts, which holds the 20 Gameplay_*/Ball*/AnimTrafo_* scripts and no
// module MF script (verified with --dump-array Logic_Scripts on Level 11).
//
// The visible differences from retail are that a personal death no longer
// re-shows the sector's collected items, and that a level reset no longer puts
// the mechanisms back at their start - both consistent with a shared world that
// keeps running while one player restarts.
//
// N2, the checkpoint deactivation: see sector_deactivate_resolve below.
bool BallanceMMOClient::death_reset_resolve() {
    death_reset_count_ = 0;
    for (auto& write: death_reset_) write = {};
    const auto refuse = [this](const std::string& why) {
        death_reset_count_ = 0;
        for (auto& write: death_reset_) write = {};
        death_reset_failed_ = true;
        logger_->Error("Physics session: the retail sector resets were NOT neutralized (%s): a death or a level "
                       "reset will reset this client's sector and rebuild the mechanisms' joints under the body "
                       "guard", why.c_str());
        return false;
    };
    CKContext* context = m_bml->GetCKContext();
    CKBehavior* ingame = m_bml->GetScriptByName("Gameplay_Ingame");
    if (!ingame) return refuse("Gameplay_Ingame is missing");
    // Every Execute Script in the script's tree whose "Script" input names
    // Gameplay_SectorManager, with the group it sits in for the log.
    std::string trouble;
    const auto walk = [&](auto&& self, CKBehavior* group) -> void {
        const int subs = group->GetSubBehaviorCount();
        for (int i = 0; i < subs; ++i) {
            CKBehavior* sub = group->GetSubBehavior(i);
            if (!sub) continue;
            if (!sub->IsUsingFunction()) { self(self, sub); continue; }
            const char* prototype = sub->GetPrototypeName();
            if (!prototype || std::strcmp(prototype, "Execute Script") != 0) continue;
            auto* target = CKBehavior::Cast(sub->GetInputParameterObject(1));   // "Script"
            const char* name = target ? target->GetName() : nullptr;
            if (!name || std::strcmp(name, "Gameplay_SectorManager") != 0) continue;
            if (death_reset_count_ >= DEATH_RESET_WRITES) {
                if (trouble.empty()) trouble = "more sector resets than the table holds";
                return;
            }
            CKParameterIn* input = sub->GetInputParameter(1);
            if (!input || input->GetGUID() != CKPGUID_SCRIPT) {
                if (trouble.empty()) trouble = "an Execute Script block has no Script input";
                return;
            }
            CKParameter* param = input->GetRealSource();
            if (!param || param->GetDataSize() != static_cast<int>(sizeof(CK_ID))) {
                if (trouble.empty()) trouble = "its Script input has no usable target parameter";
                return;
            }
            // Emptying a parameter something else reads would take its value
            // away too - Event_handler's two call sites share Level_Init's
            // published one, which is exactly why they are not touched here.
            const auto [readers, readers_text] = parameter_readers(context, param);
            if (readers != 1) {
                if (trouble.empty())
                    trouble = std::format("the target parameter of {} is read by {} inputs ({})",
                                          group->GetName() ? group->GetName() : "?", readers, readers_text);
                return;
            }
            death_reset_write& write = death_reset_[death_reset_count_++];
            write.block = sub->GetID();
            write.chain = group->GetName() ? group->GetName() : "?";
            write.input = 1;
            param->GetValue(&write.retail);
            write.applied = false;
            // Everything but the death chain's is on the level-start path too,
            // so it may only be emptied once the sector it would re-activate
            // is already up (see death_reset_write::deferred).
            write.deferred = std::strcmp(write.chain, "Deactivate Ball") != 0;
        }
    };
    walk(walk, ingame);
    if (!trouble.empty()) return refuse(trouble);
    // Both known call sites must be there: a graph that lost one is a graph
    // this fix no longer understands, and half a neutralization is worse than a
    // loud failure (the level reset one is only reached minutes into a run).
    if (death_reset_count_ < 2)
        return refuse(std::format("Gameplay_Ingame runs Gameplay_SectorManager from {} place(s), expected at "
                                  "least 2 (Deactivate Ball and Init Ingame/activate Scripts)", death_reset_count_));
    death_reset_failed_ = false;
    death_reset_apply();
    return true;
}

// Keeps the edit applied, like pause_scripts_apply: a level reset that handed
// the retail target back has to be caught before the next death runs the chain.
void BallanceMMOClient::death_reset_apply() {
    CKContext* context = m_bml->GetCKContext();
    // "The sector is up": this client resolved at least one mechanism of its
    // own, so nothing may re-activate the sector from here on.
    const bool sector_up = physics_session_.mechanism_tracking.resolved() > 0;
    for (int i = 0; i < death_reset_count_; ++i) {
        death_reset_write& write = death_reset_[i];
        if (!write.block) continue;
        if (write.deferred && !sector_up) continue;
        CKBehavior* block = session_block(context, write.block, "Execute Script");
        CKParameterIn* input = block ? block->GetInputParameter(write.input) : nullptr;
        CKParameter* param = input && input->GetGUID() == CKPGUID_SCRIPT ? input->GetRealSource() : nullptr;
        if (!param || param->GetDataSize() != static_cast<int>(sizeof(CK_ID))) continue;
        CK_ID current = 0;
        param->GetValue(&current);
        if (current == 0) {
            write.applied = true;   // already neutral (this session, or the same graph reloaded)
            continue;
        }
        if (current != write.retail) continue;   // somebody else's value: not ours to move
        const CK_ID zero = 0;
        param->SetValue(&zero, sizeof(CK_ID));
        if (!write.applied) {
            write.applied = true;
            logger_->Info("Physics session: the sector reset in %s (Execute Script -> %u) is neutralized",
                          write.chain ? write.chain : "?", write.retail);
        }
    }
}

void BallanceMMOClient::death_reset_restore() {
    CKContext* context = m_bml->GetCKContext();
    for (int i = 0; i < death_reset_count_; ++i) {
        death_reset_write& write = death_reset_[i];
        CKBehavior* block = write.block ? session_block(context, write.block, "Execute Script") : nullptr;
        CKParameterIn* input = block ? block->GetInputParameter(write.input) : nullptr;
        CKParameter* param = input && input->GetGUID() == CKPGUID_SCRIPT ? input->GetRealSource() : nullptr;
        if (param && param->GetDataSize() == static_cast<int>(sizeof(CK_ID)) && write.retail) {
            CK_ID current = 0;
            param->GetValue(&current);
            if (current == 0) {
                const CK_ID retail = write.retail;
                param->SetValue(&retail, sizeof(CK_ID));
            }
        }
        write = {};
    }
    death_reset_count_ = 0;
    death_reset_failed_ = false;
}

// N2, the checkpoint deactivation.  Crossing a checkpoint runs
// Gameplay_Events / activate Sektor, which writes IngameParameter[0][1] (the
// sector to activate) and IngameParameter[0][2] (the sector to deactivate) and
// then runs Gameplay_SectorManager, whose "Deactivate Sector" group resets and
// hides everything of the named sector - bodies the server keeps, because its
// world activates the union of every player's sectors and deactivates nothing
// (physics_world::update_sectors).  A client that deactivated its previous
// sector would unphysicalize and re-pose the shared mechanisms of a sector
// somebody else is still playing in.
//
// The deactivate write is fed a session-owned constant 0 instead of whatever
// the chain computes: the SectorManager's own "Test (Not Equal, B = 0)" then
// skips the whole deactivation, which is exactly what the automation's `sector`
// verb and the headless --start-sector write by hand.  The source is swapped
// rather than emptied because the two Set Cell blocks of the chain read the
// same kind of parameter and one of them must keep working; the original source
// (a direct source, or the input this one shares its source with) goes back at
// session end.
bool BallanceMMOClient::sector_deactivate_resolve() {
    sector_deactivate_ = {};
    const auto refuse = [this](const std::string& why) {
        sector_deactivate_ = {};
        sector_deactivate_failed_ = true;
        logger_->Error("Physics session: the checkpoint's sector deactivation was NOT neutralized (%s): crossing a "
                       "checkpoint will reset the previous sector's mechanisms here", why.c_str());
        return false;
    };
    CKContext* context = m_bml->GetCKContext();
    CKBehavior* events = m_bml->GetScriptByName("Gameplay_Events");
    if (!events) return refuse("Gameplay_Events is missing");
    CKBehavior* chain = ScriptHelper::FindFirstBB(events, "activate Sektor");
    if (!chain) return refuse("Gameplay_Events/activate Sektor is missing");
    CKDataArray* parameters = m_bml->GetArrayByName("IngameParameter");
    if (!parameters) return refuse("the IngameParameter array is missing");
    CKBehavior* block = nullptr;
    bool target_known = false;
    const int subs = chain->GetSubBehaviorCount();
    for (int i = 0; i < subs; ++i) {
        CKBehavior* sub = chain->GetSubBehavior(i);
        const char* prototype = sub && sub->IsUsingFunction() ? sub->GetPrototypeName() : nullptr;
        if (!prototype || std::strcmp(prototype, "Set Cell") != 0) continue;
        int row = -1, column = -1;
        sub->GetInputParameterValue(0, &row);
        sub->GetInputParameterValue(1, &column);
        if (row != 0 || column != 2) continue;   // [0][1] is the activate write: it stays
        // A block that demonstrably writes another array is not ours; one whose
        // target cannot be read here (it is bound through a parameter this pass
        // cannot resolve) still counts, because the chain and the cell already
        // identify it - the ambiguity is reported below.
        CKBeObject* target = sub->GetTarget();
        if (target && target != static_cast<CKBeObject*>(parameters)) continue;
        if (block) return refuse("more than one Set Cell writes IngameParameter[0][2]");
        block = sub;
        target_known = target != nullptr;
    }
    if (!block) return refuse("no Set Cell inside activate Sektor writes IngameParameter[0][2]");
    if (!target_known)
        logger_->Warn("Physics session: the checkpoint's Set Cell does not name its array here; taking the one "
                      "writing cell [0][2] of Gameplay_Events/activate Sektor");
    CKParameterIn* input = block->GetInputParameter(2);   // "Value"
    if (!input) return refuse("the Set Cell block has no Value input");
    CKParameter* source = input->GetRealSource();
    if (!source) return refuse("its Value input has no source");
    if (source->GetGUID() != CKPGUID_INT || source->GetDataSize() != static_cast<int>(sizeof(int)))
        return refuse("its Value input is not an int parameter");
    auto* zero = context->CreateCKParameterLocal(const_cast<CKSTRING>("BMMO_SectorKeep"), CKPGUID_INT, TRUE);
    if (!zero) return refuse("could not create the session's constant");
    const int none = 0;
    zero->SetValue(&none, sizeof(none));
    CKParameterIn* shared = input->GetSharedSource();
    CKParameter* direct = input->GetDirectSource();
    if (input->SetDirectSource(zero) != CK_OK) {
        context->DestroyObject(zero);
        return refuse("the Value input refused the session's constant");
    }
    sector_deactivate_.block = block->GetID();
    sector_deactivate_.input = 2;
    sector_deactivate_.retail_source = direct ? direct->GetID() : 0;
    sector_deactivate_.retail_shared = shared ? shared->GetID() : 0;
    sector_deactivate_.retail_real = source->GetID();
    sector_deactivate_.zero = zero->GetID();
    sector_deactivate_.applied = true;
    sector_deactivate_failed_ = false;
    logger_->Info("Physics session: the checkpoint's sector deactivation (activate Sektor/Set Cell -> "
                  "IngameParameter[0][2]) is pinned to 0");
    return true;
}

// Only ever moves the input back onto our own constant, and only from the
// source it had when the session resolved it: anything else in that input
// belongs to a graph this table no longer describes.
void BallanceMMOClient::sector_deactivate_apply() {
    if (!sector_deactivate_.block || !sector_deactivate_.zero) return;
    CKContext* context = m_bml->GetCKContext();
    CKBehavior* block = session_block(context, sector_deactivate_.block, "Set Cell");
    CKParameterIn* input = block ? block->GetInputParameter(sector_deactivate_.input) : nullptr;
    auto* zero = CKParameter::Cast(context->GetObject(sector_deactivate_.zero));
    if (!input || !zero) return;
    CKParameter* current = input->GetRealSource();
    if (current == zero) return;   // still ours
    if (!current || current->GetID() != sector_deactivate_.retail_real) return;
    if (input->SetDirectSource(zero) == CK_OK)
        logger_->Info("Physics session: the checkpoint's sector deactivation was re-pinned to 0");
}

void BallanceMMOClient::sector_deactivate_restore() {
    CKContext* context = m_bml->GetCKContext();
    CKBehavior* block = sector_deactivate_.block ? session_block(context, sector_deactivate_.block, "Set Cell") : nullptr;
    CKParameterIn* input = block ? block->GetInputParameter(sector_deactivate_.input) : nullptr;
    auto* zero = sector_deactivate_.zero ? CKParameter::Cast(context->GetObject(sector_deactivate_.zero)) : nullptr;
    bool referenced = false;
    if (input && zero && input->GetRealSource() == zero) {
        referenced = true;
        if (sector_deactivate_.retail_shared) {
            auto* shared = CKParameterIn::Cast(context->GetObject(sector_deactivate_.retail_shared));
            if (shared && input->ShareSourceWith(shared) == CK_OK) referenced = false;
        } else {
            auto* source = CKParameter::Cast(context->GetObject(sector_deactivate_.retail_source));
            if (source && input->SetDirectSource(source) == CK_OK) referenced = false;
        }
        if (referenced)
            logger_->Warn("Physics session: the checkpoint chain's Value input could not be handed back to its "
                          "retail source; the session's constant is left in place");
    }
    // Destroying a parameter an input still points at would leave a dangling
    // source behind, so the one case that keeps it is the one where the input
    // still reads it.
    if (zero && !referenced) context->DestroyObject(zero);
    sector_deactivate_ = {};
    sector_deactivate_failed_ = false;
}

// The automation's read ("pausechain"): one line that says whether the menu is
// open, whether the world scripts are still active, and what each recorded
// pause-chain block points at now (a target of 0 is a neutralized one, the
// retail id is a block the session has not emptied).
std::string BallanceMMOClient::pause_scripts_status() {
    CKContext* context = m_bml->GetCKContext();
    const auto active = [](CKBehavior* script) { return script && script->IsActive() ? 1 : 0; };
    const float factor = [this] {
        // The engine's scaled factor, the value the level scripts set through
        // Set Physics Globals (see kTimeFactorScriptScale).
        float value = 0.0f, delta = 0.0f;
        std::string error;
        return physics_view_.get_clock(value, delta, error) ? value : -1.0f;
    }();
    std::string out = std::format(
            "paused={} ingame_script={} events_script={} tutorial_script={} muted={} time_factor={:.6f} "
            "entries={} failed={}",
            m_bml->IsPaused() ? 1 : 0, active(m_bml->GetScriptByName("Gameplay_Ingame")),
            active(m_bml->GetScriptByName("Gameplay_Events")), active(m_bml->GetScriptByName("Gameplay_Tutorial")),
            physics_session_.input_muted ? 1 : 0, factor, pause_scripts_count_, pause_scripts_failed_ ? 1 : 0);
    // Design 9.25: the two world resets this session also neutralizes, so one
    // read says whether the client still runs a retail sector reset.
    {
        int sector_value = -1;
        if (CKBehavior* block = session_block(context, sector_deactivate_.block, "Set Cell"))
            if (CKParameterIn* input = block->GetInputParameter(sector_deactivate_.input))
                if (CKParameter* param = input->GetRealSource())
                    if (param->GetGUID() == CKPGUID_INT) param->GetValue(&sector_value);
        out += std::format(" death_reset={}/{} sector_keep={}/{}/now={}", death_reset_count_,
                           death_reset_failed_ ? "failed" : "resolved", sector_deactivate_.zero,
                           sector_deactivate_failed_ ? "failed" : (sector_deactivate_.applied ? "applied" : "resolved"),
                           sector_value);
        // One entry per call site: <group>=<retail target>/<applied?>/now=<what
        // the block reads today>, so the read still says whether this client
        // would run a retail sector reset.
        for (int i = 0; i < death_reset_count_; ++i) {
            const death_reset_write& write = death_reset_[i];
            CK_ID now = 0;
            if (CKBehavior* block = session_block(context, write.block, "Execute Script"))
                if (CKParameterIn* input = block->GetInputParameter(write.input))
                    if (CKParameter* param = input->GetRealSource()) param->GetValue(&now);
            out += std::format(" [{}={}/{}/now={}]", write.chain ? write.chain : "?", write.retail,
                               write.applied ? "applied" : "resolved", now);
        }
    }
    for (int i = 0; i < pause_scripts_count_; ++i) {
        const pause_script_write& write = pause_scripts_[i];
        CKParameter* param = pause_script_parameter(context, write.block, write.chain, write.input);
        out += std::format(" [{}/{} input{} retail={} now={}]", write.chain ? write.chain : "?",
                           pause_script_prototype(context, write.block), write.input, write.retail,
                           pause_script_value(param));
    }
    return out;
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
    // The retail pause menu stops Gameplay_Ingame and writes the time factor 0.
    // Neither is allowed to reach the world while the session runs: the pause
    // chains' script edits are neutralized (pause_scripts_resolve) and the time
    // factor block is pinned to the factor in use, so the level scripts keep
    // driving the clock exactly as they do on the server.
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
    // A level (re)entry brought a new Event_handler graph: drop the previous
    // table and resolve the pause chains' script edits again (see
    // pause_scripts_resolve).
    pause_scripts_restore();
    pause_scripts_resolve();
    // Design 9.25: the same for the two retail chains that reset this client's
    // sector behind the server's back - the death reset and the checkpoint's
    // deactivation.  Each logs its own error and the session runs on: a session
    // that refuses to start because one level's graph is unusual is worse than
    // one that plays with a known difference.
    death_reset_restore();
    sector_deactivate_restore();
    const bool death_neutralized = death_reset_resolve();
    const bool sector_neutralized = sector_deactivate_resolve();
    if (death_neutralized && sector_neutralized)
        logger_->Info("Physics session: retail world resets neutralized: %d sector reset(s) in Gameplay_Ingame no "
                      "longer run Gameplay_SectorManager (a death and a level reset), and a checkpoint activates "
                      "its sector without deactivating the previous one", death_reset_count_);
    // Design 9.26: the mechanism Sequencers start from the level file's
    // counters, as they do on the server, instead of wherever the play before
    // the restart left them (Level 11's sandbags swing the other way otherwise).
    {
        const int restored = level_sequencers_.restore(m_bml->GetCKContext());
        logger_->Info("Physics session: %d of %zu mechanism Sequencer counter(s) restored to the level file's (%s)",
                      restored, level_sequencers_.size(), level_sequencers_.describe().c_str());
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
    // Design 9.26 phase alignment: stand still here until the server numbers
    // our first step (physics_session_hold_until_assigned, at the end of this
    // frame).  The frames this used to run were renumbered away by the
    // assignment anyway, and they left this world that many steps further from
    // the anchor than the server's at every tick number of the session.
    s.hold_active = true;
    s.hold_polls = 0;
    s.hold_timed_out = false;
    s.hold_deadline = std::chrono::steady_clock::now() + kAnchorHoldTimeout;
    fixed_tick_.set_hold(true);
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
    bmmo::session::client_journal::instance().note(0, "anchor hold: the world stands still until the tick base arrives");
    SendIngameMessage("Physics session: level synchronized, waiting for the server.", bmmo::ansi::BrightGreen);
    physics_session_hold_until_assigned();
}

// The anchor hold (design 9.26 phase alignment).  Waits inside the anchor
// frame's OnProcess until the server has numbered our first step, polling the
// assignment the network thread queued - the headless client's loop
// (sim/session_client.cpp), and nothing else: no behaviour frame runs
// meanwhile, no input is recorded or sent, no rollback record is made and no
// TICK record goes into the black box, so the next frame is the world's first
// step and the box holds exactly the ticks the session has.
//
// A frame with a near-zero delta was tried first and is not still enough: it
// stops the script timers and IVP, but not the frame-counted links of the
// level graphs (a link with a delay of n frames fires after n frames whatever
// the delta), so the sector's scripts ran that many frames ahead of the
// server's and Level 11's sandbags were out of phase from the first tick
// (retail run of 2026-09-11: 62 such frames, a sandbag correction every 6
// ticks for the whole session).
//
// The picture stays on the last rendered frame for the wait - the slowest
// member's SessionReady plus one reliable round trip, which the server's start
// barrier caps at 8 s; kAnchorHoldTimeout bounds it here, so a server that
// never answers costs a warning and a phase offset instead of a frozen game.
void BallanceMMOClient::physics_session_hold_until_assigned() {
    auto& s = physics_session_;
    while (s.hold_active) {
        physics_session_drain_assignments();   // releases the hold on a start assignment
        if (!s.hold_active) break;
        if (!connected()) {
            physics_session_release_hold("the connection was lost");
            break;
        }
        if (std::chrono::steady_clock::now() >= s.hold_deadline) {
            s.hold_timed_out = true;
            physics_session_release_hold("no tick assignment arrived");
            logger_->Warn("Physics session %u: no tick assignment after %lld s; the world starts anyway and will be "
                          "out of phase with the server's", s.session,
                          static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(kAnchorHoldTimeout).count()));
            SendIngameMessage("Physics session: the server did not assign a tick base; starting out of phase.",
                              bmmo::ansi::BrightYellow);
            break;
        }
        ++s.hold_polls;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void BallanceMMOClient::physics_session_release_hold(const char* why) {
    auto& s = physics_session_;
    if (!s.hold_active) return;
    s.hold_active = false;
    fixed_tick_.set_hold(false);
    // The driver's schedule continues where the hold interrupted it, so the
    // wait is not lag: `rebases` must not move, or the next frame asks for a
    // resync.
    s.last_rebases = fixed_tick_.rebases();
    bmmo::session::client_journal::instance().note(s.tick_base,
            std::format("anchor hold released after {} polls: {}", s.hold_polls, why));
    logger_->Info("Physics session %u: anchor hold released after %llu polls (%s); the first step is tick %u",
                  s.session, static_cast<unsigned long long>(s.hold_polls), why, s.tick_base);
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
    // The pause chains' script edits stay neutralized for as long as the
    // session runs: the menu may be opened at any tick, and a target a level
    // reset handed back must be emptied again before the chain can use it.
    pause_scripts_apply();
    // The same for the death reset and the checkpoint deactivation (9.25):
    // both chains can fire at any tick.
    death_reset_apply();
    sector_deactivate_apply();

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

    // The bridge's object log for this frame, read before the record below: a
    // body created or deleted during this frame's scripts and PSIs is already
    // in the world the record describes, so its new lifetime generation has to
    // be stamped on THAT record (design 9.25) - a generation that arrived one
    // tick late would make the engine treat the record as the previous body's
    // and restore from the wrong lifetime.  Two readings of the same log:
    //
    //  * created / deleted: a body behind a tracked name changed, so its
    //    generation moves on and the mechanism registry re-resolves (the name
    //    may now have a local body, or have lost the one it had);
    //  * script_wakeup: only explicit script wake-ups belong on the
    //    authoritative timeline.  IVP's generic revived events also include
    //    predicted collisions and rollback restores: sending those back would
    //    wake the server's bodies again and feed the next correction.
    s.revived_reported_this_frame.clear();
    {
        const std::string events = physics_view_.drain_event_log();
        size_t pos = 0;
        while (pos < events.size()) {
            const size_t end = events.find(';', pos);
            if (end == std::string::npos) break;
            const std::string entry = events.substr(pos, end - pos);
            pos = end + 1;
            // "t=<seconds> <kind> <name>" (physics_state.cpp); anything else
            // (the listener's own "listener installed") has no name and is
            // skipped by the same test.
            const size_t first = entry.find(' ');
            if (first == std::string::npos) continue;
            const size_t second = entry.find(' ', first + 1);
            if (second == std::string::npos) continue;
            const std::string kind = entry.substr(first + 1, second - first - 1);
            const std::string name = entry.substr(second + 1);
            if (name.empty()) continue;
            if (kind == "created" || kind == "deleted") {
                ++s.generations[name];
                s.mechanism_tracking.mark_dirty();
                continue;
            }
            if (kind == "constraint" || kind == "birth" || kind == "psi" || kind == "force") {
                // Bridge diagnostics (design 9.26): what a body was born as, the
                // constraints and forces the scripts built, the first PSIs of a
                // watched body (BMMO_PSI_PROBE).  Logged under `session trace`.
                if (s.trace) logger_->Info("Physics session: %s %s at tick %u", kind.c_str(), name.c_str(), tick);
                continue;
            }
            if (kind != "script_wakeup") continue;
            if (name.rfind("Ball_", 0) == 0 || name == ball_name || name.find("_Peer_") != std::string::npos
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
    }
    // The mechanisms this client can be corrected on, resolved before the
    // record: the tracked set of a tick has to name bodies that exist in it.
    physics_session_resolve_mechanisms(tick);

    // Input for this tick: keys polled at this frame's PreProcess, camera
    // basis from the END of the previous frame (the retail Ball Navigation
    // executes before the camera scripts of a frame), nav state after this
    // frame's scripts.

    // Pause menu (ESC): the menu is the game's paused state, and while it is
    // open the player's arrow keys must not drive the ball.  The retail scripts
    // no longer stop Gameplay_Ingame for us (pause_scripts_resolve keeps them
    // running, so the world keeps stepping), which also means the keyboard poll
    // they drive keeps reading the held keys: report zero keys and stop the
    // replica from polling them, exactly as the old "ingame script stopped"
    // gate did when the menu deactivated it.
    const bool muted = m_bml->IsIngame() && m_bml->IsPaused();
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
        // Design 9.25: the shared mechanisms are ordinary tracked bodies again.
        // Only the names this client has a body for - a tracked name with no
        // body is dropped by every capture, which used to change the tracked
        // set on every tick.
        for (const auto& name: s.mechanism_tracking.tracked_entities()) tracked.mechanisms.push_back(name);
        // The lifetime of every tracked name, so a record of an earlier body
        // under the same name is never used to restore the current one.
        tracked.generations = s.generations;
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

    // A too_far or frozen decision sets the bodies and truncates the history
    // without re-simulating, so the world stays on the snapshot while our tick
    // counter runs on: the two timelines no longer line up.  Re-anchor instead
    // of drifting, once - request_resync itself rate-limits, and a request
    // already in flight will bring the full snapshot this needs.
    const uint64_t too_far_before = s.rollback.stats().too_far;
    const uint64_t frozen_before = s.rollback.stats().frozen;
    const uint64_t unmatched_before = s.rollback.stats().unmatched;
    physics_session_apply_queues();
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
            // The mirror of the old ball is about to go: a new lifetime for
            // that name, so no record of it can restore the body of the next
            // one (design 9.25 - this used to throw the whole history away).
            ++s.generations[remote.entity];
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
        // Queues run after this frame's history capture, so the new body (also
        // a same-name respawn) is first recorded on the NEXT live frame.  Its
        // generation moves on here: the engine then restores it from its own
        // first record instead of from a record of the body that was there
        // before, and every OTHER body keeps its history (design 9.25).
        ++s.generations[entity->GetName()];
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
        if (it->second.physicalized) ++s.generations[it->second.entity];   // the body goes: a new lifetime
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
                // The dictionary (full snapshots only) and the newest pose, for
                // the identity guard; the row itself is compared and restored
                // by the rollback engine like any other tracked body (9.25).
                if (snapshot.full && !body.name.empty()) s.mechanism_tracking.note_name(body.owner, body.name);
                s.mechanism_tracking.note_row(snapshot.tick, body);
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
        // Mechanism, with the rollback engine switched off (automation:
        // "session rollback off"): the body is simulated locally like every
        // other one and nothing corrects it, so the row only keeps the
        // dictionary and the identity guard up to date.
        if (snapshot.full && !body.name.empty()) s.mechanism_tracking.note_name(body.owner, body.name);
        s.mechanism_tracking.note_row(snapshot.tick, body);
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

// The automation `beam` verb inside a physics session (design 9.25 follow-up).
// The local write alone moves this client's ball only: the server keeps its own
// copy where it was and the next snapshot drags the ball back, which is what
// made `beam` useless for a contact test in a session.  The server's copy is
// moved the way the retail death chain moves it - Unphysicalize, then
// Physicalize at the new pose with the last reported recipe and WITHOUT the
// spawn flag (a spawn flag would kick the re-created body on every side while
// this one, beamed by hand, was not kicked).  A bare repeat Physicalize would
// not do: the server's apply_event moves the entity and then calls physicalize,
// which returns early for an entity that already has a body, and the body drives
// the entity back on the next PSI (physics_state.cpp, headless --beam).
// Returns what the caller should append to its answer.
std::string BallanceMMOClient::physics_session_report_beam(const std::string& entity, const double position[3]) {
    auto& s = physics_session_;
    if (s.phase != phase_type::running || !s.assigned) return " (no running physics session: local only)";
    if (!s.own_physicalized || !s.last_physicalize.valid)
        return " (no physicalized own ball in the session: local only)";
    // The tick the events are stamped for is the one this frame's physics step
    // will be numbered with, exactly like the Physicalize the BML hooks send.
    const uint32_t tick = s.current_tick() + 1;
    // A new lifetime under the same name: the server rebuilds its body, so no
    // record of the old one may be used to restore this one (design 9.25).
    ++s.generations[entity];
    bmmo::session_event_msg down;
    down.tick = tick;
    down.type = bmmo::session::event_type::Unphysicalize;
    physics_session_send_event(down);
    bmmo::session_event_msg up;
    up.tick = tick;
    up.type = bmmo::session::event_type::Physicalize;
    up.ball_type = s.last_physicalize.ball_type;
    up.flags = static_cast<uint8_t>(s.last_physicalize.flags & ~bmmo::session::PHYSICALIZE_FLAG_SPAWN);
    for (int k = 0; k < 3; ++k) up.position[k] = static_cast<float>(position[k]);
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) up.rotation[r * 3 + k] = r == k ? 1.0f : 0.0f;   // upright, like the local write
    up.recipe = s.last_physicalize.recipe;
    physics_session_send_event(up);
    // Keep the resend copy in step with what the server now has, and drop the
    // correction ring: it describes a ball that no longer exists on either side.
    s.last_physicalize.flags = up.flags;
    for (int k = 0; k < 3; ++k) s.last_physicalize.position[k] = up.position[k];
    for (int k = 0; k < 9; ++k) s.last_physicalize.rotation[k] = up.rotation[k];
    s.corrector.clear();
    s.snapshots_without_own = 0;
    s.own_group_set = false;
    logger_->Info("Physics session: beam of %s re-reported to the server (Unphysicalize + Physicalize at tick %u)",
                  entity.c_str(), tick);
    return std::format(" (server told: Unphysicalize+Physicalize at tick {})", tick);
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
    // BML calls this before body creation/PreSimulate, so this frame's
    // post-physics record already captures the new body.  A same-name respawn
    // is not the body the old records describe: its generation moves on here
    // (design 9.25), which is what keeps a snapshot from restoring the fresh
    // ball to the pose the previous one had - without dropping the history of
    // every other body, as the invalidation this replaces did.
    ++s.generations[target->GetName() ? target->GetName() : ""];
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
    ++s.generations[target->GetName() ? target->GetName() : ""];   // the body goes: a new lifetime
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
    // Design 9.25: the engine decides the sleep handling and the contact
    // recheck per write.  `keep` is the common case - most restores move a body
    // that is already in the state the server reports, and ensure_in_simulation
    // would re-arm its freeze timers (engine change #14); `recheck` is asked for
    // only when the write actually moved the body, because the optimized beam
    // builds no contact when it lands one body inside another.
    world.set_body = [this](const std::string& entity, const bmmo_physics_body_state& state,
                            bmmo::session::wake_mode mode, bool recheck) {
        std::string error;
        const auto bridge_mode = mode == bmmo::session::wake_mode::wake   ? bmmo::physics::wake_mode::wake
                               : mode == bmmo::session::wake_mode::freeze ? bmmo::physics::wake_mode::freeze
                                                                          : bmmo::physics::wake_mode::keep;
        if (physics_view_.set_body_state(entity.c_str(), state.position, state.rotation, state.linear, state.angular,
                                         bridge_mode, recheck, error))
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
    // No pre_step hook since 9.25: the mechanisms are tracked bodies and the
    // re-simulation steps them like everything else.  The hook stays in the
    // engine for the script-edge replay of findings/A5 section 2j.
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
        // Design 9.25: a mechanism row names one of our bodies again - the one
        // the registry resolved for that owner.  Empty when this client has no
        // body of that name (another sector) or when the identity guard refused
        // the pick; on_snapshot then skips the row (rollback.hpp).
        return s.mechanism_tracking.entity_of(body.owner);
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
    // The only explicit invalidation left in the client since 9.25: a body's
    // lifetime is a generation now, not a reason to forget every other body -
    // but the records between the re-assignment and this snapshot describe a
    // world this call is about to replace, and nothing can replay across that.
    s.rollback.invalidate_history();
    std::string error;
    const auto own_id = db_.get_client_id();
    CK3dObject* ball = get_current_ball();
    const std::string ball_name = ball && ball->GetName() ? ball->GetName() : "";
    // The dictionary and the newest rows first, then one resolve pass: this is a
    // full snapshot of a world that was just rebuilt, so it is both the moment
    // the registry knows the most and the moment its old answers are worth the
    // least (9.25).
    for (const auto& body: snapshot.bodies)
        if (body.kind == bmmo::session::body_kind::Mechanism) {
            if (!body.name.empty()) s.mechanism_tracking.note_name(body.owner, body.name);
            s.mechanism_tracking.note_row(snapshot.tick, body);
        }
    s.mechanism_tracking.mark_dirty();
    physics_session_resolve_mechanisms(snapshot.tick);
    std::string mechanism_entity;
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
            mechanism_entity = s.mechanism_tracking.entity_of(body.owner);
            if (mechanism_entity.empty()) continue;   // another sector's instance
            target = mechanism_entity.c_str();
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

// Design 9.25: which of this client's bodies each mechanism row is about.
// The dictionary names bodies of every sector the server runs, this client has
// only the ones its own sectors activated, and one name can be carried by
// several server owners - so the answer is a probe per distinct name, not per
// row.  Re-resolved when the registry says so: a new dictionary entry, a body
// created or deleted here (the bridge's event log marks it dirty), or once a
// second as a safety net.
void BallanceMMOClient::physics_session_resolve_mechanisms(uint32_t tick) {
    auto& s = physics_session_;
    if (!s.mechanism_tracking.due(tick)) return;
    s.mechanism_tracking.resolve(tick, [this](const char* entity, bmmo_physics_body_state& out) {
        std::string error;
        return physics_view_.get_body_state(entity, out, error);
    });
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
        "session={} phase={} impulse={:.3f} tick={} base={} assigned={} held={}/{}{} frames={} inputs_sent={} keys_known={} own_phys={} group_set={} "
        "snapshots={}/{}/{} last_snapshot={} remotes={} remote_inputs={} remote_corr={}/{}/{}/{} mechanisms={}/{} writes={}/{} generations={} amend_failures={} "
        "events={}/{} phys_resends={} "
        "resyncs={}/{} rollback: {} snaps={} ok={} mism={} rb={} resim={} unmatched={} far={} frozen={} max_err={:.4f} last={} "
        "corrections: compared={} ignored={} blended={} hard={} unmatched={} last_err={:.4f} max_err={:.4f} last_error='{}'",
        // held=<holding now>/<1 ms polls the hold took>[+timeout]: the anchor
        // hold of design 9.26 - the wait between the anchor and the tick base,
        // in which no frame runs at all.
        s.session, phase, s.spawn_impulse, s.current_tick(), s.tick_base, s.assigned ? 1 : 0,
        s.hold_active ? 1 : 0, s.hold_polls, s.hold_timed_out ? "+timeout" : "",
        static_cast<long long>(s.frames_since_anchor),
        s.inputs_sent, s.navigation_keys_known ? 1 : 0, s.own_physicalized ? 1 : 0, s.own_group_set ? 1 : 0,
        s.snapshots_received, s.snapshots_applied, s.snapshots_stale, s.last_snapshot_tick, s.remotes.size(),
        s.remote_inputs_received, remote_compared, remote_ignored, remote_blended, remote_hard,
        // mechanisms=<in the dictionary>/<tracked here> (design 9.25: the
        // second number is how many of them this client simulates and can be
        // corrected on), generations=<tracked names with a lifetime counter>
        s.mechanism_tracking.known(), s.mechanism_tracking.resolved(), s.body_writes, s.body_write_errors,
        s.generations.size(),
        s.amend_failures,
        s.events_sent, s.events_received, s.physicalize_resends,
        s.resyncs_sent, s.resyncs_done,
        s.rollback_enabled ? "on" : "off", rs.snapshots, rs.matched, rs.mismatched, rs.rollbacks, rs.resim_ticks, rs.unmatched,
        rs.too_far, rs.frozen, rs.max_error, rs.last_mismatch,
        st.compared, st.ignored, st.blended, st.hard, st.unmatched, st.last_error, st.max_error, s.last_error);
}
