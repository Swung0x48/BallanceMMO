// A build without GameNetworkingSockets (BMMO_WITHOUT_GNS, step T1e): for a
// platform the library's dependencies are not set up on (Android).  Such a
// server or headless client runs only coordinated (--coordinator), where the
// transport is the coordinator's (net/remote_transport.hpp); the transport
// made here, before the coordinated one replaces it, is an empty simulated
// network that nothing connects to.

#include <net/gns.hpp>
#include <net/sim_transport.hpp>

#include <chrono>

namespace bmmo::net::gns {

    bool init(log_sink, std::string&) { return true; }
    void kill() {}
    void set_log_level(bmmo::log_type) {}
    std::unique_ptr<transport> make_transport() {
        static sim_network nowhere;
        return nowhere.make_transport();
    }

}

namespace bmmo::net {
    microseconds process_timestamp() {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
}
