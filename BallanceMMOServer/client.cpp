#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif

#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <sstream>
#include "role.hpp"
#include "common.hpp"
#include "message/message_all.hpp"

struct client_data {
    std::string name;
};

class client: public role {
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

//        while (running_) {
//            poll_local_state_changes();
//        }
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

    std::string get_detailed_info() {
        char info[2048];
        interface_->GetDetailedConnectionStatus(connection_, info, 2048);
        return {info};
    }

    SteamNetConnectionRealTimeStatus_t get_info() {
        SteamNetConnectionRealTimeStatus_t status{};
        interface_->GetConnectionRealTimeStatus(connection_, &status, 0, nullptr);
        return status;
    }

    SteamNetConnectionRealTimeLaneStatus_t get_lane_info() {
        SteamNetConnectionRealTimeLaneStatus_t status{};
        interface_->GetConnectionRealTimeStatus(connection_, nullptr, 0, &status);
        return status;
    }

    void shutdown() {
        running_ = false;
        interface_->CloseConnection(connection_, 0, "Goodbye", true);
    }

private:
    void on_connection_status_changed(SteamNetConnectionStatusChangedCallback_t* pInfo) override {
        // What's the state of the connection?
        switch (pInfo->m_info.m_eState) {
            case k_ESteamNetworkingConnectionState_None:
                // NOTE: We will get callbacks here when we destroy connections.  You can ignore these.
                break;

            case k_ESteamNetworkingConnectionState_ClosedByPeer:
            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
                running_ = false;

                // Print an appropriate message
                if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting) {
                    // Note: we could distinguish between a timeout, a rejected connection,
                    // or some other transport problem.
                    Printf("We sought the remote host, yet our efforts were met with defeat.  (%s)\n",
                           pInfo->m_info.m_szEndDebug);
                } else if (pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
                    Printf("Alas, troubles beset us; we have lost contact with the host.  (%s)\n",
                           pInfo->m_info.m_szEndDebug);
                } else {
                    // NOTE: We could check the reason code for a normal disconnection
                    Printf("The host hath bidden us farewell.  (%s)", pInfo->m_info.m_szEndDebug);
                }

                // Clean up the connection.  This is important!
                // The connection is "closed" in the network sense, but
                // it has not been destroyed.  We must close it on our end, too
                // to finish up.  The reason information do not matter in this case,
                // and we cannot linger because it's already closed on the other end,
                // so we just pass 0's.
                interface_->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
                connection_ = k_HSteamNetConnection_Invalid;
                break;
            }

            case k_ESteamNetworkingConnectionState_Connecting:
                // We will get this callback when we start connecting.
                // We can ignore this.
                break;

            case k_ESteamNetworkingConnectionState_Connected: {
                Printf("Connected to server OK\n");
                bmmo::login_request_msg msg;
                msg.nickname = "Swung";
                msg.serialize();
                send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
                break;
            }
            default:
                // Silences -Wswitch
                break;
        }
    }

    void on_message(ISteamNetworkingMessage* networking_msg) override {
//        printf("\b");
//        fwrite(msg->m_pData, 1, msg->m_cbSize, stdout);
//        fputc('\n', stdout);

//        printf("\b> ");
//        Printf(reinterpret_cast<const char*>(msg->m_pData));
        auto* raw_msg = reinterpret_cast<bmmo::general_message*>(networking_msg->m_pData);
        switch (raw_msg->opcode) {
            case bmmo::OwnedBallState: {
                assert(networking_msg->m_cbSize == sizeof(bmmo::owned_ball_state_msg));
                auto* obs = reinterpret_cast<bmmo::owned_ball_state_msg*>(networking_msg->m_pData);
                Printf("%ld: (%.2lf, %.2lf, %.2lf), (%.2lf, %.2lf, %.2lf, %.2lf)",
                       obs->content.player_id,
                       obs->content.state.position.x,
                       obs->content.state.position.y,
                       obs->content.state.position.z,
                       obs->content.state.quaternion.x,
                       obs->content.state.quaternion.y,
                       obs->content.state.quaternion.z,
                       obs->content.state.quaternion.w);
                break;
            }
            default:
                break;
        }
    }

    void poll_incoming_messages() override {
        while (running_) {
            ISteamNetworkingMessage* incoming_message = nullptr;
            int msg_count = interface_->ReceiveMessagesOnConnection(connection_, &incoming_message, 1);
            if (msg_count == 0)
                break;
            if (msg_count < 0)
                FatalError("Error checking for messages.");
            assert(msg_count == 1 && incoming_message);

            on_message(incoming_message);
            incoming_message->Release();
        }
    }

    void poll_connection_state_changes() override {
        this_instance_ = this;
        interface_->RunCallbacks();
    }

    void poll_local_state_changes() override {
        std::string input;
        std::cin >> input;
        if (input == "stop") {
            shutdown();
        } else if (input == "1") {
            bmmo::ball_state_msg msg;
            msg.content.position.x = 1;
            msg.content.quaternion.y = 2;
            send(msg, k_nSteamNetworkingSend_UnreliableNoDelay);
        }
    }

    HSteamNetConnection connection_ = k_HSteamNetConnection_Invalid;
};

int main() {
    std::cout << "Initializing sockets..." << std::endl;
    client::init_socket();

    std::cout << "Creating client instance..." << std::endl;
    client client;

    std::cout << "Connecting to server..." << std::endl;
    if (!client.connect("127.0.0.1:26676")) {
        std::cerr << "Cannot connect to server." << std::endl;
        return 1;
    }

    std::thread client_thread([&client]() { client.run(); });
    do {
        std::string input;
        std::cin >> input;
        if (input == "stop") {
            client.shutdown();
        } else if (input == "1") {
            bmmo::ball_state_msg msg;
            msg.content.position.x = 1;
            msg.content.quaternion.y = 2;
//            for (int i = 0; i < 50; ++i)
            client.send(msg, k_nSteamNetworkingSend_UnreliableNoDelay);
        } else if (input == "2") {
            std::atomic_bool running = true;
            std::thread output_thread([&]() {
                while (running) {
                    bmmo::ball_state_msg msg;
                    msg.content.position.x = 1;
                    msg.content.quaternion.y = 2;
                    for (int i = 0; i < 50; ++i)
                        client.send(msg, k_nSteamNetworkingSend_UnreliableNoDelay);

                    std::cout << client.get_detailed_info() << std::endl;
                    std::this_thread::sleep_for(std::chrono::milliseconds (500));
                    std::cout << "\033[2J\033[H" << std::flush;
                }
            });
            while (running) {
                std::string in;
                std::cin >> in;
                if (in == "q") {
                    running = false;
                }
            }
            if (output_thread.joinable())
                output_thread.join();
        } else if (input == "3") {
            auto status = client.get_info();
            client::Printf("Ping: %dms\n", status.m_nPing);
            client::Printf("ConnectionQualityRemote: %.2f%\n", status.m_flConnectionQualityRemote * 100.0f);
            auto l_status = client.get_lane_info();
            client::Printf("PendingReliable: ", l_status.m_cbPendingReliable);
        } else if (input == "4") {
            if (client_thread.joinable())
                client_thread.join();

            if (!client.connect("127.0.0.1:26676")) {
                std::cerr << "Cannot connect to server." << std::endl;
                return 1;
            }

            client_thread = std::move(std::thread([&client]() { client.run(); }));
        }
    } while (client.running());

    std::cout << "Stopping..." << std::endl;
    if (client_thread.joinable())
        client_thread.join();
    client::destroy();
//    std::cout << "\033[2J\033[H" << std::flush;
}