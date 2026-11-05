#pragma once

// A clock on virtual time (docs/canned-tests-plan.md, step K4c, seam C4): now()
// is what advance_to() last set; a wait ends when its time comes or its
// condition holds, never by the OS.  For a canned test's coordinator (stage 1,
// T1): every thread of a process that takes part waits through one
// virtual_clock; wait_idle() returns once all of them wait (with the earliest
// time one waits for), and the coordinator advances the clock to a time.
//
// Idle is exact (step T1a): a wait the clock has ended - by notify() or by an
// advance_to() that reached its time - counts as running from that moment, not
// from when its thread gets to run, so the coordinator never advances past a
// thread that has been woken and not yet run.  A host's notifies go through
// the clock for that (session_clock::notify).
//
// It never starts at 0 (kDefaultStart): a steady value of 0 on the wire means
// "absent" (a SessionAssign's server_us, a clock ack's), so the session epoch
// a coordinator picks must be past it.
//
// A condition-variable wait also rechecks its condition every kRecheck of real
// time, a backstop for a notify made with a plain notify_all.
// CK-free and header-only.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
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
            const uint64_t id = add(at, &changed_);
            while (now_ < at) {
                waits_[id].woken = false;
                signal_if_idle();
                changed_.wait(lk);
            }
            waits_.erase(id);
        }
        bool wait_until(clock_site, std::condition_variable& cv, std::unique_lock<std::mutex>& lock, time_point t,
                        const ready_test& ready) override {
            uint64_t id = 0, seen = 0;
            {
                std::lock_guard lk(mutex_);
                id = add(t, &cv);
            }
            for (;;) {
                {
                    std::lock_guard lk(mutex_);
                    seen = waits_[id].generation;
                }
                if (ready()) break;
                {
                    std::lock_guard lk(mutex_);
                    if (now_ >= t) break;
                    auto& w = waits_[id];
                    if (w.generation != seen) continue;   // ended meanwhile: look again
                    w.woken = false;
                    signal_if_idle();
                }
                cv.wait_for(lock, kRecheck);
            }
            std::lock_guard lk(mutex_);
            waits_.erase(id);
            return ready();
        }
        bool wait_for(clock_site site, std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                      clock_type::duration d, const ready_test& ready) override {
            return wait_until(site, cv, lock, now(site) + d, ready);
        }
        void notify(clock_site, std::condition_variable& cv) override {
            std::lock_guard lk(mutex_);
            for (auto& [id, w]: waits_)
                if (w.cv == &cv) end(w);
            cv.notify_all();
        }

        // The coordinator's side.  `participants`: the threads that take part
        // (the server's network loop and runner; the headless client's loop) -
        // only they may wait through this clock.
        void set_participants(size_t participants) {
            std::lock_guard lk(mutex_);
            participants_ = participants;
            signal_if_idle();
        }
        // Blocks until every participant waits; the earliest time one waits
        // for (none: they all wait for something to happen).
        std::optional<time_point> wait_idle() {
            std::unique_lock lk(mutex_);
            idle_.wait(lk, [&] { return idle_locked(); });
            return next_wake_locked();
        }
        bool idle() const {
            std::lock_guard lk(mutex_);
            return idle_locked();
        }
        std::optional<time_point> next_wake() const {
            std::lock_guard lk(mutex_);
            return next_wake_locked();
        }
        // The participants waiting now (an ended wait does not count).
        size_t waiting() const {
            std::lock_guard lk(mutex_);
            return waiting_locked();
        }
        // Moves the time forward (never back) and ends every wait it reaches.
        void advance_to(time_point t) {
            std::lock_guard lk(mutex_);
            now_ = std::max(now_, t);
            std::vector<std::condition_variable*> due;
            for (auto& [id, w]: waits_)
                if (w.at <= now_) {
                    end(w);
                    due.push_back(w.cv);
                }
            for (auto* cv: due) cv->notify_all();
        }

    private:
        struct wait_entry {
            time_point at;
            std::condition_variable* cv = nullptr;
            bool woken = true;          // ended, or not yet waiting: does not count
            uint64_t generation = 0;    // one more each time the wait is ended
        };
        uint64_t add(time_point at, std::condition_variable* cv) {
            waits_[++last_id_] = wait_entry{at, cv};
            return last_id_;
        }
        static void end(wait_entry& w) {
            w.woken = true;
            ++w.generation;
        }
        size_t waiting_locked() const {
            return static_cast<size_t>(std::count_if(waits_.begin(), waits_.end(), [](const auto& e) { return !e.second.woken; }));
        }
        bool idle_locked() const { return participants_ && waiting_locked() >= participants_; }
        std::optional<time_point> next_wake_locked() const {
            std::optional<time_point> earliest;
            for (const auto& [id, w]: waits_)
                if (!w.woken && w.at != time_point::max() && (!earliest || w.at < *earliest)) earliest = w.at;
            return earliest;
        }
        void signal_if_idle() {
            if (idle_locked()) idle_.notify_all();
        }

        mutable std::mutex mutex_;
        std::condition_variable changed_, idle_;
        time_point start_, now_;
        int64_t utc_at_start_;
        std::map<uint64_t, wait_entry> waits_;
        uint64_t last_id_ = 0;
        size_t participants_ = 0;
    };

}
