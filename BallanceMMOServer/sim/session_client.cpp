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
#include "inbound_capture.hpp"

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
#include <physics/physics_savestate.hpp>
#include <physics/physics_direct.hpp>
#include <session/client_core.hpp>
#include <session/rollback_binding.hpp>
#include <session/rollback_binding_scripts.hpp>
#include <physics/physics_state.hpp>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <physics/tick_record.hpp>
#include <role/role.hpp>
#include <session/correction.hpp>
#include <session/input_relay.hpp>
#include <session/lifecycle.hpp>
#include <session/journal.hpp>
#include <session/bridge_recipe.hpp>
#include <session/lead_slew.hpp>
#include <session/mechanism_tracking.hpp>
#include <session/own_correction.hpp>
#include <session/rollback.hpp>
#include <session/sector_union.hpp>
#include <game/script_replay.hpp>
#include <game/sector_reset_guard.hpp>
#include <session/spawn_impulse.hpp>

#include <algorithm>
#include <array>
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
        // --trace-body NAME --trace-ticks A B: log describe_body(NAME) after
        // every live tick in [A, B], before every replayed tick in it and
        // after a rewind that ends in it - a body's collision bookkeeping,
        // for diffing a rewound run against a control.
        std::string trace_body;
        uint32_t trace_from = 0, trace_until = 0;
        // --trace-clock A B: the engine clock and the frame step after every
        // live tick in [A, B] and before every replayed tick in it (IVP
        // times, the PSI event queue, the manager's smoothed delta, the time
        // factor and the physics step) - for a rate or phase drift between a
        // rewound run and a control, which a body trace only shows as motion.
        uint32_t trace_clock_from = 0, trace_clock_until = 0;
        // --dump-savestate A B DIR: the record's savestate blob of every tick
        // in [A, B] written to DIR/<tick>.sav, after the live tick and again
        // after a rewind re-recorded it - so two clients' blobs of one tick
        // can be diffed field by field offline (BallanceMMOSimTool
        // --savestate-diff), where a journal only shows positions.
        uint32_t dump_savestate_from = 0, dump_savestate_until = 0;
        std::string dump_savestate_dir;
        // --compare-scripts DIR: at every tick for which DIR holds another
        // run's <tick>.scr (what --dump-savestate writes beside the blobs),
        // this run's mechanism scripts are captured and compared with it,
        // and every change of the first difference is logged by script, block
        // and field - the way to find where two runs' SCRIPT states part,
        // which a journal (poses) and a blob diff (IVP) both miss.
        std::string compare_scripts_dir;
        // --park-births: the bridge API v14 rule for a rewind across a sector
        // activation - the activation is not re-issued, a script inactive at
        // the anchor keeps its live state, a body born inside the window is
        // parked at its birth pose and re-posed after its birth tick.  The
        // default re-issues the activation inside the replay and lets the
        // restored module scripts make the bodies, joints and forces again
        // (plan doc 10).  The A/B.
        bool park_births = false;
        // --insert-births: put a parked window-born body where its Physicalize
        // made it before its birth tick's step (see the hook).  An A/B.
        bool insert_births = false;
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
        // --two-pass-recheck: recheck every body after all the poses are in
        // instead of each at its own write (bridge API v16).  Measured no
        // better on the prop probe; the A/B stays.
        bool two_pass_recheck = false;
        // --contact-flags N: BMMO_PHYSICS_CONTACT_* bits (1..16 = the recorded
        // fields to copy onto a re-created point, 256 = only for a body with no
        // contact left, 512 = only against static geometry, 1024 = wake a
        // sleeping movable partner as an impact would instead of leaving that
        // pair to IVP; the default is 768, the measured one).
        uint32_t contact_flags = BMMO_PHYSICS_CONTACT_DEFAULT;
        // The script replay (game/script_replay.hpp): the mechanism scripts'
        // state travels with the records and every replayed tick runs their
        // frame before its physics step.  --no-script-replay is the physics-only
        // replay, for the A/B.
        bool script_replay = true;
        // The IVP savestate (docs/ivp-savestate-design.md): every tick's record
        // carries the engine's whole collision state and a rollback rebuilds it
        // at the anchor.  --no-savestate is the per-body restore, for the A/B;
        // --savestate-strict makes the first blob/world inconsistency fail the
        // restore (the rollback otherwise skips the record and goes on).
        bool savestate = true;
        bool savestate_strict = false;
        uint32_t script_diff_logs = 8;           // --script-diff-logs N: how many "scripts re-executed ... differ" lines to log
        // --trace-plan: log which branch every body of a restore took.
        bool trace_plan = false;
        bool correct = true;
        bool rollback = true;         // design 9.6; --no-rollback falls back to blending
        int boot_ticks = 400;
        int anchor_timeout = 3000;
        int64_t pause_at = -1;        // test knob: stop ticking at this tick ...
        int pause_ms = 0;             // ... for this long (design 9.2 resync path)
        // --stall-ms T:MS is the same knob in the late-player harness's
        // spelling (docs/late-player-design.md 5.1 C7): a CPU stall at tick T,
        // nothing polled or stepped while it lasts.
        bool stall_named = false;
        // --hold-event TYPE:N:MS|drop (late-player design 5.1 C6): the N-th own
        // lifecycle event of TYPE (1-based) leaves this client MS ms after it
        // was made, or never.  The client applies and journals it at once, as
        // it would anyway: only the wire is gated, the reliable copy and both
        // paths of the unreliable repeats alike, so the server sees an event
        // that is late (or lost for good) and nothing else changes.
        // --window-origin T: the window: line's 60-tick grid starts at tick
        // T (a line at T - 1 closes the window before it), so an episode at T
        // begins a window instead of falling in the middle of one.
        uint32_t window_shift = 0;   // the grid's offset, (T - 1) mod 60; 0 = lines at multiples of 60
        int hold_event_type = -1;
        uint32_t hold_event_nth = 0;
        int hold_event_ms = 0;
        bool hold_event_drop = false;
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
        // --keep-lane Z: a closed-loop steer that holds the own ball on the
        // lane z = Z while the record drives (UP or DOWN held): LEFT or RIGHT
        // from the ball's sideways error and rate, a PD rule with a deadband,
        // added to the record's keys before they are injected - so the
        // navigation replica, the SessionInput mask and the journal replay
        // all see them.  The undriven record keeps the lateral line the spawn
        // kick gave the ball, and a ball more than 6.5 off the lane centre
        // never takes a checkpoint (maps/README.md, "Checkpoints are taken on
        // the lane centre"); --drive-stop still coasts, as the steer stops
        // with the record's keys.
        bool keep_lane = false;
        double keep_lane_z = 0.0;
        // --capture-inbound FILE / --replay-inbound FILE (shared-core design
        // 5.2 (5), inbound_capture.hpp): record every message handled, every
        // connection state change and every wall-clock read of the session
        // flow, by loop pass; or run the loop from such a file, unpaced, with
        // no network and nothing sent.  Off by default.
        std::string capture_inbound;
        std::string replay_inbound;
        // --trace-sends FILE: one line per payload the session flow sends
        // (tick, Steam flags, size, FNV-1a 64), written in a replay too, where
        // nothing reaches the wire: the replay's check of the sends.  Off by
        // default.
        std::string trace_sends;
    };

    // The record's keys (scripts/arena_map_test.py write_drive_record) are
    // DirectInput codes, the ones the level's Key Event blocks wait for.
    constexpr int kKeyUp = 200, kKeyDown = 208;
    // The steer: push while |error + damping * rate| exceeds the deadband.
    constexpr double kLaneDamping = 0.8;    // seconds of lateral rate weighed against the error
    constexpr double kLaneDeadband = 0.3;   // metres

    void usage() {
        std::puts("usage: BallanceMMOSessionClient --root <game dir> --server <ip:port> [--name N] [--level N]\n"
                  "       [--record <file.bmrc>] [--journal <file.bmjr>] [--join-first | --room <id> | --host --expect N]\n"
                  "       [--seconds S] [--trace] [--no-correct] [--no-rollback] [--pause-at TICK --pause-ms MS | --stall-ms TICK:MS]\n"
                  "       [--hold-event TYPE:N:MS|TYPE:N:drop]   (the N-th own event of TYPE leaves MS ms late, or never)\n"
                  "       [--window-origin TICK]   (the window: lines' 60-tick grid starts at TICK)\n"
                  "       [--start-sector N] [--beam X Y Z TICK] [--keep-lane Z]\n"
                  "       [--trace-body NAME --trace-ticks A B] [--trace-clock A B] [--trace-restore] [--dump-savestate A B DIR] [--compare-scripts DIR] [--park-births] [--insert-births]\n"
                  "       [--no-savestate] [--savestate-strict]   (the IVP savestate restore; off = the per-body restore)\n"
                  "       [--capture-inbound FILE | --replay-inbound FILE]   (record the inbound messages and clock reads; "
                  "replay them, unpaced, sending nothing)\n"
                  "       [--trace-sends FILE]   (every payload sent: tick, flags, size, FNV-1a 64; a replay writes it too)");
    }

    const char* event_type_name(int type) {
        switch (type) {
        case 0: return "physicalize";
        case 1: return "unphysicalize";
        case 2: return "sector";
        case 3: return "finish";
        case 4: return "bodyrevived";
        default: return "?";
        }
    }

    // TYPE:N:MS or TYPE:N:drop, TYPE a name (physicalize, unphysicalize,
    // sector, finish, bodyrevived) or its number.
    bool parse_hold_event(const std::string& spec, arguments& out) {
        const size_t a = spec.find(':');
        const size_t b = a == std::string::npos ? std::string::npos : spec.find(':', a + 1);
        if (b == std::string::npos) return false;
        std::string type = spec.substr(0, a);
        for (char& c: type) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        out.hold_event_type = -1;
        for (int t = 0; t <= 4; ++t)
            if (type == event_type_name(t) || type == std::to_string(t)) out.hold_event_type = t;
        if (out.hold_event_type < 0) return false;
        out.hold_event_nth = static_cast<uint32_t>(std::max(1, std::atoi(spec.substr(a + 1, b - a - 1).c_str())));
        const std::string what = spec.substr(b + 1);
        out.hold_event_drop = what == "drop";
        out.hold_event_ms = out.hold_event_drop ? 0 : std::atoi(what.c_str());
        return out.hold_event_drop || out.hold_event_ms > 0;
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
            else if (arg == "--keep-lane") { if (!(v = next())) return false; out.keep_lane = true; out.keep_lane_z = std::atof(v); }
            else if (arg == "--trace") out.trace = true;
            else if (arg == "--trace-restore") out.trace_restore = true;
            else if (arg == "--trace-body") { if (!(v = next())) return false; out.trace_body = v; }
            else if (arg == "--trace-ticks") {
                if (!(v = next())) return false;
                out.trace_from = static_cast<uint32_t>(std::atoi(v));
                if (!(v = next())) return false;
                out.trace_until = static_cast<uint32_t>(std::atoi(v));
            }
            else if (arg == "--dump-savestate") {
                if (!(v = next())) return false;
                out.dump_savestate_from = static_cast<uint32_t>(std::atoi(v));
                if (!(v = next())) return false;
                out.dump_savestate_until = static_cast<uint32_t>(std::atoi(v));
                if (!(v = next())) return false;
                out.dump_savestate_dir = v;
            }
            else if (arg == "--park-births") out.park_births = true;
            else if (arg == "--insert-births") out.insert_births = true;
            else if (arg == "--compare-scripts") {
                if (!(v = next())) return false;
                out.compare_scripts_dir = v;
            }
            else if (arg == "--trace-clock") {
                if (!(v = next())) return false;
                out.trace_clock_from = static_cast<uint32_t>(std::atoi(v));
                if (!(v = next())) return false;
                out.trace_clock_until = static_cast<uint32_t>(std::atoi(v));
            }
            else if (arg == "--no-restore-recheck") out.restore_recheck = false;
            else if (arg == "--no-restore-impulses") out.restore_impulses = false;
            else if (arg == "--no-restore-quats") out.restore_quaternions = false;
            else if (arg == "--restore-clock") out.restore_clock = true;
            else if (arg == "--no-restore-calm") out.restore_calm = false;
            else if (arg == "--no-restore-unit") out.restore_unit = false;
            else if (arg == "--no-restore-contacts") out.restore_contacts = false;
            else if (arg == "--two-pass-recheck") out.two_pass_recheck = true;
            else if (arg == "--contact-flags") { if (!(v = next())) return false; out.contact_flags = static_cast<uint32_t>(std::atoi(v)); }
            else if (arg == "--no-sleep-as-recorded") out.sleep_as_recorded = false;
            else if (arg == "--no-script-replay") out.script_replay = false;
            else if (arg == "--no-savestate") out.savestate = false;
            else if (arg == "--script-diff-logs") { if (!(v = next())) return false; out.script_diff_logs = static_cast<uint32_t>(std::atoi(v)); }
            else if (arg == "--savestate-strict") out.savestate_strict = true;
            else if (arg == "--trace-plan") out.trace_plan = true;
            else if (arg == "--no-correct") out.correct = false;
            else if (arg == "--no-rollback") out.rollback = false;
            else if (arg == "--boot-ticks") { if (!(v = next())) return false; out.boot_ticks = std::atoi(v); }
            else if (arg == "--pause-at") { if (!(v = next())) return false; out.pause_at = std::atoll(v); }
            else if (arg == "--pause-ms") { if (!(v = next())) return false; out.pause_ms = std::atoi(v); }
            else if (arg == "--stall-ms") {
                if (!(v = next())) return false;
                const std::string spec = v;
                const size_t colon = spec.find(':');
                if (colon == std::string::npos) return false;
                out.pause_at = std::atoll(spec.substr(0, colon).c_str());
                out.pause_ms = std::atoi(spec.substr(colon + 1).c_str());
                out.stall_named = true;
            }
            else if (arg == "--window-origin") {
                if (!(v = next())) return false;
                const long long origin = std::atoll(v);
                out.window_shift = static_cast<uint32_t>((((origin - 1) % 60) + 60) % 60);
            }
            else if (arg == "--hold-event") {
                if (!(v = next())) return false;
                if (!parse_hold_event(v, out)) return false;
            }
            else if (arg == "--capture-inbound") { if (!(v = next())) return false; out.capture_inbound = v; }
            else if (arg == "--replay-inbound") { if (!(v = next())) return false; out.replay_inbound = v; }
            else if (arg == "--trace-sends") { if (!(v = next())) return false; out.trace_sends = v; }
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
        if (!out.capture_inbound.empty() && !out.replay_inbound.empty()) return false;
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

    // One line written as it is, as logf writes a formatted one: the shared
    // core's log operation (it formats the text itself).
    void log_line(const std::string& text) {
        std::fputs(text.c_str(), stdout);
        std::putchar('\n');
        std::fflush(stdout);
    }

    // The wire recipe in the bridge's shape: shared with the core's journal
    // (session/bridge_recipe.hpp, moved there at step E2).
    using bmmo::session::to_bridge_recipe;

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
        explicit session_client(arguments args) : args_(std::move(args)) { core_.legacy = make_legacy_calls(); }
        // make_platform() hands core_ lambdas that hold `this`: a moved (or
        // copied) client would leave them calling into the old object.
        session_client(session_client&&) = delete;
        session_client& operator=(session_client&&) = delete;

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
            if (inbound_failed_) return 3;
            const auto& st = core_.s.corrector_.stats();
            return (st.blended + st.hard + st.unmatched) == 0 ? 0 : 2;
        }

        // ------------------------------------------------------------ role
        void run() override {
            sync_report_clock();
            const auto started = wall_now(bmmo::sim::wall_site::run_start);
            mark_running();
            const char* stopped = "the client stopped";
            while (running_) {
                if (!inbound_pass()) { stopped = "the inbound capture ended"; break; }
                update();
                release_held_events();
                if (ended_) { stopped = "the client ended the session"; break; }
                if (std::chrono::duration<double>(wall_now(bmmo::sim::wall_site::run_limit) - started).count() > args_.seconds) {
                    logf("time limit reached");
                    stopped = "the client's time limit was reached";
                    break;
                }
                switch (core_.s.phase_) {
                case phase::idle:
                    wall_sleep(std::chrono::milliseconds(5));
                    break;
                case phase::loading:
                    ++flow_steps_;
                    load_step();
                    break;
                case phase::running:
                    pace_and_frame();
                    break;
                }
                const auto now = wall_now(bmmo::sim::wall_site::run_report);
                if (now - last_report_ > std::chrono::seconds(5)) {
                    last_report_ = now;
                    report();
                }
            }
            // A session still running when the client goes down says so in its
            // box rather than just stopping mid-file.
            core_.journal().close(stopped);
            report();
            finish_inbound();
        }

        // --capture-inbound / --replay-inbound (inbound_capture.hpp).  Opened
        // before the engine boots: the engine changes the working directory.
        bool open_inbound(const std::string& command_line) {
            std::string error;
            const int64_t den = static_cast<int64_t>(clock_type::period::den / clock_type::period::num);
            if (!args_.capture_inbound.empty()) {
                if (!inbound_.open_capture(args_.capture_inbound, command_line, den, error)) {
                    logf("capture-inbound: %s", error.c_str());
                    return false;
                }
                logf("capture-inbound: recording to %s", args_.capture_inbound.c_str());
            } else if (!args_.replay_inbound.empty()) {
                if (!inbound_.open_replay(args_.replay_inbound, den, error)) {
                    logf("replay-inbound: %s", error.c_str());
                    return false;
                }
                logf("replay-inbound: replaying %s, captured as: %s", args_.replay_inbound.c_str(),
                     inbound_.command_line().c_str());
                const std::string differ = replay_arguments_differ(inbound_.command_line(), command_line);
                if (!differ.empty())
                    logf("replay-inbound: warning: the arguments differ from the capture's (%s); a divergence may be "
                         "the command line, not the code", differ.c_str());
            }
            if (!args_.trace_sends.empty()) {
                send_trace_ = std::fopen(args_.trace_sends.c_str(), "wb");
                if (!send_trace_) {
                    logf("trace-sends: cannot open %s", args_.trace_sends.c_str());
                    return false;
                }
            }
            return true;
        }

        // The arguments a replay must share with its capture: all but the
        // program, the network and output paths (--server, --journal, the
        // capture flags) and --record's path (a stored copy of the same
        // record).  "" when they agree; else the first difference.
        static std::string replay_arguments_differ(const std::string& captured, const std::string& current) {
            const auto words = [](const std::string& line) {
                std::vector<std::string> out;
                size_t at = 0;
                while (at < line.size()) {
                    const size_t space = line.find(' ', at);
                    const size_t end = space == std::string::npos ? line.size() : space;
                    if (end > at) out.push_back(line.substr(at, end - at));
                    at = end + 1;
                }
                std::vector<std::string> kept;
                for (size_t i = 1; i < out.size(); ++i) {
                    const std::string& w = out[i];
                    if (w == "--server" || w == "--journal" || w == "--capture-inbound" || w == "--replay-inbound" ||
                        w == "--trace-sends") {
                        ++i;
                        continue;
                    }
                    kept.push_back(w);
                    if (w == "--record") { kept.push_back("<path>"); ++i; }
                }
                return kept;
            };
            const auto a = words(captured), b = words(current);
            for (size_t i = 0; i < a.size() || i < b.size(); ++i) {
                const std::string x = i < a.size() ? a[i] : "(none)", y = i < b.size() ? b[i] : "(none)";
                if (x != y) return "argument " + std::to_string(i + 1) + ": captured " + x + ", here " + y;
            }
            return "";
        }

        bool replaying_inbound() const { return inbound_.replaying(); }

        int poll_incoming_messages() override {
            if (inbound_.replaying()) return replay_incoming_messages();
            const int count = interface_->ReceiveMessagesOnConnection(connection_, incoming_messages_, ONCE_RECV_MSG_COUNT);
            if (count <= 0) return 0;
            for (int i = 0; i < count; ++i) {
                if (inbound_.capturing())
                    inbound_.message(incoming_messages_[i]->m_pData, static_cast<uint32_t>(incoming_messages_[i]->m_cbSize));
                on_message(incoming_messages_[i]);
                incoming_messages_[i]->Release();
            }
            return count;
        }

        void poll_connection_state_changes() override {
            if (!inbound_.replaying()) {
                role::poll_connection_state_changes();
                return;
            }
            int32_t state = 0;
            uint32_t handle = 0;
            std::string end_debug;
            while (inbound_.take_connection(state, handle, end_debug)) {
                SteamNetConnectionStatusChangedCallback_t info{};
                info.m_hConn = handle;
                info.m_info.m_eState = static_cast<ESteamNetworkingConnectionState>(state);
                std::snprintf(info.m_info.m_szEndDebug, sizeof(info.m_info.m_szEndDebug), "%s", end_debug.c_str());
                on_connection_status_changed(&info);
            }
        }

        void on_connection_status_changed(SteamNetConnectionStatusChangedCallback_t* info) override {
            if (inbound_.capturing())
                inbound_.connection(static_cast<int32_t>(info->m_info.m_eState), info->m_hConn, info->m_info.m_szEndDebug);
            switch (info->m_info.m_eState) {
            case k_ESteamNetworkingConnectionState_ClosedByPeer:
            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
                logf("connection lost: %s", info->m_info.m_szEndDebug);
                if (!inbound_.replaying()) interface_->CloseConnection(info->m_hConn, 0, nullptr, false);
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
                    if (data.name == args_.name) core_.s.own_id_ = id;
                logf("logged in (own id %u, %zu online)", core_.s.own_id_, msg.online_players.size());
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
                if (msg.type == bmmo::room::event_type::PlayerLeft) core_.journal().leave_player(msg.subject);
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
                core_.on_session_assign(msg);
                break;
            }
            case bmmo::SessionSnapshot: {
                bmmo::session_snapshot_msg msg;
                if (!fill(msg)) return;
                ++core_.s.snapshots_received_;
                if (msg.session == core_.s.session_ && core_.s.phase_ == phase::running) apply_snapshot(msg);
                break;
            }
            case bmmo::SessionEvent: {
                bmmo::session_event_msg msg;
                if (!fill(msg)) return;
                ++core_.s.events_received_;
                if (msg.session != core_.s.session_ || core_.s.phase_ != phase::running) break;
                // The server sends every event several times (protocol 6): the
                // first arrival is the event, the rest are nothing.  Judged
                // only once this client runs: an event relayed while it loads
                // is dropped, and the server's catch-up after SessionReady
                // sends the same bytes again - those must not read as copies.
                if (!core_.s.event_seen_.first(bmmo::session::lifecycle_key_of(msg, msg.player))) {
                    ++core_.s.event_dups_;
                    break;
                }
                // Protocol 9: the server relays every event to every member,
                // its owner included.  Our own is an echo: applied at its
                // origin it is a plain ack; MOVED, SUPERSEDED or REJECTED it
                // says the server applied it elsewhere or not at all, and an
                // own birth is then moved inside our history (late-player
                // design 3.3).  Handled in the frame, before its other
                // rewinds (process_own_echoes), not here between two frames.
                if (msg.player == core_.s.own_id_ && core_.s.own_id_ != 0) {
                    core_.s.own_echoes_.push_back(std::move(msg));
                    break;
                }
                // Queued, not applied here: an event applied on arrival lands
                // at an arbitrary point relative to this client's physics step
                // and in network arrival order, while the server applies every
                // due event before its step in the members' join order.  The
                // drain in frame() matches the server; see drain_due_events.
                core_.s.pending_events_.push_back(std::move(msg));
                break;
            }
            case bmmo::SessionRemoteInput: {
                bmmo::session_remote_input_msg msg;
                if (!fill(msg)) return;
                ++core_.s.remote_inputs_received_;
                if (msg.session != core_.s.session_) return;
                note_held_section(msg);
                for (const auto& entry: msg.entries) {
                    auto it = core_.s.remotes_.find(entry.player);
                    // The journal's PLAYER record goes in at the oldest tick this
                    // entry covers, so no INPUT record of the player can precede
                    // it in tick order (a replay needs the player before its
                    // inputs); after the first time it is a no-op.
                    if (core_.journal().journalling()) core_.journal().announce_player(entry.player, msg.frame_tick(entry, 0));
                    for (size_t i = 0; i < entry.frames.size(); ++i) {
                        const uint32_t tick = msg.frame_tick(entry, i);
                        const auto& frame = entry.frames[i];
                        // The window re-delivers a frame up to relay_window
                        // times: the first arrival is the truth, the rest change
                        // nothing (no record, no correction, no predictor move).
                        if (!core_.s.relay_seen_.first(entry.player, tick)) { ++core_.s.relay_dup_; continue; }
                        ++core_.s.relay_new_;
                        // How late the first arrival is, in ticks already
                        // simulated: what the rollback has to reach to apply it.
                        if (core_.s.assigned_ && static_cast<int32_t>(current_tick() - tick) > 0) {
                            const uint32_t lag = current_tick() - tick;
                            core_.s.relay_lag_max_ = std::max(core_.s.relay_lag_max_, lag);
                            ++core_.s.relay_lag_hist_[std::min<uint32_t>(lag, kRelayLagBuckets - 1)];
                            ++core_.s.window_lag_[entry.player][lag];
                        }
                        // What the server applied for that player at that tick,
                        // whether or not this client mirrors the ball.
                        if (core_.journal().journalling())
                            core_.journal().writer().input(tick, entry.player, frame, bmmo::session::JOURNAL_INPUT_RELAYED);
                        if (it == core_.s.remotes_.end()) {
                            // No mirror yet: the datagram beat the Physicalize
                            // here (the ordered stream used to mask this).  Parked
                            // by tick, moved into the ring when the mirror is made.
                            core_.s.pending_relay_.store(entry.player, tick, frame);
                            ++core_.s.relay_parked_;
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
                break;
            }
            case bmmo::SessionLead: {
                bmmo::session_lead_msg msg;
                if (!fill(msg)) return;
                if (msg.session != core_.s.session_ || core_.s.phase_ != phase::running) break;
                on_session_lead(msg);
                break;
            }
            case bmmo::SessionEnd: {
                bmmo::session_end_msg msg;
                if (!fill(msg)) return;
                core_.on_session_end(msg);
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
        // The core's state and its types moved to session/client_core.hpp
        // (shared-core design step E1); reached as core_.s.<member>.
        using phase = bmmo::session::client_core_state::phase;
        using remote_ball = bmmo::session::client_core_state::remote_ball;
        using forced_own = bmmo::session::client_core_state::forced_own;
        using applied_death = bmmo::session::client_core_state::applied_death;
        using window_base = bmmo::session::client_core_state::window_base;

        // ------------------------------------------- the core's platform
        // The headless platform (shared-core design 2.2): the operations the
        // core calls, each the call this client makes today.  E1 starts it
        // with send, now and log; the steps that move code add the rest (E2:
        // utc_ms, body_state, list_bodies, journal_path).
        bmmo::session::session_platform make_platform() {
            bmmo::session::session_platform p;
            p.send = [this](const std::string& bytes, bmmo::session::send_mode mode) {
                int flags = k_nSteamNetworkingSend_Reliable;
                if (mode == bmmo::session::send_mode::reliable_no_nagle) flags = k_nSteamNetworkingSend_ReliableNoNagle;
                else if (mode == bmmo::session::send_mode::unreliable_no_delay) flags = k_nSteamNetworkingSend_UnreliableNoDelay;
                send_bytes(bytes, flags);
            };
            p.now = [this](bmmo::session::clock_site site) { return wall_now(site); };
            p.log = [](bmmo::session::log_level, const std::string& text) { log_line(text); };
            // The journal's (E2): --journal verbatim, the header's utc_ms,
            // and the bodies of corrections and LOCAL checkpoints.
            p.utc_ms = [this](bmmo::session::clock_site site) { return wall_utc_ms(site); };
            p.body_state = [this](const std::string& entity, bmmo_physics_body_state& out) {
                std::string error;
                return bmmo::physics::get_body_state(engine_->physics(), entity.c_str(), out, error);
            };
            p.list_bodies = [this]() { return bmmo::physics::physics_direct(engine_->physics()).list_bodies(); };
            p.journal_path = [this](const bmmo::session::journal_header&) { return std::filesystem::path(args_.journal); };
            // The host hooks (E3): the pacer's origin restarts with the
            // numbering, --hold-event, and its discards at the session's end.
            p.hooks.numbering_restarted = [this](bmmo::session::clock_site site) { origin_ = wall_now(site); };
            p.hooks.filter_event = [this](const bmmo::session_event_msg& event) {
                if (!hold_event(event)) return bmmo::session::event_send_verdict::send;
                return args_.hold_event_drop ? bmmo::session::event_send_verdict::drop
                                             : bmmo::session::event_send_verdict::hold;
            };
            p.hooks.ending = [this]() { discard_held_events(); };
            return p;
        }
        // Declared as each capability's operations arrive (design 2.3).  E3:
        // slew (SessionReady's caps = CAP_LEAD, as this client always sent)
        // and nonblocking_hold (the hold returns to the loop).
        static bmmo::session::session_capabilities make_capabilities() {
            bmmo::session::session_capabilities caps;
            caps.slew = true;
            caps.nonblocking_hold = true;
            return caps;
        }
        // The transitional back-calls (shared-core design 6.1): the functions
        // of this client that code moved into the core still calls.
        bmmo::session::legacy_calls make_legacy_calls() {
            bmmo::session::legacy_calls l;
            l.rebase_own_lives = [this]() { rebase_own_lives(); };
            l.set_aside_unanswered = [this](own_life::aside why) { set_aside_unanswered(why); };
            l.reset_held_epoch = [this](const char* why) { reset_held_epoch(why); };
            l.drop_pending_rewind = [this]() { drop_pending_rewind(); };
            l.finish_lead_slew = [this](const std::string& why, bool reanchored) { finish_lead_slew(why, reanchored); };
            l.end_session_world = [this]() { end_session_world(); };
            l.report = [this]() { report(); };
            return l;
        }
        // The core's configuration from the arguments (design 1.2).
        bmmo::session::core_config make_core_config() const {
            bmmo::session::core_config c;
            c.journal_checkpoint_ticks = args_.checkpoint_ticks;
            c.level = args_.level;
            c.verbose = args_.trace;
            c.restore_recheck = args_.restore_recheck;
            c.two_pass_recheck = args_.two_pass_recheck;
            c.restore_impulses = args_.restore_impulses;
            c.restore_quaternions = args_.restore_quaternions;
            c.restore_clock = args_.restore_clock;
            c.restore_calm = args_.restore_calm;
            c.restore_unit = args_.restore_unit;
            c.restore_contacts = args_.restore_contacts;
            c.contact_flags = args_.contact_flags;
            c.sleep_as_recorded = args_.sleep_as_recorded;
            c.script_replay = args_.script_replay;
            c.savestate = args_.savestate;
            c.script_diff_logs = args_.script_diff_logs;
            c.trace_plan = args_.trace_plan;
            return c;
        }

        // ------------------------------------------------- inbound capture
        // The wall clock as the session flow reads it: the live clock, written
        // to the capture by `site` when capturing; the captured value when
        // replaying (a read at another site than the capture's next one is a
        // divergence).
        clock_type::time_point wall_now(bmmo::sim::wall_site site) {
            if (inbound_.replaying()) {
                int64_t value = 0;
                std::string error;
                if (!inbound_.take_clock(site, value, error)) inbound_diverged(error);
                return clock_type::time_point(clock_type::duration(value));
            }
            const auto now = clock_type::now();
            if (inbound_.capturing()) inbound_.clock(site, static_cast<int64_t>(now.time_since_epoch().count()));
            return now;
        }

        // The system clock in ms since the epoch (the journal header's utc_ms,
        // read at wall_site::journal_utc).
        uint64_t wall_utc_ms(bmmo::sim::wall_site site) {
            if (inbound_.replaying()) {
                int64_t value = 0;
                std::string error;
                if (!inbound_.take_clock(site, value, error)) inbound_diverged(error);
                return static_cast<uint64_t>(value);
            }
            const auto ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            if (inbound_.capturing()) inbound_.clock(site, static_cast<int64_t>(ms));
            return ms;
        }

        // A replay is unpaced: its clock is the capture's, so a wait only
        // costs time.
        template <class Duration>
        void wall_sleep(Duration duration) {
            if (!inbound_.replaying()) std::this_thread::sleep_for(duration);
        }

        // last_report_ is read at construction, before the capture is open:
        // its value goes into the file at the start of run() instead.
        void sync_report_clock() {
            if (inbound_.replaying()) last_report_ = wall_now(bmmo::sim::wall_site::report_init);
            else if (inbound_.capturing())
                inbound_.clock(bmmo::sim::wall_site::report_init,
                               static_cast<int64_t>(last_report_.time_since_epoch().count()));
        }

        // The start of a main-loop pass: its index and the steps before it
        // (the frame index of every record that follows).  False when a
        // replay has reached the end of its capture.
        bool inbound_pass() {
            const uint64_t pass = flow_pass_++;
            if (inbound_.capturing()) inbound_.iteration(pass, flow_steps_);
            if (!inbound_.replaying()) return true;
            if (inbound_.exhausted()) {
                // The live run ended inside its last pass (a break, or the
                // connection lost): a replay that starts one more pass than
                // the capture holds has left the live run's control flow.
                if (inbound_.complete())
                    inbound_diverged("the live run ended before this pass (the capture's end marker)");
                logf("replay-inbound: the capture ends before pass %llu (%s)", static_cast<unsigned long long>(pass),
                     "no end marker: the live run was cut short");
                return false;
            }
            uint64_t index = 0, steps = 0;
            if (!inbound_.take_iteration(index, steps))
                inbound_diverged("expected a loop pass, the capture has " + inbound_.describe_next());
            if (index != pass || steps != flow_steps_)
                inbound_diverged("the capture's pass " + std::to_string(index) + " ran after " + std::to_string(steps)
                                 + " steps, this replay's pass " + std::to_string(pass) + " after "
                                 + std::to_string(flow_steps_));
            return true;
        }

        int replay_incoming_messages() {
            int count = 0;
            std::string bytes;
            while (inbound_.take_message(bytes)) {
                ISteamNetworkingMessage* message = SteamNetworkingUtils()->AllocateMessage(static_cast<int>(bytes.size()));
                if (!message) inbound_diverged("AllocateMessage failed");
                if (!bytes.empty()) std::memcpy(message->m_pData, bytes.data(), bytes.size());
                message->m_cbSize = static_cast<int>(bytes.size());
                on_message(message);
                message->Release();
                ++count;
            }
            return count;
        }

        // A replay that met a read or a pass the live run did not make: the
        // world is no longer the captured one, so it stops here.
        [[noreturn]] void inbound_diverged(const std::string& why) {
            logf("replay-inbound: DIVERGED at pass %llu (%llu steps), %s: %s",
                 static_cast<unsigned long long>(flow_pass_ ? flow_pass_ - 1 : 0),
                 static_cast<unsigned long long>(flow_steps_), inbound_.where().c_str(), why.c_str());
            core_.journal().close("the inbound replay diverged");
            std::fflush(stdout);
            std::exit(3);
        }

        void finish_inbound() {
            if (inbound_.capturing()) {
                logf("capture-inbound: %llu passes, %llu steps, %llu messages written",
                     static_cast<unsigned long long>(flow_pass_), static_cast<unsigned long long>(flow_steps_),
                     static_cast<unsigned long long>(inbound_.messages()));
                inbound_.close();
                if (inbound_.write_failed())
                    logf("capture-inbound: WRITE FAILED: %s is incomplete (no end marker); do not replay it",
                         args_.capture_inbound.c_str());
            }
            if (inbound_.replaying()) {
                if (!inbound_.exhausted()) {
                    logf("replay-inbound: DIVERGED: the replay stopped at pass %llu with %s left in the capture (%s)",
                         static_cast<unsigned long long>(flow_pass_), inbound_.describe_next().c_str(),
                         inbound_.where().c_str());
                    inbound_failed_ = true;
                } else {
                    logf("replay-inbound: done, %llu passes, %llu steps, %llu messages (%s)",
                         static_cast<unsigned long long>(flow_pass_), static_cast<unsigned long long>(flow_steps_),
                         static_cast<unsigned long long>(inbound_.messages()),
                         inbound_.complete() ? "the capture's end marker reached" : "the capture had no end marker");
                }
            }
            if (send_trace_) {
                std::fclose(send_trace_);
                send_trace_ = nullptr;
            }
        }

        // ------------------------------------------------------ networking
        // --trace-sends: before the replay's early return, so a replay's sends
        // can be compared with another binary's.  The build id (SessionReady
        // carries it, a uint16 length and the string) differs between any two
        // builds, so it and its length are replaced by a fixed token first;
        // the size printed is the masked one.
        void trace_send(const void* data, size_t size, int flags) {
            if (!send_trace_) return;
            std::string bytes(static_cast<const char*>(data), size);
            const std::string id = std::string(bmmo::physics::build_id()).substr(0, bmmo::session::MAX_NAME);
            const size_t at = id.empty() ? std::string::npos : bytes.find(id);
            if (at != std::string::npos && at >= 2 &&
                static_cast<unsigned char>(bytes[at - 2]) == (id.size() & 0xff) &&
                static_cast<unsigned char>(bytes[at - 1]) == (id.size() >> 8))
                bytes.replace(at - 2, id.size() + 2, "<build-id>");
            uint64_t h = 1469598103934665603ull;
            for (unsigned char c : bytes) { h ^= c; h *= 1099511628211ull; }
            std::fprintf(send_trace_, "%u %d %zu %016llx\n", static_cast<unsigned>(core_.current_tick()), flags,
                         bytes.size(), static_cast<unsigned long long>(h));
        }

        void send_bytes(const std::string& bytes, int flags) {
            trace_send(bytes.data(), bytes.size(), flags);
            if (inbound_.replaying()) return;   // a replay sends nothing
            interface_->SendMessageToConnection(connection_, bytes.data(), static_cast<uint32_t>(bytes.size()), flags, nullptr);
        }

        void announce_map() {
            bmmo::current_map_msg msg{};
            msg.content.map.type = bmmo::map_type::OriginalLevel;
            msg.content.map.level = args_.level;
            bmmo::hex_chars_from_string(msg.content.map.md5, bmmo::map::original_map_hashes[std::clamp(args_.level, 0, 13)]);
            msg.content.sector = 1;
            msg.content.type = bmmo::current_map_state::EnteringMap;
            trace_send(&msg, sizeof(msg), k_nSteamNetworkingSend_Reliable);
            if (inbound_.replaying()) return;
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
                    wall_sleep(std::chrono::milliseconds(500));
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
                if (m.id == core_.s.own_id_) own_ready = m.ready;
            if (!own_ready) {
                // First time, or the room went back to the lobby (session ended
                // by the server, host restart): ready again, at most once a second.
                const auto now = wall_now(bmmo::sim::wall_site::room_ready);
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
            // The session's parameters and the core's fresh state (shared-core
            // design E3: session_client_core::begin).
            if (core_.begin(msg, make_core_config()).refused) {
                ended_ = true;
                return;
            }
            snapshot_interval_ = msg.snapshot_interval;
            spawn_known_ = false;
            for (const auto& p: core_.s.players_) {
                if (p.id != core_.s.own_id_) continue;
                for (int k = 0; k < 3; ++k) spawn_position_[k] = p.spawn_position[k];
                spawn_known_ = true;
            }
            // This client's fresh runtime state.
            navigation_known_ = false;
            local_births_.clear();
            manager_activations_.clear();
            activations_replayed_ = 0;
            beam_done_ = false;
            own_group_set_ = false;
            own_capture_.valid = false;
            scripts_.clear();
            script_reset_ticks_.clear();
            reset_guard_.restore(engine_->context());
            checkpoint_held_ = false;
            sector_manager_id_ = 0;
            std::string error;
            if (bmmo::game::script_active(engine_->context(), "Gameplay_Ingame")) {
                // Restart (host started the room again): reboot to the menu.
                logf("session %u: level still loaded, rebooting the engine", core_.s.session_);
                for (auto& [id, remote]: core_.s.remotes_) {
                    if (remote.navigation) bmmo::physics::navigation_destroy(engine_->physics(), remote.entity.c_str(), error);
                }
                core_.s.remotes_.clear();
                core_.s.relay_seen_.clear();
                core_.s.held_seen_.clear();
                core_.s.held_stale_seen_.clear();
                core_.s.pending_relay_.clear();
                core_.s.event_seen_.clear();
                core_.s.own_event_repeats_.clear();
                core_.s.remote_last_physicalize_.clear();
                core_.s.remote_last_unphysicalize_.clear();
                if (!boot_engine()) { ended_ = true; return; }
            }
            engine_->clear_keys();
            engine_->request_level(core_.s.level_);
            load_waited_ = 0;
            level_sequencers_.clear();
            core_.level_requested();
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
            if (!bmmo::physics::reset_session_clock(physics, core_.s.seed_, error)) { logf("clock reset: %s", error.c_str()); ended_ = true; return; }
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
            core_.s.sectors_requested_ = {1};
            if (CKDataArray* parameters = engine_->data_array("IngameParameter"))
                parameters->GetElementValue(0, 1, &core_.s.sector_cell_);
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
                         static_cast<unsigned>(core_.s.sector_lead_));
                else
                    logf("the level's checkpoint sector activation was NOT held (%s): crossings activate here at "
                         "once and are reported after the fact - this client is out of lockstep on every sector it enters",
                         hold_error.c_str());
            }
            if (args_.start_sector > 0) {
                core_.s.start_sector_pending_ = true;
                logf("--start-sector %d will be stamped %u ticks ahead of the first assigned frame", args_.start_sector,
                     static_cast<unsigned>(core_.s.sector_lead_));
            }
            rebases_ = 0;
            hold_event_seen_ = 0;
            // The phase, the pacer's origin (hooks.numbering_restarted), the
            // hold, SessionReady and the journal (shared-core design E3:
            // session_client_core::anchor).
            bmmo::session::anchor_report report;
            report.hash = hash;
            report.physics_sha256 = sizeof(void*) == 8 ? "headless-64" : "headless-32";
            report.build_id = bmmo::physics::build_id();
            report.own_name = args_.name;
            report.load_ticks = load_waited_;
            core_.anchor(report);
        }

        // The lead the server sized for this session (SessionStart).
        uint32_t sector_lead_ticks() const { return core_.s.sector_lead_; }

        // A sector this client enters, as an input: stamped for `due`, sent at
        // once, and handed to the union, which starts it before the step of
        // that tick (or the first one after it the pacer allows) - as the
        // server does with the event, and every other member with its relay.
        // Once per sector per session.
        void hold_sector(int sector, uint32_t due, const char* why) {
            if (core_.s.sectors_requested_.count(sector)) return;
            core_.s.sectors_requested_.insert(sector);
            bmmo::session_event_msg event;
            event.tick = due;
            event.type = bmmo::session::event_type::Sector;
            event.sector = sector;
            send_event(event);
            core_.s.sectors_.stamp(core_.s.own_id_, sector, due);
            logf("sector %d stamped for tick %u (%s, lead %u, input_delay %u), event sent ahead", sector, due, why,
                 sector_lead_ticks(), static_cast<unsigned>(core_.s.input_delay_));
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

        // struct own_life: the own ball's body changes the history has to keep
        // (session/client_core.hpp).
        using own_life = bmmo::session::client_core_state::own_life;

        // The automation `beam` verb, headless: the own ball at `position`,
        // upright and at rest.  The server's copy is moved by re-reporting the
        // ball's life at that pose - a plain repeat Physicalize would not move
        // it, because physicalize() of an entity that already has a body is a
        // no-op (physics_state.cpp:741), so the pair the retail death sends is
        // used instead.
        //
        // This client does what the server does with that pair, at the pair's
        // stamp: the body is rebuilt at tick + 1, before that tick's step
        // (apply_own_lives), not moved now.  Moving it here, after the record of
        // `tick`, left the move outside every replay: a rewind anchored at or
        // before `tick` re-stepped the old path and nothing put the beam back
        // (measured: the own ball 540 m off on three of four clients), and
        // the moved core kept its floor contact, which the server's fresh one
        // does not have.  Applied at the stamp, live and in every replay that
        // crosses it, the record of `tick` stays the pre-beam world the
        // server's checkpoint of `tick` is, and the lifetime stays the same:
        // the body has the same name, recipe and group on every side.
        void beam_own_ball(uint32_t tick) {
            CK3dEntity* ball = current_ball();
            const std::string name = ball && ball->GetName() ? ball->GetName() : "";
            if (name.empty() || !core_.s.own_physicalized_) {
                logf("--beam at tick %u: no own body to beam", tick);
                return;
            }
            // Without the navigation replica (no --rollback) the ball is driven
            // by the retail force controllers, which die with the core and are
            // not re-made until the key is released: the body is moved in
            // place, as before.  There is no rewind to undo it then.
            if (!own_navigation_) {
                const double rotation[4] = {0.0, 0.0, 0.0, 1.0};
                const float still[3] = {0.0f, 0.0f, 0.0f};
                std::string error;
                if (!bmmo::physics::set_body_state(engine_->physics(), name.c_str(), args_.beam_position, rotation,
                                                   still, still, true, error)) {
                    logf("--beam at tick %u: %s", tick, error.c_str());
                    return;
                }
                ++core_.s.generations_[name];   // the server rebuilds its copy: a new lifetime
            }
            own_life beam;
            beam.what = own_life::kind::beam;
            beam.origin = beam.applied = tick + 1;
            beam.entity = name;
            {
                const auto generation = core_.s.generations_.find(name);
                beam.generation = generation == core_.s.generations_.end() ? 0u : generation->second;
            }
            beam.ball_type = static_cast<uint8_t>(std::max(ball_type_of(name), 0));
            for (int k = 0; k < 3; ++k) beam.position[k] = static_cast<float>(args_.beam_position[k]);
            for (int r = 0; r < 3; ++r)
                for (int k = 0; k < 3; ++k) beam.rotation[r * 3 + k] = r == k ? 1.0f : 0.0f;
            fill_retail_recipe(ball_type_of(name), beam.recipe);
            beam.group = core_.s.own_group_;
            bmmo::session_event_msg down;
            down.tick = beam.origin;
            down.type = bmmo::session::event_type::Unphysicalize;
            send_event(down);
            bmmo::session_event_msg up;
            up.tick = beam.origin;
            up.type = bmmo::session::event_type::Physicalize;
            up.ball_type = beam.ball_type;
            std::copy(std::begin(beam.position), std::end(beam.position), up.position);
            std::copy(std::begin(beam.rotation), std::end(beam.rotation), up.rotation);
            up.recipe = beam.recipe;
            send_event(up);
            logf("--beam: %s to (%.3f,%.3f,%.3f), reported to the server for tick %u and %s", name.c_str(),
                 args_.beam_position[0], args_.beam_position[1], args_.beam_position[2], beam.origin,
                 own_navigation_ ? "rebuilt here at that tick" : "moved here now (no navigation replica)");
            if (own_navigation_) core_.s.own_life_.push_back(std::move(beam));
        }

        // A birth the retail scripts made this frame and report_physicalize
        // sent, as an own_life: the body the scripts made stays (a replay
        // across it parks it as before), and the record is what moves it when
        // the server's echo says it applied the Physicalize later, or removes
        // it when the server never did (late-player design 3.3).
        void record_own_birth(const bmmo::session_event_msg& event, const std::string& entity) {
            if (!args_.rollback) return;   // no history to move it in
            own_life birth;
            birth.what = own_life::kind::birth;
            birth.origin = birth.applied = event.tick;
            birth.entity = entity;
            {
                const auto generation = core_.s.generations_.find(entity);
                birth.generation = generation == core_.s.generations_.end() ? 0u : generation->second;
            }
            birth.ball_type = event.ball_type;
            std::copy(std::begin(event.position), std::end(event.position), birth.position);
            std::copy(std::begin(event.rotation), std::end(event.rotation), birth.rotation);
            birth.recipe = event.recipe;
            birth.group = core_.s.own_group_;
            birth.spawn = (event.flags & bmmo::session::PHYSICALIZE_FLAG_SPAWN) != 0;
            birth.origin_live = birth.applied_live = true;
            core_.s.own_life_.push_back(std::move(birth));
        }

        // Whether the ball's lifetime `life` belongs to is still the one the
        // world has: a death or a trafo since (the drain bumped the
        // generation) ended it, and a replay that made its body again would
        // put back a body the live run no longer has, with nothing in the
        // replay to take it out again.
        bool own_life_current(const own_life& life) const {
            const auto generation = core_.s.generations_.find(life.entity);
            return (generation == core_.s.generations_.end() ? 0u : generation->second) == life.generation;
        }

        // The navigation replica shut down before the core goes: its
        // controllers would otherwise point into it.  The input of the next
        // step re-creates them on the new core, as the server's PreSimulate
        // pass does.
        void own_navigation_off(const std::string& entity, uint32_t tick) {
            if (!own_navigation_ || own_nav_entity_ != entity) return;
            const bmmo_physics_nav_state off{};
            std::string error;
            if (!bmmo::physics::navigation_set_state(engine_->physics(), own_nav_entity_.c_str(), off, error))
                logf("own body at tick %u: navigation shutdown: %s", tick, error.c_str());
        }

        // The own body of `life` taken out, with the event log muted: not a
        // lifetime change of the body the records describe (a beam's
        // Unphysicalize half, a moved birth before its applied tick).  The
        // entity is put at the pose its Physicalize carried, where the body
        // is made again.
        bool unmake_own_body(const own_life& life, uint32_t tick, const char* where) {
            own_navigation_off(life.entity, tick);
            std::string error;
            bool ok = false;
            {
                const event_log_mute mute(*this);
                ok = bmmo::physics::unphysicalize(engine_->physics(), life.entity.c_str(), error);
            }
            if (auto* entity = CK3dEntity::Cast(engine_->context()->GetObjectByNameAndParentClass(
                        const_cast<CKSTRING>(life.entity.c_str()), CKCID_3DENTITY, nullptr)))
                entity->SetWorldMatrix(matrix_from_pose(life.position, life.rotation));
            core_.s.mechanism_tracking_.mark_dirty();
            if (!ok && core_.s.own_lives_applied_ < 16)
                logf("own body at tick %u (%s): unphysicalize %s: %s", tick, where, life.entity.c_str(), error.c_str());
            return ok;
        }

        // The own body of `life` made at `tick` the way the server makes it
        // (physics_world::apply_event): the pose, recipe and group its
        // Physicalize carried, and a spawn's kick with the impulse index of
        // `tick` - the applied tick, which the server and every remote kick
        // with too.  A body still there is taken out first (a beam's pair in
        // one tick, or a body a restore parked instead of destroying).  The
        // event log is muted: the body is the same life to every side, so
        // nothing about it is reported, and its generation stays.
        bool remake_own_body(const own_life& life, uint32_t tick, const char* where) {
            CKIpionManager* physics = engine_->physics();
            own_navigation_off(life.entity, tick);
            auto* entity = CK3dEntity::Cast(engine_->context()->GetObjectByNameAndParentClass(
                    const_cast<CKSTRING>(life.entity.c_str()), CKCID_3DENTITY, nullptr));
            if (!entity) {
                logf("own body at tick %u: no 3D entity named %s", tick, life.entity.c_str());
                return false;
            }
            std::string error;
            bool made = false;
            {
                const event_log_mute mute(*this);
                bmmo_physics_body_state there{};
                if (bmmo::physics::get_body_state(physics, life.entity.c_str(), there, error)
                        && !bmmo::physics::unphysicalize(physics, life.entity.c_str(), error))
                    logf("own body at tick %u (%s): unphysicalize %s: %s", tick, where, life.entity.c_str(), error.c_str());
                entity->SetWorldMatrix(matrix_from_pose(life.position, life.rotation));
                const auto recipe = to_bridge_recipe(life.recipe);
                made = bmmo::physics::physicalize(physics, life.entity.c_str(), recipe, life.group.c_str(), error);
            }
            core_.s.mechanism_tracking_.mark_dirty();
            ++core_.s.own_lives_applied_;
            if (!made) {
                logf("own body at tick %u (%s): physicalize %s: %s", tick, where, life.entity.c_str(), error.c_str());
                return false;
            }
            if (life.spawn && core_.s.spawn_impulse_ > 0.0f) {
                const uint8_t join_order = static_cast<uint8_t>(core_.s.own_join_order_ < 0 ? 63 : core_.s.own_join_order_);
                const uint32_t idx = bmmo::session::spawn_direction_index(core_.s.seed_, join_order, tick);
                std::string impulse_error;
                if (!bmmo::physics::push_impulse(physics, life.entity.c_str(), bmmo::session::kSpawnDirectionTable[idx],
                                                 core_.s.spawn_impulse_, 0, impulse_error))
                    logf("own body at tick %u: spawn impulse: %s", tick, impulse_error.c_str());
            }
            if (core_.s.own_lives_applied_ <= 16)
                logf("own body: %s made at (%.3f,%.3f,%.3f) before tick %u (%s, %s%s)", life.entity.c_str(),
                     static_cast<double>(life.position[0]), static_cast<double>(life.position[1]),
                     static_cast<double>(life.position[2]), tick, where,
                     life.what == own_life::kind::beam ? "beam" : "moved birth", life.spawn ? ", spawn kick" : "");
            return true;
        }

        // The own body changes due before the step of `tick`, as the server
        // applies the pair or the Physicalize (physics_world::apply_event):
        //
        //   * a beam at its stamp: the body goes, and is made again at the
        //     beam pose when the server applied its Physicalize there too;
        //   * a moved birth or beam Physicalize at its applied tick: the body
        //     made again (remake_own_body).  A birth's own origin needs
        //     nothing: the scripts made the body live, a replay parks it, and
        //     a moved one was taken out at the restore (the destroy_body hook,
        //     or process_own_echoes before the first rewind).
        //
        // `live`: the frame of `tick` itself (live_own_lives), each change
        // once; otherwise a replay (drain_pending_births), where a beam's
        // stamp is redone only once its live frame has run - before that no
        // replay reaches it.  A lifetime that ended since is never made again
        // (own_life_current) and retires.
        void apply_own_lives(uint32_t tick, const char* where, bool live) {
            for (auto& life: core_.s.own_life_) {
                if (life.retired) continue;
                const bool beam_stamp = life.what == own_life::kind::beam && life.origin == tick
                                     && (live ? !life.origin_live : life.origin_live);
                const bool remake_due = life.moved() && life.applied == tick && (!live || !life.applied_live);
                if (!beam_stamp && !remake_due) continue;
                if (!own_life_current(life)) {
                    const auto generation = core_.s.generations_.find(life.entity);
                    logf("own %s stamped %u retired at tick %u (%s): %s has had another life since (generation %u -> %u)",
                         life.what == own_life::kind::beam ? "beam" : "birth", life.origin, tick, where,
                         life.entity.c_str(), life.generation, generation == core_.s.generations_.end() ? 0u : generation->second);
                    life.retired = true;
                    continue;
                }
                if (beam_stamp) {
                    if (life.made && life.applied == life.origin) {
                        remake_own_body(life, tick, where);
                    } else {
                        unmake_own_body(life, tick, where);
                        ++core_.s.own_lives_applied_;
                    }
                    if (live) life.origin_live = true;
                    if (live && life.applied == life.origin) life.applied_live = true;
                }
                if (remake_due) {
                    if (remake_own_body(life, tick, where)) ++core_.s.own_remakes_;
                    if (live) life.applied_live = true;
                }
                if (live) {
                    // The body this frame's step starts from: the capture
                    // callback of this frame must not take it for a birth of
                    // the scripts (capture_own_physicalize).
                    bmmo_physics_body_state state{};
                    std::string error;
                    if (bmmo::physics::get_body_state(engine_->physics(), life.entity.c_str(), state, error))
                        core_.s.own_body_entity_ = life.entity;
                }
            }
        }

        // Whether apply_own_lives has anything for a replayed `tick`.
        bool own_lives_due(uint32_t tick) const {
            for (const auto& life: core_.s.own_life_) {
                if (life.retired) continue;
                if (life.what == own_life::kind::beam && life.origin == tick && life.origin_live) return true;
                if (life.moved() && life.applied == tick) return true;
            }
            return false;
        }

        // The live half, in the frame of the change, after every rewind of
        // that frame (they all end at the tick before it, on the old path)
        // and before the step, at our place among the body changes stamped
        // for the same tick (apply_held_lifecycle).  A change whose live frame
        // was skipped (a resync renumbered past it) is dropped.  The records
        // retire once no rewind can be anchored before them.
        void live_own_lives(uint32_t frame_tick) {
            for (auto& life: core_.s.own_life_) {
                if (life.retired) continue;
                const bool beam_late = life.what == own_life::kind::beam && !life.origin_live
                                    && static_cast<int32_t>(frame_tick - life.origin) > 0;
                const bool remake_late = life.moved() && !life.applied_live
                                      && static_cast<int32_t>(frame_tick - life.applied) > 0;
                if (beam_late || remake_late) {
                    logf("own %s stamped %u (applied %u) never met its live frame (now %u): dropped",
                         life.what == own_life::kind::beam ? "beam" : "birth", life.origin, life.applied, frame_tick);
                    life.retired = true;
                }
            }
            apply_own_lives(frame_tick, "live", true);
            retire_own_lives();
        }

        // A record retires once the live run has made all of it and the
        // history no longer holds the tick before its last change - the
        // origin, or the applied tick of a moved one: no rewind can cross it
        // any more.  (A moved birth has to outlive its origin's anchor: a
        // rewind anchored inside the gap still crosses the applied tick, and
        // the record is what makes the body there.)
        //
        // A birth the server has not answered yet stays, past its history
        // too, until its echo or the deadline (expire_own_echoes) says where
        // the server made it: the answer past the history is a resync, and
        // without the record it could not take a never-made body out.  It
        // makes nothing in a replay (it is not moved), so keeping it is free.
        void retire_own_lives() {
            const uint32_t now = current_tick();
            core_.s.own_life_.erase(std::remove_if(core_.s.own_life_.begin(), core_.s.own_life_.end(), [this, now](const own_life& life) {
                if (life.retired) return true;
                if (life.what == own_life::kind::birth && life.state == own_life::verdict::sent
                        && static_cast<int32_t>(now - life.origin) < static_cast<int32_t>(kOwnRecordKeepTicks))
                    return false;
                const bool done = life.origin_live && (life.applied_live || !life.made);
                const uint32_t last = life.moved() ? life.applied : life.origin;
                return done && !core_.s.rollback_.knows_tick(last - 1);
            }), core_.s.own_life_.end());
        }

        // A resync renumbers onto the server's tick and its full snapshot
        // puts the server's own ball in: the births go.  A beam the new
        // numbering has not reached yet is the server's to apply at its stamp,
        // and so ours: live once more.
        // A birth still unanswered is set aside (own_unanswered_): its echo,
        // when it comes, is not one a record of the new numbering can take.
        void rebase_own_lives() {
            set_aside_unanswered(own_life::aside::resync);
            core_.s.own_life_.erase(std::remove_if(core_.s.own_life_.begin(), core_.s.own_life_.end(), [this](own_life& life) {
                if (life.what != own_life::kind::beam || static_cast<int32_t>(life.origin - core_.s.tick_base_) < 0) return true;
                life.origin_live = false;
                life.applied_live = false;
                return false;
            }), core_.s.own_life_.end());
        }

        // The births still waiting for their echo, kept aside (with the echoes
        // already queued: they answer the old numbering) when a SessionAssign
        // renumbers the history.  Bounded: the echo of each comes within a
        // round trip or two, and an old entry answers nothing any more.
        void set_aside_unanswered(own_life::aside why) {
            const uint32_t now = current_tick();
            for (const auto& life: core_.s.own_life_) {
                if (life.retired || life.what != own_life::kind::birth || life.state != own_life::verdict::sent) continue;
                own_life aside = life;
                aside.why = why;
                aside.aside_at = now;
                core_.s.own_unanswered_.push_back(std::move(aside));
            }
            for (auto& msg: core_.s.own_echoes_) core_.s.own_echoes_stale_.push_back(std::move(msg));
            core_.s.own_echoes_.clear();
            while (core_.s.own_unanswered_.size() > 16) core_.s.own_unanswered_.erase(core_.s.own_unanswered_.begin());
        }

        // The set-aside birth an echo answers, or null.
        own_life* find_unanswered(uint32_t origin, uint8_t ball_type) {
            for (auto& life: core_.s.own_unanswered_)
                if (life.origin == origin && life.ball_type == ball_type) return &life;
            return nullptr;
        }

        // A moved own birth's body, asked for by a restore (rollback_world::
        // destroy_body) when the replay will cross its applied tick: the
        // birth record of that lifetime is the applied tick's, and the body
        // is made again there (apply_own_lives).
        const own_life* moved_own_life(const std::string& entity, uint32_t birth_tick) const {
            for (const auto& life: core_.s.own_life_)
                if (!life.retired && life.moved() && life.entity == entity && life.applied == birth_tick
                        && own_life_current(life))
                    return &life;
            return nullptr;
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

        uint32_t current_tick() const { return core_.current_tick(); }

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
            if (name == core_.s.own_body_entity_) return;   // the body was there before this tick
            bmmo_physics_body_state state{};
            std::string error;
            if (!bmmo::physics::get_body_state(engine_->physics(), name.c_str(), state, error)) return;   // no body yet
            own_capture_.valid = true;
            own_capture_.tick = tick;
            own_capture_.entity = name;
            own_capture_.pose = ball->GetWorldMatrix();
            own_capture_.spawn = false;
            VxMatrix reset;
            bool have_reset = false;
            if (CKDataArray* level = engine_->data_array("CurrentLevel")) {
                if (level->GetElementValue(0, 3, &reset)) {
                    have_reset = true;
                    const float dx = own_capture_.pose[3][0] - reset[3][0];
                    const float dy = own_capture_.pose[3][1] - reset[3][1];
                    const float dz = own_capture_.pose[3][2] - reset[3][2];
                    own_capture_.spawn = (dx * dx + dy * dy + dz * dz) < 1e-6f;
                }
            }
            if (!own_capture_.spawn) return;
            const uint8_t join_order = static_cast<uint8_t>(core_.s.own_join_order_ < 0 ? 63 : core_.s.own_join_order_);
            // The birth cell (session/spawn_impulse.hpp): the ball is moved
            // off the resetpoint before anything else sees it - the kick
            // below, the pose the Physicalize event carries, this tick's step
            // - so every side creates the clone where this ball is.
            if (core_.s.spawn_spacing_ > 0.0f && have_reset && join_order % bmmo::session::kSpawnOffsets != 0) {
                const float right[3] = {reset[0][0], reset[0][1], reset[0][2]};
                const float dir[3] = {reset[2][0], reset[2][1], reset[2][2]};
                float offset[3];
                bmmo::session::spawn_offset(join_order, core_.s.spawn_spacing_, right, dir, offset);
                double target[3];
                for (int k = 0; k < 3; ++k) target[k] = state.position[k] + static_cast<double>(offset[k]);
                const float still[3] = {0.0f, 0.0f, 0.0f};
                if (!bmmo::physics::set_body_state(engine_->physics(), name.c_str(), target, state.rotation, still, still,
                                                   bmmo::physics::wake_mode::keep, true, error)) {
                    logf("spawn offset: %s", error.c_str());
                } else {
                    for (int k = 0; k < 3; ++k) own_capture_.pose[3][k] = static_cast<float>(target[k]);
                    logf("spawn offset (%.3f,%.3f,%.3f) for join order %u at tick %u", static_cast<double>(offset[0]),
                         static_cast<double>(offset[1]), static_cast<double>(offset[2]), join_order, tick);
                }
            }
            if (core_.s.spawn_impulse_ <= 0.0f) return;
            const uint32_t idx = bmmo::session::spawn_direction_index(core_.s.seed_, join_order, tick);
            if (!bmmo::physics::push_impulse(engine_->physics(), name.c_str(), bmmo::session::kSpawnDirectionTable[idx],
                        core_.s.spawn_impulse_, 0, error))
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
            if (core_.poll_hold().held) {
                wall_sleep(std::chrono::milliseconds(1));
                return;   // poll the network, step nothing
            }
            // Frames run from the assignment, and the server's world starts at
            // the same tick: a client that stepped between its anchor and the
            // assignment would be that many steps ahead of the world for good.
            // Inputs before the assignment queue up and core_.flush_inputs()
            // sends them when the tick base lands.
            const uint64_t next_frame = static_cast<uint64_t>(core_.s.frames_since_anchor_ + 1);
            if (args_.pause_at >= 0 && !paused_once_ && static_cast<int64_t>(current_tick()) >= args_.pause_at) {
                paused_once_ = true;
                logf("test pause: %d ms at tick %u", args_.pause_ms, current_tick());
                if (args_.stall_named) logf("stall: %d ms at tick %u (--stall-ms)", args_.pause_ms, current_tick());
                wall_sleep(std::chrono::milliseconds(args_.pause_ms));
            }
            // A SessionLead's slew (late-player design 2.4) moves the origin
            // before the schedule is read: one tick earlier per tick period
            // for a raise (two frames per slot), one later per ten for a
            // lower.
            if (core_.s.lead_slew_.active()) advance_lead_slew(next_frame);
            const double expected = std::chrono::duration<double>(wall_now(bmmo::sim::wall_site::pace_expected) - origin_).count() * bmmo::sim::kTickRate;
            // 33 frames behind is a stall, plus whatever a slew still has to
            // gain: the slew never runs the pacer more than a frame behind
            // (lead_slew::kRaiseMaxBehind), so a real stall still rebases.
            if (static_cast<double>(next_frame) + bmmo::session::lead_rebase_threshold(core_.s.lead_slew_.outstanding()) < expected) {
                // far behind (a long pause): restart the schedule instead of fast-forwarding
                origin_ = wall_now(bmmo::sim::wall_site::pace_rebase) - std::chrono::duration_cast<clock_type::duration>(
                    std::chrono::duration<double>(static_cast<double>(next_frame) / bmmo::sim::kTickRate));
                ++rebases_;
                // A slew in flight is not ended here: the ticks it moved are
                // stepped already, and it goes on moving the new schedule.
                // If the request below goes out, request_resync ends it and
                // acknowledges the whole delta first (the re-anchor at L_eff
                // applies all of it); if the limiter holds the request back,
                // the slew is all that still moves this client's lead.
                request_resync("tick schedule rebased");
            } else if (static_cast<double>(next_frame) > expected) {
                const auto due = origin_ + std::chrono::duration_cast<clock_type::duration>(
                    std::chrono::duration<double>(static_cast<double>(next_frame) / bmmo::sim::kTickRate));
                const auto now = wall_now(bmmo::sim::wall_site::pace_due);
                if (due > now) {
                    wall_sleep(std::min(due - now, clock_type::duration(std::chrono::milliseconds(2))));
                    return;   // poll the network again before the frame is due
                }
            }
            ++flow_steps_;
            frame();
        }

        // One step of the slew: the tick periods since the previous step and
        // how far the pacer is behind go to lead_slew::advance, and the origin
        // moves by what it returns (+1: a tick earlier, the clock runs ahead).
        // The whole delta moved: acknowledged.
        void advance_lead_slew(uint64_t next_frame) {
            const auto now = wall_now(bmmo::sim::wall_site::slew_step);
            const double wall = std::chrono::duration<double>(now - core_.s.lead_slew_clock_).count() * bmmo::sim::kTickRate;
            core_.s.lead_slew_clock_ = now;
            const double behind = std::chrono::duration<double>(now - origin_).count() * bmmo::sim::kTickRate
                                - static_cast<double>(next_frame);
            const int moved = core_.s.lead_slew_.advance(wall, behind);
            if (moved != 0)
                origin_ -= std::chrono::duration_cast<clock_type::duration>(
                    std::chrono::duration<double>(static_cast<double>(moved) / bmmo::sim::kTickRate));
            if (core_.s.lead_slew_.done()) finish_lead_slew("slew complete");
        }

        // --keep-lane: the record's keys of this frame with LEFT or RIGHT added
        // while the own ball is off the lane (arguments::keep_lane).  The
        // sideways axis is the world's z, the lane's normal on the arena; the
        // key that pushes the right way is the navigation leaf whose force,
        // taken through the camera reference the replica uses, points that
        // way - so a camera the record turned still steers towards the lane.
        const unsigned char* keep_lane(const unsigned char* keys) {
            std::memcpy(lane_keys_, keys, sizeof(lane_keys_));
            const bool driving = keys[kKeyUp] || keys[kKeyDown];
            if (!driving || !core_.s.own_physicalized_ || core_.s.own_body_entity_.empty() || !navigation_known_ || !core_.s.previous_cam_valid_)
                return lane_keys_;
            bmmo_physics_body_state body{};
            std::string error;
            if (!bmmo::physics::get_body_state(engine_->physics(), core_.s.own_body_entity_.c_str(), body, error)) return lane_keys_;
            const double error_z = body.position[2] - args_.keep_lane_z;
            const double a = error_z + kLaneDamping * static_cast<double>(body.linear[2]);
            if (std::fabs(a) < kLaneDeadband) return lane_keys_;
            const double push = a < 0.0 ? 1.0 : -1.0;   // the z direction the ball needs
            int best_key = -1;
            double best = 0.0;
            for (const auto& leaf: navigation_.leaves) {
                if (leaf.key == kKeyUp || leaf.key == kKeyDown || leaf.key < 0 || leaf.key >= 256) continue;
                const double z = leaf.direction.x * core_.s.previous_cam_[0][2] + leaf.direction.y * core_.s.previous_cam_[1][2]
                               + leaf.direction.z * core_.s.previous_cam_[2][2];
                if (z * push > best) { best = z * push; best_key = leaf.key; }
            }
            if (best_key >= 0) {
                lane_keys_[best_key] = 1;
                if (args_.trace && lane_steers_++ < 4)
                    logf("keep-lane: ball z=%.2f (lane %.2f) rate %.2f -> key %d", body.position[2], args_.keep_lane_z,
                         static_cast<double>(body.linear[2]), best_key);
            }
            return lane_keys_;
        }

        void frame() {
            std::string error;
            const int64_t f = core_.s.frames_since_anchor_ + 1;
            // Keys of this frame come from the recording (frame f of the record).
            const unsigned char* keyboard = nullptr;
            if (!record_.frames.empty() && f >= 0 && static_cast<size_t>(f) < record_.frames.size()) {
                keyboard = record_.frames[static_cast<size_t>(f)].keys.data();
                if (args_.keep_lane) keyboard = keep_lane(keyboard);
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
            const uint32_t frame_tick = core_.s.tick_base_ + static_cast<uint32_t>(std::max<int64_t>(f - 1, 0));
            // A held --start-sector: on the first frame the session is assigned
            // (tick base and lead known), stamp it `lead` ticks ahead like any
            // other sector this client enters.
            if (core_.s.start_sector_pending_ && core_.s.assigned_) {
                core_.s.start_sector_pending_ = false;
                hold_sector(args_.start_sector, frame_tick + sector_lead_ticks(), "--start-sector");
            }
            // This tick's events first - a relayed Sector event stamped for it
            // among them - then the union's pass, then the step: the order the
            // server's session_runner::step keeps.
            drain_due_events(frame_tick);
            // Our own events' echoes: a birth the server made later or never
            // is moved inside the history now, with the rewinds below still
            // to come in this frame.
            process_own_echoes();
            // The sector resets stay neutralized (the level-reset one once the
            // sector is up: this client has resolved a mechanism of its own).
            for (const auto& chain: reset_guard_.apply(engine_->context(), core_.s.mechanism_tracking_.resolved() > 0))
                logf("the sector reset in %s (Execute Script -> Gameplay_SectorManager) is neutralized", chain.c_str());
            // After the late events, so a birth and an input correction in the
            // same frame leave the world replayed from the earlier of the two - and
            // BEFORE the union's pass below: the pass sets next-frame flags on
            // Gameplay_SectorManager, which the restore (the manager travels
            // with the records now) would write back to the anchor's and lose,
            // and the replay ends one tick short of re-issuing it.  The retail
            // client has this order already.
            replay_corrected_inputs();
            core_.retry_owed_resync();
            maybe_force_rollback();
            // The body changes stamped for this very tick, the own beam among
            // them: after the rewinds above, so none of them undoes one, and
            // before the union's pass and the step, where the server applies
            // them, in its order.
            apply_held_lifecycle(frame_tick);
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
                const auto pass = core_.s.sectors_.tick(frame_tick, manager && manager->IsActive());
                if (pass) {
                    core_.s.sector_cell_ = pass.sector;   // the detect block below must not take our own write for a crossing
                    start_sector(pass.sector);
                    record_manager_activation(frame_tick, true);
                    logf("sector +%d at tick %u (player %u, stamped %u%s), running: %s", pass.sector, frame_tick,
                         pass.player, pass.stamped_tick, frame_tick > pass.stamped_tick ? ", held by the pacer" : "",
                         [this] { std::string s; for (int x: core_.s.sectors_.active()) s += std::to_string(x) + " "; return s; }().c_str());
                }
            }
            // A rewind above replayed up to the previous tick and left every
            // mirror's navigation input at the frame it last fed there; this
            // tick's step is driven by the frame recorded as applied for it
            // (remote.applied, chosen at the end of the previous frame), or
            // the record and the step disagree and the relayed frame for this
            // tick compares equal to a prediction the step never ran on.
            for (auto& [id, remote]: core_.s.remotes_) {
                if (!remote.physicalized || !remote.navigation) continue;
                const bool active = (remote.applied.flags & bmmo::session::INPUT_FLAG_NAV_ACTIVE) != 0;
                std::string drive_error;
                if (!bmmo::physics::navigation_input(engine_->physics(), remote.entity.c_str(), remote.applied.keys,
                                                     remote.applied.cam_right, remote.applied.cam_up, remote.applied.cam_dir,
                                                     active, drive_error))
                    logf("remote navigation input %s: %s", remote.entity.c_str(), drive_error.c_str());
            }
            // The own ball's Physicalize is captured inside this tick's
            // Simulate(), before the PSIs (capture_own_physicalize).  Armed
            // here, after the rewinds above: a replayed step runs the
            // PreSimulate pass too, and only the live tick may capture.
            // A tick a SessionLead forced (late-player design 2.4): the server
            // applies `held` here and drops our frame as stale, so the own
            // ball is driven with it the way a replay drives it - polling off,
            // the frame through nav_input - and our keys wait until E.  After
            // the rewinds above, whose replays switch the polling back on.
            forced_own forced_frame;
            const bool forced = [&] {
                while (!core_.s.forced_own_.empty() && static_cast<int32_t>(core_.s.forced_own_.begin()->first - frame_tick) < 0)
                    core_.s.forced_own_.erase(core_.s.forced_own_.begin());
                const auto it = core_.s.forced_own_.find(frame_tick);
                if (it == core_.s.forced_own_.end()) return false;
                forced_frame = it->second;
                core_.s.forced_own_.erase(it);
                return true;
            }();
            std::string forced_entity;
            if (forced && own_navigation_) {
                const auto& held = forced_frame.frame;
                forced_entity = own_nav_entity_;
                std::string force_error;
                if (!bmmo::physics::navigation_poll(engine_->physics(), forced_entity.c_str(), false, own_key_codes_,
                                                    own_key_blocks_, own_key_count_, force_error)
                        || !bmmo::physics::navigation_input(engine_->physics(), forced_entity.c_str(), held.keys,
                                                            held.cam_right, held.cam_up, held.cam_dir,
                                                            (held.flags & bmmo::session::INPUT_FLAG_NAV_ACTIVE) != 0,
                                                            force_error))
                    if (core_.s.lead_force_logs_++ < 8)   // the ball may be unphysicalized for the whole range
                        logf("lead: forced own input at tick %u: %s", frame_tick, force_error.c_str());
            }
            live_tick_serial_ = ++live_ticks_;
            arm_own_physicalize_capture(live_tick_serial_, frame_tick);
            const bool ticked = engine_->tick(error);
            live_tick_serial_ = 0;
            if (!forced_entity.empty()) {
                std::string force_error;
                if (!bmmo::physics::navigation_poll(engine_->physics(), forced_entity.c_str(), true, own_key_codes_,
                                                    own_key_blocks_, own_key_count_, force_error))
                    if (core_.s.lead_force_logs_++ < 8)
                        logf("lead: own polling after the forced tick %u: %s", frame_tick, force_error.c_str());
            }
            if (!ticked) { logf("tick failed: %s", error.c_str()); ended_ = true; return; }
            core_.s.frames_since_anchor_ = f;
            // An activation of Gameplay_SectorManager this frame's scripts asked
            // for (Init Ingame's Execute Script at the level start, Event_handler):
            // pending as next-frame flags now, consumed by the next frame.  A
            // pass of ours never shows here - its flags were consumed by this
            // frame.  Recorded for the replay like a pass (plan doc 10).
            if (sector_manager_id_) {
                if (auto* manager = CKBehavior::Cast(engine_->context()->GetObject(sector_manager_id_));
                        manager && (manager->GetFlags() & CKBEHAVIOR_ACTIVATENEXTFRAME))
                    record_manager_activation(frame_tick + 1, (manager->GetFlags() & CKBEHAVIOR_RESETNEXTFRAME) != 0);
            }
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
                    for (auto& [id, remote]: core_.s.remotes_) {
                        const bool navigated = remote.navigation;
                        attach_remote_navigation(id, remote);
                        // The ticks before this one ran it undriven: a replay
                        // across them must not start driving it.
                        if (!navigated && remote.navigation && static_cast<int32_t>(tick - remote.driven_from) > 0)
                            remote.driven_from = tick;
                    }
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
                    if (bmmo::physics::set_body_group(physics, ball_name.c_str(), core_.s.own_group_.c_str(), error)) own_group_set_ = true;
                    else logf("own ball group: %s", error.c_str());
                }
                if (args_.rollback && navigation_known_ && (!own_navigation_ || own_nav_entity_ != ball_name))
                    attach_own_navigation(ball_name, ball_type);
                bmmo::session::ball_pose pose;
                pose.tick = tick;
                for (int k = 0; k < 3; ++k) { pose.position[k] = own.position[k]; pose.linear[k] = own.linear[k]; pose.angular[k] = own.angular[k]; }
                for (int k = 0; k < 4; ++k) pose.rotation[k] = own.rotation[k];
                core_.s.corrector_.record(pose);
            } else {
                own_group_set_ = false;
            }
            core_.s.own_physicalized_ = physicalized;
            core_.s.own_body_entity_ = physicalized ? ball_name : std::string();

            // Mechanism states after this tick (the legacy blending path;
            // with the rollback engine on, the engine keeps its own records).
            for (const auto& name: core_.s.mechanism_tracking_.tracked_entities()) {
                bmmo_physics_body_state local{};
                if (!bmmo::physics::get_body_state(physics, name.c_str(), local, error)) continue;
                bmmo::session::ball_pose pose;
                pose.tick = tick;
                for (int k = 0; k < 3; ++k) { pose.position[k] = local.position[k]; pose.linear[k] = local.linear[k]; pose.angular[k] = local.angular[k]; }
                for (int k = 0; k < 4; ++k) pose.rotation[k] = local.rotation[k];
                core_.s.mechanism_correctors_[name].record(pose);
            }
            for (auto& [id, remote]: core_.s.remotes_) {
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
                if (kind == "recipe") {
                    // What this frame's Physicalize made the body with (engine
                    // change #19): kept by tick, so a rewind anchored before
                    // this tick can make the body again at it (recreate_body).
                    local_birth birth;
                    if (parse_recipe_event(name, birth)) {
                        local_births_[tick].push_back(std::move(birth));
                        while (!local_births_.empty() && local_births_.begin()->first + 4096 < tick)
                            local_births_.erase(local_births_.begin());
                    } else if (recipe_parse_logs_++ < 4) {
                        logf("birth recipe at tick %u could not be parsed: %.120s", tick, name.c_str());
                    }
                    continue;
                }
                if (kind == "created" || kind == "deleted") {
                    // The body behind a tracked name changed: a new lifetime for
                    // it, and the mechanism registry has to ask the world again.
                    if (!name.empty()) {
                        ++core_.s.generations_[name];
                        core_.s.mechanism_tracking_.mark_dirty();
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
            if (core_.s.mechanism_tracking_.due(tick))
                core_.s.mechanism_tracking_.resolve(tick, [physics](const char* entity, bmmo_physics_body_state& out) {
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
            const float (*basis)[3] = core_.s.previous_cam_valid_ ? core_.s.previous_cam_ : cam;
            for (int k = 0; k < 3; ++k) { input.cam_right[k] = basis[0][k]; input.cam_up[k] = basis[1][k]; input.cam_dir[k] = basis[2][k]; }
            if (cam_valid) { std::memcpy(core_.s.previous_cam_, cam, sizeof(cam)); core_.s.previous_cam_valid_ = true; }
            input.ball_type = static_cast<uint8_t>(ball_type < 0 ? 0 : ball_type);
            const bool nav_active = navigation_known_ && navigation_active();
            input.flags = static_cast<uint8_t>((physicalized ? bmmo::session::INPUT_FLAG_PHYSICALIZED : 0)
                        | (nav_active ? bmmo::session::INPUT_FLAG_NAV_ACTIVE : 0));
            // A forced tick plays the server's frame (design 2.4): recorded,
            // journaled SUBSTITUTED and sent as such - the server drops it as
            // stale, and the record holds what the server applied, so its
            // notice, when it comes, changes nothing.
            if (forced) {
                input = forced_frame.frame;
                ++core_.s.lead_forced_played_;
                if (forced_frame.count) {
                    if (held_tick_is_base(tick)) ++core_.s.subst_base_;
                    else { ++core_.s.subst_recv_; ++core_.s.subst_same_; }
                }
            }
            if (args_.trace && (input.keys != last_keys_ || nav_active != last_nav_active_))
                logf("input edge at tick %u keys=%u flags=%u", tick, input.keys, input.flags);
            last_keys_ = input.keys;
            last_nav_active_ = nav_active;
            if (own_navigation_) {
                if (!bmmo::physics::navigation_input(physics, own_nav_entity_.c_str(), 0, basis[0], basis[1], basis[2], false, error))
                    logf("own navigation rows: %s", error.c_str());
                zero_retail_forces();
            }
            core_.s.input_history_.emplace_back(tick, input);
            // The frame is finished: journal it before it goes on the wire, so
            // the box holds what this client played even when the send is lost.
            if (core_.journal().journalling())
                core_.journal().writer().input(tick, core_.s.own_id_, input,
                               forced ? bmmo::session::JOURNAL_INPUT_SUBSTITUTED : bmmo::session::JOURNAL_INPUT_FRESH);
            if (args_.rollback) {
                core_.s.own_inputs_[tick] = input;
                while (core_.s.own_inputs_.size() > 128) core_.s.own_inputs_.erase(core_.s.own_inputs_.begin());
                bmmo::session::rollback_tracked tracked;
                std::map<std::string, bmmo::session::input_frame> applied;
                if (physicalized && own_navigation_) {
                    tracked.own_entity = own_nav_entity_;
                    tracked.own_polls = true;
                    applied[own_nav_entity_] = input;
                }
                for (const auto& [id, remote]: core_.s.remotes_)
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
                tracked.generations = core_.s.generations_;
                for (CK_ID id: bmmo::game::script_replay::resets_pending(engine_->context(), scripts_)) {
                    script_reset_ticks_[id] = tick;
                    if (script_reset_logs_++ < 12) {
                        auto* beh = CKBehavior::Cast(engine_->context()->GetObject(id));
                        logf("script replay: %s is told to reset at tick %u; a rewind anchored earlier keeps it live",
                             beh && beh->GetName() ? beh->GetName() : "?", tick);
                    }
                }
                core_.s.rollback_.record(rollback_world(), tick, tracked, applied);
                dump_savestate(tick);
            }
            // A notice that named this tick before it was simulated: the
            // record just made holds what we played, the server applied
            // something else (late-player design 1.3).
            take_held_ahead(tick);
            if (core_.s.assigned_) {
                while (core_.s.input_history_.size() > bmmo::session::MAX_INPUT_FRAMES) core_.s.input_history_.pop_front();
                bmmo::session_input_msg msg;
                msg.session = core_.s.session_;
                msg.first_tick = core_.s.input_history_.front().first;
                for (const auto& [t, fr]: core_.s.input_history_) msg.frames.push_back(fr);
                core_.remember_sent(msg);
                msg.serialize();
                send_bytes(msg.raw.str(), k_nSteamNetworkingSend_UnreliableNoDelay);
                ++core_.s.inputs_sent_;
                // The unreliable copies of our recent lifecycle events, one per
                // frame (send_event; protocol 6).
                core_.s.own_event_repeats_.send_due([this](uint32_t, const std::string& bytes) {
                    send_bytes(bytes, k_nSteamNetworkingSend_UnreliableNoDelay);
                });
            } else if (core_.s.input_history_.size() > 660) {
                core_.s.input_history_.pop_front();
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
                if (parameters->GetElementValue(0, 1, &sector) && (sector != core_.s.sector_cell_ || resetpoint_moved)) {
                    core_.s.sector_cell_ = sector;
                    if (sector > 0 && !core_.s.sectors_requested_.count(sector)) {
                        if (checkpoint_held_) {
                            hold_sector(sector, tick + 1 + sector_lead_ticks(), "checkpoint");
                        } else {
                            core_.s.sectors_requested_.insert(sector);
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
                if (physicalized) apply_blend(ball_name, core_.s.corrector_);
                for (auto& [name, corrector]: core_.s.mechanism_correctors_) apply_blend(name, corrector);
                for (auto& [id, remote]: core_.s.remotes_)
                    if (remote.physicalized && remote.navigation) apply_blend(remote.entity, remote.corrector);
            }
            // Next tick's input of every predicted remote ball: the frame the
            // server applied at that tick when the relay has it already (a
            // client running behind the relay), else the last relayed frame.
            for (auto& [id, remote]: core_.s.remotes_) {
                if (!remote.physicalized || !remote.navigation) continue;
                remote.applied = live_drive(remote, tick + 1);
                const bool active = (remote.applied.flags & bmmo::session::INPUT_FLAG_NAV_ACTIVE) != 0;
                if (!bmmo::physics::navigation_input(physics, remote.entity.c_str(), remote.applied.keys, remote.applied.cam_right,
                                                     remote.applied.cam_up, remote.applied.cam_dir, active, error))
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
            if (args_.rollback && !core_.s.rollback_.amend_record(rollback_world(), tick)) {
                if (core_.s.amend_failures_++ == 0)
                    logf("amend_record found no history entry for tick %u (the tick was dropped)", tick);
            }
            if (!args_.trace_body.empty() && tick >= args_.trace_from && tick <= args_.trace_until)
                logf("body %s after live tick %u: %s", args_.trace_body.c_str(), tick,
                     bmmo::physics::describe_body(physics, args_.trace_body.c_str()).c_str());
            if (args_.trace_clock_until && tick >= args_.trace_clock_from && tick <= args_.trace_clock_until)
                logf("clock after live tick %u: %s", tick, describe_clock().c_str());
            // --trace: what pushes the own ball - the navigation replica's
            // leaves, every controller on its core, the mechanism force blocks
            // whose target it is - logged whenever that changes.
            if (args_.trace && core_.s.own_physicalized_ && own_navigation_) {
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
            if (core_.s.assigned_) report_window(tick);
            // The box's own view of this frame, last: the fingerprint of the
            // world the corrections above have finished with, and the bodies
            // when a checkpoint is due.
            if (core_.journal().journalling()) {
                bmmo::physics::world_hash hash;
                std::string hash_error;
                if (bmmo::physics::capture_world_hash(physics, hash, hash_error))
                    core_.journal().writer().tick(tick, hash, core_.journal().ms());
                if (core_.journal().checkpoint_due(tick)) {
                    CK3dEntity* own = current_ball();
                    core_.journal().local_checkpoint(tick, own && own->GetName() ? own->GetName() : std::string());
                }
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
            // The birth as the history keeps it: what the server makes when
            // its echo says it applied the event later (record_own_birth).
            record_own_birth(event, ball_name);
            own_group_set_ = false;
            if (args_.trace) trace_frames_ = 12;
            logf("own ball %s physicalized at tick %u (type %d%s) pos=%a,%a,%a", ball_name.c_str(), tick, ball_type,
                 spawn ? ", spawn" : "", static_cast<double>(event.position[0]), static_cast<double>(event.position[1]),
                 static_cast<double>(event.position[2]));
        }

        // Own events go out through the core (shared-core design E3).
        void send_event(bmmo::session_event_msg& event) { core_.send_event(event); }

        // --hold-event: true when this event is the one to hold back.  Held
        // here, before the reliable copy and before the repeat queue, so the
        // one gate covers every path the event leaves by; the bytes go out
        // whole at the release (release_held_events), the repeats after them.
        bool hold_event(const bmmo::session_event_msg& event) {
            if (args_.hold_event_type < 0 || static_cast<int>(event.type) != args_.hold_event_type) return false;
            if (++hold_event_seen_ != args_.hold_event_nth) return false;
            if (args_.hold_event_drop) {
                logf("hold-event: own %s #%u stamped tick %u dropped at tick %u: it never leaves this client",
                     event_type_name(args_.hold_event_type), hold_event_seen_, event.tick, current_tick());
                core_.journal().note("hold-event: dropped " + std::string(event_type_name(args_.hold_event_type)) + " stamped "
                             + std::to_string(event.tick));
                return true;
            }
            held_events_.push_back(held_event{event.raw.str(), event.tick, current_tick(),
                                              wall_now(bmmo::sim::wall_site::hold_event_stamp) + std::chrono::milliseconds(args_.hold_event_ms)});
            logf("hold-event: own %s #%u stamped tick %u held at tick %u for %d ms",
                 event_type_name(args_.hold_event_type), hold_event_seen_, event.tick, current_tick(), args_.hold_event_ms);
            core_.journal().note("hold-event: held " + std::string(event_type_name(args_.hold_event_type)) + " stamped "
                         + std::to_string(event.tick) + " for " + std::to_string(args_.hold_event_ms) + " ms");
            return true;
        }

        // Called on every pass of the main loop, so a release is late by one
        // poll at most, not by a frame.
        void release_held_events() {
            if (held_events_.empty()) return;
            const auto now = wall_now(bmmo::sim::wall_site::hold_event_release);
            for (auto it = held_events_.begin(); it != held_events_.end();) {
                if (now < it->release) { ++it; continue; }
                if (core_.s.phase_ != phase::running) {
                    // the session's end clears the list (discard_held_events), so this is a safety net;
                    // said aloud either way, so a hold never ends silently.
                    logf("hold-event: own %s stamped tick %u (held at tick %u) discarded: no session running",
                         event_type_name(args_.hold_event_type), it->stamp, it->held_at);
                    it = held_events_.erase(it);
                    continue;
                }
                if (!core_.s.assigned_) { ++it; continue; }   // a session but no tick base yet: wait for it
                core_.release_event(it->bytes);
                logf("hold-event: own %s stamped tick %u (held at tick %u) released at tick %u",
                     event_type_name(args_.hold_event_type), it->stamp, it->held_at, current_tick());
                core_.journal().note("hold-event: released stamped " + std::to_string(it->stamp));
                it = held_events_.erase(it);
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
            if (core_.journal().journalling()) {
                // The full flag rides along: a full snapshot is the one that
                // carries the mechanism name dictionary, and a reader has no
                // other way to tell which received rows hold it.
                const uint8_t flags = static_cast<uint8_t>(bmmo::session::JOURNAL_CHECKPOINT_RECEIVED
                    | (snapshot.full ? bmmo::session::JOURNAL_CHECKPOINT_FULL : 0));
                core_.journal().writer().checkpoint(snapshot.tick, flags, snapshot.bodies);
            }
            if (core_.s.have_snapshot_ && snapshot.tick <= core_.s.last_snapshot_tick_ && !snapshot.full) { ++core_.s.snapshots_stale_; return; }
            // (Whether the server had our input for the tick it simulated is
            // read from the relay header's `acked` now, every tick and with
            // snapshots off too: note_held_section.)
            std::string error;
            CKIpionManager* physics = engine_->physics();
            CK3dEntity* ball = current_ball();
            const std::string ball_name = ball && ball->GetName() ? ball->GetName() : "";
            if (core_.s.resync_pending_) {
                if (!snapshot.full) return;
                core_.s.have_snapshot_ = true;
                core_.s.last_snapshot_tick_ = snapshot.tick;
                ++core_.s.snapshots_applied_;
                // A hard world overwrite: the records collected while we waited
                // for this snapshot describe the world it is about to replace.
                core_.s.rollback_.invalidate_history();
                // ...and so does a replay a correction of this poll asked
                // for: its anchor is gone with the history, and a refused own
                // replay would answer this resync with another.
                drop_pending_rewind();
                // The dictionary and the newest rows first, then one resolve
                // pass - this is the moment the registry knows the most about a
                // world that has just been rebuilt (design 9.25).
                for (const auto& body: snapshot.bodies)
                    if (body.kind == bmmo::session::body_kind::Mechanism) {
                        if (!body.name.empty()) core_.s.mechanism_tracking_.note_name(body.owner, body.name);
                        core_.s.mechanism_tracking_.note_row(snapshot.tick, body);
                    }
                core_.s.mechanism_tracking_.mark_dirty();
                core_.s.mechanism_tracking_.resolve(snapshot.tick, [physics](const char* entity, bmmo_physics_body_state& out) {
                    std::string probe_error;
                    return bmmo::physics::get_body_state(physics, entity, out, probe_error);
                });
                std::string mechanism_entity;
                for (const auto& body: snapshot.bodies) {
                    const bool wake = (body.flags & bmmo::session::BODY_FLAG_SIMULATED) != 0;
                    const char* target = nullptr;
                    if (body.kind == bmmo::session::body_kind::Ball) {
                        if (body.owner == core_.s.own_id_) {
                            if (!core_.s.own_physicalized_ || ball_name.empty()) continue;
                            target = ball_name.c_str();
                        } else {
                            auto it = core_.s.remotes_.find(body.owner);
                            if (it == core_.s.remotes_.end() || !it->second.physicalized) continue;
                            target = it->second.entity.c_str();
                        }
                    } else {
                        mechanism_entity = core_.s.mechanism_tracking_.entity_of(body.owner);
                        if (mechanism_entity.empty()) continue;   // another sector's instance
                        target = mechanism_entity.c_str();
                    }
                    bmmo::physics::set_body_state(physics, target, body.position, body.rotation, body.linear, body.angular,
                                                  body.owner == core_.s.own_id_ ? true : wake, error);
                }
                core_.s.resync_pending_ = false;
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
                const uint32_t first_record = core_.s.frames_since_anchor_ >= 1 ? current_tick() + 1 : core_.s.tick_base_;
                if (static_cast<int32_t>(snapshot.tick - core_.s.held_floor_) > 0) core_.s.held_floor_ = snapshot.tick;
                if (static_cast<int32_t>(first_record - core_.s.held_floor_) > 0) core_.s.held_floor_ = first_record;
                ++core_.s.resyncs_done_;
                logf("resync applied from the full snapshot of tick %u (%zu bodies)", snapshot.tick, snapshot.bodies.size());
                core_.journal().body_correction(4, {}, snapshot.tick, 0.0, nullptr);
                core_.journal().note("resync: rebuilt from the full snapshot of tick " + std::to_string(snapshot.tick)
                             + " (" + std::to_string(snapshot.bodies.size()) + " bodies)");
                return;
            }
            core_.s.have_snapshot_ = true;
            core_.s.last_snapshot_tick_ = snapshot.tick;
            ++core_.s.snapshots_applied_;
            if (args_.rollback) {
                for (const auto& body: snapshot.bodies)
                    if (body.kind == bmmo::session::body_kind::Mechanism) {
                        if (snapshot.full && !body.name.empty()) core_.s.mechanism_tracking_.note_name(body.owner, body.name);
                        core_.s.mechanism_tracking_.note_row(snapshot.tick, body);
                    }
                auto entity_of = [&](const bmmo::session::body_state& body) -> std::string {
                    if (body.kind == bmmo::session::body_kind::Ball) {
                        if (body.owner == core_.s.own_id_) return core_.s.own_physicalized_ && own_navigation_ ? own_nav_entity_ : std::string();
                        auto it = core_.s.remotes_.find(body.owner);
                        if (it == core_.s.remotes_.end() || !it->second.physicalized || !it->second.navigation) return {};
                        return it->second.entity;
                    }
                    // Design 9.25: the resolved local body, "" when this client
                    // has none (another sector) or the identity guard refused
                    // the pick - the mod does exactly the same.
                    return core_.s.mechanism_tracking_.entity_of(body.owner);
                };
                // Only an exact hit counts, like the mod: the engine feeds a
                // relayed frame instead of the recorded prediction when this
                // succeeds, and a frame from an earlier tick would replay a key
                // edge the tick did not have (findings/A5 section 3.1 item 5).
                auto input_at = [this](const std::string& entity, uint32_t tick, bmmo::session::input_frame& out) {
                    return recorded_input_at(entity, tick, out);
                };
                const live_cells_guard keep_cells(*this);
                const bool rolled = core_.s.rollback_.on_snapshot(rollback_world(), snapshot, current_tick(), entity_of, input_at);
                if (rolled) {
                    ++core_.s.snapshot_rollbacks_;
                    core_.s.snapshot_rollback_depth_ += current_tick() - snapshot.tick;
                    // The rows were the server's world at the anchor, our own
                    // ball's among them when it has one here: that world ran on
                    // the frames the server substituted, and the replay after it
                    // fed our amended records.
                    bool own_row = false;
                    for (const auto& body: snapshot.bodies)
                        if (body.kind == bmmo::session::body_kind::Ball && body.owner == core_.s.own_id_ && !entity_of(body).empty())
                            own_row = true;
                    drop_carried_own_replay(snapshot.tick, own_row);
                }
                if (rolled && args_.trace && rollback_diag_++ < 16)
                    logf("after rollback: movable=%s", bmmo::physics::describe_movable_objects(physics).c_str());
                return;
            }
            for (const auto& body: snapshot.bodies) {
                if (body.kind == bmmo::session::body_kind::Ball) {
                    if (body.owner == core_.s.own_id_) {
                        if (!core_.s.own_physicalized_ || ball_name.empty()) continue;
                        bmmo::session::ball_pose pose;
                        pose.tick = snapshot.tick;
                        for (int k = 0; k < 3; ++k) { pose.position[k] = body.position[k]; pose.linear[k] = body.linear[k]; pose.angular[k] = body.angular[k]; }
                        for (int k = 0; k < 4; ++k) pose.rotation[k] = body.rotation[k];
                        const auto step = core_.s.corrector_.compare(pose);
                        const double err = core_.s.corrector_.stats().last_error;
                        if (step.action == bmmo::session::correction_step::kind::hard) {
                            if (++core_.s.consecutive_hard_ >= 3) request_resync("3 hard corrections in a row");
                            logf("hard correction of own ball for tick %u (error %.4f m, local tick %u)", snapshot.tick, err, current_tick());
                            // Before the set, or the recorded local pose would
                            // be the server's own.
                            core_.journal().body_correction(2, ball_name, snapshot.tick, err, body.position);
                            if (args_.correct && !bmmo::physics::set_body_state(physics, ball_name.c_str(), step.target.position,
                                    step.target.rotation, step.target.linear, step.target.angular, true, error))
                                logf("hard set: %s", error.c_str());
                        } else if (step.action == bmmo::session::correction_step::kind::blend) {
                            core_.s.consecutive_hard_ = 0;
                            logf("blend correction of own ball for tick %u (error %.4f m, local tick %u)", snapshot.tick, err, current_tick());
                            core_.journal().body_correction(3, ball_name, snapshot.tick, err, body.position);
                        } else if (args_.trace && err > 0.002) {
                            logf("own ball error %.4f m at tick %u", err, snapshot.tick);
                        }
                        continue;
                    }
                    auto it = core_.s.remotes_.find(body.owner);
                    if (it == core_.s.remotes_.end() || !it->second.physicalized) continue;
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
                            core_.journal().body_correction(2, remote.entity, snapshot.tick,
                                                    remote.corrector.stats().last_error, body.position);
                            if (args_.correct) bmmo::physics::set_body_state(physics, remote.entity.c_str(), step.target.position,
                                                                             step.target.rotation, step.target.linear, step.target.angular, wake, error);
                        } else if (step.action == bmmo::session::correction_step::kind::blend) {
                            ++remote.blends;
                            logf("blend correction of remote %s for tick %u (error %.4f m)", remote.entity.c_str(), snapshot.tick,
                                 remote.corrector.stats().last_error);
                            core_.journal().body_correction(3, remote.entity, snapshot.tick,
                                                    remote.corrector.stats().last_error, body.position);
                        }
                        continue;
                    }
                    if (!bmmo::physics::set_body_state(physics, it->second.entity.c_str(), body.position, body.rotation,
                                                       body.linear, body.angular, wake, error))
                        logf("remote %u: %s", body.owner, error.c_str());
                    else ++core_.s.remote_writes_;
                    continue;
                }
                // The legacy blending path (--no-rollback): the row corrects the
                // locally simulated mechanism through a body_corrector instead.
                if (snapshot.full && !body.name.empty()) core_.s.mechanism_tracking_.note_name(body.owner, body.name);
                core_.s.mechanism_tracking_.note_row(snapshot.tick, body);
                const std::string entity = core_.s.mechanism_tracking_.entity_of(body.owner);
                if (entity.empty()) continue;
                const bool wake = (body.flags & bmmo::session::BODY_FLAG_SIMULATED) != 0;
                bmmo_physics_body_state local{};
                if (!bmmo::physics::get_body_state(physics, entity.c_str(), local, error)) continue;
                bmmo::session::ball_pose pose;
                pose.tick = snapshot.tick;
                for (int k = 0; k < 3; ++k) { pose.position[k] = body.position[k]; pose.linear[k] = body.linear[k]; pose.angular[k] = body.angular[k]; }
                for (int k = 0; k < 4; ++k) pose.rotation[k] = body.rotation[k];
                auto& corrector = core_.s.mechanism_correctors_[entity];
                const auto step = corrector.compare(pose);
                if (step.action == bmmo::session::correction_step::kind::hard) {
                    ++core_.s.mechanism_hard_;
                    logf("hard correction of %s for tick %u (error %.4f m)", entity.c_str(), snapshot.tick, corrector.stats().last_error);
                    core_.journal().body_correction(2, entity, snapshot.tick, corrector.stats().last_error, body.position);
                    if (args_.correct) bmmo::physics::set_body_state(physics, entity.c_str(), step.target.position, step.target.rotation,
                                                                     step.target.linear, step.target.angular, wake, error);
                } else if (step.action == bmmo::session::correction_step::kind::blend) {
                    ++core_.s.mechanism_blends_;
                    logf("blend correction of %s for tick %u (error %.4f m)", entity.c_str(), snapshot.tick, corrector.stats().last_error);
                    core_.journal().body_correction(3, entity, snapshot.tick, corrector.stats().last_error, body.position);
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

        // The 99th percentile of the relayed frames' arrival lag, in ticks.
        uint32_t relay_lag_p99() const {
            uint64_t total = 0;
            for (uint32_t n: core_.s.relay_lag_hist_) total += n;
            if (!total) return 0;
            uint64_t seen = 0;
            for (uint32_t b = 0; b < kRelayLagBuckets; ++b) {
                seen += core_.s.relay_lag_hist_[b];
                if (seen * 100 >= total * 99) return b;
            }
            return kRelayLagBuckets - 1;
        }

        // Protocol 7 (late-player design 1.3): the held section of a relayed
        // datagram names our own ticks the server substituted and the frame
        // it applied there, and this is where our ball is put back on the
        // server's input.  A datagram numbered from another base (a
        // SessionAssign processed late here, or notices from before it) says
        // nothing about this numbering: its records are only counted as such,
        // once per (epoch, tick).
        //
        // Of our own epoch's records, each first arrival of a tick is
        // resolved to the frame the server applied (inline, the default
        // frame, or the bytes we sent for `source`, sent_inputs_), stored as
        // our input for that tick and journaled as SUBSTITUTED.  Then:
        //  - base: no rewind could ever reach it - without --rollback, while
        //    a resync waits for its full snapshot, or at or below held_floor_
        //    (the base tick, whose anchor precedes the numbering, and every
        //    tick a resync's full snapshot overwrote: the server's frame is in
        //    that snapshot already).  Never a resync: a resync's own first
        //    ticks held again would answer it with another one, for ever.
        //  - late: the anchor a rewind would use (tick - 1) is past the reach
        //    or out of the history, or the source is no longer in the sent
        //    ring.  The ball cannot be replayed onto the server's input, so
        //    this player (and only this player) resyncs.
        //  - recv: everything else.  A tick not simulated yet waits for its
        //    live frame (take_held_ahead); one simulated is compared with the
        //    frame its record holds (correct_own_tick).
        // recv + late + base is what the server's S2 lines count for this
        // epoch, and the header's held_total is checked against it
        // (check_held_complete).
        void note_held_section(const bmmo::session_remote_input_msg& msg) {
            if (!core_.s.assigned_) return;
            // Did the server have our frame for the tick it just simulated, or
            // substitute one?  Once per server tick, from the header, so it
            // works with snapshots off (design 1.3; it used to need a
            // snapshot's acked_input_tick).  From a datagram of any epoch -
            // its tick and acked are the server's own numbers, the one thing
            // another epoch's datagram still says - but only for the ticks of
            // our numbering.  `lead` is how far our tick runs ahead of the
            // server's newest.
            const bool newer_ack = !core_.s.acked_header_known_ || static_cast<int32_t>(msg.tick - core_.s.acked_header_tick_) > 0;
            if (newer_ack) {
                core_.s.acked_header_known_ = true;
                core_.s.acked_header_tick_ = msg.tick;
                core_.s.header_acked_ = msg.acked;
            }
            if (newer_ack && static_cast<int32_t>(msg.tick - core_.s.tick_base_) >= 0) {
                ++core_.s.input_reports_;
                if (static_cast<int32_t>(msg.tick - msg.acked) > 0) {
                    const uint32_t behind = msg.tick - msg.acked;
                    ++core_.s.input_starved_;
                    if (behind > core_.s.worst_starved_) core_.s.worst_starved_ = behind;
                }
                core_.s.lead_sum_ += static_cast<int32_t>(current_tick() - msg.tick) > 0 ? current_tick() - msg.tick : 0;
                ++core_.s.lead_count_;
            }
            if (msg.epoch != core_.s.tick_base_) {
                for (const auto& record: msg.held)
                    if (core_.s.held_stale_seen_.first(msg.epoch, msg.held_tick(record))) ++core_.s.subst_stale_epoch_;
                return;
            }
            if (!held_header_current() || static_cast<int32_t>(msg.tick - core_.s.held_header_tick_) >= 0) {
                core_.s.held_header_known_ = true;
                core_.s.held_header_epoch_ = msg.epoch;
                core_.s.held_header_tick_ = msg.tick;
                core_.s.held_header_acked_ = msg.acked;
                core_.s.held_header_total_ = msg.held_total;
            }
            for (const auto& record: msg.held) {
                const uint32_t tick = msg.held_tick(record);
                if (static_cast<int32_t>(tick - core_.s.tick_base_) < 0) continue;   // before our numbering: never noticed
                if (!core_.s.held_seen_.first(core_.s.own_id_, tick)) { ++core_.s.subst_dup_; continue; }
                learn_held_tick(tick);
                bmmo::session::input_frame frame{};
                if (!resolve_held_frame(record, frame)) {
                    if (held_tick_is_base(tick)) { ++core_.s.subst_base_; continue; }
                    ++core_.s.subst_late_;
                    if (core_.s.subst_unresolved_logs_++ < 8)
                        logf("held notice for own tick %u names source %u, which the sent ring no longer holds", tick,
                             record.source);
                    request_resync("a substituted own frame could not be resolved", true);
                    continue;
                }
                settle_held_tick(tick, frame);
            }
            check_held_complete(msg);
        }

        // A held tick no rewind could ever reach (the base rule of
        // note_held_section): without --rollback, while a resync waits for
        // its full snapshot, or at or below held_floor_.
        bool held_tick_is_base(uint32_t tick) const {
            return !args_.rollback || core_.s.resync_pending_ || static_cast<int32_t>(tick - core_.s.held_floor_) <= 0;
        }

        // One own tick the server applied `frame` at, resolved: base, held
        // for its live frame, late, or recv (note_held_section's rules; a
        // SessionLead's simulated range takes the same path).
        void settle_held_tick(uint32_t tick, const bmmo::session::input_frame& frame) {
            if (held_tick_is_base(tick)) {
                ++core_.s.subst_base_;
                // Kept all the same (design 1.3: stored with or without a
                // way to replay it) - a tick not simulated yet when its live
                // frame is.
                if (static_cast<int32_t>(tick - current_tick()) > 0) hold_ahead(tick, frame, false);
                else store_held_frame(tick, frame);
                return;
            }
            if (static_cast<int32_t>(tick - current_tick()) > 0) {   // not simulated yet
                ++core_.s.subst_recv_;
                hold_ahead(tick, frame, true);
                return;
            }
            store_held_frame(tick, frame);
            // The gate note_input_correction applies: the anchor of the
            // rewind, tick - 1, still re-simulable and in the history.
            const uint32_t anchor = tick - 1;
            const bool reachable = current_tick() - anchor <= core_.s.rollback_.thresholds().max_resim_ticks
                                && core_.s.rollback_.knows_tick(anchor);
            if (!reachable) {
                ++core_.s.subst_late_;
                if (core_.s.subst_late_logs_++ < 8)
                    logf("held notice for own tick %u arrived at tick %u, past the reach of %u ticks", tick,
                         current_tick(), core_.s.rollback_.thresholds().max_resim_ticks);
                request_resync("a substituted own frame arrived past the reach", true);
                return;
            }
            ++core_.s.subst_recv_;
            correct_own_tick(tick, frame);
        }

        // SessionLead (late-player design 2.4): the server moves this
        // client's lead by msg.delta and, for a raise, applied `held` at
        // every tick of [held_from, effective) - its buffer for us was reset
        // there, and our frames below E are stale to it.  At our current
        // tick c (bmmo::session::lead_split_range):
        //  1. the range's ticks at or before c ran on our own input: each is
        //     a substituted tick like a held notice's (settle_held_tick -
        //     stored, journaled SUBSTITUTED, the record amended and one
        //     replay from the earliest), counted once with the notices;
        //  2. the ticks of (c, E) are forced: their live frames drive the
        //     own ball with `held` (frame()), so we play what the server
        //     applies and their notices, when they come, change nothing;
        //  3. every tick of the range counts as learned for the completeness
        //     check: the server's held_total counts them, and a notice that
        //     comes later is a re-delivery;
        //  4. the clock slews by delta (pace_and_frame), and the ack goes out
        //     when the slew ends, with what it moved - or, when a resync
        //     request ends it first, with the whole delta, which the
        //     re-anchor at L_eff applies (request_resync).
        // Nothing is renumbered: the first tick played on our own input
        // again is E, stamped E, exactly where the server's buffer takes our
        // frames from.  Copies are dropped by seq; a decision of another
        // base by epoch (a resync crossed it: the server dropped it too).
        void on_session_lead(const bmmo::session_lead_msg& msg) {
            if (!core_.s.assigned_ || msg.player != core_.s.own_id_) return;
            if (core_.s.lead_seen_.count(msg.seq)) { ++core_.s.lead_dups_; return; }
            if (msg.epoch != core_.s.tick_base_) {
                // An older base: void.  A newer one: an unreliable copy that
                // overtook its assignment; the reliable one follows it.
                ++core_.s.lead_stale_;
                logf("lead: SessionLead %u of base %u dropped (our base %u)", msg.seq, msg.epoch, core_.s.tick_base_);
                return;
            }
            core_.s.lead_seen_.insert(msg.seq);
            // A slew still running (the server never overlaps two, so this is
            // defensive): it ends here with what it moved, acknowledged.
            if (core_.s.lead_slew_.active()) finish_lead_slew("superseded by SessionLead " + std::to_string(msg.seq));
            // A resync of ours in flight: the server answers it with an
            // assignment at L_eff that voids this decision (send_session_assign
            // clears the outstanding one, and our ack would come after the
            // request on the same channel and be ignored).  The range is
            // still applied - the server plays `held` there until it
            // re-anchors us - but the clock is not slewed for an assignment
            // that replaces it, and nothing is acknowledged or counted.
            const bool voided = core_.s.resync_in_flight_;
            // The slew starts before the range is settled: an amended tick
            // past the reach asks for a resync (settle_held_tick), and
            // request_resync then acknowledges this decision in full ahead of
            // the request, so the re-anchor at L_eff carries it.
            const uint64_t resyncs_before = core_.s.resyncs_sent_;
            if (!voided) {
                core_.s.lead_slew_.start(msg.seq, msg.delta);
                core_.s.lead_slew_clock_ = wall_now(bmmo::sim::wall_site::slew_start);
            }
            const uint32_t next_unsimulated = core_.s.frames_since_anchor_ >= 1 ? current_tick() + 1 : core_.s.tick_base_;
            const auto ranges = bmmo::session::lead_cap_forced(
                bmmo::session::lead_split_range(msg.held_from, msg.effective, next_unsimulated),
                static_cast<uint32_t>(bmmo::session::kSentInputRing));
            uint32_t amended = 0, forced = 0, known = 0;
            if (msg.held_from != 0) {
                // Up to the forced range's end, capped: the ticks past the cap
                // are not ours to force or learn (lead_cap_forced), their
                // notices settle them.
                for (uint32_t t = msg.held_from; static_cast<int32_t>(t - ranges.force_to) < 0; ++t) {
                    const bool first = core_.s.held_seen_.first(core_.s.own_id_, t);
                    if (first) learn_held_tick(t);
                    else ++known;
                    if (static_cast<int32_t>(t - ranges.force_from) < 0) {
                        // Simulated: a notice that came first settled it already.
                        if (first) { settle_held_tick(t, msg.held); ++amended; }
                        continue;
                    }
                    // Ahead: the live frame plays `held`.  A notice that named
                    // the tick first waits in held_ahead_ to compare the record
                    // with what we play, which is now the same frame (the
                    // server froze it for the range): dropped, and a recv one
                    // counted as the same frame here, since the forced tick
                    // does not count it again.
                    if (const auto ahead = core_.s.held_ahead_.find(t); ahead != core_.s.held_ahead_.end()) {
                        if (ahead->second.second) ++core_.s.subst_same_;
                        core_.s.held_ahead_.erase(ahead);
                    }
                    core_.s.forced_own_[t] = forced_own{msg.held, first};
                    ++forced;
                }
            }
            const bool reanchored = core_.s.resyncs_sent_ != resyncs_before;
            if (!voided) ++core_.s.lead_adjustments_;
            else ++core_.s.lead_void_;
            logf("lead: SessionLead %u %s: k=%+d N=%u E=%u at tick %u (base %u): amended %u [%u,%u) forced %u [%u,%u) "
                 "already noticed %u, held-source=%s; %s",
                 msg.seq, voided ? "void (a resync of ours is in flight), range applied" : "applied",
                 static_cast<int>(msg.delta), msg.held_from, msg.effective,
                 core_.s.frames_since_anchor_ >= 1 ? current_tick() : core_.s.tick_base_, core_.s.tick_base_, amended, ranges.amend_from,
                 ranges.amend_to, forced, ranges.force_from, ranges.force_to, known,
                 msg.held_source == bmmo::session::INPUT_SOURCE_DEFAULT ? "default" : std::to_string(msg.held_source).c_str(),
                 voided ? "no slew" : reanchored ? "acknowledged in full ahead of the resync it asked for"
                                    : msg.delta > 0 ? "slewing at 2x" : "slewing at 0.9x");
            core_.journal().note("lead: SessionLead " + std::to_string(msg.seq) + " " + (msg.delta > 0 ? "+" : "")
                         + std::to_string(msg.delta) + " held [" + std::to_string(msg.held_from) + ", "
                         + std::to_string(msg.effective) + "), forced from " + std::to_string(ranges.force_from)
                         + (voided ? " (void: resync in flight)" : ""));
            // A zero delta: nothing to slew, acknowledged at once (unless a
            // resync request did it, or the decision is void).
            if (!voided && !reanchored && !core_.s.lead_slew_.active()) finish_lead_slew("nothing to slew");
        }

        // The slew ends - moved in full, superseded, or ended by a resync
        // request (`reanchored`: acknowledged in full, the re-anchor at
        // L_eff applies all of it, lead_slew::finish_reanchored) - and the
        // server is told what it applied (its lead_offset and a resync's
        // L_eff follow that, bounded by the delta it sent).  Reliable: it is
        // ordered before the resync request request_resync sends right after
        // it, so the re-anchor already counts it.
        void finish_lead_slew(const std::string& why, bool reanchored = false) {
            const uint32_t seq = core_.s.lead_slew_.seq();
            const int delta = core_.s.lead_slew_.delta();
            const int moved = core_.s.lead_slew_.applied();
            const int applied = reanchored ? core_.s.lead_slew_.finish_reanchored() : core_.s.lead_slew_.finish();
            core_.s.lead_offset_ += applied;
            bmmo::session_lead_ack_msg ack;
            ack.session = core_.s.session_;
            ack.seq = seq;
            ack.applied = static_cast<int16_t>(std::clamp(applied, -32768, 32767));
            ack.serialize();
            send_bytes(ack.raw.str(), k_nSteamNetworkingSend_Reliable);
            logf("lead: SessionLead %u acknowledged at tick %u: applied %+d of %+d, the slew moved %+d (%s); lead offset %+d",
                 seq, current_tick(), applied, delta, moved, why.c_str(), core_.s.lead_offset_);
            core_.journal().note("lead: SessionLead " + std::to_string(seq) + " applied " + std::to_string(applied) + " of "
                         + std::to_string(delta) + " (" + why + ")");
        }

        // A new assignment voids the adjustment in flight and the forced
        // ticks (numbered from the old base): the server dropped the decision
        // with its base and ignores an ack of it.  `why`: a new base, or the
        // session's end.
        void drop_lead_state(const char* why) {
            if (core_.s.lead_slew_.active()) {
                logf("lead: SessionLead %u voided by %s after %+d of %+d ticks", core_.s.lead_slew_.seq(), why,
                     core_.s.lead_slew_.applied(), core_.s.lead_slew_.delta());
                core_.s.lead_slew_.finish();
            }
            core_.s.forced_own_.clear();
        }

        // The frame a held record says the server applied.  False when it
        // names a source our sent ring no longer holds (the server writes the
        // frame inline when the source is 96 ticks back or before our epoch,
        // so inside the reach this does not happen).
        bool resolve_held_frame(const bmmo::session_remote_input_msg::held_record& record,
                                bmmo::session::input_frame& out) const {
            if (record.has_frame) { out = record.frame; return true; }
            if (record.source == bmmo::session::kHeldSourceDefault) { out = bmmo::session::input_frame{}; return true; }
            const auto it = core_.s.sent_inputs_.find(record.source);
            if (it == core_.s.sent_inputs_.end()) return false;
            out = it->second;
            return true;
        }

        // The server's frame becomes our input for `tick`, with or without a
        // body there: a replay that re-makes the own body (design 3.3) drives
        // it from own_inputs_, and recorded_input_at must agree with the
        // record.  The journal's SUBSTITUTED record follows the tick's FRESH
        // one, and a reader takes the later.
        void store_held_frame(uint32_t tick, const bmmo::session::input_frame& frame) {
            if (args_.rollback) {
                core_.s.own_inputs_[tick] = frame;
                while (core_.s.own_inputs_.size() > 128) core_.s.own_inputs_.erase(core_.s.own_inputs_.begin());
            }
            if (core_.journal().journalling()) core_.journal().writer().input(tick, core_.s.own_id_, frame, bmmo::session::JOURNAL_INPUT_SUBSTITUTED);
        }

        // A notice for a tick this client has not simulated yet (it runs
        // behind the server's cursor: a stall, a slow start).  Its live frame
        // would overwrite our input and journal FRESH after SUBSTITUTED, so it
        // waits for that frame (take_held_ahead).  `correct`: a recv record,
        // compared and replayed then; a base one is only stored.
        void hold_ahead(uint32_t tick, const bmmo::session::input_frame& frame, bool correct) {
            core_.s.held_ahead_[tick] = {frame, correct};
            while (core_.s.held_ahead_.size() > bmmo::session::kSentInputRing) core_.s.held_ahead_.erase(core_.s.held_ahead_.begin());
        }

        // After the live frame of `tick` is recorded: a notice that named it
        // earlier is applied now, the record compared with the frame the
        // server applied and the tick replayed on the next frame when they
        // differ.  Notices the numbering moved past are dropped.
        void take_held_ahead(uint32_t tick) {
            while (!core_.s.held_ahead_.empty() && static_cast<int32_t>(core_.s.held_ahead_.begin()->first - tick) < 0)
                core_.s.held_ahead_.erase(core_.s.held_ahead_.begin());
            const auto it = core_.s.held_ahead_.find(tick);
            if (it == core_.s.held_ahead_.end()) return;
            const bmmo::session::input_frame frame = it->second.first;
            const bool correct = it->second.second;
            core_.s.held_ahead_.erase(it);
            store_held_frame(tick, frame);
            // A resync since the notice arrived: its snapshot is the server's
            // world, our record of this tick is not.
            if (!correct || !args_.rollback || core_.s.resync_pending_ || static_cast<int32_t>(tick - core_.s.held_floor_) <= 0) return;
            correct_own_tick(tick, frame);
        }

        // The own branch for a substituted tick inside the reach (design
        // 1.3).  A replay feeds the own ball from the RECORD of each tick,
        // not from own_inputs_, so the record is what has to change; its own
        // entity is the one to amend, since a trafo may have renamed the
        // ball since.  No ball at the tick: the frame drove nothing.  The
        // frame we played: the server's ball and ours ran on the same input,
        // nothing to replay.  Otherwise the record takes the server's frame
        // and a later frame replays from tick - 1 (replay_corrected_inputs:
        // one replay from the earliest tick for every correction pending,
        // own and remote; an own-only one may wait a few frames for the
        // notices still coming, own_replay_pacer).
        void correct_own_tick(uint32_t tick, const bmmo::session::input_frame& frame) {
            const std::string entity = core_.s.rollback_.recorded_own_entity(tick);
            if (entity.empty()) { ++core_.s.subst_bodiless_; return; }
            bmmo::session::input_frame played{};
            const bool have_played = core_.s.rollback_.recorded_input(entity, tick, played);
            if (have_played && same_frame(played, frame)) { ++core_.s.subst_same_; return; }
            ++core_.s.subst_diff_;
            if (!core_.s.rollback_.amend_recorded_input(entity, tick, frame)) return;   // the tick was dropped meanwhile
            // A difference no replay can see (a camera turned with no leaf
            // key down, Shift or Space, navigation off in both): the record
            // holds the server's frame now, which every later replay feeds,
            // and a replay of its own would step the same world again.
            if (have_played && !bmmo::session::replay_sees_difference(played, frame)) { ++core_.s.subst_unseen_; return; }
            // Earliest or not, the replay carries an own correction: counted
            // under subst rew, and a refusal resyncs (replay_corrected_inputs).
            core_.s.input_rewind_has_own_ = true;
            ++core_.s.input_rewind_own_amends_;
            if (!core_.s.input_rewind_pending_ || static_cast<int32_t>(tick - core_.s.input_rewind_tick_) < 0) {
                core_.s.input_rewind_pending_ = true;
                core_.s.input_rewind_tick_ = tick;
                core_.s.input_rewind_entity_ = entity;
                core_.s.input_rewind_own_ = true;
            }
        }

        // The ticks whose notice has arrived, for the completeness check: a
        // total since the epoch and the recent ticks themselves, enough to
        // count those at or below a datagram's tick (a notice rides at most
        // kHeldNoticeTicks datagrams, and a datagram is judged 64 ticks on).
        void learn_held_tick(uint32_t tick) {
            ++core_.s.held_learned_total_;
            core_.s.held_learned_recent_.insert(tick);
            while (core_.s.held_learned_recent_.size() > 256) core_.s.held_learned_recent_.erase(core_.s.held_learned_recent_.begin());
        }

        uint64_t held_learned_upto(uint32_t tick) const {
            uint64_t later = 0;
            for (auto it = core_.s.held_learned_recent_.upper_bound(tick); it != core_.s.held_learned_recent_.end(); ++it) ++later;
            return core_.s.held_learned_total_ - later;
        }

        // Completeness (design 1.3): `held_total` counts our substituted
        // ticks at or after the epoch up to the datagram's tick, and every one
        // of them is noticed in 32 consecutive datagrams.  A datagram whose
        // count is above the ticks we learned up to its tick may only be
        // ahead of a sibling datagram still on its way; if it is still above
        // 64 ticks later, every copy of a notice was lost, and that tick ran
        // on a frame we will never know.  Counted late (the server counted
        // it) and answered with a resync, unless one is on its way already -
        // its snapshot covers the tick or the next resync's will.  The
        // shortfall is credited so the datagrams after it do not count it
        // again.
        static constexpr uint32_t kHeldGapTicks = 64;
        void check_held_complete(const bmmo::session_remote_input_msg& msg) {
            if (msg.held_total > held_learned_upto(msg.tick) + core_.s.held_gap_credit_) {
                auto& total = core_.s.held_gap_watch_[msg.tick];
                total = std::max(total, msg.held_total);
                while (core_.s.held_gap_watch_.size() > 256) core_.s.held_gap_watch_.erase(core_.s.held_gap_watch_.begin());
            }
            for (auto it = core_.s.held_gap_watch_.begin(); it != core_.s.held_gap_watch_.end();) {
                if (static_cast<int32_t>(msg.tick - it->first) <= static_cast<int32_t>(kHeldGapTicks)) break;
                const uint64_t have = held_learned_upto(it->first) + core_.s.held_gap_credit_;
                if (it->second > have) {
                    const uint64_t missing = it->second - have;
                    core_.s.subst_late_ += missing;
                    core_.s.held_gap_credit_ += missing;
                    logf("held notices lost: held_total %u at tick %u, %llu of them never arrived", it->second, it->first,
                         static_cast<unsigned long long>(missing));
                    // The base rule a received record follows: no resync for
                    // what no rewind could have corrected anyway - without
                    // --rollback every record is base, and a datagram at or
                    // below the floor names only ticks a resync's snapshot
                    // covered.
                    if (!core_.s.resync_pending_ && args_.rollback && static_cast<int32_t>(it->first - core_.s.held_floor_) > 0)
                        request_resync("substituted own frames whose every notice was lost", true);
                }
                it = core_.s.held_gap_watch_.erase(it);
            }
        }

        // Everything above is numbered from our base: a new assignment (start
        // or resync) starts it over.  The sent ring goes too - the server
        // writes a source from before the epoch inline.
        // A replay asked for by a correction under the previous numbering
        // has no anchor in the new one (the history went with it): dropped,
        // not run into a refusal - a refused own replay asks for a resync.
        void reset_held_epoch(const char* why = "the new base") {
            drop_pending_rewind();
            drop_lead_state(why);
            core_.s.own_replay_pacer_.reset();
            core_.s.held_seen_.clear();
            core_.s.held_ahead_.clear();
            core_.s.sent_inputs_.clear();
            core_.s.held_learned_recent_.clear();
            core_.s.held_learned_total_ = 0;
            core_.s.held_gap_credit_ = 0;
            core_.s.held_gap_watch_.clear();
        }

        void drop_pending_rewind() {
            core_.s.input_rewind_pending_ = false;
            core_.s.input_rewind_own_ = false;
            core_.s.input_rewind_has_own_ = false;
            core_.s.input_rewind_own_amends_ = 0;
            core_.s.input_rewind_entities_.clear();
        }

        // Whether the server is still substituting our ticks: its newest
        // header of our epoch says it lacked our frame for the tick it
        // simulated (acked < tick).  Unknown (no header of this epoch yet)
        // counts as yes - the pacer's reach deadline still bounds the wait.
        bool own_notices_coming() const {
            return !held_header_current() || static_cast<int32_t>(core_.s.held_header_tick_ - core_.s.held_header_acked_) > 0;
        }

        // Another rewind that replayed from `anchor` already carried a
        // waiting own-only replay (bmmo::session::replay_subsumes; any
        // anchor when `server_rows`: a snapshot rollback that wrote the
        // server's own-ball row at its anchor): dropped instead of run.  Run
        // after it, a replay from an earlier anchor would also restore our
        // prediction over the snapshot's rows.  A pending replay that carries
        // a remote's correction is left to replay_corrected_inputs as before.
        void drop_carried_own_replay(uint32_t anchor, bool server_rows) {
            if (!core_.s.input_rewind_pending_ || !core_.s.input_rewind_own_ || !core_.s.input_rewind_entities_.empty()) return;
            if (!server_rows && !bmmo::session::replay_subsumes(anchor, core_.s.input_rewind_tick_)) return;
            ++core_.s.subst_subsumed_;
            core_.s.subst_saved_ += core_.s.input_rewind_own_amends_;
            core_.s.own_replay_pacer_.ran();
            drop_pending_rewind();
        }

        bool held_header_current() const { return core_.s.held_header_known_ && core_.s.held_header_epoch_ == core_.s.tick_base_; }

        // A relayed frame for a tick this client has already simulated: if it
        // differs from the frame the prediction used, that tick and every tick
        // after it ran on the wrong input.  Remember the earliest such tick and
        // let frame() replay from it once, before the next step - the frames of
        // several remotes arrive together, and one rewind covers them all.

        void note_input_correction(const remote_ball& remote, uint32_t tick,
                                   const bmmo::session::input_frame& relayed) {
            const std::string& entity = remote.entity;
            if (!args_.rollback || entity.empty() || tick == 0) return;
            if (static_cast<int32_t>(tick - current_tick()) > 0) return;   // not simulated yet
            // Deeper than the engine re-simulates (48 ticks), or past the
            // history: nothing to replay from.  Measured at the anchor the
            // rewind will use, the tick before the corrected one
            // (replay_corrected_inputs rewinds to tick - 1).  Counted - the
            // relay's pass criterion is that this never happens.
            const uint32_t anchor = tick - 1;
            if (current_tick() - anchor > core_.s.rollback_.thresholds().max_resim_ticks || !core_.s.rollback_.knows_tick(anchor)) {
                ++core_.s.relay_late_;
                return;
            }
            // A frame also stands in for the holes after it (the nearest
            // earlier one, relay_frame_or_earlier): it can still change the
            // replay of tick + 1 when the ring has no frame there but has a
            // later one, and the body was there at tick + 1.
            const bool feeds_next = remote.inputs.find(tick + 1) == remote.inputs.end()
                                 && remote.inputs.upper_bound(tick + 1) != remote.inputs.end()
                                 && core_.s.rollback_.recorded_body(entity, tick + 1);
            // No body behind the name at that tick - the player was dead there,
            // or the birth that makes the mirror has not arrived yet: the frame
            // drove nothing, and a replay from it reproduces what the records
            // have (measured: 154-200 rewinds per client in a lane run spent
            // on dead players' mirrors).  The frame is in the ring already, so
            // the replay of a late birth still feeds it.
            if (!core_.s.rollback_.recorded_body(entity, tick) && !feeds_next) {
                ++core_.s.relay_bodiless_;
                return;
            }
            // The birth tick: every rewind across it parks the body and re-poses
            // it at that tick's record after the step, so a different frame for
            // it changes nothing a replay can reach - a whole replay for the
            // same world.  (The one-tick error stays until a snapshot.)
            if (remote.physicalized && tick == remote.born_tick && !feeds_next) {
                ++core_.s.relay_birth_frozen_;
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
            if (core_.s.rollback_.recorded_input(entity, tick, predicted) && same_frame(predicted, relayed))
                return;   // the prediction was right
            if (!core_.s.input_rewind_pending_ || static_cast<int32_t>(tick - core_.s.input_rewind_tick_) < 0) {
                core_.s.input_rewind_pending_ = true;
                core_.s.input_rewind_tick_ = tick;
                core_.s.input_rewind_entity_ = entity;
                core_.s.input_rewind_own_ = false;
            }
            // Every ball corrected this frame, not only the earliest: the one
            // replay below re-runs them all, and the window: line credits it
            // to each source (late-player design 5.2, input rewinds naming the
            // late ball).
            core_.s.input_rewind_entities_.insert(entity);
        }

        // The engine clock and the frame step on one line, in hex floats: the
        // IVP times, the event queue, the manager's smoothed delta, the time
        // factor and the physics step it yields (--trace-clock and the
        // savestate restore report).
        std::string describe_clock() {
            bmmo_physics_clock_state clock{};
            float factor = 0.0f, physics_delta = 0.0f;
            std::string error;
            bmmo::physics::get_clock_state(engine_->physics(), clock, error);
            bmmo::physics::get_clock(engine_->physics(), factor, physics_delta, error);
            char text[400];
            std::snprintf(text, sizeof(text), "t=%a tlast=%a tnext=%a base=%a last=%a nmc=%d q=%d qmin=%a factor=%a pdt=%a",
                          clock.current_time, clock.time_of_last_psi, clock.time_of_next_psi, clock.base_time, clock.last_time,
                          static_cast<int>(clock.next_movement_check), static_cast<int>(clock.queued_events),
                          static_cast<double>(clock.queue_min), static_cast<double>(factor), static_cast<double>(physics_delta));
            return text;
        }

        // --dump-savestate: the engine's record of `tick` as a file (see the
        // argument's comment).  Overwrites: after a rewind the replayed ticks'
        // records are the replayed world's.
        void dump_savestate(uint32_t tick, const char* source = "live") {
            compare_scripts(tick, source);
            if (args_.dump_savestate_dir.empty() || tick < args_.dump_savestate_from || tick > args_.dump_savestate_until) return;
            dump_scripts(tick);
            const std::vector<uint8_t>* blob = core_.s.rollback_.recorded_savestate(tick);
            if (savestate_dump_logs_ == 0) {
                ++savestate_dump_logs_;
                logf("--dump-savestate: first tick %u in range, record %s, into %s (cwd %s)", tick, blob ? "has a blob" : "HAS NO BLOB",
                     args_.dump_savestate_dir.c_str(), std::filesystem::current_path().string().c_str());
            }
            if (!blob) return;
            std::error_code ec;
            std::filesystem::create_directories(args_.dump_savestate_dir, ec);
            const std::filesystem::path path = std::filesystem::path(args_.dump_savestate_dir) / (std::to_string(tick) + ".sav");
            if (FILE* f = std::fopen(path.string().c_str(), "wb")) {
                std::fwrite(blob->data(), 1, blob->size(), f);
                std::fclose(f);
            } else if (savestate_dump_logs_++ < 4) {
                logf("--dump-savestate: cannot write %s", path.string().c_str());
            }
        }

        // The scripts' state beside the blob: <tick>.scr, the bytes the record
        // of this tick carries (after a rewind, the replayed state's).
        void dump_scripts(uint32_t tick) {
            const std::vector<uint8_t>* recorded = core_.s.rollback_.recorded_scripts(tick);
            if (!recorded) return;
            const std::vector<uint8_t>& bytes = *recorded;
            std::error_code ec;
            std::filesystem::create_directories(args_.dump_savestate_dir, ec);
            const std::filesystem::path path = std::filesystem::path(args_.dump_savestate_dir) / (std::to_string(tick) + ".scr");
            if (FILE* f = std::fopen(path.string().c_str(), "wb")) {
                std::fwrite(bytes.data(), 1, bytes.size(), f);
                std::fclose(f);
            }
        }

        // --compare-scripts: this tick's recorded scripts against the other
        // run's.  `source` says whose record this is: the live frame's, or the
        // replay's re-record after a rewind (then a difference is the replay's
        // own, if the live record agreed).
        void compare_scripts(uint32_t tick, const char* source) {
            if (args_.compare_scripts_dir.empty() || !args_.script_replay || scripts_.empty()) return;
            const std::vector<uint8_t>* recorded = core_.s.rollback_.recorded_scripts(tick);
            if (!recorded) return;
            const std::vector<uint8_t>& mine = *recorded;
            const std::filesystem::path path = std::filesystem::path(args_.compare_scripts_dir) / (std::to_string(tick) + ".scr");
            FILE* f = std::fopen(path.string().c_str(), "rb");
            if (!f) return;
            std::vector<uint8_t> theirs;
            uint8_t buffer[4096];
            for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), f)) > 0;) theirs.insert(theirs.end(), buffer, buffer + n);
            std::fclose(f);
            if (compare_scripts_ticks_++ == 0)
                logf("--compare-scripts: first tick %u compared with %s", tick, args_.compare_scripts_dir.c_str());
            std::string key;
            if (mine != theirs) {
                const auto d = bmmo::game::script_replay::diff(engine_->context(), scripts_, theirs, mine);
                // Keyed on every logical difference the diff lists, not the
                // first alone: a standing difference in one script must not
                // hide a new one in a script after it.
                key = d.detail.empty() ? d.where : d.detail;
                key += d.logical ? " [logical]" : " [float]";
                if (key != compare_scripts_last_ && compare_scripts_logs_++ < 200)
                    logf("--compare-scripts: tick %u (%s record): this run differs from the other at %s (%d logical, %d float; theirs vs ours)%s%s",
                         tick, source, d.where.c_str(), d.logical_count, d.float_count, d.detail.empty() ? "" : " :: ", d.detail.c_str());
            } else if (!compare_scripts_last_.empty() && compare_scripts_logs_++ < 200) {
                logf("--compare-scripts: tick %u (%s record): the scripts agree again", tick, source);
            }
            compare_scripts_last_ = key;
        }

        // The live frame's own navigation input - the camera rows set after the
        // previous tick, pending for this tick's PreSimulate pass, where the
        // replica polls the keys - is consumed by a re-simulation's own input
        // feed.  Re-armed by the binding's on_poll_resumed after every rewind
        // that replayed (the snapshot path used to do it by hand; the forced
        // and input-correction rewinds did not): without it the first live
        // tick after a replay ran on the last replayed tick's keys and camera,
        // and a key edge on that tick landed one tick late (measured on the
        // arena with --force-rollback 7:5: 6 mm at the edge, a metre 1300
        // ticks later; a 20:12 cadence never put an edge on that tick).
        void rearm_own_navigation() {
            if (!own_navigation_ || !core_.s.previous_cam_valid_) return;
            std::string error;
            if (!bmmo::physics::navigation_input(engine_->physics(), own_nav_entity_.c_str(), 0, core_.s.previous_cam_[0], core_.s.previous_cam_[1],
                                                 core_.s.previous_cam_[2], false, error))
                logf("own navigation rows after a rewind: %s", error.c_str());
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
            if (args_.trace_restore && core_.s.own_physicalized_ && !core_.s.own_body_entity_.empty() && contact_traces_ < 48) {
                ++contact_traces_;
                logf("contacts of %s before the rewind at %u: %s", core_.s.own_body_entity_.c_str(), now,
                     bmmo::physics::describe_contacts(engine_->physics(), core_.s.own_body_entity_.c_str()).c_str());
            }
            if (args_.trace_restore && queue_traces_ < 24) {
                ++queue_traces_;
                logf("PreSimulate queue before the rewind at %u: %s", now,
                     bmmo::physics::describe_presimulate_queue(engine_->physics()).c_str());
            }
            const live_cells_guard keep_cells(*this);
            if (core_.s.rollback_.rewind_to(rollback_world(), anchor, now, input_at, "forced")) {
                ++core_.s.forced_rollbacks_;
                core_.s.forced_rollback_depth_ += now - anchor;
                drop_carried_own_replay(anchor, false);
                for (uint32_t t = anchor + 1; t <= now; ++t) dump_savestate(t, "replayed");
                if (args_.trace_restore && queue_traces_ < 24) {
                    ++queue_traces_;
                    logf("PreSimulate queue after the rewind at %u: %s", now,
                         bmmo::physics::describe_presimulate_queue(engine_->physics()).c_str());
                }
            } else {
                ++core_.s.forced_rollbacks_missed_;
            }
            if (args_.trace_restore && core_.s.own_physicalized_ && !core_.s.own_body_entity_.empty() && contact_traces_ < 48) {
                ++contact_traces_;
                if (!args_.trace_body.empty() && now >= args_.trace_from && now <= args_.trace_until)
                    logf("body %s after the rewind (replayed to %u): %s", args_.trace_body.c_str(), now,
                         bmmo::physics::describe_body(engine_->physics(), args_.trace_body.c_str()).c_str());
                logf("contacts of %s after the rewind (replayed to %u): %s", core_.s.own_body_entity_.c_str(), now,
                     bmmo::physics::describe_contacts(engine_->physics(), core_.s.own_body_entity_.c_str()).c_str());
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
            core_.s.own_replay_pacer_.frame();
            if (!core_.s.input_rewind_pending_) return;
            // Our own corrections alone are coalesced (own_replay_pacer): while
            // the server keeps substituting our ticks, one notice per server
            // tick would otherwise be one replay per frame across whatever the
            // last ~42 ticks hold - the sector activation on the lane - and
            // that cost kept the client late, which kept the notices coming
            // (lp4_a0/a1: five of seven runs ended in a schedule rebase).  The
            // records are amended already and the pending replay keeps the
            // earliest tick, so the one that runs carries them all; it runs
            // before that tick's anchor would leave the reach.  A remote's
            // correction is never held back, and takes ours along.
            // Only while the server still substitutes our ticks: once its
            // header says it has our frames again no notice will join the
            // waiting replay, which then runs at once.
            if (core_.s.input_rewind_own_ && core_.s.input_rewind_entities_.empty()
                    && !core_.s.own_replay_pacer_.due(current_tick(), core_.s.input_rewind_tick_, core_.s.rollback_.thresholds().max_resim_ticks,
                                              own_notices_coming())) {
                ++core_.s.subst_deferred_;
                return;
            }
            const uint32_t anchor = core_.s.input_rewind_tick_;
            const std::string entity = core_.s.input_rewind_entity_;
            // The earliest correction names the replay: our own frame the
            // server substituted (late-player design 1.3) or a remote's
            // relayed frame.  One replay covers both kinds and is counted in
            // each class it carries: subst rew / the window's substrew when
            // an own correction is among this frame's, inrew when a remote's
            // is (both, when both are).
            const bool own = core_.s.input_rewind_own_;
            const bool has_own = core_.s.input_rewind_has_own_ || own;
            const uint32_t own_amends = core_.s.input_rewind_own_amends_;
            std::set<std::string> corrected;
            corrected.swap(core_.s.input_rewind_entities_);
            const bool has_remote = !corrected.empty() || !own;
            drop_pending_rewind();
            const uint32_t now = current_tick();
            if (static_cast<int32_t>(anchor - now) > 0) return;
            auto input_at = [this](const std::string& name, uint32_t tick, bmmo::session::input_frame& out) {
                return recorded_input_at(name, tick, out);
            };
            const live_cells_guard keep_cells(*this);
            const std::string reason = own ? "own input substituted at tick " + std::to_string(anchor)
                                           : entity + " input corrected at tick " + std::to_string(anchor);
            if (has_own) core_.s.own_replay_pacer_.ran();
            if (core_.s.rollback_.rewind_to(rollback_world(), anchor - 1, now, input_at, reason)) {
                if (has_own) {
                    ++core_.s.subst_rewinds_;
                    core_.s.subst_rewind_depth_ += now - (anchor - 1);
                    if (own_amends > 1) core_.s.subst_saved_ += own_amends - 1;   // amendments that rode along
                }
                if (has_remote) {
                    ++core_.s.input_rewinds_;
                    core_.s.input_rewind_depth_ += now - (anchor - 1);
                }
                // Named by every source ball it corrects: the harness asks
                // whether rewinds naming a late player grow (late-player
                // design 5.2), and one replay covers all of this frame's.
                for (const auto& [id, remote]: core_.s.remotes_)
                    if (corrected.count(remote.entity)) ++core_.s.window_inrew_src_[id];
                return;
            }
            if (has_remote) ++core_.s.input_rewinds_missed_;
            if (has_own) ++core_.s.subst_rewinds_missed_;
            if (core_.s.input_rewinds_missed_ + core_.s.subst_rewinds_missed_ < 8)
                logf("input correction for %s at tick %u is outside the rollback history at local tick %u: "
                     "a snapshot has to re-anchor it", entity.c_str(), anchor, now);
            // Our own ball ran on a frame the server did not apply (its
            // record is amended, never replayed), and with snapshots off
            // nothing else would ever repair it.  Not while a resync waits
            // for its snapshot: that snapshot is the repair.
            if (has_own && !core_.s.resync_pending_) request_resync("a substituted own frame could not be replayed", true);
        }

        // Every movable body in this client's world except the balls, which
        // the tracked set carries separately as own/remote entities.
        std::vector<std::string> movable_body_names() {
            // Own and remote balls are tracked by entity, not as mechanisms.
            return bmmo::physics::movable_body_names(engine_->physics(), "Ball_");
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

        // The input a navigated ball had at `tick`, for a re-simulation.  The
        // own ball: only an exact hit.  A remote: the relayed frame, or for a
        // hole between relayed frames the nearest earlier one (at most a
        // window old, where the recorded prediction is a round trip old).
        bool recorded_input_at(const std::string& entity, uint32_t tick, bmmo::session::input_frame& out) const {
            const std::map<uint32_t, bmmo::session::input_frame>* inputs = nullptr;
            if (entity == own_nav_entity_) inputs = &core_.s.own_inputs_;
            else
                for (const auto& [id, remote]: core_.s.remotes_)
                    if (remote.entity == entity) inputs = &remote.inputs;
            if (!inputs) return false;
            if (inputs == &core_.s.own_inputs_) {
                auto it = inputs->find(tick);
                if (it == inputs->end()) return false;
                out = it->second;
                return true;
            }
            // A remote: the relayed frame, or for a hole the window did not
            // cover the nearest earlier one (session/input_relay.hpp); none for
            // the tail the relay has not reached, where the recorded prediction
            // stands.
            const auto* frame = bmmo::session::relay_frame_or_earlier(*inputs, tick);
            if (!frame) return false;
            out = *frame;
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
            if (core_.s.pending_events_.empty()) return;
            std::vector<bmmo::session_event_msg> due;
            auto keep = core_.s.pending_events_.begin();
            for (auto it = core_.s.pending_events_.begin(); it != core_.s.pending_events_.end(); ++it) {
                if (static_cast<int32_t>(it->tick - tick) <= 0) due.push_back(std::move(*it));
                else *keep++ = std::move(*it);
            }
            core_.s.pending_events_.erase(keep, core_.s.pending_events_.end());
            if (due.empty()) return;
            // (join, applied, origin, rank): the server applies each event at
            // its applied tick, and one player's events of one tick by origin
            // (protocol 9, late-player design 3.1).
            std::stable_sort(due.begin(), due.end(),
                             [this](const bmmo::session_event_msg& a, const bmmo::session_event_msg& b) {
                                 const uint32_t ja = join_rank(a.player), jb = join_rank(b.player);
                                 if (ja != jb) return ja < jb;
                                 if (a.tick != b.tick) return static_cast<int32_t>(a.tick - b.tick) < 0;
                                 return bmmo::session::lifecycle_apply_before(ja, a.origin_tick, a.type,
                                                                              jb, b.origin_tick, b.type);
                             });
            // A body change stamped for this very tick (on time) is held until
            // this frame's rewinds have run (apply_held_lifecycle): made now,
            // a rewind anchored before the stamp would restore the body's old
            // life onto the new core - no record carries the new generation
            // yet, so the anchor's record still counts as the same life - and
            // replay it to the tick before the stamp, undoing the change for
            // good, the way the own beam used to be undone.  Not late, it
            // asks for no rewind of its own, so holding it moves nothing else.
            for (auto& event: due) {
                if (event.tick == tick && (event.type == bmmo::session::event_type::Physicalize
                                           || event.type == bmmo::session::event_type::Unphysicalize)) {
                    core_.s.held_lifecycle_.push_back(std::move(event));   // the message is move-only
                    continue;
                }
                apply_event(event);
            }
        }

        // The body changes held by drain_due_events, after this frame's rewinds
        // and before the union's pass and the step - where the server applies
        // them - in the server's order (they were sorted by join order): our
        // own changes due at this tick (a beam, a moved birth) go in at our
        // place.
        void apply_held_lifecycle(uint32_t frame_tick) {
            std::vector<bmmo::session_event_msg> held;
            held.swap(core_.s.held_lifecycle_);
            if (!held.empty()) {
                const uint32_t own_rank = join_rank(core_.s.own_id_);
                bool own_placed = false;
                for (const auto& event: held) {
                    if (!own_placed && join_rank(event.player) > own_rank) {
                        live_own_lives(frame_tick);
                        own_placed = true;
                    }
                    apply_event(event);
                }
            }
            live_own_lives(frame_tick);   // applies once; afterwards only retires
        }

        // The echoes of our own events that arrived since the last frame,
        // before this frame's other rewinds (process_own_echo): first those
        // queued before the last SessionAssign, which can only answer the
        // births it set aside, then the rest; then the births whose echo is
        // overdue (expire_own_echoes).
        void process_own_echoes() {
            if (!core_.s.own_echoes_stale_.empty()) {
                std::vector<bmmo::session_event_msg> stale;
                stale.swap(core_.s.own_echoes_stale_);
                for (const auto& msg: stale) process_own_echo(msg, true);
            }
            if (!core_.s.own_echoes_.empty()) {
                std::vector<bmmo::session_event_msg> echoes;
                echoes.swap(core_.s.own_echoes_);
                for (const auto& msg: echoes) process_own_echo(msg, false);
            }
            expire_own_echoes();
        }

        // An echo of one of our own events (protocol 9), counted by what the
        // server did with it: echo=acked/moved/superseded/rejected/missing on
        // the status line.  An ack changes nothing.  Any other verdict goes
        // into the box beside the event as it was sent: the same (player,
        // origin, type) with the server's applied tick and state, which a
        // replay of this file takes over the earlier record (sim_tool keeps
        // the last), so the box replays to the server's world.
        //
        // A Physicalize the server applied later, or never, is moved inside
        // our history (late-player design 3.3, move_own_birth).  A MOVED echo
        // of another type is counted and kept where we stamped it: the own
        // death and the own Sector move are plan steps 22 and 23.  One
        // exception: an own Unphysicalize moved together with the birth of
        // its origin (a beam, a trafo, a respawn's pair) is a pair the server
        // applied late as a whole; the old body lived on at the server until
        // T', which moving the birth alone cannot give, so this client
        // resyncs instead (resync_own_life), whichever echo comes first.
        //
        // `stale`: queued before the last SessionAssign.  It answers a birth
        // that assign set aside (answer_set_aside) and never a record of the
        // new numbering, and asks for no resync of its own: the assign was one.
        void process_own_echo(const bmmo::session_event_msg& msg, bool stale) {
            const bool rejected = (msg.state & bmmo::session::EVENT_STATE_REJECTED) != 0;
            const bool superseded = !rejected && (msg.state & bmmo::session::EVENT_STATE_SUPERSEDED) != 0;
            const bool made = !rejected && !superseded;
            const bool moved = made && msg.tick != msg.origin_tick;
            const bool birth = msg.type == bmmo::session::event_type::Physicalize;
            own_life* life = nullptr;
            if (birth && !stale)
                for (auto& candidate: core_.s.own_life_)
                    if (!candidate.retired && candidate.origin == msg.origin_tick && candidate.ball_type == msg.ball_type
                            && candidate.state == own_life::verdict::sent) {
                        life = &candidate;
                        break;
                    }
            own_life* aside = birth && !life ? find_unanswered(msg.origin_tick, msg.ball_type) : nullptr;
            if (made && !moved) {
                ++core_.s.own_echo_acked_;
                if (life) life->state = own_life::verdict::acked;
                if (aside) answer_set_aside(*aside, true, msg.tick, "acked");
                return;
            }
            const char* verdict = rejected ? "rejected" : superseded ? "superseded" : "moved";
            if (rejected) ++core_.s.own_echo_rejected_;
            else if (superseded) ++core_.s.own_echo_superseded_;
            else ++core_.s.own_echo_moved_;
            if (birth && moved) ++core_.s.own_birth_echo_moved_;
            core_.journal().lifecycle_event(core_.s.own_id_, msg);
            core_.journal().note(std::string("own echo: ") + event_type_name(static_cast<int>(msg.type)) + " stamped "
                         + std::to_string(msg.origin_tick) + " " + verdict + " (applied " + std::to_string(msg.tick) + ")");
            if (!birth) {
                if (moved) ++core_.s.own_moves_other_;
                logf("own %s stamped tick %u echoed %s by the server (applied %u, state %02x) at local tick %u; "
                     "kept where we stamped it (only an own Physicalize is moved here)",
                     event_type_name(static_cast<int>(msg.type)), msg.origin_tick, verdict, msg.tick,
                     static_cast<unsigned>(msg.state), current_tick());
                if (msg.type == bmmo::session::event_type::Unphysicalize && moved && args_.rollback && !stale) {
                    core_.s.own_moved_unphysicalize_.push_back(msg.origin_tick);
                    while (core_.s.own_moved_unphysicalize_.size() > 32) core_.s.own_moved_unphysicalize_.erase(core_.s.own_moved_unphysicalize_.begin());
                    // The birth of the pair already moved inside the history:
                    // the old body's late end is what it missed.
                    for (auto& candidate: core_.s.own_life_)
                        if (!candidate.retired && candidate.origin == msg.origin_tick
                                && candidate.state == own_life::verdict::moved) {
                            resync_own_life(candidate, candidate.made, candidate.applied,
                                            "its Unphysicalize was moved too (an own pair, plan step 22)");
                            break;
                        }
                }
                return;
            }
            if (life) {
                life->state = rejected ? own_life::verdict::rejected
                            : superseded ? own_life::verdict::superseded : own_life::verdict::moved;
                if (std::find(core_.s.own_moved_unphysicalize_.begin(), core_.s.own_moved_unphysicalize_.end(), msg.origin_tick)
                        != core_.s.own_moved_unphysicalize_.end()) {
                    resync_own_life(*life, made, msg.tick, "its Unphysicalize was moved too (an own pair, plan step 22)");
                    return;
                }
                move_own_birth(*life, msg.tick, made);
                return;
            }
            if (aside) {
                answer_set_aside(*aside, made, msg.tick, verdict);
                return;
            }
            // No record: never recorded (no rollback), sent before the last
            // resync and answered by it, or retired after kOwnRecordKeepTicks
            // with no echo.
            const bool before_base = static_cast<int32_t>(msg.origin_tick - core_.s.tick_base_) < 0;
            if (!args_.rollback || stale || before_base) {
                logf("own physicalize stamped %u echoed %s by the server (applied %u) at local tick %u: no record of it, %s",
                     msg.origin_tick, verdict, msg.tick, current_tick(),
                     !args_.rollback ? "no rollback: kept where we stamped it" : "sent before the last resync: nothing to do");
                return;
            }
            const std::string head = "own birth moved " + std::to_string(msg.origin_tick) + " -> "
                                   + (made ? std::to_string(msg.tick) : std::string("never"));
            logf("%s at local tick %u: no record of it (past the history), resync", head.c_str(), current_tick());
            core_.journal().note(head + ": no record of it (past the history), resync");
            ++core_.s.own_moves_resynced_;
            request_resync("own birth moved past the history", true);
        }

        // The answer to a birth set aside (own_unanswered_), which leaves the
        // list.  Given up as missing but made after all: this client took
        // its body out, so it is made again and the client resyncs.  Sent
        // before a resync and never made: the body the scripts made goes and
        // the client resyncs.  The other two need nothing: the body is where
        // the server has it, and the poses are the snapshots'.
        void answer_set_aside(own_life& entry, bool made, uint32_t applied, const char* verdict) {
            own_life life = entry;
            core_.s.own_unanswered_.erase(core_.s.own_unanswered_.begin() + (&entry - core_.s.own_unanswered_.data()));
            const bool missing = life.why == own_life::aside::missing;
            if (missing == made) {
                resync_own_life(life, made, applied, missing ? "made after its echo was given up as missing"
                                                             : "sent before the last resync and never made");
                return;
            }
            logf("own physicalize stamped %u echoed %s by the server (applied %u) at local tick %u: %s, nothing to do",
                 life.origin, verdict, applied, current_tick(),
                 missing ? "taken out already as missing" : "sent before the last resync");
        }

        // An own birth with no echo while the server's tick is
        // kOwnEchoDeadlineTicks past its stamp: the Physicalize never reached
        // the server (the A6 drop variant), or it will be applied so late
        // that the move is past the reach anyway.  Either way the answer is a
        // resync of this client alone, and since the server has no body for
        // it now, the body goes first (resync_own_life with `made` false).
        // The birth is set aside, so an echo that comes after all is still
        // answered (answer_set_aside), and the box gets the event as never
        // made (state REJECTED: sim_tool drops it), so the client journal
        // replays to the server's world; a later echo is journaled after it
        // and wins.
        //
        // The server's tick comes from the relay header, which every relayed
        // datagram carries every tick (plan step 20), and from the snapshots
        // when they run: the server is authoritative with snapshots off too,
        // and a birth it never made must not live on here for good.  The
        // server echoes an event when it accepts it, before it steps that
        // event's applied tick and so before it relays that tick, so an echo
        // that exists is sent ahead of the datagram that passes the deadline;
        // one that loses the race is answered as set aside.
        //
        // The deadline is gated on our uplink (bmmo::session::
        // own_echo_deadline): it counts only once the header's acked shows
        // the server applied a frame of ours at or past the origin, and only
        // kOwnEchoUplinkSettleTicks after that was first seen.  The server's
        // tick moves with the downlink; with the uplink out for longer than
        // the deadline the reliable copy is still queued here, and giving the
        // birth up would resync this client twice (the give-up, then the
        // late copy echoed as made).
        void expire_own_echoes() {
            if (!args_.rollback || core_.s.resync_pending_) return;
            uint32_t server_tick = 0;
            if (!bmmo::session::newest_server_tick(core_.s.acked_header_known_, core_.s.acked_header_tick_, core_.s.have_snapshot_,
                                                   core_.s.last_snapshot_tick_, server_tick))
                return;
            for (auto& life: core_.s.own_life_) {
                if (life.retired || life.what != own_life::kind::birth || life.state != own_life::verdict::sent) continue;
                if (!life.echo_deadline.missing(server_tick, core_.s.acked_header_known_, core_.s.header_acked_, life.origin,
                                                kOwnEchoDeadlineTicks, kOwnEchoUplinkSettleTicks))
                    continue;
                ++core_.s.own_echo_missing_;
                life.state = own_life::verdict::missing;
                own_life aside = life;
                aside.why = own_life::aside::missing;
                aside.aside_at = current_tick();
                core_.s.own_unanswered_.push_back(aside);
                while (core_.s.own_unanswered_.size() > 16) core_.s.own_unanswered_.erase(core_.s.own_unanswered_.begin());
                bmmo::session_event_msg never;
                never.tick = never.origin_tick = life.origin;
                never.type = bmmo::session::event_type::Physicalize;
                never.state = static_cast<uint8_t>(bmmo::session::EVENT_STATE_REJECTED);
                never.ball_type = life.ball_type;
                never.flags = static_cast<uint8_t>(life.spawn ? bmmo::session::PHYSICALIZE_FLAG_SPAWN : 0);
                std::copy(std::begin(life.position), std::end(life.position), never.position);
                std::copy(std::begin(life.rotation), std::end(life.rotation), never.rotation);
                never.recipe = life.recipe;
                core_.journal().lifecycle_event(core_.s.own_id_, never);
                core_.journal().note("own echo: Physicalize stamped " + std::to_string(life.origin) + " missing at the server's tick "
                             + std::to_string(server_tick));
                resync_own_life(life, false, life.origin, "no echo by the server's tick " + std::to_string(server_tick));
            }
        }

        // The owner's answer when its history cannot carry what the server
        // did with an own birth (late-player design 3.3 step 1): a resync of
        // this client alone.  The full snapshot only sets the poses of the
        // bodies both sides have, so the own body set is put right first: a
        // birth the server never made goes, from the records too (no rewind
        // before the assign brings it back); one the server made but this
        // client has taken out is made again.  A lifetime that ended since is
        // left alone: the server's has ended too (its death came after T').
        void resync_own_life(own_life& life, bool made, uint32_t applied, const std::string& why) {
            const uint32_t now = current_tick();
            const char* what = life.what == own_life::kind::beam ? "beam" : "birth";
            const std::string head = std::string("own ") + what + " moved " + std::to_string(life.origin) + " -> "
                                   + (made ? std::to_string(applied) : std::string("never"));
            if (life.what == own_life::kind::birth && own_life_current(life)) {
                bmmo_physics_body_state state{};
                std::string error;
                const bool there = bmmo::physics::get_body_state(engine_->physics(), life.entity.c_str(), state, error);
                if (!made) {
                    core_.s.rollback_.move_birth(life.entity, life.generation, life.origin, life.origin, false);
                    if (there) unmake_own_body(life, now, "never made");
                    core_.s.own_physicalized_ = false;
                    core_.s.own_body_entity_.clear();
                } else if (!there && remake_own_body(life, now, "resync")) {
                    ++core_.s.own_remakes_;
                    core_.s.own_body_entity_ = life.entity;
                }
            }
            life.retired = true;
            ++core_.s.own_moves_resynced_;
            logf("%s at local tick %u: %s, resync", head.c_str(), now, why.c_str());
            core_.journal().note(head + ": " + why + ", resync");
            request_resync("own birth: resync", true);
        }

        // The server made our own birth stamped T at T' > T (`made`), or never
        // (late-player design 3.3).  The history is put where the server's
        // world is: no own body from T to T'-1, the body made again at T' with
        // the impulse index of T', in this replay and in every later one that
        // crosses T' (apply_own_lives).  Steps:
        //
        //   1. a rewind to T-1 must still be possible (inside the reach, T-1
        //      recorded); otherwise this client resyncs itself, alone
        //      (resync_own_life);
        //   2. the live body of a birth goes now (a beam's body is the
        //      anchor's, and the replay takes it out at its stamp), with the
        //      navigation replica shut down first and the entity put at the
        //      captured pose;
        //   3. the records lose the body (rollback_engine::move_birth): from T
        //      to T'-1 there was none, from T' on the replay re-captures it;
        //   4. the rewind to T-1 replays with that body set, and the body is
        //      made at T' by drain_pending_births, at our join rank - or by
        //      the live frame of T' when the client has not reached it yet.
        //
        // A lifetime that ended since (a death or a trafo after the birth)
        // and that the server made: the server's ball lived T'..D, and
        // nothing in a replay could end a re-made one at D (the own death's
        // move is plan step 22), so the client resyncs.  One the server
        // never made is removed from the history from T on: the server had
        // no ball, and neither has the replay.
        void move_own_birth(own_life& life, uint32_t applied, bool made) {
            const uint32_t now = current_tick();
            const uint32_t anchor = life.origin - 1;
            const char* what = life.what == own_life::kind::beam ? "beam" : "birth";
            const std::string reason = std::string("own ") + what + " moved " + std::to_string(life.origin) + " -> "
                                     + (made ? std::to_string(applied) : std::string("never"));
            if (!args_.rollback || static_cast<int32_t>(now - anchor) < 0
                    || now - anchor > core_.s.rollback_.thresholds().max_resim_ticks || !core_.s.rollback_.knows_tick(anchor)) {
                resync_own_life(life, made, applied, "past the reach (depth " + std::to_string(static_cast<int>(now - anchor)) + ")");
                return;
            }
            const bool alive = own_life_current(life);
            if (made && !alive) {
                resync_own_life(life, made, applied, "its life has ended since (plan step 22)");
                return;
            }
            life.made = made;
            life.applied = made ? applied : life.origin;
            if (life.what == own_life::kind::birth && alive) {
                bmmo_physics_body_state state{};
                std::string error;
                if (bmmo::physics::get_body_state(engine_->physics(), life.entity.c_str(), state, error))
                    unmake_own_body(life, now, "moved");
            }
            const size_t changed = core_.s.rollback_.move_birth(life.entity, life.generation, life.origin, life.applied, made);
            if (!made) {
                ++core_.s.own_births_removed_;
                if (life.what == own_life::kind::birth) life.retired = true;   // a beam's stamp still takes the old body out
            }
            // The replay below makes it when it reaches T'; past the live
            // tick the live frame of T' does.
            life.applied_live = !made || static_cast<int32_t>(life.applied - now) <= 0;
            if (life.what == own_life::kind::birth && alive && !life.applied_live) {
                core_.s.own_physicalized_ = false;
                core_.s.own_body_entity_.clear();
            }
            auto input_at = [this](const std::string& entity, uint32_t tick, bmmo::session::input_frame& out) {
                return recorded_input_at(entity, tick, out);
            };
            const live_cells_guard keep_cells(*this);
            if (!core_.s.rollback_.rewind_to(rollback_world(), anchor, now, input_at, reason)) {
                // The body set put back the way the server has it, then the
                // resync (a refusal leaves the world at the live tick).
                resync_own_life(life, made, applied, "the rewind to " + std::to_string(anchor) + " was refused");
                return;
            }
            ++core_.s.life_rewinds_;
            core_.s.life_rewind_depth_ += now - anchor;
            if (made) ++core_.s.own_births_moved_;
            drop_carried_own_replay(anchor, false);
            if (life.what == own_life::kind::birth && !made) {
                core_.s.own_physicalized_ = false;
                core_.s.own_body_entity_.clear();
            }
            logf("own physicalize stamped %u %s by the server at local tick %u: rewound to %u (%zu records changed%s)",
                 life.origin, made ? ("moved to " + std::to_string(applied)).c_str() : "never made", now, anchor, changed,
                 made ? (life.applied_live ? ", made again in the replay" : ", made again live at that tick")
                      : alive ? ", removed from the history" : ", its life has ended since: removed from the history");
            core_.journal().note(reason + ": rewound to " + std::to_string(anchor));
        }

        // A member's place in the server's order of a tick's events: its join
        // order, an unknown player after every known one.
        uint32_t join_rank(uint32_t player) const {
            for (const auto& p: core_.s.players_) if (p.id == player) return static_cast<uint32_t>(p.join_order);
            return 256u;
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
            rewind_for_lifecycle(event.player, event.tick);
        }
        void rewind_for_lifecycle(uint32_t player, uint32_t event_tick) {
            if (!args_.rollback || event_tick == 0) return;
            const uint32_t now = current_tick();
            if (static_cast<int32_t>(event_tick - now) > 0) return;   // not late: nothing to replay
            auto input_at = [this](const std::string& entity, uint32_t tick, bmmo::session::input_frame& out) {
                return recorded_input_at(entity, tick, out);
            };
            const std::string reason = "ball of " + std::to_string(player) + " born at tick "
                                     + std::to_string(event_tick);
            const live_cells_guard keep_cells(*this);
            if (core_.s.rollback_.rewind_to(rollback_world(), event_tick - 1, now, input_at, reason)) {
                ++core_.s.life_rewinds_;
                core_.s.life_rewind_depth_ += now - (event_tick - 1);
                drop_carried_own_replay(event_tick - 1, false);
                return;
            }
            ++core_.s.life_rewinds_missed_;
            logf("lifecycle event of %u stamped tick %u is outside the rollback history at local tick %u: "
                 "the body set disagrees until a snapshot re-anchors it", player, event_tick, now);
        }

        // The remote's mirror goes (an Unphysicalize applied); false when
        // there is no such remote.
        bool unphysicalize_remote(uint32_t player, uint32_t tick) {
            auto it = core_.s.remotes_.find(player);
            if (it == core_.s.remotes_.end()) return false;
            std::string error;
            if (it->second.navigation) bmmo::physics::navigation_destroy(engine_->physics(), it->second.entity.c_str(), error);
            it->second.navigation = false;
            if (it->second.physicalized) {
                ++core_.s.generations_[it->second.entity];   // the body goes: a new lifetime
                unphysicalize_mirror(it->second.entity);
            }
            it->second.physicalized = false;
            logf("remote player %u unphysicalized at tick %u", player, tick);
            return true;
        }

        // A remote's birth.  One stamped in the past, with its tick still in
        // the rollback history, is not made here: the world is rewound to the
        // tick before it and the body is made inside the replay, at its tick,
        // in the world as it stood then (pre_step below).  Made here, at the
        // live tick, its collision bookkeeping is built against the live
        // world - a ball 1.9 m from the spawn it is then put back onto -
        // where the server built it coincident with the balls born the same
        // tick (a zero contact normal, no event; measured: the host's own
        // ball knocked 0.8 m back at the first replay and 11-17 m from the
        // server's through the start pile).
        void make_remote_birth(const bmmo::session_event_msg& event) {
            const uint32_t now = current_tick();
            if (args_.rollback && event.tick != 0 && static_cast<int32_t>(event.tick - now) <= 0
                    && core_.s.rollback_.knows_tick(event.tick - 1)) {
                core_.s.pending_births_.push_back(pending_birth::of(event));
                rewind_for_lifecycle(event);
                // The replay made it at its tick; if it did not get that
                // far, make it now.
                for (auto it = core_.s.pending_births_.begin(); it != core_.s.pending_births_.end();)
                    if (it->player == event.player) {
                        logf("remote physicalize of %u: the replay did not reach tick %u, made live", event.player, event.tick);
                        physicalize_remote(*it, live_first_tick(event.tick));
                        it = core_.s.pending_births_.erase(it);
                    } else ++it;
                return;
            }
            physicalize_remote(pending_birth::of(event), live_first_tick(event.tick));
            rewind_for_lifecycle(event);
        }

        void apply_event(const bmmo::session_event_msg& event) {
            if (event.player == 0 || event.player == core_.s.own_id_) return;
            core_.journal().lifecycle_event(event.player, event);
            std::string error;
            switch (event.type) {
            case bmmo::session::event_type::Physicalize: {
                // A birth older by origin than a death of the same player this
                // client applied already (the death's relay overtook it): the
                // server applies the birth first and the death after it.  In
                // the death's own tick (both re-stamped into one, or the
                // birth's applied tick after it, which the server supersedes
                // and never relays) the two leave no ball: dropped.  Earlier,
                // the ball lived from the birth to the death: made as below,
                // inside a replay, and then the death applied again and
                // rewound for, so nothing is left of it after the death's
                // tick (review of step 16: applied alone, the mirror lived on
                // where the server and every other side had none).
                const auto death = core_.s.remote_last_unphysicalize_.find(event.player);
                const bool overtaken = death != core_.s.remote_last_unphysicalize_.end()
                                    && static_cast<int32_t>(death->second.origin - event.origin_tick) > 0;
                if (overtaken && static_cast<int32_t>(event.tick - death->second.applied) >= 0) {
                    ++core_.s.births_overtaken_;
                    logf("remote physicalize of %u stamped %u (applied %u) is older than its death stamped %u, applied"
                         " at %u already: no ball", event.player, event.origin_tick, event.tick,
                         death->second.origin, death->second.applied);
                    break;
                }
                // The origin, which the guard below compares (design 3.1);
                // never backwards (an older birth relayed late).
                if (auto [last, fresh] = core_.s.remote_last_physicalize_.try_emplace(event.player, event.origin_tick);
                        !fresh && static_cast<int32_t>(event.origin_tick - last->second) > 0)
                    last->second = event.origin_tick;
                if (overtaken) {
                    ++core_.s.births_overtaken_;
                    const applied_death later = death->second;
                    logf("remote physicalize of %u stamped %u (applied %u) is older than its death stamped %u, applied"
                         " at %u already: made, and the death applied again", event.player, event.origin_tick,
                         event.tick, later.origin, later.applied);
                    make_remote_birth(event);
                    if (unphysicalize_remote(event.player, later.applied)) rewind_for_lifecycle(event.player, later.applied);
                    break;
                }
                make_remote_birth(event);
                break;
            }
            case bmmo::session::event_type::Sector:
                // Another member entered a sector: an input to our world too.
                // Stamped into the union, which starts the sector before the
                // step of the stamped tick - this very frame when the relay was
                // on time (drain_due_events runs before the union's tick), the
                // next one when it was late, which the union counts.
                core_.s.sectors_.stamp(event.player, event.sector, event.tick);
                logf("remote player %u entered sector %d, stamped for tick %u%s", event.player, event.sector, event.tick,
                     static_cast<int32_t>(event.tick - current_tick()) <= 0 ? " (LATE: the server started it already)" : "");
                break;
            case bmmo::session::event_type::Unphysicalize: {
                // A death stamped at or before the player's last applied birth
                // is a copy that lost its place (protocol 6); applied, it would
                // take the newer mirror out.  Stamped: compared on origins, as
                // the server does (design 3.1).
                if (const auto last = core_.s.remote_last_physicalize_.find(event.player);
                        bmmo::session::lifecycle_unphysicalize_superseded(
                                event.origin_tick, last != core_.s.remote_last_physicalize_.end(),
                                last == core_.s.remote_last_physicalize_.end() ? 0 : last->second)) {
                    ++core_.s.events_superseded_;
                    return;
                }
                // Newest by origin, with its applied tick: an older birth
                // relayed after it is put back in front of it (above).
                if (auto [death, fresh] = core_.s.remote_last_unphysicalize_.try_emplace(
                            event.player, applied_death{event.origin_tick, event.tick});
                        !fresh && static_cast<int32_t>(event.origin_tick - death->second.origin) > 0)
                    death->second = applied_death{event.origin_tick, event.tick};
                if (!unphysicalize_remote(event.player, event.tick)) return;
                rewind_for_lifecycle(event);
                break;
            }
            default:
                break;
            }
        }

        using pending_birth = bmmo::session::client_core_state::pending_birth;
        // Births deferred into a replay (apply_event): made at their tick,
        // before its step.
        // The event log is muted around them: physicalize_remote bumps the
        // mirror's generation itself, and the replay labels the records it
        // re-captures from the birth tick on with that generation
        // (rollback_world::replay_remotes).  The "created" entry the live
        // frame would drain afterwards bumped it a second time, so the live
        // records after the replay named another lifetime than the replayed
        // ones and none of those could restore the body.
        // In the server's order (session_runner::step): join order, and our
        // own changes due at this tick (a beam's stamp, a moved birth's
        // applied tick: apply_own_lives) at ours - two bodies made in one tick
        // are made in that order and the order changes the world.  A beam's
        // stamp is applied here only once its live frame has run: before that
        // no replay reaches it.
        void drain_pending_births(uint32_t tick) {
            if (core_.s.pending_births_.empty() && !own_lives_due(tick)) return;
            std::vector<pending_birth> due;
            for (auto it = core_.s.pending_births_.begin(); it != core_.s.pending_births_.end();)
                if (it->tick == tick) {
                    due.push_back(*it);
                    it = core_.s.pending_births_.erase(it);
                } else ++it;
            std::stable_sort(due.begin(), due.end(), [this](const pending_birth& a, const pending_birth& b) {
                return bmmo::session::lifecycle_apply_before(join_rank(a.player), a.origin,
                                                             bmmo::session::event_type::Physicalize,
                                                             join_rank(b.player), b.origin,
                                                             bmmo::session::event_type::Physicalize);
            });
            bool own = own_lives_due(tick);
            const uint32_t own_rank = join_rank(core_.s.own_id_);
            for (const auto& birth: due) {
                if (own && join_rank(birth.player) > own_rank) {
                    apply_own_lives(tick, "replay", false);
                    own = false;
                }
                {
                    const event_log_mute mute(*this);
                    physicalize_remote(birth, tick);
                }
                core_.s.mechanism_tracking_.mark_dirty();
            }
            if (own) apply_own_lives(tick, "replay", false);
        }

        // The event log muted for a scope, nesting: a trafo inside a birth made
        // in a replay mutes inside the birth's mute, and must not end it.
        // (The bridge keeps one flag; the script replay's and destroy_body's
        // mutes never overlap these.)
        struct event_log_mute {
            session_client& owner;
            const bool was;
            explicit event_log_mute(session_client& o) : owner(o), was(o.event_log_muted_) {
                owner.event_log_muted_ = true;
                bmmo::physics::set_event_log_muted(true);
            }
            ~event_log_mute() {
                owner.event_log_muted_ = was;
                bmmo::physics::set_event_log_muted(was);
            }
            event_log_mute(const event_log_mute&) = delete;
            event_log_mute& operator=(const event_log_mute&) = delete;
        };

        // The first tick the live run steps a mirror made in this frame (before
        // the step of current_tick() + 1): that tick, or the stamp if it is
        // still ahead.
        uint32_t live_first_tick(uint32_t stamp) const {
            const uint32_t next = current_tick() + 1;
            return stamp != 0 && static_cast<int32_t>(stamp - next) > 0 ? stamp : next;
        }

        // A mirror's body goes, with the event log muted: the caller bumps the
        // generation itself, and the "deleted" entry the live frame drains
        // later bumped it a second time - after a replay had labelled the
        // records of a same-stamp rebirth with the generation in between
        // (beam, or a late death and respawn in one frame), so the live
        // records named another lifetime than the replayed ones.
        void unphysicalize_mirror(const std::string& entity) {
            std::string error;
            {
                const event_log_mute mute(*this);
                bmmo::physics::unphysicalize(engine_->physics(), entity.c_str(), error);
            }
            core_.s.mechanism_tracking_.mark_dirty();
        }

        // The frame a live step of `tick` drives a predicted mirror with: the
        // relayed frame for that tick when the ring has it, else the newest
        // relayed frame (the predictor), else no keys with the navigation off.
        static bmmo::session::input_frame live_drive(const remote_ball& remote, uint32_t tick) {
            const auto exact = remote.inputs.find(tick);
            const bool have = exact != remote.inputs.end() || remote.have_input;
            bmmo::session::input_frame drive = exact != remote.inputs.end() ? exact->second : remote.input;
            if (!have) drive.keys = 0;
            if (!have || !(drive.flags & bmmo::session::INPUT_FLAG_NAV_ACTIVE))
                drive.flags &= static_cast<uint8_t>(~bmmo::session::INPUT_FLAG_NAV_ACTIVE);
            return drive;
        }

        // `first_tick`: the first tick this world steps the body (the stamp
        // inside a replay, live_first_tick otherwise).
        void physicalize_remote(const pending_birth& event, uint32_t first_tick) {
            std::string error;
            {
                CK3dEntity* entity = remote_entity(event.player, event.ball_type, error);
                if (!entity) { logf("remote physicalize: %s", error.c_str()); return; }
                auto& remote = core_.s.remotes_[event.player];
                if (remote.navigation) bmmo::physics::navigation_destroy(engine_->physics(), remote.entity.c_str(), error);
                remote.navigation = false;
                if (remote.physicalized && remote.entity != entity->GetName()) {
                    ++core_.s.generations_[remote.entity];   // a trafo: the old mirror's lifetime ends here
                    unphysicalize_mirror(remote.entity);
                }
                entity->SetWorldMatrix(matrix_from_pose(event.position, event.rotation));
                const auto recipe = to_bridge_recipe(event.recipe);
                int join_order = 63;
                for (const auto& p: core_.s.players_) if (p.id == event.player) join_order = p.join_order;
                const std::string group = "P#" + std::to_string(join_order);
                if (!bmmo::physics::physicalize(engine_->physics(), entity->GetName(), recipe, group.c_str(), error)) {
                    logf("remote physicalize %s: %s", entity->GetName(), error.c_str());
                    return;
                }
                // The mirror's body is new: a new lifetime, so no record of the
                // previous one can restore it (design 9.25).
                ++core_.s.generations_[entity->GetName()];
                remote.entity = entity->GetName();
                remote.ball_type = event.ball_type;
                remote.physicalized = true;
                remote.born_tick = first_tick;
                remote.driven_from = first_tick;
                remote.corrector.clear();
                // Frames relayed before this mirror existed move into its ring:
                // the newest becomes the live predictor, and a rewind that later
                // reads the ring finds them - the replay of a late birth too,
                // which drives the newborn from its stamp on
                // (rollback_world::replay_remotes).  Frames of an earlier
                // lifetime stay: a rewind across the death still drives that
                // body at those ticks.
                {
                    bmmo::session::input_frame newest{};
                    const uint32_t newest_tick = core_.s.pending_relay_.take(event.player, remote.inputs, bmmo::session::kRelayRing, &newest);
                    if (newest_tick && (!remote.have_input || static_cast<int32_t>(newest_tick - remote.input_tick) >= 0)) {
                        remote.input = newest;
                        remote.input_tick = newest_tick;
                        remote.have_input = true;
                    }
                }
                if ((event.flags & bmmo::session::PHYSICALIZE_FLAG_SPAWN) && core_.s.spawn_impulse_ > 0.0f) {
                    const uint32_t idx = bmmo::session::spawn_direction_index(core_.s.seed_, static_cast<uint8_t>(join_order), event.tick);
                    std::string impulse_error;
                    if (!bmmo::physics::push_impulse(engine_->physics(), entity->GetName(), bmmo::session::kSpawnDirectionTable[idx],
                                core_.s.spawn_impulse_, 0, impulse_error))
                        logf("remote spawn impulse for %u: %s", event.player, impulse_error.c_str());
                    else
                        logf("remote spawn impulse index=%u for player %u at tick %u", idx, event.player, event.tick);
                }
                attach_remote_navigation(event.player, remote);
                // The next live step (current_tick() + 1: the birth tick when
                // made live, the tick after the replay when made inside one)
                // is driven by remote.applied, which still held the frame the
                // previous body had when it died - the end-of-frame loop skips
                // a dead mirror.  Driven as that loop would have.
                remote.applied = live_drive(remote, current_tick() + 1);
                logf("remote player %u physicalized (%s) at tick %u%s", event.player, remote.entity.c_str(), event.tick,
                     remote.navigation ? ", predicted" : ", mirrored");
                // (The rewind for a late birth happens in apply_event, once the
                // body is fully set up: the replay only drives and records a
                // remote that is physicalized AND navigated, and the spawn
                // impulse is part of the state the owner had at the stamp tick
                // - rewinding before either left the mirror outside the
                // rollback entirely, undriven and unkicked, and the balls ran
                // away instead of converging.)
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

        // The rollback engine's view of this client's world: the one binding
        // every user of the engine shares (session/rollback_binding.hpp, with
        // the in-process backend physics/physics_direct.hpp), plus what only
        // this client has - its deferred births, its trace switches, its
        // journal - as options.
        bmmo::session::rollback_world rollback_world() {
            physics_direct_.manager = engine_->physics();
            CKIpionManager* physics = physics_direct_.manager;
            bmmo::session::rollback_binding_options o;
            o.delta_ms = 1000.0f / 66.0f;
            // The clock is always read (a restore shifts the calm references it
            // puts back by the live clock's offset); rewound only on
            // --restore-clock.
            o.restore_clock = args_.restore_clock;
            o.savestate_flags = args_.savestate_strict ? bmmo::physics::SAVESTATE_STRICT : 0u;
            o.own_key_codes = own_key_codes_;
            o.own_key_blocks = own_key_blocks_;
            o.own_key_count = own_key_count_;
            // Polling resumed = a rewind replayed: the live frame's own
            // navigation input is re-armed (see rearm_own_navigation).
            o.on_poll_resumed = [this](const std::string&) { rearm_own_navigation(); };
            o.log = [](const std::string& text) { logf("%s", text.c_str()); };
            // Before each replayed tick's scripts and step: the births deferred
            // into this replay (apply_event), made at their tick in the world
            // as it stands then, with the own beam stamped for it among them
            // (drain_pending_births), and the traces.
            o.before_replayed_tick = [this](uint32_t tick) {
                drain_pending_births(tick);
                replay_manager_activation(tick);
                if (!args_.trace_body.empty() && tick >= args_.trace_from && tick <= args_.trace_until)
                    logf("body %s before replayed tick %u: %s", args_.trace_body.c_str(), tick,
                         bmmo::physics::describe_body(engine_->physics(), args_.trace_body.c_str()).c_str());
                if (args_.trace_clock_until && tick >= args_.trace_clock_from && tick <= args_.trace_clock_until)
                    logf("clock before replayed tick %u: %s", tick, describe_clock().c_str());
                if (args_.trace_restore && core_.s.own_physicalized_ && !core_.s.own_body_entity_.empty() && contact_traces_ < 48) {
                    ++contact_traces_;
                    logf("contacts of %s at replayed tick %u, before its step: %s", core_.s.own_body_entity_.c_str(), tick,
                         bmmo::physics::describe_contacts(engine_->physics(), core_.s.own_body_entity_.c_str()).c_str());
                }
            };
            // The mirrors a replayed tick drives: every predicted remote this
            // world has stepped and driven by then (driven_from: the stamp of
            // a birth made inside a replay, never a tick before a live make),
            // with the generation it has now.  A late
            // birth made by before_replayed_tick is not in the records of the
            // window it lands in (they were captured while the player was
            // dead); named here, it is fed its relayed frames from its stamp
            // and recorded from there, instead of coasting on its spawn
            // impulse to the live tick and being parked at that record by
            // every later correction.
            o.replay_remotes = [this](uint32_t tick, std::map<std::string, uint32_t>& out) {
                for (const auto& [id, remote]: core_.s.remotes_) {
                    if (!remote.physicalized || !remote.navigation || static_cast<int32_t>(remote.driven_from - tick) > 0) continue;
                    const auto generation = core_.s.generations_.find(remote.entity);
                    out[remote.entity] = generation == core_.s.generations_.end() ? 0u : generation->second;
                }
            };
            if (args_.replay_scripts) {
                // --replay-scripts: the whole frame, so every script runs for
                // the replayed tick too (an experiment, not the lockstep
                // recipe: the scripts outside the replay's selection are not
                // rewound).
                o.whole_frame = [this]() {
                    std::string error;
                    const bool ok = engine_->tick(error);
                    if (!ok) logf("replay frame: %s", error.c_str());
                    return ok;
                };
            }
            o.on_contacts_restored = [this](const std::string& entity, int made, const std::string& error,
                                            const std::vector<bmmo_physics_contact>& in) {
                if (!error.empty() && contact_restore_logs_++ < 8) logf("contact restore of %s: %s", entity.c_str(), error.c_str());
                if (made > 0 && args_.trace && contact_made_logs_++ < 40) {
                    std::string partners;
                    for (const auto& c: in) partners += std::string(partners.empty() ? "" : ",") + c.partner;
                    logf("contact restore of %s at tick %u: %d re-created of %zu recorded (%s)", entity.c_str(), current_tick(), made,
                         in.size(), partners.c_str());
                }
            };
            // The first eight only: a failure that never clears (a body the
            // capture refuses) would otherwise log every tick.  The engine
            // counts them (stats: savestate_capture_failures).
            o.on_savestate_capture_failed = [this](const std::string& error) {
                if (savestate_get_failures_++ < 8) logf("savestate capture failed: %s", error.c_str());
            };
            o.on_savestate_restored = [this](bool ok, const std::string& error, const bmmo::session::savestate_outcome&) {
                const bmmo::physics::savestate_report& report = physics_direct_.last_report;
                if (!ok && savestate_set_failures_++ < 8) logf("savestate restore failed: %s", error.c_str());
                for (const auto& warning: report.warnings)
                    if (savestate_warning_logs_++ < 8) logf("savestate restore: %s", warning.c_str());
                if (args_.trace)
                    logf("savestate restore at tick %u: %s objects=%u cores=%u collisions=%u contacts=%u systems=%u units=%u "
                         "events=%u hull=%u skipped=%u unknown=%zu dropped=%zu warnings=%zu | clock before {%s} after {%s}", current_tick(),
                         ok ? "ok" : "FAILED", report.objects, report.cores, report.collisions, report.contacts, report.systems, report.units,
                         report.events, report.hull_entries, report.skipped, report.unknown_bodies.size(), report.contacts_dropped.size(),
                         report.warnings.size(), savestate_clock_before_.c_str(), describe_clock().c_str());
            };
            // Every decision the engine takes about a snapshot, straight into
            // the box: no log parsing needed to know what this client did.
            o.on_correction = [this](const bmmo::session::rollback_correction& c) { core_.journal().rollback_correction(c); };

            bmmo::session::rollback_world world = bmmo::session::bind_rollback_world(physics_direct_, o);
            // Diagnostic (headless only: the mod has no blob diff): a replay
            // that does not reproduce the live record's blob is logged with
            // the first three differing fields, the overlap tree aside.
            world.diff_savestate = [](const std::vector<uint8_t>& live, const std::vector<uint8_t>& replayed) {
                // The overlap tree's layout, the cache ring and the unit
                // numbering are left out: a replay that destroys and re-makes a
                // body orders them differently without any value differing (the
                // 7:5 arm is exact with them shifted), and they would hide the
                // physical field this check is for.
                const std::string where = bmmo::physics::diff_savestate(live.data(), live.size(), replayed.data(), replayed.size(), true, 3, true);
                return where.rfind("no difference", 0) == 0 ? std::string() : where;   // bookkeeping only: not a mismatch
            };
            // A body born inside the window is made again inside the replay,
            // at its birth tick, with the recipe and matrix its Physicalize
            // used (plan doc 9): the same insertion into IVP's tree, units and
            // caches the live frame made, where parking and re-posing it gave
            // the tree a different layout for good.  The event log is muted
            // around both: the destroy and the re-creation are not lifetime
            // changes of the body the records describe.
            // --insert-births: the parked body is put where its Physicalize
            // made it at the start of its birth tick (set_body_matrix), its
            // joints and controllers untouched.  Not the default: a beam
            // normalizes the quaternion where the creation does not, so most
            // insertions miss their birth record in the last bit and fall back
            // to the re-pose - a second recheck after the step - and the 7:5
            // arm, exact on 52 of 53 bodies under plain parking, came out off
            // on six (measured 2026-10-22).  The A/B stays.
            if (args_.insert_births) world.insert_birth = [this](const std::string& entity, uint32_t birth_tick) {
                // The balls stay on the parking path (exact there in every arm;
                // the replica's controllers are on them).
                if (entity.rfind("Ball_", 0) == 0 || entity == core_.s.own_body_entity_) return false;
                const local_birth* birth = find_local_birth(entity, birth_tick);
                if (!birth) return false;
                std::string error;
                if (!bmmo::physics::set_body_matrix(engine_->physics(), entity.c_str(), birth->matrix,
                                                    !(birth->recipe.start_frozen || birth->recipe.fixed), error)) {
                    if (recreate_logs_++ < 8) logf("birth: inserting %s at tick %u failed: %s", entity.c_str(), birth_tick, error.c_str());
                    return false;
                }
                return true;
            };
            // The tick a body was created at (its latest birth), for the
            // untracked ones the blob does not know.
            world.birth_tick_of = [this](const std::string& entity) -> uint32_t {
                for (auto it = local_births_.rbegin(); it != local_births_.rend(); ++it)
                    for (const auto& b: it->second) if (b.name == entity) return it->first;
                return 0;
            };
            // The order the live frame created a tick's bodies in: the event
            // log's order, which is the scripts' order.
            world.birth_order = [this](uint32_t tick) {
                std::vector<std::string> names;
                const auto it = local_births_.find(tick);
                if (it != local_births_.end())
                    for (const auto& b: it->second) names.push_back(b.name);
                return names;
            };
            // Plan doc 10 (the default): a mechanism body born inside the
            // window is destroyed at the restore, with its entity put back at
            // the matrix its Physicalize found, and the replayed module script
            // - restored to its anchor state and activated again by the
            // re-issued sector activation - makes it again the live way, with
            // its joints and forces.  The balls keep the parking path.  The
            // event log is muted: the destroy is not a lifetime change of the
            // body the records describe (the replayed Physicalize runs muted
            // inside execute_frame already).
            // A moved own birth (late-player design 3.3) is destroyed here
            // too: its birth record is its applied tick's, where the replay
            // makes it again the server's way (apply_own_lives).  (Under
            // --park-births it is parked through the gap instead, out of
            // collision detection, made again at the applied tick and re-posed
            // onto that tick's record.)
            if (!args_.park_births) {
                world.destroy_body = [this](const std::string& entity, uint32_t birth_tick) {
                    if (const own_life* life = moved_own_life(entity, birth_tick)) {
                        const bool ok = unmake_own_body(*life, current_tick(), "restore");
                        if (!ok && recreate_logs_++ < 8) logf("own body %s could not be destroyed at the restore", entity.c_str());
                        return ok;
                    }
                    if (entity.rfind("Ball_", 0) == 0 || entity == core_.s.own_body_entity_) return false;
                    for (const auto& [id, remote]: core_.s.remotes_) if (remote.entity == entity) return false;
                    const local_birth* birth = find_local_birth(entity, birth_tick);
                    if (!birth) birth = find_local_birth(entity, 0);
                    std::string error;
                    bmmo::physics::set_event_log_muted(true);
                    const bool ok = bmmo::physics::unphysicalize(engine_->physics(), entity.c_str(), error);
                    if (ok) {
                        // The entity back where the live Physicalize found it.
                        // The scene's initial conditions first, the way the
                        // module scripts' own TT Restore IC puts it: the LOCAL
                        // matrix the level file has, so the manager's Set World
                        // Matrix on the module's parent recomputes the same
                        // world matrix bit for bit (writing the recorded world
                        // matrix directly left the balloon's pieces a
                        // micrometre off after that recomputation).  The
                        // recorded matrix is the fallback for an entity the
                        // scene holds no initial conditions for.
                        // The matrix the live Physicalize found: the LOCAL one
                        // for a parented entity (the replayed manager's Set
                        // World Matrix on the parent then recomputes the same
                        // world matrix), the world one otherwise.  (The scene's
                        // initial conditions are NOT it: they are the level
                        // file's poses, before the manager placed the module -
                        // measured: a box 900 m off, the ball fell to its death.)
                        if (birth) {
                            if (auto* ck = CK3dEntity::Cast(engine_->context()->GetObjectByNameAndParentClass(
                                    const_cast<CKSTRING>(entity.c_str()), CKCID_3DENTITY, nullptr))) {
                                VxMatrix matrix;
                                const bool local = birth->has_parent && birth->has_local && ck->GetParent();
                                const float* source = local ? birth->local_matrix : birth->matrix;
                                for (int r = 0; r < 4; ++r)
                                    for (int k = 0; k < 4; ++k) matrix[r][k] = source[r * 4 + k];
                                if (local) ck->SetLocalMatrix(matrix);
                                else ck->SetWorldMatrix(matrix);
                            }
                        }
                    }
                    bmmo::physics::set_event_log_muted(false);
                    if (!ok && recreate_logs_++ < 8) logf("birth: destroying %s at the restore failed: %s", entity.c_str(), error.c_str());
                    return ok;
                };
            }
            if (args_.script_replay && !scripts_.empty()) {
                bmmo::session::script_binding_options so;
                so.context = engine_->context();
                so.scripts = &scripts_;
                so.hooks = &script_hooks_;
                so.scripts_replayed = &scripts_replayed_;
                so.reset_ticks = &script_reset_ticks_;
                so.reset_skips = &script_reset_skips_;
                so.delta_ms = 1000.0f / 66.0f;
                so.diff_detail = args_.trace;
                so.replay_activation = !args_.park_births;
                so.on_restore_failed = [this](const std::string& error) {
                    if (script_restore_logs_++ < 8) logf("script replay: restore failed: %s", error.c_str());
                };
                bmmo::session::bind_script_replay(world, physics_direct_, so);
            }
            // The clock before a restore, for the report line above (--trace).
            if (args_.trace && world.set_savestate) {
                const auto inner = world.set_savestate;
                world.set_savestate = [this, inner](const std::vector<uint8_t>& blob, bmmo::session::savestate_outcome& outcome) {
                    savestate_clock_before_ = describe_clock();
                    return inner(blob, outcome);
                };
            }
            if (args_.trace && !args_.replay_scripts) {
                const auto inner = world.step;
                world.step = [this, inner, physics]() {
                    const bool diag = step_diag_ < 24;
                    if (diag)
                        for (const auto& [id, remote]: core_.s.remotes_)
                            if (remote.navigation)
                                logf("core before step: %s", bmmo::physics::describe_core(physics, remote.entity.c_str()).c_str());
                    const bool ok = inner();
                    if (diag) {
                        ++step_diag_;
                        for (const auto& [id, remote]: core_.s.remotes_)
                            if (remote.navigation)
                                logf("core after step:  %s", bmmo::physics::describe_core(physics, remote.entity.c_str()).c_str());
                    }
                    return ok;
                };
            }
            return world;
        }

        // The resync request and its limiter are the core's (shared-core
        // design E3).
        void request_resync(const char* reason, bool owed = false) { core_.request_resync(reason, owed); }

        // SessionEnd (session_client_core::end): what --hold-event still
        // holds is discarded, said aloud (hooks.ending)...
        void discard_held_events() {
            for (const auto& held: held_events_)
                logf("hold-event: own %s stamped tick %u (held at tick %u) discarded: the session ended first",
                     event_type_name(args_.hold_event_type), held.stamp, held.held_at);
            held_events_.clear();
        }

        // ...and, after the journal closed, the own body guard is lifted and
        // every mirror's navigation and body taken out (legacy_calls::
        // end_session_world).
        void end_session_world() {
            std::string error;
            if (body_guard_) {
                bmmo::physics::set_body_guard(engine_->physics(), false, nullptr, error);
                body_guard_ = false;
                body_guard_entity_.clear();
            }
            for (auto& [id, remote]: core_.s.remotes_) {
                if (remote.navigation) bmmo::physics::navigation_destroy(engine_->physics(), remote.entity.c_str(), error);
                if (remote.physicalized) bmmo::physics::unphysicalize(engine_->physics(), remote.entity.c_str(), error);
            }
        }

        // One line per kWindowTicks ticks (late-player design 5.1 C1): what
        // happened since the previous line, up to the end of frame `tick`.
        // lag[P]=L*N,... is the first-arrival lag histogram of source player
        // P's relayed frames (the status line's semantics: ticks already
        // simulated at arrival, early frames not counted); inrew[P] the input
        // rewinds whose correction named P's ball.  Pairs are count/depth in
        // ticks.  scripts/arena_map_test.py event_report() reads it.
        void report_window(uint32_t tick) {
            // A line closes the window on the first frame of the next index:
            // the ticks t with (t - shift) % 60 == 0 (--window-origin).
            const uint32_t index = (tick + kWindowTicks - args_.window_shift) / kWindowTicks;
            if (core_.s.window_index_ == index) return;
            const bool first = core_.s.window_index_ == 0xFFFFFFFFu;
            core_.s.window_index_ = index;
            window_base now;
            now.inrew = core_.s.input_rewinds_; now.inrew_depth = core_.s.input_rewind_depth_;
            now.life = core_.s.life_rewinds_; now.life_depth = core_.s.life_rewind_depth_;
            now.forced = core_.s.forced_rollbacks_; now.forced_depth = core_.s.forced_rollback_depth_;
            now.snaprb = core_.s.snapshot_rollbacks_; now.snaprb_depth = core_.s.snapshot_rollback_depth_;
            now.snaps = core_.s.snapshots_applied_; now.resyncs_sent = core_.s.resyncs_sent_; now.resyncs_done = core_.s.resyncs_done_;
            now.relay_new = core_.s.relay_new_; now.relay_late = core_.s.relay_late_; now.rebases = rebases_;
            now.subst_recv = core_.s.subst_recv_; now.subst_diff = core_.s.subst_diff_; now.subst_same = core_.s.subst_same_;
            now.subst_bodiless = core_.s.subst_bodiless_; now.subst_late = core_.s.subst_late_;
            now.substrew = core_.s.subst_rewinds_; now.substrew_depth = core_.s.subst_rewind_depth_;
            now.subst_deferred = core_.s.subst_deferred_; now.subst_unseen = core_.s.subst_unseen_;
            now.subst_subsumed = core_.s.subst_subsumed_; now.subst_saved = core_.s.subst_saved_;
            now.lead_adj = core_.s.lead_adjustments_;
            now.echo_acked = core_.s.own_echo_acked_; now.echo_moved = core_.s.own_echo_moved_;
            now.echo_superseded = core_.s.own_echo_superseded_; now.echo_rejected = core_.s.own_echo_rejected_;
            now.echo_missing = core_.s.own_echo_missing_;
            now.birth_moved = core_.s.own_birth_echo_moved_; now.own_moved = core_.s.own_births_moved_;
            now.own_removed = core_.s.own_births_removed_; now.own_resynced = core_.s.own_moves_resynced_;
            const window_base& was = core_.s.window_base_;
            if (!first) {
                auto d = [](uint64_t a, uint64_t b) { return static_cast<unsigned long long>(a - b); };
                char head[400];
                std::snprintf(head, sizeof(head),
                              "window: tick=%u inrew=%llu/%llu life=%llu/%llu forced=%llu/%llu snaprb=%llu/%llu snaps=%llu "
                              "resyncs=%llu/%llu rebases=%d relay=%llu late=%llu",
                              tick, d(now.inrew, was.inrew), d(now.inrew_depth, was.inrew_depth), d(now.life, was.life),
                              d(now.life_depth, was.life_depth), d(now.forced, was.forced), d(now.forced_depth, was.forced_depth),
                              d(now.snaprb, was.snaprb), d(now.snaprb_depth, was.snaprb_depth), d(now.snaps, was.snaps),
                              d(now.resyncs_sent, was.resyncs_sent), d(now.resyncs_done, was.resyncs_done),
                              now.rebases - was.rebases, d(now.relay_new, was.relay_new), d(now.relay_late, was.relay_late));
                std::string text = head;
                // The own correction (late-player design 1.3): subst= as on
                // the status line, this window's share, and substrew= the
                // replays it named as count/depth (a rollback class of its
                // own; inrew= counts the replays a remote's frame named),
                // and substco= the coalescing: deferred frames / unseen
                // diffs / waiting replays another rewind carried /
                // amendments that needed no own replay (the status line's
                // subst_coalesce).
                char subst[240];
                std::snprintf(subst, sizeof(subst), " subst=%llu/%llu/%llu/%llu/%llu/%llu substrew=%llu/%llu substco=%llu/%llu/%llu/%llu",
                              d(now.subst_recv, was.subst_recv), d(now.subst_diff, was.subst_diff),
                              d(now.subst_same, was.subst_same), d(now.subst_bodiless, was.subst_bodiless),
                              d(now.substrew, was.substrew), d(now.subst_late, was.subst_late),
                              d(now.substrew, was.substrew), d(now.substrew_depth, was.substrew_depth),
                              d(now.subst_deferred, was.subst_deferred), d(now.subst_unseen, was.subst_unseen),
                              d(now.subst_subsumed, was.subst_subsumed), d(now.subst_saved, was.subst_saved));
                text += subst;
                // SessionLead (late-player design 2.4): the adjustments
                // applied in this window (lead_adj, the harness's count), the
                // acknowledged offset so far (lead) and the ticks the slew in
                // flight still has to move (slew, signed).
                char lead[96];
                std::snprintf(lead, sizeof(lead), " lead_adj=%llu lead=%d slew=%d", d(now.lead_adj, was.lead_adj),
                              core_.s.lead_offset_, core_.s.lead_slew_.outstanding());
                text += lead;
                // Our own events' echoes (protocol 9): moved= the own births
                // the server applied later than stamped (the harness's moved
                // births), echo=acked/moved/superseded/rejected/missing of every type,
                // and ownlife=moved/removed/resynced what this client did with
                // a moved, superseded or rejected birth (late-player design
                // 3.3): moved inside the history, removed from it, or resynced
                // past the reach.
                char echo[160];
                std::snprintf(echo, sizeof(echo), " moved=%llu echo=%llu/%llu/%llu/%llu/%llu ownlife=%llu/%llu/%llu",
                              d(now.birth_moved, was.birth_moved), d(now.echo_acked, was.echo_acked),
                              d(now.echo_moved, was.echo_moved), d(now.echo_superseded, was.echo_superseded),
                              d(now.echo_rejected, was.echo_rejected), d(now.echo_missing, was.echo_missing),
                              d(now.own_moved, was.own_moved),
                              d(now.own_removed, was.own_removed), d(now.own_resynced, was.own_resynced));
                text += echo;
                for (const auto& [player, hist]: core_.s.window_lag_) {
                    text += " lag[" + std::to_string(player) + "]=";
                    bool comma = false;
                    for (const auto& [lag, n]: hist) {
                        if (comma) text += ',';
                        text += std::to_string(lag) + '*' + std::to_string(n);
                        comma = true;
                    }
                }
                for (const auto& [player, n]: core_.s.window_inrew_src_)
                    text += " inrew[" + std::to_string(player) + "]=" + std::to_string(n);
                logf("%s", text.c_str());
            }
            core_.s.window_base_ = now;
            core_.s.window_lag_.clear();
            core_.s.window_inrew_src_.clear();
        }

        void report() {
            const auto& st = core_.s.corrector_.stats();
            const auto& rs = core_.s.rollback_.stats();
            uint64_t rc = 0, ri = 0, rb = 0, rh = 0;
            for (const auto& [id, remote]: core_.s.remotes_) {
                const auto& rs = remote.corrector.stats();
                rc += rs.compared; ri += rs.ignored; rb += rs.blended; rh += rs.hard;
            }
            double mech_max = 0.0;
            for (const auto& [name, c]: core_.s.mechanism_correctors_) mech_max = std::max(mech_max, c.stats().max_error);
            logf("status: phase=%d session=%u tick=%u assigned=%d held=%d frames=%lld inputs=%llu events=%llu/%llu/%llu echo=%llu/%llu/%llu/%llu/%llu ownlife=%llu/%llu/%llu/%llu/%llu overtaken=%llu snapshots=%llu/%llu/%llu "
                 "life=%llu/%llu inrew=%llu/%llu bodiless=%llu/%llu forced=%llu/%llu own_phys=%d remotes=%zu remote_inputs=%llu relay=%llu/%llu/%llu/%llu lag=%u/%u subst=%llu/%llu/%llu/%llu/%llu/%llu subst_skip=%llu/%llu/%llu subst_missed=%llu subst_coalesce=%llu/%llu/%llu/%llu acked=%u held_total=%u remote_writes=%llu remote_corr=%llu/%llu/%llu/%llu mechanisms=%zu/%zu mech_blend=%llu mech_hard=%llu mech_max_err=%.4f amend_failures=%llu rebases=%d resyncs=%llu/%llu "
                 "lead_adj=%llu lead_offset=%d slew=%d lead_forced=%llu lead_skip=%llu/%llu/%llu "
                 "rollback: snaps=%llu ok=%llu mism=%llu rb=%llu resim=%llu unmatched=%llu far=%llu frozen=%llu imp=%llu ct=%llu/%llu sv=%llu/%llu/%llu births=%llu/%llu/%llu+%llu/%llu/%llu/%llu missing=%llu deferred=%llu activations=%llu scr=%llu/%llu/%llu/%llu/%llu t_rest=%.0f t_rep=%.0f t_max=%.0f t_cap=%.0f/%.0f max_err=%.4f last=%s "
                 "corrections: compared=%llu ignored=%llu blended=%llu hard=%llu unmatched=%llu last_err=%.4f max_err=%.4f "
                 "input: reports=%llu starved=%llu (%.0f%%) worst=%u lead=%.1f",
                 static_cast<int>(core_.s.phase_), core_.s.session_, current_tick(), core_.s.assigned_ ? 1 : 0, core_.s.held_ ? 1 : 0,
                 static_cast<long long>(core_.s.frames_since_anchor_),
                 static_cast<unsigned long long>(core_.s.inputs_sent_), static_cast<unsigned long long>(core_.s.events_sent_),
                 static_cast<unsigned long long>(core_.s.events_received_), static_cast<unsigned long long>(core_.s.event_dups_),
                 // echo=acked/moved/superseded/rejected/missing: our own events as the
                 // server echoed them back (protocol 9).
                 static_cast<unsigned long long>(core_.s.own_echo_acked_), static_cast<unsigned long long>(core_.s.own_echo_moved_),
                 static_cast<unsigned long long>(core_.s.own_echo_superseded_), static_cast<unsigned long long>(core_.s.own_echo_rejected_),
                 static_cast<unsigned long long>(core_.s.own_echo_missing_),
                 // ownlife=moved/removed/resynced/remade/other: own births
                 // moved inside the history, removed from it, resynced past
                 // the reach, the re-makes at an applied tick (live and in
                 // replays), and MOVED echoes of the other types, kept where
                 // stamped (late-player design 3.3).
                 static_cast<unsigned long long>(core_.s.own_births_moved_), static_cast<unsigned long long>(core_.s.own_births_removed_),
                 static_cast<unsigned long long>(core_.s.own_moves_resynced_), static_cast<unsigned long long>(core_.s.own_remakes_),
                 static_cast<unsigned long long>(core_.s.own_moves_other_),
                 // overtaken: older remote births relayed after their death
                 // and put back in front of it (dropped or re-ordered).
                 static_cast<unsigned long long>(core_.s.births_overtaken_),
                 static_cast<unsigned long long>(core_.s.snapshots_received_),
                 static_cast<unsigned long long>(core_.s.snapshots_applied_), static_cast<unsigned long long>(core_.s.snapshots_stale_),
                 static_cast<unsigned long long>(core_.s.life_rewinds_), static_cast<unsigned long long>(core_.s.life_rewinds_missed_),
                 static_cast<unsigned long long>(core_.s.input_rewinds_), static_cast<unsigned long long>(core_.s.input_rewinds_missed_),
                 static_cast<unsigned long long>(core_.s.relay_bodiless_), static_cast<unsigned long long>(core_.s.relay_birth_frozen_),
                 static_cast<unsigned long long>(core_.s.forced_rollbacks_), static_cast<unsigned long long>(core_.s.forced_rollbacks_missed_),
                 core_.s.own_physicalized_ ? 1 : 0, core_.s.remotes_.size(), static_cast<unsigned long long>(core_.s.remote_inputs_received_),
                 static_cast<unsigned long long>(core_.s.relay_new_), static_cast<unsigned long long>(core_.s.relay_dup_),
                 static_cast<unsigned long long>(core_.s.relay_parked_), static_cast<unsigned long long>(core_.s.relay_late_),
                 core_.s.relay_lag_max_, relay_lag_p99(),
                 // subst=recv/diff/same/bodiless/rew/late (late-player design
                 // 1.3): the notices of our own substituted ticks inside the
                 // reach (recv, of which diff were replayed onto the server's
                 // frame, same matched what we played and bodiless had no
                 // ball - a tick not simulated yet at arrival is sorted when
                 // its live frame is), the replays they named (rew), and the
                 // ticks past the reach or never noticed (late, a resync).
                 // subst_skip=dup/stale_epoch/base: re-deliveries, records of
                 // a datagram numbered from another base, and records no
                 // rewind could reach (the base tick, a resync's snapshot).
                 // subst_missed: own replays rewind_to refused (a resync
                 // each; inrew's second number is the remotes' only).
                 // subst_coalesce=deferred/unseen/subsumed/saved: frames on
                 // which a waiting own replay was put off (own_replay_pacer),
                 // diffs no replay could see (amended, never replayed; also
                 // counted in diff), waiting own replays a snapshot,
                 // lifecycle or forced rewind carried, and amendments that
                 // needed no own replay of their own.  recv
                 // can exceed diff + same + bodiless by the notices held
                 // for a tick not simulated yet and dropped by a resync.
                 // acked/held_total: the newest header of our own epoch (0/0
                 // until one arrives).  recv + late + base per epoch is what
                 // the server's S2 lines count.
                 static_cast<unsigned long long>(core_.s.subst_recv_), static_cast<unsigned long long>(core_.s.subst_diff_),
                 static_cast<unsigned long long>(core_.s.subst_same_), static_cast<unsigned long long>(core_.s.subst_bodiless_),
                 static_cast<unsigned long long>(core_.s.subst_rewinds_),
                 static_cast<unsigned long long>(core_.s.subst_late_),
                 static_cast<unsigned long long>(core_.s.subst_dup_), static_cast<unsigned long long>(core_.s.subst_stale_epoch_),
                 static_cast<unsigned long long>(core_.s.subst_base_),
                 static_cast<unsigned long long>(core_.s.subst_rewinds_missed_),
                 static_cast<unsigned long long>(core_.s.subst_deferred_), static_cast<unsigned long long>(core_.s.subst_unseen_),
                 static_cast<unsigned long long>(core_.s.subst_subsumed_), static_cast<unsigned long long>(core_.s.subst_saved_),
                 held_header_current() ? core_.s.held_header_acked_ : 0u, held_header_current() ? core_.s.held_header_total_ : 0u,
                 static_cast<unsigned long long>(core_.s.remote_writes_), static_cast<unsigned long long>(rc),
                 static_cast<unsigned long long>(ri), static_cast<unsigned long long>(rb), static_cast<unsigned long long>(rh),
                 core_.s.mechanism_tracking_.known(), core_.s.mechanism_tracking_.resolved(),
                 static_cast<unsigned long long>(core_.s.mechanism_blends_), static_cast<unsigned long long>(core_.s.mechanism_hard_), mech_max,
                 static_cast<unsigned long long>(core_.s.amend_failures_), rebases_,
                 static_cast<unsigned long long>(core_.s.resyncs_sent_), static_cast<unsigned long long>(core_.s.resyncs_done_),
                 // lead_adj: SessionLeads applied; lead_offset: what their
                 // acks applied in total; slew: ticks the one in flight still
                 // moves; lead_forced: live frames that played the held
                 // frame; lead_skip=dup/stale/void: copies, decisions of
                 // another base, and decisions a resync of ours in flight
                 // voids (late-player design 2.4, 2.8).  `lead=` further
                 // on is the measured lead over the server, not this.
                 static_cast<unsigned long long>(core_.s.lead_adjustments_), core_.s.lead_offset_, core_.s.lead_slew_.outstanding(),
                 static_cast<unsigned long long>(core_.s.lead_forced_played_), static_cast<unsigned long long>(core_.s.lead_dups_),
                 static_cast<unsigned long long>(core_.s.lead_stale_), static_cast<unsigned long long>(core_.s.lead_void_),
                 static_cast<unsigned long long>(rs.snapshots), static_cast<unsigned long long>(rs.matched),
                 static_cast<unsigned long long>(rs.mismatched), static_cast<unsigned long long>(rs.rollbacks),
                 static_cast<unsigned long long>(rs.resim_ticks), static_cast<unsigned long long>(rs.unmatched),
                 static_cast<unsigned long long>(rs.too_far), static_cast<unsigned long long>(rs.frozen),
                 static_cast<unsigned long long>(rs.impulses_restored),
                 static_cast<unsigned long long>(rs.contacts_restored), static_cast<unsigned long long>(rs.contact_restore_failures),
                 static_cast<unsigned long long>(rs.savestates_restored), static_cast<unsigned long long>(rs.savestate_failures),
                 static_cast<unsigned long long>(rs.savestate_capture_failures),
                 static_cast<unsigned long long>(rs.births_inserted), static_cast<unsigned long long>(rs.births_inserted_exact),
                 static_cast<unsigned long long>(rs.birth_insert_mismatches),
                 static_cast<unsigned long long>(rs.births_recreated), static_cast<unsigned long long>(rs.births_recreated_exact),
                 static_cast<unsigned long long>(rs.birth_recreate_mismatches), static_cast<unsigned long long>(rs.birth_recreate_failures),
                 static_cast<unsigned long long>(rs.births_missing),
                 static_cast<unsigned long long>(rs.unknown_rechecks_deferred), static_cast<unsigned long long>(activations_replayed_),
                 static_cast<unsigned long long>(rs.script_restores), static_cast<unsigned long long>(rs.script_restore_failures),
                 static_cast<unsigned long long>(rs.script_mismatches), static_cast<unsigned long long>(rs.script_float_mismatches),
                 static_cast<unsigned long long>(script_reset_skips_),
                 rs.restore_us / 1000.0, rs.replay_us / 1000.0, rs.rewind_us_max / 1000.0,
                 rs.capture_us / 1000.0, rs.capture_us_max / 1000.0,
                 rs.max_error, rs.last_mismatch.c_str(),
                 static_cast<unsigned long long>(st.compared), static_cast<unsigned long long>(st.ignored),
                 static_cast<unsigned long long>(st.blended), static_cast<unsigned long long>(st.hard),
                 static_cast<unsigned long long>(st.unmatched), st.last_error, st.max_error,
                 static_cast<unsigned long long>(core_.s.input_reports_), static_cast<unsigned long long>(core_.s.input_starved_),
                 core_.s.input_reports_ ? 100.0 * static_cast<double>(core_.s.input_starved_) / static_cast<double>(core_.s.input_reports_) : 0.0,
                 core_.s.worst_starved_,
                 core_.s.lead_count_ ? static_cast<double>(core_.s.lead_sum_) / static_cast<double>(core_.s.lead_count_) : 0.0);
        }

        // ------------------------------------------------------------ state
        arguments args_;
        std::unique_ptr<bmmo::sim::headless_engine> engine_;
        bmmo::physics::tick_record record_;
        HSteamNetConnection connection_ = k_HSteamNetConnection_Invalid;
        bool in_room_ = false, join_sent_ = false, ready_sent_ = false, start_sent_ = false;
        int list_retries_ = 0;
        clock_type::time_point last_ready_sent_{};
        uint32_t room_id_ = 0;
        bool ended_ = false;
        clock_type::time_point last_report_ = clock_type::now();
        // --capture-inbound / --replay-inbound (inbound_capture.hpp): the file,
        // the main loop's passes and the steps (load_step() and frame() calls)
        // so far - the frame index of every captured record - and a replay
        // that ended with records left.
        bmmo::sim::inbound_capture inbound_;
        uint64_t flow_pass_ = 0;
        uint64_t flow_steps_ = 0;
        bool inbound_failed_ = false;
        std::FILE* send_trace_ = nullptr;   // --trace-sends

        uint8_t snapshot_interval_ = 2;
        int restore_traces_ = 0;
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
        uint32_t savestate_get_failures_ = 0;                // the IVP savestate: capture and restore failures (logged once per 8)
        uint32_t savestate_set_failures_ = 0;
        uint32_t savestate_warning_logs_ = 0;
        bmmo::physics::physics_direct physics_direct_;         // the binding's in-process backend (session/rollback_binding.hpp)
        std::string savestate_clock_before_;                 // describe_clock() just before the last savestate restore (--trace)
        uint32_t savestate_dump_logs_ = 0;                   // --dump-savestate write failures logged (the first 3)                // report warnings logged so far (the first 8)
        uint32_t compare_scripts_ticks_ = 0, compare_scripts_logs_ = 0;
        uint32_t recipe_parse_logs_ = 0, recreate_logs_ = 0;
        // Every activation of Gameplay_SectorManager the live frames made, by
        // the tick whose frame consumed it: the union's own passes (start_sector
        // before the frame) and the ones scripts outside the replay asked for
        // (Init Ingame at the level start, Event_handler), seen as next-frame
        // flags after their frame.  The replay re-issues them at that tick,
        // before the tick's scripts, with the two IngameParameter cells as they
        // were (plan doc 10).
        struct manager_activation {
            bool reset = false;
            int activate = 0, deactivate = 0;   // IngameParameter[0][1], [0][2]
        };
        std::map<uint32_t, manager_activation> manager_activations_;
        uint64_t activations_replayed_ = 0;
        void record_manager_activation(uint32_t tick, bool reset) {
            manager_activation a;
            a.reset = reset;
            if (CKDataArray* parameters = engine_->data_array("IngameParameter")) {
                parameters->GetElementValue(0, 1, &a.activate);
                parameters->GetElementValue(0, 2, &a.deactivate);
            }
            manager_activations_[tick] = a;
            while (!manager_activations_.empty() && manager_activations_.begin()->first + 4096 < tick)
                manager_activations_.erase(manager_activations_.begin());
        }
        // The three data-array cells the replay rewinds and re-writes (the
        // record's, then the replayed activations') while the LIVE chains that
        // wrote them - Gameplay_Events' crossing, the death chain - are not
        // replayed: kept across a rewind, so the live world ends the frame
        // with the values its own chains left, and only the replay sees the
        // anchor's (the next live pass writes them anyway; this keeps the
        // engine's end-of-replay script check honest about them).
        struct live_cells_guard {
            session_client& client;
            int values[3] = {};
            bool have[3] = {};
            uint64_t rollbacks_before = 0;
            explicit live_cells_guard(session_client& c) : client(c), rollbacks_before(c.core_.s.rollback_.stats().rollbacks) {
                if (client.args_.park_births) return;
                for (int i = 0; i < 3; ++i) have[i] = client.read_cell(i, values[i]);
            }
            ~live_cells_guard() {
                for (int i = 0; i < 3; ++i) if (have[i]) client.write_cell(i, values[i]);
                // A rewind that ended at `now` replaced the live world with the
                // replayed one, which never saw an activation a script outside
                // the replay asked for during the live frame `now` (Init Ingame
                // at the level start, Event_handler): pending as next-frame
                // flags in the live record of `now`, absent from the replayed
                // state.  Re-issued for the frame about to run, as the replay
                // re-issues them inside its window.
                if (client.core_.s.rollback_.stats().rollbacks != rollbacks_before) client.replay_manager_activation(client.current_tick() + 1);
            }
        };
        static const char* cell_array(int i) { return i == 0 ? "CurrentLevel" : "IngameParameter"; }
        static int cell_column(int i) { return i == 0 ? 4 : i; }
        bool read_cell(int i, int& value) {
            CKDataArray* a = engine_->data_array(cell_array(i));
            return a && a->GetElementValue(0, cell_column(i), &value);
        }
        void write_cell(int i, int value) {
            if (CKDataArray* a = engine_->data_array(cell_array(i))) a->SetElementValue(0, cell_column(i), &value, sizeof(int));
        }
        void replay_manager_activation(uint32_t tick) {
            if (args_.park_births) return;
            const auto it = manager_activations_.find(tick);
            if (it == manager_activations_.end()) return;
            CKDataArray* parameters = engine_->data_array("IngameParameter");
            CKBehavior* manager = sector_manager_id_ ? CKBehavior::Cast(engine_->context()->GetObject(sector_manager_id_)) : nullptr;
            if (!parameters || !manager) return;
            parameters->SetElementValue(0, 1, &it->second.activate, sizeof(int));
            parameters->SetElementValue(0, 2, &it->second.deactivate, sizeof(int));
            if (CKScene* scene = engine_->context()->GetCurrentScene()) scene->Activate(manager, it->second.reset ? TRUE : FALSE);
            ++activations_replayed_;
            if (args_.trace_restore && recreate_logs_ < 24)
                logf("replay: sector manager activation re-issued at tick %u (activate %d, deactivate %d%s)", tick,
                     it->second.activate, it->second.deactivate, it->second.reset ? ", reset" : "");
        }
        // Every body this world made, by the tick of the record that first
        // carries it: the Physicalize recipe, collision group and the entity's
        // matrix at that moment (the event log's "recipe" line, engine change
        // #19).  A rewind anchored before that tick re-creates the body from
        // it at that tick (rollback_world::recreate_body).
        struct local_birth {
            std::string name, group;
            bmmo_physics_ball_recipe recipe{};
            float matrix[16] = {};         // the entity's world matrix at the Physicalize
            float local_matrix[16] = {};   // ...and its local one, for a parented entity
            bool has_parent = false, has_local = false;
        };
        std::map<uint32_t, std::vector<local_birth>> local_births_;
        // The birth of `entity` recorded at `tick`, or - tick 0 - its latest.
        const local_birth* find_local_birth(const std::string& entity, uint32_t tick) const {
            if (tick) {
                const auto it = local_births_.find(tick);
                if (it == local_births_.end()) return nullptr;
                for (const auto& b: it->second) if (b.name == entity) return &b;
                return nullptr;
            }
            for (auto it = local_births_.rbegin(); it != local_births_.rend(); ++it)
                for (const auto& b: it->second) if (b.name == entity) return &b;
            return nullptr;
        }
        // "<name>|fixed=|frozen=|coll=|cmc=|fr=|el=|m=|ld=|rd=|mc=x,y,z|surf=|convex=a/b|balls=x,y,z,r/..|concave=a/b|group=|mtx=16 floats"
        static bool parse_recipe_event(const std::string& text, local_birth& out) {
            std::vector<std::string> fields;
            size_t start = 0;
            for (;;) {
                const size_t bar = text.find('|', start);
                fields.push_back(text.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
                if (bar == std::string::npos) break;
                start = bar + 1;
            }
            if (fields.size() < 2 || fields[0].empty()) return false;
            out.name = fields[0];
            std::snprintf(out.recipe.collision_surface, sizeof(out.recipe.collision_surface), "%s", "");
            const auto split = [](const std::string& v, char sep) {
                std::vector<std::string> parts;
                if (v.empty()) return parts;
                size_t at = 0;
                for (;;) {
                    const size_t next = v.find(sep, at);
                    parts.push_back(v.substr(at, next == std::string::npos ? std::string::npos : next - at));
                    if (next == std::string::npos) break;
                    at = next + 1;
                }
                return parts;
            };
            const auto number = [](const std::string& v) { return std::strtod(v.c_str(), nullptr); };
            int matrix_values = 0;
            for (size_t i = 1; i < fields.size(); ++i) {
                const size_t eq = fields[i].find('=');
                if (eq == std::string::npos) continue;
                const std::string key = fields[i].substr(0, eq), value = fields[i].substr(eq + 1);
                if (key == "fixed") out.recipe.fixed = value == "1";
                else if (key == "frozen") out.recipe.start_frozen = value == "1";
                else if (key == "coll") out.recipe.enable_collision = value == "1";
                else if (key == "cmc") out.recipe.calc_mass_center = value == "1";
                else if (key == "fr") out.recipe.friction = static_cast<float>(number(value));
                else if (key == "el") out.recipe.elasticity = static_cast<float>(number(value));
                else if (key == "m") out.recipe.mass = static_cast<float>(number(value));
                else if (key == "ld") out.recipe.linear_damp = static_cast<float>(number(value));
                else if (key == "rd") out.recipe.rot_damp = static_cast<float>(number(value));
                else if (key == "mc") {
                    const auto parts = split(value, ',');
                    for (size_t k = 0; k < 3 && k < parts.size(); ++k) out.recipe.mass_center[k] = static_cast<float>(number(parts[k]));
                } else if (key == "surf") {
                    std::snprintf(out.recipe.collision_surface, sizeof(out.recipe.collision_surface), "%s", value.c_str());
                } else if (key == "convex") {
                    const auto parts = split(value, '/');
                    out.recipe.convex_count = 0;
                    for (const auto& part: parts) {
                        if (out.recipe.convex_count >= BMMO_PHYSICS_MAX_CONVEX) return false;
                        std::snprintf(out.recipe.convex[out.recipe.convex_count++], BMMO_PHYSICS_NAME_SIZE, "%s", part.c_str());
                    }
                } else if (key == "concave") {
                    const auto parts = split(value, '/');
                    out.recipe.concave_count = 0;
                    for (const auto& part: parts) {
                        if (out.recipe.concave_count >= BMMO_PHYSICS_MAX_CONCAVE) return false;
                        std::snprintf(out.recipe.concave[out.recipe.concave_count++], BMMO_PHYSICS_NAME_SIZE, "%s", part.c_str());
                    }
                } else if (key == "balls") {
                    const auto parts = split(value, '/');
                    out.recipe.ball_count = 0;
                    for (const auto& part: parts) {
                        const auto xyzr = split(part, ',');
                        if (xyzr.size() != 4 || out.recipe.ball_count >= BMMO_PHYSICS_MAX_BALLS) return false;
                        for (int k = 0; k < 3; ++k) out.recipe.ball_center[out.recipe.ball_count][k] = static_cast<float>(number(xyzr[k]));
                        out.recipe.ball_radius[out.recipe.ball_count] = static_cast<float>(number(xyzr[3]));
                        ++out.recipe.ball_count;
                    }
                } else if (key == "group") {
                    out.group = value;
                } else if (key == "mtx") {
                    const auto parts = split(value, ',');
                    if (parts.size() != 16) return false;
                    for (size_t k = 0; k < 16; ++k) out.matrix[k] = static_cast<float>(number(parts[k]));
                    matrix_values = 16;
                } else if (key == "lmtx") {
                    const auto parts = split(value, ',');
                    if (parts.size() != 16) return false;
                    for (size_t k = 0; k < 16; ++k) out.local_matrix[k] = static_cast<float>(number(parts[k]));
                    out.has_local = true;
                } else if (key == "parent") {
                    out.has_parent = value == "1";
                }
            }
            return matrix_values == 16;
        }
        uint32_t queue_traces_ = 0;                          // --trace-restore PreSimulate queue lines (the first 24)
        std::string compare_scripts_last_;   // the last logged first difference, so only changes are logged
        int contact_traces_ = 0;                            // --trace-restore: contact lines logged around rewinds
        float spawn_position_[3] = {};
        bool spawn_known_ = false;
        // The own ball's last Physicalize as capture_own_physicalize() saw it
        // before the step: what report_physicalize sends (design 9.10).
        struct {
            bool valid = false;
            uint32_t tick = 0;
            std::string entity;
            VxMatrix pose;
            bool spawn = false;                  // at CurrentLevel[0,3] then: flagged, and kicked
        } own_capture_;
        // The engine_->tick() frame() is running, numbered from 1, and 0
        // outside it - a replayed step runs the PreSimulate pass too, and only
        // the live tick an own_physicalize_callback was armed for may capture.
        uint64_t live_tick_serial_ = 0;
        uint64_t live_ticks_ = 0;
        bool own_group_set_ = false;
        int load_waited_ = 0;
        uint64_t anchor_hash_ = 0;
        bmmo::game::sequencer_state level_sequencers_;
        clock_type::time_point origin_;
        int rebases_ = 0;
        unsigned char lane_keys_[256] = {};                  // --keep-lane: this frame's keys with the steer added
        int lane_steers_ = 0;
        bmmo::game::navigation_graph navigation_;
        bool navigation_known_ = false;
        std::vector<ball_row> ball_rows_;
        uint8_t last_keys_ = 0;
        bool last_nav_active_ = false;
        int trace_frames_ = 0;
        bool beam_done_ = false;
        // How long an own birth waits for its echo: the server's tick (the
        // relay header's, or the snapshots') this far past its stamp with no
        // echo, and the Physicalize either never reached the server or will
        // be applied past the reach - a resync either way, and a never-made
        // body has to go first.  Twice the history (2.9 s): far past every
        // hold arm, so a merely late event's echo always comes first.
        static constexpr uint32_t kOwnEchoDeadlineTicks = 192;
        // ...counted only once the server has applied a frame of ours at or
        // past the birth's origin, and this long after that was first seen
        // (one second for the reliable copy's retransmission).
        static constexpr uint32_t kOwnEchoUplinkSettleTicks = 66;
        // A birth unanswered this long retires anyway (retire_own_lives).
        static constexpr uint32_t kOwnRecordKeepTicks = 512;
        bool event_log_muted_ = false;   // event_log_mute
        static constexpr uint32_t kRelayLagBuckets = bmmo::session::client_core_state::kRelayLagBuckets;
        static constexpr uint32_t kWindowTicks = bmmo::session::client_core_state::kWindowTicks;
        // --hold-event: how many own events of the held type were made, and
        // the one held back until its release time.
        struct held_event {
            std::string bytes;
            uint32_t stamp = 0;
            uint32_t held_at = 0;
            clock_type::time_point release{};
        };
        uint32_t hold_event_seen_ = 0;
        std::vector<held_event> held_events_;
        bool paused_once_ = false;
        bool own_navigation_ = false;
        std::string own_nav_entity_;
        int own_key_codes_[8] = {};
        uint32_t own_key_blocks_[8] = {};
        int own_key_count_ = 0;
        bool body_guard_ = false;
        std::string body_guard_entity_;
        int rollback_diag_ = 0;
        int step_diag_ = 0;
        // The shared session-client core (docs/session-client-core-design.md):
        // its state (design 1.2) is reached as core_.s.<member> until step E11
        // makes it private.  Last, so it is destroyed first, as the journal was.
        bmmo::session::session_client_core core_{make_platform(), make_capabilities()};
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
    // The engine changes the working directory to the game's Bin while it
    // boots: a relative dump directory means the one the client was started
    // in (the harness gives every client its own).
    for (std::string* dir: {&args.dump_savestate_dir, &args.compare_scripts_dir}) {
        if (dir->empty()) continue;
        std::error_code ec;
        const auto absolute = std::filesystem::absolute(*dir, ec);
        if (!ec) *dir = absolute.string();
    }
    role::init_socket();
    session_client client(args);
    {
        std::string command_line;
        for (int i = 0; i < argc; ++i) command_line += (i ? " " : "") + std::string(argv[i]);
        if (!client.open_inbound(command_line)) return 1;
    }
    if (!client.boot()) return 1;
    // A replay has no network: the capture is its server.
    if (!client.replaying_inbound() && !client.connect()) { logf("connect failed"); return 1; }
    client.run();
    const int code = client.exit_code();
    logf("exit code %d", code);
    return code;
}
