#pragma once

// The bridge as the headless hosts reach it for the trafo piece sets
// (game/trafo_sets.hpp trafo_set_hooks; trafo-replication design 2.11): one
// factory for the server's world (physics_world), SimTool (sim/trafo_tool.hpp)
// and the headless client (step T3).  Moved out of trafo_tool.hpp at step T2,
// unchanged but for the log callback.

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include <game/host_backend.hpp>
#include <game/trafo_sets.hpp>
#include <physics/physics_direct.hpp>
#include <physics/physics_state.hpp>

#include "CKAll.h"
#include "CKIpionManager.h"

namespace bmmo::sim {
    // `log`: where the sets' own lines go (null: stdout, SimTool's).
    inline bmmo::game::trafo_set_hooks make_trafo_hooks(CKIpionManager* physics,
                                                        std::function<void(const std::string&)> log = nullptr) {
        bmmo::game::trafo_set_hooks hooks;
        hooks.has_body = [physics](const std::string& name) {
            CKContext* context = physics ? physics->m_Context : nullptr;
            auto* entity = context ? CK3dEntity::Cast(context->GetObjectByNameAndParentClass(const_cast<CKSTRING>(name.c_str()),
                                                                                              CKCID_3DENTITY, nullptr))
                                   : nullptr;
            PhysicsObject* object = entity ? physics->GetPhysicsObject(entity) : nullptr;
            return object && object->m_RealObject;
        };
        hooks.unphysicalize = [physics](const std::string& name, std::string& error) {
            return bmmo::physics::unphysicalize(physics, name.c_str(), error);
        };
        hooks.restore_originals = [physics]() { bmmo::physics::restore_explosion_pieces(physics->m_Context); };
        hooks.register_set = [physics](uint32_t player, const uint32_t frames[3], const std::vector<uint32_t>& explosions,
                                       bmmo_physics_trafo_set_report& out, std::string& error) {
            return bmmo::physics::trafo_set_register(physics, player, frames, explosions.data(), static_cast<int>(explosions.size()),
                                                     out, error);
        };
        hooks.unregister_set = [physics](uint32_t player, std::string& error) {
            return bmmo::physics::trafo_set_unregister(physics, player, error);
        };
        hooks.seed = [physics](uint32_t frame, int32_t seed) { bmmo::physics::trafo_set_seed(physics, frame, seed); };
        hooks.unmake = [physics](uint32_t frame, std::string& error) { return bmmo::physics::trafo_set_unmake(physics, frame, error); };
        hooks.forces = bmmo::game::force_hooks_for(bmmo::physics::physics_direct(physics));   // step HU1: one copy (game/host_backend.hpp)
        if (log) hooks.log = std::move(log);
        else hooks.log = [](const std::string& line) { std::printf("%s\n", line.c_str()); };
        return hooks;
    }
}
