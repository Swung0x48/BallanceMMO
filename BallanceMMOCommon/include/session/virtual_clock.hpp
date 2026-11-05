#pragma once

// A clock on virtual time (docs/canned-tests-plan.md, step K4c, seam C4): now()
// is what advance_to() last set; a wait ends when its time comes or its
// condition holds, never by the OS.  For a canned test's coordinator (stage 1,
// T1): every thread of a process waits through one virtual_clock, and the
// coordinator advances it to the earliest pending wake (next_wake) once the
// process has nothing left to do before it.
//
// It never starts at 0 (kDefaultStart): a steady value of 0 on the wire means
// "absent" (a SessionAssign's server_us, a clock ack's), so the session epoch
// a coordinator picks must be past it.
//
// A condition-variable wait rechecks every kRecheck of real time besides being
// notified: advance_to notifies the waiting variables without the waiters'
// locks, so a notify can land between a waiter's check and its wait.
// CK-free and header-only.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include "session_clock.hpp"

namespace bmmo::session {

    class virtual_clock final : public session_clock {
    public:
        static constexpr auto kRecheck = std::chrono::milliseconds(1);
        static constexpr time_point kDefaultStart = time_point{} + std::chrono::hours(1);

        explicit virtual_clock(time_point start = kDefaultStart, int64_t utc_us_at_start = 0)
            : start_(start), now_(start), utc_at_start_(utc_us_at_start) {}

        time_point now(clock_site) override {
            std::lock_guard lk(mutex_);
            return now_;
        }
        int64_t utc_us(clock_site) override {
            std::lock_guard lk(mutex_);
            return utc_at_start_ + std::chrono::duration_cast<std::chrono::microseconds>(now_ - start_).count();
        }

        void sleep_for(clock_site, clock_type::duration d) override {
            std::unique_lock lk(mutex_);
            const auto at = now_ + d;
            const auto id = add(at, &changed_);
            changed_.wait(lk, [&] { return now_ >= at; });
            remove(id);
        }
        bool wait_until(clock_site site, std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                        time_point t, const ready_test& ready) override {
            uint64_t id;
            {
                std::lock_guard lk(mutex_);
                id = add(t, &cv);
            }
            while (!ready() && now(site) < t) cv.wait_for(lock, kRecheck);
            std::lock_guard lk(mutex_);
            remove(id);
            return ready();
        }
        bool wait_for(clock_site site, std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                      clock_type::duration d, const ready_test& ready) override {
            return wait_until(site, cv, lock, now(site) + d, ready);
        }

        // The coordinator's side: the earliest time a thread waits for (none
        // when nothing waits for a time), how many threads wait, and the step.
        std::optional<time_point> next_wake() const {
            std::lock_guard lk(mutex_);
            std::optional<time_point> earliest;
            for (const auto& w: waits_)
                if (w.at != time_point::max() && (!earliest || w.at < *earliest)) earliest = w.at;
            return earliest;
        }
        size_t waiting() const {
            std::lock_guard lk(mutex_);
            return waits_.size();
        }
        void advance_to(time_point t) {
            std::lock_guard lk(mutex_);
            now_ = std::max(now_, t);
            for (const auto& w: waits_) w.cv->notify_all();
        }

    private:
        struct wait_entry {
            uint64_t id;
            time_point at;
            std::condition_variable* cv;
        };
        uint64_t add(time_point at, std::condition_variable* cv) {
            waits_.push_back({++last_id_, at, cv});
            return last_id_;
        }
        void remove(uint64_t id) {
            waits_.erase(std::remove_if(waits_.begin(), waits_.end(), [id](const wait_entry& w) { return w.id == id; }),
                         waits_.end());
        }

        mutable std::mutex mutex_;
        std::condition_variable changed_;
        time_point start_, now_;
        int64_t utc_at_start_;
        std::vector<wait_entry> waits_;
        uint64_t last_id_ = 0;
    };

}
