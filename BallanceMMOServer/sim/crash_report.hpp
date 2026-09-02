#pragma once

namespace bmmo::sim {
    // Installs process-wide handlers that print the faulting address and a
    // symbolized backtrace to stderr when the tool crashes (access violation,
    // abort, assertion).  Headless runs are unattended and redirected to
    // files; without this a crash leaves nothing behind but an exit code.
    void install_crash_reporter();
}
