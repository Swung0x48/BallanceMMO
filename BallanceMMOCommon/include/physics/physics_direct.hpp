#pragma once

// The in-process physics backend for session/rollback_binding.hpp: the same
// methods, under the same names, as the retail mod's physics_view
// (BallanceMMOClient/physics/physics_view.hpp), each a call into
// bmmo::physics::* on the CKIpionManager this process owns.  Headless only:
// the headless session client and the SimTool have the engine in the process;
// the mod has only the plugin's C API and uses physics_view.

#include <physics/ball_navigation.hpp>
#include <physics/physics_savestate.hpp>
#include <physics/physics_state.hpp>
#include <physics/world_hash.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

class CKIpionManager;

namespace bmmo::physics {

    struct physics_direct {
        CKIpionManager* manager = nullptr;
        // What the last savestate_get / savestate_set reported (counts,
        // skipped records, warnings): the headless client's trace line.
        savestate_report last_report;

        explicit physics_direct(CKIpionManager* m = nullptr) : manager(m) {}

        bool get_body_state(const char* entity, bmmo_physics_body_state& out, std::string& error) const {
            return bmmo::physics::get_body_state(manager, entity, out, error);
        }
        // The record capture's lookup scope (physics_state.hpp
        // entity_lookup_scope_begin; rollback_world::capture_scope).
        void lookup_scope(bool on) const {
            if (on) bmmo::physics::entity_lookup_scope_begin(manager);
            else bmmo::physics::entity_lookup_scope_end(manager);
        }
        bool set_body_state(const char* entity, const double position[3], const double rotation[4], const float linear[3],
                            const float angular[3], wake_mode mode, bool recheck, std::string& error) const {
            return bmmo::physics::set_body_state(manager, entity, position, rotation, linear, angular, mode, recheck, error);
        }
        bool body_recheck(const char* entity, std::string& error) const { return bmmo::physics::recheck_body(manager, entity, error); }
        bool get_core_extra(const char* entity, bmmo_physics_core_extra& out, std::string& error) const {
            return bmmo::physics::get_core_extra(manager, entity, out, error);
        }
        bool set_core_extra(const char* entity, const bmmo_physics_core_extra& in, std::string& error) const {
            return bmmo::physics::set_core_extra(manager, entity, in, error);
        }
        bool get_clock_state(bmmo_physics_clock_state& out, std::string& error) const {
            return bmmo::physics::get_clock_state(manager, out, error);
        }
        bool set_clock_state(const bmmo_physics_clock_state& in, std::string& error) const {
            return bmmo::physics::set_clock_state(manager, in, error);
        }
        bool get_clock(float& time_factor, float& physics_delta, std::string& error) const {
            return bmmo::physics::get_clock(manager, time_factor, physics_delta, error);
        }
        int contacts_get(const char* entity, bmmo_physics_contact* out, int max, std::string& error) const {
            return bmmo::physics::get_contacts(manager, entity, out, max, error);
        }
        int contacts_set(const char* entity, const bmmo_physics_contact* in, int count, uint32_t flags, std::string& error) const {
            return bmmo::physics::set_contacts(manager, entity, in, count, flags, error);
        }
        bool navigation_get_state(const char* entity, bmmo_physics_nav_state& out, std::string& error) const {
            return bmmo::physics::navigation_get_state(manager, entity, out, error);
        }
        bool navigation_set_state(const char* entity, const bmmo_physics_nav_state& state, std::string& error) const {
            return bmmo::physics::navigation_set_state(manager, entity, state, error);
        }
        bool navigation_input(const char* entity, uint8_t keys, const float right[3], const float up[3], const float dir[3],
                              bool active, std::string& error) const {
            return bmmo::physics::navigation_input(manager, entity, keys, right, up, dir, active, error);
        }
        bool navigation_poll(const char* entity, bool enable, const int* key_codes, const uint32_t* key_blocks, int count,
                             std::string& error) const {
            return bmmo::physics::navigation_poll(manager, entity, enable, key_codes, key_blocks, count, error);
        }
        bool savestate_get(std::vector<uint8_t>& out, std::string& error) {
            return bmmo::physics::get_savestate(manager, out, error, &last_report);
        }
        bool savestate_set(const uint8_t* data, size_t size, uint32_t flags, std::string& error,
                           std::vector<std::string>* contacts_dropped = nullptr, std::vector<std::string>* unknown_bodies = nullptr) {
            const bool ok = bmmo::physics::set_savestate(manager, data, size, flags, error, &last_report);
            if (contacts_dropped) *contacts_dropped = last_report.contacts_dropped;
            if (unknown_bodies) *unknown_bodies = last_report.unknown_bodies;
            return ok;
        }
        // v20: a travelling blob (the resync state's server blob,
        // bmmo::physics::savestate_travel); `summary` gets the report's
        // counts and first warnings, as the bridge writes them.
        bool savestate_set_travelling(const uint8_t* data, size_t size, uint32_t flags,
                                      const std::vector<std::pair<std::string, std::string>>& renames,
                                      const std::vector<std::string>& excluded_prefixes, std::string& error,
                                      std::vector<std::string>* contacts_dropped, std::vector<std::string>* unknown_bodies,
                                      std::string* summary) {
            savestate_travel travel;
            travel.renames = renames;
            travel.excluded_prefixes = excluded_prefixes;
            const bool ok = bmmo::physics::set_savestate(manager, data, size, flags, error, &last_report, travel);
            if (contacts_dropped) *contacts_dropped = last_report.contacts_dropped;
            if (unknown_bodies) *unknown_bodies = last_report.unknown_bodies;
            if (summary) {
                *summary = "skipped=" + std::to_string(last_report.skipped) + " excluded=" + std::to_string(last_report.excluded)
                         + " renamed=" + std::to_string(last_report.renamed);
                for (size_t i = 0; i < last_report.warnings.size() && i < 16; ++i) *summary += " | " + last_report.warnings[i];
            }
            return ok;
        }
        bool step_physics(float delta_ms, std::string& error) const { return bmmo::physics::step_physics(manager, delta_ms, error); }
        bool set_event_log_muted(bool muted, std::string&) const {
            bmmo::physics::set_event_log_muted(muted);
            return true;
        }

        // ---- host support (docs/session-client-core-design.md 2.4) ----
        // The rest of physics_view's methods the helpers both hosts share
        // call, under physics_view's names and signatures, so the helpers are
        // templates over either backend.  Added at step E1 with no callers;
        // the headless client keeps its free calls elsewhere (design 7.1-5).
        bool capture(world_hash& out, std::string& error) const {
            return bmmo::physics::capture_world_hash(manager, out, error);
        }
        bool reset_session_clock(int seed, std::string& error) const {
            return bmmo::physics::reset_session_clock(manager, seed, error);
        }
        std::string drain_event_log() const { return bmmo::physics::drain_event_log(manager); }
        // Every physicalized entity, in physics-table order (physics_view::list_bodies).
        std::vector<bmmo_physics_body_state> list_bodies() const {
            std::vector<bmmo_physics_body_state> bodies(64);
            int total = bmmo::physics::list_bodies(manager, bodies.data(), static_cast<int>(bodies.size()));
            if (total > static_cast<int>(bodies.size())) {
                bodies.resize(static_cast<size_t>(total));
                total = bmmo::physics::list_bodies(manager, bodies.data(), static_cast<int>(bodies.size()));
            }
            bodies.resize(total > 0 ? std::min(static_cast<size_t>(total), bodies.size()) : 0);
            return bodies;
        }
        bool force_block_get(uint32_t behavior_id, bmmo_physics_force_block& out, std::string& error) const {
            return bmmo::physics::force_block_get(manager, behavior_id, out, error);
        }
        bool force_block_set(uint32_t behavior_id, const bmmo_physics_force_block& in, std::string& error) const {
            return bmmo::physics::force_block_set(manager, behavior_id, in, error);
        }
        // v27 (step RLC): a joint block's constraint image.
        bool joint_block_get(uint32_t behavior_id, bmmo_physics_joint_image& out, std::string& error) const {
            return bmmo::physics::joint_block_get(manager, behavior_id, out, error);
        }
        bool joint_block_set(uint32_t behavior_id, const bmmo_physics_joint_image& in, std::string& error) const {
            return bmmo::physics::joint_block_set(manager, behavior_id, in, error);
        }
        bool joint_blocks_order(const std::vector<uint32_t>& behavior_ids, const std::vector<int16_t>& orders,
                                uint32_t& moved, std::string& error) const {
            if (orders.size() != 2 * behavior_ids.size()) {
                error = "two places per block";
                return false;
            }
            return bmmo::physics::joint_blocks_order(manager, behavior_ids.data(), orders.data(),
                                                     static_cast<uint32_t>(behavior_ids.size()), moved, error);
        }
        // v23, the PreSimulate queue (rollback-engine-gaps design 2.2.2, gap Q).
        bool callbacks_get(std::vector<bmmo_physics_callback>& out, std::string& error) const {
            return bmmo::physics::callbacks_get(manager, out, error);
        }
        int callbacks_rebuild(const std::vector<uint32_t>& scope, const std::vector<bmmo_physics_callback>& target,
                              std::string& error) const {
            return bmmo::physics::callbacks_rebuild(manager, scope, target, error);
        }
        int callbacks_hold(const std::vector<uint32_t>& executed, std::string& error) const {
            return bmmo::physics::callbacks_hold(manager, executed, error);
        }
        int callbacks_unhold(std::string& error) const { return bmmo::physics::callbacks_unhold(manager, error); }
        bool callback_scope_set(int mode, const std::vector<uint32_t>& scope, std::string& error) const {
            return bmmo::physics::callback_scope_set(manager, mode, scope, error);
        }
        bool set_body_matrix(const char* entity, const float matrix[16], bool simulated, std::string& error) const {
            return bmmo::physics::set_body_matrix(manager, entity, matrix, simulated, error);
        }
        bool physicalize(const char* entity_name, const bmmo_physics_ball_recipe& recipe, const char* collision_group,
                         std::string& error) const {
            return bmmo::physics::physicalize(manager, entity_name, recipe, collision_group, error);
        }
        bool unphysicalize(const char* entity_name, std::string& error) const {
            return bmmo::physics::unphysicalize(manager, entity_name, error);
        }
        bool set_body_group(const char* entity_name, const char* collision_group, std::string& error) const {
            return bmmo::physics::set_body_group(manager, entity_name, collision_group, error);
        }
        bool navigation_create(const char* ball_entity, const char* direction_ref_entity, uint32_t behavior_id,
                               const float (*directions)[3], int leaf_count, float force_value, std::string& error) const {
            return bmmo::physics::navigation_create(manager, ball_entity, direction_ref_entity, behavior_id, directions,
                                                    leaf_count, force_value, error);
        }
        bool navigation_set_ball(const char* ball_entity, const char* new_ball_entity, float force_value,
                                 std::string& error) const {
            return bmmo::physics::navigation_set_ball(manager, ball_entity, new_ball_entity, force_value, error);
        }
        bool navigation_destroy(const char* ball_entity, std::string& error) const {
            return bmmo::physics::navigation_destroy(manager, ball_entity, error);
        }
        bool set_body_guard(bool enable, const char* except_entity, std::string& error) const {
            return bmmo::physics::set_body_guard(manager, enable, except_entity, error);
        }
        bool push_impulse(const char* entity_name, const float direction_ws[3], float speed, uint32_t behavior_id,
                          std::string& error) const {
            return bmmo::physics::push_impulse(manager, entity_name, direction_ws, speed, behavior_id, error);
        }
    };

    // Every movable body's name in the engine's table order, minus the names
    // starting with `skip_prefix` (the clients track their balls by entity, as
    // own and remote, not as mechanisms): the mechanism set a record tracks.
    inline std::vector<std::string> movable_body_names(CKIpionManager* physics, const char* skip_prefix = nullptr) {
        std::vector<bmmo_physics_body_state> bodies(256);
        int count = bmmo::physics::list_bodies(physics, bodies.data(), static_cast<int>(bodies.size()));
        if (count > static_cast<int>(bodies.size())) {
            bodies.resize(static_cast<size_t>(count));
            count = bmmo::physics::list_bodies(physics, bodies.data(), static_cast<int>(bodies.size()));
        }
        std::vector<std::string> names;
        const size_t skip = skip_prefix ? std::strlen(skip_prefix) : 0;
        for (int i = 0; i < count && i < static_cast<int>(bodies.size()); ++i) {
            const auto& body = bodies[static_cast<size_t>(i)];
            if (!body.movable || !body.name[0]) continue;
            if (skip && std::strncmp(body.name, skip_prefix, skip) == 0) continue;
            names.emplace_back(body.name);
        }
        return names;
    }
}
