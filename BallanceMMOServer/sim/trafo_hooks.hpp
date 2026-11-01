#pragma once

// The bridge as the headless hosts reach it for the trafo piece sets
// (game/trafo_sets.hpp trafo_set_hooks; trafo-replication design 2.11): one
// factory for the server's world (physics_world), SimTool (sim/trafo_tool.hpp)
// and the headless client (step T3).  Moved out of trafo_tool.hpp at step T2,
// unchanged but for the log callback; a wrapper over game/host_backend.hpp
// trafo_hooks_for since FULL step AUD3.

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
    // `log`: where the sets' own lines go (null: stdout, SimTool's).  Since
    // FULL step AUD3 (host-platform audit D6) the hooks are host support's
    // one factory (game/host_backend.hpp trafo_hooks_for) over physics_direct,
    // as the retail host's are over physics_view; this only adds SimTool's
    // stdout default.
    inline bmmo::game::trafo_set_hooks make_trafo_hooks(CKIpionManager* physics,
                                                        std::function<void(const std::string&)> log = nullptr) {
        if (!log) log = [](const std::string& line) { std::printf("%s\n", line.c_str()); };
        return bmmo::game::trafo_hooks_for(bmmo::physics::physics_direct(physics), std::move(log));
    }
}
