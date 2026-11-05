#pragma once

// A log line's kind, from the most to the least serious.  The network
// library's own debug output uses the same values (net/gns.hpp hands its
// lines to the same sink), so a threshold set for one is the other's too.

namespace bmmo {

    enum class log_type : int {
        none = 0,
        bug = 1,
        error = 2,
        important = 3,
        warning = 4,
        msg = 5,
        verbose = 6,
        debug = 7,
        everything = 8,
    };

}
