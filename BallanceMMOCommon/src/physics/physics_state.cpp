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
        physics->m_DeltaTime = 1000.0f / 66.0f;
        physics->m_PhysicsDeltaTime = (1000.0f / 66.0f) * physics->m_PhysicsTimeFactor;
        ivp_srand(seed == 0 ? 1 : seed);
        return true;
    }
}
