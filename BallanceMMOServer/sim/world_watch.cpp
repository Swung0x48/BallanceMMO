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
        r.heal_x = m.heal_x;
        r.held = m.held;
        r.last_full = m.last_full;
        r.k = m.k;
        r.missed_x = m.missed_x;
        r.missed = m.missed;
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
        // Step FD3b: the fallback's own re-assign is its heal (verified from
        // its base); any other assignment while a decided episode is open - a
        // resync the member asked for, another re-answer - supersedes it
        // (7.5).  An undecided one (held unsettled) goes on as FD1's.
        if (!heal_ || !m.ep || !m.ep->decided) return;
        if (m.heal.k == heal_state::kind::reassign && m.heal.x == base - 1) return;
        close(m, now, wc_outcome::superseded, now);
    }

    void world_watch::leave(uint32_t player, uint32_t now) {
        const auto it = members_.find(player);
        if (it == members_.end()) return;
        auto& m = it->second;
        queue_.erase(std::remove(queue_.begin(), queue_.end(), player), queue_.end());
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
        // Step FD3b: what the next heal is coded against and captured at.
        m.k = r.k;
        m.held_ref = r.held;
        m.last_full = r.last_full;
        if (r.flags & bmmo::session_world_report_msg::FLAG_HEAL_MISSED) missed(m, r, now);
        if (r.flags & bmmo::session_world_report_msg::FLAG_DETAIL) detail(m, r, now);
        else handle(m, r, now);
    }

    void world_watch::handle(member_state& m, world_report_in& r, uint32_t now) {
        if (r.tick % wc::kSampleTicks != 0) return not_compared(m, r, "not_sample");
        if (!m.ready || r.epoch != m.base || static_cast<int32_t>(r.tick - m.base) < 0)
            return not_compared(m, r, "old_epoch");
        // A record the in-place heal retired, the forked world's (7.1 step 3).
        if (r.flags & bmmo::session_world_report_msg::FLAG_PRE_HEAL) return not_compared(m, r, "pre_heal");
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
        // Step FD3b: a heal waiting for the other members' reports of this T
        // (this one may be the last of them).
        for (auto& [id, other]: members_)
            if (other.verify && other.verify->tick == r.tick) try_verify(other, now);
        // A heal sent and not applied yet (its report has heal_x < X), or
        // the fallback not assigned yet: the sample says nothing of it.
        const bool outstanding = heal_ && m.heal.k != heal_state::kind::none && !verifying(m, r);
        if (heal_ && verifying(m, r)) m.heal.seen = true;   // no fallback for a heal the client applied
        if (!diff.differs()) {
            m.last_equal = r.tick;
            // A key of the episode latched at this sample is not compared:
            // it keeps the episode open.
            if (m.ep && !outstanding && std::none_of(m.ep->owners.begin(), m.ep->owners.end(), [&](uint32_t o) {
                    return contains(sample.latched, o) || contains(r.latched, o);
                })) {
                const auto& ep = *m.ep;
                const bool healed = heal_ && m.heal.k != heal_state::kind::none;
                if (healed) {
                    put(stat_kind::fork_verify, {m.id, ep.id, m.heal.x, r.tick, wc::outcome_name(wc_outcome::healed),
                                                 "", "", 0, 0, ""});
                    m.failed_in_row = 0;
                    m.failed_heals.clear();
                }
                close(m, r.tick, healed ? wc_outcome::healed
                                 : !ep.decided && ep.unsettled_a ? wc_outcome::lead
                                 : !ep.final ? wc_outcome::self_healed : wc_outcome::equal_again, now);
            }
            return;
        }
        ++differing_;
        if (!m.ep) return open(m, sample, r, diff, now);
        if (outstanding) return;
        if (heal_ && m.heal.k != heal_state::kind::none) return begin_verify(m, sample, r, diff, now);
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
        // Step FD3b (5.3): a difference within 3960 ticks of an episode's
        // close with a key in common reopens it (one fork_reopen; a line only
        // after a verified heal - a lead or a self-heal reopens silently).
        if (const auto& c = m.last_closed; heal_ && c && now - c->at < kReopenTicks
                && ((c->buckets & diff.buckets) != 0
                    || std::any_of(diff.owners.begin(), diff.owners.end(), [&](uint32_t o) { return contains(c->owners, o); }))) {
            ep.id = c->id;
            ep.reopened = true;
            put(stat_kind::fork_reopen, {r.player, ep.id, r.tick, now - c->at});
            if (c->healed) {
                notice n;
                n.player = r.player;
                n.line = m.name + ", episode " + std::to_string(ep.id) + " differs again at tick " + std::to_string(r.tick)
                       + ", " + std::to_string(now - c->at) + " ticks after its heal held (reopened: "
                       + kinds_text(diff.kinds) + ")";
                emit(std::move(n));
            }
            m.last_closed.reset();
        } else {
            ep.id = ++episodes_;
        }
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
                   + std::to_string(ep.id) + ": " + what + "); " + (heal_ ? "heals " : "would heal ") + when + ", rule "
                   + wc::rule_name(rule) + (heal_ ? "" : " (report only)");
            emit(std::move(n));
        }
        deadline(m, r.player, now);
    }

    void world_watch::deadline(member_state& m, uint32_t player, uint32_t now) {
        if (!m.ep || !m.ep->decided || m.ep->final || !after_or_at(now, m.ep->would_heal)) return;
        auto& ep = *m.ep;
        ep.final = true;
        ++would_heals_;
        put(stat_kind::fork_decision, {player, ep.id, ep.first, wc::tier_name(ep.tier), wc::rule_name(ep.rule), now, 0, 1});
        if (!heal_) {
            m.healed_once = true;
            m.last_would_heal = now;
            return;
        }
        // Step FD3b: due.  A member whose last 6 heals did not hold, alone,
        // is taken out of the session instead of a 7th (5.3, Q9 / Q10); a
        // HEAL_MISSED already asked for the fallback; else the runner
        // captures its state at its next whole-record tick (next_heal).
        if (m.kicked || m.heal.k != heal_state::kind::none) return;
        if (m.failed_in_row >= kKickAfter) return kick(m, now);
        if (!m.queued) {
            m.queued = true;
            queue_.push_back(player);
        }
    }

    void world_watch::close(member_state& m, uint32_t tick, wc_outcome outcome, uint32_t now) {
        const auto ep = *m.ep;
        m.ep.reset();
        // Step FD3b: what a reopen within 3960 ticks matches.
        m.queued = false;
        queue_.erase(std::remove(queue_.begin(), queue_.end(), m.id), queue_.end());
        m.verify.reset();
        const bool healed = outcome == wc_outcome::healed;
        const bool by_reassign = m.heal.k == heal_state::kind::reassign;
        m.heal = {};
        if (heal_) m.last_closed = closed_episode{ep.id, now, ep.owners, ep.buckets, healed};
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
        } else if (!heal_) {
            const char* how = outcome == wc_outcome::self_healed ? "equal again before its would-heal tick"
                            : outcome == wc_outcome::equal_again ? "equal again"
                                                                 : "ended with the member's session";
            n.line = m.name + ", episode " + std::to_string(ep.id) + " closed at tick " + std::to_string(end_tick) + ": "
                   + how + " after " + std::to_string(forked) + " ticks (" + seconds
                   + " forked; report only, nothing healed)";
        } else {
            // Step FD3b.
            const std::string heals = std::to_string(ep.heals.size()) + " heal" + (ep.heals.size() == 1 ? "" : "s");
            std::string how;
            if (outcome == wc_outcome::healed)
                how = std::string("healed ") + (by_reassign ? "by re-assign at " : "in place at ")
                    + std::to_string(ep.heals.empty() ? 0 : ep.heals.back()) + ", equal at " + std::to_string(end_tick);
            else if (outcome == wc_outcome::self_healed) how = "equal again before its heal";
            else if (outcome == wc_outcome::equal_again) how = "equal again";
            else if (outcome == wc_outcome::superseded) how = "superseded by a re-assign at " + std::to_string(m.base);
            else how = "ended with the member's session";
            n.line = m.name + ", episode " + std::to_string(ep.id) + " closed: " + how + " (" + heals + ", forked "
                   + seconds + ")";
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
                // Step FD3b: a verify past its wait for the other members'
                // reports, and an in-place state that never showed in a
                // report (heal_x below X for 660 ticks: lost or dropped
                // unreported) - the fallback.
                if (m.verify && after_or_at(now, m.verify->deadline)) try_verify(m, now);
                if (m.ep && m.heal.k == heal_state::kind::in_place && !m.heal.seen && now - m.heal.sent_at >= kExpectedTicks)
                    fallback(m, "its state of tick " + std::to_string(m.heal.x) + " never showed in a report", now);
                else if (m.ep && m.heal.k == heal_state::kind::asked)
                    fallback(m, "", now);   // held back by the guard: asked again
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
                                         blind_max_, heals_in_place_, heals_reassign_, heals_failed_, heals_shared_, kicks_});
        char text[256];
        std::snprintf(text, sizeof(text),
                      "%u episode%s (%llu lead%s), %llu would heal, forked s max %.1f / p50 %.1f, blind s max %.1f; "
                      "%llu samples, %llu reports compared, %llu differing",
                      episodes_, episodes_ == 1 ? "" : "s", static_cast<unsigned long long>(leads_), leads_ == 1 ? "" : "s",
                      static_cast<unsigned long long>(would_heals_), max_s, p50_s,
                      static_cast<double>(blind_max_) / bmmo::session::TICK_RATE,
                      static_cast<unsigned long long>(sampled_), static_cast<unsigned long long>(compared_),
                      static_cast<unsigned long long>(differing_));
        notice n;
        n.line = text;
        // Step FD3b: the heals, or FD1's "report only".
        if (!heal_) {
            n.line += " (report only)";
        } else {
            char heals[160];
            std::snprintf(heals, sizeof(heals), "; %llu heals (%llu in place, %llu re-assign), %llu did not hold (%llu shared), "
                          "%llu kicked",
                          static_cast<unsigned long long>(heals_in_place_ + heals_reassign_),
                          static_cast<unsigned long long>(heals_in_place_), static_cast<unsigned long long>(heals_reassign_),
                          static_cast<unsigned long long>(heals_failed_), static_cast<unsigned long long>(heals_shared_),
                          static_cast<unsigned long long>(kicks_));
            n.line += heals;
        }
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

    // ---- Step FD3b: the heal (5.3-5.5, 7.1-7.6, 15.4) ----

    void world_watch::life(uint32_t player, uint32_t tick) {
        auto& ticks = lives_[player];
        ticks.push_back(tick);
        if (ticks.size() > 16) ticks.erase(ticks.begin());
    }

    bool world_watch::verifying(const member_state& m, const world_report_in& r) const {
        switch (m.heal.k) {
        case heal_state::kind::in_place:
            return static_cast<int32_t>(r.tick - m.heal.x) > 0 && r.heal_x != 0 && static_cast<int32_t>(r.heal_x - m.heal.x) >= 0;
        case heal_state::kind::reassign:   // its epoch: handle() compares the new one's only
            return static_cast<int32_t>(r.tick - m.heal.x) > 0;
        default:
            return false;
        }
    }

    std::optional<world_watch::heal_order> world_watch::next_heal(uint32_t tick, bool other_state,
                                                                  const std::function<bool(uint32_t)>& pending) {
        if (!heal_ || other_state) return std::nullopt;
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            const auto found = members_.find(*it);
            if (found == members_.end() || !found->second.ep) continue;   // leave() and close() drop it
            auto& m = found->second;
            // X is a tick the member records whole (sparse capture, 15.1 row 3).
            if (m.k > 1 && tick % m.k != 0) continue;
            if (pending && pending(m.id)) continue;
            heal_order order{m.id, m.ep->id, m.base, m.ep->would_heal, m.held_ref};
            m.queued = false;
            queue_.erase(it);
            return order;
        }
        return std::nullopt;
    }

    void world_watch::heal_sent(const heal_order& order, uint32_t x, size_t bytes, bool coded, uint32_t now) {
        const auto it = members_.find(order.player);
        if (it == members_.end() || !it->second.ep) return;
        auto& m = it->second;
        auto& ep = *m.ep;
        m.heal = {heal_state::kind::in_place, x, now};
        m.healed_once = true;
        m.last_would_heal = now;
        ep.heals.push_back(x);
        ++ep.in_place;
        ++heals_in_place_;
        put(stat_kind::fork_heal, {m.id, ep.id, "in_place", x, order.due, now - order.due, bytes, coded ? 1 : 0, m.failed_in_row});
        notice n;
        n.player = m.id;
        n.send = true;
        n.episode = ep.id;
        n.tick = ep.first;
        n.action = wc_action::healing;
        n.kinds = ep.kinds;
        n.owners = ep.owners;
        n.buckets = ep.buckets;
        n.tier = ep.tier;
        n.rule = ep.rule;
        n.tick2 = x;
        char size[48];
        std::snprintf(size, sizeof(size), "%.0f KB, %s", static_cast<double>(bytes) / 1024.0, coded ? "delta" : "plain");
        n.line = m.name + ", episode " + std::to_string(ep.id) + ": healing in place at tick " + std::to_string(x) + " (" + size
               + (m.failed_in_row ? ", after " + std::to_string(m.failed_in_row) + " of " + std::to_string(kKickAfter)
                                    + " that did not hold" : std::string()) + ")";
        emit(std::move(n));
    }

    void world_watch::missed(member_state& m, const world_report_in& r, uint32_t now) {
        using msg = bmmo::session_world_report_msg;
        const char* why = msg::missed_name(r.missed);
        if (!heal_ || !m.ep || m.heal.k != heal_state::kind::in_place || r.missed_x != m.heal.x) {
            put(stat_kind::fork_verify, {m.id, m.ep ? m.ep->id : 0u, r.missed_x, r.tick, "missed", "", "", 0, m.failed_in_row,
                                         std::string("stale: ") + why});
            return;
        }
        put(stat_kind::fork_verify, {m.id, m.ep->id, r.missed_x, r.tick, "missed", "", "", 0, m.failed_in_row, why});
        // Dropped (a resync of the member's own under way, whose assignment
        // supersedes the episode; another epoch): the heal goes out again at
        // the gap, and a second drop in the episode falls back (7.6).
        const bool dropped = r.missed == static_cast<uint8_t>(msg::missed_reason::resync_pending)
                          || r.missed == static_cast<uint8_t>(msg::missed_reason::other_epoch);
        if (dropped && ++m.ep->drops < 2) {
            m.heal = {};
            requeue(m, now);
            return;
        }
        fallback(m, "its state of tick " + std::to_string(r.missed_x) + " could not be applied (" + why
                    + (dropped ? ", the second drop" : "") + ")", now);
    }

    // The episode due again (a heal that did not hold, a dropped state): at
    // NOW, never before the gap.
    void world_watch::requeue(member_state& m, uint32_t now) {
        auto& ep = *m.ep;
        ep.final = false;
        ep.decided = true;
        ep.tier = wc_tier::now;
        ep.would_heal = m.last_would_heal + kGapTicks;
        if (!after_or_at(ep.would_heal, now)) ep.would_heal = now;
    }

    // 7.6: the re-assign resync, unasked, through the server (sink.reassign
    // -> resync_member(..., "fork"), on the simulation thread under
    // state_mutex_), verified from its base.  7.2's guard can hold it back
    // while the member's own birth is known past the reach of the base it
    // would get (asked again every tick, at most 660 ticks).
    void world_watch::fallback(member_state& m, const std::string& why, uint32_t now) {
        auto& ep = *m.ep;
        const bool again = m.heal.k == heal_state::kind::asked;
        if (!again) m.heal = {heal_state::kind::asked, 0, now};
        const uint32_t asked_at = m.heal.sent_at;
        const uint32_t base = sink_.reassign ? sink_.reassign(m.id, ep.id, m.last_full, now - asked_at < kExpectedTicks) : 0;
        const std::string head = m.name + ", episode " + std::to_string(ep.id) + ": " + (again ? "" : why + "; ");
        if (base == 0) {
            if (!again) {
                notice n;
                n.player = m.id;
                n.line = head + "the re-assign waits until its own birth is within a base's reach";
                emit(std::move(n));
            }
            return;
        }
        m.heal = {heal_state::kind::reassign, base - 1, now};
        m.healed_once = true;
        m.last_would_heal = base - 1;   // the state's tick: the gap counts from it
        ep.heals.push_back(base - 1);
        ++ep.reassigned;
        ++heals_reassign_;
        put(stat_kind::fork_heal, {m.id, ep.id, "reassign", base - 1, asked_at, now - asked_at, 0, 0, m.failed_in_row});
        notice n;
        n.player = m.id;
        n.line = head + "healing by re-assign at " + std::to_string(base);
        emit(std::move(n));
    }

    void world_watch::kick(member_state& m, uint32_t now) {
        auto& ep = *m.ep;
        m.kicked = true;
        ++kicks_;
        std::string heals;
        for (uint32_t x: m.failed_heals) heals += (heals.empty() ? "" : " ") + std::to_string(x);
        put(stat_kind::fork_kick, {m.id, ep.id, heals, m.last_keys, m.last_shared, m.failed_in_row});
        notice n;
        n.player = m.id;
        n.send = true;
        n.episode = ep.id;
        n.tick = ep.first;
        n.action = wc_action::kick;
        n.kinds = ep.kinds;
        n.owners = ep.owners;
        n.buckets = ep.buckets;
        n.tick2 = now;
        n.differing = static_cast<uint16_t>(m.failed_in_row);
        n.kick = true;
        n.reason = "the world check: the server's state did not hold after " + std::to_string(m.failed_in_row)
                 + " heals in a row; press Ready to rejoin";
        const std::string span = m.failed_heals.empty() ? std::string()
            : " (" + std::to_string(m.failed_heals.front()) + "-" + std::to_string(m.failed_heals.back()) + ")";
        n.line = m.name + ", episode " + std::to_string(ep.id) + ": " + std::to_string(m.failed_in_row)
               + " heals in a row did not hold" + span + ", not shared with other members; removed from the physics session "
                 "(stays in the room)";
        emit(std::move(n));
    }

    // 5.4 with 15.0 change 3: the first differing sample after the heal.  A
    // zone-only difference, or a presence-only one on keys the episode does
    // not hold, decides nothing (the next sample does).
    void world_watch::begin_verify(member_state& m, const sample_entry& sample, const world_report_in& r,
                                   const wc::world_diff& diff, uint32_t now) {
        auto& ep = *m.ep;
        ep.last = r.tick;
        ++ep.samples;
        if (diff.kinds & wc::KIND_OWN) ++ep.own_samples;
        if (m.verify || diff.kinds == wc::KIND_ZONE) return;
        // A key of the episode latched at T is not compared: the next sample decides.
        if (std::any_of(ep.owners.begin(), ep.owners.end(),
                        [&](uint32_t o) { return contains(sample.latched, o) || contains(r.latched, o); }))
            return;
        const bool outside = (diff.buckets & ep.buckets) == 0
            && std::none_of(diff.owners.begin(), diff.owners.end(), [&](uint32_t o) { return contains(ep.owners, o); });
        if (outside && !r.rows.empty() && sample.rows) {
            std::vector<uint32_t> latched = sample.latched;
            latched.insert(latched.end(), r.latched.begin(), r.latched.end());
            if (assess(r.player, sample.rows.get(), r.rows, diff.buckets, latched).presence_only) return;
        }
        for (uint32_t o: diff.owners)
            if (!contains(ep.owners, o)) ep.owners.push_back(o);
        ep.buckets |= diff.buckets;
        m.verify = verify_wait{r.tick, now + kVerifyWaitTicks, diff.owners, diff.buckets};
        try_verify(m, now);
    }

    // Decided once every other compared member has reported T (or the wait
    // ran out): the Q9 grouping reads their terms of T.  re_fork when a
    // differing ball's owner had a lifecycle event in the server's world
    // after X (resets the count, definition B); else heal_failed, counted
    // toward the kick unless every still-differing key is shared - some
    // other member reported the same term at T (the user's Q9, "不算").
    // Either way the episode stays open at NOW, the next heal at the gap.
    bool world_watch::try_verify(member_state& m, uint32_t now) {
        if (!m.verify || !m.ep) {
            m.verify.reset();
            return false;
        }
        const verify_wait v = *m.verify;
        if (const sample_entry* sample = find_sample(v.tick); sample && !after_or_at(now, v.deadline))
            for (const auto& [id, other]: members_)
                if (id != m.id && other.cap && other.ready && !other.kicked && !sample->reports.count(id)) return false;
        auto& ep = *m.ep;
        const uint32_t x = m.heal.x;
        const auto groups = grouping(m.id, v.tick, v.owners, v.buckets);
        // The keys still differing, each shared (some other member reported
        // the same term at T) or this member's alone.
        std::vector<std::string> keys_v;
        for (uint32_t o: v.owners) keys_v.push_back(ball_key(o));
        for (int b = 0; b < wc::kBuckets; ++b)
            if (v.buckets & (1u << b)) keys_v.push_back("b" + std::to_string(b));
        std::string keys, shared_keys, alone_keys;
        const auto append = [](std::string& list, const std::string& key) { list += (list.empty() ? "" : " ") + key; };
        for (const auto& key: keys_v) {
            const bool same = std::any_of(groups.begin(), groups.end(), [&](const group& g) { return g.key == key && g.same > 0; });
            append(keys, key);
            append(same ? shared_keys : alone_keys, key);
        }
        const bool shared = !keys_v.empty() && alone_keys.empty();
        const std::string shared_text = grouping_record(groups);
        // A re-fork: a differing ball's owner had a lifecycle event in the
        // server's world in (X, T].
        uint32_t cause_player = 0, cause_tick = 0;
        for (uint32_t o: v.owners) {
            const auto l = lives_.find(o);
            if (l == lives_.end()) continue;
            for (uint32_t t: l->second)
                if (static_cast<int32_t>(t - x) > 0 && static_cast<int32_t>(t - v.tick) <= 0) cause_player = o, cause_tick = t;
            if (cause_player) break;
        }
        const bool re_fork = cause_player != 0;
        bool counted = false;
        if (re_fork) {
            m.failed_in_row = 0;
            m.failed_heals.clear();
        } else {
            ++heals_failed_;
            if (shared) {
                ++heals_shared_;
            } else {
                counted = true;
                ++m.failed_in_row;
                m.failed_heals.push_back(x);
            }
            m.last_keys = keys;
            m.last_shared = shared_text;
        }
        const wc_outcome outcome = re_fork ? wc_outcome::re_fork : wc_outcome::heal_failed;
        const std::string cause = re_fork ? ball_key(cause_player) + "@" + std::to_string(cause_tick) : std::string();
        put(stat_kind::fork_verify, {m.id, ep.id, x, v.tick, wc::outcome_name(outcome), keys, shared_text, counted ? 1 : 0,
                                     m.failed_in_row, cause});
        m.heal = {};
        m.verify.reset();
        requeue(m, now);
        // How the keys stand, for both sides' lines (the client's is `worst`).
        const std::string standing = shared ? "shared with other members, not counted"
            : shared_keys.empty() ? std::string("this member's alone")
            : shared_keys + " shared with other members, " + alone_keys + " this member's alone: counted";
        notice n;
        n.player = m.id;
        n.send = true;
        n.episode = ep.id;
        n.tick = ep.first;
        n.action = wc_action::verified;
        n.kinds = ep.kinds;
        n.owners = v.owners;
        n.buckets = v.buckets;
        n.tier = ep.tier;
        n.rule = ep.rule;
        n.tick2 = ep.would_heal;
        n.outcome = outcome;
        n.differing = static_cast<uint16_t>(m.failed_in_row);
        n.worst = re_fork ? keys : keys + "; " + standing;
        const std::string next = m.failed_in_row >= kKickAfter ? "; the next is a removal from the physics session at "
                                                               : "; retrying in place at ";
        if (re_fork)
            n.line = m.name + ", episode " + std::to_string(ep.id) + " differs again at tick " + std::to_string(v.tick)
                   + " after its heal at " + std::to_string(x) + " (re-fork: " + body_text(ball_key(cause_player))
                   + " had a lifecycle event at " + std::to_string(cause_tick) + "); healing again at "
                   + std::to_string(ep.would_heal);
        else
            n.line = m.name + ", episode " + std::to_string(ep.id) + ": the heal at " + std::to_string(x)
                   + " did not hold (still differs at " + std::to_string(v.tick) + ": " + keys + "; " + standing + ")" + next
                   + std::to_string(ep.would_heal) + " (" + std::to_string(m.failed_in_row) + " of "
                   + std::to_string(kKickAfter) + ")";
        emit(std::move(n));
        return true;
    }
}
