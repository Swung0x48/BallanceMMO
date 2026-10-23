// The retail host of the shared session-client core
// (docs/session-client-core-design.md 3.2; step R0 of 6.2).
//
// The core (BallanceMMOCommon/src/session/client_core*.cpp) holds the whole
// client session flow; a host owns the loop and implements the platform
// interface (session/client_platform.hpp) with what it has.  This file is the
// retail mod's host:
//
//   * the mailbox: the network thread copies the bytes of every Session*
//     message, SessionStart and SessionLead included, into one mutex-guarded
//     queue, in arrival order; the snapshot entries are capped like the
//     legacy queue (64).  Whether a session's messages go there is decided
//     once, at its SessionStart (the latch);
//   * the retail platform: every design 2.2 operation, each with the retail
//     column's means (physics_view, CK objects, the BML hooks' state), and
//     the host support templates (game/session_host_support.hpp) instantiated
//     with physics_view;
//   * the transition switch 'session core on|off' (design 6.2; off by
//     default) and the verbs 'session force-rollback E:D', 'net lag|blackout'
//     and 'replay session <file>' (the R steps' testing tools).
//
// Step R0 builds the plumbing only.  With 'session core on' the core is made
// at the first SessionStart and its platform checked (the constructor throws
// on a missing operation), and the mailbox is filled and drained; the session
// itself still runs on the legacy path (physics_session_client.cpp) until R1
// hands the frame to the core.  With 'session core off' nothing here touches
// a session: the legacy path does not read the switch.
//
// The BML constraint (design 3.2): BallanceMMOClient hangs the engine at
// startup when a data member is added to it, so everything this host keeps is
// a file-scope static here.  The mailbox is one from the DLL's load on (the
// network thread pushes into it at once); the core is made lazily.
#include "../BallanceMMOClient.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

#include <game/session_host_support.hpp>
#include <session/client_core.hpp>
#include <session/client_platform.hpp>
#include <session/rollback_binding_scripts.hpp>
#include <session/session_host_world.hpp>

#include "core_host.hpp"
#include "session_journal_client.hpp"

namespace {
    namespace bs = bmmo::session;
    using clock_type = std::chrono::steady_clock;

    // ------------------------------------------------------------ the switch
    // Written on the game thread (the verb), read by the network thread
    // (core_host_offer): atomic.  `core_latched` is the switch as a session's
    // SessionStart found it, read once there (on the network thread, where
    // the arrival order is made): that session's messages go to the mailbox
    // up to its SessionEnd, and the verb refuses a change meanwhile.
    std::atomic<bool> core_on{false};
    std::atomic<bool> core_latched{false};

    // ------------------------------------------------------------ the mailbox
    // Design 3.2 "Receive": every Session* opcode's bytes, in arrival order.  A
    // load-time object: the network thread pushes before any session exists.
    struct mailbox_entry {
        int code = 0;
        std::string bytes;
    };
    class core_mailbox {
    public:
        static constexpr size_t kMaxSnapshots = bs::physics_session_state::kMaxQueuedSnapshots;

        void push(int code, const void* data, size_t size) {
            std::lock_guard lk(mutex_);
            if (code == bmmo::SessionSnapshot) {
                // The legacy queue's cap (PSH kMaxQueuedSnapshots), on the
                // snapshot entries only: the oldest one goes.
                if (snapshots_ >= kMaxSnapshots) {
                    for (auto it = entries_.begin(); it != entries_.end(); ++it)
                        if (it->code == bmmo::SessionSnapshot) {
                            entries_.erase(it);
                            --snapshots_;
                            ++dropped_;
                            break;
                        }
                }
                ++snapshots_;
            }
            entries_.push_back({code, std::string(static_cast<const char*>(data), size)});
            ++pushed_;
        }
        std::deque<mailbox_entry> take() {
            std::lock_guard lk(mutex_);
            std::deque<mailbox_entry> out;
            out.swap(entries_);
            snapshots_ = 0;
            return out;
        }
        uint64_t pushed() const {
            std::lock_guard lk(mutex_);
            return pushed_;
        }
        uint64_t dropped() const {
            std::lock_guard lk(mutex_);
            return dropped_;
        }

    private:
        mutable std::mutex mutex_;
        std::deque<mailbox_entry> entries_;
        size_t snapshots_ = 0;
        uint64_t pushed_ = 0, dropped_ = 0;
    };
    core_mailbox mailbox;

    // ------------------------------------------------------------ host state
    // The retail platform's own state (design 3.2 "Game objects", "Camera",
    // the anchor's script replay), kept apart from the legacy
    // physics_session_state, which keeps its layout until R7.
    struct retail_host {
        // The core, made at the first SessionStart with the switch on; the
        // login id is given to it before each begin (design 3.2, E11).
        std::unique_ptr<bs::session_client_core> core;
        std::string core_error;   // why it could not be made
        uint64_t sessions_seen = 0;

        // The mailbox drain: what the game thread took, dispatched to the
        // core or - while no core session runs (R0: always) - decoded, to
        // check the copy, and dropped.
        uint64_t drained = 0, discarded = 0, undecodable = 0, starts = 0;

        // The navigation graph with retail's unbound-leaf filter (design
        // 4.1), the own replica (design 9.6), the own collision group and
        // the body guard (engine change #6).
        bmmo::game::navigation_graph navigation;
        bool navigation_known = false;
        bmmo::game::own_replica own_replica;
        bool own_group_set = false;
        bool body_guard = false;
        std::string body_guard_entity;
        // The pause menu's mute in force (the replica's polling follows it;
        // design 2.2 observe_input / unforce_own).
        bool input_muted = false;
        // The mirrors this host made: entity -> player, player -> ball type
        // (the spirit ball shown, objects_.on_trafo's "from").
        std::map<std::string, uint32_t> mirror_player;
        std::map<uint32_t, uint8_t> mirror_type;
        // Physicalize_GameBall's "Force" per ball type, read at the anchor
        // (the navigation's force; the legacy ball_forces).
        std::vector<float> ball_forces;

        // Every body this world made (bind_session_world's birth log; the
        // retail feed is OnPhysicalize, design 2.4), every manager
        // activation the live frames made (replayed inside rewinds), and the
        // script replay's selection and bookkeeping (made at the anchor).
        bs::local_birth_log births;
        bmmo::game::manager_activations activations;
        uint64_t activations_replayed = 0;
        bmmo::game::script_replay::selection script_ids;
        std::vector<CK_ID> scripts_replayed;
        bmmo::game::script_replay_hooks script_hooks;
        std::map<CK_ID, uint32_t> script_reset_ticks;
        uint64_t script_reset_skips = 0;
        uint32_t restore_logs = 0, savestate_logs = 0, contact_logs = 0, destroy_logs = 0, line_logs = 0;

        // 'session force-rollback E:D' (hooks.after_rewinds): every E ticks,
        // a rewind D ticks deep.
        uint32_t force_every = 0, force_depth = 12;
        uint64_t forced = 0;

        // 'net lag <ms> [up|down]' and 'net blackout <ms> [up|down]', each
        // now or 'at <tick>': GNS's fake lag and loss (global configuration
        // values), a deadline per direction.  A blackout ends at a frame
        // boundary (core_host_process), so an anchor hold in progress (which
        // waits inside one frame) stretches it.
        int lag_up_ms = 0, lag_down_ms = 0;
        bool blackout_up = false, blackout_down = false;
        clock_type::time_point blackout_up_until{}, blackout_down_until{};
        struct net_action {
            bool lag = false;   // lag, or else a blackout
            long ms = 0;
            bool up = false, down = false;
            uint32_t at = 0;
        };
        std::vector<net_action> net_pending;

        // 'replay session <file>': frame k of a key record fed to the input
        // poll of tick first_tick + k, first_tick being the session's first
        // assigned frame; no clock reset, nothing recorded.
        struct replay_state {
            bmmo::physics::tick_record source;
            std::string path;
            bool armed = false, started = false;
            uint32_t first_tick = 0, last_tick = 0;
            uint64_t fed = 0, jumps = 0;
            int64_t last_frame = -1;
        } replay;
    };
    retail_host host;

    const char* opcode_name(int code) {
        switch (code) {
        case bmmo::SessionStart: return "SessionStart";
        case bmmo::SessionAssign: return "SessionAssign";
        case bmmo::SessionSnapshot: return "SessionSnapshot";
        case bmmo::SessionRemoteInput: return "SessionRemoteInput";
        case bmmo::SessionEvent: return "SessionEvent";
        case bmmo::SessionEnd: return "SessionEnd";
        case bmmo::SessionLead: return "SessionLead";
        default: return "?";
        }
    }

    // A mailbox entry's bytes into its message (the bytes as the network
    // thread copied them, m_cbSize of them).
    template <class Msg>
    bool decode_bytes(const std::string& bytes, Msg& msg) {
        msg.raw.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return msg.deserialize();
    }

    // Whether an entry decodes as its opcode's message: the check the drain
    // makes of what it drops while no core session runs.
    bool entry_decodes(int code, const std::string& bytes) {
        switch (code) {
        case bmmo::SessionStart: { bmmo::session_start_msg m{}; return decode_bytes(bytes, m); }
        case bmmo::SessionAssign: { bmmo::session_assign_msg m{}; return decode_bytes(bytes, m); }
        case bmmo::SessionSnapshot: { bmmo::session_snapshot_msg m{}; return decode_bytes(bytes, m); }
        case bmmo::SessionRemoteInput: { bmmo::session_remote_input_msg m{}; return decode_bytes(bytes, m); }
        case bmmo::SessionEvent: { bmmo::session_event_msg m{}; return decode_bytes(bytes, m); }
        case bmmo::SessionLead: { bmmo::session_lead_msg m{}; return decode_bytes(bytes, m); }
        case bmmo::SessionEnd: { bmmo::session_end_msg m{}; return decode_bytes(bytes, m); }
        default: return false;
        }
    }

    // The level's ball table (Physicalize_GameBall): a ball's row by entity
    // name, -1 for none - the type the wire and the journal carry (the legacy
    // path reads it through db_.get_ball_id, whose map is filled from these
    // rows).
    int ball_row_of(IBML* bml, const std::string& name) {
        if (name.empty()) return -1;
        CKDataArray* table = bml->GetArrayByName("Physicalize_GameBall");
        if (!table) return -1;
        const int rows = table->GetRowCount();
        for (int row = 0; row < rows; ++row) {
            char text[128] = {};
            table->GetElementStringValue(row, 0, text);
            text[sizeof(text) - 1] = 0;
            if (name == text) return row;
        }
        return -1;
    }

    // The retail "physicalize new Ball" recipe of a ball type from its table
    // row (design 2.2 ball_recipe_of: the beam's and the own birth's recipe).
    // The headless host fills the same rule from the same columns
    // (session_client.cpp fill_retail_recipe); both are candidates for host
    // support (design 2.4), which R0 left unchanged.
    void fill_ball_recipe(IBML* bml, int row, bs::ball_recipe& r) {
        r = {};
        CKDataArray* table = row >= 0 ? bml->GetArrayByName("Physicalize_GameBall") : nullptr;
        if (!table || row >= table->GetRowCount()) return;
        char text[128] = {};
        table->GetElementStringValue(row, 0, text);
        text[sizeof(text) - 1] = 0;
        const std::string name = text;
        r.fixed = false;
        r.start_frozen = false;
        r.enable_collision = true;
        r.calc_mass_center = false;
        table->GetElementValue(row, 1, &r.friction);
        table->GetElementValue(row, 2, &r.elasticity);
        table->GetElementValue(row, 3, &r.mass);
        table->GetElementValue(row, 5, &r.linear_damp);
        table->GetElementValue(row, 6, &r.rot_damp);
        r.collision_surface = name + "_Mesh";
        if (name == "Ball_Paper") {
            r.convex_meshes.push_back(name + "_Mesh");
        } else {
            bs::ball_recipe::sphere sphere;
            sphere.radius = 2.0f;
            r.balls.push_back(sphere);
        }
    }

    float ball_force(uint8_t ball_type) {
        if (ball_type < host.ball_forces.size()) return host.ball_forces[ball_type];
        return host.navigation.leaves.empty() ? 0.0f : host.navigation.leaves.front().force_value;
    }

    // The next tick the world steps, when a session is running and numbered:
    // the core's with the switch on and a core session running, the legacy
    // state's otherwise.  The 'replay session' feed reads it at the input
    // poll, before the step.
    bool next_session_tick(const bs::physics_session_state& legacy, uint32_t& tick) {
        if (host.core && host.core->session_phase() == bs::client_core_state::phase::running) {
            if (!host.core->assigned() || host.core->holding()) return false;
            tick = host.core->next_tick();
            return true;
        }
        if (legacy.phase != bs::physics_session_state::phase_type::running || !legacy.assigned || legacy.hold_active)
            return false;
        tick = legacy.tick_base + static_cast<uint32_t>(std::max<int64_t>(legacy.frames_since_anchor, 0));
        return true;
    }

    // The tick the world stepped last (the 'net' verbs' "from tick" and
    // their 'at <tick>'): the core's with a core session running, the legacy
    // state's otherwise.
    uint32_t session_tick_now(const bs::physics_session_state& legacy) {
        if (host.core && host.core->session_phase() != bs::client_core_state::phase::idle)
            return host.core->current_tick();
        return legacy.current_tick();
    }

    std::string join_flags(bool up, bool down) {
        if (up && down) return "both";
        if (up) return "up";
        if (down) return "down";
        return "none";
    }
}

bool bmmo::session::retail::core_enabled() {
    return core_on.load();
}

// ---------------------------------------------------------------- network thread

// Design 3.2 "Receive": every Session* message of a session whose SessionStart
// found the switch on goes into the mailbox as its bytes, SessionStart first,
// so the game thread sees them in arrival order.  The legacy handlers still
// run in R0 (the core hosts nothing yet); SessionLead, which the legacy path
// never dispatched, is the mailbox's alone (true: the caller stops there).
// With the switch off and no session latched: two atomic loads, false.
bool BallanceMMOClient::core_host_offer(ISteamNetworkingMessage* network_msg) {
    if (!core_on.load() && !core_latched.load()) return false;
    const auto* raw = reinterpret_cast<const bmmo::general_message*>(network_msg->m_pData);
    if (raw->code == bmmo::SessionStart) {
        // The switch read once per session, here.
        const bool on = core_on.load();
        core_latched.store(on);
        if (on) mailbox.push(static_cast<int>(raw->code), network_msg->m_pData, static_cast<size_t>(network_msg->m_cbSize));
        return false;
    }
    if (!core_latched.load()) return false;
    switch (raw->code) {
    case bmmo::SessionEnd:
        mailbox.push(static_cast<int>(raw->code), network_msg->m_pData, static_cast<size_t>(network_msg->m_cbSize));
        core_latched.store(false);
        return false;
    case bmmo::SessionAssign:
    case bmmo::SessionSnapshot:
    case bmmo::SessionRemoteInput:
    case bmmo::SessionEvent:
    case bmmo::SessionLead:
        mailbox.push(static_cast<int>(raw->code), network_msg->m_pData, static_cast<size_t>(network_msg->m_cbSize));
        return raw->code == bmmo::SessionLead;
    default:
        return false;
    }
}

// ---------------------------------------------------------------- game thread

// A latched session's SessionStart, taken from the mailbox in arrival order:
// the core is made the first time, its platform checked by the constructor,
// and the login id handed over.  R1 calls begin here.
void BallanceMMOClient::core_host_session_start() {
    ++host.sessions_seen;
    if (host.core) {
        host.core->set_own_id(db_.get_client_id());
        logger_->Info("Physics session core: SessionStart, own id %u (step R0: the session still runs on the legacy "
                      "path)", db_.get_client_id());
        return;
    }
    bs::session_capabilities caps;
    caps.own_body_control = false;   // the game's Physicalize block owns the own ball (design 2.3)
    caps.slew = false;               // no fixed-tick slew until R5: SessionReady caps = 0
    caps.nonblocking_hold = false;   // the anchor hold blocks inside OnProcess
    caps.whole_frame = false;
    caps.own_lifecycle = bs::own_lifecycle_source::host_hooks;   // OnPhysicalize / OnUnphysicalize
    try {
        host.core = std::make_unique<bs::session_client_core>(core_host_platform(), caps);
        host.core_error.clear();
    } catch (const std::exception& e) {
        host.core_error = e.what();
        logger_->Error("Physics session core: not made: %s", e.what());
        return;
    }
    host.core->set_own_id(db_.get_client_id());
    logger_->Info("Physics session core: made, platform complete, own id %u (step R0: the session still runs on the "
                  "legacy path)", db_.get_client_id());
}

// Every OnProcess, after the legacy session's frame: the 'net' actions armed
// for a tick, the blackouts' ends, and the mailbox drained in arrival order -
// a SessionStart to core_host_session_start, the rest dispatched to a running
// core session (R0 has none: each is decoded, to check the copy, counted and
// dropped).
void BallanceMMOClient::core_host_process() {
    if (!host.net_pending.empty()) {
        const uint32_t tick = session_tick_now(physics_session_);
        for (auto it = host.net_pending.begin(); it != host.net_pending.end();) {
            if (tick < it->at) { ++it; continue; }
            const auto action = *it;
            it = host.net_pending.erase(it);
            const std::string direction = join_flags(action.up, action.down);
            const std::string outcome = core_host_net_verb(std::format("{} {} {}", action.lag ? "lag" : "blackout",
                                                                       action.ms, direction));
            logger_->Info("Automation: net %s %ld ms (%s) armed for tick %u, at tick %u: %s", action.lag ? "lag" : "blackout",
                          action.ms, direction.c_str(), action.at, tick, outcome.c_str());
        }
    }
    if (host.blackout_up || host.blackout_down) {
        const auto now = clock_type::now();
        auto* utils = SteamNetworkingUtils();
        if (host.blackout_up && now >= host.blackout_up_until) {
            utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, 0.0f);
            host.blackout_up = false;
            logger_->Info("Automation: net blackout (up) over at tick %u", session_tick_now(physics_session_));
        }
        if (host.blackout_down && now >= host.blackout_down_until) {
            utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Recv, 0.0f);
            host.blackout_down = false;
            logger_->Info("Automation: net blackout (down) over at tick %u", session_tick_now(physics_session_));
        }
    }
    auto entries = mailbox.take();
    for (auto& entry: entries) {
        ++host.drained;
        if (entry.code == bmmo::SessionStart) {
            ++host.starts;
            if (!entry_decodes(entry.code, entry.bytes)) {
                if (host.undecodable++ < 8) logger_->Warn("Physics session core: a SessionStart could not be decoded");
                continue;
            }
            core_host_session_start();
            continue;
        }
        if (!host.core || host.core->session_phase() == bs::client_core_state::phase::idle) {
            ++host.discarded;
            if (!entry_decodes(entry.code, entry.bytes) && host.undecodable++ < 8)
                logger_->Warn("Physics session core: a %s (%zu bytes) could not be decoded", opcode_name(entry.code),
                              entry.bytes.size());
            continue;
        }
        core_host_dispatch(entry.code, entry.bytes);
    }
}

// One mailbox entry, decoded and handed to the core (design 1.3 "Messages").
void BallanceMMOClient::core_host_dispatch(int code, const std::string& bytes) {
    auto& core = *host.core;
    const auto decode = [&](auto& msg) {
        if (decode_bytes(bytes, msg)) return true;
        if (host.undecodable++ < 8) logger_->Warn("Physics session core: a %s could not be decoded", opcode_name(code));
        return false;
    };
    switch (code) {
    case bmmo::SessionAssign: {
        bmmo::session_assign_msg msg{};
        if (decode(msg)) core.on_session_assign(msg);
        break;
    }
    case bmmo::SessionSnapshot: {
        bmmo::session_snapshot_msg msg{};
        if (decode(msg)) core.on_session_snapshot(msg);
        break;
    }
    case bmmo::SessionRemoteInput: {
        bmmo::session_remote_input_msg msg{};
        if (decode(msg)) core.on_session_remote_input(msg);
        break;
    }
    case bmmo::SessionEvent: {
        bmmo::session_event_msg msg{};
        if (decode(msg)) core.on_session_event(std::move(msg));
        break;
    }
    case bmmo::SessionLead: {
        bmmo::session_lead_msg msg{};
        if (decode(msg)) core.on_session_lead(msg);
        break;
    }
    case bmmo::SessionEnd: {
        bmmo::session_end_msg msg{};
        if (decode(msg)) core.on_session_end(msg);
        break;
    }
    default:
        break;
    }
}

// A manager activation recorded for `tick`, re-issued (inside a replay, and
// for the frame about to run after one).
void BallanceMMOClient::core_host_replay_activation(uint32_t tick) {
    CKContext* context = m_bml->GetCKContext();
    CKBehavior* manager = bmmo::game::find_root_script(context, "Gameplay_SectorManager");
    if (!manager) return;
    if (host.activations.replay(context, manager->GetID(), tick)) ++host.activations_replayed;
}

// The retail SetPhysicsForce leaves push zero while the own replica drives the
// ball (the legacy physics_session_zero_retail_forces over the host's graph).
void BallanceMMOClient::core_host_zero_retail_forces() {
    CKContext* context = m_bml->GetCKContext();
    const float zero = 0.0f;
    for (const auto& leaf: host.navigation.leaves) {
        auto* block = CKBehavior::Cast(context->GetObject(leaf.force_block));
        if (!block) continue;
        if (CKParameterIn* in = block->GetInputParameter(4))
            if (CKParameter* source = in->GetRealSource()) source->SetValue(&zero, sizeof(zero));
    }
}

// ---------------------------------------------------------------- the platform

// Design 2.2, the retail column: every operation the core calls, each with the
// means the legacy retail session uses for the same thing today.
bmmo::session::session_platform BallanceMMOClient::core_host_platform() {
    bs::session_platform p;

    // ---- transport and time ----
    p.send = [this](const std::string& bytes, bs::send_mode mode) {
        int flags = k_nSteamNetworkingSend_Reliable;
        if (mode == bs::send_mode::reliable_no_nagle) flags = k_nSteamNetworkingSend_ReliableNoNagle;
        else if (mode == bs::send_mode::unreliable_no_delay) flags = k_nSteamNetworkingSend_UnreliableNoDelay;
        std::string copy = bytes;   // client::send takes a mutable buffer
        send(copy.data(), copy.size(), flags);
    };
    p.now = [](bs::clock_site) { return clock_type::now(); };
    p.log = [this](bs::log_level level, const std::string& text) {
        if (level == bs::log_level::warn) logger_->Warn("%s", text.c_str());
        else logger_->Info("%s", text.c_str());
    };
    p.utc_ms = [](bs::clock_site) {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    };

    // ---- reading the world ----
    p.body_state = [this](const std::string& entity, bmmo_physics_body_state& out) {
        std::string error;
        return physics_view_.get_body_state(entity.c_str(), out, error);
    };
    p.list_bodies = [this]() { return physics_view_.list_bodies(); };
    p.sector_manager_active = [this]() { return physics_session_sector_manager_active(); };
    // The cell is read only while the checkpoint hold is in force: otherwise
    // the checkpoint hook has reported the crossing after the fact already
    // (design 2.2 poll_sector).  The resetpoint is not watched (the hook
    // reports through request_sector).
    p.poll_sector = [this](uint32_t) {
        bs::sector_poll poll;
        if (sector_activate_.block && !sector_activate_failed_) {
            if (CKDataArray* parameters = m_bml->GetArrayByName("IngameParameter")) {
                int cell = 0;
                poll.cell_read = parameters->GetElementValue(0, 1, &cell) != 0;
                poll.cell = cell;
                poll.checkpoint_held = poll.cell_read;
            }
        }
        if (CKDataArray* level = m_bml->GetArrayByName("CurrentLevel")) {
            int phase = 0;
            poll.phase_read = level->GetElementValue(0, 4, &phase) != 0;
            poll.phase = phase;
        }
        return poll;
    };
    p.world_hash = [this](bmmo::physics::world_hash& out) {
        std::string error;
        return physics_view_.capture(out, error);
    };
    p.own_ball = [this]() {
        bs::own_ball_view own;
        CK3dObject* ball = get_current_ball();
        own.entity = ball && ball->GetName() ? ball->GetName() : "";
        own.ball_type = ball_row_of(m_bml, own.entity);
        std::string error;
        own.physicalized = !own.entity.empty() && physics_view_.get_body_state(own.entity.c_str(), own.state, error);
        return own;
    };
    // The body guard on the current ball (engine change #6), the own group,
    // and the own replica - always attached on retail (design 4.1), its
    // polling following the mute.
    p.own_body_upkeep = [this](const bs::own_ball_view& own) {
        std::string error;
        const std::string& ball_name = own.entity;
        if (!ball_name.empty() && (!host.body_guard || host.body_guard_entity != ball_name)) {
            if (physics_view_.set_body_guard(true, ball_name.c_str(), error)) {
                host.body_guard = true;
                host.body_guard_entity = ball_name;
            } else {
                logger_->Warn("Physics session core: body guard: %s", error.c_str());
            }
        }
        if (!own.physicalized) {
            host.own_group_set = false;
            return;
        }
        if (!host.own_group_set) {
            const std::string group = host.core ? host.core->own_group() : std::string();
            if (physics_view_.set_body_group(ball_name.c_str(), group.c_str(), error)) host.own_group_set = true;
            else logger_->Warn("Physics session: own ball group: %s", error.c_str());
        }
        if (!host.navigation_known || (host.own_replica.active && host.own_replica.entity == ball_name)) return;
        const uint8_t type = static_cast<uint8_t>(own.ball_type < 0 ? 0 : own.ball_type);
        switch (bmmo::game::attach_own_replica(physics_view_, host.navigation, host.own_replica, ball_name,
                                               ball_force(type), error)) {
        case bmmo::game::own_replica_attach::created:
            if (host.input_muted
                    && !physics_view_.navigation_poll(ball_name.c_str(), false, host.own_replica.key_codes,
                                                      host.own_replica.key_blocks, host.own_replica.key_count, error))
                logger_->Warn("Physics session: own ball navigation replica: %s", error.c_str());
            logger_->Info("Physics session: own ball %s driven by the navigation replica (type %u, force %.3f)",
                          ball_name.c_str(), type, ball_force(type));
            break;
        case bmmo::game::own_replica_attach::failed:
            logger_->Warn("Physics session: own ball navigation replica: %s", error.c_str());
            break;
        default:
            break;
        }
    };
    p.own_replica_name = []() { return host.own_replica.entity; };
    // Retail's leaf rule (design 4.1): unbound leaves other mods graft onto
    // Ball Navigation are dropped and the rest renumbered; four keyed leaves
    // make the graph known (the legacy frame's rule, PSC "Key bindings").
    p.discover_navigation = [this](uint32_t tick) {
        if (host.navigation_known) return false;
        auto graph = bmmo::game::read_navigation_graph(m_bml->GetCKContext());
        if (graph.valid()) {
            graph.leaves.erase(std::remove_if(graph.leaves.begin(), graph.leaves.end(),
                                              [](const bmmo::game::navigation_leaf& leaf) { return leaf.key == 0; }),
                               graph.leaves.end());
            for (size_t i = 0; i < graph.leaves.size(); ++i) graph.leaves[i].index = static_cast<int>(i);
        }
        if (!graph.valid() || graph.leaves.size() < 4) return false;
        host.navigation = graph;
        host.navigation_known = true;
        logger_->Info("Physics session: %zu navigation leaves at tick %u, keys %d/%d/%d/%d", graph.leaves.size(), tick,
                      graph.leaves[0].key, graph.leaves[1].key, graph.leaves[2].key, graph.leaves[3].key);
        return true;
    };
    // The bridge's event log: created / deleted / script_wakeup to the core
    // in order (an own body made or gone sets the own group again); the
    // diagnostics under trace.  The birth recipes are not kept from here: the
    // retail birth log is fed by OnPhysicalize (design 2.4).
    p.drain_body_events = [this](uint32_t tick, const std::function<bool(const bs::body_event&)>& each) {
        const std::string events = physics_view_.drain_event_log();
        const bool trace = host.core && host.core->config.trace;
        bs::parse_event_log(events, [&](const std::string& kind, const std::string& name) {
            if (name.empty()) return;
            bs::body_event event;
            if (kind == "created") event.what = bs::body_event::kind::created;
            else if (kind == "deleted") event.what = bs::body_event::kind::deleted;
            else if (kind == "script_wakeup") event.what = bs::body_event::kind::wakeup;
            else {
                if (trace && (kind == "constraint" || kind == "birth" || kind == "psi" || kind == "force"))
                    logger_->Info("Physics session: %s %s at tick %u", kind.c_str(), name.c_str(), tick);
                return;
            }
            event.name = name;
            if (each(event)) host.own_group_set = false;
        });
    };
    p.reportable_wakeup = [](const std::string& name, const std::string& own_ball) {
        return name.rfind("Ball_", 0) != 0 && name != own_ball && name.find("_Peer_") == std::string::npos;
    };
    // What the own replica used for the step just taken: the polled keys
    // (none while the mute is in force), the camera reference's rows, and the
    // leaves' Key Events' activity - the source the replica polls
    // (ball_navigation.cpp), not the BML hook's ball_nav_active_ (design 2.2).
    p.observe_input = [this]() {
        bs::input_observation seen;
        if (!host.input_muted && host.navigation_known && input_hook_installed_)
            seen.keys = host.navigation.keys_from_state(frame_keys_.data());
        if (auto* orient = m_bml->Get3dEntityByName("Cam_OrientRef")) {
            const VxMatrix& m = orient->GetWorldMatrix();
            for (int r = 0; r < 3; ++r)
                for (int k = 0; k < 3; ++k) seen.cam[r][k] = m[r][k];
            seen.cam_valid = true;
        }
        if (host.navigation_known) {
            CKContext* context = m_bml->GetCKContext();
            for (const auto& leaf: host.navigation.leaves)
                if (auto* key_event = CKBehavior::Cast(context->GetObject(leaf.key_block)))
                    if (key_event->IsActive()) { seen.nav_active = true; break; }
        }
        return seen;
    };
    p.own_camera_rows = [this](const float (*basis)[3]) {
        if (!host.own_replica.active) return;
        std::string error;
        if (!physics_view_.navigation_input(host.own_replica.entity.c_str(), 0, basis[0], basis[1], basis[2], false, error))
            logger_->Warn("Physics session: own navigation rows: %s", error.c_str());
        core_host_zero_retail_forces();
    };
    p.movable_mechanisms = [this]() {
        std::vector<std::string> names;
        for (const auto& body: physics_view_.list_bodies()) {
            if (!body.movable || !body.name[0]) continue;
            if (std::strncmp(body.name, "Ball_", 5) == 0) continue;
            names.emplace_back(body.name);
        }
        return names;
    };
    p.note_script_resets = [this](uint32_t tick) {
        for (CK_ID id: bmmo::game::script_replay::resets_pending(m_bml->GetCKContext(), host.script_ids))
            host.script_reset_ticks[id] = tick;
    };
    // N1-N3 kept neutralized, and N4's split kept (design 1.3 step 4; the
    // clock guard and the pause edits stay host code at the top of the frame).
    p.script_guards = [this](uint32_t, bool resolved) {
        death_reset_apply(resolved);
        sector_deactivate_apply();
        sector_activate_apply();
        if (const int again = checkpoint_phase_keep())
            logger_->Info("Physics session: the checkpoint chain's phase cell: %d target(s) handed back to "
                          "CurrentLevel were moved to the copy again", again);
    };

    // ---- writing the world ----
    p.write_body = [this](const std::string& entity, const double position[3], const double rotation[4],
                          const float linear[3], const float angular[3], bool wake, std::string& error) {
        error.clear();
        return physics_view_.set_body_state(entity.c_str(), position, rotation, linear, angular, wake, error);
    };
    // One union pass: deactivate nothing, activate `sector`, run the manager
    // from its start (the legacy physics_session_run_sector's writes; the
    // core records the activation and logs).
    p.run_sector = [this](int sector) {
        CKContext* context = m_bml->GetCKContext();
        CKDataArray* parameters = m_bml->GetArrayByName("IngameParameter");
        CKBehavior* manager = bmmo::game::find_root_script(context, "Gameplay_SectorManager");
        if (!parameters || !manager) {
            logger_->Error("Physics session: sector %d could not be activated: IngameParameter or "
                           "Gameplay_SectorManager is missing", sector);
            return;
        }
        int none = 0;
        parameters->SetElementValue(0, 2, &none, sizeof(none));
        parameters->SetElementValue(0, 1, &sector, sizeof(sector));
        // A peer's sector in the cell is not this ball's (update_current_sector).
        if (sector != current_sector_) sector_cell_is_own_ = false;
        if (CKScene* scene = context->GetCurrentScene()) scene->Activate(manager, TRUE);
    };
    p.record_activation = [this](uint32_t tick, bool reset) {
        host.activations.record(m_bml->GetCKContext(), tick, reset);
    };
    p.record_pending_activation = [this](uint32_t tick) {
        if (CKBehavior* manager = bmmo::game::find_root_script(m_bml->GetCKContext(), "Gameplay_SectorManager");
                manager && (manager->GetFlags() & CKBEHAVIOR_ACTIVATENEXTFRAME))
            host.activations.record(m_bml->GetCKContext(), tick, (manager->GetFlags() & CKBEHAVIOR_RESETNEXTFRAME) != 0);
    };

    // ---- mirrors: the game_objects spirit balls ----
    p.mirror_entity = [this](uint32_t player, uint8_t ball_type, std::string& error) {
        objects_.ensure_player(player);
        CK3dObject* entity = objects_.get_ball_entity(player, ball_type);
        if (!entity || !entity->GetName()) {
            error = std::format("no spirit ball for player {} type {}", player, ball_type);
            return std::string();
        }
        return std::string(entity->GetName());
    };
    p.make_mirror = [this](const bs::mirror_spec& spec) {
        bs::mirror_made made;
        objects_.ensure_player(spec.player);
        CK3dObject* entity = objects_.get_ball_entity(spec.player, spec.ball_type);
        if (!entity || !entity->GetName() || spec.entity != entity->GetName()) {
            made.error = "no spirit ball named '" + spec.entity + "'";
            return made;
        }
        made = bmmo::game::make_mirror_body(physics_view_, entity, spec);
        if (!made.made) return made;
        const auto shown = host.mirror_type.find(spec.player);
        const uint32_t previous = shown != host.mirror_type.end() && shown->second != spec.ball_type
                ? shown->second : std::numeric_limits<uint32_t>::max();
        host.mirror_type[spec.player] = spec.ball_type;
        host.mirror_player[spec.entity] = spec.player;
        objects_.set_physicalized(spec.player, true);
        objects_.on_trafo(spec.player, previous, spec.ball_type);
        return made;
    };
    p.unmake_mirror = [this](const std::string& entity) {
        std::string error;
        physics_view_.unphysicalize(entity.c_str(), error);
        if (const auto it = host.mirror_player.find(entity); it != host.mirror_player.end())
            objects_.set_physicalized(it->second, false);
    };
    p.detach_mirror_navigation = [this](const std::string& entity) {
        std::string error;
        physics_view_.navigation_destroy(entity.c_str(), error);
    };
    p.attach_mirror_navigation = [this](uint32_t player, const std::string& entity, uint8_t ball_type, std::string& error) {
        if (!host.navigation_known) return bs::mirror_navigation::unavailable;
        return bmmo::game::attach_mirror_navigation(physics_view_, host.navigation, entity, player, ball_force(ball_type), error);
    };
    p.mute_event_log = [this](bool muted) {
        std::string error;
        physics_view_.set_event_log_muted(muted, error);
    };

    // ---- driving ----
    p.own_driven_entity = []() { return host.own_replica.active ? host.own_replica.entity : std::string(); };
    p.drive = [this](const std::string& entity, const bs::input_frame& frame, std::string& error) {
        const bool active = (frame.flags & bs::INPUT_FLAG_NAV_ACTIVE) != 0;
        return physics_view_.navigation_input(entity.c_str(), frame.keys, frame.cam_right, frame.cam_up, frame.cam_dir,
                                              active, error);
    };
    p.force_own = [this](const bs::input_frame& held, bool& ok, std::string& error) {
        if (!host.own_replica.active) return std::string();
        const std::string entity = host.own_replica.entity;
        ok = physics_view_.navigation_poll(entity.c_str(), false, host.own_replica.key_codes, host.own_replica.key_blocks,
                                           host.own_replica.key_count, error)
          && physics_view_.navigation_input(entity.c_str(), held.keys, held.cam_right, held.cam_up, held.cam_dir,
                                            (held.flags & bs::INPUT_FLAG_NAV_ACTIVE) != 0, error);
        return entity;
    };
    // The polling comes back as the mute says, never plain on (design 2.2).
    p.unforce_own = [this](const std::string& entity, std::string& error) {
        return physics_view_.navigation_poll(entity.c_str(), !host.input_muted, host.own_replica.key_codes,
                                             host.own_replica.key_blocks, host.own_replica.key_count, error);
    };
    // arm_own_capture, own_birth_capture, make_own, unmake_own: empty (the
    // own births come from OnPhysicalize, and the game's Physicalize block
    // owns the own ball: design 2.3).

    // ---- own ball ----
    p.ball_recipe_of = [this](const std::string& entity, bs::ball_recipe& recipe) {
        const int row = ball_row_of(m_bml, entity);
        fill_ball_recipe(m_bml, row, recipe);
        return row;
    };

    // ---- rollback ----
    // The shared binding (design 2.4) with the retail options of the legacy
    // binding (physics_session_rollback_world): no clock rewind, the lenient
    // savestate, the mute, the camera re-arm after a replay.
    p.bind_world = [this](const bs::core_world_hooks& hooks) {
        bs::session_world_options w;
        bs::rollback_binding_options& o = w.binding;
        o.delta_ms = 1000.0f / 66.0f;
        o.restore_clock = false;
        o.savestate_flags = 0u;
        o.own_key_codes = host.own_replica.key_codes;
        o.own_key_blocks = host.own_replica.key_blocks;
        o.own_key_count = host.own_replica.key_count;
        o.input_muted = []() { return host.input_muted; };
        o.on_write_failed = [this](const std::string& error) {
            if (host.line_logs++ < 16) logger_->Warn("Physics session: write failed: %s", error.c_str());
        };
        o.on_contacts_restored = [this](const std::string& entity, int, const std::string& error,
                                        const std::vector<bmmo_physics_contact>&) {
            if (!error.empty() && host.contact_logs++ < 8)
                logger_->Warn("Physics session: contact restore of %s: %s", entity.c_str(), error.c_str());
        };
        o.on_savestate_capture_failed = [this](const std::string& error) {
            if (host.savestate_logs++ < 8) logger_->Warn("Physics session: savestate capture failed: %s", error.c_str());
        };
        o.on_savestate_restored = [this](bool ok, const std::string& error, const bs::savestate_outcome&) {
            if (!ok && host.savestate_logs++ < 8) logger_->Warn("Physics session: savestate restore failed: %s", error.c_str());
        };
        o.on_poll_resumed = [this](const std::string&) {
            const float (*cam)[3] = host.core ? host.core->previous_cam() : nullptr;
            if (!host.own_replica.active || !cam) return;
            std::string error;
            physics_view_.navigation_input(host.own_replica.entity.c_str(), 0, cam[0], cam[1], cam[2], false, error);
        };
        o.log = [this](const std::string& text) {
            if (host.line_logs++ < 200) logger_->Info("Physics session: %s", text.c_str());
        };
        w.replay_activation = [this](uint32_t tick) { core_host_replay_activation(tick); };
        w.births = &host.births;
        w.park_births = false;
        w.insert_births = false;
        w.put_back = [this](const std::string& entity, const bs::local_birth& birth) {
            bmmo::game::put_back_birth(m_bml->GetCKContext(), entity, birth);
        };
        w.on_own_destroy_failed = [this](const std::string& entity) {
            if (host.destroy_logs++ < 8) logger_->Warn("Physics session: own body %s could not be destroyed at the restore", entity.c_str());
        };
        w.on_destroy_failed = [this](const std::string& entity, const std::string& error) {
            if (host.destroy_logs++ < 8)
                logger_->Warn("Physics session: birth: destroying %s at the restore failed: %s", entity.c_str(), error.c_str());
        };
        if (!host.script_ids.empty()) w.bind_scripts = [this](bs::rollback_world& world) {
            bs::script_binding_options so;
            so.context = m_bml->GetCKContext();
            so.scripts = &host.script_ids;
            so.hooks = &host.script_hooks;
            so.scripts_replayed = &host.scripts_replayed;
            so.reset_ticks = &host.script_reset_ticks;
            so.reset_skips = &host.script_reset_skips;
            so.delta_ms = 1000.0f / 66.0f;
            so.diff_detail = host.core && host.core->config.trace;
            so.replay_activation = true;
            so.on_restore_failed = [this](const std::string& error) {
                if (host.restore_logs++ < 8) logger_->Warn("Physics session: script replay: restore failed: %s", error.c_str());
            };
            bs::bind_script_replay(world, physics_view_, so);
        };
        return bs::bind_session_world(physics_view_, hooks, w);
    };
    p.keep_live_cells = [this](const bs::session_client_core& core) {
        auto guard = std::make_shared<bmmo::game::live_cells_guard>(
                m_bml->GetCKContext(), core, true, [this](uint32_t tick) { core_host_replay_activation(tick); });
        return bs::scope_guard([guard]() mutable { guard.reset(); });
    };

    // ---- journal: ModLoader/BMMOJournals, the newest 10 kept, and only
    // with the SessionJournal property on (read once per session, as the
    // legacy path does) ----
    p.journal_path = [this](const bs::journal_header& header) -> std::filesystem::path {
        if (!config_manager_["session_journal"]->GetBoolean()) return {};
        const auto directory = bs::client_journal::directory();
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        bs::client_journal::prune(directory, bs::client_journal::kKeepFiles - 1);
        return directory / bs::client_journal::file_name(header.session, header.level, header.own_player);
    };

    // ---- reports and hooks ----
    p.counters = [this]() {
        bs::host_counters h;
        h.rebases = static_cast<int>(fixed_tick_.rebases());
        h.activations_replayed = host.activations_replayed;
        h.script_reset_skips = host.script_reset_skips;
        return h;
    };
    p.hooks.window = [this](const std::string& text) { logger_->Info("%s", text.c_str()); };
    p.hooks.status = [this](const std::string& text) { logger_->Info("%s", text.c_str()); };
    // 'session force-rollback E:D' (design 3.1's --force-rollback, the retail
    // verb): after the frame's own rewinds, every E ticks, D deep.
    p.hooks.after_rewinds = [this](uint32_t) {
        if (!host.force_every || !host.core) return;
        // The cadence is the verb's; whether the rewind can run this early
        // is the core's (force_rewind does nothing before depth + 2 ticks).
        const uint32_t now = host.core->current_tick();
        if (now % host.force_every != 0) return;
        const auto forced = host.core->force_rewind(host.force_depth);
        if (forced.rolled && host.forced++ < 16)
            logger_->Info("Physics session: forced rewind at tick %u, %u deep", now, host.force_depth);
    };
    // The own body guard lifted after the journal closed (design 2.2
    // hooks.ended).
    p.hooks.ended = [this]() {
        std::string error;
        if (!host.body_guard) return;
        physics_view_.set_body_guard(false, nullptr, error);
        host.body_guard = false;
        host.body_guard_entity.clear();
    };
    return p;
}

// ---------------------------------------------------------------- the verbs

// session core on|off|status, session force-rollback E:D.
std::string BallanceMMOClient::core_host_session_verb(const std::string& rest) {
    std::istringstream args(rest);
    std::string what, value;
    args >> what >> value;
    if (what == "core") {
        if (value == "on" || value == "off") {
            const bool on = value == "on";
            // The switch takes effect between sessions only: not while the
            // legacy path or the core runs one, nor while a session's
            // messages are latched to the mailbox (its SessionStart came,
            // its SessionEnd not yet; a lost connection ends that).
            if (physics_session_.phase != bs::physics_session_state::phase_type::idle)
                return "error the switch takes effect between sessions: end this one first";
            if (host.core && host.core->session_phase() != bs::client_core_state::phase::idle)
                return "error the switch takes effect between sessions: the core runs one";
            if (core_latched.load() && connected())
                return "error the switch takes effect between sessions: one has started (SessionStart came, SessionEnd "
                       "not yet)";
            core_latched.store(false);
            core_on.store(on);
            if (!on) return "ok session core off: the next session runs on the legacy path";
            return "ok session core on (step R0: the core is made and its platform checked at the next SessionStart, "
                   "and the mailbox fills; the session still runs on the legacy path until R1)";
        }
        if (!value.empty() && value != "status") return "error usage: session core on|off|status";
        return std::format("ok core={} latched={} made={} error='{}' sessions={} mailbox: pushed={} dropped={} "
                           "drained={} starts={} discarded={} undecodable={}",
                           core_on.load() ? "on" : "off", core_latched.load() ? 1 : 0, host.core ? 1 : 0,
                           host.core_error, host.sessions_seen, mailbox.pushed(), mailbox.dropped(), host.drained,
                           host.starts, host.discarded, host.undecodable);
    }
    if (what == "force-rollback") {
        const size_t colon = value.find(':');
        const long every = std::strtol(value.c_str(), nullptr, 10);
        const long depth = colon == std::string::npos ? 12 : std::strtol(value.c_str() + colon + 1, nullptr, 10);
        if (value.empty() || every < 0 || depth < 1 || depth > 1000)
            return "error usage: session force-rollback <every>:<depth> (0: off)";
        host.force_every = static_cast<uint32_t>(every);
        host.force_depth = static_cast<uint32_t>(depth);
        if (!host.force_every) return "ok forced rollbacks off";
        return std::format("ok a forced rewind {} ticks deep every {} ticks{}", host.force_depth, host.force_every,
                           core_on.load() ? "" : " (with session core on: the legacy path has no forced rollback)");
    }
    return "error usage: session core on|off|status | session force-rollback <every>:<depth>";
}

// net lag <ms> [up|down] [at <tick>] | net blackout <ms> [up|down] [at <tick>]
// | net: GNS's fake lag (one way, in one or both directions) and a loss of
// every packet in one or both directions for a while (the retail counterpart
// of the headless BMMO_FAKE_LAG_MS).  'at <tick>' arms it for the first frame
// boundary at or after that session tick (core_host_process).  A blackout
// ends at a frame boundary, so an anchor hold in progress stretches it, and
// one as long as GNS's connected timeout drops the connection.
std::string BallanceMMOClient::core_host_net_verb(const std::string& rest) {
    std::istringstream args(rest);
    std::string what;
    long ms = -1;
    args >> what >> ms;
    std::string direction, word;
    long at = -1;
    while (args >> word) {
        if (word == "at") {
            if (!(args >> at) || at < 0) return "error usage: ... at <tick>";
        } else if (direction.empty()) {
            direction = word;
        } else {
            return "error usage: net lag|blackout <ms> [up|down] [at <tick>]";
        }
    }
    const bool up = direction.empty() || direction == "up" || direction == "both";
    const bool down = direction.empty() || direction == "down" || direction == "both";
    auto* utils = SteamNetworkingUtils();
    if (what == "lag" || what == "blackout") {
        const bool lag = what == "lag";
        if (lag ? ms < 0 || ms > 10000 : ms <= 0 || ms > 60000)
            return "error usage: net lag <ms> [up|down] [at <tick>] (one way; 0: none) | net blackout <ms> [up|down] "
                   "[at <tick>]";
        if (!up && !down) return "error usage: net " + what + " <ms> [up|down] [at <tick>]";
        if (at >= 0) {
            host.net_pending.push_back({lag, ms, up, down, static_cast<uint32_t>(at)});
            return std::format("ok net {} {} ms ({}) armed for tick {} (now {})", what, ms, join_flags(up, down), at,
                               session_tick_now(physics_session_));
        }
    }
    if (what == "lag") {
        const int32_t value = static_cast<int32_t>(ms);
        bool ok = true;
        if (up) ok = utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Send, value) && ok;
        if (down) ok = utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Recv, value) && ok;
        if (!ok) return "error GNS refused the fake lag";
        if (up) host.lag_up_ms = value;
        if (down) host.lag_down_ms = value;
        const uint32_t tick = session_tick_now(physics_session_);
        logger_->Info("Automation: net lag %d ms (%s) from tick %u", value, join_flags(up, down).c_str(), tick);
        return std::format("ok net lag {} ms ({}) from tick {}: up {} ms, down {} ms", value, join_flags(up, down), tick,
                           host.lag_up_ms, host.lag_down_ms);
    }
    if (what == "blackout") {
        bool ok = true;
        if (up) ok = utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, 100.0f) && ok;
        if (down) ok = utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Recv, 100.0f) && ok;
        if (!ok) return "error GNS refused the fake loss";
        const auto until = clock_type::now() + std::chrono::milliseconds(ms);
        if (up) {
            host.blackout_up = true;
            host.blackout_up_until = until;
        }
        if (down) {
            host.blackout_down = true;
            host.blackout_down_until = until;
        }
        // GNS's connected timeout (role.hpp sets 7.2 s): a blackout that
        // long drops the connection, not just the link.
        int32_t timeout = 0;
        size_t size = sizeof(timeout);
        ESteamNetworkingConfigDataType type{};
        std::string warning;
        if (utils->GetConfigValue(k_ESteamNetworkingConfig_TimeoutConnected, k_ESteamNetworkingConfig_Global, 0, &type,
                                  &timeout, &size) == k_ESteamNetworkingGetConfigValue_OK
                && timeout > 0 && ms >= timeout)
            warning = std::format(" - at least GNS's connected timeout ({} ms): the connection will drop", timeout);
        const uint32_t tick = session_tick_now(physics_session_);
        logger_->Info("Automation: net blackout %ld ms (%s) from tick %u%s", ms, join_flags(up, down).c_str(), tick,
                      warning.c_str());
        return std::format("ok net blackout {} ms ({}) from tick {}{}", ms, join_flags(up, down), tick, warning);
    }
    if (what.empty() || what == "status") {
        return std::format("ok net lag up={} down={} blackout={} armed={}", host.lag_up_ms, host.lag_down_ms,
                           join_flags(host.blackout_up, host.blackout_down), host.net_pending.size());
    }
    return "error usage: net lag <ms> [up|down] [at <tick>] | net blackout <ms> [up|down] [at <tick>] | net status";
}

// replay session <file> | replay session stop | replay session: arm a key
// record for the next (or this) session.  Frame k is fed to the input poll of
// tick first + k, `first` being the first assigned frame's tick - the frame
// after the anchor hold - with no clock reset and nothing recorded (design
// 3.2; the legacy `replay` anchors at the first Gameplay_Ingame frame after
// the verb and resets the clock).
std::string BallanceMMOClient::core_host_replay_verb(const std::string& rest) {
    auto& r = host.replay;
    if (rest.empty() || rest == "status")
        return std::format("ok replay session armed={} started={} first_tick={} fed={} frames={} last_frame={} jumps={} "
                           "path={}",
                           r.armed ? 1 : 0, r.started ? 1 : 0, r.first_tick, r.fed, r.source.frames.size(), r.last_frame,
                           r.jumps, r.path.empty() ? "-" : r.path);
    if (rest == "stop") {
        const bool was = r.armed;
        r.armed = false;
        return std::format("ok replay session {} after {} frames", was ? "stopped" : "was not armed", r.fed);
    }
    std::string error;
    bmmo::physics::tick_record source;
    if (!source.load(rest, error)) return "error " + error;
    if (source.frames.empty()) return "error the record has no frames: " + rest;
    r = {};
    r.source = std::move(source);
    r.path = rest;
    r.armed = true;
    return std::format("ok replay session armed: {} frames from the first assigned frame", r.source.frames.size());
}

// The input poll (on_input_polled), after the automation keys: the armed key
// record's frame for the tick about to step.  The record ends the feed when
// it runs out, and so does the end of the session it started in.  The feed
// follows the session's tick numbers: a renumbering (a resync) skips record
// frames when it jumps forward and lets live keys through for the ticks
// before the first fed one when it jumps back - each jump is logged.
void BallanceMMOClient::core_host_feed_replay_keys(unsigned char* state) {
    auto& r = host.replay;
    if (!r.armed) return;
    uint32_t tick = 0;
    if (!next_session_tick(physics_session_, tick)) {
        if (r.started && physics_session_.phase != bs::physics_session_state::phase_type::running) {
            r.armed = false;
            logger_->Info("Automation: replay session: the session ended after %llu frames",
                          static_cast<unsigned long long>(r.fed));
        }
        return;
    }
    if (!r.started) {
        r.started = true;
        r.first_tick = tick;
        logger_->Info("Automation: replay session: frame 0 of %zu fed at tick %u", r.source.frames.size(), tick);
    } else if (r.fed > 0 && tick != r.last_tick && tick != r.last_tick + 1) {
        const int64_t jump = static_cast<int64_t>(tick) - static_cast<int64_t>(r.last_tick) - 1;
        if (r.jumps++ < 16)
            logger_->Info("Automation: replay session: the session tick jumped from %u to %u (%lld record frame(s) %s)",
                          r.last_tick, tick, static_cast<long long>(jump < 0 ? -jump : jump),
                          jump < 0 ? "fed again, or live keys before the first" : "skipped");
    }
    r.last_tick = tick;
    const int64_t k = static_cast<int32_t>(tick - r.first_tick);
    if (k < 0) return;
    if (k >= static_cast<int64_t>(r.source.frames.size())) {
        r.armed = false;
        logger_->Info("Automation: replay session: the record ended at tick %u after %llu frames", tick,
                      static_cast<unsigned long long>(r.fed));
        return;
    }
    const auto& keys = r.source.frames[static_cast<size_t>(k)].keys;
    std::memcpy(state, keys.data(), keys.size());
    r.last_frame = k;
    ++r.fed;
}
