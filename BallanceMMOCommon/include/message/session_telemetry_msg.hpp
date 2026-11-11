#ifndef BALLANCEMMOSERVER_SESSION_TELEMETRY_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_TELEMETRY_MSG_HPP
#include "message.hpp"
#include "message_utils.hpp"

namespace bmmo {
    // What a member's frames looked like in the last kTelemetryTicks ticks
    // (step A6): the SessionTelemetry's fields, a plain copyable value (the
    // core keeps the last one for the F3 panel).  Times in 0.1 ms, saturated.
    struct session_telemetry {
        enum host_kind : uint8_t { HostRetail = 1, HostHeadless = 2 };

        uint32_t session = 0;
        uint32_t tick = 0;         // the window's last tick (the member's numbering)
        uint8_t host = 0;          // host_kind
        uint16_t ticks = 0;        // ticks the window spans
        uint16_t frames = 0;       // frames in it
        // The period from one frame to the next (retail: OnProcess to
        // OnProcess - render, live step and host; headless: frame start to
        // frame start), and the frames longer than 33.3 / 50 / 100 ms.
        uint16_t interval_p50 = 0, interval_p99 = 0, interval_p999 = 0, interval_max = 0;
        uint16_t over_33 = 0, over_50 = 0, over_100 = 0;
        // The session core's own share of a frame (retail core_host_frame,
        // headless frame()): rewinds and replays included.
        uint16_t work_p99 = 0, work_max = 0;
        uint16_t rewinds = 0;      // rollbacks of every class in the window
        uint32_t replayed = 0;     // ticks they re-simulated
        uint16_t resyncs = 0;      // resync requests sent
        uint16_t holds = 0;        // resync holds entered
        uint16_t starved = 0;      // input reports the server answered starved
        uint16_t behind_max = 0;   // the most the pacer was behind its schedule, 0.1 tick
        // How long a SessionRemoteInput waited from the transport's receive
        // stamp to its handover to the session core (step A5's measure).
        uint16_t inbound_p50 = 0, inbound_p99 = 0, inbound_max = 0;
    };

    // client -> server, reliable (protocol 38, step A6): the session_telemetry
    // of the window, for the server's log (the public test of 2026-11-08
    // uploaded no client log, so nobody's frame times were known).  The
    // server logs it and nothing reads it: no world, journal or schedule
    // depends on it.
    struct session_telemetry_msg : public serializable_message, public session_telemetry {
        static constexpr uint8_t kSchema = 1;

        session_telemetry_msg() : serializable_message(bmmo::SessionTelemetry) {}

        bool serialize() override {
            serializable_message::serialize();
            const uint8_t schema = kSchema;
            auto put = [this](const auto& v) { raw.write(reinterpret_cast<const char*>(&v), sizeof(v)); };
            put(schema); put(session); put(tick); put(host); put(ticks); put(frames);
            put(interval_p50); put(interval_p99); put(interval_p999); put(interval_max);
            put(over_33); put(over_50); put(over_100); put(work_p99); put(work_max);
            put(rewinds); put(replayed); put(resyncs); put(holds); put(starved); put(behind_max);
            put(inbound_p50); put(inbound_p99); put(inbound_max);
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize()) return false;
            uint8_t schema = 0;
            auto get = [this](auto& v) { return message_utils::read_variable(raw, &v); };
            if (!get(schema) || schema != kSchema) return false;
            return get(session) && get(tick) && get(host) && get(ticks) && get(frames)
                && get(interval_p50) && get(interval_p99) && get(interval_p999) && get(interval_max)
                && get(over_33) && get(over_50) && get(over_100) && get(work_p99) && get(work_max)
                && get(rewinds) && get(replayed) && get(resyncs) && get(holds) && get(starved) && get(behind_max)
                && get(inbound_p50) && get(inbound_p99) && get(inbound_max);
        }
    };
}

#endif //BALLANCEMMOSERVER_SESSION_TELEMETRY_MSG_HPP
