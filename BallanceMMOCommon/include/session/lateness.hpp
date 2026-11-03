#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace bmmo::session {
    // One tick in microseconds, exactly.  The scheduler's kTickDuration is
    // truncated to 15151 us; dividing by it instead would only scale every
    // slack by 3.3e-5 (no drift: the deadlines are spaced by the same
    // truncated period either way), so this is the design's tick and nothing
    // more.
    inline constexpr double kLatenessTickMicros = 1000000.0 / 66.0;

    // A sample's slack in ticks: how long before `deadline` the datagram
    // `arrival`d.  Any clock (the server passes steady_clock time points);
    // computed in double microseconds, so the same expression builds on MSVC
    // and GCC whatever the clock's period is.
    template <class TimePoint>
    double lateness_slack_ticks(TimePoint deadline, TimePoint arrival) {
        return std::chrono::duration<double, std::micro>(deadline - arrival).count() / kLatenessTickMicros;
    }

    // The q-quantile of `values` (copied, sorted): the element at floor(q * n),
    // clamped to the last.  0 for an empty set.  Lower nearest rank, so q01 of
    // a 264-sample reference is its third-smallest sample and q10 of a window
    // of 66 its seventh.
    inline double lateness_quantile(std::vector<double> values, double q) {
        if (values.empty()) return 0.0;
        std::sort(values.begin(), values.end());
        size_t index = static_cast<size_t>(std::floor(q * static_cast<double>(values.size())));
        if (index >= values.size()) index = values.size() - 1;
        return values[index];
    }

    // LB1: first arrivals, including redundant and already stepped frames.
    class arrival_ring {
    public:
        using time_point = std::chrono::steady_clock::time_point;
        struct entry {
            uint32_t tick = 0;
            time_point at{}, seen{};
            bool as_newest = false;
            bool used = false;
        };
        bool record(uint32_t tick, time_point at, time_point seen, bool newest) {
            auto& e = entries_[tick % entries_.size()];
            if (e.used && static_cast<int32_t>(tick - e.tick) <= 0) return false;
            e = {tick, at, std::max(at, seen), newest, true};
            return true;
        }
        const entry* find(uint32_t tick) const {
            const auto& e = entries_[tick % entries_.size()];
            return e.used && e.tick == tick ? &e : nullptr;
        }
    private:
        std::array<entry, 256> entries_{};
    };

    class lateness_baseline {
    public:
        using time_point = arrival_ring::time_point;
        static constexpr size_t kSamples = 264;
        static constexpr double kFloorTicks = 3.0;
        // Reassignments retain the measurement, but retire an old correction.
        void assigned(uint32_t epoch, bool startup, uint32_t tick) {
            epoch_ = epoch;
            assigned_ = true;
            startup_ = startup;
            assigned_tick_ = tick;
        }
        void settled(uint32_t epoch, time_point at) {
            if (!assigned_ || epoch != epoch_ || started_) return;
            start_ = at;
            started_ = true;
        }
        bool timeout(uint32_t tick, time_point now) {
            if (!assigned_ || startup_ || started_
                    || static_cast<int32_t>(tick - assigned_tick_) < 660) return false;
            settled(epoch_, now);
            return true;
        }
        bool sample(uint32_t tick, double slack, time_point at) {
            if (!started_ || at < start_ || frozen() || (any_ && static_cast<int32_t>(tick - newest_) <= 0))
                return false;
            newest_ = tick;
            any_ = true;
            samples_.push_back(slack);
            if (frozen()) {
                auto sorted = samples_;
                std::sort(sorted.begin(), sorted.end());
                value_ = std::max(kFloorTicks, 0.5 * (sorted[kSamples / 2 - 1] + sorted[kSamples / 2]));
            }
            return true;
        }
        void rebase(double delta) {
            if (frozen()) value_ += delta;
            else for (auto& sample: samples_) sample += delta;
        }
        bool frozen() const { return samples_.size() == kSamples; }
        double value() const { return value_; }
        size_t samples() const { return samples_.size(); }
        uint32_t epoch() const { return epoch_; }
    private:
        std::vector<double> samples_;
        uint32_t epoch_ = 0, newest_ = 0, assigned_tick_ = 0;
        time_point start_{};
        double value_ = 0;
        bool assigned_ = false, startup_ = true, started_ = false, any_ = false;
    };

    struct patience_member {
        bool has = false, settled = false, held = false, waived = false, counted = false;
        double baseline = 0, ns = 0;
    };

    // The two timers' margins: M after the last other counted member, M2
    // after the counted majority (late-player design 6.1 R1).
    inline constexpr double kQuorumLastMargin = 4.0;
    inline constexpr double kQuorumWideMargin = 10.0;
    struct quorum_member : patience_member {
        uint32_t player = 0;
        // exempt: held, re-anchored, or below its cursor without a frame.
        bool ready = false, has_baseline = false, exempt = false;
    };
    struct quorum_timer {
        double last = 0, wide = 0; // leads before the deadline; zero means deadline
        double lead() const { return std::max(last, wide); }
    };
    template<class Member>
    inline quorum_timer quorum_patience(const std::vector<Member>& members, size_t p, double now,
                                        double margin = kQuorumLastMargin, double wide_margin = kQuorumWideMargin) {
        quorum_timer timer;
        if (!members[p].counted) return timer;
        size_t counted = 0, others = 0;
        std::vector<double> seen;
        double latest = 0;
        for (size_t i = 0; i < members.size(); ++i) {
            const auto& m = members[i];
            if (!m.counted) continue;
            ++counted;
            if (i != p) ++others;
            if (!m.has || m.ns - m.baseline > now) continue;
            seen.push_back(m.ns);
            if (i != p) latest = std::max(latest, m.ns);
        }
        size_t other_seen = seen.size();
        if (members[p].has && members[p].ns - members[p].baseline <= now) --other_seen;
        if (others && other_seen == others) timer.last = std::max(0.0, members[p].baseline - latest - margin);
        const size_t majority = (counted + 1) / 2;
        if (counted >= 4 && seen.size() >= majority) {
            std::sort(seen.begin(), seen.end());
            timer.wide = std::max(0.0, members[p].baseline - std::max(0.0, seen[majority - 1]) - wide_margin);
        }
        return timer;
    }
    // When the quorum would have stepped this tick. Off-mode never sleeps on
    // these timers, so the expiries between real wakeups are reconstructed:
    // otherwise a timer that expired before the next post would vanish from
    // the would-cut count when that post supplied the frame.
    inline double quorum_step_time(const std::vector<quorum_member>& members) {
        std::vector<double> events{0};
        for (const auto& m: members) if (m.has && m.ns < m.baseline) events.push_back(m.ns - m.baseline);
        std::sort(events.begin(), events.end());
        double earliest = 0;
        for (const double at: events) {
            double candidate = at;
            bool paced = false;
            for (const auto& m: members) if (m.has && m.ns - m.baseline <= at) paced = true;
            for (size_t i = 0; i < members.size(); ++i) {
                const auto& m = members[i];
                if ((m.has && m.ns - m.baseline <= at) || (paced && (m.settled || m.held || m.waived))) continue;
                candidate = std::max(candidate, -quorum_patience(members, i, at).lead());
            }
            earliest = std::min(earliest, candidate);
        }
        return earliest;
    }

    // The patience margin (design 2.6): a player's patience ends this many
    // ticks AFTER the q01 of its frozen reference, `r01 - 1`, so the margin
    // above its usual tail is `r10 - r01 + 1` ticks, sized from its own spread.
    // A miss (design 2.1) is a sample after that patience, so the meter's miss
    // rule uses the same number: change one and the raise rule and the
    // session's cut-off move together.
    //
    // A named knob because the plan step 7 measurement found the newest-frame
    // sampling under-samples the late tail under +-3 ticks of jitter (r10 about
    // 5.6 against a per-datagram q10 of about 3.7): a reference taken from
    // those samples sits early, and a patience anchored at r01 - 1 may cut off
    // more frames than 2.6 expects.  The runner's S1 patience line counts the
    // frames patience cut off that the deadline would still have taken, so the
    // A0j arm shows whether 1 is enough.
    inline constexpr double kPatienceMarginTicks = 1.0;

    enum class lateness_action : uint8_t {
        none,
        raise,     // run further ahead by k
        lower,     // k < 0
        give_up,   // a third raise was due after two that did not help: no more decisions this assignment
    };

    // What the meter is doing, for the S1 summary and for the steps that act on
    // it (patience, design 2.6; the SessionLead decision, 2.3).
    enum class lateness_state : uint8_t {
        baseline,         // no baseline samples yet
        watching,         // reference set, decisions possible
        pending,          // a decision was taken and is not acknowledged
        cooldown,         // acknowledged; no decision before cooldown_until()
        given_up,         // stopped deciding until the next assignment
    };

    inline const char* lateness_state_name(lateness_state s) {
        switch (s) {
            case lateness_state::baseline: return "baseline";
            case lateness_state::watching: return "watching";
            case lateness_state::pending: return "pending";
            case lateness_state::cooldown: return "cooldown";
            case lateness_state::given_up: return "given-up";
        }
        return "?";
    }

    struct lateness_decision {
        lateness_action action = lateness_action::none;
        int k = 0;              // ticks; positive = run further ahead
        uint32_t streak = 0;    // consecutive ticks the rule held
        uint32_t id = 0;        // what acknowledge() names; unique for the meter's life, across resets
    };

    // What of a decision of `k` ticks an ack's `applied` may move the offset
    // by: between 0 and k, on k's side of 0.  The ack is the client's word,
    // and an offset a client could set at will would be the one piece of
    // lead state it controls (a resync anchors at it, design 2.8).
    inline int lateness_bound_applied(int applied, int k) {
        const int lo = k < 0 ? k : 0, hi = k > 0 ? k : 0;
        return applied < lo ? lo : (applied > hi ? hi : applied);
    }

    // Revision 3's meter (late-player design 2.1, LB2): a member's lateness
    // against the others' (L: their median, L': their earliest) and against
    // its own baseline (the absolute rule), each judged over 66 samples.
    inline std::pair<double, double> relative_lateness(const std::vector<double>& normalized, size_t member) {
        std::vector<double> others;
        for (size_t index = 0; index < normalized.size(); ++index)
            if (index != member) others.push_back(std::max(0.0, normalized[index]));
        std::sort(others.begin(), others.end());
        return {normalized[member] - others[others.size() / 2], normalized[member] - others.front()};
    }

    struct relative_decision : lateness_decision {
        const char* rule = "none";
    };

    class relative_meter {
    public:
        static constexpr size_t kWindow = 66;
        static constexpr uint32_t kFreshTicks = 8;
        static constexpr int kMinRaise = 2;
        static constexpr uint32_t kCooldownTicks = 132;
        static constexpr int kMaxUnrestoredRaises = 2;
        static constexpr double kAbsoluteMissTicks = 3.0;
        static constexpr double kLowerMarginTicks = 4.0;
        static constexpr uint32_t kRaiseTicks = 132;
        static constexpr uint32_t kMinorityPersistTicks = 660;
        static constexpr uint32_t kSharedPersistTicks = 660;
        static constexpr uint32_t kLowerTicks = 660;

        void reset(uint32_t base, bool keep_absolute = false) {
            const auto previous = *this;
            const int offset = offset_;
            const uint32_t sequence = sequence_;
            *this = relative_meter{};
            offset_ = offset;
            sequence_ = sequence;
            base_ = discard_below_ = base;
            if (keep_absolute) {
                slack_ = previous.slack_;
                normalized_ = previous.normalized_;
                absolute_samples_ = previous.absolute_samples_;
                absolute_evaluated_ = previous.absolute_evaluated_;
                absolute_at_ = previous.absolute_at_;
                absolute_streak_ = previous.absolute_streak_;
                misses_total_ = previous.misses_total_;
                absolute_shared_ = previous.absolute_shared_;
            }
        }
        bool sample_absolute(uint32_t tick, double slack, double baseline) {
            if (tick < discard_below_ || (absolute_samples_ && tick <= newest_absolute_)) return false;
            newest_absolute_ = tick;
            ++absolute_samples_;
            if (slack < kAbsoluteMissTicks) ++misses_total_;
            slack_.sample(slack);
            normalized_.sample(baseline - slack);
            return true;
        }
        bool sample_relative(uint32_t tick, double relative, double minority) {
            if (tick < discard_below_ || (relative_samples_ && tick <= newest_relative_)) return false;
            newest_relative_ = tick;
            ++relative_samples_;
            relative_.sample(relative);
            minority_.sample(minority);
            return true;
        }
        relative_decision on_tick(uint32_t tick, bool shared) {
            if (shared != absolute_shared_) absolute_streak_ = 0;
            absolute_shared_ = shared;
            if (absolute_samples_ != absolute_evaluated_) {
                absolute_evaluated_ = absolute_samples_;
                absolute_at_ = tick;
            }
            if (relative_samples_ != relative_evaluated_) {
                relative_evaluated_ = relative_samples_;
                relative_at_ = tick;
            }
            last_tick_ = tick;
            const bool relative_majority = 2 * relative_misses() > kWindow;
            const bool minority_majority = 2 * minority_misses() > kWindow;
            const bool absolute = absolute_majority();
            if (judge_restore_ && tick >= cooldown_until_) {
                judge_restore_ = false;
                if (!relative_majority && !minority_majority && !absolute) unrestored_raises_ = 0;
            }
            if (relative_evaluated_ && tick - relative_at_ <= kFreshTicks) {
                relative_streak_ = relative_majority ? relative_streak_ + 1 : 0;
                minority_streak_ = minority_majority ? minority_streak_ + 1 : 0;
                const bool early = offset_ > 0 && relative_.size() == kWindow
                    && relative_quantile(.95) < -kLowerMarginTicks && !absolute;
                lower_streak_ = early ? lower_streak_ + 1 : 0;
            }
            if (absolute_evaluated_ && tick - absolute_at_ <= kFreshTicks)
                absolute_streak_ = absolute ? absolute_streak_ + 1 : 0;
            if (pending_.action != lateness_action::none || given_up_ || tick < cooldown_until_) return {};
            relative_decision decision;
            decision.action = lateness_action::raise;
            if (!at_cap_ && relative_streak_ >= kRaiseTicks) {
                decision.k = std::max(kMinRaise, static_cast<int>(std::ceil(relative_quantile(.9))));
                decision.rule = "relative";
                decision.streak = relative_streak_;
            }
            if (!at_cap_ && minority_streak_ >= kMinorityPersistTicks) {
                const int amount = std::max(kMinRaise, static_cast<int>(std::ceil(minority_quantile(.9))));
                if (amount > decision.k) {
                    decision.k = amount;
                    decision.rule = "minority";
                    decision.streak = minority_streak_;
                }
            }
            if (!at_cap_ && !(shared && minority_majority)
                    && absolute_streak_ >= (shared ? kSharedPersistTicks : kRaiseTicks)) {
                const int amount = static_cast<int>(std::ceil(normalized_.quantile(.5, shared ? kSharedPersistTicks : kRaiseTicks)));
                if (amount > decision.k) {
                    decision.k = amount;
                    decision.rule = shared ? "shared-absolute" : "absolute";
                    decision.streak = absolute_streak_;
                }
            }
            if (decision.k > 0 && unrestored_raises_ >= kMaxUnrestoredRaises) {
                given_up_ = true;
                decision.action = lateness_action::give_up;
                return decision;
            }
            if (decision.k == 0 && lower_streak_ >= kLowerTicks && !absolute) {
                decision.k = -std::min({offset_, static_cast<int>(std::floor(-relative_quantile(.95))) - 1,
                    static_cast<int>(std::floor(slack_quantile(.1))) - static_cast<int>(kAbsoluteMissTicks)});
                if (decision.k >= 0) return {};
                decision.action = lateness_action::lower;
                decision.rule = "lower";
                decision.streak = lower_streak_;
            }
            if (decision.k == 0) return {};
            decision.id = ++sequence_;
            pending_ = decision;
            return decision;
        }
        bool acknowledge(uint32_t id, int applied, uint32_t effective) {
            if (pending_.action == lateness_action::none || pending_.id != id) return false;
            applied = lateness_bound_applied(applied, pending_.k);
            if (pending_.action == lateness_action::raise && pending_capped_) at_cap_ = true;
            if (pending_.action == lateness_action::lower && applied < 0) at_cap_ = false;
            pending_capped_ = false;
            offset_ = std::max(0, offset_ + applied);
            cooldown_until_ = effective + static_cast<uint32_t>(std::abs(pending_.k)) * 2 + kCooldownTicks;
            if (pending_.action == lateness_action::raise) {
                ++unrestored_raises_;
                judge_restore_ = true;
            }
            discard_below_ = effective;
            relative_.clear();
            minority_.clear();
            slack_.clear();
            normalized_.clear();
            relative_streak_ = minority_streak_ = absolute_streak_ = lower_streak_ = 0;
            pending_ = {};
            return true;
        }
        bool cap_pending(uint32_t id, int amount, bool latch = true) {
            if (pending_.action == lateness_action::none || pending_.id != id) return false;
            pending_.k = amount;
            pending_capped_ = latch;
            return true;
        }
        bool decline(uint32_t id) {
            if (!withdraw(id)) return false;
            at_cap_ = true;
            return true;
        }
        bool defer(uint32_t id, uint32_t until_tick) {
            if (!withdraw(id)) return false;
            cooldown_until_ = std::max(cooldown_until_, until_tick);
            return true;
        }
        uint32_t base() const { return base_; }
        uint32_t cooldown_until() const { return cooldown_until_; }
        bool at_cap() const { return at_cap_; }
        lateness_action pending_action() const { return pending_.action; }
        int pending_k() const { return pending_.k; }
        uint32_t pending_id() const { return pending_.id; }
        uint64_t samples_total() const { return absolute_samples_; }
        uint64_t misses_total() const { return misses_total_; }
        uint32_t raise_streak() const { return std::max({relative_streak_, minority_streak_, absolute_streak_}); }
        uint32_t lower_streak() const { return lower_streak_; }
        void set_offset(int offset) { offset_ = std::max(0, offset); }
        int offset() const { return offset_; }
        size_t relative_misses() const { return relative_.misses(kQuorumLastMargin, false); }
        size_t minority_misses() const { return minority_.misses(kQuorumLastMargin, false); }
        size_t absolute_misses() const { return slack_.misses(kAbsoluteMissTicks, true); }
        bool absolute_majority() const { return 2 * absolute_misses() > kWindow; }
        double relative_quantile(double quantile) const { return relative_.quantile(quantile); }
        double minority_quantile(double quantile) const { return minority_.quantile(quantile); }
        double slack_quantile(double quantile) const { return slack_.quantile(quantile); }
        lateness_state state() const {
            if (given_up_) return lateness_state::given_up;
            if (pending_.action != lateness_action::none) return lateness_state::pending;
            if (!absolute_samples_) return lateness_state::baseline;
            return last_tick_ < cooldown_until_ ? lateness_state::cooldown : lateness_state::watching;
        }

    private:
        bool withdraw(uint32_t id) {
            if (pending_.action == lateness_action::none || pending_.id != id) return false;
            pending_ = {};
            pending_capped_ = false;
            relative_streak_ = minority_streak_ = absolute_streak_ = 0;
            return true;
        }
        class window {
        public:
            explicit window(size_t capacity): capacity_(capacity) {}
            void sample(double value) {
                values_.push_back(value);
                if (values_.size() > capacity_) values_.pop_front();
            }
            void clear() { values_.clear(); }
            size_t size() const { return values_.size(); }
            double quantile(double quantile, size_t count = kWindow) const {
                const auto first = values_.end() - static_cast<std::ptrdiff_t>(std::min(count, size()));
                return lateness_quantile(std::vector<double>(first, values_.end()), quantile);
            }
            size_t misses(double threshold, bool below) const {
                const auto first = values_.end() - static_cast<std::ptrdiff_t>(std::min(kWindow, size()));
                return static_cast<size_t>(std::count_if(first, values_.end(), [threshold, below](double value) {
                    return below ? value < threshold : value > threshold;
                }));
            }
        private:
            size_t capacity_;
            std::deque<double> values_;
        };
        window relative_{kWindow}, minority_{kWindow}, slack_{kSharedPersistTicks}, normalized_{kSharedPersistTicks};
        uint64_t relative_samples_ = 0, absolute_samples_ = 0, relative_evaluated_ = 0, absolute_evaluated_ = 0;
        uint32_t newest_relative_ = 0, newest_absolute_ = 0, relative_at_ = 0, absolute_at_ = 0, last_tick_ = 0;
        uint32_t relative_streak_ = 0, minority_streak_ = 0, absolute_streak_ = 0, lower_streak_ = 0;
        uint32_t sequence_ = 0, cooldown_until_ = 0, discard_below_ = 0;
        relative_decision pending_;
        uint64_t misses_total_ = 0;
        uint32_t base_ = 0;
        bool pending_capped_ = false, at_cap_ = false, absolute_shared_ = false;
        int offset_ = 0, unrestored_raises_ = 0;
        bool judge_restore_ = false, given_up_ = false;
    };

    struct patience_verdict {
        bool step = false;        // the tick may be stepped now
        bool early = false;       // stepped before the deadline with a member unsettled
        bool wait = false;        // !step and a member's patience falls before the deadline
        double wait_ticks = 0.0;  // when wait: ticks from now to the earliest such patience
    };

    // The runner's early-step test for tick T.  `due` is now >= deadline(T)
    // and `to_deadline` is deadline(T) - now in ticks (positive before it).
    //
    // Patience off: every member has its frame, or the deadline passed -
    // exactly the rule the runner had before patience existed.
    //
    // Patience on: every member is settled, or past its patience
    // (to_deadline <= lead), or waived; the deadline still steps regardless.
    // When the tick cannot step yet, `wait` names the earliest patience among
    // the members still waited for, so the runner wakes there: a
    // blacked-out member posts nothing that would wake it otherwise.
    //
    // The pacing floor.  Settled-by-cursor and waived members are exempt
    // only while some member's real frame for T is here (`has`): the waiver
    // means "wait only for the others", and the cursor rule means "do not
    // wait for a frame that will never come", but either needs somebody
    // whose real-time clock produced T.  Before patience that bound was
    // implicit (a tick stepped early only once every frame was in, so the
    // session never got ahead of its latest client).  Without it a solo
    // session, or members waived together (a shared outage), or a waived
    // member beside a freshly re-anchored one, would step every tick at once
    // - max_catch_up_ticks per pass, every post() waking another pass - and
    // run hundreds of ticks ahead of the deadline schedule, after which every
    // frame its players send is stale. With nobody's frame here, no counted
    // reference exists, so the tick waits for the deadline.
    inline patience_verdict patience_step_test(const std::vector<patience_member>& members, bool due,
                                               double to_deadline, bool patience,
                                               double margin = kQuorumLastMargin, double wide_margin = kQuorumWideMargin) {
        patience_verdict v;
        if (!patience) {
            // A member whose tick lies in a lead reset's held range, or
            // below a re-anchor's base, has nothing coming for it (design
            // 2.3: settled() replaces has() there; plan step 20 for the
            // re-anchor), behind the same pacing floor as below: only while
            // some member's real frame is here, so a solo held player still
            // steps at the deadline.  Without a reset no member is held, and
            // this is exactly the rule the runner had before patience
            // existed.
            bool paced = false;
            for (const auto& m: members) if (m.has) { paced = true; break; }
            bool all = true;
            for (const auto& m: members) if (!m.has && !(m.held && paced)) { all = false; break; }
            v.step = all || due;
            return v;
        }
        bool paced = false;
        for (const auto& m: members) if (m.has) { paced = true; break; }
        bool blocked = false, unsettled = false;
        for (size_t index = 0; index < members.size(); ++index) {
            const auto& member = members[index];
            if (member.has) continue;
            if (member.settled) {
                if (paced) continue;
            } else {
                unsettled = true;
                if (member.waived && paced) continue;
            }
            const double lead = quorum_patience(members, index, -to_deadline, margin, wide_margin).lead();
            if (to_deadline <= lead) continue;
            const double until = to_deadline - lead;
            if (!blocked || until < v.wait_ticks) v.wait_ticks = until;
            blocked = true;
        }
        v.step = due || !blocked;
        v.early = v.step && !due && unsettled;
        v.wait = !v.step && blocked;
        return v;
    }

    // The streak waiver (design 2.6).  Once p was substituted at T because
    // the session stepped without its frame, its patience is waived from the
    // next tick on: the session waits only for the others (and takes p's
    // frames that happen to be there).  The waiver ends with the first sample
    // of p that arrived inside its patience, not with p's first FRESH frame:
    // the member whose arrivals are structurally the session's last would,
    // without patience, always arrive just after the step, never be fresh
    // again, and stay substituted for good.  During a blackout or persistent
    // lateness only the first tick of the episode waits for p - as long as
    // some other member's real frame paces the tick (the pacing floor in
    // patience_step_test); with nobody's frame there, the deadline applies.
    class patience_waiver {
    public:
        // p was substituted at a tick it had not settled; returns true when
        // this starts an episode.
        bool substituted() {
            const bool starts = !waived_;
            waived_ = true;
            if (starts) ++episodes_;
            return starts;
        }
        // A seen sample of p (slack in ticks before its frame's deadline)
        // and p's lead: an arrival inside patience ends the waiver.  Returns
        // true when it did.
        bool sample(double seen_slack, double lead) {
            if (!waived_ || seen_slack < lead) return false;
            waived_ = false;
            return true;
        }
        // A new assignment (a late join made ready, a resync): the player is
        // measured anew (design 2.8), so an episode of its old assignment
        // ends here.  The episode count is kept for the S1 patience line.
        void reassigned() { waived_ = false; }
        bool waived() const { return waived_; }
        uint64_t episodes() const { return episodes_; }

    private:
        bool waived_ = false;
        uint64_t episodes_ = 0;
    };

    // ------------------------------------------------------------ the wait
    // Step R12 (late-player design 2.5 "As built (step R12)", the user's
    // decision of 2026-10-27: the reach's margin is 8 + the server's wait,
    // and the server says its wait in every relay datagram, protocol 13).
    //
    // A notice of p's substituted tick T leaves with the server's step of T,
    // and the runner steps T without p's frame only when the step test above
    // lets it: the datagrams p samples its header lag from leave when the
    // test passed with every frame in, so the notice is later than they are
    // by exactly how much longer the test would have held T for p's missing
    // frame - p's WAIT at T, in ticks:
    //   - patience off: until the deadline, `to_deadline` (deadline(T) - the
    //     instant the runner stepped T, in ticks; the step's earliness);
    //   - patience on: until p's patience, to_deadline - lead; nothing
    //     while p is waived and another member's frame paces T (the pacing
    //     floor).
    // Never negative: a step at or after the deadline (a substituted tick, a
    // catch-up after a stall) would have waited for nothing.  The case is an
    // ordinary late frame (p unsettled); a tick a lead reset froze or a
    // re-anchor skipped is substituted without a wait, which this
    // over-states (the safe side: a deeper reach costs CPU, not a resync).
    inline double server_wait_ticks(double to_deadline, const patience_member& p, bool patience,
                                    bool paced_by_others, double lead) {
        if (!patience) return std::max(0.0, to_deadline);
        if (p.waived && paced_by_others) return 0.0;
        return std::max(0.0, to_deadline - lead);
    }

    // What one member's relay header carries (session_remote_input_msg's
    // `wait`): the largest wait of its last kWindows windows of
    // kWindowTicks server ticks - the same span as the client's header-lag
    // p99 (rollback.hpp reach_meter: 4 windows of 60 own ticks) - rounded up
    // to whole ticks, at most 255.  The largest, not a percentile: the
    // notice that matters is the one of a tick the server had to wait for,
    // and its datagram's own wait (0, stepped at the deadline) says nothing
    // about it; the steps around it say how long the wait was.
    class server_wait_meter {
    public:
        static constexpr uint32_t kWindowTicks = 60;
        static constexpr size_t kWindows = 4;

        void sample(uint32_t tick, double wait) {
            const uint32_t window = tick / kWindowTicks;
            if (!any_ || static_cast<int32_t>(window - newest_) > 0) {
                newest_ = window;
                any_ = true;
            }
            auto& slot = slots_[window % kWindows];
            if (!slot.used || slot.window != window) {
                if (slot.used && static_cast<int32_t>(window - slot.window) < 0) return;   // older than the slot's
                slot = window_slot{};
                slot.used = true;
                slot.window = window;
            }
            if (wait > slot.max) slot.max = wait;
        }

        // The header's value: 0 before any sample.
        uint8_t value() const {
            double max = 0.0;
            for (const auto& slot: slots_)
                if (slot.used && any_ && static_cast<int32_t>(newest_ - slot.window) >= 0
                        && newest_ - slot.window < kWindows && slot.max > max)
                    max = slot.max;
            const double up = std::ceil(max);
            return up >= 255.0 ? 255 : static_cast<uint8_t>(up);
        }

    private:
        struct window_slot {
            bool used = false;
            uint32_t window = 0;
            double max = 0.0;
        };
        std::array<window_slot, kWindows> slots_{};
        uint32_t newest_ = 0;
        bool any_ = false;
    };
}
