#pragma once

// The retail host of the shared session-client core
// (docs/session-client-core-design.md 3.2; session/core_host.cpp).  The mod's
// main header declares the host's methods on BallanceMMOClient; this header
// only names what they return and the one thing other files ask of the host.
// Nothing here may become state of BallanceMMOClient: that class hangs the
// engine at startup when its layout moves, so the host's state is file-scope
// statics in core_host.cpp.

namespace bmmo::game {
    class proximity_countdown_state;   // game/script_state.hpp
}

namespace bmmo::session {
    struct session_platform;   // session/client_platform.hpp

    namespace retail {
        // 'session core on|off' (the transition switch of design 6.2): off by
        // default, and nothing in the legacy session path reads it.
        bool core_enabled();
        // Step Q: the level's proximity countdowns as loaded (a file-scope
        // static of core_host.cpp; both paths' anchors restore them).
        bmmo::game::proximity_countdown_state& level_proximities();
    }
}
