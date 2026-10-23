#pragma once
// Keep a windowless simulation process on the performance cores.
//
// On a hybrid CPU (here an i7-14700KF: 8 performance cores, 12 efficiency
// cores) Windows 11 marks a process without a foreground window as "power
// throttled" (EcoQoS) when it was started from an app that is not in the
// foreground, and the scheduler then keeps it on the efficiency cores.  The
// headless client is such a process whenever the harness runs unattended:
// measured on one inbound replay, 18.6 s pinned to the performance cores,
// 31.9 s pinned to the efficiency cores and 30.3 s left to the scheduler.  In
// a live run the slower clients fall behind the server's pace and rebase,
// which changes what a run measures.  Opting out of execution-speed
// throttling is the per-process switch for that; it changes no system
// setting.  Does nothing elsewhere.

#ifdef _WIN32
#include <windows.h>
#endif

namespace bmmo::sim {

inline void opt_out_of_power_throttling() {
#if defined(_WIN32) && defined(PROCESS_POWER_THROTTLING_CURRENT_VERSION)
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = 0;   // controlled, and not throttled
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
#endif
}

}
