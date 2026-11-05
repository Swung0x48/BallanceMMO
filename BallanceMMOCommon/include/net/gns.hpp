#pragma once

// GameNetworkingSockets behind the transport (net/transport.hpp): the
// process-wide start and stop of the library and its transport.  This header
// does not include the library; src/net/gns_transport.cpp is the one file
// that does.

#include <memory>

#include "../utility/log_type.hpp"
#include "transport.hpp"

namespace bmmo::net::gns {

    // Where the library's own log lines go, and the least serious kind passed on.
    using log_sink = void (*)(bmmo::log_type type, const char* text);

    // Starts the library (once per process, before any transport); false with
    // `error` filled on failure.  Its lines go to `sink` from log_type::msg on.
    bool init(log_sink sink, std::string& error);
    void kill();
    void set_log_level(bmmo::log_type least_serious);

    std::unique_ptr<transport> make_transport();

}
