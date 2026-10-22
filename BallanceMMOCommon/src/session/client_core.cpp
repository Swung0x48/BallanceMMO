// The shared session-client core (docs/session-client-core-design.md 1.1):
// begin, anchor, the hold, end; numbering, SessionAssign, the resync request
// and its limiter.  Step E1 builds only the object: the state and the
// platform it is given; E2 adds the core's logf.

#include "session/client_core.hpp"

#include <cstdarg>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>

namespace bmmo::session {

    session_client_core::session_client_core(session_platform platform, session_capabilities caps)
        : platform_(std::move(platform)), caps_(caps) {
        if (const char* missing = missing_platform_operation(platform_, caps_))
            throw std::invalid_argument(std::string("session_client_core: the platform has no ") + missing);
    }

    // The headless client's logf wrote the formatted text and a newline; the
    // platform's log writes the line it is given and the newline, so the
    // output is the same bytes.  A growing buffer: the status line alone runs
    // past 1.5 KB.
    void session_client_core::logf(const char* format, ...) {
        std::string text(256, '\0');
        va_list args;
        va_start(args, format);
        va_list again;
        va_copy(again, args);
        const int length = std::vsnprintf(text.data(), text.size(), format, args);
        if (length >= 0 && static_cast<size_t>(length) >= text.size()) {
            text.resize(static_cast<size_t>(length) + 1);
            std::vsnprintf(text.data(), text.size(), format, again);
        }
        va_end(again);
        va_end(args);
        text.resize(length > 0 ? static_cast<size_t>(length) : 0);
        platform_.log(log_level::info, text);
    }

}
