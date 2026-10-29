#pragma once

namespace bmmo::sim {
    // Installs process-wide handlers that print the faulting address and a
    // symbolized backtrace to stderr when the tool crashes (access violation,
    // abort, assertion).  Headless runs are unattended and redirected to
    // files; without this a crash leaves nothing behind but an exit code.
    void install_crash_reporter();

    // Step SIO: called by the crash handlers after the backtrace and before
    // the process ends - the host's best-effort drain of its session writers
    // (session/io_queue.hpp io::drain_all, bounded).  Null: nothing.
    void set_crash_drain(void (*drain)());
}
