#pragma once

// The session start as a handshake (FULL step SAXH, Q-SAX; the user,
// 2026-10-31: start once the handshake has passed).
//
// Until protocol 23 a member's tick clock started when its SessionAssign
// ARRIVED: a member whose assignment a burst of lost datagrams delayed ran
// that many ticks behind the others for the whole session (FULL steps SAX /
// SAXE: sixteen copies one main-loop pass apart leave about one tick per
// lost datagram, and nothing fixes a time-shaped outage).  Protocol 24 moves
// the uncertainty before the start, where it delays everyone equally:
//
//   Phase 1 (assign + ack): the members present at the start get their
//   assignment as before (reliable + the copies), flagged
//   ASSIGN_FLAG_SCHEDULED: they renumber onto the start base but keep holding
//   their anchor.  Every copy carries its send instant on the server's clock
//   and is answered by a SessionClockAck carrying the client's clock when it
//   handled the copy: one clock sample each (make_clock_sample); the
//   lowest-round-trip sample is kept (clock_estimate).
//
//   Phase 2 (scheduled start): once every member has a sample (and a few, or
//   kStartSampleWaitUs after the assignment), the server picks a start
//   instant X on its own clock (plan_start: the worst member's round trip
//   plus slack to send the start and hear its confirmation, plus a guard to
//   postpone in) and sends SessionStartAt to each member with that member's
//   start instant on its own clock - X plus its offset plus half its round
//   trip, i.e. where its assignment would have arrived on a lossless link,
//   so the start base's lead keeps the meaning it had (it was sized from the
//   round trip).  The member starts its first frame at that local instant
//   however late the message came, as long as it came before the instant,
//   and confirms it.  All confirmed: the server starts its schedule at X.  A
//   confirmation missing at the deadline (X - guard): the start moves for
//   everyone (a new seq) rather than anyone starting late; after
//   kStartMaxPostpones the missing members are refused (as a placement
//   mismatch is) and the rest start.  A member a postponement reached only
//   after the old instant has started on the old start: it answers the new
//   one CLOCK_ACK_STARTED (that seq), and the server commits the earliest
//   such start for everyone (START_AT_FLAG_COMMIT: a member still holding
//   starts at once and catches up; started_on).
//
// Late joiners and resyncs use the same clock estimate: a resync's
// assignment carries the member's start instant at once when the server's
// estimate is fresh (kClockEstimateMaxAgeUs; else, like a late joiner's, it
// comes as a correction after the first ack), a late joiner - whose first
// assignment comes before any sample - starts on arrival as before and gets
// a SessionStartAt with START_AT_FLAG_CORRECTION once its first ack is in;
// either way the client catches up the whole ticks it started late
// (start_lateness_ticks).  The base and the state a late join or a resync
// anchors at are unchanged, so exactness is unchanged: only the wall-clock
// instant of the member's frame `base` moves, to where a lossless arrival
// would have put it.
//
// CK-free and header-only; the server owns a start_handshake per session
// (server.cpp), the client core the client half (client_core.cpp).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace bmmo::session {

    // One clock sample: t1 = the copy's send instant (server clock), tc = the
    // client's clock when it handled the copy, t4 = the server's receive
    // instant of the ack.  Microseconds of each side's steady clock.  The
    // offset is client minus server (a server instant + offset = the same
    // instant on the client's clock), assuming the trip each way is half.
    struct clock_sample {
        int64_t rtt_us = 0;
        int64_t offset_us = 0;
    };
    inline clock_sample make_clock_sample(int64_t t1, int64_t tc, int64_t t4) {
        clock_sample c;
        c.rtt_us = std::max<int64_t>(0, t4 - t1);
        c.offset_us = tc - t1 - (t4 - t1) / 2;
        return c;
    }

    // A sample older than this is not used for an instant (two PCs' clocks
    // drift apart by up to ~100 ppm: 1 ms in 10 s, 240 ms in 40 minutes -
    // the gate round's review of SAXH): a newer sample replaces it whatever
    // its round trip, and an estimate whose best sample is older is not
    // fresh (the server then sends no instant and corrects after the next
    // ack, as for a late joiner).
    inline constexpr int64_t kClockEstimateMaxAgeUs = 10'000'000;

    // The lowest-round-trip sample of a member (the one least delayed by a
    // queue, a poll or a lost first copy on either leg) among its recent
    // ones; `at_us` is when the sample was taken (server clock).
    struct clock_estimate {
        uint32_t samples = 0;
        clock_sample best{};
        int64_t best_at_us = 0;
        void add(const clock_sample& c, int64_t at_us = 0) {
            if (samples == 0 || c.rtt_us < best.rtt_us || at_us - best_at_us > kClockEstimateMaxAgeUs) {
                best = c;
                best_at_us = at_us;
            }
            ++samples;
        }
        bool known() const { return samples != 0; }
        bool fresh(int64_t now_us) const { return known() && now_us - best_at_us <= kClockEstimateMaxAgeUs; }
        // The member's own clock at the moment a message the server sends at
        // `server_us` reaches it on a lossless link.
        int64_t arrival_local(int64_t server_us) const { return server_us + best.offset_us + best.rtt_us / 2; }
    };

    // The session's tick rate (every host steps 66 Hz).
    inline constexpr double kStartTickRate = 66.0;

    // Phase 1: once every member has a sample, wait for this many each, or
    // this long after the assignment went out (the copies span ~242 ms).
    inline constexpr uint32_t kStartSamplesWanted = 4;
    inline constexpr int64_t kStartSampleWaitUs = 400'000;
    // Who is still missing, said once this long after a phase began.
    inline constexpr int64_t kStartMissingNoticeUs = 1'000'000;
    // Phase 1 gives a member without a single sample up after this long.
    inline constexpr int64_t kStartAssignGiveUpUs = 5'000'000;
    // Phase 2: the start message's trip and the confirmation's back take the
    // worst round trip plus this slack (a burst of 8 lost copies at one per
    // main-loop pass is ~122 ms); the guard - the time left after the
    // deadline for a postponement to reach every member before X - is half
    // the worst round trip plus its own slack.
    inline constexpr int64_t kStartWindowSlackUs = 160'000;
    inline constexpr int64_t kStartGuardSlackUs = 60'000;
    // A start postponed this many times refuses the members still missing.
    inline constexpr int kStartMaxPostpones = 3;

    struct start_schedule {
        int64_t x_us = 0;          // the start instant, server clock
        int64_t deadline_us = 0;   // every confirmation by then, or the start moves
    };
    inline start_schedule plan_start(int64_t now_us, int64_t worst_rtt_us) {
        const int64_t rtt = std::max<int64_t>(0, worst_rtt_us);
        const int64_t window = rtt + kStartWindowSlackUs;
        const int64_t guard = rtt / 2 + kStartGuardSlackUs;
        return start_schedule{now_us + window + guard, now_us + window};
    }

    // The whole ticks a frame that was due at `due_us` and ran (or will run)
    // at `started_us` is late: what a member catches up (>= 1), else 0 - a
    // lateness under one tick, or an early start, changes nothing (on a
    // lossless link the arrival is the instant, so nothing moves).
    inline int start_lateness_ticks(int64_t started_us, int64_t due_us, double tick_rate) {
        const double late = static_cast<double>(started_us - due_us) / 1e6 * tick_rate;
        return late >= 1.0 ? static_cast<int>(std::floor(late)) : 0;
    }
    // A catch-up is at most this many ticks (half a second): more means the
    // instant is wrong (a stale or broken clock estimate), and running that
    // far ahead would only deepen every rewind; the rest is left to the
    // lead machinery (a resync, Stage B's per-player lead).
    inline constexpr int kStartMaxCatchUpTicks = 33;

    // The server's half: one session's start, from the start members'
    // assignment to the instant its schedule starts.  The server feeds it the
    // acks and polls it from its main loop with the current start members
    // (members that left are forgotten); a poll says what to do.  It also
    // keeps every member's clock estimate for the session's life (late
    // joiners' and resyncs' start instants).
    class start_handshake {
    public:
        enum class phase : uint8_t { idle, assign, start, started };

        struct poll_result {
            // Send SessionStartAt (seq, x) to every member with an estimate.
            bool send_start = false;
            bool postponed = false;          // ...because of `missing`
            // Every member confirmed: the server's schedule starts at x.
            bool start_now = false;
            // ...or (with start_now) a member already started on an earlier
            // start `commit_seq` whose postponement reached it after its
            // instant (`committed_by`): that start is the session's - send
            // every member SessionStartAt(commit_seq, START_AT_FLAG_COMMIT);
            // those still holding start at once and catch up.
            bool commit = false;
            uint32_t commit_seq = 0;
            uint32_t committed_by = 0;
            // Refuse these (out of the session, with a reason); the rest go on.
            std::vector<uint32_t> refuse;
            // Say who is missing (once per phase, kStartMissingNoticeUs in).
            bool notice_missing = false;
            std::vector<uint32_t> missing;
            uint32_t confirmed = 0, total = 0;
        };

        // The start members' assignment went out at `now_us`.
        void begin(int64_t now_us) {
            phase_ = phase::assign;
            phase_began_us_ = now_us;
            assign_sent_us_ = now_us;
            missing_noticed_ = false;
            seq_ = 0;
            postpones_ = 0;
            start_acks_.clear();
            schedules_.clear();
            committed_seq_ = 0;
            commit_pending_ = false;
            committed_by_ = 0;
        }

        void add_sample(uint32_t member, const clock_sample& c, int64_t at_us = 0) { clocks_[member].add(c, at_us); }
        // A confirmation of start message `seq`; an older seq's is void.
        void start_acked(uint32_t member, uint32_t seq) {
            if (phase_ == phase::start && seq == seq_) start_acks_.insert(member);
        }
        // The member already STARTED on start message `seq` (CLOCK_ACK_STARTED:
        // a later start reached it after that one's instant).  It cannot
        // un-start, so that start is the session's: the earliest such seq
        // is committed (the next poll says so) - the members still holding
        // start at once and catch up the whole ticks since its instant.  A
        // later report of an even earlier start commits that one instead.
        void started_on(uint32_t member, uint32_t seq) {
            if (phase_ == phase::idle || seq == 0 || seq > seq_ || !schedules_.count(seq)) return;
            if (phase_ == phase::start && seq == seq_) {   // the current one: a confirmation
                start_acks_.insert(member);
                return;
            }
            if (committed_seq_ != 0 && seq >= committed_seq_) return;
            committed_seq_ = seq;
            committed_by_ = member;
            commit_pending_ = true;
        }
        uint32_t committed_seq() const { return committed_seq_; }
        // A commit the next poll reports (also once the schedule started).
        bool commit_pending() const { return commit_pending_; }
        void forget(uint32_t member) {
            clocks_.erase(member);
            start_acks_.erase(member);
        }

        const clock_estimate* estimate(uint32_t member) const {
            const auto it = clocks_.find(member);
            return it == clocks_.end() || !it->second.known() ? nullptr : &it->second;
        }
        phase current_phase() const { return phase_; }
        uint32_t seq() const { return seq_; }
        int postpones() const { return postpones_; }
        const start_schedule& schedule() const { return schedule_; }
        int64_t worst_rtt_us(const std::vector<uint32_t>& members) const {
            int64_t worst = 0;
            for (const auto m: members)
                if (const auto* e = estimate(m)) worst = std::max(worst, e->best.rtt_us);
            return worst;
        }

        poll_result poll(int64_t now_us, const std::vector<uint32_t>& members) {
            poll_result r;
            r.total = static_cast<uint32_t>(members.size());
            if (commit_pending_ && (phase_ == phase::start || phase_ == phase::started)) {
                commit_pending_ = false;
                phase_ = phase::started;
                schedule_ = schedules_.at(committed_seq_);
                r.start_now = true;
                r.commit = true;
                r.commit_seq = committed_seq_;
                r.committed_by = committed_by_;
                r.confirmed = r.total;
                return r;
            }
            if (phase_ == phase::idle || phase_ == phase::started) return r;
            if (members.empty()) {
                phase_ = phase::idle;
                return r;
            }
            if (phase_ == phase::assign) {
                bool all = true, enough = true;
                for (const auto m: members) {
                    const auto* e = estimate(m);
                    if (!e) {
                        all = false;
                        r.missing.push_back(m);
                    } else {
                        ++r.confirmed;
                        if (e->samples < kStartSamplesWanted) enough = false;
                    }
                }
                if (all && (enough || now_us - assign_sent_us_ >= kStartSampleWaitUs)) {
                    plan(now_us, members);
                    r.send_start = true;
                    r.missing.clear();
                    return r;
                }
                if (!all && now_us - phase_began_us_ >= kStartAssignGiveUpUs) {
                    r.refuse = r.missing;
                    return r;
                }
                if (!all && !missing_noticed_ && now_us - phase_began_us_ >= kStartMissingNoticeUs) {
                    missing_noticed_ = true;
                    r.notice_missing = true;
                }
                return r;
            }
            // phase::start
            for (const auto m: members) {
                if (start_acks_.count(m)) ++r.confirmed;
                else r.missing.push_back(m);
            }
            if (r.missing.empty()) {
                phase_ = phase::started;
                committed_seq_ = seq_;
                r.start_now = true;
                return r;
            }
            if (now_us < schedule_.deadline_us) {
                if (!missing_noticed_ && now_us - phase_began_us_ >= kStartMissingNoticeUs) {
                    missing_noticed_ = true;
                    r.notice_missing = true;
                }
                return r;
            }
            // The deadline passed with a confirmation missing: nobody starts
            // late - the start moves for everyone, or, past the bound, the
            // missing members are refused and the rest get a new instant.
            ++postpones_;
            if (postpones_ > kStartMaxPostpones) r.refuse = r.missing;
            std::vector<uint32_t> rest;
            for (const auto m: members)
                if (std::find(r.refuse.begin(), r.refuse.end(), m) == r.refuse.end()) rest.push_back(m);
            if (rest.empty()) {
                phase_ = phase::idle;
                return r;
            }
            plan(now_us, rest);
            r.send_start = true;
            r.postponed = r.refuse.empty();
            return r;
        }

    private:
        void plan(int64_t now_us, const std::vector<uint32_t>& members) {
            phase_ = phase::start;
            phase_began_us_ = now_us;
            missing_noticed_ = false;
            ++seq_;
            start_acks_.clear();
            schedule_ = plan_start(now_us, worst_rtt_us(members));
            schedules_[seq_] = schedule_;
        }

        phase phase_ = phase::idle;
        int64_t phase_began_us_ = 0, assign_sent_us_ = 0;
        bool missing_noticed_ = false;
        uint32_t seq_ = 0;
        int postpones_ = 0;
        start_schedule schedule_{};
        std::map<uint32_t, start_schedule> schedules_;   // every start message's, by seq
        uint32_t committed_seq_ = 0, committed_by_ = 0;
        bool commit_pending_ = false;
        std::set<uint32_t> start_acks_;
        std::map<uint32_t, clock_estimate> clocks_;
    };
}
