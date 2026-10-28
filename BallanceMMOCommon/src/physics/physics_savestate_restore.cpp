// The IVP savestate's restore (docs/ivp-savestate-design.md, "The restore, in
// order"): tears the live collision state of the environment behind a
// CKIpionManager down and rebuilds it from a world_record, steps 0-15.
//
// Two rules decide everything below.  Nothing IVP computes during the restore
// survives it: every value a constructor or a bookkeeping call sets is
// overwritten from the record afterwards.  And no IVP transition (revive,
// freeze, recheck, impact) runs: cores are written directly, lists are linked
// directly, events and hull entries are added directly.
//
// No floating-point arithmetic here: every value is copied (the headless
// library is built without the physics FP flags).  In release builds IVP_IF()
// is dead code and IVP_ASSERT is compiled out, so the bookkeeping calls do not
// reset mindist_status and IVP_U_Vector::remove on an absent element writes
// elems[-1]: nothing is removed here that IVP may already have removed.
//
// Before anything is touched the whole record is checked against itself and
// against the live world (indices, names, types, surfaces, edges): a strict
// restore fails there with the world intact; a lenient one marks what it
// cannot apply as skipped.  Once the clock is written (step 1) any error
// leaves the world damaged and the caller re-anchors.

#include "CKAll.h"
#include "CKIpionManager.h"
#include "PhysicsCallback.h"

#include "physics_savestate_internal.hpp"

#include <physics/physics_state.hpp>
#include <physics/callback_queue_plan.hpp>

#include "ivp_controller.hxx"
#include "ivp_surface_manager.hxx"
#include "ivp_compact_ledge.hxx"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <typeinfo>
#include <utility>
#include <vector>

void ivp_srand(int);

namespace bmmo::physics::savestate {

    namespace {
        // A list longer than this is a cycle, not a world.
        constexpr int kListGuard = 1 << 22;
        // Warning lines this restore adds to the report (warn() caps the
        // report as a whole as well).
        constexpr int kMaxRestoreWarnings = 32;

        // The throw-away mindist a contact point is constructed from needs a
        // delegator; it is deleted right after and tells nobody.
        class null_delegator final : public IVP_Collision_Delegator {
        public:
            void collision_is_going_to_be_deleted_event(IVP_Collision*) override {}
        };

        // is_bmmo_callback: physics_savestate_internal.hpp (shared with the capture).

        IVP_Controller* controller_identity(IVP_Controller* c) { return c; }
        IVP_Controller* controller_of_entry(IVP_Sim_Unit_Controller_Core_List* e) { return e ? e->l_controller : nullptr; }

        // Moves `vec` into `order` (which must be a permutation of its
        // elements) with swap_elems only: IVP_U_FVector::swap_elems keeps
        // the elements' fvector_index right.
        template <class Vec, class T>
        bool permute_to(Vec& vec, const std::vector<T*>& order) {
            if (static_cast<int>(order.size()) != vec.len()) return false;
            for (int i = 0; i < static_cast<int>(order.size()); ++i) {
                int found = -1;
                for (int k = i; k < vec.len(); ++k) {
                    if (vec.element_at(k) == order[static_cast<size_t>(i)]) {
                        found = k;
                        break;
                    }
                }
                if (found < 0) return false;
                if (found != i) vec.swap_elems(i, found);
            }
            return true;
        }

        // Step 0 / 14: the environment's collision listeners swapped out,
        // every object's collision_listener_exists flag cleared and the
        // bridge event log muted, all put back on every path.
        class mute_guard {
        public:
            mute_guard() = default;
            mute_guard(const mute_guard&) = delete;
            mute_guard& operator=(const mute_guard&) = delete;
            ~mute_guard() { release(); }

            void engage(IVP_Environment* env, const live_world& live) {
                if (engaged_ || !env) return;
                env_ = env;
                engaged_ = true;
                bmmo::physics::set_event_log_muted(true);
                IVP_U_Vector<IVP_Listener_Collision>& listeners = IVP_BMMO_State::collision_listeners(*env);
                for (int i = 0; i < listeners.len(); ++i) listeners_.push_back(listeners.element_at(i));
                listeners.remove_all();
                for (const live_object& lo : live.objects) {
                    if (lo.object && lo.object->flags.collision_listener_exists) {
                        flagged_.push_back(lo.object);
                        lo.object->flags.collision_listener_exists = 0;
                    }
                }
            }

            void release() {
                if (!engaged_) return;
                engaged_ = false;
                IVP_U_Vector<IVP_Listener_Collision>& listeners = IVP_BMMO_State::collision_listeners(*env_);
                // Nothing registers a listener during a restore; should
                // something have, it stays, after the original ones.
                std::vector<IVP_Listener_Collision*> added;
                for (int i = 0; i < listeners.len(); ++i) added.push_back(listeners.element_at(i));
                listeners.remove_all();
                for (IVP_Listener_Collision* l : listeners_) listeners.add(l);
                for (IVP_Listener_Collision* l : added) listeners.add(l);
                for (IVP_Real_Object* o : flagged_) o->flags.collision_listener_exists = 1;
                listeners_.clear();
                flagged_.clear();
                // There is no getter for the mute: the restore leaves the
                // event log unmuted (see contract notes).
                bmmo::physics::set_event_log_muted(false);
            }

        private:
            bool engaged_ = false;
            IVP_Environment* env_ = nullptr;
            std::vector<IVP_Listener_Collision*> listeners_;
            std::vector<IVP_Real_Object*> flagged_;
        };

        struct built_collision {
            IVP_Collision* coll = nullptr;
            IVP_Mindist* md = nullptr;                    // kind 0 and 1
            IVP_Mindist_Recursive* recursive = nullptr;   // kind 1
            IVP_OO_Watcher* watcher = nullptr;            // kind 2
        };

        struct restore_counts {
            uint32_t objects = 0, cores = 0, collisions = 0, contacts = 0, systems = 0, units = 0, events = 0,
                     hull_entries = 0, skipped = 0;
        };

        class restorer {
        public:
            restorer(CKIpionManager* physics, const world_record& world, uint32_t flags, std::string& error,
                     savestate_report* report, const savestate_travel* travel = nullptr)
                : physics_(physics), world_(world), error_(error), report_(report),
                  strict_((flags & SAVESTATE_STRICT) != 0u), keep_unknown_((flags & SAVESTATE_KEEP_UNKNOWN) != 0u),
                  travel_(travel) {}

            bool run();
            bool rebuild();
            bool check_callbacks();
            bool unknowns();
            void salvage();
            void fill_report() const;

        private:
            // ---- inputs ----
            CKIpionManager* physics_;
            const world_record& world_;
            std::string& error_;
            savestate_report* report_;
            const bool strict_;
            const bool keep_unknown_;
            // A travelling blob (savestate_travel, resync step A3): null for
            // the ordinary, in-process restore.
            const savestate_travel* travel_;
            uint32_t excluded_ = 0, renamed_ = 0;

            // ---- progress and diagnostics ----
            int step_ = 0;
            const char* step_name_ = "preconditions";
            bool damaged_ = false;
            int warnings_ = 0;
            restore_counts counts_;

            // ---- the live world and the mapping ----
            live_world live_;
            IVP_Environment* env_ = nullptr;
            std::vector<int32_t> live_of_;      // record object -> live index, -1 = absent or skipped
            std::vector<uint8_t> unknown_;      // live index -> no record of that name at all
            mute_guard guard_;

            // ---- the plan (decided before the teardown) ----
            std::vector<uint8_t> use_coll_;
            std::vector<const IVP_Compact_Edge*> coll_edges_;      // 2 per collision
            std::vector<uint8_t> use_contact_;
            std::vector<const IVP_Compact_Edge*> contact_edges_;   // 2 per contact
            std::vector<uint8_t> use_system_;

            // ---- what the teardown keeps (SAVESTATE_KEEP_UNKNOWN) ----
            std::set<IVP_Collision*> kept_roots_;
            std::set<const void*> kept_;   // every kept collision as a time event (void*)
            std::vector<IVP_Mindist*> kept_exact_, kept_invalid_;
            std::vector<IVP_Core*> unknown_revive_;   // unknown cores the live revive list held (re-added in step 13)

            // ---- what the rebuild made ----
            std::vector<built_collision> built_;
            std::vector<IVP_Simulation_Unit*> built_units_;
            std::vector<IVP_Friction_System*> built_systems_;
            std::vector<IVP_Contact_Point*> built_contacts_;
            std::map<IVP_Controller*, std::pair<int32_t, uint8_t>> friction_handles_;

            // ---- helpers ----
            void set_step(int step, const char* name) {
                step_ = step;
                step_name_ = name;
            }
            bool fail(const std::string& message) {
                char head[160];
                std::snprintf(head, sizeof(head), "savestate restore, step %d (%s)%s: ", step_, step_name_,
                              damaged_ ? ", world damaged" : "");
                error_ = head + message;
                return false;
            }
            void note(const std::string& line) {
                if (warnings_ < kMaxRestoreWarnings) warn(report_, line);
                else if (warnings_ == kMaxRestoreWarnings) warn(report_, "... further restore warnings dropped");
                ++warnings_;
            }
            // A record the restore cannot apply: strict fails, lenient warns
            // and counts it skipped.
            bool problem(const std::string& message) {
                if (strict_) return fail(message);
                ++counts_.skipped;
                note(message);
                return true;
            }
            // A record dropped because a record it refers to was skipped
            // (already reported once).
            void skip_quietly() { ++counts_.skipped; }

            static bool in(int64_t v, size_t n) { return v >= 0 && static_cast<uint64_t>(v) < static_cast<uint64_t>(n); }
            std::string name_of(int32_t rec) const {
                if (!in(rec, world_.objects.size())) return "#" + std::to_string(rec);
                return world_.objects[static_cast<size_t>(rec)].name;
            }
            std::string coll_name(int32_t i) const {
                const collision_record& c = world_.collisions[static_cast<size_t>(i)];
                static const char* const kKinds[] = {"mindist", "recursive mindist", "watcher"};
                return std::string(c.kind <= 2 ? kKinds[c.kind] : "collision") + " " + std::to_string(i) + " ("
                    + name_of(c.objects[0]) + " / " + name_of(c.objects[1]) + ")";
            }
            const live_object* live_object_of(int32_t rec) const {
                if (!in(rec, live_of_.size())) return nullptr;
                const int32_t li = live_of_[static_cast<size_t>(rec)];
                return li < 0 ? nullptr : &live_.objects[static_cast<size_t>(li)];
            }
            IVP_Real_Object* object_of(int32_t rec) const {
                const live_object* lo = live_object_of(rec);
                return lo ? lo->object : nullptr;
            }
            IVP_Core* core_of(int32_t rec) const {
                const live_object* lo = live_object_of(rec);
                return lo ? lo->core : nullptr;
            }
            bool present(int32_t rec) const { return live_object_of(rec) != nullptr; }
            const IVP_Compact_Edge* resolve(const edge_ref& ref) const {
                if (!in(ref.object, live_of_.size())) return nullptr;
                const int32_t li = live_of_[static_cast<size_t>(ref.object)];
                if (li < 0) return nullptr;
                // resolve_edge names objects by LIVE index (name_edge on
                // the capture side indexes the same sorted table).
                edge_ref live_ref = ref;
                live_ref.object = li;
                return resolve_edge(live_, live_ref);
            }
            bool is_unknown(IVP_Real_Object* object) const {
                const int32_t li = live_.index_of(object);
                return li < 0 || unknown_[static_cast<size_t>(li)] != 0;
            }
            static const void* as_event(IVP_Collision* c) { return static_cast<const void*>(static_cast<IVP_Time_Event*>(c)); }
            IVP_Controller* friction_handle(int32_t system, uint8_t part) const;

            // ---- the steps ----
            bool validate();
            bool map_objects();
            bool plan();
            bool clock();
            bool teardown();
            void collect_kept(IVP_Collision* c, int depth);
            bool cores_pass(bool first);
            void write_core(IVP_Core* core, const core_record& r);
            bool units();
            bool collisions();
            bool build_watcher(int32_t i, IVP_Collision_Delegator* delegator);
            bool ov_tree();
            bool lists();
            bool hull_lists();
            bool events();
            bool friction();
            bool controllers();
            template <class E>
            bool order_controllers(IVP_U_Vector<E>& vec, IVP_Controller* (*controller_of)(E*),
                                   const std::vector<controller_ref>& refs, const std::string& owner);
            bool revive();
            bool physics_rt();
            bool caches();
        };

        // ------------------------------------------------------------------
        // Before anything is touched: the record against itself.
        // ------------------------------------------------------------------
        bool restorer::validate() {
            set_step(0, "record check");
            const size_t n_obj = world_.objects.size(), n_coll = world_.collisions.size(),
                         n_contacts = world_.contacts.size(), n_sys = world_.systems.size(),
                         n_units = world_.units.size(), n_nodes = world_.ov_nodes.size();
            auto bad = [&](const std::string& what) { return fail("corrupt savestate: " + what); };
            auto controllers_ok = [&](const std::vector<controller_ref>& refs) {
                for (const controller_ref& ref : refs) {
                    if (ref.kind > 1) return false;
                    if (ref.kind == 0 && (!in(ref.system, n_sys) || ref.part > 2)) return false;
                }
                return true;
            };

            for (size_t i = 0; i < n_obj; ++i) {
                const object_record& o = world_.objects[i];
                const std::string who = "object '" + o.name + "'";
                if (i > 0 && !(world_.objects[i - 1].name < o.name)) return bad("objects not sorted by name at " + who);
                if (o.core.unit != -1 && !in(o.core.unit, n_units)) return bad(who + " names unit " + std::to_string(o.core.unit));
                if (!controllers_ok(o.core.controllers)) return bad(who + " has a bad controller descriptor");
                for (const hull_entry& h : o.hull) {
                    if (h.kind > 1 || (h.kind == 1 && (!in(h.collision, n_coll) || h.side > 1)))
                        return bad(who + " has a bad hull entry");
                    if (h.kind == 1) {
                        const collision_record& c = world_.collisions[static_cast<size_t>(h.collision)];
                        if (c.objects[h.side] != static_cast<int32_t>(i))
                            return bad(who + " lists a hull entry of " + coll_name(h.collision) + " side " + std::to_string(h.side));
                    }
                }
                if (o.ov_node != -1 && !in(o.ov_node, n_nodes)) return bad(who + " names overlap node " + std::to_string(o.ov_node));
                for (int32_t c : o.ov_collisions)
                    if (!in(c, n_coll) || world_.collisions[static_cast<size_t>(c)].parent != -1)
                        return bad(who + " lists a non-root collision in its fvector");
                for (const std::vector<int32_t>* list : {&o.exact_synapses, &o.invalid_synapses}) {
                    for (int32_t s : *list) {
                        if (s < 0 || !in(s >> 1, n_coll) || world_.collisions[static_cast<size_t>(s >> 1)].kind == 2)
                            return bad(who + " lists a bad mindist synapse");
                        if (world_.collisions[static_cast<size_t>(s >> 1)].synapse[s & 1].object != static_cast<int32_t>(i))
                            return bad(who + " lists a synapse of another object");
                    }
                }
                for (int32_t s : o.friction_synapses) {
                    if (s < 0 || !in(s >> 1, n_contacts)) return bad(who + " lists a bad friction synapse");
                    if (world_.contacts[static_cast<size_t>(s >> 1)].synapse[s & 1].object != static_cast<int32_t>(i))
                        return bad(who + " lists a friction synapse of another object");
                }
            }

            for (size_t i = 0; i < n_coll; ++i) {
                const collision_record& c = world_.collisions[i];
                const int32_t self = static_cast<int32_t>(i);
                if (c.kind > 2) return bad("collision " + std::to_string(i) + " has kind " + std::to_string(c.kind));
                if (c.parent != -1) {
                    if (!in(c.parent, i)) return bad("collision " + std::to_string(i) + " comes before its parent");
                    const uint8_t pk = world_.collisions[static_cast<size_t>(c.parent)].kind;
                    if (pk == 0) return bad("collision " + std::to_string(i) + " has a plain mindist as parent");
                    if (c.kind == 2) return bad("watcher " + std::to_string(i) + " is not a root");
                }
                if (!in(c.objects[0], n_obj) || !in(c.objects[1], n_obj))
                    return bad("collision " + std::to_string(i) + " names a bad object");
                if (c.kind != 2) {
                    for (int k = 0; k < 2; ++k) {
                        if (!in(c.synapse[k].object, n_obj) || c.synapse[k].status > IVP_ST_BACKSIDE)
                            return bad("collision " + std::to_string(i) + " has a bad synapse");
                    }
                    const uint8_t st = c.status;
                    if (st != IVP_MD_UNINITIALIZED && st != IVP_MD_INVALID && st != IVP_MD_EXACT
                        && st != IVP_MD_HULL_RECURSIVE && st != IVP_MD_HULL)
                        return bad("collision " + std::to_string(i) + " has mindist status " + std::to_string(st));
                    if (c.function != IVP_MF_COLLISION || c.phantom_set != 0)
                        return fail(coll_name(self) + " is a phantom mindist: the savestate fails closed on phantoms");
                    if (c.kind == 1 && (c.recursive_status < -1 || c.recursive_status > 1))
                        return bad("collision " + std::to_string(i) + " has recursive status " + std::to_string(c.recursive_status));
                }
                if (c.kind == 0 && !c.children.empty()) return bad("plain mindist " + std::to_string(i) + " has children");
                for (int32_t ch : c.children)
                    if (!in(ch, n_coll) || ch <= self || world_.collisions[static_cast<size_t>(ch)].parent != self)
                        return bad("collision " + std::to_string(i) + " lists a bad child");
            }
            {
                std::set<int32_t> seen;
                for (const std::vector<int32_t>* list : {&world_.exact_list, &world_.invalid_list}) {
                    for (int32_t c : *list) {
                        if (!in(c, n_coll) || world_.collisions[static_cast<size_t>(c)].kind == 2 || !seen.insert(c).second)
                            return bad("the mindist manager's lists name collision " + std::to_string(c));
                    }
                }
            }
            {
                std::set<int32_t> seen;
                int psi = 0;
                for (const event_record& ev : world_.events) {
                    if (ev.collision == -1) {
                        ++psi;
                        continue;
                    }
                    if (!in(ev.collision, n_coll) || world_.collisions[static_cast<size_t>(ev.collision)].kind == 2
                        || !seen.insert(ev.collision).second)
                        return bad("the event queue names collision " + std::to_string(ev.collision));
                    if (!world_.collisions[static_cast<size_t>(ev.collision)].scheduled)
                        return bad("the event queue holds " + coll_name(ev.collision) + ", which is not scheduled");
                }
                if (psi != 1) return bad("the event queue holds " + std::to_string(psi) + " PSI events");
                for (size_t i = 0; i < n_coll; ++i)
                    if (world_.collisions[i].kind != 2 && world_.collisions[i].scheduled && !seen.count(static_cast<int32_t>(i)))
                        return bad(coll_name(static_cast<int32_t>(i)) + " is scheduled but not in the event queue");
            }

            for (size_t i = 0; i < n_contacts; ++i) {
                const contact_record& c = world_.contacts[i];
                if (!in(c.system, n_sys) || !in(c.synapse[0].object, n_obj) || !in(c.synapse[1].object, n_obj)
                    || c.synapse[0].status > IVP_ST_BACKSIDE || c.synapse[1].status > IVP_ST_BACKSIDE || c.status > IVP_CPBS_BROKEN)
                    return bad("contact " + std::to_string(i) + " is malformed");
            }
            {
                std::vector<int> owner(n_contacts, -1);
                for (size_t s = 0; s < n_sys; ++s) {
                    const friction_system_record& fs = world_.systems[s];
                    const std::string who = "friction system " + std::to_string(s);
                    for (const std::vector<int32_t>* list : {&fs.cores, &fs.movable_cores})
                        for (int32_t o : *list)
                            if (!in(o, n_obj)) return bad(who + " names a bad core");
                    for (int32_t d : fs.dists) {
                        if (!in(d, n_contacts) || world_.contacts[static_cast<size_t>(d)].system != static_cast<int32_t>(s)
                            || owner[static_cast<size_t>(d)] != -1)
                            return bad(who + " lists a bad contact");
                        owner[static_cast<size_t>(d)] = static_cast<int>(s);
                    }
                    for (const pair_record& p : fs.pairs) {
                        if (!in(p.objects[0], n_obj) || !in(p.objects[1], n_obj)) return bad(who + " has a bad pair");
                        for (int32_t d : p.dists)
                            if (!in(d, n_contacts) || world_.contacts[static_cast<size_t>(d)].system != static_cast<int32_t>(s))
                                return bad(who + " has a pair with a bad contact");
                    }
                    for (const friction_info_record& info : fs.infos) {
                        if (!in(info.object, n_obj)) return bad(who + " has a bad friction info");
                        for (int32_t d : info.springs)
                            if (!in(d, n_contacts) || world_.contacts[static_cast<size_t>(d)].system != static_cast<int32_t>(s))
                                return bad(who + " has a friction info with a bad contact");
                    }
                    if (static_cast<size_t>(fs.dist_number) != fs.dists.size())
                        return bad(who + " counts " + std::to_string(fs.dist_number) + " contacts but lists " + std::to_string(fs.dists.size()));
                }
                for (size_t i = 0; i < n_contacts; ++i)
                    if (owner[i] == -1) return bad("contact " + std::to_string(i) + " is in no system's list");
            }

            {
                std::set<int32_t> seen;
                for (size_t u = 0; u < n_units; ++u) {
                    const unit_record& ur = world_.units[u];
                    if (!controllers_ok(ur.controllers)) return bad("unit " + std::to_string(u) + " has a bad controller descriptor");
                    for (int32_t o : ur.cores)
                        if (!in(o, n_obj) || !seen.insert(o).second)
                            return bad("unit " + std::to_string(u) + " names core " + std::to_string(o));
                }
            }
            for (int32_t o : world_.revive_list)
                if (!in(o, n_obj)) return bad("the revive list names object " + std::to_string(o));
            for (int32_t o : world_.movable_objects)
                if (!in(o, n_obj)) return bad("the movable list names object " + std::to_string(o));

            {
                std::vector<int> elements_seen(n_obj, 0);
                for (size_t i = 0; i < n_nodes; ++i) {
                    const ov_node_record& node = world_.ov_nodes[i];
                    if (node.parent != -1 && !in(node.parent, n_nodes)) return bad("overlap node " + std::to_string(i) + " has a bad parent");
                    if ((node.parent == -1) != (static_cast<int32_t>(i) == world_.ov_root))
                        return bad("overlap node " + std::to_string(i) + " disagrees with the root");
                    for (int32_t ch : node.children)
                        if (!in(ch, n_nodes) || world_.ov_nodes[static_cast<size_t>(ch)].parent != static_cast<int32_t>(i))
                            return bad("overlap node " + std::to_string(i) + " lists a bad child");
                    for (int32_t e : node.elements) {
                        if (!in(e, n_obj) || elements_seen[static_cast<size_t>(e)]++ != 0
                            || world_.objects[static_cast<size_t>(e)].ov_node != static_cast<int32_t>(i)
                            || !world_.objects[static_cast<size_t>(e)].has_ov)
                            return bad("overlap node " + std::to_string(i) + " lists a bad element");
                    }
                }
                if (world_.ov_root != -1 && !in(world_.ov_root, n_nodes)) return bad("bad overlap root");
                if (world_.ov_root == -1 && n_nodes != 0) return bad("overlap nodes without a root");
            }
            {
                std::set<int32_t> seen;
                for (const cache_slot_record& slot : world_.cache_slots)
                    if (slot.object != -1 && (!in(slot.object, n_obj) || !seen.insert(slot.object).second))
                        return bad("the cache ring names object " + std::to_string(slot.object));
                if (!world_.cache_slots.empty() && !in(world_.cache_reuse_index, world_.cache_slots.size()))
                    return bad("the cache ring's reuse index is out of range");
            }
            for (const callback_record& cb : world_.callbacks)
                if (cb.slot < 0 || cb.slot > 2) return bad("a PreSimulate callback names slot " + std::to_string(cb.slot));
            return true;
        }

        // Every record object to its live twin by name.  A travelling blob
        // (savestate_travel): the name renamed first, a name several objects
        // share matched by occurrence, and an excluded name left out on both
        // sides (its live body stays unknown to the blob).
        bool restorer::map_objects() {
            set_step(0, "objects");
            live_of_.assign(world_.objects.size(), -1);
            unknown_.assign(live_.objects.size(), 1);
            std::map<std::string, size_t> occurrences;   // travelling: records of each (renamed) name so far
            for (size_t i = 0; i < world_.objects.size(); ++i) {
                const object_record& o = world_.objects[i];
                int32_t li = -1;
                if (travel_) {
                    std::string name = o.name;
                    for (const auto& [from, to]: travel_->renames)
                        if (name == from) {
                            name = to;
                            ++renamed_;
                            break;
                        }
                    bool excluded = false;
                    for (const std::string& prefix: travel_->excluded_prefixes) {
                        if (prefix.empty()) continue;
                        // '=': one name exactly (savestate_travel).
                        if (prefix[0] == '=') {
                            if (name.compare(0, std::string::npos, prefix, 1, std::string::npos) == 0) excluded = true;
                        } else if (name.compare(0, prefix.size(), prefix) == 0) {
                            excluded = true;
                        }
                    }
                    if (excluded) {
                        ++excluded_;
                        continue;   // its records are skipped below (an object is missing)
                    }
                    li = live_.find_by_occurrence(name, occurrences[name]++);
                    if (li < 0) {
                        if (!problem("object '" + o.name + "'" + (name != o.name ? " (here '" + name + "')" : std::string())
                                     + " is in the travelling savestate but not in the world"))
                            return false;
                        continue;
                    }
                } else {
                    li = live_.find_by_name(o.name, o.entity_id);
                }
                if (li < 0) {
                    if (!problem("object '" + o.name + "' is in the savestate but not in the world")) return false;
                    continue;
                }
                const live_object& lo = live_.objects[static_cast<size_t>(li)];
                unknown_[static_cast<size_t>(li)] = 0;
                std::string why;
                if (!lo.object || !lo.core) why = "has no body";
                else if (static_cast<int>(lo.object->get_type()) != static_cast<int>(o.type)) why = "changed type";
                else if (lo.surface_size != o.surface_size) why = "changed surface";
                else if ((lo.core->physical_unmoveable ? 0 : 1) != (o.movable ? 1 : 0)) why = "changed movability";
                else if ((lo.object->get_ov_element() != nullptr) != (o.has_ov != 0)) why = "changed collision detection";
                if (!why.empty()) {
                    if (!problem("object '" + o.name + "' " + why + " since the savestate")) return false;
                    continue;
                }
                live_of_[i] = li;
                ++counts_.objects;
            }
            return true;
        }

        // What can be applied: collisions, contacts, systems.
        bool restorer::plan() {
            set_step(0, "plan");
            const size_t n_coll = world_.collisions.size();
            use_coll_.assign(n_coll, 0);
            coll_edges_.assign(2 * n_coll, nullptr);
            for (size_t i = 0; i < n_coll; ++i) {
                const collision_record& c = world_.collisions[i];
                std::string why;
                if (c.parent >= 0 && !use_coll_[static_cast<size_t>(c.parent)]) why = "its parent was skipped";
                else if (!present(c.objects[0]) || !present(c.objects[1])) why = "an object is missing";
                else if (c.parent < 0 && (!object_of(c.objects[0])->get_ov_element() || !object_of(c.objects[1])->get_ov_element()))
                    why = "an object has no overlap element";
                else if (c.kind != 2) {
                    for (int k = 0; k < 2 && why.empty(); ++k) {
                        const IVP_Compact_Edge* edge = resolve(c.synapse[k]);
                        if (!edge) why = "edge " + describe_edge(world_, c.synapse[k]) + " does not resolve";
                        coll_edges_[2 * i + static_cast<size_t>(k)] = edge;
                    }
                }
                if (!why.empty()) {
                    if (c.parent >= 0 && !use_coll_[static_cast<size_t>(c.parent)]) skip_quietly();
                    else if (!problem(coll_name(static_cast<int32_t>(i)) + ": " + why)) return false;
                    continue;
                }
                use_coll_[i] = 1;
            }

            const size_t n_contacts = world_.contacts.size();
            use_contact_.assign(n_contacts, 0);
            contact_edges_.assign(2 * n_contacts, nullptr);
            for (size_t i = 0; i < n_contacts; ++i) {
                const contact_record& c = world_.contacts[i];
                std::string why;
                for (int k = 0; k < 2 && why.empty(); ++k) {
                    if (!present(c.synapse[k].object)) {
                        why = "object '" + name_of(c.synapse[k].object) + "' is missing";
                        break;
                    }
                    const IVP_Compact_Edge* edge = resolve(c.synapse[k]);
                    if (!edge) why = "edge " + describe_edge(world_, c.synapse[k]) + " does not resolve";
                    contact_edges_[2 * i + static_cast<size_t>(k)] = edge;
                }
                if (!why.empty()) {
                    if (!problem("contact " + std::to_string(i) + ": " + why)) return false;
                    continue;
                }
                use_contact_[i] = 1;
            }

            use_system_.assign(world_.systems.size(), 0);
            for (size_t s = 0; s < world_.systems.size(); ++s) {
                const friction_system_record& fs = world_.systems[s];
                std::string why;
                for (int32_t o : fs.cores)
                    if (why.empty() && !present(o)) why = "core '" + name_of(o) + "' is missing";
                for (int32_t o : fs.movable_cores)
                    if (why.empty() && !present(o)) why = "core '" + name_of(o) + "' is missing";
                for (const friction_info_record& info : fs.infos)
                    if (why.empty() && !present(info.object)) why = "core '" + name_of(info.object) + "' is missing";
                for (const pair_record& p : fs.pairs)
                    if (why.empty() && (!present(p.objects[0]) || !present(p.objects[1]))) why = "a pair's core is missing";
                for (int32_t d : fs.dists)
                    if (why.empty() && !use_contact_[static_cast<size_t>(d)]) why = "contact " + std::to_string(d) + " was skipped";
                if (!why.empty()) {
                    if (!problem("friction system " + std::to_string(s) + ": " + why)) return false;
                    for (int32_t d : fs.dists) {
                        if (use_contact_[static_cast<size_t>(d)]) {
                            use_contact_[static_cast<size_t>(d)] = 0;
                            skip_quietly();
                        }
                    }
                    // The bodies that lose their contacts with the system:
                    // named for the caller, whose own per-body record can
                    // re-create them (the rollback engine's set_contacts).
                    if (report_)
                        for (int32_t o : fs.cores) {
                            if (!present(o)) continue;
                            const std::string& name = world_.objects[static_cast<size_t>(o)].name;
                            if (std::find(report_->contacts_dropped.begin(), report_->contacts_dropped.end(), name)
                                == report_->contacts_dropped.end())
                                report_->contacts_dropped.push_back(name);
                        }
                    continue;
                }
                use_system_[s] = 1;
            }
            return true;
        }

        // ------------------------------------------------------------------
        // Step 1: the clock.
        // ------------------------------------------------------------------
        bool restorer::clock() {
            set_step(1, "clock");
            damaged_ = true;
            IVP_Environment& env = *env_;
            IVP_BMMO_State::current_time(env) = IVP_Time(world_.current_time);
            IVP_BMMO_State::time_of_last_psi(env) = IVP_Time(world_.time_of_last_psi);
            IVP_BMMO_State::time_of_next_psi(env) = IVP_Time(world_.time_of_next_psi);
            IVP_BMMO_State::next_movement_check(env) = static_cast<short>(world_.next_movement_check);
            IVP_BMMO_State::env_state(env) = static_cast<IVP_ENV_STATE>(world_.env_state);
            IVP_Time_Manager* tm = IVP_BMMO_State::time_manager(env);
            if (!tm || !tm->min_hash || !tm->psi_event) return fail("the environment has no time manager");
            tm->base_time = IVP_Time(world_.base_time);
            tm->last_time = world_.last_time;
            // The frame delta's moving average is integration state and comes
            // back; the time factor does NOT.  It is written by the level's
            // Gameplay script (SetPhysicsGlobals: 1.0 while a level loads,
            // 2.0 once it runs, 0 while paused), a script the replay does not
            // re-execute - only the mechanism scripts are.  A blob captured
            // before the script wrote 2.0 would otherwise put 1.0 back and
            // nothing would ever write 2.0 again: measured on the arena map
            // as a client whose whole world ran at half rate for the rest of
            // the session (ball x after 400 ticks: 29.99 against the
            // control's 122.53).  So the factor stays live, like the gravity
            // below, and the step is recomputed from it the way
            // CKIpionManager::SetDeltaTime does.
            physics_->m_DeltaTime = world_.delta_time_ms;
            if (physics_->m_PhysicsTimeFactor != world_.time_factor) {
                char line[160];
                std::snprintf(line, sizeof(line), "the physics time factor is %a live and %a in the savestate (kept live)",
                              static_cast<double>(physics_->m_PhysicsTimeFactor), static_cast<double>(world_.time_factor));
                note(line);
            }
            physics_->m_PhysicsDeltaTime = physics_->m_DeltaTime * physics_->m_PhysicsTimeFactor;
            ivp_srand(static_cast<int>(world_.ivp_seed));
            bmmo::physics::random_set_state(world_.random_state);
            // Format 3 (step SPGH2): the spawn ghost's per-group state, before
            // anything below rechecks a body (the pair filter reads it).  A
            // blob without it (the rule off where it was captured, format 2)
            // leaves a rule-on world with no group seen.  A travelling blob's
            // groups ("P#<join order>") are every world's names.
            {
                std::vector<bmmo::physics::spawn_ghost_record> groups;
                if (world_.spawn_ghost_present) {
                    groups.reserve(world_.spawn_ghost.size());
                    for (const auto& g : world_.spawn_ghost) {
                        bmmo::physics::spawn_ghost_record r;
                        r.group = g.group;
                        r.seen = g.seen;
                        r.present = g.present;
                        r.solid = g.solid;
                        r.marked = g.marked;
                        groups.push_back(std::move(r));
                    }
                }
                bmmo::physics::spawn_ghost_record_set(physics_, groups);
            }
            // Monotonic cache token: never rewound, bumped before any IVP
            // computation of the restore, and every cache dropped (a bump
            // alone only invalidates simulated objects' caches).
            IVP_BMMO_State::time_code(env)++;
            for (const live_object& lo : live_.objects)
                if (lo.object) IVP_Cache_Object_Manager::invalid_cache_object(lo.object);
            return true;
        }

        // ------------------------------------------------------------------
        // Step 2: the teardown.
        // ------------------------------------------------------------------
        void restorer::collect_kept(IVP_Collision* c, int depth) {
            if (!c || depth > 64) return;
            kept_.insert(as_event(c));
            if (IVP_OO_Watcher* w = dynamic_cast<IVP_OO_Watcher*>(c)) {
                IVP_U_FVector<IVP_Collision>& children = IVP_BMMO_State::watcher_children(*w);
                for (int k = 0; k < children.len(); ++k) collect_kept(children.element_at(k), depth + 1);
            } else if (IVP_Mindist_Recursive* r = dynamic_cast<IVP_Mindist_Recursive*>(c)) {
                for (int k = 0; k < r->mindists.len(); ++k) collect_kept(r->mindists.element_at(k), depth + 1);
            }
        }

        bool restorer::teardown() {
            set_step(2, "teardown");
            IVP_Environment& env = *env_;

            // (a) Friction: every contact point through its system, then the
            // emptied system.  delete_friction_distance already removes the
            // cores (remove_core_from_system) and the infos; a second removal
            // would write elems[-1] in release.
            {
                std::vector<IVP_Friction_System*> systems;
                std::set<IVP_Friction_System*> seen;
                for (const live_object& lo : live_.objects) {
                    IVP_Core* core = lo.core;
                    if (!core || core->physical_unmoveable) continue;
                    IVP_Friction_Info_For_Core* info = core->core_friction_info.for_moveables.moveable_core_friction_info;
                    if (!info || !info->l_friction_system) continue;
                    if (seen.insert(info->l_friction_system).second) systems.push_back(info->l_friction_system);
                }
                for (IVP_Friction_System* fs : systems) {
                    int guard = 0;
                    for (IVP_Contact_Point* cp = fs->get_first_friction_dist(); cp; cp = fs->get_first_friction_dist()) {
                        fs->delete_friction_distance(cp);
                        if (++guard > kListGuard) return fail("a friction system's contact list does not empty");
                    }
                    if (IVP_BMMO_State::fs_dist_number(*fs) != 0)
                        return fail("a friction system still counts " + std::to_string(IVP_BMMO_State::fs_dist_number(*fs))
                                    + " contacts after the last was deleted");
                    delete fs;
                }
            }

            // (b) Collisions: every root once (a root sits in both objects'
            // fvectors; its destructor removes events, list membership, hull
            // entries, ledge references, children and both fvector entries).
            {
                std::set<IVP_Collision*> deleted;
                for (size_t li = 0; li < live_.objects.size(); ++li) {
                    IVP_Real_Object* obj = live_.objects[li].object;
                    IVP_OV_Element* elem = obj ? obj->get_ov_element() : nullptr;
                    if (!elem) continue;
                    std::vector<IVP_Collision*> roots;
                    for (int k = 0; k < elem->collision_fvector.len(); ++k) roots.push_back(elem->collision_fvector.element_at(k));
                    for (IVP_Collision* root : roots) {
                        if (deleted.count(root) || kept_roots_.count(root)) continue;
                        if (keep_unknown_) {
                            IVP_Real_Object* pair[2] = {nullptr, nullptr};
                            root->get_objects(pair);
                            if (is_unknown(pair[0]) && is_unknown(pair[1])) {
                                kept_roots_.insert(root);
                                continue;
                            }
                        }
                        deleted.insert(root);
                        delete root;
                    }
                }
                for (IVP_Collision* root : kept_roots_) collect_kept(root, 0);
            }

            // (c) Overlap elements, statics too: the tree ends empty.  A kept
            // unknown body (SAVESTATE_KEEP_UNKNOWN) leaves the tree as well -
            // the tree is rebuilt from the record and cannot hold it - but
            // keeps its hull entry, so its next hull event rechecks it back
            // in (remove_ov_element on a node-less element is a no-op).
            IVP_OV_Tree_Manager* tree = IVP_BMMO_State::ov_tree(env);
            if (!tree) return fail("the environment has no overlap tree");
            for (size_t li = 0; li < live_.objects.size(); ++li) {
                IVP_Real_Object* obj = live_.objects[li].object;
                IVP_OV_Element* elem = obj ? obj->get_ov_element() : nullptr;
                if (!elem) continue;
                tree->remove_ov_element(elem);
                if (keep_unknown_ && unknown_[li]) continue;
                if (elem->hull_manager) {
                    elem->hull_manager->remove_synapse(elem);
                    elem->hull_manager = nullptr;
                }
            }

            // (d) What is left in a hull list is something the record does
            // not know (an anchor, a phantom's listener).
            for (size_t li = 0; li < live_.objects.size(); ++li) {
                if (keep_unknown_ && unknown_[li]) continue;
                IVP_Real_Object* obj = live_.objects[li].object;
                if (!obj) continue;
                IVP_U_Min_List& list = IVP_BMMO_State::hull_list(*obj->get_hull_manager());
                if (list.counter != 0)
                    return fail("'" + live_.objects[li].name + "' keeps " + std::to_string(list.counter)
                                + " unknown hull listener(s) after the teardown");
            }

            // (e) The revive list; the flag is cleared so the re-add of
            // step 13 is not swallowed by add_revive_core's early return.  A
            // core the blob does not know keeps its place: step 13 re-adds it
            // after the recorded ones.
            {
                IVP_U_Vector<IVP_Core>& revive = IVP_BMMO_State::revive_list(env);
                for (int i = 0; i < revive.len(); ++i) {
                    IVP_Core* core = revive.element_at(i);
                    core->is_in_wakeup_vec = IVP_FALSE;
                    if (core->objects.len() > 0 && is_unknown(core->objects.element_at(0))) unknown_revive_.push_back(core);
                }
                revive.clear();
            }

            // What survived: only the kept collisions.
            IVP_Mindist_Manager* mm = IVP_BMMO_State::mindist_manager(env);
            if (!mm) return fail("the environment has no mindist manager");
            int guard = 0;
            for (IVP_Mindist* md = mm->exact_mindists; md; md = md->next) {
                if (!kept_.count(as_event(md))) return fail("an exact mindist survived the teardown");
                kept_exact_.push_back(md);
                if (++guard > kListGuard) return fail("the exact mindist list is a cycle");
            }
            for (IVP_Mindist* md = mm->invalid_mindists; md; md = md->next) {
                if (!kept_.count(as_event(md))) return fail("an invalid mindist survived the teardown");
                kept_invalid_.push_back(md);
                if (++guard > kListGuard) return fail("the invalid mindist list is a cycle");
            }
            for (size_t i = 0; i < world_.objects.size(); ++i) {
                IVP_Real_Object* obj = object_of(static_cast<int32_t>(i));
                if (!obj) continue;
                const std::string who = "'" + world_.objects[i].name + "'";
                if (IVP_BMMO_State::exact_synapses(*obj) || IVP_BMMO_State::invalid_synapses(*obj))
                    return fail(who + " keeps mindist synapses after the teardown");
                if (IVP_BMMO_State::friction_synapses(*obj)) return fail(who + " keeps a friction contact after the teardown");
                if (obj->get_ov_element() && obj->get_ov_element()->collision_fvector.len() != 0)
                    return fail(who + " keeps collisions after the teardown");
                IVP_Core* core = core_of(static_cast<int32_t>(i));
                if (core && !core->physical_unmoveable && core->core_friction_info.for_moveables.moveable_core_friction_info)
                    return fail(who + " keeps a friction info after the teardown");
            }
            return true;
        }

        // ------------------------------------------------------------------
        // Steps 3 and 11: cores and objects.
        // ------------------------------------------------------------------
        void restorer::write_core(IVP_Core* core, const core_record& r) {
            core->movement_state = static_cast<IVP_Movement_Type>(r.movement_state);
            core->temporarily_unmovable = r.temporarily_unmovable ? IVP_TRUE : IVP_FALSE;
            core->pinned = r.pinned ? IVP_TRUE : IVP_FALSE;
            core->set_fast_piling_allowed(r.fast_piling ? IVP_TRUE : IVP_FALSE);
            core->impacts_since_last_PSI = r.impacts_since_last_psi;
            core->time_of_last_psi = IVP_Time(r.time_of_last_psi);
            core->i_delta_time = r.i_delta_time;
            for (int k = 0; k < 3; ++k) {
                core->rot_speed_change.k[k] = r.rot_speed_change[k];
                core->speed_change.k[k] = r.speed_change[k];
                core->rot_speed.k[k] = r.rot_speed[k];
                core->speed.k[k] = r.speed[k];
                core->pos_world_f_core_last_psi.k[k] = r.pos_last_psi[k];
                core->delta_world_f_core_psis.k[k] = r.delta_psis[k];
                core->rotation_axis_world_space.k[k] = r.rotation_axis_ws[k];
            }
            core->q_world_f_core_last_psi.x = r.q_last[0];
            core->q_world_f_core_last_psi.y = r.q_last[1];
            core->q_world_f_core_last_psi.z = r.q_last[2];
            core->q_world_f_core_last_psi.w = r.q_last[3];
            core->q_world_f_core_next_psi.x = r.q_next[0];
            core->q_world_f_core_next_psi.y = r.q_next[1];
            core->q_world_f_core_next_psi.z = r.q_next[2];
            core->q_world_f_core_next_psi.w = r.q_next[3];
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 3; ++col) core->m_world_f_core_last_psi.set_elem(row, col, r.m_last_psi[row * 3 + col]);
            for (int k = 0; k < 3; ++k) core->m_world_f_core_last_psi.vv.k[k] = r.m_last_psi[9 + k];
            core->current_speed = r.current_speed;
            core->abs_omega = r.abs_omega;
            core->max_surface_rot_speed = r.max_surface_rot_speed;
            for (int c = 0; c < 2; ++c) {
                core->time_of_calm_reference[c] = IVP_Time(r.calm_time[c]);
                core->q_world_f_core_calm_reference[c].x = r.calm_quat[c][0];
                core->q_world_f_core_calm_reference[c].y = r.calm_quat[c][1];
                core->q_world_f_core_calm_reference[c].z = r.calm_quat[c][2];
                core->q_world_f_core_calm_reference[c].w = r.calm_quat[c][3];
                for (int k = 0; k < 3; ++k) core->position_world_f_core_calm_reference[c].k[k] = r.calm_position[c][k];
            }
        }

        bool restorer::cores_pass(bool first) {
            if (first) set_step(3, "cores and objects");
            else set_step(11, "cores, second pass");
            std::set<IVP_Core*> written;
            for (size_t i = 0; i < world_.objects.size(); ++i) {
                const object_record& o = world_.objects[i];
                IVP_Real_Object* obj = object_of(static_cast<int32_t>(i));
                IVP_Core* core = core_of(static_cast<int32_t>(i));
                if (!obj || !core) continue;
                obj->set_movement_state(static_cast<IVP_Movement_Type>(o.movement_state));
                if (o.movable && !core->physical_unmoveable) {
                    write_core(core, o.core);
                    if (written.insert(core).second && first) ++counts_.cores;
                }
                if (first) {
                    IVP_Hull_Manager_Base_Gradient& g = IVP_BMMO_State::hull_gradient(*obj->get_hull_manager());
                    g.last_vpsi_time = IVP_Time(o.last_vpsi_time);
                    g.gradient = o.gradient;
                    g.center_gradient = o.center_gradient;
                    g.hull_value_last_vpsi = o.hull_value_last_vpsi;
                    g.hull_center_value_last_vpsi = o.hull_center_value_last_vpsi;
                    g.hull_value_next_psi = o.hull_value_next_psi;
                    g.time_of_next_reset = o.time_of_next_reset;
                }
            }
            if (first) {
                // Recorded and asserted, never written.
                const IVP_U_Point* gravity = env_->get_gravity();
                bool same = gravity != nullptr;
                for (int k = 0; k < 3 && same; ++k) same = static_cast<float>(gravity->k[k]) == world_.gravity[k];
                if (!same) {
                    if (strict_) return fail("the environment's gravity differs from the savestate's");
                    note("the environment's gravity differs from the savestate's (not written)");
                }
            } else {
                // Nothing between the steps may have moved a unit (the
                // watcher sandbox runs IVP code).
                for (size_t u = 0; u < built_units_.size(); ++u) {
                    IVP_Simulation_Unit* unit = built_units_[u];
                    if (unit && static_cast<int>(unit->get_unit_movement_type()) != static_cast<int>(world_.units[u].movement_type))
                        return fail("simulation unit " + std::to_string(u) + " changed its movement type during the rebuild");
                }
            }
            return true;
        }

        // ------------------------------------------------------------------
        // Step 4: the simulation units, in reverse record order (the
        // manager prepends).
        // ------------------------------------------------------------------
        bool restorer::units() {
            set_step(4, "simulation units");
            IVP_Sim_Units_Manager* mgr = IVP_BMMO_State::units_manager(*env_);
            if (!mgr) return fail("the environment has no simulation units manager");
            built_units_.assign(world_.units.size(), nullptr);
            std::set<IVP_Core*> placed;
            for (int u = static_cast<int>(world_.units.size()) - 1; u >= 0; --u) {
                const unit_record& ur = world_.units[static_cast<size_t>(u)];
                std::vector<IVP_Core*> cores;
                for (int32_t o : ur.cores) {
                    IVP_Core* core = core_of(o);
                    if (!core) {
                        skip_quietly();   // the object was reported
                        continue;
                    }
                    if (!placed.insert(core).second)
                        return fail("the core of '" + name_of(o) + "' is in two simulation units");
                    cores.push_back(core);
                }
                if (cores.empty()) continue;
                IVP_Simulation_Unit* unit = new IVP_Simulation_Unit();
                for (IVP_Core* core : cores) {
                    IVP_Simulation_Unit* old = core->sim_unit_of_core;
                    // Deletes an emptied unit itself: not touched afterwards.
                    if (old) old->sim_unit_remove_core(core);
                    unit->add_sim_unit_core(core);
                    core->sim_unit_of_core = unit;
                }
                unit->clean_sim_unit();
                unit->sim_unit_calc_redundants();
                // The type before the unit enters a list: the manager picks
                // the list from it.
                IVP_BMMO_State::set_unit_type(*unit, static_cast<IVP_Movement_Type>(ur.movement_type));
                unit->union_find_needed_for_sim_unit = ur.union_find_needed ? IVP_TRUE : IVP_FALSE;
                unit->sim_unit_has_fast_objects = ur.has_fast_objects ? IVP_TRUE : IVP_FALSE;
                unit->sim_unit_just_slowed_down = ur.just_slowed_down ? IVP_TRUE : IVP_FALSE;
#ifdef IVP_PORTABLE_BMMO
                unit->next_movement_check = ur.next_movement_check;
#endif
                mgr->add_sim_unit_to_manager(unit);
                built_units_[static_cast<size_t>(u)] = unit;
                ++counts_.units;
                const bool still = static_cast<int>(ur.movement_type) >= static_cast<int>(IVP_MT_NOT_SIM);
                if (still != (ur.in_still_slot != 0))
                    note("simulation unit " + std::to_string(u) + ": its type and its recorded list disagree");
            }
            for (size_t i = 0; i < world_.objects.size(); ++i) {
                IVP_Core* core = core_of(static_cast<int32_t>(i));
                if (core && !placed.count(core) && core->objects.len() > 0 && core->objects.element_at(0) == object_of(static_cast<int32_t>(i))
                    && !problem("the core of '" + world_.objects[i].name + "' is in no recorded simulation unit"))
                    return false;
            }
            return true;
        }

        // ------------------------------------------------------------------
        // Step 5: the collision checkers, parents before children.
        // ------------------------------------------------------------------
        bool restorer::build_watcher(int32_t i, IVP_Collision_Delegator* delegator) {
            const collision_record& c = world_.collisions[static_cast<size_t>(i)];
            // The constructor creates children with side effects (pushes,
            // revives, events, hull entries, cache slots): the speeds and the
            // revive list are saved and put back, the children deleted; the
            // rest is rebuilt by the later steps.
            struct saved_core {
                IVP_Core* core;
                IVP_U_Float_Point speed_change, rot_speed_change;
            };
            std::vector<saved_core> saved;
            std::set<IVP_Core*> seen;
            for (const live_object& lo : live_.objects) {
                if (!lo.core || lo.core->physical_unmoveable || !seen.insert(lo.core).second) continue;
                saved.push_back(saved_core{lo.core, lo.core->speed_change, lo.core->rot_speed_change});
            }
            // IVP_Synapse_OO::init_synapse_oo inserts each synapse into its
            // object's hull list with the value P_FLOAT_MAX, and
            // IVP_U_Min_List::add takes its search path whenever the value
            // exceeds min_value - which assumes the list is not empty (an
            // empty list's min_value is 1e10, and the first element is then
            // elems[0xFFFF]).  Live, an object's own overlap element is always
            // in its list before any collision is made; here the lists are
            // empty until step 8, so an empty list is told its minimum is
            // P_FLOAT_MAX for the duration, which sends the insert down the
            // front path.  The lists are rebuilt from the record in step 8.
            for (int k = 0; k < 2; ++k) {
                IVP_Real_Object* obj = object_of(c.objects[k]);
                IVP_U_Min_List& list = IVP_BMMO_State::hull_list(*obj->get_hull_manager());
                if (!list.has_elements()) list.min_value = P_FLOAT_MAX;
            }
            IVP_OO_Watcher* w = new IVP_OO_Watcher(delegator, object_of(c.objects[0]), object_of(c.objects[1]));
            {
                IVP_U_FVector<IVP_Collision>& children = IVP_BMMO_State::watcher_children(*w);
                std::vector<IVP_Collision*> made;
                for (int k = 0; k < children.len(); ++k) made.push_back(children.element_at(k));
                for (IVP_Collision* child : made) delete child;   // removes itself from the vector
                if (children.len() != 0) return fail(coll_name(i) + ": the watcher keeps children it made");
            }
            for (const saved_core& s : saved) {
                s.core->speed_change = s.speed_change;
                s.core->rot_speed_change = s.rot_speed_change;
            }
            IVP_U_Vector<IVP_Core>& revive = IVP_BMMO_State::revive_list(*env_);
            for (int k = 0; k < revive.len(); ++k) revive.element_at(k)->is_in_wakeup_vec = IVP_FALSE;
            revive.clear();
            built_collision& b = built_[static_cast<size_t>(i)];
            b.coll = w;
            b.watcher = w;
            return true;
        }

        bool restorer::collisions() {
            set_step(5, "collisions");
            IVP_Environment* env = env_;
            const size_t n = world_.collisions.size();
            built_.assign(n, built_collision());
            IVP_Collision_Delegator* root_delegator = nullptr;
            if (env->collision_delegator_roots.len() >= 1) root_delegator = env->collision_delegator_roots.element_at(0);
            const IVP_Time_CODE code = IVP_BMMO_State::time_code(*env);

            for (size_t i = 0; i < n; ++i) {
                if (!use_coll_[i]) continue;
                const collision_record& c = world_.collisions[i];
                const int32_t self = static_cast<int32_t>(i);
                IVP_Collision_Delegator* delegator = root_delegator;
                built_collision* parent = nullptr;
                if (c.parent >= 0) {
                    parent = &built_[static_cast<size_t>(c.parent)];
                    if (parent->recursive) delegator = parent->recursive;
                    else if (parent->watcher) delegator = parent->watcher;
                    else return fail(coll_name(self) + ": its parent was not built as a delegator");
                } else if (!delegator) {
                    return fail("the environment has no root collision delegator");
                }

                if (c.kind == 2) {
                    if (!build_watcher(self, delegator)) return false;
                } else {
                    IVP_Mindist* md = nullptr;
                    if (c.kind == 1) {
                        IVP_Mindist_Recursive* r = new IVP_Mindist_Recursive(env, delegator);
                        built_[i].recursive = r;
                        md = r;
                    } else {
                        md = new IVP_Mindist(env, delegator);
                    }
                    built_[i].coll = md;
                    built_[i].md = md;
                    for (int k = 0; k < 2; ++k) {
                        IVP_Real_Object* obj = object_of(c.synapse[k].object);
                        const IVP_Compact_Edge* edge = coll_edges_[2 * i + static_cast<size_t>(k)];
                        IVP_BMMO_State::set_synapse(*md->get_synapse(k), md, obj, edge,
                                                    static_cast<IVP_SYNAPSE_POLYGON_STATUS>(c.synapse[k].status));
                        // Balanced by ~IVP_Mindist (polygons only, as init_mindist).
                        if (obj->get_type() == IVP_POLYGON) obj->get_surface_manager()->add_reference_to_ledge(edge->get_compact_ledge());
                    }
                    md->sum_extra_radius = c.sum_extra_radius;
                    md->len_numerator = c.len_numerator;
                    md->contact_dot_diff_center = c.contact_dot_diff_center;
                    md->sum_angular_hull_time = c.sum_angular_hull_time;
                    for (int k = 0; k < 3; ++k) md->contact_plane.k[k] = c.contact_plane[k];
                    md->is_in_phantom_set = IVP_FALSE;
                    md->mindist_function = static_cast<IVP_MINDIST_FUNCTION>(c.function);
                    md->recalc_result = static_cast<IVP_MINIMAL_DIST_RECALC_RESULT>(c.recalc_result);
                    md->disable_halfspace_optimization = c.disable_halfspace ? IVP_TRUE : IVP_FALSE;
                    md->mindist_status = static_cast<IVP_MINIMAL_DIST_STATUS>(c.status);
                    md->coll_dist_selector = c.selector;
                    md->coll_dist_decrease_counter = c.counter;
                    IVP_BMMO_State::set_coll_type(*md, static_cast<IVP_COLL_TYPE>(c.coll_type));
                    IVP_BMMO_State::set_sort_flag(*md, c.sort_flag);
                    // Step 15's rule: fresh against the bumped code, else 0.
                    IVP_BMMO_State::recalc_time_stamp(*md) = c.recalc_fresh ? code : 0;
                    md->last_visited_triangle = nullptr;
                    if (built_[i].recursive) {
                        built_[i].recursive->recursive_status = static_cast<IVP_MINDIST_RECURSIVE_TYPES>(c.recursive_status);
                        built_[i].recursive->spawned_mindist_count = c.spawned_count;
                    }
                }
                ++counts_.collisions;

                if (parent) {
                    // The pointer may have moved: built_ is not resized here.
                    built_collision& p = built_[static_cast<size_t>(c.parent)];
                    if (p.recursive) p.recursive->mindists.add(built_[i].coll);
                    else IVP_BMMO_State::watcher_children(*p.watcher).add(built_[i].coll);
                }
            }

            // Roots into both objects' fvectors, then every fvector in the
            // recorded order.
            for (size_t i = 0; i < n; ++i) {
                const collision_record& c = world_.collisions[i];
                if (!built_[i].coll || c.parent != -1) continue;
                object_of(c.objects[0])->get_ov_element()->add_oo_collision(built_[i].coll);
                object_of(c.objects[1])->get_ov_element()->add_oo_collision(built_[i].coll);
            }
            for (size_t o = 0; o < world_.objects.size(); ++o) {
                IVP_Real_Object* obj = object_of(static_cast<int32_t>(o));
                IVP_OV_Element* elem = obj ? obj->get_ov_element() : nullptr;
                if (!elem) continue;
                std::vector<IVP_Collision*> order;
                for (int32_t c : world_.objects[o].ov_collisions) {
                    if (built_[static_cast<size_t>(c)].coll) order.push_back(built_[static_cast<size_t>(c)].coll);
                    else skip_quietly();
                }
                if (!permute_to(elem->collision_fvector, order))
                    return fail("the collisions of '" + world_.objects[o].name + "' do not match its recorded fvector");
            }
            // Children in the parents' recorded vector order.
            for (size_t i = 0; i < n; ++i) {
                const built_collision& b = built_[i];
                if (!b.recursive && !b.watcher) continue;
                std::vector<IVP_Collision*> order;
                for (int32_t ch : world_.collisions[i].children)
                    if (built_[static_cast<size_t>(ch)].coll) order.push_back(built_[static_cast<size_t>(ch)].coll);
                IVP_U_FVector<IVP_Collision>& children = b.recursive ? b.recursive->mindists : IVP_BMMO_State::watcher_children(*b.watcher);
                if (!permute_to(children, order)) return fail(coll_name(static_cast<int32_t>(i)) + ": its children do not match the record");
            }
            return true;
        }

        // ------------------------------------------------------------------
        // Step 6: the overlap tree, as recorded.
        // ------------------------------------------------------------------
        bool restorer::ov_tree() {
            set_step(6, "overlap tree");
            IVP_OV_Tree_Manager* tree = IVP_BMMO_State::ov_tree(*env_);
            if (IVP_BMMO_State::ov_root(*tree) != nullptr) return fail("the overlap tree is not empty after the teardown");
            IVP_ov_tree_hash* hash = IVP_BMMO_State::ov_hash(*tree);
            if (!hash) return fail("the overlap tree has no hash");
            const size_t n = world_.ov_nodes.size();
            std::vector<IVP_OV_Node*> nodes(n, nullptr);
            for (size_t i = 0; i < n; ++i) {
                const ov_node_record& r = world_.ov_nodes[i];
                IVP_OV_Node* node = new IVP_OV_Node();
                node->data.x = r.x;
                node->data.y = r.y;
                node->data.z = r.z;
                node->data.rasterlevel = r.rasterlevel;
                node->data.sizelevel = r.sizelevel;
                nodes[i] = node;
            }
            for (size_t i = 0; i < n; ++i) {
                const ov_node_record& r = world_.ov_nodes[i];
                nodes[i]->parent = r.parent >= 0 ? nodes[static_cast<size_t>(r.parent)] : nullptr;
                for (int32_t ch : r.children) nodes[i]->children.add(nodes[static_cast<size_t>(ch)]);
            }
            for (size_t i = 0; i < n; ++i) hash->add_node(nodes[i]);
            IVP_BMMO_State::ov_root(*tree) = world_.ov_root >= 0 ? nodes[static_cast<size_t>(world_.ov_root)] : nullptr;

            for (size_t o = 0; o < world_.objects.size(); ++o) {
                const object_record& r = world_.objects[o];
                IVP_Real_Object* obj = object_of(static_cast<int32_t>(o));
                IVP_OV_Element* elem = obj ? obj->get_ov_element() : nullptr;
                if (!elem || !r.has_ov) continue;
                for (int k = 0; k < 3; ++k) elem->center.k[k] = r.ov_center[k];
                elem->radius = r.ov_radius;
                elem->node = nullptr;
            }
            for (size_t i = 0; i < n; ++i) {
                for (int32_t e : world_.ov_nodes[i].elements) {
                    IVP_Real_Object* obj = object_of(e);
                    IVP_OV_Element* elem = obj ? obj->get_ov_element() : nullptr;
                    if (!elem) {
                        skip_quietly();
                        continue;
                    }
                    elem->node = nodes[i];
                    nodes[i]->elements.add(elem);
                }
            }
            // A node that ended up with no element and no child: its only
            // occupants were skipped (a body the blob knows but the world no
            // longer has - an unphysicalized ball).  The engine never keeps
            // such a node (remove_ov_element's cleanup_node deletes upward),
            // and a restore that left one would leave the tree non-empty at
            // the next teardown, which failed that restore midway.  Pruned
            // bottom-up, as cleanup_node would have.
            int pruned = 0;
            for (bool again = true; again;) {
                again = false;
                for (size_t i = 0; i < n; ++i) {
                    IVP_OV_Node* node = nodes[i];
                    if (!node || node->elements.len() != 0 || node->children.len() != 0) continue;
                    if (node == IVP_BMMO_State::ov_root(*tree)) IVP_BMMO_State::ov_root(*tree) = nullptr;
                    hash->remove_node(node);
                    IVP_BMMO_State::delete_ov_node(node);   // removes itself from its parent's children
                    nodes[i] = nullptr;
                    ++pruned;
                    again = true;
                }
            }
            if (pruned > 0) note("overlap tree: " + std::to_string(pruned) + " recorded node(s) held only skipped bodies and were pruned");
            return true;
        }

        // ------------------------------------------------------------------
        // Step 7: the mindist manager's lists and the objects' views.
        // ------------------------------------------------------------------
        namespace {
            void relink_mindists(IVP_Mindist*& head, const std::vector<IVP_Mindist*>& order) {
                head = order.empty() ? nullptr : order.front();
                for (size_t i = 0; i < order.size(); ++i) {
                    order[i]->prev = i > 0 ? order[i - 1] : nullptr;
                    order[i]->next = i + 1 < order.size() ? order[i + 1] : nullptr;
                }
            }
            void relink_synapses(IVP_Synapse_Real*& head, const std::vector<IVP_Synapse_Real*>& order) {
                head = order.empty() ? nullptr : order.front();
                for (size_t i = 0; i < order.size(); ++i) {
                    IVP_BMMO_State::synapse_prev(*order[i]) = i > 0 ? order[i - 1] : nullptr;
                    IVP_BMMO_State::synapse_next(*order[i]) = i + 1 < order.size() ? order[i + 1] : nullptr;
                }
            }
        }

        bool restorer::lists() {
            set_step(7, "mindist lists");
            for (const built_collision& b : built_) {
                if (!b.md) continue;
                b.md->next = nullptr;
                b.md->prev = nullptr;
                for (int k = 0; k < 2; ++k) {
                    IVP_BMMO_State::synapse_next(*b.md->get_synapse(k)) = nullptr;
                    IVP_BMMO_State::synapse_prev(*b.md->get_synapse(k)) = nullptr;
                }
            }
            IVP_Mindist_Manager* mm = IVP_BMMO_State::mindist_manager(*env_);
            std::set<const IVP_Mindist*> in_exact, in_invalid;
            auto manager_list = [&](const std::vector<int32_t>& rec, const std::vector<IVP_Mindist*>& kept,
                                    std::set<const IVP_Mindist*>& placed) {
                std::vector<IVP_Mindist*> order;
                for (int32_t c : rec) {
                    IVP_Mindist* md = built_[static_cast<size_t>(c)].md;
                    if (!md) {
                        skip_quietly();
                        continue;
                    }
                    order.push_back(md);
                    placed.insert(md);
                }
                // Kept unknown bodies' mindists stay listed, after the
                // recorded ones.
                for (IVP_Mindist* md : kept) order.push_back(md);
                return order;
            };
            {
                std::vector<IVP_Mindist*> order = manager_list(world_.exact_list, kept_exact_, in_exact);
                relink_mindists(mm->exact_mindists, order);
                order = manager_list(world_.invalid_list, kept_invalid_, in_invalid);
                relink_mindists(mm->invalid_mindists, order);
            }

            std::set<const IVP_Synapse*> synapse_exact, synapse_invalid;
            for (size_t o = 0; o < world_.objects.size(); ++o) {
                IVP_Real_Object* obj = object_of(static_cast<int32_t>(o));
                if (!obj) continue;
                for (int which = 0; which < 2; ++which) {
                    const std::vector<int32_t>& rec = which == 0 ? world_.objects[o].exact_synapses : world_.objects[o].invalid_synapses;
                    std::set<const IVP_Synapse*>& placed = which == 0 ? synapse_exact : synapse_invalid;
                    std::vector<IVP_Synapse_Real*> order;
                    for (int32_t s : rec) {
                        IVP_Mindist* md = built_[static_cast<size_t>(s >> 1)].md;
                        if (!md) {
                            skip_quietly();
                            continue;
                        }
                        IVP_Synapse_Real* syn = md->get_synapse(s & 1);
                        if (syn->get_object() != obj || !placed.insert(syn).second)
                            return fail("'" + world_.objects[o].name + "' lists a synapse it does not own, or twice");
                        order.push_back(syn);
                    }
                    relink_synapses(which == 0 ? IVP_BMMO_State::exact_synapses(*obj) : IVP_BMMO_State::invalid_synapses(*obj), order);
                }
            }

            // A mindist's status and its list membership must agree, or its
            // destructor unlinks from a list it is not in.
            for (size_t i = 0; i < built_.size(); ++i) {
                IVP_Mindist* md = built_[i].md;
                if (!md) continue;
                const bool exact = md->mindist_status == IVP_MD_EXACT;
                const bool invalid = md->mindist_status == IVP_MD_INVALID;
                if (exact != (in_exact.count(md) != 0) || invalid != (in_invalid.count(md) != 0))
                    return fail(coll_name(static_cast<int32_t>(i)) + ": its status and the manager's lists disagree");
                for (int k = 0; k < 2; ++k) {
                    const IVP_Synapse* syn = md->get_synapse(k);
                    if (exact != (synapse_exact.count(syn) != 0) || invalid != (synapse_invalid.count(syn) != 0))
                        return fail(coll_name(static_cast<int32_t>(i)) + ": its status and its objects' synapse lists disagree");
                }
            }
            return true;
        }

        // ------------------------------------------------------------------
        // Step 8: the hull managers' min-lists, in reverse (ties go newest
        // first, so re-adding backwards reproduces the recorded order).
        // ------------------------------------------------------------------
        bool restorer::hull_lists() {
            set_step(8, "hull lists");
            std::set<const IVP_Listener_Hull*> placed;
            for (size_t o = 0; o < world_.objects.size(); ++o) {
                const object_record& r = world_.objects[o];
                IVP_Real_Object* obj = object_of(static_cast<int32_t>(o));
                if (!obj) continue;
                IVP_Hull_Manager* hm = obj->get_hull_manager();
                IVP_U_Min_List& list = IVP_BMMO_State::hull_list(*hm);
                int guard = 0;
                while (list.has_elements()) {
                    list.remove_minlist_elem(list.first_element);
                    if (++guard > kListGuard) return fail("a hull list does not empty");
                }
                IVP_OV_Element* elem = obj->get_ov_element();
                bool ov_listed = false;
                for (int e = static_cast<int>(r.hull.size()) - 1; e >= 0; --e) {
                    const hull_entry& h = r.hull[static_cast<size_t>(e)];
                    IVP_Listener_Hull* listener = nullptr;
                    if (h.kind == 0) {
                        if (!elem) return fail("'" + r.name + "' has a hull entry for an overlap element it does not have");
                        listener = elem;
                        ov_listed = true;
                    } else {
                        const built_collision& b = built_[static_cast<size_t>(h.collision)];
                        if (b.md) {
                            IVP_Synapse_Real* syn = b.md->get_synapse(h.side);
                            if (syn->get_object() != obj) return fail("'" + r.name + "' lists a hull synapse of another object");
                            listener = syn;
                        } else if (b.watcher) {
                            IVP_Synapse_OO& syn = IVP_BMMO_State::watcher_synapse(*b.watcher, h.side);
                            if (IVP_BMMO_State::watcher_object(syn) != obj)
                                return fail("'" + r.name + "' lists a watcher synapse of another object");
                            listener = &syn;
                        } else {
                            skip_quietly();
                            continue;
                        }
                    }
                    if (!placed.insert(listener).second) return fail("'" + r.name + "' lists a hull listener twice");
                    IVP_BMMO_State::listener_index(*listener) = list.add(static_cast<void*>(listener), h.value);
                    ++counts_.hull_entries;
                }
                if (elem) {
                    if (r.ov_in_hull && !ov_listed) return fail("'" + r.name + "' has its overlap element in no hull entry");
                    elem->hull_manager = r.ov_in_hull ? hm : nullptr;
                    if (!r.ov_in_hull && ov_listed) return fail("'" + r.name + "' lists its overlap element without a hull manager");
                }
            }
            // A hull mindist or a watcher whose synapses are in no list would
            // remove a stale index when it is deleted.
            for (size_t i = 0; i < built_.size(); ++i) {
                const built_collision& b = built_[i];
                for (int k = 0; k < 2; ++k) {
                    if (b.md) {
                        const bool hull = b.md->mindist_status == IVP_MD_HULL || b.md->mindist_status == IVP_MD_HULL_RECURSIVE;
                        if (hull != (placed.count(b.md->get_synapse(k)) != 0))
                            return fail(coll_name(static_cast<int32_t>(i)) + ": its status and the hull lists disagree");
                    } else if (b.watcher && !placed.count(&IVP_BMMO_State::watcher_synapse(*b.watcher, k))) {
                        return fail(coll_name(static_cast<int32_t>(i)) + ": a watcher synapse is in no hull list");
                    }
                }
            }
            return true;
        }

        // ------------------------------------------------------------------
        // Step 9: the event queue, in reverse.
        // ------------------------------------------------------------------
        bool restorer::events() {
            set_step(9, "event queue");
            IVP_Time_Manager* tm = IVP_BMMO_State::time_manager(*env_);
            IVP_U_Min_List& list = *tm->min_hash;
            const void* psi = static_cast<const void*>(static_cast<IVP_Time_Event*>(tm->psi_event));
            {
                std::vector<unsigned int> remove;
                IVP_U_Min_List_Element* elems = IVP_BMMO_State::elements(list);
                int guard = 0;
                const unsigned int unused = static_cast<unsigned int>(IVP_U_MINLIST_UNUSED);
                for (unsigned int idx = list.first_element; idx != unused; idx = elems[idx].next) {
                    const void* e = elems[idx].element;
                    if (e != psi && kept_.count(e)) continue;   // a kept unknown body's mindist
                    if (e != psi) return fail("the event queue holds an event the savestate does not know");
                    remove.push_back(idx);
                    if (++guard > kListGuard) return fail("the event queue is a cycle");
                }
                for (unsigned int idx : remove) list.remove_minlist_elem(idx);
                tm->psi_event->index = IVP_U_MINLIST_UNUSED;
            }
            for (int k = static_cast<int>(world_.events.size()) - 1; k >= 0; --k) {
                const event_record& ev = world_.events[static_cast<size_t>(k)];
                if (ev.collision < 0) {
                    IVP_Time_Event* event = tm->psi_event;
                    event->index = static_cast<int>(list.add(static_cast<void*>(event), ev.value));
                } else {
                    IVP_Mindist* md = built_[static_cast<size_t>(ev.collision)].md;
                    if (!md) {
                        skip_quietly();
                        continue;
                    }
                    IVP_Time_Event* event = md;
                    md->index = static_cast<int>(list.add(static_cast<void*>(event), ev.value));
                }
                ++counts_.events;
            }
            return true;
        }

        // ------------------------------------------------------------------
        // Step 10: friction systems, contact points, pairs, infos.
        // ------------------------------------------------------------------
        bool restorer::friction() {
            set_step(10, "friction");
            IVP_Environment* env = env_;
            built_systems_.assign(world_.systems.size(), nullptr);
            built_contacts_.assign(world_.contacts.size(), nullptr);
            null_delegator null_del;
            for (size_t s = 0; s < world_.systems.size(); ++s) {
                if (!use_system_[s]) continue;
                const friction_system_record& sr = world_.systems[s];
                IVP_Friction_System* fs = new IVP_Friction_System(env);
                built_systems_[s] = fs;
                ++counts_.systems;
                // Movable cores get the three handles as controllers (the
                // order is fixed in step 12).
                for (int32_t o : sr.cores) fs->add_core_to_system(core_of(o));
                std::vector<IVP_Friction_Info_For_Core*> infos;
                for (const friction_info_record& ir : sr.infos) {
                    IVP_Core* core = core_of(ir.object);
                    if (!core->physical_unmoveable && core->core_friction_info.for_moveables.moveable_core_friction_info)
                        return fail("the core of '" + name_of(ir.object) + "' has two friction infos");
                    IVP_Friction_Info_For_Core* info = new IVP_Friction_Info_For_Core();
                    info->l_friction_system = fs;
                    core->add_friction_info(info);
                    infos.push_back(info);
                }

                std::vector<IVP_Contact_Point*> dists;
                for (int32_t ci : sr.dists) {
                    const contact_record& cr = world_.contacts[static_cast<size_t>(ci)];
                    // The contact point's constructor reads the mindist's
                    // sorted synapses: sort flag 0, so synapse[0], synapse[1].
                    IVP_Mindist* dummy = new IVP_Mindist(env, &null_del);
                    for (int k = 0; k < 2; ++k) {
                        IVP_Real_Object* obj = object_of(cr.synapse[k].object);
                        const IVP_Compact_Edge* edge = contact_edges_[2 * static_cast<size_t>(ci) + static_cast<size_t>(k)];
                        IVP_BMMO_State::set_synapse(*dummy->get_synapse(k), dummy, obj, edge,
                                                    static_cast<IVP_SYNAPSE_POLYGON_STATUS>(cr.synapse[k].status));
                        // The dummy's destructor removes one reference per
                        // polygon synapse.
                        if (obj->get_type() == IVP_POLYGON) obj->get_surface_manager()->add_reference_to_ledge(edge->get_compact_ledge());
                    }
                    IVP_Contact_Point* cp = new IVP_Contact_Point(dummy);
                    delete dummy;
                    IVP_BMMO_State::cp_gap(*cp) = cr.gap;
                    IVP_BMMO_State::cp_span(*cp)[0] = cr.span[0];
                    IVP_BMMO_State::cp_span(*cp)[1] = cr.span[1];
                    IVP_BMMO_State::cp_pressure(*cp) = cr.pressure;
                    IVP_BMMO_State::cp_friction_factor(*cp) = cr.friction_factor;
                    IVP_BMMO_State::cp_destroyed_energy(*cp) = cr.destroyed_energy;
                    IVP_BMMO_State::cp_inv_triangle_det(*cp) = cr.inv_triangle_det;
                    IVP_BMMO_State::cp_old_energy(*cp) = cr.old_energy;
                    IVP_BMMO_State::cp_inv_virt_mass(*cp) = cr.inv_virt_mass;
                    IVP_BMMO_State::cp_pull(*cp) = cr.pull;
                    IVP_BMMO_State::cp_keeper(*cp) = cr.keeper;
                    IVP_BMMO_State::set_cp_status(*cp, static_cast<IVP_CONTACT_POINT_BREAK_STATUS>(cr.status));
                    IVP_BMMO_State::set_cp_two_friction(*cp, cr.two_friction != 0);
                    IVP_BMMO_State::cp_recalc_time(*cp) = IVP_Time(cr.recalc_time);
                    for (int k = 0; k < 3; ++k) {
                        IVP_BMMO_State::cp_contact_ws(*cp).k[k] = cr.contact_ws[k];
                        IVP_BMMO_State::cp_normal_ws(*cp).k[k] = cr.normal_ws[k];
                    }
                    // Transaction memory: re-set by recalc_friction_s_vals,
                    // which the next PSI's energy controller runs first.
                    IVP_BMMO_State::cp_tmp_info(*cp) = nullptr;
                    IVP_BMMO_State::cp_system(*cp) = fs;
                    built_contacts_[static_cast<size_t>(ci)] = cp;
                    dists.push_back(cp);
                    ++counts_.contacts;
                }
                // The system's intrusive list, in the recorded order.
                IVP_BMMO_State::fs_first(*fs) = dists.empty() ? nullptr : dists.front();
                for (size_t k = 0; k < dists.size(); ++k) {
                    IVP_BMMO_State::cp_prev(*dists[k]) = k > 0 ? dists[k - 1] : nullptr;
                    IVP_BMMO_State::cp_next(*dists[k]) = k + 1 < dists.size() ? dists[k + 1] : nullptr;
                }
                IVP_BMMO_State::fs_dist_number(*fs) = sr.dist_number;
                IVP_BMMO_State::fs_obj_number(*fs) = sr.obj_number;
                IVP_BMMO_State::fs_complex_not_necessary(*fs) = sr.complex_not_necessary;
                fs->union_find_necessary = sr.union_find_necessary ? IVP_TRUE : IVP_FALSE;
                fs->fr_sys_simulated = sr.simulated ? IVP_TRUE : IVP_FALSE;
                if (static_cast<size_t>(sr.obj_number) != sr.cores.size())
                    note("friction system " + std::to_string(s) + " counts " + std::to_string(sr.obj_number) + " cores but lists "
                         + std::to_string(sr.cores.size()));

                // Pairs without add_fr_pair (it fires an event).
                for (const pair_record& pr : sr.pairs) {
                    IVP_Friction_Core_Pair* pair = new IVP_Friction_Core_Pair();
                    pair->objs[0] = core_of(pr.objects[0]);
                    pair->objs[1] = core_of(pr.objects[1]);
                    pair->next_ease_nr_psi = pr.next_ease;
                    pair->last_impact_time_pair = IVP_Time(pr.last_impact_time);
                    pair->integrated_anti_energy = pr.anti_energy;
                    for (int32_t d : pr.dists) IVP_BMMO_State::pair_dists(*pair).add(built_contacts_[static_cast<size_t>(d)]);
                    fs->fr_pairs_of_objs.add(pair);
                }
                for (size_t k = 0; k < infos.size(); ++k)
                    for (int32_t d : sr.infos[k].springs) infos[k]->friction_springs.add(built_contacts_[static_cast<size_t>(d)]);

                // add_core_to_system appended in the record's core order;
                // the movable vector is permuted to its own recorded order.
                std::vector<IVP_Core*> movable;
                for (int32_t o : sr.movable_cores) movable.push_back(core_of(o));
                if (!permute_to(fs->moveable_cores_of_friction_system, movable))
                    return fail("friction system " + std::to_string(s) + ": its movable cores do not match the record");
            }

            // Every object's friction synapse list, in the recorded order.
            std::set<const IVP_Synapse_Friction*> placed;
            for (IVP_Contact_Point* cp : built_contacts_) {
                if (!cp) continue;
                for (int k = 0; k < 2; ++k) {
                    IVP_BMMO_State::friction_next(IVP_BMMO_State::cp_synapse(*cp, k)) = nullptr;
                    IVP_BMMO_State::friction_prev(IVP_BMMO_State::cp_synapse(*cp, k)) = nullptr;
                }
            }
            for (size_t o = 0; o < world_.objects.size(); ++o) {
                IVP_Real_Object* obj = object_of(static_cast<int32_t>(o));
                if (!obj) continue;
                std::vector<IVP_Synapse_Friction*> order;
                for (int32_t s : world_.objects[o].friction_synapses) {
                    IVP_Contact_Point* cp = built_contacts_[static_cast<size_t>(s >> 1)];
                    if (!cp) {
                        skip_quietly();
                        continue;
                    }
                    IVP_Synapse_Friction* syn = &IVP_BMMO_State::cp_synapse(*cp, s & 1);
                    if (syn->get_object() != obj || !placed.insert(syn).second)
                        return fail("'" + world_.objects[o].name + "' lists a friction synapse it does not own, or twice");
                    order.push_back(syn);
                }
                IVP_BMMO_State::friction_synapses(*obj) = order.empty() ? nullptr : order.front();
                for (size_t k = 0; k < order.size(); ++k) {
                    IVP_BMMO_State::friction_prev(*order[k]) = k > 0 ? order[k - 1] : nullptr;
                    IVP_BMMO_State::friction_next(*order[k]) = k + 1 < order.size() ? order[k + 1] : nullptr;
                }
            }
            for (size_t c = 0; c < built_contacts_.size(); ++c) {
                IVP_Contact_Point* cp = built_contacts_[c];
                if (!cp) continue;
                for (int k = 0; k < 2; ++k)
                    if (!placed.count(&IVP_BMMO_State::cp_synapse(*cp, k)))
                        return fail("contact " + std::to_string(c) + ": a synapse is in no object's friction list");
            }
            return true;
        }

        // ------------------------------------------------------------------
        // Step 12: controller orders.
        // ------------------------------------------------------------------
        IVP_Controller* restorer::friction_handle(int32_t system, uint8_t part) const {
            if (!in(system, built_systems_.size())) return nullptr;
            IVP_Friction_System* fs = built_systems_[static_cast<size_t>(system)];
            if (!fs) return nullptr;
            switch (part) {
            case 0: return &IVP_BMMO_State::fs_static_handle(*fs);
            case 1: return fs;
            case 2: return &IVP_BMMO_State::fs_energy_handle(*fs);
            default: return nullptr;
            }
        }

        template <class E>
        bool restorer::order_controllers(IVP_U_Vector<E>& vec, IVP_Controller* (*controller_of)(E*),
                                         const std::vector<controller_ref>& refs, const std::string& owner) {
            const int n = vec.len();
            std::vector<E*> live;
            for (int i = 0; i < n; ++i) live.push_back(vec.element_at(i));
            std::vector<uint8_t> used(static_cast<size_t>(n), 0);
            std::vector<E*> target;
            // Under the name the blob records it by (set_controller_alias).
            const controller_alias* alias = controller_alias_for(physics_);
            auto name_of_controller = [alias](IVP_Controller* c) {
                return recorded_controller_name(alias, c ? c->get_controller_name() : nullptr);
            };
            for (const controller_ref& ref : refs) {
                int found = -1;
                if (ref.kind == 0) {
                    IVP_Controller* want = friction_handle(ref.system, ref.part);
                    for (int i = 0; want && i < n && found < 0; ++i)
                        if (!used[static_cast<size_t>(i)] && controller_of(live[static_cast<size_t>(i)]) == want) found = i;
                    if (found < 0) {
                        if (!want) skip_quietly();   // its system was skipped
                        else if (!problem(owner + ": friction handle " + std::to_string(ref.part) + " of system "
                                          + std::to_string(ref.system) + " is not among its controllers"))
                            return false;
                        continue;
                    }
                } else {
                    for (int i = 0; i < n && found < 0; ++i) {
                        if (used[static_cast<size_t>(i)]) continue;
                        IVP_Controller* c = controller_of(live[static_cast<size_t>(i)]);
                        if (!c || friction_handles_.count(c)) continue;
                        if (static_cast<int32_t>(c->get_controller_priority()) == ref.priority && name_of_controller(c) == ref.name)
                            found = i;
                    }
                    if (found < 0) {
                        if (!problem(owner + ": no live controller '" + ref.name + "' (priority " + std::to_string(ref.priority) + ")"))
                            return false;
                        continue;
                    }
                }
                used[static_cast<size_t>(found)] = 1;
                target.push_back(live[static_cast<size_t>(found)]);
            }
            for (int i = 0; i < n; ++i) {
                if (used[static_cast<size_t>(i)]) continue;
                IVP_Controller* c = controller_of(live[static_cast<size_t>(i)]);
                const std::string line = owner + ": live controller '" + name_of_controller(c) + "' (priority "
                    + std::to_string(c ? static_cast<int>(c->get_controller_priority()) : -1) + ") is not in the savestate";
                if (strict_) return fail(line);
                note(line);
                target.push_back(live[static_cast<size_t>(i)]);   // stays at the end, in live order
            }
            if (!permute_to(vec, target)) return fail(owner + ": the controller permutation failed");
            return true;
        }

        bool restorer::controllers() {
            set_step(12, "controllers");
            friction_handles_.clear();
            for (size_t s = 0; s < built_systems_.size(); ++s) {
                if (!built_systems_[s]) continue;
                for (uint8_t part = 0; part < 3; ++part)
                    friction_handles_[friction_handle(static_cast<int32_t>(s), part)] = std::make_pair(static_cast<int32_t>(s), part);
            }
            std::set<IVP_Core*> done;
            for (size_t o = 0; o < world_.objects.size(); ++o) {
                const object_record& r = world_.objects[o];
                IVP_Core* core = core_of(static_cast<int32_t>(o));
                if (!core || !r.movable || core->physical_unmoveable || !done.insert(core).second) continue;
                if (!order_controllers(core->controllers_of_core, &controller_identity, r.core.controllers, "core of '" + r.name + "'"))
                    return false;
            }
            for (size_t u = 0; u < built_units_.size(); ++u) {
                IVP_Simulation_Unit* unit = built_units_[u];
                if (!unit) continue;
                const std::string owner = "simulation unit " + std::to_string(u);
                if (!order_controllers(unit->controller_cores, &controller_of_entry, world_.units[u].controllers, owner))
                    return false;
                // sim_unit_sort_controllers asserts this in debug builds.
                for (int i = 0; i + 1 < unit->controller_cores.len(); ++i) {
                    IVP_Controller* a = unit->controller_cores.element_at(i)->l_controller;
                    IVP_Controller* b = unit->controller_cores.element_at(i + 1)->l_controller;
                    if (a && b && a->get_controller_priority() > b->get_controller_priority()) {
                        note(owner + ": its controllers are out of priority order after the restore");
                        break;
                    }
                }
            }
            return true;
        }

        // ------------------------------------------------------------------
        // Step 13: the revive list, through the engine's own add.
        // ------------------------------------------------------------------
        bool restorer::revive() {
            set_step(13, "revive list");
            for (int32_t o : world_.revive_list) {
                IVP_Core* core = core_of(o);
                if (!core) {
                    skip_quietly();
                    continue;
                }
                core->ensure_core_in_simulation_delayed();
            }
            for (IVP_Core* core : unknown_revive_) core->ensure_core_in_simulation_delayed();
            return true;
        }

        // ------------------------------------------------------------------
        // Step 13b: the bodies the blob does not know (born after the anchor,
        // or a sector physicalized inside the window).  The teardown took
        // them out of the overlap tree and their hull lists like every other
        // body; the rebuild filed only the recorded ones, so they are out of
        // collision detection now - no tree node, no hull entry - and nothing
        // in the engine puts such a body back on its own (it re-inserts an
        // object only on a hull event, and there is none without an entry).
        // That is left to the caller, by name: a body it will re-pose at its
        // birth tick stays inert until then (the live run had no such body
        // before that tick, and a recheck here would also file its
        // collisions ahead of the ones the live run made first), a body it
        // has no record of is rechecked back in at once.  Under
        // SAVESTATE_KEEP_UNKNOWN they keep their hull entry and their kept
        // collisions instead, and are not reported.
        bool restorer::unknowns() {
            set_step(13, "unknown bodies");
            if (keep_unknown_ || !report_) return true;
            for (size_t li = 0; li < live_.objects.size(); ++li) {
                if (!unknown_[li] || !live_.objects[li].object) continue;
                report_->unknown_bodies.push_back(live_.objects[li].name);
            }
            return true;
        }

        // ------------------------------------------------------------------
        // Step 14: the physics_RT side.
        // ------------------------------------------------------------------
        bool restorer::physics_rt() {
            set_step(14, "physics_RT");
            // A body the record does not know keeps its live membership, after
            // the recorded entries (where a body born after the anchor sat in
            // the live run).  Membership is how CKIpionManager::Simulate
            // writes a body's pose back to its CK entity; a body dropped here
            // would keep rolling in IVP while its entity - what the scripts
            // and their sensors see - stayed where the anchor left it
            // (measured: a ball born inside the window that never woke the
            // mechanism its proximity sensor should have, on every later
            // rewind, for the rest of the session).  Only a revive event
            // re-adds a body, and a rolling ball never has one.
            std::vector<IVP_Real_Object*> kept_movable;
            for (int i = 0; i < physics_->m_MovableObjects.len(); ++i) {
                IVP_Real_Object* obj = physics_->m_MovableObjects.element_at(i);
                if (obj && is_unknown(obj)) kept_movable.push_back(obj);
            }
            physics_->m_MovableObjects.clear();
            for (int32_t o : world_.movable_objects) {
                IVP_Real_Object* obj = object_of(o);
                if (!obj) {
                    skip_quietly();
                    continue;
                }
                physics_->m_MovableObjects.add(obj);
            }
            for (IVP_Real_Object* obj : kept_movable) physics_->m_MovableObjects.add(obj);
            for (size_t o = 0; o < world_.objects.size(); ++o) {
                const object_record& r = world_.objects[o];
                const live_object* lo = live_object_of(static_cast<int32_t>(o));
                if (!lo || !r.has_entity_matrix || !lo->entity) continue;
                // The capture memcpy's the VxMatrix (16 floats, row by row).
                VxMatrix m = lo->entity->GetWorldMatrix();
                static_assert(sizeof(VxMatrix) == sizeof(r.entity_matrix), "VxMatrix is 16 floats");
                // Gap P (rollback engine gaps, roadmap 4.4, step B8EG item
                // EGP, option i): a world already bit-identical to the record
                // is not written.  For a parented body SetWorldMatrix
                // re-derives the local matrix from the world and the parent's
                // world, with rounding even when the world written is the one
                // it has (P_Modul_25_Bridge001: local y -0x1.f55394p-3 ->
                // -0x1.f554p-3); a replay that then moves the parent left the
                // sleeping child's CK matrix ulps from a world that never
                // rolled back (lockstep-without-snapshots.md, As built RO2).
                // The write would also re-derive the worlds of the entity's
                // children from their locals (KeepChildren false) and mark
                // the entity moved for CK; skipped, the entity and its
                // children stay as the live world has them, and the live
                // frame never made that write either.  Measured on RO2's cases (L8 K=3, L11 K=3): every body
                // bit-identical to the control at 301/301 boundaries.
                if (std::memcmp(&m, r.entity_matrix, sizeof(r.entity_matrix)) == 0) continue;
                std::memcpy(&m, r.entity_matrix, sizeof(r.entity_matrix));
                // As CKIpionManager writes bodies back after a step.
                lo->entity->SetWorldMatrix(m);
            }

            guard_.release();
            return true;
        }

        // Before the teardown: the PreSimulate queue cannot be rebuilt (a
        // retail callback is owned by its block), so it is compared, and a
        // difference fails closed even in lenient mode - here, while the
        // world is still untouched, so a refusal costs nothing.  Excluded on
        // both sides: entries without a behavior, and BallanceMMO's own
        // callbacks - identified by type on the live side, and by the
        // behaviors those live callbacks use on the recorded side (the capture
        // records the queue as it is).  A queued retail callback is a block
        // whose Execute returned 0 and is retried every frame (a Physics Force
        // whose target has no body yet, say): the message names the block
        // and its script, since the id alone says nothing.
        bool restorer::check_callbacks() {
            set_step(0, "PreSimulate queue");
            // A travelling blob names the blocks by the capturing process's
            // CK ids, and the queue is never rebuilt - the live world keeps
            // its own (savestate_travel): only a length difference is noted.
            if (travel_) {
                size_t live = 0;
                if (PhysicsCallbackContainer* queue = physics_->m_PreSimulateCallbacks)
                    for (int slot = 0; slot < 3; ++slot)
                        for (int i = 0; i < queue->m_Callbacks[slot].len(); ++i) {
                            PhysicsCallback* cb = queue->m_Callbacks[slot].element_at(i);
                            if (cb && cb->m_BehaviorID != 0 && !is_bmmo_callback(cb)) ++live;
                        }
                size_t recorded = 0;
                for (const callback_record& cb : world_.callbacks)
                    if (cb.behavior != 0) ++recorded;
                if (live != recorded)
                    note("travelling savestate: the PreSimulate queue has " + std::to_string(live) + " retail entries here, "
                         + std::to_string(recorded) + " (BallanceMMO's included) in the capturing world; the live queue is kept");
                return true;
            }
            // Gap Q (bridge API v23, callback_scope_set): with a scope, only
            // the entries of the scripts the rollback restored are compared -
            // the entries of every other script are live state, held out of
            // the replay (rollback-engine-gaps design 2.2.2-2.2.4).  A
            // recorded entry whose block is gone is compared (fail-closed).
            const std::set<uint32_t>* scope = callback_scope_for(physics_);
            CKContext* scope_context = physics_->m_Context;
            const auto in_scope = [scope, scope_context](uint32_t behavior, bool live_side) {
                if (!scope) return true;
                CKBehavior* block = scope_context ? CKBehavior::Cast(scope_context->GetObject(static_cast<CK_ID>(behavior))) : nullptr;
                return queue_plan::compared(scope, block != nullptr, block ? callback_owner_script(block) : 0, live_side);
            };
            std::vector<callback_record> live_queue;
            std::set<uint32_t> bmmo_behaviors;
            if (PhysicsCallbackContainer* queue = physics_->m_PreSimulateCallbacks) {
                for (int slot = 0; slot < 3; ++slot) {
                    for (int i = 0; i < queue->m_Callbacks[slot].len(); ++i) {
                        PhysicsCallback* cb = queue->m_Callbacks[slot].element_at(i);
                        if (!cb || cb->m_BehaviorID == 0) continue;
                        if (is_bmmo_callback(cb)) {
                            bmmo_behaviors.insert(static_cast<uint32_t>(cb->m_BehaviorID));
                            continue;
                        }
                        if (!in_scope(static_cast<uint32_t>(cb->m_BehaviorID), true)) continue;
                        callback_record rec;
                        rec.slot = slot;
                        rec.behavior = static_cast<uint32_t>(cb->m_BehaviorID);
                        rec.type = static_cast<int32_t>(cb->m_Type);
                        live_queue.push_back(rec);
                    }
                }
            }
            std::vector<callback_record> recorded;
            for (const callback_record& cb : world_.callbacks) {
                if (cb.behavior == 0 || bmmo_behaviors.count(cb.behavior)) continue;
                if (!in_scope(cb.behavior, false)) continue;
                recorded.push_back(cb);
            }
            bool same = live_queue.size() == recorded.size();
            for (size_t i = 0; same && i < recorded.size(); ++i)
                same = live_queue[i].slot == recorded[i].slot && live_queue[i].behavior == recorded[i].behavior
                    && live_queue[i].type == recorded[i].type;
            if (!same) {
                CKContext* context = physics_->m_Context;
                auto text = [context](const std::vector<callback_record>& v) {
                    std::string out;
                    for (const callback_record& cb : v) {
                        out += (out.empty() ? "" : " ") + std::to_string(cb.slot) + ":" + std::to_string(cb.behavior) + "/"
                            + std::to_string(cb.type);
                        CKBehavior* block = context ? CKBehavior::Cast(context->GetObject(static_cast<CK_ID>(cb.behavior))) : nullptr;
                        if (!block) continue;
                        CKBehavior* owner = block->GetOwnerScript();
                        out += std::string(" (") + (block->GetName() ? block->GetName() : "?") + " in "
                            + (owner && owner->GetName() ? owner->GetName() : "?") + ")";
                    }
                    return out.empty() ? std::string("(none)") : out;
                };
                return fail("the PreSimulate queue differs from the savestate's: live " + text(live_queue) + ", recorded "
                            + text(recorded));
            }
            return true;
        }

        // ------------------------------------------------------------------
        // Step 15: the object cache ring, verbatim, freshness resolved
        // against the bumped code.
        // ------------------------------------------------------------------
        bool restorer::caches() {
            set_step(15, "object caches");
            IVP_Cache_Object_Manager* mgr = IVP_BMMO_State::cache_manager(*env_);
            if (!mgr) return fail("the environment has no cache manager");
            const int count = IVP_BMMO_State::cache_count(*mgr);
            if (count != static_cast<int>(world_.cache_slots.size()))
                return fail("the cache ring holds " + std::to_string(count) + " slots, the savestate "
                            + std::to_string(world_.cache_slots.size()));
            for (const live_object& lo : live_.objects)
                if (lo.object) IVP_BMMO_State::cache_object(*lo.object) = nullptr;
            const IVP_Time_CODE code = IVP_BMMO_State::time_code(*env_);
            for (int i = 0; i < count; ++i) {
                IVP_Cache_Object* slot = mgr->cache_object_at(i);
                const cache_slot_record& r = world_.cache_slots[static_cast<size_t>(i)];
                IVP_Real_Object* obj = r.object >= 0 ? object_of(r.object) : nullptr;
                IVP_BMMO_State::cache_reference_count(*slot) = 0;
                // The contents verbatim for every slot, a free one included:
                // a slot the live world used after the anchor keeps its
                // matrices otherwise, and the capture records them as they
                // are.
                slot->q_world_f_object.x = r.q[0];
                slot->q_world_f_object.y = r.q[1];
                slot->q_world_f_object.z = r.q[2];
                slot->q_world_f_object.w = r.q[3];
                for (int row = 0; row < 3; ++row)
                    for (int col = 0; col < 3; ++col) slot->m_world_f_object.set_elem(row, col, r.m[row * 3 + col]);
                for (int k = 0; k < 3; ++k) {
                    slot->m_world_f_object.vv.k[k] = r.m[9 + k];
                    slot->core_pos.k[k] = r.core_pos[k];
                }
                if (!obj) {
                    if (r.object >= 0) skip_quietly();
                    IVP_BMMO_State::cache_owner(*slot) = nullptr;
                    IVP_BMMO_State::cache_valid_until(*slot) = 0;
                    continue;
                }
                IVP_BMMO_State::cache_owner(*slot) = obj;
                IVP_BMMO_State::cache_object(*obj) = slot;
                IVP_BMMO_State::cache_valid_until(*slot) = r.fresh ? code : 0;
            }
            if (count > 0) IVP_BMMO_State::cache_reuse_index(*mgr) = world_.cache_reuse_index;
            return true;
        }

        // ------------------------------------------------------------------
        bool restorer::run() {
            set_step(0, "preconditions");
            if (!physics_) return fail("no physics manager");
            {
                std::string index_error;
                if (!index_live_world(physics_, live_, index_error)) return fail(index_error);
            }
            env_ = live_.env;
            if (!env_) return fail("physics environment is unavailable");
            // A travelling blob is another process's by definition (resync
            // step A3): its identity is not this environment's.
            if (!travel_ && world_.environment_id != static_cast<uint64_t>(reinterpret_cast<uintptr_t>(env_)))
                return fail("the savestate was captured in another environment");
            if (!IVP_BMMO_State::time_manager(*env_) || !IVP_BMMO_State::time_manager(*env_)->min_hash
                || !IVP_BMMO_State::time_manager(*env_)->psi_event || !IVP_BMMO_State::ov_tree(*env_)
                || !IVP_BMMO_State::mindist_manager(*env_) || !IVP_BMMO_State::units_manager(*env_)
                || !IVP_BMMO_State::cache_manager(*env_))
                return fail("the environment lacks a manager");
            if (env_->collision_delegator_roots.len() < 1 && !world_.collisions.empty())
                return fail("the environment has no root collision delegator");
            if (!validate() || !map_objects() || !plan() || !check_callbacks()) return false;

            set_step(0, "mute");
            guard_.engage(env_, live_);
            if (rebuild()) return true;
            // A failure past the teardown leaves a world the engine cannot
            // run as it is: salvage() puts back what every later tick needs
            // (the PSI event, a unit per core, every body back in collision
            // detection).  The caller still sees the failure and falls back.
            if (damaged_) salvage();
            return false;
        }

        bool restorer::rebuild() {
            if (!clock()) return false;
            if (!teardown()) return false;
            if (!cores_pass(true)) return false;
            if (!units()) return false;
            if (!collisions()) return false;
            if (!ov_tree()) return false;
            if (!lists()) return false;
            if (!hull_lists()) return false;
            if (!events()) return false;
            if (!friction()) return false;
            if (!cores_pass(false)) return false;
            if (!controllers()) return false;
            if (!revive()) return false;
            if (!unknowns()) return false;
            if (!physics_rt()) return false;
            if (!caches()) return false;
            return true;
        }

        // After a failure that followed the teardown.  Best effort, through
        // the engine's own entry points, so the world keeps simulating and the
        // per-body fallback has something consistent to write into:
        //  - the PSI event is queued (a teardown that reached step 9 may have
        //    removed it; without it no PSI ever runs again);
        //  - every movable core sits in a simulation unit;
        //  - every object whose overlap element is out of the tree is
        //    rechecked back in (tree node, hull entry, partners and their
        //    collision checkers all re-created by IVP_Mindist_Manager);
        //  - the object caches are dropped.
        // What it cannot give back is exactness: the collision state is
        // whatever IVP builds fresh, and friction contacts return only on the
        // next impact.
        void restorer::salvage() {
            IVP_Environment& env = *env_;
            std::string did;
            IVP_Time_Manager* tm = IVP_BMMO_State::time_manager(env);
            if (tm && tm->psi_event && tm->min_hash && tm->psi_event->index == static_cast<int>(IVP_U_MINLIST_UNUSED)) {
                IVP_Time_Event* event = tm->psi_event;
                const double relative = env.get_next_PSI_time().get_seconds() - tm->base_time.get_seconds();
                event->index = static_cast<int>(tm->min_hash->add(static_cast<void*>(event),
                                                                  static_cast<IVP_U_MINLIST_FIXED_POINT>(relative)));
                did += " psi-event";
            }
            if (IVP_Sim_Units_Manager* mgr = IVP_BMMO_State::units_manager(env)) {
                int made = 0;
                for (const live_object& lo : live_.objects) {
                    IVP_Core* core = lo.core;
                    if (!core || core->physical_unmoveable || core->sim_unit_of_core) continue;
                    IVP_Simulation_Unit* unit = new IVP_Simulation_Unit();
                    unit->add_sim_unit_core(core);
                    core->sim_unit_of_core = unit;
                    unit->clean_sim_unit();
                    unit->sim_unit_calc_redundants();
                    IVP_BMMO_State::set_unit_type(*unit, core->movement_state);
                    mgr->add_sim_unit_to_manager(unit);
                    ++made;
                }
                if (made) did += " " + std::to_string(made) + " unit(s)";
            }
            if (IVP_Mindist_Manager* mm = IVP_BMMO_State::mindist_manager(env)) {
                int rechecked = 0;
                for (const live_object& lo : live_.objects) {
                    IVP_OV_Element* elem = lo.object ? lo.object->get_ov_element() : nullptr;
                    if (!elem || elem->node) continue;
                    mm->recheck_ov_element(lo.object);
                    ++rechecked;
                }
                if (rechecked) did += " " + std::to_string(rechecked) + " body(ies) rechecked into collision detection";
            }
            for (const live_object& lo : live_.objects)
                if (lo.object) IVP_Cache_Object_Manager::invalid_cache_object(lo.object);
            error_ += did.empty() ? "; salvaged (nothing to repair)" : "; salvaged:" + did;
        }

        void restorer::fill_report() const {
            if (!report_) return;
            report_->objects = counts_.objects;
            report_->cores = counts_.cores;
            report_->collisions = counts_.collisions;
            report_->contacts = counts_.contacts;
            report_->systems = counts_.systems;
            report_->units = counts_.units;
            report_->events = counts_.events;
            report_->hull_entries = counts_.hull_entries;
            report_->skipped = counts_.skipped;
            report_->excluded = excluded_;
            report_->renamed = renamed_;
        }
    }

    bool restore_world(CKIpionManager* physics, const world_record& world, uint32_t flags, std::string& error,
                       savestate_report* report, const savestate_travel* travel) {
        error.clear();
        if (report) *report = savestate_report{};
        bool ok = false;
        {
            // The guard inside puts listeners, flags and the event log back
            // when the restorer goes, on every path.
            restorer r(physics, world, flags, error, report, travel);
            ok = r.run();
            r.fill_report();
        }
        return ok;
    }
}

namespace bmmo::physics {

    bool set_savestate(CKIpionManager* physics, const uint8_t* data, size_t size, uint32_t flags, std::string& error,
                       savestate_report* report) {
        error.clear();
        if (report) *report = savestate_report{};
        if (!physics) {
            error = "no physics manager";
            return false;
        }
        if (!data || size == 0) {
            error = "empty savestate";
            return false;
        }
        savestate::reader r(data, size);
        savestate::world_record world;
        if (!savestate::read_world(r, world, error)) {
            if (error.empty()) error = "the savestate does not parse";
            return false;
        }
        return savestate::restore_world(physics, world, flags, error, report);
    }

    bool set_savestate(CKIpionManager* physics, const uint8_t* data, size_t size, uint32_t flags, std::string& error,
                       savestate_report* report, const savestate_travel& travel) {
        error.clear();
        if (report) *report = savestate_report{};
        if (!physics) {
            error = "no physics manager";
            return false;
        }
        if (!data || size == 0) {
            error = "empty savestate";
            return false;
        }
        savestate::reader r(data, size);
        savestate::world_record world;
        if (!savestate::read_world(r, world, error)) {
            if (error.empty()) error = "the savestate does not parse";
            return false;
        }
        return savestate::restore_world(physics, world, flags, error, report, &travel);
    }
}
