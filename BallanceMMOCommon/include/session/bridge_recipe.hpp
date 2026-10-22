#pragma once

// A ball recipe as the wire carries it (entity/session.hpp ball_recipe) in the
// physics bridge's C shape (physics/physics_rt_api.h bmmo_physics_ball_recipe):
// what physicalize() takes and what a journal EVENT record stores.  CK-free and
// header-only, so the shared session-client core can journal a lifecycle event
// (session/client_journal.hpp) without the physics library.  Moved verbatim
// from the headless client (session_client.cpp) at step E2 of
// docs/session-client-core-design.md; the retail mod's own copy
// (physics_session_client.cpp) goes when it hosts the core (R0).

#include "../entity/session.hpp"
#include "../physics/physics_rt_api.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace bmmo::session {

    namespace bridge_recipe_detail {
        inline void copy_name(char* dst, size_t size, const std::string& src) {
            std::snprintf(dst, size, "%s", src.c_str());
        }
    }

    inline bmmo_physics_ball_recipe to_bridge_recipe(const ball_recipe& r) {
        using bridge_recipe_detail::copy_name;
        bmmo_physics_ball_recipe p{};
        p.fixed = r.fixed; p.start_frozen = r.start_frozen; p.enable_collision = r.enable_collision;
        p.calc_mass_center = r.calc_mass_center;
        p.friction = r.friction; p.elasticity = r.elasticity; p.mass = r.mass;
        p.linear_damp = r.linear_damp; p.rot_damp = r.rot_damp;
        for (int k = 0; k < 3; ++k) p.mass_center[k] = r.mass_center[k];
        copy_name(p.collision_surface, sizeof(p.collision_surface), r.collision_surface);
        p.convex_count = static_cast<int32_t>(std::min<size_t>(r.convex_meshes.size(), BMMO_PHYSICS_MAX_CONVEX));
        for (int i = 0; i < p.convex_count; ++i) copy_name(p.convex[i], sizeof(p.convex[i]), r.convex_meshes[i]);
        p.ball_count = static_cast<int32_t>(std::min<size_t>(r.balls.size(), BMMO_PHYSICS_MAX_BALLS));
        for (int i = 0; i < p.ball_count; ++i) {
            for (int k = 0; k < 3; ++k) p.ball_center[i][k] = r.balls[i].center[k];
            p.ball_radius[i] = r.balls[i].radius;
        }
        p.concave_count = static_cast<int32_t>(std::min<size_t>(r.concave_meshes.size(), BMMO_PHYSICS_MAX_CONCAVE));
        for (int i = 0; i < p.concave_count; ++i) copy_name(p.concave[i], sizeof(p.concave[i]), r.concave_meshes[i]);
        return p;
    }

}
