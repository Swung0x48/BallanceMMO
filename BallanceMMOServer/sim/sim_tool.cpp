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
#include "physics_world.hpp"
#include <entity/session.hpp>
#include <game/script_replay.hpp>
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
                std::printf("%s    link %s -> %s%s\n", pad.c_str(), io_name(link->GetInBehaviorIO()).c_str(), io_name(link->GetOutBehaviorIO()).c_str(),
                            link->GetActivationDelay() ? (" delay " + std::to_string(link->GetActivationDelay())).c_str() : "");
            }
            for (int i = 0; i < beh->GetSubBehaviorCount(); ++i)
                if (CKBehavior* child = beh->GetSubBehavior(i)) describe_behavior(context, child, depth + 1);
        }
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
            "            savestate blobs byte for byte; exit 0 PASS, 3 FAIL, 2 vacuous scene)\n");
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
    if (args.savestate_check) return run_savestate_check(args);
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
