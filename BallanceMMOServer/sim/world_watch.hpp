#pragma once

// Fork detection step FD1, the server's half (the fork detection design,
// 5.2, 6.1, 14.4): per session, the runner's terms and rows of its last
// sample ticks (every T % 66 == 0), the compare of each member's
// SessionWorldReport of T against them, and per member the episode of a
// world that differs - opened, decided ("would heal": FD1 heals nothing),
// closed - and the stretches a member is not compared (blind).
//
// Pure: no CK, no clock, no journal.  Every time is a session tick the
// runner passes in (`now`: the tick it simulates next), so what it decides
// is a function of the stamped commands and the steps (K4b).  What it does
// goes out through `sink`: journal records at `now`, checkpoint rows, and
// notices (a SessionWorldFork to a member, a default-level log line, or
// both).  It lives in the runner's session_state; the simulation thread is
// its only caller.

#include <entity/session.hpp>
#include <message/session_world_report_msg.hpp>
#include <session/journal.hpp>
#include <session/journal_stats.hpp>
#include <session/world_check.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bmmo::sim {
    // One SessionWorldReport as the runner holds it (the message's fields).
    struct world_report_in {
        uint32_t player = 0, epoch = 0, tick = 0;
        uint8_t flags = 0, why = 0;
        bmmo::session::world_check::world_terms terms;
        std::vector<uint32_t> latched;
        std::vector<bmmo::session::body_state> rows;
        uint32_t episode = 0, fold_first = 0;
        std::vector<std::pair<uint32_t, uint32_t>> folds;
        std::vector<bmmo::session_world_report_msg::rewind_entry> rewinds;
        uint16_t resyncs = 0;

        static world_report_in from(uint32_t player, const bmmo::session_world_report_msg& m);
    };

    class world_watch {
    public:
        using wc_action = bmmo::session::world_check::fork_action;
        using wc_tier = bmmo::session::world_check::fork_tier;
        using wc_rule = bmmo::session::world_check::fork_rule;
        using wc_outcome = bmmo::session::world_check::fork_outcome;

        // A SessionWorldFork for `player` (send) and/or a log line ("" none;
        // the text after "world check: ", player names included).
        struct notice {
            uint32_t player = 0;
            bool send = false;
            uint32_t episode = 0, tick = 0;
            wc_action action = wc_action::opened;
            uint8_t kinds = 0;
            std::vector<uint32_t> owners;
            uint8_t buckets = 0;
            wc_tier tier = wc_tier::unsettled;
            wc_rule rule = wc_rule::none;
            uint32_t tick2 = 0;
            wc_outcome outcome = wc_outcome::none;
            uint16_t differing = 0;
            float worst_m = 0.0f, own_m = 0.0f;
            std::string worst;
            std::string line;
        };
        struct sink {
            std::function<void(bmmo::session::stat_kind, std::initializer_list<bmmo::session::journal_value>)> record;
            std::function<void(uint32_t tick, uint8_t flags, const std::vector<bmmo::session::body_state>&, uint32_t member)> rows;
            std::function<void(const notice&)> out;
            // The member's calm moment in the server's world now: 0 none,
            // 1 its ball absent (a death, a trafo pass), 2 finished.
            std::function<int(uint32_t player)> calm;
        };

        explicit world_watch(sink s = {}) : sink_(std::move(s)) {}
        void set_sink(sink s) { sink_ = std::move(s); }

        // Sampled only while a member declares CAP_WORLD_CHECK.
        bool active() const { return capable_ > 0; }

        // SessionReady: what the member declared (a member without the
        // capability is never compared: one blind record and line).
        void member(uint32_t player, uint32_t caps, const std::string& name, uint32_t now);
        // Every assignment (player_ready): the epoch its reports carry.
        void ready(uint32_t player, uint32_t base, uint32_t now);
        void leave(uint32_t player, uint32_t now);
        // The runner's rows of T at the end of its step (snapshot_for_journal),
        // the latched players, and whether the FULL checkpoint of T holds them.
        void sample(uint32_t tick, std::vector<bmmo::session::body_state> rows, std::vector<uint32_t> latched,
                    bool full_row, uint32_t now);
        void report(world_report_in r, uint32_t now);
        // After every step: the DETAIL timeout, the would-heal ticks, blind.
        void step(uint32_t now);
        // The session's end: open episodes close `ended`, the counters.
        void end(uint32_t now);

        static constexpr uint32_t kTerms = 16;        // samples whose terms are kept
        static constexpr uint32_t kRowSamples = 4;    // samples whose rows are kept
        static constexpr uint32_t kHeld = 4;          // reports per member held for an unstepped T
        static constexpr uint32_t kDetailTicks = bmmo::session::TICK_RATE;       // DETAIL timeout
        static constexpr uint32_t kSoonTicks = 2 * bmmo::session::TICK_RATE;
        static constexpr uint32_t kLaterTicks = 6 * bmmo::session::TICK_RATE;
        static constexpr uint32_t kGapTicks = 10 * bmmo::session::TICK_RATE;     // the user's fixed gap
        static constexpr uint32_t kExpectedTicks = 10 * bmmo::session::TICK_RATE;
        static constexpr uint32_t kBlindTicks = 10 * bmmo::session::TICK_RATE;

    private:
        using rows_ptr = std::shared_ptr<const std::vector<bmmo::session::body_state>>;
        struct sample_entry {
            uint32_t tick = 0;
            bmmo::session::world_check::world_terms terms;
            std::vector<uint32_t> latched;
            rows_ptr rows;                 // null past kRowSamples
            bool full_row = false, check_row = false;
            std::map<uint32_t, bmmo::session::world_check::world_terms> reports;   // each member's, for the grouping
        };
        struct episode {
            uint32_t id = 0, first = 0, last = 0, opened_at = 0;
            uint8_t kinds = 0, buckets = 0;          // the open sample's
            std::vector<uint32_t> owners;            // every differing ball so far
            std::vector<uint32_t> latched;           // the open sample's both sides
            rows_ptr server_rows;                    // the server's rows of `first`
            bool expected = false;                   // unsettled (c): a pending own why at the open
            bool unsettled_a = false;                // the first sample held (a)
            bool detail = false, no_detail = false, decided = false, final = false;
            wc_tier tier = wc_tier::unsettled;
            wc_rule rule = wc_rule::none;
            uint32_t would_heal = 0;
            uint32_t samples = 1, own_samples = 0;
        };
        struct member_state {
            uint32_t id = 0;
            std::string name;
            bool cap = false, ready = false;
            uint32_t base = 0;
            uint32_t compared_at = 0;     // now at the last compare (or the first ready)
            uint32_t heard_at = 0;        // now at the last report of any kind
            bool heard = false;
            const char* cause = "silent"; // the last report's result, when not compared
            uint32_t last_equal = 0;      // the last equal sample T
            bool blind = false;
            const char* blind_cause = "";
            uint32_t blind_first = 0;
            std::optional<episode> ep;
            bool healed_once = false;
            uint32_t last_would_heal = 0;
            std::vector<world_report_in> held;
        };
        struct assessment {
            bool rows = false;
            uint16_t differing = 0;
            uint16_t settled = 0;          // of `differing`, the keys both sides have once each
            std::string worst;
            double worst_m = 0.0, own_m = 0.0, near_m = -1.0;
            bool presence_only = false, flags_ball = false;
            std::string presence;
        };
        // One differing class's grouping at T: `same` of `others` other
        // members reported this member's term; a ball's owner: -1 not asked
        // (the member's own), 0 no report, 1 agrees with the server, 2 not.
        struct group {
            std::string key;               // p<owner> or b<bucket>
            int bucket = -1;
            unsigned same = 0, others = 0;
            int owner = -1;
        };

        void agree(const world_report_in& r, const char* result, uint8_t kinds = 0, const std::vector<uint32_t>& owners = {});
        void not_compared(member_state& m, const world_report_in& r, const char* result);
        void handle(member_state& m, world_report_in& r, uint32_t now);
        void compare(member_state& m, sample_entry& sample, const world_report_in& r, uint32_t now);
        void open(member_state& m, sample_entry& sample, const world_report_in& r,
                  const bmmo::session::world_check::world_diff& diff, uint32_t now);
        void detail(member_state& m, const world_report_in& r, uint32_t now);
        // Decide (or hold) the episode from this sample's assessment.
        void judge(member_state& m, const world_report_in& r, const assessment& a, uint8_t kinds, bool first, uint32_t now);
        void hold(member_state& m, uint32_t player, wc_rule rule);
        void deadline(member_state& m, uint32_t player, uint32_t now);
        void close(member_state& m, uint32_t tick, wc_outcome outcome, uint32_t now);
        void blind_end(member_state& m, uint32_t now, bool say);
        assessment assess(uint32_t player, const std::vector<bmmo::session::body_state>* server,
                          const std::vector<bmmo::session::body_state>& client, uint8_t buckets,
                          const std::vector<uint32_t>& latched) const;
        std::vector<group> grouping(uint32_t player, uint32_t tick, const std::vector<uint32_t>& owners, uint8_t buckets) const;
        static std::string grouping_record(const std::vector<group>& groups);
        sample_entry* find_sample(uint32_t tick);
        const sample_entry* find_sample(uint32_t tick) const;
        void emit(notice n) const { if (sink_.out) sink_.out(n); }
        void put(bmmo::session::stat_kind kind, std::initializer_list<bmmo::session::journal_value> v) const {
            if (sink_.record) sink_.record(kind, v);
        }

        sink sink_;
        std::deque<sample_entry> samples_;
        std::map<uint32_t, member_state> members_;
        uint32_t capable_ = 0, episodes_ = 0, members_seen_ = 0;
        uint64_t sampled_ = 0, compared_ = 0, differing_ = 0, leads_ = 0, would_heals_ = 0;
        uint32_t blind_max_ = 0;
        std::vector<uint32_t> forked_;    // per closed episode (not leads)
        bool ended_ = false;
    };
}
