#pragma once

// SimTool --placement-trace PATH (docs/sector-placement-design.md 7.3, the
// invariant check I1, step S3).  After every world tick of the drop scene,
// one line per object hierarchy of the watched sector's PH rows (the rows of
// --drop-sector, each root object once, in row order):
//
//   - every entity of the hierarchy (the root, then HierarchyParser order):
//     its world matrix (the 16 floats' bits), whether it is visible, and its
//     body: list_bodies' description of every body (movable, simulated,
//     collision, movement state, pose and speeds, %a; so a body asleep and
//     no body differ) and, while the core is in the manager's list of moving
//     objects, its full core state (the world hash's core_state, unrounded:
//     describe_cores_exact's fields up to the time of the last PSI); "-" for
//     no body;
//   - every joint block of the hierarchy's scripts (Set Physics Ball Joint,
//     Hinge, Slider, Spring): whether it holds a handle (h=1/0) and the
//     world matrix of every 3D entity its input parameters name (Object2,
//     the referentials, the axis points).  Not the handle's value: the heap
//     does not hand out the same addresses in two processes (Windows' low-
//     fragmentation heap randomizes them), so three runs of one command
//     disagree on which re-created constraint reuses a freed address
//     (measured, L11 s2 with the placement off, as-built note S3).  A
//     rebuild from a moved frame shows in the referential's matrix, and any
//     rebuild that changes the constraint in the bodies' core states;
//   - every SetPhysicsForce block: force_block_get (exists, the point, the
//     force);
//   - every TT Scaleable Proximity block: its ObjectB's world matrix and its
//     two locals (the in/out state and the Last Check countdown);
//   - every script of the hierarchy: whether it is active (a=1/0; a script
//     a later pass deactivated, or a joint block that executes again, shows
//     here even while the bodies sleep).
//
// With set_ball (--placement-trace-ball), one more line per tick: player 1's
// ball, entity and body as above (the watched sector's bodies' contact
// partner, which the PH rows do not hold; design 9 Q9).
//
// Nothing in a line depends on the run's setup (the later passes, the ids),
// so the control arm's and the test arm's traces are byte-identical exactly
// when I1 holds for the watched sector; scripts/placement_check.py runs the
// arms and compares.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <game/script_replay.hpp>
#include <game/sector_placement.hpp>
#include <physics/physics_state.hpp>

#include "CKAll.h"

namespace bmmo::sim {

    class placement_trace {
    public:
        struct totals {
            long long lines = 0;
            int hierarchies = 0, entities = 0, movable = 0, simulated = 0, fixed = 0, joints = 0, joints_held = 0, forces = 0,
                proximities = 0, scripts = 0, scripts_active = 0;
        };

        placement_trace(CKContext* context, CKIpionManager* physics, int sector)
            : context_(context), physics_(physics), sector_(sector) {}
        ~placement_trace() { close(); }
        placement_trace(const placement_trace&) = delete;
        placement_trace& operator=(const placement_trace&) = delete;

        bool open(const std::string& path, std::string& error) {
            file_ = std::fopen(path.c_str(), "wb");
            if (!file_) error = "cannot write " + path;
            return file_ != nullptr;
        }
        void close() {
            if (file_) std::fclose(file_);
            file_ = nullptr;
        }
        int sector() const { return sector_; }
        // --placement-trace-ball: player 1's ball as its own line after the
        // rows, every tick (set_ball before each write: its entity's name,
        // empty while it has none).
        void trace_ball(bool on) { ball_on_ = on; }
        void set_ball(const std::string& entity) { ball_ = entity; }
        // The counts of the last tick written (stdout summary; the trace
        // itself carries no count that could differ for another reason).
        const totals& last() const { return last_; }
        long long lines() const { return lines_; }

        // One tick: `tick` is the world tick index the frame just ran.
        void write(uint32_t tick) {
            if (!file_ || !context_) return;
            last_ = totals{};
            cores_ = movable_cores();
            bodies_ = all_bodies();
            CKDataArray* ph = game::placement_detail::ph_array(context_);
            const int rows = ph ? ph->GetRowCount() : 0;
            std::set<CK_ID> seen;
            for (int r = 0; r < rows; ++r) {
                int sector = 0;
                if (!ph->GetElementValue(r, 0, &sector) || sector != sector_) continue;
                CKObject* object = ph->GetElementObject(r, 3);
                if (!object || !seen.insert(object->GetID()).second) continue;
                std::string line = "t=" + std::to_string(tick) + " row=" + std::to_string(r) + " "
                                 + game::placement_detail::name_of(object);
                if (CK3dEntity* root = CK3dEntity::Cast(object)) {
                    const std::vector<CK3dEntity*> entities = game::placement_detail::hierarchy_of(root);
                    for (CK3dEntity* e: entities) put_entity(line, e);
                    for (CK3dEntity* e: entities)
                        for (int s = 0; s < e->GetScriptCount(); ++s) put_script(line, e->GetScript(s));
                } else {
                    line += " (not a 3D entity)";
                }
                line += '\n';
                std::fwrite(line.data(), 1, line.size(), file_);
                ++last_.hierarchies;
                ++lines_;
            }
            if (ball_on_) {
                std::string line = "t=" + std::to_string(tick) + " ball " + (ball_.empty() ? std::string("-") : ball_);
                CK3dEntity* ball = ball_.empty() ? nullptr : CK3dEntity::Cast(context_->GetObjectByNameAndClass(
                    const_cast<CKSTRING>(ball_.c_str()), CKCID_3DOBJECT, nullptr));
                if (ball) put_entity(line, ball); else line += " | -";
                line += '\n';
                std::fwrite(line.data(), 1, line.size(), file_);
                ++lines_;
            }
            last_.lines = lines_;
        }

    private:
        static void put_matrix(std::string& line, const VxMatrix& m) {
            static_assert(sizeof(VxMatrix) == 16 * sizeof(uint32_t), "VxMatrix is 16 floats");
            uint32_t bits[16];
            std::memcpy(bits, &m, sizeof(bits));
            char text[16 * 9 + 2];
            char* at = text;
            for (int k = 0; k < 16; ++k)
                at += std::snprintf(at, static_cast<size_t>(text + sizeof(text) - at), k ? ",%08x" : "%08x", bits[k]);
            line += text;
        }

        static void put_entity_matrix(std::string& line, CK3dEntity* e) {
            line += game::placement_detail::name_of(e);
            line += ':';
            put_matrix(line, e->GetWorldMatrix());
        }

        // Every movable core, keyed by its object's name: the line of
        // describe_cores_exact after the name, up to the time of the last PSI
        // (the fields of the world hash's core_state; the sim unit's
        // countdown and the calm references after it are left out).
        std::multimap<std::string, std::string> movable_cores() const {
            std::multimap<std::string, std::string> out;
            const std::string text = physics::describe_cores_exact(physics_);
            size_t at = 0;
            while (at < text.size()) {
                size_t end = text.find('\n', at);
                if (end == std::string::npos) end = text.size();
                const std::string line = text.substr(at, end - at);
                at = end + 1;
                const size_t space = line.find(" st=");
                if (space == std::string::npos) continue;
                size_t stop = line.find(" nmc=", space);
                if (stop == std::string::npos) stop = line.size();
                out.emplace(line.substr(0, space), line.substr(space + 1, stop - space - 1));
            }
            return out;
        }

        // Every body (simulated or asleep, movable or not), by entity name:
        // list_bodies' description.
        std::multimap<std::string, physics::body_state> all_bodies() const {
            std::multimap<std::string, physics::body_state> out;
            const int total = physics::list_bodies(physics_, nullptr, 0);
            if (total <= 0) return out;
            std::vector<physics::body_state> bodies(static_cast<size_t>(total));
            const int count = physics::list_bodies(physics_, bodies.data(), total);
            for (int i = 0; i < count && i < total; ++i)
                out.emplace(bodies[static_cast<size_t>(i)].name, bodies[static_cast<size_t>(i)]);
            return out;
        }

        void put_entity(std::string& line, CK3dEntity* e) {
            const std::string name = game::placement_detail::name_of(e);
            line += " | ";
            put_entity_matrix(line, e);
            line += e->IsVisible() ? " vis" : " hid";
            auto [first, end] = bodies_.equal_range(name);
            if (first == end) line += " -";
            for (auto it = first; it != end; ++it) {
                const physics::body_state& b = it->second;
                char text[1024];
                std::snprintf(text, sizeof(text),
                              " body{m=%d s=%d c=%d st=%d pos=%a,%a,%a rot=%a,%a,%a,%a v=%a,%a,%a w=%a,%a,%a}",
                              b.movable ? 1 : 0, b.simulated ? 1 : 0, b.collision_enabled ? 1 : 0,
                              static_cast<int>(b.movement_state), b.position[0], b.position[1], b.position[2],
                              b.rotation[0], b.rotation[1], b.rotation[2], b.rotation[3], b.linear[0], b.linear[1],
                              b.linear[2], b.angular[0], b.angular[1], b.angular[2]);
                line += text;
                if (b.movable) ++last_.movable; else ++last_.fixed;
                if (b.simulated) ++last_.simulated;
            }
            auto [core_first, core_end] = cores_.equal_range(name);
            for (auto it = core_first; it != core_end; ++it) line += " core{" + it->second + "}";
            ++last_.entities;
        }

        static bool is_proximity(CKBehavior* beh) {
            const char* prototype = beh->IsUsingFunction() ? beh->GetPrototypeName() : nullptr;
            return prototype && std::strcmp(prototype, "TT Scaleable Proximity") == 0;
        }

        void put_script(std::string& line, CKBehavior* script) {
            if (!script) return;
            const std::string script_name = game::placement_detail::name_of(script);
            const bool active = script->IsActive() != FALSE;
            line += " || " + script_name + (active ? " a=1" : " a=0");
            ++last_.scripts;
            if (active) ++last_.scripts_active;
            int index = 0;
            game::placement_detail::walk_blocks(script, [&](CKBehavior* beh) {
                const int at = index++;
                const bool joint = game::script_replay::is_joint_block(beh);
                const bool force = game::script_replay::is_force_block(beh);
                const bool proximity = is_proximity(beh);
                if (!joint && !force && !proximity) return;
                line += " || " + script_name + "#" + std::to_string(at) + " " + game::placement_detail::name_of(beh);
                if (joint) {
                    const bool held = game::script_replay::joint_handle(beh) != nullptr;
                    if (held) ++last_.joints_held;
                    line += held ? " h=1" : " h=0";
                    for (int p = 0; p < beh->GetInputParameterCount(); ++p)
                        if (CK3dEntity* e = CK3dEntity::Cast(beh->GetInputParameterObject(p))) {
                            line += " in" + std::to_string(p) + "=";
                            put_entity_matrix(line, e);
                        }
                    ++last_.joints;
                } else if (force) {
                    bmmo_physics_force_block state{};
                    std::string error;
                    char text[256];
                    if (physics::force_block_get(physics_, static_cast<uint32_t>(beh->GetID()), state, error))
                        std::snprintf(text, sizeof(text), " exists=%d p=%a,%a,%a f=%a,%a,%a", static_cast<int>(state.exists),
                                      state.position[0], state.position[1], state.position[2], state.force[0],
                                      state.force[1], state.force[2]);
                    else
                        std::snprintf(text, sizeof(text), " <%s>", error.c_str());
                    line += text;
                    ++last_.forces;
                } else {
                    if (CK3dEntity* b = CK3dEntity::Cast(beh->GetInputParameterObject(2))) {
                        line += " B=";
                        put_entity_matrix(line, b);
                    } else {
                        line += " B=-";
                    }
                    for (int l = 0; l < 2; ++l) {
                        int value = 0;
                        CKParameterLocal* local = beh->GetLocalParameter(l);
                        if (local && local->GetValue(&value) == CK_OK)
                            line += " l" + std::to_string(l) + "=" + std::to_string(value);
                        else
                            line += " l" + std::to_string(l) + "=?";
                    }
                    ++last_.proximities;
                }
            });
        }

        CKContext* context_ = nullptr;
        CKIpionManager* physics_ = nullptr;
        int sector_ = 0;
        std::FILE* file_ = nullptr;
        long long lines_ = 0;
        totals last_;
        std::multimap<std::string, std::string> cores_;
        std::multimap<std::string, physics::body_state> bodies_;
        std::string ball_;
        bool ball_on_ = false;
    };
}
