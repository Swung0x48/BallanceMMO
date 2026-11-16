// Fork detection step FD1, the client's half (session/world_check.hpp;
// the fork detection design, 14.3): the retire observer's
// sample ticks reported to the server, the per-tick fold ring, the airship
// zone's status at the step of a sample tick, and the answers to the
// server's SessionWorldFork notices.  Report only: nothing here writes any
// world, so a session with the check off steps every world the same.
#include "session/client_core.hpp"
#include "session/journal_stats.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace bmmo::session {

    namespace {
        using wc_client = client_core_state::world_check_client;

        using world_check::body_text;
        using world_check::hex64;
        using world_check::kinds_text;
        using world_check::owner_list;
    }

    void session_client_core::world_check_zone(uint32_t tick) {
        if (!config.world_check || !s.airship_active_ || !platform_.airship_zone_contains
            || tick % world_check::kSampleTicks != 0)
            return;
        // Every ball the world may have at the tick, in one lookup scope (as
        // airship_frame): no physicalized gate, since a replayed tick's
        // bodies are not the live head's - a ball with no body answers -1.
        std::vector<uint32_t> latched;
        with_lookup_scope([&] {
            const auto in_zone = [this](const std::string& entity) {
                if (entity.empty()) return false;
                const int status = platform_.airship_zone_contains(entity);
                return status == 1 || status == 2;
            };
            if (in_zone(s.own_body_entity_)) latched.push_back(s.own_id_);
            for (const auto& [id, remote]: s.remotes_)
                if (in_zone(remote.entity)) latched.push_back(id);
        });
        std::sort(latched.begin(), latched.end());
        latched.erase(std::unique(latched.begin(), latched.end()), latched.end());
        s.world_check_.zones[tick] = std::move(latched);
    }

    void session_client_core::world_check_fold(uint32_t tick, const std::map<std::string, bmmo_physics_body_state>& bodies,
                                               const rollback_tracked& tracked,
                                               const std::map<std::string, uint32_t>& names) {
        auto& w = s.world_check_;
        // A gap in the retired ticks: a resync's re-anchor dropped the
        // records it covered, so their sample ticks are never reported - one
        // world_report each (sent 2), so the hole is in the record.
        if (w.fold_last != 0 && static_cast<int32_t>(tick - w.fold_last) > 1) {
            for (uint32_t t = (w.fold_last / world_check::kSampleTicks + 1) * world_check::kSampleTicks; t < tick;
                 t += world_check::kSampleTicks) {
                ++w.skipped;
                if (journal_.journalling())
                    journal_.writer().stat(t, stat_kind::world_report,
                                           {t, s.last_assign_first_tick_, 1, static_cast<int>(world_check::why::resync_pending),
                                            0u, 0u, 0u, std::string_view(), std::string_view(), 2, 0u, 0u, 0u});
            }
        }
        if (!w.folds.empty() && tick != w.fold_last + 1) w.folds.clear();   // the ring holds consecutive ticks
        world_check::position_sums sums;
        for (const auto& [entity, body]: bodies) {
            if (platform_.local_only_body && platform_.local_only_body(entity)) continue;
            const bool own = !tracked.own_entity.empty() && entity == tracked.own_entity;
            const bool remote = !own && std::find(tracked.remote_entities.begin(), tracked.remote_entities.end(), entity)
                                        != tracked.remote_entities.end();
            uint32_t owner = 0;
            if (own) owner = s.own_id_;
            else if (remote)
                if (const auto it = names.find(entity); it != names.end()) owner = it->second;
            sums.add(own || remote ? body_kind::Ball : body_kind::Mechanism, owner, entity, body.position);
        }
        w.folds.push_back(sums.fold());
        w.fold_last = tick;
        while (w.folds.size() > wc_client::kFolds) w.folds.pop_front();
        w.zones.erase(w.zones.begin(), w.zones.lower_bound(tick));   // a retired tick is not stepped again
    }

    void session_client_core::world_check_sample(uint32_t tick, const std::vector<body_state>& rows,
                                                 world_check::why why) {
        auto& w = s.world_check_;
        wc_client::sample sample;
        sample.tick = tick;
        sample.why = why;
        sample.rows = rows;
        sample.resyncs = static_cast<uint32_t>(s.resyncs_sent_ - w.window_resyncs_from);
        if (const auto it = w.zones.find(tick); it != w.zones.end()) sample.latched = it->second;
        w.zones.erase(w.zones.begin(), w.zones.upper_bound(tick));
        // The report's terms: the bodies only this world has (the host's
        // local_only_body) out; the FINAL row keeps them.
        sample.terms = world_check::fold_rows(sample.rows, sample.latched, [this](const std::string& name) {
            return platform_.local_only_body && platform_.local_only_body(name);
        });
        // A resync asked for and not applied: its re-anchor replaces this
        // world, so its reports are held back (at most kPendingHoldTicks),
        // then sent with PENDING (2.3).
        const bool pending = s.resync_pending_ || s.resync_in_flight_;
        if (!pending) w.holding = false;
        else if (!w.holding) {
            w.holding = true;
            w.hold_from = tick;
        }
        const bool held = pending && tick - w.hold_from < world_check::kPendingHoldTicks;
        uint8_t flags = why != world_check::why::none ? bmmo::session_world_report_msg::FLAG_UNCONFIRMED : 0;
        if (pending && !held) flags |= bmmo::session_world_report_msg::FLAG_PENDING;
        if (journal_.journalling())
            journal_.writer().stat(tick, stat_kind::world_report,
                                   {tick, s.last_assign_first_tick_, why == world_check::why::none ? 0 : 1,
                                    static_cast<int>(why), static_cast<unsigned>(sample.terms.rows),
                                    static_cast<unsigned>(sample.terms.unkeyed), static_cast<unsigned>(sample.terms.dup),
                                    owner_list(sample.latched), hex64(world_check::terms_fold(sample.terms)), held ? 0 : 1,
                                    w.window_rewinds, w.window_deepest, sample.resyncs});
        if (held) {
            ++w.held;
        } else {
            const bool rows_due = w.open && !w.decided && w.row_reports < world_check::kRowReports;
            if (rows_due) ++w.row_reports;
            world_check_send(sample, flags, rows_due);
        }
        w.window_rewinds = w.window_deepest = 0;
        w.window_resyncs_from = s.resyncs_sent_;
        w.samples.push_back(std::move(sample));
        while (w.samples.size() > wc_client::kSamples) w.samples.pop_front();
    }

    void session_client_core::world_check_send(const wc_client::sample& sample, uint8_t flags, bool rows) {
        const auto& w = s.world_check_;
        bmmo::session_world_report_msg msg;
        msg.session = s.session_;
        msg.epoch = s.last_assign_first_tick_;
        msg.tick = sample.tick;
        msg.flags = flags;
        msg.why = static_cast<uint8_t>(sample.why);
        msg.rows = sample.terms.rows;
        msg.unkeyed = sample.terms.unkeyed;
        msg.dup = sample.terms.dup;
        msg.balls = sample.terms.balls;
        std::copy(std::begin(sample.terms.buckets), std::end(sample.terms.buckets), std::begin(msg.buckets));
        msg.latched = sample.latched;
        // The rows of every player ball and of the mechanisms in the open
        // episode's buckets, by the fold's rules.
        if (rows)
            for (const auto& row: sample.rows) {
                if (keep_local_body(row.name) || (platform_.local_only_body && platform_.local_only_body(row.name))) continue;
                if (row.kind == body_kind::Ball
                    || (w.episode_buckets & (1u << world_check::bucket_of(row.name))) != 0)
                    msg.body_rows.push_back(row);
            }
        if (flags & bmmo::session_world_report_msg::FLAG_DETAIL) {
            msg.episode = w.episode;
            msg.fold_first = w.fold_last - static_cast<uint32_t>(w.folds.size()) + 1;
            for (uint64_t f: w.folds)
                msg.folds.emplace_back(static_cast<uint32_t>(f >> 32), static_cast<uint32_t>(f & 0xFFFFFFFFu));
            msg.rewinds.assign(w.rewinds.begin(), w.rewinds.end());
            msg.resyncs = static_cast<uint16_t>(std::min<uint32_t>(sample.resyncs, 0xFFFF));
        }
        msg.serialize();
        platform_.send(msg.raw.str(), send_mode::reliable);
        ++s.world_check_.reports;
    }

    void session_client_core::world_check_rewind(uint32_t anchor, uint32_t now, rewind_cause cause, uint64_t depth,
                                                 std::string_view body) {
        auto& w = s.world_check_;
        // The window's deepest is the asked depth (now - anchor), which follows
        // from the ticks (a logical record); the DETAIL list carries the
        // ticks it re-stepped (the server's record is a clock site's).
        ++w.window_rewinds;
        w.window_deepest = std::max(w.window_deepest, now - anchor);
        w.rewinds.push_back({anchor, now, static_cast<uint8_t>(cause),
                             static_cast<uint16_t>(std::min<uint64_t>(depth, 0xFFFF)), std::string(body)});
        while (w.rewinds.size() > wc_client::kRewinds) w.rewinds.pop_front();
    }

    void session_client_core::on_session_world_fork(const bmmo::session_world_fork_msg& msg) {
        if (msg.session != s.session_ || !s.assigned_) return;
        auto& w = s.world_check_;
        ++w.notices;
        const auto action = static_cast<world_check::fork_action>(msg.action);
        const auto tier = static_cast<world_check::fork_tier>(msg.tier);
        const auto outcome = static_cast<world_check::fork_outcome>(msg.outcome);
        // At `opened`, the fold ring as the DETAIL answer carries it.
        std::string folds;
        uint32_t fold_first = 0;
        if (action == world_check::fork_action::opened && !w.folds.empty()) {
            fold_first = w.fold_last - static_cast<uint32_t>(w.folds.size()) + 1;
            folds.reserve(w.folds.size() * 16);
            for (uint64_t f: w.folds) folds += hex64(f);
        }
        if (journal_.journalling())
            journal_.writer().stat(current_tick(), stat_kind::fork_notice,
                                   {msg.episode, msg.tick, world_check::action_name(action),
                                    static_cast<unsigned>(msg.kinds), owner_list(msg.owners),
                                    static_cast<unsigned>(msg.buckets), world_check::tier_name(tier),
                                    world_check::rule_name(static_cast<world_check::fork_rule>(msg.rule)), msg.tick2,
                                    world_check::outcome_name(outcome), static_cast<unsigned>(msg.differing), msg.worst,
                                    static_cast<double>(msg.worst_m), static_cast<double>(msg.own_m), fold_first, folds});
        switch (action) {
        case world_check::fork_action::opened: {
            w.open = true;
            w.decided = false;
            w.episode = msg.episode;
            w.episode_tick = msg.tick;
            w.episode_buckets = msg.buckets;
            w.row_reports = 0;
            ++w.episodes;
            // The DETAIL answer, at once: the sample's rows of the named
            // classes, the folds and the rewinds.  A sample that has left
            // the ring (it cannot in practice: `opened` comes one trip after
            // its report) is answered with its tick and no rows.
            const auto it = std::find_if(w.samples.begin(), w.samples.end(),
                                         [&msg](const wc_client::sample& x) { return x.tick == msg.tick; });
            const uint8_t detail = bmmo::session_world_report_msg::FLAG_DETAIL;
            if (it == w.samples.end()) {
                ++w.detail_misses;
                wc_client::sample missing;
                missing.tick = msg.tick;
                world_check_send(missing, detail, false);
                break;
            }
            ++w.details;
            world_check_send(*it, static_cast<uint8_t>(detail | (it->why != world_check::why::none
                                                                     ? bmmo::session_world_report_msg::FLAG_UNCONFIRMED : 0)),
                             true);
            // This world's rows of T in the journal, unless a FINAL row
            // already holds them.
            if (journal_.journalling() && !(config.journal_final_checkpoints && journal_.final_due(msg.tick)))
                journal_.writer().checkpoint(
                        msg.tick,
                        static_cast<uint8_t>(JOURNAL_CHECKPOINT_LOCAL | JOURNAL_CHECKPOINT_FINAL | JOURNAL_CHECKPOINT_CHECK
                                             | (it->why != world_check::why::none ? JOURNAL_CHECKPOINT_UNCONFIRMED : 0)),
                        it->rows, s.own_id_);
            break;
        }
        case world_check::fork_action::decided: {
            w.decided = true;
            // "the ball of player 12, 3.18 m, 2 bodies differ, the own ball 0.01 m"
            const bool own_worst = msg.worst == world_check::ball_key(s.own_id_);
            std::string what = msg.worst.empty() ? kinds_text(msg.kinds) : body_text(msg.worst, s.own_id_);
            char text[64];
            std::snprintf(text, sizeof(text), ", %.3g m", static_cast<double>(msg.worst_m));
            if (!msg.worst.empty()) what += text;
            if (msg.differing > 1) what += ", " + std::to_string(msg.differing) + " bodies differ";
            if (msg.own_m > 0.0f && !own_worst) {
                std::snprintf(text, sizeof(text), ", the own ball %.3g m", static_cast<double>(msg.own_m));
                what += text;
            }
            // The server's would-heal tick, the 10 s gap included.
            const std::string heal = tier == world_check::fork_tier::unsettled
                ? std::string("at none of its samples yet (unsettled)")
                : "by tick " + std::to_string(msg.tick2) + " (" + world_check::tier_name(tier) + ")";
            logf("world check: this world differs from the server's at tick %u (episode %u: %s); the server would heal %s, "
                 "rule %s (report only)",
                 msg.tick, msg.episode, what.c_str(), heal.c_str(),
                 world_check::rule_name(static_cast<world_check::fork_rule>(msg.rule)));
            break;
        }
        case world_check::fork_action::closed: {
            if (msg.episode == w.episode) w.open = false;
            if (outcome == world_check::fork_outcome::lead) {
                logf("world check: tick %u: lead (%s, one sample), closed", msg.tick, kinds_text(msg.kinds).c_str());
                break;
            }
            const char* how = outcome == world_check::fork_outcome::self_healed ? "equal again before its would-heal tick"
                              : outcome == world_check::fork_outcome::equal_again ? "equal again"
                              : outcome == world_check::fork_outcome::ended ? "ended with the member's session"
                                                                            : world_check::outcome_name(outcome);
            logf("world check: episode %u closed at tick %u: %s (%.1f s forked)", msg.episode, msg.tick2, how,
                 static_cast<double>(static_cast<int32_t>(msg.tick2 - msg.tick)) / TICK_RATE);
            break;
        }
        }
    }

    // The session's end: an episode the server has not closed yet closes
    // here (`ended`), since the server's own close comes after SessionEnd
    // and is dropped - so this journal lists every episode it opened as
    // closed; then the counters, once.
    void session_client_core::world_check_end() {
        auto& w = s.world_check_;
        if (w.open) {
            const uint32_t now = current_tick();
            if (journal_.journalling())
                journal_.writer().stat(now, stat_kind::fork_notice,
                                       {w.episode, w.episode_tick, world_check::action_name(world_check::fork_action::closed),
                                        0u, std::string_view(), static_cast<unsigned>(w.episode_buckets),
                                        world_check::tier_name(world_check::fork_tier::unsettled),
                                        world_check::rule_name(world_check::fork_rule::none), now,
                                        world_check::outcome_name(world_check::fork_outcome::ended), 0u, std::string_view(),
                                        0.0, 0.0, 0u, std::string_view()});
            logf("world check: episode %u closed at tick %u: ended with the session (%.1f s forked)", w.episode, now,
                 static_cast<double>(static_cast<int32_t>(now - w.episode_tick)) / TICK_RATE);
            w.open = false;
        }
        if (w.reports || w.held || w.skipped)
            logf("world check: %llu reports sent (%llu held back while a resync was pending, %llu never retired), %llu "
                 "episodes, %llu notices, %llu DETAIL answers (%llu without their sample)",
                 static_cast<unsigned long long>(w.reports), static_cast<unsigned long long>(w.held),
                 static_cast<unsigned long long>(w.skipped), static_cast<unsigned long long>(w.episodes),
                 static_cast<unsigned long long>(w.notices), static_cast<unsigned long long>(w.details),
                 static_cast<unsigned long long>(w.detail_misses));
        w.reports = w.held = w.skipped = 0;
    }

}
