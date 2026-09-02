// Retail physics_RT.dll (Ballance 1.13) private layout, taken from BallanceTAS
// (https://github.com/doyaGu/BallanceTAS, src/Game/physics_RT.h, MIT License,
// Copyright (c) 2025 kakuty).  Only used to read the retail world from the
// BallanceMMO client; no IVP code is compiled from it.

#ifndef BML_PHYSICS_RT_H
#define BML_PHYSICS_RT_H

#include <cstddef>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "VxMatrix.h"
#include "CK3dEntity.h"

// ============================================================================
// CONSTANTS AND MACROS
// ============================================================================

#define P_FLOAT_EPS 1e-10f    // used for division checking
#define P_FLOAT_RES 1e-6f     // float resolution for numbers < 1.0
#define P_FLOAT_MAX 1e16f

#define IVP_MAX_DELTA_PSI_TIME (1.0f/10.0f)
#define IVP_MIN_DELTA_PSI_TIME (1.0f/200.0f)

#define IVP_Environment_Magic_Number 123456
#define IVP_MOVEMENT_CHECK_COUNT 10 //do not always check movement state

#define qh_rand_a 16807
#define qh_rand_m 2147483647
#define qh_rand_q 127773  /* m div a */
#define qh_rand_r 2836    /* m mod a */

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

class IVP_Environment;
class IVP_Real_Object;
class IVP_Material;
class PhysicsContactData;
struct PhysicsStruct;

// ============================================================================
// BASIC TYPE DEFINITIONS
// ============================================================================

typedef float IVP_FLOAT;
typedef double IVP_DOUBLE;
typedef int IVP_Time_CODE;

enum IVP_BOOL {
    IVP_FALSE = 0,
    IVP_TRUE  = 1
};

enum IVP_OBJECT_TYPE {
    IVP_NONE,
    IVP_CLUSTER,
    IVP_POLYGON,
    IVP_BALL,
    IVP_OBJECT
};

enum IVP_Movement_Type {
    IVP_MT_UNDEFINED      = 0,
    IVP_MT_MOVING         = 0x01,
    IVP_MT_SLOW           = 0x02,
    IVP_MT_CALM           = 0x03,
    IVP_MT_NOT_SIM        = 0x08,
    IVP_MT_STATIC_PHANTOM = 0x09,
    IVP_MT_STATIC         = 0x10,
    IVP_MT_GET_MINDIST    = 0x21
};

// ============================================================================
// MEMORY MANAGEMENT FUNCTIONS AND MACROS
// ============================================================================

void *p_malloc(unsigned int size);
void p_free(void *data);

#define P_FREE(a)             \
{                             \
    if (a)                    \
    {                         \
        p_free((char *)a);    \
        a = NULL;             \
    }                         \
}

#define P_MEM_CLEAR(a) memset((char *)(a), 0, sizeof(*a));
#define P_MEM_CLEAR_M4(a) memset((char *)(a) + sizeof(void *), 0, sizeof(*a) - sizeof(void *));
#define P_MEM_CLEAR_ARRAY(clss, elems) memset((char *)(clss), 0, sizeof(*clss) * elems);

// ============================================================================
// RANDOM NUMBER FUNCTIONS
// ============================================================================

void ivp_srand(int seed);
int ivp_srand_read();
float ivp_rand();

void qh_srand(int seed);
int qh_srand_read();
int qh_rand();

// ============================================================================
// CORE DATA STRUCTURES
// ============================================================================

// ----------------------------------------------------------------------------
// Vector and Matrix Classes
// ----------------------------------------------------------------------------

class IVP_U_Vector_Base {
public:
    unsigned short memsize;
    unsigned short n_elems;
    void **elems;

    void increment_mem();
};

template <class T>
class IVP_U_Vector : public IVP_U_Vector_Base {
public:
    void ensure_capacity() {
        if (n_elems >= memsize) {
            increment_mem();
        }
    }

protected:
    //  special vector with preallocated elems
    IVP_U_Vector(void **ielems, int size) {
        elems = ielems;
        memsize = size;
        n_elems = 0;
    }

public:
    IVP_U_Vector(int size = 0) {
        memsize = size;
        n_elems = 0;
        if (size) // will be optimized by most compilers
        {
            elems = (void **) p_malloc(size * sizeof(void *));
        } else {
            elems = (void **) NULL;
        }
    }

    void clear() {
        if (elems != (void **) (this + 1)) {
            void *dummy = (void *) elems;
            P_FREE(dummy);
            elems = 0;
            memsize = 0;
        }
        n_elems = 0;
    }

    void remove_all() {
        n_elems = 0;
    }

    ~IVP_U_Vector() {
        this->clear();
    }

    int len() const {
        return n_elems;
    }

    int index_of(T *elem) {
        int i;
        for (i = n_elems - 1; i >= 0; i--) {
            if (elems[i] == elem)
                break;
        }
        return i;
    }

    int add(T *elem) {
        ensure_capacity();
        elems[n_elems] = (void *) elem;
        return n_elems++;
    }

    int install(T *elem) {
        int old_index = index_of(elem);
        if (old_index != -1)
            return old_index;
        ensure_capacity();
        elems[n_elems] = (void *) elem;
        return n_elems++;
    }

    void swap_elems(int index1, int index2) {
        void *buffer = elems[index1];
        elems[index1] = elems[index2];
        elems[index2] = buffer;
    }

    void insert_after(int index, T *elem) {
        index++;
        ensure_capacity();
        int j = n_elems;
        while (j > index) {
            elems[j] = elems[--j];
        }
        elems[index] = (void *) elem;
        n_elems++;
    }

    void remove_at(int index) {
        int j = index;
        while (j < n_elems - 1) {
            elems[j] = elems[j + 1];
            j++;
        }
        n_elems--;
    }

    void reverse() {
        for (int i = 0; i < (this->n_elems / 2); i++) {
            this->swap_elems(i, this->n_elems - 1 - i);
        }
    }

    void remove_at_and_allow_resort(int index) {
        n_elems--;
        elems[index] = elems[n_elems];
    }

    void remove_allow_resort(T *elem) {
        int index = this->index_of(elem);
        n_elems--;
        elems[index] = elems[n_elems];
    }

    void remove(T *elem) {
        int index = this->index_of(elem);
        n_elems--;
        while (index < n_elems) {
            elems[index] = (elems + 1)[index];
            index++;
        }
    }

    T *element_at(int index) const {
        return (T *) elems[index];
    }
};

class IVP_U_Point {
public:
    IVP_DOUBLE k[3];
    IVP_DOUBLE hesse_val;
};

class IVP_U_Float_Point {
public:
    float k[3];
    float hesse_val;
};

class IVP_U_Float_Hesse : public IVP_U_Float_Point {
};

class IVP_U_Matrix3 {
public:
    IVP_U_Point rows[3];

    IVP_DOUBLE get_elem(int row, int col) const { return rows[row].k[col]; };
    void set_elem(int row, int col, IVP_DOUBLE val) { rows[row].k[col] = val; };
};

class IVP_U_Matrix : public IVP_U_Matrix3 {
public:
    IVP_U_Point vv;

    IVP_U_Point *get_position() { return &vv; };
    const IVP_U_Point *get_position() const { return &vv; };
};

class IVP_U_Quat {
public:
    IVP_DOUBLE x, y, z, w;
    void set_quaternion(const IVP_U_Matrix3 *mat);
};

class IVP_U_Float_Quat {
public:
    IVP_FLOAT x, y, z, w;
};

// ----------------------------------------------------------------------------
// Min List Data Structure
// ----------------------------------------------------------------------------

typedef float IVP_U_MINLIST_FIXED_POINT;
typedef unsigned int IVP_U_MINLIST_INDEX;
#define IVP_U_MINLIST_MAXVALUE 1e10f
#define IVP_U_MINLIST_UNUSED ( (1<<16) -1 )
#define IVP_U_MINLIST_LONG_UNUSED ( (1<<16) -2 )
#define IVP_U_MINLIST_MAX_ALLOCATION ( (1<<16) - 4 )

class IVP_U_Min_List_Element {
public:
    unsigned short long_next;
    unsigned short long_prev;
    unsigned short next;
    unsigned short prev;
    IVP_U_MINLIST_FIXED_POINT value;
    void *element;
};

class IVP_U_Min_List {
public:
    unsigned short malloced_size;
    unsigned short free_list;
    IVP_U_Min_List_Element *elems;
    IVP_U_MINLIST_FIXED_POINT min_value;
    unsigned short first_long;
    unsigned short first_element;
    unsigned short counter;
};

class IVP_U_Min_List_Enumerator {
    IVP_U_Min_List *min_list;
    IVP_U_MINLIST_INDEX loop_elem;

public:
    IVP_U_Min_List_Enumerator(IVP_U_Min_List *mh);
    void *get_next_element();
    IVP_U_Min_List_Element *get_next_element_header();
    void *get_next_element_lt(IVP_FLOAT max_limit);
};

// ----------------------------------------------------------------------------
// Time Management
// ----------------------------------------------------------------------------

class IVP_Time {
    double seconds;

public:
    void operator+=(double val) { seconds += val; }
    double get_seconds() const { return seconds; }
    double get_time() const { return seconds; }
    double operator-(const IVP_Time &b) const { return static_cast<float>(this->seconds - b.seconds); }
    void operator-=(const IVP_Time &b) { this->seconds -= b.seconds; }

    IVP_Time operator+(double val) const {
        IVP_Time result;
        result.seconds = this->seconds + val;
        return result;
    }

    IVP_Time() = default;
    IVP_Time(double time) { seconds = time; }
};

class IVP_Time_Manager;

class IVP_Time_Event {
public:
    int index;
    IVP_Time_Event() = default;
    virtual void simulate_time_event(IVP_Environment *);
};

class IVP_Time_Event_PSI : public IVP_Time_Event {
public:
    IVP_Time_Event_PSI() = default;
    void simulate_time_event(IVP_Environment *env);
};

class IVP_Event_Manager {
public:
    int mode;
    virtual void simulate_time_events(IVP_Time_Manager *tman, IVP_Environment *env, IVP_Time until) = 0;
    virtual void simulate_variable_time_step(IVP_Time_Manager *tman, IVP_Environment *env, IVP_Time_Event_PSI *psi_event, IVP_FLOAT delta);
};

class IVP_Time_Manager {
public:
    int n_events; // num of events currently managed
    IVP_Event_Manager *event_manager;
    IVP_U_Min_List *min_hash;
    IVP_Time_Event_PSI *psi_event;

    IVP_DOUBLE last_time; // relative basetime
    IVP_Time base_time;   // value of elements are relativ to this
};

// ============================================================================
// CORE PHYSICS CLASSES
// ============================================================================

// ----------------------------------------------------------------------------
// Core Classes
// ----------------------------------------------------------------------------

class IVP_Vector_of_Objects : public IVP_U_Vector<IVP_Real_Object> {
public:
    IVP_Real_Object *elem_buffer[1];
    IVP_Vector_of_Objects();
    void reset();
};

union IVP_Core_Friction_Info {
    class IVP_Friction_Hash *l_friction_info_hash;
    class IVP_Friction_Info_For_Core *moveable_core_friction_info;
};

class IVP_Old_Sync_Rot_Z {
public:
    IVP_U_Float_Point old_sync_rot_speed;
    IVP_U_Quat old_sync_q_world_f_core_next_psi;
    IVP_BOOL was_pushed_during_i_s;
};

class IVP_Hull_Manager_Base_Gradient {
public:
    IVP_Time last_vpsi_time;
    float gradient;
    float center_gradient;
    float hull_value_last_vpsi;
    float hull_center_value_last_vpsi;
    float hull_value_next_psi;
    int time_of_next_reset;
};

class IVP_Hull_Manager_Base : public IVP_Hull_Manager_Base_Gradient {
public:
    IVP_U_Min_List sorted_synapses;
};

class IVP_Anchor {
public:
    IVP_Anchor *anchor_next_in_object;
    IVP_Anchor *anchor_prev_in_object;
    IVP_Real_Object *l_anchor_object;
    IVP_U_Float_Point object_pos;
    IVP_U_Float_Point core_pos;
    class IVP_Actuator *l_actuator;
};

// ----------------------------------------------------------------------------
// Core Hierarchy
// ----------------------------------------------------------------------------

class IVP_Core_Fast_Static {
public:
    IVP_BOOL fast_piling_allowed_flag : 2;
    IVP_BOOL physical_unmoveable      : 2;
    IVP_BOOL is_in_wakeup_vec         : 2;
    IVP_BOOL rot_inertias_are_equal   : 2;
    IVP_BOOL pinned                   : 2;

    IVP_FLOAT upper_limit_radius;
    IVP_FLOAT max_surface_deviation;
    IVP_Environment *environment;
    class IVP_Constraint_Car_Object *car_wheel;

    IVP_U_Float_Hesse rot_inertia;
    IVP_U_Float_Point rot_speed_damp_factor;
    IVP_U_Float_Hesse inv_rot_inertia;

    IVP_FLOAT speed_damp_factor;
    IVP_FLOAT inv_object_diameter;
    IVP_U_Float_Point *spin_clipping;
    IVP_Vector_of_Objects objects;
    IVP_Core_Friction_Info core_friction_info;

    const class IVP_U_Point_4 *get_inv_masses() { return (IVP_U_Point_4 *) &inv_rot_inertia; }
    IVP_FLOAT get_mass() const { return rot_inertia.hesse_val; };

    const IVP_U_Float_Point *get_rot_inertia() const { return &rot_inertia; };
    const IVP_U_Float_Point *get_inv_rot_inertia() const { return &inv_rot_inertia; };
    IVP_FLOAT get_inv_mass() const { return inv_rot_inertia.hesse_val; };
};

class IVP_Core_Fast_PSI : public IVP_Core_Fast_Static {
public:
    IVP_Movement_Type movement_state : 8;
    IVP_BOOL temporarily_unmovable   : 8;
    short impacts_since_last_PSI;
    IVP_Time time_of_last_psi;
    IVP_FLOAT i_delta_time;

    IVP_U_Float_Point rot_speed_change;
    IVP_U_Float_Point speed_change;
    IVP_U_Float_Point rot_speed;
    IVP_U_Float_Point speed;

    IVP_U_Point pos_world_f_core_last_psi;
    IVP_U_Float_Point delta_world_f_core_psis;

    IVP_U_Quat q_world_f_core_last_psi;
    IVP_U_Quat q_world_f_core_next_psi;
    IVP_U_Matrix m_world_f_core_last_psi;
};

class IVP_Core_Fast : public IVP_Core_Fast_PSI {
public:
    IVP_U_Float_Point rotation_axis_world_space;
    IVP_FLOAT current_speed;
    IVP_FLOAT abs_omega;
    IVP_FLOAT max_surface_rot_speed;
};

class IVP_Core : public IVP_Core_Fast {
public:
    IVP_U_Vector<class IVP_Controller> controllers_of_core;
    class IVP_Core_Merged *merged_core_which_replace_this_core;
    class IVP_Simulation_Unit *sim_unit_of_core;

    IVP_Time time_of_calm_reference[2];
    IVP_U_Float_Quat q_world_f_core_calm_reference[2];
    IVP_U_Float_Point position_world_f_core_calm_reference[2];

    IVP_Core *union_find_father;
    IVP_Old_Sync_Rot_Z *old_sync_info;
    int mindist_event_already_done;
};

// ============================================================================
// OBJECT CLASSES
// ============================================================================

class IVP_Object {
public:
    virtual ~IVP_Object() = 0;

    void set_type(IVP_OBJECT_TYPE type_in) { object_type = type_in; }
    IVP_OBJECT_TYPE get_type() const { return object_type; }

    const char *get_name() const { return name; }
    IVP_Environment *get_environment() const { return environment; }

    IVP_OBJECT_TYPE object_type;
    IVP_Object *next_in_cluster;
    IVP_Object *prev_in_cluster;
    class IVP_Cluster *father_cluster;
    const char *name;
    IVP_Environment *environment;
};

class IVP_Real_Object_Fast_Static : public IVP_Object {
public:
    class IVP_Controller_Phantom *controller_phantom;
    class IVP_Synapse_Real *exact_synapses;
    class IVP_Synapse_Real *invalid_synapses;
    class IVP_Synapse_Friction *friction_synapses;
    IVP_U_Quat *q_core_f_object;
    IVP_U_Float_Point shift_core_f_object;
};

class IVP_Real_Object_Fast : public IVP_Real_Object_Fast_Static {
public:
    class IVP_Cache_Object *cache_object;
    IVP_Hull_Manager_Base hull_manager;

    struct {
        IVP_Movement_Type object_movement_state             : 8;
        IVP_BOOL collision_detection_enabled                : 2;
        IVP_BOOL shift_core_f_object_is_zero                : 2;
        unsigned int object_listener_exists                 : 1;
        unsigned int collision_listener_exists              : 1;
        unsigned int collision_listener_listens_to_friction : 1;
    } flags;
};

class IVP_Real_Object : public IVP_Real_Object_Fast {
public:
    virtual void set_new_quat_object_f_core(const IVP_U_Quat *new_quat_object_f_core, const IVP_U_Point *trans_object_f_core) = 0;
    virtual void set_new_m_object_f_core(const IVP_U_Matrix *new_m_object_f_core) = 0;

    IVP_Core *get_core() const { return physical_core; }
    IVP_Core *get_original_core() const { return original_core; }

    IVP_BOOL is_collision_detection_enabled() { return flags.collision_detection_enabled; }

    IVP_Anchor *anchors;
    class IVP_SurfaceManager *surface_manager;
    char nocoll_group_ident[8];
    class IVP_Material *l_default_material;
    class IVP_OV_Element *ov_element;
    IVP_FLOAT extra_radius;

    IVP_Core *physical_core;
    IVP_Core *friction_core;
    IVP_Core *original_core;
    void *client_data;

    void ensure_in_simulation();
    void enable_collision_detection(IVP_BOOL enable);
    void get_m_world_f_object_AT(IVP_U_Matrix *m_world_f_object_out);
};

// ============================================================================
// LIQUID SURFACE
// ============================================================================

class IVP_Liquid_Surface_Descriptor {
public:
    virtual void calc_liquid_surface(
        IVP_Environment *environment,
        IVP_Core *core,
        IVP_U_Float_Hesse *surface_normal_out,
        IVP_U_Float_Point *abs_speed_of_current_out) = 0;
};

class IVP_Liquid_Surface_Descriptor_Simple : public IVP_Liquid_Surface_Descriptor {
public:
    IVP_U_Float_Hesse surface;
    IVP_U_Float_Point abs_speed_of_current;
};

// ============================================================================
// ENVIRONMENT AND STATE MANAGEMENT
// ============================================================================

enum IVP_ENV_STATE {
    IVP_ES_PSI, // Normal PSI controller mode
    IVP_ES_PSI_INTEGRATOR,
    IVP_ES_PSI_HULL, // Special PSI mode, where no forces are allowed
    IVP_ES_PSI_SHORT,
    IVP_ES_PSI_CRITIC,
    IVP_ES_AT // Between PSIs (Any Time), normally a collision
};

// ============================================================================
// STATISTICS AND MONITORING
// ============================================================================

class IVP_Statistic_Manager {
public:
    IVP_Environment *l_environment;
    
    // Statistic section
    IVP_Time last_statistic_output;

    // Impact section
    IVP_FLOAT max_rescue_speed;
    IVP_FLOAT max_speed_gain;
    int impact_sys_num;
    int impact_counter;
    int impact_sum_sys;
    int impact_hard_rescue_counter;
    int impact_rescue_after_counter;
    int impact_delayed_counter;
    int impact_coll_checks;
    int impact_unmov;
    IVP_DOUBLE sum_energy_destr;

    // Collision checks section
    int sum_of_mindists;
    int mindists_generated;
    int mindists_deleted;
    int range_intra_exceeded;
    int range_world_exceeded;

    // Friction
    int processed_fmindists;
    int global_fmd_counter;

    void clear_statistic();
    void output_statistic();
    IVP_Statistic_Manager();
};

class IVP_Freeze_Manager {
public:
    void init_freeze_manager();
    IVP_FLOAT freeze_check_dtime;
    IVP_Freeze_Manager();
};

enum IVP_BETTERSTATISTICSMANAGER_DATA_ENTITY_TYPE {
    INT_VALUE    = 1,
    DOUBLE_VALUE = 2,
    INT_ARRAY    = 3,
    DOUBLE_ARRAY = 4,
    STRING       = 5
};

class IVP_BetterStatisticsmanager_Data_Int_Array {
public:
    int size;
    int *array;
    int max_value;
    int xpos, ypos;
    int width, height;
    int bg_color, border_color, graph_color;
};

class IVP_BetterStatisticsmanager_Data_Double_Array {
public:
    int size;
    IVP_DOUBLE *array;
    IVP_DOUBLE max_value;
    int xpos, ypos;
    int width, height;
    int bg_color, border_color, graph_color;
};

class IVP_BetterStatisticsmanager_Data_Entity {
    IVP_BOOL enabled;
public:
    IVP_BETTERSTATISTICSMANAGER_DATA_ENTITY_TYPE type;
    union {
        int int_value;
        IVP_DOUBLE double_value;
        IVP_BetterStatisticsmanager_Data_Int_Array int_array;
        IVP_BetterStatisticsmanager_Data_Double_Array double_array;
    } data;
    char *text;
    int text_color;
    int xpos, ypos;

    void enable();
    void disable();
    IVP_BOOL get_state();
    void set_int_value(int value);
    void set_double_value(IVP_DOUBLE value);
    void set_array_size(int size);
    void set_int_array_latest_value(int value);
    void set_double_array_latest_value(IVP_DOUBLE value);
    void set_text(const char *text);
    void set_position(int x, int y);
};

class IVP_BetterStatisticsmanager_Callback_Interface {
public:
    virtual void output_request(IVP_BetterStatisticsmanager_Data_Entity *entity) = 0;
    virtual void enable() = 0;
    virtual void disable() = 0;
};

class IVP_BetterStatisticsmanager {
public:
    IVP_BOOL enabled;
    IVP_U_Vector<IVP_BetterStatisticsmanager_Callback_Interface> output_callbacks;
    IVP_U_Vector<IVP_BetterStatisticsmanager_Data_Entity> data_entities;
    IVP_DOUBLE simulation_time;
    IVP_BOOL update_delayed;
    IVP_DOUBLE update_interval;
};

// ============================================================================
// COLLISION AND CONTACT SYSTEMS
// ============================================================================

struct IVP_Contact_Situation {
    IVP_U_Float_Point surf_normal;
    IVP_U_Float_Point speed;
    IVP_U_Point contact_point_ws;
    IVP_Real_Object *objects[2];
    const class IVP_Compact_Edge *compact_edges[2];
    IVP_Material *materials[2];
    IVP_Contact_Situation() = default;
};

class IVP_Impact_Solver_Long_Term : public IVP_Contact_Situation {
public:
    short index_in_fs;
    short impacts_while_system;
    IVP_BOOL coll_time_is_valid : 8;
    IVP_BOOL friction_is_broken : 2;

    union {
        struct {
            IVP_FLOAT rescue_speed_addon;
            IVP_FLOAT distance_reached_in_time;
            IVP_FLOAT percent_energy_conservation;
        } impact;
        struct {
            IVP_Friction_Info_For_Core *friction_infos[2];
            int has_negative_pull_since;
            IVP_FLOAT dist_len;
        } friction;
    };

    IVP_FLOAT virtual_mass;
    IVP_FLOAT inv_virtual_mass;
    IVP_Core *contact_core[2];
    IVP_U_Float_Point span_friction_v[2];
    IVP_U_Float_Point contact_point_cs[2];
    IVP_U_Float_Point contact_cross_nomal_cs[2];
};

class IVP_Compact_Edge {
public:
    static int next_table[];
    static int prev_table[];
    unsigned int start_point_index : 16;
    signed int opposite_index      : 15;
    unsigned int is_virtual        : 1;
};

class IVP_Contact_Point;

class IVP_Synapse_Friction {
public:
    IVP_Synapse_Friction *next, *prev;
    IVP_Real_Object *l_obj;
    short contact_point_offset;
    short status : 8;
    const IVP_Compact_Edge *edge;
};

class IVP_Contact_Point_Fast_Static {
public:
    IVP_Contact_Point *next_dist_in_friction;
    IVP_Contact_Point *prev_dist_in_friction;
    IVP_Synapse_Friction synapse[2];
    IVP_FLOAT inv_virt_mass_mindist_no_dir;
    IVP_BOOL two_friction_values : 8;
};

enum IVP_CONTACT_POINT_BREAK_STATUS {
    IVP_CPBS_NORMAL,
    IVP_CPBS_NEEDS_RECHECK,
    IVP_CPBS_BROKEN
};

class IVP_Contact_Point_Fast : public IVP_Contact_Point_Fast_Static {
public:
    IVP_FLOAT span_friction_s[2];
    IVP_Impact_Solver_Long_Term *tmp_contact_info;
    IVP_FLOAT real_friction_factor;
    IVP_FLOAT integrated_destroyed_energy;
    IVP_FLOAT inv_triangle_det;
    IVP_FLOAT old_energy_dynamic_fr;
    IVP_FLOAT now_friction_pressure;
    IVP_FLOAT last_gap_len;

    IVP_FLOAT get_gap_length();
    short slowly_turn_on_keeper;
    IVP_CONTACT_POINT_BREAK_STATUS cp_status : 8;
    int has_negative_pull_since;
    IVP_Time last_time_of_recalc_friction_s_vals;
    IVP_U_Float_Point last_contact_point_ws;
};

class IVP_Contact_Point : public IVP_Contact_Point_Fast {
public:
    class IVP_Friction_System *l_friction_system;
};

// ============================================================================
// EVENT SYSTEMS
// ============================================================================

class IVP_Event_Object {
public:
    IVP_Environment *environment;
    IVP_Real_Object *real_object;
};

class IVP_Listener_Object {
public:
    virtual void event_object_deleted(IVP_Event_Object *) = 0;
    virtual void event_object_created(IVP_Event_Object *) = 0;
    virtual void event_object_revived(IVP_Event_Object *) = 0;
    virtual void event_object_frozen(IVP_Event_Object *) = 0;
    virtual ~IVP_Listener_Object() {};
};

class IVP_Event_Collision {
public:
    IVP_FLOAT d_time_since_last_collision;
    IVP_Environment *environment;
    IVP_Contact_Situation *contact_situation;
};

class IVP_Event_Friction {
public:
    IVP_Environment *environment;
    IVP_Contact_Situation *contact_situation;
    IVP_Contact_Point *friction_handle;
};

enum IVP_LISTENER_COLLISION_CALLBACKS {
    IVP_LISTENER_COLLISION_CALLBACK_POST_COLLISION = 0x01,
    IVP_LISTENER_COLLISION_CALLBACK_OBJECT_DELETED = 0x02,
    IVP_LISTENER_COLLISION_CALLBACK_FRICTION       = 0x04,
    IVP_LISTENER_COLLISION_CALLBACK_PRE_COLLISION  = 0x08
};

class IVP_Listener_Collision {
    int enabled_callbacks;
public:
    int get_enabled_callbacks();
    virtual void event_pre_collision(IVP_Event_Collision *);
    virtual void event_post_collision(IVP_Event_Collision *);
    virtual void event_collision_object_deleted(class IVP_Real_Object *);
    virtual void event_friction_created(IVP_Event_Friction *);
    virtual void event_friction_deleted(IVP_Event_Friction *);
    virtual void event_friction_pair_created(class IVP_Friction_Core_Pair *pair) {}
    virtual void event_friction_pair_deleted(class IVP_Friction_Core_Pair *pair) {}
    IVP_Listener_Collision(int enable_callbacks = 1);
    virtual ~IVP_Listener_Collision() {};
};

class IVP_Event_PSI {
public:
    IVP_Environment *environment;
};

class IVP_Listener_PSI {
public:
    virtual void event_PSI(IVP_Event_PSI *) = 0;
    virtual void environment_will_be_deleted(IVP_Environment *) = 0;
};

// ============================================================================
// CONTROLLER SYSTEM
// ============================================================================

class IVP_Vector_of_Cores_2 : public IVP_U_Vector<class IVP_Core> {
    IVP_Core *elem_buffer[2];
public:
    IVP_Vector_of_Cores_2();
};

class IVP_Event_Sim {
public:
    IVP_DOUBLE delta_time;
    IVP_DOUBLE i_delta_time;
    IVP_Environment *environment;
    class IVP_Simulation_Unit *sim_unit;
};

enum IVP_CONTROLLER_PRIORITY {
    IVP_CP_NONE = -1,
    IVP_CP_STATIC_FRICTION = 0,
    IVP_CP_CONSTRAINTS_MIN = 400,
    IVP_CP_CONSTRAINTS     = 405,
    IVP_CP_CONSTRAINTS_MAX = 410,
    IVP_CP_STIFF_SPRINGS = 460,
    IVP_CP_FLOATING      = 470,
    IVP_CP_MOTION = 500,
    IVP_CP_DYNAMIC_FRICTION = 600,
    IVP_CP_GRAVITY = 1000,
    IVP_CP_SPRING      = 1400,
    IVP_CP_ACTUATOR    = 1500,
    IVP_CP_FORCEFIELDS = 1600,
    IVP_CP_ENERGY_FRICTION = 2000
};

class IVP_Controller {
public:
    virtual void core_is_going_to_be_deleted_event(IVP_Core *);
    virtual IVP_DOUBLE get_minimum_simulation_frequency();
    virtual IVP_U_Vector<IVP_Core> *get_associated_controlled_cores() = 0;
    virtual void reset_time(IVP_Time /*offset*/);
    virtual void do_simulation_controller(IVP_Event_Sim *, IVP_U_Vector<IVP_Core> *core_list) = 0;
    virtual IVP_CONTROLLER_PRIORITY get_controller_priority() = 0;
    virtual const char *get_controller_name();
    virtual ~IVP_Controller();
};

class IVP_Controller_Independent : public IVP_Controller {
    static IVP_U_Vector<IVP_Core> empty_list;
    IVP_U_Vector<IVP_Core> *get_associated_controlled_cores();
};

class IVP_Controller_Dependent : public IVP_Controller {};

// ============================================================================
// CONSTRAINT SYSTEM
// ============================================================================

enum IVP_COORDINATE_INDEX {
    IVP_INDEX_X = 0,
    IVP_INDEX_Y = 1,
    IVP_INDEX_Z = 2
};

enum IVP_CONSTRAINT_AXIS_TYPE {
    IVP_CONSTRAINT_AXIS_FREE    = 0x0,
    IVP_CONSTRAINT_AXIS_FIXED   = 0x1,
    IVP_CONSTRAINT_AXIS_LIMITED = 0x2,
    IVP_CONSTRAINT_AXIS_DUMMY = 0xffffffff
};

enum IVP_CONSTRAINT_FORCE_EXCEED {
    IVP_CFE_NONE,
    IVP_CFE_CLIP,
    IVP_CFE_BREAK,
    IVP_CFE_BEND
};

enum IVP_CONSTRAINT_FLAGS {
    IVP_CONSTRAINT_ACTIVATED = 0x10,
    IVP_CONSTRAINT_GLOBAL = 0xF,
    IVP_CONSTRAINT_FAST   = 0x0,
    IVP_CONSTRAINT_SECURE = 0x10
};

enum IVP_NORM {
    IVP_NORM_MINIMUM,
    IVP_NORM_EUCLIDIC,
    IVP_NORM_MAXIMUM
};

class IVP_Constraint : public IVP_Controller_Dependent {
protected:
    IVP_BOOL is_enabled : 2;
public:
    virtual ~IVP_Constraint();
    
    // Translation constraint methods
    virtual void change_fixing_point_Ros(const IVP_U_Point *anchor_Ros);
    virtual void change_target_fixing_point_Ros(const IVP_U_Point *anchor_Ros);
    virtual void change_translation_axes_Ros(const IVP_U_Matrix3 *m_Ros_f_Rfs);
    virtual void change_target_translation_axes_Ros(const IVP_U_Matrix3 *m_Ros_f_Rfs);
    virtual void fix_translation_axis(IVP_COORDINATE_INDEX which);
    virtual void free_translation_axis(IVP_COORDINATE_INDEX which);
    virtual void limit_translation_axis(IVP_COORDINATE_INDEX which, IVP_FLOAT border_left, IVP_FLOAT border_right);
    virtual void change_max_translation_impulse(IVP_CONSTRAINT_FORCE_EXCEED impulsetype, IVP_FLOAT impulse);

    // Rotation constraint methods
    virtual void change_rotation_axes_Ros(const IVP_U_Matrix3 *m_Ros_f_Rrs);
    virtual void change_target_rotation_axes_Ros(const IVP_U_Matrix3 *m_Ros_f_Rrs);
    virtual void fix_rotation_axis(IVP_COORDINATE_INDEX which);
    virtual void free_rotation_axis(IVP_COORDINATE_INDEX which);
    virtual void limit_rotation_axis(IVP_COORDINATE_INDEX which, IVP_FLOAT border_left, IVP_FLOAT border_right);
    virtual void change_max_rotation_impulse(IVP_CONSTRAINT_FORCE_EXCEED impulsetype, IVP_FLOAT impulse);

    // Constraint relaxation
    virtual void change_Aos_to_relaxe_constraint();
    virtual void change_Ros_to_relaxe_constraint();

    IVP_Vector_of_Cores_2 cores_of_constraint_system;
    void *client_data;
};

class IVP_Listener_Constraint {
public:
    virtual void event_constraint_broken(IVP_Constraint *) = 0;
};

// ============================================================================
// MAIN ENVIRONMENT CLASS
// ============================================================================

class IVP_Environment {
    // Friend declarations
    friend class IVP_Simulation_Unit;
    friend class IVP_Friction_System;
    friend class IVP_Friction_Sys_Static;
    friend class IVP_Friction_Solver;
    friend class IVP_Friction_Core_Pair;
    friend class IVP_Impact_Solver;
    friend class IVP_Impact_System;
    friend class IVP_Impact_Solver_Long_Term;
    friend class IVP_Contact_Point;
    friend class IVP_Environment_Manager;
    friend class IVP_Time_Event_PSI;
    friend class IVP_Real_Object;
    friend class IVP_Time_Manager;
    friend class IVP_Mindist_Manager;
    friend class IVP_Calc_Next_PSI_Solver;
    friend class IVP_Mindist;
    friend class IVP_Core;

public:
    // Manager instances
    class IVP_Standard_Gravity_Controller *standard_gravity_controller;
    IVP_Time_Manager *time_manager;
    class IVP_Sim_Units_Manager *sim_units_manager;
    class IVP_Cluster_Manager *cluster_manager;
    class IVP_Mindist_Manager *mindist_manager;
    class IVP_OV_Tree_Manager *ov_tree_manager;
    class IVP_Collision_Filter *collision_filter;
    class IVP_Range_Manager *range_manager;
    class IVP_Anomaly_Manager *anomaly_manager;
    class IVP_Anomaly_Limits *anomaly_limits;
    class IVP_PerformanceCounter *performancecounter;
    class IVP_Universe_Manager *universe_manager;
    class IVP_Real_Object *static_object;

    // Statistics and management
    IVP_Statistic_Manager statistic_manager;
    IVP_Freeze_Manager freeze_manager;
    IVP_BetterStatisticsmanager *better_statisticsmanager;
    class IVP_Controller_Manager *controller_manager;
    class IVP_Cache_Object_Manager *cache_object_manager;

    // Memory management
    class IVP_U_Active_Value_Manager *l_active_value_manager;
    class IVP_Material_Manager *l_material_manager;
    class IVP_U_Memory *short_term_mem;
    class IVP_U_Memory *sim_unit_mem;

    // Timing and simulation
    IVP_DOUBLE time_since_last_blocking;
    IVP_DOUBLE delta_PSI_time;
    IVP_DOUBLE inv_delta_PSI_time;

    // Physics properties
    IVP_U_Point gravity;
    IVP_FLOAT gravity_scalar;

    // Listener collections
    IVP_U_Vector<IVP_Listener_Collision> collision_listeners;
    IVP_U_Vector<IVP_Listener_PSI> psi_listeners;
    IVP_U_Vector<IVP_Core> core_revive_list;
    IVP_U_Vector<IVP_Listener_Constraint> constraint_listeners;

    // Authentication
    char *auth_costumer_name;
    unsigned int auth_costumer_code;
    int pw_count;

    // Movement checking
    IVP_BOOL must_perform_movement_check();

    // Time management
    IVP_Time current_time;
    IVP_Time time_of_next_psi;
    IVP_Time time_of_last_psi;
    IVP_Time_CODE current_time_code;
    IVP_Time_CODE mindist_event_timestamp_reference;
    short next_movement_check;

    // Environment state
    IVP_ENV_STATE state;
    IVP_DOUBLE integrated_energy_damp;

    // Additional listeners and delegators
    IVP_U_Vector<IVP_Listener_Object> global_object_listeners;
    IVP_U_Vector<class IVP_Collision_Delegator_Root> collision_delegator_roots;

    // Debugging and client data
    void *debug_information;
    IVP_BOOL delete_debug_information;
    void *draw_vectors;
    void *client_data;
    int environment_magic_number;
};

// ============================================================================
// GAME INTEGRATION CLASSES
// ============================================================================

struct PhysicsObject {
    CKBehavior *m_Behavior;
    IVP_Real_Object *m_RealObject;
    PhysicsStruct *m_Struct;
    VxVector m_Scale;
    CKDWORD m_FrictionCount;
    CK_ID m_ID;
    IVP_Time m_CurrentTime;
    int field_28;
    PhysicsContactData *m_ContactData;

    const char *GetName() const { return m_RealObject ? m_RealObject->get_name() : nullptr; }
    CK3dEntity *GetEntity() const { return m_RealObject ? reinterpret_cast<CK3dEntity *>(m_RealObject->client_data) : nullptr; }
    void Wake();
    bool IsStatic() const;
    void EnableCollisions(bool enable);
    float GetMass() const;
    float GetInvMass() const;
    void GetInertia(VxVector &inertia) const;
    void GetInvInertia(VxVector &inertia) const;
    void GetDamping(float *speed, float *rot);
    void GetPosition(VxVector *worldPosition, VxVector *angles);
    void GetPositionMatrix(VxMatrix &positionMatrix);
    void GetVelocity(VxVector *velocity, VxVector *angularVelocity);
    void SetVelocity(const VxVector *velocity, const VxVector *angularVelocity);
};

// Binary-backed storage pool entry used by the runtime's internal object container.
struct PhysicsStruct {
    std::uint8_t reserved_00[0x1C];
    CK_ID m_ID;
    IVP_Time m_CurrentTime;
    int field_28;
    PhysicsContactData *m_ContactData;
    PhysicsStruct *m_Next;
    PhysicsContactData *m_ContactData2;
};

struct XArrayPointerTable {
    void *m_Begin;
    void *m_End;
    void *m_AllocatedEnd;
};

struct PhysicsObjectHashTable {
    XArrayPointerTable m_Table;
    int m_Count;
    int m_Threshold;
    float m_LoadFactor;
};

struct PhysicsObjectContainer {
    PhysicsStruct *m_Begin;
    PhysicsStruct *m_End;
    PhysicsStruct m_Data[200];
    PhysicsObjectHashTable m_Table;
};

class CKIpionManager : public CKBaseManager {
public:

    IVP_Environment *GetEnvironment() const { return m_Environment; }
    float GetDeltaTime() const { return m_DeltaTime; }
    void SetDeltaTime(float dt) { m_DeltaTime = dt; }
    float GetPhysicsDeltaTime() const { return m_PhysicsDeltaTime; }
    void SetPhysicsDeltaTime(float dt) { m_PhysicsDeltaTime = dt; }
    float GetPhysicsTimeFactor() const { return m_PhysicsTimeFactor; }
    void SetPhysicsTimeFactor(float factor) { m_PhysicsTimeFactor = factor; }

    virtual void Reset() = 0;
    PhysicsObject *GetPhysicsObject(CK3dEntity *entity);

    // Member variables
    IVP_U_Vector<IVP_Real_Object> m_MovableObjects;
    int field_30;
    IVP_U_Vector<CK3dEntity> m_Entities;
    IVP_U_Vector<IVP_Material> m_Materials;
    IVP_U_Vector<IVP_Liquid_Surface_Descriptor> m_LiquidSurfaces;
    void *m_PreSimulateCallbacks;
    void *m_PostSimulateCallbacks;
    void *m_ContactManager;
    void *m_CollisionListener;
    void *m_ObjectListener;
    int m_CollDetectionIDAttribType;
    void *m_CollisionFilterExclusivePair;
    int m_RefCounter[2];
    int m_Counting;
    int m_CountPSIs;
    int m_UniversePSI;
    int m_ControllersPSI;
    int m_IntegratorsPSI;
    int m_HullPSI;
    int m_ShortMindistsPSI;
    int m_CriticalMindistsPSI;
    int field_90;
    int field_94;
    int field_98;
    int field_9C;
    int field_A0;
    int field_A4;
    int field_A8;
    int field_AC;
    int field_B0;
    int field_B4;
    void *m_StringHash1;
    void *m_SurfaceManagers;
    IVP_Environment *m_Environment;
    int field_C8;
    float m_DeltaTime;
    float m_PhysicsDeltaTime;
    float m_PhysicsTimeFactor;
    int field_D4;
    int field_D8;
    int m_HasPhysicsCalls;
    int m_PhysicalizeCalls;
    int m_DePhysicalizeCalls;
    LARGE_INTEGER m_HasPhysicsTime;
    LARGE_INTEGER m_DePhysicalizeTime;
    std::uint64_t field_F8;
    std::uint64_t field_100;
    void *m_ProfilerCounter;
    PhysicsObjectContainer m_PhysicsObjects;
};

static_assert(sizeof(IVP_Time) == 0x8, "IVP_Time layout mismatch");
static_assert(sizeof(IVP_U_Vector_Base) == 0x8, "IVP_U_Vector_Base layout mismatch");
static_assert(sizeof(IVP_U_Point) == 0x20, "IVP_U_Point layout mismatch");
static_assert(sizeof(IVP_U_Matrix3) == 0x60, "IVP_U_Matrix3 layout mismatch");
static_assert(sizeof(IVP_U_Matrix) == 0x80, "IVP_U_Matrix layout mismatch");
static_assert(sizeof(IVP_U_Quat) == 0x20, "IVP_U_Quat layout mismatch");

static_assert(sizeof(PhysicsObject) == 0x30, "PhysicsObject layout mismatch");
static_assert(offsetof(PhysicsObject, m_Behavior) == 0x0, "PhysicsObject::m_Behavior offset mismatch");
static_assert(offsetof(PhysicsObject, m_RealObject) == 0x4, "PhysicsObject::m_RealObject offset mismatch");
static_assert(offsetof(PhysicsObject, m_Struct) == 0x8, "PhysicsObject::m_Struct offset mismatch");
static_assert(offsetof(PhysicsObject, m_FrictionCount) == 0x18, "PhysicsObject::m_FrictionCount offset mismatch");
static_assert(offsetof(PhysicsObject, m_ID) == 0x1C, "PhysicsObject::m_ID offset mismatch");
static_assert(offsetof(PhysicsObject, m_CurrentTime) == 0x20, "PhysicsObject::m_CurrentTime offset mismatch");
static_assert(offsetof(PhysicsObject, m_ContactData) == 0x2C, "PhysicsObject::m_ContactData offset mismatch");

static_assert(sizeof(PhysicsStruct) == 0x38, "PhysicsStruct layout mismatch");
static_assert(offsetof(PhysicsStruct, m_ID) == 0x1C, "PhysicsStruct::m_ID offset mismatch");
static_assert(offsetof(PhysicsStruct, m_CurrentTime) == 0x20, "PhysicsStruct::m_CurrentTime offset mismatch");
static_assert(offsetof(PhysicsStruct, m_ContactData) == 0x2C, "PhysicsStruct::m_ContactData offset mismatch");
static_assert(offsetof(PhysicsStruct, m_Next) == 0x30, "PhysicsStruct::m_Next offset mismatch");
static_assert(offsetof(PhysicsStruct, m_ContactData2) == 0x34, "PhysicsStruct::m_ContactData2 offset mismatch");

static_assert(sizeof(PhysicsObjectHashTable) == 0x18, "PhysicsObjectHashTable layout mismatch");
static_assert(sizeof(PhysicsObjectContainer) == 0x2BE0, "PhysicsObjectContainer layout mismatch");
static_assert(offsetof(PhysicsObjectContainer, m_Table) == 0x2BC8, "PhysicsObjectContainer::m_Table offset mismatch");

static_assert(sizeof(IVP_Time_Manager) == 0x20, "IVP_Time_Manager layout mismatch");
static_assert(offsetof(IVP_Time_Manager, base_time) == 0x18, "IVP_Time_Manager::base_time offset mismatch");

static_assert(sizeof(IVP_Environment) == 0x178, "IVP_Environment layout mismatch");
static_assert(offsetof(IVP_Environment, time_manager) == 0x4, "IVP_Environment::time_manager offset mismatch");
static_assert(offsetof(IVP_Environment, current_time) == 0x120, "IVP_Environment::current_time offset mismatch");
static_assert(offsetof(IVP_Environment, time_of_next_psi) == 0x128, "IVP_Environment::time_of_next_psi offset mismatch");
static_assert(offsetof(IVP_Environment, time_of_last_psi) == 0x130, "IVP_Environment::time_of_last_psi offset mismatch");
static_assert(offsetof(IVP_Environment, next_movement_check) == 0x140, "IVP_Environment::next_movement_check offset mismatch");

static_assert(sizeof(IVP_Core_Fast_Static) == 0x60, "IVP_Core_Fast_Static layout mismatch");
static_assert(offsetof(IVP_Core_Fast_Static, rot_inertia) == 0x14, "IVP_Core_Fast_Static::rot_inertia offset mismatch");
static_assert(offsetof(IVP_Core_Fast_Static, rot_speed_damp_factor) == 0x24, "IVP_Core_Fast_Static::rot_speed_damp_factor offset mismatch");
static_assert(offsetof(IVP_Core_Fast_Static, inv_rot_inertia) == 0x34, "IVP_Core_Fast_Static::inv_rot_inertia offset mismatch");
static_assert(offsetof(IVP_Core_Fast_Static, speed_damp_factor) == 0x44, "IVP_Core_Fast_Static::speed_damp_factor offset mismatch");

static_assert(sizeof(IVP_Core_Fast_PSI) == 0x1A8, "IVP_Core_Fast_PSI layout mismatch");
static_assert(offsetof(IVP_Core_Fast_PSI, rot_speed_change) == 0x74, "IVP_Core_Fast_PSI::rot_speed_change offset mismatch");
static_assert(offsetof(IVP_Core_Fast_PSI, speed_change) == 0x84, "IVP_Core_Fast_PSI::speed_change offset mismatch");
static_assert(offsetof(IVP_Core_Fast_PSI, rot_speed) == 0x94, "IVP_Core_Fast_PSI::rot_speed offset mismatch");
static_assert(offsetof(IVP_Core_Fast_PSI, speed) == 0xA4, "IVP_Core_Fast_PSI::speed offset mismatch");

static_assert(sizeof(IVP_Core) == 0x238, "IVP_Core layout mismatch");
static_assert(offsetof(IVP_Core, controllers_of_core) == 0x1C8, "IVP_Core::controllers_of_core offset mismatch");

static_assert(sizeof(IVP_Real_Object_Fast_Static) == 0x40, "IVP_Real_Object_Fast_Static layout mismatch");
static_assert(offsetof(IVP_Real_Object_Fast_Static, q_core_f_object) == 0x2C, "IVP_Real_Object_Fast_Static::q_core_f_object offset mismatch");
static_assert(offsetof(IVP_Real_Object_Fast_Static, shift_core_f_object) == 0x30, "IVP_Real_Object_Fast_Static::shift_core_f_object offset mismatch");

static_assert(sizeof(IVP_Real_Object_Fast) == 0x88, "IVP_Real_Object_Fast layout mismatch");
static_assert(offsetof(IVP_Real_Object_Fast, flags) == 0x80, "IVP_Real_Object_Fast::flags offset mismatch");

static_assert(sizeof(IVP_Real_Object) == 0xB8, "IVP_Real_Object layout mismatch");
static_assert(offsetof(IVP_Real_Object, physical_core) == 0xA4, "IVP_Real_Object::physical_core offset mismatch");
static_assert(offsetof(IVP_Real_Object, original_core) == 0xAC, "IVP_Real_Object::original_core offset mismatch");
static_assert(offsetof(IVP_Real_Object, client_data) == 0xB0, "IVP_Real_Object::client_data offset mismatch");

static_assert(sizeof(CKBaseManager) == 0x28, "CKBaseManager layout mismatch");
static_assert(offsetof(CKIpionManager, m_MovableObjects) == 0x28, "CKIpionManager::m_MovableObjects offset mismatch");
static_assert(offsetof(CKIpionManager, m_Environment) == 0xC0, "CKIpionManager::m_Environment offset mismatch");
static_assert(offsetof(CKIpionManager, m_DeltaTime) == 0xC8, "CKIpionManager::m_DeltaTime offset mismatch");
static_assert(offsetof(CKIpionManager, m_PhysicsDeltaTime) == 0xCC, "CKIpionManager::m_PhysicsDeltaTime offset mismatch");
static_assert(offsetof(CKIpionManager, m_PhysicsTimeFactor) == 0xD0, "CKIpionManager::m_PhysicsTimeFactor offset mismatch");
static_assert(offsetof(CKIpionManager, m_HasPhysicsCalls) == 0xDC, "CKIpionManager::m_HasPhysicsCalls offset mismatch");
static_assert(offsetof(CKIpionManager, m_PhysicsObjects) == 0x110, "CKIpionManager::m_PhysicsObjects offset mismatch");
static_assert(sizeof(CKIpionManager) == 0x2CF0, "CKIpionManager layout mismatch");

void InitPhysicsAddresses();

#endif // BML_PHYSICS_RT_H
