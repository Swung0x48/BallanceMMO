#pragma once

// The retail host of the shared session-client core
// (docs/session-client-core-design.md 3.2; session/core_host.cpp).  The mod's
// main header declares the host's methods on BallanceMMOClient; this header
// only names what they return and the one thing other files ask of the host.
// Nothing here may become state of BallanceMMOClient: that class hangs the
// engine at startup when its layout moves, so the host's state is file-scope
// statics in core_host.cpp.

#include <string>

class CKContext;

namespace bmmo::game {
    class proximity_countdown_state;   // game/script_state.hpp
}

namespace bmmo::session {
    struct session_platform;   // session/client_platform.hpp

    namespace retail {
        // 'session core on|off' (the transition switch of design 6.2): on by
        // default since 2026-10-27, and nothing in the legacy session path
        // reads it.
        bool core_enabled();
        // Step RK3: a core session past its anchor switches this level's
        // tutorial off (the world option session clock on Level 1) - the
        // core activates the tutorial's exit at its frame, so the main
        // branch's own exit on a countdown restart (OnStartLevel's timer)
        // stays out of it.  False whenever no core session runs.
        bool core_tutorial_owned();
        // FULL step ESCX: the core host's mute and the ESC menu overlay, for
        // the automation's 'pausechain' (session/menu_overlay.hpp); empty
        // while no core session runs.
        std::string menu_overlay_status(CKContext* context);
        // Step Q: the level's proximity countdowns as loaded (a file-scope
        // static of core_host.cpp; both paths' anchors restore them).
        bmmo::game::proximity_countdown_state& level_proximities();
        // Step T4: OnLoadObject of 3D Entities\Balls.nmo - the trafo
        // scripts' level reference captured (read-only), for the sets a
        // session with the trafo as an input builds; the line to log.
        std::string trafo_scripts_loaded(CKContext* context);
    }
}
