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
#include "common.hpp"

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
            auto next_update = std::chrono::steady_clock::now() + bmmo::CLIENT_RECEIVE_INTERVAL;
            if (!update())
                std::this_thread::sleep_until(next_update);
        }
    }

    ESteamNetworkingConnectionState get_connection_state() {
        return estate_;
    }

    virtual bool connected() {
        return get_connection_state() == k_ESteamNetworkingConnectionState_Connected;
    }

    virtual bool connecting() {
        return 
            get_connection_state() == k_ESteamNetworkingConnectionState_Connecting || 
            get_connection_state() == k_ESteamNetworkingConnectionState_FindingRoute;
    }

    EResult send(void* buffer, size_t size, int send_flags = k_nSteamNetworkingSend_Reliable, int64* out_message_number = nullptr) {
        return interface_->SendMessageToConnection(connection_,
            buffer,
            size,
            send_flags,
            out_message_number);

    }

    template<bmmo::trivially_copyable_msg T>
    EResult send(T msg, int send_flags = k_nSteamNetworkingSend_Reliable, int64* out_message_number = nullptr) {
        static_assert(std::is_trivially_copyable<T>());
        return send(&msg,
            sizeof(msg),
            send_flags,
            out_message_number);
    }

    virtual void receive(void* data, size_t size) {}

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

    void close_connection(HSteamNetConnection connection, bool linger = true) {
        if (connection != k_HSteamNetConnection_Invalid) {
            interface_->CloseConnection(connection, 0, "Goodbye", linger);
            assert(connection == this->connection_);
            connection_ = k_HSteamNetConnection_Invalid;
            estate_ = k_ESteamNetworkingConnectionState_None;
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
        ISteamNetworkingMessage* incoming_message[ONCE_RECV_MSG_COUNT] = { nullptr };
        int msg_count = interface_->ReceiveMessagesOnConnection(connection_, incoming_message, ONCE_RECV_MSG_COUNT);
        if (msg_count == 0)
            return 0;
        else if (msg_count < 0)
            //break;
            bmmo::FatalError("Error checking for messages.");
        assert(msg_count > 0);

        for (int i = 0; i < msg_count; ++i) {
            on_message(incoming_message[i]);
            incoming_message[i]->Release();
        }
        return msg_count;
    }

    void poll_connection_state_changes() override {
        this_instance_ = this;
        interface_->RunCallbacks();
    }

    void poll_local_state_changes() override {
        
    }

    HSteamNetConnection connection_ = k_HSteamNetConnection_Invalid;
    ESteamNetworkingConnectionState estate_{};
};
