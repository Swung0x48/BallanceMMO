// Fork detection step FD1, the server's half: sim/world_watch.hpp.
#include "world_watch.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace bmmo::sim {
    namespace wc = bmmo::session::world_check;
    using bmmo::session::body_kind;
    using bmmo::session::body_state;
    using bmmo::session::stat_kind;

    namespace {
        using wc::ball_key;
        using wc::body_text;
        using wc::kinds_text;
        using wc::owner_list;

        bool contains(const std::vector<uint32_t>& v, uint32_t x) { return std::find(v.begin(), v.end(), x) != v.end(); }

        std::string metres(double m) {
            char text[32];
            std::snprintf(text, sizeof(text), "%.3g m", m);
            return text;
        }

        double distance(const double a[3], const double b[3]) {
            const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        uint64_t ball_term(const wc::world_terms& t, uint32_t owner, bool& found) {
            for (const auto& [o, term]: t.balls)
                if (o == owner) { found = true; return term; }
            found = false;
            return 0;
        }

        bool after_or_at(uint32_t now, uint32_t tick) { return static_cast<int32_t>(now - tick) >= 0; }
    }

    world_report_in world_report_in::from(uint32_t player, const bmmo::session_world_report_msg& m) {
        world_report_in r;
        r.player = player;
        r.epoch = m.epoch;
        r.tick = m.tick;
        r.flags = m.flags;
        r.why = m.why;
        r.terms.rows = m.rows;
        r.terms.unkeyed = m.unkeyed;
        r.terms.dup = m.dup;
        r.terms.balls = m.balls;
        std::sort(r.terms.balls.begin(), r.terms.balls.end());
        std::copy(std::begin(m.buckets), std::end(m.buckets), std::begin(r.terms.buckets));
        r.latched = m.latched;
        std::sort(r.latched.begin(), r.latched.end());
        r.rows = m.body_rows;
        r.episode = m.episode;
        r.fold_first = m.fold_first;
        r.folds = m.folds;
        r.rewinds = m.rewinds;
        r.resyncs = m.resyncs;
        return r;
    }

    void world_watch::member(uint32_t player, uint32_t caps, const std::string& name, uint32_t now) {
        if (members_.count(player)) leave(player, now);
        auto& m = members_[player];
        m.id = player;
        m.name = name.empty() ? ball_key(player) : name;
        m.cap = (caps & bmmo::session::CAP_WORLD_CHECK) && (caps & bmmo::session::CAP_RESYNC_STATE);
        ++members_seen_;
        if (m.cap) {
            ++capable_;
            return;
        }
        put(stat_kind::fork_blind, {player, "no_capability", now, 0, 0});
        notice n;
        n.player = player;
        n.line = m.name + " is not compared: its client does not declare the world check (no_capability)";
        emit(std::move(n));
    }

    void world_watch::ready(uint32_t player, uint32_t base, uint32_t now) {
        const auto it = members_.find(player);
        if (it == members_.end()) return;
        auto& m = it->second;
        m.base = base;
        if (!m.ready) m.compared_at = now;   // the silent clock starts at the first assignment
        m.ready = true;
    }

    void world_watch::leave(uint32_t player, uint32_t now) {
        const auto it = members_.find(player);
        if (it == members_.end()) return;
        auto& m = it->second;
        if (m.ep) close(m, now, wc_outcome::ended, now);
        if (m.blind) blind_end(m, now, false);
        if (m.cap) --capable_;
        members_.erase(it);
    }

    world_watch::sample_entry* world_watch::find_sample(uint32_t tick) {
        for (auto& s: samples_)
            if (s.tick == tick) return &s;
        return nullptr;
    }

    const world_watch::sample_entry* world_watch::find_sample(uint32_t tick) const {
        for (const auto& s: samples_)
            if (s.tick == tick) return &s;
        return nullptr;
    }

    void world_watch::sample(uint32_t tick, std::vector<body_state> rows, std::vector<uint32_t> latched, bool full_row,
                             uint32_t now) {
        ++sampled_;
        sample_entry e;
        e.tick = tick;
        std::sort(latched.begin(), latched.end());
        e.terms = wc::fold_rows(rows, latched);
        e.latched = std::move(latched);
        e.rows = std::make_shared<const std::vector<body_state>>(std::move(rows));
        e.full_row = full_row;
        samples_.push_back(std::move(e));
        while (samples_.size() > kTerms) samples_.pop_front();
        if (samples_.size() > kRowSamples) samples_[samples_.size() - 1 - kRowSamples].rows.reset();
        // The reports that came before the runner stepped their T.
        for (auto& [player, m]: members_) {
            (void) player;
            if (m.held.empty()) continue;
            auto held = std::move(m.held);
            m.held.clear();
            for (auto& r: held) {
                if (static_cast<int32_t>(r.tick - tick) > 0) m.held.push_back(std::move(r));
                else handle(m, r, now);
            }
        }
    }

    void world_watch::report(world_report_in r, uint32_t now) {
        const auto it = members_.find(r.player);
        if (it == members_.end() || !it->second.cap) {
            agree(r, "no_capability");
            return;
        }
        auto& m = it->second;
        m.heard = true;
        m.heard_at = now;
        if (r.flags & bmmo::session_world_report_msg::FLAG_DETAIL) detail(m, r, now);
        else handle(m, r, now);
    }

    void world_watch::handle(member_state& m, world_report_in& r, uint32_t now) {
        if (r.tick % wc::kSampleTicks != 0) return not_compared(m, r, "not_sample");
        if (!m.ready || r.epoch != m.base || static_cast<int32_t>(r.tick - m.base) < 0)
            return not_compared(m, r, "old_epoch");
        if (samples_.empty() || static_cast<int32_t>(r.tick - samples_.back().tick) > 0) {
            if (m.held.size() < kHeld) m.held.push_back(std::move(r));
            else not_compared(m, r, "dropped");
            return;
        }
        sample_entry* sample = find_sample(r.tick);
        if (!sample) return not_compared(m, r, "stale");
        if (r.terms.balls.empty() && r.terms.unkeyed > 0) return not_compared(m, r, "uncomparable");
        compare(m, *sample, r, now);
    }

    void world_watch::agree(const world_report_in& r, const char* result, uint8_t kinds, const std::vector<uint32_t>& owners) {
        put(stat_kind::world_agree, {r.player, r.tick, r.epoch, result, r.flags, r.why, r.terms.rows, r.terms.unkeyed,
                                     r.terms.dup, kinds, owner_list(owners)});
    }

    void world_watch::not_compared(member_state& m, const world_report_in& r, const char* result) {
        agree(r, result);
        m.cause = result;
    }

    void world_watch::compare(member_state& m, sample_entry& sample, const world_report_in& r, uint32_t now) {
        const auto diff = wc::compare_terms(sample.terms, r.terms, sample.latched, r.latched, r.player);
        sample.reports[r.player] = r.terms;
        ++compared_;
        agree(r, diff.differs() ? "differs" : "equal", diff.kinds, diff.owners);
        m.compared_at = now;
        if (m.blind) blind_end(m, now, true);
        if (!diff.differs()) {
            m.last_equal = r.tick;
            // A key of the episode latched at this sample is not compared:
            // it keeps the episode open.
            if (m.ep && std::none_of(m.ep->owners.begin(), m.ep->owners.end(), [&](uint32_t o) {
                    return contains(sample.latched, o) || contains(r.latched, o);
                })) {
                const auto& ep = *m.ep;
                close(m, r.tick, !ep.decided && ep.unsettled_a ? wc_outcome::lead
                                 : !ep.final ? wc_outcome::self_healed : wc_outcome::equal_again, now);
            }
            return;
        }
        ++differing_;
        if (!m.ep) return open(m, sample, r, diff, now);
        auto& ep = *m.ep;
        ep.last = r.tick;
        ++ep.samples;
        if (diff.kinds & wc::KIND_OWN) ++ep.own_samples;
        for (uint32_t o: diff.owners)
            if (!contains(ep.owners, o)) ep.owners.push_back(o);
        if (ep.final) return;
        // Undecided, its DETAIL neither here nor timed out: a report without
        // rows (a T + 66 retired before the client heard `opened`, or two
        // samples a hitch released together) waits for it.
        if (!ep.decided && !ep.detail && !ep.no_detail && r.rows.empty()) return;
        // The client's rows ride on the reports of an undecided episode.
        std::vector<uint32_t> latched = sample.latched;
        latched.insert(latched.end(), r.latched.begin(), r.latched.end());
        const assessment a = r.rows.empty() || !sample.rows
            ? assessment{} : assess(r.player, sample.rows.get(), r.rows, ep.buckets, latched);
        judge(m, r, a, diff.kinds, false, now);
    }

    void world_watch::open(member_state& m, sample_entry& sample, const world_report_in& r, const wc::world_diff& diff,
                           uint32_t now) {
        episode ep;
        ep.id = ++episodes_;
        ep.first = ep.last = r.tick;
        ep.opened_at = now;
        ep.kinds = diff.kinds;
        ep.buckets = diff.buckets;
        ep.owners = diff.owners;
        ep.latched = sample.latched;
        ep.latched.insert(ep.latched.end(), r.latched.begin(), r.latched.end());
        ep.server_rows = sample.rows;
        ep.own_samples = (diff.kinds & wc::KIND_OWN) ? 1 : 0;
        m.ep = ep;
        put(stat_kind::fork_open, {r.player, ep.id, r.tick, diff.kinds, owner_list(diff.owners), diff.buckets, r.terms.rows,
                                   r.terms.unkeyed, r.terms.dup, m.last_equal, owner_list(sample.latched),
                                   owner_list(r.latched)});
        // The server's rows of T, unless its FULL checkpoint holds them.
        if (!sample.full_row && !sample.check_row && sample.rows && sink_.rows) {
            sink_.rows(r.tick, static_cast<uint8_t>(bmmo::session::JOURNAL_CHECKPOINT_FULL | bmmo::session::JOURNAL_CHECKPOINT_CHECK),
                       *sample.rows, 0);
            sample.check_row = true;
        }
        notice n;
        n.player = r.player;
        n.send = true;
        n.episode = ep.id;
        n.tick = r.tick;
        n.action = wc_action::opened;
        n.kinds = diff.kinds;
        n.owners = diff.owners;
        n.buckets = diff.buckets;
        emit(std::move(n));
        // Settled or held without rows: a zone-only sample, a pending why.
        // Everything else waits for the DETAIL answer (or its timeout).
        const auto why = static_cast<wc::why>(r.why);
        if (diff.kinds == wc::KIND_ZONE || why == wc::why::own_notices || why == wc::why::own_echo)
            judge(m, r, assessment{}, diff.kinds, true, now);
    }

    void world_watch::detail(member_state& m, const world_report_in& r, uint32_t now) {
        if (!m.ep || r.episode != m.ep->id || m.ep->detail || r.tick != m.ep->first) {
            agree(r, "detail_dropped");
            return;
        }
        auto& ep = *m.ep;
        ep.detail = true;
        if (!r.rows.empty() && sink_.rows)
            sink_.rows(r.tick, static_cast<uint8_t>(bmmo::session::JOURNAL_CHECKPOINT_LOCAL | bmmo::session::JOURNAL_CHECKPOINT_FINAL
                                                    | bmmo::session::JOURNAL_CHECKPOINT_CHECK), r.rows, r.player);
        const assessment a = r.rows.empty() || !ep.server_rows
            ? assessment{} : assess(r.player, ep.server_rows.get(), r.rows, ep.buckets, ep.latched);
        if (!a.presence.empty()) ep.kinds |= wc::KIND_PRESENCE;
        std::string folds, rewinds;
        folds.reserve(r.folds.size() * 16);
        for (const auto& [mech, balls]: r.folds) {
            char text[20];
            std::snprintf(text, sizeof(text), "%08x%08x", mech, balls);
            folds += text;
        }
        for (const auto& w: r.rewinds)
            rewinds += (rewinds.empty() ? "" : " ") + std::to_string(w.anchor) + ">" + std::to_string(w.at) + ":"
                     + std::to_string(w.cause) + ":" + std::to_string(w.depth) + (w.entity.empty() ? "" : ":" + w.entity);
        put(stat_kind::fork_detail, {r.player, ep.id, r.tick, a.differing, a.worst, a.worst_m, a.own_m, a.near_m, a.presence,
                                     grouping_record(grouping(r.player, ep.first, ep.owners, ep.buckets)),
                                     "@" + std::to_string(r.fold_first) + " " + folds, rewinds});
        if (!ep.decided) judge(m, r, a, ep.kinds, ep.samples == 1, now);
    }

    void world_watch::hold(member_state& m, uint32_t player, wc_rule rule) {
        auto& ep = *m.ep;
        if (ep.tier == wc_tier::unsettled && ep.rule == rule) return;
        ep.tier = wc_tier::unsettled;
        ep.rule = rule;
        put(stat_kind::fork_decision, {player, ep.id, ep.first, wc::tier_name(ep.tier), wc::rule_name(rule), 0, 0, 0});
    }

    void world_watch::judge(member_state& m, const world_report_in& r, const assessment& a, uint8_t kinds, bool first,
                            uint32_t now) {
        auto& ep = *m.ep;
        const auto why = static_cast<wc::why>(r.why);
        // (c): a pending own reason, within 660 ticks of the open (a decision
        // taken stands: it escalates only).
        if (!ep.decided && (why == wc::why::own_notices || why == wc::why::own_echo) && now - ep.opened_at < kExpectedTicks)
            return hold(m, r.player, wc_rule::expected);
        if (first) {
            // (a): a first sample that is zone-only or presence-only; without
            // rows the first sample is held as (a) once the DETAIL timed out.
            if (kinds == wc::KIND_ZONE) {
                ep.unsettled_a = true;
                return hold(m, r.player, wc_rule::first_zone);
            }
            if (!a.rows) {
                if (ep.no_detail) {
                    ep.unsettled_a = true;
                    hold(m, r.player, wc_rule::no_detail);
                }
                return;
            }
            if (a.presence_only) {
                ep.unsettled_a = true;
                return hold(m, r.player, wc_rule::first_presence);
            }
        }
        const int calm = sink_.calm ? sink_.calm(r.player) : 0;
        const wc_rule calm_rule = calm == 2 ? wc_rule::calm_finished : wc_rule::calm_absent;
        wc_tier tier = wc_tier::later;
        wc_rule rule = wc_rule::distant;
        if (a.rows && a.differing > 0) {
            // At the first sample a presence key is unsettled (a): the keys
            // both sides have decide (assess picks the worst among them).
            const uint32_t n = first ? a.settled : a.differing, rows = r.terms.rows;
            if (a.worst_m >= 0.5 || (!first && !a.presence.empty())) tier = wc_tier::now, rule = wc_rule::size;
            else if (a.own_m >= 0.05) tier = wc_tier::now, rule = wc_rule::own;
            else if (a.flags_ball) tier = wc_tier::now, rule = wc_rule::flags;
            else if (n >= 8 || (n >= 3 && 2 * n >= rows)) tier = wc_tier::now, rule = wc_rule::breadth;
            else if (calm) tier = wc_tier::now, rule = calm_rule;
            else if ((kinds & wc::KIND_OWN) || a.own_m > 0.0) tier = wc_tier::soon, rule = wc_rule::own;
            else if (a.near_m >= 0.0 && a.near_m <= 10.0) tier = wc_tier::soon, rule = wc_rule::nearby;
        } else {
            // No rows (no DETAIL, or none on this report): from the classes.
            const wc_rule from_classes = ep.detail ? wc_rule::classes : wc_rule::no_detail;
            if (calm) tier = wc_tier::now, rule = calm_rule;
            else if (kinds & (wc::KIND_OWN | wc::KIND_BALL | wc::KIND_SET | wc::KIND_ZONE)) tier = wc_tier::soon, rule = from_classes;
            else rule = from_classes;
        }
        uint32_t would_heal = tier == wc_tier::now ? now
                            : ep.opened_at + (tier == wc_tier::soon ? kSoonTicks : kLaterTicks);
        if (!after_or_at(would_heal, now)) would_heal = now;
        uint32_t gap_wait = 0;
        // The user's fixed gap: never within 660 ticks of the member's last would-heal.
        if (m.healed_once && !after_or_at(would_heal, m.last_would_heal + kGapTicks)) {
            gap_wait = m.last_would_heal + kGapTicks - would_heal;
            would_heal = m.last_would_heal + kGapTicks;
        }
        const bool first_decision = !ep.decided;
        if (!first_decision) {   // re-judged: it escalates only, the deadline never moves later
            if (!(tier < ep.tier || static_cast<int32_t>(would_heal - ep.would_heal) < 0)) return;
            tier = std::min(tier, ep.tier);
            if (static_cast<int32_t>(would_heal - ep.would_heal) > 0) would_heal = ep.would_heal;
        }
        ep.decided = true;
        ep.tier = tier;
        ep.rule = rule;
        ep.would_heal = would_heal;
        put(stat_kind::fork_decision, {r.player, ep.id, ep.first, wc::tier_name(tier), wc::rule_name(rule), would_heal,
                                       gap_wait, 0});
        if (first_decision) {
            notice n;
            n.player = r.player;
            n.send = true;
            n.episode = ep.id;
            n.tick = ep.first;
            n.action = wc_action::decided;
            n.kinds = ep.kinds;
            n.owners = ep.owners;
            n.buckets = ep.buckets;
            n.tier = tier;
            n.rule = rule;
            n.tick2 = would_heal;
            n.differing = a.differing;
            n.worst_m = static_cast<float>(a.worst_m);
            n.own_m = static_cast<float>(a.own_m);
            n.worst = a.worst;
            std::string what = !a.rows || a.worst.empty() ? kinds_text(ep.kinds)
                : (a.worst == ball_key(r.player) ? std::string("its own ball") : body_text(a.worst)) + ", " + metres(a.worst_m);
            if (a.differing > 1) what += ", " + std::to_string(a.differing) + " bodies differ";
            if (a.own_m > 0.0 && a.worst != ball_key(r.player)) what += ", its own ball " + metres(a.own_m);
            if (!(ep.kinds & wc::KIND_BUCKET)) what += "; mechanisms equal";
            for (const auto& g: grouping(r.player, ep.first, ep.owners, ep.buckets)) {
                what += "; " + (g.bucket >= 0 ? "mechanisms of bucket " + std::to_string(g.bucket)
                                : g.key == ball_key(r.player) ? std::string("its own ball") : body_text(g.key))
                      + ": " + std::to_string(g.same) + " of " + std::to_string(g.others) + " other members report the same";
                if (g.owner >= 0)
                    what += g.owner == 0 ? ", its owner sent no report" : g.owner == 1 ? ", its owner agrees" : ", its owner differs";
            }
            const std::string when = tier == wc_tier::now && gap_wait == 0 ? "now"
                : "by tick " + std::to_string(would_heal) + " (" + wc::tier_name(tier) + (gap_wait ? ", after the 10 s gap" : "") + ")";
            n.line = m.name + "'s world differs from the server's at tick " + std::to_string(ep.first) + " (episode "
                   + std::to_string(ep.id) + ": " + what + "); would heal " + when + ", rule " + wc::rule_name(rule)
                   + " (report only)";
            emit(std::move(n));
        }
        deadline(m, r.player, now);
    }

    void world_watch::deadline(member_state& m, uint32_t player, uint32_t now) {
        if (!m.ep || !m.ep->decided || m.ep->final || !after_or_at(now, m.ep->would_heal)) return;
        auto& ep = *m.ep;
        ep.final = true;
        ++would_heals_;
        m.healed_once = true;
        m.last_would_heal = now;
        put(stat_kind::fork_decision, {player, ep.id, ep.first, wc::tier_name(ep.tier), wc::rule_name(ep.rule), now, 0, 1});
    }

    void world_watch::close(member_state& m, uint32_t tick, wc_outcome outcome, uint32_t now) {
        const auto ep = *m.ep;
        m.ep.reset();
        const uint32_t end_tick = outcome == wc_outcome::ended ? now : tick;
        const uint32_t forked = end_tick - ep.first;
        if (outcome == wc_outcome::lead) ++leads_;
        else forked_.push_back(forked);
        put(stat_kind::fork_close, {m.id, ep.id, wc::outcome_name(outcome), ep.first, ep.last, ep.samples, forked,
                                    ep.own_samples * wc::kSampleTicks});
        notice n;
        n.player = m.id;
        n.send = true;
        n.episode = ep.id;
        n.tick = ep.first;
        n.action = wc_action::closed;
        n.kinds = ep.kinds;
        n.owners = ep.owners;
        n.buckets = ep.buckets;
        n.tier = ep.tier;
        n.rule = ep.rule;
        n.tick2 = end_tick;
        n.outcome = outcome;
        char seconds[32];
        std::snprintf(seconds, sizeof(seconds), "%.1f s", static_cast<double>(forked) / bmmo::session::TICK_RATE);
        if (outcome == wc_outcome::lead) {
            n.line = m.name + ", tick " + std::to_string(ep.first) + ": lead (" + kinds_text(ep.kinds)
                   + ", one sample, episode " + std::to_string(ep.id) + "), closed";
        } else {
            const char* how = outcome == wc_outcome::self_healed ? "equal again before its would-heal tick"
                            : outcome == wc_outcome::equal_again ? "equal again"
                                                                 : "ended with the member's session";
            n.line = m.name + ", episode " + std::to_string(ep.id) + " closed at tick " + std::to_string(end_tick) + ": "
                   + how + " after " + std::to_string(forked) + " ticks (" + seconds
                   + " forked; report only, nothing healed)";
        }
        emit(std::move(n));
    }

    void world_watch::blind_end(member_state& m, uint32_t now, bool say) {
        const uint32_t ticks = now - m.blind_first;
        blind_max_ = std::max(blind_max_, ticks);
        put(stat_kind::fork_blind, {m.id, m.blind_cause, m.blind_first, ticks, 1});
        m.blind = false;
        if (!say) return;
        notice n;
        n.player = m.id;
        n.line = m.name + " compared again from tick " + std::to_string(now) + " (blind " + std::to_string(ticks) + " ticks)";
        emit(std::move(n));
    }

    void world_watch::step(uint32_t now) {
        for (auto& [player, m]: members_) {
            if (m.ep) {
                auto& ep = *m.ep;
                if (!ep.detail && !ep.decided && !ep.no_detail && now - ep.opened_at >= kDetailTicks) {
                    ep.no_detail = true;
                    if (ep.samples == 1 && !ep.unsettled_a) {
                        ep.unsettled_a = true;
                        hold(m, player, wc_rule::no_detail);
                    }
                }
                // A calm moment brings a SOON or LATER would-heal forward (the gap still holds).
                if (ep.decided && !ep.final && ep.tier != wc_tier::now && sink_.calm) {
                    const int calm = sink_.calm(player);
                    uint32_t at = now;
                    if (m.healed_once && !after_or_at(at, m.last_would_heal + kGapTicks)) at = m.last_would_heal + kGapTicks;
                    if (calm && static_cast<int32_t>(at - ep.would_heal) < 0) {
                        ep.would_heal = at;
                        ep.rule = calm == 2 ? wc_rule::calm_finished : wc_rule::calm_absent;
                        put(stat_kind::fork_decision, {player, ep.id, ep.first, wc::tier_name(ep.tier),
                                                       wc::rule_name(ep.rule), at, at - now, 0});
                    }
                }
                deadline(m, player, now);
            }
            if (m.cap && m.ready && !m.blind && now - m.compared_at >= kBlindTicks) {
                m.blind = true;
                m.blind_cause = m.heard && static_cast<int32_t>(m.heard_at - m.compared_at) > 0 ? m.cause : "silent";
                m.blind_first = m.compared_at;
                put(stat_kind::fork_blind, {player, m.blind_cause, m.blind_first, now - m.blind_first, 0});
                notice n;
                n.player = player;
                n.line = m.name + " not compared for 10 s since tick " + std::to_string(m.blind_first) + " ("
                       + (std::string(m.blind_cause) == "silent" ? std::string("silent: no report") : std::string(m.blind_cause)) + ")";
                emit(std::move(n));
            }
        }
    }

    void world_watch::end(uint32_t now) {
        if (ended_) return;
        ended_ = true;
        while (!members_.empty()) leave(members_.begin()->first, now);
        if (members_seen_ == 0) return;
        std::vector<uint32_t> forked = forked_;
        std::sort(forked.begin(), forked.end());
        const double max_s = forked.empty() ? 0.0 : static_cast<double>(forked.back()) / bmmo::session::TICK_RATE;
        const double p50_s = forked.empty() ? 0.0 : static_cast<double>(forked[forked.size() / 2]) / bmmo::session::TICK_RATE;
        put(stat_kind::world_check_end, {members_seen_, sampled_, compared_, differing_, episodes_, leads_, would_heals_,
                                         blind_max_});
        char text[256];
        std::snprintf(text, sizeof(text),
                      "%u episode%s (%llu lead%s), %llu would heal, forked s max %.1f / p50 %.1f, blind s max %.1f; "
                      "%llu samples, %llu reports compared, %llu differing (report only)",
                      episodes_, episodes_ == 1 ? "" : "s", static_cast<unsigned long long>(leads_), leads_ == 1 ? "" : "s",
                      static_cast<unsigned long long>(would_heals_), max_s, p50_s,
                      static_cast<double>(blind_max_) / bmmo::session::TICK_RATE,
                      static_cast<unsigned long long>(sampled_), static_cast<unsigned long long>(compared_),
                      static_cast<unsigned long long>(differing_));
        notice n;
        n.line = text;
        emit(std::move(n));
    }

    world_watch::assessment world_watch::assess(uint32_t player, const std::vector<body_state>* server,
                                                const std::vector<body_state>& client, uint8_t buckets,
                                                const std::vector<uint32_t>& latched) const {
        assessment a;
        a.rows = true;
        // The rows the client sends: every keyed ball not latched, and the
        // mechanisms of the episode's buckets; the server's are filtered the same.
        const auto skip = [&](const body_state& row) {
            if (bmmo::session::keep_local_body(row.name)) return true;
            if (row.kind == body_kind::Ball) return row.owner == 0 || contains(latched, row.owner);
            return (buckets & (1u << wc::bucket_of(row.name))) == 0;
        };
        const auto key = [](const body_state& row) { return row.kind == body_kind::Ball ? ball_key(row.owner) : row.name; };
        // Per key and side: the last row, the rows and the sum of their terms
        // (two rows of one owner are a type switch's old and new mirror at T).
        struct side { const body_state* row = nullptr; unsigned rows = 0; uint64_t terms = 0; };
        std::map<std::string, std::pair<side, side>> keys;   // (server, client)
        const auto add = [](side& s, const body_state& row) { s.row = &row; ++s.rows; s.terms += wc::row_term(row); };
        const double* own = nullptr;
        for (const auto& row: *server) {
            if (row.kind == body_kind::Ball && row.owner == player) own = row.position;
            if (!skip(row)) add(keys[key(row)].first, row);
        }
        for (const auto& row: client)
            if (!skip(row)) add(keys[key(row)].second, row);
        uint16_t differing = 0;
        bool any_diff = false;
        for (const auto& [k, p]: keys) {
            const auto& [s, c] = p;
            const body_state *sv = s.row, *cv = c.row;
            if (s.rows == c.rows && s.terms == c.terms) continue;
            ++differing;
            // A second row of a key on one side only: a presence difference,
            // held like one (1.2); its rows are not compared.
            if (s.rows != c.rows && sv && cv) {
                a.presence += (a.presence.empty() ? "" : " ") + k + (s.rows > c.rows ? " dup@server" : " dup@client");
                continue;
            }
            if (sv && cv) {
                ++a.settled;
                any_diff = true;
                const double err = distance(sv->position, cv->position);
                if (a.worst.empty() || err > a.worst_m) a.worst = k, a.worst_m = err;
                if (sv->kind == body_kind::Ball) {
                    if (sv->owner == player) a.own_m = err;
                    const uint8_t mask = bmmo::session::BODY_FLAG_SIMULATED | bmmo::session::BODY_FLAG_COLLISION_ENABLED;
                    if ((sv->flags & mask) != (cv->flags & mask)) a.flags_ball = true;
                }
                if (own) {
                    const double d = std::min(distance(own, sv->position), distance(own, cv->position));
                    if (a.near_m < 0.0 || d < a.near_m) a.near_m = d;
                }
            } else {
                a.presence += (a.presence.empty() ? "" : " ") + k + (sv ? "@server" : "@client");
                if (own) {
                    const double d = distance(own, (sv ? sv : cv)->position);
                    if (a.near_m < 0.0 || d < a.near_m) a.near_m = d;
                }
            }
        }
        a.differing = differing;
        a.presence_only = !any_diff && !a.presence.empty();
        if (a.worst.empty() && !a.presence.empty()) a.worst = a.presence.substr(0, a.presence.find(' '));
        return a;
    }

    // For each differing class at T: how many other members reported the same
    // term as this member (of those that reported it), and whether a
    // differing ball's owner agrees with the server about its own ball.
    std::vector<world_watch::group> world_watch::grouping(uint32_t player, uint32_t tick, const std::vector<uint32_t>& owners,
                                                          uint8_t buckets) const {
        std::vector<group> out;
        const sample_entry* sample = find_sample(tick);
        if (!sample) return out;
        const auto mine = sample->reports.find(player);
        if (mine == sample->reports.end()) return out;
        for (uint32_t o: owners) {
            bool found = false;
            const uint64_t term = ball_term(mine->second, o, found);
            if (!found) continue;
            group g;
            g.key = ball_key(o);
            for (const auto& [q, terms]: sample->reports) {
                bool has = false;
                const uint64_t t = ball_term(terms, o, has);
                if (q == player || !has) continue;
                ++g.others;
                if (t == term) ++g.same;
            }
            if (o != player) {
                const auto owner = sample->reports.find(o);
                bool has = false, server_has = false;
                const uint64_t t = owner == sample->reports.end() ? 0 : ball_term(owner->second, o, has);
                const uint64_t s = ball_term(sample->terms, o, server_has);
                g.owner = !has ? 0 : server_has && t == s ? 1 : 2;
            }
            out.push_back(std::move(g));
        }
        for (int b = 0; b < wc::kBuckets; ++b) {
            if (!(buckets & (1u << b))) continue;
            group g;
            g.bucket = b;
            g.key = "b" + std::to_string(b);
            for (const auto& [q, terms]: sample->reports) {
                if (q == player) continue;
                ++g.others;
                if (terms.buckets[b] == mine->second.buckets[b]) ++g.same;
            }
            out.push_back(std::move(g));
        }
        return out;
    }

    // fork_detail's `same`: "p12 3/3 owner agrees; b5 0/3".
    std::string world_watch::grouping_record(const std::vector<group>& groups) {
        static constexpr const char* owner_text[] = {" owner no report", " owner agrees", " owner differs"};
        std::string out;
        for (const auto& g: groups)
            out += (out.empty() ? "" : "; ") + g.key + " " + std::to_string(g.same) + "/" + std::to_string(g.others)
                 + (g.owner >= 0 ? owner_text[g.owner] : "");
        return out;
    }
}
