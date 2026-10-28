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
#include "process_qos.hpp"
#include "trafo_hooks.hpp"
#include "live_order_trace.hpp"

#include "CKAll.h"
#include "CKIpionManager.h"
#include "PhysicsCallback.h"

#include <entity/version.hpp>
#include <game/menu_driver.hpp>
#include <game/checkpoint_phase.hpp>
#include <game/sector_activation.hpp>
#include <game/sector_deactivate_pin.hpp>
#include <game/sector_cell_split.hpp>
#include <game/sector_placement.hpp>
#include <game/navigation_graph.hpp>
#include <game/script_state.hpp>
#include <game/session_host_support.hpp>
#include <game/trafo_sets.hpp>
#include <message/message_all.hpp>
#include <physics/ball_navigation.hpp>
#include <physics/physics_savestate.hpp>
#include <physics/physics_direct.hpp>
#include <session/client_core.hpp>
#include <session/rollback_binding.hpp>
#include <session/rollback_binding_scripts.hpp>
#include <session/session_host_world.hpp>
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
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
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
        // --remote-births-parked (gap L, L-H6's control arm, step EL7): a
        // mirror born inside a rewind's window keeps the parking path.
        bool remote_births_parked = false;
        // --own-births-parked (step OWNB's control arm): an own birth the
        // scripts made inside a rewind's window keeps the parking path.
        bool own_births_parked = false;
        bool correct = true;
        bool rollback = true;         // design 9.6; --no-rollback falls back to blending
        int boot_ticks = 400;
        int anchor_timeout = 3000;
        int64_t pause_at = -1;        // test knob: stop ticking at this tick ...
        int pause_ms = 0;             // ... for this long (design 9.2 resync path)
        // --resync-at TICK | s<K>+<D> (resync design 7.3, step A3): a resync
        // asked for while the client keeps stepping (the delta > 0 case), at
        // TICK, or D ticks after the tick this client stamped its own entry
        // into sector K for.
        int64_t resync_at = -1;
        int resync_at_sector = 0;
        uint32_t resync_at_offset = 0;
        // --record-by-tick (resync design 7.6): the record's frame of a tick
        // is its offset from the first assigned base, not the frames since
        // the last numbering - a run with a resync then plays the same keys
        // at the same ticks as one without (the server's world must be the
        // same from the same inputs).  Without a resync it is the frame
        // index exactly.
        bool record_by_tick = false;
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
        // --reach N (late-player plan step 12): the rollback's reach pinned
        // at N ticks (clamped to 48..120; history 128 above 48) instead of
        // the per-client reach from the relay lag (0, the default): the
        // arms of open question 1's measurement.
        uint32_t reach_ticks = 0;
        // --body-set-corrections (the control arm of step RC3): a session
        // with its snapshots off compares and rewinds on the full snapshot
        // of each body-set change, as before RC3.  The default (false,
        // step B5RX, the user's "work as if snapshots did not exist") takes
        // it for its names and rows only (core_config::body_set_corrections).
        bool body_set_corrections = false;
        // --accept-protocol N (step BALA, test only): a SessionStart of
        // protocol N is taken as well as this build's - the wire check's
        // replay of a reference set captured before a protocol bump that
        // changed nothing its inbound messages carry (core_config::
        // accept_protocol).  0: none.
        uint32_t accept_protocol = 0;
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
        // --beam2 X Y Z TICK (step T3 harness): a second beam, e.g. a paper
        // ball onto a wood trafo for a paper explosion.
        bool beam2 = false;
        double beam2_position[3] = {};
        uint32_t beam2_tick = 0;
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
        // --keep-lane-dwell N (step RC1c, a measurement aid; rewind-cost
        // design, "Measurement RC1c"): the steer's key, once pressed, is held
        // at least N frames, and once released is not pressed again for N
        // frames - every state of the steer (no key, LEFT, RIGHT) lasts N
        // frames or more, like a player's holds; the deadband and damping
        // are unchanged.  0 or absent: today's steer, byte for byte.
        int keep_lane_dwell = 0;
        // --brake-tap N:P (step RC1d, a measurement aid; rewind-cost design,
        // "Measurement RC1d"): while the drive record holds UP, press DOWN
        // instead of UP for N frames in every P frames of UP - a hard stop
        // tapped while driving.  0 or absent: the record's keys, byte for byte.
        int brake_tap_n = 0;
        int brake_tap_p = 0;
        // --keep-lane-tap N:M (step RC1d, a measurement aid; rewind-cost
        // design, "Measurement RC1d"): while the steer wants a key, press it
        // N frames, then release it M frames, repeating - a rapid-trigger
        // pattern (the dwell is a minimum hold, this a maximum).  0 or
        // absent: the steer's key, byte for byte.
        int keep_lane_tap_n = 0;
        int keep_lane_tap_m = 0;
        // --capture-inbound FILE / --replay-inbound FILE (shared-core design
        // 5.2 (5), inbound_capture.hpp): record every message handled, every
        // connection state change and every wall-clock read of the session
        // flow, by loop pass; or run the loop from such a file, unpaced, with
        // no network and nothing sent.  Off by default.
        std::string capture_inbound;
        std::string replay_inbound;
        // --replay-free-cost-clock (step B5RC, landed as harness tooling at
        // B5RX: a measurement aid for paired replays): with --replay-inbound,
        // the rewind cost's clock reads (rewind_start, rewind_replay,
        // rewind_end) read this
        // replay's own clock and the capture's reads at those sites are
        // passed over, so a build or a rule whose rewinds differ from the
        // capture's replays it to the end.  Those reads decide only the
        // reach's cap: a replay whose cap does not bind keeps the journal
        // of a strict one.  Off: every read is the capture's, in order.
        bool replay_free_cost_clock = false;
        // --trace-sends FILE: one line per payload the session flow sends
        // (tick, Steam flags, size, FNV-1a 64), written in a replay too, where
        // nothing reaches the wire: the replay's check of the sends.  Off by
        // default.
        std::string trace_sends;
        // Step T3 test switches (trafo-replication design 7.3).
        // --no-trafo-cap (H10): the host does not declare the trafo_inputs
        // capability (SessionReady without CAP_TRAFO_INPUTS; a session with
        // the world option refuses it).
        bool no_trafo_cap = false;
        // --ignore-snapshots: every SessionSnapshot dropped unread (snapshots
        // truly off: the server still sends a full one on every body-set
        // change of a snapshots-off session).
        bool ignore_snapshots = false;
        // --trafo-no-resets (H11): the hold's own reset reports are dropped
        // at the host (the activation is cancelled, no input is made) - the
        // owner never resets its pieces, so the server's deadline must.
        bool trafo_no_resets = false;
        // --leave-at TICK, --leave-after-unphys D, --leave-after-trafo D
        // (H8): the client ends its session (and disconnects) at TICK, D
        // ticks after its own ball's first Unphysicalize at or after the
        // --beam tick, or D ticks after it stamped its first own trafo input.
        uint32_t leave_at = 0;
        int64_t leave_after_unphys = -1;
        int64_t leave_after_trafo = -1;
        // --placement-signature HEX (step S5, sector placement design 7.4
        // "mismatch"; a test switch): the host declares the placement
        // capability and reports HEX as its anchor's placement signature.
        // Since step S6 the host computes the world's signature itself
        // (place_sectors) and HEX replaces it in the report: HEX equal to the
        // server's is the matching control, any other value the mismatch
        // the server must end the session on.  The copies are built (or
        // not) as without it.
        bool placement_signature_given = false;
        uint64_t placement_signature = 0;
        // --sector-at SECTOR:TICK (step S6, sector placement design 7.4;
        // repeatable): this client enters SECTOR at TICK - asked for before
        // the frame that steps TICK, through the core's request_sector like
        // --start-sector, so it is stamped the sector lead ahead and sent as
        // this client's own Sector input.  A test switch: like --start-sector
        // it is not checked against the level's sector count.
        std::vector<std::pair<int, uint32_t>> sector_at;
        // A second and later --beam (step S6: --beam is repeatable): each at
        // its tick, once, after the first --beam and --beam2, one per frame.
        struct beam_spec { double position[3] = {}; uint32_t tick = 0; };
        std::vector<beam_spec> more_beams;
    };

    // The record's keys (scripts/arena_map_test.py write_drive_record) are
    // DirectInput codes, the ones the level's Key Event blocks wait for.
    constexpr int kKeyUp = 200, kKeyDown = 208;
    // The steer: push while |error + damping * rate| exceeds the deadband.
    constexpr double kLaneDamping = 0.8;    // seconds of lateral rate weighed against the error
    constexpr double kLaneDeadband = 0.3;   // metres
    constexpr int kLaneSteerOff = -2;       // keep_lane_key: the steer does not act this frame

    void usage() {
        std::puts("usage: BallanceMMOSessionClient --root <game dir> --server <ip:port> [--name N] [--level N]\n"
                  "       [--record <file.bmrc>] [--journal <file.bmjr>] [--join-first | --room <id> | --host --expect N]\n"
                  "       [--seconds S] [--trace] [--no-correct] [--no-rollback] [--pause-at TICK --pause-ms MS | --stall-ms TICK:MS]\n"
                  "       [--resync-at TICK | --resync-at s<SECTOR>+<TICKS>] [--record-by-tick]\n"
                  "       [--hold-event TYPE:N:MS|TYPE:N:drop]   (the N-th own event of TYPE leaves MS ms late, or never)\n"
                  "       [--window-origin TICK]   (the window: lines' 60-tick grid starts at TICK)\n"
                  "       [--reach N]   (the rollback's reach pinned at N ticks, 48..120; default: per client, from the relay lag)\n"
                  "       [--body-set-corrections]   (snapshots off: a body-set change's full snapshot still corrects, as before RC3; the control arm)\n"
                  "       [--accept-protocol N]      (test only: also take a SessionStart of physics protocol N - a reference capture's replay)\n"
                  "       [--start-sector N] [--beam X Y Z TICK] [--beam2 X Y Z TICK] [--keep-lane Z] [--keep-lane-dwell N]\n"
                  "       [--brake-tap N:P]   (while the record holds UP: DOWN instead for N frames in every P)\n"
                  "       [--keep-lane-tap N:M]   (the steer's key pressed N frames, released M, repeating)\n"
                  "       [--trace-body NAME --trace-ticks A B] [--trace-clock A B] [--trace-restore] [--dump-savestate A B DIR] [--compare-scripts DIR] [--park-births] [--insert-births]\n"
                  "       [--no-savestate] [--savestate-strict]   (the IVP savestate restore; off = the per-body restore)\n"
                  "       [--capture-inbound FILE | --replay-inbound FILE]   (record the inbound messages and clock reads; "
                  "replay them, unpaced, sending nothing)\n"
                  "       [--replay-free-cost-clock]   (with --replay-inbound: the rewind cost's clock reads are the replay's own)\n"
                  "       [--trace-sends FILE]   (every payload sent: tick, flags, size, FNV-1a 64; a replay writes it too)\n"
                  "       [--ignore-snapshots] [--no-trafo-cap] [--trafo-no-resets] [--leave-at TICK | --leave-after-unphys D | --leave-after-trafo D]\n"
                  "       [--sector-at SECTOR:TICK]...   (enter SECTOR at TICK, an own Sector input; repeatable; --beam is too)\n"
                  "       [--placement-signature HEX]   (test: report HEX as the anchor's placement signature instead of the world's)");
    }

    // The event type names (--hold-event, the own-echo lines) are the core's
    // since E7 (session/lifecycle.hpp).
    using bmmo::session::event_type_name;

    // TYPE:N:MS or TYPE:N:drop, TYPE a name (physicalize, unphysicalize,
    // sector, finish, bodyrevived) or its number.
    bool parse_hold_event(const std::string& spec, arguments& out) {
        const size_t a = spec.find(':');
        const size_t b = a == std::string::npos ? std::string::npos : spec.find(':', a + 1);
        if (b == std::string::npos) return false;
        std::string type = spec.substr(0, a);
        for (char& c: type) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        out.hold_event_type = -1;
        for (int t = 0; t <= 5; ++t)   // 5: trafo (step T3)
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
            else if (arg == "--keep-lane-dwell") { if (!(v = next())) return false; out.keep_lane_dwell = std::max(0, std::atoi(v)); }
            else if (arg == "--brake-tap") {
                if (!(v = next())) return false;
                const char* colon = std::strchr(v, ':');
                if (!colon) return false;
                out.brake_tap_n = std::max(0, std::atoi(v));
                out.brake_tap_p = std::max(0, std::atoi(colon + 1));
                if (out.brake_tap_n > 0 && out.brake_tap_p <= out.brake_tap_n) return false;
            }
            else if (arg == "--placement-signature") {
                if (!(v = next())) return false;
                char* end = nullptr;
                out.placement_signature = std::strtoull(v, &end, 16);
                if (!end || end == v || *end) return false;
                out.placement_signature_given = true;
            }
            else if (arg == "--keep-lane-tap") {
                if (!(v = next())) return false;
                const char* colon = std::strchr(v, ':');
                if (!colon) return false;
                out.keep_lane_tap_n = std::max(0, std::atoi(v));
                out.keep_lane_tap_m = std::max(0, std::atoi(colon + 1));
                if (out.keep_lane_tap_n > 0 && out.keep_lane_tap_m <= 0) return false;
            }
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
            else if (arg == "--remote-births-parked") out.remote_births_parked = true;
            else if (arg == "--own-births-parked") out.own_births_parked = true;
            else if (arg == "--no-correct") out.correct = false;
            else if (arg == "--no-rollback") out.rollback = false;
            else if (arg == "--boot-ticks") { if (!(v = next())) return false; out.boot_ticks = std::atoi(v); }
            else if (arg == "--pause-at") { if (!(v = next())) return false; out.pause_at = std::atoll(v); }
            else if (arg == "--pause-ms") { if (!(v = next())) return false; out.pause_ms = std::atoi(v); }
            else if (arg == "--record-by-tick") out.record_by_tick = true;
            else if (arg == "--resync-at") {
                if (!(v = next())) return false;
                const std::string spec = v;
                if (!spec.empty() && spec[0] == 's') {
                    const size_t plus = spec.find('+');
                    if (plus == std::string::npos) return false;
                    out.resync_at_sector = std::atoi(spec.substr(1, plus - 1).c_str());
                    out.resync_at_offset = static_cast<uint32_t>(std::atoi(spec.substr(plus + 1).c_str()));
                    if (out.resync_at_sector <= 0) return false;
                } else {
                    out.resync_at = std::atoll(v);
                }
            }
            else if (arg == "--stall-ms") {
                if (!(v = next())) return false;
                const std::string spec = v;
                const size_t colon = spec.find(':');
                if (colon == std::string::npos) return false;
                out.pause_at = std::atoll(spec.substr(0, colon).c_str());
                out.pause_ms = std::atoi(spec.substr(colon + 1).c_str());
                out.stall_named = true;
            }
            else if (arg == "--reach") {
                if (!(v = next())) return false;
                out.reach_ticks = static_cast<uint32_t>(std::max(0, std::atoi(v)));
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
            else if (arg == "--replay-free-cost-clock") out.replay_free_cost_clock = true;
            else if (arg == "--trace-sends") { if (!(v = next())) return false; out.trace_sends = v; }
            else if (arg == "--start-sector") { if (!(v = next())) return false; out.start_sector = std::atoi(v); }
            else if (arg == "--no-trafo-cap") out.no_trafo_cap = true;
            else if (arg == "--ignore-snapshots") out.ignore_snapshots = true;
            else if (arg == "--body-set-corrections") out.body_set_corrections = true;
            else if (arg == "--accept-protocol") { if (!(v = next())) return false; out.accept_protocol = static_cast<uint32_t>(std::strtoul(v, nullptr, 10)); }
            else if (arg == "--trafo-no-resets") out.trafo_no_resets = true;
            else if (arg == "--leave-at") { if (!(v = next())) return false; out.leave_at = static_cast<uint32_t>(std::strtoul(v, nullptr, 10)); }
            else if (arg == "--leave-after-unphys") { if (!(v = next())) return false; out.leave_after_unphys = std::atoll(v); }
            else if (arg == "--leave-after-trafo") { if (!(v = next())) return false; out.leave_after_trafo = std::atoll(v); }
            else if (arg == "--beam2") {
                for (int k = 0; k < 3; ++k) {
                    if (!(v = next())) return false;
                    out.beam2_position[k] = std::atof(v);
                }
                if (!(v = next())) return false;
                out.beam2_tick = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
                out.beam2 = true;
            }
            else if (arg == "--beam") {
                // The first --beam keeps its own fields (--leave-after-unphys
                // counts from its tick); a later one is appended (step S6).
                arguments::beam_spec b;
                for (int k = 0; k < 3; ++k) {
                    if (!(v = next())) return false;
                    b.position[k] = std::atof(v);
                }
                if (!(v = next())) return false;
                b.tick = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
                if (!out.beam) {
                    for (int k = 0; k < 3; ++k) out.beam_position[k] = b.position[k];
                    out.beam_tick = b.tick;
                    out.beam = true;
                } else {
                    out.more_beams.push_back(b);
                }
            }
            else if (arg == "--sector-at") {
                if (!(v = next())) return false;
                const char* colon = std::strchr(v, ':');
                if (!colon) return false;
                const int sector = std::atoi(v);
                if (sector < 1) return false;
                out.sector_at.emplace_back(sector, static_cast<uint32_t>(std::strtoul(colon + 1, nullptr, 10)));
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

    // An entity's world matrix from a Physicalize's pose: host support
    // (game/session_host_support.hpp, moved there at step E9).
    using bmmo::game::matrix_from_pose;

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

    // Step OWNB (SFD (A)): the headless form of BMLPlus's OnPhysicalize, which
    // the retail mod's birth cell rides on (core_host.cpp
    // core_host_on_physicalize): every Physicalize block of the level runs
    // through this function, which lets the client move the own ball's entity
    // to its birth cell BEFORE the block makes the body from the entity's
    // matrix, as the retail host does.  Moving the body afterwards (the old
    // capture_own_physicalize) was a beam of a fresh body: IVP added the 4.2 m
    // to its hull manager and rechecked its overlap element, so every own ball
    // of join order 1-3 began with other collision bookkeeping than the
    // server's clone and every mirror, which are made at the offset pose.  The
    // original function is the prototype's (one for every block); the client
    // is the one session running in this process.
    int own_physicalize_hook(const CKBehaviorContext& context);
    CKBEHAVIORFCT g_physicalize_original = nullptr;
    session_client* g_physicalize_client = nullptr;
    const CKGUID kPhysicalizeGuid(0x7522370e, 0x37ec15ec);   // physics_RT Behaviors/Physicalize.cpp

    class session_client : public role {
        // Needs capture_own_physicalize() (design 9.10); kept private like the
        // rest of the frame-loop internals.
        friend class own_physicalize_callback;
        friend int own_physicalize_hook(const CKBehaviorContext& context);
    public:
        explicit session_client(arguments args) : args_(std::move(args)) {}
        // Step S6: the copies go before the engine (release_placement).
        ~session_client() {
            if (g_physicalize_client == this) g_physicalize_client = nullptr;
            release_placement("the client exits");
        }
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
            release_placement("the engine reboots");
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
            const auto& st = core_.own_correction_stats();
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
                switch (core_.session_phase()) {
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
                if (args_.replay_free_cost_clock) {
                    for (const auto site: {bmmo::sim::wall_site::rewind_start, bmmo::sim::wall_site::rewind_replay,
                                           bmmo::sim::wall_site::rewind_end})
                        inbound_.set_free(site);
                    logf("replay-inbound: the rewind cost's clock reads are this replay's own (--replay-free-cost-clock)");
                }
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
                    // A replay switch with no value (step B5RX): the
                    // harness's clock choice, not the capture's.
                    if (w == "--replay-free-cost-clock") continue;
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
                    if (data.name == args_.name) core_.set_own_id(id);
                logf("logged in (own id %u, %zu online)", core_.own_id(), msg.online_players.size());
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
                // --ignore-snapshots (a test switch, step T3): every snapshot
                // dropped before the core, the server's body-set full ones
                // of a snapshots-off session included - snapshots truly off.
                if (args_.ignore_snapshots) {
                    if (snapshots_ignored_++ == 0) logf("--ignore-snapshots: the server's snapshots are dropped (the first at tick %u)", msg.tick);
                    break;
                }
                core_.on_session_snapshot(msg);
                break;
            }
            case bmmo::SessionResyncState: {
                bmmo::session_resync_state_msg msg;
                if (!fill(msg)) return;
                core_.on_session_resync_state(std::move(msg));
                break;
            }
            case bmmo::SessionGhostPose: {
                bmmo::session_ghost_pose_msg msg;
                if (!fill(msg)) return;
                core_.on_session_ghost_pose(msg);
                break;
            }
            case bmmo::SessionEvent: {
                bmmo::session_event_msg msg;
                if (!fill(msg)) return;
                core_.on_session_event(std::move(msg));
                break;
            }
            case bmmo::SessionRemoteInput: {
                bmmo::session_remote_input_msg msg;
                if (!fill(msg)) return;
                core_.on_session_remote_input(msg);
                break;
            }
            case bmmo::SessionLead: {
                bmmo::session_lead_msg msg;
                if (!fill(msg)) return;
                core_.on_session_lead(msg);
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
        // (shared-core design step E1); the state is private since E11, and
        // this client reads what it needs through the core's accessors.
        using phase = bmmo::session::client_core_state::phase;

        // --------------------------------------------- the trafo (step T3)
        // Trafo-replication design 2.3-2.5, 2.11: the headless platform's
        // half of the trafo as an input, over game/trafo_sets.hpp and the
        // bridge (sim/trafo_hooks.hpp, as the server's world reaches it).

        // The level reference (design 2.4 step 6), once per session, before
        // any original explosion: while the level loads, at the anchor at
        // the latest.
        void capture_trafo_reference(const char* where) {
            if (!core_.trafo_active() || !trafo_registry_.reference().empty()) return;
            bmmo::game::trafo_reference reference;
            std::string error;
            if (!bmmo::game::capture_trafo_reference(engine_->context(), reference, error)) {
                if (std::strcmp(where, "the anchor") == 0 || trafo_reference_logs_++ == 0)
                    logf("trafo: the level reference is not captured at %s yet: %s", where, error.c_str());
                return;
            }
            size_t blocks = 0;
            for (const auto& [name, b]: reference.scripts) blocks += b.size();
            trafo_registry_.set_reference(std::move(reference));
            logf("trafo: level reference captured at %s: 8 scripts, %zu blocks", where, blocks);
        }

        // The anchor's trafo work, before the core builds the sets (design
        // 2.3 "When the hold is armed"): the originals quiesced (a non-zero
        // count is a bug in a headless world), the hold armed, the piece x
        // local-body filter rule on (design 2.10).
        void anchor_trafo() {
            if (!core_.trafo_active()) return;
            CKIpionManager* physics = engine_->physics();
            trafo_hooks_ = bmmo::sim::make_trafo_hooks(physics, [](const std::string& line) { logf("trafo: %s", line.c_str()); });
            // The sets' force controllers by the names every world gives
            // them (physics_savestate.hpp set_controller_alias): a resync's
            // or a join's blob is the server's.
            bmmo::physics::set_controller_alias(physics, [this](const char* name, std::string& alias) {
                return bmmo::game::trafo_controller_alias(trafo_registry_, name, alias);
            });
            capture_trafo_reference("the anchor");
            const auto quiesce = bmmo::game::quiesce_originals(engine_->context(), trafo_hooks_);
            logf("%s%s", quiesce.text().c_str(),
                 quiesce.bodies || quiesce.controllers || quiesce.scripts ? " - NOT EXPECTED in a headless world (a bug)" : "");
            trafo_hold_.arm(engine_->context(), 1000.0f / 66.0f);
            std::string error;
            if (!bmmo::physics::trafo_set_filter(physics, true, error)) logf("trafo: the piece filter rule: %s", error.c_str());
            trafo_on_ = true;
            logf("trafo: inputs on: the originals held (option E %s, the Delayer %u frames), the piece x local-body filter on",
                 core_.sector_lead_ticks() <= bmmo::session::kTrafoOptionEMaxLead ? "on" : "off (lead > 66)",
                 trafo_hold_.delayer_frames());
        }

        // The session's end (design 2.4): every live piece unphysicalized,
        // the winds' controllers deleted, the sets unregistered and
        // destroyed; the hold disarmed.
        void end_trafo_sets() {
            if (!trafo_on_) return;
            const int bodies = bmmo::game::teardown_trafo_sets(engine_->context(), trafo_registry_, trafo_hooks_);
            bmmo::physics::set_controller_alias(engine_->physics(), {});
            trafo_hold_.disarm();
            trafo_on_ = false;
            logf("trafo: the session's sets torn down (%d piece bodies removed)", bodies);
        }

        // Every set's pieces among the explosion pieces (design 4.3:
        // reportable_wakeup, revivable).
        void note_trafo_pieces(uint32_t) {
            bmmo::game::add_trafo_set_pieces(engine_->context(), trafo_registry_, explosion_pieces_);
        }

        void add_trafo_operations(bmmo::session::session_platform& p) {
            p.poll_trafo = [this](uint32_t tick, bool option_e) {
                bmmo::session::trafo_poll out;
                if (!trafo_on_) return out;
                CKContext* context = engine_->context();
                for (const auto& r: trafo_hold_.poll(context, tick, option_e)) {
                    if (args_.trafo_no_resets && bmmo::session::trafo_is_reset(r.flags)) {
                        logf("--trafo-no-resets: the own %s reset cancelled at tick %u is not made an input",
                             bmmo::session::trafo_type_name(r.type), tick);
                        continue;
                    }
                    bmmo::session::trafo_report x;
                    x.flags = r.flags;
                    x.type = r.type;
                    for (int k = 0; k < 3; ++k) x.position[k] = r.position[k];
                    x.predicted_tick = r.predicted_tick;
                    x.source = r.source;
                    out.reports.push_back(std::move(x));
                }
                const auto effect = trafo_hold_.effect_check(context, trafo_hooks_);
                out.leaks = effect.leaks();
                out.first_leak = effect.first;
                return out;
            };
            p.make_trafo_set = [this](uint32_t player, std::string& error) {
                if (!trafo_on_) {
                    error = "the trafo is not an input in this world";
                    return false;
                }
                const auto r = bmmo::game::ensure_trafo_set(engine_->context(), trafo_registry_, player, trafo_hooks_);
                if (!r.ok) {
                    for (const auto& line: r.lines) logf("trafo build %u: %s", player, line.c_str());
                    error = r.error;
                    return false;
                }
                if (r.how == bmmo::game::trafo_ensure::reselected)
                    for (const auto& line: r.lines) logf("trafo: set %u %s", player, line.c_str());
                if (r.how == bmmo::game::trafo_ensure::built)
                    if (const auto* set = trafo_registry_.find(player))
                        logf("trafo: set %u built: %zu objects, %zu scripts, digest %s, value digest %s", player,
                             set->objects.size(), set->scripts.size(), set->digest.c_str(), set->value_digest.c_str());
                note_trafo_pieces(player);
                return true;
            };
            p.run_trafo = [this](const bmmo::session::trafo_input& in, std::string& error) {
                return bmmo::game::run_trafo(engine_->context(), trafo_registry_, trafo_hooks_, in.player, in.flags, in.type,
                                             in.position, in.seed, error);
            };
            p.trafo_final = [this](uint32_t player, uint8_t type, uint32_t tick) {
                bmmo::game::trafo_set_final(trafo_registry_, player, type, tick);
            };
            p.trafo_frame = [this](uint32_t) {
                if (trafo_on_) bmmo::game::trafo_frame(engine_->context(), trafo_registry_, 1000.0f / 66.0f);
            };
            p.trafo_type_set = [this](uint32_t player, uint8_t type) {
                bmmo::session::trafo_type_view view;
                const bmmo::game::trafo_set* set = trafo_registry_.find(player);
                if (!set || type >= bmmo::session::kTrafoTypes) return view;
                view.exists = true;
                for (CK_ID id: set->types[type].pieces) {
                    CKObject* o = engine_->context()->GetObject(id);
                    if (!o || !o->GetName()) continue;
                    view.pieces.emplace_back(o->GetName());
                    if (trafo_hooks_.has_body && trafo_hooks_.has_body(o->GetName())) ++view.bodies;
                }
                return view;
            };
            p.trafo_reconcile = [this](const bmmo::session::trafo_reconcile_spec& spec) {
                bmmo::session::trafo_reconcile_result r;
                CKContext* context = engine_->context();
                if (spec.action == bmmo::session::trafo_reconcile_action::unmake) {
                    r.ok = bmmo::game::unmake_type_set(context, trafo_registry_, trafo_hooks_, spec.player, spec.type,
                                                       r.bodies_removed, r.error);
                    return r;
                }
                // The remake, inside the core's mute of the event log: the
                // pieces' births go into this world's birth log at S (the
                // revive of a later rewind remakes them from these recipes,
                // the event's draws), as a replayed tick's births do.
                bmmo::physics::set_event_log_keep_muted_recipes(true);
                r.ok = bmmo::game::remake_type_set(context, trafo_registry_, trafo_hooks_, spec.player, spec.type, spec.position,
                                                   spec.seed, 1000.0f / 66.0f, r.error);
                bmmo::physics::set_event_log_keep_muted_recipes(false);
                const std::string recipes = bmmo::physics::drain_muted_recipes();
                bmmo::session::parse_event_log(recipes, [&](const std::string& kind, const std::string& text) {
                    bmmo::session::local_birth birth;
                    if (kind != "recipe" || !bmmo::session::parse_recipe_event(text, birth)) return;
                    if (core_.is_ball(birth.name)) return;
                    local_births_.add(spec.birth_tick, std::move(birth));
                });
                return r;
            };
            p.retire_trafo_sets = [this](uint32_t now, uint32_t history) {
                std::vector<uint32_t> deselected;
                if (!trafo_on_) return deselected;
                std::set<uint32_t> before;
                for (const auto& set: trafo_registry_.sets())
                    if (set.selected) before.insert(set.player);
                const size_t sets_before = trafo_registry_.sets().size();
                for (const auto& line: bmmo::game::retire_trafo_sets(engine_->context(), trafo_registry_, trafo_hooks_, now,
                                                                     history, false))
                    logf("trafo: %s", line.c_str());
                // A destroyed set's pieces are no explosion pieces any more
                // (revivable, reportable_wakeup): the originals and the sets
                // still held (review 2 of T3).
                if (trafo_registry_.sets().size() != sets_before) {
                    explosion_pieces_ = bmmo::game::explosion_pieces(engine_->context());
                    bmmo::game::add_trafo_set_pieces(engine_->context(), trafo_registry_, explosion_pieces_);
                }
                for (uint32_t player: before) {
                    const auto* set = trafo_registry_.find(player);
                    if (!set || !set->selected) deselected.push_back(player);
                }
                return deselected;
            };
        }

        // ------------------------------------------- the core's platform
        // The headless platform (shared-core design 2.2): the operations the
        // core calls, each the call this client makes today.  E1 starts it
        // with send, now and log; the steps that move code add the rest (E2:
        // utc_ms, body_state, list_bodies, journal_path; E3: the hooks; E4:
        // write_body, own_driven_entity, bind_world, keep_live_cells; E6: the
        // mirrors' operations and mute_event_log; E7: the own ball's; E8: the
        // sectors'; E9: the frame's, and bind_world with the core's hooks;
        // E10: the reports' counters and hooks).
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
            // The snapshot path's (E4): the body writes of the resync rebuild
            // and the legacy ladder, the own replica's entity, this client's
            // rollback binding, and the live cells kept across a rollback.
            p.write_body = [this](const std::string& entity, const double position[3], const double rotation[4],
                                  const float linear[3], const float angular[3], bool wake, std::string& error) {
                return bmmo::physics::set_body_state(engine_->physics(), entity.c_str(), position, rotation, linear, angular,
                                                     wake, error);
            };
            p.own_driven_entity = [this]() { return own_replica_.active ? own_replica_.entity : std::string(); };
            p.bind_world = [this](const bmmo::session::core_world_hooks& hooks) { return rollback_world(hooks); };
            p.keep_live_cells = [this](const bmmo::session::session_client_core&) {
                auto guard = std::make_shared<bmmo::game::live_cells_guard>(
                        engine_->context(), core_, !args_.park_births, [this](uint32_t tick) { replay_manager_activation(tick); },
                        &manager_activations_);
                return bmmo::session::scope_guard([guard]() mutable { guard.reset(); });
            };
            p.journal_path = [this](const bmmo::session::journal_header&) { return std::filesystem::path(args_.journal); };
            // The mirrors' (E6): the CopyObject clone (remote_entity), its
            // pose, body and spawn kick, its body and navigation taken out,
            // its navigation attached once the graph is known, and the event
            // log's mute.
            p.mirror_entity = [this](uint32_t player, uint8_t ball_type, std::string& error) {
                CK3dEntity* entity = remote_entity(player, ball_type, error);
                mirror_made_entity_ = entity;
                return std::string(entity && entity->GetName() ? entity->GetName() : "");
            };
            p.make_mirror = [this](const bmmo::session::mirror_spec& spec) {
                bmmo::session::mirror_made made;
                // The one mirror_entity found for this spec (the core asks for
                // it first, in the same frame); else the same lookup again.
                CK3dEntity* entity = mirror_made_entity_;
                // A mirror made again inside a replay (step LB) names the
                // entity it had; remote_entity would clone a new one.
                if (!entity || !entity->GetName() || spec.entity != entity->GetName())
                    entity = CK3dEntity::Cast(engine_->context()->GetObjectByNameAndClass(
                            const_cast<CKSTRING>(spec.entity.c_str()), CKCID_3DOBJECT, nullptr));
                if (!entity || !entity->GetName() || spec.entity != entity->GetName()) {
                    std::string error;
                    entity = remote_entity(spec.player, spec.ball_type, error);
                }
                if (!entity || !entity->GetName() || spec.entity != entity->GetName()) {
                    made.error = "no 3D entity named '" + spec.entity + "'";
                    return made;
                }
                // Posed, physicalized and kicked by host support (E9).
                bmmo::physics::physics_direct direct(engine_->physics());
                return bmmo::game::make_mirror_body(direct, entity, spec);
            };
            p.unmake_mirror = [this](const std::string& entity) {
                std::string error;
                bmmo::physics::unphysicalize(engine_->physics(), entity.c_str(), error);
            };
            p.detach_mirror_navigation = [this](const std::string& entity) {
                std::string error;
                bmmo::physics::navigation_destroy(engine_->physics(), entity.c_str(), error);
            };
            p.attach_mirror_navigation = [this](uint32_t player, const std::string& entity, uint8_t ball_type,
                                                std::string& error) {
                return attach_mirror_navigation(player, entity, ball_type, error);
            };
            p.mute_event_log = [](bool muted) { bmmo::physics::set_event_log_muted(muted); };
            // The own ball's (E7): its ball table row and recipe, the
            // PreSimulate capture of its Physicalize, and its body re-made
            // and taken out (own_body_control).
            p.ball_recipe_of = [this](const std::string& entity, bmmo::session::ball_recipe& recipe) {
                const int ball_type = ball_type_of(entity);
                fill_retail_recipe(ball_type, recipe);
                return ball_type;
            };
            p.own_birth_capture = [this](const std::string& entity, uint32_t tick) { return own_birth_capture(entity, tick); };
            p.make_own = [this](const bmmo::session::own_body_spec& spec) { return remake_own_body(spec); };
            p.unmake_own = [this](const std::string& entity, const float position[3], const float rotation[9], uint32_t tick,
                                  bool death, std::string& error) {
                return unmake_own_body(entity, position, rotation, tick, death, error);
            };
            // Step EL8 (gap L, L3; design 9.7): an entity's CK world matrix.
            p.entity_matrix = [this](const std::string& name, float matrix[16]) {
                auto* entity = CK3dEntity::Cast(engine_->context()->GetObjectByNameAndParentClass(
                        const_cast<CKSTRING>(name.c_str()), CKCID_3DENTITY, nullptr));
                if (!entity) return false;
                const VxMatrix& world = entity->GetWorldMatrix();
                for (int r = 0; r < 4; ++r)
                    for (int k = 0; k < 4; ++k) matrix[r * 4 + k] = world[r][k];
                return true;
            };
            // Step BALA: the airship zone over this world (physics_state.hpp);
            // no ghost display (show_ghost stays empty: a headless world draws
            // nothing, and a mirror's CK matrix stays where its body is).
            p.airship_zone = [this](bool enable, const std::string& own_group, double box[6], std::string& error) {
                bmmo::physics::airship_zone_box zone;
                if (!bmmo::physics::airship_zone_set(engine_->physics(), enable, own_group.c_str(), zone, error)) return -1;
                for (int k = 0; k < 3; ++k) {
                    box[k] = zone.min[k];
                    box[3 + k] = zone.max[k];
                }
                return zone.active ? 1 : 0;
            };
            p.airship_zone_contains = [this](const std::string& name) {
                return bmmo::physics::airship_zone_contains(engine_->physics(), name.c_str());
            };
            // Step SPGH: the spawn ghost over this world (physics_state.hpp);
            // no display (show_spawn_ghost stays empty: nothing is drawn).
            p.spawn_ghost = [this](bool enable, bool respawns, double radius, double info[7], std::string& error) {
                bmmo::physics::spawn_ghost_info out;
                if (!bmmo::physics::spawn_ghost_set(engine_->physics(), enable, respawns, radius, out, error)) return -1;
                info[0] = static_cast<double>(out.areas);
                info[1] = out.radius;
                info[2] = static_cast<double>(out.solidified);
                info[3] = static_cast<double>(out.rechecks);
                info[4] = static_cast<double>(out.overlap_waits);
                info[5] = static_cast<double>(out.births);
                info[6] = static_cast<double>(out.resumed);
                return out.active ? 1 : 0;
            };
            p.spawn_ghost_state = [this](const std::string& name) {
                return bmmo::physics::spawn_ghost_state(engine_->physics(), name.c_str());
            };
            p.spawn_ghost_birth = [this](const std::string& group) {
                return bmmo::physics::spawn_ghost_birth(engine_->physics(), group.c_str());
            };
            p.airship_zone_box = [this](double box[6]) {
                bmmo::physics::airship_zone_box zone;
                bmmo::physics::airship_zone_get(engine_->physics(), zone);
                if (!zone.active) return -1;
                for (int k = 0; k < 3; ++k) {
                    box[k] = zone.min[k];
                    box[3 + k] = zone.max[k];
                }
                return zone.ready ? 1 : 0;
            };
            p.set_entity_matrix = [this](const std::string& name, const float matrix[16]) {
                auto* entity = CK3dEntity::Cast(engine_->context()->GetObjectByNameAndParentClass(
                        const_cast<CKSTRING>(name.c_str()), CKCID_3DENTITY, nullptr));
                if (!entity) return false;
                VxMatrix world;
                for (int r = 0; r < 4; ++r)
                    for (int k = 0; k < 4; ++k) world[r][k] = matrix[r * 4 + k];
                entity->SetWorldMatrix(world);
                return true;
            };
            // The sectors' (E8): Gameplay_SectorManager's state (found once,
            // as the union pass found it), one union pass, its activation
            // record for the replay, and the own sector poll.
            p.sector_manager_active = [this]() {
                if (!sector_manager_id_)
                    if (CKBehavior* manager = bmmo::game::find_root_script(engine_->context(), "Gameplay_SectorManager"))
                        sector_manager_id_ = manager->GetID();
                auto* manager = CKBehavior::Cast(engine_->context()->GetObject(sector_manager_id_));
                return manager && manager->IsActive();
            };
            p.run_sector = [this](int sector) { start_sector(sector); };
            p.record_activation = [this](uint32_t tick, bool reset) { record_manager_activation(tick, reset); };
            p.poll_sector = [this](uint32_t tick) { return poll_sector(tick); };
            // Step B1: a pass a re-deriving replay made (or a live one it
            // forgot) in the tick's pass slot (host support), and the
            // activate cell as the replay left it (no bookkeeping here).
            p.set_union_pass = [this](uint32_t tick, int sector) {
                if (sector > 0) manager_activations_.set_pass(tick, sector);
                else manager_activations_.forget_pass(tick);
            };
            p.sector_cell_adopted = [this](int& cell) {
                CKDataArray* parameters = engine_->data_array("IngameParameter");
                return parameters && parameters->GetElementValue(0, 1, &cell);
            };
            // Step A3: the exact resync's - the server's script record
            // localized (host support), the old numbering's activations and
            // script resets forgotten at the re-assign, the births of the
            // bodies the server's state has at S forgotten after the restore,
            // the birth log for the reconcile, the trafo pieces as this
            // world's own.
            p.localize_scripts = [this](const std::vector<uint8_t>& portable, const std::vector<uint8_t>* anchor,
                                        std::vector<uint8_t>& out, std::string& error,
                                        bmmo::session::resync_script_report& report) {
                return bmmo::game::localize_session_scripts(engine_->context(), scripts_, portable, anchor, out, error, report,
                                                            trafo_on_ ? &trafo_registry_.part() : nullptr);
            };
            p.forget_activations_from = [this](uint32_t tick) {
                const size_t activations = manager_activations_.forget_from(tick);
                script_order_.forget_from(tick);   // step RO1: the old numbering's live orders
                const size_t resets = bmmo::game::forget_reset_ticks_from(script_reset_ticks_, tick);
                if (activations || resets)
                    logf("resync: %zu activation tick(s) and %zu script reset(s) of the old numbering from tick %u forgotten",
                         activations, resets, tick);
            };
            p.forget_births_after = [this](uint32_t tick, const std::vector<std::string>& entities) {
                const size_t n = local_births_.forget_after(tick, entities);
                if (n) logf("resync: %zu birth(s) after tick %u of bodies the server's state has at it forgotten", n, tick);
            };
            // Step A5: the level walk's pending activations inside a late
            // joiner's resync window (host support).
            p.forget_pending_activations = [this](uint32_t from, uint32_t to) {
                return manager_activations_.forget_pending(from, to);
            };
            p.birth_tick_of = [this](const std::string& entity) { return local_births_.birth_tick_of(entity); };
            p.unpack_bytes = [](const std::vector<uint8_t>& packed, uint32_t size, std::vector<uint8_t>& out) {
                char* bytes = CKUnPackData(static_cast<int>(size), reinterpret_cast<const char*>(packed.data()),
                                           static_cast<int>(packed.size()));
                if (!bytes) return false;
                out.assign(reinterpret_cast<const uint8_t*>(bytes), reinterpret_cast<const uint8_t*>(bytes) + size);
                CKDeletePointer(bytes);
                return true;
            };
            // Step T3: with the trafo as an input every world has every
            // set's pieces (and no original has a body): none is this
            // world's alone.  Without it, step P's owner-only pieces.
            p.local_only_body = [this](const std::string& entity) { return !trafo_on_ && explosion_pieces_.count(entity) != 0; };
            add_trafo_operations(p);
            // The host hooks (E3): the pacer's origin restarts with the
            // numbering, --hold-event, and its discards at the session's end.
            p.hooks.numbering_restarted = [this](bmmo::session::clock_site site) { origin_ = wall_now(site); };
            // Step A3: the frames a resync hold did not run are not owed.
            p.hooks.resync_hold_ended = [this](std::chrono::steady_clock::duration held) { origin_ += held; };
            p.hooks.filter_event = [this](const bmmo::session_event_msg& event) {
                if (!hold_event(event)) return bmmo::session::event_send_verdict::send;
                return args_.hold_event_drop ? bmmo::session::event_send_verdict::drop
                                             : bmmo::session::event_send_verdict::hold;
            };
            p.hooks.ending = [this]() { discard_held_events(); };
            // The frame's (E9): the platform half of the core's pre_step and
            // post_step (shared-core design 1.3, 2.2), each what frame() did
            // there.  pre_step: the sector resets kept neutralized, the
            // forced rollback after the rewinds, the mirrors driven, a
            // forced own tick, the own Physicalize capture armed.
            p.script_guards = [this](uint32_t, bool resolved) {
                for (const auto& chain: reset_guard_.apply(engine_->context(), resolved))
                    logf("the sector reset in %s (Execute Script -> Gameplay_SectorManager) is neutralized", chain.c_str());
                if (const int again = checkpoint_phase_.keep(engine_->context()))
                    logf("the checkpoint chain's phase cell: %d target(s) handed back to CurrentLevel were moved to the copy again", again);
                // N2 kept, as the retail mod keeps it (step B2SW).
                if (const int again = bmmo::game::repin_checkpoint_deactivation(engine_->context(), deactivate_pins_))
                    logf("the chains' sector deactivation: %d input(s) handed back were re-pinned to 0", again);
                // The chain's own [0][1] kept (step B2SX).
                if (chain_cell_split_.keep(engine_->context()))
                    logf("the checkpoint chain's sector cell: its Set Cell handed back to IngameParameter was moved to the copy again");
            };
            p.hooks.after_rewinds = [this](uint32_t) { maybe_force_rollback(); };
            p.drive = [this](const std::string& entity, const bmmo::session::input_frame& frame, std::string& error) {
                const bool active = (frame.flags & bmmo::session::INPUT_FLAG_NAV_ACTIVE) != 0;
                return bmmo::physics::navigation_input(engine_->physics(), entity.c_str(), frame.keys, frame.cam_right,
                                                       frame.cam_up, frame.cam_dir, active, error);
            };
            p.force_own = [this](const bmmo::session::input_frame& held, bool& ok, std::string& error) {
                if (!own_replica_.active) return std::string();
                const std::string forced_entity = own_replica_.entity;
                ok = bmmo::physics::navigation_poll(engine_->physics(), forced_entity.c_str(), false, own_replica_.key_codes,
                                                    own_replica_.key_blocks, own_replica_.key_count, error)
                  && bmmo::physics::navigation_input(engine_->physics(), forced_entity.c_str(), held.keys,
                                                     held.cam_right, held.cam_up, held.cam_dir,
                                                     (held.flags & bmmo::session::INPUT_FLAG_NAV_ACTIVE) != 0, error);
                return forced_entity;
            };
            p.unforce_own = [this](const std::string& entity, std::string& error) {
                return bmmo::physics::navigation_poll(engine_->physics(), entity.c_str(), true, own_replica_.key_codes,
                                                      own_replica_.key_blocks, own_replica_.key_count, error);
            };
            p.arm_own_capture = [this](uint32_t tick) {
                live_tick_serial_ = ++live_ticks_;
                live_tick_number_ = tick;
                arm_own_physicalize_capture(live_tick_serial_, tick);
            };
            // post_step: the pending manager activation, the navigation
            // graph, the own ball and its upkeep, the event log, the input,
            // the record's mechanisms and script resets, the hash, and the
            // hooks of the record (--dump-savestate), the amend (the traces)
            // and the journal (--trace impulses).
            // The manager's and the trafo scripts' (step P: host support's
            // record_pending, the selection's `activations`).
            p.record_pending_activation = [this](uint32_t tick) {
                manager_activations_.record_pending(engine_->context(), sector_manager_id_, script_activations_, tick);
            };
            // Step RO1: the order this live frame ran the replay's scripts in
            // (host-local, session/script_order.hpp).
            p.record_script_order = [this](uint32_t tick) {
                if (scripts_.empty()) return;
                auto live = bmmo::game::script_replay::live_frame_order(engine_->context(), scripts_);
                // Step RO2's measurement trace (live_order_trace.hpp; inert
                // when BMMO_TRACE_LIVE_ORDER is absent): the order and the
                // movable list by name, on a change - the server world's
                // physics_world::tick logs the same two lines.
                std::string names;
                if (bmmo::sim::live_order_trace_on()) names = bmmo::sim::live_order_names(engine_->context(), live);
                if (script_order_.record(tick, std::move(live)) && !names.empty())
                    logf("live-order t=%u %s", tick, names.c_str());
                if (bmmo::sim::live_order_trace_on()) {
                    static std::string movable_was;
                    std::string movable = bmmo::sim::movable_order_names(bmmo::physics::describe_movable_objects(engine_->physics()));
                    if (movable != movable_was) {
                        logf("movable-order t=%u %s", tick, movable.c_str());
                        movable_was = std::move(movable);
                    }
                }
            };
            p.discover_navigation = [this](uint32_t tick) {
                if (navigation_known_) return false;
                auto graph = bmmo::game::read_navigation_graph(engine_->context());
                bool complete = graph.valid();
                for (const auto& leaf: graph.leaves) complete = complete && leaf.key != 0;
                if (!complete) return false;
                navigation_ = graph;
                navigation_known_ = true;
                logf("navigation keys known at tick %u (%zu leaves)", tick, graph.leaves.size());
                install_physicalize_hook();
                return true;
            };
            p.own_ball = [this]() {
                bmmo::session::own_ball_view own;
                CK3dEntity* ball = current_ball();
                own.entity = ball && ball->GetName() ? ball->GetName() : "";
                own.ball_type = ball_type_of(own.entity);
                std::string error;
                own.physicalized = !own.entity.empty()
                                && bmmo::physics::get_body_state(engine_->physics(), own.entity.c_str(), own.state, error);
                return own;
            };
            p.own_body_upkeep = [this](const bmmo::session::own_ball_view& own) { own_body_upkeep(own); };
            p.own_replica_name = [this]() { return own_replica_.entity; };
            p.drain_body_events = [this](uint32_t tick, const std::function<bool(const bmmo::session::body_event&)>& each) {
                drain_body_events(tick, each);
            };
            // Not a ball (step P: the core's is_ball, not the "Ball_" prefix),
            // not a mirror, and not a trafo explosion piece: no other world
            // has the pieces, so their Physicalize's wake-up is nobody's
            // BodyRevived (game/session_host_support.hpp explosion_pieces).
            p.reportable_wakeup = [this](const std::string& name, const std::string& own_ball) {
                return !core_.is_ball(name) && name != own_ball && name.find("_BMMO_") == std::string::npos
                    && !explosion_pieces_.count(name);
            };
            p.observe_input = [this]() { return observe_input(); };
            p.own_camera_rows = [this](const float (*basis)[3]) {
                if (!own_replica_.active) return;
                std::string error;
                if (!bmmo::physics::navigation_input(engine_->physics(), own_replica_.entity.c_str(), 0, basis[0], basis[1],
                                                     basis[2], false, error))
                    logf("own navigation rows: %s", error.c_str());
                zero_retail_forces();
            };
            p.movable_bodies = [this]() { return movable_body_names(); };
            p.note_script_resets = [this](uint32_t tick) {
                for (CK_ID id: bmmo::game::script_replay::resets_pending(engine_->context(), scripts_)) {
                    script_reset_ticks_[id] = tick;
                    if (script_reset_logs_++ < 12) {
                        auto* beh = CKBehavior::Cast(engine_->context()->GetObject(id));
                        logf("script replay: %s is told to reset at tick %u; a rewind anchored earlier keeps it live",
                             beh && beh->GetName() ? beh->GetName() : "?", tick);
                    }
                }
            };
            p.world_hash = [this](bmmo::physics::world_hash& hash) {
                std::string hash_error;
                return bmmo::physics::capture_world_hash(engine_->physics(), hash, hash_error);
            };
            // Only when a dump or a comparison reads the record before its
            // amend: without the hook the core captures each live record once,
            // at the amend.
            if (!args_.dump_savestate_dir.empty() || !args_.compare_scripts_dir.empty())
                p.hooks.after_record = [this](uint32_t tick) { dump_savestate(tick); };
            p.hooks.after_amend = [this](uint32_t tick) { frame_traces(tick); };
            // E10: the reports.  The core writes the window: line (step 31,
            // after the traces) and the status line at the session's end; this
            // client prints both as one log line each, with its counters.
            p.counters = [this]() { return counters(); };
            p.hooks.window = [](const std::string& text) { log_line(text); };
            p.hooks.status = [](const std::string& text) { log_line(text); };
            p.hooks.after_journal = [this](uint32_t tick) {
                if (args_.trace && tick % 200 == 0) report_queued_impulses(tick);
            };
            // E11: the last two back-calls (legacy_calls, deleted) as hooks:
            // the own body guard lifted at the session's end, after the
            // journal closed, and the movable objects' description for the
            // --trace line after a snapshot rollback.
            p.hooks.ended = [this]() {
                own_body_guard_off();
                end_trafo_sets();   // step T3
            };
            p.hooks.describe_movables = [this]() { return bmmo::physics::describe_movable_objects(engine_->physics()); };
            return p;
        }
        // Declared as each capability's operations arrive (design 2.3).  E3:
        // slew (SessionReady's caps = CAP_LEAD, as this client always sent)
        // and nonblocking_hold (the hold returns to the loop).  E7: every
        // capability the headless client has - own_body_control (make_own,
        // unmake_own), whole_frame with --replay-scripts (read by nothing
        // until E9's bind_world), and the event log's own births.
        bmmo::session::session_capabilities make_capabilities() const {
            bmmo::session::session_capabilities caps;
            caps.own_body_control = true;
            caps.own_body_in_replay = true;   // step EL8 (gap L, L3)
            caps.slew = true;
            caps.nonblocking_hold = true;
            caps.whole_frame = args_.replay_scripts;
            caps.own_lifecycle = bmmo::session::own_lifecycle_source::event_log;
            // Step T3: the per-player trafo piece sets (--no-trafo-cap: H10).
            // Only with the script replay: the sets' scripts are restored and
            // run inside a replay by its binding (bind_script_replay's trafo
            // part), which --no-script-replay does not install - a replay
            // would set the inputs' flags and run nothing (review 2 of T3).
            caps.trafo_inputs = !args_.no_trafo_cap && args_.script_replay;
            // Step S6 (sector placement design 2.4): this host computes the
            // placement signature at every anchor and builds the copies when
            // the session's world option says so (place_sectors).
            caps.placement = true;
            return caps;
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
            c.remote_births_parked = args_.remote_births_parked;
            c.own_births_parked = args_.own_births_parked;
            c.rollback = args_.rollback;
            c.correct = args_.correct;
            c.trace = args_.trace;
            c.own_echoes = args_.rollback;
            c.input_rewind = args_.rollback;
            c.own_corrections = args_.rollback;
            c.window_shift = args_.window_shift;
            c.reach_ticks = args_.reach_ticks;
            c.body_set_corrections = args_.body_set_corrections;
            c.accept_protocol = args_.accept_protocol;
            return c;
        }

        // ------------------------------------------------- inbound capture
        // The wall clock as the session flow reads it: the live clock, written
        // to the capture by `site` when capturing; the captured value when
        // replaying (a read at another site than the capture's next one is a
        // divergence).
        clock_type::time_point wall_now(bmmo::sim::wall_site site) {
            if (inbound_.replaying() && inbound_.is_free(site)) return clock_type::now();
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
                if (args_.replay_free_cost_clock)
                    logf("replay-inbound: %llu captured rewind-cost reads passed over (--replay-free-cost-clock)",
                         static_cast<unsigned long long>(inbound_.free_skipped()));
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
                if (m.id == core_.own_id()) own_ready = m.ready;
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
            // This client's fresh runtime state.
            navigation_known_ = false;
            local_births_.clear();
            manager_activations_.clear();
            script_order_.clear();
            activations_replayed_ = 0;
            deactivate_write_logs_ = 0;
            beam_done_ = false;
            beam2_done_ = false;
            more_beams_done_.assign(args_.more_beams.size(), false);
            sector_at_done_.assign(args_.sector_at.size(), false);
            own_group_set_ = false;
            own_capture_.valid = false;
            scripts_.clear();
            script_activations_.clear();
            script_reset_ticks_.clear();
            reset_guard_.restore(engine_->context());
            checkpoint_held_ = false;
            // The previous session's split handed back, like the reset guard:
            // base.cmo's chain survives a level exit, and a split it still
            // carried would have this anchor's apply refuse.
            checkpoint_phase_.restore(engine_->context());
            // N2's pins handed back the same way (step B2SW): a pin made on
            // top of the previous session's would take its constant for the
            // retail source.
            if (const int left = bmmo::game::unpin_checkpoint_deactivation(engine_->context(), deactivate_pins_))
                logf("the chains' sector deactivation: %d input(s) could not be handed back and stay on their constant", left);
            // The chain's own [0][1] the same way (step B2SX).
            if (!chain_cell_split_.restore(engine_->context()))
                logf("the checkpoint chain's sector cell could not be handed back to IngameParameter; the copy stays");
            sector_manager_id_ = 0;
            // Step T3: the previous session's sets went at its end; the
            // registry and its level reference start over (a reboot below
            // replaces every object they name).
            end_trafo_sets();
            trafo_registry_.clear();
            trafo_registry_.set_reference(bmmo::game::trafo_reference{});
            leave_unphys_tick_ = 0;
            leave_trafo_tick_ = 0;
            // Step S6: the previous session's copies, whatever comes next (a
            // reboot below, or a level load over a level that is not running).
            release_placement("a new session begins");
            if (bmmo::game::script_active(engine_->context(), "Gameplay_Ingame")) {
                // Restart (host started the room again): reboot to the menu.
                // (The mirrors and the relay's bookkeeping were cleared here
                // until E11; core_.begin cleared the map before this branch,
                // so the loop never ran.  Mirrors a SessionEnd did not take
                // out are not taken out here either, as before.)
                logf("session %u: level still loaded, rebooting the engine", core_.session_id());
                if (!boot_engine()) { ended_ = true; return; }
            }
            engine_->clear_keys();
            engine_->request_level(core_.level());
            load_waited_ = 0;
            level_sequencers_.clear();
            level_proximities_.clear();
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
            // has them, recorded on first sight while the level loads; step Q:
            // the proximity countdowns, recorded the same way (one copy for
            // every host, step HU1).
            bmmo::game::capture_level_file_counters(engine_->context(), level_sequencers_, level_proximities_,
                                                    [](const std::string& line) { logf("%s", line.c_str()); });
            // Step T3 (trafo-replication design 2.4 step 6): the 8 original
            // trafo scripts' values as Balls.nmo has them, before any
            // original explosion, for every copy this session builds.
            capture_trafo_reference("the load");
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
            if (!bmmo::physics::reset_session_clock(physics, core_.seed(), error)) { logf("clock reset: %s", error.c_str()); ended_ = true; return; }
            // Design 9.26 / step Q: the mechanisms' Sequencer counters and the
            // proximity blocks' countdown phase are the level file's on every
            // side (one copy for every host, step HU1).
            bmmo::game::restore_level_file_counters(engine_->context(), level_sequencers_, level_proximities_,
                                                    [](const std::string& line) { logf("%s", line.c_str()); });
            // Step S6 (sector placement design 2.4): after the restores (a
            // copy inherits its source's level-file counters), before the
            // hash, the selection and every pass; a refusal ends the session
            // here, as a failed hash does.
            uint64_t placement_signature = 0;
            if (!place_sectors(placement_signature)) { ended_ = true; return; }
            bmmo::physics::world_hash hash;
            if (!bmmo::physics::capture_world_hash(physics, hash, error)) { logf("hash: %s", error.c_str()); ended_ = true; return; }
            if (!bmmo::physics::install_player_collision_filter(physics, "P#", error)) logf("collision filter: %s", error.c_str());
            bmmo::physics::drain_event_log(physics);
            read_ball_rows();
            // Sectors are inputs to the whole session (session/sector_union.hpp):
            // the level's own checkpoint chain no longer runs the manager here
            // (the server's world holds its copy too); the detect block in the
            // frame loop stamps whatever sector the chain writes, --start-sector
            // (design 9.25 harness) is stamped on the first assigned frame, and
            // the union runs the passes - ours and the other members' - before
            // the step of the tick each is due at.  Sector 1 is the level's own
            // on every side, and the anchor hash is unaffected: nothing is
            // activated before the server's world has.  The core sets both
            // (shared-core design E8: session_client_core::anchor, from the
            // report); the cell is read here, where it always was.
            int sector_cell = 0;
            if (CKDataArray* parameters = engine_->data_array("IngameParameter"))
                parameters->GetElementValue(0, 1, &sector_cell);
            // The script replay (game/script_replay.hpp): the mechanism scripts
            // whose state travels with the rollback records, and the hooks
            // that carry the force blocks' controllers through the bridge.
            // The session core's selection (step P): the trafo scripts with
            // the mechanism scripts and the sector manager.
            // Step T3: with the trafo as an input the originals never run
            // and are not selected; the sets' scripts travel beside the
            // selection as the trafo part (trafo-replication design 4.3).
            scripts_ = args_.script_replay ? bmmo::game::session_script_selection(engine_->context(), core_.trafo_active())
                                           : bmmo::game::script_replay::selection{};
            script_activations_ = bmmo::game::session_script_activations(engine_->context(), scripts_);
            // The trafo explosion pieces (step P): never a BodyRevived.
            explosion_pieces_ = bmmo::game::explosion_pieces(engine_->context());
            // The force hooks over this engine (game/host_backend.hpp, step HU1:
            // one copy for every host).
            script_hooks_ = bmmo::game::force_hooks_for(bmmo::physics::physics_direct(engine_->physics()));
            anchor_trafo();
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
                         static_cast<unsigned>(core_.sector_lead_ticks()));
                else
                    logf("the level's checkpoint sector activation was NOT held (%s): crossings activate here at "
                         "once and are reported after the fact - this client is out of lockstep on every sector it enters",
                         hold_error.c_str());
            }
            {
                // N2 (step B2SW, game/sector_deactivate_pin.hpp): the held
                // chains' deactivate cell pinned to 0, as the retail mod does,
                // so an own crossing in the frame of a union pass deactivates
                // nothing in this world either.
                std::string report;
                const int pinned = bmmo::game::pin_checkpoint_deactivation(engine_->context(), report, &deactivate_pins_);
                logf("the chains' sector deactivation (IngameParameter[0][2]) pinned to 0 in %d of 2 (%s)", pinned,
                     report.c_str());
            }
            {
                // The chain's own phase cell (game/checkpoint_phase.hpp): its
                // writes no longer reach the cell the sector's modules branch on.
                const std::string trouble = checkpoint_phase_.apply(engine_->context());
                if (trouble.empty())
                    logf("the checkpoint chain's phase cell is split: %s", checkpoint_phase_.describe().c_str());
                else
                    logf("the checkpoint chain's phase cell was NOT split (%s): a crossing 3 ticks after a sector pass "
                         "can cancel that pass's module Physicalize on this client", trouble.c_str());
            }
            if (checkpoint_held_) {
                // Step B2SX (game/sector_cell_split.hpp): the held chain's
                // [0][1] write goes to a private cell, which the own sector
                // poll reads; the manager reads only the union's passes, so an
                // own crossing of K in the frame of a pass of J activates J.
                const std::string trouble = chain_cell_split_.apply(engine_->context());
                if (trouble.empty())
                    logf("the checkpoint chain's sector cell is split: %s", chain_cell_split_.describe().c_str());
                else
                    logf("the checkpoint chain's sector cell was NOT split (%s): an own crossing in the frame of a pass of "
                         "another sector makes the manager activate the crossed one here", trouble.c_str());
            }
            if (args_.start_sector > 0) {
                // Before the assignment: pending until the first assigned
                // frame (session_client_core::request_sector, E8).
                core_.request_sector(args_.start_sector, "--start-sector");
                logf("--start-sector %d will be stamped %u ticks ahead of the first assigned frame", args_.start_sector,
                     static_cast<unsigned>(core_.sector_lead_ticks()));
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
            report.sector_cell = sector_cell;
            // The world's signature (step S6); --placement-signature (the
            // step S5 test switch, design 7.4 "mismatch") replaces it.
            report.placement = args_.placement_signature_given ? args_.placement_signature : placement_signature;
            if (args_.placement_signature_given)
                logf("--placement-signature: reporting %016llx instead of the world's %016llx",
                     static_cast<unsigned long long>(args_.placement_signature),
                     static_cast<unsigned long long>(placement_signature));
            core_.anchor(report);
        }

        // The sector placement of this world (docs/sector-placement-design.md
        // 2.3, 2.4; step S6): the core decides (placement_active: the world
        // option and the capability); the host logic - the apply with its
        // hot-source hooks, the option-off release policy and the signature -
        // is game/sector_placement.hpp's place_at_anchor, the one copy the
        // server's physics_world::place_sectors calls too (step HU1).  False:
        // the anchor is refused (logged).
        bool place_sectors(uint64_t& signature) {
            std::string error;
            if (bmmo::game::place_at_anchor(placement_, engine_->context(), bmmo::physics::physics_direct(engine_->physics()),
                                            core_.placement_active(), -1,
                                            [](const std::string& line) { logf("%s", line.c_str()); }, signature, error))
                return true;
            logf("%s", error.c_str());
            return false;
        }

        // The copies this process made, released before the engine that
        // holds them goes (sector placement design 2.3, As built (step S2):
        // the context reset destroys a copy's objects in CK's hash order, so
        // a P_Extra copy must be torn down root first).
        // The release is game/sector_placement.hpp's release_placement (step
        // S10, one helper for the server's world, this host and the retail
        // core host).
        void release_placement(const char* why) {
            if (!engine_) return;
            bmmo::game::release_placement(placement_, engine_->context(), why,
                                          [](const std::string& line) { logf("%s", line.c_str()); });
        }

        // hold_sector, request_sector and the sector lead moved into the
        // shared core at E8 (client_core_frame.cpp).  What stays is their
        // platform half (make_platform: sector_manager_active, run_sector,
        // record_activation, poll_sector).

        // One pass of the sector union: deactivate nothing, activate `sector`,
        // run Gameplay_SectorManager from its start - the writes the server's
        // update_sectors() makes (platform run_sector).
        void start_sector(int sector) {
            CKDataArray* parameters = engine_->data_array("IngameParameter");
            CKBehavior* manager = bmmo::game::find_root_script(engine_->context(), "Gameplay_SectorManager");
            if (!parameters || !manager) {
                logf("--start-sector %d: IngameParameter or Gameplay_SectorManager is missing", sector);
                return;
            }
            bmmo::game::run_sector_pass(engine_->context(), parameters, manager, sector);   // step HU1: one copy
        }

        // The own sector poll's platform half (platform poll_sector, E8): a
        // moved resetpoint (the own ball's crossing; logged with where the
        // ball is), the sector cell the level's scripts last wrote, and
        // whether the checkpoint chain's manager run is held.  The core
        // decides what the crossing is (session_client_core::poll_own_sector).
        bmmo::session::sector_poll poll_sector(uint32_t tick) {
            bmmo::session::sector_poll poll;
            const VxVector reset = level_resetpoint();
            const bool resetpoint_moved = reset != resetpoint_seen_;
            if (resetpoint_moved) {
                resetpoint_seen_ = reset;
                VxVector at(0.0f, 0.0f, 0.0f);
                if (CK3dEntity* ball = current_ball()) ball->GetPosition(&at);
                logf("resetpoint now (%.2f,%.2f,%.2f) at tick %u, own ball at (%.2f,%.2f,%.2f)",
                     reset.x, reset.y, reset.z, tick, at.x, at.y, at.z);
            }
            poll.resetpoint_moved = resetpoint_moved;
            if (chain_cell_split_.applied()) {
                // Step B2SX: the chain's own copy, which no pass writes.
                int sector = 0;
                poll.cell_read = chain_cell_split_.read(engine_->context(), sector);
                poll.cell = sector;
                poll.cell_private = true;
            } else if (CKDataArray* parameters = engine_->data_array("IngameParameter")) {
                int sector = 0;
                poll.cell_read = parameters->GetElementValue(0, 1, &sector) != 0;
                poll.cell = sector;
            }
            poll.checkpoint_held = checkpoint_held_;
            if (CKDataArray* level = engine_->data_array("CurrentLevel")) {
                int phase = 0;
                poll.phase_read = level->GetElementValue(0, 4, &phase) != 0;
                poll.phase = phase;
            }
            return poll;
        }

        // struct own_life and the own lives - the beam, the own births'
        // records, the own body re-made and taken out at the history's
        // changes, the records' retirement, rebase and set-aside - moved into
        // the shared core at E7 (client_core_lifecycle.cpp).  What stays is
        // their platform half (make_platform: make_own, unmake_own,
        // own_birth_capture, ball_recipe_of).

        // The navigation replica shut down before the core goes: its
        // controllers would otherwise point into it.  The input of the next
        // step re-creates them on the new core, as the server's PreSimulate
        // pass does.
        void own_navigation_off(const std::string& entity, uint32_t tick) {
            if (!own_replica_.active || own_replica_.entity != entity) return;
            const bmmo_physics_nav_state off{};
            std::string error;
            if (!bmmo::physics::navigation_set_state(engine_->physics(), own_replica_.entity.c_str(), off, error))
                logf("own body at tick %u: navigation shutdown: %s", tick, error.c_str());
        }

        // The own body taken out (platform unmake_own: the core mutes the
        // event log around it, and logs a failure): the navigation replica
        // shut down first, the entity put at the pose its Physicalize
        // carried, where the body is made again.  Step EL8: a replayed own
        // death (`death`) leaves the replica as the live death left it; a
        // navigation made for a revive goes with the body.  The body part is
        // host support's (make_own_body / unmake_own_body, shared with the
        // retail core host).
        bool unmake_own_body(const std::string& name, const float position[3], const float rotation[9], uint32_t tick,
                             bool death, std::string& error) {
            if (revive_navigations_.erase(name)) {
                std::string nav_error;
                if (!bmmo::physics::navigation_destroy(engine_->physics(), name.c_str(), nav_error))
                    logf("own body at tick %u: the revive's navigation of %s: %s", tick, name.c_str(), nav_error.c_str());
            } else if (!death) {
                own_navigation_off(name, tick);
            }
            auto* entity = CK3dEntity::Cast(engine_->context()->GetObjectByNameAndParentClass(
                    const_cast<CKSTRING>(name.c_str()), CKCID_3DENTITY, nullptr));
            bmmo::physics::physics_direct direct(engine_->physics());
            return bmmo::game::unmake_own_body(direct, entity, name, position, rotation, error);
        }

        // The own body made again the way the server makes it (platform
        // make_own: the core mutes the event log around it, then logs and
        // counts what it did): a body still there taken out first, the pose,
        // recipe and group its Physicalize carried, and a spawn's kick.
        // Step EL8: a revived life (with_replica) gets the navigation the
        // replay drives it with (host support's attach_revive_navigation).
        bmmo::session::own_body_made remake_own_body(const bmmo::session::own_body_spec& spec) {
            own_navigation_off(spec.entity, spec.tick);
            auto* entity = CK3dEntity::Cast(engine_->context()->GetObjectByNameAndParentClass(
                    const_cast<CKSTRING>(spec.entity.c_str()), CKCID_3DENTITY, nullptr));
            bmmo::physics::physics_direct direct(engine_->physics());
            bmmo::session::own_body_made made = bmmo::game::make_own_body(direct, entity, spec);
            bmmo::physics::hull_probe(engine_->physics(), spec.entity.c_str(), "own_remade");
            if (made.made && spec.with_replica) {
                const int ball_type = ball_type_of(spec.entity);
                const float force =
                        ball_type >= 0 && ball_type < static_cast<int>(ball_rows_.size()) ? ball_rows_[ball_type].force : 0.0f;
                bool created = false;
                std::string error;
                if (!bmmo::game::attach_revive_navigation(direct, navigation_, own_replica_, spec.entity, force, created,
                                                          error))
                    logf("own body at tick %u: the revive's navigation of %s: %s", spec.tick, spec.entity.c_str(),
                         error.c_str());
                if (created) revive_navigations_.insert(spec.entity);
            }
            return made;
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

        // Step OWNB: own_physicalize_hook on every Physicalize block of the
        // level (once, when the level's navigation is known: the scripts are
        // loaded by then).  A block made later (none in the retail levels)
        // would keep the plain function and its own birth the old test.
        void install_physicalize_hook() {
            CKContext* context = engine_->context();
            const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
            CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
            int hooked = 0;
            for (int i = 0; i < count; ++i) {
                auto* behavior = CKBehavior::Cast(context->GetObject(ids[i]));
                // A block has a prototype GUID only when it uses a function.
                if (!behavior || !behavior->IsUsingFunction() || behavior->GetPrototypeGuid() != kPhysicalizeGuid) continue;
                const CKBEHAVIORFCT function = behavior->GetFunction();
                if (function == &own_physicalize_hook) { ++hooked; continue; }
                if (!g_physicalize_original) g_physicalize_original = function;
                if (function != g_physicalize_original) continue;   // not the prototype's: left alone
                behavior->SetFunction(&own_physicalize_hook);
                ++hooked;
            }
            g_physicalize_client = this;
            physicalize_hooked_ = hooked;
            logf("Physicalize hook: %d block(s)", hooked);
        }

        // Step OWNB: before a Physicalize block of a live tick makes the own
        // ball's body - what the retail host does in OnPhysicalize, which
        // BMLPlus broadcasts before the block creates the body
        // (core_host_on_physicalize): the spawn flag (the entity at the
        // level's resetpoint CurrentLevel[0,3]) and the birth cell
        // (session/spawn_impulse.hpp), the entity moved off the resetpoint in
        // float, the retail sum, so the block makes the body exactly at the
        // pose the Physicalize event carries and every other side makes its
        // copy at.  Nothing in a replayed step (no live tick), for a block
        // that keeps an existing body, or for any other entity.
        void before_physicalize(CKBehavior* behavior) {
            if (!behavior || !behavior->IsInputActive(0) || live_tick_serial_ == 0) return;
            auto* target = CK3dEntity::Cast(behavior->GetTarget());
            CK3dEntity* ball = current_ball();
            if (!target || target != ball || !target->GetName()) return;
            CKIpionManager* physics = engine_->physics();
            if (!physics || physics->GetPhysicsObject(target)) return;
            const std::string name = target->GetName();
            if (name == core_.own_body_entity()) return;
            own_pre_.valid = true;
            own_pre_.serial = live_tick_serial_;
            own_pre_.entity = name;
            own_pre_.spawn = false;
            const VxMatrix& world = target->GetWorldMatrix();
            VxMatrix reset;
            CKDataArray* level = engine_->data_array("CurrentLevel");
            if (!level || !level->GetElementValue(0, 3, &reset)) return;
            const float dx = world[3][0] - reset[3][0];
            const float dy = world[3][1] - reset[3][1];
            const float dz = world[3][2] - reset[3][2];
            own_pre_.spawn = (dx * dx + dy * dy + dz * dz) < 1e-6f;
            // Step SPGH2: the own birth as seen before the block makes the
            // body, where the retail host's own_physicalized sees it; the
            // core decides whether it begins a spawn ghost life.
            {
                bmmo::session::own_birth_pose seen;
                seen.captured = true;
                seen.spawn = own_pre_.spawn;
                for (int k = 0; k < 3; ++k) seen.position[k] = world[3][k];
                core_.own_birth_seen(seen, live_tick_number_);
            }
            const uint8_t join_order = static_cast<uint8_t>(core_.own_join_order() < 0 ? 63 : core_.own_join_order());
            if (!own_pre_.spawn || core_.spawn_spacing() <= 0.0f || join_order % bmmo::session::kSpawnOffsets == 0) return;
            const float right[3] = {reset[0][0], reset[0][1], reset[0][2]};
            const float dir[3] = {reset[2][0], reset[2][1], reset[2][2]};
            float offset[3];
            bmmo::session::spawn_offset(join_order, core_.spawn_spacing(), right, dir, offset);
            VxMatrix moved = world;
            for (int k = 0; k < 3; ++k) moved[3][k] += offset[k];
            target->SetWorldMatrix(moved);
            logf("spawn offset (%.3f,%.3f,%.3f) for join order %u at tick %u", static_cast<double>(offset[0]),
                 static_cast<double>(offset[1]), static_cast<double>(offset[2]), join_order, live_tick_number_);
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
            if (name == core_.own_body_entity()) return;   // the body was there before this tick
            bmmo_physics_body_state state{};
            std::string error;
            if (!bmmo::physics::get_body_state(engine_->physics(), name.c_str(), state, error)) return;   // no body yet
            bmmo::physics::hull_probe(engine_->physics(), name.c_str(), "own_capture");
            // Step OWG: the body in the player's group before this tick's PSIs,
            // as the server's clone is from its Physicalize: the block made it
            // in the script's group, and upkeep (post_step step 17) comes after
            // the PSIs - the birth tick would see no player ball here (one
            // ghost frame at a trafo; at a spawn the pairs its insertion made
            // with player balls kept).  The pairs are re-asked when the group
            // changed.  The retail host does the same from its OnPhysicalize
            // (queue_own_group: this frame's PreSimulate pass).
            {
                bmmo::physics::own_group_result adopted;
                if (bmmo::physics::adopt_own_group(engine_->physics(), name.c_str(), core_.own_group().c_str(), adopted, error)) {
                    own_group_set_ = true;
                    if (own_group_logs_ < 64) {
                        ++own_group_logs_;
                        logf("own group at tick %u: %s", tick, bmmo::physics::describe_own_group(adopted).c_str());
                    }
                } else {
                    logf("own ball group at tick %u: %s", tick, error.c_str());
                }
                bmmo::physics::hull_probe(engine_->physics(), name.c_str(), "own_group");
            }
            own_capture_.valid = true;
            own_capture_.tick = tick;
            own_capture_.entity = name;
            own_capture_.pose = ball->GetWorldMatrix();
            own_capture_.spawn = false;
            // Step OWNB: the spawn flag as own_physicalize_hook saw the entity
            // before the block made the body (at the resetpoint, before the
            // birth cell moved it), as the retail host flags it.  The pose is
            // the entity's now: the block made the body there and nothing has
            // moved either since.
            if (own_pre_.valid && own_pre_.serial == live_tick_serial_ && own_pre_.entity == name) {
                own_capture_.spawn = own_pre_.spawn;
            } else {
                // No block of this tick went through the hook (it is not
                // installed): the old test, and no birth cell.
                VxMatrix reset;
                CKDataArray* level = engine_->data_array("CurrentLevel");
                if (level && level->GetElementValue(0, 3, &reset)) {
                    const float dx = own_capture_.pose[3][0] - reset[3][0];
                    const float dy = own_capture_.pose[3][1] - reset[3][1];
                    const float dz = own_capture_.pose[3][2] - reset[3][2];
                    own_capture_.spawn = (dx * dx + dy * dy + dz * dz) < 1e-6f;
                }
                if (own_capture_.spawn && core_.spawn_spacing() > 0.0f)
                    logf("spawn offset: the Physicalize hook did not see the own birth at tick %u (%d blocks hooked): no birth cell",
                         tick, physicalize_hooked_);
                // Step SPGH2: without the hook the birth is seen only here,
                // after the block made the body (still before the PSIs).
                bmmo::session::own_birth_pose seen;
                seen.captured = true;
                seen.spawn = own_capture_.spawn;
                for (int k = 0; k < 3; ++k) seen.position[k] = own_capture_.pose[3][k];
                core_.own_birth_seen(seen, tick);
            }
            own_pre_.valid = false;
            if (!own_capture_.spawn) return;
            const uint8_t join_order = static_cast<uint8_t>(core_.own_join_order() < 0 ? 63 : core_.own_join_order());
            // (The birth cell moved the entity before the block made the
            // body: own_physicalize_hook.)
            if (core_.spawn_impulse() <= 0.0f) return;
            const uint32_t idx = bmmo::session::spawn_direction_index(core_.seed(), join_order, tick);
            if (!bmmo::physics::push_impulse(engine_->physics(), name.c_str(), bmmo::session::kSpawnDirectionTable[idx],
                        core_.spawn_impulse(), 0, error))
                logf("spawn impulse: %s", error.c_str());
            else
                logf("spawn impulse index=%u at tick %u", idx, tick);
            bmmo::physics::hull_probe(engine_->physics(), name.c_str(), "own_impulse");
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
            // Step A3 (resync design 2.6.6): a pending exact resync whose
            // anchor would leave the history with the next step holds the
            // world the same way, until its state is applied (or 2 s).
            if (core_.poll_resync_hold().held) {
                wall_sleep(std::chrono::milliseconds(1));
                return;
            }
            // Frames run from the assignment, and the server's world starts at
            // the same tick: a client that stepped between its anchor and the
            // assignment would be that many steps ahead of the world for good.
            // Inputs before the assignment queue up and the core sends them
            // when the tick base lands.
            const uint64_t next_frame = static_cast<uint64_t>(core_.frames_since_anchor() + 1);
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
            if (core_.slewing()) advance_lead_slew(next_frame);
            const double expected = std::chrono::duration<double>(wall_now(bmmo::sim::wall_site::pace_expected) - origin_).count() * bmmo::sim::kTickRate;
            // 33 frames behind is a stall, plus whatever a slew still has to
            // gain: the slew never runs the pacer more than a frame behind
            // (lead_slew::kRaiseMaxBehind), so a real stall still rebases.
            if (static_cast<double>(next_frame) + core_.rebase_threshold() < expected) {
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
                core_.schedule_rebased("tick schedule rebased");
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
            const auto frame_started = std::chrono::steady_clock::now();
            frame();
            note_frame_time(std::chrono::steady_clock::now() - frame_started);
        }

        // Open question 1 (late-player plan step 12): the CPU time of every
        // frame() in 0.1 ms buckets up to 1 s, for the frame_time line at
        // each report (count, p50, p99, p99.9, max in ms).  Diagnostic
        // only: it decides nothing.
        static constexpr size_t kFrameTimeBuckets = 10001;
        std::vector<uint32_t> frame_time_hist_ = std::vector<uint32_t>(kFrameTimeBuckets, 0);
        uint64_t frame_time_count_ = 0;
        double frame_time_max_ms_ = 0.0;
        void note_frame_time(std::chrono::steady_clock::duration took) {
            const double ms = std::chrono::duration<double, std::milli>(took).count();
            const size_t bucket = std::min(kFrameTimeBuckets - 1, static_cast<size_t>(ms * 10.0));
            ++frame_time_hist_[bucket];
            ++frame_time_count_;
            frame_time_max_ms_ = std::max(frame_time_max_ms_, ms);
        }
        double frame_time_quantile(double q) const {
            if (!frame_time_count_) return 0.0;
            uint64_t seen = 0;
            for (size_t b = 0; b < kFrameTimeBuckets; ++b) {
                seen += frame_time_hist_[b];
                if (static_cast<double>(seen) >= q * static_cast<double>(frame_time_count_)) return (b + 1) / 10.0;
            }
            return kFrameTimeBuckets / 10.0;
        }

        // One step of the slew: how far the pacer is behind goes to the
        // core's slew_step with the time (the tick periods since its previous
        // step, lead_slew::advance; the whole delta moved acknowledged there),
        // and the origin moves by what it returns (+1: a tick earlier, the
        // clock runs ahead).
        void advance_lead_slew(uint64_t next_frame) {
            const auto now = wall_now(bmmo::sim::wall_site::slew_step);
            const double behind = std::chrono::duration<double>(now - origin_).count() * bmmo::sim::kTickRate
                                - static_cast<double>(next_frame);
            const int moved = core_.slew_step(now, bmmo::sim::kTickRate, behind);
            if (moved != 0)
                origin_ -= std::chrono::duration_cast<clock_type::duration>(
                    std::chrono::duration<double>(static_cast<double>(moved) / bmmo::sim::kTickRate));
        }

        // --keep-lane: the record's keys of this frame with LEFT or RIGHT added
        // while the own ball is off the lane (arguments::keep_lane).  The
        // sideways axis is the world's z, the lane's normal on the arena; the
        // key that pushes the right way is the navigation leaf whose force,
        // taken through the camera reference the replica uses, points that
        // way - so a camera the record turned still steers towards the lane.
        const unsigned char* keep_lane(const unsigned char* keys) {
            std::memcpy(lane_keys_, keys, sizeof(lane_keys_));
            const int key = keep_lane_dwell(keep_lane_key(keys));
            if (key >= 0) lane_keys_[key] = 1;
            if (args_.keep_lane_tap_n > 0 && !keep_lane_tap(key)) lane_keys_[key] = keys[key];   // --keep-lane-tap (step RC1d)
            return lane_keys_;
        }

        // The steer's key of this frame: the leaf to press, -1 inside the
        // deadband, kLaneSteerOff when the steer does not act at all.
        int keep_lane_key(const unsigned char* keys) {
            const bool driving = keys[kKeyUp] || keys[kKeyDown];
            const float (*cam)[3] = core_.previous_cam();   // nullptr until a camera was seen
            if (!driving || !core_.own_physicalized() || core_.own_body_entity().empty() || !navigation_known_ || !cam)
                return kLaneSteerOff;
            bmmo_physics_body_state body{};
            std::string error;
            if (!bmmo::physics::get_body_state(engine_->physics(), core_.own_body_entity().c_str(), body, error)) return kLaneSteerOff;
            const double error_z = body.position[2] - args_.keep_lane_z;
            const double a = error_z + kLaneDamping * static_cast<double>(body.linear[2]);
            if (std::fabs(a) < kLaneDeadband) return -1;
            const double push = a < 0.0 ? 1.0 : -1.0;   // the z direction the ball needs
            int best_key = -1;
            double best = 0.0;
            for (const auto& leaf: navigation_.leaves) {
                if (leaf.key == kKeyUp || leaf.key == kKeyDown || leaf.key < 0 || leaf.key >= 256) continue;
                const double z = leaf.direction.x * cam[0][2] + leaf.direction.y * cam[1][2]
                               + leaf.direction.z * cam[2][2];
                if (z * push > best) { best = z * push; best_key = leaf.key; }
            }
            if (best_key >= 0 && args_.trace && lane_steers_++ < 4)
                logf("keep-lane: ball z=%.2f (lane %.2f) rate %.2f -> key %d", body.position[2], args_.keep_lane_z,
                     static_cast<double>(body.linear[2]), best_key);
            return best_key;
        }

        // ---- --keep-lane-dwell (step RC1c, arguments::keep_lane_dwell) ----
        // A minimum dwell on both edges of the steer: the steer's state (no
        // key, or one key) changes only after it has lasted N frames, so a
        // pressed key stays down N frames or more and a release stays up N
        // frames or more (a direct LEFT -> RIGHT switch is a change of state
        // like any other).  Where the steer does not act (no drive keys, no
        // own body, no camera) the key is released and the next press is
        // free.  N = 0: the wanted key itself, today's steer.  N counts
        // frame() calls (one per tick in every run so far: the server's
        // journals show holds of exactly N); without --keep-lane the steer
        // is never called and the dwell is inert.
        int keep_lane_dwell(int wanted) {
            const int n = args_.keep_lane_dwell;
            if (wanted == kLaneSteerOff) {
                lane_dwell_key_ = -1;
                lane_dwell_frames_ = n;
                return -1;
            }
            if (n <= 0) return wanted;
            if (wanted != lane_dwell_key_ && lane_dwell_frames_ >= n) {
                lane_dwell_key_ = wanted;
                lane_dwell_frames_ = 0;
            }
            if (lane_dwell_frames_ < n) ++lane_dwell_frames_;
            return lane_dwell_key_;
        }
        // ---- end --keep-lane-dwell ----

        // ---- --brake-tap (step RC1d, arguments::brake_tap_n / brake_tap_p) ----
        // A hard stop tapped while driving: the frames in which the drive
        // record holds UP are counted (frame() calls, 1-based), and the last N
        // of every P of them press DOWN instead of UP (UP released).  Applied
        // before the steer, which sees the tap as driving.  Absent (N = 0):
        // never called, the record's keys unchanged.
        const unsigned char* brake_tap(const unsigned char* keys) {
            if (!keys[kKeyUp]) return keys;
            ++brake_up_frames_;
            const int64_t p = args_.brake_tap_p, n = args_.brake_tap_n;
            if ((brake_up_frames_ - 1) % p < p - n) return keys;
            std::memcpy(brake_keys_, keys, sizeof(brake_keys_));
            brake_keys_[kKeyUp] = 0;
            brake_keys_[kKeyDown] = 1;
            return brake_keys_;
        }
        // ---- end --brake-tap ----

        // ---- --keep-lane-tap (step RC1d, arguments::keep_lane_tap_n / _m) ----
        // A rapid-trigger pattern on the steer's key: while the steer (after
        // the dwell, if any) wants the same key, it is pressed N frames, then
        // released M frames, repeating; a different wanted key, or none,
        // restarts the pattern, so the next press is immediate.  Counts
        // frame() calls, like the dwell.  Absent (N = 0): never called.
        // Returns whether the wanted key stays pressed this frame (true for
        // no key); keep_lane() then restores the record key value if not.
        bool keep_lane_tap(int wanted) {
            if (wanted < 0) {
                lane_tap_key_ = -1;
                lane_tap_phase_ = 0;
                return true;
            }
            if (wanted != lane_tap_key_) {
                lane_tap_key_ = wanted;
                lane_tap_phase_ = 0;
            }
            const bool press = lane_tap_phase_ < args_.keep_lane_tap_n;
            lane_tap_phase_ = (lane_tap_phase_ + 1) % (args_.keep_lane_tap_n + args_.keep_lane_tap_m);
            return press;
        }
        // ---- end --keep-lane-tap ----

        void frame() {
            std::string error;
            int64_t f = core_.frames_since_anchor() + 1;
            if (args_.record_by_tick && core_.assigned()) {
                if (!have_first_base_) {
                    have_first_base_ = true;
                    first_base_ = core_.next_tick() - static_cast<uint32_t>(std::max<int64_t>(core_.frames_since_anchor(), 0));
                }
                f = static_cast<int64_t>(core_.next_tick() - first_base_) + 1;
            }
            // Keys of this frame come from the recording (frame f of the record).
            const unsigned char* keyboard = nullptr;
            if (!record_.frames.empty() && f >= 0 && static_cast<size_t>(f) < record_.frames.size()) {
                keyboard = record_.frames[static_cast<size_t>(f)].keys.data();
                if (args_.brake_tap_n > 0) keyboard = brake_tap(keyboard);
                if (args_.keep_lane) keyboard = keep_lane(keyboard);
                engine_->set_keyboard_state(keyboard);
            } else {
                engine_->clear_keys();
            }
            // What the own replica polls in this frame's step: the core's
            // input of the frame reads it after the step (observe_input).
            frame_keyboard_ = keyboard;
            // --beam (design 9.25 harness): at its tick, once.  Asked for
            // before the frame that steps that tick: the core makes it after
            // the step (post_step step 29), stamped tick + 1.
            if (args_.beam && !beam_done_ && core_.next_tick() >= args_.beam_tick) {
                beam_done_ = true;
                core_.request_beam(args_.beam_position);
            } else if (args_.beam2 && !beam2_done_ && core_.next_tick() >= args_.beam2_tick) {
                beam2_done_ = true;
                core_.request_beam(args_.beam2_position);
            } else {
                // A later --beam (step S6): the first one due, one per frame.
                for (size_t k = 0; k < args_.more_beams.size() && k < more_beams_done_.size(); ++k) {
                    if (more_beams_done_[k] || core_.next_tick() < args_.more_beams[k].tick) continue;
                    more_beams_done_[k] = true;
                    core_.request_beam(args_.more_beams[k].position);
                    break;
                }
            }
            // --sector-at (step S6): each at its tick, once, once the session
            // is assigned (request_sector stamps it the lead ahead).
            if (core_.assigned())
                for (size_t k = 0; k < args_.sector_at.size() && k < sector_at_done_.size(); ++k) {
                    if (sector_at_done_[k] || core_.next_tick() < args_.sector_at[k].second) continue;
                    sector_at_done_[k] = true;
                    core_.request_sector(args_.sector_at[k].first, "--sector-at");
                }
            // The flow before the step (shared-core design 1.3 pre_step:
            // the sector stamp, the events and echoes, the rewinds, the held
            // body changes, the union's pass, the drive of this tick), the
            // step, and the flow after it (post_step).
            core_.pre_step();
            const bool ticked = engine_->tick(error);
            live_tick_serial_ = 0;
            if (!core_.post_step(ticked)) { logf("tick failed: %s", error.c_str()); ended_ = true; return; }
            // Step T3 (H8): the leave switches.
            if (core_.assigned()) {
                const uint32_t now = current_tick();
                if (args_.leave_after_unphys >= 0 && !leave_unphys_tick_ && args_.beam && now >= args_.beam_tick
                        && was_physicalized_ && !core_.own_physicalized())
                    leave_unphys_tick_ = now + static_cast<uint32_t>(args_.leave_after_unphys);
                was_physicalized_ = core_.own_physicalized();
                if (args_.leave_after_trafo >= 0 && !leave_trafo_tick_ && core_.trafo_sent() > 0)
                    leave_trafo_tick_ = now + static_cast<uint32_t>(args_.leave_after_trafo);
                const char* why = nullptr;
                if (args_.leave_at && now >= args_.leave_at) why = "--leave-at";
                else if (leave_unphys_tick_ && now >= leave_unphys_tick_) why = "--leave-after-unphys";
                else if (leave_trafo_tick_ && now >= leave_trafo_tick_) why = "--leave-after-trafo";
                if (why) {
                    logf("%s: leaving the session at tick %u (own ball %s)", why, now,
                         core_.own_physicalized() ? "physicalized" : "without a body");
                    ended_ = true;
                    return;
                }
            }
            // --resync-at (step A3): a resync asked for while this client
            // keeps stepping, once.
            if (!resync_at_done_ && core_.assigned()) {
                uint32_t due = 0;
                bool armed = false;
                if (args_.resync_at >= 0) {
                    due = static_cast<uint32_t>(args_.resync_at);
                    armed = true;
                } else if (args_.resync_at_sector > 0 && core_.own_sector_stamp(args_.resync_at_sector, due)) {
                    due += args_.resync_at_offset;
                    armed = true;
                }
                if (armed && static_cast<int32_t>(current_tick() - due) >= 0) {
                    resync_at_done_ = true;
                    logf("test resync: requested at tick %u (--resync-at, due %u)", current_tick(), due);
                    core_.request_resync("test (--resync-at)");
                }
            }
        }

        // post_step step 17's upkeep (platform own_body_upkeep): the body
        // guard on the own ball, its collision group, and the own navigation
        // replica attached (with --rollback only).
        void own_body_upkeep(const bmmo::session::own_ball_view& own) {
            std::string error;
            CKIpionManager* physics = engine_->physics();
            const std::string& ball_name = own.entity;
            if (!ball_name.empty() && (!body_guard_ || body_guard_entity_ != ball_name)) {
                if (bmmo::physics::set_body_guard(physics, true, ball_name.c_str(), error)) {
                    body_guard_ = true;
                    body_guard_entity_ = ball_name;
                } else
                    logf("body guard: %s", error.c_str());
            }
            if (own.physicalized) {
                if (!own_group_set_) {
                    if (bmmo::physics::set_body_group(physics, ball_name.c_str(), core_.own_group().c_str(), error)) own_group_set_ = true;
                    else logf("own ball group: %s", error.c_str());
                }
                if (args_.rollback && navigation_known_ && (!own_replica_.active || own_replica_.entity != ball_name))
                    attach_own_navigation(ball_name, own.ball_type);
            } else {
                own_group_set_ = false;
            }
        }

        // post_step step 19's event log (platform drain_body_events): the
        // birth recipes kept here, every other entry handed to the core in
        // order; an own ball's body made or gone sets its group again.
        void drain_body_events(uint32_t tick, const std::function<bool(const bmmo::session::body_event&)>& each) {
            const std::string events = bmmo::physics::drain_event_log(engine_->physics());
            bmmo::session::parse_event_log(events, [&](const std::string& kind, const std::string& name) {
                if (kind == "recipe") {
                    // What this frame's Physicalize made the body with (engine
                    // change #19): kept by tick, so a rewind anchored before
                    // this tick can make the body again at it (recreate_body).
                    bmmo::session::local_birth birth;
                    if (bmmo::session::parse_recipe_event(name, birth)) {
                        local_births_.add(tick, std::move(birth));
                    } else if (recipe_parse_logs_++ < 4) {
                        logf("birth recipe at tick %u could not be parsed: %.120s", tick, name.c_str());
                    }
                    return;
                }
                bmmo::session::body_event event;
                if (kind == "created") event.what = bmmo::session::body_event::kind::created;
                else if (kind == "deleted") event.what = bmmo::session::body_event::kind::deleted;
                else if (kind == "script_wakeup") event.what = bmmo::session::body_event::kind::wakeup;
                else return;
                event.name = name;
                if (each(event)) {
                    own_group_set_ = false;
                    if (event.what == bmmo::session::body_event::kind::created && args_.trace) trace_frames_ = 12;
                }
            });
        }

        // post_step step 21's observation (platform observe_input): the
        // record's keys as the navigation polls them, the camera
        // reference's rows, and whether the leaves' Key Events were active.
        bmmo::session::input_observation observe_input() {
            bmmo::session::input_observation seen;
            CKContext* context = engine_->context();
            if (navigation_known_ && frame_keyboard_) seen.keys = navigation_.keys_from_state(frame_keyboard_);
            if (auto* orient = CK3dEntity::Cast(context->GetObjectByNameAndClass(const_cast<CKSTRING>("Cam_OrientRef"), CKCID_3DENTITY, nullptr))) {
                const VxMatrix& m = orient->GetWorldMatrix();
                for (int r = 0; r < 3; ++r)
                    for (int k = 0; k < 3; ++k) seen.cam[r][k] = m[r][k];
                seen.cam_valid = true;
            }
            seen.nav_active = navigation_known_ && navigation_active();
            return seen;
        }

        // post_step step 30a (hooks.after_amend): the frame's traces.
        void frame_traces(uint32_t tick) {
            CKIpionManager* physics = engine_->physics();
            if (!args_.trace_body.empty() && tick >= args_.trace_from && tick <= args_.trace_until)
                logf("body %s after live tick %u: %s", args_.trace_body.c_str(), tick,
                     describe_traced_body(physics).c_str());
            if (args_.trace_clock_until && tick >= args_.trace_clock_from && tick <= args_.trace_clock_until)
                logf("clock after live tick %u: %s", tick, describe_clock().c_str());
            // --trace: what pushes the own ball - the navigation replica's
            // leaves, every controller on its core, the mechanism force blocks
            // whose target it is - logged whenever that changes.
            if (args_.trace && core_.own_physicalized() && own_replica_.active) {
                std::string text;
                bmmo_physics_nav_state nav{};
                std::string nav_error;
                if (bmmo::physics::navigation_get_state(physics, own_replica_.entity.c_str(), nav, nav_error)) {
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
                text += " | core: " + bmmo::physics::describe_controllers(physics, own_replica_.entity.c_str());
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
                const std::string contacts = bmmo::physics::describe_contacts(physics, own_replica_.entity.c_str());
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
        }

        // The own ball's Physicalize as capture_own_physicalize saw it before
        // the step of `tick` (platform own_birth_capture; the core's
        // report_physicalize sends it): the pose the retail script
        // physicalized at and the spawn flag (design 9.10), the capture
        // consumed.  Without a capture of this birth the entity after the
        // tick, never flagged.
        bmmo::session::own_birth_pose own_birth_capture(const std::string& name, uint32_t tick) {
            bmmo::session::own_birth_pose out;
            out.captured = own_capture_.valid && own_capture_.tick == tick && own_capture_.entity == name;
            CK3dEntity* ball = current_ball();
            if (!out.captured && (!ball || !ball->GetName() || name != ball->GetName()))
                ball = CK3dEntity::Cast(engine_->context()->GetObjectByNameAndParentClass(
                        const_cast<CKSTRING>(name.c_str()), CKCID_3DENTITY, nullptr));
            if (out.captured || ball) {
                const VxMatrix& used = out.captured ? own_capture_.pose : ball->GetWorldMatrix();
                for (int k = 0; k < 3; ++k) out.position[k] = used[3][k];
                for (int r = 0; r < 3; ++r)
                    for (int k = 0; k < 3; ++k) out.rotation[r * 3 + k] = used[r][k];
            }
            out.spawn = out.captured && own_capture_.spawn;
            own_capture_.valid = false;
            return out;
        }

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
                if (core_.session_phase() != phase::running) {
                    // the session's end clears the list (discard_held_events), so this is a safety net;
                    // said aloud either way, so a hold never ends silently.
                    logf("hold-event: own %s stamped tick %u (held at tick %u) discarded: no session running",
                         event_type_name(args_.hold_event_type), it->stamp, it->held_at);
                    it = held_events_.erase(it);
                    continue;
                }
                if (!core_.assigned()) { ++it; continue; }   // a session but no tick base yet: wait for it
                core_.release_event(it->bytes);
                logf("hold-event: own %s stamped tick %u (held at tick %u) released at tick %u",
                     event_type_name(args_.hold_event_type), it->stamp, it->held_at, current_tick());
                core_.journal().note("hold-event: released stamped " + std::to_string(it->stamp));
                it = held_events_.erase(it);
            }
        }

        // apply_blend (the legacy blend's frame half, post_step step 27) moved
        // into the shared core at E9 (client_core_frame.cpp).

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

        // --trace-body's line, with the body's row in hex floats after the
        // description (step SFD): the row the journal's checkpoints carry, so
        // a traced tick's final state can be compared bit for bit with the
        // server's row of that tick.
        std::string describe_traced_body(CKIpionManager* physics) {
            std::string text = bmmo::physics::describe_body(physics, args_.trace_body.c_str());
            bmmo::physics::body_state row{};
            std::string error;
            if (bmmo::physics::get_body_state(physics, args_.trace_body.c_str(), row, error)) {
                char hex[400];
                std::snprintf(hex, sizeof(hex), " | row pos=%a,%a,%a lin=%a,%a,%a sim=%d",
                              row.position[0], row.position[1], row.position[2], static_cast<double>(row.linear[0]),
                              static_cast<double>(row.linear[1]), static_cast<double>(row.linear[2]),
                              row.simulated ? 1 : 0);
                text += hex;
            }
            return text;
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
            // Step SFD: BMMO_DUMP_FINAL_LAG=N also writes the record of
            // tick - N into <dir>/final/ at each live tick - a record that old
            // is past every rewind's reach, so it is the world's final state
            // of that tick (an ordinary rewind does not rewrite the files).
            if (const char* lag_text = args_.dump_savestate_dir.empty() ? nullptr : std::getenv("BMMO_DUMP_FINAL_LAG"); lag_text && *lag_text && !std::strcmp(source, "live")) {
                const uint32_t lag = static_cast<uint32_t>(std::atoi(lag_text));
                const uint32_t old = tick > lag ? tick - lag : 0;
                if (old && !args_.dump_savestate_dir.empty() && old >= args_.dump_savestate_from && old <= args_.dump_savestate_until)
                    if (const std::vector<uint8_t>* final_blob = core_.recorded_savestate(old)) {
                        std::error_code ec;
                        const std::filesystem::path dir = std::filesystem::path(args_.dump_savestate_dir) / "final";
                        std::filesystem::create_directories(dir, ec);
                        if (FILE* f = std::fopen((dir / (std::to_string(old) + ".sav")).string().c_str(), "wb")) {
                            std::fwrite(final_blob->data(), 1, final_blob->size(), f);
                            std::fclose(f);
                        }
                    }
            }
            compare_scripts(tick, source);
            if (args_.dump_savestate_dir.empty() || tick < args_.dump_savestate_from || tick > args_.dump_savestate_until) return;
            dump_scripts(tick);
            const std::vector<uint8_t>* blob = core_.recorded_savestate(tick);
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
            const std::vector<uint8_t>* recorded = core_.recorded_scripts(tick);
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
            const std::vector<uint8_t>* recorded = core_.recorded_scripts(tick);
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
            const float (*cam)[3] = core_.previous_cam();   // nullptr until a camera was seen
            if (!own_replica_.active || !cam) return;
            std::string error;
            if (!bmmo::physics::navigation_input(engine_->physics(), own_replica_.entity.c_str(), 0, cam[0], cam[1],
                                                 cam[2], false, error))
                logf("own navigation rows after a rewind: %s", error.c_str());
        }

        // Rewind for no reason at all, on a fixed cadence (--force-rollback).
        // A solo session's world is a pure function of its recorded inputs, so
        // the hashes of a run with these must equal those of a run without; any
        // difference is state the replay failed to restore.  The cadence and
        // the traces are this client's; the rewind is the core's
        // (force_rewind, shared-core design step E5), and the live cells it
        // keeps stay kept until `forced` goes out of scope at the end, after
        // the traces, as before.
        void maybe_force_rollback() {
            if (!args_.rollback || args_.force_rollback_every == 0) return;
            const uint32_t now = current_tick();
            if (now < args_.force_rollback_depth + 2) return;
            if (now % args_.force_rollback_every != 0) return;
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
            if (args_.trace_restore && core_.own_physicalized() && !core_.own_body_entity().empty() && contact_traces_ < 48) {
                ++contact_traces_;
                logf("contacts of %s before the rewind at %u: %s", core_.own_body_entity().c_str(), now,
                     bmmo::physics::describe_contacts(engine_->physics(), core_.own_body_entity().c_str()).c_str());
            }
            if (args_.trace_restore && queue_traces_ < 24) {
                ++queue_traces_;
                logf("PreSimulate queue before the rewind at %u: %s", now,
                     bmmo::physics::describe_presimulate_queue(engine_->physics()).c_str());
            }
            const auto forced = core_.force_rewind(args_.force_rollback_depth);
            if (forced.rolled) {
                for (uint32_t t = anchor + 1; t <= now; ++t) dump_savestate(t, "replayed");
                if (args_.trace_restore && queue_traces_ < 24) {
                    ++queue_traces_;
                    logf("PreSimulate queue after the rewind at %u: %s", now,
                         bmmo::physics::describe_presimulate_queue(engine_->physics()).c_str());
                }
            }
            if (args_.trace_restore && core_.own_physicalized() && !core_.own_body_entity().empty() && contact_traces_ < 48) {
                ++contact_traces_;
                if (!args_.trace_body.empty() && now >= args_.trace_from && now <= args_.trace_until)
                    logf("body %s after the rewind (replayed to %u): %s", args_.trace_body.c_str(), now,
                         describe_traced_body(engine_->physics()).c_str());
                logf("contacts of %s after the rewind (replayed to %u): %s", core_.own_body_entity().c_str(), now,
                     bmmo::physics::describe_contacts(engine_->physics(), core_.own_body_entity().c_str()).c_str());
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

        // Every movable body in this client's world (platform movable_bodies):
        // the core leaves the balls out by what they are (step P), not by
        // the "Ball_" prefix, which the trafo explosion pieces share.
        std::vector<std::string> movable_body_names() {
            return bmmo::physics::movable_body_names(engine_->physics());
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

        // recorded_input_at moved into the shared core at E9
        // (client_core_frame.cpp), over the platform's own_replica_name.

        // drain_due_events and apply_held_lifecycle moved into the shared core
        // at E6 (session/client_core.hpp, client_core_lifecycle.cpp).

        // process_own_echoes, process_own_echo, answer_set_aside,
        // expire_own_echoes, resync_own_life and move_own_birth moved into the
        // shared core at E7 (client_core_lifecycle.cpp).

        // join_rank, rewind_for_lifecycle, unphysicalize_remote,
        // make_remote_birth, apply_event, drain_pending_lives, the event log's
        // mute, live_first_tick, unphysicalize_mirror, live_drive,
        // physicalize_remote and attach_remote_navigation moved into the shared
        // core at E6 (client_core_lifecycle.cpp); the mirrors' platform half is
        // make_platform's (mirror_entity, make_mirror, unmake_mirror,
        // detach_mirror_navigation, attach_mirror_navigation, mute_event_log).

        // A mirror's navigation (platform attach_mirror_navigation, the host
        // half of the core's attach_remote_navigation): the shared navigation
        // with the level's leaf directions, once the graph is known.
        bmmo::session::mirror_navigation attach_mirror_navigation(uint32_t player, const std::string& entity,
                                                                  uint8_t ball_type, std::string& error) {
            if (!navigation_known_) return bmmo::session::mirror_navigation::unavailable;
            const float force = ball_type < ball_rows_.size() ? ball_rows_[ball_type].force : 0.0f;
            bmmo::physics::physics_direct direct(engine_->physics());
            return bmmo::game::attach_mirror_navigation(direct, navigation_, entity, player, force, error);
        }

        // Design 9.6: own ball driven by the replica in polling mode, retail
        // force leaves zeroed (see the mod's physics_session_attach_own_navigation).
        void attach_own_navigation(const std::string& ball_name, int ball_type) {
            if (!navigation_known_ || !navigation_.valid() || ball_name.empty()) return;
            std::string error;
            const float force = ball_type >= 0 && ball_type < static_cast<int>(ball_rows_.size()) ? ball_rows_[ball_type].force : 0.0f;
            bmmo::physics::physics_direct direct(engine_->physics());
            switch (bmmo::game::attach_own_replica(direct, navigation_, own_replica_, ball_name, force, error)) {
            case bmmo::game::own_replica_attach::failed:
                logf("own navigation replica: %s", error.c_str());
                return;
            case bmmo::game::own_replica_attach::created:
                logf("own ball %s driven by the navigation replica (force %.3f)", ball_name.c_str(), force);
                return;
            default:
                return;
            }
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
        // the in-process backend physics/physics_direct.hpp), composed with
        // the core's half (core_world_hooks: its births, the replayed
        // mirrors, the corrections, the moved own births) by the shared
        // bind_session_world (session/session_host_world.hpp, E9), plus what
        // only this client has - its trace switches, its births' options -
        // as options.
        bmmo::session::rollback_world rollback_world(const bmmo::session::core_world_hooks& hooks) {
            physics_direct_.manager = engine_->physics();
            CKIpionManager* physics = physics_direct_.manager;
            bmmo::session::session_world_options w;
            bmmo::session::rollback_binding_options& o = w.binding;
            o.delta_ms = 1000.0f / 66.0f;
            // The clock is always read (a restore shifts the calm references it
            // puts back by the live clock's offset); rewound only on
            // --restore-clock.
            o.restore_clock = args_.restore_clock;
            o.savestate_flags = args_.savestate_strict ? bmmo::physics::SAVESTATE_STRICT : 0u;
            o.own_key_codes = own_replica_.key_codes;
            o.own_key_blocks = own_replica_.key_blocks;
            o.own_key_count = own_replica_.key_count;
            // Polling resumed = a rewind replayed: the live frame's own
            // navigation input is re-armed (see rearm_own_navigation).
            o.on_poll_resumed = [this](const std::string&) { rearm_own_navigation(); };
            o.log = [](const std::string& text) { logf("%s", text.c_str()); };
            // Before each replayed tick's scripts and step, after the core's
            // births (the own beam stamped for it among them): the sector
            // manager's activation recorded for it, and the traces.
            w.replay_activation = [this](uint32_t tick) { replay_manager_activation(tick, true); };
            // Step N02R: after each replayed tick's step, the
            // IngameParameter[0][2] its live frame's chains wrote (host
            // support's manager_activations::after_replayed_step).
            w.after_replayed_step = [this](uint32_t tick) {
                if (args_.park_births) return;
                int value = 0;
                if (!manager_activations_.after_replayed_step(engine_->context(), tick)) return;
                if (deactivate_write_logs_++ < 8) {
                    if (CKDataArray* parameters = engine_->data_array("IngameParameter")) parameters->GetElementValue(0, 2, &value);
                    logf("replay: IngameParameter[0][2] = %d written after replayed tick %u (its live frame's write)", value, tick);
                }
            };
            w.before_replayed_tick = [this](uint32_t tick) {
                if (!args_.trace_body.empty() && tick >= args_.trace_from && tick <= args_.trace_until)
                    logf("body %s before replayed tick %u: %s", args_.trace_body.c_str(), tick,
                         describe_traced_body(engine_->physics()).c_str());
                if (args_.trace_clock_until && tick >= args_.trace_clock_from && tick <= args_.trace_clock_until)
                    logf("clock before replayed tick %u: %s", tick, describe_clock().c_str());
                if (args_.trace_restore && core_.own_physicalized() && !core_.own_body_entity().empty() && contact_traces_ < 48) {
                    ++contact_traces_;
                    logf("contacts of %s at replayed tick %u, before its step: %s", core_.own_body_entity().c_str(), tick,
                         bmmo::physics::describe_contacts(engine_->physics(), core_.own_body_entity().c_str()).c_str());
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
            // A body born inside the window is made again inside the replay,
            // at its birth tick, with the recipe and matrix its Physicalize
            // used (plan doc 9): the same insertion into IVP's tree, units and
            // caches the live frame made, where parking and re-posing it gave
            // the tree a different layout for good.  --insert-births: the
            // parked body is put where its Physicalize made it at the start
            // of its birth tick (set_body_matrix), its joints and controllers
            // untouched.  Not the default: a beam normalizes the quaternion
            // where the creation does not, so most insertions miss their
            // birth record in the last bit and fall back to the re-pose - a
            // second recheck after the step - and the 7:5 arm, exact on 52 of
            // 53 bodies under plain parking, came out off on six (measured
            // 2026-10-22).  The A/B stays.  Plan doc 10 (the default): a
            // mechanism body born inside the window is destroyed at the
            // restore, with its entity put back at the matrix its
            // Physicalize found (put_back_birth), and the replayed module
            // script makes it again the live way.  (Under --park-births it is
            // parked through the gap instead.)  bind_session_world installs
            // them over this client's birth log.
            w.births = &local_births_;
            // Step P: the trafo explosion pieces are made again at a
            // restore (the replayed reset takes them away again); T3: every
            // set's too.  Gap L, L4 (step EL9): bind_script_replay adds every
            // body of a selected module (rollback_world::revivable).
            w.revivable = [this](const std::string& entity) { return explosion_pieces_.count(entity) != 0; };
            w.park_births = args_.park_births;
            w.insert_births = args_.insert_births;
            w.put_back = [this](const std::string& entity, const bmmo::session::local_birth& birth) {
                bmmo::game::put_back_birth(engine_->context(), entity, birth);
            };
            w.on_own_destroy_failed = [this](const std::string& entity) {
                if (recreate_logs_++ < 8) logf("own body %s could not be destroyed at the restore", entity.c_str());
            };
            w.on_destroy_failed = [this](const std::string& entity, const std::string& error) {
                if (recreate_logs_++ < 8) logf("birth: destroying %s at the restore failed: %s", entity.c_str(), error.c_str());
            };
            w.on_insert_failed = [this](const std::string& entity, uint32_t birth_tick, const std::string& error) {
                if (recreate_logs_++ < 8) logf("birth: inserting %s at tick %u failed: %s", entity.c_str(), birth_tick, error.c_str());
            };
            if (args_.script_replay && !scripts_.empty()) w.bind_scripts = [this](bmmo::session::rollback_world& world) {
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
                // Step T3: the sets' scripts, recorded (v3), restored and run
                // first in every replayed frame, as the trafo frame live.
                so.trafo = trafo_on_ ? &trafo_registry_.part() : nullptr;
                // Step RO1: the live frames' order, and a string parameter
                // written back at its recorded length.
                so.live_order = &script_order_;
                so.resize_strings = true;
                // Step B2SW (G1, F): the behaviour manager's list as live.
                so.live_membership = true;
                // Step B7EG (gap Q): the PreSimulate queue with the records.
                so.callback_queue = true;
                so.on_restore_failed = [this](const std::string& error) {
                    if (script_restore_logs_++ < 8) logf("script replay: restore failed: %s", error.c_str());
                };
                bmmo::session::bind_script_replay(world, physics_direct_, so);
            };

            bmmo::session::rollback_world world = bmmo::session::bind_session_world(physics_direct_, hooks, w);
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
                        for (const auto& [id, remote]: core_.remotes())
                            if (remote.navigation)
                                logf("core before step: %s", bmmo::physics::describe_core(physics, remote.entity.c_str()).c_str());
                    const bool ok = inner();
                    if (diag) {
                        ++step_diag_;
                        for (const auto& [id, remote]: core_.remotes())
                            if (remote.navigation)
                                logf("core after step:  %s", bmmo::physics::describe_core(physics, remote.entity.c_str()).c_str());
                    }
                    return ok;
                };
            }
            // The births a replayed tick made, in the birth log under that
            // tick: the event log is muted around the replayed frame, so a
            // body the replay made where the live frames made none (a sector
            // activation the live frames cancelled, re-issued in the replay)
            // had no Physicalize matrix to be put back to when a later rewind
            // destroyed it, and every rewind across that birth started its
            // Physicalize from wherever the last one had left the entity.
            // Only a birth the log does not already have at that tick (a
            // replay that agrees with the live frames adds nothing), and not
            // the balls (never destroyed or put back).
            if (world.pre_step) {
                const auto inner = world.pre_step;
                world.pre_step = [this, inner](uint32_t tick) {
                    bmmo::physics::set_event_log_keep_muted_recipes(true);
                    inner(tick);
                    bmmo::physics::set_event_log_keep_muted_recipes(false);
                    const std::string recipes = bmmo::physics::drain_muted_recipes();
                    if (recipes.empty()) return;
                    bmmo::session::parse_event_log(recipes, [&](const std::string& kind, const std::string& text) {
                        bmmo::session::local_birth birth;
                        if (kind != "recipe" || !bmmo::session::parse_recipe_event(text, birth)) return;
                        if (core_.is_ball(birth.name) || local_births_.find(birth.name, tick)) return;
                        if (replay_birth_logs_++ < 8) logf("birth log: %s made by the replayed tick %u", birth.name.c_str(), tick);
                        local_births_.add(tick, std::move(birth));
                    });
                };
            }
            return world;
        }

        // SessionEnd (session_client_core::end): what --hold-event still
        // holds is discarded, said aloud (hooks.ending)...
        void discard_held_events() {
            for (const auto& held: held_events_)
                logf("hold-event: own %s stamped tick %u (held at tick %u) discarded: the session ended first",
                     event_type_name(args_.hold_event_type), held.stamp, held.held_at);
            held_events_.clear();
        }

        // ...and, after the journal closed, the own body guard is lifted
        // (hooks.ended; legacy_calls::own_body_guard_off until E11); the core
        // takes every mirror's navigation and body out after it (E6;
        // end_session_world until then).
        void own_body_guard_off() {
            std::string error;
            if (body_guard_) {
                bmmo::physics::set_body_guard(engine_->physics(), false, nullptr, error);
                body_guard_ = false;
                body_guard_entity_.clear();
            }
        }

        // The status line (shared-core design 1.3; E10): the core's text with
        // this client's own counters at their places, one log line.  The
        // window: line is the core's too (post_step step 31, hooks.window).
        bmmo::session::host_counters counters() const {
            bmmo::session::host_counters h;
            h.rebases = rebases_;
            h.activations_replayed = activations_replayed_;
            h.script_reset_skips = script_reset_skips_;
            return h;
        }
        void report() {
            log_line(core_.status_text(counters()));
            // RC4 (step B6RC's fix-up, review 2): what the dormant-script
            // encoding did in this process - trafo scripts written as one byte
            // from the ledger, walked and found built, walked and written as
            // a block, ledger entries the root guard refused, dormant entries
            // restored - so a session's evidence shows the path ran.  Only in
            // a session that runs the trafo inputs.
            if (trafo_on_) {
                const auto& d = bmmo::game::script_replay::dormancy();
                logf("trafo_dormancy: from_ledger=%llu healed=%llu walked=%llu guard_trips=%llu restored_built=%llu",
                     static_cast<unsigned long long>(d.from_ledger), static_cast<unsigned long long>(d.healed),
                     static_cast<unsigned long long>(d.walked), static_cast<unsigned long long>(d.guard_trips),
                     static_cast<unsigned long long>(d.restored_built));
            }
            // Gap Q (step B7EG): the PreSimulate queue's part of the records
            // and of every replay, in this process.
            {
                const auto& q = bmmo::game::script_replay::queue_stats();
                logf("cbq: captured=%llu rebuilt=%llu removed=%llu held=%llu holds=%llu unhold_clamped=%llu rebuild_fail=%llu "
                     "hold_fail=%llu no_record=%llu",
                     static_cast<unsigned long long>(q.captured), static_cast<unsigned long long>(q.rebuilt),
                     static_cast<unsigned long long>(q.removed), static_cast<unsigned long long>(q.held),
                     static_cast<unsigned long long>(q.holds), static_cast<unsigned long long>(q.unhold_clamped),
                     static_cast<unsigned long long>(q.rebuild_fail), static_cast<unsigned long long>(q.hold_fail),
                     static_cast<unsigned long long>(q.no_record));
            }
            if (frame_time_count_)
                logf("frame_time: frames=%llu p50=%.1f p99=%.1f p999=%.1f max=%.1f ms",
                     static_cast<unsigned long long>(frame_time_count_), frame_time_quantile(0.5),
                     frame_time_quantile(0.99), frame_time_quantile(0.999), frame_time_max_ms_);
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

        int restore_traces_ = 0;
        VxVector resetpoint_seen_;           // CurrentLevel[0,3]'s position as last seen: a move is an own crossing
        bool checkpoint_held_ = false;       // the level's checkpoint manager run is held
        CK_ID sector_manager_id_ = 0;
        bmmo::game::script_replay::selection scripts_;       // the script replay's scripts and satellites (game/script_replay.hpp)
        std::vector<CK_ID> script_activations_;              // the selected scripts whose activations are recorded (steps P, A1)
        std::set<std::string> explosion_pieces_;             // the trafo explosion pieces' entities (step P; session_host_support.hpp; T3: every set's too)
        // Step T3 (trafo-replication design 2.4, 2.11): this world's piece
        // sets, the bridge's hooks for them, the hold of the originals, and
        // whether this session runs the trafo as an input here (from the
        // anchor's quiesce to the session's end).
        bmmo::game::trafo_registry trafo_registry_;
        bmmo::game::trafo_set_hooks trafo_hooks_;
        bmmo::game::trafo_hold trafo_hold_;
        bool trafo_on_ = false;
        uint32_t trafo_reference_logs_ = 0;
        // The H8 leave switches' state.
        uint32_t leave_unphys_tick_ = 0, leave_trafo_tick_ = 0;
        bool was_physicalized_ = false;
        uint64_t snapshots_ignored_ = 0;   // --ignore-snapshots
        std::vector<CK_ID> scripts_replayed_;                // the scripts the last restore wrote: active at its anchor
        bmmo::game::script_replay_hooks script_hooks_;
        bmmo::game::checkpoint_phase_split checkpoint_phase_;  // the chain's own phase cell (game/checkpoint_phase.hpp)
        bmmo::game::sector_reset_guard reset_guard_;         // the retail sector resets, neutralized (game/sector_reset_guard.hpp)
        std::vector<bmmo::game::deactivate_pin> deactivate_pins_;   // N2's pins (game/sector_deactivate_pin.hpp)
        bmmo::game::sector_cell_split chain_cell_split_;     // the chain's own [0][1] (game/sector_cell_split.hpp, step B2SX)
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
        uint32_t recipe_parse_logs_ = 0, recreate_logs_ = 0, replay_birth_logs_ = 0;
        // Every activation of Gameplay_SectorManager the live frames made, by
        // the tick whose frame consumed it, for the replay to re-issue (host
        // support's manager_activations, plan doc 10).
        bmmo::game::manager_activations manager_activations_;
        uint64_t activations_replayed_ = 0;
        uint32_t deactivate_write_logs_ = 0;   // step N02R: the [0][2] writes replayed, logged (the first 8)
        bmmo::session::live_script_order script_order_;   // step RO1: the live frames' script order
        void record_manager_activation(uint32_t tick, bool reset) {
            manager_activations_.record(engine_->context(), tick, reset);
        }
        // `inside`: a replayed tick (an explosion's referential is placed;
        // host support's manager_activations), not the live frame a rewind's
        // end re-issues for.
        void replay_manager_activation(uint32_t tick, bool inside = false) {
            if (args_.park_births) return;
            const uint64_t placed = manager_activations_.referentials_placed();
            const bmmo::game::manager_activation* a = manager_activations_.replay(engine_->context(), sector_manager_id_, tick, inside);
            if (manager_activations_.referentials_placed() != placed && manager_activations_.referentials_placed() <= 8)
                logf("replay: an explosion's referential placed where the live frame had it at tick %u", tick);
            if (!a) return;
            ++activations_replayed_;
            if (args_.trace_restore && recreate_logs_ < 24)
                logf("replay: sector manager activation re-issued at tick %u (activate %d, deactivate %d%s)", tick,
                     a->activate, a->deactivate, a->reset ? ", reset" : "");
        }
        // Every body this world made, by the tick of the record that first
        // carries it (host support's local_birth_log: the event log's
        // "recipe" line, engine change #19).
        bmmo::session::local_birth_log local_births_;
        uint32_t queue_traces_ = 0;                          // --trace-restore PreSimulate queue lines (the first 24)
        std::string compare_scripts_last_;   // the last logged first difference, so only changes are logged
        int contact_traces_ = 0;                            // --trace-restore: contact lines logged around rewinds
        // Step OWNB: the own ball's Physicalize block as own_physicalize_hook
        // saw it before the block ran (the live tick, the entity, and the
        // spawn flag of the entity before the birth cell moved it).
        struct {
            bool valid = false;
            uint64_t serial = 0;
            std::string entity;
            bool spawn = false;
        } own_pre_;
        int physicalize_hooked_ = 0;          // the Physicalize blocks own_physicalize_hook is on
        uint32_t live_tick_number_ = 0;       // the session tick of the live tick live_tick_serial_ numbers
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
        int own_group_logs_ = 0;   // step OWG: the first 64 adoptions logged
        int load_waited_ = 0;
        bmmo::game::sequencer_state level_sequencers_;
        bmmo::game::proximity_countdown_state level_proximities_;   // step Q, as the server's
        clock_type::time_point origin_;
        int rebases_ = 0;
        unsigned char lane_keys_[256] = {};                  // --keep-lane: this frame's keys with the steer added
        int lane_steers_ = 0;
        int lane_dwell_key_ = -1;                            // --keep-lane-dwell: the steer's held key, -1 none
        int lane_dwell_frames_ = std::numeric_limits<int>::max();   // frames in that state (saturates at N)
        unsigned char brake_keys_[256] = {};                 // --brake-tap: this frame's keys with the tap
        int64_t brake_up_frames_ = 0;                        // --brake-tap: frames whose record held UP
        int lane_tap_key_ = -1;                              // --keep-lane-tap: the key being tapped, -1 none
        int lane_tap_phase_ = 0;                             // --keep-lane-tap: frames into its N + M cycle
        bmmo::game::navigation_graph navigation_;
        bool navigation_known_ = false;
        std::vector<ball_row> ball_rows_;
        // The keys frame() set for the step (post_step's observe_input).
        const unsigned char* frame_keyboard_ = nullptr;
        int trace_frames_ = 0;
        bool beam_done_ = false;
        bool beam2_done_ = false;   // --beam2
        std::vector<bool> more_beams_done_;   // a later --beam (step S6)
        std::vector<bool> sector_at_done_;    // --sector-at (step S6)
        // The sector placement (step S6): the copies this process made at
        // its anchor, released before the engine goes (release_placement).
        bmmo::game::sector_placement placement_;
        // (kOwnEchoDeadlineTicks, kOwnEchoUplinkSettleTicks and
        // kOwnRecordKeepTicks moved into the core state at E7.)
        // The entity mirror_entity named last, which make_mirror poses and
        // physicalizes (the core asks for the two in one birth).
        CK3dEntity* mirror_made_entity_ = nullptr;
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
        bool resync_at_done_ = false;   // --resync-at fired
        bool have_first_base_ = false;  // --record-by-tick: the first assigned base
        uint32_t first_base_ = 0;
        // The own ball's navigation replica (design 9.6; host support's
        // own_replica since E9): whether it drives the own ball, the entity
        // it was last attached to, and the keys it polls.
        bmmo::game::own_replica own_replica_;
        // Step EL8: the navigations made for revived own lives (the trafo's
        // old ball), destroyed with the body at the replayed death.
        std::set<std::string> revive_navigations_;
        bool body_guard_ = false;
        std::string body_guard_entity_;
        int step_diag_ = 0;
        // The shared session-client core (docs/session-client-core-design.md):
        // its state (design 1.2) is private since step E11.  Last, so it is
        // destroyed first, as the journal was.
        bmmo::session::session_client_core core_{make_platform(), make_capabilities()};
    };

    int own_physicalize_hook(const CKBehaviorContext& context) {
        if (g_physicalize_client) g_physicalize_client->before_physicalize(context.Behavior);
        return g_physicalize_original ? g_physicalize_original(context) : CKBR_GENERICERROR;
    }

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
    bmmo::sim::opt_out_of_power_throttling();
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
