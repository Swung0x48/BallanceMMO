#pragma once

// The in-process physics backend for session/rollback_binding.hpp: the same
// methods, under the same names, as the retail mod's physics_view
// (BallanceMMOClient/physics/physics_view.hpp), each a call into
// bmmo::physics::* on the CKIpionManager this process owns.  Headless only:
// the headless session client and the SimTool have the engine in the process;
// the mod has only the plugin's C API and uses physics_view.

#include <physics/physics_savestate.hpp>
#include <physics/physics_state.hpp>

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
        bool step_physics(float delta_ms, std::string& error) const { return bmmo::physics::step_physics(manager, delta_ms, error); }
        bool set_event_log_muted(bool muted, std::string&) const {
            bmmo::physics::set_event_log_muted(muted);
            return true;
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
