#include <physics/physics_state.hpp>

#include "CKAll.h"

#include "CKIpionManager.h"
#include "PhysicsCallback.h"
#include "ivp_physics.hxx"
#include "ivp_core.hxx"
#include "ivp_real_object.hxx"
#include "ivp_time.hxx"
#include <physics/ivp_private_access.hpp>
#include "ivp_listener_object.hxx"
#include "ivp_sim_unit.hxx"
#include "ivp_debug_manager.hxx"
#include "ivp_surface_manager.hxx"
#include "ivp_surman_polygon.hxx"
#include "ivp_compact_surface.hxx"

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>

int ivp_srand_read();

namespace bmmo::physics {

    // Sum of per-body signatures (name + compact surface bytes, or the
    // surface type for balls), so the result does not depend on table order.
    uint64_t surface_signature(CKIpionManager* physics) {
        uint64_t total = 0;
        for (auto it = physics->m_PhysicsObjects.Begin(); it != physics->m_PhysicsObjects.End(); ++it) {
            IVP_Real_Object* real = (*it).m_RealObject;
            if (!real) continue;
            fnv1a64 hasher;
            const char* name = real->get_name() ? real->get_name() : "";
            hasher.feed(name, std::strlen(name));
            if (IVP_SurfaceManager* manager = real->get_surface_manager()) {
                const int type = static_cast<int>(manager->get_type());
                hasher.feed(type);
                if (manager->get_type() == IVP_SURMAN_POLYGON) {
                    const IVP_Compact_Surface* surface =
                        static_cast<IVP_SurfaceManager_Polygon*>(manager)->get_compact_surface();
                    if (surface) hasher.feed(surface, static_cast<size_t>(surface->get_size()));
                }
            }
            total += hasher.value * 0x9E3779B97F4A7C15ULL;
        }
        return total;
    }

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
        const IVP_Real_Object* probe = nullptr;
        const int count = physics->m_MovableObjects.len();
        for (int i = 0; i < count; ++i) {
            IVP_Real_Object* object = physics->m_MovableObjects.element_at(i);
            const IVP_Core* core = object ? object->get_core() : nullptr;
            if (!core) continue;
            bool duplicate = false;
            for (const auto* known: seen) duplicate |= known == core;
            if (duplicate) continue;
            seen.push_back(core);
            const char* object_name = object->get_name() ? object->get_name() : "";
            if (!probe || (std::strncmp(object_name, "Ball_", 5) == 0
                           && std::strncmp(probe->get_name() ? probe->get_name() : "", "Ball_", 5) != 0))
                probe = object;
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
        out.surfaces = surface_signature(physics);
        if (probe) {
            const IVP_Core* core = probe->get_core();
            std::snprintf(out.probe_name, sizeof(out.probe_name), "%s", probe->get_name() ? probe->get_name() : "");
            for (int k = 0; k < 3; ++k) {
                out.probe_position[k] = core->pos_world_f_core_last_psi.k[k];
                out.probe_speed[k] = core->speed.k[k];
                out.probe_rot_speed[k] = core->rot_speed.k[k];
            }
        }
        out.cores = static_cast<int>(seen.size());
        out.ivp_time = env.current_time;
        out.ivp_seed = env.ivp_seed;
        out.next_movement_check = env.next_movement_check;
        out.time_of_last_psi = env.time_of_last_psi;
        out.time_of_next_psi = env.time_of_next_psi;
        out.delta_time_ms = env.delta_time_ms;
        out.physics_delta_time = env.physics_delta_time;
        out.time_factor = physics->m_PhysicsTimeFactor;
        return true;
    }

    std::string describe_movable_objects(CKIpionManager* physics) {
        std::string out;
        if (!physics) return out;
        const int count = physics->m_MovableObjects.len();
        for (int i = 0; i < count; ++i) {
            IVP_Real_Object* object = physics->m_MovableObjects.element_at(i);
            if (i) out += ';';
            if (!object) {
                out += "<null>";
                continue;
            }
            out += object->get_name() ? object->get_name() : "<unnamed>";
            if (const IVP_Core* core = object->get_core()) {
                out += "[" + std::to_string(static_cast<int>(core->movement_state));
                // the simulation unit's type and size (a unit that is not
                // simulated does not integrate its cores)
                if (IVP_Simulation_Unit* unit = core->sim_unit_of_core)
                    out += "/" + std::to_string(static_cast<int>(unit->get_unit_movement_type())) + ":"
                        + std::to_string(unit->sim_unit_cores.len());
                out += "]";
            }
        }
        return out;
    }

    std::string describe_physics_objects(CKIpionManager* physics) {
        std::string out;
        if (!physics) return out;
        char text[160];
        unsigned fpu = 0;
#if defined(_MSC_VER)
        fpu = _controlfp(0, 0);
#endif
        std::snprintf(text, sizeof(text), "calls=%d/%d fpu=%08x:", physics->m_PhysicalizeCalls,
                      physics->m_DePhysicalizeCalls, fpu);
        out += text;
        int listed = 0;
        for (auto it = physics->m_PhysicsObjects.Begin(); it != physics->m_PhysicsObjects.End(); ++it) {
            IVP_Real_Object* real = (*it).m_RealObject;
            auto* entity = real ? static_cast<CK3dEntity*>(real->client_data) : nullptr;
            if (listed++) out += ';';
            out += entity && entity->GetName() ? entity->GetName()
                 : (real && real->get_name() ? real->get_name() : "<unnamed>");
            const IVP_Core* core = real ? real->get_core() : nullptr;
            if (!core) {
                out += "[-]";
                continue;
            }
            std::snprintf(text, sizeof(text), "[%d](%.4f,%.4f,%.4f)", static_cast<int>(core->movement_state),
                          core->pos_world_f_core_last_psi.k[0], core->pos_world_f_core_last_psi.k[1],
                          core->pos_world_f_core_last_psi.k[2]);
            out += text;
        }
        return out;
    }

    namespace {
        class event_log_listener : public IVP_Listener_Object {
        public:
            std::string text;
            IVP_Environment* environment = nullptr;

            void record(const char* kind, IVP_Event_Object* event) {
                if (!event || text.size() > 256 * 1024) return;
                char stamp[64];
                std::snprintf(stamp, sizeof(stamp), "t=%.6f ", event->environment
                    ? event->environment->get_current_time().get_seconds() : -1.0);
                text += stamp;
                text += kind;
                text += ' ';
                text += event->real_object && event->real_object->get_name() ? event->real_object->get_name() : "?";
                text += ';';
            }
            void event_object_deleted(IVP_Event_Object* event) override { record("deleted", event); }
            void event_object_created(IVP_Event_Object* event) override {
                record("created", event);
                // Diagnostics: BMMO_SIM_ALLOC_PERTURB=1 leaks a pseudo-random block after
                // every body creation so later heap addresses land elsewhere; any change
                // in the physics then proves a dependence on pointer values.
                static const bool perturb = std::getenv("BMMO_SIM_ALLOC_PERTURB") != nullptr;
                if (perturb) {
                    static unsigned state = 12345u;
                    state = state * 1103515245u + 12345u;
                    static void* keep[4096];
                    static int n = 0;
                    if (n < 4096) keep[n++] = std::malloc(16 + (state >> 16) % 3000);
                }
            }
            void event_object_revived(IVP_Event_Object* event) override { record("revived", event); }
            void event_object_frozen(IVP_Event_Object* event) override { record("frozen", event); }
            void event_environment_deleted(IVP_Environment*) override { environment = nullptr; }
        };
        // Never destroyed: the IVP environment may outlive static storage at
        // process exit and would call into a dead listener.
        event_log_listener& g_event_log = *new event_log_listener();
    }

    std::string drain_event_log(CKIpionManager* physics) {
        IVP_Environment* environment = physics ? physics->GetEnvironment() : nullptr;
        if (environment && g_event_log.environment != environment) {
            environment->add_listener_object_global(&g_event_log);
            g_event_log.environment = environment;
            g_event_log.text += "listener installed;";
        }
        std::string out;
        out.swap(g_event_log.text);
        return out;
    }

    std::string describe_cores_exact(CKIpionManager* physics) {
        std::vector<std::string> lines;
        if (!physics) return {};
        const int count = physics->m_MovableObjects.len();
        char text[1024];
        for (int i = 0; i < count; ++i) {
            IVP_Real_Object* object = physics->m_MovableObjects.element_at(i);
            const IVP_Core* core = object ? object->get_core() : nullptr;
            if (!core) continue;
            const auto& p = core->pos_world_f_core_last_psi;
            const auto& ql = core->q_world_f_core_last_psi;
            const auto& qn = core->q_world_f_core_next_psi;
            std::snprintf(text, sizeof(text),
                "%s st=%d pos=%a,%a,%a ql=%a,%a,%a,%a qn=%a,%a,%a,%a v=%a,%a,%a w=%a,%a,%a dv=%a,%a,%a dw=%a,%a,%a"
                " dpsi=%a,%a,%a idt=%a tlast=%a",
                object->get_name() ? object->get_name() : "?", static_cast<int>(core->movement_state),
                p.k[0], p.k[1], p.k[2], ql.x, ql.y, ql.z, ql.w, qn.x, qn.y, qn.z, qn.w,
                core->speed.k[0], core->speed.k[1], core->speed.k[2],
                core->rot_speed.k[0], core->rot_speed.k[1], core->rot_speed.k[2],
                core->speed_change.k[0], core->speed_change.k[1], core->speed_change.k[2],
                core->rot_speed_change.k[0], core->rot_speed_change.k[1], core->rot_speed_change.k[2],
                core->delta_world_f_core_psis.k[0], core->delta_world_f_core_psis.k[1],
                core->delta_world_f_core_psis.k[2], static_cast<double>(core->i_delta_time),
                core->time_of_last_psi.get_seconds());
            lines.emplace_back(text);
        }
        std::sort(lines.begin(), lines.end());
        std::string out;
        for (const auto& line: lines) {
            out += line;
            out += '\n';
        }
        return out;
    }

    bool set_impact_trace(CKIpionManager* physics, const char* path, std::string& error) {
        error.clear();
        IVP_Environment* environment = physics ? physics->GetEnvironment() : nullptr;
        IVP_Debug_Manager* debug = environment ? environment->get_debug_manager() : nullptr;
        if (!debug) {
            error = "physics environment is unavailable";
            return false;
        }
        if (debug->out_deb_file && debug->out_deb_file != stdout) {
            std::fflush(debug->out_deb_file);
            std::fclose(debug->out_deb_file);
        }
        debug->out_deb_file = nullptr;
        debug->file_out_impacts = IVP_FALSE;
        if (!path || !*path) return true;
        debug->out_deb_file = std::fopen(path, "a");
        if (!debug->out_deb_file) {
            error = std::string("cannot open ") + path;
            return false;
        }
        debug->file_out_impacts = IVP_TRUE;
        return true;
    }

    bool step_physics(CKIpionManager* physics, float delta_ms, std::string& error) {
        error.clear();
        if (!physics || !physics->GetEnvironment()) {
            error = "no physics environment";
            return false;
        }
        physics->Simulate(delta_ms);
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
        // Fresh-world time factor (CKIpionManager::Reset): a world that already
        // ran the level keeps the 2.0 the Gameplay script set, so without this
        // the first tick after the anchor runs one PSI on one side and two on
        // the other and the IVP clocks stay 1/66 s apart for the whole session.
        physics->m_PhysicsTimeFactor = 0.001f;
        physics->m_DeltaTime = 1000.0f / 66.0f;
        physics->m_PhysicsDeltaTime = (1000.0f / 66.0f) * physics->m_PhysicsTimeFactor;
        ivp_srand(seed == 0 ? 1 : seed);
        // The generator behind the hooked Random blocks (design 9.10): the
        // trafo explosion pieces draw from it, so it starts from the same seed
        // at the same tick on every side - and the hook itself is (re)checked
        // here, the one point every session and every recording passes.
        random_reset(seed);
        install_random_block(physics->m_Context);
        // The pieces themselves start from the file's initial conditions on
        // every side, not from wherever the start-up frames dropped them.
        restore_explosion_pieces(physics->m_Context);
        return true;
    }

    // ---- bridge API v2 (design 8.4) ----

    namespace {
        // Names arrive from the network inside fixed-size fields: never assume
        // they are terminated.
        std::string bounded(const char* text, size_t size) {
            if (!text) return {};
            const char* end = static_cast<const char*>(std::memchr(text, '\0', size));
            return std::string(text, end ? static_cast<size_t>(end - text) : size);
        }

        CK3dEntity* find_entity(CKIpionManager* physics, const std::string& name) {
            if (!physics || !physics->m_Context || name.empty()) return nullptr;
            // ParentClass: balls are CK3dObjects, frames plain CK3dEntities.
            return CK3dEntity::Cast(physics->m_Context->GetObjectByNameAndParentClass(
                const_cast<CKSTRING>(name.c_str()), CKCID_3DENTITY, nullptr));
        }

        CKMesh* find_mesh(CKIpionManager* physics, const std::string& name) {
            if (!physics || !physics->m_Context || name.empty()) return nullptr;
            return CKMesh::Cast(physics->m_Context->GetObjectByNameAndClass(
                const_cast<CKSTRING>(name.c_str()), CKCID_MESH, nullptr));
        }

        int clamp_count(int32_t value, int limit) {
            if (value <= 0) return 0;
            return value > limit ? limit : static_cast<int>(value);
        }

        // IVP_U_Point is a double or a float triple depending on the IVP build
        // flags; write through the member's own type so neither is truncated
        // by an overload choice.
        template <class Point>
        void store3(Point& point, const double* source) {
            for (int k = 0; k < 3; ++k) point.k[k] = static_cast<decltype(+point.k[0])>(source[k]);
        }

        void fill_body_state(IVP_Real_Object* real, body_state& out) {
            out = body_state{};
            auto* entity = static_cast<CK3dEntity*>(real->client_data);
            const char* name = entity && entity->GetName() ? entity->GetName()
                             : (real->get_name() ? real->get_name() : "");
            std::snprintf(out.name, sizeof(out.name), "%s", name);
            const IVP_Core* core = real->get_core();
            out.movable = core && !core->physical_unmoveable;
            out.simulated = core && IVP_MTIS_SIMULATED(core->movement_state);
            out.collision_enabled = real->is_collision_detection_enabled() != IVP_FALSE;
            out.movement_state = core ? static_cast<uint8_t>(core->movement_state) : uint8_t{0};
            if (IVP_Environment* environment = real->get_environment()) {
                // After a tick the current time sits on a PSI boundary, so this
                // is the last-PSI object pose the renderer also sees.
                IVP_U_Quat rotation;
                IVP_U_Point position;
                real->calc_at_quaternion(environment->get_current_time(), &rotation, &position);
                for (int k = 0; k < 3; ++k) out.position[k] = position.k[k];
                out.rotation[0] = rotation.x;
                out.rotation[1] = rotation.y;
                out.rotation[2] = rotation.z;
                out.rotation[3] = rotation.w;
            }
            if (core) {
                for (int k = 0; k < 3; ++k) {
                    out.linear[k] = core->speed.k[k];
                    out.angular[k] = core->rot_speed.k[k];
                }
            }
        }

        // The BMMO player filter of design 8.2.  Player balls share the level's
        // collision behaviour against everything except the retail "Ball"
        // group, which they pass through exactly like the original ball does,
        // while still colliding with each other.
        class player_collision_filter : public IVP_Collision_Filter {
        public:
            IVP_Environment* environment = nullptr;
            char prefix[IVP_NO_COLL_GROUP_STRING_LEN] = {};

            bool is_player(const IVP_Real_Object* object) const {
                const char* ident = object ? object->nocoll_group_ident : nullptr;
                if (!ident || !ident[0] || !prefix[0]) return false;
                return std::strncmp(ident, prefix, std::strlen(prefix)) == 0;
            }

            // Player balls always collide with each other, even when they
            // spawn at the same point (design 9.10): IVP leaves a pair that
            // is moving apart alone (a coincident pair has a zero contact
            // normal and never schedules an event, no NaN), and a pair that
            // comes back together while still overlapping is separated by
            // the impact solver's rescue push.  A distance gate that made
            // overlapping player balls intangible was tried and dropped: the
            // filter is only re-asked when the OV tree re-inserts an object,
            // so the pair stayed intangible and rested inside each other.
            IVP_BOOL check_objects_for_collision_detection(IVP_Real_Object* object0,
                                                           IVP_Real_Object* object1) override {
                const bool player0 = is_player(object0);
                const bool player1 = is_player(object1);
                if (player0 && player1) return IVP_TRUE;
                if (player0 != player1) {
                    const IVP_Real_Object* other = player0 ? object1 : object0;
                    if (other && std::strncmp(other->nocoll_group_ident, "Ball",
                                              IVP_NO_COLL_GROUP_STRING_LEN) == 0)
                        return IVP_FALSE;
                }
                return IVP_TRUE;
            }

            void environment_will_be_deleted(IVP_Environment* env) override;
        };

        // Never destroyed: an IVP environment may outlive static storage at
        // process exit and would then walk a dead registry.
        std::vector<player_collision_filter*>& installed_filters() {
            static auto* filters = new std::vector<player_collision_filter*>();
            return *filters;
        }

        void player_collision_filter::environment_will_be_deleted(IVP_Environment*) {
            auto& filters = installed_filters();
            filters.erase(std::remove(filters.begin(), filters.end(), this), filters.end());
            // IVP_Meta_Collision_Filter only forgets its sub filters (see
            // ivp_collision_filter.cxx); every one of them owns itself.
            delete this;
        }
    }

    int list_bodies(CKIpionManager* physics, body_state* out, int max) {
        if (!physics) return 0;
        int total = 0;
        for (auto it = physics->m_PhysicsObjects.Begin(); it != physics->m_PhysicsObjects.End(); ++it) {
            IVP_Real_Object* real = (*it).m_RealObject;
            if (!real) continue;
            if (out && total < max) fill_body_state(real, out[total]);
            ++total;
        }
        return total;
    }

    bool get_body_state(CKIpionManager* physics, const char* entity_name, body_state& out,
                        std::string& error) {
        error.clear();
        out = body_state{};
        const std::string name = bounded(entity_name, BMMO_PHYSICS_NAME_SIZE);
        CK3dEntity* entity = find_entity(physics, name);
        if (!entity) {
            error = "no 3D entity named '" + name + "'";
            return false;
        }
        PhysicsObject* object = physics->GetPhysicsObject(entity);
        if (!object || !object->m_RealObject) {
            error = "'" + name + "' is not physicalized";
            return false;
        }
        fill_body_state(object->m_RealObject, out);
        return true;
    }

    bool set_body_state(CKIpionManager* physics, const char* entity_name,
                        const double position[3], const double rotation[4],
                        const float linear[3], const float angular[3], bool wake,
                        std::string& error) {
        error.clear();
        const std::string name = bounded(entity_name, BMMO_PHYSICS_NAME_SIZE);
        CK3dEntity* entity = find_entity(physics, name);
        if (!entity) {
            error = "no 3D entity named '" + name + "'";
            return false;
        }
        PhysicsObject* object = physics->GetPhysicsObject(entity);
        IVP_Real_Object* real = object ? object->m_RealObject : nullptr;
        IVP_Core* core = real ? real->get_core() : nullptr;
        if (!core) {
            error = "'" + name + "' is not physicalized";
            return false;
        }
        if (position && rotation) {
            IVP_U_Quat quaternion;
            quaternion.x = rotation[0];
            quaternion.y = rotation[1];
            quaternion.z = rotation[2];
            quaternion.w = rotation[3];
            IVP_U_Point target;
            store3(target, position);
            real->beam_object_to_new_position(&quaternion, &target, IVP_TRUE);
        }
        if (linear) core->speed.set(linear[0], linear[1], linear[2]);
        if (angular) core->rot_speed.set(angular[0], angular[1], angular[2]);
        // A hard set replaces the motion, so pushes queued for the next PSI
        // must not survive it (design 8.4).
        core->speed_change.set_to_zero();
        core->rot_speed_change.set_to_zero();
        if (wake) real->ensure_in_simulation();
        else if (IVP_MTIS_SIMULATED(core->movement_state)) real->disable_simulation();
        // Same refresh the manager does after every step, so the render side
        // follows a body that was moved between ticks.
        CKIpionManager::UpdateObjectWorldMatrix(real);
        return true;
    }

    bool physicalize(CKIpionManager* physics, const char* entity_name, const ball_recipe& recipe,
                     const char* collision_group, std::string& error) {
        error.clear();
        if (!physics || !physics->GetEnvironment()) {
            error = "physics environment is unavailable";
            return false;
        }
        const std::string name = bounded(entity_name, BMMO_PHYSICS_NAME_SIZE);
        CK3dEntity* entity = find_entity(physics, name);
        if (!entity) {
            error = "no 3D entity named '" + name + "'";
            return false;
        }
        const std::string group = bounded(collision_group, IVP_NO_COLL_GROUP_STRING_LEN * 4);
        if (group.size() >= IVP_NO_COLL_GROUP_STRING_LEN) {
            // IVP_Template_Real_Object::set_nocoll_group_ident traps on longer
            // names; refuse instead of taking the engine down.
            error = "collision group '" + group + "' is longer than "
                  + std::to_string(IVP_NO_COLL_GROUP_STRING_LEN - 1) + " characters";
            return false;
        }
        // Counted before the shortcut, exactly like the retail block.
        ++physics->m_PhysicalizeCalls;
        if (physics->GetPhysicsObject(entity)) return true;

        const int convex_count = clamp_count(recipe.convex_count, BMMO_PHYSICS_MAX_CONVEX);
        const int ball_count = clamp_count(recipe.ball_count, BMMO_PHYSICS_MAX_BALLS);
        const int concave_count = clamp_count(recipe.concave_count, BMMO_PHYSICS_MAX_CONCAVE);

        CKMesh* convexes[BMMO_PHYSICS_MAX_CONVEX] = {};
        CKMesh* concaves[BMMO_PHYSICS_MAX_CONCAVE] = {};
        VxVector ball_positions[BMMO_PHYSICS_MAX_BALLS];
        float ball_radii[BMMO_PHYSICS_MAX_BALLS];
        float ball_radius = 1.0f;  // the block's default when no ball is declared
        for (int i = 0; i < BMMO_PHYSICS_MAX_BALLS; ++i) {
            ball_positions[i].Set(0.0f, 0.0f, 0.0f);
            ball_radii[i] = 1.0f;
        }
        for (int i = 0; i < convex_count; ++i)
            convexes[i] = find_mesh(physics, bounded(recipe.convex[i], BMMO_PHYSICS_NAME_SIZE));
        for (int j = 0; j < ball_count; ++j) {
            ball_positions[j].Set(recipe.ball_center[j][0], recipe.ball_center[j][1],
                                  recipe.ball_center[j][2]);
            ball_radii[j] = recipe.ball_radius[j];
            if (j == 0) ball_radius = ball_radii[j];
        }
        for (int k = 0; k < concave_count; ++k)
            concaves[k] = find_mesh(physics, bounded(recipe.concave[k], BMMO_PHYSICS_NAME_SIZE));

        VxVector shift_mass_center(recipe.mass_center[0], recipe.mass_center[1], recipe.mass_center[2]);
        VxVector* shift_mass_center_ptr = recipe.calc_mass_center ? nullptr : &shift_mass_center;

        // CKSTRING is char*: the engine reads them but the type is not const.
        std::string surface = bounded(recipe.collision_surface, BMMO_PHYSICS_NAME_SIZE);
        std::vector<char> surface_buffer(surface.begin(), surface.end());
        surface_buffer.push_back('\0');
        std::vector<char> group_buffer(group.begin(), group.end());
        group_buffer.push_back('\0');

        IVP_Material* material = new IVP_Material_Simple(recipe.friction, recipe.elasticity);
        const int result = physics->CreatePhysicsObjectOnParameters(
            entity, convex_count, convexes, ball_count, ball_positions, ball_radii,
            concave_count, concaves, ball_radius, surface_buffer.data(), shift_mass_center_ptr,
            recipe.fixed, material, recipe.mass, group.empty() ? nullptr : group_buffer.data(),
            recipe.start_frozen, recipe.enable_collision, recipe.calc_mass_center,
            recipe.linear_damp, recipe.rot_damp);
        if (result == CK_OK) {
            physics->OwnMaterial(entity, material);
            return true;
        }
        delete material;
        error = "CreatePhysicsObjectOnParameters failed for '" + name + "' ("
              + std::to_string(result) + ")";
        return false;
    }

    bool unphysicalize(CKIpionManager* physics, const char* entity_name, std::string& error) {
        error.clear();
        if (!physics || !physics->GetEnvironment()) {
            error = "physics environment is unavailable";
            return false;
        }
        const std::string name = bounded(entity_name, BMMO_PHYSICS_NAME_SIZE);
        CK3dEntity* entity = find_entity(physics, name);
        if (!entity) {
            error = "no 3D entity named '" + name + "'";
            return false;
        }
        ++physics->m_DePhysicalizeCalls;
        PhysicsObject* object = physics->GetPhysicsObject(entity);
        if (object && object->m_RealObject) object->m_RealObject->delete_silently();
        return true;
    }

    bool set_body_guard(CKIpionManager* physics, bool enable, const char* except_entity, std::string& error) {
        error.clear();
        if (!physics) {
            error = "no physics manager";
            return false;
        }
        CK_ID except_id = 0;
        if (enable && except_entity && *except_entity) {
            const std::string name = bounded(except_entity, BMMO_PHYSICS_NAME_SIZE);
            CK3dEntity* entity = find_entity(physics, name);
            if (!entity) {
                error = "no 3D entity named '" + name + "'";
                return false;
            }
            except_id = entity->GetID();
        }
        physics->m_KeepLevelBodies = enable ? 1 : 0;
        physics->m_KeepLevelBodiesExcept = except_id;
        return true;
    }

    bool get_clock(CKIpionManager* physics, float& time_factor, float& physics_delta, std::string& error) {
        error.clear();
        if (!physics) {
            error = "no physics manager";
            return false;
        }
        time_factor = physics->m_PhysicsTimeFactor;
        physics_delta = physics->m_PhysicsDeltaTime;
        return true;
    }

    std::string describe_core(CKIpionManager* physics, const char* entity_name) {
        CK3dEntity* entity = find_entity(physics, bounded(entity_name, BMMO_PHYSICS_NAME_SIZE));
        PhysicsObject* object = entity ? physics->GetPhysicsObject(entity) : nullptr;
        IVP_Real_Object* real = object ? object->m_RealObject : nullptr;
        IVP_Core* core = real ? real->get_core() : nullptr;
        IVP_Environment* env = real ? real->get_environment() : nullptr;
        if (!core || !env) return "<no core>";
        char text[400];
        std::snprintf(text, sizeof(text),
                      "ms=%d unit=%d/%d t_env=%.6f t_env_lastpsi=%.6f t_core_lastpsi=%.6f i_dt=%.3f "
                      "pos_lastpsi=(%.4f,%.4f,%.4f) delta=(%.4f,%.4f,%.4f) speed=(%.4f,%.4f,%.4f) wakeup_vec=%d "
                      "factor=%.4f dt_ms=%.4f phys_dt=%.6f reset=%d",
                      static_cast<int>(core->movement_state),
                      core->sim_unit_of_core ? static_cast<int>(core->sim_unit_of_core->get_unit_movement_type()) : -1,
                      core->sim_unit_of_core ? core->sim_unit_of_core->sim_unit_cores.len() : -1,
                      env->get_current_time().get_time(), env->get_old_time_of_last_PSI().get_time(),
                      core->time_of_last_psi.get_time(), static_cast<double>(core->i_delta_time),
                      core->pos_world_f_core_last_psi.k[0], core->pos_world_f_core_last_psi.k[1],
                      core->pos_world_f_core_last_psi.k[2], core->delta_world_f_core_psis.k[0],
                      core->delta_world_f_core_psis.k[1], core->delta_world_f_core_psis.k[2], core->speed.k[0],
                      core->speed.k[1], core->speed.k[2], core->is_in_wakeup_vec ? 1 : 0,
                      static_cast<double>(physics->m_PhysicsTimeFactor), static_cast<double>(physics->m_DeltaTime),
                      static_cast<double>(physics->m_PhysicsDeltaTime), physics->m_ResetRequested ? 1 : 0);
        return text;
    }

    bool set_body_group(CKIpionManager* physics, const char* entity_name, const char* collision_group,
                        std::string& error) {
        error.clear();
        if (!physics || !physics->GetEnvironment()) {
            error = "physics environment is unavailable";
            return false;
        }
        const std::string name = bounded(entity_name, BMMO_PHYSICS_NAME_SIZE);
        CK3dEntity* entity = find_entity(physics, name);
        PhysicsObject* object = entity ? physics->GetPhysicsObject(entity) : nullptr;
        if (!object || !object->m_RealObject) {
            error = "no physics body for '" + name + "'";
            return false;
        }
        const std::string group = collision_group ? bounded(collision_group, 64) : std::string();
        if (group.size() >= IVP_NO_COLL_GROUP_STRING_LEN) {
            error = "collision group too long: " + group;
            return false;
        }
        object->m_RealObject->change_nocoll_group_ident(group.empty() ? nullptr : group.c_str());
        return true;
    }

    bool install_player_collision_filter(CKIpionManager* physics, const char* player_group_prefix,
                                         std::string& error) {
        error.clear();
        IVP_Environment* environment = physics ? physics->GetEnvironment() : nullptr;
        if (!environment) {
            error = "physics environment is unavailable";
            return false;
        }
        const std::string prefix = bounded(player_group_prefix, IVP_NO_COLL_GROUP_STRING_LEN * 4);
        if (prefix.empty()) {
            error = "the player collision group prefix must not be empty";
            return false;
        }
        if (prefix.size() >= IVP_NO_COLL_GROUP_STRING_LEN) {
            error = "the player collision group prefix must be shorter than "
                  + std::to_string(IVP_NO_COLL_GROUP_STRING_LEN) + " characters";
            return false;
        }
        for (auto* installed: installed_filters()) {
            if (installed->environment != environment) continue;
            std::snprintf(installed->prefix, sizeof(installed->prefix), "%s", prefix.c_str());
            return true;  // idempotent per environment
        }
        // CKIpionManager::CreateEnvironment always builds the environment's
        // filter as an IVP_Meta_Collision_Filter.
        auto* meta = static_cast<IVP_Meta_Collision_Filter*>(environment->get_collision_filter());
        if (!meta) {
            error = "the environment has no collision filter";
            return false;
        }
        auto* filter = new player_collision_filter();
        filter->environment = environment;
        std::snprintf(filter->prefix, sizeof(filter->prefix), "%s", prefix.c_str());
        meta->add_collision_filter(filter);
        installed_filters().push_back(filter);
        return true;
    }

    // ---- bridge API v6 (design 9.10): spawn impulse, deterministic Random block ----

    namespace {
        // The retail Physics Impulse block's push for Referential == the
        // entity and Position 0,0,0 (physics_RT/Behaviors/PhysicsImpulse.cpp),
        // with the impulse vector direction * speed * mass: a kick of `speed`
        // metres per second whatever the ball's mass, and no spin.  Every
        // operation in the same order on both sides, so the bits agree.
        bool push_now(CKIpionManager* physics, CK3dEntity* entity, const float direction[3], float speed) {
            PhysicsObject* object = physics->GetPhysicsObject(entity);
            IVP_Real_Object* obj = object ? object->m_RealObject : nullptr;
            IVP_Core* core = obj ? obj->get_core() : nullptr;
            if (!core) return false;
            if (core->physical_unmoveable) return true;   // the block pushes nothing either
            obj->ensure_in_simulation();
            const float magnitude = speed * core->get_mass();
            const float ix = direction[0] * magnitude;
            const float iy = direction[1] * magnitude;
            const float iz = direction[2] * magnitude;
            IVP_U_Point dir(ix, iy, iz);
            IVP_U_Matrix mat;
            obj->get_m_world_f_object_AT(&mat);
            IVP_U_Point ipz;
            mat.vimult3(&dir, &ipz);
            IVP_U_Float_Point p(0.0f, 0.0f, 0.0f);
            IVP_U_Float_Point i(&ipz);
            IVP_U_Float_Point d(&dir);
            core->async_push_core(&p, &i, &d);
            return true;
        }

        // Queued into m_PreSimulateCallbacks like navigation_callback:
        // Process(pc) runs Execute at once, which only arms it (return 0 keeps
        // it queued); the PreSimulate pass of the frame then pushes the body
        // the Physicalize block created in between and returns 1, which
        // removes and deletes it.
        class impulse_callback final : public PhysicsCallback {
        public:
            impulse_callback(CKIpionManager* manager, CKBehavior* behavior, CK_ID entity, const float direction[3],
                             float speed)
                : PhysicsCallback(manager, behavior, 2), entity_(entity), speed_(speed) {
                for (int k = 0; k < 3; ++k) direction_[k] = direction[k];
            }
            int Execute() override {
                if (!armed_) {
                    armed_ = true;
                    return 0;
                }
                CK3dEntity* entity = m_IpionManager && m_IpionManager->m_Context
                    ? CK3dEntity::Cast(m_IpionManager->m_Context->GetObject(entity_)) : nullptr;
                if (!entity || !push_now(m_IpionManager, entity, direction_, speed_))
                    std::printf("[bmmo] spawn impulse: %s has no body at the PreSimulate pass\n",
                                entity && entity->GetName() ? entity->GetName() : "?");
                return 1;
            }
        private:
            CK_ID entity_;
            float direction_[3] = {};
            float speed_;
            bool armed_ = false;
        };

        deterministic_random g_random;
        const CKGUID kRandomBlockGuid(0x0c622386, 0x1c3054f7);
        constexpr float kRandomMax = static_cast<float>(deterministic_random::kMax);

        // min + r * (max - min) / RAND_MAX, in the block's own operation order
        // (a product, then a quotient, then a sum: nothing to contract).
        float draw(float min, float max) {
            const float r = static_cast<float>(g_random.next());
            const float span = max - min;
            const float scaled = r * span;
            return min + scaled / kRandomMax;
        }

        // Logics/Behaviors/Random.cpp (Ballanced) with rand() replaced by the
        // deterministic generator and RAND_MAX by the Microsoft runtime's.
        int random_block(const CKBehaviorContext& behcontext) {
            CKBehavior* beh = behcontext.Behavior;
            beh->ActivateInput(0, FALSE);
            beh->ActivateOutput(0);
            CKParameterOut* pout = beh->GetOutputParameter(0);
            if (!pout) return CKBR_OK;
            const CKGUID guid = pout->GetGUID();
            // The block only draws when both inputs carry the output's type.
            const auto input = [&](int index) -> CKParameterIn* {
                CKParameterIn* pin = beh->GetInputParameter(index);
                return pin && pin->GetGUID() == guid ? pin : nullptr;
            };
            CKParameterManager* pm = behcontext.ParameterManager;
            if (pm && pm->IsDerivedFrom(guid, CKPGUID_FLOAT)) {
                CKParameterIn* pmin = input(0);
                if (!pmin) return CKBR_OK;
                float min = 0.0f;
                pmin->GetValue(&min);
                CKParameterIn* pmax = input(1);
                if (!pmax) return CKBR_OK;
                float max = 0.0f;
                pmax->GetValue(&max);
                float res = draw(min, max);
                pout->SetValue(&res);
                return CKBR_OK;
            }
            if (guid == CKPGUID_INT) {
                CKParameterIn* pmin = input(0);
                if (!pmin) return CKBR_OK;
                int min = 0;
                pmin->GetValue(&min);
                CKParameterIn* pmax = input(1);
                if (!pmax) return CKBR_OK;
                int max = 0;
                pmax->GetValue(&max);
                int res = min + g_random.next() * (max - min) / deterministic_random::kMax;
                pout->SetValue(&res);
                return CKBR_OK;
            }
            if (guid == CKPGUID_VECTOR) {
                CKParameterIn* pmin = input(0);
                if (!pmin) return CKBR_OK;
                VxVector min(0.0f);
                pmin->GetValue(&min);
                CKParameterIn* pmax = input(1);
                if (!pmax) return CKBR_OK;
                VxVector max(0.0f);
                pmax->GetValue(&max);
                VxVector res;
                res.x = draw(min.x, max.x);
                res.y = draw(min.y, max.y);
                res.z = draw(min.z, max.z);
                pout->SetValue(&res);
                return CKBR_OK;
            }
            if (guid == CKPGUID_2DVECTOR) {
                CKParameterIn* pmin = input(0);
                if (!pmin) return CKBR_OK;
                Vx2DVector min;
                pmin->GetValue(&min);
                CKParameterIn* pmax = input(1);
                if (!pmax) return CKBR_OK;
                Vx2DVector max;
                pmax->GetValue(&max);
                Vx2DVector res;
                res.x = draw(min.x, max.x);
                res.y = draw(min.y, max.y);
                pout->SetValue(&res);
                return CKBR_OK;
            }
            if (guid == CKPGUID_RECT) {
                CKParameterIn* pmin = input(0);
                if (!pmin) return CKBR_OK;
                VxRect min;
                pmin->GetValue(&min);
                CKParameterIn* pmax = input(1);
                if (!pmax) return CKBR_OK;
                VxRect max;
                pmax->GetValue(&max);
                VxRect res;
                res.left = draw(min.left, max.left);
                res.top = draw(min.top, max.top);
                res.right = draw(min.right, max.right);
                res.bottom = draw(min.bottom, max.bottom);
                res.Normalize();
                pout->SetValue(&res);
                return CKBR_OK;
            }
            if (guid == CKPGUID_BOOL) {
                CKBOOL res = g_random.next() & 1;
                pout->SetValue(&res);
                return CKBR_OK;
            }
            if (guid == CKPGUID_COLOR) {
                CKParameterIn* pmin = input(0);
                if (!pmin) return CKBR_OK;
                VxColor min;
                pmin->GetValue(&min);
                CKParameterIn* pmax = input(1);
                if (!pmax) return CKBR_OK;
                VxColor max;
                pmax->GetValue(&max);
                VxColor res;
                res.r = draw(min.r, max.r);
                res.g = draw(min.g, max.g);
                res.b = draw(min.b, max.b);
                res.a = draw(min.a, max.a);
                pout->SetValue(&res);
                return CKBR_OK;
            }
            return CKBR_OK;
        }
    }

    bool push_impulse(CKIpionManager* physics, const char* entity_name, const float direction_ws[3], float speed,
                      uint32_t behavior_id, std::string& error) {
        error.clear();
        if (!physics || !physics->GetEnvironment()) {
            error = "physics environment is unavailable";
            return false;
        }
        if (!direction_ws) {
            error = "no impulse direction";
            return false;
        }
        const std::string name = bounded(entity_name, BMMO_PHYSICS_NAME_SIZE);
        CK3dEntity* entity = find_entity(physics, name);
        if (!entity) {
            error = "no 3D entity named '" + name + "'";
            return false;
        }
        if (push_now(physics, entity, direction_ws, speed)) return true;
        // No body yet: the Physicalize block of this frame creates it before
        // the manager's PreSimulate pass runs.
        CKBehavior* behavior = behavior_id && physics->m_Context
            ? CKBehavior::Cast(physics->m_Context->GetObject(behavior_id)) : nullptr;
        if (!behavior || !physics->m_PreSimulateCallbacks) {
            error = "'" + name + "' is not physicalized";
            return false;
        }
        physics->m_PreSimulateCallbacks->Process(
            new impulse_callback(physics, behavior, entity->GetID(), direction_ws, speed));
        return true;
    }

    void random_reset(int32_t seed) { g_random.reset(seed == 0 ? 1 : seed); }
    int32_t random_get_state() { return static_cast<int32_t>(g_random.state); }
    void random_set_state(int32_t state) { g_random.state = static_cast<uint32_t>(state); }
    int32_t random_next() { return g_random.next(); }

    namespace {
        // The scripts whose Random blocks feed physics: the trafo explosions
        // (Balls.nmo, group All_Balls).  Only these are rerouted: every other
        // Random block in the game (menus, sounds) keeps the C runtime, so a
        // draw that happens on one side only - a sound script branching on a
        // sound manager the headless engine does not have - can never shift
        // the sequence the pieces are built from.
        const char* const kExplosionScripts[] = {"Ball_Explosion_Wood", "Ball_Explosion_Paper", "Ball_Explosion_Stone"};

        int patch_random_blocks(CKBehavior* behavior) {
            if (!behavior) return 0;
            int patched = 0;
            // A block has block data (and a prototype GUID) only when it uses
            // a function; a graph does not, and the retail CK2 may not guard
            // GetPrototypeGuid against that.
            if (behavior->IsUsingFunction() && behavior->GetPrototypeGuid() == kRandomBlockGuid
                    && behavior->GetFunction() != &random_block) {
                behavior->SetFunction(&random_block);
                ++patched;
            }
            const int count = behavior->GetSubBehaviorCount();
            for (int i = 0; i < count; ++i) patched += patch_random_blocks(behavior->GetSubBehavior(i));
            return patched;
        }
    }

    namespace {
        // The parent frames of the trafo explosion pieces (Balls.nmo).
        const char* const kPieceFrames[] = {"Ball_WoodPieces_Frame", "Ball_StonePieces_Frame", "Ball_PaperPieces_Frame"};

        int restore_initial_condition(CKScene* scene, CKBeObject* object) {
            if (!object || !object->IsInScene(scene)) return 0;
            CKStateChunk* chunk = scene->GetObjectInitialValue(object);
            if (!chunk) return 0;
            CKReadObjectState(object, chunk);
            return 1;
        }

        CK3dEntity* find_entity(CKContext* context, const char* name) {
            return CK3dEntity::Cast(context->GetObjectByNameAndParentClass(const_cast<CKSTRING>(name), CKCID_3DENTITY, nullptr));
        }

        // The "Set Position" block (3DTrans) that opens every explosion
        // script: "Set Position (0,0,0, Referential = Ball_Pos_Frame)" on the
        // pieces frame.  Wrapped so the pieces start every explosion from an
        // exact pose (see restore_explosion_pieces).
        const CKGUID kSetPositionGuid(0xe456e78a, 0x456789aa);
        CKBEHAVIORFCT g_set_position_original = nullptr;

        int explosion_set_position(const CKBehaviorContext& behcontext) {
            restore_explosion_pieces(behcontext.Context);
            return g_set_position_original ? g_set_position_original(behcontext) : CKBR_OK;
        }

        int patch_explosion_placement(CKBehavior* script) {
            int patched = 0;
            const int count = script->GetSubBehaviorCount();
            for (int i = 0; i < count; ++i) {
                CKBehavior* block = script->GetSubBehavior(i);
                if (!block || !block->IsUsingFunction() || block->GetPrototypeGuid() != kSetPositionGuid) continue;
                if (block->GetFunction() == &explosion_set_position) continue;
                if (!g_set_position_original) g_set_position_original = block->GetFunction();
                block->SetFunction(&explosion_set_position);
                ++patched;
            }
            return patched;
        }
    }

    // Why the exactness dance: the game's CK2 and the reimplemented one round
    // hierarchy transforms differently in the last bit (retail x87 code versus
    // the rewrite), and the piece frames, Balls_MF above them and Ball_Pos_Frame
    // all carry a 1e-6 skew from the level file, so the pieces' world
    // matrices - what Physicalize reads - differed by a float ulp between the
    // two engines and the pieces then flew apart differently.  With the piece
    // frames detached and exactly axis-aligned, and Ball_Pos_Frame exactly on
    // its ball, every transform in the chain is an exact operation (products
    // with 0 and 1, one addition) and both engines agree bit for bit.
    int restore_explosion_pieces(CKContext* context) {
        CKScene* scene = context ? context->GetCurrentScene() : nullptr;
        if (!scene) return -1;
        int restored = 0;
        for (const char* frame_name: kPieceFrames) {
            CK3dEntity* frame = find_entity(context, frame_name);
            if (!frame) continue;
            // TT Restore IC with Hierarchy, then the frame is taken out of the
            // skewed hierarchy and squared up; the children's initial world
            // matrices are restored last, so their local matrices (derived by
            // the engine) come out exact.
            restored += restore_initial_condition(scene, frame);
            frame->SetParent(nullptr, TRUE);
            VxMatrix world = frame->GetWorldMatrix();
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c) world[r][c] = r == c ? 1.0f : 0.0f;
            world[3][3] = 1.0f;
            frame->SetWorldMatrix(world);
            for (CK3dEntity* child = frame->HierarchyParser(nullptr); child; child = frame->HierarchyParser(child))
                restored += restore_initial_condition(scene, child);
        }
        // Ball_Pos_Frame sits on its ball's origin; the file gives it a 5e-6
        // local offset that costs an inexact product per read.
        if (CK3dEntity* pos_frame = find_entity(context, "Ball_Pos_Frame"))
            if (pos_frame->GetParent()) pos_frame->SetLocalMatrix(VxMatrix::Identity());
        return restored;
    }

    int install_random_block(CKContext* context) {
        if (!context) return -1;
        if (!CKGetPrototypeFromGuid(kRandomBlockGuid)) return -1;   // Logics is not registered
        int patched = 0;
        const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
        CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
        for (int i = 0; i < count; ++i) {
            CKBehavior* behavior = CKBehavior::Cast(context->GetObject(ids[i]));
            if (!behavior || behavior->GetParent() || !behavior->GetName()) continue;
            for (const char* script: kExplosionScripts)
                if (std::strcmp(behavior->GetName(), script) == 0)
                    patched += patch_random_blocks(behavior) + patch_explosion_placement(behavior);
        }
        return patched;
    }
}
