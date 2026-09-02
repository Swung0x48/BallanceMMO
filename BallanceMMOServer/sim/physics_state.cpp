#include "physics_state.hpp"

#include "CKAll.h"

#include "CKIpionManager.h"
#include "ivp_physics.hxx"
#include "ivp_core.hxx"
#include "ivp_real_object.hxx"
#include "ivp_time.hxx"
#include <physics/ivp_private_access.hpp>

#include <vector>

int ivp_srand_read();

namespace bmmo::sim {
    using namespace bmmo::physics;

    bool capture_world_hash(CKIpionManager* physics, world_hash& out, std::string& error) {
        error.clear();
        out = {};
        IVP_Environment* environment = physics ? physics->GetEnvironment() : nullptr;
        if (!environment) {
            error = "physics environment is unavailable";
            return false;
        }
        environment_state env{};
        env.current_time = environment->get_current_time().get_seconds();
        env.time_of_next_psi = environment->get_next_PSI_time().get_seconds();
        env.time_of_last_psi = environment->get_old_time_of_last_PSI().get_seconds();
        env.next_movement_check = bmmo::physics::ivp_access::next_movement_check(*environment);
        env.ivp_seed = ivp_srand_read();
        env.delta_time_ms = physics->m_DeltaTime;
        env.physics_delta_time = physics->m_PhysicsDeltaTime;
        fnv1a64 hasher, pose_hasher;
        feed_environment(hasher, env);

        std::vector<const IVP_Core*> seen;
        const int count = physics->m_MovableObjects.len();
        for (int i = 0; i < count; ++i) {
            IVP_Real_Object* object = physics->m_MovableObjects.element_at(i);
            const IVP_Core* core = object ? object->get_core() : nullptr;
            if (!core) continue;
            bool duplicate = false;
            for (const auto* known: seen) duplicate |= known == core;
            if (duplicate) continue;
            seen.push_back(core);
            core_state c{};
            for (int k = 0; k < 3; ++k) c.position[k] = core->pos_world_f_core_last_psi.k[k];
            c.q_last_psi[0] = core->q_world_f_core_last_psi.x;
            c.q_last_psi[1] = core->q_world_f_core_last_psi.y;
            c.q_last_psi[2] = core->q_world_f_core_last_psi.z;
            c.q_last_psi[3] = core->q_world_f_core_last_psi.w;
            c.q_next_psi[0] = core->q_world_f_core_next_psi.x;
            c.q_next_psi[1] = core->q_world_f_core_next_psi.y;
            c.q_next_psi[2] = core->q_world_f_core_next_psi.z;
            c.q_next_psi[3] = core->q_world_f_core_next_psi.w;
            for (int k = 0; k < 3; ++k) {
                c.speed[k] = core->speed.k[k];
                c.rot_speed[k] = core->rot_speed.k[k];
                c.speed_change[k] = core->speed_change.k[k];
                c.rot_speed_change[k] = core->rot_speed_change.k[k];
                c.delta_psis[k] = core->delta_world_f_core_psis.k[k];
            }
            c.movement_state = static_cast<uint8_t>(core->movement_state);
            c.i_delta_time = core->i_delta_time;
            c.time_of_last_psi = core->time_of_last_psi.get_seconds();
            feed_core(hasher, c);
            feed_core_pose(pose_hasher, c);
        }
        out.hash = hasher.value;
        out.pose = pose_hasher.value;
        out.cores = static_cast<int>(seen.size());
        out.ivp_time = env.current_time;
        out.ivp_seed = env.ivp_seed;
        out.delta_time_ms = env.delta_time_ms;
        out.physics_delta_time = env.physics_delta_time;
        out.time_factor = physics->m_PhysicsTimeFactor;
        return true;
    }

    bool reset_session_clock(CKIpionManager* physics, int seed, std::string& error) {
        error.clear();
        IVP_Environment* environment = physics ? physics->GetEnvironment() : nullptr;
        if (!environment) {
            error = "physics environment is unavailable";
            return false;
        }
        namespace access = bmmo::physics::ivp_access;
        access::time_manager(*environment)->base_time = IVP_Time(0.0);
        access::current_time(*environment) = IVP_Time(0.0);
        access::time_of_last_psi(*environment) = IVP_Time(0.0);
        access::time_of_next_psi(*environment) = IVP_Time(0.0) + environment->get_delta_PSI_time();
        access::next_movement_check(*environment) = IVP_MOVEMENT_CHECK_COUNT;
        physics->m_DeltaTime = 1000.0f / 66.0f;
        physics->m_PhysicsDeltaTime = (1000.0f / 66.0f) * physics->m_PhysicsTimeFactor;
        ivp_srand(seed == 0 ? 1 : seed);
        return true;
    }
}
