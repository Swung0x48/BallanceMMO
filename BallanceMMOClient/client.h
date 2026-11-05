#pragma once
#include <thread>
#include <chrono>
#include <cassert>

#include <memory>
#include "common.hpp"

class client : public role {
public:
    bool connect(const std::string& connection_string) {
        connection_ = transport_->connect(connection_string);
        if (connection_ == bmmo::net::kInvalidConnection)
            return false;

        return true;
    }

    void run() override {
        mark_running();
        while (running_) {
            auto next_update = std::chrono::steady_clock::now() + bmmo::CLIENT_RECEIVE_INTERVAL;
            if (!update())
                std::this_thread::sleep_until(next_update);
        }
    }

    bmmo::net::connection_state get_connection_state() {
        return estate_;
    }

    virtual bool connected() {
        return get_connection_state() == bmmo::net::connection_state::connected;
    }

    virtual bool connecting() {
        return 
            get_connection_state() == bmmo::net::connection_state::connecting || 
            get_connection_state() == bmmo::net::connection_state::finding_route;
    }

    bool send(void* buffer, size_t size, int send_flags = bmmo::net::kSendReliable) {
        return transport_->send(connection_, buffer, size, send_flags);
    }

    template<bmmo::trivially_copyable_msg T>
    bool send(T msg, int send_flags = bmmo::net::kSendReliable) {
        static_assert(std::is_trivially_copyable<T>());
        return send(&msg,
            sizeof(msg),
            send_flags);
    }

    virtual void receive(void* data, size_t size) {}

    std::string get_detailed_status() {
        return transport_->detailed_status(connection_);
    }

    bmmo::net::connection_status get_status() {
        bmmo::net::connection_status status{};
        transport_->status(connection_, status);
        return status;
    }

    int get_ping() {
        return get_status().ping_ms;
    }

    void close_connection(bmmo::net::connection connection, bool linger = true) {
        if (connection != bmmo::net::kInvalidConnection) {
            transport_->close(connection, 0, "Goodbye", linger);
            assert(connection == this->connection_);
            connection_ = bmmo::net::kInvalidConnection;
            estate_ = bmmo::net::connection_state::none;
        }
    }

    void shutdown(bool linger) {
        if (running_) {
            running_ = false;
            close_connection(this->connection_, linger);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

protected:
    int poll_incoming_messages() override {
        const int msg_count = transport_->receive([this](const bmmo::net::message& msg) { on_message(msg); },
                                                  ONCE_RECV_MSG_COUNT);
        if (msg_count < 0)
            bmmo::FatalError("Error checking for messages.");
        return msg_count;
    }

    bmmo::net::connection connection_ = bmmo::net::kInvalidConnection;
    bmmo::net::connection_state estate_{};
};
