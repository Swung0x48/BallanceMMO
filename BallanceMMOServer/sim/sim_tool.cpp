// Command-line driver for the headless engine.
//
//   BallanceMMOSimTool --root <game dir> [--level N] [--ticks N] [--level-at N]
//   BallanceMMOSimTool --root <game dir> --replay <record.bmrc> [--dump-diverge]
//   BallanceMMOSimTool --root <game dir> --replay-session <file.bmjr> [--list] [--write-journal <out.bmjr>]
//   BallanceMMOSimTool --root <game dir> --level N --spawn-test N [--spawn-impulse S] [--spawn-ball T]
//       [--ticks K] [--report-every R]
//   BallanceMMOSimTool --root <game dir> --level N --level-at T --explode wood|paper|stone AT_TICK [--ticks N]
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

#include "headless_engine.hpp"
#include "crash_report.hpp"
#include "physics_world.hpp"
#include <entity/session.hpp>
#include <physics/physics_state.hpp>
#include <session/journal.hpp>

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
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
            else return false;
        }
        // --list of a journal is pure file reading: no game data needed.
        return !out.root.empty() || !out.replay_session.empty();
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
    // Server-side mechanism check (design 8.3): boots a physics-session world
    // the way a room does, puts its player (and optionally a second one) in a
    // sector so the union starts that sector's mechanisms, and drops the
    // player's ball of the given type onto a mechanism part, then prints that
    // part's height every report interval.  A mechanism that reacts to the
    // ball (the rope bridge P_Modul_29 tearing under a stone ball) moves; one
    // that does not holds its anchor pose.
    int run_drop(const arguments& args) {
        bmmo::sim::world_options options;
        options.game_root = args.root;
        options.level = args.level > 0 ? args.level : 1;
        options.seed = 1;
        options.boot_ticks = args.boot_ticks;
        options.anchor_timeout = args.anchor_timeout;
        options.log = [](const std::string& text) { std::fprintf(stderr, "%s\n", text.c_str()); };
        std::string error;
        auto world = bmmo::sim::physics_world::create(options, error);
        if (!world) {
            std::fprintf(stderr, "world create failed: %s\n", error.c_str());
            return 1;
        }
        if (!world->add_player(1, 0, error)) {
            std::fprintf(stderr, "add_player failed: %s\n", error.c_str());
            return 1;
        }
        // The sector union follows the players: report where each of them is
        // and the world starts (and stops) the right mechanisms on its own.
        const auto report_sector = [&](uint32_t player, int sector) {
            bmmo::sim::lifecycle_event where;
            where.type = bmmo::session::event_type::Sector;
            where.sector = sector;
            if (!world->apply_event(player, where, error))
                std::fprintf(stderr, "sector event failed: %s\n", error.c_str());
        };
        if (args.drop_second_sector > 0) {
            if (!world->add_player(2, 1, error)) {
                std::fprintf(stderr, "add_player 2 failed: %s\n", error.c_str());
                return 1;
            }
            report_sector(2, args.drop_second_sector);
        }
        report_sector(1, args.drop_sector);
        for (int i = 0; i < args.drop_settle; ++i)
            if (!world->tick(error)) { std::fprintf(stderr, "tick failed: %s\n", error.c_str()); return 1; }
        if (!args.dump_array.empty()) dump_array(world->engine(), args.dump_array);
        VxVector spawn(args.drop_at[0], args.drop_at[1], args.drop_at[2]);
        if (!args.have_drop_at) {
            // Module parts share names across sectors, so --drop-at (the PH
            // table's matrix for the module) is the reliable way in.
            auto* target = CK3dEntity::Cast(world->engine().context()->GetObjectByNameAndParentClass(
                const_cast<CKSTRING>(args.drop_entity.c_str()), CKCID_3DENTITY, nullptr));
            if (!target) {
                std::fprintf(stderr, "no 3D entity named %s (pass --drop-at X Y Z)\n", args.drop_entity.c_str());
                return 1;
            }
            target->GetPosition(&spawn);
        }
        spawn.y += args.drop_height;
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
        VxVector player_spawn = args.have_drop_player_at
            ? VxVector(args.drop_player_at[0], args.drop_player_at[1], args.drop_player_at[2]) : spawn;
        bmmo::sim::lifecycle_event event;
        event.type = bmmo::session::event_type::Physicalize;
        event.ball_type = static_cast<uint8_t>(args.drop_ball);
        event.position[0] = player_spawn.x;
        event.position[1] = player_spawn.y;
        event.position[2] = player_spawn.z;
        event.recipe = world->retail_recipe(event.ball_type);
        if (!world->apply_event(1, event, error)) {
            std::fprintf(stderr, "physicalize failed: %s\n", error.c_str());
            return 1;
        }
        std::printf("dropped ball type %d (%s) at (%.3f,%.3f,%.3f), watching %s\n", args.drop_ball,
            world->ball_rows()[args.drop_ball].name.c_str(), player_spawn.x, player_spawn.y, player_spawn.z,
            args.drop_entity.c_str());
        if (!args.drop_prop.empty()) {
            // The level's own ball, physicalized by the sector activation: beam
            // it to the drop spot the same way a player would have pushed it.
            const double position[3] = {spawn.x, spawn.y, spawn.z};
            const double upright[4] = {0.0, 0.0, 0.0, 1.0};   // beaming needs a rotation as well
            const float still[3] = {0.0f, 0.0f, 0.0f};
            if (!bmmo::physics::set_body_state(world->physics(), args.drop_prop.c_str(), position, upright,
                                               still, still, true, error))
                std::fprintf(stderr, "prop %s: %s\n", args.drop_prop.c_str(), error.c_str());
            else
                std::printf("prop %s beamed to (%.3f,%.3f,%.3f)\n", args.drop_prop.c_str(), spawn.x, spawn.y, spawn.z);
        }
        report_parts(-1);
        std::map<std::string, double> start;
        for (const auto& body: watched(*world)) start.emplace(body.name, body.position[1]);
        for (int i = 0; i < args.ticks; ++i) {
            if (args.drop_move_sector > 0 && i == args.drop_move_tick) {
                report_sector(1, args.drop_move_sector);
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
        bmmo::sim::world_options options;
        options.game_root = args.root;
        options.level = args.level > 0 ? args.level : 1;
        options.seed = 1;
        options.spawn_impulse = args.spawn_impulse;
        options.boot_ticks = args.boot_ticks;
        options.anchor_timeout = args.anchor_timeout;
        options.log = [](const std::string& text) { std::fprintf(stderr, "%s\n", text.c_str()); };
        std::string error;
        auto world = bmmo::sim::physics_world::create(options, error);
        if (!world) {
            std::fprintf(stderr, "world create failed: %s\n", error.c_str());
            return 1;
        }
        const uint8_t ball_type = static_cast<uint8_t>(args.spawn_ball);
        if (ball_type >= world->ball_rows().size()) {
            std::fprintf(stderr, "--spawn-ball %d is out of range (%zu ball types)\n", args.spawn_ball, world->ball_rows().size());
            return 2;
        }
        const int n = std::max(1, args.spawn_test);
        std::vector<uint32_t> ids;
        for (int i = 0; i < n; ++i) {
            const uint32_t id = static_cast<uint32_t>(i + 1);
            if (!world->add_player(id, static_cast<uint8_t>(i), error)) {
                std::fprintf(stderr, "add_player %u failed: %s\n", id, error.c_str());
                return 1;
            }
            ids.push_back(id);
        }
        for (int i = 0; i < args.drop_settle; ++i)
            if (!world->tick(error)) { std::fprintf(stderr, "tick failed: %s\n", error.c_str()); return 1; }
        // Every player physicalizes at the resetpoint in the same tick, the
        // way N retail clients spawning together would report it.
        const VxMatrix& spawn = world->spawn_matrix();
        for (const uint32_t id: ids) {
            bmmo::sim::lifecycle_event event;
            event.type = bmmo::session::event_type::Physicalize;
            event.ball_type = ball_type;
            event.flags = bmmo::session::PHYSICALIZE_FLAG_SPAWN;
            for (int k = 0; k < 3; ++k) event.position[k] = spawn[3][k];
            for (int r = 0; r < 3; ++r)
                for (int k = 0; k < 3; ++k) event.rotation[r * 3 + k] = spawn[r][k];
            event.recipe = world->retail_recipe(ball_type);
            event.tick = world->tick_index();
            if (!world->apply_event(id, event, error)) {
                std::fprintf(stderr, "physicalize player %u failed: %s\n", id, error.c_str());
                return 1;
            }
        }
        std::printf("spawned %d players (ball type %d, %s) at tick %u, impulse %.3f m/s\n", n, ball_type,
            world->ball_rows()[ball_type].name.c_str(), world->tick_index(), static_cast<double>(args.spawn_impulse));
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
        // The stamp, not the tick the record is filed under: the spawn impulse
        // direction is derived from this field, and the two differ whenever the
        // event reached the server after the tick it was asked for.  The reader
        // already resolved a file written before the split (no trailing u32) to
        // the applied tick, so 0 here means an event stamped for tick 0.
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
                    if (!in.ok) break;
                    seen_tick(e.tick);
                    // "stamped S applied T" only when they differ: that gap is
                    // an event that reached the server after its own tick, and
                    // is itself worth seeing.
                    char stamp[48] = {};
                    if (e.event_tick != e.tick)
                        std::snprintf(stamp, sizeof(stamp), " stamped %u applied %u", e.event_tick, e.tick);
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
            // The server applies a tick's events in the members' join order
            // (session_runner::step), and that order is what a server journal's
            // file order already IS.  A client wrote them in the order they
            // reached it - its own first, the relayed ones after - so a client
            // journal has to be sorted back into the server's rule here, or the
            // replay creates two balls of the same tick the other way round and
            // diverges from the session for good.
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
                        return join_rank(a->id) < join_rank(b->id);
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
                // about the recording, not about the physics.
                if (event.tick != e.tick)
                    std::printf("tick %u: %s event from p%u was stamped for tick %u\n", e.tick,
                        event_type_name(e.type), e.id, event.tick);
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
            "[--report-every N] [--list-bodies-at N] [--dump-surfaces-at N] [--verbose]\n"
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
            "           (trafo-explosion determinism check, prints the pose hash for 200 ticks)\n");
        return 2;
    }
    if (!args.replay_session.empty()) return run_replay_session(args);
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
