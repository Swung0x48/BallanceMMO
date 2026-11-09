#pragma once

namespace bmmo::sim {
    // Installs process-wide handlers that print the faulting address and a
    // symbolized backtrace to stderr when the tool crashes (access violation,
    // abort, assertion, an uncaught exception's std::terminate with its type
    // and what(), a CRT invalid parameter), write a minidump on Windows
    // (crash_report.cpp write_minidump) and end the process with a non-zero
    // code.  Headless runs are unattended and redirected to files; without
    // this a crash leaves nothing behind but an exit code.  Install it before
    // anything else that takes SIGABRT.
    void install_crash_reporter();

    // The terminate handler for the calling thread: MSVC keeps one per
    // thread (std::set_terminate in main does not reach a std::thread), so
    // every long-lived thread calls this first; elsewhere it is process-wide
    // and the call is harmless.
    void install_thread_crash_reporter();

    // Step SIO: called by the crash handlers after the backtrace and before
    // the process ends - the host's best-effort drain of its session writers
    // (session/io_queue.hpp io::drain_all, bounded).  Null: nothing.
    void set_crash_drain(void (*drain)());

    // Called last with the crash's one-line reason ("[crash] ..."): the
    // host's log (bmmo::crash_log_line, the --log file), which stderr alone
    // does not reach.  Null: stderr only.
    void set_crash_log(void (*log)(const char*));
}
