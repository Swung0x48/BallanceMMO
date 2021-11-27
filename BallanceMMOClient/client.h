#pragma once
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif
#include <thread>
#include <chrono>
#include <cassert>

#include <memory>
#include "../../BallanceMMOCommon/common.hpp"

class client : public role {
public:
    bool connect(const std::string& connection_string) {
        SteamNetworkingIPAddr server_address{};
        if (!server_address.ParseString(connection_string.c_str())) {
            return false;
        }
        SteamNetworkingConfigValue_t opt = generate_opt();
        connection_ = interface_->ConnectByIPAddress(server_address, 1, &opt);
        if (connection_ == k_HSteamNetConnection_Invalid)
            return false;

        return true;
    }

    void run() override {
        running_ = true;
        while (running_) {
            update();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    ESteamNetworkingConnectionState get_connection_state() {
        return estate_;
    }

    bool connected() {
        return get_connection_state() == k_ESteamNetworkingConnectionState_Connected;
    }

    bool connecting() {
        return get_connection_state() == k_ESteamNetworkingConnectionState_Connecting || get_connection_state() == k_ESteamNetworkingConnectionState_FindingRoute;
    }

    EResult send(void* buffer, size_t size, int send_flags, int64* out_message_number = nullptr) {
        return interface_->SendMessageToConnection(connection_,
            buffer,
            size,
            send_flags,
            out_message_number);

    }

    template<typename T>
    EResult send(T msg, int send_flags, int64* out_message_number = nullptr) {
        static_assert(std::is_trivially_copyable<T>());
        return send(&msg,
            sizeof(msg),
            send_flags,
            out_message_number);
    }

    std::string get_detailed_status() {
        char info[2048];
        interface_->GetDetailedConnectionStatus(connection_, info, 2048);
        return { info };
    }

    SteamNetConnectionRealTimeStatus_t get_status() {
        SteamNetConnectionRealTimeStatus_t status{};
        if (interface_ != nullptr)
            interface_->GetConnectionRealTimeStatus(connection_, &status, 0, nullptr);
        return status;
    }

    int get_ping() {
        return get_status().m_nPing;
    }

    void close_connection() {
        close_connection(this->connection_);
    }

    void close_connection(HSteamNetConnection connection) {
        if (connection != k_HSteamNetConnection_Invalid) {
            interface_->CloseConnection(connection, 0, "Goodbye", true);
            assert(connection == this->connection_);
            connection_ = k_HSteamNetConnection_Invalid;
            estate_ = k_ESteamNetworkingConnectionState_None;
        }
    }

    void shutdown() {
        if (running_) {
            running_ = false;
            close_connection();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

protected:
    void poll_incoming_messages() override {
        while (running_) {
            ISteamNetworkingMessage* incoming_message[ONCE_RECV_MSG_COUNT] = { nullptr };
            int msg_count = interface_->ReceiveMessagesOnConnection(connection_, incoming_message, ONCE_RECV_MSG_COUNT);
            if (msg_count == 0)
                break;
            if (msg_count < 0)
                break;
                //FatalError("Error checking for messages.");
            assert(msg_count > 0 && incoming_message);

            for (int i = 0; i < msg_count; ++i) {
                on_message(incoming_message[i]);
                incoming_message[i]->Release();
            }
        }
    }

    void poll_connection_state_changes() override {
        this_instance_ = this;
        interface_->RunCallbacks();
    }

    void poll_local_state_changes() override {
        
    }

    HSteamNetConnection connection_ = k_HSteamNetConnection_Invalid;
    ESteamNetworkingConnectionState estate_;
    static constexpr inline size_t ONCE_RECV_MSG_COUNT = 50;
};