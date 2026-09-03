// Command-line driver for the headless engine.
//
//   BallanceMMOSimTool --root <game dir> [--level N] [--ticks N] [--level-at N]
//   BallanceMMOSimTool --root <game dir> --replay <record.bmrc> [--dump-diverge]
//
// Replay mode boots base.cmo, loads the recorded level, waits for the retail
// Gameplay_Ingame script to activate (the record's frame 0 anchor), performs
// the same session reset as the client recorder, then feeds the recorded
// keyboard state tick by tick and compares physics hashes.

#include "headless_engine.hpp"
#include "crash_report.hpp"
#include "physics_world.hpp"
#include <entity/session.hpp>
#include <physics/physics_state.hpp>

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
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {
    struct arguments {
        std::string root;
        std::string replay;
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
        std::string list_scripts;      // --list-scripts SUBSTR: every root script whose name or owner matches
        std::string dump_script;       // print this script's whole graph (blocks, parameters, links)
        int dump_at = -1;              // tick at which to dump (-1: after the run)
        std::string nav_mode;          // --nav retail-cxx|clone: replay through the session navigation (design 8.6)
        long long nav_frames = -1;     // --nav-frames N: compare only the first N frames
        int list_bodies_at = -1;       // --list-bodies-at N: bridge v2 list_bodies() at that tick
        // --drop ENTITY BALLTYPE: server-side mechanism check.  Boots a
        // physics-session world, activates the sectors, drops one player's
        // ball of that type on that entity and prints what the mechanism does.
        std::string drop_entity;
        int drop_ball = 0;
        float drop_height = 4.0f;      // --drop-height F: metres above the entity
        int drop_sectors = 12;         // --drop-sectors N: sectors to activate first
        float drop_at[3] = {};         // --drop-at X Y Z: drop there instead (PH matrix of a module)
        bool have_drop_at = false;
    };

    bool parse(int argc, char** argv, arguments& out) {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
            const char* v = nullptr;
            if (arg == "--root") { if (!(v = next())) return false; out.root = v; }
            else if (arg == "--replay") { if (!(v = next())) return false; out.replay = v; }
            else if (arg == "--level") { if (!(v = next())) return false; out.level = std::atoi(v); }
            else if (arg == "--ticks") { if (!(v = next())) return false; out.ticks = std::atoi(v); }
            else if (arg == "--level-at") { if (!(v = next())) return false; out.level_at_tick = std::atoi(v); }
            else if (arg == "--report-every") { if (!(v = next())) return false; out.report_every = std::atoi(v); }
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
            else if (arg == "--dump-array") { if (!(v = next())) return false; out.dump_array = v; }
            else if (arg == "--list-scripts") { if (!(v = next())) return false; out.list_scripts = v; }
            else if (arg == "--dump-script") { if (!(v = next())) return false; out.dump_script = v; }
            else if (arg == "--dump-at") { if (!(v = next())) return false; out.dump_at = std::atoi(v); }
            else if (arg == "--nav") { if (!(v = next())) return false; out.nav_mode = v; }
            else if (arg == "--nav-frames") { if (!(v = next())) return false; out.nav_frames = std::atoll(v); }
            else if (arg == "--list-bodies-at") { if (!(v = next())) return false; out.list_bodies_at = std::atoi(v); }
            else if (arg == "--drop") {
                if (!(v = next())) return false; out.drop_entity = v;
                if (!(v = next())) return false; out.drop_ball = std::atoi(v);
            }
            else if (arg == "--drop-at") {
                for (float& k: out.drop_at) { if (!(v = next())) return false; k = static_cast<float>(std::atof(v)); }
                out.have_drop_at = true;
            }
            else if (arg == "--drop-height") { if (!(v = next())) return false; out.drop_height = static_cast<float>(std::atof(v)); }
            else if (arg == "--drop-sectors") { if (!(v = next())) return false; out.drop_sectors = std::atoi(v); }
            else return false;
        }
        return !out.root.empty();
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
            std::printf("tick=%llu hash=%016llx pose=%016llx cores=%d ivp_time=%.6f seed=%d delta=%.4f pdelta=%.6f factor=%.6f ingame=%d\n",
                static_cast<unsigned long long>(engine.ticks()),
                static_cast<unsigned long long>(hash.hash), static_cast<unsigned long long>(hash.pose), hash.cores, hash.ivp_time,
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

    int run_free(bmmo::sim::headless_engine& engine, const arguments& args) {
        std::string error;
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
            if (!args.trace_script.empty() && i < args.trace_ticks) trace_script(engine, args.trace_script);
            if (!args.dump_script.empty() && i == args.dump_at) dump_script(engine, args.dump_script);
            if (!args.list_scripts.empty() && i == args.dump_at) list_scripts(engine, args.list_scripts);
            if (!args.dump_array.empty() && i == args.dump_at) dump_array(engine, args.dump_array);
            if (args.list_bodies_at >= 0 && i == args.list_bodies_at) list_bodies(engine);
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
    // the way a room does, activates the sectors, and drops one player's ball
    // of the given type onto a mechanism part, then prints that part's height
    // every report interval.  A mechanism that reacts to the ball (the rope
    // bridge P_Modul_29 tearing under a stone ball) moves; one that does not
    // holds its anchor pose.
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
        if (!world->add_player(1, error)) {
            std::fprintf(stderr, "add_player failed: %s\n", error.c_str());
            return 1;
        }
        // One sector at a time: Gameplay_SectorManager needs its ticks to walk
        // the PH table before the next activation resets it.
        for (int sector = 1; sector <= args.drop_sectors; ++sector) {
            world->activate_sector(sector);
            for (int i = 0; i < 30; ++i)
                if (!world->tick(error)) { std::fprintf(stderr, "tick failed: %s\n", error.c_str()); return 1; }
        }
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
        bmmo::sim::lifecycle_event event;
        event.type = bmmo::session::event_type::Physicalize;
        event.ball_type = static_cast<uint8_t>(args.drop_ball);
        event.position[0] = spawn.x;
        event.position[1] = spawn.y;
        event.position[2] = spawn.z;
        event.recipe = world->retail_recipe(event.ball_type);
        if (!world->apply_event(1, event, error)) {
            std::fprintf(stderr, "physicalize failed: %s\n", error.c_str());
            return 1;
        }
        std::printf("dropped ball type %d (%s) at (%.3f,%.3f,%.3f) onto %s\n", args.drop_ball,
            world->ball_rows()[args.drop_ball].name.c_str(), spawn.x, spawn.y, spawn.z, args.drop_entity.c_str());
        report_parts(-1);
        std::map<std::string, double> start;
        for (const auto& body: watched(*world)) start.emplace(body.name, body.position[1]);
        for (int i = 0; i < args.ticks; ++i) {
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
        if (!world->add_player(1, error)) {
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
            "[--report-every N] [--list-bodies-at N] [--verbose]\n"
            "       BallanceMMOSimTool --root <game dir> --replay <record.bmrc> [--boot-ticks N]\n"
            "           [--nav clone] (session navigation replica; --nav retail-cxx is a diagnostic mode)\n"
            "       BallanceMMOSimTool --root <game dir> --level N --drop <entity> <ball type> [--ticks N]\n"
            "           [--drop-height F] [--drop-sectors N] (mechanism check in a session world)\n");
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
