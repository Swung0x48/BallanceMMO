// BallanceMMOSessionClient: a headless physics-session client (design 8.6
// item 3).  Headless engine + GameNetworkingSockets client that joins a room,
// takes part in a physics session through the real protocol, drives its own
// retail ball from a recorded keyboard (bmrc), mirrors the other players'
// balls, and reports how far its own ball drifts from the server's snapshots.
// Single-threaded: the network is polled between engine frames.
//
//   BallanceMMOSessionClient --root <game dir> --server <ip:port>
//       [--name <nick>] [--level N] [--record <file.bmrc>]
//       [--join-first | --room <id> | --host --expect N]
//       [--seconds S] [--trace] [--no-correct]
//
// Exit code 0 when the own ball never needed a correction, 2 otherwise.

#include "headless_engine.hpp"

#include "CKAll.h"
#include "CKIpionManager.h"

#include <entity/version.hpp>
#include <game/menu_driver.hpp>
#include <game/navigation_graph.hpp>
#include <message/message_all.hpp>
#include <physics/ball_navigation.hpp>
#include <physics/physics_state.hpp>
#include <physics/tick_record.hpp>
#include <role/role.hpp>
#include <session/correction.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {
    using clock_type = std::chrono::steady_clock;

    struct arguments {
        std::string root;
        std::string server = "127.0.0.1:26676";
        std::string name = "Headless";
        int level = 1;
        std::string record;
        bool join_first = false;
        uint32_t room = 0;
        bool host = false;
        int expect = 2;
        double seconds = 180.0;
        bool trace = false;
        bool correct = true;
        int boot_ticks = 400;
        int anchor_timeout = 3000;
        int64_t pause_at = -1;        // test knob: stop ticking at this tick ...
        int pause_ms = 0;             // ... for this long (design 9.2 resync path)
    };

    void usage() {
        std::puts("usage: BallanceMMOSessionClient --root <game dir> --server <ip:port> [--name N] [--level N]\n"
                  "       [--record <file.bmrc>] [--join-first | --room <id> | --host --expect N]\n"
                  "       [--seconds S] [--trace] [--no-correct] [--pause-at TICK --pause-ms MS]");
    }

    bool parse(int argc, char** argv, arguments& out) {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
            const char* v = nullptr;
            if (arg == "--root") { if (!(v = next())) return false; out.root = v; }
            else if (arg == "--server") { if (!(v = next())) return false; out.server = v; }
            else if (arg == "--name") { if (!(v = next())) return false; out.name = v; }
            else if (arg == "--level") { if (!(v = next())) return false; out.level = std::atoi(v); }
            else if (arg == "--record") { if (!(v = next())) return false; out.record = v; }
            else if (arg == "--join-first") out.join_first = true;
            else if (arg == "--room") { if (!(v = next())) return false; out.room = static_cast<uint32_t>(std::atoi(v)); }
            else if (arg == "--host") out.host = true;
            else if (arg == "--expect") { if (!(v = next())) return false; out.expect = std::atoi(v); }
            else if (arg == "--seconds") { if (!(v = next())) return false; out.seconds = std::atof(v); }
            else if (arg == "--trace") out.trace = true;
            else if (arg == "--no-correct") out.correct = false;
            else if (arg == "--boot-ticks") { if (!(v = next())) return false; out.boot_ticks = std::atoi(v); }
            else if (arg == "--pause-at") { if (!(v = next())) return false; out.pause_at = std::atoll(v); }
            else if (arg == "--pause-ms") { if (!(v = next())) return false; out.pause_ms = std::atoi(v); }
            else return false;
        }
        if (out.root.empty()) return false;
        if (!out.host && !out.join_first && out.room == 0) out.join_first = true;
        return true;
    }

    void logf(const char* format, ...) {
        va_list args;
        va_start(args, format);
        std::vprintf(format, args);
        va_end(args);
        std::putchar('\n');
        std::fflush(stdout);
    }

    void copy_name(char* dst, size_t size, const std::string& src) {
        std::snprintf(dst, size, "%s", src.c_str());
    }

    bmmo_physics_ball_recipe to_bridge_recipe(const bmmo::session::ball_recipe& r) {
        bmmo_physics_ball_recipe p{};
        p.fixed = r.fixed; p.start_frozen = r.start_frozen; p.enable_collision = r.enable_collision;
        p.calc_mass_center = r.calc_mass_center;
        p.friction = r.friction; p.elasticity = r.elasticity; p.mass = r.mass;
        p.linear_damp = r.linear_damp; p.rot_damp = r.rot_damp;
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

    struct ball_row {
        std::string name;
        float friction = 0, elasticity = 0, mass = 0, linear_damp = 0, rot_damp = 0, force = 0;
    };

    class session_client : public role {
    public:
        explicit session_client(arguments args) : args_(std::move(args)) {}

        // A fresh engine at the main menu.  Also used when a session starts
        // while the previous level is still loaded (host restart): the menu
        // driver needs the main menu, so the whole composition is rebooted.
        bool boot_engine() {
            bmmo::sim::engine_options eo;
            eo.game_root = args_.root;
            eo.log = [](const std::string& text) { logf("[engine] %s", text.c_str()); };
            std::string error;
            engine_.reset();
            engine_ = bmmo::sim::headless_engine::create(eo, error);
            if (!engine_) { logf("engine: %s", error.c_str()); return false; }
            if (!engine_->load_composition(error)) { logf("composition: %s", error.c_str()); return false; }
            for (int i = 0; i < args_.boot_ticks; ++i)
                if (!engine_->tick(error)) { logf("boot tick: %s", error.c_str()); return false; }
            bmmo::physics::drain_event_log(engine_->physics());
            return true;
        }

        bool boot() {
            std::string error;
            if (!boot_engine()) return false;
            if (!args_.record.empty()) {
                if (!record_.load(args_.record, error)) { logf("record: %s", error.c_str()); return false; }
                logf("record: level=%d frames=%zu", record_.header.level, record_.frames.size());
            }
            logf("engine booted (%llu ticks)", static_cast<unsigned long long>(engine_->ticks()));
            return true;
        }

        bool connect() {
            SteamNetworkingIPAddr address{};
            if (!address.ParseString(args_.server.c_str())) { logf("bad server address %s", args_.server.c_str()); return false; }
            SteamNetworkingConfigValue_t opt = generate_opt();
            connection_ = interface_->ConnectByIPAddress(address, 1, &opt);
            return connection_ != k_HSteamNetConnection_Invalid;
        }

        int exit_code() const {
            const auto& st = corrector_.stats();
            return (st.blended + st.hard + st.unmatched) == 0 ? 0 : 2;
        }

        // ------------------------------------------------------------ role
        void run() override {
            const auto started = clock_type::now();
            mark_running();
            while (running_) {
                update();
                if (ended_) break;
                if (std::chrono::duration<double>(clock_type::now() - started).count() > args_.seconds) {
                    logf("time limit reached");
                    break;
                }
                switch (phase_) {
                case phase::idle:
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    break;
                case phase::loading:
                    load_step();
                    break;
                case phase::running:
                    pace_and_frame();
                    break;
                }
                const auto now = clock_type::now();
                if (now - last_report_ > std::chrono::seconds(5)) {
                    last_report_ = now;
                    report();
                }
            }
            report();
        }

        int poll_incoming_messages() override {
            const int count = interface_->ReceiveMessagesOnConnection(connection_, incoming_messages_, ONCE_RECV_MSG_COUNT);
            if (count <= 0) return 0;
            for (int i = 0; i < count; ++i) {
                on_message(incoming_messages_[i]);
                incoming_messages_[i]->Release();
            }
            return count;
        }

        void on_connection_status_changed(SteamNetConnectionStatusChangedCallback_t* info) override {
            switch (info->m_info.m_eState) {
            case k_ESteamNetworkingConnectionState_ClosedByPeer:
            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
                logf("connection lost: %s", info->m_info.m_szEndDebug);
                interface_->CloseConnection(info->m_hConn, 0, nullptr, false);
                connection_ = k_HSteamNetConnection_Invalid;
                running_ = false;
                break;
            case k_ESteamNetworkingConnectionState_Connected: {
                logf("connected, logging in as %s", args_.name.c_str());
                bmmo::login_request_v3_msg msg;
                msg.version = bmmo::current_version;
                msg.nickname = args_.name;
                msg.cheated = 0;
                std::mt19937 rng(static_cast<unsigned>(std::chrono::system_clock::now().time_since_epoch().count()));
                for (auto& byte: msg.uuid) byte = static_cast<uint8_t>(rng());
                msg.serialize();
                send_bytes(msg.raw.str(), k_nSteamNetworkingSend_Reliable);
                break;
            }
            default:
                break;
            }
        }

        void on_message(ISteamNetworkingMessage* networking_msg) override {
            if (networking_msg->m_cbSize < static_cast<int>(sizeof(bmmo::opcode))) return;
            auto* raw = reinterpret_cast<bmmo::general_message*>(networking_msg->m_pData);
            auto fill = [&](auto& msg) {
                msg.raw.write(reinterpret_cast<char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                return msg.deserialize();
            };
            switch (raw->code) {
            case bmmo::LoginAcceptedV3: {
                bmmo::login_accepted_v3_msg msg;
                if (!fill(msg)) return;
                for (const auto& [id, data]: msg.online_players)
                    if (data.name == args_.name) own_id_ = id;
                logf("logged in (own id %u, %zu online)", own_id_, msg.online_players.size());
                announce_map();
                if (args_.host) room_request(bmmo::room::action::Create, 0, "headless");
                else room_request(bmmo::room::action::List, 0);
                break;
            }
            case bmmo::RoomState: {
                bmmo::room_state_msg msg;
                if (!fill(msg)) return;
                handle_room_state(msg);
                break;
            }
            case bmmo::RoomEvent: {
                bmmo::room_event_msg msg;
                if (!fill(msg)) return;
                logf("room event %d (error %s) room %u actor %u subject %u %s", static_cast<int>(msg.type),
                     bmmo::room::error_string(msg.error), msg.room, msg.actor, msg.subject, msg.reason.c_str());
                if (msg.type == bmmo::room::event_type::RequestDenied && msg.error != bmmo::room::error_code::None) {
                    if (join_sent_ && !in_room_) join_sent_ = false;   // retry on the next state
                }
                break;
            }
            case bmmo::SessionStart: {
                bmmo::session_start_msg msg;
                if (!fill(msg)) return;
                begin_session(msg);
                break;
            }
            case bmmo::SessionAssign: {
                bmmo::session_assign_msg msg;
                if (!fill(msg)) return;
                if (msg.session != session_ || phase_ != phase::running) return;
                if (assigned_) {
                    // Resync (design 9.2): numbering restarts from the server's
                    // current tick; the next full snapshot rebuilds every body.
                    tick_base_ = msg.first_tick;
                    frames_since_anchor_ = 0;
                    input_history_.clear();
                    corrector_.clear();
                    for (auto& [name, corrector]: mechanism_correctors_) corrector.clear();
                    for (auto& [id, remote]: remotes_) remote.corrector.clear();
                    resync_pending_ = true;
                    have_snapshot_ = false;
                    origin_ = clock_type::now();
                    logf("resynced: tick base %u", tick_base_);
                    break;
                }
                assigned_ = true;
                tick_base_ = msg.first_tick;
                // The schedule restarts from the assignment: frames simulated so
                // far (none for a start member) keep their slots.
                origin_ = clock_type::now() - std::chrono::duration_cast<clock_type::duration>(
                    std::chrono::duration<double>(static_cast<double>(std::max<int64_t>(frames_since_anchor_, 0))
                                                  / bmmo::sim::kTickRate));
                logf("tick base %u assigned (%lld frames since anchor)", tick_base_, static_cast<long long>(frames_since_anchor_));
                flush_inputs();
                break;
            }
            case bmmo::SessionSnapshot: {
                bmmo::session_snapshot_msg msg;
                if (!fill(msg)) return;
                ++snapshots_received_;
                if (msg.session == session_ && phase_ == phase::running) apply_snapshot(msg);
                break;
            }
            case bmmo::SessionEvent: {
                bmmo::session_event_msg msg;
                if (!fill(msg)) return;
                ++events_received_;
                if (msg.session == session_ && phase_ == phase::running) apply_event(msg);
                break;
            }
            case bmmo::SessionRemoteInput: {
                bmmo::session_remote_input_msg msg;
                if (!fill(msg)) return;
                ++remote_inputs_received_;
                if (msg.session != session_) return;
                for (const auto& entry: msg.entries) {
                    auto it = remotes_.find(entry.player);
                    if (it == remotes_.end()) continue;
                    auto& remote = it->second;
                    if (remote.have_input && msg.tick < remote.input_tick) continue;
                    remote.input = entry.frame;
                    remote.input_tick = msg.tick;
                    remote.have_input = true;
                }
                break;
            }
            case bmmo::SessionEnd: {
                bmmo::session_end_msg msg;
                if (!fill(msg)) return;
                logf("session %u ended: %s", msg.session, msg.reason.c_str());
                if (msg.session == session_) end_session();
                break;
            }
            case bmmo::PlainText: {
                bmmo::plain_text_msg msg;
                if (fill(msg)) logf("[server] %s", msg.text_content.c_str());
                break;
            }
            default:
                break;
            }
        }

    private:
        enum class phase { idle, loading, running };

        struct remote_ball {
            std::string entity;
            uint8_t ball_type = 0;
            bool physicalized = false;
            bool navigation = false;        // driven by the shared navigation (design 9.1)
            bool have_input = false;
            uint32_t input_tick = 0;
            bmmo::session::input_frame input{};
            bmmo::session::body_corrector corrector;
            uint64_t blends = 0, hard_sets = 0;
        };

        // ------------------------------------------------------ networking
        void send_bytes(const std::string& bytes, int flags) {
            interface_->SendMessageToConnection(connection_, bytes.data(), static_cast<uint32_t>(bytes.size()), flags, nullptr);
        }

        void announce_map() {
            bmmo::current_map_msg msg{};
            msg.content.map.type = bmmo::map_type::OriginalLevel;
            msg.content.map.level = args_.level;
            bmmo::hex_chars_from_string(msg.content.map.md5, bmmo::map::original_map_hashes[std::clamp(args_.level, 0, 13)]);
            msg.content.sector = 1;
            msg.content.type = bmmo::current_map_state::EnteringMap;
            interface_->SendMessageToConnection(connection_, &msg, sizeof(msg), k_nSteamNetworkingSend_Reliable, nullptr);
        }

        void room_request(bmmo::room::action action, uint32_t room, const std::string& name = "") {
            bmmo::room_request_msg req;
            req.action = action;
            req.room = room;
            req.name = name;
            req.mode = bmmo::room::mode::Physics;
            req.serialize();
            send_bytes(req.raw.str(), k_nSteamNetworkingSend_Reliable);
        }

        void handle_room_state(const bmmo::room_state_msg& msg) {
            in_room_ = msg.own_room != 0;
            if (!in_room_) {
                if (args_.host || join_sent_) return;
                uint32_t target = args_.room;
                if (target == 0)   // prefer a lobby; a running room means a late join (design 9.3)
                    for (const auto& r: msg.rooms)
                        if (r.phase == bmmo::room::phase::Lobby) { target = r.id; break; }
                if (target == 0 && !msg.rooms.empty()) target = msg.rooms.front().id;
                if (target == 0) {
                    if (!msg.rooms.empty() || list_retries_++ % 20 == 0) logf("no lobby room to join yet (%zu rooms)", msg.rooms.size());
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    room_request(bmmo::room::action::List, 0);
                    return;
                }
                logf("joining room %u", target);
                join_sent_ = true;
                room_request(bmmo::room::action::Join, target);
                return;
            }
            room_id_ = msg.own_room;
            bool own_ready = true;
            for (const auto& m: msg.members)
                if (m.id == own_id_) own_ready = m.ready;
            if (!own_ready) {
                // First time, or the room went back to the lobby (session ended
                // by the server, host restart): ready again, at most once a second.
                const auto now = clock_type::now();
                if (!ready_sent_ || now - last_ready_sent_ > std::chrono::seconds(1)) {
                    ready_sent_ = true;
                    last_ready_sent_ = now;
                    logf("in room %u with %zu members; ready", room_id_, msg.members.size());
                    room_request(bmmo::room::action::Ready, room_id_);
                }
                return;
            }
            if (args_.host && !start_sent_ && static_cast<int>(msg.members.size()) >= args_.expect) {
                bool all_ready = true;
                for (const auto& m: msg.members) all_ready = all_ready && m.ready;
                if (all_ready) {
                    start_sent_ = true;
                    logf("all %zu members ready, starting the physics session", msg.members.size());
                    room_request(bmmo::room::action::Start, room_id_);
                }
            }
        }

        // ---------------------------------------------------------- session
        void begin_session(const bmmo::session_start_msg& msg) {
            session_ = msg.session;
            snapshot_interval_ = msg.snapshot_interval;
            input_delay_ = msg.input_delay;
            seed_ = msg.seed;
            level_ = msg.map.level > 0 ? msg.map.level : args_.level;
            players_ = msg.players;
            own_join_order_ = -1;
            spawn_known_ = false;
            for (const auto& p: players_) {
                if (p.id != own_id_) continue;
                own_join_order_ = p.join_order;
                for (int k = 0; k < 3; ++k) spawn_position_[k] = p.spawn_position[k];
                spawn_known_ = true;
            }
            own_group_ = "P#" + std::to_string(own_join_order_ < 0 ? 63 : own_join_order_);
            logf("session %u starting: level %d, %zu players, join order %d, seed %d", session_, level_, players_.size(),
                 own_join_order_, seed_);
            // Fresh runtime state.
            assigned_ = false;
            tick_base_ = 0;
            frames_since_anchor_ = -1;
            input_history_.clear();
            previous_cam_valid_ = false;
            navigation_known_ = false;
            corrector_.clear();
            mechanism_correctors_.clear();
            mechanism_names_.clear();
            remotes_.clear();
            own_group_set_ = false;
            own_physicalized_ = false;
            last_sector_ = 0;
            std::string error;
            if (bmmo::game::script_active(engine_->context(), "Gameplay_Ingame")) {
                // Restart (host started the room again): reboot to the menu.
                logf("session %u: level still loaded, rebooting the engine", session_);
                for (auto& [id, remote]: remotes_) {
                    if (remote.navigation) bmmo::physics::navigation_destroy(engine_->physics(), remote.entity.c_str(), error);
                }
                remotes_.clear();
                if (!boot_engine()) { ended_ = true; return; }
            }
            engine_->clear_keys();
            engine_->request_level(level_);
            load_waited_ = 0;
            phase_ = phase::loading;
        }

        void load_step() {
            std::string error;
            if (bmmo::game::script_active(engine_->context(), "Gameplay_Ingame")) {
                anchor();
                return;
            }
            if (!engine_->level_request().error.empty()) {
                logf("level request failed: %s", engine_->level_request().error.c_str());
                ended_ = true;
                return;
            }
            if (++load_waited_ > args_.anchor_timeout) {
                logf("Gameplay_Ingame never activated");
                ended_ = true;
                return;
            }
            if (!engine_->tick(error)) { logf("tick failed: %s", error.c_str()); ended_ = true; }
        }

        void anchor() {
            std::string error;
            CKIpionManager* physics = engine_->physics();
            if (!bmmo::physics::reset_session_clock(physics, seed_, error)) { logf("clock reset: %s", error.c_str()); ended_ = true; return; }
            bmmo::physics::world_hash hash;
            if (!bmmo::physics::capture_world_hash(physics, hash, error)) { logf("hash: %s", error.c_str()); ended_ = true; return; }
            if (!bmmo::physics::install_player_collision_filter(physics, "P#", error)) logf("collision filter: %s", error.c_str());
            bmmo::physics::drain_event_log(physics);
            read_ball_rows();
            if (CKDataArray* level = engine_->data_array("CurrentLevel")) {
                level->GetElementValue(0, 3, &spawn_matrix_);
                if (spawn_known_)
                    for (int k = 0; k < 3; ++k) spawn_offset_[k] = spawn_position_[k] - spawn_matrix_[3][k];
            }
            anchor_hash_ = hash.pose;
            frames_since_anchor_ = 0;
            phase_ = phase::running;
            origin_ = clock_type::now();
            rebases_ = 0;
            bmmo::session_ready_msg ready;
            ready.session = session_;
            ready.first_tick = 0;
            ready.anchor_hash = hash.pose;
            ready.anchor_surfaces = hash.surfaces;
            ready.physics_sha256 = sizeof(void*) == 8 ? "headless-64" : "headless-32";
            ready.build_id = "BallanceMMOSessionClient";
            ready.serialize();
            send_bytes(ready.raw.str(), k_nSteamNetworkingSend_Reliable);
            logf("anchored after %d load ticks: pose %016llx surfaces %016llx cores=%d factor=%.6f", load_waited_,
                 static_cast<unsigned long long>(hash.pose), static_cast<unsigned long long>(hash.surfaces), hash.cores,
                 hash.time_factor);
        }

        void read_ball_rows() {
            ball_rows_.clear();
            if (CKDataArray* table = engine_->data_array("Physicalize_GameBall")) {
                const int rows = table->GetRowCount();
                for (int row = 0; row < rows; ++row) {
                    ball_row entry;
                    char name[128] = {};
                    table->GetElementStringValue(row, 0, name, static_cast<int>(sizeof(name)));
                    name[sizeof(name) - 1] = 0;
                    entry.name = name;
                    table->GetElementValue(row, 1, &entry.friction);
                    table->GetElementValue(row, 2, &entry.elasticity);
                    table->GetElementValue(row, 3, &entry.mass);
                    table->GetElementValue(row, 5, &entry.linear_damp);
                    table->GetElementValue(row, 6, &entry.rot_damp);
                    table->GetElementValue(row, 7, &entry.force);
                    ball_rows_.push_back(entry);
                }
            }
        }

        int ball_type_of(const std::string& name) const {
            for (size_t i = 0; i < ball_rows_.size(); ++i)
                if (ball_rows_[i].name == name) return static_cast<int>(i);
            return -1;
        }

        // The retail "physicalize new Ball" recipe (see physics_world::retail_recipe).
        void fill_retail_recipe(int ball_type, bmmo::session::ball_recipe& r) const {
            r = {};
            if (ball_type < 0 || ball_type >= static_cast<int>(ball_rows_.size())) return;
            const ball_row& row = ball_rows_[ball_type];
            r.fixed = false;
            r.start_frozen = false;
            r.enable_collision = true;
            r.calc_mass_center = false;
            r.friction = row.friction;
            r.elasticity = row.elasticity;
            r.mass = row.mass;
            r.linear_damp = row.linear_damp;
            r.rot_damp = row.rot_damp;
            r.collision_surface = row.name + "_Mesh";
            if (row.name == "Ball_Paper") {
                r.convex_meshes.push_back(row.name + "_Mesh");
            } else {
                bmmo::session::ball_recipe::sphere sphere;
                sphere.radius = 2.0f;
                r.balls.push_back(sphere);
            }
        }

        uint32_t current_tick() const {
            return frames_since_anchor_ >= 1 ? tick_base_ + static_cast<uint32_t>(frames_since_anchor_ - 1) : tick_base_;
        }

        CK3dEntity* current_ball() const {
            CKDataArray* level = engine_->data_array("CurrentLevel");
            if (!level) return nullptr;
            return CK3dEntity::Cast(level->GetElementObject(0, 1));
        }

        bool navigation_active() const {
            CKContext* context = engine_->context();
            for (const auto& leaf: navigation_.leaves)
                if (auto* key_event = CKBehavior::Cast(context->GetObject(leaf.key_block)))
                    if (key_event->IsActive()) return true;
            return false;
        }

        void pace_and_frame() {
            if (!assigned_) {
                // Hold frame 1 until the server assigns the tick base: everybody
                // starts together instead of an early anchor running ahead.
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                return;
            }
            const uint64_t next_frame = static_cast<uint64_t>(frames_since_anchor_ + 1);
            if (args_.pause_at >= 0 && !paused_once_ && static_cast<int64_t>(current_tick()) >= args_.pause_at) {
                paused_once_ = true;
                logf("test pause: %d ms at tick %u", args_.pause_ms, current_tick());
                std::this_thread::sleep_for(std::chrono::milliseconds(args_.pause_ms));
            }
            const double expected = std::chrono::duration<double>(clock_type::now() - origin_).count() * bmmo::sim::kTickRate;
            if (static_cast<double>(next_frame) + 33.0 < expected) {
                // far behind (a long pause): restart the schedule instead of fast-forwarding
                origin_ = clock_type::now() - std::chrono::duration_cast<clock_type::duration>(
                    std::chrono::duration<double>(static_cast<double>(next_frame) / bmmo::sim::kTickRate));
                ++rebases_;
                request_resync("tick schedule rebased");
            } else if (static_cast<double>(next_frame) > expected) {
                const auto due = origin_ + std::chrono::duration_cast<clock_type::duration>(
                    std::chrono::duration<double>(static_cast<double>(next_frame) / bmmo::sim::kTickRate));
                const auto now = clock_type::now();
                if (due > now) {
                    std::this_thread::sleep_for(std::min(due - now, clock_type::duration(std::chrono::milliseconds(2))));
                    return;   // poll the network again before the frame is due
                }
            }
            frame();
        }

        void frame() {
            std::string error;
            const int64_t f = frames_since_anchor_ + 1;
            // Keys of this frame come from the recording (frame f of the record).
            const unsigned char* keyboard = nullptr;
            if (!record_.frames.empty() && f >= 0 && static_cast<size_t>(f) < record_.frames.size()) {
                keyboard = record_.frames[static_cast<size_t>(f)].keys.data();
                engine_->set_keyboard_state(keyboard);
            } else {
                engine_->clear_keys();
            }
            if (!engine_->tick(error)) { logf("tick failed: %s", error.c_str()); ended_ = true; return; }
            frames_since_anchor_ = f;
            const uint32_t tick = current_tick();
            CKIpionManager* physics = engine_->physics();
            CKContext* context = engine_->context();

            if (!navigation_known_) {
                auto graph = bmmo::game::read_navigation_graph(context);
                bool complete = graph.valid();
                for (const auto& leaf: graph.leaves) complete = complete && leaf.key != 0;
                if (complete) {
                    navigation_ = graph;
                    navigation_known_ = true;
                    logf("navigation keys known at tick %u (%zu leaves)", tick, graph.leaves.size());
                    for (auto& [id, remote]: remotes_) attach_remote_navigation(id, remote);
                }
            }

            // Own ball state after this tick.
            CK3dEntity* ball = current_ball();
            const std::string ball_name = ball && ball->GetName() ? ball->GetName() : "";
            const int ball_type = ball_type_of(ball_name);
            bmmo_physics_body_state own{};
            const bool physicalized = !ball_name.empty() && bmmo::physics::get_body_state(physics, ball_name.c_str(), own, error);
            if (physicalized) {
                if (!own_group_set_) {
                    if (bmmo::physics::set_body_group(physics, ball_name.c_str(), own_group_.c_str(), error)) own_group_set_ = true;
                    else logf("own ball group: %s", error.c_str());
                }
                bmmo::session::ball_pose pose;
                pose.tick = tick;
                for (int k = 0; k < 3; ++k) { pose.position[k] = own.position[k]; pose.linear[k] = own.linear[k]; pose.angular[k] = own.angular[k]; }
                for (int k = 0; k < 4; ++k) pose.rotation[k] = own.rotation[k];
                corrector_.record(pose);
            } else {
                own_group_set_ = false;
                // Ring offset: while the retail script holds the ball at the
                // resetpoint before physicalizing it, move it to our slot.
                if (ball && spawn_known_ && players_.size() > 1) {
                    VxVector position;
                    ball->GetPosition(&position);
                    const VxVector reset(spawn_matrix_[3][0], spawn_matrix_[3][1], spawn_matrix_[3][2]);
                    if ((position - reset).SquareMagnitude() < 1e-6f) {
                        VxVector target(reset.x + spawn_offset_[0], reset.y + spawn_offset_[1], reset.z + spawn_offset_[2]);
                        if ((target - position).SquareMagnitude() > 1e-8f) ball->SetPosition(&target);
                    }
                }
            }
            own_physicalized_ = physicalized;

            // Mechanism states after this tick.
            for (const auto& [index, name]: mechanism_names_) {
                bmmo_physics_body_state local{};
                if (!bmmo::physics::get_body_state(physics, name.c_str(), local, error)) continue;
                bmmo::session::ball_pose pose;
                pose.tick = tick;
                for (int k = 0; k < 3; ++k) { pose.position[k] = local.position[k]; pose.linear[k] = local.linear[k]; pose.angular[k] = local.angular[k]; }
                for (int k = 0; k < 4; ++k) pose.rotation[k] = local.rotation[k];
                mechanism_correctors_[name].record(pose);
            }
            for (auto& [id, remote]: remotes_) {
                if (!remote.physicalized || !remote.navigation) continue;
                bmmo_physics_body_state local{};
                if (!bmmo::physics::get_body_state(physics, remote.entity.c_str(), local, error)) continue;
                bmmo::session::ball_pose pose;
                pose.tick = tick;
                for (int k = 0; k < 3; ++k) { pose.position[k] = local.position[k]; pose.linear[k] = local.linear[k]; pose.angular[k] = local.angular[k]; }
                for (int k = 0; k < 4; ++k) pose.rotation[k] = local.rotation[k];
                remote.corrector.record(pose);
            }

            // Input for this tick.
            bmmo::session::input_frame input{};
            if (navigation_known_ && keyboard) input.keys = navigation_.keys_from_state(keyboard);
            float cam[3][3] = {};
            bool cam_valid = false;
            if (auto* orient = CK3dEntity::Cast(context->GetObjectByNameAndClass(const_cast<CKSTRING>("Cam_OrientRef"), CKCID_3DENTITY, nullptr))) {
                const VxMatrix& m = orient->GetWorldMatrix();
                for (int r = 0; r < 3; ++r)
                    for (int k = 0; k < 3; ++k) cam[r][k] = m[r][k];
                cam_valid = true;
            }
            const float (*basis)[3] = previous_cam_valid_ ? previous_cam_ : cam;
            for (int k = 0; k < 3; ++k) { input.cam_right[k] = basis[0][k]; input.cam_up[k] = basis[1][k]; input.cam_dir[k] = basis[2][k]; }
            if (cam_valid) { std::memcpy(previous_cam_, cam, sizeof(cam)); previous_cam_valid_ = true; }
            input.ball_type = static_cast<uint8_t>(ball_type < 0 ? 0 : ball_type);
            const bool nav_active = navigation_known_ && navigation_active();
            input.flags = static_cast<uint8_t>((physicalized ? bmmo::session::INPUT_FLAG_PHYSICALIZED : 0)
                        | (nav_active ? bmmo::session::INPUT_FLAG_NAV_ACTIVE : 0));
            if (args_.trace && (input.keys != last_keys_ || nav_active != last_nav_active_))
                logf("input edge at tick %u keys=%u flags=%u", tick, input.keys, input.flags);
            last_keys_ = input.keys;
            last_nav_active_ = nav_active;
            input_history_.emplace_back(tick, input);
            if (assigned_) {
                while (input_history_.size() > bmmo::session::MAX_INPUT_FRAMES) input_history_.pop_front();
                bmmo::session_input_msg msg;
                msg.session = session_;
                msg.first_tick = input_history_.front().first;
                for (const auto& [t, fr]: input_history_) msg.frames.push_back(fr);
                msg.serialize();
                send_bytes(msg.raw.str(), k_nSteamNetworkingSend_UnreliableNoDelay);
                ++inputs_sent_;
            } else if (input_history_.size() > 660) {
                input_history_.pop_front();
            }

            // Lifecycle events from the bridge's object log.
            const std::string events = bmmo::physics::drain_event_log(physics);
            std::set<std::string> revived_reported;
            size_t pos = 0;
            while (pos < events.size()) {
                const size_t end = events.find(';', pos);
                if (end == std::string::npos) break;
                std::string entry = events.substr(pos, end - pos);
                pos = end + 1;
                // "t=<time> <kind> <name>"
                const size_t space1 = entry.find(' ');
                if (space1 == std::string::npos) continue;
                const size_t space2 = entry.find(' ', space1 + 1);
                if (space2 == std::string::npos) continue;
                const std::string kind = entry.substr(space1 + 1, space2 - space1 - 1);
                const std::string name = entry.substr(space2 + 1);
                if (kind == "created" && !ball_name.empty() && name == ball_name) {
                    report_physicalize(ball, ball_name, ball_type, tick, own);
                } else if (kind == "deleted" && !ball_name.empty() && name == ball_name) {
                    bmmo::session_event_msg event;
                    event.tick = tick;
                    event.type = bmmo::session::event_type::Unphysicalize;
                    send_event(event);
                    own_group_set_ = false;
                    logf("own ball unphysicalized at tick %u", tick);
                } else if (kind == "revived" && !name.empty() && name.rfind("Ball_", 0) != 0
                           && name.find("_BMMO_") == std::string::npos && !revived_reported.count(name)) {
                    revived_reported.insert(name);
                    bmmo::session_event_msg event;
                    event.tick = tick;
                    event.type = bmmo::session::event_type::BodyRevived;
                    event.name = name;
                    send_event(event);
                }
            }
            // Sector: the sector the retail scripts last activated.
            if (CKDataArray* parameters = engine_->data_array("IngameParameter")) {
                int sector = 0;
                if (parameters->GetElementValue(0, 1, &sector) && sector > 0 && sector != last_sector_) {
                    last_sector_ = sector;
                    bmmo::session_event_msg event;
                    event.tick = tick;
                    event.type = bmmo::session::event_type::Sector;
                    event.sector = sector;
                    send_event(event);
                    logf("sector %d at tick %u", sector, tick);
                }
            }

            // Continue running blends.
            if (args_.correct) {
                if (physicalized) apply_blend(ball_name, corrector_);
                for (auto& [name, corrector]: mechanism_correctors_) apply_blend(name, corrector);
                for (auto& [id, remote]: remotes_)
                    if (remote.physicalized && remote.navigation) apply_blend(remote.entity, remote.corrector);
            }
            // Next tick's input of every predicted remote ball: the last relayed frame.
            for (auto& [id, remote]: remotes_) {
                if (!remote.physicalized || !remote.navigation) continue;
                const uint8_t keys = remote.have_input ? remote.input.keys : 0;
                const bool active = remote.have_input && (remote.input.flags & bmmo::session::INPUT_FLAG_NAV_ACTIVE) != 0;
                if (!bmmo::physics::navigation_input(physics, remote.entity.c_str(), keys, remote.input.cam_right,
                                                     remote.input.cam_up, remote.input.cam_dir, active, error))
                    logf("remote navigation input %s: %s", remote.entity.c_str(), error.c_str());
            }
            if (args_.trace && trace_frames_ > 0) {
                --trace_frames_;
                const std::string exact = bmmo::physics::describe_cores_exact(physics);
                size_t p = 0;
                while (p < exact.size()) {
                    size_t nl = exact.find('\n', p);
                    if (nl == std::string::npos) nl = exact.size();
                    logf("exact t=%u %s", tick, exact.substr(p, nl - p).c_str());
                    p = nl + 1;
                }
            }
        }

        void report_physicalize(CK3dEntity* ball, const std::string& ball_name, int ball_type, uint32_t tick,
                                const bmmo_physics_body_state& state) {
            bmmo::session_event_msg event;
            event.tick = tick;
            event.type = bmmo::session::event_type::Physicalize;
            event.ball_type = static_cast<uint8_t>(ball_type < 0 ? 0 : ball_type);
            // Pose the retail script physicalized at: the resetpoint (plus our
            // ring slot) when the body is still there, else the entity now.
            VxMatrix expected = spawn_matrix_;
            for (int k = 0; k < 3; ++k) expected[3][k] = spawn_matrix_[3][k] + (players_.size() > 1 ? spawn_offset_[k] : 0.0f);
            double distance = 0.0;
            for (int k = 0; k < 3; ++k) distance += (state.position[k] - expected[3][k]) * (state.position[k] - expected[3][k]);
            const VxMatrix& world = ball ? ball->GetWorldMatrix() : expected;
            const VxMatrix& used = distance < 0.05 * 0.05 ? expected : world;
            if (&used == &world) logf("warning: physicalize pose taken from the entity after the tick (%.3f m from the resetpoint)", std::sqrt(distance));
            for (int k = 0; k < 3; ++k) event.position[k] = used[3][k];
            for (int r = 0; r < 3; ++r)
                for (int k = 0; k < 3; ++k) event.rotation[r * 3 + k] = used[r][k];
            fill_retail_recipe(ball_type, event.recipe);
            send_event(event);
            own_group_set_ = false;
            if (args_.trace) trace_frames_ = 12;
            logf("own ball %s physicalized at tick %u (type %d) pos=%a,%a,%a", ball_name.c_str(), tick, ball_type,
                 static_cast<double>(event.position[0]), static_cast<double>(event.position[1]), static_cast<double>(event.position[2]));
        }

        void send_event(bmmo::session_event_msg& event) {
            event.session = session_;
            event.serialize();
            send_bytes(event.raw.str(), k_nSteamNetworkingSend_Reliable);
            ++events_sent_;
        }

        void flush_inputs() {
            if (!assigned_ || input_history_.empty()) return;
            size_t index = 0;
            while (index < input_history_.size()) {
                bmmo::session_input_msg msg;
                msg.session = session_;
                msg.first_tick = input_history_[index].first;
                while (index < input_history_.size() && msg.frames.size() < bmmo::session::MAX_INPUT_FRAMES) {
                    msg.frames.push_back(input_history_[index].second);
                    ++index;
                }
                msg.serialize();
                send_bytes(msg.raw.str(), k_nSteamNetworkingSend_UnreliableNoDelay);
                ++inputs_sent_;
            }
        }

        void apply_blend(const std::string& name, bmmo::session::body_corrector& corrector) {
            if (!corrector.blending()) return;
            const auto step = corrector.next_blend();
            if (step.action != bmmo::session::correction_step::kind::blend) return;
            std::string error;
            bmmo_physics_body_state current{};
            if (!bmmo::physics::get_body_state(engine_->physics(), name.c_str(), current, error)) return;
            double position[3];
            float linear[3];
            for (int k = 0; k < 3; ++k) {
                position[k] = current.position[k] + step.delta_position[k];
                linear[k] = current.linear[k] + step.delta_linear[k];
            }
            if (!bmmo::physics::set_body_state(engine_->physics(), name.c_str(), position, current.rotation, linear, current.angular, true, error))
                logf("blend %s: %s", name.c_str(), error.c_str());
        }

        void apply_snapshot(const bmmo::session_snapshot_msg& snapshot) {
            if (have_snapshot_ && snapshot.tick <= last_snapshot_tick_ && !snapshot.full) { ++snapshots_stale_; return; }
            std::string error;
            CKIpionManager* physics = engine_->physics();
            CK3dEntity* ball = current_ball();
            const std::string ball_name = ball && ball->GetName() ? ball->GetName() : "";
            if (resync_pending_) {
                if (!snapshot.full) return;
                have_snapshot_ = true;
                last_snapshot_tick_ = snapshot.tick;
                ++snapshots_applied_;
                for (const auto& body: snapshot.bodies) {
                    const bool wake = (body.flags & bmmo::session::BODY_FLAG_SIMULATED) != 0;
                    const char* target = nullptr;
                    if (body.kind == bmmo::session::body_kind::Ball) {
                        if (body.owner == own_id_) {
                            if (!own_physicalized_ || ball_name.empty()) continue;
                            target = ball_name.c_str();
                        } else {
                            auto it = remotes_.find(body.owner);
                            if (it == remotes_.end() || !it->second.physicalized) continue;
                            target = it->second.entity.c_str();
                        }
                    } else {
                        if (!body.name.empty()) mechanism_names_[body.owner] = body.name;
                        auto name = mechanism_names_.find(body.owner);
                        if (name == mechanism_names_.end()) continue;
                        target = name->second.c_str();
                    }
                    bmmo::physics::set_body_state(physics, target, body.position, body.rotation, body.linear, body.angular,
                                                  body.owner == own_id_ ? true : wake, error);
                }
                resync_pending_ = false;
                ++resyncs_done_;
                logf("resync applied from the full snapshot of tick %u (%zu bodies)", snapshot.tick, snapshot.bodies.size());
                return;
            }
            have_snapshot_ = true;
            last_snapshot_tick_ = snapshot.tick;
            ++snapshots_applied_;
            for (const auto& body: snapshot.bodies) {
                if (body.kind == bmmo::session::body_kind::Ball) {
                    if (body.owner == own_id_) {
                        if (!own_physicalized_ || ball_name.empty()) continue;
                        bmmo::session::ball_pose pose;
                        pose.tick = snapshot.tick;
                        for (int k = 0; k < 3; ++k) { pose.position[k] = body.position[k]; pose.linear[k] = body.linear[k]; pose.angular[k] = body.angular[k]; }
                        for (int k = 0; k < 4; ++k) pose.rotation[k] = body.rotation[k];
                        const auto step = corrector_.compare(pose);
                        const double err = corrector_.stats().last_error;
                        if (step.action == bmmo::session::correction_step::kind::hard) {
                            if (++consecutive_hard_ >= 3) request_resync("3 hard corrections in a row");
                            logf("hard correction of own ball for tick %u (error %.4f m, local tick %u)", snapshot.tick, err, current_tick());
                            if (args_.correct && !bmmo::physics::set_body_state(physics, ball_name.c_str(), step.target.position,
                                    step.target.rotation, step.target.linear, step.target.angular, true, error))
                                logf("hard set: %s", error.c_str());
                        } else if (step.action == bmmo::session::correction_step::kind::blend) {
                            consecutive_hard_ = 0;
                            logf("blend correction of own ball for tick %u (error %.4f m, local tick %u)", snapshot.tick, err, current_tick());
                        } else if (args_.trace && err > 0.002) {
                            logf("own ball error %.4f m at tick %u", err, snapshot.tick);
                        }
                        continue;
                    }
                    auto it = remotes_.find(body.owner);
                    if (it == remotes_.end() || !it->second.physicalized) continue;
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
                            if (args_.correct) bmmo::physics::set_body_state(physics, remote.entity.c_str(), step.target.position,
                                                                             step.target.rotation, step.target.linear, step.target.angular, wake, error);
                        } else if (step.action == bmmo::session::correction_step::kind::blend) {
                            ++remote.blends;
                            logf("blend correction of remote %s for tick %u (error %.4f m)", remote.entity.c_str(), snapshot.tick,
                                 remote.corrector.stats().last_error);
                        }
                        continue;
                    }
                    if (!bmmo::physics::set_body_state(physics, it->second.entity.c_str(), body.position, body.rotation,
                                                       body.linear, body.angular, wake, error))
                        logf("remote %u: %s", body.owner, error.c_str());
                    else ++remote_writes_;
                    continue;
                }
                if (snapshot.full && !body.name.empty()) mechanism_names_[body.owner] = body.name;
                auto name = mechanism_names_.find(body.owner);
                if (name == mechanism_names_.end()) continue;
                const bool wake = (body.flags & bmmo::session::BODY_FLAG_SIMULATED) != 0;
                bmmo_physics_body_state local{};
                if (!bmmo::physics::get_body_state(physics, name->second.c_str(), local, error)) continue;
                bmmo::session::ball_pose pose;
                pose.tick = snapshot.tick;
                for (int k = 0; k < 3; ++k) { pose.position[k] = body.position[k]; pose.linear[k] = body.linear[k]; pose.angular[k] = body.angular[k]; }
                for (int k = 0; k < 4; ++k) pose.rotation[k] = body.rotation[k];
                auto& corrector = mechanism_correctors_[name->second];
                const auto step = corrector.compare(pose);
                if (step.action == bmmo::session::correction_step::kind::hard) {
                    ++mechanism_hard_;
                    logf("hard correction of %s for tick %u (error %.4f m)", name->second.c_str(), snapshot.tick, corrector.stats().last_error);
                    if (args_.correct) bmmo::physics::set_body_state(physics, name->second.c_str(), step.target.position, step.target.rotation,
                                                                     step.target.linear, step.target.angular, wake, error);
                } else if (step.action == bmmo::session::correction_step::kind::blend) {
                    ++mechanism_blends_;
                    logf("blend correction of %s for tick %u (error %.4f m)", name->second.c_str(), snapshot.tick, corrector.stats().last_error);
                }
            }
        }

        CK3dEntity* remote_entity(uint32_t player, uint8_t ball_type, std::string& error) {
            if (ball_type >= ball_rows_.size()) { error = "unknown ball type"; return nullptr; }
            const std::string name = ball_rows_[ball_type].name + "_BMMO_" + std::to_string(player);
            CKContext* context = engine_->context();
            if (auto* existing = CK3dEntity::Cast(context->GetObjectByNameAndClass(const_cast<CKSTRING>(name.c_str()), CKCID_3DOBJECT, nullptr)))
                return existing;
            auto* source = CK3dEntity::Cast(context->GetObjectByNameAndClass(const_cast<CKSTRING>(ball_rows_[ball_type].name.c_str()), CKCID_3DOBJECT, nullptr));
            if (!source) { error = "ball entity " + ball_rows_[ball_type].name + " not found"; return nullptr; }
            CKDependencies dependencies;
            dependencies.Resize(40);
            dependencies.Fill(0);
            dependencies.m_Flags = CK_DEPENDENCIES_CUSTOM;
            dependencies[CKCID_OBJECT] = CK_DEPENDENCIES_COPY_OBJECT_NAME | CK_DEPENDENCIES_COPY_OBJECT_UNIQUENAME;
            const std::string suffix = "_BMMO_" + std::to_string(player);
            auto* clone = CK3dEntity::Cast(context->CopyObject(source, &dependencies, const_cast<CKSTRING>(suffix.c_str())));
            if (!clone) { error = "CopyObject failed for " + ball_rows_[ball_type].name; return nullptr; }
            return clone;
        }

        void apply_event(const bmmo::session_event_msg& event) {
            if (event.player == 0 || event.player == own_id_) return;
            std::string error;
            switch (event.type) {
            case bmmo::session::event_type::Physicalize: {
                CK3dEntity* entity = remote_entity(event.player, event.ball_type, error);
                if (!entity) { logf("remote physicalize: %s", error.c_str()); return; }
                auto& remote = remotes_[event.player];
                if (remote.navigation) bmmo::physics::navigation_destroy(engine_->physics(), remote.entity.c_str(), error);
                remote.navigation = false;
                if (remote.physicalized && remote.entity != entity->GetName())
                    bmmo::physics::unphysicalize(engine_->physics(), remote.entity.c_str(), error);
                entity->SetWorldMatrix(matrix_from_pose(event.position, event.rotation));
                const auto recipe = to_bridge_recipe(event.recipe);
                int join_order = 63;
                for (const auto& p: players_) if (p.id == event.player) join_order = p.join_order;
                const std::string group = "P#" + std::to_string(join_order);
                if (!bmmo::physics::physicalize(engine_->physics(), entity->GetName(), recipe, group.c_str(), error)) {
                    logf("remote physicalize %s: %s", entity->GetName(), error.c_str());
                    return;
                }
                remote.entity = entity->GetName();
                remote.ball_type = event.ball_type;
                remote.physicalized = true;
                remote.corrector.clear();
                attach_remote_navigation(event.player, remote);
                logf("remote player %u physicalized (%s) at tick %u%s", event.player, remote.entity.c_str(), event.tick,
                     remote.navigation ? ", predicted" : ", mirrored");
                break;
            }
            case bmmo::session::event_type::Unphysicalize: {
                auto it = remotes_.find(event.player);
                if (it == remotes_.end()) return;
                if (it->second.navigation) bmmo::physics::navigation_destroy(engine_->physics(), it->second.entity.c_str(), error);
                it->second.navigation = false;
                if (it->second.physicalized) bmmo::physics::unphysicalize(engine_->physics(), it->second.entity.c_str(), error);
                it->second.physicalized = false;
                logf("remote player %u unphysicalized at tick %u", event.player, event.tick);
                break;
            }
            default:
                break;
            }
        }

        void attach_remote_navigation(uint32_t player, remote_ball& remote) {
            if (!remote.physicalized || remote.navigation || !navigation_known_ || !navigation_.valid()) return;
            std::string error;
            float directions[8][3] = {};
            int count = 0;
            for (const auto& leaf: navigation_.leaves) {
                if (leaf.index < 0 || leaf.index >= 8) continue;
                directions[leaf.index][0] = leaf.direction.x;
                directions[leaf.index][1] = leaf.direction.y;
                directions[leaf.index][2] = leaf.direction.z;
                count = std::max(count, leaf.index + 1);
            }
            const std::string cam_ref = "CamRef_BMMO_" + std::to_string(player);
            const float force = remote.ball_type < ball_rows_.size() ? ball_rows_[remote.ball_type].force : 0.0f;
            if (bmmo::physics::navigation_create(engine_->physics(), remote.entity.c_str(), cam_ref.c_str(),
                                                 navigation_.ball_navigation, directions, count, force, error)) {
                remote.navigation = true;
                remote.corrector.clear();
                logf("remote %s now predicted", remote.entity.c_str());
            } else {
                logf("remote navigation for %s: %s", remote.entity.c_str(), error.c_str());
            }
        }

        void request_resync(const char* reason) {
            if (phase_ != phase::running || !assigned_) return;
            const auto now = clock_type::now();
            if (resyncs_sent_ > 0 && now - last_resync_request_ < std::chrono::seconds(2)) return;
            last_resync_request_ = now;
            consecutive_hard_ = 0;
            bmmo::session_resync_msg msg;
            msg.session = session_;
            msg.last_full_tick = last_snapshot_tick_;
            msg.serialize();
            send_bytes(msg.raw.str(), k_nSteamNetworkingSend_Reliable);
            ++resyncs_sent_;
            logf("resync requested (%s) at tick %u", reason, current_tick());
        }

        // SessionEnd: drop the mirrors and go idle; a later SessionStart of the
        // same room reloads the level (host restart) through begin_session().
        void end_session() {
            std::string error;
            for (auto& [id, remote]: remotes_) {
                if (remote.navigation) bmmo::physics::navigation_destroy(engine_->physics(), remote.entity.c_str(), error);
                if (remote.physicalized) bmmo::physics::unphysicalize(engine_->physics(), remote.entity.c_str(), error);
            }
            remotes_.clear();
            report();
            phase_ = phase::idle;
            session_ = 0;
            assigned_ = false;
            ++sessions_ended_;
        }

        void report() {
            const auto& st = corrector_.stats();
            uint64_t rc = 0, ri = 0, rb = 0, rh = 0;
            for (const auto& [id, remote]: remotes_) {
                const auto& rs = remote.corrector.stats();
                rc += rs.compared; ri += rs.ignored; rb += rs.blended; rh += rs.hard;
            }
            double mech_max = 0.0;
            for (const auto& [name, c]: mechanism_correctors_) mech_max = std::max(mech_max, c.stats().max_error);
            logf("status: phase=%d session=%u tick=%u assigned=%d frames=%lld inputs=%llu events=%llu/%llu snapshots=%llu/%llu/%llu "
                 "own_phys=%d remotes=%zu remote_inputs=%llu remote_writes=%llu remote_corr=%llu/%llu/%llu/%llu mechanisms=%zu mech_blend=%llu mech_hard=%llu mech_max_err=%.4f rebases=%d resyncs=%llu/%llu "
                 "corrections: compared=%llu ignored=%llu blended=%llu hard=%llu unmatched=%llu last_err=%.4f max_err=%.4f",
                 static_cast<int>(phase_), session_, current_tick(), assigned_ ? 1 : 0, static_cast<long long>(frames_since_anchor_),
                 static_cast<unsigned long long>(inputs_sent_), static_cast<unsigned long long>(events_sent_),
                 static_cast<unsigned long long>(events_received_), static_cast<unsigned long long>(snapshots_received_),
                 static_cast<unsigned long long>(snapshots_applied_), static_cast<unsigned long long>(snapshots_stale_),
                 own_physicalized_ ? 1 : 0, remotes_.size(), static_cast<unsigned long long>(remote_inputs_received_),
                 static_cast<unsigned long long>(remote_writes_), static_cast<unsigned long long>(rc),
                 static_cast<unsigned long long>(ri), static_cast<unsigned long long>(rb), static_cast<unsigned long long>(rh),
                 mechanism_names_.size(),
                 static_cast<unsigned long long>(mechanism_blends_), static_cast<unsigned long long>(mechanism_hard_), mech_max, rebases_,
                 static_cast<unsigned long long>(resyncs_sent_), static_cast<unsigned long long>(resyncs_done_),
                 static_cast<unsigned long long>(st.compared), static_cast<unsigned long long>(st.ignored),
                 static_cast<unsigned long long>(st.blended), static_cast<unsigned long long>(st.hard),
                 static_cast<unsigned long long>(st.unmatched), st.last_error, st.max_error);
        }

        // ------------------------------------------------------------ state
        arguments args_;
        std::unique_ptr<bmmo::sim::headless_engine> engine_;
        bmmo::physics::tick_record record_;
        HSteamNetConnection connection_ = k_HSteamNetConnection_Invalid;
        uint32_t own_id_ = 0;
        bool in_room_ = false, join_sent_ = false, ready_sent_ = false, start_sent_ = false;
        int list_retries_ = 0;
        clock_type::time_point last_ready_sent_{};
        uint32_t room_id_ = 0;
        bool ended_ = false;
        clock_type::time_point last_report_ = clock_type::now();

        phase phase_ = phase::idle;
        uint32_t session_ = 0;
        uint8_t snapshot_interval_ = 2, input_delay_ = 6;
        int32_t seed_ = 1;
        int level_ = 1;
        std::vector<bmmo::session::player_entry> players_;
        int own_join_order_ = -1;
        float spawn_position_[3] = {};
        bool spawn_known_ = false;
        float spawn_offset_[3] = {};
        VxMatrix spawn_matrix_;
        std::string own_group_;
        bool own_group_set_ = false;
        bool own_physicalized_ = false;
        int load_waited_ = 0;
        uint64_t anchor_hash_ = 0;
        bool assigned_ = false;
        uint32_t tick_base_ = 0;
        int64_t frames_since_anchor_ = -1;
        clock_type::time_point origin_;
        int rebases_ = 0;
        std::deque<std::pair<uint32_t, bmmo::session::input_frame>> input_history_;
        float previous_cam_[3][3] = {};
        bool previous_cam_valid_ = false;
        bmmo::game::navigation_graph navigation_;
        bool navigation_known_ = false;
        std::vector<ball_row> ball_rows_;
        uint8_t last_keys_ = 0;
        bool last_nav_active_ = false;
        int last_sector_ = 0;
        int trace_frames_ = 0;
        bmmo::session::body_corrector corrector_;
        std::map<std::string, bmmo::session::body_corrector> mechanism_correctors_;
        std::map<uint32_t, std::string> mechanism_names_;
        std::map<uint32_t, remote_ball> remotes_;
        bool have_snapshot_ = false;
        uint32_t last_snapshot_tick_ = 0;
        uint64_t inputs_sent_ = 0, events_sent_ = 0, events_received_ = 0;
        uint64_t snapshots_received_ = 0, snapshots_applied_ = 0, snapshots_stale_ = 0;
        uint64_t remote_writes_ = 0, mechanism_blends_ = 0, mechanism_hard_ = 0;
        uint64_t remote_inputs_received_ = 0;
        bool resync_pending_ = false;
        bool paused_once_ = false;
        int consecutive_hard_ = 0;
        clock_type::time_point last_resync_request_{};
        uint64_t resyncs_sent_ = 0, resyncs_done_ = 0, sessions_ended_ = 0;
    };
}

int main(int argc, char** argv) {
    arguments args;
    if (!parse(argc, argv, args)) { usage(); return 1; }
    role::init_socket();
    session_client client(args);
    if (!client.boot()) return 1;
    if (!client.connect()) { logf("connect failed"); return 1; }
    client.run();
    const int code = client.exit_code();
    logf("exit code %d", code);
    return code;
}
