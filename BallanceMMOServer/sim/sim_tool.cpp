// Command-line driver for the headless engine.
//
//   BallanceMMOSimTool --root <game dir> [--level N] [--ticks N] [--level-at N]
//   BallanceMMOSimTool --root <game dir> --replay <record.bmrc> [--dump-diverge]
//   BallanceMMOSimTool --root <game dir> --replay-session <file.bmjr> [--list] [--write-journal <out.bmjr>]
//   BallanceMMOSimTool --root <game dir> --level N --spawn-test N [--spawn-impulse S] [--spawn-ball T]
//       [--ticks K] [--report-every R]
//   BallanceMMOSimTool --root <game dir> --level N --level-at T --explode wood|paper|stone AT_TICK [--ticks N]
//   BallanceMMOSimTool --root <game dir> --level N --level-at T --restore-at TICK [ENTITY] [--ticks N]
//   BallanceMMOSimTool --root <game dir> --level N --savestate-check [scene switches] [--ticks N] [--rounds R]
//       [--max-depth D] [--seed S] [--savestate-every K] [--savestate-lead L] [--frames] [--legacy-restore]
//       [--savestate-no-hold] [--savestate-no-ledger] [--queue-trace] [--savestate-trace PATH] [--stop-on-divergence] [--verbose]
//   BallanceMMOSimTool --root <game dir> --level N --resync-check S B [--resync-arms ERSA] [--resync-stamp SECTOR TICK]
//       [--resync-sector K] [--resync-drop TICK --drop <entity> <ball type>] [--resync-watch NAME]
//       [--resync-player ID] [--resync-server-blob FILE [--resync-blob-rename OLD NEW] [--resync-blob-adopt-environment]]
//       [--verbose]
//
// Replay mode boots base.cmo, loads the recorded level, waits for the retail
// Gameplay_Ingame script to activate (the record's frame 0 anchor), performs
// the same session reset as the client recorder, then feeds the recorded
// keyboard state tick by tick and compares physics hashes.
//
// --replay-session is the offline side of the session black box (design 9.15):
// it rebuilds the world a room ran from its journal (members, every applied
// input frame, every lifecycle event) and compares each tick's fingerprint and
// each checkpoint's bodies with what the recording says, so a bug that only
// happens in a live multiplayer session is reproducible here.  --list is the
// triage pass (no engine boot at all).
//
// --spawn-test boots a session world (design 9.10), physicalizes N players at
// the level's resetpoint in the same tick and checks that the deterministic
// spawn kick moves them apart without ever producing NaN; --explode activates
// a trafo explosion script mid free-run and prints the pose hash and movable
// core count every tick for 200 ticks (Part B: both are diffed between the
// Windows and Linux builds for determinism).
//
// --explode is repeatable, and with --activate Ball_ResetPieces_<type> (the
// trafo sequence's own clean-up, which --explode does not run), --beam and
// --body-guard a free run can play the session's side of a second trafo:
// explode, reset, move the ball, explode again, and the guarded run must come
// out like the unguarded one (design 9.14 / engine change #13).
//
// --savestate-check is the IVP savestate's robustness matrix
// (docs/ivp-savestate-design.md, "Tests that prove it"; see run_savestate_check
// for the exact contract): a straight pass keeps the savestate blob of every
// tick of a window, then chained rounds of seeded (anchor, depth) restores
// must reproduce those blobs byte for byte, before and after re-stepping.  The
// scene is whatever the physics_world switches set up: --spawn-test N (the
// coincident spawn), --drop (a ball dropped onto a bay, --drop-prop beams a
// prop there too) or, with neither, the sector union started by --drop-sector.

#include "headless_engine.hpp"
#include "trafo_tool.hpp"
#include "trafo_synth.hpp"
#include "crash_report.hpp"
#include "process_qos.hpp"
#include "physics_world.hpp"
#include "placement_tool.hpp"
#include "placement_trace.hpp"
#include "array_census.hpp"   // step K0
#include "probe_census.hpp"   // step WG0
#include <entity/session.hpp>
#include <game/script_replay.hpp>
#include <game/session_host_support.hpp>
#include <physics/physics_savestate.hpp>
#include <physics/physics_state.hpp>
#include <session/journal.hpp>
#include <session/lifecycle.hpp>
#include <physics/physics_direct.hpp>
#include <session/rollback_binding.hpp>
#include <session/rollback_binding_scripts.hpp>
#include <session/rollback.hpp>

#include "CKAll.h"
#include "CKIpionManager.h"
#include "PhysicsCallback.h"   // step EG0: --queue-trace reads m_PreSimulateCallbacks
#include <typeinfo>

#include <physics/tick_record.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#if defined(_MSC_VER)
#include <crtdbg.h>
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The IVP environment's random generator (ivp_utility), as physics_world.cpp
// declares it: --legacy-restore puts the cursor back the way the old per-body
// restore path did not need to (the blob path carries it itself).
int ivp_srand_read();
void ivp_srand(int seed);

namespace {
    struct arguments {
        std::string root;
        std::string replay;
        // --replay-session <file.bmjr>: the session black box (design 9.15).
        // --list prints the file and simulates nothing (no --root needed);
        // --journal-trace turns on the world's per-tick diagnostics;
        // --write-journal writes this replay's own journal so two platforms'
        // replays can be diffed with scripts/journal_trace.py --diff;
        // --continue-after-jump replays past a hole in the tick numbering
        // (which the replay would otherwise stop at: the world ticks once per
        // journal tick, so past a hole it is simply on the wrong tick).
        // A client journal's RESYNC record (journal v4, resync design 4.5,
        // step T1) is restored where it stands - the server's state of tick S
        // put into the replayed world, which goes on from it - and a hole its
        // renumbering left is no reason to stop; --ignore-resync replays past
        // the record as if it were not there (the pre-T1 replay, for an A/B).
        std::string replay_session;
        // --synth-trafo-session DIR (trafo-replication design 7.2 S10, step
        // T2): a server session with the trafo inputs on the real runner,
        // its journal written to DIR (sim/trafo_synth.hpp); --synth-ticks N.
        std::string synth_trafo_session;
        uint32_t synth_ticks = 1560;
        bool list_journal = false;
        bool stop_on_divergence = false;
        bool continue_after_jump = false;
        bool ignore_resync = false;
        bool legacy_first_step = false;   // --legacy-first-step (step B2SW, T1 b): step the groups before the first simulated one
        bool journal_trace = false;
        std::string write_journal;
        // --io-flush (step IOF): the headless client's spelling (MODE or
        // log=..,journal=..,capture=..).  The SimTool has no I/O strand: its
        // --write-journal is written on its own thread with the synchronous
        // journal's flush points, which no mode changes (io_queue.hpp).
        bmmo::session::io_flush_config io_flush;
        int level = 0;
        int ticks = 660;
        int level_at_tick = 200;
        int report_every = 66;
        int boot_ticks = 400;          // ticks to let the composition reach the menu
        int anchor_timeout = 2000;     // ticks to wait for Gameplay_Ingame after Load Level
        bool verbose = false;
        std::string trace_script;      // dump this script's active blocks for the first trace_ticks ticks
        int trace_ticks = 0;
        int debug_ticks = 0;           // run the first N ticks through the CK debug stepper
        bool direct_load = false;      // --direct-load: legacy "Load Level" message instead of the menus
        long long exact_from = -1;     // --exact-frames FROM TO: exact core dumps after those replay frames
        std::string write_record;      // --write-record PATH: this run's hashes as a new BMRC (same keys)
        long long bodies_from = -1;    // --bodies-frames FROM TO: bodies + lifecycle events after those replay frames
        long long bodies_to = -1;
        long long debug_from = -1;     // --debug-frames FROM TO: block-level execution trace of those replay frames
        long long debug_to = -1;
        long long ivp_trace_from = -1; // --ivp-trace FROM TO PATH: IVP's own impact debug prints for those frames
        long long ivp_trace_to = -1;
        std::string ivp_trace_path;
        long long exact_to = -1;
        std::string dump_array;        // --dump-array NAME: print every cell of a CKDataArray
        std::string dump_entity;       // --dump-entity NAME: world matrix of a 3D entity at --dump-at (replay: before and after that frame)
        // --beam X Y Z FRAME: put the current ball there at rest before that
        // replay frame, mirroring the client automation verb "beam", so a
        // mechanism can be hit the same way on both engines.
        double beam_at[3] = {};
        long long beam_frame = -1;
        // --sector N FRAME, repeatable: activate that sector before that frame
        std::vector<std::pair<int, long long>> sectors;
        std::string list_scripts;      // --list-scripts SUBSTR: every root script whose name or owner matches
        std::string dump_script;       // print this script's whole graph (blocks, parameters, links)
        int dump_at = -1;              // tick at which to dump (-1: after the run)
        std::string nav_mode;          // --nav retail-cxx|clone: replay through the session navigation (design 8.6)
        long long nav_frames = -1;     // --nav-frames N: compare only the first N frames
        int list_bodies_at = -1;       // --list-bodies-at N: bridge v2 list_bodies() at that tick
        int dump_surfaces_at = -1;     // --dump-surfaces-at N: per-body surface signatures at that tick
        // --drop ENTITY BALLTYPE: server-side mechanism check.  Boots a
        // physics-session world, puts the player in a sector, drops their ball
        // of that type on that entity and prints what the mechanism does.
        std::string drop_entity;
        int drop_ball = 0;
        float drop_height = 4.0f;      // --drop-height F: metres above the entity
        int drop_sector = 1;           // --drop-sector N: the sector the player reports
        int drop_second_sector = 0;    // --drop-second-sector N: a second player, parked there
        int drop_settle = 30;          // --drop-settle N: ticks between the sector and the drop
        int drop_move_sector = 0;      // --drop-move SECTOR TICK: the player moves on at that tick
        int drop_move_tick = 0;
        bool n2_pin = true;            // --n2-pin on|off (step B2SW): the chains' deactivate cell pinned at the anchor
        // --session-recipe (step B2SW, Q5): savestate-check binds the clients'
        // recipe.  Step F0 (RLM's queued harness item, option (a)): the
        // default when the scene can bind it (--frames, no --legacy-restore,
        // no trafo inputs) - every core host binds it, so a chained round on
        // the old v14 recipe could keep a script state no host holds (WG2's
        // 144:4 / 87:9); --v14 asks for the old recipe.  -1 = the default.
        int session_recipe = -1;
        // --savestate-rounds T:D[:F],... (step B2SX, Q5): these rounds instead
        // of the seeded schedule - anchor tick T, depth D, then F live frames
        // past the round's end compared with the straight pass (the follow).
        std::vector<std::array<int, 3>> savestate_explicit;
        bool n2_cell = true;           // --n2-cell on|off (step B2SX): the checkpoint chain's private [0][1] cell
        bool cells_trace = false;      // --cells-trace (step B2SW): IngameParameter[0][1]/[0][2], the union, the bodies
        float drop_at[3] = {};         // --drop-at X Y Z: drop there instead (PH matrix of a module)
        bool have_drop_at = false;
        // --drop-prop ENTITY: beam that level prop (a non-player ball placed by
        // the PH table) onto the drop spot as well, to check that a mechanism
        // still only reacts to the player's ball.
        std::string drop_prop;
        float drop_player_at[3] = {};  // --drop-player-at X Y Z: the player's ball goes here instead
        bool have_drop_player_at = false;
        // Step G2 (world-agreement-gaps-design.md §7.2 V2, V4): --drop-second-ball
        // TYPE X Y Z: player 2 (added with join order 1 unless
        // --drop-second-sector made it) physicalizes a ball of TYPE at X Y Z
        // right after player 1's.  --probe-trace: every report line of the
        // drop scene also prints the proximity union's frames and identity
        // copies whose names contain the --drop name.
        int drop_second_ball = -1;
        // Step Q7 (world-agreement-gaps-design.md §7.2 V6, V7):
        // --player-mechanisms K - the drop scene's world copies the
        // per-player units K times (with --world-options PROXIMITY_UNION;
        // the bit WORLD_PLAYER_MECHANISMS is implied), and --probe-census
        // --release-check rewires the clients' class with K slots (V6).
        int mechanism_slots = 0;
        float drop_second_at[3] = {};
        bool probe_trace = false;
        // --drop-kill TICK (step G2, V4's no-candidate interval): before world
        // tick TICK every player's ball is unphysicalized (an Unphysicalize
        // event each, players 1 and 2); with --savestate-check it must come
        // before the window (the lead), which no replay re-applies.
        long long drop_kill = -1;
        // --drop-kill-player ID (stage A close-out step S2, V4b): --drop-kill
        // unphysicalizes only that player's ball (1 or 2); 0 = both.
        uint32_t drop_kill_player = 0;
        // --late-ball TYPE X Y Z TICK (close-out step S2, RO2 deferred (b)):
        // in a --late-run, player 1's ball of TYPE physicalized at rest at X Y
        // Z before root+TICK (a tick after A), the same in the control, the
        // arm and the reference: a ball moving through the late pass's pieces
        // after the replay.
        struct late_ball_spec { int type = -1; double at[3] = {}; long long tick = -1; } late_ball;
        // --probe-scramble before|after (step G2's review, V4's hold): each
        // savestate round moves every proximity frame 1000 m along x just
        // before ('before': the restore must put the hold back) or just
        // after ('after': a mutation standing in for a lost hold restore,
        // which the round must catch) the anchor's step state is restored.
        int probe_scramble = 0;   // 0 off, 1 before, 2 after
        // --spawn-test N: spawn-impulse determinism check (design 9.10 / spec
        // A.9).  Boots a session world, physicalizes N players at the level's
        // resetpoint in the same tick and checks that the spawn kick moves
        // them apart without producing NaN.
        int spawn_test = 0;
        float spawn_impulse = 3.0f;    // --spawn-impulse S: kick speed, m/s
        int spawn_ball = 1;            // --spawn-ball T: retail ball type row (default 1 = Wood)
        // --ticks / --report-every default differently for --spawn-test (200
        // / 10 instead of the free-run defaults 660 / 66); track whether the
        // user actually passed them.
        bool ticks_explicit = false;
        bool report_every_explicit = false;
        // --explode wood|paper|stone AT_TICK, repeatable: trafo-explosion
        // determinism check (Part B).  Free run only (no --level-at needed:
        // the level must already be running at AT_TICK, e.g. after
        // --level-at).  A second explosion checks that the pieces of the
        // previous one were really given back (engine changes #6 and #13).
        std::vector<std::pair<std::string, long long>> explosions;
        // --activate SCRIPT TICK, repeatable: activate any root script at that
        // tick.  The trafo sequence runs Ball_ResetPieces_<type> a moment
        // after the explosion (fade, De Physicalize, Restore IC); --explode
        // alone does not, so a two-explosion check has to ask for it.
        std::vector<std::pair<std::string, long long>> activations;
        // --body-guard ENTITY TICK: arm the session body guard (engine change
        // #6) on that entity from that tick, the way a physics session does,
        // so a free run can check what the guard does to the level's bodies.
        std::string body_guard_entity;
        long long body_guard_tick = -1;
        // --restore-at TICK [ENTITY], repeatable: right after that tick's step
        // read every movable body's state (or only ENTITY's) and write it
        // straight back through the v10 body write with wake_mode keep and no
        // recheck.  A rollback restore does exactly this to the bodies it
        // corrects, so a free run with and without it measures what the write
        // itself costs the simulation: without the leapfrog restore the run
        // diverges from the untouched one by one PSI of motion per body per
        // restore (design 9.25 / findings A3).
        std::vector<std::pair<long long, std::string>> restores;
        // --track ENTITY, repeatable: that body's pose and velocity after
        // every tick, one line each, so two runs can be subtracted body by
        // body instead of only compared by hash.
        std::vector<std::string> tracked;
        // --savestate-check: the IVP savestate's robustness matrix (see
        // run_savestate_check).  --ticks is the straight pass's length (200
        // when not given), --rounds the number of chained restores, --max-depth
        // the deepest replay, --seed the schedule's seed (NOT the world's: the
        // world keeps seed 1 like every physics_world mode here),
        // --savestate-every K keeps the full blob of every K-th tick plus every
        // tick the schedule anchors at or ends on (every other tick is still
        // compared by its blob hash), --savestate-lead L the whole world ticks
        // between the scene's last event and the window, --frames steps whole
        // engine frames instead of physics-only steps, --legacy-restore is the
        // negative control (the per-body restore path, which must FAIL on a
        // scene with contacts) and --savestate-trace appends per-tick and
        // per-round lines to a file for cross-process diffs.
        bool savestate_check = false;
        // --savestate-diff A B / --savestate-describe FILE: blobs the headless
        // client wrote with --dump-savestate, compared field by field or
        // printed whole - the tool that names the first differing field when
        // two clients' journals only show a position drift.
        std::string savestate_diff[2];
        std::string savestate_describe;
        std::string describe_script;      // --describe-script NAME: print that root script's graph
        // --portable-scripts-check (resync design 7.2, step A1): the portable
        // script record against the v2 record, at anchor+30 and after each
        // sector 2..N of the level (--drop-settle ticks apart); see
        // run_portable_scripts_check.
        bool portable_scripts_check = false;
        // --resync-check S B (resync design 7.2, step A2): the resync apply
        // offline, arms E/R/S/A; S and B are ticks after the scene's root
        // (see run_resync_check).
        bool resync_check = false;
        int resync_s = 0, resync_b = 0;
        std::string resync_arms = "E";
        std::vector<std::pair<int, int>> resync_stamps;   // --resync-stamp SECTOR TICK (default: sector 1 at 0)
        int resync_sector = 0;            // --resync-sector K: the sector arms S and A shift
        int resync_shift = 9;             // --resync-shift N
        int resync_perturb_ticks = 17;    // --resync-perturb-ticks N: arm R's perturbation at S-N
        double resync_perturb = 0.01;     // --resync-perturb M: metres (and 5*M m/s on awake bodies)
        int resync_after = 600;           // --resync-after N: compared up to B+N
        int resync_root = 240;            // --resync-root N: world ticks before the root (the retail ball parked)
        int resync_harness_diag = -1;     // --resync-harness-diag T: warm-up vs restored run at root+T (scripts, blob)
        int resync_drop = -1;             // --resync-drop TICK: player 1's --drop ball physicalized then
        std::vector<std::string> resync_watch;   // --resync-watch NAME: the gate's bodies (with the K sector's)
        double resync_gate_mm = 1.0;      // --resync-gate MM
        bool resync_destroy_after_blob = false;   // --resync-destroy-after-blob: arm A's destroy after the blob (A/B)
        bool resync_reregister = false;   // --resync-reregister: the apply re-registers restored-active scripts (A/B)
        bool resync_write_equal_rows = false;   // --resync-write-equal-rows: write rows the body already has (A/B)
        int resync_player = 1;            // --resync-player ID: the player id this process joins (its clone "_BMMO_<ID>")
        std::string resync_server_blob;   // --resync-server-blob FILE: another process's server blob for the r/s/a arms
        std::vector<std::pair<std::string, std::string>> resync_blob_rename;   // --resync-blob-rename OLD NEW
        bool resync_blob_adopt_env = false;   // --resync-blob-adopt-environment: the foreign blob's environment id := ours
        bool resync_perturb_scripts = false;  // --resync-perturb-scripts: arm R as 7.2 words it (scripts of S-N restored at S-N)
        // --late-sector-check K T A (resync design 7.2, step B1): sector K
        // stamped at T, delivered at A (see run_late_sector_check).
        bool late_sector_check = false;
        int late_sector = 0, late_t = 0, late_a = 0;
        int late_moved = -1;              // --late-moved T2: K an own stamp at T the server moved to T2 (echo at A)
        std::vector<std::pair<int, int>> late_also;   // --late-also SECTOR TICK: other stamps, on time
        int late_after = 600;             // --late-after N: compared A..A+N
        int late_root = 240;              // --late-root N: world ticks before the root
        int late_expect_forgotten = -1;   // --late-expect-forgotten N: the live passes the re-derivation must forget
        bool late_keep = false;           // --late-keep: keep the runs' capture files
        bool late_ordinary = false;       // --late-ordinary: the arm's K on time, an ordinary rewind at A (the reference)
        bool late_reference = false;      // --late-reference: the check runs that reference too and names what it shares
        std::string late_run;             // --late-run control|arm: one run (a child of the check)
        std::string late_out;             // --late-out PATH: the run's capture file, the check's prefix
        std::string self;                 // this program, absolute (the check runs itself)
        std::vector<std::string> raw;     // the arguments as given
        bool ignore_ov = false;           // --ignore-ov: --savestate-diff leaves the overlap tree out
        bool ignore_order = false;        // --ignore-order: ...and the cache ring and unit numbering
        int max_diff = 1;                 // --max-diff N: --savestate-diff lists up to N differing fields
        int savestate_rounds = 50;
        int savestate_max_depth = 48;
        unsigned long long savestate_seed = 1;
        int savestate_every = 1;
        int savestate_lead = 1;
        // --savestate-inert-from-birth (step S7, a DIAGNOSTIC, never a PASS): a
        // round anchored before a FIXED window birth, which the restore keeps as
        // an inert unknown body until its birth tick (rollback.hpp), is compared
        // from that birth's record on only.
        bool savestate_inert_from_birth = false;
        bool savestate_frames = false;
        bool legacy_restore = false;
        // Step RLQ (gap Q's hold in the harness): --savestate-check binds the
        // clients' PreSimulate outside-entry hold (B7EG's Q4, and with the
        // clients' recipe the queue section too); --savestate-no-hold leaves
        // both unbound, the binding before RLQ (the hold's negative control:
        // a window across the level start's parked-ball entries refuses).
        bool savestate_no_hold = false;
        // Step RLM: --savestate-no-ledger empties script_replay's
        // registration ledger before every restore and every replayed frame,
        // so nothing is taken back - the restore before RLM (the G1
        // membership leak's negative control; the witness line shows it).
        bool savestate_no_ledger = false;
        // Step RLC's review: --joint-shuffle takes every constraint a joint
        // block holds out and makes it again from its own image before each
        // round's rewind (highest block id first), so the cores' lists hold
        // them in another order than the anchor's record names - what a
        // constraint shut down and made again inside the window leaves;
        // --joint-order-off is its control (the restore leaves that order).
        bool joint_shuffle = false;
        bool joint_order_off = false;
        std::string savestate_trace;
        // The trafo sets (docs/trafo-replication-design.md 7.2, step T1):
        // --trafo-sets N / --trafo-players a,b,.. [--trafo-reverse]
        // [--trafo-anchor TICK] [--trafo-seed S] [--explode-player ID TYPE
        // TICK X Y Z] [--reset-player ID TYPE TICK] [--explode-original TYPE
        // TICK X Y Z SEED] [--describe-trafo] [--trafo-hold
        // [--trafo-hold-slip] [--trafo-no-option-e]] [--trafo-filter on|off]
        // [--trafo-track ID|orig TYPE] [--trafo-presim] [--trafo-contacts]
        // [--trafo-surfaces] [--trafo-no-collsound] [--trafo-record-check]
        // [--trafo-colldump] [--trafo-poke-create ID TICK]
        // [--trafo-built-params] [--trafo-ensure ID TICK] [--list-trafos].
        // See sim/trafo_tool.hpp.
        bmmo::sim::trafo_tool_options trafo;
        bool list_trafos = false;
        // --set-local SCRIPT LOCAL VALUE TICK, repeatable (free run, S12): an
        // integer local of a root script set before that tick's frame (the
        // lives of Gameplay_Ingame, "ActLifes", for a Game Over).
        struct local_write { std::string script, local; int value = 0; long long tick = 0; };
        std::vector<local_write> local_writes;
        // --beam-again X Y Z TICK, repeatable (free run, S12): --beam's move
        // of the current ball at further ticks (a death below the level).
        struct beam_write { double at[3] = {}; long long tick = 0; };
        std::vector<beam_write> beams_again;
        std::string list_entities;   // --list-entities PREFIX: as --list-trafos, any name prefix
        std::string list_group;      // --list-group NAME (close-out step S2): with --list-placement, the group's members
        // --list-placement (sector placement design 7.2 V1, step S1): the
        // PH row plan of --level at its anchor, read-only (sim/placement_tool.hpp).
        bool list_placement = false;
        // The sector placement (design 7.2, step S2): --placement on|off
        // (default off) builds every physics_world with it; with
        // --list-placement the listing then applies it in the tool (censuses,
        // fingerprints, signatures, the name review).  --placement-fail-at
        // ROW: step 5 refuses at that row (V12).  --tick-hashes (--drop):
        // the world hash after every tick (V3).  --watch-entity NAME,
        // repeatable (--drop): that entity's world position at every report
        // and at the end (V4/V5: trafos, extras, the player's ball).
        bool placement = false;
        int placement_fail_at = -1;
        bool tick_hashes = false;
        std::vector<std::string> watch_entities;
        // The invariant check I1 (design 7.3, step S3), in the drop scene:
        // --second-move SECTOR TICK, repeatable: the parked second player of
        // --drop-second-sector reports that sector before world tick TICK
        // (the world's tick index: the settle's ticks count, so 0 is before
        // the first settle tick).  --drop-again X Y Z TICK, repeatable: the
        // player's ball is beamed there at rest before world tick TICK (a
        // second drop).  --placement-trace PATH: after every world tick, the
        // --drop-sector rows' hierarchies (sim/placement_trace.hpp);
        // --placement-trace-ball: player 1's ball too, its own line.
        struct second_move { int sector = 0; long long tick = 0; };
        std::vector<second_move> second_moves;
        struct drop_again { double at[3] = {}; long long tick = 0; };
        std::vector<drop_again> drops_again;
        std::string placement_trace;
        bool placement_trace_ball = false;
        // The networked invariant (design 7.4 "L8 invariant (networked)",
        // step SP8), in a --replay-session: --placement-trace PATH traces the
        // --drop-sector rows (default 1) after every replayed tick;
        // --placement-trace-player ID adds that player's ball line (implies
        // --placement-trace-ball); --drop-sectors-above N skips every Sector
        // event of a sector above N (the replay's second arm).  0 / -1: not given.
        uint32_t placement_trace_player = 0;
        int drop_sectors_above = -1;
        // The id layout (design 7.2 V7/V9, step S7), in every session world
        // this tool makes: --perturb-ids N creates N CK objects after the
        // level load and destroys them before the anchor (ids are not
        // reused: the copies land above a gap of N); --free-ids-before-load
        // N creates N before the level request and destroys them with
        // CK_DESTROY_FREEID one by one in ascending id order, so the load and
        // then the copies take ids from the free list top-down (the copies
        // below their originals, F13).  Either prints one copy/original id
        // pair; -1 / 0: not given.
        int perturb_ids = -1;
        int free_ids_before_load = 0;
        // Step EG0 (rollback-engine-gaps-design.md §8 G0), diagnostics only,
        // off by default: --list-callback-blocks (a level run, at --dump-at
        // or the end): every block of the ten prototypes that queue into the
        // PreSimulate queue, with its root script, selected or not, and its
        // target; --queue-trace (a --replay run): after every frame where it
        // changes, the queue's entries and the current ball's body.
        bool list_callback_blocks = false;
        bool queue_trace = false;
        // --ball-off FROM TO (a --replay run; design 7.2 Q-S2's switch): the
        // current ball's body taken out before replay frame FROM and made
        // again before frame TO with the retail recipe, while the game goes
        // on as if it had one (the A6 drop's state: its blocks queue).
        long long ball_off_from = -1, ball_off_to = -1;
        // --ball-off-wake-only: before the body is made again, every queued
        // entry of the ten prototypes but the Physics WakeUps is dropped (the
        // WakeUp fact of design 2.2.4 measured alone; changes the run).
        bool ball_off_wake_only = false;
        // --ball-off-drop-all: every queued entry of the ten prototypes
        // dropped there, WakeUps too (the fact's control arm: the same
        // inputs as --ball-off-wake-only, nothing pending at the re-make).
        bool ball_off_drop_all = false;
        // Step B7EG (rollback-engine-gaps design 7.2 Q-S2's queue half, gap
        // Q): --queue-roundtrip (a --replay run, with --ball-off to make the
        // queue hold entries): after every frame whose PreSimulate queue has
        // a retail entry, the bridge's hold of every retail entry (an empty
        // executed set), its put-back, and the rebuild of the entries'
        // scripts from the queue as it was; each must give the queue back
        // unchanged (order included), and the run's --write-record must
        // equal the same run's without the switch.
        bool queue_roundtrip = false;
        // Step WG0 (world-agreement-gaps-design.md §7.2 V1, §8 G0):
        // --probe-census, the proximity census of --level's session world at
        // its anchor (sim/probe_census.hpp): per class the blocks the
        // design's rewire would take, their names, facts, identity reads,
        // gating probes, units and the signature; read-only, a mode of its
        // own (--level N [--placement on] [--world-options B]).
        bool probe_census = false;
        // Step WG1 (§7.2 V5): --release-check with --probe-census: after the
        // census, the world's proximity union is released, rewired (the
        // anchor's bit-clear rewire), released, rewired under the union rules
        // for the mechanism class (the clients' rewire, §2.3) and released
        // again; each state is checked against the first census.
        bool probe_release_check = false;
        // Step K0 (record-format-gaps-design.md §8, read-only tools):
        // --array-reads [--client-scope] [--array-reads-all]: the array-read
        // census of --level's session world at its anchor (design 2.2, S2;
        // sim/array_census.hpp).  --globals-trace: in the drop and sector
        // scenes, one line per world tick with the physics time factor, the
        // physics delta and the gravity (S5).  --world-options BITS: the
        // session world's session::WORLD_* bits, as a SessionStart carries
        // them (a number, or names joined by ',' or '|'); only the bits that
        // exist: bit 0 the sector placement's (= --placement on), bit 1
        // WORLD_TRAFO_INPUTS.
        bool array_reads = false;
        bool array_reads_client = false;
        bool array_reads_all = false;
        // Step F4: --collectible-fixture other-writer|other-row (with
        // --array-reads): the negative fixture of the per-world collectible
        // rule (session/collectible_rules.hpp).  Before the census, the first
        // P_Extra_* Set Cell whose row is an Iterator If's output is made to
        // break the rule in the loaded level only: other-writer renames its
        // root script (a non-P_Extra writer of the column), other-row feeds
        // its Row Index from a new constant local (a P_Extra writer of
        // another row).  The read must stay unrecorded, its note saying why.
        std::string collectible_fixture;
        bool globals_trace = false;
        bool world_options_given = false;
        uint32_t world_options = 0;
        // Step RK3: --tutorial-exit-frame N (a measurement switch, with
        // --world-options SESSION_CLOCK on --level 1): the frame before which
        // the world activates the tutorial's exit; default the shared
        // constant every session world uses (game/tutorial_exit.hpp).
        int tutorial_exit_frame = bmmo::game::kTutorialExitFrame;
        bool tutorial_exit_frame_given = false;
        bool placement_given = false;   // --placement was on the command line (checked against bit 0)
    };

    // The sector placement switches (step S2); false: not one of them, or a
    // value is missing or malformed.
    bool parse_placement_arg(const std::string& arg, const std::function<const char*()>& next, arguments& out) {
        const char* v = nullptr;
        if (arg == "--placement") {
            if (!(v = next())) return false;
            const std::string value = v;
            if (value != "on" && value != "off") return false;
            out.placement = value == "on";
            out.placement_given = true;   // step K0
        }
        else if (arg == "--placement-fail-at") { if (!(v = next())) return false; out.placement_fail_at = std::atoi(v); }
        else if (arg == "--tick-hashes") out.tick_hashes = true;
        else if (arg == "--watch-entity") { if (!(v = next())) return false; out.watch_entities.emplace_back(v); }
        else if (arg == "--placement-trace") {
            // Resolved now: the engine makes the game directory the working directory.
            if (!(v = next())) return false;
            std::error_code ec;
            out.placement_trace = std::filesystem::absolute(v, ec).string();
            if (ec) return false;
        }
        else if (arg == "--placement-trace-ball") out.placement_trace_ball = true;
        else if (arg == "--placement-trace-player") {   // step SP8, --replay-session only
            if (!(v = next())) return false;
            const long long id = std::atoll(v);
            if (id <= 0 || id > 0xffffffffLL) return false;
            out.placement_trace_player = static_cast<uint32_t>(id);
            out.placement_trace_ball = true;
        }
        else if (arg == "--drop-sectors-above") {   // step SP8, --replay-session only
            if (!(v = next())) return false;
            out.drop_sectors_above = std::atoi(v);
            if (out.drop_sectors_above < 0) return false;
        }
        else if (arg == "--second-move") {
            arguments::second_move m;
            if (!(v = next())) return false; m.sector = std::atoi(v);
            if (!(v = next())) return false; m.tick = std::atoll(v);
            if (m.sector < 1 || m.tick < 0) return false;
            out.second_moves.push_back(m);
        }
        else if (arg == "--drop-again") {
            arguments::drop_again d;
            for (double& k: d.at) { if (!(v = next())) return false; k = std::atof(v); }
            if (!(v = next())) return false; d.tick = std::atoll(v);
            if (d.tick < 0) return false;
            out.drops_again.push_back(d);
        }
        else return false;
        return true;
    }

    // The placement's physics hooks over this tool's engine (design 2.3
    // step 3): game/sector_placement.hpp's placement_hooks_for, the one copy
    // every host uses (step HU1).  `bodies` must outlive the hooks.
    bmmo::game::placement_hooks tool_placement_hooks(CKIpionManager* manager, int fail_at, std::set<std::string>& bodies) {
        return bmmo::game::placement_hooks_for(bmmo::physics::physics_direct(manager), bodies, fail_at);
    }

    // --replay --placement on: the placement applied at the record's first
    // frame (the engine's anchor), its log on stderr.  False (printed) when
    // it refused.
    bool apply_tool_placement(CKContext* context, CKIpionManager* manager, const arguments& args,
                              bmmo::game::sector_placement& placement) {
        std::set<std::string> bodies;
        const std::string refused = placement.apply(context, tool_placement_hooks(manager, args.placement_fail_at, bodies));
        std::fprintf(stderr, "%s", placement.describe().c_str());
        std::fprintf(stderr, "placement signature %016llx\n", static_cast<unsigned long long>(placement.signature(context)));
        if (!refused.empty()) std::fprintf(stderr, "sector placement refused: %s\n", refused.c_str());
        return refused.empty();
    }

    // The trafo set switches (sim/trafo_tool.hpp) and --set-local: parse()'s
    // chain is at the compiler's nesting limit.  False: not one of them, or a
    // value is missing.
    bool parse_trafo_arg(const std::string& arg, const std::function<const char*()>& next, arguments& out) {
        const char* v = nullptr;
        if (arg == "--trafo-sets") {
            if (!(v = next())) return false;
            out.trafo.players.clear();
            for (int k = 1; k <= std::atoi(v); ++k) out.trafo.players.push_back(static_cast<uint32_t>(k));
        }
        else if (arg == "--trafo-players") {
            if (!(v = next())) return false;
            out.trafo.players.clear();
            for (const char* c = v; *c;) {
                out.trafo.players.push_back(static_cast<uint32_t>(std::strtoul(c, nullptr, 10)));
                while (*c && *c != ',') ++c;
                if (*c == ',') ++c;
            }
        }
        else if (arg == "--trafo-reverse") out.trafo.reverse = true;
        else if (arg == "--trafo-anchor") { if (!(v = next())) return false; out.trafo.anchor = std::atoll(v); }
        else if (arg == "--trafo-seed") { if (!(v = next())) return false; out.trafo.session_seed = static_cast<uint32_t>(std::strtoul(v, nullptr, 0)); }
        else if (arg == "--explode-player" || arg == "--reset-player") {
            bmmo::sim::trafo_tool_options::input in;
            const bool reset = arg == "--reset-player";
            in.flags = reset ? bmmo::session::TRAFO_RESET : bmmo::session::TRAFO_EXPLODE;
            if (!(v = next())) return false; in.player = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
            if (!(v = next()) || !bmmo::sim::trafo_type_of(v, in.type)) return false;
            if (!(v = next())) return false; in.tick = std::atoll(v);
            if (!reset)
                for (int k = 0; k < 3; ++k) { if (!(v = next())) return false; in.position[k] = static_cast<float>(std::atof(v)); }
            out.trafo.inputs.push_back(in);
        }
        else if (arg == "--explode-original") {
            bmmo::sim::trafo_tool_options::original o;
            if (!(v = next()) || !bmmo::sim::trafo_type_of(v, o.type)) return false;
            if (!(v = next())) return false; o.tick = std::atoll(v);
            for (int k = 0; k < 3; ++k) { if (!(v = next())) return false; o.position[k] = static_cast<float>(std::atof(v)); }
            if (!(v = next())) return false; o.seed = static_cast<int32_t>(std::strtol(v, nullptr, 0));
            out.trafo.originals.push_back(o);
        }
        else if (arg == "--describe-trafo") out.trafo.describe = true;
        else if (arg == "--trafo-hold") out.trafo.hold = true;
        else if (arg == "--trafo-hold-slip") out.trafo.hold_slip = true;
        else if (arg == "--trafo-no-option-e") out.trafo.option_e = false;
        else if (arg == "--trafo-filter") { if (!(v = next())) return false; out.trafo.filter = std::string(v) == "on" ? 1 : 0; }
        else if (arg == "--trafo-track") {
            std::string who;
            uint8_t type = 0;
            if (!(v = next())) return false; who = v;
            if (!(v = next()) || !bmmo::sim::trafo_type_of(v, type)) return false;
            out.trafo.track.emplace_back(who, type);
        }
        else if (arg == "--trafo-presim") out.trafo.presim = true;
        else if (arg == "--trafo-colldump") out.trafo.colldump = true;
        else if (arg == "--trafo-built-params") out.trafo.built_params = true;
        else if (arg == "--trafo-poke-create") {
            bmmo::sim::trafo_tool_options::poke k;
            if (!(v = next())) return false; k.player = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
            if (!(v = next())) return false; k.tick = std::atoll(v);
            out.trafo.pokes.push_back(k);
        }
        else if (arg == "--trafo-contacts") out.trafo.contacts = true;
        else if (arg == "--trafo-surfaces") out.trafo.surfaces = true;
        else if (arg == "--trafo-no-collsound") out.trafo.no_collsound = true;
        else if (arg == "--trafo-record-check") out.trafo.record_check = true;
        else if (arg == "--list-trafos") out.list_trafos = true;
        else if (arg == "--trafo-unmake" || arg == "--trafo-remake") {
            bmmo::sim::trafo_tool_options::unmake u;
            u.remake = arg == "--trafo-remake";
            if (!(v = next())) return false; u.player = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
            if (!(v = next()) || !bmmo::sim::trafo_type_of(v, u.type)) return false;
            if (!(v = next())) return false; u.tick = std::atoll(v);
            if (u.remake) {
                for (int k = 0; k < 3; ++k) { if (!(v = next())) return false; u.position[k] = static_cast<float>(std::atof(v)); }
                if (!(v = next())) return false; u.seed_tick = std::atoll(v);
            }
            out.trafo.unmakes.push_back(u);
        }
        else if (arg == "--trafo-final") {
            uint32_t player = 0;
            long long tick = 0;
            if (!(v = next())) return false; player = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
            if (!(v = next())) return false; tick = std::atoll(v);
            for (uint8_t t = 0; t < 3; ++t) {
                bmmo::sim::trafo_tool_options::input in;
                in.player = player;
                in.type = t;
                in.flags = bmmo::session::TRAFO_RESET | bmmo::session::TRAFO_SERVER | bmmo::session::TRAFO_FINAL;
                in.tick = tick;
                out.trafo.inputs.push_back(in);
            }
        }
        else if (arg == "--trafo-retire") out.trafo.retire = true;
        else if (arg == "--trafo-ensure") {
            bmmo::sim::trafo_tool_options::ensure en;
            if (!(v = next())) return false; en.player = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
            if (!(v = next())) return false; en.tick = std::atoll(v);
            out.trafo.ensures.push_back(en);
        }
        else if (arg == "--trafo-teardown") { if (!(v = next())) return false; out.trafo.teardown = std::atoll(v); }
        else if (arg == "--list-entities") { if (!(v = next())) return false; out.list_entities = v; }
        else if (arg == "--list-group") { if (!(v = next())) return false; out.list_group = v; }
        else if (arg == "--list-placement") out.list_placement = true;
        // Every --placement* option goes to parse_placement_arg, which rejects one it does not know:
        // a new one (S3's --placement-trace) is added there.
        else if (arg.rfind("--placement", 0) == 0 || arg == "--tick-hashes" || arg == "--watch-entity"
                 || arg == "--second-move" || arg == "--drop-again" || arg == "--drop-sectors-above")
            return parse_placement_arg(arg, next, out);
        else if (arg == "--beam-again") {
            arguments::beam_write b;
            for (int k = 0; k < 3; ++k) { if (!(v = next())) return false; b.at[k] = std::atof(v); }
            if (!(v = next())) return false; b.tick = std::atoll(v);
            out.beams_again.push_back(b);
        }
        else if (arg == "--set-local") {
            arguments::local_write w;
            if (!(v = next())) return false; w.script = v;
            if (!(v = next())) return false; w.local = v;
            if (!(v = next())) return false; w.value = std::atoi(v);
            if (!(v = next())) return false; w.tick = std::atoll(v);
            out.local_writes.push_back(w);
        }
        else return false;
        return true;
    }

    // ---- step K0 (record-format-gaps-design.md §8): the read-only tools'
    // switches.  False: not one of them, or a value is missing or malformed.
    bool parse_world_options_value(const std::string& text, uint32_t& out) {
        out = 0;
        if (text.empty()) return false;
        if (text[0] >= '0' && text[0] <= '9') {
            char* end = nullptr;
            const unsigned long long v = std::strtoull(text.c_str(), &end, 0);
            if (!end || *end || v > 0xFFFFFFFFull) return false;   // a u32, also where long is 64 bits
            out = static_cast<uint32_t>(v);
            return true;
        }
        size_t start = 0;
        while (start <= text.size()) {
            const size_t stop = text.find_first_of(",|", start);
            const std::string name = text.substr(start, stop == std::string::npos ? std::string::npos : stop - start);
            if (name == "PLACEMENT_CLONES") out |= bmmo::session::WORLD_PLACEMENT_CLONES;   // step S5's constant
            else if (name == "TRAFO_INPUTS") out |= bmmo::session::WORLD_TRAFO_INPUTS;
            else if (name == "PROXIMITY_UNION") out |= bmmo::session::WORLD_PROXIMITY_UNION;   // step G2
            else if (name == "PLAYER_MECHANISMS") out |= bmmo::session::WORLD_PLAYER_MECHANISMS;   // step Q7
            else if (name == "SESSION_CLOCK") out |= bmmo::session::WORLD_SESSION_CLOCK;   // step RK3
            else return false;
            if (stop == std::string::npos) break;
            start = stop + 1;
        }
        return true;
    }

    // Step G2's drop-scene switches (their own function: parse()'s else-if
    // chain is at MSVC's nesting limit, C1061).
    bool parse_union_arg(const std::string& arg, const std::function<const char*()>& next, arguments& out) {
        const char* v = nullptr;
        if (arg == "--drop-second-ball") {
            if (!(v = next())) return false;
            out.drop_second_ball = std::atoi(v);
            if (out.drop_second_ball < 0) return false;
            for (float& k: out.drop_second_at) { if (!(v = next())) return false; k = static_cast<float>(std::atof(v)); }
        }
        else if (arg == "--probe-trace") out.probe_trace = true;
        // Step F0: the v14 recipe (SimTool's old default; parse()'s chain is full).
        else if (arg == "--v14") out.session_recipe = 0;
        else if (arg == "--player-mechanisms") {   // step Q7
            if (!(v = next())) return false;
            out.mechanism_slots = std::atoi(v);
            if (out.mechanism_slots < 1 || out.mechanism_slots > static_cast<int>(bmmo::session::MAX_PLAYERS_PER_SESSION))
                return false;
        }
        else if (arg == "--drop-kill") {
            if (!(v = next())) return false;
            out.drop_kill = std::atoll(v);
            if (out.drop_kill < 0) return false;
        }
        else if (arg == "--drop-kill-player") {   // close-out step S2 (V4b)
            if (!(v = next())) return false;
            const long long id = std::atoll(v);
            if (id < 0 || id > 2) return false;
            out.drop_kill_player = static_cast<uint32_t>(id);
        }
        else if (arg == "--late-ball") {   // close-out step S2 (RO2 (b))
            if (!(v = next())) return false;
            {   // a ball type: digits only (std::atoi would read "Stone" as type 0)
                const std::string type = v;
                if (type.empty() || type.size() > 3 || type.find_first_not_of("0123456789") != std::string::npos) return false;
                out.late_ball.type = std::atoi(v);
            }
            for (double& k: out.late_ball.at) { if (!(v = next())) return false; k = std::atof(v); }
            if (!(v = next())) return false;
            out.late_ball.tick = std::atoll(v);
            if (out.late_ball.type < 0 || out.late_ball.tick < 1) return false;
        }
        else if (arg == "--probe-scramble") {
            if (!(v = next())) return false;
            const std::string mode = v;
            if (mode == "before") out.probe_scramble = 1;
            else if (mode == "after") out.probe_scramble = 2;
            else return false;
        }
        else return false;
        return true;
    }

    bool parse_record_format_arg(const std::string& arg, const std::function<const char*()>& next, arguments& out) {
        const char* v = nullptr;
        if (arg == "--array-reads") out.array_reads = true;
        else if (arg == "--client-scope") out.array_reads_client = true;
        else if (arg == "--array-reads-all") out.array_reads_all = true;
        else if (arg == "--collectible-fixture") {
            if (!(v = next())) return false;
            out.collectible_fixture = v;
            if (out.collectible_fixture != "other-writer" && out.collectible_fixture != "other-row") return false;
        }
        else if (arg == "--globals-trace") out.globals_trace = true;
        else if (arg == "--world-options") {
            if (!(v = next()) || !parse_world_options_value(v, out.world_options)) return false;
            out.world_options_given = true;
        }
        else if (arg == "--tutorial-exit-frame") {   // step RK3
            if (!(v = next())) return false;
            char* end = nullptr;
            const long frame = std::strtol(v, &end, 10);
            if (!end || *end || frame < 1 || frame > 100000) return false;
            out.tutorial_exit_frame = static_cast<int>(frame);
            out.tutorial_exit_frame_given = true;
        }
        else return false;
        return true;
    }

    bool parse(int argc, char** argv, arguments& out) {
        if (argc > 0) {
            std::error_code ignored;
            out.self = std::filesystem::absolute(argv[0], ignored).make_preferred().string();
        }
        for (int i = 1; i < argc; ++i) out.raw.emplace_back(argv[i]);
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
            const char* v = nullptr;
            if (arg == "--root") { if (!(v = next())) return false; out.root = v; }
            else if (arg == "--replay") { if (!(v = next())) return false; out.replay = v; }
            else if (arg == "--replay-session") { if (!(v = next())) return false; out.replay_session = v; }
            else if (arg == "--synth-trafo-session") { if (!(v = next())) return false; out.synth_trafo_session = v; }
            else if (arg == "--synth-ticks") { if (!(v = next())) return false; out.synth_ticks = static_cast<uint32_t>(std::atoi(v)); }
            else if (arg == "--list") out.list_journal = true;
            else if (arg == "--stop-on-divergence") out.stop_on_divergence = true;
            else if (arg == "--continue-after-jump") out.continue_after_jump = true;
            else if (arg == "--ignore-resync") out.ignore_resync = true;
            else if (arg == "--legacy-first-step") out.legacy_first_step = true;
            else if (arg == "--journal-trace") out.journal_trace = true;
            else if (arg == "--write-journal" || arg == "--io-flush") {   // one branch (C1061); --io-flush: step IOF
                if (!(v = next())) return false;
                if (arg == "--write-journal") out.write_journal = v;
                else if (!bmmo::session::parse_io_flush_config(v, out.io_flush)) return false;
            }
            else if (arg == "--level") { if (!(v = next())) return false; out.level = std::atoi(v); }
            else if (arg == "--ticks") { if (!(v = next())) return false; out.ticks = std::atoi(v); out.ticks_explicit = true; }
            else if (arg == "--level-at") { if (!(v = next())) return false; out.level_at_tick = std::atoi(v); }
            else if (arg == "--report-every") {
                if (!(v = next())) return false; out.report_every = std::atoi(v); out.report_every_explicit = true;
            }
            else if (arg == "--boot-ticks") { if (!(v = next())) return false; out.boot_ticks = std::atoi(v); }
            else if (arg == "--anchor-timeout") { if (!(v = next())) return false; out.anchor_timeout = std::atoi(v); }
            else if (arg == "--verbose") out.verbose = true;
            else if (arg == "--trace") { if (!(v = next())) return false; out.trace_script = v; }
            else if (arg == "--trace-ticks") { if (!(v = next())) return false; out.trace_ticks = std::atoi(v); }
            else if (arg == "--debug-ticks") { if (!(v = next())) return false; out.debug_ticks = std::atoi(v); }
            else if (arg == "--direct-load") out.direct_load = true;
            else if (arg == "--bodies-frames") {
                if (!(v = next())) return false; out.bodies_from = std::atoll(v);
                if (!(v = next())) return false; out.bodies_to = std::atoll(v);
            }
            else if (arg == "--debug-frames") {
                if (!(v = next())) return false; out.debug_from = std::atoll(v);
                if (!(v = next())) return false; out.debug_to = std::atoll(v);
            }
            else if (arg == "--ivp-trace") {
                if (!(v = next())) return false; out.ivp_trace_from = std::atoll(v);
                if (!(v = next())) return false; out.ivp_trace_to = std::atoll(v);
                if (!(v = next())) return false; out.ivp_trace_path = v;
            }
            else if (arg == "--write-record") { if (!(v = next())) return false; out.write_record = v; }
            else if (arg == "--exact-frames") {
                if (!(v = next())) return false; out.exact_from = std::atoll(v);
                if (!(v = next())) return false; out.exact_to = std::atoll(v);
            }
            // --from A --to B: the session-replay spelling of --exact-frames,
            // where the numbers are journal ticks rather than record frames.
            else if (arg == "--from") { if (!(v = next())) return false; out.exact_from = std::atoll(v); }
            else if (arg == "--to") { if (!(v = next())) return false; out.exact_to = std::atoll(v); }
            else if (arg == "--dump-array") { if (!(v = next())) return false; out.dump_array = v; }
            else if (arg == "--dump-entity") { if (!(v = next())) return false; out.dump_entity = v; }
            else if (arg == "--sector") {
                int sector = 0;
                if (!(v = next())) return false; sector = std::atoi(v);
                if (!(v = next())) return false; out.sectors.emplace_back(sector, std::atoll(v));
            }
            else if (arg == "--beam") {
                for (double& k: out.beam_at) { if (!(v = next())) return false; k = std::atof(v); }
                if (!(v = next())) return false; out.beam_frame = std::atoll(v);
            }
            else if (arg == "--list-scripts") { if (!(v = next())) return false; out.list_scripts = v; }
            else if (arg == "--dump-script") { if (!(v = next())) return false; out.dump_script = v; }
            else if (arg == "--dump-at") { if (!(v = next())) return false; out.dump_at = std::atoi(v); }
            else if (arg == "--nav") { if (!(v = next())) return false; out.nav_mode = v; }
            else if (arg == "--nav-frames") { if (!(v = next())) return false; out.nav_frames = std::atoll(v); }
            else if (arg == "--list-bodies-at") { if (!(v = next())) return false; out.list_bodies_at = std::atoi(v); }
            else if (arg == "--dump-surfaces-at") { if (!(v = next())) return false; out.dump_surfaces_at = std::atoi(v); }
            else if (arg == "--drop") {
                if (!(v = next())) return false; out.drop_entity = v;
                if (!(v = next())) return false; out.drop_ball = std::atoi(v);
            }
            else if (arg == "--drop-at") {
                for (float& k: out.drop_at) { if (!(v = next())) return false; k = static_cast<float>(std::atof(v)); }
                out.have_drop_at = true;
            }
            else if (arg == "--drop-height") { if (!(v = next())) return false; out.drop_height = static_cast<float>(std::atof(v)); }
            else if (arg == "--drop-sector") { if (!(v = next())) return false; out.drop_sector = std::atoi(v); }
            else if (arg == "--drop-second-sector") { if (!(v = next())) return false; out.drop_second_sector = std::atoi(v); }
            else if (arg == "--perturb-ids") {   // step S7
                if (!(v = next())) return false;
                out.perturb_ids = std::atoi(v);
                if (out.perturb_ids < 0) return false;
            }
            else if (arg == "--free-ids-before-load") {   // step S7
                if (!(v = next())) return false;
                out.free_ids_before_load = std::atoi(v);
                if (out.free_ids_before_load < 1) return false;
            }
            else if (arg == "--drop-settle") { if (!(v = next())) return false; out.drop_settle = std::atoi(v); }
            else if (arg == "--drop-prop") { if (!(v = next())) return false; out.drop_prop = v; }
            else if (arg == "--drop-player-at") {
                for (float& k: out.drop_player_at) { if (!(v = next())) return false; k = static_cast<float>(std::atof(v)); }
                out.have_drop_player_at = true;
            }
            else if (arg == "--drop-move") {
                if (!(v = next())) return false; out.drop_move_sector = std::atoi(v);
                if (!(v = next())) return false; out.drop_move_tick = std::atoi(v);
            }
            else if (arg == "--n2-pin") {
                if (!(v = next())) return false;
                const std::string mode = v;
                if (mode != "on" && mode != "off") return false;
                out.n2_pin = mode == "on";
            }
            else if (arg == "--n2-cell") {   // step B2SX
                if (!(v = next())) return false;
                const std::string mode = v;
                if (mode != "on" && mode != "off") return false;
                out.n2_cell = mode == "on";
            }
            else if (arg == "--cells-trace") out.cells_trace = true;
            else if (arg == "--session-recipe") out.session_recipe = 1;
            else if (arg == "--savestate-rounds") {   // step B2SX: T:D[:F],...
                if (!(v = next())) return false;
                std::string list = v;
                size_t at = 0;
                while (at <= list.size()) {
                    const size_t comma = std::min(list.find(',', at), list.size());
                    const std::string item = list.substr(at, comma - at);
                    std::array<int, 3> round{-1, -1, 0};
                    if (std::sscanf(item.c_str(), "%d:%d:%d", &round[0], &round[1], &round[2]) < 2
                            || round[0] < 0 || round[1] < 0 || round[2] < 0) return false;
                    out.savestate_explicit.push_back(round);
                    at = comma + 1;
                }
            }
            else if (arg == "--spawn-test") { if (!(v = next())) return false; out.spawn_test = std::atoi(v); }
            else if (arg == "--spawn-impulse") { if (!(v = next())) return false; out.spawn_impulse = static_cast<float>(std::atof(v)); }
            else if (arg == "--spawn-ball") { if (!(v = next())) return false; out.spawn_ball = std::atoi(v); }
            else if (arg == "--explode") {
                std::string type;
                if (!(v = next())) return false; type = v;
                if (!(v = next())) return false; out.explosions.emplace_back(type, std::atoll(v));
            }
            else if (arg == "--activate") {
                std::string script;
                if (!(v = next())) return false; script = v;
                if (!(v = next())) return false; out.activations.emplace_back(script, std::atoll(v));
            }
            else if (arg == "--body-guard") {
                if (!(v = next())) return false; out.body_guard_entity = v;
                if (!(v = next())) return false; out.body_guard_tick = std::atoll(v);
            }
            else if (arg == "--track") { if (!(v = next())) return false; out.tracked.emplace_back(v); }
            else if (arg == "--restore-at") {
                if (!(v = next())) return false;
                const long long tick = std::atoll(v);
                // The entity is optional (every movable body when it is
                // missing), so only take the next argument when it is not the
                // next option.
                std::string entity;
                if (i + 1 < argc && std::string_view(argv[i + 1]).substr(0, 2) != "--") entity = argv[++i];
                out.restores.emplace_back(tick, std::move(entity));
            }
            else if (arg == "--savestate-check") out.savestate_check = true;
            else if (arg == "--savestate-diff") {
                if (!(v = next())) return false;
                out.savestate_diff[0] = v;
                if (!(v = next())) return false;
                out.savestate_diff[1] = v;
            }
            else if (arg == "--savestate-describe") { if (!(v = next())) return false; out.savestate_describe = v; }
            else if (arg == "--describe-script") { if (!(v = next())) return false; out.describe_script = v; }
            else if (arg == "--portable-scripts-check") out.portable_scripts_check = true;
            else if (arg == "--resync-check") {
                out.resync_check = true;
                if (!(v = next())) return false; out.resync_s = std::atoi(v);
                if (!(v = next())) return false; out.resync_b = std::atoi(v);
            }
            else if (arg == "--resync-arms") { if (!(v = next())) return false; out.resync_arms = v; }
            else if (arg == "--resync-stamp") {
                int sector = 0;
                if (!(v = next())) return false; sector = std::atoi(v);
                if (!(v = next())) return false; out.resync_stamps.emplace_back(sector, std::atoi(v));
            }
            else if (arg == "--resync-sector") { if (!(v = next())) return false; out.resync_sector = std::atoi(v); }
            else if (arg == "--resync-shift") { if (!(v = next())) return false; out.resync_shift = std::atoi(v); }
            else if (arg == "--resync-perturb-ticks") { if (!(v = next())) return false; out.resync_perturb_ticks = std::atoi(v); }
            else if (arg == "--resync-perturb") { if (!(v = next())) return false; out.resync_perturb = std::atof(v); }
            else if (arg == "--resync-harness-diag") { if (!(v = next())) return false; out.resync_harness_diag = std::atoi(v); }
            else if (arg == "--resync-root") { if (!(v = next())) return false; out.resync_root = std::atoi(v); }
            else if (arg == "--resync-after") { if (!(v = next())) return false; out.resync_after = std::atoi(v); }
            else if (arg == "--resync-drop") { if (!(v = next())) return false; out.resync_drop = std::atoi(v); }
            else if (arg == "--resync-watch") { if (!(v = next())) return false; out.resync_watch.emplace_back(v); }
            else if (arg == "--resync-gate") { if (!(v = next())) return false; out.resync_gate_mm = std::atof(v); }
            else if (arg == "--resync-destroy-after-blob") out.resync_destroy_after_blob = true;
            else if (arg == "--resync-reregister") out.resync_reregister = true;
            else if (arg == "--resync-write-equal-rows") out.resync_write_equal_rows = true;
            else if (arg == "--resync-player") { if (!(v = next())) return false; out.resync_player = std::atoi(v); }
            else if (arg == "--resync-server-blob") { if (!(v = next())) return false; out.resync_server_blob = v; }
            else if (arg == "--resync-blob-adopt-environment") out.resync_blob_adopt_env = true;
            else if (arg == "--resync-perturb-scripts") out.resync_perturb_scripts = true;
            else if (arg == "--resync-blob-rename") {
                std::string from;
                if (!(v = next())) return false; from = v;
                if (!(v = next())) return false; out.resync_blob_rename.emplace_back(from, v);
            }
            else if (arg == "--late-sector-check") {
                out.late_sector_check = true;
                if (!(v = next())) return false; out.late_sector = std::atoi(v);
                if (!(v = next())) return false; out.late_t = std::atoi(v);
                if (!(v = next())) return false; out.late_a = std::atoi(v);
            }
            else if (arg == "--late-moved") { if (!(v = next())) return false; out.late_moved = std::atoi(v); }
            else if (arg == "--late-also") {
                int sector = 0;
                if (!(v = next())) return false; sector = std::atoi(v);
                if (!(v = next())) return false; out.late_also.emplace_back(sector, std::atoi(v));
            }
            else if (arg == "--late-after") { if (!(v = next())) return false; out.late_after = std::atoi(v); }
            else if (arg == "--late-root") { if (!(v = next())) return false; out.late_root = std::atoi(v); }
            else if (arg == "--late-expect-forgotten") { if (!(v = next())) return false; out.late_expect_forgotten = std::atoi(v); }
            else if (arg == "--late-keep") out.late_keep = true;
            else if (arg == "--late-ordinary") out.late_ordinary = true;
            else if (arg == "--late-reference") out.late_reference = true;
            else if (arg == "--late-run") { if (!(v = next())) return false; out.late_run = v; }
            else if (arg == "--late-out") { if (!(v = next())) return false; out.late_out = v; }
            else if (arg == "--ignore-ov") out.ignore_ov = true;
            else if (arg == "--ignore-order") out.ignore_order = true;
            else if (arg == "--max-diff") { if (!(v = next())) return false; out.max_diff = std::atoi(v); }
            else if (arg == "--rounds") { if (!(v = next())) return false; out.savestate_rounds = std::atoi(v); }
            else if (arg == "--max-depth") { if (!(v = next())) return false; out.savestate_max_depth = std::atoi(v); }
            else if (arg == "--seed") { if (!(v = next())) return false; out.savestate_seed = std::strtoull(v, nullptr, 0); }
            else if (arg == "--savestate-every") { if (!(v = next())) return false; out.savestate_every = std::atoi(v); }
            else if (arg == "--savestate-lead") { if (!(v = next())) return false; out.savestate_lead = std::atoi(v); }
            else if (arg == "--savestate-inert-from-birth") out.savestate_inert_from_birth = true;
            else if (arg == "--frames") out.savestate_frames = true;
            else if (arg == "--legacy-restore" || arg == "--savestate-no-hold" || arg == "--savestate-no-ledger"
                     || arg == "--joint-shuffle" || arg == "--joint-order-off")   // RLQ, RLM, RLC: one branch (C1061)
                (arg == "--legacy-restore" ? out.legacy_restore
                 : arg == "--savestate-no-hold" ? out.savestate_no_hold
                 : arg == "--joint-shuffle" ? out.joint_shuffle
                 : arg == "--joint-order-off" ? out.joint_order_off : out.savestate_no_ledger) = true;
            else if (arg == "--savestate-trace") { if (!(v = next())) return false; out.savestate_trace = v; }
            else if (arg == "--list-callback-blocks") out.list_callback_blocks = true;   // step EG0
            else if (arg == "--queue-trace") out.queue_trace = true;                     // step EG0
            else if (arg == "--ball-off") {                                              // step EG0
                if (!(v = next())) return false; out.ball_off_from = std::atoll(v);
                if (!(v = next())) return false; out.ball_off_to = std::atoll(v);
                if (out.ball_off_from < 0 || out.ball_off_to <= out.ball_off_from) return false;
            }
            else if (arg == "--ball-off-wake-only") out.ball_off_wake_only = true;      // step EG0
            else if (arg == "--ball-off-drop-all") out.ball_off_drop_all = true;        // step EG0
            else if (arg == "--queue-roundtrip") out.queue_roundtrip = true;            // step B7EG
            else if (arg == "--probe-census" || arg == "--release-check")               // steps WG0, WG1 (one branch: C1061)
                (arg == "--probe-census" ? out.probe_census : out.probe_release_check) = true;
            else if (!parse_trafo_arg(arg, next, out) && !parse_record_format_arg(arg, next, out)
                     && !parse_union_arg(arg, next, out)) return false;
        }
        // --list of a journal and the savestate file verbs are pure file
        // reading: no game data needed.
        return !out.root.empty() || !out.replay_session.empty() || !out.savestate_diff[0].empty()
            || !out.savestate_describe.empty() || !out.describe_script.empty();
    }

    bool gameplay_ingame_active(const bmmo::sim::headless_engine& engine) {
        auto* script = CKBehavior::Cast(engine.context()->GetObjectByNameAndClass(
            const_cast<CKSTRING>("Gameplay_Ingame"), CKCID_BEHAVIOR, nullptr));
        return script && script->IsActive();
    }

    void dump_active(CKBehavior* behavior, int depth, std::string& out) {
        if (!behavior) return;
        const int count = behavior->GetSubBehaviorCount();
        for (int i = 0; i < count; ++i) {
            CKBehavior* sub = behavior->GetSubBehavior(i);
            if (!sub || !sub->IsActive()) continue;
            out += std::string(depth * 2, ' ') + (sub->GetName() ? sub->GetName() : "?");
            if (sub->IsUsingFunction()) {
                out += '{';
                out += sub->GetPrototypeName() ? sub->GetPrototypeName() : "?";
                out += '}';
            }
            out += '\n';
            dump_active(sub, depth + 1, out);
        }
    }

    std::string io_name(CKBehaviorIO* io) {
        return io && io->GetName() ? io->GetName() : "?";
    }

    std::string parameter_text(CKParameter* parameter) {
        if (!parameter) return "<none>";
        const int needed = parameter->GetStringValue(nullptr, TRUE);
        if (needed <= 0) return "";
        std::string value(static_cast<size_t>(needed) + 1, '\0');
        parameter->GetStringValue(value.data(), TRUE);
        value.resize(std::strlen(value.c_str()));
        return value;
    }

    // Whole graph of a behavior: every block with its prototype, input
    // parameter values and the links between sub-blocks.
    void dump_graph(CKBehavior* behavior, int depth, std::string& out) {
        const std::string indent(static_cast<size_t>(depth) * 2, ' ');
        const char* name = behavior->GetName() ? behavior->GetName() : "?";
        out += indent + name;
        if (CKBehaviorPrototype* proto = behavior->GetPrototype())
            if (proto->GetName() && std::string_view(proto->GetName()) != name)
                out += std::string(" <") + proto->GetName() + ">";
        if (behavior->IsActive()) out += " *";
        const int parameters = behavior->GetInputParameterCount();
        for (int i = 0; i < parameters; ++i) {
            CKParameterIn* in = behavior->GetInputParameter(i);
            if (!in) continue;
            CKParameter* source = in->GetRealSource();
            out += std::string(i ? ", " : " (") + (in->GetName() ? in->GetName() : "?") + "="
                 + parameter_text(source);
            if (CKObject* owner = source ? source->GetOwner() : nullptr)
                if (owner != behavior && owner->GetName()) out += std::string("@") + owner->GetName();
        }
        if (parameters) out += ")";
        const int outputs = behavior->GetOutputParameterCount();
        for (int i = 0; i < outputs; ++i) {
            CKParameterOut* parameter = behavior->GetOutputParameter(i);
            if (!parameter) continue;
            out += std::string(i ? ", " : " -> {") + (parameter->GetName() ? parameter->GetName() : "?") + "="
                 + parameter_text(parameter);
        }
        if (outputs) out += "}";
        const int locals = behavior->GetLocalParameterCount();
        for (int i = 0; i < locals; ++i) {
            CKParameterLocal* local = behavior->GetLocalParameter(i);
            if (!local) continue;
            out += std::string(i ? ", " : " [") + (local->GetName() ? local->GetName() : "?") + "="
                 + parameter_text(local);
        }
        if (locals) out += "]";
        out += '\n';
        const int subs = behavior->GetSubBehaviorCount();
        for (int i = 0; i < subs; ++i)
            if (CKBehavior* sub = behavior->GetSubBehavior(i)) dump_graph(sub, depth + 1, out);
        const int links = behavior->GetSubBehaviorLinkCount();
        for (int i = 0; i < links; ++i) {
            CKBehaviorLink* link = behavior->GetSubBehaviorLink(i);
            if (!link || !link->GetInBehaviorIO() || !link->GetOutBehaviorIO()) continue;
            CKBehavior* from = link->GetInBehaviorIO()->GetOwner();
            CKBehavior* to = link->GetOutBehaviorIO()->GetOwner();
            out += indent + "  link: " + (from && from->GetName() ? from->GetName() : "?") + "."
                 + io_name(link->GetInBehaviorIO()) + " -> " + (to && to->GetName() ? to->GetName() : "?")
                 + "." + io_name(link->GetOutBehaviorIO());
            if (link->GetActivationDelay() != 1) out += " delay=" + std::to_string(link->GetActivationDelay());
            out += '\n';
        }
    }

    // Every root script (name@owner, '*' = active) whose name or owner
    // contains `filter` ("*" lists all).
    void list_scripts(const bmmo::sim::headless_engine& engine, const std::string& filter) {
        auto* context = engine.context();
        const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
        CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
        std::string out;
        int listed = 0;
        for (int i = 0; i < count; ++i) {
            auto* behavior = CKBehavior::Cast(context->GetObject(ids[i]));
            if (!behavior || behavior->GetParent()) continue;
            CKBeObject* owner = behavior->GetOwner();
            const std::string name = behavior->GetName() ? behavior->GetName() : "?";
            const std::string owner_name = owner && owner->GetName() ? owner->GetName() : "?";
            if (filter != "*" && name.find(filter) == std::string::npos
                    && owner_name.find(filter) == std::string::npos)
                continue;
            if (listed++) out += "; ";
            out += name + "@" + owner_name + (behavior->IsActive() ? "*" : "");
        }
        std::printf("[scripts tick %llu] %d root scripts matching %s: %s\n",
            static_cast<unsigned long long>(engine.ticks()), listed, filter.c_str(), out.c_str());
        std::fflush(stdout);
    }

    // --dump-entity NAME: world matrix rows, parent and local position of a
    // 3D entity (hex floats), the counterpart of the client's "entity" verb.
    // Returned as text so a caller can hold the line of the tick before a
    // divergence and only print it when there is one.
    std::string describe_entity(const bmmo::sim::headless_engine& engine, const std::string& name) {
        char line[768] = {};
        auto* entity = CK3dEntity::Cast(engine.context()->GetObjectByNameAndParentClass(
            const_cast<CKSTRING>(name.c_str()), CKCID_3DENTITY, nullptr));
        if (!entity) {
            std::snprintf(line, sizeof(line), "[dump tick %llu] no 3D entity named %s\n",
                static_cast<unsigned long long>(engine.ticks()), name.c_str());
            return line;
        }
        const VxMatrix& world = entity->GetWorldMatrix();
        const VxMatrix& local = entity->GetLocalMatrix();
        CK3dEntity* parent = entity->GetParent();
        std::snprintf(line, sizeof(line),
            "[dump tick %llu] entity %s parent=%s world=[%a,%a,%a][%a,%a,%a][%a,%a,%a][%a,%a,%a] local_pos=[%a,%a,%a]\n",
            static_cast<unsigned long long>(engine.ticks()), name.c_str(),
            parent && parent->GetName() ? parent->GetName() : "-",
            static_cast<double>(world[0][0]), static_cast<double>(world[0][1]), static_cast<double>(world[0][2]),
            static_cast<double>(world[1][0]), static_cast<double>(world[1][1]), static_cast<double>(world[1][2]),
            static_cast<double>(world[2][0]), static_cast<double>(world[2][1]), static_cast<double>(world[2][2]),
            static_cast<double>(world[3][0]), static_cast<double>(world[3][1]), static_cast<double>(world[3][2]),
            static_cast<double>(local[3][0]), static_cast<double>(local[3][1]), static_cast<double>(local[3][2]));
        return line;
    }

    void dump_entity(const bmmo::sim::headless_engine& engine, const std::string& name) {
        std::printf("%s", describe_entity(engine, name).c_str());
        std::fflush(stdout);
    }

    // --dump-entity takes a comma separated list.
    std::string describe_entities(const bmmo::sim::headless_engine& engine, const std::string& names) {
        std::string out;
        size_t start = 0;
        while (start <= names.size()) {
            size_t comma = names.find(',', start);
            if (comma == std::string::npos) comma = names.size();
            if (comma > start) out += describe_entity(engine, names.substr(start, comma - start));
            start = comma + 1;
        }
        return out;
    }

    void dump_array(const bmmo::sim::headless_engine& engine, const std::string& name) {
        CKDataArray* array = engine.data_array(name.c_str());
        if (!array) {
            std::printf("[array] no data array named %s\n", name.c_str());
            return;
        }
        const int rows = array->GetRowCount(), columns = array->GetColumnCount();
        std::string out;
        for (int c = 0; c < columns; ++c) {
            if (c) out += " | ";
            out += array->GetColumnName(c) ? array->GetColumnName(c) : "?";
            out += "(" + std::to_string(static_cast<int>(array->GetColumnType(c))) + ")";
        }
        out += "\n";
        char cell[1024];
        for (int r = 0; r < rows; ++r) {
            out += "  " + std::to_string(r) + ": ";
            for (int c = 0; c < columns; ++c) {
                if (c) out += " | ";
                cell[0] = 0;
                array->GetElementStringValue(r, c, cell, static_cast<int>(sizeof(cell)));
                cell[sizeof(cell) - 1] = 0;
                out += cell;
            }
            out += "\n";
        }
        std::printf("[array tick %llu] %s rows=%d columns=%d\n%s", static_cast<unsigned long long>(engine.ticks()),
            name.c_str(), rows, columns, out.c_str());
        std::fflush(stdout);
    }

    void dump_script(const bmmo::sim::headless_engine& engine, const std::string& name) {
        auto* context = engine.context();
        const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
        CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
        int found = 0;
        for (int i = 0; i < count; ++i) {
            auto* behavior = CKBehavior::Cast(context->GetObject(ids[i]));
            if (!behavior || !behavior->GetName() || name != behavior->GetName() || behavior->GetParent()) continue;
            std::string out;
            dump_graph(behavior, 0, out);
            CKBeObject* owner = behavior->GetOwner();
            std::printf("[dump tick %llu] script %s owner=%s\n%s", static_cast<unsigned long long>(engine.ticks()),
                name.c_str(), owner && owner->GetName() ? owner->GetName() : "?", out.c_str());
            ++found;
        }
        if (!found) std::printf("[dump] no root behavior named %s\n", name.c_str());
        std::fflush(stdout);
    }

    void trace_script(const bmmo::sim::headless_engine& engine, const std::string& name) {
        auto* context = engine.context();
        const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
        CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
        for (int i = 0; i < count; ++i) {
            auto* behavior = CKBehavior::Cast(context->GetObject(ids[i]));
            if (!behavior || !behavior->GetName() || name != behavior->GetName() || behavior->GetParent()) continue;
            std::string out;
            dump_active(behavior, 1, out);
            std::printf("[trace tick %llu] %s active=%d\n%s", static_cast<unsigned long long>(engine.ticks()),
                name.c_str(), behavior->IsActive() ? 1 : 0, out.c_str());
        }
        std::fflush(stdout);
    }

    std::string root_scripts(const bmmo::sim::headless_engine& engine) {
        auto* context = engine.context();
        const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
        CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
        std::string out;
        int listed = 0;
        for (int i = 0; i < count; ++i) {
            auto* behavior = CKBehavior::Cast(context->GetObject(ids[i]));
            if (!behavior || behavior->GetType() != CKBEHAVIORTYPE_SCRIPT || behavior->GetParent()) continue;
            if (!behavior->IsActive()) continue;
            if (listed++) out += ", ";
            out += behavior->GetName() ? behavior->GetName() : "?";
        }
        return std::to_string(count) + " behaviors, active roots: " + out;
    }

    void report(const bmmo::sim::headless_engine& engine) {
        bmmo::physics::world_hash hash;
        std::string error;
        if (!bmmo::physics::capture_world_hash(engine.physics(), hash, error)) {
            std::printf("tick=%llu hash unavailable: %s\n",
                static_cast<unsigned long long>(engine.ticks()), error.c_str());
        } else {
            if (hash.cores > 0)
                std::printf("    movable: %s\n", bmmo::physics::describe_movable_objects(engine.physics()).c_str());
            std::printf("tick=%llu hash=%016llx pose=%016llx surfaces=%016llx cores=%d ivp_time=%.6f seed=%d delta=%.4f pdelta=%.6f factor=%.6f ingame=%d\n",
                static_cast<unsigned long long>(engine.ticks()),
                static_cast<unsigned long long>(hash.hash), static_cast<unsigned long long>(hash.pose),
                static_cast<unsigned long long>(hash.surfaces), hash.cores, hash.ivp_time,
                hash.ivp_seed, hash.delta_time_ms, hash.physics_delta_time, hash.time_factor,
                gameplay_ingame_active(engine) ? 1 : 0);
            std::printf("    %s\n", root_scripts(engine).c_str());
        }
        std::fflush(stdout);
    }

    // Smoke test for the bridge v2 body list: what the server sees of the
    // world right now, in physics-table order.
    void list_bodies(const bmmo::sim::headless_engine& engine) {
        std::vector<bmmo::physics::body_state> bodies(
            static_cast<size_t>(bmmo::physics::list_bodies(engine.physics(), nullptr, 0)));
        const int total = bmmo::physics::list_bodies(engine.physics(), bodies.data(),
                                                     static_cast<int>(bodies.size()));
        std::printf("bodies at tick %llu: %d\n",
                    static_cast<unsigned long long>(engine.ticks()), total);
        for (const auto& body: bodies)
            std::printf("  %-28s movable=%d simulated=%d coll=%d state=%d pos=(%.4f,%.4f,%.4f)\n",
                        body.name, body.movable ? 1 : 0, body.simulated ? 1 : 0,
                        body.collision_enabled ? 1 : 0, static_cast<int>(body.movement_state),
                        body.position[0], body.position[1], body.position[2]);
        std::fflush(stdout);
    }

    // The per-body terms of the handshake's surface signature: what a client
    // and this engine have to agree on before a session may start.
    void dump_surfaces(const bmmo::sim::headless_engine& engine) {
        bmmo::physics::world_hash hash;
        std::string error;
        bmmo::physics::capture_world_hash(engine.physics(), hash, error);
        std::printf("surfaces at tick %llu: %016llx\n%s",
                    static_cast<unsigned long long>(engine.ticks()),
                    static_cast<unsigned long long>(hash.surfaces),
                    bmmo::physics::describe_surfaces_exact(engine.physics()).c_str());
        std::fflush(stdout);
    }

    // --track ENTITY: one line per tick with that body's state, printed wide
    // enough (nine decimals, sub-micrometre) that two runs can be subtracted
    // body by body instead of only compared by hash.
    void track_body(const bmmo::sim::headless_engine& engine, const std::string& entity) {
        bmmo::physics::body_state body;
        std::string error;
        if (!bmmo::physics::get_body_state(engine.physics(), entity.c_str(), body, error)) {
            std::printf("track tick=%llu %s unavailable: %s\n",
                        static_cast<unsigned long long>(engine.ticks()), entity.c_str(), error.c_str());
            std::fflush(stdout);
            return;
        }
        std::printf("track tick=%llu %s sim=%d pos=(%.9f,%.9f,%.9f) rot=(%.9f,%.9f,%.9f,%.9f)"
                    " lin=(%.9f,%.9f,%.9f) ang=(%.9f,%.9f,%.9f)\n",
                    static_cast<unsigned long long>(engine.ticks()), entity.c_str(), body.simulated ? 1 : 0,
                    body.position[0], body.position[1], body.position[2],
                    body.rotation[0], body.rotation[1], body.rotation[2], body.rotation[3],
                    body.linear[0], body.linear[1], body.linear[2],
                    body.angular[0], body.angular[1], body.angular[2]);
        // Like every other diagnostic here: the tail of a fully buffered
        // stdout does not survive the engine's exit.
        std::fflush(stdout);
    }

    // --restore-at: what a rollback restore does to the bodies it corrects,
    // with the correction itself removed - every movable body (or only
    // `entity`) is written back to the state it is already in, through the
    // v10 path with wake_mode keep and recheck false.  Returns the number of
    // bodies written, -1 on an error.
    int restore_bodies(bmmo::sim::headless_engine& engine, const std::string& entity, std::string& error) {
        std::vector<bmmo::physics::body_state> bodies(
            static_cast<size_t>(bmmo::physics::list_bodies(engine.physics(), nullptr, 0)));
        const int count = bmmo::physics::list_bodies(engine.physics(), bodies.data(),
                                                     static_cast<int>(bodies.size()));
        bodies.resize(static_cast<size_t>(std::max(0, std::min(count, static_cast<int>(bodies.size())))));
        int written = 0;
        for (const auto& body: bodies) {
            if (!body.movable) continue;
            if (!entity.empty() && entity != body.name) continue;
            if (!bmmo::physics::set_body_state(engine.physics(), body.name, body.position, body.rotation,
                                               body.linear, body.angular, bmmo::physics::wake_mode::keep,
                                               false, error))
                return -1;
            ++written;
        }
        return written;
    }

    // Part B / spec A.9: root script name of a trafo explosion by --explode's
    // TYPE argument, null if TYPE is not one of wood|paper|stone.
    const char* explosion_script_name(const std::string& type) {
        if (type == "wood") return "Ball_Explosion_Wood";
        if (type == "paper") return "Ball_Explosion_Paper";
        if (type == "stone") return "Ball_Explosion_Stone";
        return nullptr;
    }

    // --list-trafos: every P_Trafo_* entity of the level with its world
    // position (the trafos' positions for the trafo set checks).
    void list_trafos(const bmmo::sim::headless_engine& engine, const std::string& prefix = "P_Trafo_") {
        auto* context = engine.context();
        const XObjectPointerArray& all = context->GetObjectListByType(CKCID_3DENTITY, TRUE);
        int n = 0;
        for (int i = 0; i < all.Size(); ++i) {
            auto* e = CK3dEntity::Cast(all[i]);
            if (!e || !e->GetName() || std::strncmp(e->GetName(), prefix.c_str(), prefix.size()) != 0) continue;
            VxVector p;
            e->GetPosition(&p);
            std::printf("trafo %s %.4f %.4f %.4f\n", e->GetName(), p.x, p.y, p.z);
            ++n;
        }
        std::printf("trafos: %d\n", n);
        std::fflush(stdout);
    }

    // ------------------------------------------------------------------
    // Step EG0 (rollback-engine-gaps-design.md §8 G0): the PreSimulate
    // queue's producers and contents.  Read-only diagnostics.
    // ------------------------------------------------------------------
    namespace eg0 {
        // The ten prototypes whose blocks queue a callback when their target
        // has no physics object (physics_RT Behaviors/*.cpp SetGuid lines).
        struct queue_prototype { CKDWORD a, b; const char* name; };
        constexpr queue_prototype kQueuePrototypes[] = {
            {0x56e20c57, 0x0b926068, "SetPhysicsForce"},
            {0x38b851b5, 0x72ca74ac, "Physics WakeUp"},
            {0x7435488d, 0x201d1188, "Physics Collision Detection"},
            {0x5e624f0a, 0x35160450, "Set Physics Ball Joint"},
            {0x41cd3653, 0x0de60c1d, "Set Physics Hinge"},
            {0x2973360e, 0x23d31aa7, "Set Physics Slider"},
            {0x24a06a3a, 0x07100fce, "Set Physics Spring"},
            {0x2c015f2b, 0x5b147512, "Physics Buoyancy"},
            {0x199e4cf1, 0x545a78fe, "Physics Continuous Contact"},
            {0x53bf75aa, 0x770c7021, "Delete Collision Surfaces"},
        };
        const char* queue_prototype_of(CKBehavior* beh) {
            if (!beh) return nullptr;
            const CKGUID guid = beh->GetPrototypeGuid();
            for (const auto& p: kQueuePrototypes)
                if (guid == CKGUID(p.a, p.b)) return p.name;
            return nullptr;
        }
        CKBehavior* root_of(CKBehavior* beh) {
            while (beh && beh->GetParent()) beh = beh->GetParent();
            return beh;
        }
        std::string object_text(CKObject* object) {
            if (!object) return "null";
            return std::string(object->GetName() ? object->GetName() : "?") + "#" + std::to_string(object->GetID());
        }
        // A block's target: its target parameter's real source (the value and
        // the object that owns the source: a Get Cell's output, a script
        // local, ...), or the owner when the block has no target parameter.
        std::string target_text(CKBehavior* beh) {
            CKParameterIn* target = beh->GetTargetParameter();
            if (!target) return "owner " + object_text(beh->GetOwner());
            CKParameter* source = target->GetRealSource();
            if (!source) return "target unset";
            std::string out = "value " + object_text(source->GetValueObject());
            if (CKObject* owner = source->GetOwner()) {
                out += " source " + object_text(owner);
                if (auto* b = CKBehavior::Cast(owner); b && b->GetPrototype() && b->GetPrototype()->GetName())
                    out += std::string(" <") + b->GetPrototype()->GetName() + ">";
            }
            return out;
        }
        void list_callback_blocks(const bmmo::sim::headless_engine& engine) {
            CKContext* context = engine.context();
            const bmmo::game::script_replay::selection sel = bmmo::game::session_script_selection(context);
            const std::set<CK_ID> selected(sel.scripts.begin(), sel.scripts.end());
            const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
            CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
            int listed = 0, outside = 0;
            for (int i = 0; i < count; ++i) {
                auto* beh = CKBehavior::Cast(context->GetObject(ids[i]));
                const char* proto = queue_prototype_of(beh);
                if (!proto) continue;
                CKBehavior* root = root_of(beh);
                const bool in = root && selected.count(root->GetID()) != 0;
                if (!in) ++outside;
                ++listed;
                std::printf("callback block %u %s script=%s owner=%s %s active=%d target: %s\n",
                            static_cast<unsigned>(beh->GetID()), proto, object_text(root).c_str(),
                            object_text(root ? root->GetOwner() : nullptr).c_str(), in ? "selected" : "OUTSIDE",
                            root && root->IsActive() ? 1 : 0, target_text(beh).c_str());
            }
            std::printf("[callback blocks tick %llu] %d blocks of the ten prototypes, %d outside the selection (%zu scripts selected)\n",
                        static_cast<unsigned long long>(engine.ticks()), listed, outside, sel.scripts.size());
            std::fflush(stdout);
        }
        // --queue-trace: one line whenever the queue or the current ball's
        // body changes - every entry (slot:block <prototype>@script, or the
        // C++ class of the bridge's own) and the ball CurrentLevel[0][1]
        // names, with its body's simulated flag.
        // --ball-off: the current ball's body out, then back with the retail
        // "physicalize new Ball" recipe at its entity's matrix, frozen = false
        // as the block makes it.  The recipe is a copy of
        // physics_world::retail_recipe (physics_world.cpp), read from the
        // Physicalize_GameBall row of the ball's name: a --replay run has no
        // physics_world to ask; change the two together.
        struct ball_off {
            std::string entity;
            bool apply(const bmmo::sim::headless_engine& engine, long long frame, long long from, long long to,
                       bool wake_only, bool drop_all) {
                if (frame != from && frame != to) return true;
                std::string error;
                if (frame == to && (wake_only || drop_all))
                    if (PhysicsCallbackContainer* q = engine.physics() ? engine.physics()->m_PreSimulateCallbacks : nullptr) {
                        // Through the container's own ClearBehavior, which also
                        // keeps m_HasCallbacks right when the queue empties.
                        std::vector<CK_ID> drop;
                        for (int slot = 0; slot < 3; ++slot)
                            for (int i = 0; i < q->m_Callbacks[slot].len(); ++i) {
                                PhysicsCallback* cb = q->m_Callbacks[slot].element_at(i);
                                auto* beh = cb ? CKBehavior::Cast(engine.context()->GetObject(cb->m_BehaviorID)) : nullptr;
                                const char* proto = queue_prototype_of(beh);
                                if (!proto || (!drop_all && std::strcmp(proto, "Physics WakeUp") == 0)) continue;
                                std::printf("ball-off: entry %u <%s> dropped before frame %lld\n",
                                            static_cast<unsigned>(cb->m_BehaviorID), proto, frame);
                                if (std::find(drop.begin(), drop.end(), cb->m_BehaviorID) == drop.end())
                                    drop.push_back(cb->m_BehaviorID);
                            }
                        for (CK_ID id: drop) q->ClearBehavior(id);
                    }
                if (frame == from) {
                    CKDataArray* level = engine.data_array("CurrentLevel");
                    auto* ball = level ? CK3dEntity::Cast(level->GetElementObject(0, 1)) : nullptr;
                    if (!ball || !ball->GetName()) { std::fprintf(stderr, "ball-off: no current ball\n"); return false; }
                    entity = ball->GetName();
                    const bool ok = bmmo::physics::unphysicalize(engine.physics(), entity.c_str(), error);
                    std::printf("ball-off: %s body taken out before frame %lld%s%s\n", entity.c_str(), frame,
                                ok ? "" : ": ", ok ? "" : error.c_str());
                    std::fflush(stdout);
                    return ok;
                }
                CKDataArray* table = engine.data_array("Physicalize_GameBall");
                bmmo_physics_ball_recipe recipe{};
                bool found = false;
                for (int row = 0; table && row < table->GetRowCount(); ++row) {
                    char name[128] = {};
                    table->GetElementStringValue(row, 0, name, static_cast<int>(sizeof(name)));
                    name[sizeof(name) - 1] = 0;
                    if (entity != name) continue;
                    recipe.fixed = false;
                    recipe.start_frozen = false;
                    recipe.enable_collision = true;
                    recipe.calc_mass_center = false;
                    table->GetElementValue(row, 1, &recipe.friction);
                    table->GetElementValue(row, 2, &recipe.elasticity);
                    table->GetElementValue(row, 3, &recipe.mass);
                    table->GetElementValue(row, 5, &recipe.linear_damp);
                    table->GetElementValue(row, 6, &recipe.rot_damp);
                    const std::string mesh = entity + "_Mesh";
                    std::snprintf(recipe.collision_surface, sizeof(recipe.collision_surface), "%s", mesh.c_str());
                    if (entity == "Ball_Paper") {
                        recipe.convex_count = 1;
                        std::snprintf(recipe.convex[0], sizeof(recipe.convex[0]), "%s", mesh.c_str());
                    } else {
                        recipe.ball_count = 1;
                        recipe.ball_radius[0] = 2.0f;
                    }
                    found = true;
                }
                if (!found) { std::fprintf(stderr, "ball-off: no Physicalize_GameBall row for %s\n", entity.c_str()); return false; }
                const bool ok = bmmo::physics::physicalize(engine.physics(), entity.c_str(), recipe, "", error);
                std::printf("ball-off: %s body made again before frame %lld%s%s\n", entity.c_str(), frame,
                            ok ? "" : ": ", ok ? "" : error.c_str());
                std::fflush(stdout);
                return ok;
            }
        };

        struct queue_tracer {
            std::string last;
            // Step RLQ: each retail entry's target too (target_text) and
            // whether that target has a physics object now (--savestate-check
            // --queue-trace; the --replay lines are unchanged without it).
            bool targets = false;
            void after_frame(const bmmo::sim::headless_engine& engine, long long frame) {
                CKContext* context = engine.context();
                std::string queue;
                if (PhysicsCallbackContainer* q = engine.physics() ? engine.physics()->m_PreSimulateCallbacks : nullptr) {
                    for (int slot = 0; slot < 3; ++slot)
                        for (int i = 0; i < q->m_Callbacks[slot].len(); ++i) {
                            PhysicsCallback* cb = q->m_Callbacks[slot].element_at(i);
                            if (!cb) continue;
                            auto* beh = CKBehavior::Cast(context->GetObject(cb->m_BehaviorID));
                            const char* proto = queue_prototype_of(beh);
                            queue += " " + std::to_string(slot) + ":";
                            if (proto) queue += std::to_string(cb->m_BehaviorID) + "<" + proto + ">@" + object_text(root_of(beh));
                            else queue += std::string("bmmo/") + typeid(*cb).name();
                            if (targets && proto) {
                                CKParameterIn* in = beh->GetTargetParameter();
                                CKParameter* source = in ? in->GetRealSource() : nullptr;
                                auto* entity = CK3dEntity::Cast(source ? source->GetValueObject() : beh->GetOwner());
                                const bool body = entity && engine.physics() && engine.physics()->GetPhysicsObject(entity);
                                queue += "{" + target_text(beh) + (body ? " has-body" : " NO-BODY") + "}";
                            }
                        }
                }
                std::string ball = "none";
                if (CKDataArray* level = engine.data_array("CurrentLevel"))
                    if (auto* entity = CK3dEntity::Cast(level->GetElementObject(0, 1))) {
                        bmmo::physics::body_state body;
                        std::string error;
                        ball = std::string(entity->GetName() ? entity->GetName() : "?")
                             + (bmmo::physics::get_body_state(engine.physics(), entity->GetName(), body, error)
                                    ? (body.simulated ? " body sim=1" : " body sim=0") : " no body");
                    }
                const std::string now = "ball " + ball + " | queue" + (queue.empty() ? " empty" : queue);
                if (now == last) return;
                last = now;
                std::printf("queue frame=%lld %s\n", frame, now.c_str());
                std::fflush(stdout);
            }
        };
    }

    namespace b7eg {
        // --queue-roundtrip (see its argument).
        struct queue_roundtrip {
            uint64_t frames = 0, entries = 0, failures = 0;
            static std::string text(const std::vector<bmmo_physics_callback>& q) {
                std::string out;
                for (const auto& e: q)
                    out += " " + std::to_string(e.slot) + ":" + std::to_string(e.behavior) + (e.bmmo ? "b" : "")
                         + "@" + std::to_string(e.owner_script);
                return out.empty() ? std::string(" (none)") : out;
            }
            static bool same(const std::vector<bmmo_physics_callback>& a, const std::vector<bmmo_physics_callback>& b) {
                if (a.size() != b.size()) return false;
                for (size_t i = 0; i < a.size(); ++i)
                    if (a[i].slot != b[i].slot || a[i].behavior != b[i].behavior || a[i].bmmo != b[i].bmmo
                            || a[i].owner_script != b[i].owner_script)
                        return false;
                return true;
            }
            void fail(long long frame, const std::string& what) {
                ++failures;
                std::printf("queue-roundtrip FAIL frame=%lld: %s\n", frame, what.c_str());
            }
            void after_frame(const bmmo::sim::headless_engine& engine, long long frame) {
                CKIpionManager* physics = engine.physics();
                std::vector<bmmo_physics_callback> q0, q;
                std::string error;
                if (!bmmo::physics::callbacks_get(physics, q0, error)) return fail(frame, "get: " + error);
                std::vector<bmmo_physics_callback> retail, kept;
                for (const auto& e: q0) (e.bmmo ? kept : retail).push_back(e);
                if (retail.empty()) return;
                ++frames;
                entries += retail.size();
                // 1. hold every retail entry (nothing executed), put it back
                const int held = bmmo::physics::callbacks_hold(physics, {}, error);
                if (held != static_cast<int>(retail.size()))
                    return fail(frame, "hold took " + std::to_string(held) + " of " + std::to_string(retail.size()) + " " + error);
                if (!bmmo::physics::callbacks_get(physics, q, error) || !same(q, kept))
                    fail(frame, "while held:" + text(q));
                if (bmmo::physics::callbacks_hold(physics, {}, error) != -1) fail(frame, "a second hold was not refused");
                const int clamped = bmmo::physics::callbacks_unhold(physics, error);
                if (clamped != 0) fail(frame, "unhold clamped " + std::to_string(clamped));
                if (!bmmo::physics::callbacks_get(physics, q, error) || !same(q, q0))
                    return fail(frame, "after the put-back:" + text(q) + " expected" + text(q0));
                // 2. rebuild the entries' scripts from the queue as it was
                std::vector<uint32_t> scope;
                for (const auto& e: retail) scope.push_back(e.owner_script);
                const int removed = bmmo::physics::callbacks_rebuild(physics, scope, retail, error);
                if (removed != static_cast<int>(retail.size()))
                    return fail(frame, "rebuild removed " + std::to_string(removed) + " of " + std::to_string(retail.size()) + " " + error);
                if (!bmmo::physics::callbacks_get(physics, q, error) || !same(q, q0))
                    return fail(frame, "after the rebuild:" + text(q) + " expected" + text(q0));
                // 3. a refused rebuild changes nothing (a block outside its scope)
                if (bmmo::physics::callbacks_rebuild(physics, {}, retail, error) != -1) fail(frame, "an out-of-scope rebuild was not refused");
                if (!bmmo::physics::callbacks_get(physics, q, error) || !same(q, q0))
                    return fail(frame, "after a refused rebuild:" + text(q));
                std::printf("queue-roundtrip frame=%lld entries=%zu ok:%s\n", frame, retail.size(), text(q0).c_str());
            }
        };
    }

    int run_free(bmmo::sim::headless_engine& engine, const arguments& args) {
        std::string error;
        // The trafo sets (sim/trafo_tool.hpp): inputs and the trafo frame
        // before every engine frame from the anchor on, the hold and the
        // diagnostics after it.  With the hold armed, --activate lands after
        // the frame (as a script of that frame would have activated it).
        std::unique_ptr<bmmo::sim::trafo_tool> trafo;
        if (args.trafo.any()) trafo = std::make_unique<bmmo::sim::trafo_tool>(args.trafo, engine.physics(), engine.context());
        const bool post_frame_activations = trafo && args.trafo.hold;
        // --explode: activated once at args.explode_at_tick, then the pose
        // hash and movable core count print for 200 ticks (determinism check,
        // Part B: the trafo pieces draw Random through the hooked block).
        int explode_ticks_left = 0;
        for (int i = 0; i < args.ticks; ++i) {
            if (args.level > 0 && i == args.level_at_tick) {
                if (args.direct_load) {
                    if (!engine.load_level(args.level, error)) {
                        std::fprintf(stderr, "load_level failed: %s\n", error.c_str());
                        return 1;
                    }
                } else {
                    engine.request_level(args.level);
                }
                std::fprintf(stderr, "requested level %d at tick %d (%s)\n", args.level, i,
                    args.direct_load ? "direct Load Level message" : "through the menus");
            }
            if (!engine.level_request().error.empty()) {
                std::fprintf(stderr, "level request failed: %s\n", engine.level_request().error.c_str());
                return 1;
            }
            if (args.beam_frame >= 0 && static_cast<long long>(i) == args.beam_frame) {
                CKDataArray* level = engine.data_array("CurrentLevel");
                CK3dEntity* ball = level ? CK3dEntity::Cast(level->GetElementObject(0, 1)) : nullptr;
                const double upright[4] = {0.0, 0.0, 0.0, 1.0};
                const float still[3] = {0.0f, 0.0f, 0.0f};
                if (!ball || !bmmo::physics::set_body_state(engine.physics(), ball->GetName(), args.beam_at, upright,
                                                            still, still, true, error)) {
                    std::fprintf(stderr, "beam failed at tick %d: %s\n", i, error.c_str());
                    return 2;
                }
                std::fprintf(stderr, "beam: %s to (%.3f,%.3f,%.3f) at tick %d\n", ball->GetName(),
                             args.beam_at[0], args.beam_at[1], args.beam_at[2], i);
            }
            if (args.body_guard_tick >= 0 && static_cast<long long>(i) == args.body_guard_tick) {
                if (!bmmo::physics::set_body_guard(engine.physics(), true, args.body_guard_entity.c_str(), error)) {
                    std::fprintf(stderr, "body guard failed: %s\n", error.c_str());
                    return 1;
                }
                std::fprintf(stderr, "body guard armed at tick %d (except %s, %d entities exempt)\n", i,
                             args.body_guard_entity.c_str(), engine.physics()->m_KeepLevelBodiesFree.Size());
            }
            std::vector<CKBehavior*> post_activations;
            for (const auto& [name, tick]: args.activations) {
                if (static_cast<long long>(i) != tick) continue;
                CKBehavior* script = bmmo::game::find_root_script(engine.context(), name.c_str());
                if (!script) {
                    std::fprintf(stderr, "activate: no root script named %s\n", name.c_str());
                    return 1;
                }
                if (post_frame_activations) {
                    post_activations.push_back(script);
                    std::fprintf(stderr, "activate: %s after the frame of tick %d%s\n", name.c_str(), i,
                                 args.trafo.hold_slip ? " (slipping past the hold)" : "");
                    continue;
                }
                script->Activate(TRUE, TRUE);
                std::fprintf(stderr, "activate: %s at tick %d\n", name.c_str(), i);
            }
            for (const auto& [type, tick]: args.explosions) {
                if (static_cast<long long>(i) != tick) continue;
                const char* script_name = explosion_script_name(type);
                if (!script_name) {
                    std::fprintf(stderr, "--explode type must be wood, paper or stone\n");
                    return 2;
                }
                CKBehavior* explosion = bmmo::game::find_root_script(engine.context(), script_name);
                if (!explosion) {
                    std::fprintf(stderr, "explode: no root script named %s\n", script_name);
                    return 1;
                }
                explosion->Activate(TRUE, TRUE);
                explode_ticks_left = 200;
                std::fprintf(stderr, "explode: activated %s at tick %d\n", script_name, i);
            }
            for (const auto& b: args.beams_again) {
                if (b.tick != i) continue;
                CKDataArray* level = engine.data_array("CurrentLevel");
                CK3dEntity* ball = level ? CK3dEntity::Cast(level->GetElementObject(0, 1)) : nullptr;
                const double upright[4] = {0.0, 0.0, 0.0, 1.0};
                const float still[3] = {0.0f, 0.0f, 0.0f};
                if (!ball || !bmmo::physics::set_body_state(engine.physics(), ball->GetName(), b.at, upright, still, still, true, error))
                    std::fprintf(stderr, "beam-again failed at tick %d: %s\n", i, error.c_str());
                else
                    std::fprintf(stderr, "beam-again: %s to (%.3f,%.3f,%.3f) at tick %d\n", ball->GetName(), b.at[0], b.at[1], b.at[2], i);
            }
            for (const auto& w: args.local_writes) {
                if (w.tick != i) continue;
                CKBehavior* script = bmmo::game::find_root_script(engine.context(), w.script.c_str());
                bool done = false;
                for (int k = 0; script && k < script->GetLocalParameterCount(); ++k)
                    if (CKParameterLocal* l = script->GetLocalParameter(k); l && l->GetName() && w.local == l->GetName()) {
                        int value = w.value;
                        l->SetValue(&value);
                        done = true;
                    }
                std::fprintf(stderr, "set-local: %s/%s = %d at tick %d%s\n", w.script.c_str(), w.local.c_str(), w.value, i,
                             done ? "" : " (NOT FOUND)");
            }
            if (trafo) trafo->before_frame(i);
            if (i < args.debug_ticks) {
                std::string last;
                const bool ok = engine.tick_debug([&](const std::string& script, const std::string& block,
                                                      const std::string& sub, int action) {
                    if (action != 1) return;  // CKDEBUG_BEHEXECUTE only
                    const std::string line = script + " :: " + block + (sub.empty() ? "" : " / " + sub);
                    if (line == last) return;
                    last = line;
                    std::printf("  [exec tick %d] %s\n", i, line.c_str());
                }, error);
                std::fflush(stdout);
                if (!ok) {
                    std::fprintf(stderr, "debug tick %d failed: %s\n", i, error.c_str());
                    return 1;
                }
            } else if (!engine.tick(error)) {
                std::fprintf(stderr, "tick %d failed: %s\n", i, error.c_str());
                return 1;
            }
            for (const auto& [tick, entity]: args.restores) {
                if (static_cast<long long>(i) != tick) continue;
                const int written = restore_bodies(engine, entity, error);
                if (written < 0) {
                    std::fprintf(stderr, "restore failed at tick %d: %s\n", i, error.c_str());
                    return 2;
                }
                const std::string what = entity.empty() ? "every movable body" : entity;
                std::fprintf(stderr, "restore: %d bodies written back at tick %d (%s)\n", written, i,
                             what.c_str());
            }
            if (trafo) trafo->after_frame(i, post_activations);
            // After the restore: the state the tick really ends with.
            for (const auto& entity: args.tracked) track_body(engine, entity);
            if (explode_ticks_left > 0) {
                bmmo::physics::world_hash hash;
                std::string hash_error;
                if (bmmo::physics::capture_world_hash(engine.physics(), hash, hash_error))
                    std::printf("explode t=%d pose=%016llx cores=%d\n", i,
                                static_cast<unsigned long long>(hash.pose), hash.cores);
                else
                    std::fprintf(stderr, "explode: hash failed at tick %d: %s\n", i, hash_error.c_str());
                std::fflush(stdout);
                --explode_ticks_left;
            }
            if (!args.trace_script.empty() && i < args.trace_ticks) trace_script(engine, args.trace_script);
            if (!args.dump_script.empty() && i == args.dump_at) dump_script(engine, args.dump_script);
            if (!args.list_scripts.empty() && i == args.dump_at) list_scripts(engine, args.list_scripts);
            if (args.list_callback_blocks && i == args.dump_at) eg0::list_callback_blocks(engine);   // step EG0
            if (!args.dump_array.empty() && i == args.dump_at) dump_array(engine, args.dump_array);
            if (args.list_trafos && i == args.dump_at) list_trafos(engine);
            if (!args.list_entities.empty() && i == args.dump_at) list_trafos(engine, args.list_entities);
            if (args.list_bodies_at >= 0 && i == args.list_bodies_at) list_bodies(engine);
            if (args.dump_surfaces_at >= 0 && i == args.dump_surfaces_at) dump_surfaces(engine);
            if (args.report_every > 0 && engine.ticks() % static_cast<uint64_t>(args.report_every) == 0)
                report(engine);
        }
        if (!args.dump_script.empty() && args.dump_at < 0) dump_script(engine, args.dump_script);
        if (!args.list_scripts.empty() && args.dump_at < 0) list_scripts(engine, args.list_scripts);
        if (args.list_callback_blocks && args.dump_at < 0) eg0::list_callback_blocks(engine);   // step EG0
        if (!args.dump_array.empty() && args.dump_at < 0) dump_array(engine, args.dump_array);
        if (args.list_trafos && args.dump_at < 0) list_trafos(engine);
        if (trafo) return trafo->finish();
        return 0;
    }

    int run_replay(bmmo::sim::headless_engine& engine, const arguments& args) {
        bmmo::physics::tick_record record;
        std::string error;
        if (!record.load(args.replay, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        std::fprintf(stderr, "record: level=%d frames=%zu physics=%s\n",
            record.header.level, record.frames.size(), record.header.physics_sha256);
        for (int i = 0; i < args.boot_ticks; ++i)
            if (!engine.tick(error)) { std::fprintf(stderr, "boot tick failed: %s\n", error.c_str()); return 1; }
        bmmo::physics::drain_event_log(engine.physics());  // install the listener, discard boot history
        const int level = args.level > 0 ? args.level : record.header.level;
        if (level <= 0) {
            std::fprintf(stderr, "the record does not name a level; pass --level N\n");
            return 1;
        }
        engine.request_level(level);
        int waited = 0;
        while (!gameplay_ingame_active(engine)) {
            if (!engine.level_request().error.empty()) {
                std::fprintf(stderr, "level request failed: %s\n", engine.level_request().error.c_str());
                return 1;
            }
            if (++waited > args.anchor_timeout) {
                std::fprintf(stderr, "Gameplay_Ingame never activated\n");
                return 1;
            }
            if (!engine.tick(error)) { std::fprintf(stderr, "tick failed: %s\n", error.c_str()); return 1; }
        }
        std::fprintf(stderr, "anchor reached after %d ticks (engine tick %llu)\n", waited,
            static_cast<unsigned long long>(engine.ticks()));
        if (!bmmo::physics::reset_session_clock(engine.physics(), 1, error)) {
            std::fprintf(stderr, "session reset failed: %s\n", error.c_str());
            return 1;
        }
        std::printf("anchor bodies: %s\nanchor events: %s\n",
            bmmo::physics::describe_physics_objects(engine.physics()).c_str(),
            bmmo::physics::drain_event_log(engine.physics()).c_str());
        // --placement on (step S2): at the record's first frame; the copies are
        // released (root first) before the engine goes, as physics_world does.
        // Step S5: on by default in this mode (a session world places from
        // protocol 12 on when its world option says so); --placement off
        // replays a record made without the copies - every retail record of
        // a pooled level made before the retail core host places (step S10).
        bmmo::game::sector_placement placement;
        struct release_on_exit {
            bmmo::game::sector_placement& placement;
            CKContext* context;
            ~release_on_exit() { placement.release(context); }
        } release_guard{placement, engine.context()};
        const bool place = args.placement_given ? args.placement : true;
        std::fprintf(stderr, "record replay: placement %s%s\n", place ? "on" : "off",
                     args.placement_given ? "" : " (the default of --replay since step S5; --placement off for a record "
                                                 "made without the copies)");
        if (place && !apply_tool_placement(engine.context(), engine.physics(), args, placement)) return 1;

        size_t matched = 0;
        long long first_divergence = -1;
        bmmo::physics::tick_record_writer output;
        if (!args.write_record.empty()) {
            bmmo::physics::tick_record_header header = record.header;
            std::snprintf(header.physics_sha256, sizeof(header.physics_sha256), "headless-%s",
                          sizeof(void*) == 8 ? "64" : "32");
            if (!output.open(args.write_record, header)) {
                std::puts("cannot open the output record");
                return 1;
            }
        }
        eg0::queue_tracer queue_trace;   // step EG0: --queue-trace
        eg0::ball_off ball_off;          // step EG0: --ball-off
        b7eg::queue_roundtrip queue_roundtrip;   // step B7EG: --queue-roundtrip
        for (size_t frame = 0; frame < record.frames.size(); ++frame) {
            const auto& expected = record.frames[frame];
            if (args.ivp_trace_from >= 0) {
                const long long f = static_cast<long long>(frame);
                if (f == args.ivp_trace_from && !bmmo::physics::set_impact_trace(engine.physics(), args.ivp_trace_path.c_str(), error))
                    std::puts(error.c_str());
                if (f == args.ivp_trace_to + 1) bmmo::physics::set_impact_trace(engine.physics(), nullptr, error);
            }
            // --explode TYPE FRAME in a replay: the client automation verb
            // "explode <type>" activated the same script from OnProcess of
            // record frame F (its response names F), which the engine runs at
            // the next Process, i.e. inside frame F + 1 of the record.
            for (const auto& [type, tick]: args.explosions) {
                if (static_cast<long long>(frame) != tick) continue;
                const char* script_name = explosion_script_name(type);
                CKBehavior* script = script_name ? bmmo::game::find_root_script(engine.context(), script_name) : nullptr;
                if (!script) {
                    std::fprintf(stderr, "explode: no root script for --explode %s\n", type.c_str());
                    return 2;
                }
                script->Activate(TRUE, TRUE);
                std::fprintf(stderr, "explode: activated %s before replay frame %zu\n", script_name, frame);
            }
            const auto dump_entities = [&](const char* when) {
                if (args.dump_entity.empty() || static_cast<long long>(frame) != args.dump_at) return;
                std::printf("--dump-entity %s frame %zu:\n", when, frame);
                size_t start = 0;
                while (start <= args.dump_entity.size()) {
                    size_t comma = args.dump_entity.find(',', start);
                    if (comma == std::string::npos) comma = args.dump_entity.size();
                    if (comma > start) dump_entity(engine, args.dump_entity.substr(start, comma - start));
                    start = comma + 1;
                }
            };
            dump_entities("before");
            for (const auto& [sector, at]: args.sectors) {
                if (at != static_cast<long long>(frame)) continue;
                CKDataArray* parameters = engine.data_array("IngameParameter");
                CKBehavior* manager = bmmo::game::find_root_script(engine.context(), "Gameplay_SectorManager");
                if (!parameters || !manager) {
                    std::fprintf(stderr, "sector: IngameParameter or Gameplay_SectorManager missing\n");
                    return 2;
                }
                int none = 0, activate = sector;
                parameters->SetElementValue(0, 2, &none, sizeof(none));
                parameters->SetElementValue(0, 1, &activate, sizeof(activate));
                if (CKScene* scene = engine.context()->GetCurrentScene()) scene->Activate(manager, TRUE);
                std::fprintf(stderr, "sector: %d activated before replay frame %zu\n", sector, frame);
            }
            if (args.beam_frame >= 0 && static_cast<long long>(frame) == args.beam_frame) {
                CKDataArray* level = engine.data_array("CurrentLevel");
                CK3dEntity* ball = level ? CK3dEntity::Cast(level->GetElementObject(0, 1)) : nullptr;
                const double upright[4] = {0.0, 0.0, 0.0, 1.0};
                const float still[3] = {0.0f, 0.0f, 0.0f};
                if (!ball || !bmmo::physics::set_body_state(engine.physics(), ball->GetName(), args.beam_at, upright,
                                                            still, still, true, error)) {
                    std::fprintf(stderr, "beam failed at frame %zu: %s\n", frame, error.c_str());
                    return 2;
                }
                std::fprintf(stderr, "beam: %s to (%.3f,%.3f,%.3f) before replay frame %zu\n", ball->GetName(),
                             args.beam_at[0], args.beam_at[1], args.beam_at[2], frame);
            }
            if (args.ball_off_from >= 0
                    && !ball_off.apply(engine, static_cast<long long>(frame), args.ball_off_from, args.ball_off_to,
                                       args.ball_off_wake_only, args.ball_off_drop_all))
                return 2;   // step EG0: --ball-off
            engine.set_keyboard_state(expected.keys.data());
            const long long frame_index = static_cast<long long>(frame);
            if (args.debug_from >= 0 && frame_index >= args.debug_from && frame_index <= args.debug_to) {
                std::string last;
                const bool ok = engine.tick_debug([&](const std::string& script, const std::string& block,
                                                      const std::string& sub, int action) {
                    if (action != 1) return;
                    const std::string line = script + " :: " + block + (sub.empty() ? "" : " / " + sub);
                    if (line == last) return;
                    last = line;
                    std::printf("  [exec frame %lld] %s\n", frame_index, line.c_str());
                }, error);
                if (!ok) { std::fprintf(stderr, "debug tick failed: %s\n", error.c_str()); return 1; }
            } else if (!engine.tick(error)) { std::fprintf(stderr, "tick failed: %s\n", error.c_str()); return 1; }
            dump_entities("after");
            if (args.queue_trace) queue_trace.after_frame(engine, frame_index);   // step EG0
            if (args.queue_roundtrip) queue_roundtrip.after_frame(engine, frame_index);   // step B7EG
            if (args.bodies_from >= 0 && frame_index >= args.bodies_from && frame_index <= args.bodies_to) {
                std::printf("bodies frame%lld: %s\nevents frame%lld: %s\n", frame_index,
                    bmmo::physics::describe_physics_objects(engine.physics()).c_str(), frame_index,
                    bmmo::physics::drain_event_log(engine.physics()).c_str());
                std::fflush(stdout);
            }
            bmmo::physics::world_hash actual;
            if (!bmmo::physics::capture_world_hash(engine.physics(), actual, error)) {
                std::fprintf(stderr, "hash failed: %s\n", error.c_str());
                return 1;
            }
            if (output.is_open()) {
                bmmo::physics::tick_record_frame written = expected;
                written.hash = actual.hash;
                written.pose = actual.pose;
                written.surfaces = actual.surfaces;
                written.ivp_time = actual.ivp_time;
                written.cores = actual.cores;
                std::snprintf(written.probe_name, sizeof(written.probe_name), "%s", actual.probe_name);
                for (int k = 0; k < 3; ++k) {
                    written.probe_position[k] = actual.probe_position[k];
                    written.probe_speed[k] = actual.probe_speed[k];
                    written.probe_rot_speed[k] = actual.probe_rot_speed[k];
                }
                output.write(written);
            }
            const bool same = actual.hash == expected.hash;
            if (same) ++matched;
            else if (first_divergence < 0) {
                first_divergence = static_cast<long long>(frame);
                std::printf("    server movable: %s\n",
                    bmmo::physics::describe_movable_objects(engine.physics()).c_str());
                std::printf("    server bodies: %s\n",
                    bmmo::physics::describe_physics_objects(engine.physics()).c_str());
                std::printf("    server events: %s\n",
                    bmmo::physics::drain_event_log(engine.physics()).c_str());
                std::printf("    surfaces expected=%016llx actual=%016llx pose expected=%016llx actual=%016llx probe=%s/%s\n",
                    static_cast<unsigned long long>(expected.surfaces),
                    static_cast<unsigned long long>(actual.surfaces),
                    static_cast<unsigned long long>(expected.pose), static_cast<unsigned long long>(actual.pose),
                    expected.probe_name, actual.probe_name);
                std::printf("    probe expected pos=(%.9g,%.9g,%.9g) speed=(%.9g,%.9g,%.9g)\n"
                            "    probe actual   pos=(%.9g,%.9g,%.9g) speed=(%.9g,%.9g,%.9g)\n",
                    expected.probe_position[0], expected.probe_position[1], expected.probe_position[2],
                    expected.probe_speed[0], expected.probe_speed[1], expected.probe_speed[2],
                    actual.probe_position[0], actual.probe_position[1], actual.probe_position[2],
                    actual.probe_speed[0], actual.probe_speed[1], actual.probe_speed[2]);
                std::printf("DIVERGE frame=%zu expected=%016llx actual=%016llx cores=%d/%d ivp_time=%.6f/%.6f\n",
                    frame, static_cast<unsigned long long>(expected.hash),
                    static_cast<unsigned long long>(actual.hash), expected.cores, actual.cores,
                    expected.ivp_time, actual.ivp_time);
            }
            if (args.exact_from >= 0 && static_cast<long long>(frame) >= args.exact_from
                    && static_cast<long long>(frame) <= args.exact_to)
                std::printf("exact frame%zu ivp_time=%.6f seed=%d\n%s", frame, actual.ivp_time, actual.ivp_seed,
                    bmmo::physics::describe_cores_exact(engine.physics()).c_str());
            if (args.report_every > 0 && (frame % static_cast<size_t>(args.report_every)) == 0)
                std::printf("frame=%zu %s expected=%016llx actual=%016llx cores=%d/%d ivp_time=%.6f/%.6f seed=%d mc=%d psi=%.6f/%.6f"
                            " pose=%s probe=%s/%s dpos=(%.3g,%.3g,%.3g) dspeed=(%.3g,%.3g,%.3g)\n",
                    frame, same ? "ok" : "MISMATCH", static_cast<unsigned long long>(expected.hash),
                    static_cast<unsigned long long>(actual.hash), expected.cores, actual.cores,
                    expected.ivp_time, actual.ivp_time, actual.ivp_seed, static_cast<int>(actual.next_movement_check),
                    actual.time_of_last_psi, actual.time_of_next_psi,
                    expected.pose == actual.pose ? "same" : "DIFF", expected.probe_name, actual.probe_name,
                    actual.probe_position[0] - expected.probe_position[0],
                    actual.probe_position[1] - expected.probe_position[1],
                    actual.probe_position[2] - expected.probe_position[2],
                    actual.probe_speed[0] - expected.probe_speed[0],
                    actual.probe_speed[1] - expected.probe_speed[1],
                    actual.probe_speed[2] - expected.probe_speed[2]);
        }
        std::printf("summary: frames=%zu matched=%zu first_divergence=%lld\n",
            record.frames.size(), matched, first_divergence);
        std::fflush(stdout);
        return first_divergence < 0 ? 0 : 3;
    }
}

namespace {
    // ------------------------------------------- the physics_world family
    // The session world a room builds and the scenes its switches describe,
    // shared by --drop, --spawn-test and --savestate-check.  Every helper that
    // can fail prints its own message and returns the exit code (0 = done),
    // so each mode keeps the codes and the output it always had.

    // Step S7 (design 7.2 V7/V9): `n` plain CK objects (no name, no scene,
    // no tick runs while they exist), and their destruction - with
    // `free_ids`, one call per object in ascending id order, so the free
    // list (CKObjectManager: pushed at UnRegisterObject, popped from the
    // back) hands them out again top-down; a batch would push them in its
    // dependency map's hash order.
    std::vector<CK_ID> make_dummy_objects(CKContext* context, int n) {
        std::vector<CK_ID> ids;
        ids.reserve(static_cast<size_t>(std::max(0, n)));
        for (int i = 0; i < n; ++i)
            if (CKObject* o = context->CreateObject(CKCID_OBJECT, nullptr, CK_OBJECTCREATION_NONAMECHECK))
                ids.push_back(o->GetID());
        std::sort(ids.begin(), ids.end());
        return ids;
    }
    void destroy_dummy_objects(CKContext* context, std::vector<CK_ID>& ids, bool free_ids) {
        CKDependencies none;
        if (!free_ids) {
            if (!ids.empty()) context->DestroyObjects(ids.data(), static_cast<int>(ids.size()), 0, &none);
            return;
        }
        for (CK_ID& id: ids) context->DestroyObjects(&id, 1, CK_DESTROY_FREEID, &none);
    }
    // The first copy's root and its original's ids, and how many copies
    // are below their originals (the id layout the arm asked for).
    void print_copy_ids(const bmmo::sim::physics_world& world) {
        const auto& copies = world.placement().copies();
        size_t below = 0;
        for (const auto& c: copies) below += c.root < c.source_id ? 1u : 0u;
        if (copies.empty()) {
            std::printf("ids: no copies (placement off or nothing to copy)\n");
            return;
        }
        const auto& c = copies.front();
        std::printf("ids: copy %s id=%u, original %s id=%u (%s); %zu of %zu copies below their originals\n",
                    c.name.c_str(), static_cast<unsigned>(c.root), c.source.c_str(), static_cast<unsigned>(c.source_id),
                    c.root < c.source_id ? "below" : "above", below, copies.size());
    }

    std::unique_ptr<bmmo::sim::physics_world> create_session_world(const arguments& args, float spawn_impulse) {
        bmmo::sim::world_options options;
        options.game_root = args.root;
        options.level = args.level > 0 ? args.level : 1;
        options.seed = 1;
        options.spawn_impulse = spawn_impulse;
        options.boot_ticks = args.boot_ticks;
        options.anchor_timeout = args.anchor_timeout;
        options.placement_clones = args.placement;            // step S2
        options.placement_fail_at = args.placement_fail_at;
        options.trafo_inputs = (args.world_options & bmmo::session::WORLD_TRAFO_INPUTS) != 0;   // step K0
        options.proximity_union = (args.world_options & bmmo::session::WORLD_PROXIMITY_UNION) != 0;   // step G2
        // Step Q7: the per-player units under the union (the drop scene, V7).
        options.mechanism_slots = options.proximity_union ? static_cast<uint8_t>(args.mechanism_slots) : 0;
        // Step RK3: the Level 1 tutorial's exit (physics_world::tick).
        options.session_clock = (args.world_options & bmmo::session::WORLD_SESSION_CLOCK) != 0;
        options.tutorial_exit_frame = args.tutorial_exit_frame;
        options.pin_checkpoint_deactivation = args.n2_pin;   // step B2SW: --n2-pin off is the A/B
        options.split_sector_cell = args.n2_cell;            // step B2SX: --n2-cell off is the A/B
        options.log = [](const std::string& text) { std::fprintf(stderr, "%s\n", text.c_str()); };
        // Step S7: the id layout (--free-ids-before-load, --perturb-ids).
        if (args.free_ids_before_load > 0) {
            const int n = args.free_ids_before_load;
            options.before_level_load = [n](CKContext* context) {
                std::vector<CK_ID> ids = make_dummy_objects(context, n);
                destroy_dummy_objects(context, ids, true);
                std::printf("ids: %zu dummies created before the level load and freed (CK_DESTROY_FREEID, ascending), "
                            "ids %u..%u\n", ids.size(), ids.empty() ? 0u : static_cast<unsigned>(ids.front()),
                            ids.empty() ? 0u : static_cast<unsigned>(ids.back()));
            };
        }
        if (args.perturb_ids > 0) {
            const int n = args.perturb_ids;
            options.before_anchor = [n](CKContext* context) {
                std::vector<CK_ID> ids = make_dummy_objects(context, n);
                destroy_dummy_objects(context, ids, false);
                std::printf("ids: %zu dummies created after the level load and destroyed before the anchor, ids %u..%u\n",
                            ids.size(), ids.empty() ? 0u : static_cast<unsigned>(ids.front()),
                            ids.empty() ? 0u : static_cast<unsigned>(ids.back()));
            };
        }
        std::string error;
        auto world = bmmo::sim::physics_world::create(options, error);
        if (!world) std::fprintf(stderr, "world create failed: %s\n", error.c_str());
        if (world && (args.perturb_ids >= 0 || args.free_ids_before_load > 0)) print_copy_ids(*world);
        return world;
    }

    // --tick-hashes / --watch-entity (step S2) in the drop scene: one line
    // per tick with the world hash (every field), and the watched entities'
    // world positions on the report ticks.
    void placement_tick_report(const arguments& args, bmmo::sim::physics_world& world, int tick, bool report) {
        if (args.tick_hashes) {
            bmmo::physics::world_hash hash;
            std::string error;
            if (bmmo::physics::capture_world_hash(world.physics(), hash, error))
                std::printf("tickhash tick=%d full=%016llx pose=%016llx surfaces=%016llx cores=%d\n", tick,
                            static_cast<unsigned long long>(hash.hash), static_cast<unsigned long long>(hash.pose),
                            static_cast<unsigned long long>(hash.surfaces), hash.cores);
            else
                std::printf("tickhash tick=%d <%s>\n", tick, error.c_str());
        }
        if (!report || args.watch_entities.empty()) return;
        std::string text;
        for (const std::string& name: args.watch_entities) {
            auto* entity = CK3dEntity::Cast(world.engine().context()->GetObjectByNameAndParentClass(
                const_cast<CKSTRING>(name.c_str()), CKCID_3DENTITY, nullptr));
            char part[200];
            if (!entity) {
                std::snprintf(part, sizeof(part), " %s(none)", name.c_str());
            } else {
                VxVector p;
                entity->GetPosition(&p);
                std::snprintf(part, sizeof(part), " %s(%.4f,%.4f,%.4f)%s", name.c_str(), p.x, p.y, p.z,
                              entity->IsVisible() ? "" : "[hidden]");
            }
            text += part;
        }
        std::printf("watch tick=%d%s\n", tick, text.c_str());
    }

    // The sector union follows the players: report where each of them is and
    // the world starts the right mechanisms on its own.
    void report_sector(bmmo::sim::physics_world& world, uint32_t player, int sector) {
        bmmo::sim::lifecycle_event where;
        where.type = bmmo::session::event_type::Sector;
        where.sector = sector;
        std::string error;
        if (!world.apply_event(player, where, error))
            std::fprintf(stderr, "sector event failed: %s\n", error.c_str());
    }

    // `ticks` whole world ticks; false (after printing why) on a failed tick.
    bool settle_world(bmmo::sim::physics_world& world, int ticks) {
        std::string error;
        for (int i = 0; i < ticks; ++i)
            if (!world.tick(error)) { std::fprintf(stderr, "tick failed: %s\n", error.c_str()); return false; }
        return true;
    }

    // ---- the invariant check I1's scene (sector placement design 7.3, step S3)

    // --placement-trace's writer while a drop scene runs; null otherwise.
    std::unique_ptr<bmmo::sim::placement_trace>& scene_trace() {
        static std::unique_ptr<bmmo::sim::placement_trace> trace;
        return trace;
    }

    // ---- step K0: --globals-trace (record-format-gaps-design.md 7.2 S5)

    // The physics globals of a world: CKIpionManager's time factor and
    // physics delta (get_clock) and the environment's gravity (GetGravity,
    // the floats SetPhysicsGlobals' VxVector held).  Read-only.
    struct physics_globals_sample {
        bool ok = false;
        float factor = 0.0f, delta = 0.0f;
        float gravity[3] = {};
        bool same(const physics_globals_sample& o) const {
            return ok == o.ok && std::memcmp(&factor, &o.factor, sizeof(float)) == 0
                && std::memcmp(gravity, o.gravity, sizeof(gravity)) == 0;
        }
    };
    physics_globals_sample sample_physics_globals(bmmo::sim::physics_world& world) {
        physics_globals_sample s;
        CKIpionManager* manager = world.physics();
        std::string error;
        if (!manager || !manager->GetEnvironment() || !bmmo::physics::get_clock(manager, s.factor, s.delta, error)) return s;
        VxVector g;
        manager->GetGravity(g);
        s.gravity[0] = g.x; s.gravity[1] = g.y; s.gravity[2] = g.z;
        s.ok = true;
        return s;
    }

    struct globals_trace_state {
        bool on = false;
        bool have = false;
        physics_globals_sample last;
        long long lines = 0;
        std::vector<std::string> changes;   // "tick T: before -> after"
    };
    globals_trace_state& globals_trace() {
        static globals_trace_state state;
        return state;
    }

    std::string globals_text(const physics_globals_sample& s) {
        if (!s.ok) return "unavailable";
        char text[256];
        std::snprintf(text, sizeof(text), "factor=%.9g (%a) pdelta=%.9g (%a) gravity=(%.9g,%.9g,%.9g)",
                      static_cast<double>(s.factor), static_cast<double>(s.factor), static_cast<double>(s.delta),
                      static_cast<double>(s.delta), static_cast<double>(s.gravity[0]), static_cast<double>(s.gravity[1]),
                      static_cast<double>(s.gravity[2]));
        return text;
    }

    // One line for `tick` ("anchor" for the state the world was created
    // with, else the world tick index the line follows); a change of the
    // factor or the gravity from the line before is marked and kept for the
    // summary.  The physics delta is printed, not compared: SetDeltaTime
    // averages it every frame.
    void globals_trace_tick(bmmo::sim::physics_world& world, const std::string& tick) {
        globals_trace_state& t = globals_trace();
        if (!t.on) return;
        const physics_globals_sample s = sample_physics_globals(world);
        const bool changed = t.have && !s.same(t.last);
        std::printf("globals tick=%s %s%s\n", tick.c_str(), globals_text(s).c_str(), changed ? " CHANGED" : "");
        if (changed) t.changes.push_back("tick " + tick + ": " + globals_text(t.last) + " -> " + globals_text(s));
        t.last = s;
        t.have = true;
        ++t.lines;
    }

    void open_globals_trace(const arguments& args, bmmo::sim::physics_world& world) {
        if (!args.globals_trace) return;
        globals_trace() = globals_trace_state{};
        globals_trace().on = true;
        globals_trace_tick(world, "anchor");
    }

    void close_globals_trace() {
        globals_trace_state& t = globals_trace();
        if (!t.on) return;
        std::printf("globals-trace: %lld lines, %zu changes after the anchor\n", t.lines, t.changes.size());
        for (const std::string& c: t.changes) std::printf("globals-trace change %s\n", c.c_str());
        t.on = false;
        std::fflush(stdout);   // a mode's own exit path may not flush (the engine's shutdown)
    }

    // Closes the trace (prints its summary) on every way out of the scope,
    // the early returns of a failed scene included; a no-op after an explicit
    // close_globals_trace().
    struct globals_trace_guard {
        globals_trace_guard() = default;
        globals_trace_guard(const globals_trace_guard&) = delete;
        globals_trace_guard& operator=(const globals_trace_guard&) = delete;
        ~globals_trace_guard() { close_globals_trace(); }
    };

    // One world tick of the drop scene: the second player's --second-move
    // passes and the ball's --drop-again due before this tick, the tick, then
    // the trace's lines for it.
    bool scene_tick(const arguments& args, bmmo::sim::physics_world& world, std::string& error) {
        const uint32_t tick = world.tick_index();
        if (args.drop_kill >= 0 && static_cast<long long>(tick) == args.drop_kill) {   // step G2
            for (uint32_t id: {1u, 2u}) {
                if (args.drop_kill_player && id != args.drop_kill_player) continue;   // close-out step S2 (V4b)
                if (!world.player_physicalized(id)) continue;
                bmmo::sim::lifecycle_event death;
                death.type = bmmo::session::event_type::Unphysicalize;
                death.tick = tick;
                std::string kill_error;
                if (!world.apply_event(id, death, kill_error))
                    std::fprintf(stderr, "--drop-kill: player %u: %s\n", id, kill_error.c_str());
                else
                    std::printf("tick=%u player %u's ball unphysicalized (--drop-kill)\n", tick, id);
            }
        }
        for (const arguments::second_move& m: args.second_moves)
            if (m.tick == static_cast<long long>(tick)) {
                report_sector(world, 2, m.sector);
                std::printf("tick=%u player 2 moved to sector %d\n", tick, m.sector);
            }
        for (const arguments::drop_again& d: args.drops_again) {
            if (d.tick != static_cast<long long>(tick)) continue;
            const std::string ball = world.player_ball_entity(1);
            const double upright[4] = {0.0, 0.0, 0.0, 1.0};
            const float still[3] = {0.0f, 0.0f, 0.0f};
            std::string beam_error;
            if (ball.empty() || !bmmo::physics::set_body_state(world.physics(), ball.c_str(), d.at, upright, still, still,
                                                               true, beam_error)) {
                error = "--drop-again at tick " + std::to_string(tick) + ": "
                      + (ball.empty() ? std::string("player 1 has no ball") : beam_error);
                return false;
            }
            std::printf("tick=%u ball %s dropped again at (%.3f,%.3f,%.3f)\n", tick, ball.c_str(), d.at[0], d.at[1], d.at[2]);
        }
        if (!world.tick(error)) return false;
        if (const auto& trace = scene_trace()) {
            trace->set_ball(world.player_ball_entity(1));
            trace->write(tick);
        }
        globals_trace_tick(world, std::to_string(tick));   // step K0
        return true;
    }

    // The sector scene's settle through scene_tick.
    bool settle_scene(const arguments& args, bmmo::sim::physics_world& world) {
        std::string error;
        for (int i = 0; i < args.drop_settle; ++i)
            if (!scene_tick(args, world, error)) { std::fprintf(stderr, "tick failed: %s\n", error.c_str()); return false; }
        return true;
    }

    // Opens --placement-trace on the drop scene's world (watched sector:
    // --drop-sector); 0 = done (or nothing asked).
    int open_scene_trace(const arguments& args, bmmo::sim::physics_world& world) {
        if (args.placement_trace.empty()) return 0;
        auto trace = std::make_unique<bmmo::sim::placement_trace>(world.engine().context(), world.physics(),
                                                                   args.drop_sector);
        std::string error;
        if (!trace->open(args.placement_trace, error)) {
            std::fprintf(stderr, "--placement-trace: %s\n", error.c_str());
            return 1;
        }
        trace->trace_ball(args.placement_trace_ball);
        scene_trace() = std::move(trace);
        return 0;
    }

    // Closes the trace and prints what its last tick held, and the world's
    // physics time factor (0 stops every body: Level 1's tutorial).
    void close_scene_trace(bmmo::sim::physics_world& world) {
        auto& trace = scene_trace();
        if (!trace) return;
        const auto& t = trace->last();
        std::printf("placement trace: sector %d, %lld lines; last tick: %d hierarchies, %d entities, %d movable bodies"
                    " (%d simulated), %d fixed, %d joint blocks (%d holding a handle), %d force blocks, %d proximity"
                    " blocks, %d scripts (%d active)\n",
                    trace->sector(), trace->lines(), t.hierarchies, t.entities, t.movable, t.simulated, t.fixed, t.joints,
                    t.joints_held, t.forces, t.proximities, t.scripts, t.scripts_active);
        float time_factor = 0.0f, physics_delta = 0.0f;
        std::string error;
        if (bmmo::physics::get_clock(world.physics(), time_factor, physics_delta, error))
            std::printf("placement trace: physics time factor %g, physics delta %g\n", time_factor, physics_delta);
        else
            std::printf("placement trace: physics clock unavailable: %s\n", error.c_str());
        trace.reset();
    }

    // The S3 switches belong to the drop scene alone; 0 = fine.
    int check_scene_switches(const arguments& args) {
        // Step SP8: the networked invariant's switches belong to --replay-session.
        if ((args.placement_trace_player != 0 || args.drop_sectors_above >= 0) && args.replay_session.empty()) {
            std::fprintf(stderr, "--placement-trace-player / --drop-sectors-above: need --replay-session\n");
            return 2;
        }
        // run_replay_session is dispatched before every other mode and reads
        // none of them, so a second mode next to the invariant's switches would
        // be ignored without a word: refuse the combination instead.
        if (!args.replay_session.empty()
            && (args.placement_trace_player != 0 || args.drop_sectors_above >= 0 || !args.placement_trace.empty()
                || args.placement_trace_ball)
            && (!args.drop_entity.empty() || args.savestate_check || !args.late_run.empty() || args.late_sector_check
                || args.resync_check || args.portable_scripts_check || args.list_placement || args.array_reads
                || args.probe_census || args.spawn_test > 0 || !args.nav_mode.empty() || !args.synth_trafo_session.empty()
                || !args.describe_script.empty() || !args.savestate_diff[0].empty()
                || !args.savestate_describe.empty())) {
            std::fprintf(stderr, "--placement-trace / --placement-trace-player / --drop-sectors-above with "
                                 "--replay-session: the replay is the only mode (no --drop, --savestate-check, "
                                 "--late-run, --late-sector-check, --resync-check, --portable-scripts-check, "
                                 "--list-placement, --array-reads, --spawn-test, --nav, --synth-trafo-session, "
                                 "--describe-script, --savestate-diff/-describe)\n");
            return 2;
        }
        if (!args.replay_session.empty() && args.drop_entity.empty() && !args.savestate_check) {
            const char* why = nullptr;
            if (!args.second_moves.empty() || !args.drops_again.empty())
                why = "--second-move and --drop-again are for the --drop scene (and --savestate-check)";
            else if (args.placement_trace_ball && args.placement_trace.empty())
                why = "--placement-trace-ball needs --placement-trace";
            else if (args.placement_trace_ball && args.placement_trace_player == 0)
                why = "in a --replay-session the ball line needs --placement-trace-player ID";
            else if (args.list_journal && (!args.placement_trace.empty() || args.drop_sectors_above >= 0))
                why = "--list replays nothing";
            if (!why) return 0;
            std::fprintf(stderr, "--placement-trace / --drop-sectors-above: %s\n", why);
            return 2;
        }
        if (args.second_moves.empty() && args.drops_again.empty() && args.placement_trace.empty()
            && !args.placement_trace_ball)
            return 0;
        const char* why = nullptr;
        // Step S7: --savestate-check takes --second-move (a pass inside the
        // window, or before it) in its drop and sector scenes; the trace and
        // the second drop stay the plain --drop scene's.
        if (args.savestate_check) {
            if (!args.drops_again.empty() || !args.placement_trace.empty() || args.placement_trace_ball)
                why = "--drop-again and --placement-trace are for the --drop scene only (--savestate-check takes --second-move)";
            else if (args.drop_entity.empty() && args.spawn_test > 0)
                why = "--second-move needs --savestate-check's drop or sector scene, not --spawn-test";
            else if (args.drop_second_sector <= 0)
                why = "--second-move moves the second player: it needs --drop-second-sector";
            else if (!args.replay_session.empty() || !args.late_run.empty() || args.late_sector_check || args.resync_check
                     || args.portable_scripts_check || args.list_placement)
                why = "are for the --drop scene and --savestate-check only";
            if (!why) return 0;
            std::fprintf(stderr, "--second-move / --drop-again / --placement-trace: %s\n", why);
            return 2;
        }
        if (args.drop_entity.empty()) why = "need --drop or --replay-session (--placement-trace only)";
        else if (!args.replay_session.empty() || !args.synth_trafo_session.empty() || !args.describe_script.empty()
                 || !args.nav_mode.empty() || !args.savestate_diff[0].empty() || !args.savestate_describe.empty())
            why = "are for the --drop scene only";
        else if (args.placement_trace_ball && args.placement_trace.empty())
            why = "--placement-trace-ball needs --placement-trace";
        else if (!args.late_run.empty() || args.late_sector_check || args.resync_check
                 || args.portable_scripts_check || args.list_placement)
            why = "are for the --drop scene only (and --second-move for --savestate-check, step S7)";
        else if (!args.second_moves.empty() && args.drop_second_sector <= 0)
            why = "--second-move moves the second player: it needs --drop-second-sector";
        for (const arguments::drop_again& d: args.drops_again)
            if (!why && d.tick < args.drop_settle) why = "--drop-again's tick is before the ball's drop (--drop-settle)";
        if (!why) return 0;
        std::fprintf(stderr, "--second-move / --drop-again / --placement-trace: %s\n", why);
        return 2;
    }

    // The sector scene: player 1 (and, with --drop-second-sector, player 2
    // parked there) reports its sector, then --drop-settle world ticks run
    // the union's passes and let the sector's bodies come to rest.
    int setup_sector_scene(const arguments& args, bmmo::sim::physics_world& world) {
        std::string error;
        if (!world.add_player(1, 0, error)) {
            std::fprintf(stderr, "add_player failed: %s\n", error.c_str());
            return 1;
        }
        if (args.drop_second_sector > 0) {
            if (!world.add_player(2, 1, error)) {
                std::fprintf(stderr, "add_player 2 failed: %s\n", error.c_str());
                return 1;
            }
            report_sector(world, 2, args.drop_second_sector);
        }
        report_sector(world, 1, args.drop_sector);
        if (!settle_scene(args, world)) return 1;   // settle_world plus the S3 switches (step S3)
        if (!args.dump_array.empty()) dump_array(world.engine(), args.dump_array);
        return 0;
    }

    // The drop scene: the sector scene, then player 1's ball of --drop's type
    // physicalized --drop-height above --drop-at (or the --drop entity) - or
    // at --drop-player-at - and the --drop-prop prop beamed to the drop spot.
    int setup_drop_scene(const arguments& args, bmmo::sim::physics_world& world) {
        if (const int code = setup_sector_scene(args, world)) return code;
        std::string error;
        VxVector spawn(args.drop_at[0], args.drop_at[1], args.drop_at[2]);
        if (!args.have_drop_at) {
            // Module parts share names across sectors, so --drop-at (the PH
            // table's matrix for the module) is the reliable way in.
            auto* target = CK3dEntity::Cast(world.engine().context()->GetObjectByNameAndParentClass(
                const_cast<CKSTRING>(args.drop_entity.c_str()), CKCID_3DENTITY, nullptr));
            if (!target) {
                std::fprintf(stderr, "no 3D entity named %s (pass --drop-at X Y Z)\n", args.drop_entity.c_str());
                return 1;
            }
            target->GetPosition(&spawn);
        }
        spawn.y += args.drop_height;
        VxVector player_spawn = args.have_drop_player_at
            ? VxVector(args.drop_player_at[0], args.drop_player_at[1], args.drop_player_at[2]) : spawn;
        bmmo::sim::lifecycle_event event;
        event.type = bmmo::session::event_type::Physicalize;
        event.ball_type = static_cast<uint8_t>(args.drop_ball);
        event.position[0] = player_spawn.x;
        event.position[1] = player_spawn.y;
        event.position[2] = player_spawn.z;
        event.recipe = world.retail_recipe(event.ball_type);
        if (!world.apply_event(1, event, error)) {
            std::fprintf(stderr, "physicalize failed: %s\n", error.c_str());
            return 1;
        }
        std::printf("dropped ball type %d (%s) at (%.3f,%.3f,%.3f), watching %s\n", args.drop_ball,
            world.ball_rows()[args.drop_ball].name.c_str(), player_spawn.x, player_spawn.y, player_spawn.z,
            args.drop_entity.c_str());
        if (args.drop_second_ball >= 0) {
            // Step G2 (V2, V4): a second player's ball, a second candidate.
            if (static_cast<size_t>(args.drop_second_ball) >= world.ball_rows().size()) {
                std::fprintf(stderr, "--drop-second-ball %d is out of range (%zu ball types)\n", args.drop_second_ball,
                             world.ball_rows().size());
                return 2;
            }
            if (!world.has_player(2) && !world.add_player(2, 1, error)) {
                std::fprintf(stderr, "add_player 2 failed: %s\n", error.c_str());
                return 1;
            }
            bmmo::sim::lifecycle_event second;
            second.type = bmmo::session::event_type::Physicalize;
            second.ball_type = static_cast<uint8_t>(args.drop_second_ball);
            for (int k = 0; k < 3; ++k) second.position[k] = args.drop_second_at[k];
            second.recipe = world.retail_recipe(second.ball_type);
            if (!world.apply_event(2, second, error)) {
                std::fprintf(stderr, "physicalize player 2 failed: %s\n", error.c_str());
                return 1;
            }
            std::printf("player 2 dropped ball type %d (%s) at (%.3f,%.3f,%.3f)\n", args.drop_second_ball,
                        world.ball_rows()[args.drop_second_ball].name.c_str(), args.drop_second_at[0],
                        args.drop_second_at[1], args.drop_second_at[2]);
        }
        if (!args.drop_prop.empty()) {
            // The level's own ball, physicalized by the sector activation: beam
            // it to the drop spot the same way a player would have pushed it.
            const double position[3] = {spawn.x, spawn.y, spawn.z};
            const double upright[4] = {0.0, 0.0, 0.0, 1.0};   // beaming needs a rotation as well
            const float still[3] = {0.0f, 0.0f, 0.0f};
            if (!bmmo::physics::set_body_state(world.physics(), args.drop_prop.c_str(), position, upright,
                                               still, still, true, error))
                std::fprintf(stderr, "prop %s: %s\n", args.drop_prop.c_str(), error.c_str());
            else
                std::printf("prop %s beamed to (%.3f,%.3f,%.3f)\n", args.drop_prop.c_str(), spawn.x, spawn.y, spawn.z);
        }
        return 0;
    }

    // The spawn scene: --spawn-test N players, --drop-settle world ticks, then
    // every player physicalizes its --spawn-ball at the level's resetpoint in
    // the same tick with the SPAWN flag, the way N retail clients spawning
    // together would report it.  `ids` gets the players.
    int setup_spawn_scene(const arguments& args, bmmo::sim::physics_world& world, std::vector<uint32_t>& ids) {
        std::string error;
        const uint8_t ball_type = static_cast<uint8_t>(args.spawn_ball);
        if (ball_type >= world.ball_rows().size()) {
            std::fprintf(stderr, "--spawn-ball %d is out of range (%zu ball types)\n", args.spawn_ball, world.ball_rows().size());
            return 2;
        }
        const int n = std::max(1, args.spawn_test);
        ids.clear();
        for (int i = 0; i < n; ++i) {
            const uint32_t id = static_cast<uint32_t>(i + 1);
            if (!world.add_player(id, static_cast<uint8_t>(i), error)) {
                std::fprintf(stderr, "add_player %u failed: %s\n", id, error.c_str());
                return 1;
            }
            ids.push_back(id);
        }
        if (!settle_world(world, args.drop_settle)) return 1;
        const VxMatrix& spawn = world.spawn_matrix();
        for (const uint32_t id: ids) {
            bmmo::sim::lifecycle_event event;
            event.type = bmmo::session::event_type::Physicalize;
            event.ball_type = ball_type;
            event.flags = bmmo::session::PHYSICALIZE_FLAG_SPAWN;
            for (int k = 0; k < 3; ++k) event.position[k] = spawn[3][k];
            for (int r = 0; r < 3; ++r)
                for (int k = 0; k < 3; ++k) event.rotation[r * 3 + k] = spawn[r][k];
            event.recipe = world.retail_recipe(ball_type);
            event.tick = world.tick_index();
            if (!world.apply_event(id, event, error)) {
                std::fprintf(stderr, "physicalize player %u failed: %s\n", id, error.c_str());
                return 1;
            }
        }
        std::printf("spawned %d players (ball type %d, %s) at tick %u, impulse %.3f m/s\n", n, ball_type,
            world.ball_rows()[ball_type].name.c_str(), world.tick_index(), static_cast<double>(args.spawn_impulse));
        return 0;
    }

    // Step G2 (--probe-trace): the union's frames (BMMO_Prox_*) and identity
    // copies (CurrentLevel#BMMOId_*, their ActiveBall cell) whose names hold
    // `filter`, as they are after the tick (placed before it: a frame shows
    // where its ball was at the end of the previous tick, or the hold).
    std::string probe_trace_lines(bmmo::sim::physics_world& world, const std::string& filter, int tick) {
        CKContext* context = world.engine().context();
        std::string text;
        std::map<std::string, std::string> lines;   // by name: a stable order
        const int count = context->GetObjectsCountByClassID(CKCID_3DENTITY);
        CK_ID* ids = context->GetObjectsListByClassID(CKCID_3DENTITY);
        for (int i = 0; i < count; ++i) {
            auto* frame = CK3dEntity::Cast(context->GetObject(ids[i]));
            const char* name = frame ? frame->GetName() : nullptr;
            if (!name || std::strncmp(name, "BMMO_Prox_", 10) != 0 || !std::strstr(name, filter.c_str())) continue;
            VxVector v;
            frame->GetPosition(&v);
            char line[256];
            std::snprintf(line, sizeof(line), "  probe tick=%d %s (%.3f,%.3f,%.3f)\n", tick, name, v.x, v.y, v.z);
            lines[name] = line;
        }
        const int arrays = context->GetObjectsCountByClassID(CKCID_DATAARRAY);
        CK_ID* array_ids = context->GetObjectsListByClassID(CKCID_DATAARRAY);
        for (int i = 0; i < arrays; ++i) {
            auto* array = CKDataArray::Cast(context->GetObject(array_ids[i]));
            const char* name = array ? array->GetName() : nullptr;
            if (!name || std::strncmp(name, "CurrentLevel#BMMOId_", 20) != 0 || !std::strstr(name, filter.c_str())) continue;
            int column = -1;
            for (int c = 0; c < array->GetColumnCount(); ++c)
                if (array->GetColumnName(c) && std::string_view(array->GetColumnName(c)) == "ActiveBall") column = c;
            CKObject* cell = column >= 0 && array->GetRowCount() > 0 ? array->GetElementObject(0, column) : nullptr;
            lines[name] = "  identity tick=" + std::to_string(tick) + " " + name + " = "
                          + (cell && cell->GetName() ? cell->GetName() : "(none)") + "\n";
        }
        // Step Q7 (V7): each unit force block whose alias names the filter -
        // its slot and whether it holds a force (SetPhysicsForce's local 0,
        // the IVP handle, non-zero).
        for (const auto& [id, alias]: world.proximity().force_aliases()) {
            if (alias.find(filter) == std::string::npos) continue;
            auto* block = CKBehavior::Cast(context->GetObject(id));
            CKParameterLocal* handle = block && block->GetLocalParameterCount() > 0 ? block->GetLocalParameter(0) : nullptr;
            bool held = false;
            if (handle) {
                const auto* bytes = static_cast<const uint8_t*>(handle->GetReadDataPtr(FALSE));
                for (int k = 0; bytes && k < handle->GetDataSize(); ++k) held = held || bytes[k] != 0;
            }
            lines["~" + alias] = "  force tick=" + std::to_string(tick) + " " + alias + " rt:force/" + std::to_string(id)
                                 + (held ? " HELD" : " none") + "\n";
        }
        for (const auto& [name, line]: lines) text += line;
        const auto& u = world.proximity();
        text += "  union tick=" + std::to_string(tick) + " skipped=" + std::to_string(u.skipped()) + " relatched="
                + std::to_string(u.relatched()) + (world.proximity_union_on() ? " (bit set)" : " (bit clear)") + "\n";
        return text;
    }

    // Server-side mechanism check (design 8.3): boots a physics-session world
    // the way a room does, puts its player (and optionally a second one) in a
    // sector so the union starts that sector's mechanisms, and drops the
    // player's ball of the given type onto a mechanism part, then prints that
    // part's height every report interval.  A mechanism that reacts to the
    // ball (the rope bridge P_Modul_29 tearing under a stone ball) moves; one
    // that does not holds its anchor pose.
    int run_drop(const arguments& args) {
        auto world = create_session_world(args, 0.0f);
        if (!world) return 1;
        if (const int code = open_scene_trace(args, *world)) return code;   // step S3
        open_globals_trace(args, *world);                                    // step K0
        const globals_trace_guard globals_guard;                             // step K0
        if (const int code = setup_drop_scene(args, *world)) return code;
        std::string error;
        // The trafo sets in the drop scene (S11, sim/trafo_tool.hpp): built
        // after the scene, their inputs keyed by the run's tick index, the
        // trafo frame inside the world's frame (its frame hook).
        std::unique_ptr<bmmo::sim::trafo_tool> trafo;
        if (args.trafo.any()) {
            trafo = std::make_unique<bmmo::sim::trafo_tool>(args.trafo, world->physics(), world->engine().context());
            if (!trafo->anchor(0)) return 1;
            bmmo::sim::trafo_tool* tool = trafo.get();
            world->set_frame_hook([tool]() { tool->run_frame(); });
        }
        // The parts of the mechanism this run watches: every physics body whose
        // name carries the --drop name.
        const auto watched = [&args](const bmmo::sim::physics_world& w) {
            std::vector<bmmo::physics::body_state> bodies(
                static_cast<size_t>(bmmo::physics::list_bodies(w.physics(), nullptr, 0)));
            const int total = bmmo::physics::list_bodies(w.physics(), bodies.data(), static_cast<int>(bodies.size()));
            bodies.resize(static_cast<size_t>(std::max(total, 0)));
            std::erase_if(bodies, [&args](const bmmo::physics::body_state& body) {
                return std::string_view(body.name).find(args.drop_entity) == std::string_view::npos;
            });
            return bodies;
        };
        const auto report_parts = [&](int tick) {
            std::string text;
            for (const auto& body: watched(*world)) {
                char line[160];
                std::snprintf(line, sizeof(line), " %s(%.3f,%.3f,%.3f)%s", body.name,
                              body.position[0], body.position[1], body.position[2],
                              body.simulated ? "*" : "");
                text += line;
            }
            std::printf("tick=%d parts:%s\n  %s\n", tick, text.empty() ? " none physicalized" : text.c_str(),
                        root_scripts(world->engine()).c_str());
            if (args.probe_trace) std::printf("%s", probe_trace_lines(*world, args.drop_entity, tick).c_str());
        };
        report_parts(-1);
        placement_tick_report(args, *world, -1, true);
        // Step B2SW (--cells-trace): the sector manager's two cells, the
        // union and the physicalized bodies, whenever one of them changes.
        std::string cells_last;
        const auto trace_cells = [&](int tick) {
            if (!args.cells_trace) return;
            CKContext* context = world->engine().context();
            auto* parameters = CKDataArray::Cast(context->GetObjectByNameAndClass(const_cast<CKSTRING>("IngameParameter"),
                                                                                  CKCID_DATAARRAY, nullptr));
            int activate = -1, deactivate = -1;
            if (parameters) {
                parameters->GetElementValue(0, 1, &activate);
                parameters->GetElementValue(0, 2, &deactivate);
            }
            std::string union_text;
            for (int s: world->active_sectors()) union_text += (union_text.empty() ? "" : ",") + std::to_string(s);
            const int bodies = bmmo::physics::list_bodies(world->physics(), nullptr, 0);
            // Step B2SX: the checkpoint chain's private [0][1] (the own sector
            // poll's cell), when this world split it.
            int chain = -1;
            const bool split = world->chain_sector_cell(chain);
            char text[192];
            std::snprintf(text, sizeof(text), "cells [0][1]=%d [0][2]=%d chain[0][1]=%s union={%s} bodies=%d", activate,
                          deactivate, split ? std::to_string(chain).c_str() : "-", union_text.c_str(), bodies);
            if (cells_last == text) return;
            cells_last = text;
            std::printf("tick=%d %s\n", tick, text);
        };
        trace_cells(-1);
        std::map<std::string, double> start;
        for (const auto& body: watched(*world)) start.emplace(body.name, body.position[1]);
        for (int i = 0; i < args.ticks; ++i) {
            if (args.drop_move_sector > 0 && i == args.drop_move_tick) {
                report_sector(*world, 1, args.drop_move_sector);
                std::printf("tick=%d player 1 moved to sector %d\n", i, args.drop_move_sector);
            }
            if (trafo) trafo->apply_inputs(static_cast<uint32_t>(i));
            // Step S7 (V8): every body's surface before world tick N (the
            // --second-move unit).
            if (args.dump_surfaces_at >= 0 && static_cast<long long>(world->tick_index()) == args.dump_surfaces_at)
                dump_surfaces(world->engine());
            if (!scene_tick(args, *world, error)) { std::fprintf(stderr, "tick failed: %s\n", error.c_str()); return 1; }
            if (trafo) trafo->after_frame(i, {});
            trace_cells(i);   // step B2SW
            if (args.report_every > 0 && i % args.report_every == 0) report_parts(i);
            placement_tick_report(args, *world, i, (args.report_every > 0 && i % args.report_every == 0) || i + 1 == args.ticks);
        }
        if (args.dump_surfaces_at >= 0 && static_cast<long long>(world->tick_index()) == args.dump_surfaces_at)
            dump_surfaces(world->engine());   // step S7: N = the world tick after the last
        double fell = 0.0;
        for (const auto& body: watched(*world)) {
            auto it = start.find(body.name);
            if (it != start.end()) fell = std::max(fell, it->second - body.position[1]);
        }
        std::printf("summary: the lowest watched part of %s fell %.3f m\n", args.drop_entity.c_str(), fell);
        close_scene_trace(*world);   // step S3
        close_globals_trace();       // step K0
        std::fflush(stdout);
        if (trafo) return trafo->finish();
        return 0;
    }

    // Spawn impulse determinism check (spec A.9 / design 9.10): N players
    // physicalize at the level's resetpoint in the same tick with the SPAWN
    // flag; the deterministic kick should move them apart without ever
    // producing NaN.  Windows vs Linux builds diff the per-tick pose hashes.
    int run_spawn_test(const arguments& args) {
        auto world = create_session_world(args, args.spawn_impulse);
        if (!world) return 1;
        std::vector<uint32_t> ids;
        if (const int code = setup_spawn_scene(args, *world, ids)) return code;
        std::string error;
        const uint8_t ball_type = static_cast<uint8_t>(args.spawn_ball);
        const int ticks = args.ticks_explicit ? args.ticks : 200;
        const int report_every = args.report_every_explicit ? args.report_every : 10;
        const std::string entity_prefix = world->ball_rows()[ball_type].name + "_BMMO_";
        const auto entity_name = [&](uint32_t id) { return entity_prefix + std::to_string(id); };
        bool any_nan = false;
        // The largest centre distance every pair reached: the kick has done
        // its job once a pair was clearly apart at some point, whatever the
        // resetpoint's slope makes them do afterwards (Level 2's balls roll
        // back together; with the overlap gate they then collide normally).
        std::vector<double> max_distance(ids.size() * ids.size(), 0.0);
        const auto track_distances = [&]() {
            std::vector<std::array<double, 3>> now(ids.size());
            std::vector<bool> present(ids.size(), false);
            for (size_t i = 0; i < ids.size(); ++i) {
                bmmo_physics_body_state state{};
                std::string body_error;
                if (!bmmo::physics::get_body_state(world->physics(), entity_name(ids[i]).c_str(), state, body_error)) continue;
                present[i] = true;
                for (int k = 0; k < 3; ++k) {
                    now[i][k] = state.position[k];
                    if (std::isnan(state.position[k])) any_nan = true;
                }
            }
            for (size_t i = 0; i < ids.size(); ++i)
                for (size_t j = i + 1; j < ids.size(); ++j) {
                    if (!present[i] || !present[j]) continue;
                    double d = 0.0;
                    for (int k = 0; k < 3; ++k) d += (now[i][k] - now[j][k]) * (now[i][k] - now[j][k]);
                    max_distance[i * ids.size() + j] = std::max(max_distance[i * ids.size() + j], std::sqrt(d));
                }
        };
        for (int i = 0; i < ticks; ++i) {
            if (!world->tick(error)) { std::fprintf(stderr, "tick failed: %s\n", error.c_str()); return 1; }
            track_distances();
            if (report_every <= 0 || i % report_every != 0) continue;
            std::string line;
            for (const uint32_t id: ids) {
                bmmo_physics_body_state state{};
                std::string body_error;
                if (!bmmo::physics::get_body_state(world->physics(), entity_name(id).c_str(), state, body_error)) continue;
                char part[192];
                const double speed = std::sqrt(static_cast<double>(state.linear[0]) * state.linear[0]
                    + static_cast<double>(state.linear[1]) * state.linear[1] + static_cast<double>(state.linear[2]) * state.linear[2]);
                std::snprintf(part, sizeof(part), " p%u=(%.3f,%.3f,%.3f)v=%.3f", id, state.position[0], state.position[1],
                              state.position[2], speed);
                line += part;
                for (int k = 0; k < 3; ++k) if (std::isnan(state.position[k])) any_nan = true;
            }
            bmmo::physics::world_hash hash;
            std::string hash_error;
            if (bmmo::physics::capture_world_hash(world->physics(), hash, hash_error))
                std::printf("tick=%d%s pose=%016llx\n", i, line.c_str(), static_cast<unsigned long long>(hash.pose));
            else
                std::printf("tick=%d%s pose=<%s>\n", i, line.c_str(), hash_error.c_str());
        }
        std::vector<std::array<double, 3>> positions(ids.size());
        bool ok = true;
        for (size_t i = 0; i < ids.size(); ++i) {
            bmmo_physics_body_state state{};
            std::string body_error;
            if (!bmmo::physics::get_body_state(world->physics(), entity_name(ids[i]).c_str(), state, body_error)) {
                std::fprintf(stderr, "player %u has no body at the end: %s\n", ids[i], body_error.c_str());
                ok = false;
                continue;
            }
            for (int k = 0; k < 3; ++k) {
                positions[i][k] = state.position[k];
                if (std::isnan(state.position[k])) any_nan = true;
            }
        }
        for (size_t i = 0; i < positions.size(); ++i)
            for (size_t j = i + 1; j < positions.size(); ++j) {
                double d = 0.0;
                for (int k = 0; k < 3; ++k) {
                    const double diff = positions[i][k] - positions[j][k];
                    d += diff * diff;
                }
                d = std::sqrt(d);
                const double widest = max_distance[i * ids.size() + j];
                std::printf("distance p%u-p%u = %.3f m at the end, %.3f m at the widest\n", ids[i], ids[j], d, widest);
                // Apart at some point: two radius-2 balls that never reached
                // 3.9 stayed inside each other the whole time.
                if (widest < 3.9) ok = false;
            }
        if (any_nan) std::printf("summary: NaN detected in the final positions\n");
        std::printf("summary: %s\n", (ok && !any_nan) ? "ok" : "FAILED");
        std::fflush(stdout);
        return (ok && !any_nan) ? 0 : 2;
    }

    // --------------------------------------------------- --savestate-check
    //
    // The IVP savestate's robustness matrix (docs/ivp-savestate-design.md,
    // "Tests that prove it").  The claim under test: the physics after any
    // number of rewinds is bit for bit the physics of the straight run, and
    // every comparison is of WHOLE savestate blobs - a blob is the engine's
    // entire dynamic state, so two equal blobs mean the next step cannot
    // differ, and the first differing field names the bug.
    //
    //   scene     what the physics_world switches set up, with whole world
    //             ticks: --spawn-test N (the coincident spawn), --drop (a ball
    //             dropped onto a bay, --drop-prop beams a prop there too) or,
    //             with neither, the sector union of --drop-sector /
    //             --drop-second-sector; then --savestate-lead more world
    //             ticks.  Every event happens BEFORE the window.  A body born
    //             inside it is out of scope (a restore anchored before its
    //             birth tears its collision state down with the rest - the
    //             rollback parks and re-poses such a body, this mode would only
    //             report the difference), and so is --drop-move.  The
    //             exception (step S7): --second-move SECTOR TICK with TICK
    //             inside the window (needs --frames and --drop-second-sector)
    //             makes the union's pass there, its module bodies born in the
    //             window; the binding is then the session world's, the pass
    //             re-issued in the replay and the window's births destroyed at
    //             the restore and made again by the replayed scripts (the
    //             clients' recipe), and a round whose chained live world holds
    //             another body set than the record of its end has that world
    //             made again from T0 first (as the trafo rounds do always).
    //   straight  for t in [T0, T0+N), at the tick boundary: the blob B[t]
    //             (get_savestate: the whole world), its hash and size, the
    //             world hash H[t] (every field), the exact core dump E[t], the
    //             world's own counters (physics_world::step_state) and, with
    //             --frames, the script replay's bytes S[t]; then one step -
    //             world.pre_simulate() + bmmo::physics::step_physics (the
    //             primitive the headless client re-steps with), or with
    //             --frames one whole world.tick() in the straight pass and,
    //             in a round, the selected scripts' execute_frame + the same
    //             physics step (the client's own replay recipe).  B, E and S are kept for
    //             every --savestate-every K-th tick and every tick a round
    //             anchors at or ends on; H, the blob hash and the counters for
    //             every tick.
    //   rounds    R (anchor a, depth d) pairs from a seeded LCG - the depth
    //             uniform in [0, min(D, N-1)], then the anchor uniform over the
    //             ticks that leave room for it - CHAINED: the world is wherever
    //             the previous round left it, which is the live case (the world
    //             has moved on with another history).  A round puts the
    //             counters back, with --frames the scripts (before the blob, so
    //             the force blocks' controllers exist when the savestate orders
    //             them), then set_savestate(B[a], STRICT), and checks
    //               restore identity  a fresh capture equals B[a] byte for byte
    //               replay identity   after each of the d steps the blob equals
    //                                 B[t] (byte for byte where kept, by hash
    //                                 and size elsewhere), and at a+d the world
    //                                 hash, the exact core dump and (--frames)
    //                                 the script bytes equal the straight pass's.
    //             The first mismatch of a round prints
    //               FAIL round=r anchor=a depth=d tick=t: <what>
    //             (<what> is diff_savestate's line for a blob).  A restore that
    //             returns false ends the check: the world is not guaranteed
    //             consistent after it (physics_savestate.hpp).
    //   result    'summary: savestate-check ...' and 'PASS rounds=R failed=0'
    //             (exit 0) or 'FAIL rounds=R failed=F' (exit 3); with
    //             --stop-on-divergence the rounds stop at the first failure.  A
    //             window none of whose captures holds a contact point or a
    //             friction system proves nothing about the collision state:
    //             'vacuous scene', exit 2, before any round runs.
    //
    // --legacy-restore is the negative control: the anchor is put back the
    // old per-body way - the clock (set_clock_state), the manager's floats and
    // the RNG cursors, then every movable body through set_body_state
    // (wake_mode keep, no recheck) and set_core_extra, like restore_bodies -
    // which keeps the live world's collision bookkeeping and must FAIL on a
    // scene with contacts.
    //
    // --frames is the integration check for the mechanism levels.  Beyond the
    // blob, H and E it compares the script replay's bytes of the mechanism
    // scripts (script_replay::select at T0).  It does not restore, and so
    // assumes unchanged across the window, the CK state of every script
    // outside that selection (Gameplay_*, the checkpoints, the extras) and the
    // engine's own clock - neither does the session's rollback; a difference
    // they make shows up as a blob or script mismatch like any other.  Every
    // selected script active at the anchor is written back; one the frame
    // told to reset (script_replay::resets_pending) inside a round's window
    // cannot be reproduced by construction, so such rounds are flagged in the
    // verbose line and the count is in the summary (script_resets=).
    //
    // --savestate-trace PATH appends one line per straight tick and per round
    // and the summary: world hashes, blob sizes and counts only - never the
    // blob's hash, which carries the environment's address - so the files of
    // two builds or platforms can be diffed directly.  On the first failure
    // whose expected blob was kept, describe_savestate of both blobs goes to
    // PATH.round<r>.expected.txt / .actual.txt.

    struct savestate_round { int anchor = 0; int depth = 0; };   // window indices

    // Step RLC's review (--joint-shuffle): every constraint a joint block
    // holds, taken out by the block's own reset and made again from its own
    // image (the same fields), highest block id first - each lands last in
    // its cores' controller lists.  Returns how many were made again.
    int shuffle_joints(CKContext* context, const bmmo::game::script_replay_hooks& hooks) {
        using bmmo::game::script_replay;
        if (!context || !hooks.capture_joint || !hooks.restore_joint) return 0;
        std::vector<CK_ID> blocks;
        const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
        CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
        for (int i = 0; i < count; ++i) {
            auto* beh = CKBehavior::Cast(context->GetObject(ids[i]));
            if (beh && script_replay::is_joint_block(beh) && script_replay::joint_handle(beh)) blocks.push_back(ids[i]);
        }
        std::sort(blocks.rbegin(), blocks.rend());
        int made = 0;
        for (CK_ID id: blocks) {
            auto* beh = CKBehavior::Cast(context->GetObject(id));
            std::vector<uint8_t> image;
            std::array<int16_t, 2> order{-1, -1};
            if (!hooks.capture_joint(beh, image, order) || image.empty()) continue;
            beh->CallCallbackFunction(CKM_BEHAVIORRESET);
            if (hooks.restore_joint(beh, image)) ++made;
            else std::fprintf(stderr, "--joint-shuffle: block %u could not be made again\n", static_cast<unsigned>(id));
        }
        return made;
    }

    // Step B2SX (Q5): every delayed link of `graph`'s tree (initial delay > 1)
    // as "source.outN->destination.inM countdown C/I dest D", where D is
    // whether the destination input is active (a link that fired at the end
    // of the last frame has switched it on); `in_flight` counts the links
    // whose countdown is between 0 and the initial delay, `fired` those
    // whose destination input is on.
    std::string describe_delayed_links(CKBehavior* graph, int& in_flight, int& fired) {
        std::string text;
        if (!graph || graph->IsUsingFunction()) return text;
        const auto io_name = [](CKBehaviorIO* io, bool output) {
            CKBehavior* owner = io ? io->GetOwner() : nullptr;
            std::string name = owner && owner->GetName() ? owner->GetName() : "?";
            if (owner) {
                const int count = output ? owner->GetOutputCount() : owner->GetInputCount();
                for (int k = 0; k < count; ++k)
                    if ((output ? owner->GetOutput(k) : owner->GetInput(k)) == io)
                        return name + (output ? ".out" : ".in") + std::to_string(k);
            }
            return name;
        };
        for (int i = 0; i < graph->GetSubBehaviorLinkCount(); ++i) {
            CKBehaviorLink* link = graph->GetSubBehaviorLink(i);
            if (!link || link->GetInitialActivationDelay() <= 1) continue;
            const int initial = link->GetInitialActivationDelay(), live = link->GetActivationDelay();
            CKBehaviorIO* to = link->GetOutBehaviorIO();
            CKBehavior* owner = to ? to->GetOwner() : nullptr;
            bool active = false;
            if (owner)
                for (int k = 0; k < owner->GetInputCount(); ++k)
                    if (owner->GetInput(k) == to) active = owner->IsInputActive(k) != FALSE;
            if (live > 0 && live < initial) ++in_flight;
            if (active) ++fired;
            text += (text.empty() ? "" : "; ") + io_name(link->GetInBehaviorIO(), true) + "->" + io_name(to, false)
                  + " countdown " + std::to_string(live) + "/" + std::to_string(initial) + " dest " + (active ? "1" : "0");
        }
        for (int i = 0; i < graph->GetSubBehaviorCount(); ++i) {
            const std::string sub = describe_delayed_links(graph->GetSubBehavior(i), in_flight, fired);
            if (!sub.empty()) text += (text.empty() ? "" : "; ") + sub;
        }
        return text;
    }

    // Knuth's MMIX LCG; the high half per draw.  The same seed gives the same
    // schedule on every platform.
    std::vector<savestate_round> savestate_schedule(unsigned long long seed, int rounds, int max_depth, int window) {
        uint64_t state = static_cast<uint64_t>(seed);
        const auto next = [&state]() {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            return static_cast<uint32_t>(state >> 32);
        };
        const auto upto = [&next](int n) {   // [0, n]
            return n <= 0 ? 0 : static_cast<int>(next() % (static_cast<uint32_t>(n) + 1u));
        };
        static_cast<void>(next());   // mix the seed in before the first draw
        static_cast<void>(next());
        std::vector<savestate_round> out;
        const int deepest = std::max(0, std::min(max_depth, window - 1));
        for (int r = 0; r < rounds; ++r) {
            savestate_round entry;
            entry.depth = upto(deepest);
            entry.anchor = upto(window - 1 - entry.depth);
            out.push_back(entry);
        }
        return out;
    }

    // --legacy-restore: what the old per-body restore path wrote back.
    template <class T> bool same_bits(const T& a, const T& b) { return std::memcmp(&a, &b, sizeof(T)) == 0; }

    // The first field two world hashes differ in ("" when every field is the
    // same, floats compared bit for bit), with both values.
    std::string diff_world_hashes(const bmmo::physics::world_hash& expected, const bmmo::physics::world_hash& actual) {
        char text[320] = {};
        const auto u64 = [&text](const char* field, uint64_t a, uint64_t b) {
            if (a == b) return false;
            std::snprintf(text, sizeof(text), "%s %016llx vs %016llx", field, static_cast<unsigned long long>(a),
                          static_cast<unsigned long long>(b));
            return true;
        };
        const auto integer = [&text](const char* field, long long a, long long b) {
            if (a == b) return false;
            std::snprintf(text, sizeof(text), "%s %lld vs %lld", field, a, b);
            return true;
        };
        const auto f64 = [&text](const char* field, const double* a, const double* b, int n) {
            for (int k = 0; k < n; ++k) {
                if (same_bits(a[k], b[k])) continue;
                std::snprintf(text, sizeof(text), "%s[%d] %a vs %a", field, k, a[k], b[k]);
                return true;
            }
            return false;
        };
        const auto f32 = [&text](const char* field, const float* a, const float* b, int n) {
            for (int k = 0; k < n; ++k) {
                if (same_bits(a[k], b[k])) continue;
                std::snprintf(text, sizeof(text), "%s[%d] %a vs %a", field, k, static_cast<double>(a[k]),
                              static_cast<double>(b[k]));
                return true;
            }
            return false;
        };
        // The named fields first, the folded hashes last: a field says more.
        if (integer("cores", expected.cores, actual.cores)) return text;
        if (std::strncmp(expected.probe_name, actual.probe_name, sizeof(expected.probe_name)) != 0) {
            std::snprintf(text, sizeof(text), "probe_name %.*s vs %.*s", static_cast<int>(sizeof(expected.probe_name)),
                          expected.probe_name, static_cast<int>(sizeof(actual.probe_name)), actual.probe_name);
            return text;
        }
        if (f64("probe_position", expected.probe_position, actual.probe_position, 3)
                || f32("probe_speed", expected.probe_speed, actual.probe_speed, 3)
                || f32("probe_rot_speed", expected.probe_rot_speed, actual.probe_rot_speed, 3)
                || f64("ivp_time", &expected.ivp_time, &actual.ivp_time, 1)
                || integer("ivp_seed", expected.ivp_seed, actual.ivp_seed)
                || integer("next_movement_check", expected.next_movement_check, actual.next_movement_check)
                || f64("time_of_last_psi", &expected.time_of_last_psi, &actual.time_of_last_psi, 1)
                || f64("time_of_next_psi", &expected.time_of_next_psi, &actual.time_of_next_psi, 1)
                || f32("delta_time_ms", &expected.delta_time_ms, &actual.delta_time_ms, 1)
                || f32("physics_delta_time", &expected.physics_delta_time, &actual.physics_delta_time, 1)
                || f32("time_factor", &expected.time_factor, &actual.time_factor, 1)
                || u64("surfaces", expected.surfaces, actual.surfaces) || u64("pose", expected.pose, actual.pose)
                || u64("hash", expected.hash, actual.hash))
            return text;
        return {};
    }

    // Which named fields of which cores differ between two
    // describe_cores_exact dumps ("name field=... field=..." per line, sorted
    // by name): the token-wise compare of session_client.cpp's
    // diff_core_dumps, capped for a one-line report.
    std::string diff_exact_dumps(const std::string& expected, const std::string& actual) {
        const auto split = [](const std::string& text) {
            std::map<std::string, std::vector<std::string>> out;
            size_t pos = 0;
            while (pos < text.size()) {
                const size_t end = text.find('\n', pos);
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
        const auto a = split(expected), b = split(actual);
        std::string out;
        int changed = 0;
        const auto add = [&out, &changed](const std::string& body) {
            if (++changed <= 4) out += " " + (body.size() > 400 ? body.substr(0, 400) + "..." : body) + ";";
        };
        static const std::string missing = "<none>";
        for (const auto& [name, tokens]: a) {
            const auto other = b.find(name);
            if (other == b.end()) { add(name + " GONE"); continue; }
            std::string fields;
            const size_t n = std::max(tokens.size(), other->second.size());
            for (size_t i = 1; i < n; ++i) {
                const std::string& x = i < tokens.size() ? tokens[i] : missing;
                const std::string& y = i < other->second.size() ? other->second[i] : missing;
                if (x != y) fields += " " + x + "->" + y;
            }
            if (!fields.empty()) add(name + fields);
        }
        for (const auto& [name, tokens]: b)
            if (a.find(name) == a.end()) add(name + " NEW");
        if (changed > 4) out += " (+" + std::to_string(changed - 4) + " more bodies)";
        if (out.empty()) out = " the dumps differ in text but in no body's fields";
        return out;
    }

    std::string blob_difference(const std::vector<uint8_t>& expected, const std::vector<uint8_t>& actual) {
        std::string line = bmmo::physics::diff_savestate(expected.data(), expected.size(), actual.data(), actual.size());
        if (line.empty())
            line = "the blobs differ (" + std::to_string(expected.size()) + " vs " + std::to_string(actual.size())
                 + " bytes) but diff_savestate names no field";
        return line;
    }

    // --savestate-trace: appended to, closed with the check.
    class trace_file {
    public:
        explicit trace_file(const std::string& path) {
            if (!path.empty()) file_ = std::fopen(path.c_str(), "a");
        }
        ~trace_file() { if (file_) std::fclose(file_); }
        trace_file(const trace_file&) = delete;
        trace_file& operator=(const trace_file&) = delete;
        bool open() const { return file_ != nullptr; }
        void put(const char* line) {
            if (!file_) return;
            std::fputs(line, file_);
            std::fputc('\n', file_);
        }
    private:
        std::FILE* file_ = nullptr;
    };

    bool write_text_file(const std::string& path, const std::string& text) {
        std::FILE* file = std::fopen(path.c_str(), "wb");
        if (!file) return false;
        const bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
        std::fclose(file);
        return ok;
    }

    // What the straight pass keeps of one tick boundary.
    struct straight_tick {
        uint64_t blob_hash = 0;
        size_t blob_size = 0;
        bool kept = false;
        std::vector<uint8_t> blob;       // kept ticks
        std::string exact;               // kept ticks: describe_cores_exact
        std::vector<uint8_t> scripts;    // kept ticks, --frames: script_replay::capture
        std::string membership;          // kept ticks, --frames: each selected script's owner in the next frame (step RLM)
        std::vector<std::string> bodies; // a pass inside the window (step S7): the movable bodies, sorted
        std::vector<uint32_t> cells;     // the written-cell columns (step RK2's review): per column rows, then the values
        // Close-out step FX2: before the step at this tick, the activations
        // pending for its frame (Gameplay_SectorManager and the selected
        // scripts session_script_activations names: id, reset) and the
        // manager's IngameParameter cells [0][1] / [0][2] - what a re-made
        // live world re-issues where a script outside the selection asked.
        std::vector<std::pair<CK_ID, bool>> pending;
        int manager_cells[2] = {0, 0};
        bool have_manager_cells = false;
        bmmo::physics::world_hash hash;
        bmmo::sim::physics_world::step_state counters;
    };

    // --savestate-diff A B and --savestate-describe FILE: savestate blobs
    // written by BallanceMMOSessionClient --dump-savestate.  The diff is
    // diff_savestate's first differing field (the environment identity
    // excepted: two processes never share one); describe prints the whole
    // record as text.  Needs no game: the blob is self-contained.
    int run_savestate_files(const arguments& args) {
        const auto read_file = [](const std::string& path, std::vector<uint8_t>& out) {
            out.clear();
            FILE* f = std::fopen(path.c_str(), "rb");
            if (!f) return false;
            uint8_t buffer[65536];
            size_t n;
            while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) out.insert(out.end(), buffer, buffer + n);
            std::fclose(f);
            return true;
        };
        std::string error;
        if (!args.savestate_describe.empty()) {
            std::vector<uint8_t> blob;
            if (!read_file(args.savestate_describe, blob)) {
                std::fprintf(stderr, "cannot read %s\n", args.savestate_describe.c_str());
                return 1;
            }
            if (!bmmo::physics::check_savestate(blob.data(), blob.size(), error)) {
                std::fprintf(stderr, "%s: %s\n", args.savestate_describe.c_str(), error.c_str());
                return 1;
            }
            std::printf("%s", bmmo::physics::describe_savestate(blob.data(), blob.size()).c_str());
            return 0;
        }
        std::vector<uint8_t> blobs[2];
        for (int k = 0; k < 2; ++k) {
            if (!read_file(args.savestate_diff[k], blobs[k])) {
                std::fprintf(stderr, "cannot read %s\n", args.savestate_diff[k].c_str());
                return 1;
            }
            if (!bmmo::physics::check_savestate(blobs[k].data(), blobs[k].size(), error)) {
                std::fprintf(stderr, "%s: %s\n", args.savestate_diff[k].c_str(), error.c_str());
                return 1;
            }
        }
        const std::string difference = bmmo::physics::diff_savestate(blobs[0].data(), blobs[0].size(), blobs[1].data(),
                                                                     blobs[1].size(), args.ignore_ov, args.max_diff, args.ignore_order);
        if (difference.empty()) {
            std::printf("identical (%zu bytes)\n", blobs[0].size());
            return 0;
        }
        std::printf("differ: %s\n", difference.c_str());
        return 3;
    }

    int run_savestate_check(const arguments& args) {
        const int window = args.ticks_explicit ? args.ticks : 200;
        if (window < 1 || args.savestate_rounds < 0 || args.savestate_max_depth < 0 || args.savestate_every < 1
                || args.savestate_lead < 0) {
            std::fprintf(stderr, "--savestate-check needs --ticks >= 1, --rounds >= 0, --max-depth >= 0, "
                                 "--savestate-every >= 1 and --savestate-lead >= 0\n");
            return 2;
        }
        if (args.drop_move_sector > 0) {
            std::fprintf(stderr, "--drop-move is an event inside the window; --savestate-check sets every event up "
                                 "before it\n");
            return 2;
        }
        const bool frames = args.savestate_frames;
        const bool legacy = args.legacy_restore;
        const bool drop = !args.drop_entity.empty();
        const bool spawn = !drop && args.spawn_test > 0;

        // ---- the scene, with whole world ticks
        auto world = create_session_world(args, spawn ? args.spawn_impulse : 0.0f);
        if (!world) return 1;
        open_globals_trace(args, *world);   // step K0: the scene's ticks (drop, sector)
        const globals_trace_guard globals_guard;   // step K0: closes on a failed scene's return
        std::string scene;
        if (drop) {
            if (const int code = setup_drop_scene(args, *world)) return code;
            scene = "drop:" + args.drop_entity + ":" + std::to_string(args.drop_ball);
        } else if (spawn) {
            std::vector<uint32_t> ids;
            if (const int code = setup_spawn_scene(args, *world, ids)) return code;
            scene = "spawn-test:" + std::to_string(ids.size());
        } else {
            if (const int code = setup_sector_scene(args, *world)) return code;
            scene = "sector:" + std::to_string(args.drop_sector);
            if (args.drop_second_sector > 0) scene += "+" + std::to_string(args.drop_second_sector);
        }
        close_globals_trace();   // step K0
        {
            // The lead through scene_tick (step S7): a --second-move due in
            // it is made like the scene's own.
            std::string lead_error;
            for (int i = 0; i < args.savestate_lead; ++i)
                if (!scene_tick(args, *world, lead_error)) {
                    std::fprintf(stderr, "tick failed: %s\n", lead_error.c_str());
                    return 1;
                }
        }
        scene = "level" + std::to_string(world->level()) + ":" + scene;
        CKIpionManager* physics = world->physics();
        CKContext* context = world->engine().context();
        if (!physics || !physics->GetEnvironment()) {
            std::fprintf(stderr, "savestate-check: no physics environment after the scene\n");
            return 1;
        }

        // ---- the passes inside the window (sector placement design 7.2 V9,
        // step S7): the --second-move ticks in [T0, T0+N) are reported before
        // that world tick of the straight pass, as the scene reports the
        // earlier ones.  The union's pass then runs inside the window, so the
        // window has births (the sector's module bodies) and a rewind across
        // it is the clients' recipe: the binding is the session world's
        // (bind_session_world), the pass recorded in the tick's pass slot of
        // host support's manager_activations and re-issued before the
        // replayed tick (replay_activation, the manager in the selection,
        // every selected script written back), a window-born body destroyed
        // at the restore and put back where its Physicalize found it
        // (put_back_birth), and made again by the replayed module script.
        const uint32_t window_t0 = world->tick_index();
        std::map<uint32_t, std::vector<int>> window_moves;   // world tick -> sectors player 2 reports before it
        for (const arguments::second_move& m: args.second_moves) {
            if (m.tick < static_cast<long long>(window_t0)) continue;   // made by the scene or the lead
            if (m.tick >= static_cast<long long>(window_t0) + window) {
                std::fprintf(stderr, "--second-move %d %lld is after the window (%u..%u)\n", m.sector, m.tick, window_t0,
                             window_t0 + static_cast<uint32_t>(window) - 1u);
                return 2;
            }
            window_moves[static_cast<uint32_t>(m.tick)].push_back(m.sector);
        }
        // Step B2SW (record-format Q5 / A23): --session-recipe binds the
        // clients' recipe - the session world's binding, every frame's
        // pending activations recorded and re-issued, the window's births
        // destroyed at a restore and made again by the replay, every selected
        // script written back - without a --second-move too: the window that
        // starts at the session start holds Init Ingame's sector-1 activation
        // and its births, which a client's history holds with this recipe.
        // Step F0: the clients' recipe by default where the scene can bind it.
        const bool recipe_possible = frames && !legacy && args.trafo.players.empty();
        bool session_recipe = args.session_recipe < 0 ? recipe_possible : args.session_recipe != 0;
        if (args.session_recipe < 0 && session_recipe
                && !bmmo::game::find_root_script(context, "Gameplay_SectorManager")) {
            session_recipe = false;   // a default the level cannot bind falls back, said here
            std::printf("savestate-check: recipe v14 (default: the level has no Gameplay_SectorManager to bind the "
                        "clients' recipe)\n");
        }
        else
            std::printf("savestate-check: recipe %s (%s)\n", session_recipe ? "session (the clients')" : "v14",
                        args.session_recipe > 0 ? "--session-recipe" : args.session_recipe == 0 ? "--v14"
                        : session_recipe ? "default" : "default: the scene cannot bind the clients' recipe (no --frames, "
                                                       "--legacy-restore or trafo inputs)");
        const bool passes = !window_moves.empty() || session_recipe;
        if (passes && (!frames || legacy || !args.trafo.players.empty())) {
            std::fprintf(stderr, "savestate-check: a --second-move inside the window needs --frames (the pass runs the module"
                                 " scripts), and is not built with --legacy-restore or trafo inputs\n");
            return 2;
        }
        CKBehavior* sector_manager = passes ? bmmo::game::find_root_script(context, "Gameplay_SectorManager") : nullptr;
        if (passes && !sector_manager) {
            std::fprintf(stderr, "savestate-check: no Gameplay_SectorManager for the passes inside the window\n");
            return 1;
        }
        const CK_ID manager_id = sector_manager ? sector_manager->GetID() : 0;
        bmmo::game::manager_activations activations;
        bmmo::session::local_birth_log pass_births;
        // The same births in the order they were made (record tick, name):
        // the live world's re-make takes out every one after T0, fixed bodies
        // included, which the movable-body names do not list.
        std::vector<std::pair<uint32_t, std::string>> pass_birth_order;
        const auto has_body = [&](const std::string& name) {
            auto* entity = CK3dEntity::Cast(context->GetObjectByNameAndParentClass(const_cast<CKSTRING>(name.c_str()),
                                                                                   CKCID_3DENTITY, nullptr));
            PhysicsObject* object = entity ? physics->GetPhysicsObject(entity) : nullptr;
            return object && object->m_RealObject;
        };
        std::map<std::string, uint32_t> pass_generations;
        std::set<std::string> ball_names;
        for (const auto& row: world->ball_rows()) ball_names.insert(row.name);
        const auto is_ball = [ball_names](const std::string& name) {
            return name.find("_BMMO_") != std::string::npos || ball_names.count(name) != 0;
        };
        const auto moves_before = [&](uint32_t t) {   // player 2's reports due before world tick t
            const auto due = window_moves.find(t);
            if (due == window_moves.end()) return;
            for (int sector: due->second) report_sector(*world, 2, sector);
        };
        size_t pass_birth_count = 0, passes_made = 0;
        int inert_rounds = 0;   // --savestate-inert-from-birth rounds compared from a fixed birth on
        uint64_t pass_reissued = 0, pass_recreated = 0, pass_recreated_exact = 0, pass_recreate_failures = 0,
                 pass_missing = 0;
        int pass_remakes = 0, pass_rounds_crossing = 0;
        std::set<uint32_t> pass_ticks;   // replayed-tick keys of the passes (t+1 of the straight step at t)

        // ---- the trafo sets (S6, trafo-replication design 7.2): built after
        // the scene, their inputs inside the window.  An input's TICK is a
        // window offset i: it is applied before the straight pass's step i -
        // the step a rewind re-runs as tick t0 + i + 1, which is the key the
        // input log keeps it under - and the trafo frame runs inside the
        // world's frame (physics_world's frame hook), after the union's pass.
        // The window then has births and deaths of the pieces, so the
        // binding is the session world's (bind_session_world): a window-born
        // piece is destroyed at the restore and made again by the replayed
        // explosion, a piece the window's reset took is revived at the
        // restore, from the births this harness logs (the event log's
        // recipes) - the hosts' path.
        std::unique_ptr<bmmo::sim::trafo_tool> trafo;
        if (!args.trafo.players.empty()) {
            if (!frames) {
                std::fprintf(stderr, "savestate-check: trafo inputs need --frames (the explosions are scripts)\n");
                return 2;
            }
            bmmo::sim::trafo_tool_options o = args.trafo;
            o.anchor = -1;
            for (auto& in: o.inputs) in.tick = static_cast<long long>(world->tick_index()) + in.tick + 1;
            trafo = std::make_unique<bmmo::sim::trafo_tool>(o, physics, context);
            if (!trafo->anchor(world->tick_index())) return 1;
            bmmo::sim::trafo_tool* tool = trafo.get();
            world->set_frame_hook([tool]() { tool->run_frame(); });
            bmmo::physics::drain_event_log(physics);   // the window's births only
        }
        bmmo::session::local_birth_log trafo_births;
        std::map<std::string, uint32_t> trafo_generations;
        size_t trafo_birth_count = 0;
        std::set<uint32_t> trafo_birth_ticks, trafo_death_ticks;   // record ticks a piece first / no longer has
        int trafo_rounds_births = 0, trafo_rounds_deaths = 0;
        uint64_t trafo_recreated = 0, trafo_recreated_exact = 0, trafo_missing = 0;
        // Gap L, L1 (step EL6): the lives alive at a round's anchor that end
        // inside its window, revived by restore_lives' halves or not; rounds
        // with any, and those whose every one was revived.
        uint64_t trafo_lives_ended = 0, trafo_lives_revived = 0, trafo_lives_unrevivable = 0;
        int trafo_rounds_lives = 0, trafo_rounds_lives_revived = 0;

        // ---- the script replay (--frames): the mechanism scripts and the
        // hooks that carry the force blocks' controllers, as the headless
        // client installs them.
        bmmo::game::script_replay::selection scripts;
        bmmo::game::script_replay_hooks hooks;
        bmmo::game::script_replay::joint_order_off() = args.joint_order_off;   // step RLC's review: the control
        if (frames) {
            scripts = bmmo::game::script_replay::select(context);
            hooks = bmmo::game::force_hooks_for(bmmo::physics::physics_direct(physics));   // step HU1: one copy (game/host_backend.hpp)
            const auto st = bmmo::game::script_replay::describe(context, scripts);
            std::printf("savestate-check: script replay: %d scripts (%d active), %d blocks, %d parameters, "
                        "%d force blocks, %d opaque\n", st.scripts, st.active, st.blocks, st.parameters,
                        st.force_blocks, st.opaque);
        }

        // ---- the schedule first: it decides which blobs are kept
        // Step B2SX (Q5): --savestate-rounds names the rounds instead (anchor
        // ticks, so window index = T - t0), each with its follow.
        std::vector<savestate_round> schedule;
        std::vector<int> follows;
        if (args.savestate_explicit.empty()) {
            schedule = savestate_schedule(args.savestate_seed, args.savestate_rounds, args.savestate_max_depth, window);
            follows.assign(schedule.size(), 0);
        } else {
            const int base = static_cast<int>(world->tick_index());
            for (const auto& [tick, depth, follow]: args.savestate_explicit) {
                savestate_round entry;
                entry.anchor = tick - base;
                entry.depth = depth;
                if (entry.anchor < 0 || entry.anchor + depth + follow >= window) {
                    std::fprintf(stderr, "savestate-check: --savestate-rounds %d:%d:%d is outside the window %d..%d\n", tick,
                                 depth, follow, base, base + window - 1);
                    return 2;
                }
                schedule.push_back(entry);
                follows.push_back(follow);
            }
            if (!frames) {
                std::fprintf(stderr, "savestate-check: --savestate-rounds' follow steps whole frames: it needs --frames\n");
                return 2;
            }
        }
        std::vector<char> keep(static_cast<size_t>(window), 0);
        for (int i = 0; i < window; i += args.savestate_every) keep[static_cast<size_t>(i)] = 1;
        for (size_t r = 0; r < schedule.size(); ++r) {
            const auto& entry = schedule[r];
            keep[static_cast<size_t>(entry.anchor)] = 1;
            for (int k = 0; k <= follows[r]; ++k) keep[static_cast<size_t>(entry.anchor + entry.depth + k)] = 1;
        }
        int kept = 0;
        for (char k: keep) kept += k ? 1 : 0;

        const uint32_t t0 = world->tick_index();
        const char* mode = frames ? "frames" : "physics";
        const char* restore = legacy ? "legacy" : "savestate";
        trace_file trace(args.savestate_trace);
        if (!args.savestate_trace.empty() && !trace.open()) {
            std::fprintf(stderr, "cannot open --savestate-trace %s\n", args.savestate_trace.c_str());
            return 1;
        }
        {
            char line[512];
            std::snprintf(line, sizeof(line),
                "savestate-check scene=%s mode=%s restore=%s window=%u..%u ticks=%d rounds=%zu max_depth=%d seed=%llu"
                " every=%d kept=%d lead=%d", scene.c_str(), mode, restore, t0, t0 + static_cast<uint32_t>(window) - 1u,
                window, schedule.size(), args.savestate_max_depth, args.savestate_seed, args.savestate_every, kept,
                args.savestate_lead);
            std::printf("%s\n", line);
            trace.put(line);
        }

        // The straight pass steps whole engine frames (--frames) or physics
        // only.  A replayed tick steps the way the headless client re-steps
        // (session_client.cpp, rollback_world::pre_step + step): the selected
        // scripts' frame through script_replay::execute_frame, the world's
        // PreSimulate pass, one physics step - never a whole frame, whose
        // other scripts (Gameplay_*, the checkpoints, the extras) are not
        // rewound and would run on from where the live run left them.  So a
        // --frames mismatch is a defect of the recipe the client uses, not of
        // the harness.
        // One physics world, bound for the rollback engine the way the headless
        // client binds it (session/rollback_binding.hpp): the straight
        // pass records every tick through the engine, and every round is the
        // engine's own rewind_to - the restore (the savestate, or the per-body
        // path as the --legacy-restore negative control), the mechanism
        // scripts' frame and the physics step are the client's code paths, not
        // a copy of them.  The straight pass steps whole engine frames
        // (--frames) or physics only; a replayed tick is what the binding
        // steps: the selected scripts' frame, the world's PreSimulate pass,
        // one physics step - never a whole frame, whose other scripts
        // (Gameplay_*, the checkpoints) are not rewound.
        const auto step = [&](std::string& step_error) -> bool {
            if (frames) return world->tick(step_error);
            world->pre_simulate();
            return bmmo::physics::step_physics(physics, bmmo::sim::kFixedDeltaMs, step_error);
        };
        std::vector<CK_ID> scripts_replayed;
        // Step RO1: the order the straight pass's frames ran the selected
        // scripts in, under the tick a rewind replays each as (t+1), for the
        // binding (the clients' recording); the re-made live worlds do not
        // record (their rounds compare with the straight pass).  And the
        // window's fixed births the restore destroyed for the replayed
        // scripts to make again: back after their birth tick, or missing.
        bmmo::session::live_script_order live_order;
        uint64_t live_orders_applied = 0, fixed_births_back = 0, fixed_births_missing = 0;
        uint32_t probe_tick = 0;                            // a round's first replayed tick: the restored world
        std::vector<uint8_t> restored_blob;                 // ... captured there, before any step
        std::vector<std::vector<uint8_t>> replayed_blobs;   // the blob after each replayed step
        std::string restore_error, capture_error, engine_log;
        bool restore_refused = false;
        bmmo::physics::physics_direct backend(physics);
        bmmo::session::rollback_binding_options binding_options;
        binding_options.delta_ms = bmmo::sim::kFixedDeltaMs;
        binding_options.savestate = !legacy;
        binding_options.savestate_flags = bmmo::physics::SAVESTATE_STRICT;
        binding_options.physics_globals = true;   // C3 (step RK1): the clients' rule, as bind_session_world sets it
        // Every selected script whose owner the behaviour manager holds in its
        // pending table or list (state no restore writes back): S2's
        // "owners held live" reading, and step FX1's after the restore.
        const auto owners_held = [&]() {
            CKBehaviorManager* bm = context->GetBehaviorManager();
            std::string held;
            for (CK_ID id: scripts.scripts) {
                auto* beh = CKBehavior::Cast(context->GetObject(id));
                CKBeObject* owner = beh ? beh->GetOwner() : nullptr;
                if (!owner || !bm) continue;
                int pending = 0;
                for (auto it = bm->m_BeObjectNextFrame.Begin(); it != bm->m_BeObjectNextFrame.End(); ++it)
                    if (it.GetKey() == owner->GetID()) { pending = *it; break; }
                bool listed = false;
                for (int i = 0; i < bm->m_BeObjects.Size(); ++i)
                    listed = listed || bm->m_BeObjects[i] == static_cast<CKObject*>(owner);
                if (!pending && !listed) continue;
                held += std::string(held.empty() ? "" : ", ") + (beh->GetName() ? beh->GetName() : "?") + " pending="
                      + std::to_string(pending) + " listed=" + (listed ? "1" : "0") + " flags=0x"
                      + [&] { char b[16]; std::snprintf(b, sizeof(b), "%08lx", static_cast<unsigned long>(beh->GetFlags())); return std::string(b); }();
            }
            return held.empty() ? std::string("none") : held;
        };
        binding_options.before_replayed_tick = [&](uint32_t tick) {
            if (args.savestate_no_ledger) bmmo::game::script_replay::registrations().clear();   // step RLM's control (FX1: its replayed adds too)
            if (tick == probe_tick) {
                std::string probe_error;
                if (!bmmo::physics::get_savestate(physics, restored_blob, probe_error)) capture_error = probe_error;
                // Step FX1: the table as the restore left it (the earlier
                // replays' pending adds taken back, its own registrations made).
                if (args.verbose && frames)
                    std::printf("  after the restore, before tick %u: owners held live: %s\n", tick, owners_held().c_str());
            }
            if (trafo) trafo->apply_inputs(tick, true);
            // Step S7: the server world's own pre-script work of a tick
            // (physics_world::tick): the proximity union's probe frames go
            // to the nearest player ball and the ball identity copies follow
            // it.  The binding steps the selected scripts, never tick(), so
            // without this a replayed proximity block measured the frame where
            // the live world's last tick left it (its "Current Distance"
            // differed in 47 of 50 rounds with no pass in the window).
            // Step G2: under WORLD_PROXIMITY_UNION the union's rule from the
            // replayed tick's candidates (the restored balls), the hold put
            // back with the step state (restore_step_state).
            if (frames) {
                if (world->proximity_union_on()) world->place_probes();
                else world->place_proximity_legacy();   // step G1: game/proximity_union.hpp's bit-clear path
            }
        };
        binding_options.on_savestate_restored = [&](bool ok, const std::string& error, const bmmo::session::savestate_outcome&) {
            if (!ok) {
                restore_refused = true;
                restore_error = error;
                return;
            }
            if (args.verbose)
                for (const auto& warning: backend.last_report.warnings) std::printf("  restore warning: %s\n", warning.c_str());
        };
        binding_options.log = [&](const std::string& text) {
            engine_log = text;
            if (args.verbose) std::printf("  engine: %s\n", text.c_str());
        };
        bmmo::session::script_binding_options script_options;
        script_options.context = context;
        script_options.scripts = &scripts;
        script_options.hooks = &hooks;
        script_options.scripts_replayed = &scripts_replayed;
        script_options.delta_ms = bmmo::sim::kFixedDeltaMs;
        if (trafo) script_options.trafo = &trafo->registry().part();
        script_options.replay_activation = passes;   // step S7: every selected script written back
        script_options.live_order = &live_order;       // step RO1
        script_options.live_orders_applied = &live_orders_applied;
        script_options.resize_strings = true;
        script_options.live_membership = true;   // step B2SW (G1, F): the core hosts' rule
        script_options.written_cells = passes;   // step RK2 (record-format Q4): the core hosts' written-cell section
        // Step RK2's review: the binding's columns, for the round check below
        // (every row of each is set to a sentinel before the rewind; at the
        // round's end it must hold the straight pass's values at e - one
        // world restoring itself, not a compare between worlds).
        std::vector<bmmo::game::written_column> written_cols;
        script_options.written_columns = &written_cols;
        const auto read_written = [context, &written_cols]() {
            std::vector<uint32_t> out;
            for (const bmmo::game::written_column& w: written_cols) {
                auto* arr = CKDataArray::Cast(context->GetObject(w.array));
                const int rows = arr && w.column >= 0 && w.column < arr->GetColumnCount() ? arr->GetRowCount() : 0;
                out.push_back(static_cast<uint32_t>(rows));
                for (int row = 0; row < rows; ++row) {
                    uint32_t value = 0;
                    arr->GetElementValue(row, w.column, &value);
                    out.push_back(value);
                }
            }
            return out;
        };
        uint64_t cells_sentinel_rows = 0;
        int cells_checked_rounds = 0, cells_differ_rounds = 0;
        // Step RLQ (gap Q's hold, rollback-engine-gaps design 2.2.4): the
        // clients' PreSimulate queue binding.  With the clients' recipe
        // (replay_activation) the queue section and the hold, as both core
        // hosts bind them; on the v14 recipe the hold alone.  Every entry of
        // a script the replay does not execute (Gameplay_Ingame's and
        // Sound_Manager's for the level start's retail ball, which the world
        // parks at once, so they stay queued) is then live state set aside
        // for the replay instead of a whole-queue compare that refuses.
        script_options.callback_queue = !args.savestate_no_hold;
        script_options.hold_outside = !args.savestate_no_hold;
        std::vector<CK_ID> script_activations;
        if (passes) script_activations = bmmo::game::session_script_activations(context, scripts);
        bmmo::session::rollback_world binding;
        if (passes) {
            // Step S7: the session world's binding, as the headless client's
            // (and the late-sector check's) - the activation re-issued before
            // each replayed tick, the window's births destroyed at the restore.
            bmmo::session::core_world_hooks core;
            core.replay_mechanisms = [physics]() { return bmmo::physics::movable_body_names(physics); };
            core.ball = is_ball;
            bmmo::session::session_world_options so;
            so.binding = binding_options;
            so.before_replayed_tick = binding_options.before_replayed_tick;
            so.replay_activation = [&](uint32_t tick) {
                if (activations.replay(context, manager_id, tick, true)) ++pass_reissued;
            };
            so.births = &pass_births;
            so.put_back = [context](const std::string& entity, const bmmo::session::local_birth& birth) {
                bmmo::game::put_back_birth(context, entity, birth);
            };
            so.fixed_birth_remade = [&](const std::string&, uint32_t, bool back) {   // step RO1
                ++(back ? fixed_births_back : fixed_births_missing);
            };
            so.bind_scripts = [&backend, &script_options](bmmo::session::rollback_world& w) {
                bmmo::session::bind_script_replay(w, backend, script_options);
            };
            binding = bmmo::session::bind_session_world(backend, core, so);
        } else if (trafo) {
            std::set<std::string> pieces;
            for (const auto& set: trafo->registry().sets())
                for (const auto& ts: set.types)
                    for (CK_ID id: ts.pieces)
                        if (CKObject* o = context->GetObject(id)) pieces.insert(o->GetName());
            bmmo::session::core_world_hooks core;
            core.replay_mechanisms = [physics]() { return bmmo::physics::movable_body_names(physics); };
            bmmo::session::session_world_options so;
            so.binding = binding_options;
            // bind_session_world composes before_replayed_tick from the
            // core's births, the activation and the host's own: the probe
            // and the inputs are the host's.
            so.before_replayed_tick = binding_options.before_replayed_tick;
            so.births = &trafo_births;
            so.revivable = [pieces](const std::string& entity) { return pieces.count(entity) != 0; };
            so.put_back = [context](const std::string& entity, const bmmo::session::local_birth& birth) {
                bmmo::game::put_back_birth(context, entity, birth);
            };
            so.bind_scripts = [&backend, &script_options](bmmo::session::rollback_world& w) {
                bmmo::session::bind_script_replay(w, backend, script_options);
            };
            binding = bmmo::session::bind_session_world(backend, core, so);
        } else {
            binding = bmmo::session::bind_rollback_world(backend, binding_options);
            if (frames) bmmo::session::bind_script_replay(binding, backend, script_options);
        }
        {
            // The world's own PreSimulate pass before the physics step (the
            // physics-only recipe above), and the blob after it.
            const auto inner_step = binding.step;
            binding.step = [&, inner_step]() {
                world->pre_simulate();
                const bool ok = inner_step();
                std::vector<uint8_t> after;
                std::string after_error;
                if (!bmmo::physics::get_savestate(physics, after, after_error)) capture_error = after_error;
                replayed_blobs.push_back(std::move(after));
                return ok;
            };
        }
        bmmo::session::rollback_engine engine;
        engine.thresholds().history_ticks = static_cast<size_t>(window) + 2u;
        engine.thresholds().max_resim_ticks = static_cast<uint32_t>(window) + 2u;
        engine.set_exact_restore(!legacy);
        engine.set_replay_scripts(frames);
        // A replay re-records every tick it steps, so one round's result would
        // become the next round's records: every round starts from the engine
        // as the straight pass left it.
        bmmo::session::rollback_engine pristine;
        bmmo::session::rollback_tracked tracked;
        for (const auto& name: bmmo::physics::movable_body_names(physics)) tracked.mechanisms.push_back(name);
        const std::map<std::string, bmmo::session::input_frame> no_inputs;
        const auto no_input = [](const std::string&, uint32_t, bmmo::session::input_frame&) { return false; };

        // Close-out step FX2: a re-made live world (below) restores t0 and
        // steps whole frames to e, but the scripts outside the selection are
        // not rewound - they stand wherever the straight pass and the earlier
        // rounds left them - so their one-shot work of the window's frames
        // never recurs there: Init Ingame's Execute Script of
        // Gameplay_SectorManager at the session start (sector 1, the births
        // at 7) and its Set Physics Globals of frame 1 (the A18 write).  The
        // straight pass notes, before each step, the activations its frame
        // consumes (the manager's, with its two cells, and the selected
        // scripts'); the re-make re-issues each one its own world lacks
        // before the same step - only those, so an activation the selection
        // itself makes again is not issued twice (CKScene::Activate counts
        // in m_BeObjectNextFrame, step FX1) - and puts back the globals the
        // straight step ran with (record t+1's, rollback.hpp's C3 rule 2.3
        // step 3).  A union pass is made by the re-stepped frame itself
        // (moves_before, update_sectors), never re-issued here.
        std::vector<CK_ID> pending_watch;
        if (passes) {
            if (manager_id) pending_watch.push_back(manager_id);
            for (CK_ID id: script_activations)
                if (id != manager_id) pending_watch.push_back(id);
        }
        const auto capture_pending = [&](straight_tick& rec) {
            rec.pending.clear();
            for (CK_ID id: pending_watch) {
                auto* beh = CKBehavior::Cast(context->GetObject(id));
                if (beh && (beh->GetFlags() & CKBEHAVIOR_ACTIVATENEXTFRAME))
                    rec.pending.emplace_back(id, (beh->GetFlags() & CKBEHAVIOR_RESETNEXTFRAME) != 0);
            }
            rec.have_manager_cells = false;
            if (CKDataArray* parameters = bmmo::game::session_data_array(context, "IngameParameter"))
                rec.have_manager_cells = parameters->GetElementValue(0, 1, &rec.manager_cells[0])
                                      && parameters->GetElementValue(0, 2, &rec.manager_cells[1]);
        };
        uint64_t remake_reissued = 0, remake_extra_pending = 0, remake_globals_written = 0, remake_steps = 0;
        const auto reissue_pending = [&](const straight_tick& rec, uint32_t t) {
            CKScene* scene = context->GetCurrentScene();
            for (const auto& [id, reset]: rec.pending) {
                auto* beh = CKBehavior::Cast(context->GetObject(id));
                if (!beh || !scene || (beh->GetFlags() & CKBEHAVIOR_ACTIVATENEXTFRAME)) continue;
                if (id == manager_id && rec.have_manager_cells)
                    if (CKDataArray* parameters = bmmo::game::session_data_array(context, "IngameParameter")) {
                        parameters->SetElementValue(0, 1, const_cast<int*>(&rec.manager_cells[0]), sizeof(int));
                        parameters->SetElementValue(0, 2, const_cast<int*>(&rec.manager_cells[1]), sizeof(int));
                    }
                scene->Activate(beh, reset ? TRUE : FALSE);
                ++remake_reissued;
                if (args.verbose)
                    std::printf("  re-made live world: before the step at %u re-issued %s's activation%s\n", t,
                                beh->GetName() ? beh->GetName() : "?", reset ? " (reset)" : "");
            }
            // The other way round (reported, not undone): an activation the
            // re-made world has pending that the straight pass's frame had not.
            for (CK_ID id: pending_watch) {
                auto* beh = CKBehavior::Cast(context->GetObject(id));
                if (!beh || !(beh->GetFlags() & CKBEHAVIOR_ACTIVATENEXTFRAME)) continue;
                if (std::none_of(rec.pending.begin(), rec.pending.end(), [id](const auto& p) { return p.first == id; })) {
                    ++remake_extra_pending;
                    if (args.verbose)
                        std::printf("  re-made live world: before the step at %u %s is pending, the straight pass's was not\n",
                                    t, beh->GetName() ? beh->GetName() : "?");
                }
            }
        };

        // ---- the straight pass
        // Step RLQ: --queue-trace, the PreSimulate queue after each straight
        // step whenever it changes (frame = the tick the step leads to), each
        // entry with its target and whether that has a body.
        eg0::queue_tracer queue_tracer;
        queue_tracer.targets = true;
        // Step RLQ: with the hold bound, an outside entry is live state (gap
        // Q4): the replay never runs its script, and a blob captured inside a
        // replay lacks it (rollback-engine-gaps "As built (step B7EG)").  So
        // the blobs a round compares - the straight pass's expected B[t] and
        // the depth-0 restore probe - are captured with the same entries set
        // aside (callbacks_hold of the executed scripts, the capture, the
        // put-back to their places), as a replay captures; the executed
        // scripts' entries stay in and are compared byte for byte.  The
        // engine's own record of a tick keeps the whole queue (the restore's
        // scoped check is what skips the outside entries there).
        const bool hold_bound = frames && !args.savestate_no_hold && !scripts.scripts.empty();
        const std::vector<uint32_t> executed =
            hold_bound ? bmmo::session::replay_executed_scripts(scripts, trafo ? &trafo->registry().part() : nullptr)
                       : std::vector<uint32_t>{};
        int outside_ticks = 0, outside_max = 0;   // straight ticks with outside entries, the most at once
        const auto capture_compared = [&](const std::vector<uint8_t>* whole, std::vector<uint8_t>& out, std::string& why,
                                          int& held) {
            held = 0;
            if (!hold_bound) {
                if (whole) { out = *whole; return true; }
                return bmmo::physics::get_savestate(physics, out, why);
            }
            held = bmmo::physics::callbacks_hold(physics, executed, why);
            if (held < 0) return false;
            bool ok = true;
            if (held == 0 && whole) out = *whole;
            else ok = bmmo::physics::get_savestate(physics, out, why);
            std::string put_back_error;
            if (bmmo::physics::callbacks_unhold(physics, put_back_error) != 0) {
                why = "the outside entries' put-back was clamped (their order changed)";
                ok = false;
            }
            return ok;
        };
        std::vector<uint8_t> compared;
        if (passes) bmmo::physics::drain_event_log(physics);   // step S7: the window's births only
        std::string error;
        std::vector<straight_tick> straight(static_cast<size_t>(window));
        std::vector<int> reset_ticks;   // --frames: window indices at which a selected script had a reset pending
        int script_resets = 0;
        uint32_t contacts_min = std::numeric_limits<uint32_t>::max(), collisions_min = contacts_min,
                 systems_min = contacts_min;
        uint32_t contacts_max = 0, collisions_max = 0, systems_max = 0;
        size_t blob_max = 0;
        std::vector<uint8_t> blob;
        // Step RLM (the G1 membership leak's witness): whether the next live
        // frame would run each selected script's owner (script_replay::
        // owner_in_frame: the behaviour manager's pending table, then its
        // list), one character per script - the straight pass's at a kept
        // tick, a round's at its end.  Reported, not judged: the replay does
        // not run the live frame's list (gap F).
        const auto owner_membership = [&]() {
            std::string out;
            for (CK_ID id: scripts.scripts) {
                auto* beh = CKBehavior::Cast(context->GetObject(id));
                out += !beh ? '-' : bmmo::game::script_replay::owner_in_frame(context, beh) ? '1' : '0';
            }
            return out;
        };
        int membership_rounds = 0, membership_differ = 0;
        std::string membership_first;
        const bmmo::game::script_replay::membership_counters membership_before = bmmo::game::script_replay::membership();
        const auto started = std::chrono::steady_clock::now();
        for (int i = 0; i < window; ++i) {
            straight_tick& rec = straight[static_cast<size_t>(i)];
            const uint32_t t = t0 + static_cast<uint32_t>(i);
            const bool keep_this = keep[static_cast<size_t>(i)] != 0;
            rec.counters = world->capture_step_state();
            if (frames) {
                const auto pending = bmmo::game::script_replay::resets_pending(context, scripts);
                if (!pending.empty()) {
                    reset_ticks.push_back(i);
                    script_resets += static_cast<int>(pending.size());
                }
                if (keep_this && !bmmo::game::script_replay::capture(context, scripts, hooks, rec.scripts)) {
                    std::fprintf(stderr, "savestate-check: script capture failed at tick %u\n", t);
                    return 1;
                }
                if (keep_this) rec.membership = owner_membership();
                if (keep_this && args.verbose)   // close-out step S2 (V4b's review)
                    std::printf("  straight t=%u owner membership %s\n", t, rec.membership.c_str());
                if (!written_cols.empty()) rec.cells = read_written();   // step RK2's review
            }
            bmmo::physics::savestate_report report;
            if (!bmmo::physics::get_savestate(physics, blob, error, &report)) {
                std::fprintf(stderr, "savestate-check: capture failed at tick %u: %s\n", t, error.c_str());
                return 1;
            }
            // Step RLQ: the blob a round compares (the outside entries set
            // aside when the hold is bound; `blob` itself when it is not).
            int held = 0;
            if (!capture_compared(&blob, compared, error, held)) {
                std::fprintf(stderr, "savestate-check: the compared capture failed at tick %u: %s\n", t, error.c_str());
                return 1;
            }
            if (held > 0) {
                ++outside_ticks;
                outside_max = std::max(outside_max, held);
            }
            rec.blob_hash = bmmo::physics::hash_savestate(compared.data(), compared.size());
            rec.blob_size = compared.size();
            blob_max = std::max(blob_max, blob.size());
            contacts_min = std::min(contacts_min, report.contacts);
            collisions_min = std::min(collisions_min, report.collisions);
            systems_min = std::min(systems_min, report.systems);
            contacts_max = std::max(contacts_max, report.contacts);
            collisions_max = std::max(collisions_max, report.collisions);
            systems_max = std::max(systems_max, report.systems);
            if (i == 0)
                std::printf("savestate-check: at t0=%u objects=%u cores=%u collisions=%u contacts=%u systems=%u units=%u"
                            " events=%u hull_entries=%u blob_bytes=%zu\n", t, report.objects, report.cores,
                            report.collisions, report.contacts, report.systems, report.units, report.events,
                            report.hull_entries, blob.size());
            if (!bmmo::physics::capture_world_hash(physics, rec.hash, error)) {
                std::fprintf(stderr, "savestate-check: world hash failed at tick %u: %s\n", t, error.c_str());
                return 1;
            }
            if (keep_this) {
                rec.kept = true;
                rec.blob = compared;   // step RLQ
                rec.exact = bmmo::physics::describe_cores_exact(physics);
            }
            // The engine's record of this tick: bodies, extras, contacts,
            // scripts and (unless --legacy-restore) the blob - what a rewind
            // to this tick restores.
            if (trafo) {
                tracked.mechanisms = bmmo::physics::movable_body_names(physics);
                tracked.generations = trafo_generations;
            }
            if (passes) {   // step S7: the body set changes at the pass
                tracked.mechanisms = bmmo::physics::movable_body_names(physics);
                tracked.generations = pass_generations;
                rec.bodies = tracked.mechanisms;
                std::sort(rec.bodies.begin(), rec.bodies.end());
            }
            engine.record(binding, t, tracked, no_inputs);
            if (!legacy) {
                const std::vector<uint8_t>* recorded = engine.recorded_savestate(t);
                if (!recorded || *recorded != blob) {
                    std::fprintf(stderr, "savestate-check: the engine's record of tick %u does not carry this tick's blob\n", t);
                    return 1;
                }
            }
            char line[320];
            std::snprintf(line, sizeof(line),
                "straight t=%u hash=%016llx pose=%016llx cores=%d blob_bytes=%zu contacts=%u collisions=%u systems=%u",
                t, static_cast<unsigned long long>(rec.hash.hash), static_cast<unsigned long long>(rec.hash.pose),
                rec.hash.cores, blob.size(), report.contacts, report.collisions, report.systems);
            trace.put(line);
            if (args.verbose || (args.report_every > 0 && i % args.report_every == 0)) std::printf("%s\n", line);
            if (trafo) trafo->apply_inputs(t + 1u, false);
            std::set<int> active_before;
            if (passes) {
                active_before = world->active_sectors();
                if (const auto due = window_moves.find(t); due != window_moves.end())
                    for (int sector: due->second) std::printf("savestate-check: tick=%u player 2 moved to sector %d\n", t, sector);
                moves_before(t);
                capture_pending(rec);   // close-out step FX2
            }
            if (!step(error)) {
                std::fprintf(stderr, "savestate-check: the straight pass's step at tick %u failed: %s\n", t, error.c_str());
                return 1;
            }
            if (frames) live_order.record(t + 1u, bmmo::game::script_replay::live_frame_order(context, scripts));   // step RO1
            if (args.queue_trace) queue_tracer.after_frame(world->engine(), static_cast<long long>(t) + 1);   // step RLQ
            if (passes) {
                // Step S7: the step's births (keyed by the record that first
                // carries them, t+1, the tick a rewind replays this step as),
                // its lifetime changes, the union's pass in the pass slot of
                // that replayed tick and the frame's pending activations for
                // the next one (the late-sector check's recording, shifted to
                // this mode's record convention).
                bmmo::session::parse_event_log(bmmo::physics::drain_event_log(physics),
                                               [&](const std::string& kind, const std::string& text) {
                    if (kind == "recipe") {
                        bmmo::session::local_birth birth;
                        if (bmmo::session::parse_recipe_event(text, birth) && !is_ball(birth.name)) {
                            pass_birth_order.emplace_back(t + 1u, birth.name);
                            pass_births.add(t + 1u, std::move(birth));
                            ++pass_birth_count;
                        }
                    } else if ((kind == "created" || kind == "deleted") && !text.empty()) {
                        ++pass_generations[text];
                    }
                });
                for (int s: world->active_sectors())
                    if (!active_before.count(s)) {
                        activations.set_pass(t + 1u, s);
                        pass_ticks.insert(t + 1u);
                        ++passes_made;
                        std::printf("savestate-check: sector +%d in the step at tick %u (replayed as %u)\n", s, t, t + 1u);
                    }
                activations.record_pending(context, manager_id, script_activations, t + 2u);
            }
            if (trafo) {
                // The step's births (the recipes), keyed by the record that
                // first carries them, and every lifetime change.
                const std::string events = bmmo::physics::drain_event_log(physics);
                bmmo::session::parse_event_log(events, [&](const std::string& kind, const std::string& name) {
                    if (kind == "recipe") {
                        bmmo::session::local_birth birth;
                        if (bmmo::session::parse_recipe_event(name, birth)) { trafo_births.add(t + 1u, std::move(birth)); ++trafo_birth_count; }
                    } else if (kind == "created" || kind == "deleted") {
                        ++trafo_generations[name];
                        (kind == "created" ? trafo_birth_ticks : trafo_death_ticks).insert(t + 1u);
                    }
                });
            }
        }
        const double straight_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        pristine = engine;

        const auto summary = [&](int rounds_run, int rounds_failed) {
            char line[768];
            std::snprintf(line, sizeof(line),
                "summary: savestate-check rounds=%d failed=%d ticks=%d blob_bytes=%zu contacts_min=%u collisions_min=%u"
                " systems_min=%u contacts_max=%u collisions_max=%u systems_max=%u scheduled=%zu kept=%d mode=%s"
                " restore=%s scene=%s seed=%llu max_depth=%d every=%d t0=%u script_resets=%d",
                rounds_run, rounds_failed, window, blob_max, contacts_min, collisions_min, systems_min, contacts_max,
                collisions_max, systems_max, schedule.size(), kept, mode, restore, scene.c_str(), args.savestate_seed,
                args.savestate_max_depth, args.savestate_every, t0, script_resets);
            std::printf("%s\n", line);
            trace.put(line);
        };
        if (contacts_max == 0 && systems_max == 0) {
            summary(0, 0);
            std::printf("vacuous scene: no tick of the window holds a contact point or a friction system (contacts_max=0"
                        " systems_max=0), so a pass would prove nothing about the collision state\n");
            return 2;
        }

        // ---- the rounds, chained: the world is wherever the previous round
        // left it, which is the live case (the world has moved on with another
        // history).  Every round is the engine's rewind_to from a+d to a: the
        // engine restores the anchor from its own record and re-steps a+1..a+d
        // through the binding, re-recording each tick.
        int run = 0, failed = 0;
        // C3 (step RK1): the rounds' globals writes (set, replays, replays
        // anchored before the first change, unknown ticks, failures).
        uint64_t globals_sum[5] = {};
        // The harness's live world and the globals (C3, step RK1).  A round's
        // live world stands at its live tick e in a client, so its globals are
        // e's record's; the chained world here stands wherever the previous
        // round left it, and the engine puts the globals it had before the
        // rewind back after the replay (design 2.3 step 4, as a client must:
        // the retail frame may rewind after its scripts wrote the globals of
        // its coming step).  So the stand-in enters every round with the
        // straight pass's end's globals - what every round had before C3, a
        // running clock - and is given e's record's right after the rewind,
        // before the witnesses and the follow: the world a client has at e.
        // (The replay identity already compares every replayed tick's
        // globals: the blob carries the factor, the physics delta and the
        // gravity.)  The re-made live worlds get t0's after their restore,
        // and each re-stepped frame the straight step's (step FX2).
        bmmo_physics_globals end_globals{};
        bool have_end_globals = false;
        {
            std::string globals_error;
            have_end_globals = backend.get_physics_globals(end_globals, globals_error);
        }
        uint64_t globals_put_failures = 0;
        const auto put_globals = [&](const bmmo_physics_globals* globals) {
            std::string globals_error;
            if (globals && !backend.set_physics_globals(*globals, globals_error)) {
                ++globals_put_failures;
                std::printf("  the harness could not put the globals back: %s\n", globals_error.c_str());
            }
        };
        // Close-out step FX2: before a re-made live world's step at t, the
        // globals the straight step at t ran with (the record of t+1's); a
        // write that changes them is counted.
        const auto put_step_globals = [&](uint32_t t) {
            const bmmo_physics_globals* g = pristine.recorded_globals(t + 1u);
            if (!g) return;
            bmmo_physics_globals now{};
            std::string globals_error;
            if (backend.get_physics_globals(now, globals_error)
                    && now.time_factor == g->time_factor && now.gravity[0] == g->gravity[0]
                    && now.gravity[1] == g->gravity[1] && now.gravity[2] == g->gravity[2])
                return;
            put_globals(g);
            ++remake_globals_written;
        };
        // Step B2SX (Q5): depth-0 compares that differed only in outputs the
        // restore re-armed (ignored, option (a)), and the follows run.
        int rearmed_ignored_rounds = 0, rearmed_ignored_bits = 0, follow_ticks = 0, follow_fired = 0;
        CKBehavior* const manager_graph = manager_id ? CKBehavior::Cast(context->GetObject(manager_id)) : nullptr;
        bool dumped = false;
        bool remade_dumped = false;   // close-out step S2: the premise dump has its own flag
        std::vector<uint8_t> current;
        const auto rounds_started = std::chrono::steady_clock::now();
        for (size_t r = 0; r < schedule.size(); ++r) {
            const int a = schedule[r].anchor, d = schedule[r].depth, e = a + d;
            const straight_tick& anchor = straight[static_cast<size_t>(a)];
            if (!anchor.kept) {
                std::fprintf(stderr, "savestate-check: round %zu anchors at an unkept tick (harness bug)\n", r);
                return 1;
            }
            ++run;
            std::string failure;
            int fail_index = a;
            bool restore_broke = false;
            const std::vector<uint8_t>* expected_blob = nullptr;   // set when a kept blob differed
            bool crosses_reset = false;
            for (int t: reset_ticks) crosses_reset = crosses_reset || (t > a && t <= e);

            // Trafo inputs (S6): the window's births and deaths change the
            // body set, and a rewind's body plan reads it from the engine's
            // records - the live world has to be the one the record of the
            // round's live tick e describes, as a session's always is.  A
            // world chained from the previous round's end (another tick, so
            // another set of pieces) is not, so each round's live world is
            // made again: every piece body taken out, the record of t0 put
            // back (no piece, every set's scripts at t0), and the straight
            // pass's steps re-run to e with its inputs.
            // The round's premise: a live world that is not the straight
            // pass's (or a refused t0 restore) fails the round.
            std::string premise;
            if (trafo) {
                std::string remake_error;
                for (const auto& set: trafo->registry().sets())
                    for (const auto& ts: set.types)
                        for (CK_ID id: ts.pieces)
                            if (CKObject* o = context->GetObject(id); o && trafo->hooks().has_body(o->GetName()))
                                trafo->hooks().unphysicalize(o->GetName(), remake_error);
                world->restore_step_state(straight[0].counters);
                engine = pristine;
                engine.forget_after(t0);
                if (!engine.rewind_to(binding, t0, t0, no_input, "savestate-check: the round's live world")) {
                    std::printf("  round %zu: the restore of t0 for its live world was refused\n", r);
                    premise = "the restore of t0 for the round's live world was refused";
                }
                put_globals(pristine.recorded_globals(t0));   // C3: the re-made world at t0
                for (int i = 0; i < e; ++i) {
                    trafo->apply_inputs(t0 + static_cast<uint32_t>(i) + 1u, true);
                    put_step_globals(t0 + static_cast<uint32_t>(i));   // step FX2
                    ++remake_steps;
                    if (!step(remake_error)) {
                        std::fprintf(stderr, "savestate-check: re-stepping round %zu's live world failed: %s\n", r, remake_error.c_str());
                        return 1;
                    }
                }
                bmmo::physics::drain_event_log(physics);
                std::vector<uint8_t> live;
                int live_held = 0;   // step RLQ: captured as the straight pass captured e
                if (!capture_compared(nullptr, live, remake_error, live_held) || !straight[static_cast<size_t>(e)].kept
                        || live != straight[static_cast<size_t>(e)].blob) {
                    std::printf("  round %zu: the re-made live world at %u is not the straight pass's%s\n", r,
                                t0 + static_cast<uint32_t>(e), straight[static_cast<size_t>(e)].kept ? "" : " (not kept)");
                    if (premise.empty())
                        premise = "the re-made live world at " + std::to_string(t0 + static_cast<uint32_t>(e)) + " is not the straight pass's";
                }
            }
            // Step S7: a pass inside the window changes the body set, and the
            // rewind's body plan reads it from the engine's records - the live
            // world has to hold the bodies the record of e names.  A world
            // chained from the previous round's end on the other side of a
            // birth does not, so it is made again as the trafo rounds' is, but
            // only then: every window-born body taken out (its script's joint
            // handles released, the event log muted) and put back where its
            // Physicalize found it, the record of t0 restored, the straight
            // pass's whole world ticks re-run to e with the moves.
            if (passes) {
                std::vector<std::string> now = bmmo::physics::movable_body_names(physics);
                std::sort(now.begin(), now.end());
                const straight_tick& end_rec = straight[static_cast<size_t>(e)];
                // Every body first born after T0 (fixed ones included: the
                // movable names do not list them), with its earliest birth.
                const uint32_t end_tick = t0 + static_cast<uint32_t>(e);
                std::map<std::string, uint32_t> first_birth;
                for (const auto& [tick, name]: pass_birth_order) first_birth.emplace(name, tick);
                bool fixed_differ = false;
                for (const auto& [name, tick]: first_birth)
                    if (tick > t0 && has_body(name) != (tick <= end_tick)) fixed_differ = true;
                // Step B2SX: a named round's live world is always made again
                // at its e - the scripts the binding does not rewind (the
                // Gameplay_* chains) then stand at e like the selection, as in
                // a client, whose rewind always ends at its live tick; a world
                // chained from another round's end has them at that tick, and
                // its next live frame (the follow) is not the straight pass's.
                if (now != end_rec.bodies || fixed_differ || !args.savestate_explicit.empty()) {
                    ++pass_remakes;
                    std::string remake_error, mute_error;
                    // Newest first, each name once, put back at its earliest
                    // birth (the destroy recipe of the record-capture mode).
                    std::set<std::string> done;
                    for (auto it = pass_birth_order.rbegin(); it != pass_birth_order.rend(); ++it) {
                        const std::string& name = it->second;
                        const uint32_t born = first_birth[name];
                        if (born <= t0 || !done.insert(name).second || !has_body(name)) continue;
                        const auto held = bmmo::game::script_replay::joints_on(context, scripts, name);
                        backend.set_event_log_muted(true, mute_error);
                        if (backend.unphysicalize(name.c_str(), remake_error)) {
                            if (!held.empty()) bmmo::game::script_replay::release_joints(held);
                            if (const bmmo::session::local_birth* birth = pass_births.find(name, born))
                                bmmo::game::put_back_birth(context, name, *birth);
                        } else if (premise.empty()) {
                            premise = "taking " + name + " out for the round's live world failed: " + remake_error;
                        }
                        backend.set_event_log_muted(false, mute_error);
                    }
                    world->restore_step_state(straight[0].counters);
                    engine = pristine;
                    engine.forget_after(t0);
                    if (!engine.rewind_to(binding, t0, t0, no_input, "savestate-check: the round's live world")
                            && premise.empty())
                        premise = "the restore of t0 for the round's live world was refused";
                    put_globals(pristine.recorded_globals(t0));   // C3: the re-made world at t0
                    for (int i = 0; i < e; ++i) {
                        moves_before(t0 + static_cast<uint32_t>(i));
                        // Step FX2: what the outside scripts' frames did in
                        // the straight pass and do not do again here.
                        reissue_pending(straight[static_cast<size_t>(i)], t0 + static_cast<uint32_t>(i));
                        put_step_globals(t0 + static_cast<uint32_t>(i));
                        ++remake_steps;
                        if (!step(remake_error)) {
                            std::fprintf(stderr, "savestate-check: re-stepping round %zu's live world failed: %s\n", r,
                                         remake_error.c_str());
                            return 1;
                        }
                    }
                    bmmo::physics::drain_event_log(physics);
                    std::vector<uint8_t> live;
                    std::vector<std::string> remade = bmmo::physics::movable_body_names(physics);
                    std::sort(remade.begin(), remade.end());
                    int live_held = 0;   // step RLQ: captured as the straight pass captured e
                    if (!capture_compared(nullptr, live, remake_error, live_held) || !end_rec.kept || live != end_rec.blob
                            || remade != end_rec.bodies
                            || std::any_of(first_birth.begin(), first_birth.end(), [&](const auto& b) {
                                   return b.second > t0 && has_body(b.first) != (b.second <= end_tick);
                               })) {
                        std::printf("  round %zu: the re-made live world at %u is not the straight pass's%s\n", r,
                                    t0 + static_cast<uint32_t>(e), end_rec.kept ? "" : " (not kept)");
                        if (premise.empty())
                            premise = "the re-made live world at " + std::to_string(t0 + static_cast<uint32_t>(e))
                                    + " is not the straight pass's";
                        // Close-out step S2: --savestate-trace describes both
                        // worlds of the first such round (the straight pass's
                        // record of e and the re-made one).
                        if (!remade_dumped && end_rec.kept && !args.savestate_trace.empty() && !live.empty()) {
                            remade_dumped = true;
                            const std::string base = args.savestate_trace + ".remade" + std::to_string(r);
                            const bool ok = write_text_file(base + ".expected.txt",
                                                bmmo::physics::describe_savestate(end_rec.blob.data(), end_rec.blob.size()))
                                         && write_text_file(base + ".actual.txt",
                                                bmmo::physics::describe_savestate(live.data(), live.size()));
                            std::printf("  %s both worlds of round %zu's re-made live world: %s.expected.txt / .actual.txt\n",
                                        ok ? "wrote" : "could not write", r, base.c_str());
                        }
                    }
                }
            }
            // Close-out step S2 (V4b's review): with --verbose, every selected
            // script whose owner the live world holds in the behaviour
            // manager's pending table or list before the rewind (state no
            // restore writes back).
            if (args.verbose && frames)
                std::printf("  round %zu: before the rewind to %u: owners held live: %s\n", r, t0 + static_cast<uint32_t>(a),
                            owners_held().c_str());
            // Step B2SX (Q5): a named round says whether the live world it
            // rewinds has a delayed link in flight (the re-arm's two cases).
            if (!args.savestate_explicit.empty() && manager_graph) {
                int in_flight = 0, fired = 0;
                const std::string links = describe_delayed_links(manager_graph, in_flight, fired);
                std::printf("  round %zu: before the rewind to %u: %s delayed link(s) of Gameplay_SectorManager in flight"
                            " live: %s\n", r, t0 + static_cast<uint32_t>(a), std::to_string(in_flight).c_str(),
                            links.c_str());
            }
            // The world's own per-tick bookkeeping (tick index, RNG cursors,
            // the sector union) at the anchor; the engine knows the physics.
            // Step G2's review: --probe-scramble moves the frames around the
            // step state's restore (V4's hold, exercised).
            if (args.probe_scramble == 1) world->displace_probe_frames(1000.0f);
            const uint64_t hold_failures_before = world->proximity_hold_failures();
            world->restore_step_state(anchor.counters);
            if (args.probe_scramble == 2) world->displace_probe_frames(1000.0f);
            if (have_end_globals) put_globals(&end_globals);   // C3: the stand-in's running clock
            engine = pristine;
            if (trafo || passes) engine.forget_after(t0 + static_cast<uint32_t>(e));
            probe_tick = t0 + static_cast<uint32_t>(a) + 1u;
            restored_blob.clear();
            replayed_blobs.clear();
            restore_error.clear();
            capture_error.clear();
            engine_log.clear();
            restore_refused = false;
            const uint64_t restored_before = engine.stats().savestates_restored;
            const uint64_t resim_before = engine.stats().resim_ticks;
            // Step RLQ's review: the compared blobs leave the outside entries
            // out, so their order is judged here - the engine's put-back at
            // the end of the replay must not clamp one, nor its hold fail.
            const uint64_t clamped_before = bmmo::game::script_replay::queue_stats().unhold_clamped;
            const uint64_t hold_fail_before = bmmo::game::script_replay::queue_stats().hold_fail;
            if (args.savestate_no_ledger) bmmo::game::script_replay::registrations().clear();   // step RLM's control (FX1: its replayed adds too)
            if (args.joint_shuffle) {
                const int shuffled = shuffle_joints(context, hooks.capture_joint ? hooks
                                                    : bmmo::game::force_hooks_for(bmmo::physics::physics_direct(physics)));
                std::printf("  round %zu: --joint-shuffle made %d constraint(s) again before the rewind\n", r, shuffled);
            }
            // Step RK2's review: every row of the written-cell columns set to a
            // sentinel (distinct per row) - a restore that skips a column, or
            // writes another one, leaves it there for the round-end check.
            for (const bmmo::game::written_column& w: written_cols) {
                auto* arr = CKDataArray::Cast(context->GetObject(w.array));
                const int rows = arr && w.column >= 0 && w.column < arr->GetColumnCount() ? arr->GetRowCount() : 0;
                for (int row = 0; row < rows; ++row) {
                    uint32_t sentinel = 0x5EB7C300u + static_cast<uint32_t>(row);
                    arr->SetElementValue(row, w.column, &sentinel, sizeof(sentinel));
                    ++cells_sentinel_rows;
                }
            }
            const bmmo::session::rollback_stats globals_before = engine.stats();   // C3 (step RK1)
            const bool rewound = engine.rewind_to(binding, t0 + static_cast<uint32_t>(a), t0 + static_cast<uint32_t>(e),
                                                  no_input, "savestate-check");
            globals_sum[0] += engine.stats().globals_set - globals_before.globals_set;
            globals_sum[1] += engine.stats().globals_replays - globals_before.globals_replays;
            globals_sum[2] += engine.stats().globals_replays_start - globals_before.globals_replays_start;
            globals_sum[3] += engine.stats().globals_unknown - globals_before.globals_unknown;
            globals_sum[4] += engine.stats().globals_failures - globals_before.globals_failures;
            put_globals(pristine.recorded_globals(t0 + static_cast<uint32_t>(e)));   // C3: the live world at e
            // rewind_to's true says the request was handled, not that a replay
            // ran: the stats say what happened.
            if (!rewound) {
                failure = "the engine refused the rewind" + (engine_log.empty() ? std::string() : ": " + engine_log);
            } else if (!legacy && restore_refused) {
                failure = "restore error: " + restore_error;
                restore_broke = true;
            } else if (world->proximity_hold_failures() != hold_failures_before) {
                failure = "the proximity union's hold was not restored (physics_world::restore_step_state)";
            } else if (bmmo::game::script_replay::queue_stats().hold_fail != hold_fail_before) {
                failure = "the PreSimulate queue's outside-entry hold failed (step RLQ)";
            } else if (bmmo::game::script_replay::queue_stats().unhold_clamped != clamped_before) {
                failure = "the outside entries' put-back was clamped (their order changed): "
                        + std::to_string(bmmo::game::script_replay::queue_stats().unhold_clamped - clamped_before)
                        + " entr" + (bmmo::game::script_replay::queue_stats().unhold_clamped - clamped_before == 1 ? "y" : "ies");
            } else if (!legacy && engine.stats().savestates_restored != restored_before + 1u) {
                failure = "the engine did not restore the anchor's blob (fell back to the per-body path)";
            } else if (engine.stats().resim_ticks != resim_before + static_cast<uint64_t>(d)) {
                failure = "the engine re-simulated " + std::to_string(engine.stats().resim_ticks - resim_before) + " ticks of "
                        + std::to_string(d);
            } else if (!capture_error.empty()) {
                failure = "capture error: " + capture_error;
            }
            // Restore identity: the world the first replayed tick starts from
            // (d == 0: the world the rewind left) is the anchor's blob.
            if (failure.empty() && restored_blob.empty()) {
                // Step RLQ: as the straight pass captured the anchor (the
                // rewind's put-back is done: no hold is active here).
                std::string probe_error;
                int probe_held = 0;
                if (!capture_compared(nullptr, restored_blob, probe_error, probe_held)) failure = "capture error: " + probe_error;
            }
            // --savestate-inert-from-birth (a diagnostic): the latest FIXED
            // body first born inside (anchor, e]; until its record the
            // restored world holds it inert and is not the anchor's.
            uint32_t inert_until = 0;
            if (passes && args.savestate_inert_from_birth) {
                const std::vector<std::string>& movable_at_e = straight[static_cast<size_t>(e)].bodies;
                std::map<std::string, uint32_t> first;
                for (const auto& [tick, name]: pass_birth_order) first.emplace(name, tick);
                for (const auto& [name, tick]: first)
                    if (tick > t0 + static_cast<uint32_t>(a) && tick <= t0 + static_cast<uint32_t>(e)
                            && !std::binary_search(movable_at_e.begin(), movable_at_e.end(), name))
                        inert_until = std::max(inert_until, tick);
                if (inert_until) ++inert_rounds;
            }
            if (failure.empty() && restored_blob != anchor.blob && !inert_until) {
                failure = "restore identity: " + blob_difference(anchor.blob, restored_blob);
                expected_blob = &anchor.blob;
                current = restored_blob;
            }
            // Replay identity: every replayed tick's blob - the engine's own
            // re-record of the tick (made after everything it does to a tick,
            // a window-born body's re-pose included); with the blob switched
            // off (--legacy-restore) the step wrapper's capture.
            for (int k = 1; failure.empty() && k <= d; ++k) {
                const int i = a + k;
                if (t0 + static_cast<uint32_t>(i) < inert_until) continue;
                fail_index = i;
                const straight_tick& expected = straight[static_cast<size_t>(i)];
                const std::vector<uint8_t>* recorded = legacy ? nullptr : engine.recorded_savestate(t0 + static_cast<uint32_t>(i));
                if (!recorded && static_cast<size_t>(k - 1) >= replayed_blobs.size()) {
                    failure = "the replay stepped " + std::to_string(replayed_blobs.size()) + " ticks of " + std::to_string(d);
                    break;
                }
                const std::vector<uint8_t>& got = recorded ? *recorded : replayed_blobs[static_cast<size_t>(k - 1)];
                if (expected.kept) {
                    if (got != expected.blob) {
                        failure = blob_difference(expected.blob, got);
                        expected_blob = &expected.blob;
                        current = got;
                    }
                } else if (got.size() != expected.blob_size
                           || bmmo::physics::hash_savestate(got.data(), got.size()) != expected.blob_hash) {
                    char text[256];
                    std::snprintf(text, sizeof(text),
                        "blob hash %016llx vs %016llx, %zu vs %zu bytes (not kept at this tick: --savestate-every 1"
                        " names the field)", static_cast<unsigned long long>(expected.blob_hash),
                        static_cast<unsigned long long>(bmmo::physics::hash_savestate(got.data(), got.size())),
                        expected.blob_size, got.size());
                    failure = text;
                }
            }
            // The independent witnesses at the end of the round.
            bmmo::physics::world_hash end_hash;
            if (failure.empty()) {
                fail_index = e;
                const straight_tick& expected = straight[static_cast<size_t>(e)];
                if (!bmmo::physics::capture_world_hash(physics, end_hash, error)) {
                    failure = "world hash error: " + error;
                } else {
                    const std::string what = diff_world_hashes(expected.hash, end_hash);
                    if (!what.empty()) failure = "world hash: " + what;
                }
                if (failure.empty()) {
                    const std::string exact = bmmo::physics::describe_cores_exact(physics);
                    if (exact != expected.exact) failure = "exact cores:" + diff_exact_dumps(expected.exact, exact);
                }
                if (failure.empty() && frames) {
                    std::vector<uint8_t> bytes;
                    if (!bmmo::game::script_replay::capture(context, scripts, hooks, bytes)) {
                        failure = "script capture error";
                    } else if (bytes != expected.scripts) {
                        // Step B2SX (record-format Q5 / A23, the user's option
                        // (a)): at depth 0 no frame ran after the restore, so
                        // an output it turned back on to re-arm an in-flight
                        // delayed link (script_replay.hpp restore_behavior)
                        // still reads on where the record has it off; exactly
                        // those bits are left out, everything else compared.
                        const auto masks = d == 0 ? bmmo::game::script_replay::rearmed_output_masks()
                                                  : std::map<CK_ID, uint32_t>{};
                        const auto difference = bmmo::game::script_replay::diff(context, scripts, expected.scripts, bytes,
                                                                                masks.empty() ? nullptr : &masks);
                        if (difference.any) {
                            failure = "scripts: " + difference.where + " (" + std::to_string(difference.logical_count)
                                    + " logical, " + std::to_string(difference.float_count) + " float)";
                            // Step G2: --verbose names every logical difference (up to a dozen).
                            if (args.verbose && !difference.detail.empty()) failure += "; detail: " + difference.detail;
                        } else {
                            ++rearmed_ignored_rounds;
                            for (const auto& [block, bits]: masks)
                                for (uint32_t m = bits; m; m &= m - 1) ++rearmed_ignored_bits;
                        }
                    }
                }
                // Step RK2's review: the written-cell columns at e are the
                // straight pass's (the sentinel set before the rewind is gone).
                if (failure.empty() && !written_cols.empty()) {
                    ++cells_checked_rounds;
                    const std::vector<uint32_t> now = read_written();
                    if (now != expected.cells) {
                        ++cells_differ_rounds;
                        size_t k = 0;
                        while (k < now.size() && k < expected.cells.size() && now[k] == expected.cells[k]) ++k;
                        char text[160];
                        std::snprintf(text, sizeof(text), "written cells (step RK2): word %zu is 0x%08x, the straight pass's 0x%08x",
                                      k, k < now.size() ? now[k] : 0u, k < expected.cells.size() ? expected.cells[k] : 0u);
                        failure = text;
                    }
                }
                // Step RLM: the owners' membership at e, the witness.
                if (frames && expected.kept && !expected.membership.empty()) {
                    ++membership_rounds;
                    const std::string now = owner_membership();
                    if (now != expected.membership) {
                        ++membership_differ;
                        std::string which;
                        for (size_t k = 0; k < now.size() && k < expected.membership.size(); ++k)
                            if (now[k] != expected.membership[k]) {
                                auto* beh = CKBehavior::Cast(context->GetObject(scripts.scripts[k]));
                                which += std::string(which.empty() ? "" : ", ") + (beh && beh->GetName() ? beh->GetName() : "?") + " "
                                       + expected.membership[k] + " vs " + now[k];
                            }
                        const std::string line = "round=" + std::to_string(r) + " anchor=" + std::to_string(t0 + static_cast<uint32_t>(a)) + " depth="
                                               + std::to_string(d) + ": " + which;
                        if (membership_first.empty()) membership_first = line;
                        if (args.verbose) std::printf("  membership: %s\n", line.c_str());
                    }
                }
                // Step G2's review: the proximity union's hold at e (the
                // frames and the by-name cells) is the straight pass's.
                if (failure.empty() && world->proximity_union_on()
                        && world->capture_step_state().proximity_hold != expected.counters.proximity_hold)
                    failure = "proximity hold: the frames or the by-name cells differ from the straight pass's";
            }
            // Step B2SX (Q5's untested case): a named round's follow - F live
            // frames past e, as the straight pass stepped them, each tick's
            // blob and scripts compared with the straight pass's; a delayed
            // link the restore re-armed must fire once, where the straight
            // pass's fired (its destination input on at that record only).
            const int follow = failure.empty() && r < follows.size() ? follows[r] : 0;
            if (follow > 0) {
                if (trafo) {
                    failure = "follow: not built with trafo inputs";
                } else {
                    // The world's counters as the straight pass had them at e;
                    // the RNG cursors as the replay left them.
                    auto counters = straight[static_cast<size_t>(e)].counters;
                    counters.rng_cursor = ivp_srand_read();
                    counters.random_cursor = bmmo::physics::random_get_state();
                    world->restore_step_state(counters);
                }
                for (int k = 1; failure.empty() && k <= follow; ++k) {
                    const int i = e + k;
                    const uint32_t t = t0 + static_cast<uint32_t>(i) - 1u;   // the step at t makes record i
                    fail_index = i;
                    if (passes) moves_before(t);
                    // What a client does for the live frame a rewind ends
                    // before (live_cells_guard, step B1): the activations the
                    // straight pass had pending for this frame re-issued - a
                    // restore writes back the scripts, not the live world's
                    // pending activations of another tick.
                    if (passes && k == 1) activations.replay(context, manager_id, t + 1u, false);
                    std::string follow_error;
                    if (!step(follow_error)) { failure = "follow: the step at " + std::to_string(t) + " failed: " + follow_error; break; }
                    bmmo::physics::drain_event_log(physics);
                    ++follow_ticks;
                    const straight_tick& expected = straight[static_cast<size_t>(i)];
                    std::vector<uint8_t> blob_now;
                    std::string follow_capture;
                    int follow_held = 0;   // step RLQ: captured as the straight pass captured this tick
                    if (!capture_compared(nullptr, blob_now, follow_capture, follow_held)) {
                        failure = "follow: capture error at " + std::to_string(t + 1u) + ": " + follow_capture;
                        break;
                    }
                    if (expected.kept ? blob_now != expected.blob
                                      : bmmo::physics::hash_savestate(blob_now.data(), blob_now.size()) != expected.blob_hash) {
                        failure = "follow: blob at " + std::to_string(t + 1u) + ": "
                                + (expected.kept ? blob_difference(expected.blob, blob_now) : std::string("hash differs"));
                        break;
                    }
                    std::vector<uint8_t> bytes;
                    if (expected.kept && !bmmo::game::script_replay::capture(context, scripts, hooks, bytes)) {
                        failure = "follow: script capture error";
                        break;
                    }
                    if (expected.kept && bytes != expected.scripts) {
                        const auto difference = bmmo::game::script_replay::diff(context, scripts, expected.scripts, bytes);
                        failure = "follow: scripts at " + std::to_string(t + 1u) + ": " + difference.where;
                        break;
                    }
                    int in_flight = 0, fired = 0;
                    const std::string links = manager_graph ? describe_delayed_links(manager_graph, in_flight, fired) : "";
                    follow_fired += fired;
                    std::printf("  round %zu: follow tick %u: blob and scripts as the straight pass; manager's delayed"
                                " link(s): %s\n", r, t + 1u, links.c_str());
                }
            }

            if (trafo) {
                const auto crosses = [&](const std::set<uint32_t>& ticks) {
                    for (uint32_t x: ticks)
                        if (x > t0 + static_cast<uint32_t>(a) && x <= t0 + static_cast<uint32_t>(e)) return true;
                    return false;
                };
                if (crosses(trafo_birth_ticks)) ++trafo_rounds_births;
                if (crosses(trafo_death_ticks)) ++trafo_rounds_deaths;
                trafo_recreated += engine.stats().births_recreated - pristine.stats().births_recreated;
                trafo_recreated_exact += engine.stats().births_recreated_exact - pristine.stats().births_recreated_exact;
                trafo_missing += engine.stats().births_missing - pristine.stats().births_missing;
                const uint64_t ended = engine.stats().lives_ended - pristine.stats().lives_ended;
                const auto revived_all = [](const bmmo::session::rollback_stats& s) {   // every kind (own, mirror, mechanism)
                    return s.lives_revived_own + s.lives_revived_mirror + s.lives_revived_mech;
                };
                const uint64_t revived = revived_all(engine.stats()) - revived_all(pristine.stats());
                const uint64_t unrevivable = engine.stats().lives_unrevivable - pristine.stats().lives_unrevivable;
                trafo_lives_ended += ended;
                trafo_lives_revived += revived;
                trafo_lives_unrevivable += unrevivable;
                if (ended) {
                    ++trafo_rounds_lives;
                    if (!unrevivable && revived == ended) ++trafo_rounds_lives_revived;
                }
                if (args.verbose && ended)
                    std::printf("  round %zu: lives ended %llu, revived %llu, unrevivable %llu\n", r,
                                static_cast<unsigned long long>(ended), static_cast<unsigned long long>(revived),
                                static_cast<unsigned long long>(unrevivable));
            }
            if (passes) {   // step S7
                for (uint32_t x: pass_ticks)
                    if (x > t0 + static_cast<uint32_t>(a) && x <= t0 + static_cast<uint32_t>(e)) { ++pass_rounds_crossing; break; }
                pass_recreated += engine.stats().births_recreated - pristine.stats().births_recreated;
                pass_recreated_exact += engine.stats().births_recreated_exact - pristine.stats().births_recreated_exact;
                pass_recreate_failures += engine.stats().birth_recreate_failures - pristine.stats().birth_recreate_failures;
                pass_missing += engine.stats().births_missing - pristine.stats().births_missing;
            }
            if (!premise.empty()) failure = failure.empty() ? "premise: " + premise : failure + " [premise: " + premise + "]";
            char line[1024];
            if (!failure.empty()) {
                ++failed;
                std::snprintf(line, sizeof(line), "FAIL round=%zu anchor=%u depth=%d tick=%u: %s", r,
                              t0 + static_cast<uint32_t>(a), d, t0 + static_cast<uint32_t>(fail_index), failure.c_str());
                std::printf("%s%s\n", line, crosses_reset ? " (the window crosses a script reset)" : "");
                trace.put(line);
                if (!dumped && expected_blob && !args.savestate_trace.empty()) {
                    dumped = true;
                    const std::string base = args.savestate_trace + ".round" + std::to_string(r);
                    const bool ok = write_text_file(base + ".expected.txt",
                                        bmmo::physics::describe_savestate(expected_blob->data(), expected_blob->size()))
                                 && write_text_file(base + ".actual.txt",
                                        bmmo::physics::describe_savestate(current.data(), current.size()));
                    std::printf("  %s both blobs of round %zu: %s.expected.txt / .actual.txt\n",
                                ok ? "wrote" : "could not write", r, base.c_str());
                }
            } else {
                std::snprintf(line, sizeof(line), "round=%zu anchor=%u depth=%d ok end_hash=%016llx%s", r,
                              t0 + static_cast<uint32_t>(a), d, static_cast<unsigned long long>(end_hash.hash),
                              crosses_reset ? " (crosses a script reset)" : "");
                trace.put(line);
                if (args.verbose) std::printf("%s\n", line);
            }
            std::fflush(stdout);
            if (restore_broke) {
                std::printf("the world was salvaged, not restored, after a refused blob (physics_savestate.hpp): "
                            "no further round runs on it\n");
                break;
            }
            if (!failure.empty() && args.stop_on_divergence) {
                std::printf("stopping at the first failed round (--stop-on-divergence)\n");
                break;
            }
        }
        const double rounds_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - rounds_started).count();
        std::fprintf(stderr, "savestate-check: straight pass %.2f s, %d rounds %.2f s\n", straight_seconds, run,
                     rounds_seconds);
        summary(run, failed);
        std::printf("savestate-check: globals (C3) set=%llu replays=%llu replays_start=%llu unknown=%llu failures=%llu"
                    " first_change=%u harness_put_failures=%llu\n",
                    static_cast<unsigned long long>(globals_sum[0]), static_cast<unsigned long long>(globals_sum[1]),
                    static_cast<unsigned long long>(globals_sum[2]), static_cast<unsigned long long>(globals_sum[3]),
                    static_cast<unsigned long long>(globals_sum[4]), pristine.first_globals_change(),
                    static_cast<unsigned long long>(globals_put_failures));
        if (frames)   // step B2SX (Q5)
            std::printf("savestate-check: depth-0 script compares equal but for outputs the restore re-armed (ignored):"
                        " %d round(s), %d bit(s); follow ticks %d, a delayed link's destination on in %d of them\n",
                        rearmed_ignored_rounds, rearmed_ignored_bits, follow_ticks, follow_fired);
        if (trafo) {
            std::printf("savestate-check: trafo births logged %zu, lifetimes changed %zu, inputs %zu, sets %zu\n",
                        trafo_birth_count, trafo_generations.size(), trafo->log().size(), trafo->registry().sets().size());
            std::printf("savestate-check: trafo rounds crossing a piece birth %d, a piece death %d (birth ticks %zu, death"
                        " ticks %zu); bodies destroyed at a restore and made again by the replay %llu, %llu of them exactly at"
                        " their birth record, missing %llu\n", trafo_rounds_births, trafo_rounds_deaths, trafo_birth_ticks.size(),
                        trafo_death_ticks.size(), static_cast<unsigned long long>(trafo_recreated),
                        static_cast<unsigned long long>(trafo_recreated_exact), static_cast<unsigned long long>(trafo_missing));
            std::printf("savestate-check: lives (gap L, L1): rounds with a life ended inside the window %d, every one revived"
                        " in %d; lives ended %llu, revived %llu, unrevivable %llu\n", trafo_rounds_lives,
                        trafo_rounds_lives_revived, static_cast<unsigned long long>(trafo_lives_ended),
                        static_cast<unsigned long long>(trafo_lives_revived),
                        static_cast<unsigned long long>(trafo_lives_unrevivable));
            std::printf("savestate-check: re-made live worlds (step FX2): steps %llu, per-tick globals written %llu\n",
                        static_cast<unsigned long long>(remake_steps), static_cast<unsigned long long>(remake_globals_written));
        }
        if (passes) {   // step S7
            std::printf("savestate-check: passes inside the window %zu (rounds crossing one %d), window births logged %zu,"
                        " activations re-issued %llu; bodies destroyed at a restore and made again by the replay %llu, %llu"
                        " of them exactly at their birth record, recreate failures %llu, missing %llu; live worlds re-made"
                        " %d\n", passes_made, pass_rounds_crossing, pass_birth_count,
                        static_cast<unsigned long long>(pass_reissued), static_cast<unsigned long long>(pass_recreated),
                        static_cast<unsigned long long>(pass_recreated_exact),
                        static_cast<unsigned long long>(pass_recreate_failures), static_cast<unsigned long long>(pass_missing),
                        pass_remakes);
            std::printf("savestate-check: re-made live worlds (step FX2): steps %llu, outside activations re-issued %llu,"
                        " pending the straight pass's frame had not %llu, per-tick globals written %llu\n",
                        static_cast<unsigned long long>(remake_steps), static_cast<unsigned long long>(remake_reissued),
                        static_cast<unsigned long long>(remake_extra_pending),
                        static_cast<unsigned long long>(remake_globals_written));
            std::printf("savestate-check: fixed window births destroyed at a restore for the replayed scripts (step RO1): back"
                        " after their birth tick %llu, missing %llu\n", static_cast<unsigned long long>(fixed_births_back),
                        static_cast<unsigned long long>(fixed_births_missing));
            if (passes_made == 0 && !window_moves.empty())
                std::printf("savestate-check: no --second-move inside the window made a pass\n");
            if (args.savestate_inert_from_birth)
                std::printf("savestate-check: --savestate-inert-from-birth: %d rounds compared from a fixed window birth's"
                            " record on (a diagnostic: no verdict)\n", inert_rounds);
        }
        {
            // Step RLC: the joint ledger (constraints named by their image in
            // the records, made again or replaced at a restore).
            const auto& j = bmmo::game::script_replay::joint_stats();
            std::printf("savestate-check: joints (step RLC): imaged=%llu recreated=%llu replaced=%llu missing=%llu"
                        " unmatched=%llu reordered=%llu order_failed=%llu\n", static_cast<unsigned long long>(j.imaged),
                        static_cast<unsigned long long>(j.recreated), static_cast<unsigned long long>(j.replaced),
                        static_cast<unsigned long long>(j.missing), static_cast<unsigned long long>(j.unmatched),
                        static_cast<unsigned long long>(j.reordered), static_cast<unsigned long long>(j.order_failed));
        }
        const bool pass = failed == 0 && run == static_cast<int>(schedule.size()) && (!trafo || trafo->failures() == 0)
                       && (!passes || ((passes_made > 0 || window_moves.empty()) && pass_recreate_failures == 0
                                       && pass_missing == 0 && fixed_births_missing == 0));
        if (frames) {
            const auto& m = bmmo::game::script_replay::membership();
            std::printf("savestate-check: owner membership at round ends (step RLM): %d of %d round(s) differ from the straight"
                        " pass%s%s; registrations %llu, taken back by a restore %llu, settled %llu\n",
                        membership_differ, membership_rounds, membership_first.empty() ? "" : ", first: ",
                        membership_first.c_str(),
                        static_cast<unsigned long long>(m.reregistered - membership_before.reregistered),
                        static_cast<unsigned long long>(m.unregistered - membership_before.unregistered),
                        static_cast<unsigned long long>(m.settled - membership_before.settled));
            // Step FX1: the replayed frames' pending adds and the restores
            // that took them back.
            std::printf("savestate-check: replayed frames' pending adds (step FX1): made %llu (removes %llu), taken back by a"
                        " restore %llu, kept %llu, not takeable %llu\n",
                        static_cast<unsigned long long>(m.replayed_adds - membership_before.replayed_adds),
                        static_cast<unsigned long long>(m.replayed_removes - membership_before.replayed_removes),
                        static_cast<unsigned long long>(m.replayed_taken_back - membership_before.replayed_taken_back),
                        static_cast<unsigned long long>(m.replayed_kept - membership_before.replayed_kept),
                        static_cast<unsigned long long>(m.replayed_unrecoverable - membership_before.replayed_unrecoverable));
        }
        if (frames)
            std::printf("savestate-check: live script order (step RO1): %zu change(s) recorded, %llu replayed frame(s) ran"
                        " in it\n", static_cast<size_t>(live_order.changes()),
                        static_cast<unsigned long long>(live_orders_applied));
        if (frames) {
            // Step RLQ: the PreSimulate queue binding's counters (the clients'
            // cbq line).
            const auto& q = bmmo::game::script_replay::queue_stats();
            {
                const auto& w = bmmo::game::script_replay::written_cells_stats();   // step RK2
                std::printf("savestate-check: cells (step RK2): captured=%llu columns=%llu rows=%llu restores=%llu "
                            "restored_rows=%llu skipped=%llu\n",
                            static_cast<unsigned long long>(w.captured), static_cast<unsigned long long>(w.captured_columns),
                            static_cast<unsigned long long>(w.captured_rows), static_cast<unsigned long long>(w.restores),
                            static_cast<unsigned long long>(w.rows), static_cast<unsigned long long>(w.skipped));
                std::printf("savestate-check: cells round check (step RK2's review): %zu column(s), %d round(s) checked,"
                            " %d differ, sentinel rows %llu\n", written_cols.size(), cells_checked_rounds,
                            cells_differ_rounds, static_cast<unsigned long long>(cells_sentinel_rows));
            }
            std::printf("savestate-check: cbq (step RLQ, hold %s): straight ticks with outside entries=%d (at most %d)"
                        " captured=%llu rebuilt=%llu removed=%llu held=%llu holds=%llu"
                        " unhold_clamped=%llu rebuild_fail=%llu hold_fail=%llu no_record=%llu\n",
                        args.savestate_no_hold ? "off" : passes ? "+queue section" : "only", outside_ticks, outside_max,
                        static_cast<unsigned long long>(q.captured), static_cast<unsigned long long>(q.rebuilt),
                        static_cast<unsigned long long>(q.removed), static_cast<unsigned long long>(q.held),
                        static_cast<unsigned long long>(q.holds), static_cast<unsigned long long>(q.unhold_clamped),
                        static_cast<unsigned long long>(q.rebuild_fail), static_cast<unsigned long long>(q.hold_fail),
                        static_cast<unsigned long long>(q.no_record));
        }
        std::printf("%s rounds=%d failed=%d\n", args.savestate_inert_from_birth ? "DIAGNOSTIC" : pass ? "PASS" : "FAIL", run,
                    failed);
        std::fflush(stdout);
        return pass && !args.savestate_inert_from_birth ? 0 : 3;
    }

    // Replays a client recording through the physics-session machinery
    // (physics_world + player_navigation) instead of the retail Ball
    // Navigation:
    //   retail-cxx : the retail ball stays live; its navigation leaves are
    //                forced to zero force and the C++ navigation attached to
    //                the retail ball pushes it from the recorded keys.
    //   clone      : the retail ball is parked and a clone is physicalized in
    //                its place (same tick, pose and recipe); the C++
    //                navigation drives the clone.
    // Both feed the full recorded keyboard to the null input manager as well,
    // so the tutorial and every other key-driven script behave as recorded.
    // A bit-exact hash match proves the server-side navigation replica.
    int run_replay_nav(const arguments& args) {
        bmmo::physics::tick_record record;
        std::string error;
        if (!record.load(args.replay, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        const int level = args.level > 0 ? args.level : record.header.level;
        if (level <= 0) {
            std::fprintf(stderr, "the record does not name a level; pass --level N\n");
            return 1;
        }
        const bool clone = args.nav_mode == "clone";
        if (!clone && args.nav_mode != "retail-cxx") {
            std::fprintf(stderr, "--nav must be retail-cxx or clone\n");
            return 2;
        }
        bmmo::sim::world_options options;
        options.game_root = args.root;
        options.level = level;
        options.seed = 1;
        options.boot_ticks = args.boot_ticks;
        options.anchor_timeout = args.anchor_timeout;
        options.park_retail_ball = clone;
        options.auto_clone_players = clone;
        options.zero_retail_force = !clone;
        options.retail_nav_from_script = true;
        options.mirror_clone_to_retail = clone;
        options.placement_clones = args.placement;            // step S2
        options.placement_fail_at = args.placement_fail_at;
        options.log = [](const std::string& text) { std::fprintf(stderr, "%s\n", text.c_str()); };
        auto world = bmmo::sim::physics_world::create(options, error);
        if (!world) {
            std::fprintf(stderr, "world create failed: %s\n", error.c_str());
            return 1;
        }
        if (!world->add_player(1, 0, error)) {
            std::fprintf(stderr, "add_player failed: %s\n", error.c_str());
            return 1;
        }
        if (!clone && !world->attach_player_to_retail_ball(1, error)) {
            std::fprintf(stderr, "attach failed: %s\n", error.c_str());
            return 1;
        }
        const auto& graph = world->navigation();
        for (const auto& leaf: graph.leaves)
            std::fprintf(stderr, "leaf %d: key=%d direction=(%g,%g,%g) force=%g\n", leaf.index, leaf.key,
                leaf.direction.x, leaf.direction.y, leaf.direction.z, leaf.force_value);
        std::fprintf(stderr, "record: level=%d frames=%zu mode=%s anchor_hash=%016llx\n", record.header.level,
            record.frames.size(), args.nav_mode.c_str(), static_cast<unsigned long long>(world->anchor_hash()));

        size_t matched = 0, pose_matched = 0;
        long long first_divergence = -1, first_pose_divergence = -1;
        const size_t frames = args.nav_frames >= 0
            ? std::min(record.frames.size(), static_cast<size_t>(args.nav_frames)) : record.frames.size();
        for (size_t frame = 0; frame < frames; ++frame) {
            const auto& expected = record.frames[frame];
            world->engine().set_keyboard_state(expected.keys.data());
            bmmo::session::input_frame input{};
            input.keys = graph.keys_from_state(expected.keys.data());
            if (world->retail_navigation_active()) input.flags |= bmmo::session::INPUT_FLAG_NAV_ACTIVE;
            if (CK3dEntity* ref = world->retail_direction_ref()) {
                const VxMatrix& m = ref->GetWorldMatrix();
                for (int k = 0; k < 3; ++k) {
                    input.cam_right[k] = m[0][k];
                    input.cam_up[k] = m[1][k];
                    input.cam_dir[k] = m[2][k];
                }
            }
            world->set_input(1, input);
            if (!world->tick(error)) {
                std::fprintf(stderr, "tick failed at frame %zu: %s\n", frame, error.c_str());
                return 1;
            }
            bmmo::physics::world_hash actual;
            if (!bmmo::physics::capture_world_hash(world->physics(), actual, error)) {
                std::fprintf(stderr, "hash failed: %s\n", error.c_str());
                return 1;
            }
            const bool same = actual.hash == expected.hash;
            if (same) ++matched;
            if (actual.pose == expected.pose) ++pose_matched;
            else if (first_pose_divergence < 0) first_pose_divergence = static_cast<long long>(frame);
            if (!same && first_divergence < 0) {
                first_divergence = static_cast<long long>(frame);
                std::printf("DIVERGE frame=%zu expected=%016llx actual=%016llx pose=%s cores=%d/%d ivp_time=%.6f/%.6f\n"
                            "    probe expected %s pos=(%.9g,%.9g,%.9g) speed=(%.9g,%.9g,%.9g)\n"
                            "    probe actual   %s pos=(%.9g,%.9g,%.9g) speed=(%.9g,%.9g,%.9g)\n    %s\n    bodies: %s\n",
                    frame, static_cast<unsigned long long>(expected.hash), static_cast<unsigned long long>(actual.hash),
                    expected.pose == actual.pose ? "same" : "DIFF", expected.cores, actual.cores,
                    expected.ivp_time, actual.ivp_time, expected.probe_name,
                    expected.probe_position[0], expected.probe_position[1], expected.probe_position[2],
                    expected.probe_speed[0], expected.probe_speed[1], expected.probe_speed[2], actual.probe_name,
                    actual.probe_position[0], actual.probe_position[1], actual.probe_position[2],
                    actual.probe_speed[0], actual.probe_speed[1], actual.probe_speed[2],
                    world->describe().c_str(), bmmo::physics::describe_physics_objects(world->physics()).c_str());
            }
            if (args.report_every > 0 && (frame % static_cast<size_t>(args.report_every)) == 0)
                std::printf("frame=%zu %s keys=%02x nav=%d cores=%d/%d ivp_time=%.6f pose=%s seed=%d mc=%d psi=%.6f/%.6f dpos=(%.3g,%.3g,%.3g) %s\n",
                    frame, same ? "ok" : "MISMATCH", input.keys,
                    (input.flags & bmmo::session::INPUT_FLAG_NAV_ACTIVE) ? 1 : 0, expected.cores, actual.cores,
                    actual.ivp_time, expected.pose == actual.pose ? "same" : "DIFF", actual.ivp_seed,
                    static_cast<int>(actual.next_movement_check), actual.time_of_last_psi, actual.time_of_next_psi,
                    actual.probe_position[0] - expected.probe_position[0],
                    actual.probe_position[1] - expected.probe_position[1],
                    actual.probe_position[2] - expected.probe_position[2],
                    world->describe().c_str());
        }
        std::printf("summary: mode=%s frames=%zu matched=%zu first_divergence=%lld pose_matched=%zu first_pose_divergence=%lld\n",
            args.nav_mode.c_str(), frames, matched, first_divergence, pose_matched, first_pose_divergence);
        std::fflush(stdout);
        return first_divergence < 0 ? 0 : 3;
    }

    // ------------------------------------------------------- session journal
    // The offline side of the session black box (design 9.15).  A server
    // journal holds everything its physics_world consumed - the members with
    // their join orders, every input frame the world was actually fed, every
    // client-reported lifecycle event as it was applied - plus the fingerprint
    // of every tick and a full body checkpoint now and then.  Feeding those
    // back into a world built from the same header reproduces the room bit for
    // bit; the first tick whose hash disagrees is the bug.

    const char* event_type_name(bmmo::session::event_type type) {
        switch (type) {
            case bmmo::session::event_type::Physicalize: return "Physicalize";
            case bmmo::session::event_type::Unphysicalize: return "Unphysicalize";
            case bmmo::session::event_type::Sector: return "Sector";
            case bmmo::session::event_type::Finish: return "Finish";
            case bmmo::session::event_type::BodyRevived: return "BodyRevived";
            case bmmo::session::event_type::Trafo: return "Trafo";
        }
        return "unknown";
    }

    const char* correction_kind_name(uint8_t kind) {
        static const char* names[] = {"mismatch", "rollback", "hard", "blend", "resync", "too_far", "frozen", "unmatched"};
        return kind < sizeof(names) / sizeof(names[0]) ? names[kind] : "?";
    }

    // A RESYNC record's `outcome` (journal.hpp JOURNAL_RESYNC_*).
    const char* resync_outcome_name(uint8_t outcome) {
        switch (outcome) {
            case bmmo::session::JOURNAL_RESYNC_APPLIED: return "applied";
            case bmmo::session::JOURNAL_RESYNC_REFUSED_SCRIPTS: return "applied-scripts-refused";
            case bmmo::session::JOURNAL_RESYNC_TIMED_OUT: return "timed-out";
            default: return "?";
        }
    }

    // A header from a machine with a broken clock, or a corrupted one, carries
    // an utc_ms no calendar holds.  Unguarded that kills the whole listing:
    // MSVC's strftime calls the invalid parameter handler on the tm gmtime_s
    // refused to fill in, and the process dies before a single buffered line
    // reaches the console.  Print the raw number instead - every record after
    // the header is still worth reading, which is the point of --list.
    std::string utc_string(uint64_t utc_ms) {
        const std::time_t seconds = static_cast<std::time_t>(utc_ms / 1000);
        std::tm parts{};
#ifdef _WIN32
        if (gmtime_s(&parts, &seconds) != 0)
#else
        if (gmtime_r(&seconds, &parts) == nullptr)
#endif
            return "utc_ms=" + std::to_string(utc_ms) + " (out of range)";
        char stamp[32] = {};
        if (std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &parts) == 0)
            return "utc_ms=" + std::to_string(utc_ms) + " (out of range)";
        char text[64] = {};
        std::snprintf(text, sizeof(text), "%s.%03uZ", stamp, static_cast<unsigned>(utc_ms % 1000));
        return text;
    }

    // The engine half of a build id ("ballanced-<sha>" before the '+'): the
    // part that has to match for a replay to mean anything.
    std::string engine_build(const std::string& build_id) {
        const size_t plus = build_id.find('+');
        return plus == std::string::npos ? build_id : build_id.substr(0, plus);
    }

    // The journal's event back into the world's own struct: the mirror of
    // session_runner.cpp's to_journal_event (journal.hpp deliberately knows
    // nothing about the server's headers).
    bmmo::sim::lifecycle_event to_lifecycle_event(const bmmo::session::journal_event& e) {
        bmmo::sim::lifecycle_event out;
        out.type = e.type;
        out.ball_type = e.ball_type;
        out.flags = e.flags;
        for (int k = 0; k < 3; ++k) out.position[k] = e.position[k];
        for (int k = 0; k < 9; ++k) out.rotation[k] = e.rotation[k];
        out.recipe = e.recipe;
        out.sector = e.sector;
        out.name = e.name;
        // The tick the world was handed in the event, not necessarily the one
        // the record is filed under: the spawn impulse direction is derived
        // from this field.  A server journal of protocol 9 (journal v3) has the
        // applied tick here, the one every side kicked the ball with; an older
        // one has the stamp, which is what that server used.  The reader
        // already resolved a file written before the split (no trailing u32)
        // to the applied tick, so 0 here means an event stamped for tick 0.
        out.tick = e.event_tick;
        return out;
    }

    // --list: the whole file in a screen, without booting the engine.  This is
    // the triage pass on a journal fetched from a live server, so it streams:
    // read_journal would expand a file at the 256 MB cap into some 13 million
    // structs, and every line here needs only the record in front of it.  The
    // counting summary is printed after the lines instead of before them - the
    // price of not holding the file in memory.
    int list_journal(const std::string& path) {
        bmmo::session::journal_header h;
        std::string error;
        uint64_t dropped = 0, unknown = 0, delivered = 0;
        size_t tick_records = 0, inputs = 0;
        uint32_t first_ms = 0, last_ms = 0, low_tick = 0, high_tick = 0;
        bool have_ms = false, have_tick = false, seen_tick_record = false;
        // The renumbering a resync leaves behind: the highest tick a TICK
        // record has reached, and the records that came back below it.
        uint32_t high_tick_record = 0, first_duplicate = 0;
        size_t duplicate_ticks = 0;
        std::vector<uint32_t> group_ticks;   // deduped against the previous entry, sorted at the end
        std::vector<uint32_t> full_checkpoints, local_checkpoints, received_checkpoints;
        uint64_t parse_error_record = 0;
        int parse_error_tag = 0;
        std::printf("=== %s\n", path.c_str());
        // The header record is parsed by scan_journal itself, so the first
        // callback already knows it: print it from there, once.
        bool header_printed = false;
        const auto print_header = [&] {
            if (header_printed) return;
            header_printed = true;
            std::printf("header: %s session=%u level=%d seed=%d impulse=%.3f input_delay=%u checkpoint_ticks=%u"
                        " first_tick=%u\n",
                h.kind == bmmo::session::journal_kind::server ? "server" : "client", h.session, h.level, h.seed,
                static_cast<double>(h.spawn_impulse), h.input_delay, h.checkpoint_ticks, h.first_tick);
            std::printf("        anchor hash=%016llx surfaces=%016llx build=%s\n",
                static_cast<unsigned long long>(h.anchor_hash), static_cast<unsigned long long>(h.anchor_surfaces),
                h.build_id.c_str());
            std::printf("        utc=%s own_player=%u own_join_order=%u\n", utc_string(h.utc_ms).c_str(),
                h.own_player, static_cast<unsigned>(h.own_join_order));
        };
        auto on_record = [&](bmmo::session::journal_tag tag, const std::string& payload) {
            print_header();
            bmmo::session::journal_detail::cursor in(payload);
            const auto seen_tick = [&](uint32_t tick) {
                if (!have_tick) { low_tick = high_tick = tick; have_tick = true; }
                low_tick = std::min(low_tick, tick);
                high_tick = std::max(high_tick, tick);
                if (group_ticks.empty() || group_ticks.back() != tick) group_ticks.push_back(tick);
            };
            switch (tag) {
                case bmmo::session::journal_tag::player: {
                    bmmo::session::journal_player p;
                    p.tick = in.u32();
                    p.id = in.u32();
                    p.join_order = in.u8();
                    p.added = in.u8() != 0;
                    p.name = in.str();
                    if (!in.ok) break;
                    seen_tick(p.tick);
                    // A founding member (added at first_tick, before any TICK
                    // record) gets the member line as well as its own record.
                    if (!seen_tick_record && p.added && p.tick == h.first_tick)
                        std::printf("member: p%u join=%u \"%s\"\n", p.id, static_cast<unsigned>(p.join_order),
                            p.name.c_str());
                    std::printf("tick %-8u player %cp%u join=%u \"%s\"\n", p.tick, p.added ? '+' : '-', p.id,
                        static_cast<unsigned>(p.join_order), p.name.c_str());
                    ++delivered;
                    return true;
                }
                case bmmo::session::journal_tag::input: {
                    // Counted, never printed: the inputs are the bulk of the
                    // file and no human reads them one per line.
                    const uint32_t tick = in.u32();
                    if (!in.ok) break;
                    seen_tick(tick);
                    ++inputs;
                    ++delivered;
                    return true;
                }
                case bmmo::session::journal_tag::event: {
                    bmmo::session::journal_event e;
                    e.tick = in.u32();
                    e.id = in.u32();
                    e.type = static_cast<bmmo::session::event_type>(in.u8());
                    e.ball_type = in.u8();
                    e.flags = in.u8();
                    in.f32_array(e.position, 3);
                    in.f32_array(e.rotation, 9);
                    e.sector = in.i32();
                    e.name = in.str();
                    if (!in.ok || !bmmo::session::journal_detail::read_recipe(in, e.recipe)) break;
                    e.event_tick = in.size - in.pos >= 4 ? in.u32() : e.tick;
                    e.origin_tick = in.size - in.pos >= 4 ? in.u32() : e.event_tick;
                    e.state = in.size - in.pos >= 1 ? in.u8() : 0;
                    if (!in.ok) break;
                    seen_tick(e.tick);
                    // "stamped S applied T" only when they differ: that gap is
                    // an event that reached the server after its own tick, and
                    // is itself worth seeing.  Since journal v3 the stamp is
                    // the origin (the server re-stamps a late event, protocol
                    // 9); an older file has it in event_tick.
                    char stamp[64] = {};
                    const uint32_t stamped = e.origin_tick != e.tick ? e.origin_tick : e.event_tick;
                    if (stamped != e.tick)
                        std::snprintf(stamp, sizeof(stamp), " stamped %u applied %u", stamped, e.tick);
                    if (e.state != 0) {
                        char state[24] = {};
                        std::snprintf(state, sizeof(state), " state=%02x", static_cast<unsigned>(e.state));
                        std::strncat(stamp, state, sizeof(stamp) - std::strlen(stamp) - 1);
                    }
                    std::printf("tick %-8u event p%u %s ball=%u flags=%02x sector=%d pos=(%.3f,%.3f,%.3f) name=%s%s\n",
                        e.tick, e.id, event_type_name(e.type), static_cast<unsigned>(e.ball_type),
                        static_cast<unsigned>(e.flags), e.sector, static_cast<double>(e.position[0]),
                        static_cast<double>(e.position[1]), static_cast<double>(e.position[2]), e.name.c_str(), stamp);
                    ++delivered;
                    return true;
                }
                case bmmo::session::journal_tag::tick: {
                    const uint32_t tick = in.u32();
                    in.u64();
                    in.u64();
                    in.i32();
                    const uint32_t ms = in.u32();
                    if (!in.ok) break;
                    seen_tick(tick);
                    // A client renumbers its ticks at a resync, so the same
                    // tick can carry two fingerprints and read_journal keeps
                    // only the last of each.  Streaming cannot remember which
                    // ticks already had a record, but a TICK that comes back
                    // at or below the highest one seen IS that renumbering,
                    // and on a real resync journal this counts what
                    // read_journal counts.
                    if (seen_tick_record && tick <= high_tick_record) {
                        if (duplicate_ticks++ == 0) first_duplicate = tick;
                    }
                    high_tick_record = std::max(high_tick_record, tick);
                    seen_tick_record = true;
                    ++tick_records;
                    last_ms = ms;
                    if (!have_ms) { first_ms = ms; have_ms = true; }
                    ++delivered;
                    return true;
                }
                case bmmo::session::journal_tag::checkpoint: {
                    const uint32_t tick = in.u32();
                    const uint8_t flags = in.u8();
                    if (!in.ok) break;
                    seen_tick(tick);
                    if (flags & bmmo::session::JOURNAL_CHECKPOINT_FULL) full_checkpoints.push_back(tick);
                    if (flags & bmmo::session::JOURNAL_CHECKPOINT_LOCAL) local_checkpoints.push_back(tick);
                    if (flags & bmmo::session::JOURNAL_CHECKPOINT_RECEIVED) received_checkpoints.push_back(tick);
                    ++delivered;
                    return true;
                }
                case bmmo::session::journal_tag::note: {
                    bmmo::session::journal_note n;
                    n.tick = in.u32();
                    n.text = in.str();
                    if (!in.ok) break;
                    seen_tick(n.tick);
                    std::printf("tick %-8u note %s\n", n.tick, n.text.c_str());
                    ++delivered;
                    return true;
                }
                case bmmo::session::journal_tag::correction: {
                    bmmo::session::journal_correction c;
                    c.tick = in.u32();
                    c.local_tick = in.u32();
                    c.kind = in.u8();
                    c.entity = in.str();
                    c.error_m = in.f32();
                    c.velocity_error = in.f32();
                    in.f64_array(c.local_position, 3);
                    in.f64_array(c.server_position, 3);
                    if (!in.ok) break;
                    seen_tick(c.tick);
                    std::printf("tick %-8u correction %s local=%u %s error=%.4f m dv=%.3f local=(%.3f,%.3f,%.3f)"
                                " server=(%.3f,%.3f,%.3f)\n",
                        c.tick, correction_kind_name(c.kind), c.local_tick, c.entity.c_str(),
                        static_cast<double>(c.error_m), static_cast<double>(c.velocity_error), c.local_position[0],
                        c.local_position[1], c.local_position[2], c.server_position[0], c.server_position[1],
                        c.server_position[2]);
                    ++delivered;
                    return true;
                }
                case bmmo::session::journal_tag::resync: {
                    // Version 4 (resync design 5): the state a client applied
                    // as tick S.  Its byte strings are sized, not printed.
                    const uint32_t tick = in.u32();
                    const uint32_t base = in.u32();
                    const int32_t delta = static_cast<int32_t>(in.u32());
                    const uint32_t applied_at = in.u32();
                    const uint8_t outcome = in.u8();
                    uint32_t sizes[3] = {};
                    for (uint32_t& size: sizes) {
                        size = in.u32();
                        std::string skipped;
                        if (!in.ok || !in.bytes(skipped, size)) {
                            in.ok = false;
                            break;
                        }
                    }
                    if (!in.ok) break;
                    seen_tick(tick);
                    std::printf("tick %-8u resync base=%u delta=%+d applied_at=%u %s union=%u scripts=%u savestate=%u bytes\n",
                        tick, base, delta, applied_at, resync_outcome_name(outcome), sizes[0], sizes[1], sizes[2]);
                    ++delivered;
                    return true;
                }
                case bmmo::session::journal_tag::wait: {
                    // Version 7 (step R12): the server's wait in a player's
                    // relay header from `tick` on (a client's: the header's
                    // server tick).
                    const uint32_t tick = in.u32();
                    const uint32_t player = in.u32();
                    const uint8_t wait = in.u8();
                    if (!in.ok) break;
                    seen_tick(tick);
                    std::printf("tick %-8u wait player=%u %u ticks\n", tick, player, static_cast<unsigned>(wait));
                    ++delivered;
                    return true;
                }
                case bmmo::session::journal_tag::header:
                default:
                    break;
            }
            // The header is record 1 and this one is not counted yet: the same
            // 1-based position read_journal reports.
            parse_error_record = 2 + delivered + unknown;
            parse_error_tag = static_cast<int>(tag);
            return false;
        };
        if (!bmmo::session::scan_journal(path, h, on_record, error, &dropped, &unknown)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        print_header();   // an empty file still has a header to show
        std::sort(group_ticks.begin(), group_ticks.end());
        group_ticks.erase(std::unique(group_ticks.begin(), group_ticks.end()), group_ticks.end());
        std::error_code ec;
        const uintmax_t file_size = std::filesystem::file_size(path, ec);
        const uint64_t read = !ec && file_size >= dropped ? static_cast<uint64_t>(file_size) - dropped : 0;
        std::printf("ticks: %u..%u groups=%zu tick_records=%zu inputs=%zu records=%llu unknown=%llu"
                    " read=%llu dropped=%llu\n",
            low_tick, high_tick, group_ticks.size(), tick_records, inputs,
            static_cast<unsigned long long>(1 + delivered + unknown), static_cast<unsigned long long>(unknown),
            static_cast<unsigned long long>(read), static_cast<unsigned long long>(dropped));
        if (have_ms)
            std::printf("wall clock: first tick +%u ms, last tick +%u ms after %s\n", first_ms, last_ms,
                utc_string(h.utc_ms).c_str());
        std::string warning;
        const auto warn = [&](const std::string& text) {
            if (!warning.empty()) warning += "; ";
            warning += text;
        };
        if (parse_error_record != 0)
            warn("record " + std::to_string(parse_error_record) + " (tag " + std::to_string(parse_error_tag)
               + ") does not parse");
        if (dropped != 0) warn("truncated: " + std::to_string(dropped) + " bytes dropped");
        if (unknown != 0) warn(std::to_string(unknown) + " unknown records skipped");
        if (duplicate_ticks != 0)
            warn(std::to_string(duplicate_ticks) + " duplicate TICK records (first at tick "
               + std::to_string(first_duplicate) + ", kept the last of each)");
        if (!warning.empty()) std::printf("warning: %s\n", warning.c_str());
        const auto print_checkpoints = [](const char* what, const std::vector<uint32_t>& ticks) {
            if (ticks.empty()) return;
            std::string line;
            for (size_t i = 0; i < ticks.size() && i < 24; ++i) line += (i ? ", " : "") + std::to_string(ticks[i]);
            if (ticks.size() > 24) line += ", ...";
            std::printf("checkpoints %s: %zu [%s]\n", what, ticks.size(), line.c_str());
        };
        print_checkpoints("FULL", full_checkpoints);
        print_checkpoints("LOCAL", local_checkpoints);
        print_checkpoints("RECEIVED", received_checkpoints);
        std::fflush(stdout);
        return 0;
    }

    // How a recorded checkpoint compares with the same world now.
    struct checkpoint_diff {
        size_t compared = 0, differing = 0, missing = 0, extra = 0, renumbered = 0, unresolved = 0;
        double worst = 0.0;
        std::string worst_name;
        std::string worst_detail;
        std::string first_missing;   // the identity of a recorded body the replay does not have
        std::string first_extra;     // ... and of a replayed body the recording does not have
    };

    // What to call a body in a report: its name when it has one, and what it is
    // otherwise (a delta snapshot's rows carry no names).
    std::string body_label(const bmmo::session::body_state& body, const std::string& name) {
        if (!name.empty()) return name;
        return (body.kind == bmmo::session::body_kind::Ball ? "ball of p" : "mechanism #")
             + std::to_string(body.owner);
    }

    // Bodies are matched by name where there is one and by (kind, owner)
    // otherwise.  A mechanism's `owner` is only the number the run that wrote
    // the record happened to give it - the server numbers a mechanism when one
    // of its own snapshots first carries it, the replay when one of ITS
    // snapshots does, and a delta row carries no name to catch the difference -
    // so a nameless mechanism row is resolved through `dictionary`, the names
    // the file's own full snapshots taught us, before the replay's numbering is
    // touched at all.  A row that resolves to nothing is counted as unresolved
    // rather than compared: it must never pass for a match or a mismatch.
    checkpoint_diff compare_checkpoint(const std::vector<bmmo::session::body_state>& recorded,
                                       const std::vector<bmmo::session::body_state>& replayed,
                                       const std::map<uint32_t, std::string>& dictionary) {
        checkpoint_diff diff;
        std::map<std::pair<uint8_t, uint32_t>, size_t> by_key;
        std::map<std::string, size_t> by_name;
        for (size_t i = 0; i < replayed.size(); ++i) {
            by_key.emplace(std::make_pair(static_cast<uint8_t>(replayed[i].kind), replayed[i].owner), i);
            if (!replayed[i].name.empty()) by_name.emplace(replayed[i].name, i);
        }
        std::vector<bool> used(replayed.size(), false);
        for (const auto& body: recorded) {
            std::string name = body.name;
            if (name.empty() && body.kind == bmmo::session::body_kind::Mechanism) {
                auto known = dictionary.find(body.owner);
                if (known == dictionary.end()) { ++diff.unresolved; continue; }
                name = known->second;
            }
            size_t index = replayed.size();
            auto key = by_key.find(std::make_pair(static_cast<uint8_t>(body.kind), body.owner));
            if (!name.empty()) {
                auto named = by_name.find(name);
                if (named != by_name.end()) {
                    index = named->second;
                    if (key == by_key.end() || key->second != index) ++diff.renumbered;
                }
            }
            if (index == replayed.size() && key != by_key.end()) {
                const auto& other = replayed[key->second];
                if (name.empty() || other.name.empty()) index = key->second;
            }
            if (index == replayed.size()) {
                ++diff.missing;
                if (diff.first_missing.empty()) diff.first_missing = body_label(body, name);
                continue;
            }
            used[index] = true;
            ++diff.compared;
            const auto& other = replayed[index];
            double dp = 0.0, dv = 0.0, da = 0.0, dr = 0.0;
            for (int k = 0; k < 3; ++k) {
                const double p = body.position[k] - other.position[k];
                const double v = static_cast<double>(body.linear[k]) - static_cast<double>(other.linear[k]);
                const double a = static_cast<double>(body.angular[k]) - static_cast<double>(other.angular[k]);
                dp += p * p;
                dv += v * v;
                da += a * a;
            }
            for (int k = 0; k < 4; ++k) dr = std::max(dr, std::abs(body.rotation[k] - other.rotation[k]));
            dp = std::sqrt(dp);
            dv = std::sqrt(dv);
            da = std::sqrt(da);
            const bool differs = dp > 1e-6 || dv > 1e-4 || da > 1e-4 || dr > 1e-6 || body.flags != other.flags;
            // Only a body that actually disagrees can be the worst offender:
            // naming one that matches to the last bit hides the real problem,
            // which is usually a body that is missing altogether.
            if (differs) ++diff.differing;
            if (differs && dp >= diff.worst) {
                diff.worst = dp;
                diff.worst_name = other.name.empty() ? name : other.name;
                if (diff.worst_name.empty()) diff.worst_name = body_label(body, name);
                char detail[320];
                std::snprintf(detail, sizeof(detail),
                    "dp=%.6f dv=%.4f da=%.4f drot=%.6f flags=%02x/%02x recorded=(%.4f,%.4f,%.4f) replayed=(%.4f,%.4f,%.4f)",
                    dp, dv, da, dr, static_cast<unsigned>(body.flags), static_cast<unsigned>(other.flags),
                    body.position[0], body.position[1], body.position[2],
                    other.position[0], other.position[1], other.position[2]);
                diff.worst_detail = detail;
            }
        }
        for (size_t i = 0; i < replayed.size(); ++i) {
            if (used[i]) continue;
            ++diff.extra;
            if (diff.first_extra.empty()) diff.first_extra = body_label(replayed[i], replayed[i].name);
        }
        return diff;
    }

    // ------------------------------------------------------------------
    // --replay-session through a resync (docs/resync-and-sector-timing-design.md
    // 4.5 and 5, step T1).  A client journal replays the SERVER's view (its own
    // frames as sent and substituted, the relayed ones, the events); at a
    // resync the client renumbered and took the server's state of tick S, and
    // its RESYNC record carries that state as it was received: the union, the
    // portable script record and the server's IVP savestate, with the rows in
    // the RECEIVED|FULL|RESYNC checkpoint of the same tick.  The replay steps
    // tick S as it stepped every tick before, compares what it has with that
    // state ("before the restore": the blob and the rows), then makes the
    // world S's and goes on from S + 1 with the journal's records:
    //
    //   structure  the balls: a player the rows have and the world has not
    //              physicalized is physicalized from its last birth the
    //              replay applied (at the row's place, no spawn kick: the
    //              blob sets its state); one the world has and the rows do
    //              not is unphysicalized.  Mechanism bodies are only counted
    //              (the server's and not the replay's, and the other way
    //              round): a module the replay never physicalized cannot be
    //              made at S, and the replay after S is then not the
    //              server's world - said, never hidden.
    //   scripts    the server's record localized against the replay's own v2
    //              record with NO script kept local (the replay is the
    //              server's world, the balloon and the trafo included) and
    //              restored with replay_activation (every script, active or
    //              not, and the three data cells), the restored-active ones
    //              re-registered with the behaviour manager (whole world ticks
    //              follow, not a replay's execute_frame);
    //   blob       the server's savestate as a travelling one (another
    //              process's environment), nothing excluded, a clone renamed
    //              only where this world's CK name differs from the blob's
    //              and exactly one blob name can be it;
    //   rows       written only where the restored body differs from its row
    //              (a body the blob did not know); a live body the blob did
    //              not know and no row covers is rechecked back in;
    //   counters   the world's tick := S + 1, its RNG cursors as the blob left
    //              the generators, the PreSimulate callback not pending, the
    //              sector union the server's (sector_union::deserialize).
    //
    // A restore that is refused, or leaves the world not S's (a body only one
    // side has, a ball with no birth or whose event failed, the scripts not
    // restored, an ambiguous clone, a row unmapped, or a row written over a
    // body the blob restored - which the RESYNC checkpoint, compared after
    // the write, could not see), is a divergence: rc 3, and
    // --stop-on-divergence stops there.  A refused restore that excused a
    // jump in the numbering stops the replay unless --continue-after-jump.
    //
    // What the record does not carry stays the replay's own: every player's
    // navigation (the keys held and the force controllers their press made -
    // the wire's SessionResyncState has it, the journal record does not), the
    // players' sectors and the snapshot numbering.  A replay that had every
    // input before S has those exactly; one that did not (a late joiner's
    // journal, a relayed frame that never came) may not.
    // ------------------------------------------------------------------
    int rc_unregistered_scripts(CKContext* context, const bmmo::game::script_replay::selection& sel, bool fix,
                                std::string* names);   // defined with --resync-check below

    // Step B2SW (resync finding G1): a restore of the tools that stands for
    // a client's runs with the core hosts' rule - the script restore
    // re-registers what it writes back active (script_replay
    // live_membership), as the hosts' bindings ask for.
    struct live_membership_scope {
        bool previous;
        explicit live_membership_scope(bool on) : previous(bmmo::game::script_replay::live_membership()) {
            bmmo::game::script_replay::live_membership() = on;
        }
        ~live_membership_scope() { bmmo::game::script_replay::live_membership() = previous; }
        live_membership_scope(const live_membership_scope&) = delete;
        live_membership_scope& operator=(const live_membership_scope&) = delete;
    };

    struct replay_resync_tools {
        bool ready = false;
        bmmo::game::script_replay::selection sel;
        bmmo::game::script_replay_hooks hooks;
        std::unique_ptr<bmmo::physics::physics_direct> backend;
        bmmo::session::rollback_world binding;

        void prepare(bmmo::sim::physics_world& world) {
            if (ready) return;
            ready = true;
            CKContext* context = world.engine().context();
            CKIpionManager* physics = world.physics();
            sel = bmmo::game::session_script_selection(context);
            hooks = bmmo::game::force_hooks_for(bmmo::physics::physics_direct(physics));   // step HU1: one copy (game/host_backend.hpp)
            backend = std::make_unique<bmmo::physics::physics_direct>(physics);
            bmmo::session::rollback_binding_options options;
            options.delta_ms = bmmo::sim::kFixedDeltaMs;
            options.savestate = false;   // the blob is restored directly, as a travelling one
            binding = bmmo::session::bind_rollback_world(*backend, options);
        }
    };

    struct replay_resync_result {
        std::string before_blob;      // the replay's world at S vs the server's blob: "" identical, else the first differences
        std::string before_blob_ov;   // ... with the overlap tree and the order bookkeeping aside
        bool have_rows = false;
        checkpoint_diff before_rows;  // the rows against the replay's world at S, before the restore
        uint32_t made = 0, unmade = 0, unborn = 0;   // balls physicalized, unphysicalized, rowed with no birth to make
        uint32_t events_failed = 0;   // a ball the reconcile could not make or unmake
        std::vector<std::string> missing, extra;     // mechanism bodies: the server's only, the replay's only
        std::string scripts;          // what became of the script record
        bool scripts_restored = false;
        int unregistered = 0;
        std::string unregistered_names;
        bmmo::physics::savestate_report blob;
        std::vector<std::pair<std::string, std::string>> renames;
        uint32_t renames_ambiguous = 0;   // a clone with more than one blob name it could be: not renamed
        uint32_t rows_written = 0, rows_equal = 0, rows_unmapped = 0, unknown_rechecked = 0;
        // Rows written over a body the blob restored: the blob did not make
        // it the server's row, so the restore itself is wrong there (the
        // write hides it from the RESYNC checkpoint, which then cannot fail).
        uint32_t rows_restore_differ = 0;
        std::string active;           // the union's active sectors after the restore

        // The world after the restore is not the server's world of S.
        bool incomplete() const {
            return !missing.empty() || !extra.empty() || unborn != 0 || events_failed != 0 || !scripts_restored
                || renames_ambiguous != 0 || rows_unmapped != 0 || rows_restore_differ != 0;
        }
    };

    // The server's names of the ball clones in a blob: "object <name> ..."
    // lines of describe_savestate whose name carries the clone tag.
    std::set<std::string> blob_clone_names(const std::string& blob) {
        std::set<std::string> out;
        const std::string text = bmmo::physics::describe_savestate(reinterpret_cast<const uint8_t*>(blob.data()), blob.size());
        size_t pos = 0;
        while (pos < text.size()) {
            const size_t end = text.find('\n', pos);
            const size_t stop = end == std::string::npos ? text.size() : end;
            if (text.compare(pos, 7, "object ") == 0) {
                const size_t space = text.find(' ', pos + 7);
                const std::string name = text.substr(pos + 7, (space == std::string::npos || space > stop ? stop : space) - pos - 7);
                if (name.find("_BMMO_") != std::string::npos) out.insert(name);
            }
            pos = stop + 1;
        }
        return out;
    }

    // Puts the server's state of tick r.tick into the replayed world (see the
    // note above).  False when the world could not be made S's; `error` says
    // why, and whether the world was touched at all (`touched`): a refused
    // union or blob framing leaves it as it was, a failed script or blob
    // restore does not.
    bool restore_replay_resync(bmmo::sim::physics_world& world, replay_resync_tools& tools,
                               const bmmo::session::journal_resync& r,
                               const std::vector<bmmo::session::body_state>* rows,
                               const std::map<uint32_t, bmmo::session::journal_event>& births,
                               const std::function<void(uint32_t)>& ensure_player, replay_resync_result& result,
                               bool& touched, std::string& error) {
        using bmmo::game::script_replay;
        using bmmo::session::body_kind;
        touched = false;
        CKContext* context = world.engine().context();
        CKIpionManager* physics = world.physics();
        if (!context || !physics) {
            error = "no physics manager";
            return false;
        }
        const uint32_t S = r.tick;
        // Everything that can be refused without touching the world first.
        bmmo::session::sector_union::state_type server_union;
        if (r.union_state.empty()
                || !bmmo::session::sector_union::deserialize(reinterpret_cast<const uint8_t*>(r.union_state.data()),
                                                             r.union_state.size(), server_union)) {
            error = "the union state does not read (" + std::to_string(r.union_state.size()) + " bytes)";
            return false;
        }
        const auto* blob = reinterpret_cast<const uint8_t*>(r.savestate.data());
        std::string e;
        if (r.savestate.empty() || !bmmo::physics::check_savestate(blob, r.savestate.size(), e)) {
            error = "the savestate does not read (" + std::to_string(r.savestate.size()) + " bytes" + (e.empty() ? "" : ": " + e) + ")";
            return false;
        }
        tools.prepare(world);

        // Before the restore: the replay's own world at S against the state.
        std::vector<uint8_t> own;
        if (bmmo::physics::get_savestate(physics, own, e)) {
            // Identical past the 16-byte header (magic, format version and
            // the capturing environment's identity, which is another
            // process's by definition: x64 and x86 blobs of one world are
            // identical past it too).
            constexpr size_t kBlobHeader = 16;
            const bool same = own.size() == r.savestate.size() && own.size() >= kBlobHeader
                           && std::memcmp(own.data() + kBlobHeader, blob + kBlobHeader, own.size() - kBlobHeader) == 0;
            if (!same)
                result.before_blob = bmmo::physics::diff_savestate(blob, r.savestate.size(), own.data(), own.size(), false, 3, false);
            if (!result.before_blob.empty())
                result.before_blob_ov = bmmo::physics::diff_savestate(blob, r.savestate.size(), own.data(), own.size(), true, 3, true);
        } else {
            result.before_blob = "the replay's own savestate does not capture: " + e;
        }
        std::vector<bmmo::session::body_state> mine;
        world.snapshot_for_journal(mine);
        static const std::map<uint32_t, std::string> no_dictionary;
        if (rows) {
            result.have_rows = true;
            result.before_rows = compare_checkpoint(*rows, mine, no_dictionary);
        }

        touched = true;
        // The structure: the balls.
        std::set<uint32_t> ball_rows;
        std::set<std::string> mechanism_rows;
        if (rows)
            for (const auto& row: *rows) {
                if (row.kind == body_kind::Ball) ball_rows.insert(row.owner);
                else mechanism_rows.insert(row.name);
            }
        if (rows) {
            for (uint32_t id: world.player_ids()) {
                if (!world.player_physicalized(id) || ball_rows.count(id)) continue;
                bmmo::sim::lifecycle_event death;
                death.type = bmmo::session::event_type::Unphysicalize;
                death.tick = S;
                if (world.apply_event(id, death, e)) {
                    ++result.unmade;
                } else {
                    ++result.events_failed;
                    std::printf("tick %u: resync: unphysicalizing p%u failed: %s\n", S, id, e.c_str());
                }
            }
            for (const auto& row: *rows) {
                if (row.kind != body_kind::Ball) continue;
                ensure_player(row.owner);
                if (!world.has_player(row.owner) || world.player_physicalized(row.owner)) continue;
                const auto birth = births.find(row.owner);
                if (birth == births.end()) {
                    ++result.unborn;
                    continue;
                }
                bmmo::sim::lifecycle_event made = to_lifecycle_event(birth->second);
                made.flags = static_cast<uint8_t>(made.flags & ~bmmo::session::PHYSICALIZE_FLAG_SPAWN);
                for (int k = 0; k < 3; ++k) made.position[k] = static_cast<float>(row.position[k]);
                made.tick = S;
                if (world.apply_event(row.owner, made, e)) {
                    ++result.made;
                } else {
                    ++result.events_failed;
                    std::printf("tick %u: resync: physicalizing p%u failed: %s\n", S, row.owner, e.c_str());
                }
            }
            // The mechanisms are counted, not made (see the note above).
            std::set<std::string> mine_mechanisms;
            for (const auto& body: mine)
                if (body.kind == body_kind::Mechanism) mine_mechanisms.insert(body.name);
            for (const auto& name: mechanism_rows)
                if (!mine_mechanisms.count(name)) result.missing.push_back(name);
            for (const auto& name: mine_mechanisms)
                if (!mechanism_rows.count(name)) result.extra.push_back(name);
        }

        // The scripts: the server's record over the replay's own.
        if (r.scripts.empty()) {
            result.scripts = "EMPTY, the replay's own kept";
        } else {
            std::vector<uint8_t> v2, localized;
            const std::vector<uint8_t> portable(r.scripts.begin(), r.scripts.end());
            script_replay::portable_report report;
            if (!script_replay::capture(context, tools.sel, tools.hooks, v2)) {
                result.scripts = "the replay's own record does not capture: the replay's own kept";
            } else if (!script_replay::localize(context, tools.sel, portable, &v2, script_replay::keep_local_fn(), localized, &e,
                                                &report)) {
                result.scripts = "REFUSED (" + e + "): the replay's own kept";
            } else if (const live_membership_scope live(true);   // step B2SW (G1): the clients' rule
                       !script_replay::restore(context, tools.sel, tools.hooks, localized, &e, nullptr, nullptr, true)) {
                error = "the script restore failed: " + e;
                return false;
            } else {
                result.scripts_restored = true;
                result.scripts = "restored (" + std::to_string(portable.size()) + " bytes, unportable "
                               + std::to_string(report.unportable) + ", " + std::to_string(report.objects_resolved)
                               + " objects resolved)";
            }
            result.unregistered = rc_unregistered_scripts(context, tools.sel, true, &result.unregistered_names);
        }

        // The blob, travelling.  The clones are named alike on every server
        // world (CK's own name bump included); a rename only where this
        // world's clone of a rowed player is not among the blob's.  The
        // journal record has no clone list (the wire's `clones` is not
        // recorded), so the pairing is a guess, made only where it cannot
        // pair the wrong two: never with a rowed ball left unmade (its clone
        // is no candidate's owner here, so its blob name would be free to be
        // taken by another), and never between two candidates.
        bmmo::physics::savestate_travel travel;
        if (result.unborn == 0) {
            const std::set<std::string> theirs = blob_clone_names(r.savestate);
            std::set<std::string> ours, taken;
            for (uint32_t id: ball_rows) {
                const std::string name = world.player_ball_entity(id);
                if (!name.empty()) ours.insert(name);
            }
            for (uint32_t id: ball_rows) {
                const std::string name = world.player_ball_entity(id);
                if (name.empty() || theirs.count(name)) continue;
                // The same "<ball>_BMMO_" prefix, the lowest suffix at or
                // above the id that no clone of this world has.
                const size_t tag = name.rfind("_BMMO_");
                if (tag == std::string::npos) continue;
                const std::string prefix = name.substr(0, tag + 6);
                std::string best;
                size_t candidates = 0;
                for (const auto& candidate: theirs) {
                    if (candidate.compare(0, prefix.size(), prefix) != 0 || ours.count(candidate) || taken.count(candidate)) continue;
                    const unsigned long long suffix = std::strtoull(candidate.c_str() + prefix.size(), nullptr, 10);
                    if (suffix < id) continue;
                    ++candidates;
                    best = candidate;
                }
                if (candidates > 1) {
                    ++result.renames_ambiguous;
                    std::printf("tick %u: resync: the clone %s of p%u has %zu blob names it could be: not renamed\n", S,
                        name.c_str(), id, candidates);
                    continue;
                }
                if (best.empty()) continue;
                taken.insert(best);
                travel.renames.emplace_back(best, name);
            }
            result.renames = travel.renames;
        }
        if (!bmmo::physics::set_savestate(physics, blob, r.savestate.size(), 0u, e, &result.blob, travel)) {
            error = "the server's savestate was refused: " + e;
            return false;
        }

        // The rows: only where the restored body is not its row.
        std::set<std::string> unknown(result.blob.unknown_bodies.begin(), result.blob.unknown_bodies.end());
        std::set<std::string> rowed;
        if (rows) {
            for (const auto& row: *rows) {
                const std::string entity = row.kind == body_kind::Ball ? world.player_ball_entity(row.owner) : row.name;
                bmmo_physics_body_state live{};
                if (entity.empty() || !tools.binding.get_body(entity, live)) {
                    ++result.rows_unmapped;
                    continue;
                }
                rowed.insert(entity);
                bmmo_physics_body_state state{};
                for (int k = 0; k < 3; ++k) {
                    state.position[k] = row.position[k];
                    state.linear[k] = row.linear[k];
                    state.angular[k] = row.angular[k];
                }
                for (int k = 0; k < 4; ++k) state.rotation[k] = row.rotation[k];
                state.simulated = (row.flags & bmmo::session::BODY_FLAG_SIMULATED) != 0;
                if (!unknown.count(entity) && std::memcmp(state.position, live.position, sizeof state.position) == 0
                        && std::memcmp(state.rotation, live.rotation, sizeof state.rotation) == 0
                        && std::memcmp(state.linear, live.linear, sizeof state.linear) == 0
                        && std::memcmp(state.angular, live.angular, sizeof state.angular) == 0
                        && state.simulated == live.simulated) {
                    ++result.rows_equal;
                    continue;
                }
                if (!unknown.count(entity)) ++result.rows_restore_differ;
                if (tools.binding.set_body(entity, state, state.simulated ? bmmo::session::wake_mode::wake
                                                                           : bmmo::session::wake_mode::freeze, true))
                    ++result.rows_written;
            }
        }
        for (const auto& name: unknown)
            if (!rowed.count(name) && tools.binding.recheck_body(name)) ++result.unknown_rechecked;

        // The world's counters: tick S done, the RNG as the blob left it.
        auto counters = world.capture_step_state();
        counters.tick = S + 1;
        counters.rng_cursor = ivp_srand_read();
        counters.random_cursor = bmmo::physics::random_get_state();
        counters.callback_pending = false;
        counters.sectors.restore(server_union);
        world.restore_step_state(counters);
        for (int sector: world.active_sectors()) result.active += (result.active.empty() ? "" : ",") + std::to_string(sector);
        // The restore's own physics events are no tick's.
        bmmo::physics::drain_event_log(physics);
        return true;
    }

    int run_replay_session(const arguments& args) {
        // --list streams the file and never builds the in-memory journal: it
        // is the pass that has to work on a 256 MB box fetched from a server.
        if (args.list_journal) return list_journal(args.replay_session);
        bmmo::session::journal journal;
        std::string error;
        if (!bmmo::session::read_journal(args.replay_session, journal, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        if (!journal.warning.empty())
            std::fprintf(stderr, "journal warning: %s\n", journal.warning.c_str());
        if (args.root.empty()) {
            std::fprintf(stderr, "--replay-session needs --root <game dir> (only --list works without it)\n");
            return 2;
        }
        if (journal.ticks.empty()) {
            std::fprintf(stderr, "the journal has no ticks to replay\n");
            return 1;
        }
        const auto& header = journal.header;
        const bool client_journal = header.kind == bmmo::session::journal_kind::client;
        if (client_journal) {
            std::printf("client journal of player %u: replaying the SERVER's view from what this client sent and\n"
                        "received.  Its own INPUT records are what it sent, not necessarily what the server applied,\n"
                        "and its TICK hashes are of its own world, so only the received snapshots are compared.\n",
                        header.own_player);
            std::printf("client journal: same-tick events applied in join-order order, the server's rule\n");
            // Its own events the server applied elsewhere or never are in it
            // twice, as sent and as echoed (late-player design 3.3): the last
            // record of each is the server's world.
            const size_t superseded_records = bmmo::session::journal_keep_last_events(journal);
            if (superseded_records != 0)
                std::printf("client journal: %zu event records replaced by a later record of the same event (the"
                            " server's echo: moved, superseded or rejected)\n", superseded_records);
        }
        const std::string build = bmmo::physics::build_id();
        if (engine_build(build) != engine_build(header.build_id))
            std::printf("warning: engine build differs: journal %s, this tool %s\n",
                header.build_id.c_str(), build.c_str());

        bmmo::sim::world_options options;
        options.game_root = args.root;
        options.level = header.level > 0 ? header.level : (args.level > 0 ? args.level : 1);
        options.seed = header.seed;
        options.spawn_impulse = header.spawn_impulse;
        options.trace = args.journal_trace;
        options.boot_ticks = args.boot_ticks;
        options.anchor_timeout = args.anchor_timeout;
        // Journal v5 (protocol 11): the session's world options.  The trafo
        // inputs build every player's piece set at its PLAYER record and run
        // the Trafo EVENT records (the server's own resets among them).
        options.trafo_inputs = (header.world_options & bmmo::session::WORLD_TRAFO_INPUTS) != 0;
        // Journal v6 (protocol 12, sector placement design 5, step S5): the
        // placement copies when the session's world option built them.
        options.placement_clones = (header.world_options & bmmo::session::WORLD_PLACEMENT_CLONES) != 0;
        // Step G2 (world-agreement-gaps-design.md §2.4): the proximity
        // union's rule, placed at the top of each stepped tick, before its
        // PLAYER and EVENT records - the server's point.
        // Server journals only until G3: a client journal of a bit-on
        // session carries the server's world options (client_journal.cpp),
        // but no client runs the rule before G3, so its replay does not.
        // Step G3: a client journal too - the clients run the same rule at
        // the same point since G3 (a client journal carries the server's
        // world options, client_journal.cpp).  Under the bit a BodyRevived
        // record is not applied (step G5, physics_world::apply_event).
        options.proximity_union = (header.world_options & bmmo::session::WORLD_PROXIMITY_UNION) != 0;
        // Journal v8 (step Q7, world-agreement-gaps-design.md 5): K slot
        // copies of every per-player unit, bound per tick by join order.
        options.mechanism_slots = options.proximity_union && (header.world_options & bmmo::session::WORLD_PLAYER_MECHANISMS)
            ? header.mechanism_slots : 0;
        // Step RK3 (protocol 20): the Level 1 tutorial's exit at frame 10 -
        // the server's world and every client world take it at the same
        // frame after their anchor, so a server or a client journal replays
        // with it.
        options.session_clock = (header.world_options & bmmo::session::WORLD_SESSION_CLOCK) != 0;
        if (header.world_options != 0) std::printf("journal world options %08x%s%s%s%s%s\n", header.world_options,
                                                   options.trafo_inputs ? " (trafo inputs)" : "",
                                                   options.placement_clones ? " (placement clones)" : "",
                                                   options.proximity_union ? " (proximity union)" : "",
                                                   options.mechanism_slots ? " (player mechanisms)" : "",
                                                   options.session_clock ? " (session clock)" : "");
        if (options.mechanism_slots) std::printf("journal mechanism slots %u\n", static_cast<unsigned>(options.mechanism_slots));
        options.log = [](const std::string& text) { std::fprintf(stderr, "%s\n", text.c_str()); };
        auto world = bmmo::sim::physics_world::create(options, error);
        if (!world) {
            std::fprintf(stderr, "world create failed: %s\n", error.c_str());
            return 1;
        }
        // Journal v6: the replay's world must hold the objects the recorded
        // one did.  A header without a signature (an older file, or a client
        // whose host does not compute it) is not compared.
        if (header.anchor_placement != 0 && header.anchor_placement != world->anchor_placement()) {
            std::fprintf(stderr, "journal placement %016llx, replay %016llx: the replay's world does not hold the "
                                 "recorded world's objects (refused)\n",
                         static_cast<unsigned long long>(header.anchor_placement),
                         static_cast<unsigned long long>(world->anchor_placement()));
            return 1;
        }
        // Wrong game data is the first thing to check when everything diverges
        // from the very first tick, so it is checked before the very first tick.
        if (header.anchor_hash != 0 && header.anchor_hash != world->anchor_hash())
            std::printf("warning: anchor hash %016llx, journal says %016llx (different game data or engine build)\n",
                static_cast<unsigned long long>(world->anchor_hash()),
                static_cast<unsigned long long>(header.anchor_hash));
        if (header.anchor_surfaces != 0 && header.anchor_surfaces != world->anchor_surfaces())
            std::printf("warning: anchor surfaces %016llx, journal says %016llx (different game data)\n",
                static_cast<unsigned long long>(world->anchor_surfaces()),
                static_cast<unsigned long long>(header.anchor_surfaces));

        // Step SP8, the networked invariant (design 7.4): the watched sector's
        // rows traced after every replayed tick, in the drop scene's format.
        if (!args.placement_trace.empty()) {
            if (const int rc = open_scene_trace(args, *world)) return rc;
            std::printf("placement trace: sector %d rows to %s%s\n", args.drop_sector, args.placement_trace.c_str(),
                        args.placement_trace_player ? (" (and p" + std::to_string(args.placement_trace_player)
                                                       + "'s ball)").c_str() : "");
        }
        size_t sectors_dropped = 0;
        if (args.drop_sectors_above >= 0)
            std::printf("--drop-sectors-above %d: every Sector event of a higher sector is skipped\n",
                        args.drop_sectors_above);

        // --write-journal: this replay's own box, so the same journal replayed
        // on two platforms can be diffed with scripts/journal_trace.py --diff.
        bmmo::session::journal_writer out;
        const auto started = std::chrono::steady_clock::now();
        if (!args.write_journal.empty()) {
            bmmo::session::journal_header out_header;
            out_header.kind = bmmo::session::journal_kind::server;
            out_header.session = header.session;
            out_header.level = world->level();
            out_header.seed = options.seed;
            out_header.spawn_impulse = options.spawn_impulse;
            out_header.input_delay = header.input_delay;
            out_header.checkpoint_ticks = header.checkpoint_ticks;
            out_header.first_tick = journal.ticks.front().tick;
            out_header.anchor_hash = world->anchor_hash();
            out_header.anchor_surfaces = world->anchor_surfaces();
            out_header.build_id = build;
            out_header.world_options = header.world_options;
            out_header.anchor_placement = world->anchor_placement();   // journal v6
            out_header.utc_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            const std::filesystem::path path = args.write_journal;
            std::error_code ec;
            if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
            std::string open_error;
            // No poster: synchronous, so --io-flush's journal mode is
            // passed for the record and does not apply (step IOF).
            if (!out.open(path, out_header, 0, open_error, {}, bmmo::session::kJournalMaxQueued,
                          args.io_flush.journal)) {
                std::fprintf(stderr, "%s\n", open_error.c_str());
                return 1;
            }
            out.note(out_header.first_tick, "start: replay of " + args.replay_session);
            std::printf("writing the replay's journal to %s\n", path.string().c_str());
        }

        // The last tick the recording side really ran: past it only the
        // leaving players and the end note remain (or, after a crash, nothing).
        uint32_t last_sim_tick = journal.ticks.front().tick;
        for (const auto& group: journal.ticks)
            if (group.has_tick || !group.inputs.empty() || !group.events.empty()) last_sim_tick = group.tick;
        // Where the recording's world took its FIRST step.  A session numbers
        // its world from the members' start base (design 9.25 phase alignment),
        // so a server journal's first simulated tick is that base and the
        // groups before it carry only notes and PLAYER records - not a hole in
        // the recording.  The replayed world is renamed to it before the first
        // step, exactly as the runner renames the live one, so the tick numbers
        // printed here are the recording's and no gap is reported.  A journal
        // whose world did start at 0 (every file written before this change,
        // and every client journal, whose anchor era is numbered from 0) is
        // unaffected: the first simulated group is at 0 and nothing is set.
        uint32_t replay_first_tick = journal.ticks.front().tick;
        for (const auto& group: journal.ticks)
            if (group.has_tick || !group.inputs.empty() || !group.events.empty()) {
                replay_first_tick = group.tick;
                break;
            }
        if (replay_first_tick != world->tick_index()) {
            std::printf("the journal's world starts at tick %u (header first_tick=%u); numbering the replay from there\n",
                replay_first_tick, header.first_tick);
            world->set_tick_index(replay_first_tick);
        }

        size_t simulated = 0, matched = 0, compared = 0, checkpoints = 0, checkpoint_mismatches = 0;
        long long first_divergence = -1, first_checkpoint_divergence = -1;
        std::string dump_before;   // --dump-entity: the tick before the divergence
        bool warned_offset = false;
        bool stopped_at_jump = false;
        std::vector<bmmo::session::body_state> bodies;
        // The players the replayed world knows, and the join orders they hold:
        // a client journal only ever names the members its own SessionStart
        // listed, so an id that turns up in a relayed input or event has to be
        // added here or its ball never exists in the replay.
        std::map<uint32_t, uint8_t> replay_players;
        // Mechanism index -> name, learned from every recorded row that carries
        // one.  Delta snapshot rows are nameless and their index is the
        // recording's own numbering, which is not this replay's.
        std::map<uint32_t, std::string> dictionary;
        uint32_t last_written_tick = journal.ticks.front().tick;
        bool have_previous_group = false;
        uint32_t previous_group = 0;
        // The RESYNC records (step T1): the ticks S whose record carries a
        // state to restore, the last birth of every player the replay applied
        // (a ball the state has and the world lacks is made from it), and
        // what the restores did.
        std::vector<uint32_t> resync_states;
        size_t resync_records = 0;
        for (const auto& group: journal.ticks)
            for (const auto& r: group.resyncs) {
                ++resync_records;
                if (r.outcome != bmmo::session::JOURNAL_RESYNC_TIMED_OUT && !r.savestate.empty()
                        && (resync_states.empty() || resync_states.back() != group.tick))
                    resync_states.push_back(group.tick);
            }
        const bool restore_resyncs = !args.ignore_resync && !resync_states.empty();
        // A hole this close before a state's tick is that resync's
        // renumbering: the state replaces whatever the world steps up to S.
        constexpr uint32_t kResyncJumpReach = 256;
        const auto resync_ahead = [&](uint32_t from) -> long long {
            for (uint32_t s: resync_states)
                if (s >= from && s - from <= kResyncJumpReach) return static_cast<long long>(s);
            return -1;
        };
        std::map<uint32_t, bmmo::session::journal_event> last_births;
        size_t pre_first_groups = 0;   // step B2SW (T1 b): groups before the first simulated one, not stepped
        replay_resync_tools resync_tools;
        size_t resyncs_restored = 0, resyncs_refused = 0, resyncs_incomplete = 0, resyncs_timed_out = 0,
               resyncs_no_state = 0, resync_before_identical = 0, resync_before_differ = 0, checkpoints_after_resync = 0,
               mismatches_after_resync = 0;
        long long first_resync = -1, first_resync_failure = -1;
        // The state tick S that excused a jump in the numbering without
        // --continue-after-jump: if its restore is refused, the replay stops
        // there as the jump itself used to stop it.
        long long jump_excused_by = -1;
        // A player that has records but no PLAYER record of its own (a late
        // join the recording side was never told about).  The join order is a
        // guess - the world falls back to a free slot anyway - and the guess
        // decides the spawn impulse direction, so it is said out loud.
        const auto ensure_player = [&](uint32_t id, uint32_t tick) {
            if (id == 0 || replay_players.count(id)) return;
            uint8_t order = 0;
            while (order < 63) {
                bool taken = false;
                for (const auto& [other, other_order]: replay_players) {
                    (void) other;
                    if (other_order == order) { taken = true; break; }
                }
                if (!taken) break;
                ++order;
            }
            if (!world->add_player(id, order, error)) {
                std::printf("tick %u: add_player %u failed: %s\n", tick, id, error.c_str());
                return;
            }
            replay_players[id] = order;
            std::printf("note: p%u has records but no PLAYER record (late join); added with join order %u\n",
                id, static_cast<unsigned>(order));
            if (out.is_open()) out.player(tick, id, order, true, {});
        };
        for (const auto& group: journal.ticks) {
            for (const auto& n: group.notes) std::printf("tick %u note: %s\n", n.tick, n.text.c_str());
            for (const auto& c: group.corrections)
                std::printf("tick %u correction: %s local=%u %s error=%.4f m\n", c.tick,
                    correction_kind_name(c.kind), c.local_tick, c.entity.c_str(), static_cast<double>(c.error_m));
            for (const auto& cp: group.checkpoints) {
                // A LOCAL checkpoint is the client's own body list, where every
                // owner is 0: it knows names but no numbering, and letting it
                // near the dictionary would rename index 0 every 660 ticks.
                if (cp.flags & bmmo::session::JOURNAL_CHECKPOINT_LOCAL) continue;
                for (const auto& body: cp.bodies)
                    if (body.kind == bmmo::session::body_kind::Mechanism && !body.name.empty())
                        dictionary[body.owner] = body.name;
            }
            if (group.tick > last_sim_tick) continue;   // the session's tail
            // Step G2: under WORLD_PROXIMITY_UNION the union is placed before
            // the tick's PLAYER and EVENT records, as the server's step does
            // it before its leavers and its events; not for a group the world
            // does not step (the anchor era below).
            const bool anchor_era = !args.legacy_first_step && group.tick < replay_first_tick && !group.has_tick
                && group.inputs.empty() && group.events.empty();
            if (options.proximity_union && !anchor_era) world->place_probes();
            for (const auto& p: group.players) {
                if (p.added) {
                    if (!world->add_player(p.id, p.join_order, error, p.life))
                        std::printf("tick %u: add_player %u failed: %s\n", p.tick, p.id, error.c_str());
                    else
                        replay_players[p.id] = p.join_order;
                } else {
                    world->remove_player(p.id);
                    replay_players.erase(p.id);
                }
                if (out.is_open()) out.player(p.tick, p.id, p.join_order, p.added, p.name, p.life);
            }
            // Resync finding T1 (b) (step B2SW): a group before the world's
            // first simulated one carries only notes and PLAYER records - a
            // client journal's anchor era, numbered from 0 - and is not a
            // tick the recording side stepped.  Stepping it ran the world one
            // empty tick ahead (numbered as the first simulated one), so no
            // TICK hash of a client journal's replay equalled the server's
            // before its first resync restore.  Its records are applied; the
            // world does not step for it.  (--legacy-first-step: the old
            // replay, for the A/B.)
            if (!args.legacy_first_step && group.tick < replay_first_tick && !group.has_tick && group.inputs.empty()
                    && group.events.empty()) {
                ++pre_first_groups;
                continue;
            }
            // The world ticks once per group, so a hole in the numbering leaves
            // it behind for the rest of the file.  A client's numbering restarts
            // on a late join and on every resync, which is exactly why this has
            // to run for a client journal too.
            if (!warned_offset && world->tick_index() != group.tick) {
                warned_offset = true;
                std::printf("warning: the world is at tick %u where the journal is at %u; the recording has a gap\n",
                    world->tick_index(), group.tick);
            }
            // A hole in the numbering, not only between two ticks that both
            // have a TICK record: a late joiner's journal has its anchor era
            // under the provisional base 0 and everything after the assignment
            // hundreds of ticks higher, with nothing in between.
            if (have_previous_group && group.tick != previous_group + 1) {
                const long long resync_at = restore_resyncs && group.tick > previous_group ? resync_ahead(group.tick) : -1;
                std::printf("warning: the journal jumps from tick %u to tick %u (a late join or a resync restarts the"
                            " numbering); everything after this point is compared against a world that is %lld ticks"
                            " behind\n", previous_group, group.tick,
                    static_cast<long long>(group.tick) - static_cast<long long>(world->tick_index()));
                if (resync_at >= 0) {
                    std::printf("the jump is a resync's renumbering: the server's state of tick %lld (its RESYNC record)"
                                " is restored there and the replay goes on from it\n", resync_at);
                    if (!args.continue_after_jump) jump_excused_by = resync_at;
                } else if (!args.continue_after_jump) {
                    std::printf("stopping at the jump (--continue-after-jump replays past it anyway)\n");
                    stopped_at_jump = true;
                    break;
                }
            }
            have_previous_group = true;
            previous_group = group.tick;
            for (const auto& in: group.inputs) {
                ensure_player(in.id, group.tick);
                world->set_input(in.id, in.frame);
                if (out.is_open()) out.input(in.tick, in.id, in.frame, in.flags);
            }
            // The server applies a tick's events in the members' join order,
            // and one player's by origin and then type rank (session_runner::
            // step, late-player design 3.1), and that order is what a server
            // journal's file order already IS.  A client wrote them in the
            // order they reached it - its own first, the relayed ones after -
            // so a client journal has to be sorted back into the server's rule
            // here, from the v3 record's origin, or the replay creates two
            // balls of the same tick the other way round, or a re-stamped
            // birth after its death, and diverges from the session for good.
            std::vector<const bmmo::session::journal_event*> ordered;
            ordered.reserve(group.events.size());
            for (const auto& e: group.events) ordered.push_back(&e);
            if (client_journal && ordered.size() > 1) {
                const auto join_rank = [&replay_players](uint32_t id) {
                    const auto it = replay_players.find(id);
                    return it == replay_players.end() ? 256u : static_cast<uint32_t>(it->second);
                };
                std::stable_sort(ordered.begin(), ordered.end(),
                    [&join_rank](const bmmo::session::journal_event* a, const bmmo::session::journal_event* b) {
                        return bmmo::session::lifecycle_apply_before(join_rank(a->id), a->origin_tick, a->type,
                                                                     join_rank(b->id), b->origin_tick, b->type);
                    });
            }
            for (const auto* entry: ordered) {
                const auto& e = *entry;
                if (static_cast<uint8_t>(e.type) > static_cast<uint8_t>(bmmo::session::event_type::Trafo)) {
                    // A newer writer's event type: the world would not know
                    // what to do with it, and guessing would be worse.
                    std::printf("tick %u: event type %u from p%u is unknown to this build, skipped\n", e.tick,
                        static_cast<unsigned>(e.type), e.id);
                    continue;
                }
                if (args.drop_sectors_above >= 0 && e.type == bmmo::session::event_type::Sector
                        && static_cast<int>(e.sector) > args.drop_sectors_above) {
                    // Skipped before ensure_player: a player whose first event
                    // is a dropped Sector event is created at its next event
                    // in this arm (none in the L8/L11 server journals).
                    ++sectors_dropped;
                    std::printf("tick %u: Sector %d event from p%u dropped (--drop-sectors-above %d)\n", e.tick,
                                static_cast<int>(e.sector), e.id, args.drop_sectors_above);
                    continue;
                }
                if (out.is_open()) out.event(e);
                ensure_player(e.id, group.tick);
                const bmmo::sim::lifecycle_event event = to_lifecycle_event(e);
                // The gap between the two ticks is the event arriving after the
                // tick it asked for: worth seeing, because a divergence here is
                // about the recording, not about the physics.  Since journal v3
                // the stamp is the origin and the world is handed the applied
                // tick (the server re-stamps, protocol 9).
                if (event.tick != e.tick)
                    std::printf("tick %u: %s event from p%u was stamped for tick %u\n", e.tick,
                        event_type_name(e.type), e.id, event.tick);
                else if (e.origin_tick != e.tick)
                    std::printf("tick %u: %s event from p%u stamped for tick %u, re-stamped to its applied tick\n",
                        e.tick, event_type_name(e.type), e.id, e.origin_tick);
                if (!world->apply_event(e.id, event, error)) {
                    std::printf("tick %u: %s event from p%u failed: %s\n", e.tick, event_type_name(e.type), e.id,
                        error.c_str());
                    if (out.is_open()) out.note(e.tick, "event failed: " + error);
                } else if (e.type == bmmo::session::event_type::Physicalize) {
                    last_births[e.id] = e;
                }
            }
            if (!args.dump_entity.empty()) dump_before = describe_entities(world->engine(), args.dump_entity);
            if (!world->tick(error)) {
                std::fprintf(stderr, "tick %u failed: %s\n", group.tick, error.c_str());
                // The one case where the reason matters most is the one that
                // must not leave the written box without an end note.
                if (out.is_open()) {
                    out.note(group.tick, "end: replay stopped: tick " + std::to_string(group.tick) + " failed: " + error);
                    out.close();
                }
                return 1;
            }
            ++simulated;
            last_written_tick = group.tick;
            // Step SFD: BMMO_REPLAY_SAVESTATE_DUMP=FROM,UNTIL,DIR writes the
            // stepped world's savestate blob of every tick in [FROM, UNTIL] to
            // DIR/<tick>.sav - the server's side of a client's --dump-savestate,
            // for --savestate-diff / --savestate-describe (the hidden collision
            // state a row does not show).
            if (const char* spec = std::getenv("BMMO_REPLAY_SAVESTATE_DUMP"); spec && *spec) {
                unsigned from = 0, until = 0;
                char dir[1024] = {0};
                if (std::sscanf(spec, "%u,%u,%1023[^\n]", &from, &until, dir) == 3 && group.tick >= from && group.tick <= until) {
                    std::vector<uint8_t> blob;
                    std::string dump_error;
                    if (bmmo::physics::get_savestate(world->physics(), blob, dump_error)) {
                        std::error_code ec;
                        std::filesystem::create_directories(dir, ec);
                        const std::filesystem::path path = std::filesystem::path(dir) / (std::to_string(group.tick) + ".sav");
                        if (FILE* f = std::fopen(path.string().c_str(), "wb")) {
                            std::fwrite(blob.data(), 1, blob.size(), f);
                            std::fclose(f);
                        }
                    } else {
                        std::fprintf(stderr, "tick %u: savestate dump: %s\n", group.tick, dump_error.c_str());
                    }
                }
            }
            // Step SP8: the stepped world, before a RESYNC record below replaces
            // it - a client journal's resync tick traces the replay's own state,
            // not the server's.  The invariant replays server journals, which
            // carry no RESYNC records, so both arms trace the same thing.
            if (const auto& trace = scene_trace()) {
                if (args.placement_trace_player) trace->set_ball(world->player_ball_entity(args.placement_trace_player));
                trace->write(group.tick);
            }
            // A client's RESYNC record of this tick (step T1): the server's
            // state of S replaces the world the replay stepped to S, and the
            // replay goes on from it.  The record's checkpoint (flag RESYNC)
            // is compared below like every received one - after the restore,
            // so it checks the restore; the replay's own world before it is
            // compared here.
            for (const auto& r: group.resyncs) {
                if (r.outcome != bmmo::session::JOURNAL_RESYNC_TIMED_OUT && !r.savestate.empty()) continue;
                if (r.outcome == bmmo::session::JOURNAL_RESYNC_TIMED_OUT) ++resyncs_timed_out;
                else ++resyncs_no_state;
                std::printf("tick %u: RESYNC (base %u, delta %+d, at %u) %s: no state to restore\n", group.tick, r.base,
                    r.delta, r.applied_at, resync_outcome_name(r.outcome));
            }
            const bmmo::session::journal_resync* resync_state = nullptr;
            if (restore_resyncs)
                for (const auto& r: group.resyncs)
                    if (r.outcome != bmmo::session::JOURNAL_RESYNC_TIMED_OUT && !r.savestate.empty()) resync_state = &r;
            // A restore that failed, was left incomplete, or (at a second or
            // later record) found the replay off the server's state: a
            // divergence of the replay for the return code and for
            // --stop-on-divergence.  restored_here: this tick's RESYNC
            // checkpoint checks the restore that just wrote its rows, so it
            // is not counted among the checkpoints after the restore.
            bool resync_bad = false, restored_here = false, stop_at_refused_jump = false;
            const auto resync_failed = [&](uint32_t tick) {
                resync_bad = true;
                if (first_resync_failure < 0) first_resync_failure = static_cast<long long>(tick);
            };
            if (resync_state) {
                const auto& r = *resync_state;
                const std::vector<bmmo::session::body_state>* rows = nullptr;
                for (const auto& cp: group.checkpoints)
                    if (cp.flags & bmmo::session::JOURNAL_CHECKPOINT_RESYNC) rows = &cp.bodies;
                replay_resync_result rr;
                bool touched = false;
                std::string resync_error;
                const bool ok = restore_replay_resync(*world, resync_tools, r, rows, last_births,
                    [&](uint32_t id) { ensure_player(id, group.tick); }, rr, touched, resync_error);
                std::string before = rr.before_blob.empty() ? "blob IDENTICAL (past the header's environment identity)"
                                                            : "blob differs: " + rr.before_blob;
                if (!rr.before_blob_ov.empty()) before += " (overlap tree and order aside: " + rr.before_blob_ov + ")";
                if (rr.have_rows) {
                    char text[200];
                    std::snprintf(text, sizeof(text), "; rows: %zu compared, %zu differ, %zu only the server's, %zu only the"
                                  " replay's", rr.before_rows.compared, rr.before_rows.differing, rr.before_rows.missing,
                                  rr.before_rows.extra);
                    before += text;
                    if (!rr.before_rows.worst_name.empty())
                        before += ", worst " + rr.before_rows.worst_name + " " + rr.before_rows.worst_detail;
                } else {
                    before += "; no rows (no RESYNC checkpoint at this tick)";
                }
                std::printf("tick %u: RESYNC (base %u, delta %+d, applied by the client at %u, %s): the replay's world"
                            " before the restore against the server's state: %s\n", group.tick, r.base, r.delta,
                    r.applied_at, resync_outcome_name(r.outcome), before.c_str());
                // Once the replay runs from a restored state, the next state
                // is a check of it: anything but an identical blob is a
                // divergence of the replay after the RESYNC record.
                if (rr.before_blob.empty()) ++resync_before_identical;
                else ++resync_before_differ;
                if (resyncs_restored != 0 && !rr.before_blob.empty()) {
                    ++checkpoint_mismatches;
                    ++mismatches_after_resync;
                    if (first_checkpoint_divergence < 0) first_checkpoint_divergence = static_cast<long long>(group.tick);
                    resync_bad = true;
                }
                if (!ok) {
                    ++resyncs_refused;
                    resync_failed(group.tick);
                    std::printf("tick %u: RESYNC NOT RESTORED: %s%s\n", group.tick, resync_error.c_str(),
                        touched ? "" : " (the replay's world is left as it was)");
                    if (!touched && jump_excused_by == static_cast<long long>(group.tick)) stop_at_refused_jump = true;
                    if (touched) {
                        // Half restored: no tick after this one means anything.
                        std::fprintf(stderr, "tick %u: the resync restore failed after it changed the world: %s\n",
                            group.tick, resync_error.c_str());
                        if (out.is_open()) {
                            out.note(group.tick, "end: replay stopped: the resync restore of tick "
                                   + std::to_string(group.tick) + " failed: " + resync_error);
                            out.close();
                        }
                        return 1;
                    }
                } else {
                    ++resyncs_restored;
                    restored_here = true;
                    if (first_resync < 0) first_resync = static_cast<long long>(group.tick);
                    std::string missing, extra, renames;
                    for (size_t i = 0; i < rr.missing.size() && i < 4; ++i) missing += " " + rr.missing[i];
                    for (size_t i = 0; i < rr.extra.size() && i < 4; ++i) extra += " " + rr.extra[i];
                    for (const auto& [from, to]: rr.renames) renames += " " + from + "->" + to;
                    std::printf("tick %u: RESYNC restored: balls made %u, unmade %u, no birth to make %u; mechanisms only the"
                                " server's %zu%s, only the replay's %zu%s; scripts %s, %d re-registered%s; blob objects %u"
                                " cores %u skipped %u unknown %zu renamed %u%s, ambiguous %u; rows written %u (over a"
                                " body the blob restored %u), equal %u, unmapped %u; unknown rechecked %u; ball events"
                                " failed %u; union {%s}; world tick %u\n",
                        group.tick, rr.made, rr.unmade, rr.unborn, rr.missing.size(), missing.c_str(), rr.extra.size(),
                        extra.c_str(), rr.scripts.c_str(), rr.unregistered, rr.unregistered_names.c_str(), rr.blob.objects,
                        rr.blob.cores, rr.blob.skipped, rr.blob.unknown_bodies.size(), rr.blob.renamed, renames.c_str(),
                        rr.renames_ambiguous, rr.rows_written, rr.rows_restore_differ, rr.rows_equal, rr.rows_unmapped,
                        rr.unknown_rechecked, rr.events_failed, rr.active.c_str(), world->tick_index());
                    if (rr.incomplete()) {
                        ++resyncs_incomplete;
                        resync_failed(group.tick);
                        std::printf("tick %u: RESYNC INCOMPLETE: the replay's world after the restore is not the server's"
                                    " state of S (body set, scripts, clone names or restored rows above): the replay after"
                                    " it is not the server's world\n", group.tick);
                    }
                    if (out.is_open())
                        out.note(group.tick, "resync: the server's state of tick " + std::to_string(group.tick)
                               + " restored from the client journal's RESYNC record");
                }
            }
            bmmo::physics::world_hash actual;
            if (!bmmo::physics::capture_world_hash(world->physics(), actual, error)) {
                std::fprintf(stderr, "hash failed at tick %u: %s\n", group.tick, error.c_str());
                if (out.is_open()) {
                    out.note(group.tick, "end: replay stopped: hash failed at tick " + std::to_string(group.tick)
                           + ": " + error);
                    out.close();
                }
                return 1;
            }
            if (out.is_open())
                out.tick(group.tick, actual, static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started).count()));
            bool same = true;
            // A client journal's hashes are of its own world; a tick without a
            // TICK record (the crash tail) was simulated but is not compared.
            const bool comparable = !client_journal && group.has_tick;
            if (comparable) {
                const auto& expected = group.record;
                ++compared;
                same = actual.hash == expected.hash;
                if (same) {
                    ++matched;
                } else if (first_divergence < 0) {
                    first_divergence = static_cast<long long>(group.tick);
                    std::printf("    replay movable: %s\n",
                        bmmo::physics::describe_movable_objects(world->physics()).c_str());
                    std::printf("    replay bodies: %s\n",
                        bmmo::physics::describe_physics_objects(world->physics()).c_str());
                    std::printf("    replay events: %s\n",
                        bmmo::physics::drain_event_log(world->physics()).c_str());
                    std::printf("    pose expected=%016llx actual=%016llx cores=%d/%d probe=%s/%s\n",
                        static_cast<unsigned long long>(expected.pose),
                        static_cast<unsigned long long>(actual.pose), expected.cores, actual.cores,
                        expected.probe_name.c_str(), actual.probe_name);
                    std::printf("    probe expected pos=(%.9g,%.9g,%.9g) speed=(%.9g,%.9g,%.9g)\n"
                                "    probe actual   pos=(%.9g,%.9g,%.9g) speed=(%.9g,%.9g,%.9g)\n",
                        expected.probe_position[0], expected.probe_position[1], expected.probe_position[2],
                        static_cast<double>(expected.probe_speed[0]), static_cast<double>(expected.probe_speed[1]),
                        static_cast<double>(expected.probe_speed[2]),
                        actual.probe_position[0], actual.probe_position[1], actual.probe_position[2],
                        static_cast<double>(actual.probe_speed[0]), static_cast<double>(actual.probe_speed[1]),
                        static_cast<double>(actual.probe_speed[2]));
                    if (!args.dump_entity.empty())
                        std::printf("--dump-entity before tick %u:\n%s--dump-entity after tick %u:\n%s", group.tick,
                            dump_before.c_str(), group.tick, describe_entities(world->engine(), args.dump_entity).c_str());
                    std::printf("DIVERGE tick=%u expected=%016llx actual=%016llx\n", group.tick,
                        static_cast<unsigned long long>(expected.hash), static_cast<unsigned long long>(actual.hash));
                }
            }
            // Checkpoints: the recording's bodies against the same world now.
            // A client's LOCAL checkpoint is its own world, not the server's,
            // so only FULL (server) and RECEIVED (authoritative) rows compare.
            bool checkpoint_bad = false;
            for (const auto& cp: group.checkpoints) {
                const bool full = (cp.flags & bmmo::session::JOURNAL_CHECKPOINT_FULL) != 0;
                const bool received = (cp.flags & bmmo::session::JOURNAL_CHECKPOINT_RECEIVED) != 0;
                if (!full && !received) continue;
                ++checkpoints;
                world->snapshot(true, bodies);
                const checkpoint_diff diff = compare_checkpoint(cp.bodies, bodies, dictionary);
                const bool bad = diff.differing != 0 || diff.missing != 0 || (full && diff.extra != 0);
                if (resyncs_restored != 0 && !(restored_here && (cp.flags & bmmo::session::JOURNAL_CHECKPOINT_RESYNC))) {
                    ++checkpoints_after_resync;
                    if (bad) ++mismatches_after_resync;
                }
                if (bad) {
                    checkpoint_bad = true;
                    ++checkpoint_mismatches;
                    if (first_checkpoint_divergence < 0) first_checkpoint_divergence = static_cast<long long>(cp.tick);
                }
                if (bad || diff.renumbered != 0 || diff.unresolved != 0) {
                    std::string detail;
                    const auto add = [&detail](const std::string& text) {
                        if (!detail.empty()) detail += "; ";
                        detail += text;
                    };
                    // Name what is actually wrong: a missing or an extra body
                    // has no dp to be the worst offender with, and it is the
                    // more interesting half of the two.
                    if (!diff.first_missing.empty()) add("missing: " + diff.first_missing);
                    // Only a full snapshot claims to hold every body; a delta
                    // leaves out what did not change, so its "extra" rows are
                    // the normal case and not worth a name.
                    if (full && !diff.first_extra.empty()) add("extra: " + diff.first_extra);
                    if (!diff.worst_name.empty()) add("worst " + diff.worst_name + " " + diff.worst_detail);
                    std::printf("checkpoint tick %u (%s): %zu bodies, %zu differ, %zu only recorded, %zu only replayed,"
                                " %zu renumbered, %zu unresolved%s%s\n",
                        cp.tick, full ? "FULL" : "RECEIVED", diff.compared, diff.differing, diff.missing, diff.extra,
                        diff.renumbered, diff.unresolved, detail.empty() ? "" : "; ", detail.c_str());
                }
            }
            if (out.is_open() && header.checkpoint_ticks != 0 && group.tick % header.checkpoint_ticks == 0) {
                world->snapshot(true, bodies);
                out.checkpoint(group.tick, bmmo::session::JOURNAL_CHECKPOINT_FULL, bodies);
            }
            if (args.exact_from >= 0 && static_cast<long long>(group.tick) >= args.exact_from
                    && static_cast<long long>(group.tick) <= args.exact_to)
                std::printf("exact tick%u ivp_time=%.6f seed=%d\n%s", group.tick, actual.ivp_time, actual.ivp_seed,
                    bmmo::physics::describe_cores_exact(world->physics()).c_str());
            if (args.report_every > 0 && ((simulated - 1) % static_cast<size_t>(args.report_every)) == 0) {
                if (comparable)
                    std::printf("tick=%u %s expected=%016llx actual=%016llx cores=%d/%d pose=%s probe=%s/%s"
                                " dpos=(%.3g,%.3g,%.3g)\n",
                        group.tick, same ? "ok" : "MISMATCH",
                        static_cast<unsigned long long>(group.record.hash),
                        static_cast<unsigned long long>(actual.hash), group.record.cores, actual.cores,
                        group.record.pose == actual.pose ? "same" : "DIFF", group.record.probe_name.c_str(),
                        actual.probe_name, actual.probe_position[0] - group.record.probe_position[0],
                        actual.probe_position[1] - group.record.probe_position[1],
                        actual.probe_position[2] - group.record.probe_position[2]);
                else
                    std::printf("tick=%u hash=%016llx pose=%016llx cores=%d probe=%s\n", group.tick,
                        static_cast<unsigned long long>(actual.hash), static_cast<unsigned long long>(actual.pose),
                        actual.cores, actual.probe_name);
                std::fflush(stdout);
            }
            // A client journal has no comparable hash at all: its checkpoints
            // are the only divergence there is, so the flag has to see them.
            if (stop_at_refused_jump) {
                std::printf("stopping: the restore of tick %u, which the jump in the numbering was excused by, was refused"
                            " (--continue-after-jump replays past it anyway)\n", group.tick);
                stopped_at_jump = true;
                break;
            }
            if ((!same || checkpoint_bad || resync_bad) && args.stop_on_divergence) {
                std::printf("stopping at the first divergence (tick %u, %s)\n", group.tick,
                    !same ? (checkpoint_bad ? "the hash and a checkpoint" : "the hash")
                          : (checkpoint_bad ? "a checkpoint" : "the resync restore"));
                break;
            }
            if (args.ticks_explicit && simulated >= static_cast<size_t>(std::max(args.ticks, 0))) {
                std::printf("stopping after %zu ticks (--ticks)\n", simulated);
                break;
            }
        }
        if (out.is_open()) {
            // The last tick this replay really ran, not the input file's last
            // group: with --ticks or --stop-on-divergence they are thousands of
            // ticks apart, and a NOTE past the last TICK record invents a tick.
            out.note(last_written_tick, "end: replay finished");
            out.close();
        }
        std::printf("summary: ticks=%zu matched=%zu first_divergence=%lld checkpoints=%zu checkpoint_mismatches=%zu\n",
            simulated, matched, first_divergence, checkpoints, checkpoint_mismatches);
        if (pre_first_groups)   // step B2SW (T1 b)
            std::printf("note: %zu group(s) before the first simulated tick applied without a step\n", pre_first_groups);
        if (stopped_at_jump)
            std::printf("note: the replay stopped at a tick jump; the summary covers only the ticks before it\n");
        if (args.drop_sectors_above >= 0)   // step SP8
            std::printf("--drop-sectors-above %d: %zu Sector events dropped\n", args.drop_sectors_above, sectors_dropped);
        close_scene_trace(*world);   // step SP8: nothing without --placement-trace
        // Only a journal with RESYNC records gets this line: the summary line
        // above is what tools compare across SimTool builds.
        if (resync_records != 0)
            std::printf("resync summary: records=%zu restored=%zu refused=%zu incomplete=%zu timed_out=%zu no_state=%zu%s"
                        " first_restored=%lld first_failed=%lld before_restore_identical=%zu differ=%zu"
                        " after_first_restore: checkpoints=%zu mismatches=%zu\n",
                resync_records, resyncs_restored, resyncs_refused, resyncs_incomplete, resyncs_timed_out,
                resyncs_no_state, args.ignore_resync ? " (--ignore-resync: none restored)" : "", first_resync,
                first_resync_failure, resync_before_identical, resync_before_differ, checkpoints_after_resync,
                mismatches_after_resync);
        if (client_journal)
            std::printf("note: a client journal's own TICK hashes are of its own world; the %zu received snapshots"
                        " are the server's word and are what was compared\n", checkpoints);
        else if (compared != simulated)
            std::printf("note: %zu of %zu ticks had no TICK record and were simulated without comparing\n",
                simulated - compared, simulated);
        std::fflush(stdout);
        return first_divergence < 0 && first_checkpoint_divergence < 0 && first_resync_failure < 0 && !stopped_at_jump
            ? 0 : 3;
    }
    // ------------------------------------------------------------------
    // --describe-script NAME: the level loaded, that root script's graph as
    // text - every behaviour with its prototype, flags and active IOs, the
    // links between them with their activation delay (the "next frame" links
    // are the ones a replayed frame has to reproduce), and every parameter
    // whose value fits eight bytes.  The way to read what a replay-fidelity
    // mismatch names ("P_Modul_18_MF Script / Physics Force: local 20
    // (TurnON_Permission)"): which block writes it, from which input.
    // ------------------------------------------------------------------
    namespace {
        std::string io_name(CKBehaviorIO* io) {
            if (!io) return "?";
            CKBehavior* owner = io->GetOwner();
            std::string name = owner && owner->GetName() ? owner->GetName() : "?";
            const bool out = io->GetType() == CK_BEHAVIORIO_OUT;
            int index = -1;
            if (owner) {
                const int count = out ? owner->GetOutputCount() : owner->GetInputCount();
                for (int i = 0; i < count; ++i)
                    if ((out ? owner->GetOutput(i) : owner->GetInput(i)) == io) { index = i; break; }
            }
            return name + (out ? ".out" : ".in") + std::to_string(index) + (io->GetName() ? std::string("(") + io->GetName() + ")" : "");
        }

        std::string parameter_text(CKContext* context, CKParameter* p) {
            if (!p) return "-";
            const CKGUID guid = p->GetGUID();
            CKSTRING type = context->GetParameterManager()->ParameterGuidToName(guid);
            std::string text = type ? type : "?";
            const int size = p->GetDataSize();
            const void* data = p->GetReadDataPtr(FALSE);
            if (!data || size <= 0) return text + " (no data)";
            char buf[64];
            if (guid == CKPGUID_FLOAT && size == 4) {
                float f = 0; std::memcpy(&f, data, 4);
                std::snprintf(buf, sizeof(buf), " = %g", static_cast<double>(f));
            } else if (size == 4) {
                uint32_t u = 0; std::memcpy(&u, data, 4);
                std::snprintf(buf, sizeof(buf), " = %u", u);
                if (guid == CKPGUID_MESSAGE)
                    if (CKSTRING message = context->GetMessageManager()->GetMessageTypeName(static_cast<CKMessageType>(u)))
                        return text + buf + " (" + message + ")";
            } else if (size <= 8) {
                text += " =";
                for (int i = 0; i < size; ++i) {
                    std::snprintf(buf, sizeof(buf), " %02x", static_cast<const uint8_t*>(data)[i]);
                    text += buf;
                }
                return text;
            } else {
                std::snprintf(buf, sizeof(buf), " (%d bytes)", size);
            }
            return text + buf;
        }

        void describe_behavior(CKContext* context, CKBehavior* beh, int depth) {
            const std::string pad(static_cast<size_t>(depth) * 2, ' ');
            const char* name = beh->GetName() ? beh->GetName() : "?";
            const std::string proto = beh->IsUsingFunction() && beh->GetPrototypeName() ? std::string("  [") + beh->GetPrototypeName() + "]" : std::string();
            std::printf("%s%s%s  id=%u flags=0x%08x", pad.c_str(), name, proto.c_str(), static_cast<unsigned>(beh->GetID()),
                        static_cast<unsigned>(beh->GetFlags()));
            std::string active;
            for (int i = 0; i < beh->GetInputCount(); ++i) if (beh->IsInputActive(i)) active += " in" + std::to_string(i);
            for (int i = 0; i < beh->GetOutputCount(); ++i) if (beh->IsOutputActive(i)) active += " out" + std::to_string(i);
            if (!active.empty()) std::printf("  active:%s", active.c_str());
            std::printf("\n");
            for (int i = 0; i < beh->GetInputParameterCount(); ++i) {
                CKParameterIn* in = beh->GetInputParameter(i);
                std::printf("%s    pin %d %s: %s\n", pad.c_str(), i, in && in->GetName() ? in->GetName() : "", parameter_text(context, in ? in->GetRealSource() : nullptr).c_str());
            }
            for (int i = 0; i < beh->GetOutputParameterCount(); ++i) {
                CKParameterOut* out = beh->GetOutputParameter(i);
                std::printf("%s    pout %d %s: %s\n", pad.c_str(), i, out && out->GetName() ? out->GetName() : "", parameter_text(context, out).c_str());
            }
            for (int i = 0; i < beh->GetLocalParameterCount(); ++i) {
                CKParameterLocal* local = beh->GetLocalParameter(i);
                std::printf("%s    local %d %s: %s\n", pad.c_str(), i, local && local->GetName() ? local->GetName() : "", parameter_text(context, local).c_str());
            }
            if (beh->IsUsingFunction()) return;
            for (int i = 0; i < beh->GetSubBehaviorLinkCount(); ++i) {
                CKBehaviorLink* link = beh->GetSubBehaviorLink(i);
                if (!link) continue;
                // The blocks' ids last ([#from -> #to]): a graph has many blocks of one name.
                CKBehaviorIO* from = link->GetInBehaviorIO();
                CKBehaviorIO* to = link->GetOutBehaviorIO();
                std::printf("%s    link %s -> %s%s [#%u -> #%u]\n", pad.c_str(), io_name(from).c_str(), io_name(to).c_str(),
                            link->GetActivationDelay() ? (" delay " + std::to_string(link->GetActivationDelay())).c_str() : "",
                            static_cast<unsigned>(from && from->GetOwner() ? from->GetOwner()->GetID() : 0),
                            static_cast<unsigned>(to && to->GetOwner() ? to->GetOwner()->GetID() : 0));
            }
            for (int i = 0; i < beh->GetSubBehaviorCount(); ++i)
                if (CKBehavior* child = beh->GetSubBehavior(i)) describe_behavior(context, child, depth + 1);
        }
    }

    // ------------------------------------------------------------------
    // --list-placement: the session world at its anchor (before any pass),
    // its PH table, the builder's copy block and the row plan; nothing is
    // written to the world.
    int run_list_placement(const arguments& args) {
        // The listing is of the unplaced world; --placement on applies the
        // placement afterwards, in the tool (step S2).
        arguments unplaced = args;
        unplaced.placement = false;
        auto world = create_session_world(unplaced, 0.0f);
        if (!world) return 1;
        const int listed = bmmo::sim::list_placement(world->engine().context(), args.level > 0 ? args.level : 1);
        // Close-out step S2 (SP8 (g)): a group's members with their world
        // boxes, after the listing (and after the tool's apply below).
        const auto list_group = [&](const char* when) {
            if (args.list_group.empty()) return;
            CKContext* context = world->engine().context();
            auto* group = CKGroup::Cast(context->GetObjectByNameAndClass(const_cast<CKSTRING>(args.list_group.c_str()),
                                                                         CKCID_GROUP, nullptr));
            if (!group) {
                std::printf("group %s (%s): not found\n", args.list_group.c_str(), when);
                return;
            }
            std::printf("group %s (%s): %d member(s)\n", args.list_group.c_str(), when, group->GetObjectCount());
            for (int i = 0; i < group->GetObjectCount(); ++i) {
                CKBeObject* o = group->GetObject(i);
                CK3dEntity* e = CK3dEntity::Cast(o);
                if (!e) {
                    std::printf("  #%d %s (class %d, not 3D)\n", i, o && o->GetName() ? o->GetName() : "?",
                                o ? static_cast<int>(o->GetClassID()) : -1);
                    continue;
                }
                const VxBbox& b = e->GetBoundingBox(FALSE);
                std::printf("  #%d %s id=%lu visible=%d box min=(%.3f,%.3f,%.3f) max=(%.3f,%.3f,%.3f)\n", i,
                            e->GetName() ? e->GetName() : "?", static_cast<unsigned long>(e->GetID()), e->IsVisible() ? 1 : 0,
                            b.Min.x, b.Min.y, b.Min.z, b.Max.x, b.Max.y, b.Max.z);
                const VxBbox& lb = e->GetBoundingBox(TRUE);
                const VxMatrix& m = e->GetWorldMatrix();
                std::printf("     local box min=(%a,%a,%a) max=(%a,%a,%a)\n", lb.Min.x, lb.Min.y, lb.Min.z, lb.Max.x, lb.Max.y, lb.Max.z);
                for (int row = 0; row < 4; ++row)
                    std::printf("     world row %d = (%a,%a,%a,%a)\n", row, m[row][0], m[row][1], m[row][2], m[row][3]);
            }
            std::fflush(stdout);   // the x64 teardown can crash after the apply (design 12.5): print it first
        };
        list_group("unplaced");
        if (!args.placement) return listed;
        std::set<std::string> bodies;
        const bmmo::game::placement_hooks hooks = tool_placement_hooks(world->physics(), args.placement_fail_at, bodies);
        std::printf("anchor bodies: %zu\n", bodies.size());
        const int applied = bmmo::sim::list_placement_apply(world->engine().context(), hooks);
        list_group("after the tool's apply, release and re-apply");
        return listed ? listed : applied;
    }

    // ------------------------------------------------------------------
    // --portable-scripts-check (docs/resync-and-sector-timing-design.md 7.2,
    // step A1).  The server's world (physics_world) of --level, one player
    // in sector 1; at anchor+30 and after each sector 2..N (stamped by the
    // player, --drop-settle ticks apart), with the session selection
    // (game/session_host_support.hpp session_script_selection):
    //
    //   v2       the local record (script_replay::capture)
    //   portable the portable record (capture_portable), its size and what
    //            it met (values, object references, registry entries)
    //   names    localize(portable) with no keep-local script == v2 byte
    //            for byte (the anchor is v2 itself)
    //   rule     the same with the core's keep_local_script rule, anchored
    //            on the PREVIOUS checkpoint's v2 record: == v2 with the
    //            keep-local scripts' entries taken from that anchor
    //            (kept_changed = how many of them differ from now)
    //   scrubbed every object reference of the selected scripts zeroed and
    //            every registry entry set to -1 in the live world first, so
    //            each one is found again by its name alone (the world a
    //            server's names meet: the local values say nothing), then
    //            localize == v2
    //   restored the scrubbed localization restored (replay_activation, as
    //            the resync does) and captured again == v2
    //
    // And the balloon's activation (design 2.8, 9 Q8): after every frame,
    // each keep-local script that is not a trafo script (the PE_Balloon*
    // scripts) is checked for a pending activation (next-frame flags).
    //
    // In one process every name exists, so localize's `unportable` is 0 by
    // construction; what a client's world would lack is listed from the
    // capture instead: references with no name, names a lookup does not
    // find again as the same object (ambiguous), and the server's private
    // objects (a name containing "BMMO").  Every registry-typed parameter
    // is listed, and every value copied as bytes whose type is index-like
    // (a class id, a parameter type, an ID, a structure): one process
    // cannot tell those from portable bytes, so the list is the check.
    // localize must also meet no name of several objects (ambiguous).
    // PASS: every comparison identical at every checkpoint.
    // ------------------------------------------------------------------
    int run_portable_scripts_check(const arguments& args) {
        using bmmo::game::script_replay;
        auto world = create_session_world(args, 0.0f);
        if (!world) return 1;
        std::string error;
        if (!world->add_player(1, 0, error)) {
            std::fprintf(stderr, "add_player failed: %s\n", error.c_str());
            return 1;
        }
        report_sector(*world, 1, 1);
        CKContext* context = world->engine().context();
        CKIpionManager* physics = world->physics();
        const script_replay::selection sel = bmmo::game::session_script_selection(context);
        const std::vector<CK_ID> activations = bmmo::game::session_script_activations(context, sel);
        bmmo::game::script_replay_hooks hooks;
        hooks = bmmo::game::force_hooks_for(bmmo::physics::physics_direct(physics));   // step HU1: one copy (game/host_backend.hpp)
        const auto owner_of = [](CKBehavior* beh) {
            CKBeObject* owner = beh ? beh->GetOwner() : nullptr;
            return std::string(owner && owner->GetName() ? owner->GetName() : "");
        };
        const auto name_of = [](CKBehavior* beh) { return std::string(beh && beh->GetName() ? beh->GetName() : ""); };
        const script_replay::keep_local_fn keep_rule = [](const std::string& owner, const std::string& script) {
            return bmmo::session::keep_local_script(owner, script);
        };

        // The selection, the keep-local scripts and the recorded activations.
        std::vector<CK_ID> balloon;   // keep-local, not trafo: the Q8 watch
        std::string keep_text, activation_text;
        for (CK_ID id: sel.scripts) {
            auto* beh = CKBehavior::Cast(context->GetObject(id));
            if (!beh || !keep_rule(owner_of(beh), name_of(beh))) continue;
            keep_text += (keep_text.empty() ? "" : ", ") + owner_of(beh) + "/" + name_of(beh);
            if (!script_replay::is_trafo_script(beh)) balloon.push_back(id);
        }
        for (CK_ID id: activations) {
            auto* beh = CKBehavior::Cast(context->GetObject(id));
            activation_text += (activation_text.empty() ? "" : ", ") + owner_of(beh) + "/" + name_of(beh);
        }
        // The level's sectors: the highest Sector of the PH table (the
        // placement rows the manager walks; the Sector_NN groups are gone
        // once Levelinit has read them).
        int sectors = 0;
        if (auto* ph = CKDataArray::Cast(context->GetObjectByNameAndClass(const_cast<CKSTRING>("PH"), CKCID_DATAARRAY, nullptr))) {
            int column = -1;
            for (int c = 0; c < ph->GetColumnCount(); ++c)
                if (ph->GetColumnName(c) && std::strcmp(ph->GetColumnName(c), "Sector") == 0) column = c;
            for (int r = 0; column >= 0 && r < ph->GetRowCount(); ++r) {
                int sector = 0;
                if (ph->GetElementValue(r, column, &sector)) sectors = std::max(sectors, sector);
            }
        }
        std::printf("portable-scripts-check level=%d sectors=%d selected=%zu keep_local=%s\n", world->level(), sectors,
                    sel.scripts.size(), keep_text.empty() ? "-" : keep_text.c_str());
        std::printf("  recorded activations (session_script_activations): %zu: %s\n", activations.size(),
                    activation_text.empty() ? "-" : activation_text.c_str());

        // The balloon's pending activations, per frame (Q8).
        std::vector<std::string> balloon_seen;
        uint32_t stamped_at = world->tick_index();
        int stamped_sector = 1;
        const auto run_ticks = [&](int n) {
            for (int i = 0; i < n; ++i) {
                std::string tick_error;
                if (!world->tick(tick_error)) {
                    std::fprintf(stderr, "tick failed: %s\n", tick_error.c_str());
                    return false;
                }
                for (CK_ID id: balloon) {
                    auto* beh = CKBehavior::Cast(context->GetObject(id));
                    if (!beh) continue;
                    const CKDWORD flags = beh->GetFlags();
                    if (!(flags & CKBEHAVIOR_ACTIVATENEXTFRAME)) continue;
                    char line[400];
                    std::snprintf(line, sizeof(line), "%s/%s pending activation (next-frame flags, reset=%d, active=%d) after"
                                  " the frame of tick %u; sector %d stamped at tick %u", owner_of(beh).c_str(),
                                  name_of(beh).c_str(), (flags & CKBEHAVIOR_RESETNEXTFRAME) ? 1 : 0, beh->IsActive() ? 1 : 0,
                                  world->tick_index() - 1u, stamped_sector, stamped_at);
                    balloon_seen.emplace_back(line);
                    std::printf("  balloon: %s\n", line);
                }
            }
            return true;
        };

        // Every object reference zeroed, every registry entry -1, in the
        // live world (the restore of the localized record puts them back).
        const auto scrub = [&]() {
            int scrubbed = 0;
            std::function<void(CKBehavior*)> walk = [&](CKBehavior* beh) {
                const auto one = [&](CKParameter* p) {
                    if (!p) return;
                    const CKGUID guid = p->GetGUID();
                    if (guid == CKPGUID_POINTER || guid == CKPGUID_OBJECTARRAY || guid == CKPGUID_VOIDBUF || guid == CKPGUID_2DCURVE)
                        return;
                    if (p->GetDataSize() != 4) return;
                    const bool object = p->GetParameterClassID() != 0;
                    const bool registry = guid == CKPGUID_MESSAGE || guid == CKPGUID_ATTRIBUTE;
                    if (!object && !registry) return;
                    if (void* buffer = p->GetWriteDataPtr()) {
                        const int32_t value = object ? 0 : -1;
                        std::memcpy(buffer, &value, 4);
                        ++scrubbed;
                    }
                };
                for (int i = 0; i < beh->GetLocalParameterCount(); ++i) one(beh->GetLocalParameter(i));
                for (int i = 0; i < beh->GetOutputParameterCount(); ++i) one(beh->GetOutputParameter(i));
                if (beh->IsUsingFunction()) return;
                for (int i = 0; i < beh->GetSubBehaviorCount(); ++i)
                    if (CKBehavior* child = beh->GetSubBehavior(i)) walk(child);
            };
            for (CK_ID id: sel.scripts)
                if (auto* beh = CKBehavior::Cast(context->GetObject(id))) walk(beh);
            return scrubbed;
        };

        std::set<std::string> registry_all, unnamed_all, ambiguous_all, private_all, unportable_all, index_like_all,
            ambiguous_local_all;
        int checkpoints = 0, failures = 0;
        std::vector<uint8_t> previous_v2;   // the anchor of the keep-local arm: the last checkpoint's record
        // The record `v2` with the keep-local scripts' entries taken from
        // `anchor` (what localize with that anchor must produce); `changed`
        // gets how many of those entries differ between the two.
        const auto splice = [&](const std::vector<uint8_t>& v2, const std::vector<uint8_t>& anchor,
                                std::vector<uint8_t>& out, int& changed) {
            script_replay::record_index current, old;
            if (!script_replay::index_record(v2, current) || !script_replay::index_record(anchor, old)) return false;
            out.assign(v2.begin(), v2.begin() + 12);
            changed = 0;
            for (const auto& s: current.scripts) {
                auto* beh = CKBehavior::Cast(context->GetObject(s.id));
                const bool keep = beh && keep_rule(owner_of(beh), name_of(beh));
                const auto o = std::find_if(old.scripts.begin(), old.scripts.end(),
                                            [&](const script_replay::record_index::script& x) { return x.id == s.id; });
                if (keep && o == old.scripts.end()) return false;
                if (keep) {
                    const std::vector<uint8_t> a(anchor.begin() + o->begin, anchor.begin() + o->end);
                    const std::vector<uint8_t> b(v2.begin() + s.begin, v2.begin() + s.end);
                    if (a != b) ++changed;
                    out.insert(out.end(), a.begin(), a.end());
                } else {
                    out.insert(out.end(), v2.begin() + s.begin, v2.begin() + s.end);
                }
            }
            out.insert(out.end(), v2.begin() + current.tail, v2.end());
            return true;
        };
        size_t portable_max = 0, v2_max = 0;
        const auto compare = [&](const std::vector<uint8_t>& expected, const std::vector<uint8_t>& got) -> std::string {
            if (expected == got) return "SAME";
            const auto d = script_replay::diff(context, sel, expected, got);
            return "DIFF(" + (d.where.empty() ? std::to_string(expected.size()) + " vs " + std::to_string(got.size()) + " bytes"
                              : d.where) + ")";
        };
        const auto checkpoint = [&](const std::string& label) {
            ++checkpoints;
            std::vector<uint8_t> v2, portable, names, rule, scrubbed, again;
            script_replay::portable_report captured;
            captured.lists = true;
            std::string e;
            if (!script_replay::capture(context, sel, hooks, v2)) {
                std::printf("FAIL %s: v2 capture failed\n", label.c_str());
                ++failures;
                return;
            }
            if (!script_replay::capture_portable(context, sel, hooks, portable, &e, &captured)) {
                std::printf("FAIL %s: portable capture failed: %s\n", label.c_str(), e.c_str());
                ++failures;
                return;
            }
            portable_max = std::max(portable_max, portable.size());
            v2_max = std::max(v2_max, v2.size());
            if (args.verbose)
                for (const auto& x: captured.registry_list) std::printf("  %s registry: %s\n", label.c_str(), x.c_str());
            for (auto& x: captured.registry_list) registry_all.insert(x);
            for (auto& x: captured.unnamed_list) unnamed_all.insert(x);
            for (auto& x: captured.ambiguous_list) ambiguous_all.insert(x);
            for (auto& x: captured.server_private_list) private_all.insert(x);
            for (auto& x: captured.index_like_list) index_like_all.insert(x);
            bool ok = true;
            const auto run = [&](const script_replay::keep_local_fn& keep, const std::vector<uint8_t>& anchor,
                                 const std::vector<uint8_t>& expected, std::vector<uint8_t>& out,
                                 script_replay::portable_report& report) {
                report.lists = true;
                std::string why;
                if (!script_replay::localize(context, sel, portable, &anchor, keep, out, &why, &report)) return "REFUSED(" + why + ")";
                for (auto& x: report.unportable_list) unportable_all.insert(x);
                for (auto& x: report.ambiguous_list) ambiguous_local_all.insert(x);
                return compare(expected, out);
            };
            // The keep-local arm anchors on the previous checkpoint's record:
            // its keep-local entries must come from there, the rest from the
            // portable record (`kept_changed` of them differ from now).
            const std::vector<uint8_t>& anchor = previous_v2.empty() ? v2 : previous_v2;
            std::vector<uint8_t> rule_expected;
            int kept_changed = 0;
            const bool spliced = splice(v2, anchor, rule_expected, kept_changed);
            script_replay::portable_report r_names, r_rule, r_scrub;
            const std::string s_names = run(nullptr, v2, v2, names, r_names);
            const std::string s_rule = spliced ? run(keep_rule, anchor, rule_expected, rule, r_rule) : std::string("NO-SPLICE");
            const int zeroed = scrub();
            const std::string s_scrub = run(nullptr, v2, v2, scrubbed, r_scrub);
            // Put the world back: the localized record when it is the v2
            // record, else v2 itself (so the run goes on unharmed).
            std::vector<CK_ID> replayed;
            const std::vector<uint8_t>& back = s_scrub == "SAME" ? scrubbed : v2;
            std::string s_restored;
            if (!script_replay::restore(context, sel, hooks, back, &e, &replayed, nullptr, true)) {
                s_restored = "REFUSED(" + e + ")";
            } else if (!script_replay::capture(context, sel, hooks, again)) {
                s_restored = "CAPTURE-FAILED";
            } else {
                s_restored = compare(v2, again);
            }
            const auto& lr = script_replay::last_restore();
            ok = s_names == "SAME" && s_rule == "SAME" && s_scrub == "SAME" && s_restored == "SAME"
              && r_names.unportable == 0 && r_rule.unportable == 0 && r_scrub.unportable == 0
              && r_names.ambiguous == 0 && r_rule.ambiguous == 0 && r_scrub.ambiguous == 0;
            if (!ok) ++failures;
            std::printf("%s %s tick=%u v2=%zu portable=%zu scripts=%u blocks=%u values=%u objects=%u registry=%u opaque=%u"
                        " unnamed=%u ambiguous=%u server_private=%u index_like=%u | names=%s rule=%s (kept_local=%u"
                        " anchor=%s kept_changed=%d) scrubbed=%s"
                        " (zeroed=%d kept=%u resolved=%u unportable=%u) restored=%s (cells=%u links_rearmed=%u"
                        " joints_reset=%u)\n", ok ? "ok  " : "FAIL", label.c_str(), world->tick_index(), v2.size(),
                        portable.size(), captured.scripts, captured.blocks, captured.values, captured.objects,
                        captured.registry, captured.opaque, captured.unnamed, captured.ambiguous, captured.server_private,
                        captured.index_like, s_names.c_str(), s_rule.c_str(), r_rule.kept_local,
                        previous_v2.empty() ? "now" : "previous", kept_changed, s_scrub.c_str(), zeroed, r_scrub.objects_kept,
                        r_scrub.objects_resolved, r_scrub.unportable, s_restored.c_str(), lr.cells_written,
                        lr.links_rearmed, lr.joints_reset);
            std::fflush(stdout);
            previous_v2 = v2;
        };

        if (!run_ticks(30)) return 1;
        checkpoint("anchor+30");
        for (int k = 2; k <= sectors; ++k) {
            stamped_at = world->tick_index();
            stamped_sector = k;
            report_sector(*world, 1, k);
            if (!run_ticks(std::max(1, args.drop_settle))) return 1;
            checkpoint("sector" + std::to_string(k));
        }
        const auto print_list = [](const char* what, const std::set<std::string>& list) {
            std::printf("%s: %zu\n", what, list.size());
            for (const auto& x: list) std::printf("  %s\n", x.c_str());
        };
        print_list("registry-typed parameters", registry_all);
        print_list("object references without a name", unnamed_all);
        print_list("ambiguous object references (a name lookup finds another object)", ambiguous_all);
        print_list("server-private object references (BMMO)", private_all);
        print_list("unportable at localize", unportable_all);
        print_list("ambiguous at localize (a name of several objects here)", ambiguous_local_all);
        print_list("index-like values copied as bytes (class id, parameter type, ID, structure)", index_like_all);
        std::printf("balloon activations seen: %zu%s\n", balloon_seen.size(),
                    balloon.empty() ? " (the level has no keep-local balloon script)" : "");
        std::printf("summary: portable-scripts-check level=%d checkpoints=%d failed=%d portable_max=%zu v2_max=%zu"
                    " registry=%zu unnamed=%zu ambiguous=%zu server_private=%zu unportable=%zu ambiguous_local=%zu"
                    " index_like=%zu balloon_pending=%zu\n",
                    world->level(), checkpoints, failures, portable_max, v2_max, registry_all.size(), unnamed_all.size(),
                    ambiguous_all.size(), private_all.size(), unportable_all.size(), ambiguous_local_all.size(),
                    index_like_all.size(), balloon_seen.size());
        std::printf("%s checkpoints=%d failed=%d\n", failures == 0 ? "PASS" : "FAIL", checkpoints, failures);
        std::fflush(stdout);
        return failures == 0 ? 0 : 3;
    }

    // ------------------------------------------------------------------
    // --resync-check S B (docs/resync-and-sector-timing-design.md 7.2, step
    // A2): the resync apply of 2.6 offline, against the server's own world
    // (physics_world), with no wire.  One process, one world; every run
    // starts from the scene's ROOT (--resync-root whole ticks after player 1
    // joins, before any stamp), put back the way a rollback restores (the
    // mechanism scripts with replay_activation, this run's births destroyed
    // with their joint handles released and their entities put back where
    // their Physicalize found them, the savestate, the world's counters) and,
    // harness-only, every 3D entity's matrix, visibility and parent, every
    // data-array cell, and the restored-active scripts re-registered with the
    // behaviour manager (rc_unregistered_scripts); a warm-up run from the
    // live root checks that.  Ticks on the command line count from the root;
    // a "boundary t" is the world before the step of tick t, and the state
    // "at S" is boundary S (the server's end-of-tick state of S-1).
    //
    //   schedule  --resync-stamp SECTOR TICK (player 1's Sector events;
    //             default sector 1 at 0), --resync-drop TICK (player 1's
    //             --drop ball physicalized then; it must be before every
    //             arm's divergence)
    //   straight  root -> B+N (--resync-after, 600): at every boundary of
    //             [S, B+N] the movable bodies and the blob; at S the server's
    //             resync state: rows (snapshot_for_journal = a full
    //             snapshot's bodies), the portable script record (session
    //             selection) and the union (the world's step state).
    //   arms      each an ANCHOR run from the root to B, its own world's
    //             state kept at S (blob, local v2 scripts, counters, contacts:
    //             the synthetic anchor of 2.6.1), then the APPLY at B and the
    //             re-step S..B+N with the straight schedule's events from S on:
    //       E     the anchor run is the straight one;
    //       R     ... perturbed at S-17 (--resync-perturb-ticks): every
    //             movable body moved by M (--resync-perturb, 1 cm) in x and
    //             z, awake ones 5*M m/s faster, so the anchor's contacts,
    //             mindists, queues and calm references at S are its own
    //             (resync design 7.2 words arm R as the scripts of S-17
    //             restored at S-17 and stepped: --resync-perturb-scripts);
    //       S     ... with sector K (--resync-sector) stamped 9
    //             (--resync-shift) ticks later: K's bodies exist in the
    //             server's state at S but not in the anchor's blob;
    //       A     ... with K stamped 9 ticks earlier: K's bodies exist in the
    //             anchor at S, the server's pass of K comes after S.
    //   apply     (2.6.3/2.6.4, the restore order of rollback_engine::
    //             restore_and_replay): the world's counters from the anchor
    //             with the server's union; the server's portable record
    //             localized against the anchor's v2 (keep_local_script) and
    //             restored with replay_activation; the bodies the anchor run
    //             made for a pass the server's union does not have at S
    //             destroyed and put back (arm A; before the blob, like the
    //             engine's births, or after it with
    //             --resync-destroy-after-blob); the anchor's blob (lenient,
    //             as the clients run it); every row written (wake/freeze as
    //             the row says, rechecked when it moves the body or the blob
    //             did not know it); live bodies the blob did not know and no
    //             row covers rechecked at once (resync mode: no deferral);
    //             the anchor's own recorded contacts re-made for the rowed
    //             bodies; then whole world ticks S..B+N.
    //   r, s, a   the same cases applied with the SERVER's blob of S instead
    //             of the anchor's (resync design 9 Q1's alternative); with
    //             --resync-server-blob FILE, another process's server blob
    //             (its --savestate-trace PREFIX.server.blob), its names
    //             mapped by --resync-blob-rename OLD NEW (the cross-world
    //             restore test: run the capturing process as --resync-player
    //             1 and this one as another id, so the own clone's name
    //             differs, as the own retail ball's does on a client);
    //             --resync-blob-adopt-environment writes this process's
    //             environment identity into its header (the restore refuses
    //             a blob of another environment).
    //   rows      a row the body already has bit for bit (and the blob knows
    //             the body) is not written: the beam would lose the hull
    //             manager's state, a sleeping core's PSI stamp and the entity
    //             matrix's low bits (--resync-write-equal-rows is the A/B).
    //   result    per arm: the blob of every boundary (S, B+N] against the
    //             straight run's (exact ticks), and per body the largest
    //             position error over (S, B] and over (B, B+N].  The gate
    //             bodies are --resync-watch plus the movable bodies of
    //             sector K.  E passes when every blob is identical; R and S
    //             when every gate body stays <= --resync-gate mm (1) over
    //             (B, B+N]; A when K's bodies are bit-exact from their
    //             re-birth on and the rest stay within arm R's worst error
    //             (when R ran too; else <= the gate).  Arm S also reports
    //             K's constraints after the apply (each core's named
    //             controllers against the server's blob at S).  An arm whose
    //             schedule does not produce its case (S: K born in the
    //             straight run before S and in the arm's after it; A: K
    //             passed by the server after S and born in the arm's before
    //             it) is INVALID and fails the check.  A gate body never
    //             compared fails its arm; a --resync-watch name that is not
    //             a movable body refuses the run.  The harness line (warm-up
    //             vs restored root) is informational: all runs share the
    //             restored root.
    //
    // The re-step is the server world's own whole tick, not a client's
    // replay recipe (the selected scripts' frame + PreSimulate + the physics
    // step): that recipe's fidelity is --savestate-check --frames' and the
    // sector-activation replay's, and the union re-derivation inside a
    // replay is B1's.  This check measures what the apply's STATE leaves
    // behind (resync design 9 Q1).
    // ------------------------------------------------------------------
    struct rc_pose {
        double p[3] = {};
        double q[4] = {};
        float v[3] = {};
        float w[3] = {};
        bool sim = false;
    };
    using rc_poses = std::map<std::string, rc_pose>;

    struct rc_birth {
        uint32_t tick = 0;
        bmmo::session::local_birth birth;
    };

    struct rc_state {
        uint32_t tick = 0;
        std::vector<uint8_t> blob;
        std::vector<uint8_t> v2;
        bmmo::sim::physics_world::step_state counters;
        std::map<std::string, std::vector<bmmo_physics_contact>> contacts;
        std::vector<bmmo::session::body_state> rows;   // the server's
        std::vector<uint8_t> portable;                 // the server's
    };

    struct rc_event {
        uint32_t tick = 0;
        int kind = 0;     // 0 stamp, 1 drop, 2 perturb
        int sector = 0;
    };

    struct rc_tick {
        rc_poses poses;
        std::vector<uint8_t> blob;
        uint64_t blob_hash = 0;
    };

    // Every movable body's state, by name (a repeated name gets "#n").
    rc_poses rc_capture_poses(CKIpionManager* physics) {
        rc_poses out;
        std::vector<bmmo_physics_body_state> bodies(256);
        int count = bmmo::physics::list_bodies(physics, bodies.data(), static_cast<int>(bodies.size()));
        if (count > static_cast<int>(bodies.size())) {
            bodies.resize(static_cast<size_t>(count));
            count = bmmo::physics::list_bodies(physics, bodies.data(), static_cast<int>(bodies.size()));
        }
        bodies.resize(static_cast<size_t>(std::max(0, count)));
        for (const auto& b: bodies) {
            if (!b.movable) continue;
            std::string name = b.name;
            for (int n = 2; out.count(name); ++n) name = std::string(b.name) + "#" + std::to_string(n);
            rc_pose& p = out[name];
            for (int k = 0; k < 3; ++k) { p.p[k] = b.position[k]; p.v[k] = b.linear[k]; p.w[k] = b.angular[k]; }
            for (int k = 0; k < 4; ++k) p.q[k] = b.rotation[k];
            p.sim = b.simulated;
        }
        return out;
    }

    bool rc_same_bits(const rc_pose& a, const rc_pose& b) {
        return std::memcmp(a.p, b.p, sizeof a.p) == 0 && std::memcmp(a.q, b.q, sizeof a.q) == 0
            && std::memcmp(a.v, b.v, sizeof a.v) == 0 && std::memcmp(a.w, b.w, sizeof a.w) == 0 && a.sim == b.sim;
    }

    double rc_distance(const rc_pose& a, const rc_pose& b) {
        double d = 0.0;
        for (int k = 0; k < 3; ++k) d += (a.p[k] - b.p[k]) * (a.p[k] - b.p[k]);
        return std::sqrt(d);
    }

    // "core <name> ... controllers=[...]" of a describe_savestate text: the
    // named controllers (kind 1: constraints, gravity, forces) per core,
    // the friction handles (kind 0, whose system index is bookkeeping) left
    // out.
    std::map<std::string, std::string> rc_named_controllers(const std::string& text) {
        std::map<std::string, std::string> out;
        size_t pos = 0;
        while (pos < text.size()) {
            const size_t end = text.find('\n', pos);
            const std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? text.size() : end + 1;
            if (line.rfind("core ", 0) != 0) continue;
            const size_t space = line.find(' ', 5);
            const std::string name = line.substr(5, space == std::string::npos ? std::string::npos : space - 5);
            const size_t at = line.find(" controllers=[");
            if (at == std::string::npos) continue;
            const size_t close = line.find(']', at);
            const std::string list = line.substr(at + 14, close == std::string::npos ? std::string::npos : close - at - 14);
            std::vector<std::string> named;
            size_t p = 0;
            while ((p = list.find('{', p)) != std::string::npos) {
                const size_t q = list.find('}', p);
                const std::string entry = list.substr(p + 1, q == std::string::npos ? std::string::npos : q - p - 1);
                p = q == std::string::npos ? list.size() : q + 1;
                if (entry.rfind("k1 ", 0) != 0) continue;
                // "k1 s<sys> p<part> <priority> <name>": the priority and the name
                size_t f = entry.find(' ', 3);
                f = f == std::string::npos ? f : entry.find(' ', f + 1);
                named.push_back(f == std::string::npos ? entry : entry.substr(f + 1));
            }
            std::sort(named.begin(), named.end());
            std::string joined;
            for (const auto& n: named) joined += (joined.empty() ? "" : ",") + n;
            out[name] = joined;
        }
        return out;
    }

    // A birth's pass (design 2.6.3): the pass at M with M + 5 == b, else the
    // newest with M + 5 <= b, else the newest with M <= b; 0 = none (a body
    // of the root's world).
    int pass_of_list(const std::vector<std::pair<uint32_t, int>>& passes, uint32_t b) {
        for (const auto& [m, s]: passes) if (m + 5 == b) return s;
        int found = 0;
        for (const auto& [m, s]: passes) if (m + 5 <= b) found = s;
        if (found) return found;
        for (const auto& [m, s]: passes) if (m <= b) found = s;
        return found;
    }

    std::vector<rc_birth> births_of_list(const std::vector<rc_birth>& births,
                                         const std::vector<std::pair<uint32_t, int>>& passes, int sector) {
        std::vector<rc_birth> out;
        for (const auto& b: births) if (pass_of_list(passes, b.tick) == sector) out.push_back(b);
        return out;
    }

    // Every data array's cells (the harness's root restore): the level's
    // run-time tables (PH's Set? column, the manager's and the modules'
    // cells) are no script's state and no body's, so neither record carries
    // them.  Integer, float and object cells by value, strings by text,
    // parameter cells by their data bytes.
    struct rc_cell {
        CK_ID array = 0;
        int row = 0, column = 0;
        CK_ARRAYTYPE type = CKARRAYTYPE_INT;
        CKUINTPTR value = 0;
        std::string text;
        std::vector<uint8_t> bytes;
    };

    std::vector<rc_cell> rc_capture_arrays(CKContext* context, std::map<CK_ID, int>& rows) {
        std::vector<rc_cell> out;
        rows.clear();
        const int count = context->GetObjectsCountByClassID(CKCID_DATAARRAY);
        CK_ID* ids = context->GetObjectsListByClassID(CKCID_DATAARRAY);
        for (int i = 0; i < count; ++i) {
            auto* a = CKDataArray::Cast(context->GetObject(ids[i]));
            if (!a) continue;
            rows[ids[i]] = a->GetRowCount();
            for (int r = 0; r < a->GetRowCount(); ++r)
                for (int c = 0; c < a->GetColumnCount(); ++c) {
                    rc_cell cell;
                    cell.array = ids[i];
                    cell.row = r;
                    cell.column = c;
                    cell.type = a->GetColumnType(c);
                    CKUINTPTR* e = a->GetElement(static_cast<size_t>(r), static_cast<size_t>(c));
                    if (!e) continue;
                    if (cell.type == CKARRAYTYPE_STRING) {
                        const char* t = reinterpret_cast<const char*>(*e);
                        cell.text = t ? t : "";
                    } else if (cell.type == CKARRAYTYPE_PARAMETER) {
                        auto* p = reinterpret_cast<CKParameter*>(*e);
                        if (!p) continue;
                        const auto* data = static_cast<const uint8_t*>(p->GetReadDataPtr(FALSE));
                        if (data) cell.bytes.assign(data, data + p->GetDataSize());
                    } else {
                        cell.value = *e;
                    }
                    out.push_back(std::move(cell));
                }
        }
        return out;
    }

    // Writes the cells back where they differ; `changed` gets "array:cells"
    // of what differed.
    int rc_restore_arrays(CKContext* context, const std::vector<rc_cell>& cells, const std::map<CK_ID, int>& rows,
                          std::string& changed) {
        std::map<std::string, int> by_array;
        int written = 0;
        for (const auto& [id, n]: rows)
            if (auto* a = CKDataArray::Cast(context->GetObject(id)); a && a->GetRowCount() != n)
                changed += std::string(" ") + (a->GetName() ? a->GetName() : "?") + ":rows " + std::to_string(a->GetRowCount())
                         + "->" + std::to_string(n) + "(not restored)";
        for (const auto& cell: cells) {
            auto* a = CKDataArray::Cast(context->GetObject(cell.array));
            if (!a || cell.row >= a->GetRowCount() || cell.column >= a->GetColumnCount()) continue;
            CKUINTPTR* e = a->GetElement(static_cast<size_t>(cell.row), static_cast<size_t>(cell.column));
            if (!e) continue;
            bool differs = false;
            if (cell.type == CKARRAYTYPE_STRING) {
                const char* t = reinterpret_cast<const char*>(*e);
                if (cell.text != (t ? t : "")) {
                    differs = true;
                    a->SetElementStringValue(cell.row, cell.column, const_cast<CKSTRING>(cell.text.c_str()));
                }
            } else if (cell.type == CKARRAYTYPE_PARAMETER) {
                auto* p = reinterpret_cast<CKParameter*>(*e);
                if (!p || static_cast<size_t>(p->GetDataSize()) != cell.bytes.size()) continue;
                auto* data = static_cast<uint8_t*>(p->GetWriteDataPtr());
                if (data && std::memcmp(data, cell.bytes.data(), cell.bytes.size()) != 0) {
                    differs = true;
                    std::memcpy(data, cell.bytes.data(), cell.bytes.size());
                }
            } else if (*e != cell.value) {
                differs = true;
                *e = cell.value;
            }
            if (differs) {
                ++written;
                ++by_array[a->GetName() ? a->GetName() : "?"];
            }
        }
        for (const auto& [name, n]: by_array) changed += " " + name + ":" + std::to_string(n);
        return written;
    }

    // The selected scripts the script record wrote ACTIVE whose activity the
    // behaviour manager does not know: script_replay::restore sets
    // CKBEHAVIOR_ACTIVE with ModifyFlags (script_replay.hpp restore_behavior),
    // but a script that finished on its own cleared its scene descriptor's
    // CK_SCENEOBJECT_ACTIVE (CK2 CKBehavior.cpp CheckBehaviorActivity) and an
    // owner with no active script left the manager's object list
    // (CKBeObject::ExecuteBehaviors -> RemoveObjectNextFrame); only a scene
    // activation puts it back (CKScene::ActivateScript -> AddObjectNextFrame).
    // So the live frames never execute such a script again (a replay's
    // execute_frame does: it calls Execute directly).  Counted, and with `fix`
    // re-registered: the scene flag set and the owner added for the next
    // frame, the way ActivateScript does it, without its next-frame flags or
    // callbacks (the record already holds the script's state).
    int rc_unregistered_scripts(CKContext* context, const bmmo::game::script_replay::selection& sel, bool fix,
                                std::string* names = nullptr) {
        CKScene* scene = context ? context->GetCurrentScene() : nullptr;
        if (!scene) return 0;
        int count = 0;
        for (CK_ID id: sel.scripts) {
            auto* beh = CKBehavior::Cast(context->GetObject(id));
            if (!beh || !beh->IsActive()) continue;
            const CKDWORD flags = scene->GetObjectFlags(beh);
            if (flags & CK_SCENEOBJECT_ACTIVE) continue;
            ++count;
            if (names && count <= 8) {
                CKBeObject* owner = beh->GetOwner();
                *names += std::string(" ") + (owner && owner->GetName() ? owner->GetName() : "?") + "/"
                        + (beh->GetName() ? beh->GetName() : "?");
            }
            if (!fix) continue;
            scene->SetObjectFlags(beh, static_cast<CK_SCENEOBJECT_FLAGS>(flags | CK_SCENEOBJECT_ACTIVE));
            if (CKBeObject* owner = beh->GetOwner()) context->GetBehaviorManager()->AddObjectNextFrame(owner);
        }
        return count;
    }

    int run_resync_check(const arguments& args) {
        using bmmo::game::script_replay;
        // Unbuffered: MSVC's _IOLBF is full buffering, and a fast-fail
        // (heap corruption) takes the buffer with it.
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        const int after = args.resync_after;
        if (args.resync_s < 1 || args.resync_b < args.resync_s || after < 1) {
            std::fprintf(stderr, "--resync-check S B needs 1 <= S <= B and --resync-after >= 1\n");
            return 2;
        }
        for (char arm: args.resync_arms)
            if (std::string("ERSArsa").find(arm) == std::string::npos) {
                std::fprintf(stderr, "--resync-arms takes E, R, S and A (r, s, a: the same with the server's blob)\n");
                return 2;
            }
        const bool want_s = args.resync_arms.find_first_of("Ss") != std::string::npos;
        const bool want_a = args.resync_arms.find_first_of("Aa") != std::string::npos;
        const bool want_r = args.resync_arms.find_first_of("Rr") != std::string::npos;
        std::vector<std::pair<int, int>> stamps = args.resync_stamps;
        if (stamps.empty()) stamps.emplace_back(1, 0);
        const int k_sector = args.resync_sector;
        if ((want_s || want_a)
                && std::none_of(stamps.begin(), stamps.end(), [&](const std::pair<int, int>& s) { return s.first == k_sector; })) {
            std::fprintf(stderr, "arms S and A shift the stamp of --resync-sector K, which --resync-stamp must schedule\n");
            return 2;
        }
        const bool drop = args.resync_drop >= 0;
        if (drop && args.drop_entity.empty() && !args.have_drop_at) {
            std::fprintf(stderr, "--resync-drop needs --drop ENTITY TYPE (and optionally --drop-at X Y Z)\n");
            return 2;
        }
        if (drop && want_r && args.resync_drop >= args.resync_s - args.resync_perturb_ticks) {
            std::fprintf(stderr, "--resync-drop must come before arm R's perturbation (S-%d)\n", args.resync_perturb_ticks);
            return 2;
        }
        if (drop && args.resync_drop >= args.resync_s) {
            std::fprintf(stderr, "--resync-drop must come before S (a lifecycle event of the re-step is not modelled)\n");
            return 2;
        }
        // Arm R's perturbation is an event of the anchor run, which starts at
        // the root: at or before the root it would never be applied and arm R
        // would quietly be arm E.
        if (want_r && (args.resync_perturb_ticks < 1 || args.resync_s <= args.resync_perturb_ticks)) {
            std::fprintf(stderr, "arm R needs 1 <= --resync-perturb-ticks < S (the perturbation at S-%d is before the root)\n",
                         args.resync_perturb_ticks);
            return 2;
        }
        // Arm A is judged against arm R's band, so R must run first.
        {
            const size_t first_a = args.resync_arms.find_first_of("Aa");
            const size_t first_r = args.resync_arms.find('R');
            if (first_a != std::string::npos && first_r != std::string::npos && first_r > first_a) {
                std::fprintf(stderr, "--resync-arms: R must come before A and a (they are judged against R's band)\n");
                return 2;
            }
        }
        if (args.resync_player < 1) {
            std::fprintf(stderr, "--resync-player needs an id >= 1\n");
            return 2;
        }
        if (!args.resync_server_blob.empty() && args.resync_arms.find_first_of("rsa") == std::string::npos) {
            std::fprintf(stderr, "--resync-server-blob is used by the r, s and a arms only\n");
            return 2;
        }
        const uint32_t pid = static_cast<uint32_t>(args.resync_player);

        auto world = create_session_world(args, 0.0f);
        if (!world) return 1;
        std::string error;
        if (!world->add_player(pid, 0, error)) {
            std::fprintf(stderr, "add_player failed: %s\n", error.c_str());
            return 1;
        }
        CKContext* context = world->engine().context();
        CKIpionManager* physics = world->physics();
        if (!physics || !physics->GetEnvironment()) {
            std::fprintf(stderr, "resync-check: no physics environment\n");
            return 1;
        }
        const script_replay::selection sel = bmmo::game::session_script_selection(context);
        bmmo::game::script_replay_hooks hooks;
        hooks = bmmo::game::force_hooks_for(bmmo::physics::physics_direct(physics));   // step HU1: one copy (game/host_backend.hpp)
        const script_replay::keep_local_fn keep_rule = [](const std::string& owner, const std::string& script) {
            return bmmo::session::keep_local_script(owner, script);
        };
        // The client's binding over this world: the body, recheck, contact and
        // savestate writes are the rollback's own code paths (lenient, as both
        // clients run the savestate).
        bmmo::physics::physics_direct backend(physics);
        bmmo::session::rollback_binding_options binding_options;
        binding_options.delta_ms = bmmo::sim::kFixedDeltaMs;
        binding_options.savestate = true;
        binding_options.savestate_flags = 0u;
        binding_options.physics_globals = true;   // C3 (step RK1): the clients' rule, as bind_session_world sets it
        std::string blob_refusal;   // the last set_savestate's error (empty when it was taken)
        binding_options.on_savestate_restored = [&blob_refusal](bool ok, const std::string& e,
                                                                const bmmo::session::savestate_outcome&) {
            blob_refusal = ok ? std::string() : e;
        };
        bmmo::session::rollback_world binding = bmmo::session::bind_rollback_world(backend, binding_options);

        // The PH table: which module is in which sector (the sector K of the
        // arms is chosen from this).
        if (auto* ph = CKDataArray::Cast(context->GetObjectByNameAndClass(const_cast<CKSTRING>("PH"), CKCID_DATAARRAY, nullptr))) {
            int column = -1, name_column = 0;
            std::string columns;
            for (int c = 0; c < ph->GetColumnCount(); ++c) {
                const char* cn = ph->GetColumnName(c);
                if (cn && std::strcmp(cn, "Sector") == 0) column = c;
                if (cn && (std::strcmp(cn, "Objekt") == 0 || std::strcmp(cn, "Object") == 0 || std::strcmp(cn, "Name") == 0
                           || std::strcmp(cn, "Entity") == 0))
                    name_column = c;
                columns += std::string(" ") + (cn ? cn : "?");
            }
            if (args.verbose) std::printf("PH columns:%s\n", columns.c_str());
            std::map<int, std::string> by_sector;
            for (int r = 0; column >= 0 && r < ph->GetRowCount(); ++r) {
                int sector = 0;
                char name[256] = {};
                if (!ph->GetElementValue(r, column, &sector)) continue;
                ph->GetElementStringValue(r, name_column, name, static_cast<int>(sizeof(name)));
                by_sector[sector] += std::string(by_sector[sector].empty() ? "" : " ") + name;
            }
            for (const auto& [sector, names]: by_sector) std::printf("PH sector %d: %s\n", sector, names.c_str());
        }

        // The root: --resync-root whole world ticks first, with no event, so
        // the retail ball is parked (its body removed, design 9.x) and the
        // retail blocks that then retry every frame (Sound_Manager's
        // collision detections, Gameplay_Ingame's wake-up) are queued as they
        // will be at every later restore: the savestate refuses a PreSimulate
        // queue that differs from its own.
        if (!settle_world(*world, std::max(0, args.resync_root))) return 1;
        const uint32_t root_tick = world->tick_index();
        const uint32_t S = root_tick + static_cast<uint32_t>(args.resync_s);
        const uint32_t B = root_tick + static_cast<uint32_t>(args.resync_b);
        const uint32_t end = B + static_cast<uint32_t>(after);
        std::printf("resync-check level=%d root=%u S=%u B=%u end=%u (depth %u) arms=%s K=%d shift=%d perturb=%.4f m at S-%d"
                    " selected=%zu gate=%.3f mm\n", world->level(), root_tick, S, B, end, B - S, args.resync_arms.c_str(),
                    k_sector, args.resync_shift, args.resync_perturb, args.resync_perturb_ticks, sel.scripts.size(),
                    args.resync_gate_mm);

        // ---- the run machinery
        std::vector<rc_birth> births;                  // this run's, in order
        std::vector<std::pair<uint32_t, int>> passes;  // this run's (tick stepped, sector)
        std::set<int> active_before;
        std::set<std::string> ball_names;
        for (const auto& row: world->ball_rows()) ball_names.insert(row.name);
        const auto is_ball = [&](const std::string& name) {
            return name.find("_BMMO_") != std::string::npos || ball_names.count(name) != 0;
        };
        const auto drain = [&](uint32_t t) {
            const std::string events = bmmo::physics::drain_event_log(physics);
            bmmo::session::parse_event_log(events, [&](const std::string& kind, const std::string& text) {
                if (kind != "recipe") return;
                rc_birth b;
                if (!bmmo::session::parse_recipe_event(text, b.birth) || is_ball(b.birth.name)) return;
                b.tick = t;
                births.push_back(std::move(b));
            });
        };
        const auto note_passes = [&](uint32_t t) {
            const std::set<int>& now = world->active_sectors();
            for (int s: now) if (!active_before.count(s)) passes.emplace_back(t, s);
            active_before = now;
        };
        const auto pass_of = [&](uint32_t b) { return pass_of_list(passes, b); };
        const auto births_of = [&](int sector) { return births_of_list(births, passes, sector); };
        const auto ball_entity = [&]() {
            return world->ball_rows()[world->player_ball_type(pid)].name + "_BMMO_" + std::to_string(pid);
        };
        const auto apply_events = [&](uint32_t t, const std::vector<rc_event>& events) -> bool {
            for (const auto& e: events) {
                if (e.tick != t) continue;
                std::string event_error;
                if (e.kind == 0) {
                    bmmo::sim::lifecycle_event where;
                    where.type = bmmo::session::event_type::Sector;
                    where.sector = e.sector;
                    where.tick = t;
                    if (!world->apply_event(pid, where, event_error)) {
                        std::fprintf(stderr, "sector event failed: %s\n", event_error.c_str());
                        return false;
                    }
                } else if (e.kind == 1) {
                    VxVector spawn(args.drop_at[0], args.drop_at[1], args.drop_at[2]);
                    if (!args.have_drop_at) {
                        auto* target = CK3dEntity::Cast(context->GetObjectByNameAndParentClass(
                            const_cast<CKSTRING>(args.drop_entity.c_str()), CKCID_3DENTITY, nullptr));
                        if (!target) {
                            std::fprintf(stderr, "no 3D entity named %s (pass --drop-at X Y Z)\n", args.drop_entity.c_str());
                            return false;
                        }
                        target->GetPosition(&spawn);
                    }
                    spawn.y += args.drop_height;
                    bmmo::sim::lifecycle_event event;
                    event.type = bmmo::session::event_type::Physicalize;
                    event.ball_type = static_cast<uint8_t>(args.drop_ball);
                    event.position[0] = spawn.x;
                    event.position[1] = spawn.y;
                    event.position[2] = spawn.z;
                    event.recipe = world->retail_recipe(event.ball_type);
                    event.tick = t;
                    if (!world->apply_event(pid, event, event_error)) {
                        std::fprintf(stderr, "physicalize failed: %s\n", event_error.c_str());
                        return false;
                    }
                } else if (e.kind == 2 && args.resync_perturb_scripts) {
                    // Arm R as 7.2 words it: the selected scripts of S-N
                    // captured and restored at S-N (replay_activation), then
                    // stepped - a client rollback's script restore, with no
                    // body moved.
                    std::vector<uint8_t> record;
                    std::string e2;
                    if (!script_replay::capture(context, sel, hooks, record)
                            || !script_replay::restore(context, sel, hooks, record, &e2, nullptr, nullptr, true)) {
                        std::fprintf(stderr, "arm R: the script restore at %u failed: %s\n", t, e2.c_str());
                        return false;
                    }
                    if (args.verbose) std::printf("  scripts restored at tick %u\n", t);
                } else if (e.kind == 2) {
                    // Arm R: every movable body moved, the awake ones sped up.
                    const double m = args.resync_perturb;
                    int moved = 0;
                    for (const auto& [name, pose]: rc_capture_poses(physics)) {
                        if (name.find('#') != std::string::npos) continue;
                        bmmo_physics_body_state state{};
                        if (!binding.get_body(name, state)) continue;
                        state.position[0] += m;
                        state.position[2] += m;
                        if (state.simulated) {
                            state.linear[0] += static_cast<float>(5.0 * m);
                            state.linear[2] += static_cast<float>(5.0 * m);
                        }
                        if (binding.set_body(name, state, bmmo::session::wake_mode::keep, true)) ++moved;
                    }
                    if (args.verbose) std::printf("  perturbed %d bodies at tick %u\n", moved, t);
                }
            }
            return true;
        };
        // Steps until boundary `stop`, calling `at` at every boundary on the
        // way (before that boundary's events), `stop` included.
        const auto run_until = [&](uint32_t stop, const std::vector<rc_event>& events,
                                   const std::function<bool(uint32_t)>& at) -> bool {
            for (;;) {
                const uint32_t t = world->tick_index();
                if (at && !at(t)) return false;
                if (t >= stop) return true;
                if (!apply_events(t, events)) return false;
                std::string tick_error;
                if (!world->tick(tick_error)) {
                    std::fprintf(stderr, "tick %u failed: %s\n", t, tick_error.c_str());
                    return false;
                }
                drain(t);
                note_passes(t);
            }
        };
        const auto capture_state = [&](rc_state& out, bool server) -> bool {
            out = rc_state{};
            out.tick = world->tick_index();
            std::string e;
            if (!bmmo::physics::get_savestate(physics, out.blob, e)) {
                std::fprintf(stderr, "savestate capture failed at %u: %s\n", out.tick, e.c_str());
                return false;
            }
            if (!script_replay::capture(context, sel, hooks, out.v2)) {
                std::fprintf(stderr, "script capture failed at %u\n", out.tick);
                return false;
            }
            out.counters = world->capture_step_state();
            for (const auto& [name, pose]: rc_capture_poses(physics)) {
                if (name.find('#') != std::string::npos) continue;
                std::vector<bmmo_physics_contact> contacts;
                if (binding.get_contacts(name, contacts) > 0) out.contacts[name] = std::move(contacts);
            }
            if (server) {
                world->snapshot_for_journal(out.rows);
                if (!script_replay::capture_portable(context, sel, hooks, out.portable, &e)) {
                    std::fprintf(stderr, "portable capture failed at %u: %s\n", out.tick, e.c_str());
                    return false;
                }
            }
            return true;
        };
        // Destroys births (newest first, each name once), the event log
        // muted, each entity put back where its first listed Physicalize
        // found it, and the joint handles the scripts held on it nulled
        // (IVP freed those joints with the body) - the rollback's
        // destroy_body + put_back_birth + the script binding's
        // release_joints (rollback_binding_scripts.hpp).
        const auto destroy = [&](const std::vector<rc_birth>& list) {
            std::set<std::string> done;
            int destroyed = 0;
            bmmo::physics::set_event_log_muted(true);
            for (auto it = list.rbegin(); it != list.rend(); ++it) {
                if (!done.insert(it->birth.name).second) continue;
                const auto first = std::find_if(list.begin(), list.end(),
                                                [&](const rc_birth& b) { return b.birth.name == it->birth.name; });
                const std::vector<CKBehavior*> held = script_replay::joints_on(context, sel, it->birth.name);
                std::string e;
                if (bmmo::physics::unphysicalize(physics, it->birth.name.c_str(), e)) {
                    ++destroyed;
                    script_replay::release_joints(held);
                }
                bmmo::game::put_back_birth(context, it->birth.name, first->birth);
            }
            bmmo::physics::set_event_log_muted(false);
            return destroyed;
        };

        // ---- the root, and the warm-up run (run 0) that makes every run after
        // it a run from a restored root
        std::vector<rc_event> straight_events;
        for (const auto& [sector, tick]: stamps) straight_events.push_back({root_tick + static_cast<uint32_t>(tick), 0, sector});
        if (drop) straight_events.push_back({root_tick + static_cast<uint32_t>(args.resync_drop), 1, 0});
        rc_state root;
        if (!capture_state(root, false)) return 1;
        // Every 3D entity's world matrix at the root: the savestate carries
        // the matrices of the physicalized entities only, and the server
        // world's no-ball proximity fallback is Ball_Pos_Frame, the child of
        // the parked retail ball, which follows the last clone (world-
        // agreement-gaps design G-c6): left where the previous run ended, it
        // gives the TT Scaleable Proximity countdowns another phase before
        // the drop (Last Check 29 vs 8 at root+39 on Level 11, the far
        // pusher woken at root+68 in one run and never in the other).  Put
        // back at every root restore, each with its children kept.
        std::map<CK_ID, int> root_array_rows;
        const std::vector<rc_cell> root_cells = rc_capture_arrays(context, root_array_rows);
        // The hierarchy too: a module script re-parents its parts (the
        // balloon's), and a pass places a module by moving its frame, which
        // carries only the children it still has.
        // (GetObjectsListByClassID lists one class exactly: every 3D class.)
        struct root_entity { CK_ID id = 0; CK_ID parent = 0; VxMatrix matrix; bool visible = false; };
        std::vector<root_entity> root_matrices;
        for (const CK_CLASSID cid: {CKCID_3DENTITY, CKCID_3DOBJECT, CKCID_PLACE, CKCID_CURVE, CKCID_SPRITE3D, CKCID_CHARACTER,
                                    CKCID_GRID, CKCID_CAMERA, CKCID_TARGETCAMERA, CKCID_LIGHT, CKCID_TARGETLIGHT}) {
            const int count = context->GetObjectsCountByClassID(cid);
            CK_ID* ids = context->GetObjectsListByClassID(cid);
            for (int i = 0; i < count; ++i)
                if (auto* entity = CK3dEntity::Cast(context->GetObject(ids[i]))) {
                    root_entity r;
                    r.id = ids[i];
                    r.parent = entity->GetParent() ? entity->GetParent()->GetID() : 0;
                    r.matrix = entity->GetWorldMatrix();
                    r.visible = entity->IsVisible() != FALSE;
                    root_matrices.push_back(r);
                }
        }
        active_before = world->active_sectors();
        bmmo::physics::drain_event_log(physics);
        const auto restore_root = [&]() -> bool {
            std::string e;
            if (world->player_physicalized(pid)) {
                bmmo::sim::lifecycle_event gone;
                gone.type = bmmo::session::event_type::Unphysicalize;
                if (!world->apply_event(pid, gone, e)) {
                    std::fprintf(stderr, "root: unphysicalize failed: %s\n", e.c_str());
                    return false;
                }
            }
            const uint64_t reregistered_before = script_replay::membership().reregistered;
            {
                const live_membership_scope live(true);   // step B2SW (G1): the restore re-registers
                if (!script_replay::restore(context, sel, hooks, root.v2, &e, nullptr, nullptr, true)) {
                    std::fprintf(stderr, "root: script restore failed: %s\n", e.c_str());
                    return false;
                }
            }
            // What the record made active is re-registered by the restore
            // itself since step B2SW (G1); the harness only counts what is
            // left (and re-registers it with --resync-reregister, the A/B).
            std::string names;
            const uint64_t reregistered = script_replay::membership().reregistered - reregistered_before;
            if (reregistered && args.verbose)
                std::printf("  root: the restore re-registered %llu script(s) with the behaviour manager\n",
                            static_cast<unsigned long long>(reregistered));
            if (const int n = rc_unregistered_scripts(context, sel, args.resync_reregister, &names); n > 0)
                std::printf("  root: %d restored-active script(s) unknown to the behaviour manager%s:%s\n", n,
                            args.resync_reregister ? " (re-registered)" : " (left)", names.c_str());
            destroy(births);
            int reparented = 0;
            for (const auto& r: root_matrices) {
                auto* entity = CK3dEntity::Cast(context->GetObject(r.id));
                if (!entity) continue;
                const CK_ID now = entity->GetParent() ? entity->GetParent()->GetID() : 0;
                if (now == r.parent) continue;
                entity->SetParent(CK3dEntity::Cast(context->GetObject(r.parent)), TRUE);
                ++reparented;
            }
            if (reparented && args.verbose) std::printf("  root: %d entities re-parented\n", reparented);
            std::string cells_changed;
            if (const int n = rc_restore_arrays(context, root_cells, root_array_rows, cells_changed); (n || !cells_changed.empty())
                    && args.verbose)
                std::printf("  root: %d data-array cells put back:%s\n", n, cells_changed.c_str());
            bmmo::physics::savestate_report report;
            if (!bmmo::physics::set_savestate(physics, root.blob.data(), root.blob.size(), 0u, e, &report)) {
                std::fprintf(stderr, "root: savestate refused: %s\n", e.c_str());
                return false;
            }
            // After the blob, whose entity writes carry their children along.
            int moved = 0, shown = 0;
            for (const auto& r: root_matrices)
                if (auto* entity = CK3dEntity::Cast(context->GetObject(r.id))) {
                    if ((entity->IsVisible() != FALSE) != r.visible) {
                        entity->Show(r.visible ? CKSHOW : CKHIDE);
                        ++shown;
                    }
                    if (std::memcmp(&entity->GetWorldMatrix(), &r.matrix, sizeof(VxMatrix)) == 0) continue;
                    entity->SetWorldMatrix(r.matrix, TRUE);
                    ++moved;
                }
            if ((moved || shown) && args.verbose)
                std::printf("  root: %d entity matrices and %d visibilities put back\n", moved, shown);
            if (!report.unknown_bodies.empty() || report.skipped) {
                std::fprintf(stderr, "root: the restored world is not the root's (unknown=%zu, first %s; skipped=%u)\n",
                             report.unknown_bodies.size(),
                             report.unknown_bodies.empty() ? "-" : report.unknown_bodies.front().c_str(), report.skipped);
                return false;
            }
            world->restore_step_state(root.counters);
            births.clear();
            passes.clear();
            active_before = world->active_sectors();
            bmmo::physics::drain_event_log(physics);
            return true;
        };
        std::map<uint32_t, rc_poses> warm;   // the warm-up run's bodies at every boundary
        const auto started = std::chrono::steady_clock::now();
        const uint32_t diag_tick = args.resync_harness_diag >= 0 ? root_tick + static_cast<uint32_t>(args.resync_harness_diag) : 0;
        std::vector<uint8_t> diag_scripts[2], diag_blob[2];
        std::string diag_cells[2];
        const auto diag_capture = [&](int k) {
            script_replay::capture(context, sel, hooks, diag_scripts[k]);
            for (const auto& name: args.resync_watch) {
                auto* entity = CK3dEntity::Cast(context->GetObjectByNameAndParentClass(const_cast<CKSTRING>(name.c_str()),
                                                                                     CKCID_3DENTITY, nullptr));
                if (!entity) continue;
                VxVector at;
                entity->GetPosition(&at);
                CK3dEntity* parent = entity->GetParent();
                char line[400];
                int same_name = 0;
                {
                    const int count = context->GetObjectsCountByClassID(CKCID_3DENTITY);
                    CK_ID* ids = context->GetObjectsListByClassID(CKCID_3DENTITY);
                    for (int i = 0; i < count; ++i) {
                        CKObject* o = context->GetObject(ids[i]);
                        if (o && o->GetName() && name == o->GetName()) ++same_name;
                    }
                }
                std::snprintf(line, sizeof(line), "\n  entity %s #%u (%d of the name) at (%.4f,%.4f,%.4f) parent %s visible %d",
                              name.c_str(), static_cast<unsigned>(entity->GetID()), same_name, at.x, at.y,
                              at.z, parent && parent->GetName() ? parent->GetName() : "-", entity->IsVisible() ? 1 : 0);
                diag_cells[k] += line;
            }
            CKScene* scene = context->GetCurrentLevel() ? context->GetCurrentLevel()->GetCurrentScene() : nullptr;
            for (CK_ID id: sel.scripts) {
                auto* beh = CKBehavior::Cast(context->GetObject(id));
                if (!beh || !beh->GetName() || std::string(beh->GetName()).find("P_Modul_01") == std::string::npos) continue;
                std::string children;
                for (int i = 0; i < beh->GetSubBehaviorCount(); ++i) {
                    CKBehavior* c = beh->GetSubBehavior(i);
                    if (c && c->IsActive()) children += std::string(" ") + (c->GetName() ? c->GetName() : "?");
                }
                diag_cells[k] += "\n  " + std::to_string(id) + " active=" + std::to_string(beh->IsActive() ? 1 : 0)
                               + " flags=" + std::to_string(beh->GetFlags()) + " scene="
                               + std::to_string(scene ? static_cast<unsigned>(scene->GetObjectFlags(beh)) : 0u)
                               + " active children:" + children;
            }
            std::string ignored;
            bmmo::physics::get_savestate(physics, diag_blob[k], ignored);
        };
        if (!run_until(end, straight_events, [&](uint32_t t) {
                warm[t] = rc_capture_poses(physics);
                if (diag_tick && t == diag_tick) diag_capture(0);
                return true;
            }))
            return 1;
        std::map<uint32_t, rc_poses> early;  // the straight run's before S

        // ---- the straight run (the server)
        if (!restore_root()) return 1;
        std::map<uint32_t, rc_tick> straight;
        rc_state server;
        std::string blob_error;
        if (!run_until(end, straight_events, [&](uint32_t t) {
                if (t == S && !capture_state(server, true)) return false;
                if (diag_tick && t == diag_tick) diag_capture(1);
                if (t < S) {
                    early[t] = rc_capture_poses(physics);
                    return true;
                }
                rc_tick& rec = straight[t];
                rec.poses = rc_capture_poses(physics);
                if (!bmmo::physics::get_savestate(physics, rec.blob, blob_error)) {
                    std::fprintf(stderr, "resync-check: the straight run's savestate at %u failed: %s\n", t, blob_error.c_str());
                    return false;
                }
                rec.blob_hash = bmmo::physics::hash_savestate(rec.blob.data(), rec.blob.size());
                return true;
            }))
            return 1;
        if (diag_tick) {
            std::printf("harness diag warm-up:%s\nharness diag restored:%s\n", diag_cells[0].c_str(), diag_cells[1].c_str());
            const auto d = script_replay::diff(context, sel, diag_scripts[0], diag_scripts[1]);
            std::printf("harness diag at %u: scripts %s (%d logical, %d float); blob: %s\n", diag_tick,
                        d.any ? (d.where + " | " + d.detail).c_str() : "identical", d.logical_count, d.float_count,
                        bmmo::physics::diff_savestate(diag_blob[0].data(), diag_blob[0].size(), diag_blob[1].data(),
                                                      diag_blob[1].size(), false, 6, false).c_str());
        }
        const std::vector<rc_birth> straight_births = births;
        const std::vector<std::pair<uint32_t, int>> straight_passes = passes;
        {
            // The harness itself: a run from the restored root against the
            // warm-up run (the first run, from the live root): the first
            // boundary a body differs at, and the worst at the end.
            double worst = 0.0;
            std::string worst_name, first_name;
            uint32_t first_tick = 0;
            int missing = 0;
            for (uint32_t t = root_tick; t <= end && !first_tick; ++t) {
                const rc_poses& mine = t < S ? early[t] : straight[t].poses;
                for (const auto& [name, pose]: mine) {
                    const auto it = warm[t].find(name);
                    if (it == warm[t].end() || !rc_same_bits(pose, it->second)) {
                        first_tick = t;
                        first_name = name;
                        break;
                    }
                }
            }
            for (const auto& [name, pose]: straight[end].poses) {
                const auto it = warm[end].find(name);
                if (it == warm[end].end()) { ++missing; continue; }
                const double d = rc_distance(pose, it->second);
                if (d > worst) { worst = d; worst_name = name; }
            }
            if (first_tick) {
                std::printf("harness: the warm-up run and the runs from the restored root first differ at boundary %u (%s)\n",
                            first_tick, first_name.c_str());
                {
                    const rc_poses& mine = first_tick < S ? early[first_tick] : straight[first_tick].poses;
                    std::string differing;
                    int n = 0;
                    for (const auto& [name, pose]: mine) {
                        const auto it = warm[first_tick].find(name);
                        if (it != warm[first_tick].end() && rc_same_bits(pose, it->second)) continue;
                        if (++n <= 40) differing += " " + name;
                    }
                    std::printf("harness:   %d bodies differ there:%s\n", n, differing.c_str());
                }
                for (uint32_t t = first_tick > root_tick + 2 ? first_tick - 2 : root_tick; t <= std::min(end, first_tick + 6); ++t) {
                    const rc_poses& mine = t < S ? early[t] : straight[t].poses;
                    const auto x = warm[t].find(first_name);
                    const auto y = mine.find(first_name);
                    if (x == warm[t].end() || y == mine.end()) continue;
                    std::printf("harness:   %u warm (%.9f,%.9f,%.9f)%s restored (%.9f,%.9f,%.9f)%s\n", t, x->second.p[0],
                                x->second.p[1], x->second.p[2], x->second.sim ? "*" : "", y->second.p[0], y->second.p[1],
                                y->second.p[2], y->second.sim ? "*" : "");
                }
            }
            std::string pass_text;
            for (const auto& [m, s]: straight_passes) pass_text += " " + std::to_string(s) + "@" + std::to_string(m);
            std::printf("straight: %zu births, passes:%s; rows at S=%zu, portable=%zu bytes, blob=%zu bytes, union at S {",
                        straight_births.size(), pass_text.c_str(), server.rows.size(), server.portable.size(),
                        server.blob.size());
            for (int s: server.counters.sectors.active()) std::printf(" %d", s);
            std::printf(" }\n");
            // What a server blob that travelled would have to be mapped by
            // (Q1): its objects by name, the names it carries that a client
            // world spells otherwise (the clones, "_BMMO_<player>": the own
            // ball's is the client's retail ball), and the names several
            // objects share, which the restore tells apart by the capturing
            // process's entity ids.
            {
                const std::string text = bmmo::physics::describe_savestate(server.blob.data(), server.blob.size());
                std::map<std::string, int> names;
                size_t pos = 0;
                while (pos < text.size()) {
                    const size_t eol = text.find('\n', pos);
                    const std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
                    pos = eol == std::string::npos ? text.size() : eol + 1;
                    if (line.rfind("object ", 0) != 0) continue;
                    const size_t sp = line.find(' ', 7);
                    ++names[line.substr(7, sp == std::string::npos ? std::string::npos : sp - 7)];
                }
                int clones = 0, shared = 0;
                std::string clone_names, shared_names;
                for (const auto& [name, n]: names) {
                    if (name.find("_BMMO_") != std::string::npos) { ++clones; clone_names += " " + name; }
                    if (n > 1) { shared += n; shared_names += " " + name + "x" + std::to_string(n); }
                }
                std::printf("server blob at S: %zu bytes, %zu object names; clone names %d:%s; shared names %d:%s\n",
                            server.blob.size(), names.size(), clones, clone_names.empty() ? " -" : clone_names.c_str(), shared,
                            shared_names.empty() ? " -" : shared_names.c_str());
                if (!args.savestate_trace.empty()) {
                    if (std::FILE* f = std::fopen((args.savestate_trace + ".server.blob").c_str(), "wb")) {
                        std::fwrite(server.blob.data(), 1, server.blob.size(), f);
                        std::fclose(f);
                    }
                }
            }
            std::printf("harness: warm-up run vs the run from the restored root at %u: worst %.9f m (%s), %d missing%s\n", end,
                        worst, worst_name.empty() ? "-" : worst_name.c_str(), missing,
                        worst == 0.0 && missing == 0 ? " (identical)" : "");
        }
        // --resync-server-blob FILE: the cross-world restore test of Q1.  The
        // r/s/a arms restore ANOTHER process's server blob of S (written by
        // --savestate-trace PREFIX as PREFIX.server.blob), with the object
        // names --resync-blob-rename maps (the own clone "_BMMO_<id>" of the
        // capturing process to this process's own ball), instead of this
        // process's own.  The rename rewrites the length-prefixed strings of
        // the blob that spell OLD exactly; check_savestate validates the
        // result.  This process's own server blob of S stays the reference.
        std::vector<uint8_t> foreign;
        if (!args.resync_server_blob.empty()) {
            std::FILE* f = std::fopen(args.resync_server_blob.c_str(), "rb");
            if (!f) {
                std::fprintf(stderr, "cannot open --resync-server-blob %s\n", args.resync_server_blob.c_str());
                return 2;
            }
            uint8_t chunk[65536];
            size_t n = 0;
            while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) foreign.insert(foreign.end(), chunk, chunk + n);
            std::fclose(f);
            const size_t loaded = foreign.size();
            std::string renames;
            for (const auto& [from, to]: args.resync_blob_rename) {
                std::vector<uint8_t> pattern(4), replacement(4);
                const uint32_t from_size = static_cast<uint32_t>(from.size()), to_size = static_cast<uint32_t>(to.size());
                for (int k = 0; k < 4; ++k) {
                    pattern[k] = static_cast<uint8_t>(from_size >> (8 * k));
                    replacement[k] = static_cast<uint8_t>(to_size >> (8 * k));
                }
                pattern.insert(pattern.end(), from.begin(), from.end());
                replacement.insert(replacement.end(), to.begin(), to.end());
                std::vector<uint8_t> out;
                out.reserve(foreign.size());
                int hits = 0;
                size_t i = 0;
                while (i < foreign.size()) {
                    if (foreign.size() - i >= pattern.size() && std::equal(pattern.begin(), pattern.end(), foreign.begin() + i)) {
                        out.insert(out.end(), replacement.begin(), replacement.end());
                        i += pattern.size();
                        ++hits;
                    } else {
                        out.push_back(foreign[i++]);
                    }
                }
                foreign.swap(out);
                renames += " " + from + "->" + to + " x" + std::to_string(hits);
            }
            // The blob's header is the magic, the format version and the
            // capturing environment's identity (the IVP_Environment pointer
            // as a u64): the restore refuses a blob of another environment
            // ("captured in another environment"), so a blob cannot travel
            // as it is.  --resync-blob-adopt-environment writes this
            // process's identity over it - what a restore flag for a
            // travelling blob would waive (A3) - to measure past it.
            if (args.resync_blob_adopt_env && foreign.size() >= 16 && server.blob.size() >= 16) {
                uint64_t theirs = 0, ours = 0;
                std::memcpy(&theirs, foreign.data() + 8, 8);
                std::memcpy(&ours, server.blob.data() + 8, 8);
                std::memcpy(foreign.data() + 8, server.blob.data() + 8, 8);
                renames += " environment " + std::to_string(theirs) + "->" + std::to_string(ours);
            }
            std::string check_error;
            const bool valid = bmmo::physics::check_savestate(foreign.data(), foreign.size(), check_error);
            const bool same = foreign == server.blob;
            std::printf("foreign server blob %s: %zu bytes loaded, %zu after the renames (%s), check %s; against this"
                        " process's own server blob at S (%zu bytes): %s\n", args.resync_server_blob.c_str(), loaded,
                        foreign.size(), renames.empty() ? "none" : renames.substr(1).c_str(),
                        valid ? "ok" : ("REFUSED: " + check_error).c_str(), server.blob.size(),
                        same ? "BYTE-IDENTICAL" : "differs");
            if (!same)
                std::printf("foreign server blob: first differences: %s\n",
                            bmmo::physics::diff_savestate(server.blob.data(), server.blob.size(), foreign.data(), foreign.size(),
                                                          false, 4, false).c_str());
        }
        // Sector K's bodies, as the straight run made them: the movable ones
        // join the gate.
        // The level end's bodies (the PE_ module: the balloon, its airship and
        // its slide) are local to each player (the user's exception (c),
        // invariant I3): reported, never judged.
        const auto is_local = [](const std::string& name) { return name.rfind("PE_", 0) == 0; };
        std::set<std::string> k_movable;
        if (k_sector > 0) {
            std::string list;
            for (const auto& b: births_of(k_sector)) {
                if (b.birth.recipe.fixed || is_local(b.birth.name)) continue;
                if (k_movable.insert(b.birth.name).second) list += " " + b.birth.name + "@" + std::to_string(b.tick);
            }
            std::printf("sector %d's movable bodies (straight run, the level end's left out):%s\n", k_sector,
                        list.empty() ? " none" : list.c_str());
        }
        std::set<std::string> gate(args.resync_watch.begin(), args.resync_watch.end());
        gate.insert(k_movable.begin(), k_movable.end());
        for (const auto& name: args.resync_watch)
            if (!straight[S].poses.count(name) && !straight[end].poses.count(name)) {
                std::fprintf(stderr, "--resync-watch %s is not a movable body of this scene\n", name.c_str());
                return 2;
            }

        // ---- the arms
        struct arm_result {
            char arm = 0;
            bool valid = true;
            std::string invalid;
            int exact_ticks = 0, compared_ticks = 0;
            uint32_t first_inexact = 0;
            std::string first_diff;
            std::map<std::string, double> window_err, after_err;
            std::map<std::string, bool> bit_exact;   // over (S, end]
            int missing = 0;
            double gate_worst = 0.0, rest_worst = 0.0;
            std::string gate_worst_name, rest_worst_name;
            bool pass = false;
        };
        std::vector<arm_result> results;
        double r_band = -1.0;   // arm R's worst error over (B, end], every body
        for (char arm: args.resync_arms) {
            // A lower-case arm is its upper-case case applied with the
            // SERVER's blob of S instead of the anchor's (the Q1 alternative:
            // the server's IVP savestate travels with the resync state).
            const bool server_blob = arm >= 'a' && arm <= 'z';
            const char kind = server_blob ? static_cast<char>(arm - 'a' + 'A') : arm;
            arm_result res;
            res.arm = arm;
            if (!restore_root()) return 1;
            std::vector<rc_event> events = straight_events;
            if (kind == 'S' || kind == 'A')
                for (auto& e: events)
                    if (e.kind == 0 && e.sector == k_sector)
                        e.tick = kind == 'S' ? e.tick + static_cast<uint32_t>(args.resync_shift)
                                            : e.tick - static_cast<uint32_t>(std::min<int>(args.resync_shift,
                                                                                            static_cast<int>(e.tick - root_tick)));
            if (kind == 'R') events.push_back({S - static_cast<uint32_t>(args.resync_perturb_ticks), 2, 0});
            rc_state anchor;
            rc_poses anchor_at_s;
            if (!run_until(B, events, [&](uint32_t t) {
                    if (t == S) {
                        anchor_at_s = rc_capture_poses(physics);
                        return capture_state(anchor, false);
                    }
                    return true;
                }))
                return 1;
            const std::vector<rc_birth> anchor_births = births;
            // The arm's case, checked against what the runs did.
            if (kind == 'S' || kind == 'A') {
                uint32_t straight_first = 0, straight_last = 0, arm_first = 0, arm_last = 0;
                bool any_straight = false, any_arm = false;
                for (const auto& b: straight_births)
                    if (pass_of_list(straight_passes, b.tick) == k_sector) {
                        straight_first = any_straight ? std::min(straight_first, b.tick) : b.tick;
                        straight_last = any_straight ? std::max(straight_last, b.tick) : b.tick;
                        any_straight = true;
                    }
                for (const auto& b: anchor_births)
                    if (pass_of(b.tick) == k_sector) {
                        arm_first = any_arm ? std::min(arm_first, b.tick) : b.tick;
                        arm_last = any_arm ? std::max(arm_last, b.tick) : b.tick;
                        any_arm = true;
                    }
                const bool server_has_k = server.counters.sectors.active().count(k_sector) != 0;
                char text[256];
                std::snprintf(text, sizeof(text), "sector %d births: straight %u..%u%s, arm %u..%u%s; server's union at S %s it",
                              k_sector, straight_first, straight_last, any_straight ? "" : " (none)", arm_first, arm_last,
                              any_arm ? "" : " (none)", server_has_k ? "has" : "lacks");
                if (kind == 'S' && !(any_straight && any_arm && server_has_k && straight_last < S && arm_first >= S && arm_last < B))
                    res.valid = false;
                if (kind == 'A' && !(any_straight && any_arm && !server_has_k && straight_first >= S && arm_last < S))
                    res.valid = false;
                res.invalid = text;
                std::printf("arm %c: %s%s\n", arm, text, res.valid ? "" : " -> INVALID: not the arm's case");
            }

            // ---- the apply, at B
            bmmo::sim::physics_world::step_state counters = anchor.counters;
            counters.sectors = server.counters.sectors;
            const bool rng_same = anchor.counters.rng_cursor == server.counters.rng_cursor
                               && anchor.counters.random_cursor == server.counters.random_cursor;
            world->restore_step_state(counters);
            // The scripts: the server's portable record localized against the
            // anchor's own v2 record.
            std::vector<uint8_t> localized;
            script_replay::portable_report localize_report;
            std::string e;
            const bool localized_ok = script_replay::localize(context, sel, server.portable, &anchor.v2, keep_rule, localized,
                                                              &e, &localize_report);
            if (!localized_ok) std::printf("arm %c: localize refused the server's record: %s\n", arm, e.c_str());
            const live_membership_scope live(true);   // step B2SW (G1): the clients' rule
            bool scripts_ok = script_replay::restore(context, sel, hooks, localized_ok ? localized : anchor.v2, &e, nullptr,
                                                     nullptr, true);
            if (!scripts_ok) std::printf("arm %c: script restore failed: %s\n", arm, e.c_str());
            std::string unregistered_names;
            const int unregistered = rc_unregistered_scripts(context, sel, args.resync_reregister, &unregistered_names);
            if (unregistered)
                std::printf("arm %c: %d restored-active script(s) unknown to the behaviour manager%s:%s\n", arm, unregistered,
                            args.resync_reregister ? " (re-registered)" : " (left: the live frames will not run them)",
                            unregistered_names.c_str());
            // The anchor's bodies of a pass the server has not run at S.
            std::vector<rc_birth> doomed;
            for (const auto& b: anchor_births) {
                const int sector = pass_of(b.tick);
                if (sector > 0 && !server.counters.sectors.active().count(sector)) doomed.push_back(b);
            }
            int destroyed = 0;
            if (!args.resync_destroy_after_blob) destroyed = destroy(doomed);
            bmmo::session::savestate_outcome outcome;
            const std::vector<uint8_t>& restored_blob = !server_blob ? anchor.blob : foreign.empty() ? server.blob : foreign;
            const bool blob_ok = binding.set_savestate(restored_blob, outcome);
            if (!blob_ok)
                std::printf("arm %c: the %s blob was refused: %s\n", arm,
                            !server_blob ? "anchor's" : foreign.empty() ? "server's" : "foreign server", blob_refusal.c_str());
            if (server_blob && !foreign.empty())
                std::printf("arm %c: restored the foreign server blob: skipped=%u unknown bodies=%zu%s\n", arm,
                            backend.last_report.skipped, outcome.unknown_bodies.size(),
                            outcome.unknown_bodies.empty() ? "" : (" (first " + outcome.unknown_bodies.front() + ")").c_str());
            const bmmo::physics::savestate_report blob_report = backend.last_report;
            if (args.resync_destroy_after_blob) destroyed = destroy(doomed);
            std::set<std::string> unknown(outcome.unknown_bodies.begin(), outcome.unknown_bodies.end());
            // The rows.
            std::set<std::string> rowed;
            int rows_written = 0, rows_unmapped = 0, row_rechecks = 0, contacts_made = 0, unknown_rechecks = 0, rows_equal = 0;
            std::string unmapped_names, written_names;
            for (const auto& row: server.rows) {
                std::string entity = row.kind == bmmo::session::body_kind::Ball ? ball_entity() : row.name;
                bmmo_physics_body_state live{};
                if (entity.empty() || !binding.get_body(entity, live)) {
                    ++rows_unmapped;
                    if (rows_unmapped <= 6) unmapped_names += " " + (entity.empty() ? std::string("?") : entity);
                    continue;
                }
                bmmo_physics_body_state state{};
                for (int k = 0; k < 3; ++k) {
                    state.position[k] = row.position[k];
                    state.linear[k] = row.linear[k];
                    state.angular[k] = row.angular[k];
                }
                for (int k = 0; k < 4; ++k) state.rotation[k] = row.rotation[k];
                state.simulated = (row.flags & bmmo::session::BODY_FLAG_SIMULATED) != 0;
                // A row the body already has, bit for bit, in a body the blob
                // knows: not written.  The write is a beam, which loses what a
                // row does not carry - the hull manager's gradient and values,
                // the core's PSI time stamp and delta of a sleeping body, the
                // entity matrix rebuilt from the quaternion (low bits) - so
                // writing the server's own state onto the server's own world
                // is not a no-op (measured: arm E, Level 11, the ball's hull
                // and matrix, 16 rows -> 0 of 630 ticks exact).
                if (!args.resync_write_equal_rows && !unknown.count(entity)
                        && std::memcmp(state.position, live.position, sizeof state.position) == 0
                        && std::memcmp(state.rotation, live.rotation, sizeof state.rotation) == 0
                        && std::memcmp(state.linear, live.linear, sizeof state.linear) == 0
                        && std::memcmp(state.angular, live.angular, sizeof state.angular) == 0
                        && state.simulated == live.simulated) {
                    ++rows_equal;
                    continue;
                }
                double dp = 0.0, dv = 0.0;
                for (int k = 0; k < 3; ++k) {
                    dp += (state.position[k] - live.position[k]) * (state.position[k] - live.position[k]);
                    dv += (static_cast<double>(state.linear[k]) - live.linear[k])
                        * (static_cast<double>(state.linear[k]) - live.linear[k]);
                }
                const bool recheck = unknown.count(entity) != 0 || std::sqrt(dp) > bmmo::session::kRecheckPositionM
                                  || std::sqrt(dv) > bmmo::session::kRecheckVelocityMs;
                if (recheck) ++row_rechecks;
                if (binding.set_body(entity, state, state.simulated ? bmmo::session::wake_mode::wake
                                                                     : bmmo::session::wake_mode::freeze, recheck)) {
                    ++rows_written;
                    if (args.verbose && rows_written <= 12) written_names += " " + entity;
                }
                rowed.insert(entity);
            }
            for (const auto& name: unknown)
                if (!rowed.count(name) && binding.recheck_body(name)) ++unknown_rechecks;
            for (const auto& name: rowed) {
                const auto it = anchor.contacts.find(name);
                if (it == anchor.contacts.end()) continue;
                const int made = binding.set_contacts(name, it->second, BMMO_PHYSICS_CONTACT_DEFAULT);
                if (made > 0) contacts_made += made;
            }
            active_before = world->active_sectors();
            // births: kept, the next root restore destroys every body made since the root
            passes.clear();
            bmmo::physics::drain_event_log(physics);
            if (args.verbose && !written_names.empty()) std::printf("arm %c: rows written to:%s\n", arm, written_names.c_str());
            std::vector<uint8_t> applied;
            if (!bmmo::physics::get_savestate(physics, applied, e)) applied.clear();
            if (!args.savestate_trace.empty()) {
                // --savestate-trace PREFIX: both worlds at S as text, for a
                // field-by-field diff.
                const std::string base = args.savestate_trace + ".arm" + std::string(1, arm);
                write_text_file(base + ".server.txt", bmmo::physics::describe_savestate(server.blob.data(), server.blob.size()));
                write_text_file(base + ".anchor.txt", bmmo::physics::describe_savestate(anchor.blob.data(), anchor.blob.size()));
                write_text_file(base + ".applied.txt", bmmo::physics::describe_savestate(applied.data(), applied.size()));
            }
            std::printf("arm %c: apply at B=%u of S=%u: rng %s, localize %s (unportable=%u kept_local=%u resolved=%u), scripts %s,"
                        " blob %s (unknown=%zu skipped=%u contacts_dropped=%zu), destroyed=%d, rows written=%d equal=%d unmapped=%d%s"
                        " rechecked=%d, unknown rechecked=%d, contacts re-made=%d\n",
                        arm, B, S, rng_same ? "same" : "DIFFERS", localized_ok ? "ok" : "REFUSED", localize_report.unportable,
                        localize_report.kept_local, localize_report.objects_resolved, scripts_ok ? "ok" : "FAILED",
                        blob_ok ? "ok" : "REFUSED", outcome.unknown_bodies.size(), blob_report.skipped,
                        outcome.contacts_dropped.size(), destroyed, rows_written, rows_equal, rows_unmapped,
                        unmapped_names.empty() ? "" : (" (" + unmapped_names + " )").c_str(), row_rechecks, unknown_rechecks,
                        contacts_made);
            {
                // The applied world against the server's at S.
                const std::string d = bmmo::physics::diff_savestate(server.blob.data(), server.blob.size(), applied.data(),
                                                                    applied.size(), false, 3, false);
                std::printf("arm %c: applied world vs the server's at S: %s\n", arm, d.empty() ? "IDENTICAL" : d.c_str());
                if (!d.empty()) {
                    const std::string o = bmmo::physics::diff_savestate(server.blob.data(), server.blob.size(), applied.data(),
                                                                        applied.size(), true, 3, true);
                    std::printf("arm %c:   (overlap tree and order bookkeeping aside: %s)\n", arm, o.c_str());
                }
                // Every body's pose against the server's rows: the rows' write.
                double worst = 0.0;
                std::string worst_name;
                for (const auto& [name, pose]: rc_capture_poses(physics)) {
                    const auto it = straight[S].poses.find(name);
                    if (it == straight[S].poses.end()) continue;
                    const double dd = rc_distance(pose, it->second);
                    if (dd > worst) { worst = dd; worst_name = name; }
                }
                std::printf("arm %c: after the apply, worst body vs the server at S: %.9f m (%s)\n", arm, worst,
                            worst_name.empty() ? "-" : worst_name.c_str());
                if (kind == 'S') {
                    // K's constraints: each core's named controllers.
                    const auto expected = rc_named_controllers(
                        bmmo::physics::describe_savestate(server.blob.data(), server.blob.size()));
                    const auto got = rc_named_controllers(bmmo::physics::describe_savestate(applied.data(), applied.size()));
                    std::set<std::string> k_all;
                    for (const auto& b: births_of_list(straight_births, straight_passes, k_sector)) k_all.insert(b.birth.name);
                    int same = 0, differ = 0, absent = 0, with_constraints = 0, same_constrained = 0, local_differ = 0;
                    for (const auto& name: k_all) {
                        const auto x = expected.find(name), y = got.find(name);
                        if (x == expected.end() || y == got.end()) {
                            ++absent;
                            std::printf("arm %c: constraint check: %s %s\n", arm, name.c_str(),
                                        x == expected.end() ? "has no core in the server's blob" : "has no core after the apply");
                            continue;
                        }
                        const bool constrained = !x->second.empty() && !is_local(name);
                        if (constrained) ++with_constraints;
                        if (x->second == y->second) {
                            if (!is_local(name)) ++same;
                            if (constrained) ++same_constrained;
                            if (args.verbose && !x->second.empty())
                                std::printf("arm %c: constraint check: %s [%s] same\n", arm, name.c_str(), x->second.c_str());
                        } else if (is_local(name)) {
                            ++local_differ;
                        } else {
                            ++differ;
                            std::printf("arm %c: constraint check: %s server [%s] applied [%s]\n", arm, name.c_str(),
                                        x->second.c_str(), y->second.c_str());
                        }
                    }
                    std::printf("arm %c: constraints of sector %d's %zu bodies after the apply: the level end's aside, %d same,"
                                " %d differ, %d absent, and of those with named controllers %d of %d same; the level end's"
                                " (local, I3): %d differ\n", arm, k_sector, k_all.size(), same, differ, absent, same_constrained,
                                with_constraints, local_differ);
                }
            }

            // ---- the re-step S..end, compared with the straight run
            std::vector<rc_event> rest;
            for (const auto& ev: straight_events) if (ev.tick >= S) rest.push_back(ev);
            std::vector<uint8_t> blob;
            if (!run_until(end, rest, [&](uint32_t t) {
                    if (t <= S) return true;
                    const rc_tick& expected = straight[t];
                    const rc_poses poses = rc_capture_poses(physics);
                    std::string ce;
                    if (!bmmo::physics::get_savestate(physics, blob, ce)) {
                        std::fprintf(stderr, "resync-check: arm %c's savestate at %u failed: %s\n", arm, t, ce.c_str());
                        return false;
                    }
                    ++res.compared_ticks;
                    if (blob.size() == expected.blob.size()
                            && bmmo::physics::hash_savestate(blob.data(), blob.size()) == expected.blob_hash && blob == expected.blob) {
                        ++res.exact_ticks;
                    } else if (!res.first_inexact) {
                        res.first_inexact = t;
                        res.first_diff = bmmo::physics::diff_savestate(expected.blob.data(), expected.blob.size(), blob.data(),
                                                                       blob.size(), false, 2, false);
                    }
                    auto& errs = t <= B ? res.window_err : res.after_err;
                    for (const auto& [name, pose]: expected.poses) {
                        const auto it = poses.find(name);
                        if (it == poses.end()) { ++res.missing; continue; }
                        const double d = rc_distance(pose, it->second);
                        double& slot = errs[name];
                        slot = std::max(slot, d);
                        auto be = res.bit_exact.emplace(name, true).first;
                        be->second = be->second && rc_same_bits(pose, it->second);
                    }
                    for (const auto& [name, pose]: poses) if (!expected.poses.count(name)) ++res.missing;
                    return true;
                }))
                return 1;

            // ---- the verdict
            double local_worst = 0.0;
            std::string local_worst_name;
            for (const auto& [name, err]: res.after_err) {
                if (is_local(name)) {
                    if (err > local_worst) { local_worst = err; local_worst_name = name; }
                    continue;
                }
                const bool in_gate = gate.count(name) != 0;
                double& worst = in_gate ? res.gate_worst : res.rest_worst;
                std::string& worst_name = in_gate ? res.gate_worst_name : res.rest_worst_name;
                if (err > worst) { worst = err; worst_name = name; }
            }
            const double gate_m = args.resync_gate_mm / 1000.0;
            if (kind == 'E') {
                res.pass = res.valid && res.exact_ticks == res.compared_ticks && res.missing == 0;
            } else if (kind == 'R' || kind == 'S') {
                std::string unseen;
                for (const auto& name: gate)
                    if (!res.after_err.count(name)) unseen += " " + name;
                if (!unseen.empty()) std::printf("arm %c: gate bodies never compared (the arm fails):%s\n", arm, unseen.c_str());
                res.pass = res.valid && res.missing == 0 && res.gate_worst <= gate_m && unseen.empty();
                if (arm == 'R') r_band = std::max(res.gate_worst, res.rest_worst);
            } else if (kind == 'A') {
                bool k_exact = !k_movable.empty();
                for (const auto& name: k_movable) {
                    const auto it = res.bit_exact.find(name);
                    k_exact = k_exact && it != res.bit_exact.end() && it->second;
                }
                // the rest: every body outside K
                double rest = 0.0;
                std::string rest_name;
                for (const auto& [name, err]: res.after_err)
                    if (!k_movable.count(name) && !is_local(name) && err > rest) { rest = err; rest_name = name; }
                const double band = r_band >= 0.0 ? r_band : gate_m;
                res.pass = res.valid && k_exact && res.missing == 0 && rest <= band;
                std::printf("arm %c: sector %d's %zu movable bodies %s; the rest worst %.9f m (%s) against %s %.9f m\n", arm,
                            k_sector, k_movable.size(), k_exact ? "bit-exact" : "NOT bit-exact", rest, rest_name.empty() ? "-" : rest_name.c_str(),
                            r_band >= 0.0 ? "arm R's band" : "the gate", band);
            }
            // The report: the gate bodies, then the ten worst others.
            std::printf("arm %c: blobs identical at %d of %d ticks", arm, res.exact_ticks, res.compared_ticks);
            if (res.first_inexact) std::printf(" (first differing at %u: %s)", res.first_inexact, res.first_diff.c_str());
            std::printf("; bodies missing/extra %d\n", res.missing);
            std::vector<std::pair<double, std::string>> ranked;
            for (const auto& [name, err]: res.after_err) ranked.emplace_back(err, name);
            std::sort(ranked.rbegin(), ranked.rend());
            for (const auto& name: gate) {
                const auto w = res.window_err.find(name), a = res.after_err.find(name);
                std::printf("arm %c: gate %-28s window(S,B] %.9f m  after(B,B+%d] %.9f m%s\n", arm, name.c_str(),
                            w == res.window_err.end() ? -1.0 : w->second, after, a == res.after_err.end() ? -1.0 : a->second,
                            res.bit_exact.count(name) && res.bit_exact[name] ? " bit-exact" : "");
            }
            int shown = 0;
            for (const auto& [err, name]: ranked) {
                if (gate.count(name)) continue;
                if (++shown > 10) break;
                std::printf("arm %c: other %-28s after %.9f m  window %.9f m%s\n", arm, name.c_str(), err,
                            res.window_err.count(name) ? res.window_err[name] : -1.0,
                            res.bit_exact.count(name) && res.bit_exact[name] ? " bit-exact" : "");
            }
            if (!local_worst_name.empty())
                std::printf("arm %c: the level end's bodies (local, I3, not judged): worst %.9f m (%s)\n", arm, local_worst,
                            local_worst_name.c_str());
            std::printf("RESULT arm=%c %s gate_worst=%.9f m (%s) rest_worst=%.9f m (%s) exact_ticks=%d/%d%s\n", arm,
                        res.pass ? "PASS" : "FAIL", res.gate_worst, res.gate_worst_name.empty() ? "-" : res.gate_worst_name.c_str(),
                        res.rest_worst, res.rest_worst_name.empty() ? "-" : res.rest_worst_name.c_str(), res.exact_ticks,
                        res.compared_ticks, res.valid ? "" : " INVALID");
            std::fflush(stdout);
            results.push_back(res);
        }
        int failed = 0;
        for (const auto& r: results) failed += r.pass ? 0 : 1;
        std::fprintf(stderr, "resync-check: %.1f s\n",
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
        std::printf("%s arms=%zu failed=%d\n", failed == 0 ? "PASS" : "FAIL", results.size(), failed);
        std::fflush(stdout);
        return failed == 0 ? 0 : 3;
    }

    // ------------------------------------------------ --late-sector-check
    //
    // Resync design 7.2 (step B1): a late Sector stamp re-derived inside a
    // rewind gives the world an on-time stamp gives.  Two runs of the same
    // scene, each in a process of its own from a fresh load (a load is
    // deterministic: the A2 cross-process test found two processes' blobs
    // byte-identical), with the client's live and replay recipes on the
    // SimTool world:
    //
    //   root      player 1 joins, --late-root whole world ticks (the retail
    //             ball parked), as --resync-check's root.
    //   live      whole world ticks.  A Sector stamp is handed to the world's
    //             union at its tick (the server's request, which is what a
    //             client's stamp applied at its tick is); every pass the union
    //             starts is recorded in the tick's pass slot of host
    //             support's manager_activations (run_sector's cells), the
    //             frame's pending activations after it (record_pending), and
    //             every tick into the client's rollback engine (savestate,
    //             session script selection, the movable bodies but the balls)
    //             with the union's state beside it.  The own poll's cell rule
    //             runs after each tick: a cell change no pass of this world
    //             explains is a Sector event the client would send.
    //   control   sector K stamped at T (with --late-moved T2: at T2), the
    //             --late-also stamps at their ticks.
    //   arm       the same, but K is not handed over at T: it arrives at A
    //             (its frame's drain), and the core's rule runs - one rewind
    //             to T-1 (the rollback engine's rewind_to over the binding
    //             bind_session_world makes), the union re-derived inside the
    //             replay from the union record of T-1 with the stamp log's
    //             stamps (sector_union::with_pending), each replayed tick's
    //             pass in the pass slot (manager_activations::set_pass, or
    //             forget_pass for a live pass not re-derived), births of a
    //             moved pass attributed (sector_union::birth_moved); then the
    //             cells adopted (no write-back), the activation of the frame
    //             about to run re-issued, the cell re-read, the world's union
    //             and RNG cursors taken from the replay.  With --late-moved
    //             T2, K is an own stamp handed over at T (its live pass runs)
    //             and the server's echo moving it to T2 arrives at A.
    //   compare   every boundary A..A+N (--late-after, 600): the savestate
    //             byte for byte and with the order bookkeeping left out
    //             (diff_savestate ignore_order), the session script record
    //             byte for byte without the keep-local scripts (the
    //             balloon's, exception (c): never compared), the union
    //             (sets, origins, pacer).
    //   pass      the blobs equal ignoring the order bookkeeping and the
    //             scripts identical at every boundary, births_missing 0, the
    //             forgotten passes as --late-expect-forgotten says (when
    //             given), no Sector event from the cell, the rewind made.
    //   reference (--late-reference) a third run: the arm with K on time
    //             and an ordinary rewind to T-1 at A (no re-derivation, the
    //             live cells written back) - what any replay across the pass
    //             leaves.  A script difference of the arm at a boundary where
    //             the reference has the same one is the replay's, not this
    //             rule's: the verdict is then PASS-EXCEPT-REFERENCE (exit 4).
    //
    // The driver (--late-sector-check K T A) runs both as child processes
    // (--late-run control|arm --late-out FILE) and compares their files.
    // Ticks count from the root.  This is the core's rule on the core's
    // pieces (the re-derivation itself - sector_rederivation, the code the
    // core's rewind_for_sectors / rederive_union_tick run -, the union, its
    // state, the activation slots, the engine, the binding); the frame
    // around it is the SimTool world's, which cannot host the session core
    // itself.  The rewound line's coincident= counts the window's ticks
    // whose two activation slots are both filled (none: the replay
    // re-issues what the one-slot map before B1 did).
    // ------------------------------------------------------------------
    bool ls_put(FILE* f, const std::vector<uint8_t>& bytes) {
        const uint32_t n = static_cast<uint32_t>(bytes.size());
        return std::fwrite(&n, 4, 1, f) == 1 && (n == 0 || std::fwrite(bytes.data(), 1, n, f) == n);
    }
    bool ls_get(FILE* f, std::vector<uint8_t>& bytes) {
        uint32_t n = 0;
        if (std::fread(&n, 4, 1, f) != 1) return false;
        bytes.resize(n);
        return n == 0 || std::fread(bytes.data(), 1, n, f) == n;
    }

    int run_late_sector_run(const arguments& args) {
        using bmmo::game::script_replay;
        using bmmo::session::sector_union;
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        const bool arm = args.late_run == "arm";
        if (!arm && args.late_run != "control") {
            std::fprintf(stderr, "--late-run takes control or arm\n");
            return 2;
        }
        const int k = args.late_sector;
        const bool moved = args.late_moved >= 0;
        const uint32_t pid = 1;
        auto world = create_session_world(args, 0.0f);
        if (!world) return 1;
        std::string error;
        if (!world->add_player(pid, 0, error)) {
            std::fprintf(stderr, "add_player failed: %s\n", error.c_str());
            return 1;
        }
        CKContext* context = world->engine().context();
        CKIpionManager* physics = world->physics();
        if (!physics || !physics->GetEnvironment()) {
            std::fprintf(stderr, "late-sector: no physics environment\n");
            return 1;
        }
        if (!settle_world(*world, std::max(0, args.late_root))) return 1;
        const uint32_t root = world->tick_index();
        const uint32_t t_stamp = root + static_cast<uint32_t>(args.late_t);
        const uint32_t t_arrive = root + static_cast<uint32_t>(args.late_a);
        const uint32_t t_applied = moved ? root + static_cast<uint32_t>(args.late_moved) : t_stamp;
        const uint32_t end = t_arrive + static_cast<uint32_t>(args.late_after);
        // The stamps this run hands the world's union, by tick; and the stamp
        // log the arm has at A (every stamp with the server's tick).
        std::map<uint32_t, std::vector<int>> stamps;
        // --late-ordinary: the arm gets K on time like the control and makes
        // an ordinary rewind at A (no re-derivation, the live cells written
        // back): the replay's own fidelity, the reference the B1 arm is
        // judged beside.
        const bool ordinary = arm && args.late_ordinary;
        if (!arm || ordinary) stamps[t_applied].push_back(k);
        else if (moved) stamps[t_stamp].push_back(k);
        std::vector<sector_union::stamped> log;
        log.push_back({t_applied, pid, k});
        for (const auto& [sector, tick]: args.late_also) {
            stamps[root + static_cast<uint32_t>(tick)].push_back(sector);
            log.push_back({root + static_cast<uint32_t>(tick), pid, sector});
        }
        std::set<int> own = {1};
        if (moved) own.insert(k);

        const script_replay::selection sel = bmmo::game::session_script_selection(context);
        const std::vector<CK_ID> script_activations = bmmo::game::session_script_activations(context, sel);
        bmmo::game::script_replay_hooks hooks;
        hooks = bmmo::game::force_hooks_for(bmmo::physics::physics_direct(physics));   // step HU1: one copy (game/host_backend.hpp)
        CKBehavior* manager = bmmo::game::find_root_script(context, "Gameplay_SectorManager");
        const CK_ID manager_id = manager ? manager->GetID() : 0;
        if (!manager) {
            std::fprintf(stderr, "late-sector: no Gameplay_SectorManager\n");
            return 1;
        }
        bmmo::game::manager_activations activations;
        bmmo::session::local_birth_log births;
        std::map<std::string, uint32_t> generations;
        std::set<std::string> ball_names;
        for (const auto& row: world->ball_rows()) ball_names.insert(row.name);
        const auto is_ball = [&](const std::string& name) {
            return name.find("_BMMO_") != std::string::npos || ball_names.count(name) != 0;
        };
        const auto mechanisms = [&]() {
            std::vector<std::string> out;
            for (auto& name: bmmo::physics::movable_body_names(physics))
                if (!is_ball(name)) out.push_back(std::move(name));
            return out;
        };
        const auto read_cell = [&]() {
            int cell = 0;
            if (CKDataArray* parameters = bmmo::game::session_data_array(context, "IngameParameter"))
                parameters->GetElementValue(0, 1, &cell);
            return cell;
        };

        // The re-derivation: the core's own rule (sector_rederivation, which
        // client_core_frame.cpp rewind_for_sectors / rederive_union_tick
        // drive the same way); this run does the host's side of each tick
        // (the activation's pass slot) and the logs.
        bool rd_active = false;
        bmmo::session::sector_rederivation rd;
        std::map<uint32_t, int> pass_log;
        std::map<uint32_t, sector_union::state_type> union_records;
        bmmo::session::core_world_hooks core_hooks;
        core_hooks.before_replayed_tick = [&](uint32_t t) {
            if (!rd_active) return;
            const auto step = rd.tick(t, manager->IsActive() != FALSE);
            if (step.made) {
                activations.set_pass(t, step.made.sector);
                pass_log[t] = step.made.sector;
                std::printf("  re-derived: sector +%d at tick %u (root+%u)\n", step.made.sector, t, t - root);
            } else if (step.clear_slot) {
                activations.forget_pass(t);
            }
            if (step.forgotten)
                std::printf("  forgotten: the live pass of sector %d at tick %u (root+%u)\n", step.forgotten, t, t - root);
            union_records[t] = rd.u().state();
        };
        core_hooks.replay_mechanisms = mechanisms;
        core_hooks.ball = is_ball;
        core_hooks.birth_moved = [&](const std::string&, uint32_t b) { return rd_active && rd.birth_moved(b); };
        bmmo::physics::physics_direct backend(physics);
        bmmo::session::session_world_options w;
        w.binding.delta_ms = bmmo::sim::kFixedDeltaMs;
        w.binding.savestate = true;
        w.binding.savestate_flags = 0u;   // lenient, as the clients run it
        w.binding.log = [&](const std::string& text) { std::printf("  engine: %s\n", text.c_str()); };
        uint64_t reissued = 0;
        w.replay_activation = [&](uint32_t t) {
            if (activations.replay(context, manager_id, t, true)) ++reissued;
        };
        w.births = &births;
        w.put_back = [context](const std::string& entity, const bmmo::session::local_birth& birth) {
            bmmo::game::put_back_birth(context, entity, birth);
        };
        std::vector<CK_ID> scripts_replayed;
        bmmo::session::live_script_order late_live_order;   // step RO1: the live frames' script order, by tick
        w.bind_scripts = [&](bmmo::session::rollback_world& bound) {
            bmmo::session::script_binding_options so;
            so.context = context;
            so.scripts = &sel;
            so.hooks = &hooks;
            so.scripts_replayed = &scripts_replayed;
            so.delta_ms = bmmo::sim::kFixedDeltaMs;
            so.replay_activation = true;
            so.live_order = &late_live_order;   // step RO1
            so.resize_strings = true;
            so.live_membership = true;   // step B2SW (G1, F): the core hosts' rule
            so.written_cells = true;     // step RK2 (record-format Q4): the core hosts' written-cell section
            bmmo::session::bind_script_replay(bound, backend, so);
        };
        bmmo::session::rollback_world binding = bmmo::session::bind_session_world(backend, core_hooks, w);
        {
            // The world's own PreSimulate pass before the physics step, as a
            // whole world tick has it (--savestate-check's recipe).
            const auto inner = binding.step;
            binding.step = [&world, inner]() {
                world->pre_simulate();
                return inner();
            };
        }
        bmmo::session::rollback_engine engine;   // the client's thresholds: history 96, reach 48
        engine.set_recheck_on_restore(true);
        engine.set_restore_impulses(true);
        engine.set_restore_quaternions(true);
        engine.set_restore_clock(false);
        engine.set_restore_calm(true);
        engine.set_restore_unit(true);
        engine.set_restore_contacts(true);
        engine.set_contact_flags(BMMO_PHYSICS_CONTACT_DEFAULT);
        engine.set_sleep_as_recorded(true);
        engine.set_replay_scripts(true);
        engine.set_exact_restore(true);
        const auto no_input = [](const std::string&, uint32_t, bmmo::session::input_frame&) { return false; };

        FILE* out = std::fopen(args.late_out.c_str(), "wb");
        if (!out) {
            std::fprintf(stderr, "cannot write %s\n", args.late_out.c_str());
            return 1;
        }
        std::printf("late-sector run=%s level=%d root=%u K=%d T=%u A=%u%s end=%u selected=%zu\n", args.late_run.c_str(),
                    world->level(), root, k, t_stamp, t_arrive,
                    moved ? (" moved->" + std::to_string(t_applied)).c_str() : "", end, sel.scripts.size());
        std::set<int> active_before = world->active_sectors();
        int sector_cell = read_cell();
        uint32_t phantom = 0;
        bool rewound = !arm;
        bmmo::physics::drain_event_log(physics);
        for (;;) {
            const uint32_t t = world->tick_index();
            if (arm && t == t_arrive) {
                // The frame of A: the late stamp in its drain (or the moved
                // echo), one rewind to the stamp - 1 with the union
                // re-derived, the cells adopted.
                const uint32_t anchor = t_stamp - 1, now = t - 1;
                const auto record = union_records.find(anchor);
                if (record == union_records.end()) {
                    std::fprintf(stderr, "late-sector: no union record of %u\n", anchor);
                    return 3;
                }
                rd.start(record->second, log, pass_log, t_stamp);
                // The three live cells (live_cells_guard), for an ordinary rewind.
                int cells[3] = {};
                const char* cell_array[3] = {"CurrentLevel", "IngameParameter", "IngameParameter"};
                const int cell_column[3] = {4, 1, 2};
                for (int i = 0; i < 3; ++i)
                    if (CKDataArray* a = bmmo::game::session_data_array(context, cell_array[i]))
                        a->GetElementValue(0, cell_column[i], &cells[i]);
                if (ordinary) rd.abandon(pass_log);
                // Ticks of the window where the frame's pending activation
                // and a union pass share the tick (the two slots): where none
                // does, the replay re-issues what the one-slot map before B1
                // did.
                uint32_t coincident = 0;
                for (uint32_t x = t_stamp; x != t + 1; ++x)
                    if (const auto* e = activations.slots().find(x); e && e->have_pending && e->have_pass) ++coincident;
                rd_active = !ordinary;
                // Step RO2's experiment switch (inert when absent): the
                // control's live orders (BMMO_RO2_ORDER_IN, the file the
                // control wrote with BMMO_RO2_ORDER_OUT) of the window's ticks
                // replace this run's, so the replay runs the late pass in the
                // order the on-time world ran it live.
                if (const char* order_in = std::getenv("BMMO_RO2_ORDER_IN"); order_in && *order_in)
                    if (FILE* f = std::fopen(order_in, "rb")) {
                        uint32_t at = 0, n = 0, injected = 0;
                        while (std::fread(&at, 4, 1, f) == 1 && std::fread(&n, 4, 1, f) == 1) {
                            std::vector<uint32_t> ids(n);
                            if (n && std::fread(ids.data(), 4, n, f) != n) break;
                            if (at + 1u >= anchor && at <= now) {
                                late_live_order.record(at, std::move(ids));
                                ++injected;
                            }
                        }
                        std::fclose(f);
                        std::printf("RO2: %u live orders of the control injected (%u..%u)\n", injected, anchor - 1u, now);
                    }
                const bool rolled = engine.rewind_to(binding, anchor, now, no_input, ordinary ? "ordinary" : "late sector");
                rd_active = false;
                const auto u = ordinary ? world->capture_step_state().sectors.state() : rd.u().state();
                rewound = rolled && (ordinary || rd.through(now));
                if (!rewound) {
                    std::printf("late-sector: the rewind to %u was refused or replayed nothing\n", anchor);
                    break;
                }
                // Adopt mode: the replayed cells stay (an ordinary rewind:
                // the live ones written back); the activation of the frame
                // about to run re-issued (live_cells_guard), the cell re-read
                // (sector_cell_adopted).
                activations.end_override(context);
                if (ordinary)
                    for (int i = 0; i < 3; ++i)
                        if (CKDataArray* a = bmmo::game::session_data_array(context, cell_array[i]))
                            a->SetElementValue(0, cell_column[i], &cells[i], sizeof(int));
                activations.replay(context, manager_id, t, false);
                if (!ordinary) sector_cell = read_cell();
                // The world's counters as the replay left the engine: its
                // union, its RNG cursors.
                auto st = world->capture_step_state();
                st.rng_cursor = ivp_srand_read();
                st.random_cursor = bmmo::physics::random_get_state();
                st.sectors.reset();
                st.sectors.restore(u);
                world->restore_step_state(st);
                active_before = world->active_sectors();
                std::printf("late-sector: rewound to %u at A=%u (%u ticks), forgotten=%u births_missing=%llu "
                            "births_moved=%llu recreated=%llu/%llu reissued=%llu cell=%d coincident=%u\n", anchor, t,
                            now - anchor, rd.forgotten(), static_cast<unsigned long long>(engine.stats().births_missing),
                            static_cast<unsigned long long>(engine.stats().births_moved),
                            static_cast<unsigned long long>(engine.stats().births_recreated_exact),
                            static_cast<unsigned long long>(engine.stats().births_recreated),
                            static_cast<unsigned long long>(reissued), sector_cell, coincident);
            }
            if (t >= t_arrive) {
                std::vector<uint8_t> blob, scripts;
                if (!bmmo::physics::get_savestate(physics, blob, error) || !script_replay::capture(context, sel, hooks, scripts)) {
                    std::fprintf(stderr, "late-sector: capture at %u failed: %s\n", t, error.c_str());
                    return 1;
                }
                auto u = world->capture_step_state().sectors.state();
                u.pending.clear();   // the server's union requests at the tick; a client's holds stamps ahead
                // The record without the keep-local scripts (the balloon's,
                // exception (c): never compared; resync_rules.hpp).
                std::vector<uint8_t> compared;
                script_replay::record_index index;
                if (script_replay::index_record(scripts, index)) {
                    size_t from = 0;
                    for (const auto& s: index.scripts) {
                        auto* beh = CKBehavior::Cast(context->GetObject(s.id));
                        CKBeObject* owner = beh ? beh->GetOwner() : nullptr;
                        if (!beh || !bmmo::session::keep_local_script(owner && owner->GetName() ? owner->GetName() : "",
                                                                      beh->GetName() ? beh->GetName() : ""))
                            continue;
                        compared.insert(compared.end(), scripts.begin() + static_cast<std::ptrdiff_t>(from),
                                        scripts.begin() + static_cast<std::ptrdiff_t>(s.begin));
                        from = s.end;
                    }
                    compared.insert(compared.end(), scripts.begin() + static_cast<std::ptrdiff_t>(from), scripts.end());
                } else {
                    compared = scripts;
                }
                const uint32_t tick = t - root;
                if (std::fwrite(&tick, 4, 1, out) != 1 || !ls_put(out, blob) || !ls_put(out, scripts) || !ls_put(out, compared)
                        || !ls_put(out, sector_union::serialize(u))) {
                    std::fprintf(stderr, "late-sector: write failed\n");
                    return 1;
                }
            }
            if (t >= end) break;
            if (const auto due = stamps.find(t); due != stamps.end())
                for (int sector: due->second) {
                    bmmo::sim::lifecycle_event where;
                    where.type = bmmo::session::event_type::Sector;
                    where.sector = sector;
                    where.tick = t;
                    if (!world->apply_event(pid, where, error)) {
                        std::fprintf(stderr, "sector event failed: %s\n", error.c_str());
                        return 1;
                    }
                }
            // Close-out step S2 (RO2 deferred (b)): --late-ball, the same in
            // every run, after A (the arm's rewind is behind it).
            if (args.late_ball.type >= 0 && t == root + static_cast<uint32_t>(args.late_ball.tick)) {
                if (static_cast<size_t>(args.late_ball.type) >= world->ball_rows().size()) {
                    std::fprintf(stderr, "--late-ball: type %d out of range\n", args.late_ball.type);
                    return 1;
                }
                bmmo::sim::lifecycle_event birth;
                birth.type = bmmo::session::event_type::Physicalize;
                birth.ball_type = static_cast<uint8_t>(args.late_ball.type);
                for (int i = 0; i < 3; ++i) birth.position[i] = static_cast<float>(args.late_ball.at[i]);
                birth.tick = t;
                birth.recipe = world->retail_recipe(birth.ball_type);
                if (!world->apply_event(pid, birth, error)) {
                    std::fprintf(stderr, "--late-ball: physicalize failed: %s\n", error.c_str());
                    return 1;
                }
                std::printf("  ball: player %u's %s physicalized at (%.3f,%.3f,%.3f) before tick %u (root+%u)\n", pid,
                            world->ball_rows()[args.late_ball.type].name.c_str(), args.late_ball.at[0], args.late_ball.at[1],
                            args.late_ball.at[2], t, t - root);
            }
            if (!world->tick(error)) {
                std::fprintf(stderr, "tick %u failed: %s\n", t, error.c_str());
                return 1;
            }
            bmmo::session::parse_event_log(bmmo::physics::drain_event_log(physics),
                                           [&](const std::string& kind, const std::string& text) {
                if (kind == "recipe") {
                    bmmo::session::local_birth birth;
                    if (bmmo::session::parse_recipe_event(text, birth) && !is_ball(birth.name)) births.add(t, std::move(birth));
                } else if ((kind == "created" || kind == "deleted") && !text.empty()) {
                    ++generations[text];
                }
            });
            for (int s: world->active_sectors())
                if (!active_before.count(s)) {
                    pass_log[t] = s;
                    activations.set_pass(t, s);
                    sector_cell = s;
                    std::printf("  live: sector +%d at tick %u (root+%u)\n", s, t, t - root);
                }
            active_before = world->active_sectors();
            activations.record_pending(context, manager_id, script_activations, t + 1);
            {
                std::vector<uint32_t> live = bmmo::game::script_replay::live_frame_order(context, sel);
                // Step RO2's experiment switch (inert when absent): the
                // control writes every live frame's order to the file
                // BMMO_RO2_ORDER_OUT names (tick, count, ids), for an arm
                // that replays the late pass in the control's live order.
                if (const char* order_out = std::getenv("BMMO_RO2_ORDER_OUT"); order_out && *order_out && !arm)
                    if (FILE* f = std::fopen(order_out, t == root ? "wb" : "ab")) {
                        const uint32_t n = static_cast<uint32_t>(live.size());
                        std::fwrite(&t, 4, 1, f);
                        std::fwrite(&n, 4, 1, f);
                        if (n) std::fwrite(live.data(), 4, n, f);
                        std::fclose(f);
                    }
                late_live_order.record(t, std::move(live));   // step RO1
            }
            bmmo::session::rollback_tracked tracked;
            tracked.mechanisms = mechanisms();
            tracked.generations = generations;
            engine.record(binding, t, tracked, {});
            union_records[t] = world->capture_step_state().sectors.state();
            // The own poll's rule (poll_own_sector): a cell no pass of this
            // world wrote is a crossing the client would report.
            if (const int cell = read_cell(); cell != sector_cell) {
                if (cell > 0 && !own.count(cell)) {
                    ++phantom;
                    std::printf("  own Sector event from the cell: %d at tick %u (root+%u)\n", cell, t, t - root);
                }
                sector_cell = cell;
            }
        }
        std::fclose(out);
        std::string passes;
        for (const auto& [t, s]: pass_log) passes += " " + std::to_string(s) + "@" + std::to_string(t - root);
        std::printf("late-sector run=%s done: rewound=%s passes:%s forgotten=%u births_missing=%llu births_moved=%llu "
                    "phantom=%u\n", args.late_run.c_str(), !arm ? "-" : rewound ? "1" : "0", passes.c_str(), rd.forgotten(),
                    static_cast<unsigned long long>(engine.stats().births_missing),
                    static_cast<unsigned long long>(engine.stats().births_moved), phantom);
        return rewound ? 0 : 3;
    }

    // Two runs' capture files compared boundary by boundary (the driver of
    // --late-sector-check): the blobs byte for byte and with the order
    // bookkeeping left out, the script records byte for byte (each
    // difference named against a freshly loaded scene's selection: the
    // records' CK_IDs agree between processes that load the same way), the
    // unions.
    struct ls_compare {
        bool ok = false;
        int boundaries = 0, bytes_equal = 0, order_equal = 0, scripts_equal = 0, scripts_whole_equal = 0, union_equal = 0;
        std::string first_blob, first_strict, first_union, error;
        std::set<std::pair<uint32_t, std::string>> script_diffs;   // (boundary, what)
    };
    ls_compare ls_compare_runs(const arguments& args, const std::string& file_a, const std::string& file_b,
                               std::unique_ptr<bmmo::sim::physics_world>& diff_world,
                               bmmo::game::script_replay::selection& diff_sel) {
        ls_compare c;
        FILE* a = std::fopen(file_a.c_str(), "rb");
        FILE* b = std::fopen(file_b.c_str(), "rb");
        if (!a || !b) {
            if (a) std::fclose(a);
            if (b) std::fclose(b);
            c.error = "a capture file is missing";
            return c;
        }
        for (;;) {
            uint32_t ta = 0, tb = 0;
            const bool more_a = std::fread(&ta, 4, 1, a) == 1, more_b = std::fread(&tb, 4, 1, b) == 1;
            if (!more_a || !more_b) {
                if (more_a != more_b) c.error = "the runs end at different boundaries";
                break;
            }
            std::vector<uint8_t> blob_a, blob_b, scr_a, scr_b, cmp_a, cmp_b, uni_a, uni_b;
            if (ta != tb || !ls_get(a, blob_a) || !ls_get(a, scr_a) || !ls_get(a, cmp_a) || !ls_get(a, uni_a)
                    || !ls_get(b, blob_b) || !ls_get(b, scr_b) || !ls_get(b, cmp_b) || !ls_get(b, uni_b)) {
                c.error = "unreadable or misaligned files";
                break;
            }
            ++c.boundaries;
            if (blob_a == blob_b) {
                ++c.bytes_equal;
                ++c.order_equal;
            } else {
                const std::string where = bmmo::physics::diff_savestate(blob_a.data(), blob_a.size(), blob_b.data(),
                                                                         blob_b.size(), false, 3, true);
                if (where.rfind("no difference", 0) == 0) ++c.order_equal;
                else if (c.first_blob.empty()) c.first_blob = "root+" + std::to_string(ta) + ": " + where;
                if (c.first_strict.empty())
                    c.first_strict = "root+" + std::to_string(ta) + ": "
                                   + bmmo::physics::diff_savestate(blob_a.data(), blob_a.size(), blob_b.data(), blob_b.size(),
                                                                   false, 3, false);
            }
            if (scr_a == scr_b) ++c.scripts_whole_equal;
            if (cmp_a == cmp_b) {
                ++c.scripts_equal;
            } else {
                if (!diff_world) {
                    diff_world = create_session_world(args, 0.0f);
                    if (diff_world) diff_sel = bmmo::game::session_script_selection(diff_world->engine().context());
                }
                std::string what = std::to_string(cmp_a.size()) + " vs " + std::to_string(cmp_b.size()) + " bytes";
                if (diff_world) {
                    // Named on the whole records with the keep-local scripts'
                    // entries of the second spliced into the first, so the
                    // names are the compared scripts' only.
                    CKContext* context = diff_world->engine().context();
                    std::vector<uint8_t> spliced;
                    bmmo::game::script_replay::record_index ia, ib;
                    if (bmmo::game::script_replay::index_record(scr_a, ia) && bmmo::game::script_replay::index_record(scr_b, ib)
                            && ia.scripts.size() == ib.scripts.size()) {
                        size_t from = 0;
                        for (size_t i = 0; i < ia.scripts.size(); ++i) {
                            auto* beh = CKBehavior::Cast(context->GetObject(ia.scripts[i].id));
                            CKBeObject* owner = beh ? beh->GetOwner() : nullptr;
                            if (!beh || !bmmo::session::keep_local_script(owner && owner->GetName() ? owner->GetName() : "",
                                                                          beh->GetName() ? beh->GetName() : ""))
                                continue;
                            spliced.insert(spliced.end(), scr_a.begin() + static_cast<std::ptrdiff_t>(from),
                                           scr_a.begin() + static_cast<std::ptrdiff_t>(ia.scripts[i].begin));
                            spliced.insert(spliced.end(), scr_b.begin() + static_cast<std::ptrdiff_t>(ib.scripts[i].begin),
                                           scr_b.begin() + static_cast<std::ptrdiff_t>(ib.scripts[i].end));
                            from = ia.scripts[i].end;
                        }
                        spliced.insert(spliced.end(), scr_a.begin() + static_cast<std::ptrdiff_t>(from), scr_a.end());
                    } else {
                        spliced = scr_a;
                    }
                    const auto d = bmmo::game::script_replay::diff(context, diff_sel, spliced, scr_b);
                    what = d.where + " | " + d.detail;
                }
                c.script_diffs.emplace(ta, what);
            }
            if (uni_a == uni_b) ++c.union_equal;
            else if (c.first_union.empty()) c.first_union = "root+" + std::to_string(ta);
        }
        std::fclose(a);
        std::fclose(b);
        c.ok = c.error.empty();
        return c;
    }

    void ls_print(const char* label, const ls_compare& c) {
        std::printf("  %s: boundaries=%d blobs identical=%d equal-ignoring-order=%d scripts identical=%d (with the keep-local "
                    "ones %d) union identical=%d%s%s\n",
                    label, c.boundaries, c.bytes_equal, c.order_equal, c.scripts_equal, c.scripts_whole_equal, c.union_equal,
                    c.error.empty() ? "" : " ERROR: ", c.error.c_str());
        if (!c.first_blob.empty()) std::printf("    first blob difference: %s\n", c.first_blob.c_str());
        if (!c.first_strict.empty()) std::printf("    first byte difference (order bookkeeping included): %s\n", c.first_strict.c_str());
        if (!c.first_union.empty()) std::printf("    first union difference: %s\n", c.first_union.c_str());
        std::map<std::string, std::pair<int, std::pair<uint32_t, uint32_t>>> kinds;
        for (const auto& [t, what]: c.script_diffs) {
            auto& k = kinds[what];
            if (!k.first++) k.second.first = t;
            k.second.second = t;
        }
        for (const auto& [what, k]: kinds)
            std::printf("    script difference at %d boundaries, root+%u..root+%u: %s\n", k.first, k.second.first,
                        k.second.second, what.c_str());
    }

    int run_late_sector_check(const arguments& args) {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        if (args.late_t < 1 || args.late_a < args.late_t || args.late_after < 1 || args.late_sector < 2) {
            std::fprintf(stderr, "--late-sector-check K T A needs K >= 2, 1 <= T <= A and --late-after >= 1\n");
            return 2;
        }
        if (args.late_moved >= 0 && args.late_moved <= args.late_t) {
            std::fprintf(stderr, "--late-moved T2 needs T2 > T\n");
            return 2;
        }
        const std::string prefix = args.late_out.empty() ? std::string("late_sector") : args.late_out;
        // The children: this program with the same arguments and the run's.
        // The reference (--late-reference) is the arm with K on time and an
        // ordinary rewind at A: what any replay across the pass leaves.
        std::string base = "\"" + args.self + "\"";
        for (const auto& a: args.raw)
            base += " \"" + a + "\"";
        std::vector<std::pair<std::string, std::string>> runs = {{"control", "control"}, {"arm", "arm"}};
        if (args.late_reference) runs.emplace_back("reference", "arm --late-ordinary");
        std::map<std::string, int> codes;
        std::map<std::string, std::string> summaries;
        for (const auto& [name, run]: runs) {
            const std::string file = prefix + "." + name + ".bin";
            const std::string command = "\"" + base + " --late-run " + run + " --late-out \"" + file + "\" > \"" + prefix
                                      + "." + name + ".log\" 2>&1\"";
            codes[name] = std::system(command.c_str());
            FILE* f = std::fopen((prefix + "." + name + ".log").c_str(), "rb");
            char line[4096];
            while (f && std::fgets(line, sizeof(line), f))
                if (std::strstr(line, "done: rewound") || std::strncmp(line, "late-sector: rewound", 20) == 0
                        || std::strncmp(line, "  re-derived", 12) == 0 || std::strncmp(line, "  forgotten", 11) == 0
                        || std::strncmp(line, "  live:", 7) == 0 || std::strncmp(line, "  own Sector event", 18) == 0)
                    summaries[name] += line;
            if (f) std::fclose(f);
            std::printf("late-sector-check: %s run exit %d (log %s.%s.log)\n%s", name.c_str(), codes[name], prefix.c_str(),
                        name.c_str(), summaries[name].c_str());
        }
        for (const auto& [name, code]: codes)
            if (code != 0) {
                std::printf("FAIL late-sector-check: the %s run failed\n", name.c_str());
                return 3;
            }
        std::unique_ptr<bmmo::sim::physics_world> diff_world;
        bmmo::game::script_replay::selection diff_sel;
        const ls_compare arm = ls_compare_runs(args, prefix + ".control.bin", prefix + ".arm.bin", diff_world, diff_sel);
        ls_compare reference;
        if (args.late_reference)
            reference = ls_compare_runs(args, prefix + ".control.bin", prefix + ".reference.bin", diff_world, diff_sel);
        // The arm's counters from its summary line.
        const std::string& summary = summaries["arm"];
        const auto counter = [&](const char* key) -> long long {
            const size_t at = summary.rfind(key);
            return at == std::string::npos ? -1 : std::atoll(summary.c_str() + at + std::strlen(key));
        };
        const long long missing = counter("births_missing="), forgotten = counter("forgotten="),
                        phantom = counter("phantom="), rewound = counter("rewound=");
        const int expected = args.late_after + 1;
        // Script differences the reference has too, at the same boundary and
        // of the same kind: what an ordinary replay across the pass leaves,
        // not this rule's.
        int arm_only = 0;
        for (const auto& d: arm.script_diffs)
            if (!args.late_reference || !reference.script_diffs.count(d)) ++arm_only;
        const bool exact = arm.ok && arm.boundaries == expected && arm.order_equal == arm.boundaries
                        && arm.scripts_equal == arm.boundaries && missing == 0 && phantom == 0 && rewound == 1
                        && (args.late_expect_forgotten < 0 || forgotten == args.late_expect_forgotten);
        const bool covered = arm.ok && arm.boundaries == expected && arm.order_equal == arm.boundaries && arm_only == 0
                          && missing == 0 && phantom == 0 && rewound == 1
                          && (args.late_expect_forgotten < 0 || forgotten == args.late_expect_forgotten);
        std::printf("late-sector-check K=%d T=%d A=%d%s: boundaries=%d/%d births_missing=%lld forgotten=%lld%s phantom=%lld\n",
                    args.late_sector, args.late_t, args.late_a,
                    args.late_moved >= 0 ? (" moved->" + std::to_string(args.late_moved)).c_str() : "", arm.boundaries,
                    expected, missing, forgotten,
                    args.late_expect_forgotten >= 0 ? (" (expected " + std::to_string(args.late_expect_forgotten) + ")").c_str() : "",
                    phantom);
        ls_print("control vs arm", arm);
        if (args.late_reference) {
            ls_print("control vs reference (ordinary rewind)", reference);
            std::printf("  script differences of the arm the reference does not have: %d\n", arm_only);
        }
        if (!args.late_keep)
            for (const auto& [name, run]: runs) std::remove((prefix + "." + name + ".bin").c_str());
        const char* verdict = exact ? "PASS" : covered && args.late_reference ? "PASS-EXCEPT-REFERENCE" : "FAIL";
        std::printf("%s\n", verdict);
        return exact ? 0 : covered && args.late_reference ? 4 : 3;
    }

    int run_describe_script(const arguments& args) {
        auto world = create_session_world(args, 0.0f);
        if (!world) return 1;
        CKContext* context = world->engine().context();
        CKBehavior* script = bmmo::game::find_root_script(context, args.describe_script.c_str());
        if (!script) {
            std::fprintf(stderr, "no root script named %s\n", args.describe_script.c_str());
            return 1;
        }
        describe_behavior(context, script, 0);
        std::fflush(stdout);   // before the world goes: the engine's shutdown may not return through main
        return 0;
    }

    // ---- step K0 (record-format-gaps-design.md §8): the read-only tools

    // The K0 switches against the mode they belong to; bit 0 of
    // --world-options turns the placement on (the bit's meaning, step S2's
    // physics_world option).  0 = fine, 2 = refused (printed).
    int resolve_record_format_switches(arguments& args) {
        const bool scene = !args.drop_entity.empty() || (args.savestate_check && args.spawn_test <= 0);
        const bool world_mode = scene || args.spawn_test > 0 || args.savestate_check || args.portable_scripts_check
            || args.list_placement || args.resync_check || args.late_sector_check || !args.late_run.empty()
            || args.array_reads || args.probe_census;
        const bool file_mode = !args.replay_session.empty() || !args.synth_trafo_session.empty() || !args.replay.empty()
            || !args.nav_mode.empty() || !args.savestate_diff[0].empty() || !args.savestate_describe.empty()
            || !args.describe_script.empty();
        const char* why = nullptr;
        if ((args.array_reads_client || args.array_reads_all || !args.collectible_fixture.empty()) && !args.array_reads)
            why = "--client-scope, --array-reads-all and --collectible-fixture go with --array-reads";
        else if (args.array_reads && (file_mode || scene || args.spawn_test > 0 || args.savestate_check
                                      || args.portable_scripts_check || args.list_placement || args.resync_check
                                      || args.late_sector_check || !args.late_run.empty()))
            why = "--array-reads is a mode of its own (--level N [--world-options B] [--placement on])";
        else if (args.probe_census && (args.array_reads || file_mode || scene || args.spawn_test > 0 || args.savestate_check
                                       || args.portable_scripts_check || args.list_placement || args.resync_check
                                       || args.late_sector_check || !args.late_run.empty()))
            why = "--probe-census is a mode of its own (--level N [--world-options B] [--placement on])";   // step WG0
        else if (args.probe_release_check && !args.probe_census)
            why = "--release-check goes with --probe-census";   // step WG1
        else if (args.globals_trace && (file_mode || !scene || args.late_sector_check || !args.late_run.empty()
                                        || args.resync_check || args.portable_scripts_check || args.list_placement))
            why = "--globals-trace is for the drop scene (--drop) and the sector scene (--savestate-check without "
                  "--drop or --spawn-test)";
        else if (args.world_options_given && (file_mode || !world_mode))
            why = "--world-options sets a session world's bits (a --replay-session takes them from the journal "
                  "header)";
        else if (args.world_options_given
                 && (args.world_options & ~(bmmo::session::WORLD_PLACEMENT_CLONES | bmmo::session::WORLD_TRAFO_INPUTS
                                            | bmmo::session::WORLD_PROXIMITY_UNION | bmmo::session::WORLD_SESSION_CLOCK
                                            | bmmo::session::WORLD_PLAYER_MECHANISMS)))
            why = "--world-options: only bit 0 (PLACEMENT_CLONES), bit 1 (TRAFO_INPUTS), bit 2 (PROXIMITY_UNION), bit 3 "
                  "(SESSION_CLOCK, step RK3) and bit 4 (PLAYER_MECHANISMS, with --player-mechanisms K) exist";
        else if (args.tutorial_exit_frame_given && !(args.world_options & bmmo::session::WORLD_SESSION_CLOCK))
            why = "--tutorial-exit-frame N goes with --world-options SESSION_CLOCK (the exit runs on --level 1 only)";
        else if (args.mechanism_slots && !(args.probe_census && args.probe_release_check)
                 && !(!args.drop_entity.empty() && (args.world_options & bmmo::session::WORLD_PROXIMITY_UNION)))
            why = "--player-mechanisms K: with --probe-census --release-check (V6), or the drop scene under --world-options "
                  "PROXIMITY_UNION (V7)";
        else if ((args.world_options & bmmo::session::WORLD_PROXIMITY_UNION)
                 && (args.drop_entity.empty() || args.array_reads || args.probe_census || args.late_sector_check
                     || !args.late_run.empty() || args.resync_check || args.portable_scripts_check
                     || args.list_placement))
            // Step G2: the drop scene (--drop) and its --savestate-check are
            // the server world's checks under the bit (V2, V4); the censuses
            // read the bit-clear rewire, and the other modes bind their own
            // per-tick work without the union's place point.
            why = "--world-options PROXIMITY_UNION: only with --drop (the drop scene, alone or with --savestate-check)";
        else if ((args.drop_second_ball >= 0 || args.probe_trace || args.drop_kill >= 0) && args.drop_entity.empty())
            why = "--drop-second-ball, --drop-kill and --probe-trace are for the drop scene (--drop)";
        else if (args.drop_kill >= 0
                 && (args.drop_kill < args.drop_settle
                     || (args.savestate_check && args.drop_kill >= static_cast<long long>(args.drop_settle) + args.savestate_lead)))
            why = "--drop-kill: after the drop (--drop-settle), and with --savestate-check before the window (the lead)";
        else if (args.drop_kill_player && args.drop_kill < 0)
            why = "--drop-kill-player goes with --drop-kill";
        else if (args.late_ball.type >= 0
                 && ((!args.late_sector_check && args.late_run.empty()) || args.late_ball.tick <= args.late_a))
            why = "--late-ball: with --late-sector-check, its tick after A";
        else if (args.probe_scramble
                 && (!args.savestate_check || args.drop_entity.empty()
                     || !(args.world_options & bmmo::session::WORLD_PROXIMITY_UNION)))
            why = "--probe-scramble: the drop scene's --savestate-check under --world-options PROXIMITY_UNION";
        else if ((args.world_options & bmmo::session::WORLD_TRAFO_INPUTS)
                 && !(args.array_reads || args.probe_census || (!args.drop_entity.empty() && !args.trafo.any() && args.late_run.empty()
                                          && !args.late_sector_check && !args.resync_check && !args.savestate_check
                                          && !args.portable_scripts_check && !args.list_placement)))
            // Only the world honours the bit: the other modes make their own
            // script selection without it (the savestate, portable-scripts,
            // resync, late-run and late-sector checks), and the drop scene's
            // trafo sets (--trafo-*) build a second registry for the same
            // players.  A verdict about a selection the world does not run
            // would mislead.
            why = "--world-options TRAFO_INPUTS: only with --array-reads or --probe-census, or --drop without the --trafo-* sets (the "
                  "other modes make their own script selection without the bit)";
        else if (args.world_options_given && args.placement_given
                 && ((args.world_options & bmmo::session::WORLD_PLACEMENT_CLONES) != 0) != args.placement)
            why = "--world-options bit 0 (PLACEMENT_CLONES) and --placement disagree";
        if (why) {
            std::fprintf(stderr, "%s\n", why);
            return 2;
        }
        if (args.world_options & bmmo::session::WORLD_PLACEMENT_CLONES) args.placement = true;
        if (args.world_options_given) std::fprintf(stderr, "world options %08x\n", args.world_options);
        return 0;
    }

    // --array-reads: the census of --level's session world at its anchor
    // (sim/array_census.hpp), nothing ticked, nothing written.  The selection
    // is the one every world makes (session_script_selection, with the
    // world's trafo bit).  Exit 0 (a listing), 1 when the world fails.
    int run_array_reads(const arguments& args) {
        auto world = create_session_world(args, 0.0f);
        if (!world) return 1;
        CKContext* context = world->engine().context();
        const bool trafo = (args.world_options & bmmo::session::WORLD_TRAFO_INPUTS) != 0;
        const auto sel = bmmo::game::session_script_selection(context, trafo);
        if (!args.collectible_fixture.empty()) {   // step F4: the rule's negative fixture
            const std::string done = bmmo::sim::array_census::break_collectible_rule(context, args.collectible_fixture);
            std::printf("collectible fixture %s: %s\n", args.collectible_fixture.c_str(), done.c_str());
            if (done.rfind("no ", 0) == 0) { std::fflush(stdout); return 1; }
        }
        const auto census = bmmo::sim::array_census::run(context, sel, args.array_reads_client);
        bmmo::sim::array_census::print(census, world->level(), args.array_reads_client, args.array_reads_all);
        std::fflush(stdout);   // before the world goes: the engine's shutdown may not return through main
        return 0;
    }

    // --probe-census (world-agreement-gaps-design.md §7.2 V1, step WG0):
    // the proximity census of --level's session world at its anchor
    // (sim/probe_census.hpp), nothing ticked, nothing written.  The
    // selection is the one every world makes (session_script_selection,
    // with the world's trafo bit).  Exit 0 PASS, 3 FAIL, 1 when the world
    // fails.
    int probe_release_check(bmmo::sim::physics_world& world, const bmmo::game::script_replay::selection& sel,
                            const bmmo::sim::probe_census::result& first, uint8_t slots);   // step WG1, below

    int run_probe_census(const arguments& args) {
        auto world = create_session_world(args, 0.0f);
        if (!world) return 1;
        CKContext* context = world->engine().context();
        const bool trafo = (args.world_options & bmmo::session::WORLD_TRAFO_INPUTS) != 0;
        const auto sel = bmmo::game::session_script_selection(context, trafo);
        auto census = bmmo::sim::probe_census::run(context, sel, world->placement(), world->resetpoints(),
                                                   world->proximity_probe_count());
        // Step WG1: the world's union (game/proximity_union.hpp) gives the census's signatures.
        census.have_union = true;
        census.union_mechanism = world->proximity().signature(bmmo::game::probe_class::mechanism);
        census.union_all = world->proximity().signature(bmmo::game::probe_class::all);
        int code = bmmo::sim::probe_census::print(census, world->level(), args.placement);
        if (args.probe_release_check) {
            const int v5 = probe_release_check(*world, sel, census, static_cast<uint8_t>(args.mechanism_slots));
            if (code == 0) code = v5;
        }
        std::fflush(stdout);   // before the world goes: the engine's shutdown may not return through main
        return code;
    }

    // --probe-census --release-check (§7.2 V5, step WG1).  0 PASS, 3 FAIL.
    int probe_release_check(bmmo::sim::physics_world& world, const bmmo::game::script_replay::selection& sel,
                            const bmmo::sim::probe_census::result& first, uint8_t slots) {
        CKContext* context = world.engine().context();
        bool pass = true;
        const auto check = [&pass](const char* name, bool ok, const std::string& detail) {
            std::printf("check %-22s %s%s%s\n", name, ok ? "PASS" : "FAIL", detail.empty() ? "" : "  ", detail.c_str());
            pass = pass && ok;
        };
        const auto census = [&]() {
            return bmmo::sim::probe_census::run(context, sel, world.placement(), world.resetpoints(),
                                                world.proximity_probe_count());
        };
        // Objects of the union still in the world (frames, copies, their locals, BMMO_NoBall).
        const auto leftovers = [context]() {
            size_t n = 0;
            for (CK_CLASSID cid: {CKCID_3DENTITY, CKCID_DATAARRAY, CKCID_PARAMETERLOCAL}) {
                const int count = context->GetObjectsCountByClassID(cid);
                CK_ID* ids = context->GetObjectsListByClassID(cid);
                for (int i = 0; i < count; ++i) {
                    CKObject* o = context->GetObject(ids[i]);
                    const char* name = o ? o->GetName() : nullptr;
                    if (!name) continue;
                    const std::string_view v(name);
                    if (v.rfind("BMMO_Prox_", 0) == 0 || v.rfind("CurrentLevel#BMMOId_", 0) == 0 || v == "BMMO_NoBall") ++n;
                }
            }
            return n;
        };
        const auto hex = [](uint64_t v) {
            char text[20];
            std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(v));
            return std::string(text);
        };
        size_t mech = 0, mech_ids = 0;
        for (const auto& p: first.probes) mech += p.mechanism ? 1u : 0u;
        for (const auto& i: first.identities) mech_ids += i.in_mechanism ? 1u : 0u;
        const size_t all = first.probes.size(), ids = first.identities.size();
        // Step F1 (IDCP): the parameter outputs in the world after a release,
        // against the count after the first one (the anchor's union gone):
        // an identity copy's outputs left behind show as a difference (the
        // objects are nameless, so leftovers() cannot see them).
        int outputs_released = -1;
        const auto released = [&](const char* label, int restored, size_t expected) {
            const auto r = census();
            const size_t left = leftovers();
            const int outputs = context->GetObjectsCountByClassID(CKCID_PARAMETEROUT);
            if (outputs_released < 0) outputs_released = outputs;
            const int outputs_left = outputs - outputs_released;
            check(label, static_cast<size_t>(restored) == expected && r.unrewired == r.probes.size()
                             && r.probes.size() == all && r.identities_on_original == r.identities.size()
                             && r.identities.size() == ids && left == 0 && world.proximity_probe_count() == 0
                             && outputs_left == 0
                             && r.signature_mechanism == first.signature_mechanism && r.signature_all == first.signature_all,
                  "restored " + std::to_string(restored) + " of " + std::to_string(expected) + ", on Ball_Pos_Frame "
                      + std::to_string(r.unrewired) + "/" + std::to_string(r.probes.size()) + ", reads on CurrentLevel "
                      + std::to_string(r.identities_on_original) + "/" + std::to_string(r.identities.size())
                      + ", union objects left " + std::to_string(left) + ", parameter outputs left "
                      + std::to_string(outputs_left));
        };
        std::printf("release check: the anchor's union %zu blocks, %zu identity reads; mechanism %zu, %zu\n", all, ids,
                    mech, mech_ids);
        released("release 1", world.release_proximity(), all + ids);
        // The anchor's rewire again (bit clear, every block).
        std::string error = world.rewire_proximity(false, bmmo::game::probe_class::all);
        {
            const auto r = census();
            check("rewire bit clear", error.empty() && r.unrewired == 0 && r.probes.size() == all
                                          && r.identities_on_original == 0 && r.identities.size() == ids
                                          && world.proximity_probe_count() == all
                                          && r.signature_mechanism == first.signature_mechanism
                                          && r.signature_all == first.signature_all
                                          && world.proximity().signature(bmmo::game::probe_class::mechanism) == first.signature_mechanism
                                          && world.proximity().signature(bmmo::game::probe_class::all) == first.signature_all,
                  error.empty() ? "signature mechanism " + hex(world.proximity().signature(bmmo::game::probe_class::mechanism))
                                      + " all " + hex(world.proximity().signature(bmmo::game::probe_class::all))
                                : error);
        }
        released("release 2", world.release_proximity(), all + ids);
        // The clients' rewire: the mechanism class under the union rules (§2.3).
        error = world.rewire_proximity(true, bmmo::game::probe_class::mechanism);
        {
            const auto r = census();
            const bool no_ball = world.proximity().no_ball() && context->GetObject(world.proximity().no_ball());
            check("rewire union rules", error.empty() && world.proximity_probe_count() == mech
                                            && world.proximity().identity_count() == mech_ids
                                            && r.unrewired == all - mech && no_ball
                                            && world.proximity().signature(bmmo::game::probe_class::mechanism) == first.signature_mechanism,
                  error.empty() ? "blocks " + std::to_string(world.proximity_probe_count()) + ", reads "
                                      + std::to_string(world.proximity().identity_count()) + ", left on Ball_Pos_Frame "
                                      + std::to_string(r.unrewired) + ", signature mechanism "
                                      + hex(world.proximity().signature(bmmo::game::probe_class::mechanism))
                                      + (no_ball ? ", BMMO_NoBall" : ", no BMMO_NoBall")
                                : error);
        }
        released("release 3", world.release_proximity(), mech + mech_ids);
        // Step Q7 (§7.2 V6, --player-mechanisms K): the clients' rewire with
        // K mechanism slots - the copy step on every unit (its listing, the
        // force aliases, the signature) - then the release back to the
        // pre-copy graph (the CK object counts per class), twice, with the
        // same signature both times.
        if (slots) {
            const auto counts = [context]() {
                std::vector<int> c;
                for (CK_CLASSID cid: {CKCID_BEHAVIOR, CKCID_BEHAVIORLINK, CKCID_BEHAVIORIO, CKCID_PARAMETEROPERATION,
                                      CKCID_PARAMETERIN, CKCID_PARAMETEROUT, CKCID_PARAMETERLOCAL})
                    c.push_back(context->GetObjectsCountByClassID(cid));
                return c;
            };
            const auto text = [](const std::vector<int>& c) {
                std::string t;
                for (int v: c) t += (t.empty() ? "" : "/") + std::to_string(v);
                return t;
            };
            // The baseline: the union-rules rewire without units, released.
            // An identity copy (CopyObject(CurrentLevel), step G1's code)
            // leaves parameter outputs behind at its release; measured per
            // identity read here, so the rounds below tell the unit copies'
            // own objects (which must all go) from that.
            std::vector<int> before = counts();
            error = world.rewire_proximity(true, bmmo::game::probe_class::mechanism, 0);
            const int base_reads = static_cast<int>(world.proximity().identity_count());
            // Step F1 (IDCP): what one identity copy owns - the parameter
            // outputs whose owner it is, and how many of them its own
            // parameter columns hold (the rest are not reached by its
            // destruction alone).
            {
                CKDataArray* copy = nullptr;
                const int arrays = context->GetObjectsCountByClassID(CKCID_DATAARRAY);
                CK_ID* array_ids = context->GetObjectsListByClassID(CKCID_DATAARRAY);
                for (int i = 0; i < arrays && !copy; ++i) {
                    CKObject* o = context->GetObject(array_ids[i]);
                    if (o && o->GetName() && std::string_view(o->GetName()).rfind("CurrentLevel#BMMOId_", 0) == 0)
                        copy = CKDataArray::Cast(o);
                }
                if (copy) {
                    int owned = 0, in_columns = 0, columns = 0;
                    std::set<CKObject*> column_outputs;
                    for (int c = 0; c < copy->GetColumnCount(); ++c) {
                        if (copy->GetColumnType(c) != CKARRAYTYPE_PARAMETER) continue;
                        ++columns;
                        for (int row = 0; row < copy->GetRowCount(); ++row)
                            if (auto* cell = copy->GetElement(row, c))
                                if (*cell) column_outputs.insert(reinterpret_cast<CKObject*>(*cell));
                    }
                    const int outs = context->GetObjectsCountByClassID(CKCID_PARAMETEROUT);
                    CK_ID* out_ids = context->GetObjectsListByClassID(CKCID_PARAMETEROUT);
                    for (int i = 0; i < outs; ++i) {
                        auto* parameter = static_cast<CKParameterOut*>(context->GetObject(out_ids[i]));
                        if (!parameter || parameter->GetOwner() != copy) continue;
                        ++owned;
                        in_columns += column_outputs.count(parameter) ? 1 : 0;
                    }
                    std::printf("identity copy %s: %d rows, %d parameter columns, owns %d parameter outputs, %d of them "
                                "in its parameter columns\n",
                                copy->GetName(), copy->GetRowCount(), columns, owned, in_columns);
                }
            }
            world.release_proximity();
            const std::vector<int> base = counts();
            const int base_leak = base[5] - before[5];
            bool base_other = true;
            for (size_t i = 0; i < base.size(); ++i) base_other = base_other && (i == 5 || base[i] == before[i]);
            const int per_read = base_reads > 0 ? (base_leak % base_reads == 0 ? base_leak / base_reads : -1) : (base_leak == 0 ? 0 : -1);
            // Step F1 (IDCP): no parameter output is left (2 per identity
            // copy before the release took the copy's own outputs with it).
            check("baseline", error.empty() && base_other && base_leak == 0,
                  "union rules without units: " + std::to_string(base_reads) + " identity reads, objects " + text(before)
                      + " -> " + text(base) + " (" + std::to_string(base_leak) + " parameter outputs left, "
                      + std::to_string(per_read) + " per identity copy)");
            before = base;
            uint64_t first_signature = 0;
            for (int round = 1; round <= 2; ++round) {
                error = world.rewire_proximity(true, bmmo::game::probe_class::mechanism, slots);
                const auto& u = world.proximity();
                if (round == 1) {
                    for (const std::string& line: u.unit_lines()) std::printf("  %s\n", line.c_str());
                    for (const auto& [id, alias]: u.force_aliases()) std::printf("  force alias %s\n", alias.c_str());
                    std::printf("%s\n", u.units_summary().c_str());
                }
                const size_t units = u.unit_count(), copies = u.copy_count();
                const size_t probes = world.proximity_probe_count(), reads = u.identity_count();
                const uint64_t signature = u.signature(bmmo::game::probe_class::mechanism);
                if (round == 1) first_signature = signature;
                const std::string label = "player mechanisms " + std::to_string(round);
                check(label.c_str(),
                      error.empty() && units == mech_ids && copies == units * (slots - 1u) && probes == mech + copies
                          && reads == mech_ids * slots && signature == first_signature
                          && (slots == 1 || signature != first.signature_mechanism),
                      error.empty() ? "K " + std::to_string(slots) + ", units " + std::to_string(units) + ", copies "
                                          + std::to_string(copies) + ", blocks " + std::to_string(probes) + ", reads "
                                          + std::to_string(reads) + ", signature " + hex(signature)
                                    : error);
                const std::string release_label = "release " + std::to_string(3 + round);
                released(release_label.c_str(), world.release_proximity(), probes + reads);
                std::vector<int> expected = before;
                expected[5] += per_read * static_cast<int>(reads);
                const std::vector<int> after = counts();
                const std::string graph_label = "pre-copy graph " + std::to_string(round);
                check(graph_label.c_str(), after == expected,
                      "objects (behaviours/links/IOs/operations/pIn/pOut/locals) " + text(before) + " -> " + text(after)
                          + ", expected " + text(expected) + " (the baseline's identity-copy outputs x "
                          + std::to_string(reads) + ")");
                before = after;
            }
            // Step F1 (UGRD, the unit-set guard): the server's copy step (class
            // all, every identity read's unit; then its selection marks the
            // class) against a client's (the mechanism class) in this world.
            // Control: with the world's selection the server copies no unit
            // outside the class and its signature is the clients' (round 1's).
            // Fixture: the first unit's root script taken out of the
            // selection (a level whose identity read lies outside a mechanism
            // script): the server keeps that unit's copies, a client makes
            // none.  Without the guard the two signatures are equal (the
            // divergence went unrefused); with it they differ, and the server
            // refuses the member (server.cpp: anchor_probes differ -> "probe
            // mismatch").
            if (mech_ids > 0) {
                error = world.rewire_proximity(true, bmmo::game::probe_class::all, slots);
                const auto& u = world.proximity();
                world.classify_proximity(sel);
                const size_t control_outside = u.non_mechanism_unit_count();
                const uint64_t control_server = u.signature(bmmo::game::probe_class::mechanism);
                const std::vector<CK_ID> roots = u.unit_roots();
                check("unit guard control", error.empty() && !roots.empty() && control_outside == 0
                                                && control_server == first_signature,
                      error.empty() ? "the server's copy step: units " + std::to_string(u.unit_count()) + ", outside the class "
                                          + std::to_string(control_outside) + ", signature " + hex(control_server)
                                          + (control_server == first_signature ? " = " : " != ") + "the clients' "
                                          + hex(first_signature)
                                    : error);
                if (error.empty() && !roots.empty()) {
                    bmmo::game::script_replay::selection reduced = sel;
                    reduced.scripts.erase(std::remove(reduced.scripts.begin(), reduced.scripts.end(), roots.front()),
                                          reduced.scripts.end());
                    world.classify_proximity(reduced);
                    const size_t outside = u.non_mechanism_unit_count();
                    const uint64_t server = u.signature(bmmo::game::probe_class::mechanism);
                    const uint64_t server_unguarded = u.signature(bmmo::game::probe_class::mechanism, false);
                    const size_t server_units = u.unit_count();
                    world.release_proximity();
                    error = world.rewire_proximity(true, bmmo::game::probe_class::mechanism, slots, &reduced);
                    const uint64_t client = world.proximity().signature(bmmo::game::probe_class::mechanism);
                    const size_t client_units = world.proximity().unit_count();
                    const size_t client_outside = world.proximity().non_mechanism_unit_count();
                    check("unit guard", error.empty() && outside > 0 && client_outside == 0 && client_units < server_units
                                            && server_unguarded == client && server != client,
                          error.empty() ? "a unit's script out of the selection: server units " + std::to_string(server_units)
                                              + " (" + std::to_string(outside) + " outside the class), client units "
                                              + std::to_string(client_units) + "; signature server " + hex(server)
                                              + " (without the guard " + hex(server_unguarded) + "), client " + hex(client)
                                              + (server != client ? ": the member is refused (probe mismatch)"
                                                                  : ": NOT refused")
                                        : error);
                }
                world.release_proximity();
                const std::vector<int> after = counts();
                check("unit guard release", after == before,
                      "objects (behaviours/links/IOs/operations/pIn/pOut/locals) " + text(before) + " -> " + text(after));
            }
        }
        std::printf("RELEASE-CHECK: %s\n", pass ? "PASS" : "FAIL");
        return pass ? 0 : 3;
    }

}

int main(int argc, char** argv) {
    bmmo::sim::install_crash_reporter();
    bmmo::sim::opt_out_of_power_throttling();
    // Runs are redirected to files; keep every line up to a crash.
    std::setvbuf(stdout, nullptr, _IOLBF, 1 << 16);
#if defined(_MSC_VER) && defined(_DEBUG)
    // Debug CRT: run-time check failures (uninitialised reads, stack
    // corruption) and assertions go to stderr instead of a dialog box.
    for (int report: {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(report, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(report, _CRTDBG_FILE_STDERR);
    }
#endif
    // Diagnostics: BMMO_SIM_HEAP_PERTURB=<bytes> shifts every later heap
    // address, exposing any dependence of the physics on pointer values.
    if (const char* perturb = std::getenv("BMMO_SIM_HEAP_PERTURB")) {
        static void* keep = std::malloc(static_cast<size_t>(std::atoll(perturb)) + 1);
        (void)keep;
    }
    arguments args;
    if (!parse(argc, argv, args)) {
        std::fprintf(stderr,
            "usage: BallanceMMOSimTool --root <game dir> [--level N] [--ticks N] [--level-at N] "
            "[--report-every N] [--list-bodies-at N] [--dump-surfaces-at N] [--restore-at TICK [ENTITY]] "
            "[--track ENTITY] [--verbose]\n"
            "       BallanceMMOSimTool --root <game dir> --replay <record.bmrc> [--boot-ticks N]\n"
            "           [--nav clone] (session navigation replica; --nav retail-cxx is a diagnostic mode)\n"
            "       BallanceMMOSimTool [--root <game dir>] --replay-session <file.bmjr> [--list] [--ticks N]\n"
            "           [--from A --to B] [--dump-entity NAME] [--stop-on-divergence] [--journal-trace]\n"
            "           [--write-journal <out.bmjr>] [--report-every N] [--continue-after-jump]\n"
            "           [--io-flush MODE | journal=MODE,...]   (step IOF; the SimTool writes --write-journal on its own\n"
            "            thread, with the synchronous journal's flush points: no mode changes them)\n"
            "           [--ignore-resync]\n"
            "           [--placement-trace PATH [--drop-sector N] [--placement-trace-player ID]] [--drop-sectors-above N]\n"
            "           (step SP8, the networked invariant: sector N's rows (default 1) traced after every replayed\n"
            "            tick, with player ID's ball line; Sector events of sectors above N skipped)\n"
            "           (session black box: replays a room's journal and compares every tick and checkpoint;\n"
            "            a client journal's RESYNC record puts the server's state of its tick into the replay;\n"
            "            --list prints the file without booting the engine)\n"
            "       BallanceMMOSimTool --root <game dir> --level N --drop <entity> <ball type> [--ticks N]\n"
            "           [--drop-at X Y Z] [--drop-height F] [--drop-sector N] [--drop-second-sector N]\n"
            "           [--drop-settle N] [--drop-move SECTOR TICK] [--drop-prop ENTITY]\n"
            "           [--drop-player-at X Y Z] [--cells-trace] (mechanism check in a session world; --cells-trace:\n"
            "            IngameParameter[0][1]/[0][2], the union and the physicalized bodies on every change)\n"
            "           [--drop-second-ball TYPE X Y Z] [--drop-kill TICK [--drop-kill-player 1|2]] [--probe-trace] [--probe-scramble before|after] [--world-options PROXIMITY_UNION]\n"
            "           (step G2: player 2's ball as a second candidate; the union's frames and identity cells\n"
            "            on every report line; the server's world under the proximity union's rule)\n"
            "       any session world: [--n2-pin on|off]  (step B2SW: the checkpoint and level-reset chains'\n"
            "            IngameParameter[0][2] Set Cell pinned to 0 at the anchor, default on; off is the A/B)\n"
            "           [--n2-cell on|off]  (step B2SX: the checkpoint chain's IngameParameter[0][1] Set Cell writes a\n"
            "            private copy of the cell, default on; off is the A/B)\n"
            "       BallanceMMOSimTool --root <game dir> --level N --spawn-test N [--spawn-impulse S]\n"
            "           [--spawn-ball T] [--ticks K] [--report-every R] (spawn-impulse determinism check)\n"
            "       BallanceMMOSimTool --root <game dir> --level N --level-at T --explode wood|paper|stone AT_TICK\n"
            "           [--explode ... AT_TICK] [--activate SCRIPT TICK] [--body-guard ENTITY TICK]\n"
            "           [--beam X Y Z TICK] [--ticks N]\n"
            "           (trafo-explosion determinism check, prints the pose hash for 200 ticks)\n"
            "       BallanceMMOSimTool --root <game dir> --level N --savestate-check [--spawn-test N | --drop <entity> <ball type>\n"
            "           [--drop-at X Y Z] [--drop-prop ENTITY] ... | --drop-sector N] [--ticks N] [--rounds R] [--max-depth D]\n"
            "           [--seed S] [--savestate-every K] [--savestate-lead L] [--frames] [--legacy-restore]\n"
            "           [--savestate-trace PATH] [--stop-on-divergence] [--verbose] [--savestate-inert-from-birth]\n"
            "           [--session-recipe | --v14]  (step B2SW: the clients' recipe - births, activations, every script\n"
            "            written back - without a --second-move; step F0: the default with --frames and without\n"
            "            --legacy-restore or trafo inputs, --v14 the old recipe; a line says which one ran;\n"
            "            --v14 means something to --savestate-check only, other modes ignore it)\n"
            "           [--savestate-rounds T:D[:F],...]  (step B2SX: these rounds - anchor tick T, depth D - instead of\n"
            "            the seeded ones, each followed by F live frames compared with the straight pass; needs --frames)\n"
            "           (IVP savestate robustness check: chained seeded rollbacks must reproduce the straight run's\n"
            "            savestate blobs byte for byte; exit 0 PASS, 3 FAIL, 2 vacuous scene)\n"
            "       BallanceMMOSimTool --root <game dir> --level N --portable-scripts-check [--drop-settle N] [--verbose]\n"
            "           (the portable script record localized back must equal the v2 record byte for byte at anchor+30\n"
            "            and after each sector; lists the registry and unportable references; exit 0 PASS, 3 FAIL)\n"
            "       BallanceMMOSimTool --root <game dir> --level N --list-placement\n"
            "           (sector placement: the builder's copy block, its dependency words, the PH rows and the row plan\n"
            "            at the anchor, read-only; exit 0, 3 on a block or plan error)\n"
            "           [--placement on [--placement-fail-at ROW]]  (then applies the placement in the tool: censuses,\n"
            "            fingerprints, signatures, the name review; with the fail row the all-or-nothing check V12)\n"
            "       --placement on|off  (any physics_world mode: the sector placement at the anchor, default off;\n"
            "            --replay: at the record's first frame, default on since step S5; --replay-session follows\n"
            "            the journal header's world options and refuses a journal whose placement signature differs)\n"
            "       --drop ... [--tick-hashes] [--watch-entity NAME]...  (world hash per tick; entity positions)\n"
            "       --drop ... --drop-second-sector N [--second-move SECTOR TICK]... [--drop-again X Y Z TICK]...\n"
            "           [--placement-trace PATH [--placement-trace-ball]]  (the invariant check I1,\n"
            "            scripts/placement_check.py: the second player's passes and the ball's second drops before\n"
            "            those world ticks; the --drop-sector rows' hierarchies, and player 1's ball, after every\n"
            "            world tick)\n"
            "       --savestate-check --frames ... --drop-second-sector N --second-move SECTOR TICK...  (step S7: a pass\n"
            "            inside the window, its births destroyed at a restore and made again by the replay)\n"
            "       any session world: [--perturb-ids N] [--free-ids-before-load N]  (step S7: N CK objects made and\n"
            "            destroyed before the anchor / freed before the level load; prints a copy/original id pair)\n"
            "       --drop ... [--dump-surfaces-at N]  (every body's surface before world tick N, V8)\n"
            "       BallanceMMOSimTool --root <game dir> --level N --resync-check S B [--resync-arms ERSA]\n"
            "           [--resync-stamp SECTOR TICK ...] [--resync-sector K] [--resync-shift N] [--resync-perturb M]\n"
            "           [--resync-perturb-ticks N] [--resync-after N] [--resync-drop TICK --drop <entity> <ball type>\n"
            "           [--drop-at X Y Z] [--drop-height F]] [--resync-watch NAME ...] [--resync-gate MM]\n"
            "           [--resync-root N] [--resync-destroy-after-blob] [--resync-write-equal-rows] [--resync-reregister]\n"
            "           [--resync-harness-diag T] [--savestate-trace PREFIX] [--resync-player ID]\n"
            "           [--resync-server-blob FILE [--resync-blob-rename OLD NEW ...] [--resync-blob-adopt-environment]]\n"
            "           [--resync-perturb-scripts]\n"
            "           [--verbose]\n"
            "           (the resync apply offline: the server's rows, portable scripts and union at S over a client anchor's\n"
            "            blob, re-stepped to B+N against the straight run; exit 0 PASS, 3 FAIL, 2 bad arguments; the\n"
            "            harness line (warm-up run vs the run from the restored root) is informational: every arm and\n"
            "            the straight run start from the same restored root)\n"
            "       BallanceMMOSimTool --root <game dir> --level N --late-sector-check K T A [--late-moved T2]\n"
            "           [--late-also SECTOR TICK]... [--late-after N] [--late-root N] [--late-expect-forgotten N]\n"
            "           [--late-out PREFIX] [--late-keep] [--late-reference] [--late-ball TYPE X Y Z TICK]\n"
            "           (step B1: sector K stamped at T and delivered at A - the late rewind with the union re-derived -\n"
            "            against an on-time control, each run a child process; every boundary A..A+N compared; exit 0\n"
            "            PASS, 3 FAIL, 4 PASS-EXCEPT-REFERENCE (every difference is one an ordinary rewind across the\n"
            "            pass leaves too, --late-reference), 2 bad arguments)\n"
            "       BallanceMMOSimTool --root <game dir> --level N --list-callback-blocks [--dump-at T]\n"
            "           (step EG0: every block of the ten PreSimulate-queueing prototypes, its root script, selected\n"
            "            or OUTSIDE the session selection, and its target's source; read-only)\n"
            "       --replay ... [--queue-trace] [--ball-off FROM TO [--ball-off-wake-only | --ball-off-drop-all]] [--queue-roundtrip]\n"
            "           (step EG0: the PreSimulate queue and the current ball's body whenever they change; the ball's\n"
            "            body out before frame FROM and made again before TO with the retail recipe, and before the\n"
            "            re-make every queued entry but the WakeUps, or every entry, dropped - measurement switches\n"
            "            that change the run)\n"
            "       BallanceMMOSimTool --root <game dir> --level N --array-reads [--client-scope] [--array-reads-all]\n"
            "           (step K0: every data-array access of the selected scripts at the anchor, classified recorded,\n"
            "            private, load-time, sound or unrecorded; --client-scope: a client world's view, the server's\n"
            "            ball identity copies read as the array they copy; read-only)\n"
            "       BallanceMMOSimTool --root <game dir> --level N --probe-census [--placement on]\n"
            "           (step WG0: the proximity census at the anchor - per block its class, frame name, ObjectB,\n"
            "            Distance, axes, Use Outputs, Barycenter; per identity read its kind, gating probe and unit;\n"
            "            the signature; V1's checks against design 1.1.2; read-only; exit 0 PASS, 3 FAIL)\n"
            "       --drop ... | --savestate-check --drop-sector N ... [--globals-trace]  (step K0: the physics time\n"
            "            factor, delta and gravity after every scene tick, and every change)\n"
            "       --world-options BITS  (step K0, a physics_world mode: a number or PLACEMENT_CLONES,TRAFO_INPUTS,\n"
            "            PROXIMITY_UNION,SESSION_CLOCK,PLAYER_MECHANISMS; the session world's session::WORLD_* bits;\n"
            "            bit 0 = --placement on; TRAFO_INPUTS only with --array-reads or --drop without the --trafo-* sets;\n"
            "            SESSION_CLOCK: on --level 1 the tutorial's exit before frame 10, step RK3)\n"
            "       --tutorial-exit-frame N  (step RK3, with --world-options SESSION_CLOCK: the exit before frame N)\n");
        return 2;
    }
    if (const int code = check_scene_switches(args)) return code;   // step S3: before every dispatch
    if (const int code = resolve_record_format_switches(args)) return code;   // step K0
    if (!args.replay_session.empty()) return run_replay_session(args);
    if (!args.synth_trafo_session.empty()) {
        if (args.root.empty()) {
            std::fprintf(stderr, "--synth-trafo-session needs --root <game dir>\n");
            return 2;
        }
        bmmo::sim::synth_trafo_options so;
        so.root = args.root;
        so.out_dir = args.synth_trafo_session;
        so.level = args.level > 0 ? args.level : 2;
        so.ticks = args.synth_ticks;
        return bmmo::sim::run_synth_trafo_session(so);
    }
    if (!args.savestate_diff[0].empty() || !args.savestate_describe.empty()) return run_savestate_files(args);
    if (!args.describe_script.empty()) return run_describe_script(args);
    if (args.root.empty()) {
        std::fprintf(stderr, "--root <game dir> is required\n");
        return 2;
    }
    if (!args.nav_mode.empty()) {
        if (args.replay.empty()) {
            std::fprintf(stderr, "--nav needs --replay <record.bmrc>\n");
            return 2;
        }
        return run_replay_nav(args);
    }
    // Before --drop and --spawn-test: it composes with their scenes.
    if (!args.late_run.empty()) return run_late_sector_run(args);
    if (args.late_sector_check) return run_late_sector_check(args);
    if (args.resync_check) return run_resync_check(args);
    if (args.savestate_check) return run_savestate_check(args);
    if (args.portable_scripts_check) return run_portable_scripts_check(args);
    if (args.list_placement) return run_list_placement(args);
    if (args.array_reads) return run_array_reads(args);   // step K0
    if (args.probe_census) return run_probe_census(args);   // step WG0
    if (!args.drop_entity.empty()) return run_drop(args);
    if (args.spawn_test > 0) return run_spawn_test(args);
    bmmo::sim::engine_options options;
    options.game_root = args.root;
    options.verbose = args.verbose;
    options.log = [](const std::string& text) { std::fprintf(stderr, "%s\n", text.c_str()); };

    std::string error;
    const auto started = std::chrono::steady_clock::now();
    auto engine = bmmo::sim::headless_engine::create(options, error);
    if (!engine) {
        std::fprintf(stderr, "create failed: %s\n", error.c_str());
        return 1;
    }
    if (!engine->load_composition(error)) {
        std::fprintf(stderr, "load failed: %s\n", error.c_str());
        return 1;
    }
    std::fprintf(stderr, "composition loaded in %.2f s: objects=%d entities=%d level=%p playing=%d; %s\n",
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(),
        engine->context()->GetObjectsCountByClassID(CKCID_OBJECT),
        engine->context()->GetObjectsCountByClassID(CKCID_3DENTITY),
        static_cast<void*>(engine->context()->GetCurrentLevel()),
        engine->context()->IsPlaying() ? 1 : 0, root_scripts(*engine).c_str());

    const int result = args.replay.empty() ? run_free(*engine, args) : run_replay(*engine, args);
    std::fprintf(stderr, "done: %llu ticks in %.2f s\n",
        static_cast<unsigned long long>(engine->ticks()),
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    return result;
}
