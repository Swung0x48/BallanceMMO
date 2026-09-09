#pragma once

// The two lifecycle judgements the physics session makes about events, kept as
// pure functions so the unit tests can exercise them without linking the
// simulation (the test target has no physics).
//
//   physicalize_pending    - the server's lifecycle barrier: is a Physicalize
//                            for this player already queued within reach?
//   repeat_event_duplicate - the intake guard: has this exact repeat event
//                            (BodyRevived) been accepted recently?

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "../entity/session.hpp"

namespace bmmo::session {
    // True when `events` holds a Physicalize for `player` stamped no later than
    // `horizon`.  The range may be any container whose elements expose
    // .player, .tick and .event.type (session_runner::pending_event does).
    //
    // The barrier used to ask for `tick`, which is too strict: the client
    // stamps a lifecycle event one tick past the frame that reports it
    // (physics_session_client.cpp:1135/1230 use current_tick() + 1) and the
    // server runs `input_delay` ticks behind the client frame it consumes, so
    // an event that belongs to the frame at `tick` is stamped at most
    // tick + input_delay + 1.  Asking for that horizon does not apply anything
    // earlier - the consumer still takes e.tick <= tick - it only stops the
    // barrier from burning a full second of wall clock on an event that is
    // already queued.
    template <typename EventRange>
    inline bool physicalize_pending(const EventRange& events, uint32_t player, uint32_t horizon) {
        for (const auto& e: events)
            if (e.player == player && e.tick <= horizon && e.event.type == event_type::Physicalize) return true;
        return false;
    }

    // One accepted repeat event, keyed by what repeats: the body, not the
    // stamp.  Measured on the production journals: one burst of the same body
    // carries stamps up to 18 ticks apart, so a key containing the stamp would
    // never match.
    struct repeat_event_key {
        uint32_t tick = 0;
        uint8_t ball_type = 0;
        std::string name;
    };

    // ~1.3 s at 66 ms/tick: comfortably wider than the 18-tick span measured
    // inside one repeat burst, narrow enough not to swallow a real re-report.
    inline constexpr int32_t kRepeatEventWindow = 90;
    // Per-connection ring of accepted repeat events.
    inline constexpr size_t kRepeatEventRing = 64;

    // True when `recent` already holds the same (ball_type, name) within
    // `window` ticks of `tick`.  The age is compared as a signed difference:
    // a late event's stamp is *older* than the entries around it, so the ring
    // cannot be assumed newest-last in stamp order.
    inline bool repeat_event_duplicate(const std::vector<repeat_event_key>& recent, uint8_t ball_type,
                                       const std::string& name, uint32_t tick,
                                       int32_t window = kRepeatEventWindow) {
        for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
            if (it->ball_type != ball_type || it->name != name) continue;
            if (std::abs(static_cast<int32_t>(tick - it->tick)) <= window) return true;
        }
        return false;
    }

    // Append an accepted repeat event to the ring, evicting the oldest.
    inline void record_repeat_event(std::vector<repeat_event_key>& recent, uint8_t ball_type,
                                    const std::string& name, uint32_t tick) {
        recent.push_back({tick, ball_type, name});
        if (recent.size() > kRepeatEventRing) recent.erase(recent.begin());
    }
}
