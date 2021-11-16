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
#include "role.hpp"
#include "common.hpp"

struct client_data {
    std::string name;
};

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
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

//        while (running_) {
//            poll_local_state_changes();
//        }
    }

    template<typename T>
    EResult send(T msg, int send_flags, int64* out_message_number = nullptr) {
        static_assert(std::is_trivially_copyable<T>());
        return interface_->SendMessageToConnection(connection_,
                                            &msg,
                                            sizeof(msg),
                                            send_flags,
                                            out_message_number);

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
                    printf("We sought the remote host, yet our efforts were met with defeat.  (%s)\n",
                           pInfo->m_info.m_szEndDebug);
                } else if (pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
                    printf("Alas, troubles beset us; we have lost contact with the host.  (%s)\n",
                           pInfo->m_info.m_szEndDebug);
                } else {
                    // NOTE: We could check the reason code for a normal disconnection
                    printf("The host hath bidden us farewell.  (%s)", pInfo->m_info.m_szEndDebug);
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

            case k_ESteamNetworkingConnectionState_Connected:
                printf("Connected to server OK\n");
                break;

            default:
                // Silences -Wswitch
                break;
        }
    }

    void on_message(ISteamNetworkingMessage* msg) override {
        fwrite(msg->m_pData, 1, msg->m_cbSize, stdout);
        fputc('\n', stdout);
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
            msg.state.position.x = 1;
            msg.state.quaternion.y = 2;
            send(msg, k_nSteamNetworkingSend_UnreliableNoDelay);
        } else {
            interface_->SendMessageToConnection(connection_,
                                                input.c_str(),
                                                input.length() + 1,
                                                k_nSteamNetworkingSend_Reliable,
                                                nullptr);
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
            msg.state.position.x = 1;
            msg.state.quaternion.y = 2;
            client.send(msg, k_nSteamNetworkingSend_UnreliableNoDelay);
        }
    } while (client.running());

    std::cout << "Stopping..." << std::endl;
    if (client_thread.joinable())
        client_thread.join();
    client::destroy();
}