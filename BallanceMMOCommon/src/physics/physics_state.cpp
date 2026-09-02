#include <physics/physics_state.hpp>

#include "CKAll.h"

#include "CKIpionManager.h"
#include "ivp_physics.hxx"
#include "ivp_core.hxx"
#include "ivp_real_object.hxx"
#include "ivp_time.hxx"
#include <physics/ivp_private_access.hpp>
#include "ivp_listener_object.hxx"
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
            if (const IVP_Core* core = object->get_core())
                out += "[" + std::to_string(static_cast<int>(core->movement_state)) + "]";
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
}
