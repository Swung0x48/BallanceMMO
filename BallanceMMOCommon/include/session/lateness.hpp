#pragma once

// The server's lateness meter (late-player design 2.1): how early each
// player's input arrives against the session's deadline, and whether its
// lateness has persisted long enough that its own lead should change.  Pure
// bookkeeping with no clock of its own, so the rule is unit-tested on
// synthetic series and the runner only feeds it.
//
// Samples.  Every uplink datagram gives one sample from its NEWEST frame:
// `slack = (deadline(t_new) - arrival) / tick`, positive when the frame was
// early.  A datagram whose newest tick is not newer than one already sampled
// (a duplicate, or one a later datagram overtook) gives none.  Loss gives no
// sample at all and a frame recovered from the redundancy window is never one,
// so loss cannot look like latency and nothing waits for a frame to be
// classified as lost.
//
// Reference.  q10 and q01 of the samples of the player's first windows of 66
// after an assignment, taken only from windows where at most 10 % of the
// samples fell after the deadline.  Then it is frozen: it is never measured
// again until the next assignment.  Re-measured after an adjustment, a raise
// that was capped or never applied would leave the arrivals late, the
// reference would follow them down, and patience (design 2.6) would move later
// for everybody, for good.
//
// Raise.  A sample below `r01 - 1` is a miss: that frame came after the
// player's patience.  A raise needs more than half of the trailing 66 samples
// to be misses at every evaluated tick for 132 consecutive ticks.  A majority
// takes 34 late samples, so a one-second spike, a CPU stall with its 33-frame
// catch-up, or a blackout (no samples at all) can never raise anything,
// wherever it falls.  Only a tick with a sample in its last 8 counts: during
// a silence the trailing window is frozen on whatever it held, so the streaks
// hold there instead of advancing on stale samples (a spike followed by a
// blackout must not add up to 132).  K = ceil(r10 - q10 of those 66), at least 2: it puts the
// arrivals back where the reference had them, not merely at the deadline.
//
// Lower.  q05 of the trailing 66 above `r10 + 6` at every tick for 660 ticks;
// K = -floor(q05 - r10 - 1), never taking the player's total offset below 0.
// Wider and slower than the raise, which with the cooldown is the hysteresis
// that keeps the lead from oscillating.
//
// Cooldown and gates.  A decision is taken at most once until it is
// acknowledged (the client acks the applied delta, naming the decision by its
// id; an ack of anything but the pending decision is ignored, so one that
// crossed a reassignment changes nothing); after an acknowledgment
// nothing is decided before `E + |k|*2 + 132`, and samples of frames below E
// are discarded.  A raise that did not bring the arrivals back inside patience
// by the end of its cooldown allows one more raise; after a second one the
// meter stops deciding until the next assignment.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
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

    // The median of the other members' references, for a member whose own
    // could not be measured (design 2.1: structurally late from its start).
    // Each quantity separately; an even count takes the mean of the middle two.
    inline double lateness_median(std::vector<double> values) {
        if (values.empty()) return 0.0;
        std::sort(values.begin(), values.end());
        const size_t n = values.size();
        return n % 2 ? values[n / 2] : 0.5 * (values[n / 2 - 1] + values[n / 2]);
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
        measuring,        // no reference yet
        wants_fallback,   // 8 windows without a qualifying one: waits for the others' median
        watching,         // reference set, decisions possible
        pending,          // a decision was taken and is not acknowledged
        cooldown,         // acknowledged; no decision before cooldown_until()
        given_up,         // stopped deciding until the next assignment
    };

    inline const char* lateness_state_name(lateness_state s) {
        switch (s) {
            case lateness_state::measuring: return "measuring";
            case lateness_state::wants_fallback: return "wants-fallback";
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
        double quantile = 0.0;  // the trailing q10 (raise) or q05 (lower) k came from
        int misses = 0;         // misses among the trailing samples at the decision
        int samples = 0;        // trailing samples at the decision
        uint32_t streak = 0;    // consecutive ticks the rule held
        uint32_t id = 0;        // what acknowledge() names; unique for the meter's life, across resets
    };

    class lateness_meter {
    public:
        static constexpr int kWindow = 66;                  // samples per window, and the trailing window
        static constexpr int kReferenceWindows = 4;         // qualifying windows that set the reference
        static constexpr int kReferenceMaxWindows = 8;      // windows after which a reference is set anyway
        static constexpr int kQualifyingMaxLate = kWindow / 10;   // at most 10 % after the deadline: 6 of 66
        static constexpr double kMissMargin = kPatienceMarginTicks;   // a miss is below r01 - 1: after patience
        static constexpr uint32_t kRaiseTicks = 132;        // consecutive ticks of a majority of misses
        static constexpr uint32_t kFreshTicks = 8;          // a tick counts toward a streak only with a sample this recent
        static constexpr int kMinRaise = 2;
        static constexpr double kLowerMargin = 6.0;         // q05 above r10 + 6
        static constexpr uint32_t kLowerTicks = 660;
        static constexpr uint32_t kCooldownTicks = 132;     // plus |k| * 2 after E
        static constexpr int kMaxUnrestoredRaises = 2;

        // A new assignment (session start, late join, resync) with the first
        // tick the player was assigned: samples, windows, reference, streaks
        // and gates start over.  The total offset is the player's
        // (lead_offset, design 2.8) and survives a resync; so does the
        // decision id, so an ack of a decision from before the reset matches
        // nothing after it.
        void reset(uint32_t base) {
            const int offset = offset_;
            const uint32_t sequence = sequence_;
            *this = lateness_meter{};
            offset_ = offset;
            sequence_ = sequence;
            base_ = base;
        }

        // One datagram's sample.  Returns false when it gives none: a tick
        // below the assignment base or below an acknowledged E (frames of the
        // old schedule), or not newer than one already sampled.
        bool sample(uint32_t newest_tick, double slack) {
            if (newest_tick < base_ || newest_tick < discard_below_) return false;
            if (have_sampled_ && newest_tick <= newest_sampled_) return false;
            have_sampled_ = true;
            newest_sampled_ = newest_tick;
            last_slack_ = slack;
            ++samples_total_;

            trailing_[static_cast<size_t>(trailing_head_)] = slack;
            trailing_head_ = (trailing_head_ + 1) % kWindow;
            if (trailing_count_ < kWindow) ++trailing_count_;

            last_miss_ = has_reference_ && is_miss(slack);
            if (last_miss_) ++misses_total_;

            if (!has_reference_) {
                window_.push_back(slack);
                if (static_cast<int>(window_.size()) == kWindow) close_window();
            }
            return true;
        }

        // Once per stepped tick, after the tick's samples: advances the
        // persistence streaks and returns the decision taken at this tick, if
        // any.  A decision leaves the meter pending until acknowledge().
        lateness_decision on_tick(uint32_t tick) {
            lateness_decision none;
            have_tick_ = true;
            last_tick_ = tick;
            if (samples_total_ != samples_at_last_tick_) {
                samples_at_last_tick_ = samples_total_;
                last_sample_tick_ = tick;
            }
            if (!has_reference_ || given_up_) return none;

            const int misses = trailing_misses();
            const bool majority = 2 * misses > kWindow;

            // The end of a raise's cooldown: did it bring the arrivals back
            // inside patience?  If not, it counts against the two allowed.
            if (judge_restore_ && tick >= cooldown_until_) {
                judge_restore_ = false;
                if (!majority) unrestored_raises_ = 0;
            }

            // A tick with no sample in its last kFreshTicks says nothing new
            // about the path: the streaks hold (neither advance nor reset).
            // 8 covers the A4 bursts and the gaps overtaken datagrams leave
            // under jitter, so a steady late path still advances every tick.
            if (fresh(tick)) {
                raise_streak_ = majority ? raise_streak_ + 1 : 0;
                const bool early = offset_ > 0 && trailing_count_ == kWindow
                    && trailing_quantile(0.05) > r10_ + kLowerMargin;
                lower_streak_ = early ? lower_streak_ + 1 : 0;
            }

            if (pending_ || tick < cooldown_until_) return none;

            if (raise_streak_ >= kRaiseTicks) {
                if (unrestored_raises_ >= kMaxUnrestoredRaises) {
                    given_up_ = true;
                    lateness_decision d;
                    d.action = lateness_action::give_up;
                    d.misses = misses;
                    d.samples = trailing_count_;
                    d.streak = raise_streak_;
                    return d;
                }
                const double q10 = trailing_quantile(0.10);
                int k = static_cast<int>(std::ceil(r10_ - q10));
                if (k < kMinRaise) k = kMinRaise;
                return decide(lateness_action::raise, k, q10, misses, raise_streak_);
            }
            if (lower_streak_ >= kLowerTicks) {
                const double q05 = trailing_quantile(0.05);
                int k = -static_cast<int>(std::floor(q05 - r10_ - 1.0));
                if (k < -offset_) k = -offset_;
                if (k < 0) return decide(lateness_action::lower, k, q05, misses, lower_streak_);
            }
            return none;
        }

        // The client applied `applied` ticks of the pending decision `id`, and
        // the server expects the new schedule from `effective` (E) on.  An
        // ack that names anything but the pending decision (none is pending,
        // or a reset came in between) is ignored and returns false.  The total
        // offset moves by what was applied; the reference does NOT (see the
        // header: a raise restores the arrivals to the reference, so moving
        // the reference by the delta would read the restored arrivals as
        // misses and raise again).  The cooldown is the decided k's (design
        // 2.1: E + |k|*2 + 132), whatever part of it was applied.
        bool acknowledge(uint32_t id, int applied, uint32_t effective) {
            if (!pending_ || id != pending_id_) return false;
            const bool was_raise = pending_action_ == lateness_action::raise;
            pending_ = false;
            pending_action_ = lateness_action::none;
            offset_ += applied;
            if (offset_ < 0) offset_ = 0;
            const uint32_t magnitude = static_cast<uint32_t>(pending_k_ < 0 ? -pending_k_ : pending_k_);
            cooldown_until_ = effective + magnitude * 2 + kCooldownTicks;
            discard_below_ = effective;
            trailing_count_ = 0;
            trailing_head_ = 0;
            raise_streak_ = 0;
            lower_streak_ = 0;
            if (was_raise) {
                ++unrestored_raises_;
                judge_restore_ = true;
            }
            return true;
        }

        // The player's total offset as the session keeps it (design 2.8's
        // lead_offset), for a meter that did not see the adjustments that
        // made it (a player re-created, a test).  Never below 0.
        void set_offset(int offset) { offset_ = offset < 0 ? 0 : offset; }

        // The fallback reference (design 2.1): the median of the other
        // members' references, for a member with no qualifying window after 8.
        void adopt_reference(double r10, double r01) {
            if (has_reference_) return;
            set_reference(r10, r01, false);
        }

        // --- what patience (design 2.6) and the decision (2.3) read ---
        bool has_reference() const { return has_reference_; }
        bool reference_measured() const { return has_reference_ && measured_; }
        bool wants_fallback() const { return !has_reference_ && windows_ >= kReferenceMaxWindows; }
        double r10() const { return r10_; }
        double r01() const { return r01_; }
        // A sample after the player's patience.  Only meaningful with a reference.
        bool is_miss(double slack) const { return slack < r01_ - kMissMargin; }
        bool last_miss() const { return last_miss_; }
        double last_slack() const { return last_slack_; }
        bool have_sampled() const { return have_sampled_; }
        uint32_t newest_sampled() const { return newest_sampled_; }

        int trailing_count() const { return trailing_count_; }
        int trailing_misses() const {
            if (!has_reference_) return 0;
            int misses = 0;
            for (int i = 0; i < trailing_count_; ++i) misses += is_miss(trailing_[static_cast<size_t>(i)]) ? 1 : 0;
            return misses;
        }
        // Misses over the trailing window's full size (the rule's denominator:
        // fewer samples than 66 cannot make a majority sooner).
        double miss_fraction() const { return static_cast<double>(trailing_misses()) / kWindow; }
        double trailing_quantile(double q) const {
            return lateness_quantile(std::vector<double>(trailing_.begin(), trailing_.begin() + trailing_count_), q);
        }

        lateness_state state() const {
            if (given_up_) return lateness_state::given_up;
            if (pending_) return lateness_state::pending;
            if (!has_reference_) return windows_ >= kReferenceMaxWindows ? lateness_state::wants_fallback
                                                                        : lateness_state::measuring;
            if (have_tick_ && last_tick_ < cooldown_until_) return lateness_state::cooldown;
            return lateness_state::watching;
        }
        lateness_action pending_action() const { return pending_action_; }
        int pending_k() const { return pending_k_; }
        int offset() const { return offset_; }
        uint32_t base() const { return base_; }
        uint32_t cooldown_until() const { return cooldown_until_; }
        uint32_t raise_streak() const { return raise_streak_; }
        // Whether `tick` had a sample in its last kFreshTicks (the streaks
        // only move on such ticks).  Valid for the tick on_tick() last saw.
        bool fresh(uint32_t tick) const {
            return samples_at_last_tick_ > 0 && tick - last_sample_tick_ <= kFreshTicks;
        }
        uint32_t pending_id() const { return pending_ ? pending_id_ : 0; }
        uint32_t lower_streak() const { return lower_streak_; }
        int windows() const { return windows_; }
        int qualifying_windows() const { return qualifying_; }
        uint64_t samples_total() const { return samples_total_; }
        uint64_t misses_total() const { return misses_total_; }
        int decisions() const { return decisions_; }

    private:
        void close_window() {
            ++windows_;
            int late = 0;
            for (double s: window_) late += s < 0.0 ? 1 : 0;
            if (late <= kQualifyingMaxLate) {
                ++qualifying_;
                reference_samples_.insert(reference_samples_.end(), window_.begin(), window_.end());
            }
            window_.clear();
            // Four qualifying windows set it.  So does the eighth window with
            // at least one behind it: a player whose arrivals cross the
            // deadline often but not always would otherwise never get one.
            if (qualifying_ >= kReferenceWindows || (windows_ >= kReferenceMaxWindows && qualifying_ > 0))
                set_reference(lateness_quantile(reference_samples_, 0.10),
                              lateness_quantile(reference_samples_, 0.01), true);
        }

        void set_reference(double r10, double r01, bool measured) {
            has_reference_ = true;
            measured_ = measured;
            r10_ = r10;
            r01_ = r01;
            reference_samples_.clear();
            reference_samples_.shrink_to_fit();
            window_.clear();
        }

        lateness_decision decide(lateness_action action, int k, double q, int misses, uint32_t streak) {
            pending_ = true;
            pending_action_ = action;
            pending_k_ = k;
            pending_id_ = ++sequence_;
            ++decisions_;
            lateness_decision d;
            d.id = pending_id_;
            d.action = action;
            d.k = k;
            d.quantile = q;
            d.misses = misses;
            d.samples = trailing_count_;
            d.streak = streak;
            return d;
        }

        uint32_t base_ = 0;
        uint32_t discard_below_ = 0;
        uint32_t newest_sampled_ = 0;
        bool have_sampled_ = false;
        double last_slack_ = 0.0;
        bool last_miss_ = false;
        uint64_t samples_total_ = 0, misses_total_ = 0;

        std::array<double, kWindow> trailing_{};
        int trailing_head_ = 0, trailing_count_ = 0;

        std::vector<double> window_;              // the reference window being filled
        std::vector<double> reference_samples_;   // the qualifying windows so far
        int windows_ = 0, qualifying_ = 0;
        bool has_reference_ = false, measured_ = false;
        double r10_ = 0.0, r01_ = 0.0;

        uint32_t raise_streak_ = 0, lower_streak_ = 0;
        bool have_tick_ = false;
        uint32_t last_tick_ = 0;
        uint64_t samples_at_last_tick_ = 0;   // samples_total_ when on_tick() last ran
        uint32_t last_sample_tick_ = 0;       // the tick at which on_tick() last saw a new sample
        uint32_t pending_id_ = 0;
        uint32_t sequence_ = 0;               // decision ids; survives reset()
        bool pending_ = false;
        lateness_action pending_action_ = lateness_action::none;
        int pending_k_ = 0;
        uint32_t cooldown_until_ = 0;
        bool judge_restore_ = false;
        int unrestored_raises_ = 0;
        bool given_up_ = false;
        int offset_ = 0;
        int decisions_ = 0;
    };

    // ---- Patience (late-player design 2.6) ----------------------------------
    //
    // Without it the session waits until deadline(T) for every missing frame,
    // so one late player puts everybody's step on deadlines.  With it, player
    // p is waited for only until
    //
    //     patience_p(T) = min(deadline(T), deadline(T) - (r01_p - 1) ticks),
    //
    // anchored on its FROZEN reference (a reference that followed late
    // arrivals down would move patience later for everybody, for good).  All
    // of it is in ticks before the deadline, the unit samples are in: a frame
    // whose slack is at least the lead arrived inside patience.

    // How many ticks before deadline(T) a member with reference q01 `r01`
    // stops being waited for; 0 means at the deadline (the min() above).
    inline double patience_lead_ticks(double r01) {
        const double lead = r01 - kPatienceMarginTicks;
        return lead > 0.0 ? lead : 0.0;
    }

    // One member's view at the tick being decided.
    struct patience_member {
        bool has = false;              // its frame for T arrived (the rule without patience)
        bool settled = false;          // input_buffer::settled(T): arrived, or below its cursor
        bool waived = false;           // the streak waiver holds
        bool has_statistics = false;   // a frozen reference, measured or adopted
        double r01 = 0.0;              // that reference's q01, when has_statistics
        double lead = 0.0;             // set by patience_assign_leads
        bool capped = false;           // lead is the statistics-less cap, not its own
        // Assigned after the session's start (a late joiner, a resync): the
        // statistics-less cap applies to it.  A member measuring since the
        // start waits for the deadline until its own reference is set.
        bool late_assignment = false;
    };

    // The runner's view of one member (lead still unassigned): `has` and
    // `settled` from its input buffer at T, `waived` from its waiver, and
    // its lateness meter, or null when it has none yet (a member added but
    // not ready).  A member without a meter, or whose meter was reset to a
    // base past the session's start base (a late join or a resync: the
    // server assigns start members the start base and everybody else a
    // later one), is a late assignment.
    inline patience_member patience_member_of(bool has, bool settled, bool waived,
                                              const lateness_meter* meter, uint32_t start_base) {
        patience_member m;
        m.has = has;
        m.settled = settled;
        m.waived = waived;
        m.late_assignment = meter == nullptr || meter->base() > start_base;
        // The frozen reference, measured or adopted from the others' median
        // (design 2.1); a member still measuring has none.
        if (meter != nullptr && meter->has_reference()) {
            m.has_statistics = true;
            m.r01 = meter->r01();
        }
        return m;
    }

    // Every member's lead.  A member with statistics has its own.  One
    // without that was assigned after the start (a late joiner in its first
    // windows, or a member re-measuring after a resync) gets the latest
    // patience among the members that have statistics, i.e. the smallest
    // lead: the others' schedule, so it costs them nothing.  When nobody has
    // statistics that is the deadline, today's behaviour.
    //
    // A member measuring since the session's start gets the deadline until
    // its own reference is set, even when others already have theirs.  The
    // members of the start measure together, but not in step: the newest-frame
    // sampling gives a member whose datagrams overtake each other fewer
    // samples, so the structurally latest member (the one with the most
    // jitter, typically) is the last to fill its windows.  Capped at the
    // others' schedule meanwhile, it would be substituted for those ticks at
    // every session start - the patience_test steady-link run showed 21.
    //
    // The cap is deliberately NOT applied to members with statistics: the
    // structurally latest member would then be cut off on every tick, its
    // reference comes from that same distribution, the meter would never see
    // a shift, and it would stay substituted for good.
    inline void patience_assign_leads(std::vector<patience_member>& members) {
        bool any = false;
        double latest = 0.0;
        for (auto& m: members) {
            if (!m.has_statistics) continue;
            m.lead = patience_lead_ticks(m.r01);
            m.capped = false;
            if (!any || m.lead < latest) latest = m.lead;
            any = true;
        }
        for (auto& m: members) {
            if (m.has_statistics) continue;
            m.capped = any && m.late_assignment;
            m.lead = m.capped ? latest : 0.0;
        }
    }

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
    // frame its players send is stale.  With nobody's frame here, every
    // member that lacks one is waited for until its own patience, as if
    // unwaived: the tick steps at the latest of those patiences, never later
    // than the deadline.
    inline patience_verdict patience_step_test(const std::vector<patience_member>& members, bool due,
                                               double to_deadline, bool patience) {
        patience_verdict v;
        if (!patience) {
            bool all = true;
            for (const auto& m: members) if (!m.has) { all = false; break; }
            v.step = all || due;
            return v;
        }
        bool paced = false;
        for (const auto& m: members) if (m.has) { paced = true; break; }
        bool blocked = false, unsettled = false;
        for (const auto& m: members) {
            if (m.has) continue;
            if (m.settled) {
                if (paced) continue;
            } else {
                unsettled = true;
                if (m.waived && paced) continue;
            }
            if (to_deadline <= m.lead) continue;
            const double until = to_deadline - m.lead;
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
    // patience_step_test); with nobody's frame there, p's patience applies.
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
        // A lateness sample of p (slack in ticks before its frame's deadline)
        // and p's lead: an arrival inside patience ends the waiver.  Returns
        // true when it did.
        bool sample(double slack, double lead) {
            if (!waived_ || slack < lead) return false;
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
}
