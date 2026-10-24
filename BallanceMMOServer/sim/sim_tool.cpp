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
//       [--savestate-trace PATH] [--stop-on-divergence] [--verbose]
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
#include "crash_report.hpp"
#include "process_qos.hpp"
#include "physics_world.hpp"
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
        std::string replay_session;
        bool list_journal = false;
        bool stop_on_divergence = false;
        bool continue_after_jump = false;
        bool journal_trace = false;
        std::string write_journal;
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
        float drop_at[3] = {};         // --drop-at X Y Z: drop there instead (PH matrix of a module)
        bool have_drop_at = false;
        // --drop-prop ENTITY: beam that level prop (a non-player ball placed by
        // the PH table) onto the drop spot as well, to check that a mechanism
        // still only reacts to the player's ball.
        std::string drop_prop;
        float drop_player_at[3] = {};  // --drop-player-at X Y Z: the player's ball goes here instead
        bool have_drop_player_at = false;
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
        bool ignore_ov = false;           // --ignore-ov: --savestate-diff leaves the overlap tree out
        bool ignore_order = false;        // --ignore-order: ...and the cache ring and unit numbering
        int max_diff = 1;                 // --max-diff N: --savestate-diff lists up to N differing fields
        int savestate_rounds = 50;
        int savestate_max_depth = 48;
        unsigned long long savestate_seed = 1;
        int savestate_every = 1;
        int savestate_lead = 1;
        bool savestate_frames = false;
        bool legacy_restore = false;
        std::string savestate_trace;
    };

    bool parse(int argc, char** argv, arguments& out) {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
            const char* v = nullptr;
            if (arg == "--root") { if (!(v = next())) return false; out.root = v; }
            else if (arg == "--replay") { if (!(v = next())) return false; out.replay = v; }
            else if (arg == "--replay-session") { if (!(v = next())) return false; out.replay_session = v; }
            else if (arg == "--list") out.list_journal = true;
            else if (arg == "--stop-on-divergence") out.stop_on_divergence = true;
            else if (arg == "--continue-after-jump") out.continue_after_jump = true;
            else if (arg == "--journal-trace") out.journal_trace = true;
            else if (arg == "--write-journal") { if (!(v = next())) return false; out.write_journal = v; }
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
            else if (arg == "--ignore-ov") out.ignore_ov = true;
            else if (arg == "--ignore-order") out.ignore_order = true;
            else if (arg == "--max-diff") { if (!(v = next())) return false; out.max_diff = std::atoi(v); }
            else if (arg == "--rounds") { if (!(v = next())) return false; out.savestate_rounds = std::atoi(v); }
            else if (arg == "--max-depth") { if (!(v = next())) return false; out.savestate_max_depth = std::atoi(v); }
            else if (arg == "--seed") { if (!(v = next())) return false; out.savestate_seed = std::strtoull(v, nullptr, 0); }
            else if (arg == "--savestate-every") { if (!(v = next())) return false; out.savestate_every = std::atoi(v); }
            else if (arg == "--savestate-lead") { if (!(v = next())) return false; out.savestate_lead = std::atoi(v); }
            else if (arg == "--frames") out.savestate_frames = true;
            else if (arg == "--legacy-restore") out.legacy_restore = true;
            else if (arg == "--savestate-trace") { if (!(v = next())) return false; out.savestate_trace = v; }
            else return false;
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

    int run_free(bmmo::sim::headless_engine& engine, const arguments& args) {
        std::string error;
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
            for (const auto& [name, tick]: args.activations) {
                if (static_cast<long long>(i) != tick) continue;
                CKBehavior* script = bmmo::game::find_root_script(engine.context(), name.c_str());
                if (!script) {
                    std::fprintf(stderr, "activate: no root script named %s\n", name.c_str());
                    return 1;
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
            if (!args.dump_array.empty() && i == args.dump_at) dump_array(engine, args.dump_array);
            if (args.list_bodies_at >= 0 && i == args.list_bodies_at) list_bodies(engine);
            if (args.dump_surfaces_at >= 0 && i == args.dump_surfaces_at) dump_surfaces(engine);
            if (args.report_every > 0 && engine.ticks() % static_cast<uint64_t>(args.report_every) == 0)
                report(engine);
        }
        if (!args.dump_script.empty() && args.dump_at < 0) dump_script(engine, args.dump_script);
        if (!args.list_scripts.empty() && args.dump_at < 0) list_scripts(engine, args.list_scripts);
        if (!args.dump_array.empty() && args.dump_at < 0) dump_array(engine, args.dump_array);
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

    std::unique_ptr<bmmo::sim::physics_world> create_session_world(const arguments& args, float spawn_impulse) {
        bmmo::sim::world_options options;
        options.game_root = args.root;
        options.level = args.level > 0 ? args.level : 1;
        options.seed = 1;
        options.spawn_impulse = spawn_impulse;
        options.boot_ticks = args.boot_ticks;
        options.anchor_timeout = args.anchor_timeout;
        options.log = [](const std::string& text) { std::fprintf(stderr, "%s\n", text.c_str()); };
        std::string error;
        auto world = bmmo::sim::physics_world::create(options, error);
        if (!world) std::fprintf(stderr, "world create failed: %s\n", error.c_str());
        return world;
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
        if (!settle_world(world, args.drop_settle)) return 1;
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
        if (const int code = setup_drop_scene(args, *world)) return code;
        std::string error;
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
        };
        report_parts(-1);
        std::map<std::string, double> start;
        for (const auto& body: watched(*world)) start.emplace(body.name, body.position[1]);
        for (int i = 0; i < args.ticks; ++i) {
            if (args.drop_move_sector > 0 && i == args.drop_move_tick) {
                report_sector(*world, 1, args.drop_move_sector);
                std::printf("tick=%d player 1 moved to sector %d\n", i, args.drop_move_sector);
            }
            if (!world->tick(error)) { std::fprintf(stderr, "tick failed: %s\n", error.c_str()); return 1; }
            if (args.report_every > 0 && i % args.report_every == 0) report_parts(i);
        }
        double fell = 0.0;
        for (const auto& body: watched(*world)) {
            auto it = start.find(body.name);
            if (it != start.end()) fell = std::max(fell, it->second - body.position[1]);
        }
        std::printf("summary: the lowest watched part of %s fell %.3f m\n", args.drop_entity.c_str(), fell);
        std::fflush(stdout);
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
    //             report the difference), and so is --drop-move.
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
        if (!settle_world(*world, args.savestate_lead)) return 1;
        scene = "level" + std::to_string(world->level()) + ":" + scene;
        CKIpionManager* physics = world->physics();
        CKContext* context = world->engine().context();
        if (!physics || !physics->GetEnvironment()) {
            std::fprintf(stderr, "savestate-check: no physics environment after the scene\n");
            return 1;
        }

        // ---- the script replay (--frames): the mechanism scripts and the
        // hooks that carry the force blocks' controllers, as the headless
        // client installs them.
        bmmo::game::script_replay::selection scripts;
        bmmo::game::script_replay_hooks hooks;
        if (frames) {
            scripts = bmmo::game::script_replay::select(context);
            hooks.capture_force = [physics](CKBehavior* block, bmmo::game::force_block_state& out) {
                bmmo_physics_force_block state{};
                std::string error;
                if (!bmmo::physics::force_block_get(physics, static_cast<uint32_t>(block->GetID()), state, error))
                    return false;
                out.exists = state.exists != 0;
                for (int k = 0; k < 3; ++k) { out.position[k] = state.position[k]; out.force[k] = state.force[k]; }
                return true;
            };
            hooks.restore_force = [physics](CKBehavior* block, const bmmo::game::force_block_state& in) {
                bmmo_physics_force_block state{};
                state.exists = in.exists ? 1 : 0;
                for (int k = 0; k < 3; ++k) { state.position[k] = in.position[k]; state.force[k] = in.force[k]; }
                std::string error;
                return bmmo::physics::force_block_set(physics, static_cast<uint32_t>(block->GetID()), state, error);
            };
            const auto st = bmmo::game::script_replay::describe(context, scripts);
            std::printf("savestate-check: script replay: %d scripts (%d active), %d blocks, %d parameters, "
                        "%d force blocks, %d opaque\n", st.scripts, st.active, st.blocks, st.parameters,
                        st.force_blocks, st.opaque);
        }

        // ---- the schedule first: it decides which blobs are kept
        const std::vector<savestate_round> schedule = savestate_schedule(
            args.savestate_seed, args.savestate_rounds, args.savestate_max_depth, window);
        std::vector<char> keep(static_cast<size_t>(window), 0);
        for (int i = 0; i < window; i += args.savestate_every) keep[static_cast<size_t>(i)] = 1;
        for (const auto& entry: schedule) {
            keep[static_cast<size_t>(entry.anchor)] = 1;
            keep[static_cast<size_t>(entry.anchor + entry.depth)] = 1;
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
        binding_options.before_replayed_tick = [&](uint32_t tick) {
            if (tick != probe_tick) return;
            std::string probe_error;
            if (!bmmo::physics::get_savestate(physics, restored_blob, probe_error)) capture_error = probe_error;
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
        bmmo::session::rollback_world binding = bmmo::session::bind_rollback_world(backend, binding_options);
        if (frames) {
            bmmo::session::script_binding_options script_options;
            script_options.context = context;
            script_options.scripts = &scripts;
            script_options.hooks = &hooks;
            script_options.scripts_replayed = &scripts_replayed;
            script_options.delta_ms = bmmo::sim::kFixedDeltaMs;
            bmmo::session::bind_script_replay(binding, backend, script_options);
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

        // ---- the straight pass
        std::string error;
        std::vector<straight_tick> straight(static_cast<size_t>(window));
        std::vector<int> reset_ticks;   // --frames: window indices at which a selected script had a reset pending
        int script_resets = 0;
        uint32_t contacts_min = std::numeric_limits<uint32_t>::max(), collisions_min = contacts_min,
                 systems_min = contacts_min;
        uint32_t contacts_max = 0, collisions_max = 0, systems_max = 0;
        size_t blob_max = 0;
        std::vector<uint8_t> blob;
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
            }
            bmmo::physics::savestate_report report;
            if (!bmmo::physics::get_savestate(physics, blob, error, &report)) {
                std::fprintf(stderr, "savestate-check: capture failed at tick %u: %s\n", t, error.c_str());
                return 1;
            }
            rec.blob_hash = bmmo::physics::hash_savestate(blob.data(), blob.size());
            rec.blob_size = blob.size();
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
                rec.blob = blob;
                rec.exact = bmmo::physics::describe_cores_exact(physics);
            }
            // The engine's record of this tick: bodies, extras, contacts,
            // scripts and (unless --legacy-restore) the blob - what a rewind
            // to this tick restores.
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
            if (!step(error)) {
                std::fprintf(stderr, "savestate-check: the straight pass's step at tick %u failed: %s\n", t, error.c_str());
                return 1;
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
        bool dumped = false;
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

            // The world's own per-tick bookkeeping (tick index, RNG cursors,
            // the sector union) at the anchor; the engine knows the physics.
            world->restore_step_state(anchor.counters);
            engine = pristine;
            probe_tick = t0 + static_cast<uint32_t>(a) + 1u;
            restored_blob.clear();
            replayed_blobs.clear();
            restore_error.clear();
            capture_error.clear();
            engine_log.clear();
            restore_refused = false;
            const uint64_t restored_before = engine.stats().savestates_restored;
            const uint64_t resim_before = engine.stats().resim_ticks;
            const bool rewound = engine.rewind_to(binding, t0 + static_cast<uint32_t>(a), t0 + static_cast<uint32_t>(e),
                                                  no_input, "savestate-check");
            // rewind_to's true says the request was handled, not that a replay
            // ran: the stats say what happened.
            if (!rewound) {
                failure = "the engine refused the rewind" + (engine_log.empty() ? std::string() : ": " + engine_log);
            } else if (!legacy && restore_refused) {
                failure = "restore error: " + restore_error;
                restore_broke = true;
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
                std::string probe_error;
                if (!bmmo::physics::get_savestate(physics, restored_blob, probe_error)) failure = "capture error: " + probe_error;
            }
            if (failure.empty() && restored_blob != anchor.blob) {
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
                        const auto difference = bmmo::game::script_replay::diff(context, scripts, expected.scripts, bytes);
                        failure = "scripts: " + difference.where + " (" + std::to_string(difference.logical_count)
                                + " logical, " + std::to_string(difference.float_count) + " float)";
                    }
                }
            }

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
        const bool pass = failed == 0 && run == static_cast<int>(schedule.size());
        std::printf("%s rounds=%d failed=%d\n", pass ? "PASS" : "FAIL", run, failed);
        std::fflush(stdout);
        return pass ? 0 : 3;
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
        }
        return "unknown";
    }

    const char* correction_kind_name(uint8_t kind) {
        static const char* names[] = {"mismatch", "rollback", "hard", "blend", "resync", "too_far", "frozen", "unmatched"};
        return kind < sizeof(names) / sizeof(names[0]) ? names[kind] : "?";
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
        options.log = [](const std::string& text) { std::fprintf(stderr, "%s\n", text.c_str()); };
        auto world = bmmo::sim::physics_world::create(options, error);
        if (!world) {
            std::fprintf(stderr, "world create failed: %s\n", error.c_str());
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
            out_header.utc_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            const std::filesystem::path path = args.write_journal;
            std::error_code ec;
            if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
            std::string open_error;
            if (!out.open(path, out_header, 0, open_error)) {
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
            for (const auto& p: group.players) {
                if (p.added) {
                    if (!world->add_player(p.id, p.join_order, error))
                        std::printf("tick %u: add_player %u failed: %s\n", p.tick, p.id, error.c_str());
                    else
                        replay_players[p.id] = p.join_order;
                } else {
                    world->remove_player(p.id);
                    replay_players.erase(p.id);
                }
                if (out.is_open()) out.player(p.tick, p.id, p.join_order, p.added, p.name);
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
                std::printf("warning: the journal jumps from tick %u to tick %u (a late join or a resync restarts the"
                            " numbering); everything after this point is compared against a world that is %lld ticks"
                            " behind\n", previous_group, group.tick,
                    static_cast<long long>(group.tick) - static_cast<long long>(world->tick_index()));
                if (!args.continue_after_jump) {
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
                if (static_cast<uint8_t>(e.type) > static_cast<uint8_t>(bmmo::session::event_type::BodyRevived)) {
                    // A newer writer's event type: the world would not know
                    // what to do with it, and guessing would be worse.
                    std::printf("tick %u: event type %u from p%u is unknown to this build, skipped\n", e.tick,
                        static_cast<unsigned>(e.type), e.id);
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
            if ((!same || checkpoint_bad) && args.stop_on_divergence) {
                std::printf("stopping at the first divergence (tick %u, %s)\n", group.tick,
                    !same ? (checkpoint_bad ? "the hash and a checkpoint" : "the hash") : "a checkpoint");
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
        if (stopped_at_jump)
            std::printf("note: the replay stopped at a tick jump; the summary covers only the ticks before it\n");
        if (client_journal)
            std::printf("note: a client journal's own TICK hashes are of its own world; the %zu received snapshots"
                        " are the server's word and are what was compared\n", checkpoints);
        else if (compared != simulated)
            std::printf("note: %zu of %zu ticks had no TICK record and were simulated without comparing\n",
                simulated - compared, simulated);
        std::fflush(stdout);
        return first_divergence < 0 && first_checkpoint_divergence < 0 && !stopped_at_jump ? 0 : 3;
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
        hooks.capture_force = [physics](CKBehavior* block, bmmo::game::force_block_state& out) {
            bmmo_physics_force_block state{};
            std::string e;
            if (!bmmo::physics::force_block_get(physics, static_cast<uint32_t>(block->GetID()), state, e)) return false;
            out.exists = state.exists != 0;
            for (int k = 0; k < 3; ++k) { out.position[k] = state.position[k]; out.force[k] = state.force[k]; }
            return true;
        };
        hooks.restore_force = [physics](CKBehavior* block, const bmmo::game::force_block_state& in) {
            bmmo_physics_force_block state{};
            state.exists = in.exists ? 1 : 0;
            for (int k = 0; k < 3; ++k) { state.position[k] = in.position[k]; state.force[k] = in.force[k]; }
            std::string e;
            return bmmo::physics::force_block_set(physics, static_cast<uint32_t>(block->GetID()), state, e);
        };
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
        hooks.capture_force = [physics](CKBehavior* block, bmmo::game::force_block_state& out) {
            bmmo_physics_force_block state{};
            std::string e;
            if (!bmmo::physics::force_block_get(physics, static_cast<uint32_t>(block->GetID()), state, e)) return false;
            out.exists = state.exists != 0;
            for (int k = 0; k < 3; ++k) { out.position[k] = state.position[k]; out.force[k] = state.force[k]; }
            return true;
        };
        hooks.restore_force = [physics](CKBehavior* block, const bmmo::game::force_block_state& in) {
            bmmo_physics_force_block state{};
            state.exists = in.exists ? 1 : 0;
            for (int k = 0; k < 3; ++k) { state.position[k] = in.position[k]; state.force[k] = in.force[k]; }
            std::string e;
            return bmmo::physics::force_block_set(physics, static_cast<uint32_t>(block->GetID()), state, e);
        };
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
            if (!script_replay::restore(context, sel, hooks, root.v2, &e, nullptr, nullptr, true)) {
                std::fprintf(stderr, "root: script restore failed: %s\n", e.c_str());
                return false;
            }
            // The harness's own restore re-registers what the record made
            // active (rc_unregistered_scripts): the root is the warm-up run's.
            std::string names;
            if (const int n = rc_unregistered_scripts(context, sel, true, &names); n > 0 && args.verbose)
                std::printf("  root: %d script(s) re-registered with the behaviour manager:%s\n", n, names.c_str());
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
            "           (session black box: replays a room's journal and compares every tick and checkpoint;\n"
            "            --list prints the file without booting the engine)\n"
            "       BallanceMMOSimTool --root <game dir> --level N --drop <entity> <ball type> [--ticks N]\n"
            "           [--drop-at X Y Z] [--drop-height F] [--drop-sector N] [--drop-second-sector N]\n"
            "           [--drop-settle N] [--drop-move SECTOR TICK] [--drop-prop ENTITY]\n"
            "           [--drop-player-at X Y Z] (mechanism check in a session world)\n"
            "       BallanceMMOSimTool --root <game dir> --level N --spawn-test N [--spawn-impulse S]\n"
            "           [--spawn-ball T] [--ticks K] [--report-every R] (spawn-impulse determinism check)\n"
            "       BallanceMMOSimTool --root <game dir> --level N --level-at T --explode wood|paper|stone AT_TICK\n"
            "           [--explode ... AT_TICK] [--activate SCRIPT TICK] [--body-guard ENTITY TICK]\n"
            "           [--beam X Y Z TICK] [--ticks N]\n"
            "           (trafo-explosion determinism check, prints the pose hash for 200 ticks)\n"
            "       BallanceMMOSimTool --root <game dir> --level N --savestate-check [--spawn-test N | --drop <entity> <ball type>\n"
            "           [--drop-at X Y Z] [--drop-prop ENTITY] ... | --drop-sector N] [--ticks N] [--rounds R] [--max-depth D]\n"
            "           [--seed S] [--savestate-every K] [--savestate-lead L] [--frames] [--legacy-restore]\n"
            "           [--savestate-trace PATH] [--stop-on-divergence] [--verbose]\n"
            "           (IVP savestate robustness check: chained seeded rollbacks must reproduce the straight run's\n"
            "            savestate blobs byte for byte; exit 0 PASS, 3 FAIL, 2 vacuous scene)\n"
            "       BallanceMMOSimTool --root <game dir> --level N --portable-scripts-check [--drop-settle N] [--verbose]\n"
            "           (the portable script record localized back must equal the v2 record byte for byte at anchor+30\n"
            "            and after each sector; lists the registry and unportable references; exit 0 PASS, 3 FAIL)\n"
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
            "            the straight run start from the same restored root)\n");
        return 2;
    }
    if (!args.replay_session.empty()) return run_replay_session(args);
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
    if (args.resync_check) return run_resync_check(args);
    if (args.savestate_check) return run_savestate_check(args);
    if (args.portable_scripts_check) return run_portable_scripts_check(args);
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
