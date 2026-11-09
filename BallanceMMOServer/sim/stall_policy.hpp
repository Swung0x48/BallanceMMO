#pragma once
// A stalled member (the user, 2026-11-09: "3 s, 20 s").  In the 2026-11-08
// public test a client whose game thread froze kept its connection (its
// network thread lives on) and the server applied its last frame - keys
// held - for 93 s, its ball rolling on in every world.
//
// A held tick whose substitute is older than kStallReleaseTicks is applied
// with the keys released: the same frame, keys 0 (its ball type, flags and
// camera stay).  The relay writes such a record inline (held_inline: a source
// kHeldInlineDistance ticks back or more), so every world - the owner's
// included - applies the same bytes, and the server journal records them.
// Never inside a lead hold (input_buffer::held): the SessionLead hands the
// owner the hold's frozen frame as final and the owner's client drops the
// range's held records as already learned, so a released record there would
// split the owner from every other world (the batch review, 2026-11-09).  A
// member stalled through a hold is released from the hold's end on.
// After kStallRemoveTimeout with no input datagram the server takes the
// member out of the session as a leave (server.cpp check_stalled_members).

#include <chrono>
#include <cstdint>

#include <entity/session.hpp>
#include <session/input_relay.hpp>

namespace bmmo::sim {
    inline constexpr uint32_t kStallReleaseTicks = 3 * bmmo::session::TICK_RATE;
    inline constexpr std::chrono::seconds kStallRemoveTimeout{20};
    // The released frame is no frame its owner sent, so it must reach the
    // owner inline: a reference would make its client resolve the source's
    // bytes, keys held, and split from the server.
    static_assert(kStallReleaseTicks >= bmmo::session::kHeldInlineDistance,
                  "a released held record must go inline (held_inline)");

    // The frame a held tick applies: the substitute, with its keys released
    // when it is older than kStallReleaseTicks and the tick is not in a lead
    // hold (`lead_held`, input_buffer::held).  `source` is the substitute's
    // tick (INPUT_SOURCE_DEFAULT: the default frame, keyless already).
    inline bmmo::session::input_frame stall_frame(const bmmo::session::input_frame& substitute, uint32_t tick,
                                                  uint32_t source, bool lead_held, bool& released) {
        bmmo::session::input_frame frame = substitute;
        released = !lead_held && source != bmmo::session::INPUT_SOURCE_DEFAULT
                && static_cast<int32_t>(tick - source) > static_cast<int32_t>(kStallReleaseTicks);
        if (released) frame.keys = 0;
        return frame;
    }
}
