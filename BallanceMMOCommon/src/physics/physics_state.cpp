#include <physics/physics_state.hpp>

#include "CKAll.h"

#include "CKIpionManager.h"
#include "PhysicsCallback.h"
#include "ivp_physics.hxx"
#include "ivp_core.hxx"
#include "ivp_controller.hxx"
#include "ivp_real_object.hxx"
#include "ivp_templates.hxx"
#include "ivp_mindist.hxx"
#include "ivp_friction.hxx"
#include "ivp_clustering_longrange.hxx"
#include "ivp_constraint_local.hxx"
#include <physics/ball_navigation.hpp>
#include <session/resync_rules.hpp>
#include "ivp_time.hxx"
#include <physics/ivp_private_access.hpp>
#include <ivp_time.hxx>
#include <ivu_min_list.hxx>
#include "ivp_listener_object.hxx"
#include "ivp_listener_psi.hxx"
#include "ivp_sim_unit.hxx"
#include "ivp_debug_manager.hxx"
#include "ivp_surface_manager.hxx"
#include "ivp_surman_polygon.hxx"
#include "ivp_compact_surface.hxx"
#include "ivp_compact_ledge.hxx"

#include <bmmo_build_id.h>

#include <algorithm>
#include <iterator>
#include <array>
#include <typeinfo>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string_view>
#include <unordered_map>
#include <vector>

int ivp_srand_read();

#if defined(_WIN32)
#include <float.h>
#include <xmmintrin.h>
#endif

// Diagnostic (design 9.26): the floating-point environment of the calling
// thread - x87 control word and SSE MXCSR - as "cw=<hex>|mxcsr=<hex>".
static void bmmo_fp_env_text(char* out, size_t size) {
#if defined(_WIN32)
    unsigned int cw = 0;
    _controlfp_s(&cw, 0, 0);
    const unsigned int mxcsr = _mm_getcsr();
    snprintf(out, size, "cw=%08x|mxcsr=%08x", cw, mxcsr);
#else
    snprintf(out, size, "cw=-|mxcsr=-");
#endif
}

// The contact point's per-contact state is protected (ivp_friction.hxx,
// IVP_Contact_Point_Fast); read the same way ivp_private_access.hpp reads the
// environment's clock, without touching the pinned headers.
namespace bmmo::physics::ivp_access {
    struct cp_keeper { using type = short IVP_Contact_Point_Fast::*; friend type steal(cp_keeper); };
    struct cp_gap { using type = IVP_FLOAT IVP_Contact_Point_Fast::*; friend type steal(cp_gap); };
    struct cp_pull { using type = int IVP_Contact_Point_Fast::*; friend type steal(cp_pull); };
    struct cp_span { using type = IVP_FLOAT (IVP_Contact_Point_Fast::*)[2]; friend type steal(cp_span); };
    struct cp_pressure { using type = IVP_FLOAT IVP_Contact_Point_Fast::*; friend type steal(cp_pressure); };
    struct cp_friction { using type = IVP_FLOAT IVP_Contact_Point_Fast::*; friend type steal(cp_friction); };
    template struct thief<cp_keeper, &IVP_Contact_Point_Fast::slowly_turn_on_keeper>;
    template struct thief<cp_gap, &IVP_Contact_Point_Fast::last_gap_len>;
    template struct thief<cp_pull, &IVP_Contact_Point_Fast::has_negative_pull_since>;
    template struct thief<cp_span, &IVP_Contact_Point_Fast::span_friction_s>;
    template struct thief<cp_pressure, &IVP_Contact_Point_Fast::now_friction_pressure>;
    template struct thief<cp_friction, &IVP_Contact_Point_Fast::real_friction_factor>;
    inline short& keeper(IVP_Contact_Point_Fast& cp) { return cp.*steal(cp_keeper()); }
    inline IVP_FLOAT& gap(IVP_Contact_Point_Fast& cp) { return cp.*steal(cp_gap()); }
    inline int& pull(IVP_Contact_Point_Fast& cp) { return cp.*steal(cp_pull()); }
    inline IVP_FLOAT* span(IVP_Contact_Point_Fast& cp) { return cp.*steal(cp_span()); }
    inline IVP_FLOAT& pressure(IVP_Contact_Point_Fast& cp) { return cp.*steal(cp_pressure()); }
    inline IVP_FLOAT& friction(IVP_Contact_Point_Fast& cp) { return cp.*steal(cp_friction()); }
    // The time manager's event list: an event's scheduled time is its
    // element's value, relative to the manager's base time.
    struct ml_elems { using type = IVP_U_Min_List_Element* IVP_U_Min_List::*; friend type steal(ml_elems); };
    template struct thief<ml_elems, &IVP_U_Min_List::elems>;
    inline IVP_U_Min_List_Element* elements(IVP_U_Min_List& list) { return list.*steal(ml_elems()); }

    // IVP_Real_Object::get_all_near_mindists is protected: the recheck of the
    // object's overlap element in the IVP_MT_GET_MINDIST movement state, which
    // creates the mindists to everything in range as EXACT ones at once (the
    // ordinary recheck leaves them as hulls that turn exact a PSI later).
    struct ro_near_mindists { using type = void (IVP_Real_Object::*)(); friend type steal(ro_near_mindists); };
    template struct thief<ro_near_mindists, &IVP_Real_Object::get_all_near_mindists>;
    inline void scan_near_mindists(IVP_Real_Object& object) { (object.*steal(ro_near_mindists()))(); }

    // IVP_Mindist::mindist_hull_limit_exceeded_event is protected: what the
    // PSI does to a hull mindist whose hull was exceeded - recalculate it and,
    // within range, make it exact (for a mesh pair, spawn the exact mindists
    // to the ledges in range).
    struct md_hull_event { using type = void (IVP_Mindist::*)(IVP_HTIME); friend type steal(md_hull_event); };
    template struct thief<md_hull_event, &IVP_Mindist::mindist_hull_limit_exceeded_event>;
    // A pair with a mesh on either side is checked by an IVP_OO_Watcher, whose
    // hull event (re)creates the exact mindists to the ledges in range.
    struct oo_hull_event { using type = void (IVP_OO_Watcher::*)(); friend type steal(oo_hull_event); };
    template struct thief<oo_hull_event, &IVP_OO_Watcher::hull_limit_exceeded_event>;
    struct oo_children { using type = IVP_U_FVector<IVP_Collision> IVP_OO_Watcher::*; friend type steal(oo_children); };
    template struct thief<oo_children, &IVP_OO_Watcher::mindists>;

    // One collision checker and everything under it, driven to exact
    // mindists: a watcher (re)creates its children, a recursive mindist its
    // own, and every hull child gets the event the PSI would give it when its
    // hull is exceeded, which turns it exact (ivp_mindist.cxx,
    // mindist_hull_limit_exceeded_event).
    inline void drive_exact(IVP_Collision* coll, int depth = 0) {
        if (!coll || depth > 4) return;
        if (auto* watcher = dynamic_cast<IVP_OO_Watcher*>(coll)) {
            (watcher->*steal(oo_hull_event()))();
            IVP_U_FVector<IVP_Collision>& children = watcher->*steal(oo_children());
            for (int i = children.len() - 1; i >= 0; --i) drive_exact(children.element_at(i), depth + 1);
            return;
        }
        auto* md = dynamic_cast<IVP_Mindist*>(coll);
        if (!md) return;
        if (md->mindist_status == IVP_MD_HULL_RECURSIVE) {
            auto* recursive = static_cast<IVP_Mindist_Recursive*>(md);
            recursive->rec_hull_limit_exceeded_event();
            for (int i = recursive->mindists.len() - 1; i >= 0; --i) drive_exact(recursive->mindists.element_at(i), depth + 1);
        } else if (md->mindist_status == IVP_MD_HULL) {
            (md->*steal(md_hull_event()))(0.0f);
        }
    }

    // The collision checkers between `object` and the object named `partner`,
    // as the beam's recheck left them - hulls that would turn exact a PSI
    // later - driven to their exact mindists now.  Returns false when the
    // overlap list has no checker for the pair at all.
    inline bool force_exact_mindists(IVP_Real_Object& object, const char* partner) {
        IVP_OV_Element* elem = object.get_ov_element();
        IVP_Core* core = object.get_core();
        if (!elem || !core) return false;
        // In the IVP_MT_GET_MINDIST movement state a mindist created now stays
        // exact (IVP_Mindist_Manager::insert_and_recalc_exact_mindist does not
        // convert it to a hull), which is how get_all_near_mindists works.
        const auto saved_state = core->movement_state;
        const IVP_FLOAT saved_speed = core->current_speed;
        core->movement_state = IVP_MT_GET_MINDIST;
        // The beam left current_speed at 0, and a hull mindist's event keeps
        // it a hull while its estimated distance exceeds the look-ahead
        // (speed * PSI * 6): give the look-ahead the speed the body has.
        core->current_speed = static_cast<IVP_FLOAT>(core->speed.real_length()) + 1.0f;
        bool found = false;
        for (int i = elem->collision_fvector.len() - 1; i >= 0; --i) {
            IVP_Collision* coll = elem->collision_fvector.element_at(i);
            if (!coll) continue;
            IVP_Real_Object* objects[2] = {nullptr, nullptr};
            coll->get_objects(objects);
            IVP_Real_Object* other = objects[0] == &object ? objects[1] : objects[0];
            if (!other || !other->get_name() || std::strcmp(other->get_name(), partner) != 0) continue;
            found = true;
            drive_exact(coll);
        }
        core->current_speed = saved_speed;
        core->movement_state = saved_state;
        return found;
    }
}

namespace bmmo::physics {
    // physics_savestate_capture.cpp (step OWNB's probe).
    std::string describe_hull_bookkeeping(IVP_Real_Object* object);


    const char* build_id() { return BMMO_PHYSICS_BUILD_ID; }

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
        uint64_t core_sum = 0, pose_sum = 0;

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
            // Order-independent (world_hash.hpp): a term per core, summed.
            fnv1a64 core_hasher, core_pose_hasher;
            feed_core(core_hasher, c);
            feed_core_pose(core_pose_hasher, c);
            core_sum += fold_term(core_hasher.value);
            pose_sum += fold_term(core_pose_hasher.value);
        }
        fnv1a64 hasher, pose_hasher;
        feed_environment(hasher, env);
        hasher.feed(core_sum);
        pose_hasher.feed(pose_sum);
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
        // Design 9.26 diagnostics: bodies whose first PSIs are dumped
        // (BMMO_PSI_PROBE=<name prefix>).
        struct psi_watch {
            IVP_Real_Object* object;
            std::string name;
            int remaining;
        };
        std::vector<psi_watch>& psi_watches() {
            static std::vector<psi_watch>* v = new std::vector<psi_watch>();
            return *v;
        }
        void psi_watch_forget(IVP_Real_Object* object) {
            auto& v = psi_watches();
            v.erase(std::remove_if(v.begin(), v.end(), [object](const psi_watch& w) { return w.object == object; }), v.end());
        }
        // Step OWNB's probe (BMMO_HULL_PROBE=<name prefix>, lines appended
        // to BMMO_HULL_PROBE_FILE): a body's collision bookkeeping at its
        // creation, its script wake, the start of its first PSIs and the
        // hosts' own probe points (hull_probe).  Diagnostics only.
        const char* hull_probe_prefix() {
            static const char* prefix = std::getenv("BMMO_HULL_PROBE");
            return prefix && *prefix ? prefix : nullptr;
        }
        bool hull_probe_matches(const IVP_Real_Object* object) {
            const char* prefix = hull_probe_prefix();
            const char* name = object ? const_cast<IVP_Real_Object*>(object)->get_name() : nullptr;
            return prefix && name && std::strncmp(name, prefix, std::strlen(prefix)) == 0;
        }
        void hull_probe_write(IVP_Real_Object* object, const char* label) {
            static const char* file = std::getenv("BMMO_HULL_PROBE_FILE");
            if (!file || !*file || !object) return;
            if (FILE* f = std::fopen(file, "ab")) {
                std::fprintf(f, "%s %s | %s\n", label, object->get_name() ? object->get_name() : "?",
                             describe_hull_bookkeeping(object).c_str());
                std::fclose(f);
            }
        }
        std::vector<psi_watch>& hull_watches() {
            static std::vector<psi_watch>* v = new std::vector<psi_watch>();
            return *v;
        }
        std::string describe_core_for_psi(const IVP_Real_Object* object) {
            const IVP_Core* core = object ? object->get_core() : nullptr;
            if (!core) return "?";
            char line[1200];
            std::snprintf(line, sizeof(line),
                "pos=%a,%a,%a|ql=%a,%a,%a,%a|qn=%a,%a,%a,%a|v=%a,%a,%a|w=%a,%a,%a|dv=%a,%a,%a|dw=%a,%a,%a|dpsi=%a,%a,%a|tlast=%a|idt=%a|st=%d",
                core->pos_world_f_core_last_psi.k[0], core->pos_world_f_core_last_psi.k[1], core->pos_world_f_core_last_psi.k[2],
                core->q_world_f_core_last_psi.x, core->q_world_f_core_last_psi.y, core->q_world_f_core_last_psi.z, core->q_world_f_core_last_psi.w,
                core->q_world_f_core_next_psi.x, core->q_world_f_core_next_psi.y, core->q_world_f_core_next_psi.z, core->q_world_f_core_next_psi.w,
                static_cast<double>(core->speed.k[0]), static_cast<double>(core->speed.k[1]), static_cast<double>(core->speed.k[2]),
                static_cast<double>(core->rot_speed.k[0]), static_cast<double>(core->rot_speed.k[1]), static_cast<double>(core->rot_speed.k[2]),
                static_cast<double>(core->speed_change.k[0]), static_cast<double>(core->speed_change.k[1]), static_cast<double>(core->speed_change.k[2]),
                static_cast<double>(core->rot_speed_change.k[0]), static_cast<double>(core->rot_speed_change.k[1]), static_cast<double>(core->rot_speed_change.k[2]),
                core->delta_world_f_core_psis.k[0], core->delta_world_f_core_psis.k[1], core->delta_world_f_core_psis.k[2],
                core->time_of_last_psi.get_seconds(), static_cast<double>(core->i_delta_time), static_cast<int>(core->movement_state));
            std::string out = line;
            IVP_Simulation_Unit* unit = const_cast<IVP_Core*>(core)->sim_unit_of_core;
            if (!unit) return out + "|unit=-";
            out += "|unit=";
            for (int i = 0; i < unit->sim_unit_cores.len(); ++i) {
                IVP_Core* c = unit->sim_unit_cores.element_at(i);
                IVP_Real_Object* o = c && c->objects.len() ? c->objects.element_at(0) : nullptr;
                if (i) out += ',';
                out += o && o->get_name() ? o->get_name() : "?";
            }
            out += "|ctrl=";
            for (int i = 0; i < unit->controller_cores.len(); ++i) {
                IVP_Sim_Unit_Controller_Core_List* entry = unit->controller_cores.element_at(i);
                if (i) out += ',';
                out += std::to_string(static_cast<int>(entry->l_controller->get_controller_priority()));
                out += '(';
                for (int k = 0; k < entry->cores_controlled_by.len(); ++k) {
                    IVP_Core* c = entry->cores_controlled_by.element_at(k);
                    IVP_Real_Object* o = c && c->objects.len() ? c->objects.element_at(0) : nullptr;
                    if (k) out += '+';
                    out += o && o->get_name() ? o->get_name() : "?";
                }
                out += ')';
            }
            return out;
        }

        class event_log_listener : public IVP_Listener_Object {
        public:
            std::string text;
            IVP_Environment* environment = nullptr;
            CKIpionManager* manager = nullptr;   // for the creation parameters (engine change #19)

            // A replay re-executes mechanism scripts whose Physics WakeUp
            // and births would otherwise be logged as live events
            // (game/script_replay.hpp); the clients mute the log around it.
            bool muted = false;
            void record(const char* kind, IVP_Event_Object* event) {
                if (muted || !event || text.size() > 256 * 1024) return;
                char stamp[64];
                std::snprintf(stamp, sizeof(stamp), "t=%.6f ", event->environment
                    ? event->environment->get_current_time().get_seconds() : -1.0);
                text += stamp;
                text += kind;
                text += ' ';
                text += event->real_object && event->real_object->get_name() ? event->real_object->get_name() : "?";
                text += ';';
            }
            // Design 9.26 diagnostics: the state a body is born with, before
            // its first PSI - "t=<s> birth <name>|pos=..|q=..|v=..|w=..|m=..|I=..|damp=..|tlast=..|surface=..".
            void record_birth(IVP_Event_Object* event) {
                if (muted) return;
                IVP_Real_Object* object = event ? event->real_object : nullptr;
                const IVP_Core* core = object ? object->get_core() : nullptr;
                if (!core || text.size() > 256 * 1024) return;
                uint64_t surface = 0;
                if (IVP_SurfaceManager* manager = object->get_surface_manager()) {
                    if (manager->get_type() == IVP_SURMAN_POLYGON) {
                        const IVP_Compact_Surface* compact =
                            static_cast<IVP_SurfaceManager_Polygon*>(manager)->get_compact_surface();
                        if (compact) {
                            fnv1a64 hasher;
                            hasher.feed(compact, static_cast<size_t>(compact->get_size()));
                            surface = hasher.value;
                        }
                    }
                }
                char fp[48];
                bmmo_fp_env_text(fp, sizeof(fp));
                char line[960];
                std::snprintf(line, sizeof(line),
                    "t=%.6f birth %s|pos=%a,%a,%a|q=%a,%a,%a,%a|v=%a,%a,%a|w=%a,%a,%a|m=%a|I=%a,%a,%a|damp=%a,%a,%a,%a"
                    "|tlast=%a|idt=%a|st=%d|surface=%016llx|%s;",
                    event->environment ? event->environment->get_current_time().get_seconds() : -1.0,
                    object->get_name() ? object->get_name() : "?",
                    core->pos_world_f_core_last_psi.k[0], core->pos_world_f_core_last_psi.k[1], core->pos_world_f_core_last_psi.k[2],
                    core->q_world_f_core_last_psi.x, core->q_world_f_core_last_psi.y, core->q_world_f_core_last_psi.z,
                    core->q_world_f_core_last_psi.w,
                    static_cast<double>(core->speed.k[0]), static_cast<double>(core->speed.k[1]), static_cast<double>(core->speed.k[2]),
                    static_cast<double>(core->rot_speed.k[0]), static_cast<double>(core->rot_speed.k[1]), static_cast<double>(core->rot_speed.k[2]),
                    static_cast<double>(core->get_mass()),
                    static_cast<double>(core->get_rot_inertia()->k[0]), static_cast<double>(core->get_rot_inertia()->k[1]),
                    static_cast<double>(core->get_rot_inertia()->k[2]),
                    static_cast<double>(core->speed_damp_factor), static_cast<double>(core->rot_speed_damp_factor.k[0]),
                    static_cast<double>(core->rot_speed_damp_factor.k[1]), static_cast<double>(core->rot_speed_damp_factor.k[2]),
                    core->time_of_last_psi.get_seconds(), static_cast<double>(core->i_delta_time),
                    static_cast<int>(core->movement_state), static_cast<unsigned long long>(surface), fp);
                text += line;
            }
            // The recipe the body was created with and the entity's matrix at
            // that moment (engine change #19, CKIpionManager::m_CurrentCreate):
            // "t=<s> recipe <name>|fixed=|frozen=|coll=|cmc=|fr=|el=|m=|ld=|rd=|mc=x,y,z
            // |surf=<name>|convex=a/b|balls=x,y,z,r/..|concave=a/b|group=<g>|mtx=16 hex floats;"
            // - what a rollback needs to re-create a body born inside its
            // window the way the live frame did (docs/rollback-core-unification-plan.md, 9).
            // While the log is muted (a replayed frame), a recipe goes to its
            // own buffer instead when the caller asked for it
            // (set_event_log_keep_muted_recipes): the body the replayed
            // scripts made, for the caller's birth log.
            bool keep_muted_recipes = false;
            std::string muted_recipes;
            void record_recipe(IVP_Event_Object* event) {
                std::string* sink = muted ? (keep_muted_recipes ? &muted_recipes : nullptr) : &text;
                if (!sink || !manager || !manager->m_CurrentCreate || sink->size() > 256 * 1024) return;
                const CKIpionManager::PhysicsCreateParams& p = *manager->m_CurrentCreate;
                if (!p.target || !event || !event->real_object) return;
                char line[2600];
                const int n = std::snprintf(line, sizeof(line),
                    "t=%.6f recipe %s|fixed=%d|frozen=%d|coll=%d|cmc=%d|fr=%a|el=%a|m=%a|ld=%a|rd=%a|mc=%a,%a,%a|surf=%s|convex=",
                    event->environment ? event->environment->get_current_time().get_seconds() : -1.0,
                    p.target->GetName() ? p.target->GetName() : "?", p.fixed ? 1 : 0, p.startFrozen ? 1 : 0,
                    p.enableCollision ? 1 : 0, p.autoCalcMassCenter ? 1 : 0,
                    p.material ? static_cast<double>(p.material->get_friction_factor()) : 0.0,
                    p.material ? static_cast<double>(p.material->get_elasticity()) : 0.0,
                    static_cast<double>(p.mass), static_cast<double>(p.linearSpeedDampening),
                    static_cast<double>(p.rotSpeedDampening),
                    p.shiftMassCenter ? static_cast<double>(p.shiftMassCenter->x) : 0.0,
                    p.shiftMassCenter ? static_cast<double>(p.shiftMassCenter->y) : 0.0,
                    p.shiftMassCenter ? static_cast<double>(p.shiftMassCenter->z) : 0.0,
                    p.collisionSurface ? p.collisionSurface : "");
                if (n < 0 || n >= static_cast<int>(sizeof(line))) return;
                std::string out(line);
                for (int i = 0; i < p.convexCount; ++i) {
                    if (i) out += '/';
                    out += p.convexes && p.convexes[i] && p.convexes[i]->GetName() ? p.convexes[i]->GetName() : "";
                }
                out += "|balls=";
                for (int i = 0; i < p.ballCount; ++i) {
                    const VxVector c = p.ballPositions ? p.ballPositions[i] : VxVector(0.0f, 0.0f, 0.0f);
                    const float r = p.ballRadii ? p.ballRadii[i] : p.ballRadius;
                    std::snprintf(line, sizeof(line), "%s%a,%a,%a,%a", i ? "/" : "", static_cast<double>(c.x),
                                  static_cast<double>(c.y), static_cast<double>(c.z), static_cast<double>(r));
                    out += line;
                }
                if (p.ballCount == 0 && (!p.collisionSurface || !p.collisionSurface[0])) {
                    // A plain ball of ballRadius: what CreatePhysicsObjectOnParameters makes then.
                    std::snprintf(line, sizeof(line), "%a,%a,%a,%a", 0.0, 0.0, 0.0, static_cast<double>(p.ballRadius));
                    out += line;
                }
                out += "|concave=";
                for (int i = 0; i < p.concaveCount; ++i) {
                    if (i) out += '/';
                    out += p.concaves && p.concaves[i] && p.concaves[i]->GetName() ? p.concaves[i]->GetName() : "";
                }
                out += "|group=";
                out += p.collisionGroup ? p.collisionGroup : "";
                out += "|mtx=";
                const VxMatrix& m = p.target->GetWorldMatrix();
                for (int r = 0; r < 4; ++r)
                    for (int k = 0; k < 4; ++k) {
                        std::snprintf(line, sizeof(line), "%s%a", (r || k) ? "," : "", static_cast<double>(m[r][k]));
                        out += line;
                    }
                // The local matrix too, and whether the entity has a parent: a
                // body re-made under a module's parent gets its local matrix
                // back, so the manager's Set World Matrix on that parent
                // recomputes the same world matrix the live creation saw
                // (writing the world matrix left a parented piece a micrometre off).
                out += "|parent=";
                out += p.target->GetParent() ? "1" : "0";
                out += "|lmtx=";
                const VxMatrix& l = p.target->GetLocalMatrix();
                for (int r = 0; r < 4; ++r)
                    for (int k = 0; k < 4; ++k) {
                        std::snprintf(line, sizeof(line), "%s%a", (r || k) ? "," : "", static_cast<double>(l[r][k]));
                        out += line;
                    }
                out += ';';
                *sink += out;
            }
            void event_object_deleted(IVP_Event_Object* event) override {
                record("deleted", event);
                if (event && event->real_object) psi_watch_forget(event->real_object);
                if (event && event->real_object) {
                    auto& h = hull_watches();
                    IVP_Real_Object* gone = event->real_object;
                    h.erase(std::remove_if(h.begin(), h.end(), [gone](const psi_watch& w) { return w.object == gone; }), h.end());
                }
            }
            void event_object_created(IVP_Event_Object* event) override {
                record("created", event);
                record_recipe(event);
                record_birth(event);
                // BMMO_PSI_PROBE=<name prefix>: the first PSIs of every body
                // born with that prefix go into the event log as "psi" entries.
                static const char* probe = std::getenv("BMMO_PSI_PROBE");
                if (probe && *probe && event && event->real_object && event->real_object->get_name()
                        && std::strncmp(event->real_object->get_name(), probe, std::strlen(probe)) == 0)
                    psi_watches().push_back({event->real_object, event->real_object->get_name(), 6});
                if (event && hull_probe_matches(event->real_object)) {
                    hull_probe_write(event->real_object, muted ? "created(replay)" : "created");
                    hull_watches().push_back({event->real_object, event->real_object->get_name(), 8});
                }
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

        // Fires at the beginning of every PSI: the watched bodies' state as
        // the PSI finds them, i.e. the result of the previous PSI.
        class psi_probe : public IVP_Listener_PSI {
        public:
            IVP_Environment* environment = nullptr;
            void event_PSI(IVP_Event_PSI* event) override {
                if (!hull_watches().empty()) {
                    auto& h = hull_watches();
                    for (auto& w: h) {
                        hull_probe_write(w.object, g_event_log.muted ? "psi(replay)" : "psi");
                        --w.remaining;
                    }
                    h.erase(std::remove_if(h.begin(), h.end(), [](const psi_watch& w) { return w.remaining <= 0; }), h.end());
                }
                if (g_event_log.muted) return;
                auto& v = psi_watches();
                if (v.empty() || g_event_log.text.size() > 256 * 1024) return;
                char stamp[64];
                std::snprintf(stamp, sizeof(stamp), "t=%.6f ", event && event->environment
                    ? event->environment->get_current_time().get_seconds() : -1.0);
                for (auto& w: v) {
                    g_event_log.text += stamp;
                    g_event_log.text += "psi ";
                    g_event_log.text += w.name;
                    g_event_log.text += '|';
                    g_event_log.text += describe_core_for_psi(w.object);
                    g_event_log.text += ';';
                    --w.remaining;
                }
                v.erase(std::remove_if(v.begin(), v.end(), [](const psi_watch& w) { return w.remaining <= 0; }), v.end());
            }
            void environment_will_be_deleted(IVP_Environment*) override { environment = nullptr; psi_watches().clear(); }
        };
        psi_probe& g_psi_probe = *new psi_probe();

        // The engine calls this only for an explicit script wake-up; the
        // environment check already happened in CKIpionManager. It shares the
        // listener's log so the entries keep one format and one drain point.
        void record_script_wakeup(IVP_Real_Object* object) {
            if (hull_probe_matches(object)) hull_probe_write(object, g_event_log.muted ? "script_wakeup(replay)" : "script_wakeup");
            if (g_event_log.muted) return;
            if (!object || g_event_log.text.size() > 256 * 1024) return;
            IVP_Environment* environment = object->get_environment();
            char stamp[64];
            std::snprintf(stamp, sizeof(stamp), "t=%.6f ", environment
                ? environment->get_current_time().get_seconds() : -1.0);
            g_event_log.text += stamp;
            g_event_log.text += "script_wakeup";
            g_event_log.text += ' ';
            g_event_log.text += object->get_name() ? object->get_name() : "?";
            g_event_log.text += ';';
        }

        // A SetPhysicsForce block built its controller (design 9.26
        // diagnostics): "t=<s> force <name>|f=..|p=..|<immediate|deferred>".
        void record_force(IVP_Real_Object* object, const IVP_U_Point* force, const IVP_U_Point* pos, int deferred) {
            if (g_event_log.muted) return;
            if (!object || !force || !pos || g_event_log.text.size() > 256 * 1024) return;
            IVP_Environment* environment = object->get_environment();
            char line[400];
            std::snprintf(line, sizeof(line), "t=%.6f force %s|f=%a,%a,%a|p=%a,%a,%a|%s;",
                environment ? environment->get_current_time().get_seconds() : -1.0,
                object->get_name() ? object->get_name() : "?", force->k[0], force->k[1], force->k[2],
                pos->k[0], pos->k[1], pos->k[2], deferred ? "deferred" : "immediate");
            g_event_log.text += line;
        }

        // A Set Physics Ball Joint built its constraint (design 9.26
        // diagnostics): "t=<s> constraint <reference>|<attached>|<immediate|deferred>".
        void record_constraint(IVP_Real_Object* reference, IVP_Real_Object* attached, int deferred) {
            if (g_event_log.muted) return;
            if (!reference || g_event_log.text.size() > 256 * 1024) return;
            IVP_Environment* environment = reference->get_environment();
            char stamp[64];
            std::snprintf(stamp, sizeof(stamp), "t=%.6f ", environment
                ? environment->get_current_time().get_seconds() : -1.0);
            g_event_log.text += stamp;
            g_event_log.text += "constraint ";
            g_event_log.text += reference->get_name() ? reference->get_name() : "?";
            g_event_log.text += '|';
            g_event_log.text += attached && attached->get_name() ? attached->get_name() : "?";
            g_event_log.text += deferred ? "|deferred|" : "|immediate|";
            char fp[48];
            bmmo_fp_env_text(fp, sizeof(fp));
            g_event_log.text += fp;
            g_event_log.text += ';';
        }
    }

    void set_event_log_muted(bool muted) { g_event_log.muted = muted; }
    void set_event_log_keep_muted_recipes(bool keep) { g_event_log.keep_muted_recipes = keep; }
    std::string drain_muted_recipes() {
        std::string out;
        out.swap(g_event_log.muted_recipes);
        return out;
    }

    std::string drain_event_log(CKIpionManager* physics) {
        IVP_Environment* environment = physics ? physics->GetEnvironment() : nullptr;
        if (environment && g_event_log.environment != environment) {
            environment->add_listener_object_global(&g_event_log);
            g_event_log.environment = environment;
            g_event_log.text += "listener installed;";
        }
        if (environment) g_event_log.manager = physics;
        if (environment && g_psi_probe.environment != environment) {
            environment->add_listener_PSI(&g_psi_probe);
            g_psi_probe.environment = environment;
        }
        // The observer is a manager member, so a new environment or manager
        // needs its own hook even though the log itself is shared.
        if (physics && physics->m_ScriptWakeupObserver != &record_script_wakeup)
            physics->m_ScriptWakeupObserver = &record_script_wakeup;
        if (physics && physics->m_ConstraintObserver != &record_constraint)
            physics->m_ConstraintObserver = &record_constraint;
        if (physics && physics->m_ForceObserver != &record_force)
            physics->m_ForceObserver = &record_force;
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
                " dpsi=%a,%a,%a idt=%a tlast=%a nmc=%d calm=%a,%a",
                object->get_name() ? object->get_name() : "?", static_cast<int>(core->movement_state),
                p.k[0], p.k[1], p.k[2], ql.x, ql.y, ql.z, ql.w, qn.x, qn.y, qn.z, qn.w,
                core->speed.k[0], core->speed.k[1], core->speed.k[2],
                core->rot_speed.k[0], core->rot_speed.k[1], core->rot_speed.k[2],
                core->speed_change.k[0], core->speed_change.k[1], core->speed_change.k[2],
                core->rot_speed_change.k[0], core->rot_speed_change.k[1], core->rot_speed_change.k[2],
                core->delta_world_f_core_psis.k[0], core->delta_world_f_core_psis.k[1],
                core->delta_world_f_core_psis.k[2], static_cast<double>(core->i_delta_time),
                core->time_of_last_psi.get_seconds(),
#ifdef IVP_PORTABLE_BMMO
                core->sim_unit_of_core ? static_cast<int>(core->sim_unit_of_core->next_movement_check) : -1,
#else
                -1,
#endif
                core->time_of_calm_reference[0].get_seconds(), core->time_of_calm_reference[1].get_seconds());
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

    namespace {
        // Summary of one compact surface: its ledge tree walked, so a body
        // whose hull differs between two worlds can be told apart from a body
        // whose hull is merely stored differently.  The point count is
        // deliberately not reported: a ledge whose points were merged into the
        // surface's shared array keeps the size field of the unmerged ledge it
        // was copied from, so there is no honest per-ledge count to give.
        struct surface_walk {
            fnv1a64 header;   // mass centre, rotation inertia, radius
            int nodes = 0, ledges = 0, triangles = 0;
        };

        void walk_node(surface_walk& w, const IVP_Compact_Ledgetree_Node* node) {
            if (!node) return;
            ++w.nodes;
            const IVP_Compact_Ledge* ledge = node->is_terminal() ? node->get_compact_ledge()
                                                                 : node->get_compact_hull();
            if (ledge) {
                ++w.ledges;
                w.triangles += ledge->get_n_triangles();
            }
            if (node->is_terminal()) return;
            walk_node(w, node->left_son());
            walk_node(w, node->right_son());
        }

        surface_walk walk_surface(const IVP_Compact_Surface* surface) {
            surface_walk w;
            if (!surface) return w;
            for (int k = 0; k < 3; ++k) w.header.feed(surface->mass_center.k[k]);
            for (int k = 0; k < 3; ++k) w.header.feed(surface->rotation_inertia.k[k]);
            w.header.feed(surface->upper_limit_radius);
            walk_node(w, surface->get_compact_ledge_tree_root());
            return w;
        }
    }

    // Diagnostics: the per-body terms surface_signature() sums, one line per
    // body, sorted by name so two worlds whose surface signatures differ can
    // be diffed textually down to the body that differs.  `blob` is what the
    // signature itself hashes; `head` and the counts describe the same hull
    // independently of how the engine stored it, which is what tells a real
    // geometry difference from a storage one.
    std::string describe_surfaces_exact(CKIpionManager* physics) {
        if (!physics) return {};
        std::vector<std::string> lines;
        char text[512];
        for (auto it = physics->m_PhysicsObjects.Begin(); it != physics->m_PhysicsObjects.End(); ++it) {
            IVP_Real_Object* real = (*it).m_RealObject;
            if (!real) continue;
            const char* name = real->get_name() ? real->get_name() : "";
            int type = -1, size = 0;
            fnv1a64 blob;
            surface_walk walk;
            if (IVP_SurfaceManager* manager = real->get_surface_manager()) {
                type = static_cast<int>(manager->get_type());
                if (manager->get_type() == IVP_SURMAN_POLYGON) {
                    const IVP_Compact_Surface* surface =
                        static_cast<IVP_SurfaceManager_Polygon*>(manager)->get_compact_surface();
                    if (surface) {
                        size = surface->get_size();
                        blob.feed(surface, static_cast<size_t>(size));
                        walk = walk_surface(surface);
                    }
                }
            }
            std::snprintf(text, sizeof(text),
                          "%s type=%d size=%d blob=%016llx head=%016llx nodes=%d ledges=%d tris=%d",
                          name, type, size, static_cast<unsigned long long>(blob.value),
                          static_cast<unsigned long long>(walk.header.value),
                          walk.nodes, walk.ledges, walk.triangles);
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

        // entity_lookup_scope_begin/_end: per thread, so a scope never
        // serves another thread's lookups.  The table is open addressing
        // over views of the entities' own names (no allocation per entity),
        // built once a scope has looked up kScanLookups names by scanning: a
        // capture of a few bodies scans, one of every member's trafo pieces
        // pays one pass over the entities.
        struct entity_lookup_scope {
            static constexpr int kScanLookups = 12;
            CKIpionManager* physics = nullptr;
            int depth = 0;
            int lookups = 0;
            bool built = false;
            std::vector<std::pair<std::string_view, CK3dEntity*>> slots;   // empty view: a free slot
            static size_t hash(std::string_view s) {
                uint64_t h = 1469598103934665603ull;
                for (const char ch: s) {
                    h ^= static_cast<unsigned char>(ch);
                    h *= 1099511628211ull;
                }
                return static_cast<size_t>(h ^ (h >> 29));
            }
            // The first entity of a name stays (the scan's answer).
            void add(std::string_view name, CK3dEntity* entity) {
                const size_t mask = slots.size() - 1;
                for (size_t i = hash(name) & mask;; i = (i + 1) & mask) {
                    if (slots[i].first.empty()) {
                        slots[i] = {name, entity};
                        return;
                    }
                    if (slots[i].first == name) return;
                }
            }
            CK3dEntity* find(std::string_view name) const {
                const size_t mask = slots.size() - 1;
                for (size_t i = hash(name) & mask;; i = (i + 1) & mask) {
                    if (slots[i].first.empty()) return nullptr;
                    if (slots[i].first == name) return slots[i].second;
                }
            }
        };
        entity_lookup_scope& lookup_scope() {
            thread_local entity_lookup_scope scope;
            return scope;
        }

        CK3dEntity* find_entity(CKIpionManager* physics, const std::string& name) {
            if (!physics || !physics->m_Context || name.empty()) return nullptr;
            entity_lookup_scope& scope = lookup_scope();
            if (scope.depth > 0 && scope.physics == physics && ++scope.lookups > entity_lookup_scope::kScanLookups) {
                if (!scope.built) {
                    // The scan's order: every class derived from 3D entity
                    // in class-id order, each class list in its order
                    // (GetObjectListByType(derived) walks exactly that); the
                    // first entity of a name wins, as the scan returns it.
                    // An empty name never matches a lookup (find_entity
                    // refuses one), so it is not entered.
                    const XObjectPointerArray& all = physics->m_Context->GetObjectListByType(CKCID_3DENTITY, TRUE);
                    size_t size = 16;
                    while (size < 2 * static_cast<size_t>(all.Size()) + 2) size <<= 1;
                    scope.slots.assign(size, {std::string_view(), nullptr});
                    for (int i = 0; i < all.Size(); ++i) {
                        CKObject* object = all[i];
                        const char* object_name = object ? object->GetName() : nullptr;
                        if (!object_name || !*object_name) continue;
                        scope.add(std::string_view(object_name), static_cast<CK3dEntity*>(object));
                    }
                    scope.built = true;
                }
                return scope.find(std::string_view(name));
            }
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

    void entity_lookup_scope_begin(CKIpionManager* physics) {
        entity_lookup_scope& scope = lookup_scope();
        if (scope.depth++ == 0) {
            scope.physics = physics;
            scope.lookups = 0;
            scope.built = false;
        }
    }

    void entity_lookup_scope_end(CKIpionManager* physics) {
        entity_lookup_scope& scope = lookup_scope();
        (void)physics;
        if (scope.depth > 0 && --scope.depth == 0) {
            scope.physics = nullptr;
            scope.lookups = 0;
            scope.built = false;
        }
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

    std::string describe_presimulate_queue(CKIpionManager* physics) {
        if (!physics || !physics->m_PreSimulateCallbacks) return "no queue";
        std::string out;
        int count = 0;
        for (int slot = 0; slot < 3; ++slot) {
            IVP_U_Vector<PhysicsCallback>& callbacks = physics->m_PreSimulateCallbacks->m_Callbacks[slot];
            for (int i = 0; i < callbacks.len(); ++i) {
                PhysicsCallback* cb = callbacks.element_at(i);
                if (!cb) continue;
                ++count;
                CKBehavior* beh = cb->GetBehavior();
                CKBehavior* root = beh;
                while (root && root->GetParent()) root = root->GetParent();
                CKObject* target = beh ? beh->GetTarget() : nullptr;
                CK3dEntity* entity = target && CKIsChildClassOf(target, CKCID_3DENTITY) ? static_cast<CK3dEntity*>(target) : nullptr;
                const bool has_body = entity && physics->GetPhysicsObject(entity) != nullptr;
                char text[400];
                std::snprintf(text, sizeof(text), "%s{slot %d %s beh %u \"%s\" in \"%s\" target \"%s\"%s}", out.empty() ? "" : " ", slot,
                              typeid(*cb).name(), static_cast<unsigned>(cb->m_BehaviorID),
                              beh && beh->GetName() ? beh->GetName() : "?", root && root->GetName() ? root->GetName() : "?",
                              target && target->GetName() ? target->GetName() : "-", entity ? (has_body ? " body" : " NO BODY") : "");
                out += text;
            }
        }
        return std::to_string(count) + " queued" + (out.empty() ? "" : ": " + out);
    }

    std::string describe_contacts(CKIpionManager* physics, const char* entity_name) {
        CK3dEntity* entity = physics ? find_entity(physics, bounded(entity_name, BMMO_PHYSICS_NAME_SIZE)) : nullptr;
        PhysicsObject* object = entity ? physics->GetPhysicsObject(entity) : nullptr;
        IVP_Real_Object* real = object ? object->m_RealObject : nullptr;
        if (!real) return "no body";
        const auto other_name = [real](IVP_Real_Object* a, IVP_Real_Object* b) -> const char* {
            IVP_Real_Object* other = a == real ? b : a;
            return other && other->get_name() ? other->get_name() : "?";
        };
        char text[240];
        std::string out = "fr=";
        int n = 0;
        std::string list;
        for (IVP_Synapse_Friction* syn = real->get_first_friction_synapse(); syn; syn = syn->get_next()) {
            IVP_Contact_Point* cp = syn->get_contact_point();
            if (!cp) continue;
            ++n;
            IVP_Contact_Point_Fast& f = *cp;
            std::snprintf(text, sizeof(text), "{%s keeper=%d gap=%.5f pull=%d s=(%.4f,%.4f) press=%.4f mu=%.3f}",
                          other_name(cp->get_synapse(0)->get_object(), cp->get_synapse(1)->get_object()),
                          static_cast<int>(ivp_access::keeper(f)), static_cast<double>(ivp_access::gap(f)), ivp_access::pull(f),
                          static_cast<double>(ivp_access::span(f)[0]), static_cast<double>(ivp_access::span(f)[1]),
                          static_cast<double>(ivp_access::pressure(f)), static_cast<double>(ivp_access::friction(f)));
            list += text;
        }
        out += std::to_string(n) + list + " mind=";
        n = 0;
        list.clear();
        for (IVP_Synapse_Real* syn = real->get_first_exact_synapse(); syn; syn = syn->get_next()) {
            IVP_Mindist* md = syn->get_mindist();
            if (!md) continue;
            ++n;
            std::snprintf(text, sizeof(text), "{%s st=%d len=%.5f}",
                          other_name(md->get_synapse(0)->get_object(), md->get_synapse(1)->get_object()),
                          static_cast<int>(md->mindist_status), static_cast<double>(md->get_length()));
            list += text;
        }
        out += std::to_string(n) + list;
        return out;
    }

    void hull_probe(CKIpionManager* physics, const char* entity_name, const char* label) {
        if (!hull_probe_prefix() || !physics || !entity_name) return;
        CK3dEntity* entity = find_entity(physics, bounded(entity_name, BMMO_PHYSICS_NAME_SIZE));
        PhysicsObject* object = entity ? physics->GetPhysicsObject(entity) : nullptr;
        if (object && object->m_RealObject) hull_probe_write(object->m_RealObject, label);
    }

    std::string describe_body(CKIpionManager* physics, const char* entity_name) {
        CK3dEntity* entity = physics ? find_entity(physics, bounded(entity_name, BMMO_PHYSICS_NAME_SIZE)) : nullptr;
        PhysicsObject* object = entity ? physics->GetPhysicsObject(entity) : nullptr;
        IVP_Real_Object* real = object ? object->m_RealObject : nullptr;
        IVP_Core* core = real ? real->get_core() : nullptr;
        if (!core) return "no body";
        IVP_Environment* env = real->get_environment();
        const auto other_of = [real](IVP_Real_Object* a, IVP_Real_Object* b) -> IVP_Real_Object* { return a == real ? b : a; };
        const auto name_of = [](IVP_Real_Object* o) -> const char* { return o && o->get_name() ? o->get_name() : "?"; };
        char text[320];
        std::snprintf(text, sizeof(text), "state=%d v=(%.6f,%.6f,%.6f) w=(%.6f,%.6f,%.6f) dv=(%.6f,%.6f,%.6f) dw=(%.6f,%.6f,%.6f)",
                      static_cast<int>(core->movement_state),
                      static_cast<double>(core->speed.k[0]), static_cast<double>(core->speed.k[1]), static_cast<double>(core->speed.k[2]),
                      static_cast<double>(core->rot_speed.k[0]), static_cast<double>(core->rot_speed.k[1]), static_cast<double>(core->rot_speed.k[2]),
                      static_cast<double>(core->speed_change.k[0]), static_cast<double>(core->speed_change.k[1]), static_cast<double>(core->speed_change.k[2]),
                      static_cast<double>(core->rot_speed_change.k[0]), static_cast<double>(core->rot_speed_change.k[1]), static_cast<double>(core->rot_speed_change.k[2]));
        std::string out = text;
        // Sleep bookkeeping in one place: the object's own movement state
        // (IVP_Real_Object::ensure_in_simulation branches on this one, not the
        // core's), the unit's type and size, the delayed-revive flag, the
        // environment's revive list, and where the CK entity says the body is.
        {
            IVP_Simulation_Unit* unit = core->sim_unit_of_core;
            const VxVector& p = entity->GetWorldMatrix()[3];
            std::snprintf(text, sizeof(text), " obj_ms=%d unit=%d/%d wakeup=%d revive_len=%d entity=(%.4f,%.4f,%.4f)",
                          static_cast<int>(real->get_movement_state()),
                          unit ? static_cast<int>(unit->get_unit_movement_type()) : -1, unit ? unit->sim_unit_cores.len() : -1,
                          core->is_in_wakeup_vec ? 1 : 0, ivp_access::revive_list(*env).len(),
                          static_cast<double>(p.x), static_cast<double>(p.y), static_cast<double>(p.z));
            out += text;
        }
        out += " | " + describe_contacts(physics, entity_name);
        // The friction pairs this core is in, with the pair state that lives
        // on neither body: the easing countdown, the integrated anti-energy,
        // the time since the pair's last impact.
        out += " | pairs=";
        IVP_Friction_Info_For_Core* info = core->physical_unmoveable ? nullptr : core->moveable_core_has_friction_info();
        IVP_Friction_System* system = info ? info->l_friction_system : nullptr;
        if (system) {
            for (IVP_Synapse_Friction* syn = real->get_first_friction_synapse(); syn; syn = syn->get_next()) {
                IVP_Contact_Point* cp = syn->get_contact_point();
                if (!cp) continue;
                IVP_Real_Object* other = other_of(cp->get_synapse(0)->get_object(), cp->get_synapse(1)->get_object());
                IVP_Core* other_core = other ? other->get_core() : nullptr;
                IVP_Friction_Core_Pair* pair = other_core ? system->get_pair_info_for_objs(core, other_core) : nullptr;
                if (!pair) {
                    out += std::string("{") + name_of(other) + " none}";
                    continue;
                }
                std::snprintf(text, sizeof(text), "{%s ease=%d anti=%.6f since_impact=%.5f}", name_of(other), pair->next_ease_nr_psi,
                              static_cast<double>(pair->integrated_anti_energy),
                              static_cast<double>(env->get_current_time() - pair->last_impact_time_pair));
                out += text;
            }
        } else {
            out += "(no friction system)";
        }
        // The exact mindists with the collision-distance bookkeeping and the
        // time until each one's scheduled event.
        IVP_Time_Manager* manager = env->get_time_manager();
        IVP_U_Min_List_Element* events = manager && manager->min_hash ? ivp_access::elements(*manager->min_hash) : nullptr;
        out += " | mind=";
        for (IVP_Synapse_Real* syn = real->get_first_exact_synapse(); syn; syn = syn->get_next()) {
            IVP_Mindist* md = syn->get_mindist();
            if (!md) continue;
            double event_in = 0.0;
            bool scheduled = false;
            if (events && md->index >= 0) {
                IVP_Time at = manager->base_time;
                at += static_cast<double>(events[md->index].value);
                event_in = at - env->get_current_time();
                scheduled = true;
            }
            std::snprintf(text, sizeof(text), "{%s st=%d rr=%d len=%.6f coll=%.4f sel=%u dec=%u extra=%.4f ahull=%.6f ev=%s%.5f}",
                          name_of(other_of(md->get_synapse(0)->get_object(), md->get_synapse(1)->get_object())),
                          static_cast<int>(md->mindist_status), static_cast<int>(md->recalc_result),
                          static_cast<double>(md->get_length()), static_cast<double>(md->get_coll_dist()),
                          static_cast<unsigned>(md->coll_dist_selector), static_cast<unsigned>(md->coll_dist_decrease_counter),
                          static_cast<double>(md->sum_extra_radius), static_cast<double>(md->sum_angular_hull_time),
                          scheduled ? "" : "none ", event_in);
            out += text;
        }
        return out;
    }

    namespace {
        IVP_Real_Object* real_object_of(CKIpionManager* physics, const char* entity_name, std::string& error) {
            const std::string name = bounded(entity_name, BMMO_PHYSICS_NAME_SIZE);
            CK3dEntity* entity = physics ? find_entity(physics, name) : nullptr;
            PhysicsObject* object = entity ? physics->GetPhysicsObject(entity) : nullptr;
            if (!object || !object->m_RealObject) {
                error = "'" + name + "' is not physicalized";
                return nullptr;
            }
            return object->m_RealObject;
        }

        const char* contact_partner(IVP_Real_Object* self, IVP_Real_Object* a, IVP_Real_Object* b) {
            IVP_Real_Object* other = a == self ? b : a;
            return other && other->get_name() ? other->get_name() : "";
        }
    }

    int get_contacts(CKIpionManager* physics, const char* entity_name, bmmo_physics_contact* out, int max,
                     std::string& error) {
        error.clear();
        IVP_Real_Object* real = real_object_of(physics, entity_name, error);
        if (!real) return -1;
        int n = 0;
        for (IVP_Synapse_Friction* syn = real->get_first_friction_synapse(); syn; syn = syn->get_next()) {
            IVP_Contact_Point* cp = syn->get_contact_point();
            if (!cp) continue;
            if (out && n < max) {
                bmmo_physics_contact& c = out[n];
                c = bmmo_physics_contact{};
                std::snprintf(c.partner, sizeof(c.partner), "%s",
                              contact_partner(real, cp->get_synapse(0)->get_object(), cp->get_synapse(1)->get_object()));
                IVP_Contact_Point_Fast& f = *cp;
                c.gap = ivp_access::gap(f);
                c.span[0] = ivp_access::span(f)[0];
                c.span[1] = ivp_access::span(f)[1];
                c.pressure = ivp_access::pressure(f);
                c.pull = ivp_access::pull(f);
                c.keeper = ivp_access::keeper(f);
            }
            ++n;
        }
        return n;
    }

    int set_contacts(CKIpionManager* physics, const char* entity_name, const bmmo_physics_contact* in, int count,
                     uint32_t flags, std::string& error) {
        error.clear();
        IVP_Real_Object* real = real_object_of(physics, entity_name, error);
        if (!real) return -1;
        IVP_Core* core = real->get_core();
        if (!core || count <= 0 || !in) return 0;
        // A beam keeps the body's contact points, so after a restore the body
        // still rests on the partners it had at the LIVE tick, not the
        // anchor's (measured: a prop ball restored twelve ticks back onto one
        // plate kept its contact with the plate it was on NOW and had none
        // with the one it was on THEN).  With PRUNE, every live contact whose
        // partner the record does not name goes first, through IVP's own
        // deletion (the pair, the friction infos, the system if it empties).
        if (flags & BMMO_PHYSICS_CONTACT_PRUNE) {
            std::vector<IVP_Real_Object*> stale;
            for (IVP_Synapse_Friction* syn = real->get_first_friction_synapse(); syn; syn = syn->get_next()) {
                IVP_Contact_Point* cp = syn->get_contact_point();
                if (!cp) continue;
                IVP_Real_Object* a = cp->get_synapse(0)->get_object();
                IVP_Real_Object* b = cp->get_synapse(1)->get_object();
                IVP_Real_Object* other = a == real ? b : a;
                const char* partner = other && other->get_name() ? other->get_name() : "";
                bool recorded = false;
                for (int i = 0; i < count && !recorded; ++i) recorded = std::strcmp(in[i].partner, partner) == 0;
                if (!recorded && other && std::find(stale.begin(), stale.end(), other) == stale.end()) stale.push_back(other);
            }
            for (IVP_Real_Object* other: stale) real->unlink_contact_points_for_object(other);
        }
        // Which recorded partners the body still touches: those contacts
        // survived the beam with their state and are left alone.
        std::vector<bool> present(static_cast<size_t>(count), false);
        int live = 0;
        for (IVP_Synapse_Friction* syn = real->get_first_friction_synapse(); syn; syn = syn->get_next()) {
            IVP_Contact_Point* cp = syn->get_contact_point();
            if (!cp) continue;
            ++live;
            const char* partner = contact_partner(real, cp->get_synapse(0)->get_object(), cp->get_synapse(1)->get_object());
            for (int i = 0; i < count; ++i)
                if (!present[i] && std::strcmp(in[i].partner, partner) == 0) { present[i] = true; break; }
        }
        int missing = 0;
        for (int i = 0; i < count; ++i) if (!present[i]) ++missing;
        if (!missing) return 0;
        if ((flags & BMMO_PHYSICS_CONTACT_ONLY_WHEN_NONE) && live > 0) return 0;
        int made = 0;
        for (int i = 0; i < count; ++i) {
            if (present[i]) continue;
            // The pair's collision checker, as the beam's recheck left it: a
            // hull (recursive, for a mesh) that turns exact a PSI later.  Its
            // hull event now does what that PSI would: the exact mindists to
            // the ledges in range exist at once.  A partner the overlap list
            // does not know yet is looked for first.
            if (!ivp_access::force_exact_mindists(*real, in[i].partner)) {
                ivp_access::scan_near_mindists(*real);
                ivp_access::force_exact_mindists(*real, in[i].partner);
            }
            IVP_Mindist* best = nullptr;
            for (IVP_Synapse_Real* syn = real->get_first_exact_synapse(); syn; syn = syn->get_next()) {
                IVP_Mindist* md = syn->get_mindist();
                if (!md || md->mindist_status != IVP_MD_EXACT) continue;
                const char* partner = contact_partner(real, md->get_synapse(0)->get_object(), md->get_synapse(1)->get_object());
                if (std::strcmp(partner, in[i].partner) != 0) continue;
                if (!best || md->get_length() < best->get_length()) best = md;
            }
            if (best) {
                IVP_Real_Object* a = best->get_synapse(0)->get_object();
                IVP_Real_Object* b = best->get_synapse(1)->get_object();
                IVP_Real_Object* other = a == real ? b : a;
                IVP_Core* other_core = other ? other->get_core() : nullptr;
                if (!other_core) continue;
                if ((flags & BMMO_PHYSICS_CONTACT_STATIC_ONLY) && !other_core->physical_unmoveable) continue;   // a movable partner: left to IVP
                // IVP makes a contact point only at an impact, and an impact
                // revives both objects first - so the movable cores of a
                // friction system are all simulated, or none is.  A point
                // made here between a simulated core and a sleeping one
                // breaks that: the next impact system pulls the sleeper in
                // through the pair, skips its synchronization (done only for
                // a simulated core) and dereferences the sync record it never
                // made (test_loop_all_pairs, old_sync_info == NULL).  So wake
                // the sleeper the way the impact would, or leave the pair to
                // IVP.
                if (!other_core->physical_unmoveable) {
                    const bool live = IVP_MTIS_SIMULATED(core->movement_state) && !core->pinned;
                    const bool other_live = IVP_MTIS_SIMULATED(other_core->movement_state) && !other_core->pinned;
                    if (live != other_live) {
                        if (!(flags & BMMO_PHYSICS_CONTACT_REVIVE) || core->pinned || other_core->pinned) continue;
                        (live ? other_core : core)->ensure_core_to_be_in_simulation();
                    }
                }
            }
            if (!best) {
                int exact = 0;
                std::string seen;
                for (IVP_Synapse_Real* syn = real->get_first_exact_synapse(); syn; syn = syn->get_next()) {
                    ++exact;
                    if (IVP_Mindist* md = syn->get_mindist())
                        seen += std::string(" ") + contact_partner(real, md->get_synapse(0)->get_object(), md->get_synapse(1)->get_object())
                              + "/st" + std::to_string(static_cast<int>(md->mindist_status));
                }
                std::string checkers;
                if (IVP_OV_Element* elem = real->get_ov_element())
                    for (int c = 0; c < elem->collision_fvector.len(); ++c) {
                        IVP_Collision* coll = elem->collision_fvector.element_at(c);
                        IVP_Real_Object* objects[2] = {nullptr, nullptr};
                        if (coll) coll->get_objects(objects);
                        const char* kind = dynamic_cast<IVP_OO_Watcher*>(coll) ? "watcher" : dynamic_cast<IVP_Mindist*>(coll) ? "mindist" : "other";
                        checkers += std::string(" ") + contact_partner(real, objects[0], objects[1]) + "(" + kind + ")";
                    }
                error += (error.empty() ? "" : "; ") + std::string("no exact mindist to ") + in[i].partner + " (exact:" + seen
                       + "; checkers:" + checkers + ")";
                continue;
            }
            IVP_Friction_System* system = nullptr;
            IVP_BOOL fresh = IVP_FALSE;
            IVP_Contact_Point* cp = best->try_to_generate_managed_friction(&system, &fresh, core->sim_unit_of_core, IVP_TRUE);
            if (!cp) {
                error += (error.empty() ? "" : "; ") + std::string("no contact point for ") + in[i].partner;
                continue;
            }
            IVP_Contact_Point_Fast& f = *cp;
            if (flags & BMMO_PHYSICS_CONTACT_KEEPER) ivp_access::keeper(f) = static_cast<short>(in[i].keeper);
            if (flags & BMMO_PHYSICS_CONTACT_GAP) ivp_access::gap(f) = in[i].gap;
            if (flags & BMMO_PHYSICS_CONTACT_PULL) ivp_access::pull(f) = in[i].pull;
            if (flags & BMMO_PHYSICS_CONTACT_SPAN) {
                ivp_access::span(f)[0] = in[i].span[0];
                ivp_access::span(f)[1] = in[i].span[1];
            }
            if (flags & BMMO_PHYSICS_CONTACT_PRESSURE) ivp_access::pressure(f) = in[i].pressure;
            ++made;
        }
        return made;
    }

    bool recheck_body(CKIpionManager* physics, const char* entity_name, std::string& error) {
        error.clear();
        IVP_Real_Object* real = real_object_of(physics, entity_name, error);
        if (!real) return false;
        real->recheck_collision_filter();
        return true;
    }

    std::string describe_controllers(CKIpionManager* physics, const char* entity_name) {
        CK3dEntity* entity = physics ? find_entity(physics, bounded(entity_name, BMMO_PHYSICS_NAME_SIZE)) : nullptr;
        PhysicsObject* object = entity ? physics->GetPhysicsObject(entity) : nullptr;
        IVP_Core* core = object && object->m_RealObject ? object->m_RealObject->get_core() : nullptr;
        if (!core) return "no body";
        std::string out = "n=" + std::to_string(core->controllers_of_core.len());
        char text[160];
        for (int i = 0; i < core->controllers_of_core.len(); ++i) {
            IVP_Controller* controller = core->controllers_of_core.element_at(i);
            if (!controller) continue;
            double pos[3] = {}, force[3] = {};
            if (navigation_controller_info(controller, pos, force))
                std::snprintf(text, sizeof(text), " %s/%d f=(%.3f,%.3f,%.3f)", controller->get_controller_name(),
                              static_cast<int>(controller->get_controller_priority()), force[0], force[1], force[2]);
            else
                std::snprintf(text, sizeof(text), " %s/%d", controller->get_controller_name(),
                              static_cast<int>(controller->get_controller_priority()));
            out += text;
        }
        return out;
    }

    namespace {
        // What a body write does to the sleep state.  `legacy_wake` is the
        // unconditional ensure_in_simulation() of the pre-v10 entries: on an
        // already simulated body that call re-arms IVP's freeze counters, so
        // v10's `wake` only revives a body that is actually frozen.
        enum class sleep_action { legacy_wake, wake_if_frozen, freeze_if_simulated, keep };

        bool write_body_state(CKIpionManager* physics, const char* entity_name,
                              const double position[3], const double rotation[4],
                              const float linear[3], const float angular[3],
                              sleep_action action, bool recheck, bool restore_psi_state,
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
            bool beamed = false;
            if (hull_probe_matches(real)) hull_probe_write(real, "write_body_state");
            // IVP integrates with a leapfrog: at every PSI the core's position
            // is advanced by the delta written at the PREVIOUS one, and the
            // rotation of the next PSI was predicted at this one.  The beam
            // (IVP_Calc_Next_PSI_Solver::set_transformation) zeroes the delta
            // and collapses q_world_f_core_next_psi onto the pose it is given,
            // so a body beamed to the state it is already in still loses one
            // PSI of translation (|v|/66 m) and one PSI of rotation.  A
            // restore-style write keeps both: the per-PSI rotation increment
            // is taken across the beam, and the delta is rewritten from the
            // velocity the caller installs (which at a PSI boundary is what
            // calc_next_PSI_matrix put there).
            IVP_U_Quat psi_rotation_step;
            psi_rotation_step.init();
            if (restore_psi_state)
                psi_rotation_step.set_invert_mult(&core->q_world_f_core_last_psi,
                                                  &core->q_world_f_core_next_psi);
            if (position && rotation) {
                IVP_U_Quat quaternion;
                quaternion.x = rotation[0];
                quaternion.y = rotation[1];
                quaternion.z = rotation[2];
                quaternion.w = rotation[3];
                IVP_U_Point target;
                store3(target, position);
                // The optimized path generates no collision when the body lands
                // inside another one; a snap write needs the recheck (see the
                // header).
                real->beam_object_to_new_position(&quaternion, &target, recheck ? IVP_FALSE : IVP_TRUE);
                beamed = true;
            }
            if (linear) core->speed.set(linear[0], linear[1], linear[2]);
            if (angular) core->rot_speed.set(angular[0], angular[1], angular[2]);
            if (restore_psi_state && beamed) {
                if (linear) core->delta_world_f_core_psis.set(&core->speed);
                // The pose the caller hands in is what get_body_state reads
                // back: the core interpolated to the environment's current
                // time (IVP_Core::inline_calc_at_position / _quaternion), a
                // fraction f of a PSI past the anchor the beam just set the
                // core on (core->time_of_last_psi).  The beam stored that
                // reading AS the anchor, so read back it would already have
                // moved on by f of a PSI - a body written with the state it
                // was just read in ends one PSI ahead of itself when f is 1,
                // which is where the client's frames leave the clock (the
                // server's world sits on the boundary, f = 0, and never saw
                // it).  Move the anchor back to where the reading came from,
                // so a restore is idempotent whatever the phase.
                IVP_Environment* env = real->get_environment();
                const IVP_DOUBLE f_t = env->get_current_time() - core->time_of_last_psi;
                core->pos_world_f_core_last_psi.add_multiple(&core->delta_world_f_core_psis, -f_t);
                core->m_world_f_core_last_psi.get_position()->set(&core->pos_world_f_core_last_psi);
                // The rotation reading interpolates last -> next by f; at
                // either end it is one of the two exactly, and the frames only
                // ever leave the clock at an end (f = 0 on the server's
                // boundary, f = 1 on the client's).  Whichever end the reading
                // is, the other is one carried PSI step away.
                const IVP_DOUBLE f = f_t * env->get_inv_delta_PSI_time();
                const IVP_U_Quat reading = core->q_world_f_core_last_psi;
                if (f > 0.5) {
                    core->q_world_f_core_next_psi = reading;
                    core->q_world_f_core_last_psi.set_div_unit_quat(&reading, &psi_rotation_step);
                    core->q_world_f_core_last_psi.fast_normize_quat();
                    core->q_world_f_core_last_psi.set_matrix(&core->m_world_f_core_last_psi);
                } else {
                    core->q_world_f_core_next_psi.inline_set_mult_quat(&reading, &psi_rotation_step);
                    core->q_world_f_core_next_psi.fast_normize_quat();
                }
            }
            // A hard set replaces the motion, so pushes queued for the next PSI
            // must not survive it (design 8.4).
            //
            // A REPLAY RESTORE IS NOT A HARD SET, and this is where a rollback
            // used to lose the world: the impact solver parks a delayed push on
            // the core with the larger virt_mass - the plate, not the ball - and
            // zeroing it costs a hinged plate 1.6-3.6 cm per rewind against a
            // floor of zero.  Such a caller restores the queue afterwards with
            // set_core_extra, which is sound precisely because nothing below
            // this line touches it again.
            core->speed_change.set_to_zero();
            core->rot_speed_change.set_to_zero();
            switch (action) {
                case sleep_action::legacy_wake: real->ensure_in_simulation(); break;
                case sleep_action::wake_if_frozen:
                    if (!IVP_MTIS_SIMULATED(core->movement_state)) real->ensure_in_simulation();
                    break;
                case sleep_action::freeze_if_simulated:
                    if (IVP_MTIS_SIMULATED(core->movement_state)) real->disable_simulation();
                    break;
                case sleep_action::keep: break;
            }
            // Same refresh the manager does after every step, so the render side
            // follows a body that was moved between ticks.
            CKIpionManager::UpdateObjectWorldMatrix(real);
            return true;
        }
    }

    bool set_body_state(CKIpionManager* physics, const char* entity_name,
                        const double position[3], const double rotation[4],
                        const float linear[3], const float angular[3], bool wake,
                        std::string& error, bool recheck) {
        // Byte for byte what this entry has always done: the unconditional
        // wake, and no leapfrog restore.
        return write_body_state(physics, entity_name, position, rotation, linear, angular,
                                wake ? sleep_action::legacy_wake : sleep_action::freeze_if_simulated,
                                recheck, false, error);
    }

    bool set_body_state(CKIpionManager* physics, const char* entity_name,
                        const double position[3], const double rotation[4],
                        const float linear[3], const float angular[3], wake_mode mode,
                        bool recheck, std::string& error) {
        sleep_action action = sleep_action::keep;
        if (mode == wake_mode::wake) action = sleep_action::wake_if_frozen;
        else if (mode == wake_mode::freeze) action = sleep_action::freeze_if_simulated;
        return write_body_state(physics, entity_name, position, rotation, linear, angular,
                                action, recheck, true, error);
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

    bool set_body_matrix(CKIpionManager* physics, const char* entity_name, const float matrix[16], bool simulated,
                         std::string& error) {
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
        PhysicsObject* object = physics->GetPhysicsObject(entity);
        if (!object || !object->m_RealObject) {
            error = "'" + name + "' has no body";
            return false;
        }
        VxMatrix world;
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 4; ++k) world[r][k] = matrix[r * 4 + k];
        entity->SetWorldMatrix(world);
        // The creation's own derivation of the pose from the matrix, so the
        // quaternion is the one a Physicalize would have given the body.
        IVP_Template_Real_Object templ;
        IVP_U_Point position;
        IVP_U_Quat quaternion;
        IVP_U_Matrix mass_center;
        CKIpionManager::FillTemplateInfo(&templ, &position, &quaternion, const_cast<CKSTRING>(name.c_str()), 1.0f, nullptr,
                                         0.0f, 0.0f, entity, FALSE, const_cast<CKSTRING>(""), &mass_center, nullptr);
        const double pos[3] = {position.k[0], position.k[1], position.k[2]};
        const double rot[4] = {quaternion.x, quaternion.y, quaternion.z, quaternion.w};
        const float still[3] = {0.0f, 0.0f, 0.0f};
        return set_body_state(physics, name.c_str(), pos, rot, still, still, simulated ? wake_mode::wake : wake_mode::freeze,
                              true, error);
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
        // The exempt entities (engine change #13) are named by
        // restore_explosion_pieces, which runs at every anchor - i.e. before
        // the guard goes on again; ids of a level that is being left behind
        // do not outlive the guard they belonged to.
        if (!enable) physics->m_KeepLevelBodiesFree.Clear();
        return true;
    }

    namespace {
        // The core behind a name, or nullptr with `error` set.  Shared by the
        // two core_extra entries, whose only job is to move six floats.
        IVP_Core* core_for(CKIpionManager* physics, const char* entity_name, std::string& error) {
            CK3dEntity* entity = find_entity(physics, bounded(entity_name, BMMO_PHYSICS_NAME_SIZE));
            PhysicsObject* object = entity ? physics->GetPhysicsObject(entity) : nullptr;
            IVP_Real_Object* real = object ? object->m_RealObject : nullptr;
            IVP_Core* core = real ? real->get_core() : nullptr;
            if (!core) error = "no physics body named " + std::string(entity_name ? entity_name : "");
            return core;
        }
    }

    bool get_core_extra(CKIpionManager* physics, const char* entity_name,
                        bmmo_physics_core_extra& out, std::string& error) {
        error.clear();
        out = bmmo_physics_core_extra{};
        IVP_Core* core = core_for(physics, entity_name, error);
        if (!core) return false;
        out.fields = BMMO_PHYSICS_EXTRA_IMPULSES | BMMO_PHYSICS_EXTRA_QUATS | BMMO_PHYSICS_EXTRA_CALM;
        for (int k = 0; k < 3; ++k) {
            out.speed_change[k] = static_cast<float>(core->speed_change.k[k]);
            out.rot_speed_change[k] = static_cast<float>(core->rot_speed_change.k[k]);
        }
        // v13: the freeze bookkeeping calc_movement_state reads (see the
        // struct's comment).
        out.movement_state = static_cast<int32_t>(core->movement_state);
        for (int k = 0; k < 2; ++k) {
            out.calm_time[k] = core->time_of_calm_reference[k].get_seconds();
            const auto& q = core->q_world_f_core_calm_reference[k];
            out.calm_quat[k][0] = q.x; out.calm_quat[k][1] = q.y; out.calm_quat[k][2] = q.z; out.calm_quat[k][3] = q.w;
            const auto& p = core->position_world_f_core_calm_reference[k];
            for (int j = 0; j < 3; ++j) out.calm_position[k][j] = p.k[j];
        }
#ifdef IVP_PORTABLE_BMMO
        if (core->sim_unit_of_core) {
            out.fields |= BMMO_PHYSICS_EXTRA_UNIT;
            out.unit_next_movement_check = core->sim_unit_of_core->next_movement_check;
        }
#endif
        const auto& ql = core->q_world_f_core_last_psi;
        const auto& qn = core->q_world_f_core_next_psi;
        out.q_last_psi[0] = ql.x; out.q_last_psi[1] = ql.y;
        out.q_last_psi[2] = ql.z; out.q_last_psi[3] = ql.w;
        out.q_next_psi[0] = qn.x; out.q_next_psi[1] = qn.y;
        out.q_next_psi[2] = qn.z; out.q_next_psi[3] = qn.w;
        return true;
    }

    bool set_core_extra(CKIpionManager* physics, const char* entity_name,
                        const bmmo_physics_core_extra& in, std::string& error) {
        error.clear();
        IVP_Core* core = core_for(physics, entity_name, error);
        if (!core) return false;
        if (in.fields & BMMO_PHYSICS_EXTRA_IMPULSES) {
            core->speed_change.set(in.speed_change[0], in.speed_change[1], in.speed_change[2]);
            core->rot_speed_change.set(in.rot_speed_change[0], in.rot_speed_change[1], in.rot_speed_change[2]);
        }
        if (in.fields & BMMO_PHYSICS_EXTRA_QUATS) {
            // Both halves of the rotation, replacing whatever
            // write_body_state's restore_psi_state path re-derived from the
            // live rotation step.  The matrix is refreshed the same way that
            // path does it: the IVP_U_Matrix3 overload of set_matrix writes
            // only the 3x3, so the position the beam just set is preserved.
            // Member-wise, like write_body_state: IVP_U_Quat::set takes three
            // Euler angles, not four components.
            auto& ql = core->q_world_f_core_last_psi;
            auto& qn = core->q_world_f_core_next_psi;
            ql.x = in.q_last_psi[0]; ql.y = in.q_last_psi[1];
            ql.z = in.q_last_psi[2]; ql.w = in.q_last_psi[3];
            qn.x = in.q_next_psi[0]; qn.y = in.q_next_psi[1];
            qn.z = in.q_next_psi[2]; qn.w = in.q_next_psi[3];
            core->q_world_f_core_last_psi.set_matrix(&core->m_world_f_core_last_psi);
        }
        if (in.fields & BMMO_PHYSICS_EXTRA_CALM) {
            // The references the next movement check compares against, as the
            // record has them.  Not movement_state: the sleep state goes
            // through the wake modes of write_body_state, which keep the
            // unit manager's lists consistent.
            for (int k = 0; k < 2; ++k) {
                core->time_of_calm_reference[k] = IVP_Time(in.calm_time[k]);
                auto& q = core->q_world_f_core_calm_reference[k];
                q.x = static_cast<float>(in.calm_quat[k][0]); q.y = static_cast<float>(in.calm_quat[k][1]);
                q.z = static_cast<float>(in.calm_quat[k][2]); q.w = static_cast<float>(in.calm_quat[k][3]);
                auto& p = core->position_world_f_core_calm_reference[k];
                for (int j = 0; j < 3; ++j) p.k[j] = static_cast<float>(in.calm_position[k][j]);
            }
        }
#ifdef IVP_PORTABLE_BMMO
        if ((in.fields & BMMO_PHYSICS_EXTRA_UNIT) && core->sim_unit_of_core)
            core->sim_unit_of_core->next_movement_check = static_cast<short>(in.unit_next_movement_check);
#endif
        return true;
    }

    // The two helpers live next to the controller class, in
    // physics_RT/Behaviors/PhysicsForce.cpp (the same library this bridge is
    // built into, as a plugin and as the headless static library).
    extern "C" int BmmoPhysicsForceBlockState(CKBehavior* beh, int* exists, double pos[3], double force[3]);
    extern "C" int BmmoPhysicsForceBlockRestore(CKBehavior* beh, int exists, const double pos[3], const double force[3]);

    bool force_block_get(CKIpionManager* physics, uint32_t behavior_id, bmmo_physics_force_block& out, std::string& error) {
        error.clear();
        out = bmmo_physics_force_block{};
        CKContext* context = physics ? physics->m_Context : nullptr;
        auto* beh = context ? CKBehavior::Cast(context->GetObject(static_cast<CK_ID>(behavior_id))) : nullptr;
        if (!beh) {
            error = "no behavior " + std::to_string(behavior_id);
            return false;
        }
        int exists = 0;
        if (!BmmoPhysicsForceBlockState(beh, &exists, out.position, out.force)) {
            error = "not a force block state";
            return false;
        }
        out.exists = exists;
        return true;
    }

    bool force_block_set(CKIpionManager* physics, uint32_t behavior_id, const bmmo_physics_force_block& in, std::string& error) {
        error.clear();
        CKContext* context = physics ? physics->m_Context : nullptr;
        auto* beh = context ? CKBehavior::Cast(context->GetObject(static_cast<CK_ID>(behavior_id))) : nullptr;
        if (!beh) {
            error = "no behavior " + std::to_string(behavior_id);
            return false;
        }
        if (!BmmoPhysicsForceBlockRestore(beh, in.exists, in.position, in.force)) {
            error = "the force block's body is not physicalized";
            return false;
        }
        return true;
    }

    // ------------------------------------------------------------------
    // v27 (step RLC): a joint block's constraint as an image.
    //
    // Set Physics Hinge, Physics Ball Joint and Set Physics Slider keep the
    // IVP_Constraint_Local their callback made in local 0.  Its fields are
    // fixed when it is made: init() turns the world-space anchor and axes
    // into each core's space with the bodies' poses of that moment, and
    // nothing the blocks call changes them afterwards.  A Shutdown inside a
    // rollback window deletes it, and nothing at the anchor's poses can
    // compute it again - so the script replay keeps the image of every
    // constraint it sees (game/script_replay.hpp joint ledger) and a restore
    // to a tick at which the block held one makes a constraint on the same
    // two bodies and gives it every field of the image.  Transient fields
    // are left out of the image: the FIXED bit do_simulation_controller sets
    // and clears on a LIMITED axis every step, and matrix_size, which it
    // recomputes before any use; the restore writes them as init() leaves
    // them.  The image is this build's own bytes and never leaves the
    // process.  No IVP change: the private fields are reached through the
    // explicit-instantiation shim (ivp_private_access.hpp's thief).
    // ------------------------------------------------------------------
}

namespace bmmo::physics::ivp_access {
    using cl_float6 = IVP_FLOAT[IVP_TR_INDEX_MAX];
    using cl_axis6 = IVP_CONSTRAINT_AXIS_TYPE[IVP_TR_INDEX_MAX];
    struct cl_force_factor { using type = IVP_FLOAT IVP_Constraint_Local::*; friend type steal(cl_force_factor); };
    struct cl_damp { using type = IVP_FLOAT IVP_Constraint_Local::*; friend type steal(cl_damp); };
    struct cl_fixed { using type = cl_axis6 IVP_Constraint_Local::*; friend type steal(cl_fixed); };
    struct cl_left { using type = cl_float6 IVP_Constraint_Local::*; friend type steal(cl_left); };
    struct cl_right { using type = cl_float6 IVP_Constraint_Local::*; friend type steal(cl_right); };
    struct cl_stiffness { using type = IVP_FLOAT IVP_Constraint_Local::*; friend type steal(cl_stiffness); };
    struct cl_maxforce { using type = IVP_Constraint_Local_MaxImpulse* IVP_Constraint_Local::*; friend type steal(cl_maxforce); };
    struct cl_anchor_r { using type = IVP_Constraint_Local_Anchor IVP_Constraint_Local::*; friend type steal(cl_anchor_r); };
    struct cl_anchor_a { using type = IVP_Constraint_Local_Anchor IVP_Constraint_Local::*; friend type steal(cl_anchor_a); };
    struct cl_map_t { using type = IVP_U_Mapping IVP_Constraint_Local::*; friend type steal(cl_map_t); };
    struct cl_map_r { using type = IVP_U_Mapping IVP_Constraint_Local::*; friend type steal(cl_map_r); };
    struct cl_ftd { using type = unsigned char IVP_Constraint_Local::*; friend type steal(cl_ftd); };
    struct cl_frd { using type = unsigned char IVP_Constraint_Local::*; friend type steal(cl_frd); };
    struct cl_ltd { using type = unsigned char IVP_Constraint_Local::*; friend type steal(cl_ltd); };
    struct cl_lrd { using type = unsigned char IVP_Constraint_Local::*; friend type steal(cl_lrd); };
    struct cl_msize { using type = unsigned char IVP_Constraint_Local::*; friend type steal(cl_msize); };

    template struct thief<cl_force_factor, &IVP_Constraint_Local::force_factor>;
    template struct thief<cl_damp, &IVP_Constraint_Local::damp_factor_div_force>;
    template struct thief<cl_fixed, &IVP_Constraint_Local::fixed>;
    template struct thief<cl_left, &IVP_Constraint_Local::borderleft_Rfs>;
    template struct thief<cl_right, &IVP_Constraint_Local::borderright_Rfs>;
    template struct thief<cl_stiffness, &IVP_Constraint_Local::limited_axis_stiffness>;
    template struct thief<cl_maxforce, &IVP_Constraint_Local::maxforce>;
    template struct thief<cl_anchor_r, &IVP_Constraint_Local::m_Rfs_f_Rcs>;
    template struct thief<cl_anchor_a, &IVP_Constraint_Local::m_Afs_f_Acs>;
    template struct thief<cl_map_t, &IVP_Constraint_Local::mapping_uRfs_f_Rfs>;
    template struct thief<cl_map_r, &IVP_Constraint_Local::mapping_uRrs_f_Rrs>;
    template struct thief<cl_ftd, &IVP_Constraint_Local::fixedtrans_dim>;
    template struct thief<cl_frd, &IVP_Constraint_Local::fixedrot_dim>;
    template struct thief<cl_ltd, &IVP_Constraint_Local::limitedtrans_dim>;
    template struct thief<cl_lrd, &IVP_Constraint_Local::limitedrot_dim>;
    template struct thief<cl_msize, &IVP_Constraint_Local::matrix_size>;
}

namespace bmmo::physics {
    namespace {
        constexpr uint8_t kJointImageVersion = 1;

        // The blocks whose local 0 is an IVP_Constraint (Set Physics Spring
        // keeps an IVP_Actuator_Spring: not one of them).
        bool is_constraint_joint(CKBehavior* beh) {
            if (!beh || !beh->IsUsingFunction()) return false;
            const CKGUID g = beh->GetPrototypeGuid();
            return g == CKGUID(0x5e624f0a, 0x35160450) || g == CKGUID(0x41cd3653, 0x0de60c1d)
                || g == CKGUID(0x2973360e, 0x23d31aa7);
        }

        // The block's two bodies: role 1 its target, role 2 its Object2
        // (input 0 of all three blocks).
        CK3dEntity* joint_entity(CKBehavior* beh, uint8_t role) {
            if (role == 1) return static_cast<CK3dEntity*>(beh->GetTarget());
            if (role == 2 && beh->GetInputParameterCount() > 0)
                return static_cast<CK3dEntity*>(beh->GetInputParameterObject(0));
            return nullptr;
        }
        IVP_Real_Object* joint_body(CKIpionManager* physics, CKBehavior* beh, uint8_t role) {
            CK3dEntity* entity = joint_entity(beh, role);
            if (!entity) return nullptr;
            PhysicsObject* po = physics->GetPhysicsObject(entity, FALSE);
            return po ? po->m_RealObject : nullptr;
        }
        // The CK id of a role's entity (0: none), in the image: a block id a
        // later level reuses never gets an image of other bodies (step RLC's
        // review; the ledger is the process's).
        int32_t joint_entity_id(CKBehavior* beh, uint8_t role) {
            CK3dEntity* entity = role ? joint_entity(beh, role) : nullptr;
            return entity ? static_cast<int32_t>(entity->GetID()) : 0;
        }

        struct image_writer {
            bmmo_physics_joint_image& out;
            bool overflow = false;
            void put(const void* p, size_t n) {
                if (out.size + n > sizeof(out.bytes)) { overflow = true; return; }
                std::memcpy(out.bytes + out.size, p, n);
                out.size += static_cast<uint32_t>(n);
            }
            void u8(uint8_t v) { put(&v, 1); }
            void i32(int32_t v) { put(&v, 4); }
            void f(IVP_FLOAT v) { put(&v, sizeof(v)); }
            void d(double v) { put(&v, 8); }
        };
        struct image_reader {
            const bmmo_physics_joint_image& in;
            size_t pos = 0;
            bool bad = false;
            void get(void* p, size_t n) {
                if (bad || pos + n > in.size || in.size > sizeof(in.bytes)) { bad = true; std::memset(p, 0, n); return; }
                std::memcpy(p, in.bytes + pos, n);
                pos += n;
            }
            uint8_t u8() { uint8_t v = 0; get(&v, 1); return v; }
            int32_t i32() { int32_t v = 0; get(&v, 4); return v; }
            IVP_FLOAT f() { IVP_FLOAT v = 0; get(&v, sizeof(v)); return v; }
            double d() { double v = 0; get(&v, 8); return v; }
        };

        void put_matrix3(image_writer& w, const IVP_U_Matrix3& m) {
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) w.d(static_cast<double>(m.get_elem(r, c)));
        }
        void get_matrix3(image_reader& rd, IVP_U_Matrix3& m) {
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) m.set_elem(r, c, static_cast<IVP_DOUBLE>(rd.d()));
        }
        void put_anchor(image_writer& w, const IVP_Constraint_Local_Anchor& a) {
            put_matrix3(w, a);
            for (int k = 0; k < 3; ++k) w.d(static_cast<double>(a.vv.k[k]));
            w.u8(a.rot ? 1 : 0);
            if (a.rot) put_matrix3(w, *a.rot);
        }
        void get_anchor(image_reader& rd, IVP_Constraint_Local_Anchor& a) {
            get_matrix3(rd, a);
            for (int k = 0; k < 3; ++k) a.vv.k[k] = static_cast<decltype(+a.vv.k[0])>(rd.d());
            if (rd.u8()) {
                if (!a.rot) a.rot = new IVP_U_Matrix3;
                get_matrix3(rd, *a.rot);
            } else if (a.rot) {
                P_DELETE(a.rot);
                a.rot = nullptr;
            }
        }
        // A LIMITED axis without the FIXED bit do_simulation_controller
        // sets and clears on it every step (the DUMMY marker kept as is).
        int32_t steady_axis(IVP_CONSTRAINT_AXIS_TYPE t) {
            const int32_t v = static_cast<int32_t>(t);
            if (t == IVP_CONSTRAINT_AXIS_DUMMY) return v;
            return (v & IVP_CONSTRAINT_AXIS_LIMITED) ? static_cast<int32_t>(IVP_CONSTRAINT_AXIS_LIMITED) : v;
        }
        CKBehavior* behavior_of(CKIpionManager* physics, uint32_t behavior_id, std::string& error) {
            CKContext* context = physics ? physics->m_Context : nullptr;
            auto* beh = context ? CKBehavior::Cast(context->GetObject(static_cast<CK_ID>(behavior_id))) : nullptr;
            if (!beh) error = "no behavior " + std::to_string(behavior_id);
            else if (!is_constraint_joint(beh)) { error = "not a constraint joint block"; beh = nullptr; }
            return beh;
        }

        // The key the savestate blob matches controllers by (priority and
        // name; physics_savestate_restore.cpp order_controllers).
        bool same_key(IVP_Controller* x, IVP_Controller* c) {
            if (!x || !c || x->get_controller_priority() != c->get_controller_priority()) return false;
            const char* a = x->get_controller_name();
            const char* b = c->get_controller_name();
            return (a && b) ? std::strcmp(a, b) == 0 : a == b;
        }
        // The block's constraint, found among its bodies' cores' controllers
        // and never by dereferencing local 0 on its own: a body a script
        // Dephysicalizes deletes its constraint (core_is_going_to_be_deleted)
        // without the block's handle being nulled (step RLC's review).
        IVP_Constraint* held_constraint(CKIpionManager* physics, CKBehavior* beh) {
            IVP_Constraint* handle = nullptr;
            beh->GetLocalParameterValue(0, &handle);
            if (!handle) return nullptr;
            for (uint8_t role = 1; role <= 2; ++role) {
                IVP_Real_Object* object = joint_body(physics, beh, role);
                IVP_Core* core = object ? object->get_core() : nullptr;
                if (!core) continue;
                for (int i = 0; i < core->controllers_of_core.len(); ++i) {
                    auto* constraint = dynamic_cast<IVP_Constraint*>(core->controllers_of_core.element_at(i));
                    if (constraint && constraint == handle) return constraint;
                }
            }
            return nullptr;
        }
        // `c`'s place in `core`'s controllers, counted among those of its key.
        int16_t place_in(IVP_Core* core, IVP_Controller* c) {
            if (!core) return -1;
            int16_t place = 0;
            for (int i = 0; i < core->controllers_of_core.len(); ++i) {
                IVP_Controller* x = core->controllers_of_core.element_at(i);
                if (x == c) return place;
                if (same_key(x, c)) ++place;
            }
            return -1;
        }
    }

    bool joint_block_get(CKIpionManager* physics, uint32_t behavior_id, bmmo_physics_joint_image& out, std::string& error) {
        namespace access = bmmo::physics::ivp_access;
        error.clear();
        out.size = 0;
        out.order[0] = out.order[1] = -1;
        CKBehavior* beh = behavior_of(physics, behavior_id, error);
        if (!beh) return false;
        // No image for a handle none of its bodies' cores holds (none, or a
        // stale one): the record keeps the presence byte alone.
        IVP_Constraint* handle = held_constraint(physics, beh);
        if (!handle) return true;
        auto* c = dynamic_cast<IVP_Constraint_Local*>(handle);
        if (!c) {
            error = "the block's constraint is not an IVP_Constraint_Local";
            return false;
        }
        if (c->*steal(access::cl_maxforce())) {
            error = "a constraint with a maximum impulse is not imaged";
            return false;
        }
        const IVP_Constraint_Local_Anchor& ar = c->*steal(access::cl_anchor_r());
        const IVP_Constraint_Local_Anchor& aa = c->*steal(access::cl_anchor_a());
        auto role_of = [&](IVP_Real_Object* object, uint8_t& role) {
            role = 0;
            if (!object) return true;
            for (uint8_t r = 1; r <= 2; ++r)
                if (joint_body(physics, beh, r) == object) { role = r; return true; }
            return false;
        };
        uint8_t role_r = 0, role_a = 0;
        if (!role_of(ar.object, role_r) || !role_of(aa.object, role_a)) {
            error = "a body of the constraint is neither the block's target nor its Object2";
            return false;
        }
        image_writer w{out};
        w.u8(kJointImageVersion);
        w.u8(role_r);
        w.u8(role_a);
        w.i32(joint_entity_id(beh, role_r));
        w.i32(joint_entity_id(beh, role_a));
        const IVP_CONSTRAINT_AXIS_TYPE* fixed = c->*steal(access::cl_fixed());
        const IVP_FLOAT* left = c->*steal(access::cl_left());
        const IVP_FLOAT* right = c->*steal(access::cl_right());
        for (int i = 0; i < IVP_TR_INDEX_MAX; ++i) w.i32(steady_axis(fixed[i]));
        for (int i = 0; i < IVP_TR_INDEX_MAX; ++i) w.f(left[i]);
        for (int i = 0; i < IVP_TR_INDEX_MAX; ++i) w.f(right[i]);
        w.f(c->*steal(access::cl_force_factor()));
        w.f(c->*steal(access::cl_damp()));
        w.f(c->*steal(access::cl_stiffness()));
        put_anchor(w, ar);
        put_anchor(w, aa);
        const IVP_U_Mapping& mt = c->*steal(access::cl_map_t());
        const IVP_U_Mapping& mr = c->*steal(access::cl_map_r());
        for (int k = 0; k < 3; ++k) w.u8(static_cast<uint8_t>(mt.k[k]));
        for (int k = 0; k < 3; ++k) w.u8(static_cast<uint8_t>(mr.k[k]));
        w.u8(c->*steal(access::cl_ftd()));
        w.u8(c->*steal(access::cl_frd()));
        w.u8(c->*steal(access::cl_ltd()));
        w.u8(c->*steal(access::cl_lrd()));
        if (w.overflow) {
            out.size = 0;
            error = "the constraint's image does not fit";
            return false;
        }
        out.order[0] = ar.object ? place_in(ar.object->get_core(), c) : static_cast<int16_t>(-1);
        out.order[1] = aa.object ? place_in(aa.object->get_core(), c) : static_cast<int16_t>(-1);
        return true;
    }

    bool joint_block_set(CKIpionManager* physics, uint32_t behavior_id, const bmmo_physics_joint_image& in, std::string& error) {
        namespace access = bmmo::physics::ivp_access;
        error.clear();
        CKBehavior* beh = behavior_of(physics, behavior_id, error);
        if (!beh) return false;
        IVP_Constraint* handle = nullptr;
        beh->GetLocalParameterValue(0, &handle);
        if (handle) {
            error = "the block holds a constraint";
            return false;
        }
        if (!physics->GetEnvironment()) {
            error = "physics environment is unavailable";
            return false;
        }
        {
            // The whole image read once before anything is made (step RLC's
            // review: CreateConstraint links the cores and units at once).
            image_reader dry{in};
            const bool version = dry.u8() == kJointImageVersion;
            for (int i = 0; i < 2; ++i) dry.u8();
            for (int i = 0; i < 2; ++i) dry.i32();
            for (int i = 0; i < IVP_TR_INDEX_MAX; ++i) dry.i32();
            for (int i = 0; i < 2 * IVP_TR_INDEX_MAX + 3; ++i) dry.f();
            for (int a = 0; a < 2; ++a) {
                for (int i = 0; i < 12; ++i) dry.d();
                if (dry.u8())
                    for (int i = 0; i < 9; ++i) dry.d();
            }
            for (int i = 0; i < 10; ++i) dry.u8();
            if (!version || dry.bad || dry.pos != in.size) {
                error = version ? "a truncated or overlong joint image" : "not a joint image of this build";
                return false;
            }
        }
        image_reader rd{in};
        if (rd.u8() != kJointImageVersion || rd.bad) {
            error = "not a joint image of this build";
            return false;
        }
        const uint8_t role_r = rd.u8(), role_a = rd.u8();
        const int32_t id_r = rd.i32(), id_a = rd.i32();
        if (id_r != joint_entity_id(beh, role_r) || id_a != joint_entity_id(beh, role_a)) {
            error = "the image is of other bodies than the block's";
            return false;
        }
        IVP_Real_Object* object_r = role_r ? joint_body(physics, beh, role_r) : nullptr;
        IVP_Real_Object* object_a = role_a ? joint_body(physics, beh, role_a) : nullptr;
        if ((role_r && !object_r) || (role_a && !object_a) || (!object_r && !object_a)) {
            error = "a body of the constraint is not physicalized";
            return false;
        }
        // The objects alone decide what the constructor links (the cores of
        // the constraint system, the unit fusion); every field it computed
        // from the poses of now is written over below.
        IVP_Template_Constraint tmpl;
        tmpl.objectR = object_r;
        tmpl.objectA = object_a;
        IVP_Constraint* made = physics->CreateConstraint(&tmpl);
        auto* c = dynamic_cast<IVP_Constraint_Local*>(made);
        if (!c) {
            if (made) delete made;
            error = "the constraint could not be made";
            return false;
        }
        IVP_CONSTRAINT_AXIS_TYPE* fixed = c->*steal(access::cl_fixed());
        IVP_FLOAT* left = c->*steal(access::cl_left());
        IVP_FLOAT* right = c->*steal(access::cl_right());
        for (int i = 0; i < IVP_TR_INDEX_MAX; ++i) fixed[i] = static_cast<IVP_CONSTRAINT_AXIS_TYPE>(rd.i32());
        for (int i = 0; i < IVP_TR_INDEX_MAX; ++i) left[i] = rd.f();
        for (int i = 0; i < IVP_TR_INDEX_MAX; ++i) right[i] = rd.f();
        c->*steal(access::cl_force_factor()) = rd.f();
        c->*steal(access::cl_damp()) = rd.f();
        c->*steal(access::cl_stiffness()) = rd.f();
        get_anchor(rd, c->*steal(access::cl_anchor_r()));
        get_anchor(rd, c->*steal(access::cl_anchor_a()));
        IVP_U_Mapping& mt = c->*steal(access::cl_map_t());
        IVP_U_Mapping& mr = c->*steal(access::cl_map_r());
        for (int k = 0; k < 3; ++k) mt.k[k] = static_cast<char>(rd.u8());
        for (int k = 0; k < 3; ++k) mr.k[k] = static_cast<char>(rd.u8());
        unsigned char& ftd = c->*steal(access::cl_ftd());
        unsigned char& frd = c->*steal(access::cl_frd());
        unsigned char& ltd = c->*steal(access::cl_ltd());
        unsigned char& lrd = c->*steal(access::cl_lrd());
        ftd = rd.u8();
        frd = rd.u8();
        ltd = rd.u8();
        lrd = rd.u8();
        c->*steal(access::cl_msize()) = static_cast<unsigned char>(ftd + frd + ltd + lrd);
        if (rd.bad || rd.pos != in.size) {
            delete c;
            error = "a truncated or overlong joint image";
            return false;
        }
        beh->SetLocalParameterValue(0, &made);
        return true;
    }

    bool joint_blocks_order(CKIpionManager* physics, const uint32_t* behavior_ids, const int16_t* orders, uint32_t count,
                            uint32_t& moved, std::string& error) {
        namespace access = bmmo::physics::ivp_access;
        error.clear();
        moved = 0;
        if (count && (!behavior_ids || !orders)) {
            error = "null argument";
            return false;
        }
        struct member {
            IVP_Controller* controller;
            int16_t place;
        };
        std::vector<std::pair<IVP_Core*, std::vector<member>>> lists;   // first seen first
        for (uint32_t i = 0; i < count; ++i) {
            CKBehavior* beh = behavior_of(physics, behavior_ids[i], error);
            if (!beh) return false;
            auto* c = dynamic_cast<IVP_Constraint_Local*>(held_constraint(physics, beh));
            if (!c) {
                error = "block " + std::to_string(behavior_ids[i]) + " holds no constraint to order";
                return false;
            }
            IVP_Real_Object* objects[2] = {(c->*steal(access::cl_anchor_r())).object,
                                           (c->*steal(access::cl_anchor_a())).object};
            for (int k = 0; k < 2; ++k) {
                const int16_t place = orders[2 * i + static_cast<uint32_t>(k)];
                IVP_Core* core = objects[k] ? objects[k]->get_core() : nullptr;
                if (place < 0 || !core) continue;
                auto it = std::find_if(lists.begin(), lists.end(), [core](const auto& l) { return l.first == core; });
                if (it == lists.end()) {
                    lists.emplace_back(core, std::vector<member>{});
                    it = std::prev(lists.end());
                }
                it->second.push_back(member{c, place});
            }
        }
        // Every list checked before any is changed.
        struct plan_entry {
            IVP_Core* core;
            std::vector<int> slots;                   // the places of the key's controllers in the list
            std::vector<IVP_Controller*> by_place;   // what each of them is to hold
        };
        std::vector<plan_entry> plan;
        for (const auto& [core, members]: lists) {
            IVP_Controller* key = members.front().controller;
            plan_entry entry{core, {}, std::vector<IVP_Controller*>(members.size(), nullptr)};
            for (int i = 0; i < core->controllers_of_core.len(); ++i)
                if (same_key(core->controllers_of_core.element_at(i), key)) entry.slots.push_back(i);
            for (const member& m: members) {
                const auto place = static_cast<size_t>(m.place);
                if (!same_key(m.controller, key) || place >= entry.by_place.size() || entry.by_place[place]) {
                    error = "the recorded places of a core's constraints are not 0.." + std::to_string(members.size() - 1);
                    return false;
                }
                entry.by_place[place] = m.controller;
            }
            if (entry.slots.size() != members.size()) {
                error = "a core holds " + std::to_string(entry.slots.size()) + " constraints of the key, "
                      + std::to_string(members.size()) + " of them ordered";
                return false;
            }
            for (int slot: entry.slots)
                if (std::find(entry.by_place.begin(), entry.by_place.end(), core->controllers_of_core.element_at(slot))
                        == entry.by_place.end()) {
                    error = "a core holds a constraint no ordered block holds";
                    return false;
                }
            plan.push_back(std::move(entry));
        }
        for (plan_entry& entry: plan) {
            IVP_U_Vector<IVP_Controller>& list = entry.core->controllers_of_core;
            bool changed = false;
            for (size_t j = 0; j < entry.slots.size(); ++j) {
                if (list.element_at(entry.slots[j]) == entry.by_place[j]) continue;
                for (size_t k = j + 1; k < entry.slots.size(); ++k)
                    if (list.element_at(entry.slots[k]) == entry.by_place[j]) {
                        list.swap_elems(entry.slots[j], entry.slots[k]);
                        changed = true;
                        break;
                    }
            }
            if (changed) ++moved;
        }
        return true;
    }

    bool get_clock_state(CKIpionManager* physics, bmmo_physics_clock_state& out, std::string& error) {
        error.clear();
        out = bmmo_physics_clock_state{};
        IVP_Environment* env = physics ? physics->GetEnvironment() : nullptr;
        if (!env) {
            error = "physics environment is unavailable";
            return false;
        }
        namespace access = bmmo::physics::ivp_access;
        IVP_Time_Manager* tm = access::time_manager(*env);
        out.current_time = env->get_current_time().get_seconds();
        out.time_of_last_psi = env->get_old_time_of_last_PSI().get_seconds();
        out.time_of_next_psi = env->get_next_PSI_time().get_seconds();
        out.base_time = tm->base_time.get_seconds();
        out.last_time = tm->last_time;
        out.next_movement_check = access::next_movement_check(*env);
        out.queued_events = tm->min_hash->counter;
        out.queue_min = tm->min_hash->has_elements() ? tm->min_hash->find_min_value() : 0.0f;
        return true;
    }

    bool set_clock_state(CKIpionManager* physics, const bmmo_physics_clock_state& in, std::string& error) {
        error.clear();
        IVP_Environment* env = physics ? physics->GetEnvironment() : nullptr;
        if (!env) {
            error = "physics environment is unavailable";
            return false;
        }
        namespace access = bmmo::physics::ivp_access;
        IVP_Time_Manager* tm = access::time_manager(*env);
        // Queued events are relative to base_time.  Moving the base alone
        // would move every event with it, so shift the values the other way
        // and each event stays at the absolute time it was scheduled for -
        // the same rebase IVP_Time_Manager::reset_time performs.
        const double shift = tm->base_time.get_seconds() - in.base_time;
        if (shift != 0.0 && tm->min_hash->has_elements()) {
            IVP_U_Min_List_Enumerator enumerator(tm->min_hash);
            while (IVP_U_Min_List_Element* h = enumerator.get_next_element_header())
                h->value += static_cast<IVP_U_MINLIST_FIXED_POINT>(shift);
            tm->min_hash->min_value += static_cast<IVP_U_MINLIST_FIXED_POINT>(shift);
        }
        tm->base_time = IVP_Time(in.base_time);
        tm->last_time = in.last_time;
        access::current_time(*env) = IVP_Time(in.current_time);
        access::time_of_last_psi(*env) = IVP_Time(in.time_of_last_psi);
        access::time_of_next_psi(*env) = IVP_Time(in.time_of_next_psi);
        access::next_movement_check(*env) = static_cast<short>(in.next_movement_check);
        // The PSI event is the one queued event whose time the record knows:
        // it is due at the anchor's next PSI, not at the live one.
        if (tm->psi_event && tm->psi_event->index != IVP_U_MINLIST_UNUSED)
            tm->update_event(tm->psi_event, IVP_Time(in.time_of_next_psi));
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

        // ---- bridge API v21 (docs/trafo-replication-design.md 2.4, 2.6,
        // 2.10; step T1): the per-player piece sets of game/trafo_sets.hpp.
        // One registry per physics manager: every registered set's three
        // piece frames (by type), the entities the body guard leaves alone,
        // the piece names the collision filter knows, and the seed registry
        // (frame -> the seed its next explosion draws from).  Nothing here
        // changes a world without registered sets: the originals keep today's
        // path, the filter is installed only when switched on.
        class trafo_piece_filter;
        struct trafo_bridge_set {
            uint32_t frames[3] = {};
            std::string frame_names[3];           // what each frame id named when registered (stale-entry check)
            std::vector<CK_ID> entities;          // the three frames and every piece below them
            std::vector<std::string> pieces;      // the pieces' names (their bodies' IVP names)
        };
        struct trafo_bridge_state {
            std::map<uint32_t, trafo_bridge_set> sets;    // by player
            std::map<CK_ID, int32_t> seeds;               // piece frame -> seed (an original's: tests only)
            std::map<std::string, uint32_t, std::less<>> pieces;   // piece name -> player (looked up by IVP's const char*)
            bool filter_enabled = false;
            std::vector<trafo_piece_filter*> filters;     // installed, per environment
        };
        // Never destroyed: an IVP environment (and its filter) may outlive
        // static storage at process exit.
        std::map<CKIpionManager*, trafo_bridge_state>& trafo_states() {
            static auto* states = new std::map<CKIpionManager*, trafo_bridge_state>();
            return *states;
        }
        trafo_bridge_state* find_trafo_state(CKIpionManager* physics) {
            auto& states = trafo_states();
            const auto it = states.find(physics);
            return it == states.end() ? nullptr : &it->second;
        }
        bool is_set_frame(const trafo_bridge_state& state, CK_ID frame) {
            for (const auto& [player, set]: state.sets)
                for (uint32_t f: set.frames)
                    if (f == frame) return true;
            return false;
        }
        // A registered set whose frames are gone, or whose ids now name other
        // objects (a world torn down without teardown_trafo_sets, an abort
        // path, a manager at a reused address, CK reusing the freed ids), is
        // dropped before the registry is read for a decision: a stale frame id
        // must never send an original's explosion down the per-set path (the
        // body guard would then swallow the reset's Unphysicalize, the
        // regression engine change #13 fixed) nor take an unrelated object
        // off the guard.  The exemption list is not touched here: every
        // reader that rebuilds it (restore_explosion_pieces) prunes first.
        void prune_stale_sets(CKIpionManager* physics, trafo_bridge_state& state) {
            CKContext* context = physics ? physics->m_Context : nullptr;
            for (auto it = state.sets.begin(); it != state.sets.end();) {
                bool alive = context != nullptr;
                for (int t = 0; alive && t < 3; ++t) {
                    CKObject* frame = context->GetObject(static_cast<CK_ID>(it->second.frames[t]));
                    alive = frame && CKIsChildClassOf(frame, CKCID_3DENTITY) && frame->GetName()
                         && it->second.frame_names[t] == frame->GetName();
                }
                if (alive) {
                    ++it;
                    continue;
                }
                for (const std::string& name: it->second.pieces) state.pieces.erase(name);
                for (uint32_t frame: it->second.frames) state.seeds.erase(static_cast<CK_ID>(frame));
                it = state.sets.erase(it);
            }
        }

        // The piece x local-body rule (design 2.10): while the trafo inputs
        // are on, a registered piece never collides with a body whose name
        // the resync's keep-local predicate names (the level-end balloon and
        // its airship, exception (c)).  Asked when a pair first becomes a
        // collision candidate, so it is a static pair rule; every other pair
        // gets IVP_TRUE, which the meta filter ANDs with the others' - no
        // other pair's result changes.
        class trafo_piece_filter : public IVP_Collision_Filter {
        public:
            CKIpionManager* physics = nullptr;
            IVP_Environment* environment = nullptr;
            IVP_BOOL check_objects_for_collision_detection(IVP_Real_Object* object0, IVP_Real_Object* object1) override {
                const trafo_bridge_state* state = find_trafo_state(physics);
                if (!state || !state->filter_enabled || state->pieces.empty()) return IVP_TRUE;
                const char* n0 = object0 ? object0->get_name() : nullptr;
                const char* n1 = object1 ? object1->get_name() : nullptr;
                if (!n0 || !n1) return IVP_TRUE;
                const bool piece0 = state->pieces.count(n0) != 0, piece1 = state->pieces.count(n1) != 0;
                if (piece0 && !piece1 && bmmo::session::keep_local_body(n1)) return IVP_FALSE;
                if (piece1 && !piece0 && bmmo::session::keep_local_body(n0)) return IVP_FALSE;
                return IVP_TRUE;
            }
            void environment_will_be_deleted(IVP_Environment*) override {
                if (trafo_bridge_state* state = find_trafo_state(physics))
                    state->filters.erase(std::remove(state->filters.begin(), state->filters.end(), this), state->filters.end());
                delete this;
            }
        };

        // Detached, squared and back at its initial conditions: one piece
        // frame and every piece below it, with their stale hulls forgotten
        // while they have no body - restore_explosion_pieces' work for one
        // frame (see there).  `exempt` gets the frame and its pieces.
        int restore_piece_frame(CKScene* scene, CKIpionManager* physics, CK3dEntity* frame, std::vector<CK_ID>* exempt) {
            int restored = 0;
            if (exempt) exempt->push_back(frame->GetID());
            restored += restore_initial_condition(scene, frame);
            frame->SetParent(nullptr, TRUE);
            VxMatrix world = frame->GetWorldMatrix();
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c) world[r][c] = r == c ? 1.0f : 0.0f;
            world[3][3] = 1.0f;
            frame->SetWorldMatrix(world);
            for (CK3dEntity* child = frame->HierarchyParser(nullptr); child; child = frame->HierarchyParser(child)) {
                restored += restore_initial_condition(scene, child);
                if (exempt) exempt->push_back(child->GetID());
                if (physics && !physics->GetPhysicsObject(child))
                    if (CKMesh* mesh = child->GetCurrentMesh())
                        if (mesh->GetName()) physics->RemoveCollisionSurface(mesh->GetName());
            }
            return restored;
        }

        // Every registered set's entities appended to the body guard's
        // exemption list (engine change #13), in player order.
        void append_set_exemptions(CKIpionManager* physics) {
            trafo_bridge_state* state = physics ? find_trafo_state(physics) : nullptr;
            if (!state) return;
            prune_stale_sets(physics, *state);
            for (const auto& [player, set]: state->sets)
                for (CK_ID id: set.entities)
                    if (!physics->m_KeepLevelBodiesFree.IsHere(id)) physics->m_KeepLevelBodiesFree.PushBack(id);
        }

        // The "Set Position" block (3DTrans) that opens every explosion
        // script: "Set Position (0,0,0, Referential = Ball_Pos_Frame)" on the
        // pieces frame.  Wrapped so the pieces start every explosion from an
        // exact pose (see restore_explosion_pieces).  A registered set's
        // frame (bridge v21) is restored alone (its referential is its own
        // BMMO_TrafoRef, not Ball_Pos_Frame); every other target takes the
        // originals' path as before.  Then, when the seed registry has an
        // entry for the target frame, the generator restarts from it: the
        // explosion's draws are a function of (seed, set) alone (design 2.6).
        const CKGUID kSetPositionGuid(0xe456e78a, 0x456789aa);
        CKBEHAVIORFCT g_set_position_original = nullptr;

        int explosion_set_position(const CKBehaviorContext& behcontext) {
            CKBehavior* beh = behcontext.Behavior;
            CKObject* target = beh ? beh->GetTarget() : nullptr;
            CKIpionManager* physics = CKIpionManager::GetManager(behcontext.Context);
            trafo_bridge_state* state = physics ? find_trafo_state(physics) : nullptr;
            if (state) prune_stale_sets(physics, *state);
            const CK_ID frame = target ? target->GetID() : 0;
            CK3dEntity* set_frame = state && frame && is_set_frame(*state, frame) ? CK3dEntity::Cast(target) : nullptr;
            CKScene* scene = behcontext.Context ? behcontext.Context->GetCurrentScene() : nullptr;
            if (set_frame && scene) restore_piece_frame(scene, physics, set_frame, nullptr);
            else restore_explosion_pieces(behcontext.Context);
            if (state && frame)
                if (const auto seed = state->seeds.find(frame); seed != state->seeds.end()) random_reset(seed->second);
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
        CKIpionManager* physics = CKIpionManager::GetManager(context);
        // Engine change #13: the pieces are the one set of bodies the retail
        // scripts create and destroy again inside a session - the explosion
        // physicalizes them, Ball_ResetPieces_<type> gives them back two
        // seconds later.  The body guard of engine change #6 would swallow
        // that Unphysicalize like a mechanism's, and the next explosion would
        // find the pieces still physicalized, keep the bodies of the previous
        // one and snap the pieces back to where they came to rest then (the
        // trafo before this one).  Name them here, where the piece hierarchy
        // is walked anyway, and the guard leaves them alone.
        if (physics) physics->m_KeepLevelBodiesFree.Clear();
        int restored = 0;
        for (const char* frame_name: kPieceFrames) {
            CK3dEntity* frame = find_entity(context, frame_name);
            if (!frame) continue;
            if (physics) physics->m_KeepLevelBodiesFree.PushBack(frame->GetID());
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
            for (CK3dEntity* child = frame->HierarchyParser(nullptr); child; child = frame->HierarchyParser(child)) {
                restored += restore_initial_condition(scene, child);
                if (physics) physics->m_KeepLevelBodiesFree.PushBack(child->GetID());
                // The piece's convex hull is compiled once, at the very first
                // Physicalize of the process - Balls_Init does one at game
                // start, long before this - and cached under the mesh's name.
                // It is built from the entity's scale, which CK derives from
                // the local matrix, which comes from a product with the
                // skewed parent above: the game's CK2 and the headless
                // reimplementation round three of the wood pieces
                // differently there, and the hull, its rotation inertia and
                // every later spin of that piece follow.  Forget the cached
                // hull while the piece has no body, so the next Physicalize
                // compiles it from the exact hierarchy installed here.
                if (physics && !physics->GetPhysicsObject(child))
                    if (CKMesh* mesh = child->GetCurrentMesh())
                        if (mesh->GetName()) physics->RemoveCollisionSurface(mesh->GetName());
            }
        }
        // Ball_Pos_Frame sits on its ball's origin; the file gives it a 5e-6
        // local offset that costs an inexact product per read.
        if (CK3dEntity* pos_frame = find_entity(context, "Ball_Pos_Frame"))
            if (pos_frame->GetParent()) pos_frame->SetLocalMatrix(VxMatrix::Identity());
        // Bridge v21: the list is rebuilt from the originals and every
        // registered trafo set (none outside a session with trafo inputs).
        if (physics) append_set_exemptions(physics);
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

    // ---- bridge API v21: the trafo sets (game/trafo_sets.hpp) ----

    namespace {
        // The blocks a set's explosions route through the bridge: Random
        // blocks through the generator, opening Set Position blocks through
        // the wrapper - counted as they stand after the patch (a copy may
        // carry the patched function already).
        void count_routed(CKBehavior* behavior, int& randoms, int& set_positions) {
            if (!behavior) return;
            if (behavior->IsUsingFunction()) {
                if (behavior->GetPrototypeGuid() == kRandomBlockGuid && behavior->GetFunction() == &random_block) ++randoms;
                if (behavior->GetPrototypeGuid() == kSetPositionGuid && behavior->GetFunction() == &explosion_set_position)
                    ++set_positions;
            }
            const int count = behavior->GetSubBehaviorCount();
            for (int i = 0; i < count; ++i) count_routed(behavior->GetSubBehavior(i), randoms, set_positions);
        }
    }

    bool trafo_set_register(CKIpionManager* physics, uint32_t player, const uint32_t frames[3], const uint32_t* explosions,
                            int explosion_count, trafo_set_report& out, std::string& error) {
        error.clear();
        out = trafo_set_report{};
        CKContext* context = physics ? physics->m_Context : nullptr;
        if (!context || !frames) {
            error = "the trafo set registry needs a physics manager with a context";
            return false;
        }
        if (!CKGetPrototypeFromGuid(kRandomBlockGuid)) {
            error = "the Random building block (Logics) is not registered";
            return false;
        }
        trafo_bridge_set set;
        for (int t = 0; t < 3; ++t) {
            auto* frame = CK3dEntity::Cast(context->GetObject(static_cast<CK_ID>(frames[t])));
            if (!frame) {
                error = "piece frame " + std::to_string(frames[t]) + " is not a 3D entity";
                return false;
            }
            set.frames[t] = frames[t];
            set.frame_names[t] = frame->GetName() ? frame->GetName() : "";
            set.entities.push_back(frame->GetID());
            for (CK3dEntity* child = frame->HierarchyParser(nullptr); child; child = frame->HierarchyParser(child)) {
                set.entities.push_back(child->GetID());
                if (child->GetName()) set.pieces.emplace_back(child->GetName());
            }
        }
        for (int i = 0; i < explosion_count; ++i) {
            auto* script = CKBehavior::Cast(context->GetObject(static_cast<CK_ID>(explosions ? explosions[i] : 0u)));
            if (!script) {
                error = "explosion script " + std::to_string(explosions ? explosions[i] : 0u) + " is not a behavior";
                return false;
            }
            patch_random_blocks(script);
            patch_explosion_placement(script);
            count_routed(script, out.randoms, out.set_positions);
        }
        trafo_bridge_state& state = trafo_states()[physics];
        prune_stale_sets(physics, state);
        // One registration per player: a second one would leave the first's
        // entities on the guard's exemption list and its frames' seeds behind
        // (the builder refuses an existing player, reselect unregisters first).
        if (state.sets.count(player)) {
            error = "player " + std::to_string(player) + " has a registered trafo set already";
            return false;
        }
        for (const std::string& name: set.pieces) state.pieces[name] = player;
        out.exempt = static_cast<int32_t>(set.entities.size());
        out.pieces = static_cast<int32_t>(set.pieces.size());
        state.sets[player] = std::move(set);
        append_set_exemptions(physics);
        return true;
    }

    bool trafo_set_unregister(CKIpionManager* physics, uint32_t player, std::string& error) {
        error.clear();
        trafo_bridge_state* state = physics ? find_trafo_state(physics) : nullptr;
        if (!state || !state->sets.count(player)) {
            error = "no trafo set registered for player " + std::to_string(player);
            return false;
        }
        const auto it = state->sets.find(player);
        for (const std::string& name: it->second.pieces) state->pieces.erase(name);
        for (uint32_t frame: it->second.frames) state->seeds.erase(static_cast<CK_ID>(frame));
        const std::vector<CK_ID> entities = it->second.entities;
        state->sets.erase(it);
        // Off the guard's exemption list (the originals' and the other sets'
        // entries stay where they are).
        for (CK_ID id: entities) physics->m_KeepLevelBodiesFree.Remove(id);
        // The manager's entry goes with its last set while no filter is
        // installed (a later manager at the same address starts clean).
        if (state->sets.empty() && state->filters.empty()) trafo_states().erase(physics);
        return true;
    }

    void trafo_set_seed(CKIpionManager* physics, uint32_t frame_id, int32_t seed) {
        if (!physics) return;
        trafo_bridge_state& state = trafo_states()[physics];
        if (seed == 0) state.seeds.erase(static_cast<CK_ID>(frame_id));
        else state.seeds[static_cast<CK_ID>(frame_id)] = seed;
    }

    int trafo_set_unmake(CKIpionManager* physics, uint32_t frame_id, std::string& error) {
        error.clear();
        CKContext* context = physics ? physics->m_Context : nullptr;
        CKScene* scene = context ? context->GetCurrentScene() : nullptr;
        auto* frame = context ? CK3dEntity::Cast(context->GetObject(static_cast<CK_ID>(frame_id))) : nullptr;
        if (!frame || !scene) {
            error = "piece frame " + std::to_string(frame_id) + " is not a 3D entity of the current scene";
            return -1;
        }
        const trafo_bridge_state* state = find_trafo_state(physics);
        if (!state || !is_set_frame(*state, frame->GetID())) {
            error = "piece frame " + std::to_string(frame_id) + " belongs to no registered trafo set";
            return -1;
        }
        int removed = 0;
        for (CK3dEntity* child = frame->HierarchyParser(nullptr); child; child = frame->HierarchyParser(child)) {
            PhysicsObject* object = physics->GetPhysicsObject(child);
            if (!object || !object->m_RealObject) continue;
            ++physics->m_DePhysicalizeCalls;
            object->m_RealObject->delete_silently();
            ++removed;
        }
        restore_piece_frame(scene, physics, frame, nullptr);
        return removed;
    }

    bool trafo_set_filter(CKIpionManager* physics, bool enable, std::string& error) {
        error.clear();
        IVP_Environment* environment = physics ? physics->GetEnvironment() : nullptr;
        if (!physics) {
            error = "no physics manager";
            return false;
        }
        trafo_bridge_state& state = trafo_states()[physics];
        state.filter_enabled = enable;
        if (!enable) return true;
        if (!environment) {
            error = "physics environment is unavailable";
            return false;
        }
        for (trafo_piece_filter* installed: state.filters)
            if (installed->environment == environment) return true;   // idempotent per environment
        auto* meta = static_cast<IVP_Meta_Collision_Filter*>(environment->get_collision_filter());
        if (!meta) {
            error = "the environment has no collision filter";
            return false;
        }
        auto* filter = new trafo_piece_filter();
        filter->physics = physics;
        filter->environment = environment;
        meta->add_collision_filter(filter);
        state.filters.push_back(filter);
        return true;
    }

    std::string describe_trafo_sets(CKIpionManager* physics) {
        const trafo_bridge_state* state = physics ? find_trafo_state(physics) : nullptr;
        if (!state) return "no trafo sets";
        std::string out = "sets=" + std::to_string(state->sets.size()) + " pieces=" + std::to_string(state->pieces.size())
                        + " seeds=" + std::to_string(state->seeds.size()) + " filter=" + (state->filter_enabled ? "on" : "off")
                        + " exempt=" + std::to_string(physics->m_KeepLevelBodiesFree.Size());
        for (const auto& [player, set]: state->sets)
            out += " | " + std::to_string(player) + ": frames " + std::to_string(set.frames[0]) + "/"
                 + std::to_string(set.frames[1]) + "/" + std::to_string(set.frames[2]) + ", "
                 + std::to_string(set.entities.size()) + " entities";
        return out;
    }

    // ---- bridge API v24: the airship zone (step BALA) ----

    namespace {
        // The player balls' nocoll group prefix every host installs
        // (install_player_collision_filter "P#").
        constexpr const char* kAirshipPlayerPrefix = "P#";
        // The margins around the walkable parts' box (the user's rule of
        // 2026-10-28: the planks sag 3.2 m and the platform sinks 1.7 m under
        // the own ball; a ball's centre is about its radius above what it
        // rolls on, and must be inside the box before the ball can touch the
        // airship at its starting pose).  Binary fractions: the box is the
        // same double in every world once its faces are on the 1/8 m grid.
        constexpr double kAirshipMarginSide = 3.0;
        constexpr double kAirshipMarginBelow = 4.5;
        constexpr double kAirshipMarginAbove = 5.0;
        constexpr double kAirshipGrid = 8.0;   // faces on multiples of 1/8 m, rounded outward

        class airship_zone_filter;
        class airship_zone_listener;
        struct airship_zone_state {
            bool enabled = false;
            // The walkable parts (PE_Balloon_Platform*, PE_Balloon_Platte*)
            // found when the zone was switched on; the box is taken once the
            // platform has its body - the level-end script places the airship
            // and physicalizes it after the level loads, so the parts are at
            // the origin of the level file until then.  Until `ready` no ball
            // is in the zone.
            std::vector<CK_ID> parts;
            CK_ID platform = 0;
            bool ready = false;
            double min[3] = {}, max[3] = {};
            char own_group[IVP_NO_COLL_GROUP_STRING_LEN] = {};
            // The own ball's latch (one way for the life): the IVP clock of
            // the first PSI its core was in the box, and of the first PSI it
            // had no body after that (open while alive).  Clock values, so a
            // rollback's restore - which puts the clock back - reads the
            // latch as it stood at its anchor, and a replay re-enters at the
            // same PSI (same clock) and rechecks there as the live run did.
            struct own_life {
                double entered = 0.0;
                double gone = 0.0;
                bool open = true;
                bool asleep_logged = false;   // step ASL: its first PSI asleep logged
            };
            std::vector<own_life> own_lives;
            uint64_t own_rechecks = 0;
            uint64_t frozen = 0;
            airship_zone_filter* filter = nullptr;       // installed in `environment`
            airship_zone_listener* listener = nullptr;
            IVP_Environment* environment = nullptr;
        };
        // Never destroyed: an IVP environment (and its filter) may outlive
        // static storage at process exit.
        std::map<CKIpionManager*, airship_zone_state>& airship_states() {
            static auto* states = new std::map<CKIpionManager*, airship_zone_state>();
            return *states;
        }
        airship_zone_state* find_airship_state(CKIpionManager* physics) {
            auto& states = airship_states();
            const auto it = states.find(physics);
            return it == states.end() ? nullptr : &it->second;
        }
        bool airship_is_player(const IVP_Real_Object* object) {
            const char* ident = object ? object->nocoll_group_ident : nullptr;
            return ident && std::strncmp(ident, kAirshipPlayerPrefix, std::strlen(kAirshipPlayerPrefix)) == 0;
        }
        // A body's collision group as a string (the ident is not
        // NUL-terminated at its full length).
        std::string player_ball_group(const IVP_Real_Object* object) {
            const char* ident = object ? object->nocoll_group_ident : nullptr;
            if (!ident) return {};
            size_t n = 0;
            while (n < IVP_NO_COLL_GROUP_STRING_LEN && ident[n]) ++n;
            return std::string(ident, n);
        }
        // Every player ball in collision detection, asleep or not, for the
        // PSI listeners (the airship zone's and the spawn ghost's; step ASL):
        // the manager's whole object table, not m_MovableObjects -
        // CKIpionManager takes a frozen (sleeping) core out of that list
        // (physics_RT PhysicsObjectListener::event_object_frozen), and a ball
        // at rest would read as bodiless.  A body with no overlap-tree node
        // is left out: a rollback's restore parks a body born after its
        // anchor that way until its birth tick (it is not in this tick's
        // world yet).  Sorted by group, then name: the table's hash order is
        // never used.
        std::vector<std::pair<std::string, IVP_Real_Object*>> player_balls_in_detection(CKIpionManager* physics) {
            std::vector<std::pair<std::string, IVP_Real_Object*>> balls;
            for (auto it = physics->m_PhysicsObjects.Begin(); it != physics->m_PhysicsObjects.End(); ++it) {
                IVP_Real_Object* object = (*it).m_RealObject;
                IVP_Core* core = object ? object->get_core() : nullptr;
                if (!core || core->physical_unmoveable || !airship_is_player(object)) continue;
                const IVP_OV_Element* e = object->get_ov_element();
                if (!e || !e->node) continue;
                balls.emplace_back(player_ball_group(object), object);
            }
            std::sort(balls.begin(), balls.end(), [](const auto& x, const auto& y) {
                if (x.first != y.first) return x.first < y.first;
                const char* nx = x.second->get_name();
                const char* ny = y.second->get_name();
                return std::strcmp(nx ? nx : "", ny ? ny : "") < 0;
            });
            return balls;
        }
        bool airship_is_own(const airship_zone_state& state, const IVP_Real_Object* object) {
            return state.own_group[0]
                && std::strncmp(object->nocoll_group_ident, state.own_group, IVP_NO_COLL_GROUP_STRING_LEN) == 0;
        }
        bool airship_in_box(const airship_zone_state& state, IVP_Real_Object* object) {
            IVP_Core* core = object ? object->get_core() : nullptr;
            if (!core || !state.ready) return false;
            const IVP_U_Point* p = core->get_position_PSI();
            for (int k = 0; k < 3; ++k)
                if (!(p->k[k] >= state.min[k] && p->k[k] <= state.max[k])) return false;
            return true;
        }
        double airship_now(const IVP_Environment* environment) {
            return environment ? const_cast<IVP_Environment*>(environment)->get_current_time().get_seconds() : 0.0;
        }
        // The own life latched at `now`, or null.
        const airship_zone_state::own_life* airship_own_life(const airship_zone_state& state, double now) {
            for (const auto& life: state.own_lives)
                if (life.entered <= now && (life.open || now < life.gone)) return &life;
            return nullptr;
        }
        bool airship_body(const IVP_Real_Object* object) {
            const char* name = object ? object->get_name() : nullptr;
            return name && bmmo::session::keep_local_body(name);
        }
        bool airship_take_box(CKIpionManager* physics, airship_zone_state& state);

        // The pair rule (asked when a pair first becomes a collision
        // candidate, and again by a recheck).
        bool airship_pair_allowed(const airship_zone_state& state, IVP_Real_Object* a, IVP_Real_Object* b, double now) {
            IVP_Real_Object* pair[2] = {a, b};
            for (int side = 0; side < 2; ++side) {
                IVP_Real_Object* ball = pair[side];
                IVP_Real_Object* other = pair[1 - side];
                if (!airship_is_player(ball) || !other) continue;
                const bool own = airship_is_own(state, ball);
                const bool other_airship = airship_body(other);
                if (!own && other_airship) return false;
                if (own ? airship_own_life(state, now) != nullptr || airship_in_box(state, ball) : airship_in_box(state, ball)) {
                    if (!own) return false;
                    IVP_Core* core = other->get_core();
                    if (!other_airship && core && !core->physical_unmoveable) return false;
                }
            }
            return true;
        }

        class airship_zone_filter : public IVP_Collision_Filter {
        public:
            CKIpionManager* physics = nullptr;
            IVP_Environment* environment = nullptr;
            IVP_BOOL check_objects_for_collision_detection(IVP_Real_Object* object0, IVP_Real_Object* object1) override {
                const airship_zone_state* state = find_airship_state(physics);
                if (!state || !state->enabled) return IVP_TRUE;
                return airship_pair_allowed(*state, object0, object1, airship_now(environment)) ? IVP_TRUE : IVP_FALSE;
            }
            void environment_will_be_deleted(IVP_Environment*) override {
                if (airship_zone_state* state = find_airship_state(physics))
                    if (state->filter == this) state->filter = nullptr;
                delete this;
            }
        };

        // At the start of every PSI (after IVP revived the cores woken since
        // the last one, before the controllers and the integration): every
        // simulated player ball that is not the own one and whose core is in
        // the box is taken out of the simulation - its pairs rechecked
        // (the filter now refuses every partner), then frozen.  A ghost woken
        // since (the navigation's Physics WakeUp on a key edge) is frozen
        // again before it moves.  The candidates come sorted by their group
        // ("P#<join order>", the same in every world), then name, so two
        // entering in one PSI are frozen in one order everywhere.  The balls
        // are read from the manager's whole object table
        // (player_balls_in_detection, step ASL): a latched own ball that
        // falls asleep keeps its body, and its latch.
        class airship_zone_listener : public IVP_Listener_PSI {
        public:
            CKIpionManager* physics = nullptr;
            IVP_Environment* environment = nullptr;
            void event_PSI(IVP_Event_PSI*) override {
                airship_zone_state* state = find_airship_state(physics);
                if (!state || !state->enabled || !physics) return;
                if (!state->ready && !airship_take_box(physics, *state)) return;
                std::vector<IVP_Real_Object*> ghosts;
                IVP_Real_Object* own = nullptr;
                for (const auto& entry: player_balls_in_detection(physics)) {
                    IVP_Real_Object* object = entry.second;
                    if (airship_is_own(*state, object)) {
                        if (!own) own = object;
                        continue;
                    }
                    IVP_Core* core = object->get_core();
                    if (!core || !IVP_MTIS_SIMULATED(core->movement_state)) continue;
                    if (airship_in_box(*state, object)) ghosts.push_back(object);
                }
                // The own ball first (one ball, before the ghosts: one order
                // in every replay).  Its latch opens at the first PSI its core
                // is in the box - a later one of a life a rollback took back
                // is forgotten first - and at the PSI it opened its pairs are
                // rechecked (live, and again when a replay reaches that PSI):
                // a contact it had with another ball or a mechanism is
                // dropped where every other world freezes its copy.  No body
                // (a death): the life's latch closes at this PSI.
                if (state->own_group[0]) {
                    const double now = airship_now(environment);
                    if (own && airship_in_box(*state, own)) {
                        if (!airship_own_life(*state, now)) {
                            auto& lives = state->own_lives;
                            lives.erase(std::remove_if(lives.begin(), lives.end(),
                                                       [now](const airship_zone_state::own_life& l) { return l.entered > now; }),
                                        lives.end());
                            lives.push_back(airship_zone_state::own_life{now, 0.0, true});
                            while (lives.size() > 16) lives.erase(lives.begin());
                        }
                        const auto* life = airship_own_life(*state, now);
                        if (life && life->entered == now) {
                            own->recheck_collision_filter();
                            ++state->own_rechecks;
                        }
                    } else if (!own) {
                        for (auto& life: state->own_lives)
                            if (life.entered <= now && (life.open || now < life.gone)) {
                                life.open = false;
                                life.gone = now;
                            }
                    }
                    // Step ASL: a latched own ball asleep (its core frozen, so
                    // out of m_MovableObjects) keeps its latch; its first such
                    // PSI in the life is logged (stdout only: no state, no
                    // decision reads it).
                    IVP_Core* own_core = own ? own->get_core() : nullptr;
                    if (own_core && !IVP_MTIS_SIMULATED(own_core->movement_state))
                        for (auto& life: state->own_lives)
                            if (life.entered <= now && (life.open || now < life.gone) && !life.asleep_logged) {
                                life.asleep_logged = true;
                                const char* name = own->get_name();
                                std::printf("[bmmo] airship: the own ball %s is asleep in its latch at clock %.6f "
                                            "(latched at %.6f; %s m_MovableObjects): the latch holds\n",
                                            name ? name : "?", now, life.entered,
                                            physics->m_MovableObjects.index_of(own) < 0 ? "not in" : "still in");
                            }
                }
                if (ghosts.empty()) return;
                for (IVP_Real_Object* object: ghosts) {
                    object->recheck_collision_filter();
                    object->disable_simulation();
                    ++state->frozen;
                }
            }
            void environment_will_be_deleted(IVP_Environment*) override {
                if (airship_zone_state* state = find_airship_state(physics))
                    if (state->listener == this) state->listener = nullptr;
                delete this;
            }
        };

        // The walkable parts of the level's airship, by name.
        void airship_find_parts(CKContext* context, airship_zone_state& state) {
            state.parts.clear();
            state.platform = 0;
            if (!context) return;
            const CK_CLASSID classes[2] = {CKCID_3DOBJECT, CKCID_3DENTITY};
            for (CK_CLASSID cid: classes) {
                const int count = context->GetObjectsCountByClassID(cid);
                CK_ID* ids = context->GetObjectsListByClassID(cid);
                for (int i = 0; i < count; ++i) {
                    auto* entity = CK3dEntity::Cast(context->GetObject(ids[i]));
                    const char* name = entity ? entity->GetName() : nullptr;
                    if (!name) continue;
                    const std::string_view n(name);
                    const bool platform = n.substr(0, 19) == "PE_Balloon_Platform";
                    if (!platform && n.substr(0, 17) != "PE_Balloon_Platte") continue;
                    if (std::find(state.parts.begin(), state.parts.end(), entity->GetID()) != state.parts.end()) continue;
                    state.parts.push_back(entity->GetID());
                    if (platform && !state.platform) state.platform = entity->GetID();
                }
            }
        }

        // The box, once the platform has its body: every part's local
        // (mesh) box through its world matrix as the placing script left it -
        // the airship's starting pose - grown to the 1/8 m grid outward and
        // by the margins.  The corners are this code's arithmetic on the
        // mesh's extremes and the matrix, so every world that placed the
        // airship alike gets the same box (the grid absorbs an ulp).
        bool airship_take_box(CKIpionManager* physics, airship_zone_state& state) {
            CKContext* context = physics ? physics->m_Context : nullptr;
            auto* platform = context ? CK3dEntity::Cast(context->GetObject(state.platform)) : nullptr;
            PhysicsObject* body = platform ? physics->GetPhysicsObject(platform) : nullptr;
            if (!body || !body->m_RealObject) return false;
            double lo[3] = {}, hi[3] = {};
            bool any = false;
            for (CK_ID id: state.parts) {
                auto* entity = CK3dEntity::Cast(context->GetObject(id));
                if (!entity) continue;
                const VxBbox& local = entity->GetBoundingBox(TRUE);
                const VxMatrix& world = entity->GetWorldMatrix();
                for (int corner = 0; corner < 8; ++corner) {
                    const double c[3] = {(corner & 1) ? local.Max.x : local.Min.x, (corner & 2) ? local.Max.y : local.Min.y,
                                         (corner & 4) ? local.Max.z : local.Min.z};
                    for (int k = 0; k < 3; ++k) {
                        const double v = c[0] * world[0][k] + c[1] * world[1][k] + c[2] * world[2][k] + world[3][k];
                        if (!any || v < lo[k]) lo[k] = v;
                        if (!any || v > hi[k]) hi[k] = v;
                    }
                    any = true;
                }
            }
            if (!any) return false;
            for (int k = 0; k < 3; ++k) {
                const double below = k == 1 ? kAirshipMarginBelow : kAirshipMarginSide;
                const double above = k == 1 ? kAirshipMarginAbove : kAirshipMarginSide;
                state.min[k] = std::floor(lo[k] * kAirshipGrid) / kAirshipGrid - below;
                state.max[k] = std::ceil(hi[k] * kAirshipGrid) / kAirshipGrid + above;
            }
            state.ready = true;
            return true;
        }
    }

    bool airship_zone_set(CKIpionManager* physics, bool enable, const char* own_group, airship_zone_box& out,
                          std::string& error) {
        error.clear();
        out = airship_zone_box{};
        if (!physics) {
            error = "no physics manager";
            return false;
        }
        airship_zone_state& state = airship_states()[physics];
        state.enabled = false;
        if (!enable) return true;
        IVP_Environment* environment = physics->GetEnvironment();
        if (!environment) {
            error = "physics environment is unavailable";
            return false;
        }
        airship_find_parts(physics->m_Context, state);
        state.ready = false;
        if (!state.platform) return true;   // no airship: the zone stays off
        std::snprintf(state.own_group, sizeof(state.own_group), "%s", own_group ? own_group : "");
        state.own_lives.clear();
        if (state.environment != environment) {
            // A new environment (a level loaded since): the old one's filter
            // and listener went with it (environment_will_be_deleted).
            state.filter = nullptr;
            state.listener = nullptr;
            state.environment = environment;
        }
        if (!state.filter) {
            auto* meta = static_cast<IVP_Meta_Collision_Filter*>(environment->get_collision_filter());
            if (!meta) {
                error = "the environment has no collision filter";
                return false;
            }
            auto* filter = new airship_zone_filter();
            filter->physics = physics;
            filter->environment = environment;
            meta->add_collision_filter(filter);
            state.filter = filter;
        }
        if (!state.listener) {
            auto* listener = new airship_zone_listener();
            listener->physics = physics;
            listener->environment = environment;
            environment->add_listener_PSI(listener);
            state.listener = listener;
        }
        state.enabled = true;
        airship_take_box(physics, state);   // already placed (a world anchored after the level end's set-up)
        return airship_zone_get(physics, out);
    }

    bool airship_zone_get(CKIpionManager* physics, airship_zone_box& out) {
        out = airship_zone_box{};
        const airship_zone_state* state = physics ? find_airship_state(physics) : nullptr;
        if (!state || !state->enabled) return true;
        out.active = true;
        out.ready = state->ready;
        out.frozen = state->frozen;
        for (int k = 0; k < 3; ++k) {
            out.min[k] = state->min[k];
            out.max[k] = state->max[k];
        }
        return true;
    }

    int airship_zone_contains(CKIpionManager* physics, const char* entity_name) {
        const airship_zone_state* state = physics ? find_airship_state(physics) : nullptr;
        CK3dEntity* entity = physics ? find_entity(physics, bounded(entity_name, BMMO_PHYSICS_NAME_SIZE)) : nullptr;
        PhysicsObject* object = entity ? physics->GetPhysicsObject(entity) : nullptr;
        if (!object || !object->m_RealObject) return -1;
        if (!state || !state->enabled) return 0;
        IVP_Real_Object* real = object->m_RealObject;
        if (!airship_is_player(real)) return 0;
        if (airship_in_box(*state, real)) return 1;
        return airship_is_own(*state, real) && airship_own_life(*state, airship_now(physics->GetEnvironment())) ? 1 : 0;
    }

    // ---- bridge API v25: the spawn ghost (steps SPGH, SPGH2) ----

    namespace {
        class spawn_ghost_filter;
        class spawn_ghost_listener;
        struct spawn_ghost_state_t {
            bool enabled = false;
            bool respawns = true;
            double radius = 0.0;
            // The spawn areas' centres (the level start and every
            // resetpoint), in the order the tables give them; duplicates
            // dropped.  Read when the rule is switched on, and at a PSI again
            // while none was found.
            std::vector<std::array<double, 3>> areas;
            // Per group, the state the IVP savestate carries
            // (spawn_ghost_record): step SPGH2 moved it there from IVP clock
            // intervals, so a rollback and the one state transfer restore it
            // with the world it belongs to.
            struct group_state {
                bool seen = false;      // its first life began
                bool present = false;   // a body in collision detection at the last PSI
                bool solid = false;
                bool marked = false;    // a spawn birth since the last PSI
            };
            std::map<std::string, group_state> groups;
            uint64_t solidified = 0, rechecks = 0, overlap_waits = 0, births = 0, resumed = 0;
            spawn_ghost_filter* filter = nullptr;
            spawn_ghost_listener* listener = nullptr;
            IVP_Environment* environment = nullptr;
        };
        std::map<CKIpionManager*, spawn_ghost_state_t>& spawn_ghost_states() {
            static auto* states = new std::map<CKIpionManager*, spawn_ghost_state_t>();   // never destroyed (see airship_states)
            return *states;
        }
        spawn_ghost_state_t* find_spawn_ghost_state(CKIpionManager* physics) {
            auto& states = spawn_ghost_states();
            const auto it = states.find(physics);
            return it == states.end() ? nullptr : &it->second;
        }
        // A ghost: a player ball whose group is not solid, or had no body at
        // the last PSI (a body made since - a birth, a trafo's new ball -
        // is inserted as a ghost, and the next PSI settles it: a solid group
        // has its pairs rechecked there).  A group never seen is a ghost.
        bool spawn_ghost_is_ghost(const spawn_ghost_state_t& state, const IVP_Real_Object* object) {
            if (!airship_is_player(object)) return false;
            const auto it = state.groups.find(player_ball_group(object));
            return it == state.groups.end() || !it->second.present || !it->second.solid;
        }
        bool spawn_ghost_in_area(const spawn_ghost_state_t& state, const IVP_U_Point* p) {
            const double r2 = state.radius * state.radius;
            for (const auto& c: state.areas) {
                const double dx = p->k[0] - c[0], dy = p->k[1] - c[1], dz = p->k[2] - c[2];
                if (dx * dx + dy * dy + dz * dz < r2) return true;
            }
            return false;
        }
        void spawn_ghost_read_areas(CKIpionManager* physics, spawn_ghost_state_t& state) {
            state.areas.clear();
            CKContext* context = physics ? physics->m_Context : nullptr;
            if (!context) return;
            auto add = [&](const VxMatrix& m) {
                const std::array<double, 3> c = {m[3][0], m[3][1], m[3][2]};
                if (std::find(state.areas.begin(), state.areas.end(), c) == state.areas.end()) state.areas.push_back(c);
            };
            VxMatrix matrix;
            if (auto* level = static_cast<CKDataArray*>(context->GetObjectByNameAndClass(
                        const_cast<CKSTRING>("CurrentLevel"), CKCID_DATAARRAY)))
                if (level->GetRowCount() > 0 && level->GetColumnCount() > 3 && level->GetElementValue(0, 3, &matrix))
                    add(matrix);
            if (auto* table = static_cast<CKDataArray*>(context->GetObjectByNameAndClass(
                        const_cast<CKSTRING>("ResetPoints"), CKCID_DATAARRAY)))
                for (int row = 0; row < table->GetRowCount(); ++row)
                    if (table->GetElementValue(row, 0, &matrix)) add(matrix);
        }

        // The pair rule: two player balls collide only when neither is a
        // ghost.  Every other pair: IVP_TRUE (the meta filter ANDs).
        class spawn_ghost_filter : public IVP_Collision_Filter {
        public:
            CKIpionManager* physics = nullptr;
            IVP_Environment* environment = nullptr;
            IVP_BOOL check_objects_for_collision_detection(IVP_Real_Object* object0, IVP_Real_Object* object1) override {
                const spawn_ghost_state_t* state = find_spawn_ghost_state(physics);
                if (!state || !state->enabled) return IVP_TRUE;
                if (!airship_is_player(object0) || !airship_is_player(object1)) return IVP_TRUE;
                return spawn_ghost_is_ghost(*state, object0) || spawn_ghost_is_ghost(*state, object1) ? IVP_FALSE : IVP_TRUE;
            }
            void environment_will_be_deleted(IVP_Environment*) override {
                if (spawn_ghost_state_t* state = find_spawn_ghost_state(physics))
                    if (state->filter == this) state->filter = nullptr;
                delete this;
            }
        };

        // At the start of every PSI: the groups brought up to date (a
        // group's first body begins its first life as a ghost; a body that
        // came to a solid group since the last PSI - a trafo's new ball - is
        // solid and rechecked, its pairs were refused while it was
        // inserted), then every ghost outside every area that overlaps no
        // other player ball turned solid - all decided on the positions as
        // they stand, before any recheck - and the new solid balls rechecked
        // in group order.
        class spawn_ghost_listener : public IVP_Listener_PSI {
        public:
            CKIpionManager* physics = nullptr;
            IVP_Environment* environment = nullptr;
            void event_PSI(IVP_Event_PSI*) override {
                spawn_ghost_state_t* state = find_spawn_ghost_state(physics);
                if (!state || !state->enabled || !physics) return;
                if (state->areas.empty()) spawn_ghost_read_areas(physics, *state);
                // Every player ball in collision detection, asleep or not,
                // sorted by group, then name (player_balls_in_detection).
                const std::vector<std::pair<std::string, IVP_Real_Object*>> balls = player_balls_in_detection(physics);
                // The groups: present now or not, the first life begun, the
                // bodies that need their pairs rechecked noted.
                std::vector<IVP_Real_Object*> recheck;
                std::map<std::string, bool> has_body;
                for (const auto& [group, object]: balls) has_body[group] = true;
                for (auto& [group, g]: state->groups)
                    if (!has_body.count(group)) {
                        g.present = false;
                        g.marked = false;
                    }
                // One decision per group, on its first body by name.  A group
                // has one body in collision detection at a PSI (the ball; a
                // body a rollback parks is left out above), so the name
                // order, which differs across worlds (a server clone, a
                // Ball_*, a mirror entity), never picks between two.
                std::string last;
                for (const auto& [group, object]: balls) {
                    auto& g = state->groups[group];
                    if (!last.empty() && last == group) continue;
                    last = group;
                    if (!g.seen) {
                        g.seen = true;
                        g.solid = false;   // the first life: a ghost
                    }
                    if (!g.present && g.solid) {
                        // A body that came to a solid group (a trafo's new
                        // ball, a beam across a PSI): it stays solid, and its
                        // pairs, refused while it was inserted, are made now.
                        ++state->resumed;
                        recheck.push_back(object);
                    } else if (g.present && g.marked) {
                        // A spawn birth while a body was there at the last
                        // PSI (a death and a respawn between two PSIs): the
                        // pairs the new body was given while it read solid go.
                        recheck.push_back(object);
                    }
                    g.present = true;
                    g.marked = false;
                }
                // The decisions, on the positions as they stand.
                for (const auto& [group, object]: balls) {
                    auto& g = state->groups[group];
                    if (g.solid) continue;
                    IVP_Core* core = object->get_core();
                    const IVP_U_Point* p = core->get_position_PSI();
                    if (spawn_ghost_in_area(*state, p)) continue;
                    bool overlaps = false;
                    for (const auto& [other_group, other]: balls) {
                        if (other == object) continue;
                        IVP_Core* oc = other->get_core();
                        const IVP_U_Point* q = oc->get_position_PSI();
                        const double reach = static_cast<double>(core->upper_limit_radius) + static_cast<double>(oc->upper_limit_radius);
                        const double dx = p->k[0] - q->k[0], dy = p->k[1] - q->k[1], dz = p->k[2] - q->k[2];
                        if (dx * dx + dy * dy + dz * dz < reach * reach) {
                            overlaps = true;
                            break;
                        }
                    }
                    if (overlaps) {
                        ++state->overlap_waits;
                        continue;
                    }
                    g.solid = true;
                    ++state->solidified;
                    if (std::find(recheck.begin(), recheck.end(), object) == recheck.end()) recheck.push_back(object);
                }
                // The pairs a ghost was refused: made now where another
                // player ball's overlap-tree sphere meets its own (IVP would
                // have a collision checker there); otherwise the next
                // re-insertion of either finds the pair, as for any two
                // bodies coming together.  In group order.
                std::sort(recheck.begin(), recheck.end(), [&](IVP_Real_Object* x, IVP_Real_Object* y) {
                    const std::string gx = player_ball_group(x), gy = player_ball_group(y);
                    if (gx != gy) return gx < gy;
                    const char* nx = x->get_name();
                    const char* ny = y->get_name();
                    return std::strcmp(nx ? nx : "", ny ? ny : "") < 0;
                });
                for (IVP_Real_Object* object: recheck) {
                    const IVP_OV_Element* e = object->get_ov_element();
                    if (!e) continue;
                    bool close_by = false;
                    for (const auto& [other_group, other]: balls) {
                        const IVP_OV_Element* f = other == object ? nullptr : other->get_ov_element();
                        if (!f) continue;
                        const double reach = static_cast<double>(e->radius) + static_cast<double>(f->radius);
                        const double dx = static_cast<double>(e->center.k[0]) - f->center.k[0];
                        const double dy = static_cast<double>(e->center.k[1]) - f->center.k[1];
                        const double dz = static_cast<double>(e->center.k[2]) - f->center.k[2];
                        if (dx * dx + dy * dy + dz * dz < reach * reach) {
                            close_by = true;
                            break;
                        }
                    }
                    if (!close_by) continue;
                    object->recheck_collision_filter();
                    ++state->rechecks;
                }
            }
            void environment_will_be_deleted(IVP_Environment*) override {
                if (spawn_ghost_state_t* state = find_spawn_ghost_state(physics))
                    if (state->listener == this) state->listener = nullptr;
                delete this;
            }
        };
    }

    bool spawn_ghost_set(CKIpionManager* physics, bool enable, bool respawns, double radius, spawn_ghost_info& out,
                         std::string& error) {
        error.clear();
        out = spawn_ghost_info{};
        if (!physics) {
            error = "no physics manager";
            return false;
        }
        spawn_ghost_state_t& state = spawn_ghost_states()[physics];
        if (!enable) {
            // Off: `out` carries the counters as they stood (active false).
            spawn_ghost_get(physics, out);
            out.active = false;
            state.enabled = false;
            return true;
        }
        state.enabled = false;
        IVP_Environment* environment = physics->GetEnvironment();
        if (!environment) {
            error = "physics environment is unavailable";
            return false;
        }
        if (!(radius > 0.0)) {
            error = "the spawn area radius must be positive";
            return false;
        }
        state.respawns = respawns;
        state.radius = radius;
        state.groups.clear();
        state.solidified = state.rechecks = state.overlap_waits = state.births = state.resumed = 0;
        spawn_ghost_read_areas(physics, state);
        if (state.environment != environment) {
            // A new environment (a level loaded since): the old one's filter
            // and listener went with it (environment_will_be_deleted).
            state.filter = nullptr;
            state.listener = nullptr;
            state.environment = environment;
        }
        if (!state.filter) {
            auto* meta = static_cast<IVP_Meta_Collision_Filter*>(environment->get_collision_filter());
            if (!meta) {
                error = "the environment has no collision filter";
                return false;
            }
            auto* filter = new spawn_ghost_filter();
            filter->physics = physics;
            filter->environment = environment;
            meta->add_collision_filter(filter);
            state.filter = filter;
        }
        if (!state.listener) {
            auto* listener = new spawn_ghost_listener();
            listener->physics = physics;
            listener->environment = environment;
            environment->add_listener_PSI(listener);
            state.listener = listener;
        }
        state.enabled = true;
        return spawn_ghost_get(physics, out);
    }

    bool spawn_ghost_get(CKIpionManager* physics, spawn_ghost_info& out) {
        out = spawn_ghost_info{};
        const spawn_ghost_state_t* state = physics ? find_spawn_ghost_state(physics) : nullptr;
        if (!state || !state->enabled) return true;
        out.active = true;
        out.areas = static_cast<uint32_t>(state->areas.size());
        out.radius = state->radius;
        out.solidified = state->solidified;
        out.rechecks = state->rechecks;
        out.overlap_waits = state->overlap_waits;
        out.births = state->births;
        out.resumed = state->resumed;
        return true;
    }

    int spawn_ghost_state(CKIpionManager* physics, const char* entity_name) {
        const spawn_ghost_state_t* state = physics ? find_spawn_ghost_state(physics) : nullptr;
        CK3dEntity* entity = physics ? find_entity(physics, bounded(entity_name, BMMO_PHYSICS_NAME_SIZE)) : nullptr;
        PhysicsObject* object = entity ? physics->GetPhysicsObject(entity) : nullptr;
        if (!object || !object->m_RealObject) return -1;
        if (!state || !state->enabled) return 0;
        return spawn_ghost_is_ghost(*state, object->m_RealObject) ? 1 : 0;
    }

    bool spawn_ghost_birth(CKIpionManager* physics, const char* group) {
        spawn_ghost_state_t* state = physics ? find_spawn_ghost_state(physics) : nullptr;
        if (!state || !state->enabled || !group || !*group) return false;
        auto& g = state->groups[bounded(group, IVP_NO_COLL_GROUP_STRING_LEN)];
        if (g.seen && !state->respawns) return true;   // only the first life is a ghost
        g.solid = false;
        g.marked = true;
        ++state->births;
        return true;
    }

    // ---- step OWG: the own ball's player group before its birth tick's PSIs
    namespace {
        int count_player_pairs(IVP_Real_Object* object) {
            IVP_OV_Element* e = object ? object->get_ov_element() : nullptr;
            if (!e) return -1;
            int n = 0;
            for (int k = 0; k < e->collision_fvector.len(); ++k) {
                IVP_Collision* c = e->collision_fvector.element_at(k);
                if (!c) continue;
                IVP_Real_Object* objects[2] = {nullptr, nullptr};
                c->get_objects(objects);
                IVP_Real_Object* other = objects[0] == object ? objects[1] : objects[0];
                if (other && other != object && airship_is_player(other)) ++n;
            }
            return n;
        }

        bool adopt_group_now(CKIpionManager* physics, IVP_Real_Object* object, const char* group, own_group_result& out) {
            out.entity = object->get_name() ? object->get_name() : "";
            out.old_group = player_ball_group(object);
            const std::string wanted = group ? bounded(group, IVP_NO_COLL_GROUP_STRING_LEN * 4) : std::string();
            if (wanted.size() >= IVP_NO_COLL_GROUP_STRING_LEN) return false;
            out.player_pairs_before = count_player_pairs(object);
            object->change_nocoll_group_ident(wanted.empty() ? nullptr : wanted.c_str());
            // The pairs the insertion made under the old group are re-asked
            // only when the group changed and the body has an overlap-tree
            // element (a body made without collision has none).
            if (out.old_group != wanted && object->get_ov_element()) {
                object->recheck_collision_filter();
                out.rechecked = true;
                out.player_pairs_after = count_player_pairs(object);
            }
            const spawn_ghost_state_t* state = find_spawn_ghost_state(physics);
            out.ghost = state && state->enabled ? (spawn_ghost_is_ghost(*state, object) ? 1 : 0) : 0;
            return true;
        }

        // Queued at the own ball's OnPhysicalize (retail), run in the same
        // frame's PreSimulate pass: the block has made the body by then.
        // The first Execute is the enqueue's own call (impulse_callback).
        class own_group_callback final : public PhysicsCallback {
        public:
            own_group_callback(CKIpionManager* manager, CKBehavior* behavior, CK_ID entity, const std::string& group)
                : PhysicsCallback(manager, behavior, 2), entity_(entity), group_(group) {}
            int Execute() override {
                if (!armed_) {
                    armed_ = true;
                    return 0;
                }
                CK3dEntity* entity = m_IpionManager && m_IpionManager->m_Context
                    ? CK3dEntity::Cast(m_IpionManager->m_Context->GetObject(entity_)) : nullptr;
                PhysicsObject* object = entity ? m_IpionManager->GetPhysicsObject(entity) : nullptr;
                own_group_result r;
                if (!object || !object->m_RealObject || !adopt_group_now(m_IpionManager, object->m_RealObject, group_.c_str(), r))
                    std::printf("[bmmo] own group: %s has no body at the PreSimulate pass\n",
                                entity && entity->GetName() ? entity->GetName() : "?");
                else
                    std::printf("[bmmo] own group: %s\n", describe_own_group(r).c_str());
                return 1;
            }
        private:
            CK_ID entity_;
            std::string group_;
            bool armed_ = false;
        };
    }

    std::string describe_own_group(const own_group_result& r) {
        char text[300];
        std::snprintf(text, sizeof(text), "%s '%s' -> player group, %s (player pairs %d -> %d), %s", r.entity.c_str(),
                      r.old_group.c_str(), r.rechecked ? "rechecked" : "not rechecked", r.player_pairs_before,
                      r.player_pairs_after, r.ghost == 1 ? "a spawn ghost" : "solid or rule off");
        return text;
    }

    bool adopt_own_group(CKIpionManager* physics, const char* entity_name, const char* group, own_group_result& out,
                         std::string& error) {
        error.clear();
        out = own_group_result{};
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
        if (!adopt_group_now(physics, object->m_RealObject, group, out)) {
            error = std::string("collision group too long: ") + (group ? group : "");
            return false;
        }
        return true;
    }

    int queue_own_group(CKIpionManager* physics, const char* entity_name, const char* group, uint32_t behavior_id,
                        std::string& error) {
        error.clear();
        if (!physics || !physics->GetEnvironment()) {
            error = "physics environment is unavailable";
            return 0;
        }
        const std::string name = bounded(entity_name, BMMO_PHYSICS_NAME_SIZE);
        CK3dEntity* entity = find_entity(physics, name);
        if (!entity) {
            error = "no 3D entity named '" + name + "'";
            return 0;
        }
        const std::string wanted = group ? bounded(group, IVP_NO_COLL_GROUP_STRING_LEN * 4) : std::string();
        if (wanted.size() >= IVP_NO_COLL_GROUP_STRING_LEN) {
            error = "collision group too long: " + wanted;
            return 0;
        }
        if (PhysicsObject* object = physics->GetPhysicsObject(entity); object && object->m_RealObject) {
            // The body exists already: set now, logged as the queued path
            // logs it (the callback), so every retail adoption leaves a trace.
            own_group_result r;
            adopt_group_now(physics, object->m_RealObject, wanted.c_str(), r);
            std::printf("[bmmo] own group (set at once): %s\n", describe_own_group(r).c_str());
            return 1;
        }
        CKBehavior* behavior = behavior_id && physics->m_Context
            ? CKBehavior::Cast(physics->m_Context->GetObject(behavior_id)) : nullptr;
        if (!behavior) {
            error = "no live behavior to anchor the PreSimulate pass for '" + name + "' (behavior id " +
                    std::to_string(behavior_id) + ")";
            return 0;
        }
        if (!physics->m_PreSimulateCallbacks) {
            error = "the physics manager has no PreSimulate queue";
            return 0;
        }
        physics->m_PreSimulateCallbacks->Process(new own_group_callback(physics, behavior, entity->GetID(), wanted));
        return 2;
    }

    void spawn_ghost_forget(CKIpionManager* physics, const char* group) {
        spawn_ghost_state_t* state = physics ? find_spawn_ghost_state(physics) : nullptr;
        if (!state || !state->enabled || !group || !*group) return;
        state->groups.erase(bounded(group, IVP_NO_COLL_GROUP_STRING_LEN));
    }

    bool spawn_ghost_record_get(CKIpionManager* physics, std::vector<spawn_ghost_record>& out) {
        out.clear();
        const spawn_ghost_state_t* state = physics ? find_spawn_ghost_state(physics) : nullptr;
        if (!state || !state->enabled) return false;
        out.reserve(state->groups.size());
        for (const auto& [group, g]: state->groups) {   // std::map: sorted by group
            spawn_ghost_record r;
            r.group = group;
            r.seen = g.seen ? 1 : 0;
            r.present = g.present ? 1 : 0;
            r.solid = g.solid ? 1 : 0;
            r.marked = g.marked ? 1 : 0;
            out.push_back(std::move(r));
        }
        return true;
    }

    void spawn_ghost_record_set(CKIpionManager* physics, const std::vector<spawn_ghost_record>& in) {
        spawn_ghost_state_t* state = physics ? find_spawn_ghost_state(physics) : nullptr;
        if (!state || !state->enabled) return;
        state->groups.clear();
        for (const auto& r: in) {
            auto& g = state->groups[r.group];
            g.seen = r.seen != 0;
            g.present = r.present != 0;
            g.solid = r.solid != 0;
            g.marked = r.marked != 0;
        }
    }

    // ---- bridge API v8: session clock guard ----

    namespace {
        struct clock_guard_entry {
            bool enabled = false;
            float target = 1.0f;
            uint32_t pause_behavior = 0;
            bool paused = false;
            bool callback_queued = false;
        };

        std::map<CKIpionManager*, clock_guard_entry>& clock_guards() {
            // Never destroyed: the physics manager may outlive static storage
            // at process exit (see the event log listener above).
            static auto* guards = new std::map<CKIpionManager*, clock_guard_entry>();
            return *guards;
        }

        // The retail pause menu is the Event_handler chain deactivating
        // Gameplay_Ingame, and the PreSimulate pass runs after this frame's
        // scripts, so the sensor already reads the state the menu put it in.
        // The mod cannot supply this flag itself: its frame hook runs after
        // the physics step, one frame too late for the pause frame.
        bool pause_sensor_paused(CKIpionManager* physics, uint32_t behavior_id) {
            if (!behavior_id || !physics->m_Context) return false;
            CKBehavior* sensor = CKBehavior::Cast(physics->m_Context->GetObject(behavior_id));
            return sensor && !sensor->IsActive();
        }

        // Queued into m_PreSimulateCallbacks and left there for the whole
        // session, unlike the one-shot callbacks above: the pass runs after
        // SetDeltaTime recomputed the step from whatever factor the level
        // scripts left behind, so the factor is put back and the physics delta
        // recomputed with it before simulate_dtime() reads it.  Return 0 keeps
        // it queued; 1 (once disabled) lets the container delete it.
        class clock_guard_callback final : public PhysicsCallback {
        public:
            clock_guard_callback(CKIpionManager* manager, CKBehavior* behavior)
                : PhysicsCallback(manager, behavior, 2) {}
            ~clock_guard_callback() override {
                auto it = clock_guards().find(m_IpionManager);
                if (it != clock_guards().end()) it->second.callback_queued = false;
            }
            int Execute() override {
                auto it = clock_guards().find(m_IpionManager);
                if (it == clock_guards().end() || !it->second.enabled) return 1;
                clock_guard_entry& guard = it->second;
                const bool paused = pause_sensor_paused(m_IpionManager, guard.pause_behavior);
                // Pinned while the menu is open and for the single pass in
                // which it closes: that is the pass the unpause chain uses to
                // write 2.0, which the run before the pause may not have had.
                // Every other pass only samples, so the level scripts stay in
                // charge of the clock exactly as they are on the server.
                const bool hold = paused || guard.paused;
                guard.paused = paused;
                if (!hold) guard.target = m_IpionManager->m_PhysicsTimeFactor;
                if (m_IpionManager->m_PhysicsTimeFactor != guard.target) {
                    m_IpionManager->m_PhysicsTimeFactor = guard.target;
                    m_IpionManager->m_PhysicsDeltaTime = m_IpionManager->m_DeltaTime * guard.target;
                }
                return 0;
            }
        };
    }

    bool set_clock_guard(CKIpionManager* physics, bool enable, float time_factor, uint32_t behavior_id,
                         uint32_t pause_behavior_id, std::string& error) {
        error.clear();
        if (!physics) {
            error = "the clock guard needs an existing physics manager";
            return false;
        }
        if (!enable) {
            // The queued callback sees enabled == false at its next pass and
            // removes itself; nothing is deleted from under the container.
            // callback_queued stays set until its destructor runs, so enabling
            // again before that pass simply resumes the same callback.
            auto it = clock_guards().find(physics);
            if (it != clock_guards().end()) it->second.enabled = false;
            return true;
        }
        clock_guard_entry& guard = clock_guards()[physics];
        guard.enabled = true;
        guard.pause_behavior = pause_behavior_id;
        if (guard.callback_queued) return true;
        guard.target = time_factor < 0.0f ? 0.0f : time_factor;
        guard.paused = pause_sensor_paused(physics, pause_behavior_id);
        CKBehavior* behavior = behavior_id && physics->m_Context
            ? CKBehavior::Cast(physics->m_Context->GetObject(behavior_id)) : nullptr;
        if (!behavior || !physics->m_PreSimulateCallbacks) {
            error = "the clock guard needs an existing behavior to attach to";
            return false;
        }
        guard.callback_queued = true;   // before Process: the callback may run inside it
        physics->m_PreSimulateCallbacks->Process(new clock_guard_callback(physics, behavior));
        return guard.callback_queued;
    }
}
