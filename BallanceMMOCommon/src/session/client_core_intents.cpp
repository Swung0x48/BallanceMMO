// The shared session-client core (docs/session-client-core-design.md 1.1):
// the player intents (step INT, session/intent.hpp; docs/realtime-design.md
// 'Built (player intents in the journal, step INT, 2026-11-15)').
//
// An intent is journaled (stat kind `intent`) and sent to the server
// (SessionIntent) by the live frame that shows it - never by a replayed
// tick, which never reaches post_step - at the session tick it applies at:
//
//   * reset_level: the tick whose frame delivered "Reset Level" to Level
//     (the platform's poll_level_intents, game::level_intent_watch), with
//     the source a BMMO trigger noted (note_reset_request), else dead_menu
//     after a game over, else pause_menu when the frame before was paused;
//   * game_over: the tick whose frame left Menu_Dead active (a rising edge);
//   * pause_enter / pause_leave: the frame whose observed pause changed
//     (post_step, on the live observation);
//   * exit_level: the core's tick at the host's request (a label: the exit
//     takes effect at the server's removal of the member).
//
// None of this moves a world: the reset's world-visible effects are the
// owner's lifecycle events, stamped, sent and replayed as before.

#include "session/client_core.hpp"

#include <cstdint>
#include <string>

#include "session/journal_stats.hpp"

namespace bmmo::session {

    namespace {
        // A noted reset no frame delivered within this many ticks is dropped.
        constexpr uint32_t kResetRequestTicks = 5 * static_cast<uint32_t>(TICK_RATE);

        int64_t or_none(uint32_t v) { return v == INTENT_NONE ? -1 : static_cast<int64_t>(v); }
    }

    void session_client_core::note_reset_request(intent_source source, uint32_t arg) {
        s.pending_reset_ = {true, source, current_tick(), arg};
        logf("intent: a level reset asked (%s) after tick %u", intent_source_name(source), current_tick());
    }

    void session_client_core::request_level_exit(intent_source source) {
        if (s.level_exit_recorded_) return;
        s.level_exit_recorded_ = true;
        if (source == intent_source::unknown)
            source = s.game_over_seen_ ? intent_source::dead_menu
                   : s.finished_seen_  ? intent_source::finish_menu
                   : s.last_paused_    ? intent_source::pause_menu
                                       : intent_source::unknown;
        record_intent(intent_kind::exit_level, source, current_tick(), INTENT_NONE, INTENT_NONE);
    }

    void session_client_core::record_intent(intent_kind kind, intent_source source, uint32_t tick, uint32_t request,
                                            uint32_t arg) {
        // Before the assignment the ticks are not the session's yet (the
        // assignment renumbers them).
        if (!s.assigned_) {
            logf("intent: %s (%s) at tick %u before the assignment: not recorded", intent_kind_name(kind),
                 intent_source_name(source), tick);
            return;
        }
        // Journaled before the send, as the frames are.
        if (journal_.journalling())
            journal_.writer().stat(tick, stat_kind::intent,
                                   {tick, or_none(request), static_cast<unsigned>(s.own_member_life_),
                                    intent_kind_name(kind), intent_source_name(source), or_none(arg)});
        logf("intent: %s (%s) at tick %u%s", intent_kind_name(kind), intent_source_name(source), tick,
             request == INTENT_NONE ? "" : (" (asked after tick " + std::to_string(request) + ")").c_str());
        bmmo::session_intent_msg msg;
        msg.session = s.session_;
        msg.tick = tick;
        msg.request = request;
        msg.arg = arg;
        msg.life = s.own_member_life_;
        msg.kind = static_cast<uint8_t>(kind);
        msg.source = static_cast<uint8_t>(source);
        msg.serialize();
        platform_.send(msg.raw.str(), send_mode::reliable);
    }

    void session_client_core::poll_level_intents(uint32_t tick, bool paused_before) {
        if (s.pending_reset_.armed && static_cast<int32_t>(tick - s.pending_reset_.request) > static_cast<int32_t>(kResetRequestTicks)) {
            logf("intent: the level reset asked (%s) after tick %u was not delivered by tick %u: dropped",
                 intent_source_name(s.pending_reset_.source), s.pending_reset_.request, tick);
            s.pending_reset_ = {};
        }
        if (!platform_.poll_level_intents) return;
        const level_intents_poll seen = platform_.poll_level_intents(tick);
        if (!s.menu_dead_primed_) {
            s.menu_dead_primed_ = true;   // an already open Menu_Dead is no edge
            s.menu_dead_ = seen.menu_dead;
        } else if (seen.menu_dead != s.menu_dead_) {
            s.menu_dead_ = seen.menu_dead;
            if (seen.menu_dead) {
                record_intent(intent_kind::game_over, intent_source::unknown, tick, INTENT_NONE, INTENT_NONE);
                s.game_over_seen_ = true;
            }
        }
        if (!seen.reset) return;
        intent_source source = intent_source::unknown;
        uint32_t request = INTENT_NONE, arg = INTENT_NONE;
        if (s.pending_reset_.armed) {
            source = s.pending_reset_.source;
            request = s.pending_reset_.request;
            arg = s.pending_reset_.arg;
            s.pending_reset_ = {};
        } else if (s.game_over_seen_) {
            source = intent_source::dead_menu;
        } else if (paused_before) {
            source = intent_source::pause_menu;
        }
        record_intent(intent_kind::reset_level, source, tick, request, arg);
        s.game_over_seen_ = s.finished_seen_ = false;   // a later exit is from this run of the level
    }

}
