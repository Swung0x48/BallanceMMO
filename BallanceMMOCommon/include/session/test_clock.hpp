#pragma once

// LB0/H11: an opt-in scheduler clock fault. Network/handshake clocks stay real.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>

namespace bmmo::session {
    struct test_clock_skew {
        uint32_t tick = 0;
        uint32_t ms = 0;
        double percent = 0;
        bool enabled() const { return ms != 0 && percent > 0 && percent < 100; }
    };

    inline bool parse_test_clock_skew(const std::string& text, test_clock_skew& out) {
        out = {};
        if (text.empty()) return true;
        std::istringstream in(text);
        uint64_t tick = 0, ms = 0;
        double percent = 0;
        char a = 0, b = 0;
        if (!(in >> tick >> a >> ms >> b >> percent) || a != ':' || b != ':'
                || tick > UINT32_MAX || ms == 0 || ms > 600000 || !std::isfinite(percent)
                || percent <= 0 || percent >= 100) return false;
        in >> std::ws;
        if (!in.eof()) return false;
        out = {static_cast<uint32_t>(tick), static_cast<uint32_t>(ms), percent};
        return true;
    }

    class scheduler_clock_fault {
    public:
        using clock = std::chrono::steady_clock;
        void begin(clock::time_point now, test_clock_skew spec) { start_ = now; spec_ = spec; }
        // Inverse of virtual clock: +percent for ms, then -percent for ms.
        // Returning real deadlines keeps input arrival stamps, sleeps and
        // slack measurements on one clock, including catch-up cutoffs.
        clock::time_point real_deadline(clock::time_point nominal) const {
            if (!spec_.enabled() || nominal <= start_) return nominal;
            const double t = std::chrono::duration<double>(nominal - start_).count();
            const double span = spec_.ms / 1000.0, rate = spec_.percent / 100.0;
            if (t >= 2 * span) return nominal;
            const double real = t <= span * (1 + rate) ? t / (1 + rate)
                : span + (t - span * (1 + rate)) / (1 - rate);
            return start_ + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(real));
        }
    private:
        test_clock_skew spec_;
        clock::time_point start_{};
    };
}
