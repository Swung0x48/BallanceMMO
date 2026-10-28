#pragma once

// The client's window onto the physics world, through the BallanceMMO bridge
// exported by the open-source physics_RT plugin (physics/physics_rt_api.h).
// The mod never touches IVP layouts: everything runs inside the plugin, from
// the same physics_state.cpp the headless server links.  With the retail
// physics_RT.dll there is no bridge and initialize() fails, which is how
// physics sessions get reported as unavailable.

#include <string>
#include <vector>

#include <physics/physics_rt_api.h>
#include <physics/physics_state.hpp>   // bmmo::physics::wake_mode
#include <physics/world_hash.hpp>

class CKContext;

namespace bmmo::physics {
    class physics_view {
    public:
        bool initialize(CKContext* context, std::string& error);
        void shutdown();
        bool available() const { return api_ != nullptr && manager_ != nullptr; }

        // Hash of the IVP world after the last physics step.
        bool capture(world_hash& out, std::string& error) const;
        // Session clock + RNG reset performed at the same tick as the server.
        bool reset_session_clock(int seed, std::string& error) const;
        bool reset_random(int seed, std::string& error) const;
        // Diagnostics: the movable bodies the world simulates right now.
        std::string describe_movable_objects() const;
        std::string describe_physics_objects() const;
        std::string drain_event_log() const;
        std::string describe_cores_exact() const;

        // ---- bridge API v2 (design 8.4) ----

        // Every physicalized entity of the world, in physics-table order.
        std::vector<bmmo_physics_body_state> list_bodies() const;
        bool get_body_state(const char* entity_name, bmmo_physics_body_state& out,
                            std::string& error) const;
        // Beam a body and overwrite its core speeds; `wake` wakes it, otherwise
        // a simulated body is frozen.  Any array may be null to skip that part.
        // `recheck` rechecks the collision pairs at the new pose instead of
        // taking IVP's optimized per-frame path, which generates no collision
        // when the body is beamed into another one: the mechanism snap writes
        // need the contact pair, everyone else wants the default.
        bool set_body_state(const char* entity_name, const double position[3],
                            const double rotation[4], const float linear[3], const float angular[3],
                            bool wake, std::string& error, bool recheck = false) const;
        // ---- bridge API v10 (design 9.25) ----
        // The same write with the sleep state and the contact recheck decided
        // separately: `keep` leaves the body's simulation state (and its freeze
        // timers) alone, which the plain call cannot do - it always calls
        // ensure_in_simulation or disable_simulation, and ensure_in_simulation
        // re-arms the resting detection of a body that was already awake
        // (engine change #14).  A rollback restore writes the pose of most
        // bodies without wanting to touch either, and rechecks the contacts
        // only when the write actually moved the body.
        bool set_body_state(const char* entity_name, const double position[3],
                            const double rotation[4], const float linear[3], const float angular[3],
                            bmmo::physics::wake_mode mode, bool recheck, std::string& error) const;
        // ---- bridge API v11 ----
        // The core's queued impulses, which a body state does not carry and a
        // rollback restore needs: set_body_state zeroes them (design 8.4, right
        // for a hard set), so a replay restore calls set_core_extra straight
        // after the pose write to put the recorded queue back.
        bool get_core_extra(const char* entity_name, bmmo_physics_core_extra& out, std::string& error) const;
        bool set_core_extra(const char* entity_name, const bmmo_physics_core_extra& in, std::string& error) const;
        // ---- bridge API v13 ----
        // The environment's clock: read so a restore can shift the calm
        // references it puts back by the clock offset; written only by the
        // opt-in clock rewind.
        bool get_clock_state(bmmo_physics_clock_state& out, std::string& error) const;
        bool set_clock_state(const bmmo_physics_clock_state& in, std::string& error) const;
        // ---- bridge API v14: the script replay ----
        bool force_block_get(uint32_t behavior_id, bmmo_physics_force_block& out, std::string& error) const;
        bool force_block_set(uint32_t behavior_id, const bmmo_physics_force_block& in, std::string& error) const;
        bool set_event_log_muted(bool muted, std::string& error) const;
        // ---- bridge API v23: the PreSimulate queue (rollback-engine-gaps
        // design 2.2.2, gap Q); the bridge's counts, -1 on an error ----
        bool callbacks_get(std::vector<bmmo_physics_callback>& out, std::string& error) const;
        int callbacks_rebuild(const std::vector<uint32_t>& scope, const std::vector<bmmo_physics_callback>& target,
                              std::string& error) const;
        int callbacks_hold(const std::vector<uint32_t>& executed, std::string& error) const;
        int callbacks_unhold(std::string& error) const;
        bool callback_scope_set(int mode, const std::vector<uint32_t>& scope, std::string& error) const;
        // v15: a body's friction contacts (bmmo_physics_contact); the counts
        // the bridge returns, -1 on an error.
        int contacts_get(const char* entity, bmmo_physics_contact* out, int max, std::string& error) const;
        int contacts_set(const char* entity, const bmmo_physics_contact* in, int count, uint32_t flags, std::string& error) const;
        // v16: rebuild the body's collision pairs against the world as it stands now.
        bool body_recheck(const char* entity, std::string& error) const;
        // v19: the body put where its Physicalize made it (bmmo::physics::set_body_matrix).
        bool set_body_matrix(const char* entity, const float matrix[16], bool simulated, std::string& error) const;
        // ---- bridge API v17: the IVP savestate (docs/ivp-savestate-design.md) ----
        // The engine's whole dynamic state as a local byte blob; never on the
        // wire.  savestate_get replaces `out`; savestate_set takes the
        // BMMO_PHYSICS_SAVESTATE_* flags and, on a false return, leaves the
        // world in no guaranteed state (the caller re-anchors).
        bool savestate_get(std::vector<uint8_t>& out, std::string& error) const;
        // `contacts_dropped` (optional out): the bodies whose friction
        // contacts the blob could not put back; the rollback engine
        // re-creates them from its own record.  `unknown_bodies` (optional
        // out): the live bodies the blob does not know, left out of collision
        // detection for the engine to park or recheck.
        bool savestate_set(const uint8_t* data, size_t size, uint32_t flags, std::string& error,
                           std::vector<std::string>* contacts_dropped = nullptr,
                           std::vector<std::string>* unknown_bodies = nullptr) const;
        // v20: a travelling blob, the resync state's server blob (renames:
        // the own clone to the own ball; excluded: the keep-local prefixes);
        // `summary` gets "skipped=N excluded=N renamed=N" and the first
        // warnings.
        bool savestate_set_travelling(const uint8_t* data, size_t size, uint32_t flags,
                                      const std::vector<std::pair<std::string, std::string>>& renames,
                                      const std::vector<std::string>& excluded_prefixes, std::string& error,
                                      std::vector<std::string>* contacts_dropped, std::vector<std::string>* unknown_bodies,
                                      std::string* summary) const;

        // The retail Physicalize recipe, applied without a behavior graph.
        bool physicalize(const char* entity_name, const bmmo_physics_ball_recipe& recipe,
                         const char* collision_group, std::string& error) const;
        bool unphysicalize(const char* entity_name, std::string& error) const;
        // Idempotent; must be re-run whenever the IVP environment is rebuilt.
        bool install_player_collision_filter(const char* player_group_prefix,
                                             std::string& error) const;
        bool set_body_group(const char* entity_name, const char* collision_group, std::string& error) const;

        // ---- bridge API v3 (design 9.1): navigation for mirrored remote balls ----
        bool navigation_create(const char* ball_entity, const char* direction_ref_entity, uint32_t behavior_id,
                               const float (*directions)[3], int leaf_count, float force_value, std::string& error) const;
        bool navigation_input(const char* ball_entity, uint8_t keys, const float right[3], const float up[3],
                              const float dir[3], bool active, std::string& error) const;
        bool navigation_set_ball(const char* ball_entity, const char* new_ball_entity, float force_value,
                                 std::string& error) const;
        bool navigation_destroy(const char* ball_entity, std::string& error) const;

        // ---- bridge API v4 (design 9.6): rollback ----
        bool step_physics(float delta_ms, std::string& error) const;
        bool navigation_poll(const char* ball_entity, bool enable, const int* key_codes, const uint32_t* key_blocks,
                             int count, std::string& error) const;
        bool navigation_get_state(const char* ball_entity, bmmo_physics_nav_state& out, std::string& error) const;
        bool navigation_set_state(const char* ball_entity, const bmmo_physics_nav_state& state, std::string& error) const;

        // ---- bridge API v5 (engine change #6): body guard ----
        bool set_body_guard(bool enable, const char* except_entity, std::string& error) const;
        bool get_clock(float& time_factor, float& physics_delta, std::string& error) const;

        // ---- bridge API v6 (design 9.10): spawn impulse, deterministic Random block ----
        // Impulse direction_ws * speed * mass at the body's mass centre; applied
        // now, or at this frame's PreSimulate pass when the body does not
        // exist yet (behavior_id anchors the queued callback).
        bool push_impulse(const char* entity_name, const float direction_ws[3], float speed, uint32_t behavior_id,
                          std::string& error) const;
        // Routes the Virtools "Random" block through the bridge's deterministic
        // generator (prototype + existing instances); returns the number of
        // instances patched, -1 without the prototype.
        int install_random_block(std::string& error) const;
        int32_t random_next(std::string& error) const;

        // ---- bridge API v8: session clock guard ----
        // Keeps the retail pause menu from freezing the session clock: the
        // factor is pinned to the value the run had before the menu opened
        // while pause_behavior_id is inactive, and sampled (so the level
        // scripts stay in charge) on every other pass.  behavior_id anchors
        // the callback so it is re-attached after a reset.
        bool set_clock_guard(bool enable, float time_factor, uint32_t behavior_id, uint32_t pause_behavior_id,
                             std::string& error) const;

        // ---- bridge API v21: the trafo sets (trafo-replication design 2.4,
        // 2.6, 2.10; game/trafo_sets.hpp trafo_set_hooks, the retail host's
        // half since step T4) ----
        // Whether the entity has a physics body now.
        bool has_body(const char* entity_name) const;
        bool trafo_set_register(uint32_t player, const uint32_t frames[3], const uint32_t* explosions, int explosion_count,
                                bmmo_physics_trafo_set_report& out, std::string& error) const;
        bool trafo_set_unregister(uint32_t player, std::string& error) const;
        void trafo_set_seed(uint32_t frame_id, int32_t seed) const;
        // The bodies removed, -1 with a message.
        int trafo_set_unmake(uint32_t frame_id, std::string& error) const;
        bool trafo_set_filter(bool enable, std::string& error) const;
        // ---- bridge API v24 (step BALA): the airship zone
        // (physics_state.hpp airship_zone_set / airship_zone_contains).
        bool airship_zone_set(bool enable, const char* own_group, bmmo::physics::airship_zone_box& out,
                              std::string& error) const;
        int airship_zone_contains(const char* entity_name) const;
        // 1 with the box (min xyz, max xyz) once taken, 0 not yet, -1 off.
        int airship_zone_get(double box[6]) const;
        // ---- bridge API v25 (step SPGH): the spawn ghost
        // (physics_state.hpp spawn_ghost_set / spawn_ghost_state).
        bool spawn_ghost_set(bool enable, bool respawns, double radius, bmmo::physics::spawn_ghost_info& out,
                             std::string& error) const;
        // 1 ghost, 0 solid (or off), -1 no body.
        int spawn_ghost_state(const char* entity_name) const;
        // Step SPGH2: a spawn birth of `group` (physics_state.hpp
        // spawn_ghost_birth); false when the rule is off.
        bool spawn_ghost_birth(const char* group) const;
        // ---- bridge API v22 (step T4): the savestate's controller alias
        // (physics_savestate.hpp set_controller_alias); a null `alias`
        // removes it.
        bool set_controller_alias(int32_t (*alias)(void* user, const char* name, char* out, uint32_t out_size), void* user,
                                  std::string& error) const;

        // Identification of the loaded physics module (filled even when the
        // bridge is missing, so the retail DLL can be reported).
        const std::string& dll_sha256() const { return dll_sha256_; }
        const std::string& dll_path() const { return dll_path_; }
        const std::string& build_id() const { return build_id_; }

    private:
        CKContext* context_ = nullptr;
        void* manager_ = nullptr;
        const bmmo_physics_api_v2* api_ = nullptr;
        std::string dll_sha256_;
        std::string dll_path_;
        std::string build_id_;
        // The size of the last blob savestate_get fetched: the next fetch
        // starts from it, so the common case is one capture instead of a size
        // query plus a fetch.
        mutable uint32_t savestate_size_hint_ = 0;
    };
}
