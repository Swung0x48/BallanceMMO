#pragma once

// The platform layer between the session code and the network library
// (docs/canned-tests-plan.md 3.1, seam C2).  Every host - the server, the
// headless client, the retail client, the console tools - talks to the
// network through a `transport` and these types only; GameNetworkingSockets
// is one implementation (net/gns.hpp; src/net/gns_transport.cpp is the only
// file that includes the library), a simulated network (net/sim_transport.hpp)
// another.
//
// The values below are the library's own (the handles it hands out, its send
// flags, its connection states and end-reason ranges): a handle is also a
// player's id on the wire, a send trace records the flags, and the logs print
// the states, so none of them changes with the layer.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace bmmo::net {

    // A connection's handle (the server also sends it as the player's id).
    using connection = uint32_t;
    inline constexpr connection kInvalidConnection = 0;

    // The transport's own clock, in microseconds: a message's receive time
    // and the timestamps the legacy (non-session) stream carries.
    using microseconds = int64_t;

    // Send flags: a combination of these bits.
    inline constexpr int kSendUnreliable = 0;
    inline constexpr int kSendNoNagle = 1;
    inline constexpr int kSendNoDelay = 4;
    inline constexpr int kSendReliable = 8;
    inline constexpr int kSendUnreliableNoNagle = kSendUnreliable | kSendNoNagle;
    inline constexpr int kSendUnreliableNoDelay = kSendUnreliable | kSendNoDelay | kSendNoNagle;
    inline constexpr int kSendReliableNoNagle = kSendReliable | kSendNoNagle;

    enum class connection_state : int {
        none = 0,
        connecting = 1,
        finding_route = 2,
        connected = 3,
        closed_by_peer = 4,
        problem_detected_locally = 5,
    };

    // Why a connection ended: 0 none given, [kEndApp, kEndAppException) the
    // application's own codes (entity/connection_end), from kEndAppException
    // on an application error.
    inline constexpr int kEndInvalid = 0;
    inline constexpr int kEndApp = 1000;
    inline constexpr int kEndAppMax = 1999;
    inline constexpr int kEndAppException = 2000;

    // The largest message a send takes (512 KiB).
    inline constexpr size_t kMaxMessageSize = 512 * 1024;

    // A received message, valid while the receiver's callback runs.
    struct message {
        connection conn = kInvalidConnection;
        void* data = nullptr;   // the receiver may decode in place
        int size = 0;
        microseconds received = 0;   // on the transport's clock (local_timestamp)
    };

    // A connection's state changed (in the order the transport saw it).
    struct status_change {
        connection conn = kInvalidConnection;
        connection_state state = connection_state::none;
        connection_state old_state = connection_state::none;
        int end_reason = kEndInvalid;
        std::string end_debug;     // the closing side's text
        std::string description;   // the transport's name for the connection (address, name)
    };

    // A connection's live numbers (all 0 when the transport has none for it).
    struct connection_status {
        int ping_ms = 0;           // the library's -1: not measured yet
        float quality_local = 0, quality_remote = 0;   // 1 = no loss; < 0 = unknown
        float out_packets_per_sec = 0, out_bytes_per_sec = 0;
        float in_packets_per_sec = 0, in_bytes_per_sec = 0;
        int send_rate_bytes_per_sec = 0;
        int pending_unreliable = 0, pending_reliable = 0, sent_unacked_reliable = 0;
        microseconds queue_time = 0;
    };

    // The network layer's process clock (the library's): every transport's
    // local_timestamp, the log's elapsed stamps, the legacy stream's times.
    microseconds process_timestamp();

    // Step T1b: the sending thread's rank in a coordinated run (remote_transport:
    // the coordinator applies one step's sends of a process by rank, then
    // sequence).  The server's network loop 0, its runner 1; a thread that sets
    // none is 0.  Nothing else reads it.
    inline thread_local uint8_t t_thread_rank = 0;
    inline void set_thread_rank(uint8_t rank) { t_thread_rank = rank; }
    inline uint8_t thread_rank() { return t_thread_rank; }

    class transport {
    public:
        virtual ~transport() = default;

        // Server side: listen on `port`; an arriving connection shows up as a
        // status change to `connecting`, and receives only once accepted.
        virtual bool listen(uint16_t port) = 0;
        enum class accept_result { ok, refused, no_receive_set };
        // Accepts and adds it to the connections receive() reads; on failure
        // the caller closes it.
        virtual accept_result accept(connection c) = 0;
        // Client side: connect to "host:port"; kInvalidConnection on failure.
        virtual connection connect(const std::string& address) = 0;

        virtual bool send(connection c, const void* data, size_t size, int flags) = 0;
        virtual void flush(connection c) = 0;
        // `linger`: the reliable data still queued is sent before it closes;
        // an empty `debug` gives the library no text.
        virtual void close(connection c, int reason, const std::string& debug, bool linger) = 0;
        virtual void set_name(connection c, const std::string& name) = 0;

        virtual bool status(connection c, connection_status& out) = 0;
        virtual std::string detailed_status(connection c) = 0;
        virtual std::string remote_address(connection c) = 0;   // the peer's IP, no port

        // Every message waiting (up to `max`; the server's accepted
        // connections, a client's one), each handed to `each` and freed when
        // it returns.  The count.
        virtual int receive(const std::function<void(const message&)>& each, int max) = 0;
        // The state changes since the last call, in order.
        virtual void poll_status(const std::function<void(const status_change&)>& each) = 0;

        virtual microseconds local_timestamp() = 0;

        // Test link faults on every connection of this process (the harness's
        // and the retail automation's `net lag` / `net blackout`): extra
        // latency in ms, loss in percent, per direction.  Whether it took.
        enum class direction { send, recv };
        virtual bool set_fake_lag(direction d, int ms) = 0;
        virtual bool set_fake_loss(direction d, float percent) = 0;
        // A connection is dropped after this long without an answer.
        virtual int connection_timeout_ms() = 0;
    };

}
