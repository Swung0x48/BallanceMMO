#pragma once

// Access to a handful of private IVP_Environment members that the
// determinism harness must read and reset on both the client and the headless
// server.  Explicit template instantiation is exempt from access checking
// ([temp.explicit]), so this needs no change to the pinned IVP headers.

class IVP_Environment;
class IVP_Time_Manager;
class IVP_Time;
class IVP_Core;
template <class T> class IVP_U_Vector;

namespace bmmo::physics::ivp_access {
    template <class Tag, typename Tag::type Member>
    struct thief {
        friend typename Tag::type steal(Tag) { return Member; }
    };

    struct env_time_manager { using type = IVP_Time_Manager* IVP_Environment::*; friend type steal(env_time_manager); };
    struct env_current_time { using type = IVP_Time IVP_Environment::*; friend type steal(env_current_time); };
    struct env_time_of_next_psi { using type = IVP_Time IVP_Environment::*; friend type steal(env_time_of_next_psi); };
    struct env_time_of_last_psi { using type = IVP_Time IVP_Environment::*; friend type steal(env_time_of_last_psi); };
    struct env_next_movement_check { using type = short IVP_Environment::*; friend type steal(env_next_movement_check); };
    struct env_core_revive_list { using type = IVP_U_Vector<IVP_Core> IVP_Environment::*; friend type steal(env_core_revive_list); };

    template struct thief<env_core_revive_list, &IVP_Environment::core_revive_list>;
    template struct thief<env_time_manager, &IVP_Environment::time_manager>;
    template struct thief<env_current_time, &IVP_Environment::current_time>;
    template struct thief<env_time_of_next_psi, &IVP_Environment::time_of_next_psi>;
    template struct thief<env_time_of_last_psi, &IVP_Environment::time_of_last_psi>;
    template struct thief<env_next_movement_check, &IVP_Environment::next_movement_check>;

    inline IVP_Time_Manager*& time_manager(IVP_Environment& env) { return env.*steal(env_time_manager()); }
    inline IVP_Time& current_time(IVP_Environment& env) { return env.*steal(env_current_time()); }
    inline IVP_Time& time_of_next_psi(IVP_Environment& env) { return env.*steal(env_time_of_next_psi()); }
    inline IVP_Time& time_of_last_psi(IVP_Environment& env) { return env.*steal(env_time_of_last_psi()); }
    inline short& next_movement_check(IVP_Environment& env) { return env.*steal(env_next_movement_check()); }
    // The delayed-revive list (IVP_Environment::add_revive_core, drained at
    // the next PSI): read by the body diagnostics.
    inline IVP_U_Vector<IVP_Core>& revive_list(IVP_Environment& env) { return env.*steal(env_core_revive_list()); }
}
