// The shared session-client core (docs/session-client-core-design.md 1.1):
// the trafo as a lockstep input (docs/trafo-replication-design.md 2.3-2.9,
// 4.6-4.8; step T3).  With the session's world option WORLD_TRAFO_INPUTS and
// a host that can (session_capabilities::trafo_inputs), every member's trafo
// explosion, paper wind and reset run in this world on that member's own
// piece set, driven by the inputs the server's ledger fixed:
//
//   * the hold (poll_own_trafo): the originals' activations the live frame's
//     scripts asked for are cancelled by the host and reported; each report
//     is an own input, stamped tick + 1 + lead (the Delayer's prediction
//     under option E, decision 9.5), before the assignment on the first
//     assigned frame (hold_trafo, hold_pending_trafo), and logged;
//   * the trafo input log: own, remote and server-stamped inputs by applied
//     tick, applied before the union's pass of their tick (pre_step) and
//     again in every replayed tick (before_replayed_tick); the trafo frame
//     after the pass (platform trafo_frame);
//   * late inputs join the frame's input rewind; past the reach a resync
//     (trafo_rewind): a snapshot cannot make, reset or blow pieces;
//   * the own echoes: MOVED re-keyed and replayed, REJECTED / SUPERSEDED
//     dropped and replayed (process_trafo_echo);
//   * sets on first sight, never inside a replay (ensure_trafo_set);
//   * the reconcile at a resync's apply (reconcile_trafo);
//   * retirement and the log's pruning after the live frame (retire_trafo).

#include "session/client_core.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace bmmo::session {

    namespace {
        const char* trafo_kind(uint8_t flags) { return trafo_is_reset(flags) ? "reset" : "explosion"; }
        std::string trafo_flags_text(uint8_t flags) {
            std::string text;
            if (flags & TRAFO_SERVER) text += " (server)";
            if (flags & TRAFO_FINAL) text += " (final)";
            return text;
        }
        int32_t seed_of(int32_t session_seed, uint32_t player, uint32_t applied, uint8_t type, uint8_t flags) {
            return trafo_is_reset(flags) ? 0 : trafo_seed(static_cast<uint32_t>(session_seed), player, applied, type);
        }
    }

    // ------------------------------------------------------------- sets
    // Design 2.4 "When sets are made": at the anchor for every member and
    // the own player, for a player first seen in a message, for every player
    // a resync's ledger names - outside any replay, before any of the
    // player's inputs is applied (the first sight of a player whose input
    // arrives is that input's arrival).
    bool session_client_core::ensure_trafo_set(uint32_t player, const char* why) {
        if (!s.trafo_active_ || player == 0) return false;
        if (s.trafo_sets_.count(player)) return true;
        std::string error;
        if (!platform_.make_trafo_set(player, error)) {
            if (s.trafo_set_failures_++ < 8)
                logf("trafo: no piece set for player %u (%s): %s - its trafo inputs cannot run in this world", player, why,
                     error.c_str());
            return false;
        }
        s.trafo_sets_.insert(player);
        for (uint8_t type = 0; type < kTrafoTypes; ++type)
            for (const auto& piece: platform_.trafo_type_set(player, type).pieces) s.trafo_pieces_[piece] = player;
        ++s.trafo_sets_made_;
        logf("trafo: piece set of player %u ready (%s)", player, why);
        return true;
    }

    // ------------------------------------------------------------- hold
    // Design 2.3: an own activation the hold cancelled, as an input stamped
    // for `due` - sent like a sector entry (send_event: journaled, repeated),
    // and an own input of the log from now on, so the live tick and every
    // replay apply it where it was stamped until an echo says otherwise.
    void session_client_core::hold_trafo(const trafo_report& r, uint32_t due, const char* why) {
        const bool reset = trafo_is_reset(r.flags);
        bmmo::session_event_msg event;
        event.tick = due;
        event.type = event_type::Trafo;
        event.ball_type = r.type;
        event.flags = static_cast<uint8_t>(r.flags & TRAFO_RESET);
        for (int k = 0; k < 3; ++k) event.position[k] = reset ? 0.0f : r.position[k];
        send_event(event);
        trafo_input in;
        in.player = s.own_id_;
        in.type = r.type;
        in.flags = event.flags;
        in.origin = due;
        in.applied = due;
        for (int k = 0; k < 3; ++k) in.position[k] = event.position[k];
        in.seed = seed_of(s.seed_, s.own_id_, due, r.type, in.flags);
        in.own = true;
        s.trafo_log_.add(in);
        ++s.trafo_sent_;
        if (reset)
            logf("trafo: own %s reset stamped for tick %u (%s, %s; lead %u)", trafo_type_name(r.type), due, why,
                 r.source.c_str(), sector_lead_ticks());
        else
            logf("trafo: own %s explosion stamped for tick %u (%s, %s; lead %u) pos=%a,%a,%a seed=%d", trafo_type_name(r.type),
                 due, why, r.source.c_str(), sector_lead_ticks(), static_cast<double>(in.position[0]),
                 static_cast<double>(in.position[1]), static_cast<double>(in.position[2]), in.seed);
    }

    // Before the assignment the reports wait, in order, and are stamped on
    // the first assigned frame at that tick + the lead, as request_sector /
    // hold_pending_sector do: an anchor-relative stamp is never sent (a
    // predicted frame of the old numbering included).
    void session_client_core::hold_pending_trafo(uint32_t tick) {
        if (s.trafo_pending_.empty() || !s.assigned_) return;
        std::vector<trafo_report> waiting;
        waiting.swap(s.trafo_pending_);
        for (const auto& r: waiting) hold_trafo(r, tick + sector_lead_ticks(), "held until the assignment");
    }

    // After the live frame of `tick` (post_step, beside the own sector
    // poll): the host's hold and effect checks.  Option E (decision 9.5):
    // the Delayer's prediction is the stamp when the lead lets every world
    // have it in time; never earlier than tick + 1 + lead in any case.
    void session_client_core::poll_own_trafo(uint32_t tick) {
        if (!s.trafo_active_) return;
        const bool option_e = sector_lead_ticks() <= kTrafoOptionEMaxLead;
        const trafo_poll poll = platform_.poll_trafo(tick, option_e);
        if (poll.leaks > 0) {
            s.trafo_leaks_ += static_cast<uint64_t>(poll.leaks);
            if (s.trafo_leak_logs_++ < 16)
                logf("trafo_original_leaks at tick %u: %d (%s; %llu so far)", tick, poll.leaks, poll.first_leak.c_str(),
                     static_cast<unsigned long long>(s.trafo_leaks_));
        }
        for (const auto& r: poll.reports) {
            if (!s.assigned_) {
                s.trafo_pending_.push_back(r);
                logf("trafo: own %s %s at tick %u waits for the assignment", trafo_type_name(r.type), trafo_kind(r.flags), tick);
                continue;
            }
            uint32_t due = tick + 1 + sector_lead_ticks();
            const char* why = "the cancelled activation";
            if (r.predicted_tick) {
                why = "the Delayer's prediction";
                due = trafo_tick_max(due, r.predicted_tick);
            }
            hold_trafo(r, due, why);
        }
    }

    // ------------------------------------------------------------ intake
    // A remote or server-stamped Trafo event (the server-stamped resets of
    // our own player among them, design 4.6): its set made on first sight,
    // the input logged at the applied tick the server fixed, and a rewind
    // when that tick has been stepped already (design 4.7).
    void session_client_core::on_trafo_event(const bmmo::session_event_msg& msg) {
        ++s.trafo_received_;
        journal_.lifecycle_event(msg.player, msg);
        if (!s.trafo_active_) {
            logf("trafo: a %s of player %u at tick %u in a session without trafo inputs here: ignored", trafo_kind(msg.flags),
                 msg.player, msg.tick);
            return;
        }
        if (msg.ball_type >= kTrafoTypes) {
            logf("trafo: an input of player %u at tick %u with piece type %u: ignored", msg.player, msg.tick,
                 static_cast<unsigned>(msg.ball_type));
            return;
        }
        ensure_trafo_set(msg.player, "its trafo input");
        trafo_input in;
        in.player = msg.player;
        in.type = msg.ball_type;
        in.flags = msg.flags;
        in.origin = msg.origin_tick;
        in.applied = msg.tick;
        for (int k = 0; k < 3; ++k) in.position[k] = trafo_is_reset(msg.flags) ? 0.0f : msg.position[k];
        in.seed = seed_of(s.seed_, msg.player, msg.tick, msg.ball_type, msg.flags);
        s.trafo_log_.add(in);
        const bool stepped = s.frames_since_anchor_ >= 1;
        const bool late = stepped && static_cast<int32_t>(msg.tick - current_tick()) <= 0;
        logf("trafo: player %u %s %s for tick %u (stamped %u%s) received at local tick %u%s", msg.player,
             trafo_type_name(msg.ball_type), trafo_kind(msg.flags), msg.tick, msg.origin_tick,
             trafo_flags_text(msg.flags).c_str(), current_tick(), late ? ", late" : "");
        if (!late) return;
        // Before this numbering's first tick (a late joiner's catch-up, a
        // resync's base): no record of this numbering reaches it, and the
        // server's state the numbering started from has it.
        if (static_cast<int32_t>(msg.tick - s.tick_base_) < 0) {
            ++s.trafo_covered_;
            logf("trafo: input of %u at tick %u precedes this numbering's base %u: the server's state covers it", msg.player,
                 msg.tick, s.tick_base_);
            return;
        }
        trafo_rewind(msg.tick, msg.player, "late");
    }

    // A trafo input whose tick is stepped already: never applied at the live
    // tick (audit #5), but in a replay from the tick before it - this frame's
    // input rewind, which one replay runs for every correction of the frame
    // (replay_corrected_inputs).  Past the reach, a resync (design 4.7 "a
    // missed input"); a late input before a pending exact resync's anchor is
    // the state's.  Gated like the relay's corrections (rollback and
    // input_rewind, design 4.6), never by own_echoes or lifecycle_rewind.
    void session_client_core::trafo_rewind(uint32_t tick, uint32_t player, const char* why) {
        const uint32_t now = current_tick();
        if (!config.rollback || !config.input_rewind) {
            ++s.trafo_missed_;
            logf("trafo: input of %u at tick %u (%s) cannot be replayed at local tick %u: this client does not rewind for "
                 "inputs (trafo_missed %llu)", player, tick, why, now, static_cast<unsigned long long>(s.trafo_missed_));
            return;
        }
        if (s.resync_pending_ && s.resync_exact_ && static_cast<int32_t>(tick - s.resync_base_) < 0) {
            ++s.trafo_covered_;
            logf("trafo: input of %u at tick %u (%s) precedes the pending resync's anchor %u: the server's state covers it",
                 player, tick, why, s.resync_base_ - 1);
            return;
        }
        const uint32_t anchor = tick - 1;
        if (tick == 0 || !s.rollback_.knows_tick(anchor) || now - anchor > window_rewind_reach(tick)) {
            ++s.trafo_missed_;
            logf("trafo: input of %u at tick %u (%s) cannot be replayed at local tick %u (outside the history or the reach): "
                 "resync (trafo_missed %llu)", player, tick, why, now, static_cast<unsigned long long>(s.trafo_missed_));
            journal_.note("trafo input of " + std::to_string(player) + " at tick " + std::to_string(tick)
                          + " missed at local tick " + std::to_string(now) + ": resync");
            if (!s.resync_pending_) request_resync("a trafo input could not be replayed", true);
            return;
        }
        ++s.trafo_late_;
        const std::string entity = "trafo of " + std::to_string(player);
        if (!s.input_rewind_pending_ || static_cast<int32_t>(tick - s.input_rewind_tick_) < 0) {
            s.input_rewind_pending_ = true;
            s.input_rewind_tick_ = tick;
            s.input_rewind_entity_ = entity;
            s.input_rewind_own_ = false;
        }
        s.input_rewind_entities_.insert(entity);
        s.input_rewind_trafo_ = true;
        logf("trafo: input of %u at tick %u (%s) joins the input rewind at local tick %u (depth %u)", player, tick, why, now,
             now - anchor);
    }

    // ------------------------------------------------------- application
    // The inputs of `tick`, in the log's order (player, type, kind, origin):
    // flags and referentials of distinct type sets, so the order only has to
    // be the same everywhere (design 2.2 "Order inside a tick").  Live
    // (pre_step, after the held lifecycle and before the union's pass) and
    // in every replayed tick (before_replayed_tick).  A TRAFO_FINAL reset
    // marks its type set final (retirement, design 2.4).
    void session_client_core::apply_trafo_inputs(uint32_t tick, bool replayed) {
        if (!s.trafo_active_) return;
        const std::vector<trafo_input> inputs = s.trafo_log_.at(tick);
        for (const auto& in: inputs) {
            std::string error;
            if (!platform_.run_trafo(in, error)) {
                if (s.trafo_run_failures_++ < 8)
                    logf("trafo: the %s of player %u at tick %u did not run%s: %s", trafo_kind(in.flags), in.player, tick,
                         replayed ? " (replay)" : "", error.c_str());
                continue;
            }
            if (in.flags & TRAFO_FINAL) platform_.trafo_final(in.player, in.type, in.applied);
            if (replayed) {
                ++s.trafo_replayed_;
                continue;
            }
            ++s.trafo_applied_;
            logf("trafo: %s %s of player %u applied at tick %u%s%s", trafo_type_name(in.type), trafo_kind(in.flags), in.player,
                 tick, in.own ? " (own)" : "", trafo_flags_text(in.flags).c_str());
        }
    }

    // ------------------------------------------------------------ echoes
    // Design 4.6: an own Trafo input's echo.  Acked: nothing.  MOVED: the
    // input re-keyed to the server's tick with that tick's seed, and, when
    // either tick has been stepped, the input rewind from the earlier of the
    // two - the explosion made at the origin undone and made again at the
    // applied tick.  REJECTED / SUPERSEDED: the input dropped, and the input
    // rewind from its origin when that was stepped.
    void session_client_core::process_trafo_echo(const bmmo::session_event_msg& msg, bool stale) {
        const bool rejected = (msg.state & EVENT_STATE_REJECTED) != 0;
        const bool superseded = !rejected && (msg.state & EVENT_STATE_SUPERSEDED) != 0;
        const bool made = !rejected && !superseded;
        const bool moved = made && msg.tick != msg.origin_tick;
        if (made && !moved) {
            ++s.own_echo_acked_;
            return;
        }
        const char* verdict = rejected ? "rejected" : superseded ? "superseded" : "moved";
        if (rejected) ++s.own_echo_rejected_;
        else if (superseded) ++s.own_echo_superseded_;
        else ++s.own_echo_moved_;
        journal_.lifecycle_event(s.own_id_, msg);
        journal_.note(std::string("own echo: trafo ") + trafo_kind(msg.flags) + " stamped " + std::to_string(msg.origin_tick)
                      + " " + verdict + " (applied " + std::to_string(msg.tick) + ")");
        if (!s.trafo_active_ || msg.ball_type >= kTrafoTypes) {
            logf("own trafo %s stamped tick %u echoed %s (applied %u): no trafo inputs here", trafo_kind(msg.flags),
                 msg.origin_tick, verdict, msg.tick);
            return;
        }
        const uint32_t own = s.own_id_;
        const bool stepped = s.frames_since_anchor_ >= 1;
        const uint32_t now = current_tick();
        const auto reached = [&](uint32_t tick) { return stepped && static_cast<int32_t>(tick - now) <= 0; };
        if (moved) {
            ++s.trafo_echo_moved_;
            const int32_t seed = seed_of(s.seed_, own, msg.tick, msg.ball_type, msg.flags);
            if (!s.trafo_log_.move(own, msg.ball_type, msg.flags, msg.origin_tick, msg.tick, seed)) {
                // Not logged (sent before a resync's renumbering pruned it,
                // or never logged): the server's input all the same.
                trafo_input in;
                in.player = own;
                in.type = msg.ball_type;
                in.flags = msg.flags;
                in.origin = msg.origin_tick;
                in.applied = msg.tick;
                for (int k = 0; k < 3; ++k) in.position[k] = trafo_is_reset(msg.flags) ? 0.0f : msg.position[k];
                in.seed = seed;
                in.own = true;
                s.trafo_log_.add(in);
            }
            const uint32_t from = static_cast<int32_t>(msg.tick - msg.origin_tick) < 0 ? msg.tick : msg.origin_tick;
            logf("own trafo %s %s stamped tick %u echoed moved by the server (applied %u) at local tick %u%s: %s",
                 trafo_type_name(msg.ball_type), trafo_kind(msg.flags), msg.origin_tick, msg.tick, now, stale ? " (stale)" : "",
                 reached(from) ? "rewound for" : "re-keyed before its tick");
            if (reached(from)) trafo_rewind(from, own, "own echo moved");
            return;
        }
        ++s.trafo_echo_dropped_;
        const bool logged = s.trafo_log_.drop(own, msg.ball_type, msg.flags, msg.origin_tick);
        logf("own trafo %s %s stamped tick %u echoed %s by the server at local tick %u%s: dropped%s",
             trafo_type_name(msg.ball_type), trafo_kind(msg.flags), msg.origin_tick, verdict, now, stale ? " (stale)" : "",
             !logged ? " (not logged)" : reached(msg.origin_tick) ? ", rewound for" : " before its tick");
        if (logged && reached(msg.origin_tick)) trafo_rewind(msg.origin_tick, own, verdict);
    }

    // --------------------------------------------------------- reconcile
    // Design 4.7, at a resync's apply (try_apply_resync), outside the
    // replay and before its restore (adapted at T3, see the design's "As
    // built (step TR3)": the design runs it inside the resync after the
    // anchor's restore; here it makes the LIVE world's type sets what the
    // server's ledger has at S, so the restore finds every piece the
    // server's rows and blob name, and keeps out the ones it has none for):
    //   * a set for every player the ledger names (ensure_trafo_set), the
    //     final marks, and the ledger's explosion and reset in the log (the
    //     newest explosion at or before t outlives the history);
    //   * a type set the server has no bodies for at S: unmade when this
    //     world has any now, and every piece of it in `gone` (no record of
    //     the anchor brings it back: the restore plan and the revive skip
    //     them);
    //   * a type set with bodies at S: kept when this world's newest
    //     explosion applied by now is the ledger's and every piece has its
    //     body; otherwise unmade (when it has any) and made again with the
    //     ledger's position and seed - mass and friction are the event's
    //     draws, which no row carries; the rows and the blob put the server's
    //     state of S over it and the portable record the scripts'.
    // The event log is muted throughout (no lifetime change the records
    // describe: a remade piece keeps its generation, as a mirror made from
    // its row does).
    void session_client_core::reconcile_trafo(const bmmo::session_resync_state_msg& st, uint32_t anchor,
                                              std::set<std::string>& gone, std::vector<journal_trafo_entry>& journal) {
        std::map<std::pair<uint32_t, uint8_t>, const trafo_ledger_entry*> ledger;
        for (const auto& e: st.trafo_ledger) {
            if (e.type >= kTrafoTypes) continue;
            ensure_trafo_set(e.player, "the resync ledger");
            ledger[{e.player, e.type}] = &e;
        }
        std::set<std::pair<uint32_t, uint8_t>> keys;
        for (uint32_t player: s.trafo_sets_)
            for (uint8_t type = 0; type < kTrafoTypes; ++type) keys.insert({player, type});
        for (const auto& [key, e]: ledger) keys.insert(key);
        const uint32_t now = current_tick();
        uint32_t kept = 0, unmade = 0, remade = 0, failed = 0;
        const event_log_mute mute(*this);
        for (const auto& [player, type]: keys) {
            const auto it = ledger.find({player, type});
            const trafo_ledger_entry* e = it == ledger.end() ? nullptr : it->second;
            journal_trafo_entry j;
            j.player = player;
            j.type = type;
            if (e) {
                j.bits = static_cast<uint8_t>((e->has_explosion ? 1 : 0) | (e->has_bodies ? 2 : 0) | (e->has_reset ? 4 : 0)
                                              | (e->final ? 8 : 0));
                j.explode_applied = e->explode_applied;
                for (int k = 0; k < 3; ++k) j.position[k] = e->position[k];
                j.reset_applied = e->reset_applied;
                j.final_applied = e->final_applied;
            } else {
                j.bits = 16;
            }
            // This world's generation: its newest explosion applied by now.
            const trafo_input* mine = s.trafo_log_.newest_explosion(player, type, now);
            const uint32_t mine_applied = mine ? mine->applied : 0;
            const bool mine_known = mine != nullptr;
            if (e) {
                if (e->final) platform_.trafo_final(player, type, e->final_applied);
                const auto learn = [&](uint8_t flags, uint32_t applied) {
                    const trafo_input* had = s.trafo_log_.newest(player, type, trafo_is_reset(flags), applied);
                    if (had && had->applied == applied) return;
                    trafo_input in;
                    in.player = player;
                    in.type = type;
                    in.flags = flags;
                    in.origin = applied;
                    in.applied = applied;
                    if (!trafo_is_reset(flags))
                        for (int k = 0; k < 3; ++k) in.position[k] = e->position[k];
                    in.seed = seed_of(s.seed_, player, applied, type, flags);
                    in.own = player == s.own_id_;
                    s.trafo_log_.add(in);
                };
                if (e->has_explosion) learn(TRAFO_EXPLODE, e->explode_applied);
                if (e->has_reset) learn(TRAFO_RESET, e->reset_applied);
            }
            const trafo_type_view view = platform_.trafo_type_set(player, type);
            const bool want = e && e->has_bodies;
            if (!view.exists) {
                if (want) {
                    j.action = JOURNAL_TRAFO_FAILED;
                    ++failed;
                    logf("trafo reconcile at S %u: player %u %s has bodies at the server and no set here", anchor, player,
                         trafo_type_name(type));
                }
                journal.push_back(j);
                continue;
            }
            const bool have = view.bodies > 0;
            const bool whole = view.bodies == static_cast<int>(view.pieces.size());
            const auto unmake = [&]() {
                trafo_reconcile_spec spec;
                spec.action = trafo_reconcile_action::unmake;
                spec.player = player;
                spec.type = type;
                const trafo_reconcile_result r = platform_.trafo_reconcile(spec);
                if (!r.ok) {
                    j.action |= JOURNAL_TRAFO_FAILED;
                    ++failed;
                    logf("trafo reconcile at S %u: unmaking player %u %s failed: %s", anchor, player, trafo_type_name(type),
                         r.error.c_str());
                    return;
                }
                j.action |= JOURNAL_TRAFO_UNMADE;
                ++unmade;
                ++s.trafo_unmade_;
            };
            if (!want) {
                for (const auto& piece: view.pieces) gone.insert(piece);
                if (have) unmake();
                else ++kept;
                if (have)
                    logf("trafo reconcile at S %u: player %u %s has no bodies at the server, %d here: unmade", anchor, player,
                         trafo_type_name(type), view.bodies);
                journal.push_back(j);
                continue;
            }
            if (have && whole && mine_known && mine_applied == e->explode_applied) {
                ++kept;
                journal.push_back(j);
                continue;
            }
            if (have) unmake();
            trafo_reconcile_spec spec;
            spec.action = trafo_reconcile_action::remake;
            spec.player = player;
            spec.type = type;
            for (int k = 0; k < 3; ++k) spec.position[k] = e->position[k];
            spec.seed = seed_of(s.seed_, player, e->explode_applied, type, TRAFO_EXPLODE);
            spec.birth_tick = anchor;
            const trafo_reconcile_result r = platform_.trafo_reconcile(spec);
            if (!r.ok) {
                j.action |= JOURNAL_TRAFO_FAILED;
                ++failed;
                logf("trafo reconcile at S %u: remaking player %u %s (explosion %u) failed: %s", anchor, player,
                     trafo_type_name(type), e->explode_applied, r.error.c_str());
            } else {
                j.action |= JOURNAL_TRAFO_REMADE;
                ++remade;
                ++s.trafo_remade_;
                logf("trafo reconcile at S %u: player %u %s remade from the explosion at %u (here: %d of %zu bodies, newest "
                     "explosion %s)", anchor, player, trafo_type_name(type), e->explode_applied, view.bodies, view.pieces.size(),
                     mine_known ? std::to_string(mine_applied).c_str() : "none");
            }
            journal.push_back(j);
        }
        logf("trafo reconcile at S %u: %zu type sets (%zu in the ledger): kept %u, unmade %u, remade %u, failed %u; %zu pieces "
             "kept out of the restore", anchor, keys.size(), ledger.size(), kept, unmade, remade, failed, gone.size());
    }

    // --------------------------------------------------------- retirement
    // After the live frame (design 2.4 "When sets go"): the host's
    // retirement (deselect, then destroy history_ticks later), whose
    // deselected players get a set again at their next sight; the log's
    // entries older than the history dropped but each type set's newest
    // explosion and reset.
    void session_client_core::retire_trafo(uint32_t tick) {
        if (!s.trafo_active_) return;
        const uint32_t history = static_cast<uint32_t>(s.rollback_.thresholds().history_ticks);
        for (uint32_t player: platform_.retire_trafo_sets(tick, history)) {
            s.trafo_sets_.erase(player);
            for (auto it = s.trafo_pieces_.begin(); it != s.trafo_pieces_.end();)
                it = it->second == player ? s.trafo_pieces_.erase(it) : std::next(it);
            ++s.trafo_retired_;
            logf("trafo: set of player %u retired at tick %u", player, tick);
        }
        if (tick > history) s.trafo_log_.prune(tick - history);
    }

}
