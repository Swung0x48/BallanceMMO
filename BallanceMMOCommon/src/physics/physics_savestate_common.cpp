// The IVP savestate (docs/ivp-savestate-design.md): what both halves share -
// the live-world index, edge naming, the blob's byte format, its text
// rendering and diff - and the public entries except set_savestate (which
// lives with the restore in physics_savestate_restore.cpp).
//
// No floating-point arithmetic here: every physics value is copied, and the
// text rendering prints floats as hex (%a) so a one-ulp change shows.

#include "CKAll.h"
#include "CKIpionManager.h"

#include "physics_savestate_internal.hpp"

#include "ivp_surface_manager.hxx"
#include "ivp_surman_polygon.hxx"
#include "ivp_compact_surface.hxx"
#include "ivp_compact_ledge.hxx"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace bmmo::physics::savestate {

    // ------------------------------------------------------------------
    // The live world
    // ------------------------------------------------------------------

    bool index_live_world(CKIpionManager* physics, live_world& out, std::string& error, bool names) {
        out = live_world{};
        error.clear();
        if (!physics) {
            error = "no physics manager";
            return false;
        }
        IVP_Environment* env = physics->GetEnvironment();
        if (!env) {
            error = "physics environment is unavailable";
            return false;
        }
        out.physics = physics;
        out.env = env;
        CKContext* context = physics->m_Context;
        // One object into the table: its core, name, entity, surface.
        auto add_object = [&](IVP_Real_Object* real, CK3dEntity* entity) -> bool {
            live_object lo;
            lo.object = real;
            lo.core = real->get_core();
            lo.name = real->get_name() ? real->get_name() : "";
            lo.entity = entity;
            lo.entity_id = entity ? static_cast<uint32_t>(entity->GetID()) : 0u;
            if (real->get_controller_phantom()) {
                error = "object '" + lo.name + "' is a phantom: the savestate fails closed on phantoms";
                return false;
            }
            if (!lo.core) {
                error = "object '" + lo.name + "' has no core";
                return false;
            }
            if (real->get_environment() != env) {
                error = "object '" + lo.name + "' belongs to another environment";
                return false;
            }
            if (IVP_SurfaceManager* manager = real->get_surface_manager()) {
                if (manager->get_type() == IVP_SURMAN_POLYGON) {
                    const IVP_Compact_Surface* surface =
                        static_cast<IVP_SurfaceManager_Polygon*>(manager)->get_compact_surface();
                    if (surface) {
                        lo.surface_base = reinterpret_cast<const char*>(surface);
                        const int size = surface->get_size();
                        lo.surface_size = size > 0 ? static_cast<uint32_t>(size) : 0u;
                    }
                } else if (manager->get_type() == IVP_SURMAN_BALL) {
                    // Every ball shares one static dummy ledge.
                    const IVP_Compact_Ledge* ledge = manager->get_single_convex();
                    if (ledge && ledge->get_first_triangle())
                        lo.ball_edge = ledge->get_first_triangle()->get_first_edge();
                }
            }
            out.objects.push_back(std::move(lo));
            return true;
        };
        ptr_index tabled;
        tabled.reserve(static_cast<size_t>(physics->m_PhysicsObjects.Size()) + 8);
        out.objects.reserve(static_cast<size_t>(physics->m_PhysicsObjects.Size()) + 8);
        for (auto it = physics->m_PhysicsObjects.Begin(); it != physics->m_PhysicsObjects.End(); ++it) {
            IVP_Real_Object* real = (*it).m_RealObject;
            if (!real) continue;
            // The table is keyed by the entity's CK_ID; a lookup by id cannot
            // hand back a dangling pointer the way client_data could.
            CK3dEntity* entity = context ? CK3dEntity::Cast(context->GetObject(it.GetKey()))
                                         : static_cast<CK3dEntity*>(real->client_data);
            if (!add_object(real, entity)) return false;
            tabled.emplace(real, 0);
        }
        // Objects IVP owns outside the manager's table: the environment's own
        // static object (every environment creates one - a ball with a core
        // and a simulation unit of its own, named "static_object") and
        // anything else that sits in the cluster tree.  They have no entity.
        {
            std::vector<IVP_Cluster*> stack;
            if (IVP_Cluster* root = env->get_root_cluster()) stack.push_back(root);
            while (!stack.empty()) {
                IVP_Cluster* cluster = stack.back();
                stack.pop_back();
                for (IVP_Object* o = cluster->get_first_object_of_cluster(); o; o = cluster->get_next_object_in_cluster(o)) {
                    if (o->get_type() == IVP_CLUSTER) {
                        stack.push_back(o->to_cluster());
                        continue;
                    }
                    IVP_Real_Object* real = o->to_real();
                    if (!real || tabled.find(real) >= 0) continue;
                    if (!add_object(real, nullptr)) return false;
                    tabled.emplace(real, 0);
                }
            }
        }
        // By name, then by entity id: two bodies of one name (two instances
        // of one element type on a map) get a stable order and are told apart
        // by the id the record carries (live_world::find_by_name).
        std::sort(out.objects.begin(), out.objects.end(), [](const live_object& a, const live_object& b) {
            return a.name != b.name ? a.name < b.name : a.entity_id < b.entity_id;
        });
        out.by_object.reserve(out.objects.size());
        for (size_t i = 0; i < out.objects.size(); ++i) {
            const live_object& lo = out.objects[i];
            if (i > 0 && out.objects[i - 1].name == lo.name && out.objects[i - 1].entity_id == lo.entity_id) {
                error = "two bodies named '" + lo.name + "' share entity id " + std::to_string(lo.entity_id);
                return false;
            }
            if (!out.by_object.emplace(lo.object, static_cast<int32_t>(i))) {
                error = "object '" + lo.name + "' is listed twice in the physics table";
                return false;
            }
            if (names) out.by_name.emplace(lo.name, static_cast<int32_t>(i));
        }
        return true;
    }

    edge_ref name_edge(const live_world& live, const IVP_Real_Object* object, const IVP_Compact_Edge* edge,
                       IVP_SYNAPSE_POLYGON_STATUS status) {
        edge_ref ref;
        const int status_value = static_cast<int>(status);
        // object stays -1 (unnamed) on every failure; the caller fails closed.
        if (status_value < 0 || status_value > static_cast<int>(IVP_ST_BACKSIDE)) return ref;
        ref.status = static_cast<uint8_t>(status_value);
        const int32_t index = live.index_of(object);
        if (index < 0 || !edge) return ref;
        const live_object& lo = live.objects[static_cast<size_t>(index)];
        if (lo.ball_edge && edge == lo.ball_edge) {
            ref.object = index;
            ref.offset = -1;
            return ref;
        }
        if (!lo.surface_base) return ref;
        const uintptr_t base = reinterpret_cast<uintptr_t>(lo.surface_base);
        const uintptr_t at = reinterpret_cast<uintptr_t>(edge);
        if (at < base) return ref;
        const uintptr_t offset = at - base;
        if (offset > 0x7fffffffu || offset + sizeof(IVP_Compact_Edge) > lo.surface_size) return ref;
        ref.object = index;
        ref.offset = static_cast<int32_t>(offset);
        return ref;
    }

    const IVP_Compact_Edge* resolve_edge(const live_world& live, const edge_ref& ref) {
        if (ref.object < 0 || static_cast<size_t>(ref.object) >= live.objects.size()) return nullptr;
        const live_object& lo = live.objects[static_cast<size_t>(ref.object)];
        if (ref.offset == -1) return lo.ball_edge;   // nullptr for a polygon: wrong type
        if (ref.offset < 0 || !lo.surface_base) return nullptr;   // a ball has no surface bytes
        if (static_cast<size_t>(ref.offset) + sizeof(IVP_Compact_Edge) > lo.surface_size) return nullptr;
        return reinterpret_cast<const IVP_Compact_Edge*>(lo.surface_base + ref.offset);
    }

    namespace {
        std::string object_name(const world_record& world, int32_t index) {
            if (index == -1) return "-";
            if (index < 0 || static_cast<size_t>(index) >= world.objects.size()) return "?" + std::to_string(index);
            return world.objects[static_cast<size_t>(index)].name;
        }
    }

    std::string describe_edge(const world_record& world, const edge_ref& ref) {
        std::string out = object_name(world, ref.object);
        out += '@';
        out += ref.offset == -1 ? std::string("ball") : std::to_string(ref.offset);
        out += '/';
        out += std::to_string(static_cast<unsigned>(ref.status));
        return out;
    }

    void warn(savestate_report* report, const std::string& line) {
        if (!report) return;
        constexpr size_t kMaxWarnings = 64;
        if (report->warnings.size() < kMaxWarnings)
            report->warnings.push_back(line);
        else if (report->warnings.size() == kMaxWarnings)
            report->warnings.push_back("... further warnings dropped");
    }

    // ------------------------------------------------------------------
    // Bytes.  One field list (io below) drives both the writer and the
    // reader, so the two can never disagree on the order.
    // ------------------------------------------------------------------
    namespace {
        struct out_io {
            writer& w;
            static constexpr bool reading = false;
            uint32_t count(size_t n) {
                w.u32(static_cast<uint32_t>(n));
                return static_cast<uint32_t>(n);
            }
            void v(uint8_t& x) { w.u8(x); }
            void v(int8_t& x) { w.i8(x); }
            void v(uint16_t& x) { w.u16(x); }
            void v(int16_t& x) { w.i16(x); }
            void v(uint32_t& x) { w.u32(x); }
            void v(int32_t& x) { w.i32(x); }
            void v(uint64_t& x) { w.u64(x); }
            void v(float& x) { w.f32(x); }
            void v(double& x) { w.f64(x); }
            void v(std::string& x) { w.str(x); }
        };

        struct in_io {
            reader& r;
            static constexpr bool reading = true;
            uint32_t count(size_t) { return r.count(); }
            void v(uint8_t& x) { x = r.u8(); }
            void v(int8_t& x) { x = r.i8(); }
            void v(uint16_t& x) { x = r.u16(); }
            void v(int16_t& x) { x = r.i16(); }
            void v(uint32_t& x) { x = r.u32(); }
            void v(int32_t& x) { x = r.i32(); }
            void v(uint64_t& x) { x = r.u64(); }
            void v(float& x) { x = r.f32(); }
            void v(double& x) { x = r.f64(); }
            void v(std::string& x) { x = r.str(); }
        };

        // Every overload lives in this namespace, next to out_io/in_io, so
        // the dependent calls inside the templates find them all by ADL.
        // Declared inline: the x64 build is RelWithDebInfo (/Ob1), which
        // inlines only what says so, and a blob with every member's trafo
        // pieces is tens of thousands of these calls (step TR3).
        template <class IO, class T>
        inline typename std::enable_if<std::is_arithmetic<T>::value>::type io(IO& s, T& x) { s.v(x); }
        template <class IO>
        inline void io(IO& s, std::string& x) { s.v(x); }
        template <class IO, class T, size_t N>
        inline void io(IO& s, T (&a)[N]) {
            for (size_t i = 0; i < N; ++i) io(s, a[i]);
        }
        template <class IO, class T>
        inline void io(IO& s, std::vector<T>& v) {
            const uint32_t n = s.count(v.size());
            if constexpr (IO::reading) v.assign(n, T());
            for (size_t i = 0; i < v.size(); ++i) io(s, v[i]);
        }

        template <class IO>
        inline void io(IO& s, edge_ref& e) {
            io(s, e.object);
            io(s, e.offset);
            io(s, e.status);
        }

        template <class IO>
        inline void io(IO& s, controller_ref& c) {
            io(s, c.kind);
            io(s, c.system);
            io(s, c.part);
            io(s, c.priority);
            io(s, c.name);
        }

        template <class IO>
        inline void io(IO& s, core_record& c) {
            io(s, c.movement_state);
            io(s, c.temporarily_unmovable);
            io(s, c.pinned);
            io(s, c.fast_piling);
            io(s, c.in_revive_list);
            io(s, c.impacts_since_last_psi);
            io(s, c.time_of_last_psi);
            io(s, c.i_delta_time);
            io(s, c.rot_speed_change);
            io(s, c.speed_change);
            io(s, c.rot_speed);
            io(s, c.speed);
            io(s, c.pos_last_psi);
            io(s, c.delta_psis);
            io(s, c.q_last);
            io(s, c.q_next);
            io(s, c.m_last_psi);
            io(s, c.rotation_axis_ws);
            io(s, c.current_speed);
            io(s, c.abs_omega);
            io(s, c.max_surface_rot_speed);
            io(s, c.calm_time);
            io(s, c.calm_quat);
            io(s, c.calm_position);
            io(s, c.unit);
            io(s, c.controllers);
        }

        template <class IO>
        inline void io(IO& s, hull_entry& h) {
            io(s, h.kind);
            io(s, h.collision);
            io(s, h.side);
            io(s, h.value);
        }

        template <class IO>
        inline void io(IO& s, object_record& o) {
            io(s, o.name);
            io(s, o.entity_id);
            io(s, o.type);
            io(s, o.movable);
            io(s, o.movement_state);
            io(s, o.surface_size);
            io(s, o.core);
            io(s, o.last_vpsi_time);
            io(s, o.gradient);
            io(s, o.center_gradient);
            io(s, o.hull_value_last_vpsi);
            io(s, o.hull_center_value_last_vpsi);
            io(s, o.hull_value_next_psi);
            io(s, o.time_of_next_reset);
            io(s, o.hull);
            io(s, o.has_ov);
            io(s, o.ov_in_hull);
            io(s, o.ov_center);
            io(s, o.ov_radius);
            io(s, o.ov_node);
            io(s, o.ov_collisions);
            io(s, o.exact_synapses);
            io(s, o.invalid_synapses);
            io(s, o.friction_synapses);
            io(s, o.has_entity_matrix);
            io(s, o.entity_matrix);
        }

        template <class IO>
        inline void io(IO& s, collision_record& c) {
            io(s, c.kind);
            io(s, c.parent);
            io(s, c.objects);
            io(s, c.synapse);
            io(s, c.coll_type);
            io(s, c.sort_flag);
            io(s, c.phantom_set);
            io(s, c.function);
            io(s, c.recalc_result);
            io(s, c.disable_halfspace);
            io(s, c.status);
            io(s, c.selector);
            io(s, c.counter);
            io(s, c.recalc_fresh);
            io(s, c.sum_extra_radius);
            io(s, c.len_numerator);
            io(s, c.contact_dot_diff_center);
            io(s, c.sum_angular_hull_time);
            io(s, c.contact_plane);
            io(s, c.scheduled);
            io(s, c.event_value);
            io(s, c.recursive_status);
            io(s, c.spawned_count);
            io(s, c.children);
        }

        template <class IO>
        inline void io(IO& s, contact_record& c) {
            io(s, c.synapse);
            io(s, c.system);
            io(s, c.gap);
            io(s, c.span);
            io(s, c.pressure);
            io(s, c.friction_factor);
            io(s, c.destroyed_energy);
            io(s, c.inv_triangle_det);
            io(s, c.old_energy);
            io(s, c.inv_virt_mass);
            io(s, c.pull);
            io(s, c.keeper);
            io(s, c.status);
            io(s, c.two_friction);
            io(s, c.recalc_time);
            io(s, c.contact_ws);
            io(s, c.normal_ws);
        }

        template <class IO>
        inline void io(IO& s, pair_record& p) {
            io(s, p.objects);
            io(s, p.next_ease);
            io(s, p.last_impact_time);
            io(s, p.anti_energy);
            io(s, p.dists);
        }

        template <class IO>
        inline void io(IO& s, friction_info_record& f) {
            io(s, f.object);
            io(s, f.springs);
        }

        template <class IO>
        inline void io(IO& s, friction_system_record& f) {
            io(s, f.cores);
            io(s, f.movable_cores);
            io(s, f.dists);
            io(s, f.pairs);
            io(s, f.infos);
            io(s, f.obj_number);
            io(s, f.dist_number);
            io(s, f.complex_not_necessary);
            io(s, f.union_find_necessary);
            io(s, f.simulated);
        }

        template <class IO>
        inline void io(IO& s, unit_record& u) {
            io(s, u.cores);
            io(s, u.movement_type);
            io(s, u.union_find_needed);
            io(s, u.has_fast_objects);
            io(s, u.just_slowed_down);
            io(s, u.next_movement_check);
            io(s, u.in_still_slot);
            io(s, u.controllers);
        }

        template <class IO>
        inline void io(IO& s, event_record& e) {
            io(s, e.collision);
            io(s, e.value);
        }

        template <class IO>
        inline void io(IO& s, ov_node_record& n) {
            io(s, n.x);
            io(s, n.y);
            io(s, n.z);
            io(s, n.rasterlevel);
            io(s, n.sizelevel);
            io(s, n.parent);
            io(s, n.children);
            io(s, n.elements);
        }

        template <class IO>
        inline void io(IO& s, cache_slot_record& c) {
            io(s, c.object);
            io(s, c.fresh);
            io(s, c.q);
            io(s, c.m);
            io(s, c.core_pos);
        }

        template <class IO>
        inline void io(IO& s, callback_record& c) {
            io(s, c.slot);
            io(s, c.behavior);
            io(s, c.type);
        }

        template <class IO>
        inline void io(IO& s, world_record& w) {
            io(s, w.environment_id);
            io(s, w.current_time);
            io(s, w.time_of_last_psi);
            io(s, w.time_of_next_psi);
            io(s, w.base_time);
            io(s, w.last_time);
            io(s, w.next_movement_check);
            io(s, w.env_state);
            io(s, w.gravity);
            io(s, w.delta_time_ms);
            io(s, w.physics_delta_time);
            io(s, w.time_factor);
            io(s, w.ivp_seed);
            io(s, w.random_state);
            io(s, w.movable_objects);
            io(s, w.callbacks);
            io(s, w.objects);
            io(s, w.collisions);
            io(s, w.exact_list);
            io(s, w.invalid_list);
            io(s, w.events);
            io(s, w.contacts);
            io(s, w.systems);
            io(s, w.units);
            io(s, w.revive_list);
            io(s, w.ov_nodes);
            io(s, w.ov_root);
            io(s, w.cache_slots);
            io(s, w.cache_reuse_index);
        }

        // ------------------------------------------------------------------
        // Validation: every index in range, every kind known.
        // ------------------------------------------------------------------
        struct validator {
            const world_record& w;
            std::string error;
            int32_t n_objects, n_collisions, n_contacts, n_systems, n_units, n_nodes;

            explicit validator(const world_record& world)
                : w(world),
                  n_objects(static_cast<int32_t>(world.objects.size())),
                  n_collisions(static_cast<int32_t>(world.collisions.size())),
                  n_contacts(static_cast<int32_t>(world.contacts.size())),
                  n_systems(static_cast<int32_t>(world.systems.size())),
                  n_units(static_cast<int32_t>(world.units.size())),
                  n_nodes(static_cast<int32_t>(world.ov_nodes.size())) {}

            bool fail(const std::string& what, int64_t value) {
                if (error.empty()) error = what + " out of range (" + std::to_string(value) + ")";
                return false;
            }
            static bool in(int64_t v, int32_t n) { return v >= 0 && v < n; }
            static bool in_or_none(int64_t v, int32_t n) { return v == -1 || in(v, n); }

            bool list(const std::vector<int32_t>& v, int32_t n, const std::string& what) {
                for (int32_t x : v)
                    if (!in(x, n)) return fail(what, x);
                return true;
            }
            bool encoded(const std::vector<int32_t>& v, int32_t n, const std::string& what) {
                for (int32_t x : v)
                    if (x < 0 || !in(x >> 1, n)) return fail(what, x);
                return true;
            }
            bool edge(const edge_ref& e, const std::string& what) {
                if (!in(e.object, n_objects)) return fail(what + " object", e.object);
                if (e.offset < -1) return fail(what + " offset", e.offset);
                // A ball's edge is the shared dummy (-1); a polygon's lies inside its surface.
                if (e.offset >= 0
                    && static_cast<uint64_t>(e.offset) + sizeof(IVP_Compact_Edge) > w.objects[static_cast<size_t>(e.object)].surface_size)
                    return fail(what + " offset", e.offset);
                return true;
            }
            bool controllers(const std::vector<controller_ref>& v, const std::string& what) {
                for (const auto& c : v) {
                    if (c.kind > 1) return fail(what + " controller kind", c.kind);
                    if (c.kind == 0) {
                        if (!in(c.system, n_systems)) return fail(what + " controller system", c.system);
                        if (c.part > 2) return fail(what + " controller part", c.part);
                    }
                }
                return true;
            }

            bool run() {
                if (!list(w.movable_objects, n_objects, "movable object")) return false;
                for (const auto& cb : w.callbacks)
                    if (cb.slot < 0 || cb.slot > 2) return fail("callback slot", cb.slot);
                for (size_t i = 0; i < w.objects.size(); ++i) {
                    const object_record& o = w.objects[i];
                    const std::string at = "object '" + o.name + "'";
                    if (!in_or_none(o.core.unit, n_units)) return fail(at + " unit", o.core.unit);
                    if (!controllers(o.core.controllers, at)) return false;
                    for (const auto& h : o.hull) {
                        if (h.kind > 1) return fail(at + " hull entry kind", h.kind);
                        if (h.kind == 1) {
                            if (!in(h.collision, n_collisions)) return fail(at + " hull entry collision", h.collision);
                            if (h.side > 1) return fail(at + " hull entry side", h.side);
                        }
                    }
                    if (!in_or_none(o.ov_node, n_nodes)) return fail(at + " ov node", o.ov_node);
                    if (!list(o.ov_collisions, n_collisions, at + " ov collision")) return false;
                    if (!encoded(o.exact_synapses, n_collisions, at + " exact synapse")) return false;
                    if (!encoded(o.invalid_synapses, n_collisions, at + " invalid synapse")) return false;
                    if (!encoded(o.friction_synapses, n_contacts, at + " friction synapse")) return false;
                }
                for (int32_t i = 0; i < n_collisions; ++i) {
                    const collision_record& c = w.collisions[static_cast<size_t>(i)];
                    const std::string at = "collision #" + std::to_string(i);
                    if (c.kind > 2) return fail(at + " kind", c.kind);
                    // Parents come before their children (the restore builds in order).
                    if (c.parent != -1 && !(c.parent >= 0 && c.parent < i)) return fail(at + " parent", c.parent);
                    if (c.parent != -1 && w.collisions[static_cast<size_t>(c.parent)].kind == 0)
                        return fail(at + " parent kind", w.collisions[static_cast<size_t>(c.parent)].kind);
                    for (int k = 0; k < 2; ++k)
                        if (!in(c.objects[k], n_objects)) return fail(at + " object", c.objects[k]);
                    if (c.kind != 2)
                        for (int k = 0; k < 2; ++k)
                            if (!edge(c.synapse[k], at + " synapse")) return false;
                    for (int32_t child : c.children)
                        if (!(child > i && child < n_collisions)) return fail(at + " child", child);
                    if (c.kind == 0 && !c.children.empty()) return fail(at + " children of a plain mindist", static_cast<int64_t>(c.children.size()));
                }
                if (!list(w.exact_list, n_collisions, "exact list")) return false;
                if (!list(w.invalid_list, n_collisions, "invalid list")) return false;
                for (const auto& e : w.events)
                    if (!in_or_none(e.collision, n_collisions)) return fail("event collision", e.collision);
                for (int32_t i = 0; i < n_contacts; ++i) {
                    const contact_record& c = w.contacts[static_cast<size_t>(i)];
                    const std::string at = "contact #" + std::to_string(i);
                    for (int k = 0; k < 2; ++k)
                        if (!edge(c.synapse[k], at + " synapse")) return false;
                    if (!in(c.system, n_systems)) return fail(at + " system", c.system);
                }
                for (int32_t i = 0; i < n_systems; ++i) {
                    const friction_system_record& s = w.systems[static_cast<size_t>(i)];
                    const std::string at = "system #" + std::to_string(i);
                    if (!list(s.cores, n_objects, at + " core")) return false;
                    if (!list(s.movable_cores, n_objects, at + " movable core")) return false;
                    if (!list(s.dists, n_contacts, at + " dist")) return false;
                    for (const auto& p : s.pairs) {
                        for (int k = 0; k < 2; ++k)
                            if (!in(p.objects[k], n_objects)) return fail(at + " pair object", p.objects[k]);
                        if (!list(p.dists, n_contacts, at + " pair dist")) return false;
                    }
                    for (const auto& info : s.infos) {
                        if (!in(info.object, n_objects)) return fail(at + " info object", info.object);
                        if (!list(info.springs, n_contacts, at + " info spring")) return false;
                    }
                }
                for (int32_t i = 0; i < n_units; ++i) {
                    const unit_record& u = w.units[static_cast<size_t>(i)];
                    const std::string at = "unit #" + std::to_string(i);
                    if (!list(u.cores, n_objects, at + " core")) return false;
                    if (!controllers(u.controllers, at)) return false;
                }
                if (!list(w.revive_list, n_objects, "revive list")) return false;
                for (int32_t i = 0; i < n_nodes; ++i) {
                    const ov_node_record& n = w.ov_nodes[static_cast<size_t>(i)];
                    const std::string at = "ov node #" + std::to_string(i);
                    if (!in_or_none(n.parent, n_nodes)) return fail(at + " parent", n.parent);
                    if (!list(n.children, n_nodes, at + " child")) return false;
                    if (!list(n.elements, n_objects, at + " element")) return false;
                }
                if (!in_or_none(w.ov_root, n_nodes)) return fail("ov root", w.ov_root);
                for (const auto& c : w.cache_slots)
                    if (!in_or_none(c.object, n_objects)) return fail("cache slot object", c.object);
                return true;
            }
        };
    }

    void write_world(writer& w, const world_record& world) {
        w.u32(kMagic);
        w.u32(kFormatVersion);
        out_io o{w};
        // The writer only reads the record; one field list serves both ways.
        io(o, const_cast<world_record&>(world));
    }

    bool read_world(reader& r, world_record& world, std::string& error) {
        error.clear();
        world = world_record{};
        const uint32_t magic = r.u32();
        const uint32_t version = r.u32();
        if (!r.ok()) {
            error = "savestate truncated (no header)";
            return false;
        }
        if (magic != kMagic) {
            char text[64];
            std::snprintf(text, sizeof(text), "not a savestate (magic %08x)", static_cast<unsigned>(magic));
            error = text;
            return false;
        }
        if (version != kFormatVersion) {
            error = "savestate format " + std::to_string(version) + ", this build reads " + std::to_string(kFormatVersion);
            return false;
        }
        in_io i{r};
        io(i, world);
        if (!r.ok()) {
            error = "savestate truncated or corrupt at byte " + std::to_string(r.pos) + " of " + std::to_string(r.size);
            world = world_record{};
            return false;
        }
        if (r.pos != r.size) {
            error = "savestate has " + std::to_string(r.size - r.pos) + " trailing bytes";
            world = world_record{};
            return false;
        }
        validator v(world);
        if (!v.run()) {
            error = "savestate invalid: " + v.error;
            world = world_record{};
            return false;
        }
        return true;
    }

    // ------------------------------------------------------------------
    // Text.  One flat list of (kind, name, field, value) entries is built
    // per world; describe joins a record's entries on one line, diff walks
    // two lists to the first difference.  Counts come first, so a
    // structural difference shows as a count.
    // ------------------------------------------------------------------
    namespace {
        struct field_entry {
            std::string kind, name, field, value;
        };

        std::string hex(double v) {
            char text[64];
            std::snprintf(text, sizeof(text), "%a", v);
            return text;
        }
        std::string hex(float v) { return hex(static_cast<double>(v)); }
        std::string num(int64_t v) { return std::to_string(v); }

        template <class T, size_t N>
        std::string hex_list(const T (&a)[N]) {
            std::string out;
            for (size_t i = 0; i < N; ++i) {
                if (i) out += ',';
                out += hex(a[i]);
            }
            return out;
        }

        class emitter {
        public:
            explicit emitter(const world_record& w) : world(w) {}
            const world_record& world;
            std::vector<field_entry> items;

            void rec(const std::string& kind, const std::string& name) {
                kind_ = kind;
                name_ = name;
            }
            void put(const char* field, const std::string& value) {
                items.push_back(field_entry{kind_, name_, field, value});
            }
            void put(const char* field, int64_t value) { put(field, num(value)); }
            void putf(const char* field, float value) { put(field, hex(value)); }
            void putd(const char* field, double value) { put(field, hex(value)); }

            std::string obj(int32_t i) const { return object_name(world, i); }
            std::string coll(int32_t i) const {
                if (i == -1) return "-";
                std::string out = "#" + std::to_string(i);
                if (i >= 0 && static_cast<size_t>(i) < world.collisions.size()) {
                    const collision_record& c = world.collisions[static_cast<size_t>(i)];
                    out += "(" + obj(c.objects[0]) + "|" + obj(c.objects[1]) + ")";
                }
                return out;
            }
            std::string contact(int32_t i) const {
                if (i == -1) return "-";
                std::string out = "#" + std::to_string(i);
                if (i >= 0 && static_cast<size_t>(i) < world.contacts.size()) {
                    const contact_record& c = world.contacts[static_cast<size_t>(i)];
                    out += "(" + obj(c.synapse[0].object) + "|" + obj(c.synapse[1].object) + ")";
                }
                return out;
            }
            std::string objects(const std::vector<int32_t>& v) const {
                std::string out = "[";
                for (size_t i = 0; i < v.size(); ++i) {
                    if (i) out += ',';
                    out += obj(v[i]);
                }
                return out + "]";
            }
            std::string colls(const std::vector<int32_t>& v) const {
                std::string out = "[";
                for (size_t i = 0; i < v.size(); ++i) {
                    if (i) out += ',';
                    out += coll(v[i]);
                }
                return out + "]";
            }
            std::string contacts(const std::vector<int32_t>& v) const {
                std::string out = "[";
                for (size_t i = 0; i < v.size(); ++i) {
                    if (i) out += ',';
                    out += contact(v[i]);
                }
                return out + "]";
            }
            static std::string indices(const std::vector<int32_t>& v) {
                std::string out = "[";
                for (size_t i = 0; i < v.size(); ++i) {
                    if (i) out += ',';
                    out += std::to_string(v[i]);
                }
                return out + "]";
            }
            // (collision << 1) | side
            std::string coll_sides(const std::vector<int32_t>& v) const {
                std::string out = "[";
                for (size_t i = 0; i < v.size(); ++i) {
                    if (i) out += ',';
                    out += coll(v[i] >> 1) + "." + std::to_string(v[i] & 1);
                }
                return out + "]";
            }
            std::string contact_sides(const std::vector<int32_t>& v) const {
                std::string out = "[";
                for (size_t i = 0; i < v.size(); ++i) {
                    if (i) out += ',';
                    out += contact(v[i] >> 1) + "." + std::to_string(v[i] & 1);
                }
                return out + "]";
            }
            static std::string controllers(const std::vector<controller_ref>& v) {
                std::string out = "[";
                for (size_t i = 0; i < v.size(); ++i) {
                    const controller_ref& c = v[i];
                    if (i) out += ',';
                    out += "{k" + std::to_string(static_cast<unsigned>(c.kind)) + " s" + std::to_string(c.system) + " p"
                         + std::to_string(static_cast<unsigned>(c.part)) + " " + std::to_string(c.priority) + " " + c.name + "}";
                }
                return out + "]";
            }

        private:
            std::string kind_, name_;
        };

        void emit_world(emitter& e) {
            const world_record& w = e.world;
            e.rec("world", "-");
            e.put("objects", static_cast<int64_t>(w.objects.size()));
            e.put("collisions", static_cast<int64_t>(w.collisions.size()));
            e.put("exact_list", static_cast<int64_t>(w.exact_list.size()));
            e.put("invalid_list", static_cast<int64_t>(w.invalid_list.size()));
            e.put("events", static_cast<int64_t>(w.events.size()));
            e.put("contacts", static_cast<int64_t>(w.contacts.size()));
            e.put("systems", static_cast<int64_t>(w.systems.size()));
            e.put("units", static_cast<int64_t>(w.units.size()));
            e.put("revive_list", static_cast<int64_t>(w.revive_list.size()));
            e.put("ov_nodes", static_cast<int64_t>(w.ov_nodes.size()));
            e.put("cache_slots", static_cast<int64_t>(w.cache_slots.size()));
            e.put("movable_objects", static_cast<int64_t>(w.movable_objects.size()));
            e.put("callbacks", static_cast<int64_t>(w.callbacks.size()));

            e.rec("clock", "-");
            {
                char text[32];
                std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(w.environment_id));
                e.put("environment", text);
            }
            e.putd("current_time", w.current_time);
            e.putd("time_of_last_psi", w.time_of_last_psi);
            e.putd("time_of_next_psi", w.time_of_next_psi);
            e.putd("base_time", w.base_time);
            e.putf("last_time", w.last_time);
            e.put("next_movement_check", w.next_movement_check);
            e.put("env_state", w.env_state);
            e.put("gravity", hex_list(w.gravity));
            e.putf("delta_time_ms", w.delta_time_ms);
            e.putf("physics_delta_time", w.physics_delta_time);
            e.putf("time_factor", w.time_factor);
            e.put("ivp_seed", w.ivp_seed);
            e.put("random_state", w.random_state);
            e.put("ov_root", w.ov_root);
            e.put("cache_reuse_index", w.cache_reuse_index);

            e.rec("movable", "-");
            e.put("objects", e.objects(w.movable_objects));

            for (size_t i = 0; i < w.callbacks.size(); ++i) {
                const callback_record& c = w.callbacks[i];
                e.rec("callback", "#" + std::to_string(i));
                e.put("slot", c.slot);
                e.put("behavior", static_cast<int64_t>(c.behavior));
                e.put("type", c.type);
            }

            for (const object_record& o : w.objects) {
                e.rec("object", o.name);
                e.put("entity_id", static_cast<int64_t>(o.entity_id));
                e.put("type", o.type);
                e.put("movable", o.movable);
                e.put("movement_state", o.movement_state);
                e.put("surface_size", static_cast<int64_t>(o.surface_size));
                e.put("has_entity_matrix", o.has_entity_matrix);
                e.put("entity_matrix", hex_list(o.entity_matrix));

                const core_record& c = o.core;
                e.rec("core", o.name);
                e.put("movement_state", c.movement_state);
                e.put("temporarily_unmovable", c.temporarily_unmovable);
                e.put("pinned", c.pinned);
                e.put("fast_piling", c.fast_piling);
                e.put("in_revive_list", c.in_revive_list);
                e.put("impacts_since_last_psi", c.impacts_since_last_psi);
                e.putd("time_of_last_psi", c.time_of_last_psi);
                e.putf("i_delta_time", c.i_delta_time);
                e.put("rot_speed_change", hex_list(c.rot_speed_change));
                e.put("speed_change", hex_list(c.speed_change));
                e.put("rot_speed", hex_list(c.rot_speed));
                e.put("speed", hex_list(c.speed));
                e.put("pos_last_psi", hex_list(c.pos_last_psi));
                e.put("delta_psis", hex_list(c.delta_psis));
                e.put("q_last", hex_list(c.q_last));
                e.put("q_next", hex_list(c.q_next));
                e.put("m_last_psi", hex_list(c.m_last_psi));
                e.put("rotation_axis_ws", hex_list(c.rotation_axis_ws));
                e.putf("current_speed", c.current_speed);
                e.putf("abs_omega", c.abs_omega);
                e.putf("max_surface_rot_speed", c.max_surface_rot_speed);
                e.put("calm_time", hex_list(c.calm_time));
                e.put("calm_quat0", hex_list(c.calm_quat[0]));
                e.put("calm_quat1", hex_list(c.calm_quat[1]));
                e.put("calm_position0", hex_list(c.calm_position[0]));
                e.put("calm_position1", hex_list(c.calm_position[1]));
                e.put("unit", c.unit);
                e.put("controllers", emitter::controllers(c.controllers));

                e.rec("hull", o.name);
                e.putd("last_vpsi_time", o.last_vpsi_time);
                e.putf("gradient", o.gradient);
                e.putf("center_gradient", o.center_gradient);
                e.putf("hull_value_last_vpsi", o.hull_value_last_vpsi);
                e.putf("hull_center_value_last_vpsi", o.hull_center_value_last_vpsi);
                e.putf("hull_value_next_psi", o.hull_value_next_psi);
                e.put("time_of_next_reset", o.time_of_next_reset);
                {
                    std::string entries = "[";
                    for (size_t i = 0; i < o.hull.size(); ++i) {
                        const hull_entry& h = o.hull[i];
                        if (i) entries += ',';
                        if (h.kind == 0)
                            entries += "ov";
                        else
                            entries += e.coll(h.collision) + "." + std::to_string(static_cast<unsigned>(h.side));
                        if (h.kind == 0 && (h.collision != -1 || h.side != 0))
                            entries += "{" + std::to_string(h.collision) + "." + std::to_string(static_cast<unsigned>(h.side)) + "}";
                        if (h.kind > 1) entries += "{kind" + std::to_string(static_cast<unsigned>(h.kind)) + "}";
                        entries += "@" + hex(h.value);
                    }
                    e.put("entries", entries + "]");
                }

                e.rec("ov", o.name);
                e.put("has_ov", o.has_ov);
                e.put("in_hull", o.ov_in_hull);
                e.put("center", hex_list(o.ov_center));
                e.putf("radius", o.ov_radius);
                e.put("node", o.ov_node);
                e.put("collisions", e.colls(o.ov_collisions));

                e.rec("lists", o.name);
                e.put("exact", e.coll_sides(o.exact_synapses));
                e.put("invalid", e.coll_sides(o.invalid_synapses));
                e.put("friction", e.contact_sides(o.friction_synapses));
            }

            for (size_t i = 0; i < w.collisions.size(); ++i) {
                const collision_record& c = w.collisions[i];
                e.rec("collision", e.coll(static_cast<int32_t>(i)));
                e.put("kind", c.kind);
                e.put("parent", c.parent);
                e.put("objects", e.obj(c.objects[0]) + "|" + e.obj(c.objects[1]));
                e.put("synapse0", describe_edge(w, c.synapse[0]));
                e.put("synapse1", describe_edge(w, c.synapse[1]));
                e.put("coll_type", c.coll_type);
                e.put("sort_flag", c.sort_flag);
                e.put("phantom_set", c.phantom_set);
                e.put("function", c.function);
                e.put("recalc_result", c.recalc_result);
                e.put("disable_halfspace", c.disable_halfspace);
                e.put("status", c.status);
                e.put("selector", c.selector);
                e.put("counter", c.counter);
                e.put("recalc_fresh", c.recalc_fresh);
                e.putf("sum_extra_radius", c.sum_extra_radius);
                e.putf("len_numerator", c.len_numerator);
                e.putf("contact_dot_diff_center", c.contact_dot_diff_center);
                e.putd("sum_angular_hull_time", c.sum_angular_hull_time);
                e.put("contact_plane", hex_list(c.contact_plane));
                e.put("scheduled", c.scheduled);
                e.putf("event_value", c.event_value);
                e.put("recursive_status", c.recursive_status);
                e.put("spawned_count", c.spawned_count);
                e.put("children", e.indices(c.children));
            }

            e.rec("exact_list", "-");
            e.put("order", e.colls(w.exact_list));
            e.rec("invalid_list", "-");
            e.put("order", e.colls(w.invalid_list));

            for (size_t i = 0; i < w.events.size(); ++i) {
                const event_record& ev = w.events[i];
                e.rec("event", "#" + std::to_string(i));
                e.put("collision", ev.collision == -1 ? std::string("psi") : e.coll(ev.collision));
                e.putf("value", ev.value);
            }

            for (size_t i = 0; i < w.contacts.size(); ++i) {
                const contact_record& c = w.contacts[i];
                e.rec("contact", e.contact(static_cast<int32_t>(i)));
                e.put("synapse0", describe_edge(w, c.synapse[0]));
                e.put("synapse1", describe_edge(w, c.synapse[1]));
                e.put("system", c.system);
                e.putf("gap", c.gap);
                e.put("span", hex_list(c.span));
                e.putf("pressure", c.pressure);
                e.putf("friction_factor", c.friction_factor);
                e.putf("destroyed_energy", c.destroyed_energy);
                e.putf("inv_triangle_det", c.inv_triangle_det);
                e.putf("old_energy", c.old_energy);
                e.putf("inv_virt_mass", c.inv_virt_mass);
                e.put("pull", c.pull);
                e.put("keeper", c.keeper);
                e.put("status", c.status);
                e.put("two_friction", c.two_friction);
                e.putd("recalc_time", c.recalc_time);
                e.put("contact_ws", hex_list(c.contact_ws));
                e.put("normal_ws", hex_list(c.normal_ws));
            }

            for (size_t s = 0; s < w.systems.size(); ++s) {
                const friction_system_record& fs = w.systems[s];
                const std::string sname = "#" + std::to_string(s);
                e.rec("system", sname);
                e.put("cores", e.objects(fs.cores));
                e.put("movable_cores", e.objects(fs.movable_cores));
                e.put("dists", e.contacts(fs.dists));
                e.put("pairs", static_cast<int64_t>(fs.pairs.size()));
                e.put("infos", static_cast<int64_t>(fs.infos.size()));
                e.put("obj_number", fs.obj_number);
                e.put("dist_number", fs.dist_number);
                e.put("complex_not_necessary", fs.complex_not_necessary);
                e.put("union_find_necessary", fs.union_find_necessary);
                e.put("simulated", fs.simulated);
                for (size_t p = 0; p < fs.pairs.size(); ++p) {
                    const pair_record& pr = fs.pairs[p];
                    e.rec("pair", sname + "." + std::to_string(p) + "(" + e.obj(pr.objects[0]) + "|" + e.obj(pr.objects[1]) + ")");
                    e.put("objects", e.obj(pr.objects[0]) + "|" + e.obj(pr.objects[1]));
                    e.put("next_ease", pr.next_ease);
                    e.putd("last_impact_time", pr.last_impact_time);
                    e.putf("anti_energy", pr.anti_energy);
                    e.put("dists", e.contacts(pr.dists));
                }
                for (size_t k = 0; k < fs.infos.size(); ++k) {
                    const friction_info_record& info = fs.infos[k];
                    e.rec("info", sname + "." + std::to_string(k) + "(" + e.obj(info.object) + ")");
                    e.put("object", e.obj(info.object));
                    e.put("springs", e.contacts(info.springs));
                }
            }

            for (size_t i = 0; i < w.units.size(); ++i) {
                const unit_record& u = w.units[i];
                e.rec("unit", "#" + std::to_string(i));
                e.put("cores", e.objects(u.cores));
                e.put("movement_type", u.movement_type);
                e.put("union_find_needed", u.union_find_needed);
                e.put("has_fast_objects", u.has_fast_objects);
                e.put("just_slowed_down", u.just_slowed_down);
                e.put("next_movement_check", u.next_movement_check);
                e.put("in_still_slot", u.in_still_slot);
                e.put("controllers", emitter::controllers(u.controllers));
            }

            e.rec("revive", "-");
            e.put("cores", e.objects(w.revive_list));

            for (size_t i = 0; i < w.ov_nodes.size(); ++i) {
                const ov_node_record& n = w.ov_nodes[i];
                e.rec("ovnode", "#" + std::to_string(i));
                e.put("x", n.x);
                e.put("y", n.y);
                e.put("z", n.z);
                e.put("rasterlevel", n.rasterlevel);
                e.put("sizelevel", n.sizelevel);
                e.put("parent", n.parent);
                e.put("children", e.indices(n.children));
                e.put("elements", e.objects(n.elements));
            }

            for (size_t i = 0; i < w.cache_slots.size(); ++i) {
                const cache_slot_record& c = w.cache_slots[i];
                e.rec("cache", "#" + std::to_string(i));
                e.put("object", e.obj(c.object));
                e.put("fresh", c.fresh);
                e.put("q", hex_list(c.q));
                e.put("m", hex_list(c.m));
                e.put("core_pos", hex_list(c.core_pos));
            }
        }

        std::vector<field_entry> flatten(const world_record& world) {
            emitter e(world);
            emit_world(e);
            return std::move(e.items);
        }
    }

    std::string describe_world(const world_record& world) {
        const std::vector<field_entry> entries = flatten(world);
        std::string out;
        const field_entry* line = nullptr;
        for (const field_entry& f : entries) {
            if (!line || line->kind != f.kind || line->name != f.name) {
                if (line) out += '\n';
                out += f.kind + " " + f.name;
                line = &f;
            }
            out += " " + f.field + "=" + f.value;
        }
        if (line) out += '\n';
        return out;
    }

    namespace {
        // Everything that describes the overlap tree's layout.
        bool is_ov_field(const field_entry& f) {
            return f.kind == "ovnode" || (f.kind == "world" && f.field == "ov_nodes") || (f.kind == "clock" && f.field == "ov_root")
                || (f.kind == "ov" && f.field == "node");
        }
        // The other bookkeeping whose order two runs may keep differently
        // without any value differing: the object cache ring and its reuse
        // index, the simulation units' numbering (a core's unit index, the
        // unit records themselves).
        bool is_order_field(const field_entry& f) {
            return f.kind == "cache" || (f.kind == "clock" && f.field == "cache_reuse_index")
                || (f.kind == "world" && (f.field == "cache_slots" || f.field == "units"))
                || (f.kind == "core" && f.field == "unit") || f.kind == "unit";
        }
        std::vector<field_entry> flatten_without(const world_record& world, bool ov, bool order) {
            std::vector<field_entry> all = flatten(world), kept;
            kept.reserve(all.size());
            for (field_entry& f : all)
                if (!(ov && is_ov_field(f)) && !(order && is_order_field(f))) kept.push_back(std::move(f));
            return kept;
        }
    }

    std::string diff_world(const world_record& a, const world_record& b, bool ignore_ov, int max_differences, bool ignore_order) {
        const std::vector<field_entry> ea = flatten_without(a, ignore_ov, ignore_order);
        const std::vector<field_entry> eb = flatten_without(b, ignore_ov, ignore_order);
        const size_t n = std::min(ea.size(), eb.size());
        std::string out;
        int listed = 0;
        for (size_t i = 0; i < n; ++i) {
            const field_entry& x = ea[i];
            const field_entry& y = eb[i];
            if (x.kind == y.kind && x.name == y.name && x.field == y.field) {
                // The environment identity is a guard for the restore, not
                // state: blobs of two processes never share one, and a diff
                // between two clients' records is what this is for.
                if (x.kind == "clock" && x.field == "environment") continue;
                if (x.value != y.value) {
                    out += (out.empty() ? "" : " | ") + x.kind + " " + x.name + " " + x.field + " " + x.value + " " + y.value;
                    if (++listed >= std::max(1, max_differences)) return out;
                }
                continue;
            }
            if (!out.empty()) return out + " | <records no longer line up>";
            // The two worlds' records no longer line up (a name differs).
            return x.kind + " " + x.name + " " + x.field + " " + x.value + " " + y.kind + "/" + y.name + "/" + y.field + "=" + y.value;
        }
        if (ea.size() != eb.size()) {
            const field_entry& x = ea.size() > n ? ea[n] : eb[n];
            return (out.empty() ? "" : out + " | ") + x.kind + " " + x.name + " " + x.field + " "
                 + (ea.size() > n ? x.value + " <end>" : "<end> " + x.value);
        }
        return out;
    }
}

namespace bmmo::physics::savestate {
    namespace {
        std::map<const CKIpionManager*, controller_alias>& controller_aliases() {
            static std::map<const CKIpionManager*, controller_alias> aliases;
            return aliases;
        }
    }

    const controller_alias* controller_alias_for(const CKIpionManager* physics) {
        const auto& aliases = controller_aliases();
        if (aliases.empty()) return nullptr;
        const auto it = aliases.find(physics);
        return it == aliases.end() ? nullptr : &it->second;
    }
}

namespace bmmo::physics {

    void set_controller_alias(CKIpionManager* physics, std::function<bool(const char* name, std::string& alias)> alias) {
        if (alias) savestate::controller_aliases()[physics] = std::move(alias);
        else savestate::controller_aliases().erase(physics);
    }

    bool get_savestate(CKIpionManager* physics, std::vector<uint8_t>& out, std::string& error, savestate_report* report) {
        error.clear();
        if (report) *report = savestate_report{};
        savestate::world_record world;
        if (!savestate::capture_world(physics, world, error, report)) {
            if (error.empty()) error = "savestate capture failed";
            return false;
        }
        // Sized by the last blob of this thread: the next one is about as
        // large (and grows by doubling when not).
        thread_local size_t last_size = 0;
        savestate::writer w(last_size + last_size / 8);
        savestate::write_world(w, world);
        w.finish();
        last_size = w.size();
        out.swap(w.bytes);
        return true;
    }

    bool check_savestate(const uint8_t* data, size_t size, std::string& error) {
        error.clear();
        if (!data && size) {
            error = "no savestate data";
            return false;
        }
        static const uint8_t empty = 0;
        savestate::reader r(data ? data : &empty, data ? size : 0);
        savestate::world_record world;
        return savestate::read_world(r, world, error);
    }

    std::string describe_savestate(const uint8_t* data, size_t size) {
        static const uint8_t empty = 0;
        savestate::reader r(data ? data : &empty, data ? size : 0);
        savestate::world_record world;
        std::string error;
        if (!savestate::read_world(r, world, error)) return "invalid savestate: " + error + "\n";
        return savestate::describe_world(world);
    }

    std::string diff_savestate(const uint8_t* a, size_t a_size, const uint8_t* b, size_t b_size, bool ignore_ov,
                               int max_differences, bool ignore_order) {
        static const uint8_t empty = 0;
        if (!a) a_size = 0;
        if (!b) b_size = 0;
        if (a_size == b_size && (a_size == 0 || std::memcmp(a, b, a_size) == 0)) return "";
        savestate::world_record wa, wb;
        std::string error;
        savestate::reader ra(a ? a : &empty, a_size);
        if (!savestate::read_world(ra, wa, error)) return "savestate a unreadable: " + error;
        savestate::reader rb(b ? b : &empty, b_size);
        if (!savestate::read_world(rb, wb, error)) return "savestate b unreadable: " + error;
        std::string d = savestate::diff_world(wa, wb, ignore_ov, max_differences, ignore_order);
        if (d.empty())
            d = std::string(ignore_ov || ignore_order ? "no difference outside the ignored bookkeeping" : "bytes differ but every field renders the same")
              + " (sizes " + std::to_string(a_size) + " and " + std::to_string(b_size) + ")";
        return d;
    }

    uint64_t hash_savestate(const uint8_t* data, size_t size) {
        uint64_t h = 14695981039346656037ULL;
        if (!data) return h;
        for (size_t i = 0; i < size; ++i) {
            h ^= data[i];
            h *= 1099511628211ULL;
        }
        return h;
    }
}
