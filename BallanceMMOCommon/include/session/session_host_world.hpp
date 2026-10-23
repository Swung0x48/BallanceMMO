#pragma once

// Host support, the CK-free half (docs/session-client-core-design.md 2.4;
// step E9): platform code both hosts of the shared session-client core need
// identically, templated on a duck-typed physics backend (physics_direct on
// the headless client, physics_view on the retail mod), with no CK header so
// that the unit tests can bind a fake backend:
//
//   * local_birth / local_birth_log: every body this world made, by the tick
//     of the record that first carries it (the event log's "recipe" entries,
//     engine change #19), behind the binding's birth_tick_of, birth_order,
//     insert_birth and destroy_body;
//   * parse_recipe_event and parse_event_log: the event log's formats;
//   * bind_session_world: the rollback binding (bind_rollback_world) with the
//     core's half (core_world_hooks) and the host's (session_world_options)
//     in design 2.4's order.
//
// The CK half (the manager activations, the live cells, the mirrors' bodies,
// the navigation attach) is game/session_host_support.hpp.  Moved from the
// headless client (session_client.cpp at 0ea2e05) at E9, verbatim.

#include "client_platform.hpp"
#include "rollback.hpp"
#include "rollback_binding.hpp"
#include "../physics/physics_rt_api.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace bmmo::session {

    // A body this world made (the event log's "recipe" line): the
    // Physicalize recipe, collision group and the entity's matrix at that
    // moment.  A rewind anchored before its tick re-creates or re-poses it
    // from this.
    struct local_birth {
        std::string name, group;
        bmmo_physics_ball_recipe recipe{};
        float matrix[16] = {};         // the entity's world matrix at the Physicalize
        float local_matrix[16] = {};   // ...and its local one, for a parented entity
        bool has_parent = false, has_local = false;
    };

    // Every body this world made, by the tick of the record that first
    // carries it, the last 4096 ticks.
    class local_birth_log {
    public:
        void clear() { by_tick_.clear(); }
        void add(uint32_t tick, local_birth&& birth) {
            by_tick_[tick].push_back(std::move(birth));
            while (!by_tick_.empty() && by_tick_.begin()->first + 4096 < tick) by_tick_.erase(by_tick_.begin());
        }
        // The birth of `entity` recorded at `tick`, or - tick 0 - its latest.
        const local_birth* find(const std::string& entity, uint32_t tick) const {
            if (tick) {
                const auto it = by_tick_.find(tick);
                if (it == by_tick_.end()) return nullptr;
                for (const auto& b: it->second) if (b.name == entity) return &b;
                return nullptr;
            }
            for (auto it = by_tick_.rbegin(); it != by_tick_.rend(); ++it)
                for (const auto& b: it->second) if (b.name == entity) return &b;
            return nullptr;
        }
        // The tick a body was created at (its latest birth); 0 when unknown.
        uint32_t birth_tick_of(const std::string& entity) const {
            for (auto it = by_tick_.rbegin(); it != by_tick_.rend(); ++it)
                for (const auto& b: it->second) if (b.name == entity) return it->first;
            return 0;
        }
        // The order the live frame created a tick's bodies in: the event
        // log's order, which is the scripts' order.
        std::vector<std::string> birth_order(uint32_t tick) const {
            std::vector<std::string> names;
            const auto it = by_tick_.find(tick);
            if (it != by_tick_.end())
                for (const auto& b: it->second) names.push_back(b.name);
            return names;
        }

    private:
        std::map<uint32_t, std::vector<local_birth>> by_tick_;
    };

    // "<name>|fixed=|frozen=|coll=|cmc=|fr=|el=|m=|ld=|rd=|mc=x,y,z|surf=|convex=a/b|balls=x,y,z,r/..|concave=a/b|group=|mtx=16 floats"
    inline bool parse_recipe_event(const std::string& text, local_birth& out) {
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

    // The bridge's event log ("t=<time> <kind> <name>;" per entry): `each`
    // gets every well-formed entry's kind and name, in order.
    template <class Each>
    void parse_event_log(const std::string& events, Each&& each) {
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
            each(kind, name);
        }
    }

    // The host's half of bind_session_world (design 2.4): its binding
    // options and what only it has.
    struct session_world_options {
        // The host's switches and callbacks; before_replayed_tick,
        // replay_remotes and on_correction are bind_session_world's (the
        // core's, then the host's below).
        rollback_binding_options binding;
        // Before each replayed tick, after the core's births: the manager
        // activation recorded for it (the host's manager_activations), then
        // the host's traces.
        std::function<void(uint32_t tick)> replay_activation;
        std::function<void(uint32_t tick)> before_replayed_tick;
        // The bodies this world made (birth_tick_of, birth_order, and the
        // birth an insert or a destroy uses).
        const local_birth_log* births = nullptr;
        // --park-births: no destroy_body (a birth inside the window is
        // parked through the gap); --insert-births: insert_birth installed.
        bool park_births = false;
        bool insert_births = false;
        // A destroyed mechanism body's entity put back where its live
        // Physicalize found it (CK: the host's).
        std::function<void(const std::string& entity, const local_birth& birth)> put_back;
        // The failures, for the host's log lines.
        std::function<void(const std::string& entity)> on_own_destroy_failed;
        std::function<void(const std::string& entity, const std::string& error)> on_destroy_failed;
        std::function<void(const std::string& entity, uint32_t birth_tick, const std::string& error)> on_insert_failed;
        // The mechanism scripts' part of the binding (CK:
        // bind_script_replay), last.
        std::function<void(rollback_world& world)> bind_scripts;
    };

    // The rollback engine's view of a host's world (design 2.4): the one
    // binding every user of the engine shares, with the core's hooks and
    // the host's options in one order - before each replayed tick the
    // core's births, then the manager activation, then the host's traces
    // (and the scripts' frame after them, bind_script_replay); the
    // replayed mirrors and the corrections the core's; destroy_body the
    // core's moved own birth first, then the shared mechanism destroy.
    // `backend` must outlive the returned world.
    template <class Backend>
    rollback_world bind_session_world(Backend& backend, const core_world_hooks& hooks, const session_world_options& o) {
        Backend* b = &backend;
        rollback_binding_options binding = o.binding;
        const auto core_births = hooks.before_replayed_tick;
        const auto activation = o.replay_activation;
        const auto host_before = o.before_replayed_tick;
        binding.before_replayed_tick = [core_births, activation, host_before](uint32_t tick) {
            if (core_births) core_births(tick);
            if (activation) activation(tick);
            if (host_before) host_before(tick);
        };
        binding.replay_remotes = hooks.replay_remotes;
        binding.replay_mechanisms = hooks.replay_mechanisms;
        binding.on_correction = hooks.on_correction;
        rollback_world world = bind_rollback_world(backend, binding);
        const local_birth_log* births = o.births;
        // A body born inside the window is put where its Physicalize made it
        // at the start of its birth tick (set_body_matrix), its joints and
        // controllers untouched (--insert-births; the balls stay on the
        // parking path, the replica's controllers are on them).
        if (o.insert_births) {
            const auto own_body = hooks.own_body;
            const auto failed = o.on_insert_failed;
            world.insert_birth = [b, own_body, births, failed](const std::string& entity, uint32_t birth_tick) {
                if (entity.rfind("Ball_", 0) == 0 || (own_body && own_body(entity))) return false;
                const local_birth* birth = births ? births->find(entity, birth_tick) : nullptr;
                if (!birth) return false;
                std::string error;
                if (!b->set_body_matrix(entity.c_str(), birth->matrix, !(birth->recipe.start_frozen || birth->recipe.fixed), error)) {
                    if (failed) failed(entity, birth_tick, error);
                    return false;
                }
                return true;
            };
        }
        // The tick a body was created at (its latest birth), for the
        // untracked ones the blob does not know.
        world.birth_tick_of = [births](const std::string& entity) -> uint32_t {
            return births ? births->birth_tick_of(entity) : 0u;
        };
        world.birth_order = [births](uint32_t tick) {
            return births ? births->birth_order(tick) : std::vector<std::string>{};
        };
        // Plan doc 10 (the default): a mechanism body born inside the
        // window is destroyed at the restore, with its entity put back at
        // the matrix its Physicalize found, and the replayed module script
        // makes it again the live way, with its joints and forces.  The
        // balls keep the parking path, and a moved own birth (late-player
        // design 3.3) is the core's.  The event log is muted: the destroy is
        // not a lifetime change of the body the records describe.
        if (!o.park_births) {
            const auto own_birth = hooks.destroy_own_birth;
            const auto own_body = hooks.own_body;
            const auto mirror = hooks.mirror;
            const auto put_back = o.put_back;
            const auto own_failed = o.on_own_destroy_failed;
            const auto failed = o.on_destroy_failed;
            world.destroy_body = [b, own_birth, own_body, mirror, births, put_back, own_failed, failed](
                                         const std::string& entity, uint32_t birth_tick) {
                bool destroyed = false;
                if (own_birth && own_birth(entity, birth_tick, destroyed)) {
                    if (!destroyed && own_failed) own_failed(entity);
                    return destroyed;
                }
                if (entity.rfind("Ball_", 0) == 0 || (own_body && own_body(entity))) return false;
                if (mirror && mirror(entity)) return false;
                const local_birth* birth = births ? births->find(entity, birth_tick) : nullptr;
                if (!birth && births) birth = births->find(entity, 0);
                std::string error, mute_error;
                b->set_event_log_muted(true, mute_error);
                const bool ok = b->unphysicalize(entity.c_str(), error);
                if (ok && birth && put_back) put_back(entity, *birth);
                b->set_event_log_muted(false, mute_error);
                if (!ok && failed) failed(entity, error);
                return ok;
            };
        }
        if (o.bind_scripts) o.bind_scripts(world);
        return world;
    }

}
