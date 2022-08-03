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
#include <chrono>
#include <mutex>
#include <condition_variable>
#include "../BallanceMMOCommon/role/role.hpp"
#include "../BallanceMMOCommon/common.hpp"

#include "ya_getopt.h"

bool cheat = false;
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
        startup_cv_.notify_all();
        while (running_) {
            if (!update())
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

    void set_nickname(const std::string& name) {
        nickname_ = name;
    };

    void set_uuid(std::string& uuid) {
        while (uuid.find('-') != std::string::npos) {
            uuid.erase(uuid.find('-'), 1);
        }
        bmmo::hex_chars_from_string(uuid_, uuid);
    };

    void set_print_states(bool print_states) {
        print_states_ = print_states;
    }

    void shutdown() {
        running_ = false;
        interface_->CloseConnection(connection_, 0, "Goodbye", true);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    void wait_till_started() {
        while (!running()) {
            std::unique_lock<std::mutex> lk(startup_mutex_);
            startup_cv_.wait(lk);
        }
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
                //bmmo::login_request_msg msg;
                bmmo::login_request_v3_msg msg;
                msg.nickname = nickname_;
                msg.cheated = 0;
                memcpy(msg.uuid, uuid_, sizeof(uuid_));
                // msg.version = bmmo::version_t{1, 0, 0, bmmo::Alpha, 0};
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
        switch (raw_msg->code) {
            case bmmo::LoginAcceptedV2: {
                bmmo::login_accepted_v2_msg msg{};
                msg.raw.write(reinterpret_cast<char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();
                Printf("%d player(s) online:", msg.online_players.size());
                for (const auto& i: msg.online_players) {
                    Printf("%s (#%u)%s", i.second.name.c_str(), i.first, (i.second.cheated ? " [CHEAT]" : ""));
                }
                break;
            }
            case bmmo::PlayerConnectedV2: {
                bmmo::player_connected_v2_msg msg{};
                msg.raw.write(reinterpret_cast<char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();
                Printf("%s (#%u) logged in with cheat mode %s.", msg.name.c_str(), msg.connection_id, (msg.cheated ? "on" : "off"));
                break;
            }
            case bmmo::PlayerDisconnected: {
                auto* msg = reinterpret_cast<bmmo::player_disconnected_msg*>(networking_msg->m_pData);
                Printf("#%u disconnected.", msg->content.connection_id);
                break;
            }
            case bmmo::OwnedBallState: {
                if (!print_states_)
                    break;
                assert(networking_msg->m_cbSize == sizeof(bmmo::owned_ball_state_msg));
                auto* obs = reinterpret_cast<bmmo::owned_ball_state_msg*>(networking_msg->m_pData);
                Printf("%ld: %d, (%.2lf, %.2lf, %.2lf), (%.2lf, %.2lf, %.2lf, %.2lf)",
                       obs->content.player_id,
                       obs->content.state.type,
                       obs->content.state.position.x,
                       obs->content.state.position.y,
                       obs->content.state.position.z,
                       obs->content.state.rotation.x,
                       obs->content.state.rotation.y,
                       obs->content.state.rotation.z,
                       obs->content.state.rotation.w);
                break;
            }
            case bmmo::OwnedBallStateV2: {
                if (!print_states_)
                    break;
                bmmo::owned_ball_state_v2_msg msg;
                msg.raw.write(reinterpret_cast<char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                for (auto& ball : msg.balls) {
                    Printf("%ld: %d, (%.2lf, %.2lf, %.2lf), (%.2lf, %.2lf, %.2lf, %.2lf)",
                           ball.player_id,
                           ball.state.type,
                           ball.state.position.x,
                           ball.state.position.y,
                           ball.state.position.z,
                           ball.state.rotation.x,
                           ball.state.rotation.y,
                           ball.state.rotation.z,
                           ball.state.rotation.w);
                }

                break;
            }
            case bmmo::OwnedCheatState: {
                assert(networking_msg->m_cbSize == sizeof(bmmo::owned_cheat_state_msg));
                auto* ocs = reinterpret_cast<bmmo::owned_cheat_state_msg*>(networking_msg->m_pData);
                Printf("%ld turned cheat %s.", ocs->content.player_id, ocs->content.state.cheated ? "on" : "off");
                break;
            }
            case bmmo::Chat: {
                bmmo::chat_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                if (msg.player_id == k_HSteamNetConnection_Invalid)
                    Printf("[Server]: %s", msg.chat_content.c_str());
                else
                    Printf("%u: %s", msg.player_id, msg.chat_content.c_str());
                break;
            }
            case bmmo::CheatToggle: {
                auto* msg = reinterpret_cast<bmmo::cheat_toggle_msg*>(networking_msg->m_pData);
                cheat = msg->content.cheated;
                Printf("Server toggled cheat %s globally!", cheat ? "on" : "off");
                bmmo::cheat_state_msg state_msg{};
                state_msg.content.cheated = cheat;
                state_msg.content.notify = false;
                send(state_msg, k_nSteamNetworkingSend_Reliable);
                break;
            }
            case bmmo::OwnedCheatToggle: {
                auto* msg = reinterpret_cast<bmmo::owned_cheat_toggle_msg*>(networking_msg->m_pData);
                cheat = msg->content.state.cheated;
                Printf("#%u toggled cheat %s globally!", msg->content.player_id, cheat ? "on" : "off");
                bmmo::cheat_state_msg state_msg{};
                state_msg.content.cheated = cheat;
                state_msg.content.notify = false;
                send(state_msg, k_nSteamNetworkingSend_Reliable);
                break;
            }
            case bmmo::PlayerKicked: {
                bmmo::player_kicked_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                Printf("%s was kicked by %s%s%s.",
                    msg.kicked_player_name.c_str(),
                    (msg.executor_name == "")? "the server" : msg.executor_name.c_str(),
                    (msg.reason == "")? "" : (" (" + msg.reason + ")").c_str(),
                    msg.crashed ? " and crashed subsequently" : ""
                );
                break;
            }
            case bmmo::SimpleAction: {
                auto* msg = reinterpret_cast<bmmo::simple_action_msg*>(networking_msg->m_pData);
                switch (msg->content.action) {
                    case bmmo::LoginDenied: {
                        Printf("Login denied.");
                        break;
                    }
                    case bmmo::CurrentMapQuery: {
                        bmmo::current_map_msg new_msg{};
                        send(new_msg, k_nSteamNetworkingSend_Reliable);
                        break;
                    }
                    case bmmo::UnknownAction: {
                        Printf("Unknown action request received.");
                        break;
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    int poll_incoming_messages() override {
        int msg_count = interface_->ReceiveMessagesOnConnection(connection_, incoming_messages_, ONCE_RECV_MSG_COUNT);
        if (msg_count == 0)
            return 0;
        if (msg_count < 0)
            FatalError("Error checking for messages.");
        assert(msg_count > 0);

        for (int i = 0; i < msg_count; ++i) {
            on_message(incoming_messages_[i]);
            incoming_messages_[i]->Release();
        }
        
        return msg_count;
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
            msg.content.rotation.y = 2;
            send(msg, k_nSteamNetworkingSend_UnreliableNoDelay);
        }
    }

    std::mutex startup_mutex_;
    std::condition_variable startup_cv_;
    HSteamNetConnection connection_ = k_HSteamNetConnection_Invalid;
    std::string nickname_;
    uint8_t uuid_[16];
    bool print_states_ = false;
};

// parse command line arguments (server/name/uuid/help/version) with getopt
int parse_args(int argc, char** argv, std::string& server, std::string& name, std::string& uuid, std::string& log_path, bool* print_states) {
    static struct option long_options[] = {
        {"server", required_argument, 0, 's'},
        {"name", required_argument, 0, 'n'},
        {"uuid", required_argument, 0, 'u'},
        {"log", required_argument, 0, 'l'},
        {"print", no_argument, 0, 'p'},
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'v'},
        {0, 0, 0, 0}
    };
    int opt, opt_index = 0;
    while ((opt = getopt_long(argc, argv, "s:n:u:l:phv", long_options, &opt_index))!= -1) {
        switch (opt) {
            case 's':
                server = optarg;
                break;
            case 'n':
                name = optarg;
                break;
            case 'u':
                uuid = optarg;
                break;
            case 'l':
                log_path = optarg;
                break;
            case 'p':
                *print_states = true;
                break;
            case 'h':
                printf("Usage: %s [OPTION]...\n", argv[0]);
                puts("Options:");
                puts("  -s, --server=ADDRESS\t Connect to the server at ADDRESS instead (default: 127.0.0.1:26676).");
                puts("  -n, --name=NAME\t Set your name to NAME (default: \"Swung\")");
                puts("  -u, --uuid=UUID\t Set your UUID to UUID (default: \"00010002-0003-0004-0005-000600070008\")");
                puts("  -l, --log=PATH\t Write log to the file at PATH in addition to stdout.");
                puts("  -p, --print\t\t Print player state changes.");
                puts("  -h, --help\t\t Display this help and exit.");
                puts("  -v, --version\t\t Display version information and exit.");
                return -1;
            case 'v':
                puts("Ballance MMO mock client by Swung0x48 and BallanceBug.");
                printf("Version: %s.\n", bmmo::version_t().to_string().c_str());
                return -1;
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    std::string server_addr = "127.0.0.1:26676", username = "Swung",
                uuid = "00010002-0003-0004-0005-000600070008",
                log_path;
    bool print_states = false;
    if (parse_args(argc, argv, server_addr, username, uuid, log_path, &print_states) != 0)
        return 0;
    
    bmmo::hostname_parser hp(server_addr);
    server_addr = hp.get_address() + ":" + hp.get_port();

    FILE* log_file = nullptr;
    if (!log_path.empty()) {
        log_file = fopen(log_path.c_str(), "a");
        if (log_file == nullptr) {
            std::cerr << "Fatal: failed to open log file." << std::endl;
            return 1;
        }
        client::set_log_file(log_file);
    }

    std::cout << "Initializing sockets..." << std::endl;
    client::init_socket();

    std::cout << "Creating client instance..." << std::endl;
    client client;
    client.set_nickname(username);
    client.set_uuid(uuid);
    client.set_print_states(print_states);

    std::cout << "Connecting to server..." << std::endl;
    if (!client.connect(server_addr)) {
        std::cerr << "Cannot connect to server." << std::endl;
        return 1;
    }

    std::thread client_thread([&client]() { client.run(); });

    client.wait_till_started();
    while (client.running()) {
        std::cout << "\r> " << std::flush;
        std::string input, cmd;
#ifdef _WIN32
        std::wstring wline;
        std::getline(std::wcin, wline);
        input = bmmo::message_utils::ConvertWideToANSI(wline);
        if (auto pos = input.rfind('\r'); pos != std::string::npos)
            input.erase(pos);
#else
        std::getline(std::cin, input);
#endif
        bmmo::command_parser parser(input);
        cmd = parser.get_next_word();
        // std::cin >> input;
        if (cmd == "stop") {
            client.shutdown();
        } else if (cmd == "move") {
            bmmo::ball_state_msg msg;
            if (std::string temp = parser.get_next_word(); !temp.empty()) {
                msg.content.position.x = atof(temp.c_str());
                msg.content.position.y = atof(parser.get_next_word().c_str());
                msg.content.position.z = atof(parser.get_next_word().c_str());
            } else {
                msg.content.position.x = (rand() % 2000 - 1000) / 100.0f;
                msg.content.position.y = (rand() % 2000 - 1000) / 100.0f;
                msg.content.position.z = (rand() % 2000 - 1000) / 100.0f;
            }
            msg.content.rotation.x = (rand() % 3600 - 1800) / 10.0f;
            msg.content.rotation.y = (rand() % 3600 - 1800) / 10.0f;
            msg.content.rotation.z = (rand() % 3600 - 1800) / 10.0f;
            msg.content.rotation.w = (rand() % 3600 - 1800) / 10.0f;
            client::Printf("Sending ball state message: (%.2f, %.2f, %.2f), (%.1f, %.1f, %.1f, %.1f)",
                msg.content.position.x, msg.content.position.y, msg.content.position.z,
                msg.content.rotation.x, msg.content.rotation.y, msg.content.rotation.z, msg.content.rotation.w);
//            for (int i = 0; i < 50; ++i)
            client.send(msg, k_nSteamNetworkingSend_UnreliableNoDelay);
        } else if (cmd == "getinfo-detailed") {
            std::atomic_bool running = true;
            std::thread output_thread([&]() {
                while (running) {
                    // bmmo::ball_state_msg msg;
                    // msg.content.position.x = 1;
                    // msg.content.rotation.y = 2;
                    // for (int i = 0; i < 50; ++i)
                    //     client.send(msg, k_nSteamNetworkingSend_UnreliableNoDelay);

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
        } else if (cmd == "getinfo") {
            auto status = client.get_info();
            client::Printf("Ping: %dms\n", status.m_nPing);
            client::Printf("ConnectionQualityRemote: %.2f%\n", status.m_flConnectionQualityRemote * 100.0f);
            auto l_status = client.get_lane_info();
            client::Printf("PendingReliable: ", l_status.m_cbPendingReliable);
        } else if (cmd == "reconnect") {
            if (client_thread.joinable())
                client_thread.join();

            if (!client.connect(server_addr)) {
                std::cerr << "Cannot connect to server." << std::endl;
                return 1;
            }

            client_thread = std::move(std::thread([&client]() { client.run(); }));
            client.wait_till_started();
        } else if (cmd == "cheat") {
            cheat = !cheat;
            bmmo::cheat_state_msg msg;
            msg.content.cheated = cheat;
            client.send(msg, k_nSteamNetworkingSend_Reliable);
        } else if (!cmd.empty()) {
            bmmo::chat_msg msg{};
            msg.chat_content = cmd;
            msg.serialize();
            client.send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        }
    }

    std::cout << "Stopping..." << std::endl;
    if (client_thread.joinable())
        client_thread.join();
    client::destroy();
}
