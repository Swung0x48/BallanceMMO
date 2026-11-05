// BallanceMMOCoordinator (docs/canned-tests-plan.md, step T1c): the network and
// the time of a coordinated run.  The server and the headless clients started
// with --coordinator 127.0.0.1:PORT connect to it; it runs them on one virtual
// time (net/coordinator.hpp) until --virtual-seconds, then stops them.
//
//   BallanceMMOCoordinator --processes N [--port P] [--latency-ms L] [--loss-pct X]
//                          [--seed S] [--virtual-seconds T] [--lockstep 1] [--trace 1]
// --lockstep 1: one wake time per round, no lookahead (step T1e's reference).
// --latency-ms-for NAME:MS (repeatable): that client's latency instead of --latency-ms (step T2).

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <net/coordinator.hpp>

int main(int argc, char** argv) {
    bmmo::net::coordinator_options options;
    double seconds = 0.0;
    bool trace = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
        if (!v) { std::fprintf(stderr, "%s needs a value\n", arg.c_str()); return 2; }
        ++i;
        if (arg == "--processes") options.processes = std::strtoul(v, nullptr, 10);
        else if (arg == "--port") options.port = static_cast<uint16_t>(std::atoi(v));
        else if (arg == "--latency-ms") options.client_latency_ms = std::atoi(v);
        else if (arg == "--loss-pct") options.client_loss_pct = static_cast<float>(std::atof(v));
        else if (arg == "--seed") options.seed = std::strtoull(v, nullptr, 10);
        else if (arg == "--virtual-seconds") seconds = std::atof(v);
        else if (arg == "--trace") { trace = std::atoi(v) != 0; }
        else if (arg == "--lockstep") options.lockstep = std::atoi(v) != 0;
        else if (arg == "--latency-ms-for") {   // NAME:MS, a client's own latency (step T2)
            const std::string spec = v;
            const auto colon = spec.rfind(':');
            if (colon == std::string::npos) { std::fprintf(stderr, "--latency-ms-for NAME:MS\n"); return 2; }
            options.latency_ms_for[spec.substr(0, colon)] = std::atoi(spec.c_str() + colon + 1);
        }
        else { std::fprintf(stderr, "unknown argument %s\n", arg.c_str()); return 2; }
    }
    if (options.processes == 0) { std::fprintf(stderr, "usage: BallanceMMOCoordinator --processes N [...]\n"); return 2; }
    if (seconds > 0) options.until_us = options.start_us + static_cast<bmmo::net::microseconds>(seconds * 1e6);
    bmmo::net::coordinator coordinator(options);
    coordinator.set_trace(trace);
    coordinator.set_report([](const std::string& line) {
        std::printf("coordinator: %s\n", line.c_str());
        std::fflush(stdout);
    });
    std::printf("coordinator: listening on 127.0.0.1:%u for %zu processes\n", coordinator.port(), options.processes);
    std::fflush(stdout);
    const auto started = std::chrono::steady_clock::now();
    std::string error;
    const bool ok = coordinator.run(error);
    const double real = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const double virtual_s = static_cast<double>(coordinator.now() - options.start_us) / 1e6;
    std::printf("coordinator: %s - %llu rounds%s, %.3f virtual s in %.3f real s (%.2fx)\n", ok ? "done" : error.c_str(),
                static_cast<unsigned long long>(coordinator.rounds()), options.lockstep ? " (lockstep)" : "", virtual_s, real, real > 0 ? virtual_s / real : 0.0);
    std::fflush(stdout);
    return ok ? 0 : 1;
}
