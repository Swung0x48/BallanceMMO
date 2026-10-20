// BallanceMMOSessionClient: a headless physics-session client (design 8.6
// item 3).  Headless engine + GameNetworkingSockets client that joins a room,
// takes part in a physics session through the real protocol, drives its own
// retail ball from a recorded keyboard (bmrc), mirrors the other players'
// balls, and reports how far its own ball drifts from the server's snapshots.
// Single-threaded: the network is polled between engine frames.
//
//   BallanceMMOSessionClient --root <game dir> --server <ip:port>
//       [--name <nick>] [--level N] [--record <file.bmrc>] [--journal <file.bmjr>]
//       [--join-first | --room <id> | --host --expect N]
//       [--seconds S] [--trace] [--no-correct]
//       [--start-sector N] [--beam X Y Z TICK]
//
// --start-sector activates a later sector at the anchor and --beam puts the own
// ball at a pose at a given session tick (design 9.25): no level has a movable
// prop at its resetpoint, so without them a recorded run never touches one.
//
// --journal writes a client-kind session journal (design 9.15): the same
// records the retail mod's black box writes - own and relayed inputs, own and
// received events, this world's per-tick fingerprint, every received snapshot,
// every correction and a checkpoint of its own bodies now and then - so the
// end-to-end test exercises the client side of the format without the retail
// game.  Read it back with BallanceMMOSimTool --replay-session or
// scripts/journal_trace.py.
//
// Exit code 0 when the own ball never needed a correction, 2 otherwise.

#include "crash_report.hpp"
#include "headless_engine.hpp"

#include "CKAll.h"
#include "CKIpionManager.h"
#include "PhysicsCallback.h"

#include <entity/version.hpp>
#include <game/menu_driver.hpp>
#include <game/sector_activation.hpp>
#include <game/navigation_graph.hpp>
#include <game/script_state.hpp>
#include <message/message_all.hpp>
#include <physics/ball_navigation.hpp>
#include <physics/physics_state.hpp>
#include <cstdlib>
#include <cstring>
#include <physics/tick_record.hpp>
#include <role/role.hpp>
#include <session/correction.hpp>
#include <session/journal.hpp>
#include <session/mechanism_tracking.hpp>
#include <session/rollback.hpp>
#include <session/sector_union.hpp>
#include <game/script_replay.hpp>
#include <game/sector_reset_guard.hpp>
#include <session/spawn_impulse.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <random>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {
    using clock_type = std::chrono::steady_clock;

    // How long the world waits at the anchor for SessionAssign before it starts
    // stepping anyway (design 9.25 phase alignment).  The assignment is one
    // reliable round trip behind SessionReady, so this is a fault timeout, not
    // a schedule: a server that never answers must not freeze the client.
    constexpr std::chrono::seconds kAnchorHoldTimeout{10};

    struct arguments {
        std::string root;
        std::string server = "127.0.0.1:26676";
        std::string name = "Headless";
        int level = 1;
        std::string record;
        std::string journal;          // --journal <file.bmjr>: the session black box (design 9.15)
        // How often the box records this client's OWN world, in ticks.  The
        // default is one every ten seconds, which is a black box: enough to
        // show where a session ended up.  To find WHICH body first disagreed
        // with the server, it has to be dense enough to bisect - pass 2, the
        // same as the server's journal_checkpoint_ticks.
        uint32_t checkpoint_ticks = 660;
        // --force-rollback <every>:<depth>: rewind `depth` ticks every `every`
        // ticks, with no network reason to.  Isolates how faithful a replay is
        // from everything else: in a solo session the world is a pure function
        // of the recorded inputs, so a run with forced rollbacks must produce
        // the same per-tick hashes as one without.  Whatever differs is state
        // the replay does not restore - which is the question to answer before
        // trying to make the scripts replayable.
        uint32_t force_rollback_every = 0;
        uint32_t force_rollback_depth = 12;
        // --replay-scripts: step a replayed tick with the WHOLE frame
        // (context_->Process(), i.e. behaviours and managers) instead of the
        // physics alone.
        //
        // The replay is faithful for a navigated body to 5 mm and ruins a
        // script-driven one on the first rewind, because a mechanism's script
        // does not run in a replay and the plates take unscripted steps and
        // fall.  Running the whole frame is the direct answer to that; what it
        // cannot fix by itself is behaviour state, which is still at its live
        // value rather than the anchor tick's, and the behaviour clock, which
        // this advances by the replay's depth on top of the live frames.  So
        // this is a measurement, not a finished feature.
        //
        // MEASURED, and it is far worse, not better.  Solo, 40 forced rewinds
        // of depth 12, mean error against the same run without them:
        //   physics-only replay   ball 0.0017 m   mechanisms  23.3 m
        //   whole-frame replay    ball 24.0   m   mechanisms 103.3 m
        // It ruins the one thing that worked.  The scripts re-run for ticks
        // they already ran so one-shot actions fire twice, the behaviour clock
        // advances by the replay's depth on top of the live frames, and the
        // retail navigation script fights the bridge's own nav_input for the
        // ball.  Kept only so the experiment is repeatable; do not turn it on
        // expecting it to help.
        bool replay_scripts = false;
        bool join_first = false;
        uint32_t room = 0;
        bool host = false;
        int expect = 2;
        double seconds = 180.0;
        bool trace = false;
        // --trace-restore: dump every IVP core field (describe_cores_exact,
        // hex floats, so the comparison is exact) on both sides of a forced
        // rewind and log only the fields that moved.  Names the state the
        // restore fails to put back, per body and per field, instead of
        // inferring it from a hash that diverged forty ticks later.
        bool trace_restore = false;
        // --no-restore-recheck: do not ask IVP to recheck contacts for a
        // body the restore moved.  Isolates the recheck's own cost to
        // replay fidelity from everything else.
        bool restore_recheck = true;
        // --no-restore-impulses: reproduce the pre-v11 restore, which dropped
        // whatever IVP had queued for the next PSI.  Here so the fix can be
        // measured against itself in ONE binary - two builds' plain runs
        // disagree by more than this effect is worth.
        bool restore_impulses = true;
        // --no-restore-quats: let write_body_state re-derive the second
        // rotation quaternion from the LIVE rotation step, as it did before
        // v11.  The other half of the same A/B.
        bool restore_quaternions = true;
        // --restore-clock: put the IVP environment's clock back to the anchor
        // before the body writes (bmmo_physics_clock_state).  OPT-IN: measured
        // on the solo probe it removed the one-tick time_of_last_psi offset of
        // every replayed step (85/85 cores) and changed the body error not at
        // all (3.823e-3 -> 3.815e-3 m), and IVP's hull manager keeps
        // time-anchored state (last_vpsi_time) a rewound clock turns into
        // negative deltas; one null dereference in the mindist minimizer
        // appeared in four clock-on networked runs against none in eight
        // without.  Not proven to be the cause, not worth the risk for no gain.
        bool restore_clock = false;
        // v13, IVP's freeze bookkeeping (scripts/sleep_divergence.py measured
        // the premise: 44 bodies asleep or awake where the plain run was not,
        // after forced rewinds).  Each half is an A/B in one binary:
        // --no-restore-calm leaves the calm references at their live values,
        // --no-restore-unit the simulation units' check countdown,
        // --no-sleep-as-recorded restores a body in its LIVE sleep state (the
        // pre-v13 "keep", and births parked awake) instead of the record's.
        bool restore_calm = true;
        bool restore_unit = true;
        bool sleep_as_recorded = true;
        // --no-restore-contacts: leave a body that lost a friction contact
        // inside the window resting on nothing after the restore (v15 A/B).
        bool restore_contacts = true;
        // --contact-flags N: BMMO_PHYSICS_CONTACT_* bits (1..16 = the recorded
        // fields to copy onto a re-created point, 256 = only for a body with no
        // contact left, 512 = only against static geometry; the default is
        // 768, the measured one).
        uint32_t contact_flags = BMMO_PHYSICS_CONTACT_DEFAULT;
        // The script replay (game/script_replay.hpp): the mechanism scripts'
        // state travels with the records and every replayed tick runs their
        // frame before its physics step.  --no-script-replay is the physics-only
        // replay, for the A/B.
        bool script_replay = true;
        // --trace-plan: log which branch every body of a restore took.
        bool trace_plan = false;
        bool correct = true;
        bool rollback = true;         // design 9.6; --no-rollback falls back to blending
        int boot_ticks = 400;
        int anchor_timeout = 3000;
        int64_t pause_at = -1;        // test knob: stop ticking at this tick ...
        int pause_ms = 0;             // ... for this long (design 9.2 resync path)
        // Harness knobs (design 9.25): no level has a mechanism at its
        // resetpoint, so a recorded run that keeps the ball near the spawn
        // never touches one.  --start-sector activates a later sector at the
        // anchor (the automation's `sector` verb), --beam drops the own ball at
        // a pose at a given session tick (the `beam` verb) - together they put
        // a ball on a mechanism in a few seconds instead of a minute of driving.
        // A sector is an input to the whole session (session/sector_union.hpp):
        // --start-sector is stamped the server's sector lead ahead on the
        // first assigned frame, sent, and started here, on the server and on
        // every other member at that tick - like a crossed checkpoint, whose
        // own manager run the level's chain no longer makes
        // (game/sector_activation.hpp).
        int start_sector = 0;
        bool beam = false;
        double beam_position[3] = {};
        uint32_t beam_tick = 0;
    };

    void usage() {
        std::puts("usage: BallanceMMOSessionClient --root <game dir> --server <ip:port> [--name N] [--level N]\n"
                  "       [--record <file.bmrc>] [--journal <file.bmjr>] [--join-first | --room <id> | --host --expect N]\n"
                  "       [--seconds S] [--trace] [--no-correct] [--no-rollback] [--pause-at TICK --pause-ms MS]\n"
                  "       [--start-sector N] [--beam X Y Z TICK]");
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
            else if (arg == "--journal") { if (!(v = next())) return false; out.journal = v; }
            else if (arg == "--replay-scripts") out.replay_scripts = true;
            else if (arg == "--force-rollback") {
                if (!(v = next())) return false;
                const std::string spec = v;
                const size_t colon = spec.find(':');
                out.force_rollback_every = static_cast<uint32_t>(std::atoi(spec.substr(0, colon).c_str()));
                if (colon != std::string::npos)
                    out.force_rollback_depth = static_cast<uint32_t>(std::atoi(spec.substr(colon + 1).c_str()));
                // depth 0 is meaningful and is the sharpest probe there is:
                // restore the record for the CURRENT tick over a world that
                // already holds exactly that state and replay nothing.  A
                // pure capture->restore round trip, so any difference is the
                // write path and not the re-simulation.
            }
            else if (arg == "--checkpoint-ticks") {
                if (!(v = next())) return false;
                out.checkpoint_ticks = static_cast<uint32_t>(std::max(1, std::atoi(v)));
            }
            else if (arg == "--join-first") out.join_first = true;
            else if (arg == "--room") { if (!(v = next())) return false; out.room = static_cast<uint32_t>(std::atoi(v)); }
            else if (arg == "--host") out.host = true;
            else if (arg == "--expect") { if (!(v = next())) return false; out.expect = std::atoi(v); }
            else if (arg == "--seconds") { if (!(v = next())) return false; out.seconds = std::atof(v); }
            else if (arg == "--trace") out.trace = true;
            else if (arg == "--trace-restore") out.trace_restore = true;
            else if (arg == "--no-restore-recheck") out.restore_recheck = false;
            else if (arg == "--no-restore-impulses") out.restore_impulses = false;
            else if (arg == "--no-restore-quats") out.restore_quaternions = false;
            else if (arg == "--restore-clock") out.restore_clock = true;
            else if (arg == "--no-restore-calm") out.restore_calm = false;
            else if (arg == "--no-restore-unit") out.restore_unit = false;
            else if (arg == "--no-restore-contacts") out.restore_contacts = false;
            else if (arg == "--contact-flags") { if (!(v = next())) return false; out.contact_flags = static_cast<uint32_t>(std::atoi(v)); }
            else if (arg == "--no-sleep-as-recorded") out.sleep_as_recorded = false;
            else if (arg == "--no-script-replay") out.script_replay = false;
            else if (arg == "--trace-plan") out.trace_plan = true;
            else if (arg == "--no-correct") out.correct = false;
            else if (arg == "--no-rollback") out.rollback = false;
            else if (arg == "--boot-ticks") { if (!(v = next())) return false; out.boot_ticks = std::atoi(v); }
            else if (arg == "--pause-at") { if (!(v = next())) return false; out.pause_at = std::atoll(v); }
            else if (arg == "--pause-ms") { if (!(v = next())) return false; out.pause_ms = std::atoi(v); }
            else if (arg == "--start-sector") { if (!(v = next())) return false; out.start_sector = std::atoi(v); }
            else if (arg == "--beam") {
                for (int k = 0; k < 3; ++k) {
                    if (!(v = next())) return false;
                    out.beam_position[k] = std::atof(v);
                }
                if (!(v = next())) return false;
                out.beam_tick = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
                out.beam = true;
            }
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

    // Session journal (design 9.15), the same policy the retail mod's
    // client_journal follows: a LOCAL checkpoint every 10 s and one after a
    // rollback or a hard correction, but never more often than every half
    // second - a burst of corrections must not fill the file with bodies
    // instead of inputs.
    constexpr uint32_t kJournalCheckpointTicks = 660;
    constexpr uint32_t kJournalCheckpointGap = 33;
    constexpr uint64_t kJournalMaxBytes = 256ull << 20;

    class session_client;

    // Own-ball Physicalize capture (design 9.10): a PreSimulate callback
    // anchored to Ball Navigation, like physics_world::tick_callback - the
    // container executes it immediately (this only arms it) and again from
    // inside Simulate(), after this tick's scripts and before the PSIs, which
    // is when the retail script's Physicalize block (if it ran this tick) has
    // already created the body and nothing has moved it yet.  Execute() is
    // defined after session_client (it needs the full class).
    class own_physicalize_callback final : public PhysicsCallback {
    public:
        own_physicalize_callback(CKIpionManager* manager, CKBehavior* behavior, session_client* client,
                                 uint64_t serial, uint32_t tick)
            : PhysicsCallback(manager, behavior, 2), client_(client), serial_(serial), tick_(tick) {}
        int Execute() override;
    private:
        session_client* client_;
        uint64_t serial_;   // the live tick it was armed for (session_client::live_tick_serial_)
        uint32_t tick_;     // the session tick that live tick steps
        bool armed_ = false;
    };

    class session_client : public role {
        // Needs capture_own_physicalize() (design 9.10); kept private like the
        // rest of the frame-loop internals.
        friend class own_physicalize_callback;
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
            // BMMO_FAKE_LAG_MS=<one way ms>: reproduce a distant server against
            // a local one, so the server's own logs are at hand.
            if (const char* lag = std::getenv("BMMO_FAKE_LAG_MS"); lag && *lag) {
                const int32_t ms = static_cast<int32_t>(std::atoi(lag));
                SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Send, ms);
                SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Recv, ms);
                logf("fake packet lag: %d ms each way (round trip %d ms)", ms, ms * 2);
            }
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
            const char* stopped = "the client stopped";
            while (running_) {
                update();
                if (ended_) { stopped = "the client ended the session"; break; }
                if (std::chrono::duration<double>(clock_type::now() - started).count() > args_.seconds) {
                    logf("time limit reached");
                    stopped = "the client's time limit was reached";
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
            // A session still running when the client goes down says so in its
            // box rather than just stopping mid-file.
            close_journal(stopped);
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
                if (msg.type == bmmo::room::event_type::PlayerLeft) journal_leave_player(msg.subject);
                if (msg.type == bmmo::room::event_type::RequestDenied && msg.error != bmmo::room::error_code::None) {
                    if (join_sent_ && !in_room_) join_sent_ = false;   // retry on the next state
                }
                break;
            }
            case bmmo::SessionStart: {
                bmmo::session_start_msg msg;
                if (!fill(msg)) {
                    // A server of another physics protocol (entity/session.hpp).
                    logf("SessionStart could not be decoded: the server runs a different physics protocol than this "
                         "client (%u)", bmmo::session::PHYSICS_PROTOCOL_VERSION);
                    ended_ = true;
                    return;
                }
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
                    // A renumbering makes every record unreachable: no incoming
                    // snapshot can match a tick of the old base (design 9.25 -
                    // the retail mod does this in rebase_tick).  The registry
                    // goes with it: its rows are keyed by the old numbers.
                    rollback_.invalidate_history();
                    own_inputs_.clear();
                    mechanism_tracking_.clear();
                    resync_pending_ = true;
                    have_snapshot_ = false;
                    origin_ = clock_type::now();
                    logf("resynced: tick base %u", tick_base_);
                    journal_note("resync: tick base " + std::to_string(tick_base_));
                    break;
                }
                assigned_ = true;
                // A numbered base is the ordinary case: the server anchors the
                // members present at the start at its start lead and a late
                // joiner (or a resync) at its current tick plus the same lead,
                // so the numbering is not the one this client has been running
                // since its own anchor.  Renumber onto it, exactly as the retail
                // client does (physics_session_client.cpp).  Base 0 is the one
                // case left over: a server that still numbers a start member
                // from its own anchor leaves the numbering alone.
                if (msg.first_tick != 0) {
                    frames_since_anchor_ = 0;
                    input_history_.clear();
                    // Same as the resync above: the frames recorded under the
                    // anchor-relative numbering name ticks the server never
                    // asked us for, and nothing can match them again.
                    rollback_.invalidate_history();
                    own_inputs_.clear();
                    mechanism_tracking_.clear();
                    origin_ = clock_type::now();
                }
                tick_base_ = msg.first_tick;
                // The anchor hold ends here: the next frame is the world's
                // first step and it is numbered `tick_base_`, exactly like the
                // server's own first step (design 9.25 phase alignment).
                if (held_) {
                    held_ = false;
                    origin_ = clock_type::now();
                    logf("anchor hold released after %d polls: the first frame is tick %u", hold_polls_, tick_base_);
                    journal_note("anchor hold released at tick base " + std::to_string(tick_base_));
                }
                logf("tick base %u assigned (%lld frames since anchor)", tick_base_, static_cast<long long>(frames_since_anchor_));
                journal_note("assigned: tick base " + std::to_string(tick_base_) + " ("
                             + std::to_string(frames_since_anchor_) + " frames since the anchor)");
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
                // Queued, not applied here: an event applied on arrival lands
                // at an arbitrary point relative to this client's physics step
                // and in network arrival order, while the server applies every
                // due event before its step in the members' join order.  The
                // drain in frame() matches the server; see drain_due_events.
                if (msg.session == session_ && phase_ == phase::running) pending_events_.push_back(std::move(msg));
                break;
            }
            case bmmo::SessionRemoteInput: {
                bmmo::session_remote_input_msg msg;
                if (!fill(msg)) return;
                ++remote_inputs_received_;
                if (msg.session != session_) return;
                for (const auto& entry: msg.entries) {
                    // What the server applied for that player at that tick,
                    // whether or not this client mirrors the ball.
                    if (journalling()) {
                        journal_announce_player(entry.player, msg.tick);
                        journal_.input(msg.tick, entry.player, entry.frame, bmmo::session::JOURNAL_INPUT_RELAYED);
                    }
                    auto it = remotes_.find(entry.player);
                    if (it == remotes_.end()) continue;
                    auto& remote = it->second;
                    // A reordered frame is still authoritative for its
                    // historical tick: keep it for the rollback without
                    // letting it replace the live predictor with an older
                    // input.  The retail mod has always done it in this order
                    // (physics_session_client.cpp); doing the guard first, as
                    // this client used to, dropped a late frame on the floor
                    // and left the rollback replaying a prediction the server
                    // had already corrected.
                    remote.inputs[msg.tick] = entry.frame;
                    while (remote.inputs.size() > 128) remote.inputs.erase(remote.inputs.begin());
                    note_input_correction(remote.entity, msg.tick, entry.frame);
                    if (remote.have_input && msg.tick < remote.input_tick) continue;   // out of order
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
                if (msg.session == session_) end_session(msg.reason.empty() ? "session ended" : msg.reason);
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
            bmmo::session::input_frame applied{};
            std::map<uint32_t, bmmo::session::input_frame> inputs;   // relayed frames by tick
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
                        if (r.room_phase == bmmo::room::phase::Lobby) { target = r.id; break; }
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
            // A server of another protocol (entity/session.hpp) computes and
            // compares other things: refuse, loudly, rather than play a session
            // that cannot agree.
            if (msg.protocol != bmmo::session::PHYSICS_PROTOCOL_VERSION) {
                logf("session %u refused: the server runs physics protocol %u, this client runs %u", msg.session,
                     msg.protocol, bmmo::session::PHYSICS_PROTOCOL_VERSION);
                ended_ = true;
                return;
            }
            // A box of the previous session is closed with its own numbering,
            // before begin_session resets the tick base under it.
            close_journal("a new session started");
            rollback_.set_verbose(args_.trace);
            rollback_.set_recheck_on_restore(args_.restore_recheck);
            rollback_.set_restore_impulses(args_.restore_impulses);
            rollback_.set_restore_quaternions(args_.restore_quaternions);
            rollback_.set_restore_clock(args_.restore_clock);
            rollback_.set_restore_calm(args_.restore_calm);
            rollback_.set_restore_unit(args_.restore_unit);
            rollback_.set_restore_contacts(args_.restore_contacts);
            rollback_.set_contact_flags(args_.contact_flags);
            rollback_.set_sleep_as_recorded(args_.sleep_as_recorded);
            rollback_.set_replay_scripts(args_.script_replay);
            rollback_.set_trace_plan(args_.trace_plan);
            session_ = msg.session;
            snapshot_interval_ = msg.snapshot_interval;
            input_delay_ = msg.input_delay;
            sector_lead_ = msg.sector_lead;
            seed_ = msg.seed;
            spawn_impulse_ = msg.spawn_impulse;
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
            logf("session %u starting: level %d, %zu players, join order %d, seed %d, input delay %u, sector lead %u",
                 session_, level_, players_.size(), own_join_order_, seed_, static_cast<unsigned>(input_delay_),
                 static_cast<unsigned>(sector_lead_));
            // Fresh runtime state.
            assigned_ = false;
            held_ = false;
            hold_polls_ = 0;
            tick_base_ = 0;
            frames_since_anchor_ = -1;
            input_history_.clear();
            previous_cam_valid_ = false;
            navigation_known_ = false;
            corrector_.clear();
            mechanism_correctors_.clear();
            mechanism_tracking_.clear();
            generations_.clear();
            amend_failures_ = 0;
            beam_done_ = false;
            remotes_.clear();
            own_group_set_ = false;
            own_physicalized_ = false;
            own_body_entity_.clear();
            own_capture_.valid = false;
            start_sector_pending_ = false;
            sectors_.reset();
            sectors_requested_.clear();
            sector_cell_ = 0;
            scripts_.clear();
            script_reset_ticks_.clear();
            reset_guard_.restore(engine_->context());
            checkpoint_held_ = false;
            sector_manager_id_ = 0;
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
            level_sequencers_.clear();
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
            // Design 9.26: the mechanism Sequencer counters as the level file
            // has them, recorded on first sight while the level loads.
            if (const int recorded = level_sequencers_.capture_new(engine_->context()); recorded > 0)
                logf("%d mechanism Sequencer counter(s) recorded from the level file (%s)", recorded,
                     level_sequencers_.describe().c_str());
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
            {
                // Design 9.26: the mechanisms start from the level file's
                // Sequencer counters on every side.
                const int restored = level_sequencers_.restore(engine_->context());
                logf("%d of %zu mechanism Sequencer counter(s) restored to the level file's (%s)", restored,
                     level_sequencers_.size(), level_sequencers_.describe().c_str());
            }
            bmmo::physics::world_hash hash;
            if (!bmmo::physics::capture_world_hash(physics, hash, error)) { logf("hash: %s", error.c_str()); ended_ = true; return; }
            if (!bmmo::physics::install_player_collision_filter(physics, "P#", error)) logf("collision filter: %s", error.c_str());
            bmmo::physics::drain_event_log(physics);
            read_ball_rows();
            anchor_hash_ = hash.pose;
            // Sectors are inputs to the whole session (session/sector_union.hpp):
            // the level's own checkpoint chain no longer runs the manager here
            // (the server's world holds its copy too); the detect block in the
            // frame loop stamps whatever sector the chain writes, --start-sector
            // (design 9.25 harness) is stamped on the first assigned frame, and
            // the union runs the passes - ours and the other members' - before
            // the step of the tick each is due at.  Sector 1 is the level's own
            // on every side, and the anchor hash is unaffected: nothing is
            // activated before the server's world has.
            sectors_requested_ = {1};
            if (CKDataArray* parameters = engine_->data_array("IngameParameter"))
                parameters->GetElementValue(0, 1, &sector_cell_);
            // The script replay (game/script_replay.hpp): the mechanism scripts
            // whose state travels with the rollback records, and the hooks
            // that carry the force blocks' controllers through the bridge.
            scripts_ = args_.script_replay ? bmmo::game::script_replay::select(engine_->context()) : bmmo::game::script_replay::selection{};
            {
                CKIpionManager* physics = engine_->physics();
                script_hooks_.capture_force = [physics](CKBehavior* block, bmmo::game::force_block_state& out) {
                    bmmo_physics_force_block state{};
                    std::string error;
                    if (!bmmo::physics::force_block_get(physics, static_cast<uint32_t>(block->GetID()), state, error)) return false;
                    out.exists = state.exists != 0;
                    for (int k = 0; k < 3; ++k) { out.position[k] = state.position[k]; out.force[k] = state.force[k]; }
                    return true;
                };
                script_hooks_.restore_force = [physics](CKBehavior* block, const bmmo::game::force_block_state& in) {
                    bmmo_physics_force_block state{};
                    state.exists = in.exists ? 1 : 0;
                    for (int k = 0; k < 3; ++k) { state.position[k] = in.position[k]; state.force[k] = in.force[k]; }
                    std::string error;
                    return bmmo::physics::force_block_set(physics, static_cast<uint32_t>(block->GetID()), state, error);
                };
            }
            if (!scripts_.empty()) {
                const auto st = bmmo::game::script_replay::describe(engine_->context(), scripts_);
                logf("script replay: %d scripts (%d active) + %d satellite blocks, %d blocks, %d parameters, %d force blocks, %d opaque: %s",
                     st.scripts, st.active, st.satellites, st.blocks, st.parameters, st.force_blocks, st.opaque,
                     bmmo::game::script_replay::names(engine_->context(), scripts_).c_str());
            } else if (args_.script_replay) {
                logf("script replay: no mechanism script in this level");
            }
            // The retail sector resets (a death's, a level reset's) neutralized
            // for the session, as the retail mod does (game/sector_reset_guard.hpp):
            // the server's world never resets, so neither may this client.
            {
                const std::string trouble = reset_guard_.resolve(engine_->context());
                if (!trouble.empty())
                    logf("the retail sector resets were NOT neutralized (%s): a death will reset this client's sector", trouble.c_str());
            }
            resetpoint_seen_ = level_resetpoint();
            {
                std::string hold_error;
                checkpoint_held_ = bmmo::game::hold_checkpoint_activation(engine_->context(), hold_error) != nullptr;
                if (checkpoint_held_)
                    logf("the level's checkpoint sector activation is held: a crossing is stamped %u ticks ahead",
                         static_cast<unsigned>(sector_lead_));
                else
                    logf("the level's checkpoint sector activation was NOT held (%s): crossings activate here at "
                         "once and are reported after the fact - this client is out of lockstep on every sector it enters",
                         hold_error.c_str());
            }
            if (args_.start_sector > 0) {
                start_sector_pending_ = true;
                logf("--start-sector %d will be stamped %u ticks ahead of the first assigned frame", args_.start_sector,
                     static_cast<unsigned>(sector_lead_));
            }
            frames_since_anchor_ = 0;
            phase_ = phase::running;
            origin_ = clock_type::now();
            rebases_ = 0;
            // Hold the world here until the tick base arrives (design 9.25
            // phase alignment): the first frame this client steps must be the
            // first frame the server's world steps, the one both of them number
            // with the start base.  The network keeps being polled while we
            // wait; nothing else in the frame runs, so no input, no journal
            // TICK record and no rollback record is made for a tick that does
            // not exist.
            held_ = true;
            hold_polls_ = 0;
            hold_deadline_ = clock_type::now() + kAnchorHoldTimeout;
            bmmo::session_ready_msg ready;
            ready.session = session_;
            ready.first_tick = 0;
            ready.anchor_hash = hash.pose;
            ready.anchor_surfaces = hash.surfaces;
            ready.physics_sha256 = sizeof(void*) == 8 ? "headless-64" : "headless-32";
            ready.build_id = bmmo::physics::build_id();
            ready.serialize();
            send_bytes(ready.raw.str(), k_nSteamNetworkingSend_Reliable);
            logf("anchored after %d load ticks: pose %016llx surfaces %016llx cores=%d factor=%.6f", load_waited_,
                 static_cast<unsigned long long>(hash.pose), static_cast<unsigned long long>(hash.surfaces), hash.cores,
                 hash.time_factor);
            // The box starts where the session does: at the anchor the server
            // has to agree with, before the tick base is known.
            open_journal(hash);
        }

        // The lead the server sized for this session (SessionStart).
        uint32_t sector_lead_ticks() const { return sector_lead_; }

        // A sector this client enters, as an input: stamped for `due`, sent at
        // once, and handed to the union, which starts it before the step of
        // that tick (or the first one after it the pacer allows) - as the
        // server does with the event, and every other member with its relay.
        // Once per sector per session.
        void hold_sector(int sector, uint32_t due, const char* why) {
            if (sectors_requested_.count(sector)) return;
            sectors_requested_.insert(sector);
            bmmo::session_event_msg event;
            event.tick = due;
            event.type = bmmo::session::event_type::Sector;
            event.sector = sector;
            send_event(event);
            sectors_.stamp(own_id_, sector, due);
            logf("sector %d stamped for tick %u (%s, lead %u, input_delay %u), event sent ahead", sector, due, why,
                 sector_lead_ticks(), static_cast<unsigned>(input_delay_));
        }

        // One pass of the sector union: deactivate nothing, activate `sector`,
        // run Gameplay_SectorManager from its start - the writes the server's
        // update_sectors() makes.
        void start_sector(int sector) {
            CKDataArray* parameters = engine_->data_array("IngameParameter");
            CKBehavior* manager = bmmo::game::find_root_script(engine_->context(), "Gameplay_SectorManager");
            if (!parameters || !manager) {
                logf("--start-sector %d: IngameParameter or Gameplay_SectorManager is missing", sector);
                return;
            }
            int none = 0;
            parameters->SetElementValue(0, 2, &none, sizeof(none));
            parameters->SetElementValue(0, 1, &sector, sizeof(sector));
            if (CKScene* scene = engine_->context()->GetCurrentScene()) scene->Activate(manager, TRUE);
        }

        // The automation `beam` verb, headless: the own ball at `position`,
        // upright and at rest.  The server's copy is moved by re-reporting the
        // ball's life at that pose - a plain repeat Physicalize would not move
        // it, because physicalize() of an entity that already has a body is a
        // no-op (physics_state.cpp:741), so the pair the retail death sends is
        // used instead.
        void beam_own_ball(uint32_t tick) {
            CK3dEntity* ball = current_ball();
            const std::string name = ball && ball->GetName() ? ball->GetName() : "";
            if (name.empty() || !own_physicalized_) {
                logf("--beam at tick %u: no own body to beam", tick);
                return;
            }
            const double rotation[4] = {0.0, 0.0, 0.0, 1.0};
            const float still[3] = {0.0f, 0.0f, 0.0f};
            std::string error;
            if (!bmmo::physics::set_body_state(engine_->physics(), name.c_str(), args_.beam_position, rotation,
                                               still, still, true, error)) {
                logf("--beam at tick %u: %s", tick, error.c_str());
                return;
            }
            ++generations_[name];   // the server rebuilds its copy: a new lifetime
            bmmo::session_event_msg down;
            down.tick = tick + 1;
            down.type = bmmo::session::event_type::Unphysicalize;
            send_event(down);
            bmmo::session_event_msg up;
            up.tick = tick + 1;
            up.type = bmmo::session::event_type::Physicalize;
            up.ball_type = static_cast<uint8_t>(std::max(ball_type_of(name), 0));
            for (int k = 0; k < 3; ++k) up.position[k] = static_cast<float>(args_.beam_position[k]);
            for (int r = 0; r < 3; ++r)
                for (int k = 0; k < 3; ++k) up.rotation[r * 3 + k] = r == k ? 1.0f : 0.0f;
            fill_retail_recipe(ball_type_of(name), up.recipe);
            send_event(up);
            logf("--beam: %s put at (%.3f,%.3f,%.3f) at tick %u, re-reported to the server for tick %u", name.c_str(),
                 args_.beam_position[0], args_.beam_position[1], args_.beam_position[2], tick, tick + 1);
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

        // The level's current resetpoint, CurrentLevel[0,3] - where the retail
        // death respawns the ball.  Only Gameplay_Events moves it: on a
        // "Checkpoint reached" from the armed PC_TwoFlames flame, which fires
        // when THIS world's Ball_Pos_Frame (the own ball) comes within 6.5 of
        // the flame.  A sector union pass never touches it.
        VxVector level_resetpoint() const {
            VxMatrix reset;
            CKDataArray* level = engine_->data_array("CurrentLevel");
            if (!level || !level->GetElementValue(0, 3, &reset)) return VxVector(0.0f, 0.0f, 0.0f);
            return VxVector(reset[3][0], reset[3][1], reset[3][2]);
        }

        // Arms own_physicalize_callback for the live tick `serial`, which
        // steps session tick `tick`: every tick, because a spawn, a respawn
        // and a trafo all create the own ball's body inside a frame's scripts.
        void arm_own_physicalize_capture(uint64_t serial, uint32_t tick) {
            if (!navigation_known_) return;
            CKIpionManager* physics = engine_->physics();
            if (!physics || !physics->m_PreSimulateCallbacks) return;
            if (auto* behavior = CKBehavior::Cast(engine_->context()->GetObject(navigation_.ball_navigation)))
                physics->m_PreSimulateCallbacks->Process(new own_physicalize_callback(physics, behavior, this, serial, tick));
        }

        // The own ball's Physicalize as the retail script made it (design
        // 9.10), from own_physicalize_callback's PreSimulate pass of the live
        // tick: after this tick's scripts, so a Physicalize block that ran
        // this tick has already created the body, and before the PSIs, so
        // neither the body nor the entity has moved since (balls born on one
        // point have pushed each other apart by the end of the tick, and a
        // kicked one has rolled).  What a retail client sends from its
        // OnPhysicalize hook (physics_session_client.cpp): the entity's world
        // matrix, flagged a spawn when it is the level's resetpoint
        // CurrentLevel[0,3] as it is NOW - a respawn is at the resetpoint of
        // the last checkpoint crossed, not at the level's first.  The own kick
        // follows the flag, so this ball and every other side's copy of it get
        // the same impulse or none.  `tick` is the tick report_physicalize
        // sends the capture with, once the event log confirms the birth.
        void capture_own_physicalize(uint32_t tick) {
            CK3dEntity* ball = current_ball();
            if (!ball || !ball->GetName()) return;
            const std::string name = ball->GetName();
            if (name == own_body_entity_) return;   // the body was there before this tick
            bmmo_physics_body_state state{};
            std::string error;
            if (!bmmo::physics::get_body_state(engine_->physics(), name.c_str(), state, error)) return;   // no body yet
            own_capture_.valid = true;
            own_capture_.tick = tick;
            own_capture_.entity = name;
            own_capture_.pose = ball->GetWorldMatrix();
            own_capture_.spawn = false;
            if (CKDataArray* level = engine_->data_array("CurrentLevel")) {
                VxMatrix reset;
                if (level->GetElementValue(0, 3, &reset)) {
                    const float dx = own_capture_.pose[3][0] - reset[3][0];
                    const float dy = own_capture_.pose[3][1] - reset[3][1];
                    const float dz = own_capture_.pose[3][2] - reset[3][2];
                    own_capture_.spawn = (dx * dx + dy * dy + dz * dz) < 1e-6f;
                }
            }
            if (!own_capture_.spawn || spawn_impulse_ <= 0.0f) return;
            const uint8_t join_order = static_cast<uint8_t>(own_join_order_ < 0 ? 63 : own_join_order_);
            const uint32_t idx = bmmo::session::spawn_direction_index(seed_, join_order, tick);
            if (!bmmo::physics::push_impulse(engine_->physics(), name.c_str(), bmmo::session::kSpawnDirectionTable[idx],
                        spawn_impulse_, 0, error))
                logf("spawn impulse: %s", error.c_str());
            else
                logf("spawn impulse index=%u at tick %u", idx, tick);
        }

        bool navigation_active() const {
            CKContext* context = engine_->context();
            for (const auto& leaf: navigation_.leaves)
                if (auto* key_event = CKBehavior::Cast(context->GetObject(leaf.key_block)))
                    if (key_event->IsActive()) return true;
            return false;
        }

        void pace_and_frame() {
            // Design 9.25 phase alignment: the world stands at the anchor until
            // SessionAssign arrives.  Its first step and the server world's
            // first step are then the same tick number, so the two worlds are
            // the same number of steps from their anchors at every tick of the
            // session and a script-driven mechanism runs in phase on both.
            // (The frames this used to run were renumbered away by the
            // assignment anyway - they cost inputs the server never asked for
            // and bought nothing but the offset.)  A bounded wait: if the
            // assignment never comes the client runs on rather than sitting
            // here for ever, and says so.
            if (held_) {
                ++hold_polls_;
                if (clock_type::now() < hold_deadline_) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    return;   // poll the network, step nothing
                }
                held_ = false;
                origin_ = clock_type::now();
                logf("anchor hold: no tick base after %.1f s; running anyway (the world will be out of phase with "
                     "the server's)", std::chrono::duration<double>(kAnchorHoldTimeout).count());
                journal_note("anchor hold timed out; running without a tick base");
            }
            // Frames run from the assignment, and the server's world starts at
            // the same tick: a client that stepped between its anchor and the
            // assignment would be that many steps ahead of the world for good.
            // Inputs before the assignment queue up and flush_inputs() sends
            // them when the tick base lands.
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
            // Where the server applies them: after this tick's inputs and
            // before its physics step (session_runner::step).  current_tick()
            // is not advanced to this frame yet, so name the tick the engine is
            // about to simulate - the one current_tick() returns once
            // frames_since_anchor_ is advanced to f below, and so the tick
            // report_physicalize sends for this very frame.
            const uint32_t frame_tick = tick_base_ + static_cast<uint32_t>(std::max<int64_t>(f - 1, 0));
            // A held --start-sector: on the first frame the session is assigned
            // (tick base and lead known), stamp it `lead` ticks ahead like any
            // other sector this client enters.
            if (start_sector_pending_ && assigned_) {
                start_sector_pending_ = false;
                hold_sector(args_.start_sector, frame_tick + sector_lead_ticks(), "--start-sector");
            }
            // This tick's events first - a relayed Sector event stamped for it
            // among them - then the union's pass, then the step: the order the
            // server's session_runner::step keeps.
            drain_due_events(frame_tick);
            // The sector resets stay neutralized (the level-reset one once the
            // sector is up: this client has resolved a mechanism of its own).
            for (const auto& chain: reset_guard_.apply(engine_->context(), mechanism_tracking_.resolved() > 0))
                logf("the sector reset in %s (Execute Script -> Gameplay_SectorManager) is neutralized", chain.c_str());
            // The sector union (session/sector_union.hpp), the rule the server's
            // update_sectors() runs: one count of the pacer per step, before
            // it, from the manager's state after the previous step, and one
            // pass per idle manager for the lowest sector owed.  The two worlds
            // run the same scripts from the same anchor on the same stamps, so
            // the tick a pass starts at is the same on every side.
            {
                if (!sector_manager_id_)
                    if (CKBehavior* manager = bmmo::game::find_root_script(engine_->context(), "Gameplay_SectorManager"))
                        sector_manager_id_ = manager->GetID();
                auto* manager = CKBehavior::Cast(engine_->context()->GetObject(sector_manager_id_));
                const auto pass = sectors_.tick(frame_tick, manager && manager->IsActive());
                if (pass) {
                    sector_cell_ = pass.sector;   // the detect block below must not take our own write for a crossing
                    start_sector(pass.sector);
                    logf("sector +%d at tick %u (player %u, stamped %u%s), running: %s", pass.sector, frame_tick,
                         pass.player, pass.stamped_tick, frame_tick > pass.stamped_tick ? ", held by the pacer" : "",
                         [this] { std::string s; for (int x: sectors_.active()) s += std::to_string(x) + " "; return s; }().c_str());
                }
            }
            // After the events, so a birth and an input correction in the same
            // frame leave the world replayed from the earlier of the two.
            replay_corrected_inputs();
            maybe_force_rollback();
            // The own ball's Physicalize is captured inside this tick's
            // Simulate(), before the PSIs (capture_own_physicalize).  Armed
            // here, after the rewinds above: a replayed step runs the
            // PreSimulate pass too, and only the live tick may capture.
            live_tick_serial_ = ++live_ticks_;
            arm_own_physicalize_capture(live_tick_serial_, frame_tick);
            const bool ticked = engine_->tick(error);
            live_tick_serial_ = 0;
            if (!ticked) { logf("tick failed: %s", error.c_str()); ended_ = true; return; }
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
            if (!ball_name.empty() && (!body_guard_ || body_guard_entity_ != ball_name)) {
                if (bmmo::physics::set_body_guard(physics, true, ball_name.c_str(), error)) {
                    body_guard_ = true;
                    body_guard_entity_ = ball_name;
                } else
                    logf("body guard: %s", error.c_str());
            }
            bmmo_physics_body_state own{};
            const bool physicalized = !ball_name.empty() && bmmo::physics::get_body_state(physics, ball_name.c_str(), own, error);
            if (physicalized) {
                if (!own_group_set_) {
                    if (bmmo::physics::set_body_group(physics, ball_name.c_str(), own_group_.c_str(), error)) own_group_set_ = true;
                    else logf("own ball group: %s", error.c_str());
                }
                if (args_.rollback && navigation_known_ && (!own_navigation_ || own_nav_entity_ != ball_name))
                    attach_own_navigation(ball_name, ball_type);
                bmmo::session::ball_pose pose;
                pose.tick = tick;
                for (int k = 0; k < 3; ++k) { pose.position[k] = own.position[k]; pose.linear[k] = own.linear[k]; pose.angular[k] = own.angular[k]; }
                for (int k = 0; k < 4; ++k) pose.rotation[k] = own.rotation[k];
                corrector_.record(pose);
            } else {
                own_group_set_ = false;
            }
            own_physicalized_ = physicalized;
            own_body_entity_ = physicalized ? ball_name : std::string();

            // Mechanism states after this tick (the legacy blending path;
            // with the rollback engine on, the engine keeps its own records).
            for (const auto& name: mechanism_tracking_.tracked_entities()) {
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

            // Lifecycle events from the bridge's object log, read before the
            // rollback record below: a body created or deleted during this
            // frame is already in the world that record describes, so the new
            // lifetime generation belongs on THAT record (design 9.25).
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
                if (kind == "created" || kind == "deleted") {
                    // The body behind a tracked name changed: a new lifetime for
                    // it, and the mechanism registry has to ask the world again.
                    if (!name.empty()) {
                        ++generations_[name];
                        mechanism_tracking_.mark_dirty();
                    }
                }
                if (kind == "created" && !ball_name.empty() && name == ball_name) {
                    report_physicalize(ball, ball_name, ball_type, tick);
                } else if (kind == "deleted" && !ball_name.empty() && name == ball_name) {
                    bmmo::session_event_msg event;
                    event.tick = tick;
                    event.type = bmmo::session::event_type::Unphysicalize;
                    send_event(event);
                    own_group_set_ = false;
                    logf("own ball unphysicalized at tick %u", tick);
                } else if (kind == "script_wakeup" && !name.empty() && name.rfind("Ball_", 0) != 0
                           && name.find("_BMMO_") == std::string::npos && !revived_reported.count(name)) {
                    revived_reported.insert(name);
                    bmmo::session_event_msg event;
                    event.tick = tick;
                    event.type = bmmo::session::event_type::BodyRevived;
                    event.name = name;
                    send_event(event);
                }
            }
            // Which mechanism rows are about one of our bodies, before the
            // record: the tracked set of a tick has to name bodies it contains.
            if (mechanism_tracking_.due(tick))
                mechanism_tracking_.resolve(tick, [physics](const char* entity, bmmo_physics_body_state& out) {
                    std::string probe_error;
                    return bmmo::physics::get_body_state(physics, entity, out, probe_error);
                });

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
            if (own_navigation_) {
                if (!bmmo::physics::navigation_input(physics, own_nav_entity_.c_str(), 0, basis[0], basis[1], basis[2], false, error))
                    logf("own navigation rows: %s", error.c_str());
                zero_retail_forces();
            }
            input_history_.emplace_back(tick, input);
            // The frame is finished: journal it before it goes on the wire, so
            // the box holds what this client played even when the send is lost.
            if (journalling()) journal_.input(tick, own_id_, input, bmmo::session::JOURNAL_INPUT_FRESH);
            if (args_.rollback) {
                own_inputs_[tick] = input;
                while (own_inputs_.size() > 128) own_inputs_.erase(own_inputs_.begin());
                bmmo::session::rollback_tracked tracked;
                std::map<std::string, bmmo::session::input_frame> applied;
                if (physicalized && own_navigation_) {
                    tracked.own_entity = own_nav_entity_;
                    tracked.own_polls = true;
                    applied[own_nav_entity_] = input;
                }
                for (const auto& [id, remote]: remotes_)
                    if (remote.physicalized && remote.navigation) {
                        tracked.remote_entities.push_back(remote.entity);
                        applied[remote.entity] = remote.applied;
                    }
                // Design 9.25: only the mechanism names this client has a body
                // for - a tracked name with no body is dropped by every
                // capture, which used to change the tracked set every tick.
                // Every movable body this client has, not only the ones the
                // server has named and the registry resolved.
                //
                // A body missing from the tracked set is not recorded and so
                // not restored - but the replay's step() still steps it, so
                // every rollback advances it by the replay's depth.  Measured
                // solo with forced rewinds: a P_Modul_29 plate, which hangs
                // from physics hinges and swings continuously, came out of a
                // depth-12 rewind at exactly the pose the un-rewound run had
                // 12 ticks LATER - not displaced, phase-shifted by the depth.
                // `mechanisms=22/12` had been in every run all along: ten of
                // the client's own bodies were being stepped by every replay
                // and restored by none.
                //
                // So the tracked set is the client's own world, which is what
                // the replay has to put back. A body the snapshot says nothing
                // about is restored from our own record, which is right.
                for (const auto& name: movable_body_names()) tracked.mechanisms.push_back(name);
                tracked.generations = generations_;
                for (CK_ID id: bmmo::game::script_replay::resets_pending(engine_->context(), scripts_)) {
                    script_reset_ticks_[id] = tick;
                    if (script_reset_logs_++ < 12) {
                        auto* beh = CKBehavior::Cast(engine_->context()->GetObject(id));
                        logf("script replay: %s is told to reset at tick %u; a rewind anchored earlier keeps it live",
                             beh && beh->GetName() ? beh->GetName() : "?", tick);
                    }
                }
                rollback_.record(rollback_world(), tick, tracked, applied);
            }
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

            // Sector: the sector the level's scripts last wrote.  With the
            // checkpoint chain held this is a crossing whose manager run did
            // not happen: it becomes an input, stamped ahead like the retail
            // client's cell poll (physics_session_frame).  This frame's step was
            // `tick`, so the earliest the manager can run is before tick + 1.
            // Without the hold (a graph the hold did not understand) the chain
            // already activated it: report it after the fact, out of lockstep.
            //
            // A moved resetpoint is a crossing too, the own ball's alone (only
            // the checkpoint chain moves it, in the frame it writes the cell).
            // It is what catches a trailing ball: a peer's crossing has the
            // union pass write the same sector into the cell first, so the
            // chain's own write changes nothing there - the retail client
            // reports that crossing from OnPostCheckpointReached.
            const VxVector reset = level_resetpoint();
            const bool resetpoint_moved = reset != resetpoint_seen_;
            if (resetpoint_moved) {
                resetpoint_seen_ = reset;
                VxVector at(0.0f, 0.0f, 0.0f);
                if (CK3dEntity* ball = current_ball()) ball->GetPosition(&at);
                logf("resetpoint now (%.2f,%.2f,%.2f) at tick %u, own ball at (%.2f,%.2f,%.2f)",
                     reset.x, reset.y, reset.z, tick, at.x, at.y, at.z);
            }
            if (CKDataArray* parameters = engine_->data_array("IngameParameter")) {
                int sector = 0;
                if (parameters->GetElementValue(0, 1, &sector) && (sector != sector_cell_ || resetpoint_moved)) {
                    sector_cell_ = sector;
                    if (sector > 0 && !sectors_requested_.count(sector)) {
                        if (checkpoint_held_) {
                            hold_sector(sector, tick + 1 + sector_lead_ticks(), "checkpoint");
                        } else {
                            sectors_requested_.insert(sector);
                            bmmo::session_event_msg event;
                            event.tick = tick;
                            event.type = bmmo::session::event_type::Sector;
                            event.sector = sector;
                            send_event(event);
                            logf("sector %d at tick %u", sector, tick);
                        }
                    }
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
                remote.applied = remote.input;
                remote.applied.keys = keys;
                if (!active) remote.applied.flags &= static_cast<uint8_t>(~bmmo::session::INPUT_FLAG_NAV_ACTIVE);
                if (!bmmo::physics::navigation_input(physics, remote.entity.c_str(), keys, remote.input.cam_right,
                                                     remote.input.cam_up, remote.input.cam_dir, active, error))
                    logf("remote navigation input %s: %s", remote.entity.c_str(), error.c_str());
            }
            // --beam (design 9.25 harness): at its tick, once.
            if (args_.beam && !beam_done_ && tick >= args_.beam_tick) {
                beam_done_ = true;
                beam_own_ball(tick);
            }
            // The record for this tick was captured before the corrections
            // above, so a rollback to it would restore the divergent pose and
            // lose the correction on the first re-simulated tick.  Re-capture
            // it now, exactly like the retail mod (design 9.25 item 4 of
            // findings/A5 section 3.1: the headless client never did).
            if (args_.rollback && !rollback_.amend_record(rollback_world(), tick)) {
                if (amend_failures_++ == 0)
                    logf("amend_record found no history entry for tick %u (the tick was dropped)", tick);
            }
            // --trace: what pushes the own ball - the navigation replica's
            // leaves, every controller on its core, the mechanism force blocks
            // whose target it is - logged whenever that changes.
            if (args_.trace && own_physicalized_ && own_navigation_) {
                std::string text;
                bmmo_physics_nav_state nav{};
                std::string nav_error;
                if (bmmo::physics::navigation_get_state(physics, own_nav_entity_.c_str(), nav, nav_error)) {
                    char buf[200];
                    std::snprintf(buf, sizeof(buf), "nav active=%d keys=%02x ctl=%02x pending=%02x", nav.active, nav.key_mask,
                                  nav.controller_mask, nav.create_pending_mask);
                    text += buf;
                    for (int i = 0; i < 8; ++i)
                        if (nav.controller_mask & (1u << i)) {
                            std::snprintf(buf, sizeof(buf), " f%d=(%.3f,%.3f,%.3f)", i, nav.force[i][0], nav.force[i][1], nav.force[i][2]);
                            text += buf;
                        }
                }
                text += " | core: " + bmmo::physics::describe_controllers(physics, own_nav_entity_.c_str());
                if (!scripts_.empty()) {
                    CK3dEntity* ball = current_ball();
                    const std::string blocks = bmmo::game::script_replay::describe_force_blocks(engine_->context(), scripts_, script_hooks_,
                                                                                               ball ? ball->GetID() : 0);
                    if (!blocks.empty()) text += " | blocks on the ball: " + blocks;
                }
                if (text != last_ball_forces_) {
                    logf("ball forces at tick %u: %s", tick, text.c_str());
                    last_ball_forces_ = text;
                }
                // The contacts, whenever a partner, the keeper counter or the
                // pull counter changes (the floats change every tick).
                const std::string contacts = bmmo::physics::describe_contacts(physics, own_nav_entity_.c_str());
                std::string shape;
                for (size_t i = 0; i < contacts.size(); ++i) {
                    if (contacts.compare(i, 5, " gap=") == 0 || contacts.compare(i, 3, " s=") == 0
                            || contacts.compare(i, 7, " press=") == 0 || contacts.compare(i, 4, " mu=") == 0
                            || contacts.compare(i, 5, " len=") == 0) {
                        const size_t end = contacts.find_first_of(" }", i + 1);
                        i = (end == std::string::npos ? contacts.size() : end) - 1;
                        continue;
                    }
                    shape += contacts[i];
                }
                if (shape != last_ball_contacts_) {
                    logf("ball contacts at tick %u: %s", tick, contacts.c_str());
                    last_ball_contacts_ = shape;
                }
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
            // The box's own view of this frame, last: the fingerprint of the
            // world the corrections above have finished with, and the bodies
            // when a checkpoint is due.
            if (journalling()) {
                bmmo::physics::world_hash hash;
                std::string hash_error;
                if (bmmo::physics::capture_world_hash(physics, hash, hash_error))
                    journal_.tick(tick, hash, journal_ms());
                if (journal_checkpoint_due(tick)) journal_local_checkpoint(tick);
            if (args_.trace && tick % 200 == 0) report_queued_impulses(tick);
            }
        }

        void report_physicalize(CK3dEntity* ball, const std::string& ball_name, int ball_type, uint32_t tick) {
            bmmo::session_event_msg event;
            event.tick = tick;
            event.type = bmmo::session::event_type::Physicalize;
            event.ball_type = static_cast<uint8_t>(ball_type < 0 ? 0 : ball_type);
            // The pose the retail script physicalized at and the spawn flag
            // (design 9.10), both taken before the step moved the body
            // (capture_own_physicalize).  Without a capture of this birth -
            // no Ball Navigation yet to anchor the callback to - the entity
            // after the tick is all there is, and it is never flagged: no
            // kick was applied here, so none may be applied anywhere else.
            const bool captured = own_capture_.valid && own_capture_.tick == tick && own_capture_.entity == ball_name;
            const VxMatrix& used = captured ? own_capture_.pose : ball->GetWorldMatrix();
            if (!captured) logf("warning: physicalize pose taken from the entity after the tick (no capture before the step)");
            for (int k = 0; k < 3; ++k) event.position[k] = used[3][k];
            for (int r = 0; r < 3; ++r)
                for (int k = 0; k < 3; ++k) event.rotation[r * 3 + k] = used[r][k];
            const bool spawn = captured && own_capture_.spawn;
            if (spawn) event.flags |= bmmo::session::PHYSICALIZE_FLAG_SPAWN;
            own_capture_.valid = false;
            fill_retail_recipe(ball_type, event.recipe);
            send_event(event);
            own_group_set_ = false;
            if (args_.trace) trace_frames_ = 12;
            logf("own ball %s physicalized at tick %u (type %d%s) pos=%a,%a,%a", ball_name.c_str(), tick, ball_type,
                 spawn ? ", spawn" : "", static_cast<double>(event.position[0]), static_cast<double>(event.position[1]),
                 static_cast<double>(event.position[2]));
        }

        void send_event(bmmo::session_event_msg& event) {
            event.session = session_;
            event.serialize();
            // ReliableNoNagle, not Reliable: a reliable message waits up to the
            // Nagle delay to be batched, and this frame's input goes out
            // unreliable with no delay a moment later - so an event stamped
            // for this tick could arrive after the input that lets the server
            // step it, and be applied a tick late (the server logs it: "late
            // by 1", seen on BodyRevived and Physicalize).
            send_bytes(event.raw.str(), k_nSteamNetworkingSend_ReliableNoNagle);
            ++events_sent_;
            // Own events go on the wire with player 0; the box names the
            // player the server will fill in.
            journal_lifecycle_event(own_id_, event);
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
            // Everything the network delivered, stale rows included: what the
            // server said about a tick is authoritative whether or not this
            // client still had a use for it.
            if (journalling()) {
                // The full flag rides along: a full snapshot is the one that
                // carries the mechanism name dictionary, and a reader has no
                // other way to tell which received rows hold it.
                const uint8_t flags = static_cast<uint8_t>(bmmo::session::JOURNAL_CHECKPOINT_RECEIVED
                    | (snapshot.full ? bmmo::session::JOURNAL_CHECKPOINT_FULL : 0));
                journal_.checkpoint(snapshot.tick, flags, snapshot.bodies);
            }
            if (have_snapshot_ && snapshot.tick <= last_snapshot_tick_ && !snapshot.full) { ++snapshots_stale_; return; }
            // Diagnostic: did the server have our input for the tick it just
            // simulated, or did it fall back on the last one it had?  The
            // input carries the camera rows, which change every tick, so a
            // stale frame steers the ball slightly differently.
            if (assigned_) {
                ++input_reports_;
                if (snapshot.acked_input_tick < snapshot.tick) {
                    const uint32_t behind = snapshot.tick - snapshot.acked_input_tick;
                    ++input_starved_;
                    if (behind > worst_starved_) worst_starved_ = behind;
                }
                lead_sum_ += current_tick() > snapshot.tick ? current_tick() - snapshot.tick : 0;
                ++lead_count_;
            }
            std::string error;
            CKIpionManager* physics = engine_->physics();
            CK3dEntity* ball = current_ball();
            const std::string ball_name = ball && ball->GetName() ? ball->GetName() : "";
            if (resync_pending_) {
                if (!snapshot.full) return;
                have_snapshot_ = true;
                last_snapshot_tick_ = snapshot.tick;
                ++snapshots_applied_;
                // A hard world overwrite: the records collected while we waited
                // for this snapshot describe the world it is about to replace.
                rollback_.invalidate_history();
                // The dictionary and the newest rows first, then one resolve
                // pass - this is the moment the registry knows the most about a
                // world that has just been rebuilt (design 9.25).
                for (const auto& body: snapshot.bodies)
                    if (body.kind == bmmo::session::body_kind::Mechanism) {
                        if (!body.name.empty()) mechanism_tracking_.note_name(body.owner, body.name);
                        mechanism_tracking_.note_row(snapshot.tick, body);
                    }
                mechanism_tracking_.mark_dirty();
                mechanism_tracking_.resolve(snapshot.tick, [physics](const char* entity, bmmo_physics_body_state& out) {
                    std::string probe_error;
                    return bmmo::physics::get_body_state(physics, entity, out, probe_error);
                });
                std::string mechanism_entity;
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
                        mechanism_entity = mechanism_tracking_.entity_of(body.owner);
                        if (mechanism_entity.empty()) continue;   // another sector's instance
                        target = mechanism_entity.c_str();
                    }
                    bmmo::physics::set_body_state(physics, target, body.position, body.rotation, body.linear, body.angular,
                                                  body.owner == own_id_ ? true : wake, error);
                }
                resync_pending_ = false;
                ++resyncs_done_;
                logf("resync applied from the full snapshot of tick %u (%zu bodies)", snapshot.tick, snapshot.bodies.size());
                journal_body_correction(4, {}, snapshot.tick, 0.0, nullptr);
                journal_note("resync: rebuilt from the full snapshot of tick " + std::to_string(snapshot.tick)
                             + " (" + std::to_string(snapshot.bodies.size()) + " bodies)");
                return;
            }
            have_snapshot_ = true;
            last_snapshot_tick_ = snapshot.tick;
            ++snapshots_applied_;
            if (args_.rollback) {
                for (const auto& body: snapshot.bodies)
                    if (body.kind == bmmo::session::body_kind::Mechanism) {
                        if (snapshot.full && !body.name.empty()) mechanism_tracking_.note_name(body.owner, body.name);
                        mechanism_tracking_.note_row(snapshot.tick, body);
                    }
                auto entity_of = [&](const bmmo::session::body_state& body) -> std::string {
                    if (body.kind == bmmo::session::body_kind::Ball) {
                        if (body.owner == own_id_) return own_physicalized_ && own_navigation_ ? own_nav_entity_ : std::string();
                        auto it = remotes_.find(body.owner);
                        if (it == remotes_.end() || !it->second.physicalized || !it->second.navigation) return {};
                        return it->second.entity;
                    }
                    // Design 9.25: the resolved local body, "" when this client
                    // has none (another sector) or the identity guard refused
                    // the pick - the mod does exactly the same.
                    return mechanism_tracking_.entity_of(body.owner);
                };
                // Only an exact hit counts, like the mod: the engine feeds a
                // relayed frame instead of the recorded prediction when this
                // succeeds, and a frame from an earlier tick would replay a key
                // edge the tick did not have (findings/A5 section 3.1 item 5).
                auto input_at = [this](const std::string& entity, uint32_t tick, bmmo::session::input_frame& out) {
                    return recorded_input_at(entity, tick, out);
                };
                const bool rolled = rollback_.on_snapshot(rollback_world(), snapshot, current_tick(), entity_of, input_at);
                if (rolled && args_.trace && rollback_diag_++ < 16)
                    logf("after rollback: movable=%s", bmmo::physics::describe_movable_objects(physics).c_str());
                if (rolled && own_navigation_ && previous_cam_valid_)
                    bmmo::physics::navigation_input(physics, own_nav_entity_.c_str(), 0, previous_cam_[0], previous_cam_[1],
                                                    previous_cam_[2], false, error);
                return;
            }
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
                            // Before the set, or the recorded local pose would
                            // be the server's own.
                            journal_body_correction(2, ball_name, snapshot.tick, err, body.position);
                            if (args_.correct && !bmmo::physics::set_body_state(physics, ball_name.c_str(), step.target.position,
                                    step.target.rotation, step.target.linear, step.target.angular, true, error))
                                logf("hard set: %s", error.c_str());
                        } else if (step.action == bmmo::session::correction_step::kind::blend) {
                            consecutive_hard_ = 0;
                            logf("blend correction of own ball for tick %u (error %.4f m, local tick %u)", snapshot.tick, err, current_tick());
                            journal_body_correction(3, ball_name, snapshot.tick, err, body.position);
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
                            journal_body_correction(2, remote.entity, snapshot.tick,
                                                    remote.corrector.stats().last_error, body.position);
                            if (args_.correct) bmmo::physics::set_body_state(physics, remote.entity.c_str(), step.target.position,
                                                                             step.target.rotation, step.target.linear, step.target.angular, wake, error);
                        } else if (step.action == bmmo::session::correction_step::kind::blend) {
                            ++remote.blends;
                            logf("blend correction of remote %s for tick %u (error %.4f m)", remote.entity.c_str(), snapshot.tick,
                                 remote.corrector.stats().last_error);
                            journal_body_correction(3, remote.entity, snapshot.tick,
                                                    remote.corrector.stats().last_error, body.position);
                        }
                        continue;
                    }
                    if (!bmmo::physics::set_body_state(physics, it->second.entity.c_str(), body.position, body.rotation,
                                                       body.linear, body.angular, wake, error))
                        logf("remote %u: %s", body.owner, error.c_str());
                    else ++remote_writes_;
                    continue;
                }
                // The legacy blending path (--no-rollback): the row corrects the
                // locally simulated mechanism through a body_corrector instead.
                if (snapshot.full && !body.name.empty()) mechanism_tracking_.note_name(body.owner, body.name);
                mechanism_tracking_.note_row(snapshot.tick, body);
                const std::string entity = mechanism_tracking_.entity_of(body.owner);
                if (entity.empty()) continue;
                const bool wake = (body.flags & bmmo::session::BODY_FLAG_SIMULATED) != 0;
                bmmo_physics_body_state local{};
                if (!bmmo::physics::get_body_state(physics, entity.c_str(), local, error)) continue;
                bmmo::session::ball_pose pose;
                pose.tick = snapshot.tick;
                for (int k = 0; k < 3; ++k) { pose.position[k] = body.position[k]; pose.linear[k] = body.linear[k]; pose.angular[k] = body.angular[k]; }
                for (int k = 0; k < 4; ++k) pose.rotation[k] = body.rotation[k];
                auto& corrector = mechanism_correctors_[entity];
                const auto step = corrector.compare(pose);
                if (step.action == bmmo::session::correction_step::kind::hard) {
                    ++mechanism_hard_;
                    logf("hard correction of %s for tick %u (error %.4f m)", entity.c_str(), snapshot.tick, corrector.stats().last_error);
                    journal_body_correction(2, entity, snapshot.tick, corrector.stats().last_error, body.position);
                    if (args_.correct) bmmo::physics::set_body_state(physics, entity.c_str(), step.target.position, step.target.rotation,
                                                                     step.target.linear, step.target.angular, wake, error);
                } else if (step.action == bmmo::session::correction_step::kind::blend) {
                    ++mechanism_blends_;
                    logf("blend correction of %s for tick %u (error %.4f m)", entity.c_str(), snapshot.tick, corrector.stats().last_error);
                    journal_body_correction(3, entity, snapshot.tick, corrector.stats().last_error, body.position);
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

        static bool same_frame(const bmmo::session::input_frame& a, const bmmo::session::input_frame& b) {
            if (a.keys != b.keys || a.ball_type != b.ball_type || a.flags != b.flags) return false;
            for (int k = 0; k < 3; ++k)
                if (a.cam_right[k] != b.cam_right[k] || a.cam_up[k] != b.cam_up[k] || a.cam_dir[k] != b.cam_dir[k])
                    return false;
            return true;
        }

        // A relayed frame for a tick this client has already simulated: if it
        // differs from the frame the prediction used, that tick and every tick
        // after it ran on the wrong input.  Remember the earliest such tick and
        // let frame() replay from it once, before the next step - the frames of
        // several remotes arrive together, and one rewind covers them all.
        void note_input_correction(const std::string& entity, uint32_t tick,
                                   const bmmo::session::input_frame& relayed) {
            if (!args_.rollback || entity.empty() || tick == 0) return;
            if (static_cast<int32_t>(tick - current_tick()) > 0) return;   // not simulated yet
            if (!rollback_.knows_tick(tick)) return;   // outside the history: nothing to replay from
            // No record of an input for this body at this tick is NEWS, not a
            // reason to do nothing: we stepped that tick without driving the
            // body at all.  It is the normal state of a mirror that was just
            // rewound into existence - the replay could only feed it the frames
            // that had arrived by then, which for most of the birth window is
            // none - so skipping it here was stopping those balls from ever
            // converging.
            bmmo::session::input_frame predicted{};
            if (rollback_.recorded_input(entity, tick, predicted) && same_frame(predicted, relayed))
                return;   // the prediction was right
            if (!input_rewind_pending_ || static_cast<int32_t>(tick - input_rewind_tick_) < 0) {
                input_rewind_pending_ = true;
                input_rewind_tick_ = tick;
                input_rewind_entity_ = entity;
            }
        }

        // Rewind for no reason at all, on a fixed cadence (--force-rollback).
        // A solo session's world is a pure function of its recorded inputs, so
        // the hashes of a run with these must equal those of a run without; any
        // difference is state the replay failed to restore.
        void maybe_force_rollback() {
            if (!args_.rollback || args_.force_rollback_every == 0) return;
            const uint32_t now = current_tick();
            if (now < args_.force_rollback_depth + 2) return;
            if (now % args_.force_rollback_every != 0) return;
            auto input_at = [this](const std::string& name, uint32_t tick, bmmo::session::input_frame& out) {
                return recorded_input_at(name, tick, out);
            };
            const uint32_t anchor = now - args_.force_rollback_depth;
            std::string before;
            float factor_before = 0.0f, delta_before = 0.0f, factor_after = 0.0f, delta_after = 0.0f;
            bmmo_physics_clock_state clock_before{}, clock_after{};
            std::string clock_error;
            if (args_.trace_restore) {
                before = bmmo::physics::describe_cores_exact(engine_->physics());
                bmmo::physics::get_clock(engine_->physics(), factor_before, delta_before, clock_error);
                bmmo::physics::get_clock_state(engine_->physics(), clock_before, clock_error);
            }
            if (args_.trace_restore && own_physicalized_ && !own_body_entity_.empty() && contact_traces_ < 48) {
                ++contact_traces_;
                logf("contacts of %s before the rewind at %u: %s", own_body_entity_.c_str(), now,
                     bmmo::physics::describe_contacts(engine_->physics(), own_body_entity_.c_str()).c_str());
            }
            if (rollback_.rewind_to(rollback_world(), anchor, now, input_at, "forced")) ++forced_rollbacks_;
            else ++forced_rollbacks_missed_;
            if (args_.trace_restore && own_physicalized_ && !own_body_entity_.empty() && contact_traces_ < 48) {
                ++contact_traces_;
                logf("contacts of %s after the rewind (replayed to %u): %s", own_body_entity_.c_str(), now,
                     bmmo::physics::describe_contacts(engine_->physics(), own_body_entity_.c_str()).c_str());
            }
            if (args_.trace_restore && restore_traces_ < 8) {
                ++restore_traces_;
                const std::string after = bmmo::physics::describe_cores_exact(engine_->physics());
                bmmo::physics::get_clock(engine_->physics(), factor_after, delta_after, clock_error);
                bmmo::physics::get_clock_state(engine_->physics(), clock_after, clock_error);
                // Its own line, with every argument cast: the status line's
                // printf taught what one missing specifier costs.
                logf("restore trace: clock before t=%a tlast=%a tnext=%a base=%a last=%a nmc=%d q=%d qmin=%a",
                     clock_before.current_time, clock_before.time_of_last_psi, clock_before.time_of_next_psi,
                     clock_before.base_time, clock_before.last_time, static_cast<int>(clock_before.next_movement_check),
                     static_cast<int>(clock_before.queued_events), static_cast<double>(clock_before.queue_min));
                logf("restore trace: clock after  t=%a tlast=%a tnext=%a base=%a last=%a nmc=%d q=%d qmin=%a",
                     clock_after.current_time, clock_after.time_of_last_psi, clock_after.time_of_next_psi,
                     clock_after.base_time, clock_after.last_time, static_cast<int>(clock_after.next_movement_check),
                     static_cast<int>(clock_after.queued_events), static_cast<double>(clock_after.queue_min));
                int changed = 0;
                const std::string fields = diff_core_dumps(before, after, changed);
                // CKIpionManager::SetDeltaTime is an exponential moving average
                // over a PERSISTENT m_DeltaTime, and Simulate() calls it every
                // time: so every replayed step moves the smoothed delta that
                // governs the integration, and nothing puts it back.  Printing
                // it exactly is the difference between suspecting that and
                // knowing it.
                logf("restore trace: forced rewind %u -> %u (depth %u) moved %d cores, "
                     "physics delta %a -> %a (%s, factor %a -> %a):%s",
                     now, anchor, args_.force_rollback_depth, changed,
                     static_cast<double>(delta_before), static_cast<double>(delta_after),
                     delta_before == delta_after ? "UNCHANGED" : "MOVED",
                     static_cast<double>(factor_before), static_cast<double>(factor_after),
                     fields.empty() ? " none" : fields.c_str());
            }
        }

        // One replay for every input correction this frame collected.
        void replay_corrected_inputs() {
            if (!input_rewind_pending_) return;
            const uint32_t anchor = input_rewind_tick_;
            const std::string entity = input_rewind_entity_;
            input_rewind_pending_ = false;
            const uint32_t now = current_tick();
            if (static_cast<int32_t>(anchor - now) > 0) return;
            auto input_at = [this](const std::string& name, uint32_t tick, bmmo::session::input_frame& out) {
                return recorded_input_at(name, tick, out);
            };
            if (rollback_.rewind_to(rollback_world(), anchor - 1, now, input_at,
                                    entity + " input corrected at tick " + std::to_string(anchor))) {
                ++input_rewinds_;
                return;
            }
            ++input_rewinds_missed_;
            if (input_rewinds_missed_ < 8)
                logf("input correction for %s at tick %u is outside the rollback history at local tick %u: "
                     "a snapshot has to re-anchor it", entity.c_str(), anchor, now);
        }

        // Every movable body in this client's world except the balls, which
        // the tracked set carries separately as own/remote entities.
        std::vector<std::string> movable_body_names() {
            std::vector<bmmo_physics_body_state> bodies(256);
            int count = bmmo::physics::list_bodies(engine_->physics(), bodies.data(), static_cast<int>(bodies.size()));
            if (count > static_cast<int>(bodies.size())) {
                bodies.resize(static_cast<size_t>(count));
                count = bmmo::physics::list_bodies(engine_->physics(), bodies.data(), static_cast<int>(bodies.size()));
            }
            std::vector<std::string> names;
            for (int i = 0; i < count && i < static_cast<int>(bodies.size()); ++i) {
                const auto& body = bodies[static_cast<size_t>(i)];
                if (!body.movable || !body.name[0]) continue;
                if (std::strncmp(body.name, "Ball_", 5) == 0) continue;   // own and remote balls are tracked by entity
                names.emplace_back(body.name);
            }
            return names;
        }

        // How many bodies have an impulse queued for the next PSI at the moment
        // the rollback captures state (--trace, every 200 ticks)?
        //
        // Written to settle whether core->speed_change could be the replay'''s
        // remaining infidelity: it is hashed (world_hash.hpp feeds it), it is
        // NOT in bmmo_physics_body_state, and write_body_state zeroes it on
        // every write (design 8.4), so if it were non-zero at capture time
        // every restore would drop real motion.
        //
        // ANSWER: it is zero for essentially every body - 0 of 10 to 14 at
        // almost every sample over a 1700-tick solo session, with only
        // P_Modul_25_Bridge occasionally carrying a constant |dv| of 1.2121.
        // So there is nothing meaningful to carry and growing the bridge struct
        // would buy nothing.  Kept because it answers a recurring class of
        // question cheaply: is some IVP per-step state non-zero when we snap?
        // Which named fields of which cores differ between two
        // describe_cores_exact dumps.  Both are "name field=... field=..." per
        // line, sorted by name, so this is a token-wise compare keyed on the
        // name - and it reports the fields rather than a hash, which is the
        // whole point: a hash says "something moved", this says what.
        static std::string diff_core_dumps(const std::string& before, const std::string& after,
                                           int& bodies_changed) {
            const auto split = [](const std::string& text) {
                std::map<std::string, std::vector<std::string>> out;
                size_t pos = 0;
                while (pos < text.size()) {
                    const size_t end = text.find(0x0a, pos);
                    const std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                    pos = end == std::string::npos ? text.size() : end + 1;
                    if (line.empty()) continue;
                    std::vector<std::string> tokens;
                    size_t t = 0;
                    while (t < line.size()) {
                        const size_t space = line.find(' ', t);
                        tokens.push_back(line.substr(t, space == std::string::npos ? std::string::npos : space - t));
                        t = space == std::string::npos ? line.size() : space + 1;
                    }
                    if (!tokens.empty()) out[tokens[0]] = std::move(tokens);
                }
                return out;
            };
            const auto a = split(before), b = split(after);
            std::string out;
            bodies_changed = 0;
            for (const auto& [name, tokens]: a) {
                const auto other = b.find(name);
                if (other == b.end()) { out += "\n    " + name + " GONE"; ++bodies_changed; continue; }
                std::string fields;
                for (size_t i = 1; i < tokens.size() && i < other->second.size(); ++i) {
                    if (tokens[i] == other->second[i]) continue;
                    fields += " " + tokens[i] + "->" + other->second[i];
                }
                if (fields.empty()) continue;
                out += "\n    " + name + fields;
                ++bodies_changed;
            }
            for (const auto& [name, tokens]: b)
                if (a.find(name) == a.end()) { out += "\n    " + name + " NEW"; ++bodies_changed; }
            return out;
        }

        void report_queued_impulses(uint32_t tick) {
            const std::string dump = bmmo::physics::describe_cores_exact(engine_->physics());
            int bodies = 0, queued = 0;
            std::string worst_name;
            double worst = 0.0;
            size_t pos = 0;
            while ((pos = dump.find("dv=", pos)) != std::string::npos) {
                ++bodies;
                // the name is the start of this body's entry: back to the
                // previous separator
                const size_t entry = dump.rfind(0x0a, pos);
                const size_t start = entry == std::string::npos ? 0 : entry + 1;
                std::string name = dump.substr(start, dump.find(' ', start) - start);
                double v[3] = {0, 0, 0};
                if (std::sscanf(dump.c_str() + pos + 3, "%la,%la,%la", &v[0], &v[1], &v[2]) == 3) {
                    const double mag = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                    if (mag > 0.0) {
                        ++queued;
                        if (mag > worst) { worst = mag; worst_name = name; }
                    }
                }
                pos += 3;
            }
            logf("queued impulses at tick %u: %d of %d bodies, worst %s |dv|=%.9f",
                 tick, queued, bodies, worst_name.empty() ? "-" : worst_name.c_str(), worst);
        }

        // The input a navigated ball had at `tick`, for a re-simulation.  Only
        // an exact hit counts, like the mod: the engine feeds a relayed frame
        // instead of the recorded prediction when this succeeds, and a frame
        // from an earlier tick would replay a key edge the tick did not have.
        bool recorded_input_at(const std::string& entity, uint32_t tick, bmmo::session::input_frame& out) const {
            const std::map<uint32_t, bmmo::session::input_frame>* inputs = nullptr;
            if (entity == own_nav_entity_) inputs = &own_inputs_;
            else
                for (const auto& [id, remote]: remotes_)
                    if (remote.entity == entity) inputs = &remote.inputs;
            if (!inputs) return false;
            auto it = inputs->find(tick);
            if (it == inputs->end()) return false;
            out = it->second;
            return true;
        }

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
        void drain_due_events(uint32_t tick) {
            if (pending_events_.empty()) return;
            std::vector<bmmo::session_event_msg> due;
            auto keep = pending_events_.begin();
            for (auto it = pending_events_.begin(); it != pending_events_.end(); ++it) {
                if (static_cast<int32_t>(it->tick - tick) <= 0) due.push_back(std::move(*it));
                else *keep++ = std::move(*it);
            }
            pending_events_.erase(keep, pending_events_.end());
            if (due.empty()) return;
            const auto join_rank = [this](uint32_t player) {
                for (const auto& p: players_) if (p.id == player) return static_cast<uint32_t>(p.join_order);
                return 256u;
            };
            std::stable_sort(due.begin(), due.end(),
                             [&join_rank](const bmmo::session_event_msg& a, const bmmo::session_event_msg& b) {
                                 return join_rank(a.player) < join_rank(b.player);
                             });
            for (const auto& event: due) apply_event(event);
        }

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
        // and a body that went away is simply absent from the replay, which is
        // also what its owner had.  (A destroyed body cannot be restored by the
        // engine, and does not need to be - the replay runs without it.)
        //
        // A stamp older than the rollback history has nothing to rewind to, and
        // then the snapshot path takes over - which is what it is there for.
        void rewind_for_lifecycle(const bmmo::session_event_msg& event) {
            if (!args_.rollback || event.tick == 0) return;
            const uint32_t now = current_tick();
            if (static_cast<int32_t>(event.tick - now) > 0) return;   // not late: nothing to replay
            auto input_at = [this](const std::string& entity, uint32_t tick, bmmo::session::input_frame& out) {
                return recorded_input_at(entity, tick, out);
            };
            const std::string reason = "ball of " + std::to_string(event.player) + " born at tick "
                                     + std::to_string(event.tick);
            if (rollback_.rewind_to(rollback_world(), event.tick - 1, now, input_at, reason)) {
                ++life_rewinds_;
                return;
            }
            ++life_rewinds_missed_;
            logf("lifecycle event of %u stamped tick %u is outside the rollback history at local tick %u: "
                 "the body set disagrees until a snapshot re-anchors it", event.player, event.tick, now);
        }

        void apply_event(const bmmo::session_event_msg& event) {
            if (event.player == 0 || event.player == own_id_) return;
            journal_lifecycle_event(event.player, event);
            std::string error;
            switch (event.type) {
            case bmmo::session::event_type::Physicalize: {
                CK3dEntity* entity = remote_entity(event.player, event.ball_type, error);
                if (!entity) { logf("remote physicalize: %s", error.c_str()); return; }
                auto& remote = remotes_[event.player];
                if (remote.navigation) bmmo::physics::navigation_destroy(engine_->physics(), remote.entity.c_str(), error);
                remote.navigation = false;
                if (remote.physicalized && remote.entity != entity->GetName()) {
                    ++generations_[remote.entity];   // a trafo: the old mirror's lifetime ends here
                    bmmo::physics::unphysicalize(engine_->physics(), remote.entity.c_str(), error);
                }
                entity->SetWorldMatrix(matrix_from_pose(event.position, event.rotation));
                const auto recipe = to_bridge_recipe(event.recipe);
                int join_order = 63;
                for (const auto& p: players_) if (p.id == event.player) join_order = p.join_order;
                const std::string group = "P#" + std::to_string(join_order);
                if (!bmmo::physics::physicalize(engine_->physics(), entity->GetName(), recipe, group.c_str(), error)) {
                    logf("remote physicalize %s: %s", entity->GetName(), error.c_str());
                    return;
                }
                // The mirror's body is new: a new lifetime, so no record of the
                // previous one can restore it (design 9.25).
                ++generations_[entity->GetName()];
                remote.entity = entity->GetName();
                remote.ball_type = event.ball_type;
                remote.physicalized = true;
                remote.corrector.clear();
                if ((event.flags & bmmo::session::PHYSICALIZE_FLAG_SPAWN) && spawn_impulse_ > 0.0f) {
                    const uint32_t idx = bmmo::session::spawn_direction_index(seed_, static_cast<uint8_t>(join_order), event.tick);
                    std::string impulse_error;
                    if (!bmmo::physics::push_impulse(engine_->physics(), entity->GetName(), bmmo::session::kSpawnDirectionTable[idx],
                                spawn_impulse_, 0, impulse_error))
                        logf("remote spawn impulse for %u: %s", event.player, impulse_error.c_str());
                    else
                        logf("remote spawn impulse index=%u for player %u at tick %u", idx, event.player, event.tick);
                }
                attach_remote_navigation(event.player, remote);
                logf("remote player %u physicalized (%s) at tick %u%s", event.player, remote.entity.c_str(), event.tick,
                     remote.navigation ? ", predicted" : ", mirrored");
                // LAST, once the body is fully set up.  The replay only drives
                // and records a remote that is physicalized AND navigated (see
                // the tracked set in frame()), and the spawn impulse above is
                // part of the state the owner had at the stamp tick - rewinding
                // before either left the mirror outside the rollback entirely,
                // undriven and unkicked, and the balls ran away instead of
                // converging (2.2 m at birth to 11 m twenty ticks later).
                rewind_for_lifecycle(event);
                break;
            }
            case bmmo::session::event_type::Sector:
                // Another member entered a sector: an input to our world too.
                // Stamped into the union, which starts the sector before the
                // step of the stamped tick - this very frame when the relay was
                // on time (drain_due_events runs before the union's tick), the
                // next one when it was late, which the union counts.
                sectors_.stamp(event.player, event.sector, event.tick);
                logf("remote player %u entered sector %d, stamped for tick %u%s", event.player, event.sector, event.tick,
                     static_cast<int32_t>(event.tick - current_tick()) <= 0 ? " (LATE: the server started it already)" : "");
                break;
            case bmmo::session::event_type::Unphysicalize: {
                auto it = remotes_.find(event.player);
                if (it == remotes_.end()) return;
                if (it->second.navigation) bmmo::physics::navigation_destroy(engine_->physics(), it->second.entity.c_str(), error);
                it->second.navigation = false;
                if (it->second.physicalized) {
                    ++generations_[it->second.entity];   // the body goes: a new lifetime
                    bmmo::physics::unphysicalize(engine_->physics(), it->second.entity.c_str(), error);
                }
                it->second.physicalized = false;
                logf("remote player %u unphysicalized at tick %u", event.player, event.tick);
                rewind_for_lifecycle(event);
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

        // Design 9.6: own ball driven by the replica in polling mode, retail
        // force leaves zeroed (see the mod's physics_session_attach_own_navigation).
        void attach_own_navigation(const std::string& ball_name, int ball_type) {
            if (!navigation_known_ || !navigation_.valid() || ball_name.empty()) return;
            std::string error;
            const float force = ball_type >= 0 && ball_type < static_cast<int>(ball_rows_.size()) ? ball_rows_[ball_type].force : 0.0f;
            if (own_navigation_ && own_nav_entity_ != ball_name) {
                if (bmmo::physics::navigation_set_ball(engine_->physics(), own_nav_entity_.c_str(), ball_name.c_str(), force, error)) {
                    own_nav_entity_ = ball_name;
                    return;
                }
                bmmo::physics::navigation_destroy(engine_->physics(), own_nav_entity_.c_str(), error);
                own_navigation_ = false;
            }
            float directions[8][3] = {};
            int count = 0;
            for (const auto& leaf: navigation_.leaves) {
                if (leaf.index < 0 || leaf.index >= 8) continue;
                directions[leaf.index][0] = leaf.direction.x;
                directions[leaf.index][1] = leaf.direction.y;
                directions[leaf.index][2] = leaf.direction.z;
                own_key_codes_[leaf.index] = leaf.key;
                own_key_blocks_[leaf.index] = leaf.key_block;
                count = std::max(count, leaf.index + 1);
            }
            own_key_count_ = count;
            if (!bmmo::physics::navigation_create(engine_->physics(), ball_name.c_str(), "CamRef_BMMO_self",
                                                  navigation_.ball_navigation, directions, count, force, error)
                    || !bmmo::physics::navigation_poll(engine_->physics(), ball_name.c_str(), true, own_key_codes_,
                                                       own_key_blocks_, count, error)) {
                logf("own navigation replica: %s", error.c_str());
                return;
            }
            own_navigation_ = true;
            own_nav_entity_ = ball_name;
            logf("own ball %s driven by the navigation replica (force %.3f)", ball_name.c_str(), force);
        }

        void zero_retail_forces() {
            CKContext* context = engine_->context();
            const float zero = 0.0f;
            for (const auto& leaf: navigation_.leaves) {
                auto* block = CKBehavior::Cast(context->GetObject(leaf.force_block));
                if (!block) continue;
                if (CKParameterIn* in = block->GetInputParameter(4))
                    if (CKParameter* source = in->GetRealSource()) source->SetValue(&zero, sizeof(zero));
            }
        }

        bmmo::session::rollback_world rollback_world() {
            bmmo::session::rollback_world world;
            CKIpionManager* physics = engine_->physics();
            world.get_body = [physics](const std::string& entity, bmmo_physics_body_state& out) {
                std::string error;
                return bmmo::physics::get_body_state(physics, entity.c_str(), out, error);
            };
            // Design 9.25: the engine decides the sleep handling and the
            // contact recheck per write (see the mod's adapter for why).
            world.set_body = [physics](const std::string& entity, const bmmo_physics_body_state& state,
                                       bmmo::session::wake_mode mode, bool recheck) {
                std::string error;
                const auto bridge_mode = mode == bmmo::session::wake_mode::wake   ? bmmo::physics::wake_mode::wake
                                       : mode == bmmo::session::wake_mode::freeze ? bmmo::physics::wake_mode::freeze
                                                                                  : bmmo::physics::wake_mode::keep;
                return bmmo::physics::set_body_state(physics, entity.c_str(), state.position, state.rotation, state.linear,
                                                     state.angular, bridge_mode, recheck, error);
            };
            // Not installed at all when neither half is wanted: a caller that
            // restores nothing should not pay to capture it either, and it
            // makes --no-restore-impulses --no-restore-quats a true pre-v11
            // control rather than one that still runs the new capture.
            // The extra state is always captured; the engine's switches
            // decide what of it a restore writes back (so every half is an
            // A/B in one binary).
            world.get_body_extra = [physics](const std::string& entity, bmmo_physics_core_extra& out) {
                std::string error;
                return bmmo::physics::get_core_extra(physics, entity.c_str(), out, error);
            };
            world.set_body_extra = [physics](const std::string& entity, const bmmo_physics_core_extra& state) {
                std::string error;
                return bmmo::physics::set_core_extra(physics, entity.c_str(), state, error);
            };
            // The clock is always READ (a restore shifts the calm references
            // it puts back by the live clock's offset); it is rewound only on
            // --restore-clock.
            world.get_clock = [physics](bmmo_physics_clock_state& out) {
                std::string error;
                return bmmo::physics::get_clock_state(physics, out, error);
            };
            if (args_.restore_clock) {
                world.set_clock = [physics](const bmmo_physics_clock_state& state) {
                    std::string error;
                    return bmmo::physics::set_clock_state(physics, state, error);
                };
            }
            if (args_.script_replay && !scripts_.empty()) {
                CKContext* context = engine_->context();
                world.capture_scripts = [this, context](std::vector<uint8_t>& out) {
                    return bmmo::game::script_replay::capture(context, scripts_, script_hooks_, out);
                };
                world.restore_scripts = [this, context](const std::vector<uint8_t>& bytes, uint32_t anchor) {
                    // A script the game reset after the anchor keeps its live
                    // state: the anchor's record is what the reset tore down.
                    std::vector<CK_ID> kept_live;
                    for (const auto& [id, reset_tick]: script_reset_ticks_)
                        if (reset_tick > anchor) kept_live.push_back(id);
                    script_reset_skips_ += kept_live.size();
                    std::string error;
                    const bool ok = bmmo::game::script_replay::restore(context, scripts_, script_hooks_, bytes, &error,
                                                                       &scripts_replayed_, &kept_live);
                    if (!ok && script_restore_logs_++ < 8) logf("script replay: restore failed: %s", error.c_str());
                    return ok;
                };
                world.diff_scripts = [this, context](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, bool& logical) {
                    const auto d = bmmo::game::script_replay::diff(context, scripts_, a, b);
                    logical = d.logical;
                    return d.where + " (" + std::to_string(d.logical_count) + " logical, " + std::to_string(d.float_count) + " float)"
                         + (d.logical && args_.trace ? " :: " + d.detail : std::string());
                };
                // The mechanism scripts' frame, where the live frame runs it:
                // before the physics step.  The bridge event log is muted for
                // it - a replayed Physics WakeUp is not a live event for the
                // server.
                world.pre_step = [this, context](uint32_t tick) {
                    if (args_.trace_restore && own_physicalized_ && !own_body_entity_.empty() && contact_traces_ < 48) {
                        ++contact_traces_;
                        logf("contacts of %s at replayed tick %u, before its step: %s", own_body_entity_.c_str(), tick,
                             bmmo::physics::describe_contacts(engine_->physics(), own_body_entity_.c_str()).c_str());
                    }
                    bmmo::physics::set_event_log_muted(true);
                    bmmo::game::script_replay::execute_frame(context, scripts_, 1000.0f / 66.0f, &scripts_replayed_);
                    bmmo::physics::set_event_log_muted(false);
                };
            }
            world.get_nav = [physics](const std::string& entity, bmmo_physics_nav_state& out) {
                std::string error;
                return bmmo::physics::navigation_get_state(physics, entity.c_str(), out, error);
            };
            world.set_nav = [physics](const std::string& entity, const bmmo_physics_nav_state& state) {
                std::string error;
                return bmmo::physics::navigation_set_state(physics, entity.c_str(), state, error);
            };
            world.get_contacts = [physics](const std::string& entity, std::vector<bmmo_physics_contact>& out) {
                std::string error;
                bmmo_physics_contact buffer[16];
                const int n = bmmo::physics::get_contacts(physics, entity.c_str(), buffer, 16, error);
                if (n < 0) return -1;
                out.assign(buffer, buffer + std::min(n, 16));
                return n;
            };
            world.set_contacts = [this, physics](const std::string& entity, const std::vector<bmmo_physics_contact>& in, uint32_t flags) {
                std::string error;
                const int made = bmmo::physics::set_contacts(physics, entity.c_str(), in.data(), static_cast<int>(in.size()), flags, error);
                if (!error.empty() && contact_restore_logs_++ < 8) logf("contact restore of %s: %s", entity.c_str(), error.c_str());
                if (made > 0 && args_.trace && contact_made_logs_++ < 40) {
                    std::string partners;
                    for (const auto& c: in) partners += std::string(partners.empty() ? "" : ",") + c.partner;
                    logf("contact restore of %s at tick %u: %d re-created of %zu recorded (%s)", entity.c_str(), current_tick(), made,
                         in.size(), partners.c_str());
                }
                return made;
            };
            world.nav_input = [physics](const std::string& entity, const bmmo::session::input_frame& frame) {
                std::string error;
                const bool active = (frame.flags & bmmo::session::INPUT_FLAG_NAV_ACTIVE) != 0;
                return bmmo::physics::navigation_input(physics, entity.c_str(), frame.keys, frame.cam_right, frame.cam_up,
                                                       frame.cam_dir, active, error);
            };
            world.nav_poll = [this, physics](const std::string& entity, bool enable) {
                std::string error;
                return bmmo::physics::navigation_poll(physics, entity.c_str(), enable, own_key_codes_, own_key_blocks_,
                                                      own_key_count_, error);
            };
            world.step = [this, physics]() {
                std::string error;
                if (args_.replay_scripts) {
                    // The whole frame, so the scripts that drive the mechanisms
                    // run for the replayed tick too.
                    const bool ok = engine_->tick(error);
                    if (!ok) logf("replay frame: %s", error.c_str());
                    return ok;
                }
                const bool diag = args_.trace && step_diag_ < 24;
                if (diag)
                    for (const auto& [id, remote]: remotes_)
                        if (remote.navigation)
                            logf("core before step: %s", bmmo::physics::describe_core(physics, remote.entity.c_str()).c_str());
                const bool ok = bmmo::physics::step_physics(physics, 1000.0f / 66.0f, error);
                if (diag) {
                    ++step_diag_;
                    for (const auto& [id, remote]: remotes_)
                        if (remote.navigation)
                            logf("core after step:  %s", bmmo::physics::describe_core(physics, remote.entity.c_str()).c_str());
                }
                return ok;
            };
            world.simulating = [physics]() {
                float factor = 0.0f, delta = 0.0f;
                std::string error;
                return !bmmo::physics::get_clock(physics, factor, delta, error) || (factor > 0.0f && delta > 0.0f);
            };
            world.log = [](const std::string& text) { logf("%s", text.c_str()); };
            // Every decision the engine takes about a snapshot, straight into
            // the box: no log parsing needed to know what this client did.
            world.on_correction = [this](const bmmo::session::rollback_correction& c) {
                journal_rollback_correction(c);
            };
            return world;
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
            // Why this client gave up on its own world belongs in the box: the
            // reason ("tick schedule rebased") leaves no other trace in the
            // file, and the assignment that answers it looks unprompted.
            journal_note(std::string("resync: requested (") + reason + ")");
        }

        // ---------------------------------------------------- session journal
        // The black box (design 9.15).  Everything this client fed its own
        // world, everything the server sent it and every correction it made,
        // in the shared format so the SimTool and journal_trace.py can line it
        // up with the server's own journal of the same room.  Single threaded
        // like the rest of this client: the network is polled between frames,
        // so there is no lock anywhere near the writer.
        bool journalling() const { return journal_.is_open() && !journal_.capped(); }

        uint32_t journal_ms() const {
            return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                clock_type::now() - journal_start_).count());
        }

        // Opened at the anchor, where the hash the server has to agree with is
        // known and the tick base is not: first_tick is 0 until the assignment
        // arrives, which lands in a NOTE of its own.
        void open_journal(const bmmo::physics::world_hash& hash) {
            if (args_.journal.empty()) return;
            close_journal("a new session started");
            const std::filesystem::path path = args_.journal;
            std::error_code ec;
            if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
            bmmo::session::journal_header header;
            header.kind = bmmo::session::journal_kind::client;
            header.session = session_;
            header.level = level_;
            header.seed = seed_;
            header.spawn_impulse = spawn_impulse_;
            header.input_delay = input_delay_;
            header.checkpoint_ticks = args_.checkpoint_ticks ? args_.checkpoint_ticks : kJournalCheckpointTicks;
            header.first_tick = tick_base_;
            header.anchor_hash = hash.pose;
            header.anchor_surfaces = hash.surfaces;
            header.build_id = bmmo::physics::build_id();
            header.utc_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            header.own_player = own_id_;
            header.own_join_order = own_join_order_ < 0 ? 255 : static_cast<uint8_t>(own_join_order_);
            journal_start_ = clock_type::now();
            std::string error;
            if (!journal_.open(path, header, kJournalMaxBytes, error)) {
                logf("journal: %s", error.c_str());
                return;
            }
            // The headless client knows ids, not nicknames: only its own name
            // comes from the command line.
            std::string note = "start: session " + std::to_string(session_) + " level " + std::to_string(level_)
                             + " as " + args_.name + "(" + std::to_string(own_id_) + ", join "
                             + std::to_string(own_join_order_) + "):";
            journal_announced_.clear();
            for (const auto& p: players_) {
                const std::string name = p.id == own_id_ ? args_.name : std::to_string(p.id);
                journal_.player(header.first_tick, p.id, p.join_order, true, name);
                journal_announced_.insert(p.id);
                note += " " + name + "(" + std::to_string(p.id) + ", join " + std::to_string(p.join_order) + ")";
            }
            journal_.note(header.first_tick, note);
            journal_last_checkpoint_ = 0;
            journal_have_checkpoint_ = false;
            journal_checkpoint_pending_ = false;
            logf("journal: %s", path.string().c_str());
        }

        void close_journal(const std::string& reason) {
            if (!journal_.is_open()) return;
            journal_.note(current_tick(), "end: " + reason);
            journal_.close();
        }

        void journal_note(const std::string& text) {
            if (journalling()) journal_.note(current_tick(), text);
        }

        // The roster only ever comes from SessionStart, which the server sends
        // to the joining client alone: a player who joins later is never named
        // to us, yet its relayed frames and its events are journalled all the
        // same.  Without a PLAYER record a replay drops every one of them and
        // that player's ball is silently missing, so the first record that
        // names an unknown id announces it here.  The join order decides the
        // spawn impulse direction, so a guessed one is worth a warning rather
        // than a silent wrong answer.
        void journal_announce_player(uint32_t player, uint32_t tick) {
            if (player == 0 || !journal_announced_.insert(player).second) return;
            int join_order = -1;
            for (const auto& p: players_) if (p.id == player) join_order = p.join_order;
            journal_.player(tick, player, join_order < 0 ? 255 : static_cast<uint8_t>(join_order), true,
                            std::to_string(player));
            if (join_order < 0)
                journal_.note(tick, "late join: p" + std::to_string(player)
                              + " has records but was never announced to this client; join order unknown");
        }

        // The other end of journal_announce_player: PlayerLeft is the only
        // unambiguous "that member is gone" signal a remaining client gets (the
        // leaver's relayed Unphysicalize is the same message a living player
        // sends when its ball dies), and the server drops it from the session
        // world at that moment, so a replay of this file has to drop it too.
        // Forgetting the id lets a rejoin announce itself again.
        void journal_leave_player(uint32_t player) {
            if (!journalling() || journal_announced_.erase(player) == 0) return;
            journal_.player(current_tick(), player, 0, false, {});
        }

        // A lifecycle event as the wire carries it; `player` is who it belongs
        // to (the server fills that in on the way out, so an own event has to
        // name itself here).
        void journal_lifecycle_event(uint32_t player, const bmmo::session_event_msg& e) {
            if (!journalling()) return;
            journal_announce_player(player, e.tick);
            bmmo::session::journal_event out;
            out.tick = e.tick;
            // The tick the server applied it at is the server's own business
            // and never reaches a client; the stamp is all this side knows, so
            // both fields carry it and a replay of a client journal is the best
            // effort the SimTool already says it is.
            out.event_tick = e.tick;
            out.id = player;
            out.type = e.type;
            out.ball_type = e.ball_type;
            out.flags = e.flags;
            for (int k = 0; k < 3; ++k) out.position[k] = e.position[k];
            for (int k = 0; k < 9; ++k) out.rotation[k] = e.rotation[k];
            out.sector = e.sector;
            out.name = e.name;
            out.recipe = to_bridge_recipe(e.recipe);
            journal_.event(out);
        }

        // The rollback engine's own verdict about a snapshot (kinds 0, 1, 5,
        // 6, 7): no log parsing needed to know what the client did.
        void journal_rollback_correction(const bmmo::session::rollback_correction& c) {
            if (!journalling()) return;
            bmmo::session::journal_correction out;
            out.tick = c.tick;
            out.local_tick = c.local_tick;
            out.kind = c.kind;
            out.entity = c.entity;
            out.error_m = static_cast<float>(c.error_m);
            out.velocity_error = static_cast<float>(c.velocity_error);
            for (int k = 0; k < 3; ++k) {
                out.local_position[k] = c.local_position[k];
                out.server_position[k] = c.server_position[k];
            }
            journal_.correction(out);
            // A rollback, a too-far set and a freeze all moved bodies: the
            // next checkpoint says where they ended up.
            if (c.kind == 1 || c.kind == 5 || c.kind == 6) journal_checkpoint_pending_ = true;
        }

        // The blending corrector's decisions (hard = 2, blend = 3) and the
        // full-snapshot resync (4), which the rollback engine never sees.
        void journal_body_correction(uint8_t kind, const std::string& entity, uint32_t tick, double error,
                                     const double server_position[3]) {
            if (!journalling()) return;
            bmmo::session::journal_correction c;
            c.tick = tick;
            c.local_tick = current_tick();
            c.kind = kind;
            c.entity = entity;
            c.error_m = static_cast<float>(error);
            bmmo_physics_body_state local{};
            std::string body_error;
            if (!entity.empty() && bmmo::physics::get_body_state(engine_->physics(), entity.c_str(), local, body_error))
                for (int k = 0; k < 3; ++k) c.local_position[k] = local.position[k];
            if (server_position)
                for (int k = 0; k < 3; ++k) c.server_position[k] = server_position[k];
            journal_.correction(c);
            if (kind == 2 || kind == 4) journal_checkpoint_pending_ = true;
        }

        bool journal_checkpoint_due(uint32_t tick) const {
            const uint32_t cadence = args_.checkpoint_ticks ? args_.checkpoint_ticks : kJournalCheckpointTicks;
            // `tick % cadence`, the same rule the server's journal uses
            // (session_runner::step), so the two sides checkpoint the SAME
            // ticks and can be compared body by body.  A "every N since the
            // last one" rule drifts onto the opposite parity and then never
            // coincides with the server's at all - which is how a first attempt
            // at this comparison got 1055 client and 1320 server checkpoints
            // with zero ticks in common.
            if (cadence != 0 && tick % cadence == 0) return true;
            if (!journal_have_checkpoint_ || tick < journal_last_checkpoint_) return true;
            const uint32_t since = tick - journal_last_checkpoint_;
            return journal_checkpoint_pending_ && since >= std::min(cadence, kJournalCheckpointGap);
        }

        // This client's own world, from the bridge's body list: the balls
        // (every entity named Ball_*) and the mechanisms it is running.
        //
        // Balls carry the OWNER, because the name cannot be an identity across
        // sides.  The server records a ball by owner and leaves the name empty;
        // this client names its own ball Ball_<type> whatever its id, and names
        // a mirror Ball_<type>_BMMO_<id> - except CopyObject is asked for
        // CK_DEPENDENCIES_COPY_OBJECT_UNIQUENAME, so Virtools increments the
        // trailing digits whenever that name is already taken by an earlier
        // lifetime, and the id in the name drifts away from the real one (it
        // was observed one and then two higher).  The owner is known here for
        // every ball, so record it and let the comparison use it.
        void journal_local_checkpoint(uint32_t tick) {
            CK3dEntity* own = current_ball();
            const std::string own_name = own && own->GetName() ? own->GetName() : std::string();
            std::vector<bmmo_physics_body_state> bodies(256);
            int count = bmmo::physics::list_bodies(engine_->physics(), bodies.data(), static_cast<int>(bodies.size()));
            if (count > static_cast<int>(bodies.size())) {
                bodies.resize(static_cast<size_t>(count));
                count = bmmo::physics::list_bodies(engine_->physics(), bodies.data(), static_cast<int>(bodies.size()));
            }
            bodies.resize(static_cast<size_t>(std::max(count, 0)));
            std::vector<bmmo::session::body_state> out;
            out.reserve(bodies.size());
            for (const auto& body: bodies) {
                if (!body.movable) continue;
                bmmo::session::body_state state;
                state.kind = std::strncmp(body.name, "Ball_", 5) == 0 ? bmmo::session::body_kind::Ball
                                                                     : bmmo::session::body_kind::Mechanism;
                state.owner = 0;
                if (state.kind == bmmo::session::body_kind::Ball) {
                    if (!own_name.empty() && own_name == body.name) state.owner = own_id_;
                    else
                        for (const auto& [id, remote]: remotes_)
                            if (remote.physicalized && remote.entity == body.name) { state.owner = id; break; }
                }
                state.name = body.name;
                for (int k = 0; k < 3; ++k) {
                    state.position[k] = body.position[k];
                    state.linear[k] = body.linear[k];
                    state.angular[k] = body.angular[k];
                }
                for (int k = 0; k < 4; ++k) state.rotation[k] = body.rotation[k];
                state.flags = static_cast<uint8_t>((body.simulated ? bmmo::session::BODY_FLAG_SIMULATED : 0u)
                    | (body.collision_enabled ? bmmo::session::BODY_FLAG_COLLISION_ENABLED : 0u));
                out.push_back(std::move(state));
            }
            journal_.checkpoint(tick, bmmo::session::JOURNAL_CHECKPOINT_LOCAL, out);
            journal_last_checkpoint_ = tick;
            journal_have_checkpoint_ = true;
            journal_checkpoint_pending_ = false;
        }

        // SessionEnd: drop the mirrors and go idle; a later SessionStart of the
        // same room reloads the level (host restart) through begin_session().
        void end_session(const std::string& reason = "session ended") {
            close_journal(reason);
            std::string error;
            if (body_guard_) {
                bmmo::physics::set_body_guard(engine_->physics(), false, nullptr, error);
                body_guard_ = false;
                body_guard_entity_.clear();
            }
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
            const auto& rs = rollback_.stats();
            uint64_t rc = 0, ri = 0, rb = 0, rh = 0;
            for (const auto& [id, remote]: remotes_) {
                const auto& rs = remote.corrector.stats();
                rc += rs.compared; ri += rs.ignored; rb += rs.blended; rh += rs.hard;
            }
            double mech_max = 0.0;
            for (const auto& [name, c]: mechanism_correctors_) mech_max = std::max(mech_max, c.stats().max_error);
            logf("status: phase=%d session=%u tick=%u assigned=%d held=%d frames=%lld inputs=%llu events=%llu/%llu snapshots=%llu/%llu/%llu "
                 "life=%llu/%llu inrew=%llu/%llu forced=%llu/%llu own_phys=%d remotes=%zu remote_inputs=%llu remote_writes=%llu remote_corr=%llu/%llu/%llu/%llu mechanisms=%zu/%zu mech_blend=%llu mech_hard=%llu mech_max_err=%.4f amend_failures=%llu rebases=%d resyncs=%llu/%llu "
                 "rollback: snaps=%llu ok=%llu mism=%llu rb=%llu resim=%llu unmatched=%llu far=%llu frozen=%llu imp=%llu ct=%llu/%llu scr=%llu/%llu/%llu/%llu/%llu max_err=%.4f last=%s "
                 "corrections: compared=%llu ignored=%llu blended=%llu hard=%llu unmatched=%llu last_err=%.4f max_err=%.4f "
                 "input: reports=%llu starved=%llu (%.0f%%) worst=%u lead=%.1f",
                 static_cast<int>(phase_), session_, current_tick(), assigned_ ? 1 : 0, held_ ? 1 : 0,
                 static_cast<long long>(frames_since_anchor_),
                 static_cast<unsigned long long>(inputs_sent_), static_cast<unsigned long long>(events_sent_),
                 static_cast<unsigned long long>(events_received_), static_cast<unsigned long long>(snapshots_received_),
                 static_cast<unsigned long long>(snapshots_applied_), static_cast<unsigned long long>(snapshots_stale_),
                 static_cast<unsigned long long>(life_rewinds_), static_cast<unsigned long long>(life_rewinds_missed_),
                 static_cast<unsigned long long>(input_rewinds_), static_cast<unsigned long long>(input_rewinds_missed_),
                 static_cast<unsigned long long>(forced_rollbacks_), static_cast<unsigned long long>(forced_rollbacks_missed_),
                 own_physicalized_ ? 1 : 0, remotes_.size(), static_cast<unsigned long long>(remote_inputs_received_),
                 static_cast<unsigned long long>(remote_writes_), static_cast<unsigned long long>(rc),
                 static_cast<unsigned long long>(ri), static_cast<unsigned long long>(rb), static_cast<unsigned long long>(rh),
                 mechanism_tracking_.known(), mechanism_tracking_.resolved(),
                 static_cast<unsigned long long>(mechanism_blends_), static_cast<unsigned long long>(mechanism_hard_), mech_max,
                 static_cast<unsigned long long>(amend_failures_), rebases_,
                 static_cast<unsigned long long>(resyncs_sent_), static_cast<unsigned long long>(resyncs_done_),
                 static_cast<unsigned long long>(rs.snapshots), static_cast<unsigned long long>(rs.matched),
                 static_cast<unsigned long long>(rs.mismatched), static_cast<unsigned long long>(rs.rollbacks),
                 static_cast<unsigned long long>(rs.resim_ticks), static_cast<unsigned long long>(rs.unmatched),
                 static_cast<unsigned long long>(rs.too_far), static_cast<unsigned long long>(rs.frozen),
                 static_cast<unsigned long long>(rs.impulses_restored),
                 static_cast<unsigned long long>(rs.contacts_restored), static_cast<unsigned long long>(rs.contact_restore_failures),
                 static_cast<unsigned long long>(rs.script_restores), static_cast<unsigned long long>(rs.script_restore_failures),
                 static_cast<unsigned long long>(rs.script_mismatches), static_cast<unsigned long long>(rs.script_float_mismatches),
                 static_cast<unsigned long long>(script_reset_skips_), rs.max_error,
                 rs.last_mismatch.c_str(),
                 static_cast<unsigned long long>(st.compared), static_cast<unsigned long long>(st.ignored),
                 static_cast<unsigned long long>(st.blended), static_cast<unsigned long long>(st.hard),
                 static_cast<unsigned long long>(st.unmatched), st.last_error, st.max_error,
                 static_cast<unsigned long long>(input_reports_), static_cast<unsigned long long>(input_starved_),
                 input_reports_ ? 100.0 * static_cast<double>(input_starved_) / static_cast<double>(input_reports_) : 0.0,
                 worst_starved_,
                 lead_count_ ? static_cast<double>(lead_sum_) / static_cast<double>(lead_count_) : 0.0);
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
        uint64_t life_rewinds_ = 0, life_rewinds_missed_ = 0;
        uint64_t input_rewinds_ = 0, input_rewinds_missed_ = 0;
        uint64_t forced_rollbacks_ = 0, forced_rollbacks_missed_ = 0;
        int restore_traces_ = 0;
        bool start_sector_pending_ = false;  // a --start-sector waiting for the first assigned frame
        bmmo::session::sector_union sectors_;   // the sectors this session runs, the rule every side runs
        uint8_t sector_lead_ = 0;            // SessionStart::sector_lead
        std::set<int> sectors_requested_;    // every sector THIS client reported (1 from the anchor)
        int sector_cell_ = 0;                // IngameParameter[0][1] as last seen or written here
        VxVector resetpoint_seen_;           // CurrentLevel[0,3]'s position as last seen: a move is an own crossing
        bool checkpoint_held_ = false;       // the level's checkpoint manager run is held
        CK_ID sector_manager_id_ = 0;
        bmmo::game::script_replay::selection scripts_;       // the script replay's scripts and satellites (game/script_replay.hpp)
        std::vector<CK_ID> scripts_replayed_;                // the scripts the last restore wrote: active at its anchor
        bmmo::game::script_replay_hooks script_hooks_;
        bmmo::game::sector_reset_guard reset_guard_;         // the retail sector resets, neutralized (game/sector_reset_guard.hpp)
        uint32_t script_restore_logs_ = 0;
        uint32_t script_reset_logs_ = 0;
        std::map<CK_ID, uint32_t> script_reset_ticks_;        // the tick each script was last told to reset
        uint64_t script_reset_skips_ = 0;                    // scripts a restore kept live for having been reset after its anchor
        std::string last_ball_forces_;                       // --trace: the last "ball forces" line
        std::string last_ball_contacts_;                     // --trace: the shape of the last "ball contacts" line
        uint32_t contact_restore_logs_ = 0;
        uint32_t contact_made_logs_ = 0;
        int contact_traces_ = 0;                             // --trace-restore: contact lines logged around rewinds
        bool input_rewind_pending_ = false;
        uint32_t input_rewind_tick_ = 0;
        std::string input_rewind_entity_;
        // Lifecycle events waiting for the tick they are due at (drain_due_events).
        std::vector<bmmo::session_event_msg> pending_events_;
        int32_t seed_ = 1;
        int level_ = 1;
        std::vector<bmmo::session::player_entry> players_;
        int own_join_order_ = -1;
        float spawn_position_[3] = {};
        bool spawn_known_ = false;
        float spawn_impulse_ = 0.0f;             // SessionStart::spawn_impulse (design 9.10)
        // The own ball's last Physicalize as capture_own_physicalize() saw it
        // before the step: what report_physicalize sends (design 9.10).
        struct {
            bool valid = false;
            uint32_t tick = 0;
            std::string entity;
            VxMatrix pose;
            bool spawn = false;                  // at CurrentLevel[0,3] then: flagged, and kicked
        } own_capture_;
        std::string own_body_entity_;            // the own ball entity that had a body after the last frame
        // The engine_->tick() frame() is running, numbered from 1, and 0
        // outside it - a replayed step runs the PreSimulate pass too, and only
        // the live tick an own_physicalize_callback was armed for may capture.
        uint64_t live_tick_serial_ = 0;
        uint64_t live_ticks_ = 0;
        std::string own_group_;
        bool own_group_set_ = false;
        bool own_physicalized_ = false;
        int load_waited_ = 0;
        uint64_t anchor_hash_ = 0;
        bool assigned_ = false;
        // Design 9.25 phase alignment: between the anchor and the assignment
        // the world does not step.  The server's world takes its own first step
        // under the start base it hands out, so a client that ran k frames in
        // that window would be k steps further from the anchor at every tick
        // number for the rest of the session - the phase offset that made every
        // script-driven mechanism (Level 11's sandbags) fight the rollback
        // engine.  Bounded: the game must not freeze for ever if the assignment
        // never comes.
        bool held_ = false;
        clock_type::time_point hold_deadline_{};
        int hold_polls_ = 0;
        bmmo::game::sequencer_state level_sequencers_;
        uint32_t tick_base_ = 0;
        int64_t frames_since_anchor_ = -1;
        clock_type::time_point origin_;
        int rebases_ = 0;
        std::deque<std::pair<uint32_t, bmmo::session::input_frame>> input_history_;
        float previous_cam_[3][3] = {};
        bool previous_cam_valid_ = false;
        // Input freshness as the server saw it (session_snapshot_msg.acked_input_tick).
        uint64_t input_reports_ = 0, input_starved_ = 0;
        uint32_t worst_starved_ = 0;
        uint64_t lead_sum_ = 0, lead_count_ = 0;
        bmmo::game::navigation_graph navigation_;
        bool navigation_known_ = false;
        std::vector<ball_row> ball_rows_;
        uint8_t last_keys_ = 0;
        bool last_nav_active_ = false;
        int trace_frames_ = 0;
        bmmo::session::body_corrector corrector_;
        std::map<std::string, bmmo::session::body_corrector> mechanism_correctors_;
        // Design 9.25: the mechanisms are tracked bodies of this client's own
        // world again, so a row has to be resolved onto one of its bodies
        // (mechanism_tracking.hpp), and every tracked name carries the
        // generation of the body behind it.
        bmmo::session::mechanism_registry mechanism_tracking_;
        std::map<std::string, uint32_t> generations_;
        uint64_t amend_failures_ = 0;
        bool beam_done_ = false;
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
        bool own_navigation_ = false;
        std::string own_nav_entity_;
        int own_key_codes_[8] = {};
        uint32_t own_key_blocks_[8] = {};
        int own_key_count_ = 0;
        bmmo::session::rollback_engine rollback_;
        std::map<uint32_t, bmmo::session::input_frame> own_inputs_;
        bool body_guard_ = false;
        std::string body_guard_entity_;
        int rollback_diag_ = 0;
        int step_diag_ = 0;
        // The black box (design 9.15).  `journal_start_` is the instant the
        // header was written, the origin every TICK record's ms is measured
        // from; the checkpoint bookkeeping is the LOCAL cadence and its rate
        // limit.
        bmmo::session::journal_writer journal_;
        clock_type::time_point journal_start_{};
        uint32_t journal_last_checkpoint_ = 0;
        bool journal_have_checkpoint_ = false;
        bool journal_checkpoint_pending_ = false;
        std::set<uint32_t> journal_announced_;   // ids that already have a PLAYER record
    };

    int own_physicalize_callback::Execute() {
        if (!armed_) {
            armed_ = true;
            return 0;
        }
        // Only inside the live tick it was armed for: a rewind's replayed
        // steps run this pass too, and a tick that never reached its
        // Simulate() leaves a stale callback behind.
        if (client_->live_tick_serial_ == serial_) client_->capture_own_physicalize(tick_);
        return 1;
    }
}

int main(int argc, char** argv) {
    // The SimTool has had this since the reset heisenbug; the session client
    // had not, which is why a crash here used to be a bare 0xC0000005 from the
    // harness with no stack at all.
    bmmo::sim::install_crash_reporter();
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
