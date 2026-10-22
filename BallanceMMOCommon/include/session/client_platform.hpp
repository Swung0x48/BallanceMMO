#pragma once

// The platform interface of the shared session-client core
// (docs/session-client-core-design.md 2): what the core needs from the world,
// as a struct of std::function fields each host fills with what it has.  An
// empty field means "this host cannot" (the rollback_world convention,
// session/rollback_binding.hpp); where a decision depends on more than one
// field the host declares it in session_capabilities, and the core's
// constructor checks that every field a declared capability needs is filled.
//
// Step E1 (design 6.1) starts it with the three operations every host has
// and the headless platform fills: send, now and log.  Each later step adds
// the operations its moved code calls (design 2.2), and the capability checks
// with them.
//
// CK-free and header-only.

#include "clock_site.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace bmmo::session {

    // How a message goes out; the host maps it to k_nSteamNetworkingSend_*.
    enum class send_mode : uint8_t { reliable, reliable_no_nagle, unreliable_no_delay };

    enum class log_level : uint8_t { info, warn };

    // Where the own ball's Physicalize / Unphysicalize are learned (design
    // 2.3): the event log at post_step (headless), or the host's hooks,
    // queued and committed at post_step (retail).
    enum class own_lifecycle_source : uint8_t { event_log, host_hooks };

    struct session_platform {
        // ---- transport and time (design 2.2) ----
        std::function<void(const std::string& bytes, send_mode mode)> send;
        // The wall clock as the flow reads it, by the read's place in the flow
        // (session/clock_site.hpp): the headless client hands it to the
        // inbound capture or takes the captured value; retail ignores it.
        std::function<std::chrono::steady_clock::time_point(clock_site site)> now;
        // One line of text, written as it is (the harness parses the headless
        // client's lines).
        std::function<void(log_level level, const std::string& text)> log;
    };

    // Design 2.3.  Every flag defaults to "cannot"; a host declares what it
    // can.  At E1 no flag is read yet: each comes with the operations it
    // needs (own_body_control: make_own / unmake_own at E7; slew at E5;
    // nonblocking_hold at E3; whole_frame with bind_world at E9).
    struct session_capabilities {
        bool own_body_control = false;   // make_own / unmake_own filled
        bool slew = false;               // the host's pacer applies slew_step (2x raise, 0.9x lower)
        bool nonblocking_hold = false;   // the anchor hold returns to the host loop
        bool whole_frame = false;        // a replayed tick may run a whole engine frame (bind_world fills whole_frame)
        own_lifecycle_source own_lifecycle = own_lifecycle_source::event_log;   // or host_hooks
    };

    // Transitional (design 6.1, "Linking while the core is incomplete"): one
    // std::function per host function that code already moved into the core
    // still calls.  Each E step adds the entries its moved code needs and
    // deletes those whose target it moves; E11 deletes the struct.  E1 moves
    // no function, so it is empty.
    struct legacy_calls {
    };

    // The first operation `platform` lacks that every host must fill, or that
    // a capability `caps` declares needs; nullptr when nothing is missing.
    inline const char* missing_platform_operation(const session_platform& platform, const session_capabilities& caps) {
        (void)caps;
        if (!platform.send) return "send";
        if (!platform.now) return "now";
        if (!platform.log) return "log";
        return nullptr;
    }

}
