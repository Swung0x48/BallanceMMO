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
#include <cstdlib>
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
        // Step ZRX: a release applied at tick + 1 closed the span at the
        // clock that ends this tick's step, so a rewind re-stepping the tick
        // after the release reads the ball out; every world that stepped the
        // tick before the release (the live head, the server) holds it latched.
        for (const auto& release: s.airship_releases_)
            if (release.tick == tick + 1
                && (release.player == s.own_id_ || s.remotes_.count(release.player) != 0))
                latched.push_back(release.player);
        std::sort(latched.begin(), latched.end());
        latched.erase(std::unique(latched.begin(), latched.end()), latched.end());
        s.world_check_.zones[tick] = std::move(latched);
    }

    // Step FD3b (test only; FD1's headless --inject-fork, moved into the
    // core so retail takes it too): after the step of a spec's tick (every
    // tick from it to `until` with `every`), live at post_step's entry and in
    // every replay of it, the body moved along x with its velocities kept.
    void session_client_core::inject_fork(uint32_t tick, bool live) {
        if (config.inject_fork.empty() || !s.assigned_ || !platform_.body_state || !platform_.write_body) return;
        const char* where = live ? "live" : "replayed";
        for (const auto& spec: config.inject_fork) {
            const bool due = spec.every ? static_cast<int32_t>(tick - spec.tick) >= 0
                                              && (spec.until == 0 || static_cast<int32_t>(tick - spec.until) <= 0)
                                        : tick == spec.tick;
            if (!due) continue;
            // Every application of a one-shot is logged; of an `every`, the
            // live ones on sample ticks.
            const bool say = !spec.every || (live && tick % world_check::kSampleTicks == 0);
            std::string entity = spec.entity;
            if (entity == "own") {
                entity = s.own_body_entity_;
            } else if (entity.size() > 1 && entity[0] == 'p'
                       && std::all_of(entity.begin() + 1, entity.end(), [](char c) { return c >= '0' && c <= '9'; })) {
                const auto it = s.remotes_.find(static_cast<uint32_t>(std::strtoul(entity.c_str() + 1, nullptr, 10)));
                entity = it == s.remotes_.end() ? std::string() : it->second.entity;
            }
            bmmo_physics_body_state body{};
            if (entity.empty() || !platform_.body_state(entity, body)) {
                if (say) logf("inject-fork: no body for %s at the %s step of tick %u", spec.entity.c_str(), where, tick);
                continue;
            }
            body.position[0] += spec.dx;
            std::string error;
            if (!platform_.write_body(entity, body.position, body.rotation, body.linear, body.angular, true, error)) {
                if (say) logf("inject-fork: %s could not be moved at the %s step of tick %u: %s", entity.c_str(), where, tick,
                              error.c_str());
                continue;
            }
            if (say) logf("inject-fork: %s moved %+g m along x after the %s step of tick %u", entity.c_str(), spec.dx, where, tick);
        }
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
            // Step FD2: a record from before the hole is no sample's record.
            w.whole.reset();
            w.ring.clear();
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

    // Step FD2: the newest whole record that retired, its blob and scripts
    // moved out of the history (the record is dropped right after), with its
    // balls by owner - kept only while the episodes' blobs may still be
    // journaled.
    void session_client_core::world_check_keep(uint32_t tick, rollback_engine::retired_whole& whole,
                                               const rollback_tracked& tracked,
                                               const std::map<std::string, uint32_t>& names) {
        auto& w = s.world_check_;
        if (!s.assigned_ || w.state_episodes >= wc_client::kStateEpisodes || !journal_.journalling()) return;
        auto& r = w.whole.emplace();
        r.tick = tick;
        r.savestate = std::move(whole.savestate);
        r.scripts = std::move(whole.scripts);
        if (!tracked.own_entity.empty()) r.names.emplace_back(s.own_id_, tracked.own_entity);
        for (const auto& entity: tracked.remote_entities) {
            const auto it = names.find(entity);
            const uint32_t owner = it != names.end() ? it->second : mirror_player_of(entity);
            if (owner != 0) r.names.emplace_back(owner, entity);
        }
    }

    // Once per episode the core kept records for (opened while the journal
    // recorded and the cap was unused): one line, whatever became of them.
    void session_client_core::world_check_journal_states(uint32_t episode) {
        auto& w = s.world_check_;
        w.pending_episode = 0;
        if (w.pending.empty()) {
            // A report held back while a resync was pending, or an `opened`
            // more than a sample late: the ring no longer had T's records.
            logf("world check: episode %u: no records kept (first bad and last good had left the ring)", episode);
            return;
        }
        size_t bytes = 0;
        uint32_t first_bad = 0, last_good = 0;
        const bool recording = journal_.journalling();
        if (recording)
            for (const auto& r: w.pending) {
                journal_.writer().state(r.tick, episode, r.role, r.names, r.savestate, r.scripts);
                bytes += r.savestate.size() + r.scripts.size();
                (r.role == JOURNAL_STATE_FIRST_BAD ? first_bad : last_good) = r.tick;
            }
        w.pending.clear();
        if (!recording) {
            // The cap or a writer failure stopped the journal after `opened`:
            // nothing written, the episode cap not spent.
            logf("world check: episode %u: records not journaled (the journal stopped)", episode);
            return;
        }
        ++w.state_episodes;
        // The cap and a full queue are seen here, on the tick thread (the
        // record that trips either is dropped); "journaled" is queued.
        logf("world check: episode %u: the records of tick %s (first bad) and %s (last good) %s (%zu KB)", episode,
             first_bad ? std::to_string(first_bad).c_str() : "none", last_good ? std::to_string(last_good).c_str() : "none",
             journal_.journalling() ? "journaled" : "not all journaled (the journal stopped)", bytes / 1024);
        if (w.state_episodes >= wc_client::kStateEpisodes) {
            w.whole.reset();
            w.ring.clear();
        }
    }

    void session_client_core::world_check_sample(uint32_t tick, const std::vector<body_state>& rows,
                                                 world_check::why why) {
        auto& w = s.world_check_;
        // Step FD2: the sample's record (the newest whole one at or before it).
        if (w.whole) {
            w.ring.push_back(std::move(*w.whole));
            w.whole.reset();
            while (w.ring.size() > 2) w.ring.pop_front();
        }
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
        // Step FD3b: a record the in-place heal retired (the forked world's).
        if (w.heal_x != 0 && static_cast<int32_t>(tick - w.heal_x) <= 0) flags |= bmmo::session_world_report_msg::FLAG_PRE_HEAL;
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
            // Step FD3b (15.4): the rows ride the reports of an open
            // episode until its heal tick (a re-judge can still bring it
            // forward) and from a heal until its verify; not after (FD1's
            // server never read them past the decision: KL-like recurring
            // forks would upload them every second).
            const bool rows_due = w.open && (!w.decided || w.healing || static_cast<int32_t>(tick - w.rows_until) <= 0);
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
        // Step FD3b (schema 2): the last in-place heal, the reference a heal
        // is coded against, a missed heal once.
        msg.heal_x = w.heal_x;
        msg.held = s.resync_ref_;
        msg.last_full = s.last_snapshot_tick_;
        msg.k = static_cast<uint8_t>(std::min<uint32_t>(s.rollback_.sparse_every(), 0xFF));
        if (w.missed) {
            msg.flags |= bmmo::session_world_report_msg::FLAG_HEAL_MISSED;
            msg.missed_x = w.missed_x;
            msg.missed = w.missed_reason;
            s.world_check_.missed = false;
        }
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
            w.decided = w.healing = false;
            w.episode = msg.episode;
            w.episode_tick = msg.tick;
            w.episode_buckets = msg.buckets;
            ++w.episodes;
            // Step FD2: the records of T (first bad) and of the sample
            // before (last good) wait for the decision: first bad in
            // (T - 66, T], last good in (T - 132, T - 66].
            w.pending.clear();
            w.pending_episode =
                journal_.journalling() && w.state_episodes < wc_client::kStateEpisodes ? msg.episode : 0;
            for (auto it = w.ring.begin(); w.pending_episode != 0 && it != w.ring.end();) {
                const uint32_t before = msg.tick - it->tick;
                if (before < world_check::kSampleTicks) it->role = JOURNAL_STATE_FIRST_BAD;
                else if (before < 2 * world_check::kSampleTicks) it->role = JOURNAL_STATE_LAST_GOOD;
                else {
                    ++it;
                    continue;
                }
                w.pending.push_back(std::move(*it));
                it = w.ring.erase(it);
            }
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
            w.rows_until = msg.tick2;
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
                 "rule %s",
                 msg.tick, msg.episode, what.c_str(), heal.c_str(),
                 world_check::rule_name(static_cast<world_check::fork_rule>(msg.rule)));
            // Step FD2: the episode's records, after the line that says why
            // (a re-judge's second `decided` finds none pending).
            if (msg.episode == w.pending_episode) world_check_journal_states(msg.episode);
            break;
        }
        case world_check::fork_action::healing:
            // Step FD3b: the state of X follows on this channel.
            w.healing = true;
            logf("world check: episode %u: the server heals this world with its state of tick %u", msg.episode, msg.tick2);
            break;
        case world_check::fork_action::verified:
            w.healing = false;
            w.rows_until = msg.tick2;
            if (outcome == world_check::fork_outcome::re_fork)
                logf("world check: episode %u differs again after the heal (re-fork, %s); the server heals again at tick %u",
                     msg.episode, msg.worst.c_str(), msg.tick2);
            else   // `worst`: the keys still differing and whether they are shared (counted toward the removal or not)
                logf("world check: episode %u: the heal did not hold (still differing: %s); %s at tick %u (%u of 6)", msg.episode,
                     msg.worst.c_str(), msg.differing >= 6 ? "the next is a removal from the physics session"
                                                           : "the server retries in place",
                     msg.tick2, static_cast<unsigned>(msg.differing));
            break;
        case world_check::fork_action::kick:
            // The SessionEnd that follows ends the session as today.
            logf("world check: episode %u: removed from the physics session: the server's state did not hold after %u heals; "
                 "press Ready to rejoin", msg.episode, static_cast<unsigned>(msg.differing));
            break;
        case world_check::fork_action::closed: {
            if (msg.episode == w.episode) w.open = w.healing = false;
            if (msg.episode == w.pending_episode) {   // closed undecided (a lead, or equal again before any decision)
                w.pending.clear();
                w.pending_episode = 0;
            }
            if (outcome == world_check::fork_outcome::lead) {
                logf("world check: tick %u: lead (%s, one sample), closed", msg.tick, kinds_text(msg.kinds).c_str());
                break;
            }
            const char* how = outcome == world_check::fork_outcome::self_healed ? "equal again before its would-heal tick"
                              : outcome == world_check::fork_outcome::equal_again ? "equal again"
                              : outcome == world_check::fork_outcome::ended ? "ended with the member's session"
                              : outcome == world_check::fork_outcome::healed ? "healed: equal to the server's again"
                              : outcome == world_check::fork_outcome::superseded ? "superseded by a re-assign"
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
        w.whole.reset();
        w.ring.clear();
        w.pending.clear();
        w.pending_episode = 0;
    }

}
