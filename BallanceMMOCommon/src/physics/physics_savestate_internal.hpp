#pragma once

// Internal to the savestate's translation units (physics_savestate_common.cpp,
// physics_savestate_capture.cpp, physics_savestate_restore.cpp): the record
// structs the blob is made of, the byte writer/reader, the accessor the IVP
// headers befriend (engine change #17) and the helpers both halves share.
// Nothing here is part of the bridge API; see physics/physics_savestate.hpp.
//
// Every list order below is the engine's own order at the tick boundary and
// is restored as such: the friction solver's matrix follows the contact-point
// list, the time manager and the hull managers resolve ties by insertion
// order (newest first), a simulation unit integrates its cores in vector
// order, the overlap tree hands out partners in traversal order.  See
// docs/ivp-savestate-design.md, "What a tick boundary holds" and "The
// restore, in order".
//
// These files must not do floating-point arithmetic of their own: the
// headless library is built without the physics FP flags, so every value is
// copied, never re-derived.

#include <physics/physics_savestate.hpp>

#include "ivp_physics.hxx"
#include "ivp_core.hxx"
#include "ivp_real_object.hxx"
#include "ivp_mindist.hxx"
#include "ivp_friction.hxx"
#include "ivp_clustering_longrange.hxx"
#include "ivp_clustering_lrange_hash.hxx"
#include "ivp_hull_manager.hxx"
#include "ivp_cache_object.hxx"
#include "ivp_time.hxx"
#include "ivp_sim_unit.hxx"
#include "ivu_min_list.hxx"

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

class CKIpionManager;

// ---------------------------------------------------------------------------
// The accessor the IVP headers befriend.  Static member functions only: a
// friend class's members may read and write the protected and private members
// of the befriending classes, which is the whole point - the protected
// bitfields (coll_type, synapse_sort_flag, cp_status, two_friction_values)
// have no other road.  Everything else here could be a thief<> member pointer
// as in physics_state.cpp; routing it all through one place keeps the two
// halves of the savestate reading and writing the same fields, and avoids a
// second explicit instantiation of the same thief<> specialization (ill-formed).
// ---------------------------------------------------------------------------
class IVP_BMMO_State {
public:
    // --- mindists ---
    static IVP_COLL_TYPE coll_type(const IVP_Mindist_Base& md) { return md.coll_type; }
    static void set_coll_type(IVP_Mindist_Base& md, IVP_COLL_TYPE t) { md.coll_type = t; }
    static unsigned sort_flag(const IVP_Mindist_Base& md) { return md.synapse_sort_flag; }
    static void set_sort_flag(IVP_Mindist_Base& md, unsigned f) { md.synapse_sort_flag = f & 3u; }
    static IVP_Time_CODE& recalc_time_stamp(IVP_Mindist& md) { return md.recalc_time_stamp; }
    // A synapse's own fields (status is protected; edge and l_obj are public).
    // Through the derived type: the friend is declared on IVP_Synapse_Real, and
    // a friend reaches a base's protected member only through the befriending
    // class's objects.
    static short synapse_status(const IVP_Synapse_Real& s) { return s.status; }
    static void set_synapse(IVP_Synapse_Real& s, IVP_Mindist* md, IVP_Real_Object* obj, const IVP_Compact_Edge* e,
                            IVP_SYNAPSE_POLYGON_STATUS st) {
        s.set_synapse_mindist(static_cast<IVP_Mindist_Base*>(md));
        s.l_obj = obj;
        s.edge = e;
        s.status = static_cast<short>(st);
    }
    static IVP_Synapse*& synapse_next(IVP_Synapse& s) { return s.next; }
    static IVP_Synapse*& synapse_prev(IVP_Synapse& s) { return s.prev; }

    // --- OO watchers ---
    static IVP_Synapse_OO& watcher_synapse(IVP_OO_Watcher& w, int i) { return w.synapses[i]; }
    static IVP_U_FVector<IVP_Collision>& watcher_children(IVP_OO_Watcher& w) { return w.mindists; }
    static IVP_Real_Object*& watcher_object(IVP_Synapse_OO& s) { return s.object; }
    static IVP_OO_Watcher*& watcher_of(IVP_Synapse_OO& s) { return s.watcher; }

    // --- hull managers and their listeners ---
    static IVP_Hull_Manager_Base_Gradient& hull_gradient(IVP_Hull_Manager_Base& hm) { return hm; }
    static IVP_U_Min_List& hull_list(IVP_Hull_Manager_Base& hm) { return hm.sorted_synapses; }
    static unsigned int& listener_index(IVP_Listener_Hull& l) { return l.minlist_index; }

    // --- min lists ---
    static IVP_U_Min_List_Element* elements(IVP_U_Min_List& list) { return list.elems; }
    static unsigned short malloced_size(const IVP_U_Min_List& list) { return list.malloced_size; }

    // --- objects ---
    static IVP_Synapse_Real*& exact_synapses(IVP_Real_Object& o) { return o.exact_synapses; }
    static IVP_Synapse_Real*& invalid_synapses(IVP_Real_Object& o) { return o.invalid_synapses; }
    static IVP_Synapse_Friction*& friction_synapses(IVP_Real_Object& o) { return o.friction_synapses; }
    static IVP_Cache_Object*& cache_object(IVP_Real_Object& o) { return o.cache_object; }

    // --- the overlap tree ---
    static IVP_OV_Node*& ov_root(IVP_OV_Tree_Manager& t) { return t.root; }
    static IVP_ov_tree_hash* ov_hash(IVP_OV_Tree_Manager& t) { return t.hash_table; }
    static void delete_ov_node(IVP_OV_Node* node) { delete node; }   // the destructor is private

    // --- the object cache ring ---
    static int cache_count(const IVP_Cache_Object_Manager& m) { return m.n_cache_objects; }
    static int& cache_reuse_index(IVP_Cache_Object_Manager& m) { return m.reuse_loop_index; }
    static IVP_Time_CODE& cache_valid_until(IVP_Cache_Object& c) { return c.valid_until_time_code; }
    static int& cache_reference_count(IVP_Cache_Object& c) { return c.reference_count; }
    static IVP_Real_Object*& cache_owner(IVP_Cache_Object& c) { return c.object; }

    // --- contact points ---
    static IVP_Contact_Point*& cp_next(IVP_Contact_Point& cp) { return cp.next_dist_in_friction; }
    static IVP_Contact_Point*& cp_prev(IVP_Contact_Point& cp) { return cp.prev_dist_in_friction; }
    static IVP_Synapse_Friction& cp_synapse(IVP_Contact_Point& cp, int i) { return cp.synapse[i]; }
    static IVP_FLOAT& cp_inv_virt_mass(IVP_Contact_Point& cp) { return cp.inv_virt_mass_mindist_no_dir; }
    static bool cp_two_friction(const IVP_Contact_Point& cp) { return cp.two_friction_values != IVP_FALSE; }
    static void set_cp_two_friction(IVP_Contact_Point& cp, bool v) { cp.two_friction_values = v ? IVP_TRUE : IVP_FALSE; }
    static IVP_FLOAT* cp_span(IVP_Contact_Point& cp) { return cp.span_friction_s; }
    static IVP_Impact_Solver_Long_Term*& cp_tmp_info(IVP_Contact_Point& cp) { return cp.tmp_contact_info; }
    static IVP_FLOAT& cp_friction_factor(IVP_Contact_Point& cp) { return cp.real_friction_factor; }
    static IVP_FLOAT& cp_destroyed_energy(IVP_Contact_Point& cp) { return cp.integrated_destroyed_energy; }
    static IVP_FLOAT& cp_inv_triangle_det(IVP_Contact_Point& cp) { return cp.inv_triangle_det; }
    static IVP_FLOAT& cp_old_energy(IVP_Contact_Point& cp) { return cp.old_energy_dynamic_fr; }
    static IVP_FLOAT& cp_pressure(IVP_Contact_Point& cp) { return cp.now_friction_pressure; }
    static IVP_FLOAT& cp_gap(IVP_Contact_Point& cp) { return cp.last_gap_len; }
    static short& cp_keeper(IVP_Contact_Point& cp) { return cp.slowly_turn_on_keeper; }
    static IVP_CONTACT_POINT_BREAK_STATUS cp_status(const IVP_Contact_Point& cp) { return cp.cp_status; }
    static void set_cp_status(IVP_Contact_Point& cp, IVP_CONTACT_POINT_BREAK_STATUS s) { cp.cp_status = s; }
    static int& cp_pull(IVP_Contact_Point& cp) { return cp.has_negative_pull_since; }
    static IVP_Time& cp_recalc_time(IVP_Contact_Point& cp) { return cp.last_time_of_recalc_friction_s_vals; }
    static IVP_U_Float_Point& cp_contact_ws(IVP_Contact_Point& cp) { return cp.last_contact_point_ws; }
    static IVP_U_Float_Point& cp_normal_ws(IVP_Contact_Point& cp) { return cp.surf_normal_ws; }
    static IVP_Friction_System*& cp_system(IVP_Contact_Point& cp) { return cp.l_friction_system; }
    static void delete_contact_point(IVP_Contact_Point* cp) { delete cp; }   // the destructor is private
    // A friction synapse's status is protected; the contact point's
    // constructor sets it from the mindist handed in.
    static short friction_synapse_status(const IVP_Synapse_Friction& s) { return s.status; }
    static IVP_Synapse_Friction*& friction_next(IVP_Synapse_Friction& s) { return s.next; }
    static IVP_Synapse_Friction*& friction_prev(IVP_Synapse_Friction& s) { return s.prev; }

    // --- friction systems and pairs ---
    static IVP_Contact_Point*& fs_first(IVP_Friction_System& fs) { return fs.first_friction_dist; }
    static short& fs_obj_number(IVP_Friction_System& fs) { return fs.friction_obj_number; }
    static short& fs_dist_number(IVP_Friction_System& fs) { return fs.friction_dist_number; }
    static short& fs_complex_not_necessary(IVP_Friction_System& fs) { return fs.complex_not_necessary_number; }
    static IVP_Friction_Sys_Static& fs_static_handle(IVP_Friction_System& fs) { return fs.static_fs_handle; }
    static IVP_Friction_Sys_Energy& fs_energy_handle(IVP_Friction_System& fs) { return fs.energy_fs_handle; }
    static IVP_Environment*& fs_environment(IVP_Friction_System& fs) { return fs.l_environment; }
    static IVP_U_Vector<IVP_Contact_Point>& pair_dists(IVP_Friction_Core_Pair& p) { return p.fr_dists; }

    // --- environment ---
    static IVP_U_Vector<IVP_Core>& revive_list(IVP_Environment& env) { return env.core_revive_list; }
    static IVP_Time_CODE& time_code(IVP_Environment& env) { return env.current_time_code; }
    static IVP_Time_CODE& event_reference(IVP_Environment& env) { return env.mindist_event_timestamp_reference; }
    static IVP_Time& current_time(IVP_Environment& env) { return env.current_time; }
    static IVP_Time& time_of_last_psi(IVP_Environment& env) { return env.time_of_last_psi; }
    static IVP_Time& time_of_next_psi(IVP_Environment& env) { return env.time_of_next_psi; }
    static short& next_movement_check(IVP_Environment& env) { return env.next_movement_check; }
    static IVP_ENV_STATE& env_state(IVP_Environment& env) { return env.state; }
    static IVP_Time_Manager* time_manager(IVP_Environment& env) { return env.time_manager; }
    static IVP_Mindist_Manager* mindist_manager(IVP_Environment& env) { return env.mindist_manager; }
    static IVP_OV_Tree_Manager* ov_tree(IVP_Environment& env) { return env.ov_tree_manager; }
    static IVP_Sim_Units_Manager* units_manager(IVP_Environment& env) { return env.sim_units_manager; }
    static IVP_Cache_Object_Manager* cache_manager(IVP_Environment& env) { return env.cache_object_manager; }
    static IVP_U_Vector<IVP_Listener_Collision>& collision_listeners(IVP_Environment& env) { return env.collision_listeners; }

    // --- simulation units ---
    static void set_unit_type(IVP_Simulation_Unit& u, IVP_Movement_Type t) { u.sim_unit_movement_type = t; }
};

namespace bmmo::physics::savestate {

    constexpr uint32_t kMagic = 0x53565349u;     // "ISVS"
    constexpr uint32_t kFormatVersion = 2u;

    // ------------------------------------------------------------------
    // Records.  Every index is into the tables of the same world_record.
    // ------------------------------------------------------------------

    // Where a synapse sits on an object's surface.
    struct edge_ref {
        int32_t object = -1;    // objects[]
        int32_t offset = -1;    // byte offset of the IVP_Compact_Edge inside the object's compact surface; -1 = the balls' shared dummy ledge
        uint8_t status = 0;     // IVP_SYNAPSE_POLYGON_STATUS
    };

    // One controller in a core's controllers_of_core or a unit's
    // controller_cores, in list order.
    struct controller_ref {
        uint8_t kind = 0;         // 0 = friction system handle, 1 = any other controller (kept alive across the restore, or re-created by the caller before it)
        int32_t system = -1;      // kind 0: systems[]
        uint8_t part = 0;         // kind 0: 0 = static handle (priority 0), 1 = the system itself (600), 2 = energy handle (2000)
        int32_t priority = 0;     // kind 1: get_controller_priority()
        std::string name;         // kind 1: get_controller_name()
    };

    struct core_record {
        uint8_t movement_state = 0;        // IVP_Movement_Type
        uint8_t temporarily_unmovable = 0;
        uint8_t pinned = 0;
        uint8_t fast_piling = 0;           // fast_piling_allowed_flag (sticky, set by static_fr_oversized_matrix_panic)
        uint8_t in_revive_list = 0;        // is_in_wakeup_vec
        int16_t impacts_since_last_psi = 0;
        double time_of_last_psi = 0.0;     // per core: a frozen core's is its freeze or beam time
        float i_delta_time = 0.0f;
        float rot_speed_change[3] = {}, speed_change[3] = {}, rot_speed[3] = {}, speed[3] = {};
        double pos_last_psi[3] = {};
        float delta_psis[3] = {};
        double q_last[4] = {}, q_next[4] = {};        // x y z w
        double m_last_psi[12] = {};                   // m_world_f_core_last_psi verbatim: rows[0..2] (3 each) then vv
        float rotation_axis_ws[3] = {};
        float current_speed = 0.0f, abs_omega = 0.0f, max_surface_rot_speed = 0.0f;
        double calm_time[2] = {};
        float calm_quat[2][4] = {};
        float calm_position[2][3] = {};
        int32_t unit = -1;                 // units[]
        std::vector<controller_ref> controllers;      // controllers_of_core order
    };

    // One entry of an object's hull-manager min-list, in sorted order.
    struct hull_entry {
        uint8_t kind = 0;        // 0 = the object's own OV element, 1 = synapse `side` of collisions[collision] (a mindist's or a watcher's)
        int32_t collision = -1;
        uint8_t side = 0;
        float value = 0.0f;      // the min-list value (hull time)
    };

    struct object_record {
        std::string name;
        uint32_t entity_id = 0;        // CK_ID of the CK3dEntity (0 when unknown)
        uint8_t type = 0;              // IVP_OBJECT_TYPE (IVP_BALL / IVP_POLYGON)
        uint8_t movable = 0;           // !core->physical_unmoveable
        uint8_t movement_state = 0;    // flags.object_movement_state
        uint32_t surface_size = 0;     // compact surface bytes (0 for a ball): validates edge offsets
        core_record core;              // the core's state; for a static core only movement_state, unit and controllers are meaningful
        // hull manager
        double last_vpsi_time = 0.0;
        float gradient = 0.0f, center_gradient = 0.0f;
        float hull_value_last_vpsi = 0.0f, hull_center_value_last_vpsi = 0.0f, hull_value_next_psi = 0.0f;
        int32_t time_of_next_reset = 0;
        std::vector<hull_entry> hull;
        // overlap element
        uint8_t has_ov = 0;
        uint8_t ov_in_hull = 0;        // ov_element->hull_manager != NULL (its entry is in `hull`)
        float ov_center[3] = {};
        float ov_radius = 0.0f;
        int32_t ov_node = -1;          // ov_nodes[]
        std::vector<int32_t> ov_collisions;   // root collisions in collision_fvector order
        // per-object intrusive lists, in list order
        std::vector<int32_t> exact_synapses;     // encoded (collision << 1) | side
        std::vector<int32_t> invalid_synapses;   // encoded (collision << 1) | side
        std::vector<int32_t> friction_synapses;  // encoded (contact << 1) | side
        // the CK entity's world matrix (physics_RT side), when the entity exists
        uint8_t has_entity_matrix = 0;
        float entity_matrix[16] = {};
    };

    struct collision_record {
        uint8_t kind = 0;                 // 0 = IVP_Mindist, 1 = IVP_Mindist_Recursive, 2 = IVP_OO_Watcher
        int32_t parent = -1;              // collisions[] (a watcher or a recursive mindist), -1 = root (delegator = the environment's root delegator)
        int32_t objects[2] = {-1, -1};    // the pair, as get_objects() reports it (a watcher's synapse order; a mindist's synapse[0]/[1])
        // mindists (kind 0 and 1)
        edge_ref synapse[2];
        uint8_t coll_type = 0, sort_flag = 0, phantom_set = 0, function = 0, recalc_result = 0, disable_halfspace = 0;
        uint8_t status = 0;               // IVP_MINIMAL_DIST_STATUS
        uint8_t selector = 0, counter = 0;
        uint8_t recalc_fresh = 0;         // recalc_time_stamp == current_time_code at capture
        float sum_extra_radius = 0.0f, len_numerator = 0.0f, contact_dot_diff_center = 0.0f;
        double sum_angular_hull_time = 0.0;
        float contact_plane[3] = {};
        uint8_t scheduled = 0;            // index != IVP_U_MINLIST_UNUSED
        float event_value = 0.0f;         // the time manager's element value (relative to base_time)
        // recursive (kind 1)
        int8_t recursive_status = -1;     // IVP_MINDIST_RECURSIVE_TYPES
        int32_t spawned_count = 0;
        // kind 1 and 2
        std::vector<int32_t> children;    // collisions[] in vector order
    };

    struct contact_record {
        edge_ref synapse[2];              // the contact point's synapse[0]/[1]
        int32_t system = -1;              // systems[]
        float gap = 0.0f, span[2] = {}, pressure = 0.0f, friction_factor = 0.0f, destroyed_energy = 0.0f;
        float inv_triangle_det = 0.0f, old_energy = 0.0f, inv_virt_mass = 0.0f;
        int32_t pull = 0;
        int16_t keeper = 0;
        uint8_t status = 0;               // IVP_CONTACT_POINT_BREAK_STATUS
        uint8_t two_friction = 0;
        double recalc_time = 0.0;         // last_time_of_recalc_friction_s_vals
        float contact_ws[3] = {}, normal_ws[3] = {};
    };

    struct pair_record {
        int32_t objects[2] = {-1, -1};    // the pair's cores, as objects[] of their first object
        int32_t next_ease = 0;
        double last_impact_time = 0.0;
        float anti_energy = 0.0f;
        std::vector<int32_t> dists;       // contacts[] in fr_dists order
    };

    struct friction_info_record {
        int32_t object = -1;              // the core (objects[] of its first object); static cores keep one per system
        std::vector<int32_t> springs;     // contacts[] in friction_springs order
    };

    struct friction_system_record {
        std::vector<int32_t> cores;           // cores_of_friction_system, as objects[]
        std::vector<int32_t> movable_cores;   // moveable_cores_of_friction_system
        std::vector<int32_t> dists;           // the intrusive list from first_friction_dist, in order
        std::vector<pair_record> pairs;       // fr_pairs_of_objs order
        std::vector<friction_info_record> infos;
        int16_t obj_number = 0, dist_number = 0, complex_not_necessary = 0;
        uint8_t union_find_necessary = 0, simulated = 0;
    };

    struct unit_record {
        std::vector<int32_t> cores;       // sim_unit_cores, as objects[]
        uint8_t movement_type = 0;        // IVP_Movement_Type
        uint8_t union_find_needed = 0, has_fast_objects = 0, just_slowed_down = 0;
        int16_t next_movement_check = 0;  // engine change #5
        uint8_t in_still_slot = 0;        // which manager list it is in
        std::vector<controller_ref> controllers;   // controller_cores order
    };

    struct event_record {
        int32_t collision = -1;           // collisions[] of a scheduled mindist, -1 = the PSI event
        float value = 0.0f;               // relative to base_time
    };

    // One node of the overlap tree.
    struct ov_node_record {
        int32_t x = 0, y = 0, z = 0, rasterlevel = 0, sizelevel = 0;
        int32_t parent = -1;              // ov_nodes[]
        std::vector<int32_t> children;    // ov_nodes[] in vector order
        std::vector<int32_t> elements;    // objects[] whose OV element sits in this node, in vector order
    };

    // One slot of the object cache ring.
    struct cache_slot_record {
        int32_t object = -1;              // objects[] (-1: free or an object the blob does not know)
        uint8_t fresh = 0;                // valid_until_time_code == current_time_code at capture
        double q[4] = {};                 // q_world_f_object x y z w
        double m[12] = {};                // m_world_f_object rows[0..2] then vv
        double core_pos[3] = {};
    };

    // A PreSimulate callback the manager holds (physics_RT side, compared, never rebuilt).
    struct callback_record {
        int32_t slot = 0;                 // m_Callbacks[slot]
        uint32_t behavior = 0;            // m_BehaviorID
        int32_t type = 0;                 // m_Type
    };

    struct world_record {
        // identity and clock
        uint64_t environment_id = 0;      // the IVP_Environment pointer, as an integer: a blob never applies to a rebuilt environment
        double current_time = 0.0, time_of_last_psi = 0.0, time_of_next_psi = 0.0, base_time = 0.0;
        float last_time = 0.0f;
        int32_t next_movement_check = 0;
        uint8_t env_state = 0;
        float gravity[3] = {};
        // the manager (physics_RT side)
        float delta_time_ms = 0.0f, physics_delta_time = 0.0f, time_factor = 0.0f;
        int32_t ivp_seed = 0;             // ivp_srand_read()
        int32_t random_state = 0;         // bmmo::physics::random_get_state()
        std::vector<int32_t> movable_objects;      // CKIpionManager::m_MovableObjects as objects[], in order (duplicates kept)
        std::vector<callback_record> callbacks;    // the PreSimulate queue, slot by slot in vector order
        // tables
        std::vector<object_record> objects;        // sorted by name
        std::vector<collision_record> collisions;  // roots in object order then fvector order, children after their parent
        std::vector<int32_t> exact_list;           // IVP_Mindist_Manager::exact_mindists, in list order
        std::vector<int32_t> invalid_list;         // IVP_Mindist_Manager::invalid_mindists
        std::vector<event_record> events;          // the time manager's queue in sorted order
        std::vector<contact_record> contacts;
        std::vector<friction_system_record> systems;
        std::vector<unit_record> units;            // slot-0 list order, then the still slot
        std::vector<int32_t> revive_list;          // objects[] of the cores in core_revive_list, in order
        std::vector<ov_node_record> ov_nodes;      // the overlap tree
        int32_t ov_root = -1;
        std::vector<cache_slot_record> cache_slots;   // the cache ring, slot by slot
        int32_t cache_reuse_index = 0;
    };

    // ------------------------------------------------------------------
    // Bytes.  Little-endian, fixed-width scalars, length-prefixed strings
    // and vectors; the reader refuses a wrong magic/version/size.
    // ------------------------------------------------------------------
    class writer {
    public:
        std::vector<uint8_t> bytes;
        void u8(uint8_t v) { bytes.push_back(v); }
        void i8(int8_t v) { bytes.push_back(static_cast<uint8_t>(v)); }
        void u16(uint16_t v) { raw(&v, 2); }
        void i16(int16_t v) { raw(&v, 2); }
        void u32(uint32_t v) { raw(&v, 4); }
        void i32(int32_t v) { raw(&v, 4); }
        void u64(uint64_t v) { raw(&v, 8); }
        void f32(float v) { raw(&v, 4); }
        void f64(double v) { raw(&v, 8); }
        void str(const std::string& s) { u32(static_cast<uint32_t>(s.size())); raw(s.data(), s.size()); }
        void raw(const void* p, size_t n) {
            const uint8_t* b = static_cast<const uint8_t*>(p);
            bytes.insert(bytes.end(), b, b + n);
        }
    };

    class reader {
    public:
        reader(const uint8_t* d, size_t n) : data(d), size(n) {}
        const uint8_t* data;
        size_t size;
        size_t pos = 0;
        bool failed = false;
        bool ok() const { return !failed; }
        bool take(void* out, size_t n) {
            if (failed || pos + n > size) { failed = true; std::memset(out, 0, n); return false; }
            std::memcpy(out, data + pos, n);
            pos += n;
            return true;
        }
        uint8_t u8() { uint8_t v = 0; take(&v, 1); return v; }
        int8_t i8() { int8_t v = 0; take(&v, 1); return v; }
        uint16_t u16() { uint16_t v = 0; take(&v, 2); return v; }
        int16_t i16() { int16_t v = 0; take(&v, 2); return v; }
        uint32_t u32() { uint32_t v = 0; take(&v, 4); return v; }
        int32_t i32() { int32_t v = 0; take(&v, 4); return v; }
        uint64_t u64() { uint64_t v = 0; take(&v, 8); return v; }
        float f32() { float v = 0; take(&v, 4); return v; }
        double f64() { double v = 0; take(&v, 8); return v; }
        std::string str() {
            const uint32_t n = u32();
            if (failed || n > size - pos) { failed = true; return {}; }
            std::string s(reinterpret_cast<const char*>(data + pos), n);
            pos += n;
            return s;
        }
        // Guard for vector lengths: a corrupt count must not allocate the moon.
        uint32_t count() {
            const uint32_t n = u32();
            if (!failed && n > size - pos) failed = true;
            return failed ? 0u : n;
        }
    };

    // physics_savestate_common.cpp
    void write_world(writer& w, const world_record& world);
    bool read_world(reader& r, world_record& world, std::string& error);
    std::string describe_world(const world_record& world);   // what describe_savestate prints
    std::string diff_world(const world_record& a, const world_record& b);

    // ------------------------------------------------------------------
    // The live world, indexed.  Built by both halves from the manager's
    // physics-object table: objects sorted by name, their cores, and the
    // surfaces edges are named against.
    // ------------------------------------------------------------------
    struct live_object {
        IVP_Real_Object* object = nullptr;
        IVP_Core* core = nullptr;
        CK3dEntity* entity = nullptr;
        std::string name;
        uint32_t entity_id = 0;               // the entity's CK_ID (0 for an object IVP owns): tells two same-named bodies apart
        const char* surface_base = nullptr;   // the compact surface's bytes (polygons), nullptr for a ball
        uint32_t surface_size = 0;
        const IVP_Compact_Edge* ball_edge = nullptr;   // the balls' shared dummy edge (balls only)
    };

    struct live_world {
        CKIpionManager* physics = nullptr;
        IVP_Environment* env = nullptr;
        std::vector<live_object> objects;                 // sorted by name, then by entity id
        std::map<const IVP_Real_Object*, int32_t> by_object;
        std::multimap<std::string, int32_t> by_name;
        int32_t index_of(const IVP_Real_Object* o) const {
            auto it = by_object.find(o);
            return it == by_object.end() ? -1 : it->second;
        }
        // The live object a record names.  Names are not unique on a Ballance
        // map (two instances of one element type give two bodies of one name:
        // Level 8 has two P_Modul_30_Wippe), so a name shared by several
        // bodies is resolved by the entity id the record carries; a blob
        // never leaves the process that captured it, so the id is stable.
        int32_t find_by_name(const std::string& name, uint32_t entity_id) const {
            auto range = by_name.equal_range(name);
            if (range.first == range.second) return -1;
            if (std::next(range.first) == range.second) return range.first->second;
            for (auto it = range.first; it != range.second; ++it)
                if (objects[static_cast<size_t>(it->second)].entity_id == entity_id) return it->second;
            return -1;
        }
        // The object a core belongs to (its first object), or -1.
        int32_t index_of_core(const IVP_Core* core) const {
            if (!core || core->objects.len() == 0) return -1;
            return index_of(core->objects.element_at(0));
        }
    };

    // Index every physicalized object.  Fails on a missing environment and on
    // an object with a phantom controller (the savestate fails closed on
    // phantoms).  Same-named bodies are allowed and ordered by entity id.
    bool index_live_world(CKIpionManager* physics, live_world& out, std::string& error);

    // Name an edge, or resolve one.  resolve returns nullptr when the offset
    // is outside the object's surface or the object has the wrong type.
    edge_ref name_edge(const live_world& live, const IVP_Real_Object* object, const IVP_Compact_Edge* edge,
                       IVP_SYNAPSE_POLYGON_STATUS status);
    const IVP_Compact_Edge* resolve_edge(const live_world& live, const edge_ref& ref);

    // A readable name for messages.
    std::string describe_edge(const world_record& world, const edge_ref& ref);

    // physics_savestate_capture.cpp
    bool capture_world(CKIpionManager* physics, world_record& out, std::string& error, savestate_report* report);

    // physics_savestate_restore.cpp
    bool restore_world(CKIpionManager* physics, const world_record& world, uint32_t flags, std::string& error,
                       savestate_report* report);

    // Append a capped warning.
    void warn(savestate_report* report, const std::string& line);
}
