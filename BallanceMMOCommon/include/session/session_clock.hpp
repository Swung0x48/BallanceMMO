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
// An implementation is called from any thread (the server reads from its
// network thread and its runner thread).  CK-free and header-only.

#include <chrono>
#include <cstdint>

#include "clock_site.hpp"

namespace bmmo::session {

    class session_clock {
    public:
        using clock_type = std::chrono::steady_clock;
        using time_point = clock_type::time_point;

        virtual ~session_clock() = default;
        virtual time_point now(clock_site site) = 0;
        // Microseconds since the Unix epoch (the system clock); utc_ms is the
        // same read in whole milliseconds.
        virtual int64_t utc_us(clock_site site) = 0;
        uint64_t utc_ms(clock_site site) { return static_cast<uint64_t>(utc_us(site) / 1000); }
    };

    class real_clock final : public session_clock {
    public:
        time_point now(clock_site) override { return clock_type::now(); }
        int64_t utc_us(clock_site) override {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }
    };

    // The process's real clock, for a host that is given no other.
    inline session_clock& real_session_clock() {
        static real_clock clock;
        return clock;
    }

}
