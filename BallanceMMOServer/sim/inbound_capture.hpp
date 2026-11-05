#ifndef BALLANCEMMO_SIM_INBOUND_CAPTURE_HPP
#define BALLANCEMMO_SIM_INBOUND_CAPTURE_HPP

// The headless session client's inbound capture (step E0), since step K3 the
// shared session/capture.hpp; the replay goes through session/capture_seams.hpp
// (a replaying clock and transport).  The sim's old names, kept.

#include <session/capture.hpp>
#include <session/clock_site.hpp>

namespace bmmo::sim {

    using wall_site = bmmo::session::clock_site;
    using inbound_capture = bmmo::session::capture_file;

    inline const char* wall_site_name(wall_site site) { return bmmo::session::clock_site_name(site); }

}

#endif
