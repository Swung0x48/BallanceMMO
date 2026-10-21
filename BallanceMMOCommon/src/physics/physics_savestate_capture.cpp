// The IVP savestate's capture (docs/ivp-savestate-design.md, "What a tick
// boundary holds"): reads the environment behind a CKIpionManager into a
// world_record.  Read-only: nothing in the world is touched, no IVP function
// with a side effect is called, no floating-point value is computed - every
// value is copied as the engine holds it.
//
// Fails closed: an object the record cannot name (a collision partner that
// is not in the manager's physics table, an edge outside its surface, a time
// event or hull listener of an unknown kind, a phantom) fails the capture
// rather than producing a blob the restore would apply partially.

#include "CKAll.h"
#include "CKIpionManager.h"
#include "PhysicsCallback.h"

#include "physics_savestate_internal.hpp"

#include <physics/physics_state.hpp>

#include "ivp_controller.hxx"
#include "ivp_surface_manager.hxx"

#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

int ivp_srand_read();

namespace bmmo::physics::savestate {

    namespace {
        // A list longer than this is a cycle, not a world.
        constexpr int kListGuard = 1 << 22;
        constexpr int kDepthGuard = 256;

        struct capture_context {
            live_world live;
            world_record* world = nullptr;
            IVP_Environment* env = nullptr;
            IVP_Time_Manager* tm = nullptr;
            IVP_Time_CODE time_code = 0;
            std::map<const IVP_Collision*, int32_t> collisions;          // every checker
            std::map<const IVP_Time_Event*, int32_t> mindist_events;     // mindists only, as their time events
            std::map<const IVP_Contact_Point*, int32_t> contacts;
            std::vector<IVP_Friction_System*> systems;
            std::map<const IVP_Friction_System*, int32_t> system_index;
            std::map<const IVP_Simulation_Unit*, int32_t> units;
            std::map<const IVP_OV_Node*, int32_t> ov_nodes;
            std::string error;

            bool fail(const std::string& e) {
                if (error.empty()) error = e;
                return false;
            }
            std::string name_of(const IVP_Real_Object* o) const {
                const int32_t i = live.index_of(o);
                if (i >= 0) return live.objects[static_cast<size_t>(i)].name;
                return o && o->get_name() ? std::string("<unindexed ") + o->get_name() + ">" : std::string("<unindexed>");
            }
            int32_t collision_index(const IVP_Collision* c) const {
                auto it = collisions.find(c);
                return it == collisions.end() ? -1 : it->second;
            }
            int32_t contact_index(const IVP_Contact_Point* cp) const {
                auto it = contacts.find(cp);
                return it == contacts.end() ? -1 : it->second;
            }
        };

        template <class Point>
        void copy3(float (&out)[3], const Point& p) {
            for (int k = 0; k < 3; ++k) out[k] = static_cast<float>(p.k[k]);
        }
        template <class Point>
        void copy3(double (&out)[3], const Point& p) {
            for (int k = 0; k < 3; ++k) out[k] = static_cast<double>(p.k[k]);
        }
        template <class Quat>
        void copy4(double (&out)[4], const Quat& q) {
            out[0] = static_cast<double>(q.x);
            out[1] = static_cast<double>(q.y);
            out[2] = static_cast<double>(q.z);
            out[3] = static_cast<double>(q.w);
        }
        template <class Quat>
        void copy4(float (&out)[4], const Quat& q) {
            out[0] = static_cast<float>(q.x);
            out[1] = static_cast<float>(q.y);
            out[2] = static_cast<float>(q.z);
            out[3] = static_cast<float>(q.w);
        }
        // rows[0..2] (three each) then vv
        void copy_matrix(double (&out)[12], const IVP_U_Matrix& m) {
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 3; ++col) out[row * 3 + col] = static_cast<double>(m.get_elem(row, col));
            for (int k = 0; k < 3; ++k) out[9 + k] = static_cast<double>(m.vv.k[k]);
        }

        // ------------------------------------------------------------------
        // Collision checkers
        // ------------------------------------------------------------------
        bool fill_mindist(capture_context& c, IVP_Mindist* md, int32_t index, collision_record& rec) {
            for (int k = 0; k < 2; ++k) {
                IVP_Synapse_Real* syn = md->get_synapse(k);
                rec.synapse[k] = name_edge(c.live, syn->get_object(), syn->get_edge(), syn->get_status());
                if (rec.synapse[k].object < 0)
                    return c.fail("collision #" + std::to_string(index) + " synapse " + std::to_string(k) + " on "
                                  + c.name_of(syn->get_object()) + ": the edge cannot be named");
            }
            rec.coll_type = static_cast<uint8_t>(static_cast<int>(IVP_BMMO_State::coll_type(*md)));
            rec.sort_flag = static_cast<uint8_t>(IVP_BMMO_State::sort_flag(*md));
            rec.phantom_set = static_cast<uint8_t>(static_cast<int>(md->is_in_phantom_set));
            rec.function = static_cast<uint8_t>(static_cast<int>(md->mindist_function));
            rec.recalc_result = static_cast<uint8_t>(static_cast<int>(md->recalc_result));
#ifdef IVP_HALFSPACE_OPTIMIZATION_ENABLED
            rec.disable_halfspace = static_cast<uint8_t>(static_cast<int>(md->disable_halfspace_optimization));
#endif
            rec.status = static_cast<uint8_t>(static_cast<int>(md->mindist_status));
            rec.selector = static_cast<uint8_t>(md->coll_dist_selector);
            rec.counter = static_cast<uint8_t>(md->coll_dist_decrease_counter);
            rec.recalc_fresh = IVP_BMMO_State::recalc_time_stamp(*md) == c.time_code ? 1 : 0;
            rec.sum_extra_radius = md->sum_extra_radius;
            rec.len_numerator = md->len_numerator;
#ifdef IVP_HALFSPACE_OPTIMIZATION_ENABLED
            rec.contact_dot_diff_center = md->contact_dot_diff_center;
            rec.sum_angular_hull_time = static_cast<double>(md->sum_angular_hull_time);
#endif
            copy3(rec.contact_plane, md->contact_plane);
            if (md->index != IVP_U_MINLIST_UNUSED) {
                IVP_U_Min_List& queue = *c.tm->min_hash;
                if (md->index < 0 || md->index >= static_cast<int>(IVP_BMMO_State::malloced_size(queue)))
                    return c.fail("collision #" + std::to_string(index) + " has event index " + std::to_string(md->index)
                                  + " outside the time manager's list");
                const IVP_U_Min_List_Element& element = IVP_BMMO_State::elements(queue)[md->index];
                if (element.element != static_cast<void*>(static_cast<IVP_Time_Event*>(md)))
                    return c.fail("collision #" + std::to_string(index) + " event index points at another event");
                rec.scheduled = 1;
                rec.event_value = element.value;
            }
            return true;
        }

        // Index `coll` (and everything under it, depth-first, children in
        // vector order) at first encounter.  A root reached a second time
        // (from its other object) returns its index.
        bool add_collision(capture_context& c, IVP_Collision* coll, int32_t parent, int depth, int32_t& index_out) {
            index_out = -1;
            if (!coll) return c.fail("null collision checker");
            if (depth > kDepthGuard) return c.fail("collision tree too deep");
            const int32_t known = c.collision_index(coll);
            if (known >= 0) {
                if (parent != -1) return c.fail("collision #" + std::to_string(known) + " reached twice in the checker trees");
                index_out = known;
                return true;
            }
            const int32_t index = static_cast<int32_t>(c.world->collisions.size());
            c.collisions.emplace(coll, index);
            c.world->collisions.emplace_back();

            collision_record rec;
            rec.parent = parent;
            IVP_Real_Object* objects[2] = {nullptr, nullptr};
            coll->get_objects(objects);   // public virtual on IVP_Collision
            for (int k = 0; k < 2; ++k) {
                rec.objects[k] = c.live.index_of(objects[k]);
                if (rec.objects[k] < 0)
                    return c.fail("collision #" + std::to_string(index) + " involves " + c.name_of(objects[k])
                                  + ", which is not in the physics table");
            }
            IVP_U_FVector<IVP_Collision>* children = nullptr;
            IVP_Collision_Delegator* as_delegator = nullptr;
            if (auto* watcher = dynamic_cast<IVP_OO_Watcher*>(coll)) {
                rec.kind = 2;
                children = &IVP_BMMO_State::watcher_children(*watcher);
                as_delegator = static_cast<IVP_Collision_Delegator*>(watcher);
            } else if (auto* md = dynamic_cast<IVP_Mindist*>(coll)) {
                if (md->is_recursive()) {
                    auto* recursive = static_cast<IVP_Mindist_Recursive*>(md);
                    rec.kind = 1;
                    rec.recursive_status = static_cast<int8_t>(static_cast<int>(recursive->recursive_status));
                    rec.spawned_count = static_cast<int32_t>(recursive->spawned_mindist_count);
                    children = &recursive->mindists;
                    as_delegator = static_cast<IVP_Collision_Delegator*>(recursive);
                } else {
                    rec.kind = 0;
                }
                if (!fill_mindist(c, md, index, rec)) return false;
                c.mindist_events.emplace(static_cast<IVP_Time_Event*>(md), index);
            } else {
                return c.fail("collision #" + std::to_string(index) + " between " + c.name_of(objects[0]) + " and "
                              + c.name_of(objects[1]) + " is of an unknown kind");
            }
            c.world->collisions[static_cast<size_t>(index)] = rec;
            if (children) {
                for (int i = 0; i < children->len(); ++i) {
                    IVP_Collision* child = children->element_at(i);
                    if (!child) return c.fail("collision #" + std::to_string(index) + " has a null child");
                    if (child->delegator != as_delegator)
                        return c.fail("collision #" + std::to_string(index) + " has a child delegated elsewhere");
                    int32_t child_index = -1;
                    if (!add_collision(c, child, index, depth + 1, child_index)) return false;
                    c.world->collisions[static_cast<size_t>(index)].children.push_back(child_index);
                }
            }
            index_out = index;
            return true;
        }

        // (collision << 1) | side of a hull/exact/invalid synapse.
        bool encode_synapse(capture_context& c, IVP_Synapse_Real* syn, const std::string& where, int32_t& out) {
            IVP_Mindist* md = syn->get_mindist();
            const int32_t index = c.collision_index(md);
            if (index < 0) return c.fail(where + ": a synapse of a mindist outside the checker trees");
            int side = -1;
            if (syn == md->get_synapse(0)) side = 0;
            else if (syn == md->get_synapse(1)) side = 1;
            if (side < 0) return c.fail(where + ": a synapse that is neither side of its mindist");
            out = (index << 1) | side;
            return true;
        }

        // ------------------------------------------------------------------
        // Controllers
        // ------------------------------------------------------------------
        bool describe_controller(capture_context& c, IVP_Controller* ctrl, controller_ref& out) {
            out = controller_ref{};
            if (!ctrl) return c.fail("null controller");
            for (size_t s = 0; s < c.systems.size(); ++s) {
                IVP_Friction_System* fs = c.systems[s];
                int part = -1;
                if (ctrl == static_cast<IVP_Controller*>(&IVP_BMMO_State::fs_static_handle(*fs))) part = 0;
                else if (ctrl == static_cast<IVP_Controller*>(fs)) part = 1;
                else if (ctrl == static_cast<IVP_Controller*>(&IVP_BMMO_State::fs_energy_handle(*fs))) part = 2;
                if (part >= 0) {
                    out.kind = 0;
                    out.system = static_cast<int32_t>(s);
                    out.part = static_cast<uint8_t>(part);
                    return true;
                }
            }
            out.kind = 1;
            out.priority = static_cast<int32_t>(ctrl->get_controller_priority());
            const char* name = ctrl->get_controller_name();
            out.name = name ? name : "";
            return true;
        }

        // ------------------------------------------------------------------
        // Friction
        // ------------------------------------------------------------------
        bool fill_contact(capture_context& c, IVP_Contact_Point* cp, int32_t system, int32_t index, contact_record& rec) {
            for (int k = 0; k < 2; ++k) {
                IVP_Synapse_Friction* syn = cp->get_synapse(k);
                rec.synapse[k] = name_edge(c.live, syn->get_object(), syn->edge, syn->get_status());
                if (rec.synapse[k].object < 0)
                    return c.fail("contact #" + std::to_string(index) + " synapse " + std::to_string(k) + " on "
                                  + c.name_of(syn->get_object()) + ": the edge cannot be named");
            }
            rec.system = system;
            rec.gap = IVP_BMMO_State::cp_gap(*cp);
            IVP_FLOAT* span = IVP_BMMO_State::cp_span(*cp);
            rec.span[0] = span[0];
            rec.span[1] = span[1];
            rec.pressure = IVP_BMMO_State::cp_pressure(*cp);
            rec.friction_factor = IVP_BMMO_State::cp_friction_factor(*cp);
            rec.destroyed_energy = IVP_BMMO_State::cp_destroyed_energy(*cp);
            // Written by the constructor only when synapse 1 is a triangle
            // (ivp_friction.cxx, IVP_Contact_Point::IVP_Contact_Point) and read
            // only by the point-face case (p_calc_friction_qr_PF): for every
            // other contact it is heap memory, so it is normalized to 0.
            rec.inv_triangle_det = cp->get_synapse(1)->get_status() == IVP_ST_TRIANGLE
                                 ? IVP_BMMO_State::cp_inv_triangle_det(*cp) : 0.0f;
            rec.old_energy = IVP_BMMO_State::cp_old_energy(*cp);
            rec.inv_virt_mass = IVP_BMMO_State::cp_inv_virt_mass(*cp);
            rec.pull = static_cast<int32_t>(IVP_BMMO_State::cp_pull(*cp));
            rec.keeper = static_cast<int16_t>(IVP_BMMO_State::cp_keeper(*cp));
            rec.status = static_cast<uint8_t>(static_cast<int>(IVP_BMMO_State::cp_status(*cp)));
            rec.two_friction = IVP_BMMO_State::cp_two_friction(*cp) ? 1 : 0;
            rec.recalc_time = IVP_BMMO_State::cp_recalc_time(*cp).get_seconds();
            copy3(rec.contact_ws, IVP_BMMO_State::cp_contact_ws(*cp));
            copy3(rec.normal_ws, IVP_BMMO_State::cp_normal_ws(*cp));
            return true;
        }

        bool capture_system(capture_context& c, int32_t s) {
            IVP_Friction_System* fs = c.systems[static_cast<size_t>(s)];
            friction_system_record rec;
            const std::string at = "friction system #" + std::to_string(s);

            for (int i = 0; i < fs->cores_of_friction_system.len(); ++i) {
                const int32_t o = c.live.index_of_core(fs->cores_of_friction_system.element_at(i));
                if (o < 0) return c.fail(at + " holds a core outside the physics table");
                rec.cores.push_back(o);
            }
            for (int i = 0; i < fs->moveable_cores_of_friction_system.len(); ++i) {
                const int32_t o = c.live.index_of_core(fs->moveable_cores_of_friction_system.element_at(i));
                if (o < 0) return c.fail(at + " holds a movable core outside the physics table");
                rec.movable_cores.push_back(o);
            }
            int guard = 0;
            for (IVP_Contact_Point* cp = fs->get_first_friction_dist(); cp; cp = fs->get_next_friction_dist(cp)) {
                if (++guard > kListGuard) return c.fail(at + ": the contact list does not end");
                if (c.contact_index(cp) >= 0) return c.fail(at + " lists a contact point twice or shares it");
                if (IVP_BMMO_State::cp_system(*cp) != fs) return c.fail(at + " lists a contact point of another system");
                const int32_t ci = static_cast<int32_t>(c.world->contacts.size());
                c.contacts.emplace(cp, ci);
                contact_record contact;
                if (!fill_contact(c, cp, s, ci, contact)) return false;
                c.world->contacts.push_back(contact);
                rec.dists.push_back(ci);
            }
            for (int i = 0; i < fs->fr_pairs_of_objs.len(); ++i) {
                IVP_Friction_Core_Pair* pair = fs->fr_pairs_of_objs.element_at(i);
                if (!pair) return c.fail(at + " has a null pair");
                pair_record pr;
                for (int k = 0; k < 2; ++k) {
                    pr.objects[k] = c.live.index_of_core(pair->objs[k]);
                    if (pr.objects[k] < 0) return c.fail(at + " has a pair with a core outside the physics table");
                }
                pr.next_ease = static_cast<int32_t>(pair->next_ease_nr_psi);
                pr.last_impact_time = pair->last_impact_time_pair.get_seconds();
                pr.anti_energy = pair->integrated_anti_energy;
                IVP_U_Vector<IVP_Contact_Point>& dists = IVP_BMMO_State::pair_dists(*pair);
                for (int k = 0; k < dists.len(); ++k) {
                    const int32_t ci = c.contact_index(dists.element_at(k));
                    if (ci < 0) return c.fail(at + " has a pair holding a contact point the system does not list");
                    pr.dists.push_back(ci);
                }
                rec.pairs.push_back(pr);
            }
            for (int i = 0; i < fs->cores_of_friction_system.len(); ++i) {
                IVP_Core* core = fs->cores_of_friction_system.element_at(i);
                IVP_Friction_Info_For_Core* info = core->get_friction_info(fs);
                if (!info) return c.fail(at + ": core of " + c.name_of(core->objects.element_at(0)) + " has no friction info");
                friction_info_record ir;
                ir.object = c.live.index_of_core(core);
                for (int k = 0; k < info->friction_springs.len(); ++k) {
                    const int32_t ci = c.contact_index(info->friction_springs.element_at(k));
                    if (ci < 0) return c.fail(at + ": a friction info holds a contact point the system does not list");
                    ir.springs.push_back(ci);
                }
                rec.infos.push_back(ir);
            }
            rec.obj_number = IVP_BMMO_State::fs_obj_number(*fs);
            rec.dist_number = IVP_BMMO_State::fs_dist_number(*fs);
            // IVP_Friction_System's constructor never initializes it, and
            // reorder_mindists_for_complex writes it (always 0: its counter
            // is commented out) before every read inside do_friction_system.
            // A fresh system carries heap memory there, so it is normalized.
            rec.complex_not_necessary = 0;
            (void)IVP_BMMO_State::fs_complex_not_necessary(*fs);
            rec.union_find_necessary = fs->union_find_necessary ? 1 : 0;
            // Declared, never initialized, never read anywhere in IVP: heap
            // memory on a fresh system, so it is normalized to 0.
            rec.simulated = 0;
            c.world->systems.push_back(rec);
            return true;
        }

        // ------------------------------------------------------------------
        // Simulation units
        // ------------------------------------------------------------------
        bool add_unit(capture_context& c, IVP_Simulation_Unit* unit, bool still) {
            if (c.units.count(unit)) return c.fail("a simulation unit is linked twice");
            const int32_t index = static_cast<int32_t>(c.world->units.size());
            c.units.emplace(unit, index);
            const std::string at = "unit #" + std::to_string(index);
            unit_record rec;
            for (int i = 0; i < unit->sim_unit_cores.len(); ++i) {
                const int32_t o = c.live.index_of_core(unit->sim_unit_cores.element_at(i));
                if (o < 0) return c.fail(at + " holds a core outside the physics table");
                rec.cores.push_back(o);
            }
            rec.movement_type = static_cast<uint8_t>(static_cast<int>(unit->get_unit_movement_type()));
            rec.union_find_needed = unit->union_find_needed_for_sim_unit ? 1 : 0;
            rec.has_fast_objects = unit->sim_unit_has_fast_objects ? 1 : 0;
            rec.just_slowed_down = unit->sim_unit_just_slowed_down ? 1 : 0;
#ifdef IVP_PORTABLE_BMMO
            rec.next_movement_check = static_cast<int16_t>(unit->next_movement_check);
#endif
            rec.in_still_slot = still ? 1 : 0;
            for (int i = 0; i < unit->controller_cores.len(); ++i) {
                IVP_Sim_Unit_Controller_Core_List* entry = unit->controller_cores.element_at(i);
                if (!entry) return c.fail(at + " has a null controller entry");
                controller_ref ref;
                if (!describe_controller(c, entry->l_controller, ref)) return false;
                rec.controllers.push_back(ref);
            }
            c.world->units.push_back(rec);
            return true;
        }

        bool capture_units(capture_context& c) {
            IVP_Sim_Units_Manager* manager = IVP_BMMO_State::units_manager(*c.env);
            if (!manager) return c.fail("no simulation units manager");
            for (int slot = 1; slot < IVP_SIM_SLOTS_NUM; ++slot)
                if (manager->sim_units_slots[slot]) return c.fail("simulation unit slot " + std::to_string(slot) + " is in use");
            int guard = 0;
            for (IVP_Simulation_Unit* u = manager->sim_units_slots[0]; u; u = u->next_sim_unit) {
                if (++guard > kListGuard) return c.fail("the simulated unit list does not end");
                if (!add_unit(c, u, false)) return false;
            }
            for (IVP_Simulation_Unit* u = manager->still_slot; u; u = u->next_sim_unit) {
                if (++guard > kListGuard) return c.fail("the still unit list does not end");
                if (!add_unit(c, u, true)) return false;
            }
            return true;
        }

        // ------------------------------------------------------------------
        // Per object
        // ------------------------------------------------------------------
        bool capture_core(capture_context& c, const live_object& lo, core_record& rec) {
            IVP_Core* core = lo.core;
            rec.movement_state = static_cast<uint8_t>(static_cast<int>(core->movement_state));
            if (core->sim_unit_of_core) {
                auto it = c.units.find(core->sim_unit_of_core);
                if (it == c.units.end()) return c.fail("the core of " + lo.name + " is in a unit the manager does not list");
                rec.unit = it->second;
            }
            for (int i = 0; i < core->controllers_of_core.len(); ++i) {
                controller_ref ref;
                if (!describe_controller(c, core->controllers_of_core.element_at(i), ref)) return false;
                rec.controllers.push_back(ref);
            }
            if (core->physical_unmoveable) return true;   // a static core: movement_state, unit, controllers only

            rec.temporarily_unmovable = core->temporarily_unmovable ? 1 : 0;
            rec.pinned = core->pinned ? 1 : 0;
            rec.fast_piling = core->fast_piling_allowed() ? 1 : 0;
            rec.in_revive_list = core->is_in_wakeup_vec ? 1 : 0;
            rec.impacts_since_last_psi = static_cast<int16_t>(core->impacts_since_last_PSI);
            rec.time_of_last_psi = core->time_of_last_psi.get_seconds();
            rec.i_delta_time = core->i_delta_time;
            copy3(rec.rot_speed_change, core->rot_speed_change);
            copy3(rec.speed_change, core->speed_change);
            copy3(rec.rot_speed, core->rot_speed);
            copy3(rec.speed, core->speed);
            copy3(rec.pos_last_psi, core->pos_world_f_core_last_psi);
            copy3(rec.delta_psis, core->delta_world_f_core_psis);
            copy4(rec.q_last, core->q_world_f_core_last_psi);
            copy4(rec.q_next, core->q_world_f_core_next_psi);
            copy_matrix(rec.m_last_psi, core->m_world_f_core_last_psi);
            copy3(rec.rotation_axis_ws, core->rotation_axis_world_space);
            rec.current_speed = core->current_speed;
            rec.abs_omega = core->abs_omega;
            rec.max_surface_rot_speed = core->max_surface_rot_speed;
            for (int k = 0; k < 2; ++k) {
                rec.calm_time[k] = core->time_of_calm_reference[k].get_seconds();
                copy4(rec.calm_quat[k], core->q_world_f_core_calm_reference[k]);
                copy3(rec.calm_position[k], core->position_world_f_core_calm_reference[k]);
            }
            return true;
        }

        bool capture_hull(capture_context& c, int32_t index, object_record& rec) {
            const live_object& lo = c.live.objects[static_cast<size_t>(index)];
            IVP_Real_Object* object = lo.object;
            IVP_Hull_Manager* hm = object->get_hull_manager();
            if (!hm) return c.fail(lo.name + " has no hull manager");
            const IVP_Hull_Manager_Base_Gradient& g = IVP_BMMO_State::hull_gradient(*hm);
            rec.last_vpsi_time = g.last_vpsi_time.get_seconds();
            rec.gradient = g.gradient;
            rec.center_gradient = g.center_gradient;
            rec.hull_value_last_vpsi = g.hull_value_last_vpsi;
            rec.hull_center_value_last_vpsi = g.hull_center_value_last_vpsi;
            rec.hull_value_next_psi = g.hull_value_next_psi;
            rec.time_of_next_reset = static_cast<int32_t>(g.time_of_next_reset);

            const std::string at = "hull of " + lo.name;
            IVP_U_Min_List& list = IVP_BMMO_State::hull_list(*hm);
            IVP_U_Min_List_Enumerator enumerator(&list);
            int guard = 0;
            while (IVP_U_Min_List_Element* header = enumerator.get_next_element_header()) {
                if (++guard > kListGuard) return c.fail(at + ": the list does not end");
                auto* listener = static_cast<IVP_Listener_Hull*>(header->element);
                if (!listener) return c.fail(at + ": a null listener");
                hull_entry entry;
                entry.value = header->value;
                switch (listener->get_type()) {
                case IVP_HULL_ELEM_OO_CONNECTOR: {
                    auto* elem = static_cast<IVP_OV_Element*>(listener);
                    if (elem != object->get_ov_element()) return c.fail(at + " holds another object's overlap element");
                    entry.kind = 0;
                    break;
                }
                case IVP_HULL_ELEM_POLYGON: {
                    auto* syn = static_cast<IVP_Synapse_Real*>(listener);
                    int32_t encoded = -1;
                    if (!encode_synapse(c, syn, at, encoded)) return false;
                    entry.kind = 1;
                    entry.collision = encoded >> 1;
                    entry.side = static_cast<uint8_t>(encoded & 1);
                    break;
                }
                case IVP_HULL_ELEM_OO_WATCHER: {
                    auto* syn = static_cast<IVP_Synapse_OO*>(listener);
                    IVP_OO_Watcher* watcher = IVP_BMMO_State::watcher_of(*syn);
                    const int32_t ci = watcher ? c.collision_index(watcher) : -1;
                    if (ci < 0) return c.fail(at + ": a watcher synapse of a watcher outside the checker trees");
                    int side = -1;
                    if (syn == &IVP_BMMO_State::watcher_synapse(*watcher, 0)) side = 0;
                    else if (syn == &IVP_BMMO_State::watcher_synapse(*watcher, 1)) side = 1;
                    if (side < 0) return c.fail(at + ": a watcher synapse that is neither side of its watcher");
                    entry.kind = 1;
                    entry.collision = ci;
                    entry.side = static_cast<uint8_t>(side);
                    break;
                }
                default:
                    return c.fail(at + ": a hull listener of kind " + std::to_string(static_cast<int>(listener->get_type()))
                                  + " (anchor or unknown)");
                }
                rec.hull.push_back(entry);
            }
            return true;
        }

        bool capture_object_lists(capture_context& c, int32_t index, object_record& rec) {
            const live_object& lo = c.live.objects[static_cast<size_t>(index)];
            IVP_Real_Object* object = lo.object;
            int guard = 0;
            for (IVP_Synapse_Real* syn = object->get_first_exact_synapse(); syn; syn = syn->get_next()) {
                if (++guard > kListGuard) return c.fail("exact synapses of " + lo.name + ": the list does not end");
                int32_t encoded = -1;
                if (!encode_synapse(c, syn, "exact synapses of " + lo.name, encoded)) return false;
                rec.exact_synapses.push_back(encoded);
            }
            guard = 0;
            for (IVP_Synapse_Real* syn = IVP_BMMO_State::invalid_synapses(*object); syn; syn = syn->get_next()) {
                if (++guard > kListGuard) return c.fail("invalid synapses of " + lo.name + ": the list does not end");
                int32_t encoded = -1;
                if (!encode_synapse(c, syn, "invalid synapses of " + lo.name, encoded)) return false;
                rec.invalid_synapses.push_back(encoded);
            }
            guard = 0;
            for (IVP_Synapse_Friction* syn = object->get_first_friction_synapse(); syn; syn = syn->get_next()) {
                if (++guard > kListGuard) return c.fail("friction synapses of " + lo.name + ": the list does not end");
                IVP_Contact_Point* cp = syn->get_contact_point();
                const int32_t ci = c.contact_index(cp);
                if (ci < 0) return c.fail("friction synapses of " + lo.name + ": a contact point no friction system lists");
                int side = -1;
                if (syn == cp->get_synapse(0)) side = 0;
                else if (syn == cp->get_synapse(1)) side = 1;
                if (side < 0) return c.fail("friction synapses of " + lo.name + ": a synapse that is neither side of its contact");
                rec.friction_synapses.push_back((ci << 1) | side);
            }
            return true;
        }

        // ------------------------------------------------------------------
        // The overlap tree
        // ------------------------------------------------------------------
        bool add_ov_node(capture_context& c, IVP_OV_Node* node, int32_t parent, int depth, int32_t& index_out) {
            index_out = -1;
            if (!node) return c.fail("null overlap node");
            if (depth > kDepthGuard) return c.fail("overlap tree too deep");
            if (c.ov_nodes.count(node)) return c.fail("an overlap node is reached twice");
            const int32_t index = static_cast<int32_t>(c.world->ov_nodes.size());
            c.ov_nodes.emplace(node, index);
            ov_node_record rec;
            rec.x = node->data.x;
            rec.y = node->data.y;
            rec.z = node->data.z;
            rec.rasterlevel = node->data.rasterlevel;
            rec.sizelevel = node->data.sizelevel;
            rec.parent = parent;
            for (int i = 0; i < node->elements.len(); ++i) {
                IVP_OV_Element* elem = node->elements.element_at(i);
                const int32_t o = elem ? c.live.index_of(elem->real_object) : -1;
                if (o < 0) return c.fail("overlap node #" + std::to_string(index) + " holds an element of an object outside the physics table");
                rec.elements.push_back(o);
            }
            c.world->ov_nodes.push_back(rec);
            for (int i = 0; i < node->children.len(); ++i) {
                int32_t child = -1;
                if (!add_ov_node(c, node->children.element_at(i), index, depth + 1, child)) return false;
                c.world->ov_nodes[static_cast<size_t>(index)].children.push_back(child);
            }
            index_out = index;
            return true;
        }
    }

    bool capture_world(CKIpionManager* physics, world_record& out, std::string& error, savestate_report* report) {
        error.clear();
        out = world_record{};
        capture_context c;
        c.world = &out;
        if (!index_live_world(physics, c.live, error)) {
            out = world_record{};
            return false;
        }
        IVP_Environment* env = c.live.env;
        c.env = env;
        c.tm = env->get_time_manager();
        if (!c.tm || !c.tm->min_hash) {
            error = "the environment has no time manager";
            out = world_record{};
            return false;
        }
        c.time_code = env->get_current_time_code();
        const size_t n = c.live.objects.size();

        auto failed = [&]() {
            error = c.error.empty() ? std::string("savestate capture failed") : c.error;
            out = world_record{};
            return false;
        };

        // Objects (identity), in name order.
        out.objects.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const live_object& lo = c.live.objects[i];
            object_record& rec = out.objects[i];
            rec.name = lo.name;
            rec.type = static_cast<uint8_t>(static_cast<int>(lo.object->get_type()));
            rec.movable = lo.core->physical_unmoveable ? 0 : 1;
            rec.movement_state = static_cast<uint8_t>(static_cast<int>(lo.object->get_movement_state()));
            rec.surface_size = lo.surface_size;
            if (lo.entity) {
                rec.entity_id = static_cast<uint32_t>(lo.entity->GetID());
                const VxMatrix& matrix = lo.entity->GetWorldMatrix();
                static_assert(sizeof(VxMatrix) == sizeof(rec.entity_matrix), "VxMatrix is 16 floats");
                std::memcpy(rec.entity_matrix, &matrix, sizeof(rec.entity_matrix));
                rec.has_entity_matrix = 1;
            }
        }

        // Collision checkers: every object's roots in collision_fvector
        // order, each indexed once, children depth-first after their parent.
        for (size_t i = 0; i < n; ++i) {
            IVP_Real_Object* object = c.live.objects[i].object;
            IVP_OV_Element* elem = object->get_ov_element();
            if (!elem) continue;
            if (elem->real_object != object) {
                c.fail(c.live.objects[i].name + "'s overlap element belongs to another object");
                return failed();
            }
            for (int k = 0; k < elem->collision_fvector.len(); ++k) {
                int32_t index = -1;
                if (!add_collision(c, elem->collision_fvector.element_at(k), -1, 0, index)) return failed();
                out.objects[i].ov_collisions.push_back(index);
            }
        }

        // Friction systems, found through the movable cores in object order.
        for (size_t i = 0; i < n; ++i) {
            IVP_Core* core = c.live.objects[i].core;
            if (core->physical_unmoveable) continue;
            IVP_Friction_Info_For_Core* info = core->moveable_core_has_friction_info();
            IVP_Friction_System* fs = info ? info->l_friction_system : nullptr;
            if (!fs || c.system_index.count(fs)) continue;
            c.system_index.emplace(fs, static_cast<int32_t>(c.systems.size()));
            c.systems.push_back(fs);
        }
        for (size_t s = 0; s < c.systems.size(); ++s)
            if (!capture_system(c, static_cast<int32_t>(s))) return failed();

        // Simulation units (they name controllers, so after the systems).
        if (!capture_units(c)) return failed();

        // Per object: core, hull manager, overlap element, intrusive lists.
        size_t hull_entries = 0;
        std::set<const IVP_Core*> movable_cores;
        for (size_t i = 0; i < n; ++i) {
            const live_object& lo = c.live.objects[i];
            object_record& rec = out.objects[i];
            if (!capture_core(c, lo, rec.core)) return failed();
            if (!lo.core->physical_unmoveable) movable_cores.insert(lo.core);
            if (!capture_hull(c, static_cast<int32_t>(i), rec)) return failed();
            hull_entries += rec.hull.size();
            if (IVP_OV_Element* elem = lo.object->get_ov_element()) {
                rec.has_ov = 1;
                rec.ov_in_hull = elem->hull_manager ? 1 : 0;
                copy3(rec.ov_center, elem->center);
                rec.ov_radius = elem->radius;
            }
            if (!capture_object_lists(c, static_cast<int32_t>(i), rec)) return failed();
        }

        // The mindist manager's lists.
        IVP_Mindist_Manager* mm = env->get_mindist_manager();
        if (!mm) {
            c.fail("the environment has no mindist manager");
            return failed();
        }
        {
            int guard = 0;
            for (IVP_Mindist* md = mm->exact_mindists; md; md = md->next) {
                if (++guard > kListGuard) { c.fail("the exact mindist list does not end"); return failed(); }
                const int32_t index = c.collision_index(md);
                if (index < 0) { c.fail("an exact mindist outside the checker trees"); return failed(); }
                out.exact_list.push_back(index);
            }
            guard = 0;
            for (IVP_Mindist* md = mm->invalid_mindists; md; md = md->next) {
                if (++guard > kListGuard) { c.fail("the invalid mindist list does not end"); return failed(); }
                const int32_t index = c.collision_index(md);
                if (index < 0) { c.fail("an invalid mindist outside the checker trees"); return failed(); }
                out.invalid_list.push_back(index);
            }
        }

        // The event queue, in sorted order.
        {
            IVP_U_Min_List_Enumerator enumerator(c.tm->min_hash);
            int guard = 0;
            while (IVP_U_Min_List_Element* header = enumerator.get_next_element_header()) {
                if (++guard > kListGuard) { c.fail("the event queue does not end"); return failed(); }
                auto* event = static_cast<IVP_Time_Event*>(header->element);
                event_record ev;
                ev.value = header->value;
                if (event && event == static_cast<IVP_Time_Event*>(c.tm->psi_event)) {
                    ev.collision = -1;
                } else {
                    auto it = c.mindist_events.find(event);
                    if (it == c.mindist_events.end()) { c.fail("unknown time event"); return failed(); }
                    ev.collision = it->second;
                }
                out.events.push_back(ev);
            }
        }

        // The overlap tree, depth-first from the root, children in order.
        {
            IVP_OV_Tree_Manager* tree = IVP_BMMO_State::ov_tree(*env);
            if (!tree) { c.fail("the environment has no overlap tree"); return failed(); }
            if (IVP_OV_Node* root = IVP_BMMO_State::ov_root(*tree)) {
                if (!add_ov_node(c, root, -1, 0, out.ov_root)) return failed();
            }
            for (size_t i = 0; i < n; ++i) {
                IVP_OV_Element* elem = c.live.objects[i].object->get_ov_element();
                if (!elem || !elem->node) continue;
                auto it = c.ov_nodes.find(elem->node);
                if (it == c.ov_nodes.end()) {
                    c.fail(c.live.objects[i].name + "'s overlap element sits in a node outside the tree");
                    return failed();
                }
                out.objects[i].ov_node = it->second;
            }
        }

        // The revive list.
        {
            IVP_U_Vector<IVP_Core>& revive = IVP_BMMO_State::revive_list(*env);
            for (int i = 0; i < revive.len(); ++i) {
                const int32_t o = c.live.index_of_core(revive.element_at(i));
                if (o < 0) { c.fail("the revive list holds a core outside the physics table"); return failed(); }
                out.revive_list.push_back(o);
            }
        }

        // The object cache ring.
        {
            IVP_Cache_Object_Manager* cache = IVP_BMMO_State::cache_manager(*env);
            if (!cache) { c.fail("the environment has no cache manager"); return failed(); }
            const int count = IVP_BMMO_State::cache_count(*cache);
            for (int i = 0; i < count; ++i) {
                IVP_Cache_Object* slot = cache->cache_object_at(i);
                cache_slot_record rec;
                rec.object = c.live.index_of(IVP_BMMO_State::cache_owner(*slot));
                rec.fresh = IVP_BMMO_State::cache_valid_until(*slot) == c.time_code ? 1 : 0;
                copy4(rec.q, slot->q_world_f_object);
                copy_matrix(rec.m, slot->m_world_f_object);
                copy3(rec.core_pos, slot->core_pos);
                out.cache_slots.push_back(rec);
            }
            out.cache_reuse_index = static_cast<int32_t>(IVP_BMMO_State::cache_reuse_index(*cache));
        }

        // Clock, environment and the physics_RT side.
        out.environment_id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(env));
        out.current_time = env->get_current_time().get_seconds();
        out.time_of_last_psi = env->get_old_time_of_last_PSI().get_seconds();
        out.time_of_next_psi = env->get_next_PSI_time().get_seconds();
        out.base_time = c.tm->base_time.get_seconds();
        // IVP_DOUBLE in the header, but only ever assigned an event's float
        // value (ivp_time.cxx) or zero: the float holds it exactly.
        out.last_time = static_cast<float>(c.tm->last_time);
        out.next_movement_check = static_cast<int32_t>(IVP_BMMO_State::next_movement_check(*env));
        out.env_state = static_cast<uint8_t>(static_cast<int>(IVP_BMMO_State::env_state(*env)));
        if (const IVP_U_Point* gravity = env->get_gravity()) copy3(out.gravity, *gravity);
        out.delta_time_ms = physics->m_DeltaTime;
        out.physics_delta_time = physics->m_PhysicsDeltaTime;
        out.time_factor = physics->m_PhysicsTimeFactor;
        out.ivp_seed = static_cast<int32_t>(ivp_srand_read());
        out.random_state = bmmo::physics::random_get_state();
        for (int i = 0; i < physics->m_MovableObjects.len(); ++i) {
            const int32_t o = c.live.index_of(physics->m_MovableObjects.element_at(i));
            if (o < 0) { c.fail("the movable list holds an object outside the physics table"); return failed(); }
            out.movable_objects.push_back(o);
        }
        if (PhysicsCallbackContainer* queue = physics->m_PreSimulateCallbacks) {
            for (int slot = 0; slot < 3; ++slot) {
                for (int i = 0; i < queue->m_Callbacks[slot].len(); ++i) {
                    PhysicsCallback* callback = queue->m_Callbacks[slot].element_at(i);
                    // The bridge's own callbacks are not retail state: the
                    // key-polling one is queued live and absent for the whole
                    // of a replay, and a blob that carried it never matched
                    // its own replay (the restore ignores them already).
                    if (!callback || is_bmmo_callback(callback)) continue;
                    callback_record rec;
                    rec.slot = slot;
                    rec.behavior = static_cast<uint32_t>(callback->m_BehaviorID);
                    rec.type = static_cast<int32_t>(callback->m_Type);
                    out.callbacks.push_back(rec);
                }
            }
        }

        if (report) {
            report->objects = static_cast<uint32_t>(out.objects.size());
            report->cores = static_cast<uint32_t>(movable_cores.size());
            report->collisions = static_cast<uint32_t>(out.collisions.size());
            report->contacts = static_cast<uint32_t>(out.contacts.size());
            report->systems = static_cast<uint32_t>(out.systems.size());
            report->units = static_cast<uint32_t>(out.units.size());
            report->events = static_cast<uint32_t>(out.events.size());
            report->hull_entries = static_cast<uint32_t>(hull_entries);
        }
        return true;
    }
}
