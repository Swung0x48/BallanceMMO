// The shared session-client core (docs/session-client-core-design.md 1.1):
// begin, anchor, the hold, end; numbering, SessionAssign, the resync request
// and its limiter.  Step E1 builds only the object: the state and the
// platform it is given.

#include "session/client_core.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace bmmo::session {

    session_client_core::session_client_core(session_platform platform, session_capabilities caps)
        : platform_(std::move(platform)), caps_(caps) {
        if (const char* missing = missing_platform_operation(platform_, caps_))
            throw std::invalid_argument(std::string("session_client_core: the platform has no ") + missing);
    }

}
