#ifndef BALLANCEMMOSERVER_SIM_ASSIGN_COPIES_HPP
#define BALLANCEMMOSERVER_SIM_ASSIGN_COPIES_HPP
// The unreliable copies of a SessionAssign (FULL steps SAX and SAXE).
//
// A SessionAssign is what starts a member's frames, so one that arrives late
// starts them that late for good.  The server sends it reliably and then
// unreliably again from its main loop (server.cpp send_assign_repeats), each
// copy in a datagram of its own; the clients apply the first to arrive and
// ignore the rest (client_core.cpp on_session_assign).
//
// Until SAX that was three copies 40 ms apart: four datagrams in a row on a
// downlink that carries nothing else between the start barrier and the
// session's first tick.  A loss burst counts datagrams, not milliseconds
// (scripts/arena_map_test.py's proxy: a drop takes the next burst - 1
// datagrams in the same direction; a real link's outage is the time-shaped
// case), so a burst of 4 that began at the reliable copy took all four (DFR,
// swing_snapoff, 150 ms, 10 % loss, burst 4, seed 0: the reliable retransmit
// ~330 ms later, the member ~25 ticks late, DX6's starved rule resynced it).
// SAX sent eight copies 30 ms apart: a burst of b from the reliable copy
// still started the member b x 30 ms late (8 ticks for b = 4).
//
// SAXE (the user's Q-SAX (e)) tried a volley: k copies in the assignment's
// own pass, each sent k_nSteamNetworkingSend_UnreliableNoNagle, so that a
// count-shaped burst would leave one at once.  This GameNetworkingSockets
// cannot send them as separate datagrams: no send flag builds a packet in the
// caller's thread (SendMessageToConnection passes no think-immediately out
// parameter, csteamnetworkingsockets.cpp:1248-1255; NoNagle only clears the
// Nagle timers and queues, steamnetworkingsockets_snp.cpp:421-429, then
// SetNextThinkTimeASAP, :526; even UseCurrentThread ends in
// CheckConnectionStateOrScheduleWakeUp, which is SetNextThinkTimeASAP and a
// FIXME, steamnetworkingsockets_connections.cpp:3317-3323), and the service
// thread fills one packet with every segment queued by then (snp.cpp
// ~2430-2500).  The volley stays behind the test flag --assign-volley
// (server.cpp) so the wire can show it again; the schedule below is the
// plan's fallback (b): one copy per main-loop pass.
//
// So the copies go one per main-loop pass (SERVER_RECEIVE_INTERVAL, ~15.2
// ms), never in the pass of the datagram before them and never sooner than
// kAssignCopyMinGap after it (an assignment the simulation thread makes just
// before a pass boundary must not put its first copy into the reliable one's
// packet).  kAssignRepeats = 16 keeps SAX's span (~242 ms, inside the ~330
// ms reliable retransmit at 150 ms each way, so a time-shaped outage shorter
// than that leaves one) and survives any one burst of up to 16 datagrams;
// after a burst of b from the reliable copy the first copy through goes b
// passes late (~61 ms = 4 ticks for b = 4), and with physics.lead_adjust off
// the member stays that late.  A copy is 48 bytes on the wire; an assignment
// is rare (the start, a late join, a resync), so the cost is nothing.
//
// The uplink cannot retire the copies early: a session_input_msg names no
// epoch, and a resynced member's old numbering already sends ticks at or
// after the new base, so no frame proves the new assignment arrived.

#include <chrono>
#include <cstdint>

namespace bmmo::sim {
    inline constexpr int kAssignRepeats = 16;   // unreliable copies after the reliable one, one per main-loop pass
    inline constexpr std::chrono::milliseconds kAssignCopyMinGap{5};

    // One assignment's copies still due.  `pass` is the server's main-loop
    // pass counter (it grows by one at the start of every pass), `now` the
    // steady clock; `sent` is called for the reliable copy (at the
    // assignment) and for every unreliable one.
    struct assign_copy_schedule {
        using time_point = std::chrono::steady_clock::time_point;
        int remaining = kAssignRepeats;
        uint64_t last_pass = 0;
        time_point last_sent{};

        static assign_copy_schedule after_reliable(uint64_t pass, time_point now, int copies = kAssignRepeats) {
            assign_copy_schedule s;
            s.remaining = copies;
            s.last_pass = pass;
            s.last_sent = now;
            return s;
        }
        bool due(uint64_t pass, time_point now) const {
            return remaining > 0 && pass > last_pass && now - last_sent >= kAssignCopyMinGap;
        }
        void sent(uint64_t pass, time_point now) {
            --remaining;
            last_pass = pass;
            last_sent = now;
        }
        bool done() const { return remaining <= 0; }
    };

    // A MODEL OF THE HARNESS PROXY's loss rule (scripts/arena_map_test.py),
    // not of a real link, used by the unit test only; it does not exercise
    // the server's send path.  On an idle downlink: datagram 0 is the reliable
    // copy, 1..copies the unreliable ones, and one burst drops `burst`
    // consecutive datagrams from index `start` (negative: it began on an
    // earlier datagram).  The index of the first datagram that arrives, or
    // -1 when the burst took every one.  Only the idle downlink before the
    // first tick: once frames are relayed, a copy shares its pass's datagram
    // with them, and "b lost datagrams = b passes late" no longer holds.
    constexpr int first_assign_copy_through_burst(int copies, int start, int burst) {
        for (int i = 0; i <= copies; ++i)
            if (i < start || i >= start + burst) return i;
        return -1;
    }
}

#endif
