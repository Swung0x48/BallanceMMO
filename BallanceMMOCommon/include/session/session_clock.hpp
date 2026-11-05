#pragma once

// The one entry for every clock read that decides something - a deadline, a
// schedule, an arrival, a stamp on the wire or in a journal - in every host
// (docs/canned-tests-plan.md 3.1, seam C1).  A read names its place
// (clock_site.hpp, one enum for the client and the server), so a recorder can
// write it, a replayer hand it back by site, and a virtual clock run a whole
// session on one time.  real_clock, the production implementation, is what
// the reads were before: steady_clock and system_clock.
//
// Not through here: the reads that time the CPU (a step's cost, a rewind's
// split, a placement's slice budget) - they measure real work and stay
// steady_clock - and the log's date stamps.
//
// The waits of every host's loops go through it too (step K4c, seam C4), by
// site, so a virtual clock (virtual_clock.hpp) ends a wait when its time
// comes instead of the OS: sleep_for (nothing to wait on but time),
// wait_until / wait_for (a condition variable, with its lock held, until
// `ready()` or the time).  The real clock sleeps and waits exactly as the
// loops did before.  A wait reads no clock: a capture holds the same reads
// with or without it.
//
// An implementation is called from any thread (the server reads from its
// network thread and its runner thread).  CK-free and header-only.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

#include "clock_site.hpp"

namespace bmmo::session {

    class session_clock {
    public:
        using clock_type = std::chrono::steady_clock;
        using time_point = clock_type::time_point;

        virtual ~session_clock() = default;
        virtual time_point now(clock_site site) = 0;
        // Microseconds since the Unix epoch (the system clock); utc_ms is the
        // same read in whole milliseconds (a read of its own: a capture holds
        // the value in the unit it was read in).
        virtual int64_t utc_us(clock_site site) = 0;
        virtual uint64_t utc_ms(clock_site site) { return static_cast<uint64_t>(utc_us(site) / 1000); }

        using ready_test = std::function<bool()>;
        virtual void sleep_for(clock_site site, clock_type::duration d) = 0;
        // Returns ready().
        virtual bool wait_until(clock_site site, std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                                time_point t, const ready_test& ready) = 0;
        virtual bool wait_for(clock_site site, std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                              clock_type::duration d, const ready_test& ready) = 0;
    };

    class real_clock final : public session_clock {
    public:
        time_point now(clock_site) override { return clock_type::now(); }
        int64_t utc_us(clock_site) override {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }
        void sleep_for(clock_site, clock_type::duration d) override { std::this_thread::sleep_for(d); }
        bool wait_until(clock_site, std::condition_variable& cv, std::unique_lock<std::mutex>& lock, time_point t,
                        const ready_test& ready) override {
            if (t == time_point::max()) {
                cv.wait(lock, ready);
                return true;
            }
            return cv.wait_until(lock, t, ready);
        }
        bool wait_for(clock_site, std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                      clock_type::duration d, const ready_test& ready) override {
            return cv.wait_for(lock, d, ready);
        }
    };

    // The process's real clock, for a host that is given no other.
    inline session_clock& real_session_clock() {
        static real_clock clock;
        return clock;
    }

}
