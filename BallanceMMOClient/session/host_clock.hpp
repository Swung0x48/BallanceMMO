#pragma once

// K7b (docs/k7-retail-capture-design.md): the retail game thread's clock.
// Every read that can steer a session - the core's, the core host's, the
// tick driver's - goes through it by site, so a capture can record it and a
// replay hand it back; the real clock otherwise.  A read only a panel, a log
// or a measurement uses takes clock_site::retail_display (a free site, never
// recorded).  The legacy mode's own timers (CKTimeManager::GetTime, the
// transport's process_timestamp: the classic highscore timer, its points,
// the legacy display) feed no session and stay on the real clock.

#include <atomic>

#include <session/session_clock.hpp>

namespace bmmo::retail {

    inline std::atomic<bmmo::session::session_clock*>& host_clock_slot() {
        static std::atomic<bmmo::session::session_clock*> slot{nullptr};
        return slot;
    }

    inline bmmo::session::session_clock& host_clock() {
        auto* clock = host_clock_slot().load();
        return clock ? *clock : bmmo::session::real_session_clock();
    }

    // nullptr: the real clock again.
    inline void set_host_clock(bmmo::session::session_clock* clock) { host_clock_slot().store(clock); }

}
