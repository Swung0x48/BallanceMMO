#pragma once

// Host-side logic written once (step HU1; docs/roadmap.md section 2, the
// user's decision of 2026-10-27, and docs/session-client-core-design.md 2.4):
// the pieces every host builds the same way over its physics backend.  The
// backend is duck-typed, as in session_host_support.hpp: physics_direct (the
// in-process engine: the server's world, the headless client, SimTool) or
// physics_view (the retail mod's bridge), passed by value (physics_direct is
// a handle on the manager) or by pointer (the retail host's view lives as
// long as the session; a copy of it would not follow a re-initialize).  A
// host keeps only what only its platform can do.
//
//   * force_hooks_for: the script replay's force hooks (capture_force /
//     restore_force) over the backend's force_block_get / force_block_set,
//     and its joint hooks (capture_joint / restore_joint, step RLC) over
//     joint_block_get / joint_block_set;
//   * body_names_of: the names of every physicalized entity right now
//     (list_bodies).
//
// Only the public CK2 API and the bridge's C structs: the header compiles in
// the x86 client target (Virtools SDK) and against Ballanced.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>
#include <set>
#include <string>
#include <type_traits>
#include <utility>

#include "CKAll.h"

#include <physics/physics_rt_api.h>

#include "script_replay.hpp"

namespace bmmo::game {
    namespace host_backend_detail {
        // The backend behind a handle: itself, or what a pointer points at.
        template <class B> decltype(auto) backend_of(const B& b) {
            if constexpr (std::is_pointer_v<B>) return (*b);
            else return (b);
        }
    }

    // The force hooks the script replay, the trafo sets and the resync's
    // portable record take (script_replay_hooks): the block's IVP controller
    // through the bridge, in the bridge's struct.
    template <class Backend>
    script_replay_hooks force_hooks_for(Backend backend) {
        script_replay_hooks hooks;
        hooks.capture_force = [backend](CKBehavior* block, force_block_state& out) {
            bmmo_physics_force_block state{};
            std::string error;
            if (!host_backend_detail::backend_of(backend).force_block_get(static_cast<uint32_t>(block->GetID()), state, error))
                return false;
            out.exists = state.exists != 0;
            for (int k = 0; k < 3; ++k) { out.position[k] = state.position[k]; out.force[k] = state.force[k]; }
            return true;
        };
        hooks.restore_force = [backend](CKBehavior* block, const force_block_state& in) {
            bmmo_physics_force_block state{};
            state.exists = in.exists ? 1 : 0;
            for (int k = 0; k < 3; ++k) { state.position[k] = in.position[k]; state.force[k] = in.force[k]; }
            std::string error;
            return host_backend_detail::backend_of(backend).force_block_set(static_cast<uint32_t>(block->GetID()), state, error);
        };
        // Step RLC (bridge API v27): a joint block's constraint image, kept
        // by the script replay's joint ledger and made again at a restore.
        hooks.capture_joint = [backend](CKBehavior* block, std::vector<uint8_t>& image, std::array<int16_t, 2>& order) {
            bmmo_physics_joint_image state{};
            std::string error;
            image.clear();
            order = {-1, -1};
            if (!host_backend_detail::backend_of(backend).joint_block_get(static_cast<uint32_t>(block->GetID()), state, error))
                return false;
            image.assign(state.bytes, state.bytes + std::min<uint32_t>(state.size, BMMO_PHYSICS_JOINT_IMAGE_SIZE));
            order = {state.order[0], state.order[1]};
            return true;
        };
        hooks.restore_joint = [backend](CKBehavior* block, const std::vector<uint8_t>& image) {
            if (image.empty() || image.size() > BMMO_PHYSICS_JOINT_IMAGE_SIZE) return false;
            bmmo_physics_joint_image state{};
            state.size = static_cast<uint32_t>(image.size());
            std::memcpy(state.bytes, image.data(), image.size());
            std::string error;
            return host_backend_detail::backend_of(backend).joint_block_set(static_cast<uint32_t>(block->GetID()), state, error);
        };
        hooks.order_joints = [backend](const std::vector<std::pair<CKBehavior*, std::array<int16_t, 2>>>& blocks,
                                       std::string& error) {
            std::vector<uint32_t> ids;
            std::vector<int16_t> orders;
            for (const auto& [block, order]: blocks) {
                ids.push_back(static_cast<uint32_t>(block->GetID()));
                orders.push_back(order[0]);
                orders.push_back(order[1]);
            }
            uint32_t moved = 0;
            if (!host_backend_detail::backend_of(backend).joint_blocks_order(ids, orders, moved, error)) return -1;
            return static_cast<int>(moved);
        };
        return hooks;
    }

    // Every physicalized entity's name, now.
    template <class Backend>
    std::set<std::string> body_names_of(const Backend& backend) {
        std::set<std::string> names;
        for (const bmmo_physics_body_state& body: host_backend_detail::backend_of(backend).list_bodies()) names.insert(body.name);
        return names;
    }
}
