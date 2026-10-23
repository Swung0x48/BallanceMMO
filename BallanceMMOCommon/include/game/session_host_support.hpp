#pragma once

// Host support, the CK half (docs/session-client-core-design.md 2.4; step
// E9): platform code both hosts of the shared session-client core need
// identically, templated on a duck-typed physics backend (physics_direct on
// the headless client, physics_view on the retail mod).  The CK-free half -
// the local birth log, the event log's formats and bind_session_world - is
// session/session_host_world.hpp, which this header includes.
//
//   * matrix_from_pose: an entity's world matrix from a Physicalize's pose;
//   * make_mirror_body: a mirror's body posed, physicalized and kicked
//     (platform make_mirror);
//   * navigation_leaves, attach_mirror_navigation, attach_own_replica: the
//     shared navigation for a mirror, and the own ball's replica (design
//     9.6);
//   * manager_activations: every activation of Gameplay_SectorManager the
//     live frames made, recorded for the replay to re-issue (plan doc 10);
//   * live_cells_guard: the three data-array cells kept across a rollback
//     (platform keep_live_cells);
//   * put_back_birth: a destroyed mechanism body's entity put back where
//     its live Physicalize found it (bind_session_world's put_back).
//
// Moved from the headless client (session_client.cpp at 0ea2e05) at E9,
// verbatim but for the backend's methods in place of the free calls; used
// by the headless client only until the retail host (R steps).

#include <session/client_core.hpp>
#include <session/session_host_world.hpp>
#include <session/spawn_impulse.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>

#include "CKAll.h"

#include "navigation_graph.hpp"

namespace bmmo::game {

    inline VxMatrix matrix_from_pose(const float position[3], const float rotation[9]) {
        VxMatrix matrix;
        matrix.SetIdentity();
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k) matrix[r][k] = rotation[r * 3 + k];
        matrix[3][0] = position[0];
        matrix[3][1] = position[1];
        matrix[3][2] = position[2];
        matrix[3][3] = 1.0f;
        return matrix;
    }

    inline CKDataArray* session_data_array(CKContext* context, const char* name) {
        return CKDataArray::Cast(context->GetObjectByNameAndClass(const_cast<CKSTRING>(name), CKCID_DATAARRAY, nullptr));
    }

    // A mirror's body (platform make_mirror): the entity posed as the
    // birth says, physicalized with its recipe in the player's group, and
    // kicked with the spawn impulse the spec names.
    template <class Backend>
    bmmo::session::mirror_made make_mirror_body(Backend& backend, CK3dEntity* entity, const bmmo::session::mirror_spec& spec) {
        bmmo::session::mirror_made made;
        entity->SetWorldMatrix(matrix_from_pose(spec.position, spec.rotation));
        if (!backend.physicalize(entity->GetName(), spec.recipe, spec.group.c_str(), made.error))
            return made;
        made.made = true;
        if (spec.kick)
            made.kicked = backend.push_impulse(entity->GetName(), bmmo::session::kSpawnDirectionTable[spec.kick_index],
                                               spec.kick_strength, 0, made.kick_error);
        return made;
    }

    // The level's navigation leaves by leaf index: their directions and,
    // for the own replica, the key codes and Key Event blocks it polls.
    // Returns the leaf count.
    inline int navigation_leaves(const navigation_graph& graph, float directions[8][3], int* key_codes = nullptr,
                                 uint32_t* key_blocks = nullptr) {
        int count = 0;
        for (const auto& leaf: graph.leaves) {
            if (leaf.index < 0 || leaf.index >= 8) continue;
            directions[leaf.index][0] = leaf.direction.x;
            directions[leaf.index][1] = leaf.direction.y;
            directions[leaf.index][2] = leaf.direction.z;
            if (key_codes) key_codes[leaf.index] = leaf.key;
            if (key_blocks) key_blocks[leaf.index] = leaf.key_block;
            count = std::max(count, leaf.index + 1);
        }
        return count;
    }

    // A mirror's navigation (platform attach_mirror_navigation): the shared
    // navigation with the level's leaf directions, the ball row's force and
    // the player's camera reference.  Unavailable while the graph is not
    // valid (the host checks first that it is known).
    template <class Backend>
    bmmo::session::mirror_navigation attach_mirror_navigation(Backend& backend, const navigation_graph& graph,
                                                              const std::string& entity, uint32_t player, float force,
                                                              std::string& error) {
        if (!graph.valid()) return bmmo::session::mirror_navigation::unavailable;
        float directions[8][3] = {};
        const int count = navigation_leaves(graph, directions);
        const std::string cam_ref = "CamRef_BMMO_" + std::to_string(player);
        if (backend.navigation_create(entity.c_str(), cam_ref.c_str(), graph.ball_navigation, directions, count, force,
                                      error))
            return bmmo::session::mirror_navigation::attached;
        return bmmo::session::mirror_navigation::failed;
    }

    // The own ball's navigation replica (design 9.6): the own ball driven
    // by the replica in polling mode.  `entity` is the name it was last
    // attached to, kept after it is gone (platform own_replica_name).
    struct own_replica {
        bool active = false;
        std::string entity;
        int key_codes[8] = {};
        uint32_t key_blocks[8] = {};
        int key_count = 0;
    };
    enum class own_replica_attach : uint8_t { unavailable, moved, created, failed };

    // The replica moved to `ball_name` when one exists (the old one
    // destroyed if that fails), else created on it, polling the leaves'
    // keys; `failed` with `error` when the create or the polling failed.
    template <class Backend>
    own_replica_attach attach_own_replica(Backend& backend, const navigation_graph& graph, own_replica& replica,
                                          const std::string& ball_name, float force, std::string& error) {
        if (!graph.valid() || ball_name.empty()) return own_replica_attach::unavailable;
        if (replica.active && replica.entity != ball_name) {
            if (backend.navigation_set_ball(replica.entity.c_str(), ball_name.c_str(), force, error)) {
                replica.entity = ball_name;
                return own_replica_attach::moved;
            }
            backend.navigation_destroy(replica.entity.c_str(), error);
            replica.active = false;
        }
        float directions[8][3] = {};
        const int count = navigation_leaves(graph, directions, replica.key_codes, replica.key_blocks);
        replica.key_count = count;
        if (!backend.navigation_create(ball_name.c_str(), "CamRef_BMMO_self", graph.ball_navigation, directions, count,
                                       force, error)
                || !backend.navigation_poll(ball_name.c_str(), true, replica.key_codes, replica.key_blocks, count, error))
            return own_replica_attach::failed;
        replica.active = true;
        replica.entity = ball_name;
        return own_replica_attach::created;
    }

    // Every activation of Gameplay_SectorManager the live frames made, by
    // the tick whose frame consumed it: the union's own passes (before the
    // frame) and the ones scripts outside the replay asked for (Init
    // Ingame at the level start, Event_handler), seen as next-frame flags
    // after their frame.  The replay re-issues them at that tick, before
    // the tick's scripts, with the two IngameParameter cells as they were
    // (plan doc 10).
    struct manager_activation {
        bool reset = false;
        int activate = 0, deactivate = 0;   // IngameParameter[0][1], [0][2]
    };
    class manager_activations {
    public:
        void clear() { by_tick_.clear(); }
        void record(CKContext* context, uint32_t tick, bool reset) {
            manager_activation a;
            a.reset = reset;
            if (CKDataArray* parameters = session_data_array(context, "IngameParameter")) {
                parameters->GetElementValue(0, 1, &a.activate);
                parameters->GetElementValue(0, 2, &a.deactivate);
            }
            by_tick_[tick] = a;
            while (!by_tick_.empty() && by_tick_.begin()->first + 4096 < tick) by_tick_.erase(by_tick_.begin());
        }
        // The activation recorded for `tick` re-issued (the cells, then the
        // manager activated, reset as recorded); nullptr when none was
        // recorded or the array or the manager is missing.
        const manager_activation* replay(CKContext* context, CK_ID manager_id, uint32_t tick) {
            const auto it = by_tick_.find(tick);
            if (it == by_tick_.end()) return nullptr;
            CKDataArray* parameters = session_data_array(context, "IngameParameter");
            CKBehavior* manager = manager_id ? CKBehavior::Cast(context->GetObject(manager_id)) : nullptr;
            if (!parameters || !manager) return nullptr;
            parameters->SetElementValue(0, 1, &it->second.activate, sizeof(int));
            parameters->SetElementValue(0, 2, &it->second.deactivate, sizeof(int));
            if (CKScene* scene = context->GetCurrentScene()) scene->Activate(manager, it->second.reset ? TRUE : FALSE);
            return &it->second;
        }

    private:
        std::map<uint32_t, manager_activation> by_tick_;
    };

    // The three data-array cells the replay rewinds and re-writes (the
    // record's, then the replayed activations') while the LIVE chains that
    // wrote them - Gameplay_Events' crossing, the death chain - are not
    // replayed: kept across a rewind, so the live world ends the frame
    // with the values its own chains left, and only the replay sees the
    // anchor's (the next live pass writes them anyway; this keeps the
    // engine's end-of-replay script check honest about them).  `keep`
    // false (--park-births): nothing kept.  A rollback in its scope
    // re-issues, at its end, the activation of the frame about to run
    // (`reissue`).
    class live_cells_guard {
    public:
        live_cells_guard(CKContext* context, const bmmo::session::session_client_core& core, bool keep,
                         std::function<void(uint32_t tick)> reissue)
            : context_(context), core_(core), rollbacks_before_(core.rollback_stats().rollbacks), reissue_(std::move(reissue)) {
            if (!keep) return;
            for (int i = 0; i < 3; ++i) have_[i] = read_cell(i, values_[i]);
        }
        ~live_cells_guard() {
            for (int i = 0; i < 3; ++i) if (have_[i]) write_cell(i, values_[i]);
            // A rewind that ended at `now` replaced the live world with the
            // replayed one, which never saw an activation a script outside
            // the replay asked for during the live frame `now` (Init Ingame
            // at the level start, Event_handler): pending as next-frame
            // flags in the live record of `now`, absent from the replayed
            // state.  Re-issued for the frame about to run, as the replay
            // re-issues them inside its window.
            if (core_.rollback_stats().rollbacks != rollbacks_before_ && reissue_) reissue_(core_.current_tick() + 1);
        }
        live_cells_guard(const live_cells_guard&) = delete;
        live_cells_guard& operator=(const live_cells_guard&) = delete;

    private:
        static const char* cell_array(int i) { return i == 0 ? "CurrentLevel" : "IngameParameter"; }
        static int cell_column(int i) { return i == 0 ? 4 : i; }
        bool read_cell(int i, int& value) {
            CKDataArray* a = session_data_array(context_, cell_array(i));
            return a && a->GetElementValue(0, cell_column(i), &value);
        }
        void write_cell(int i, int value) {
            if (CKDataArray* a = session_data_array(context_, cell_array(i))) a->SetElementValue(0, cell_column(i), &value, sizeof(int));
        }

        CKContext* context_;
        const bmmo::session::session_client_core& core_;
        uint64_t rollbacks_before_ = 0;
        std::function<void(uint32_t tick)> reissue_;
        int values_[3] = {};
        bool have_[3] = {};
    };

    // A mechanism body destroyed at a restore (bind_session_world's
    // put_back): its entity back at the matrix its live Physicalize found -
    // the LOCAL one for a parented entity (the replayed manager's Set World
    // Matrix on the parent then recomputes the same world matrix), the
    // world one otherwise.  (The scene's initial conditions are NOT it:
    // they are the level file's poses, before the manager placed the
    // module - measured: a box 900 m off, the ball fell to its death.)
    inline void put_back_birth(CKContext* context, const std::string& entity, const bmmo::session::local_birth& birth) {
        if (auto* ck = CK3dEntity::Cast(context->GetObjectByNameAndParentClass(const_cast<CKSTRING>(entity.c_str()),
                                                                              CKCID_3DENTITY, nullptr))) {
            VxMatrix matrix;
            const bool local = birth.has_parent && birth.has_local && ck->GetParent();
            const float* source = local ? birth.local_matrix : birth.matrix;
            for (int r = 0; r < 4; ++r)
                for (int k = 0; k < 4; ++k) matrix[r][k] = source[r * 4 + k];
            if (local) ck->SetLocalMatrix(matrix);
            else ck->SetWorldMatrix(matrix);
        }
    }

}
