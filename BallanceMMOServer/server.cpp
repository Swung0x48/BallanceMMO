#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif

#include "../BallanceMMOCommon/role/role.hpp"
#include <vector>
#include "../BallanceMMOCommon/common.hpp"

#include "ya_getopt.h"

struct client_data {
    std::string name;
    bool cheated = false;
};

class server : public role {
public:
    explicit server(uint16_t port) {
        port_ = port;
    }

    void run() override {
        if (!setup())
            FatalError("Server failed on setup.");

        running_ = true;
        while (running_) {
            update();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

//        while (running_) {
//            poll_local_state_changes();
//        }
    }

    void poll_local_state_changes() override {
        std::string cmd;
        std::cin >> cmd;
        if (cmd == "stop") {
            shutdown();
        }
    }

    EResult send(HSteamNetConnection destination, void* buffer, size_t size, int send_flags, int64* out_message_number = nullptr) {
        return interface_->SendMessageToConnection(destination,
                                                   buffer,
                                                   size,
                                                   send_flags,
                                                   out_message_number);

    }

    template<typename T>
    EResult send(HSteamNetConnection destination, T msg, int send_flags, int64* out_message_number = nullptr) {
        static_assert(std::is_trivially_copyable<T>());
        return send(destination,
                    &msg,
                    sizeof(msg),
                    send_flags,
                    out_message_number);
    }

    void broadcast_message(void* buffer, size_t size, int send_flags, const HSteamNetConnection* ignored_client = nullptr) {
        for (auto& i: clients_)
            if (ignored_client == nullptr || *ignored_client != i.first)
                send(i.first, buffer, size,
                                                    send_flags,
                                                    nullptr);
    }

    template<typename T>
    void broadcast_message(T msg, int send_flags, HSteamNetConnection* ignored_client = nullptr) {
        static_assert(std::is_trivially_copyable<T>());

        broadcast_message(&msg, sizeof(msg), send_flags, ignored_client);
    }

    HSteamNetConnection get_client_id(std::string username) {
        HSteamNetConnection client = k_HSteamNetConnection_Invalid;
        auto username_it = username_.find(username);
        if (username_it == username_.end()) {
            Printf("Error: client \"%s\" not found.", username.c_str());
            return k_HSteamNetConnection_Invalid;
        }
        client = username_it->second;
        return client;
    };

    bool kick_client(HSteamNetConnection client, std::string reason = "", HSteamNetConnection executor = k_HSteamNetConnection_Invalid, bool crash = false) {
        if (!client_exists(client))
            return false;
        bmmo::player_kicked_msg msg{};
        msg.kicked_player_name = clients_[client].name;

        std::string kick_notice = "Kicked by ";
        if (executor != k_HSteamNetConnection_Invalid) {
            if (!client_exists(executor))
                return false;
            kick_notice += clients_[executor].name;
            msg.executor_name = clients_[executor].name;
        } else {
            kick_notice += "the server";
        }
        if (reason != "") {
            kick_notice.append(" (" + reason + ")");
            msg.reason = reason;
        }
        kick_notice.append(".");

        msg.crashed = crash;

        interface_->CloseConnection(client, k_ESteamNetConnectionEnd_App_Min + (crash ? 102 : 101), kick_notice.c_str(), true);
        msg.serialize();
        broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);

        return true;
    }

    void print_clients() {
        Printf("%d clients online:", clients_.size());
        for (auto& i: clients_) {
            Printf("%u: %s%s", i.first, i.second.name.c_str(), (i.second.cheated ? " [CHEAT]" : ""));
        }
    }

    void print_version_info() {
        Printf("Server version: %s; minimum accepted client version: %s.",
                        bmmo::version_t().to_string().c_str(),
                        bmmo::minimum_client_version.to_string().c_str());
        auto uptime = SteamNetworkingUtils()->GetLocalTimestamp() - init_timestamp_;
        std::string time_str(20, 0);
        time_str.resize(std::strftime(&time_str[0], time_str.size(), 
            "%F %X", std::localtime(&init_time_t_)));
        Printf("Server uptime: %.2f seconds since %s.",
                        uptime * 1e-6, time_str.c_str());
    }

    void toggle_cheat(bool cheat) {
        bmmo::cheat_toggle_msg msg;
        msg.content.cheated = (uint8_t)cheat;
        broadcast_message(msg, k_nSteamNetworkingSend_Reliable);
    }

    void shutdown() {
        for (auto& i: clients_) {
            interface_->CloseConnection(i.first, 0, "Server closed", true);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        running_ = false;
//        if (server_thread_.joinable())
//            server_thread_.join();
    }

protected:
    bool setup() {
        SteamNetworkingIPAddr local_address{};
        local_address.Clear();
        local_address.m_port = port_;
        SteamNetworkingConfigValue_t opt = generate_opt();
        listen_socket_ = interface_->CreateListenSocketIP(local_address, 1, &opt);
        if (listen_socket_ == k_HSteamListenSocket_Invalid) {
            return false;
        }

        poll_group_ = interface_->CreatePollGroup();
        if (poll_group_ == k_HSteamNetPollGroup_Invalid) {
            return false;
        }

        return true;
    }

    void cleanup_disconnected_client(HSteamNetConnection* client) {
        // Locate the client.  Note that it should have been found, because this
        // is the only codepath where we remove clients (except on shutdown),
        // and connection change callbacks are dispatched in queue order.
        auto itClient = clients_.find(*client);
        //assert(itClient != clients_.end()); // It might in limbo state...So may not yet to be found

        bmmo::player_disconnected_msg msg;
        msg.content.connection_id = *client;
        broadcast_message(msg, k_nSteamNetworkingSend_Reliable, client);
        if (itClient == clients_.end())
            return;
        std::string name = itClient->second.name;
        if (username_.find(name) != username_.end())
            username_.erase(name);
        if (itClient != clients_.end())
            clients_.erase(itClient);
    }

    bool client_exists(HSteamNetConnection client) {
        if (client == k_HSteamNetConnection_Invalid)
            return false;
        auto it = clients_.find(client);
        if (it == clients_.end()) {
            Printf("Error: client %u not found.", client);
            return false;
        }
        return true;
    }

    bool validate_client(HSteamNetConnection client, bmmo::login_request_v2_msg& msg) {
        int nReason = k_ESteamNetConnectionEnd_Invalid;
        std::stringstream reason;

        // verify client version
        if (msg.version < bmmo::minimum_client_version) {
            reason << "Outdated client (client: " << msg.version.to_string()
                    << "; minimum: " << bmmo::minimum_client_version.to_string() << ")";
            nReason = k_ESteamNetConnectionEnd_App_Min + 1;
        }
        // check if name exists
        else if (username_.find(msg.nickname) != username_.end()) {
            reason << "A player with a same username \"" << msg.nickname << "\" already exists on this serer.";
            nReason = k_ESteamNetConnectionEnd_App_Min + 2;
        }
        // validate nickname length
        else if (!bmmo::name_validator::is_of_valid_length(msg.nickname)) {
            reason << "Nickname must be between "
                    << bmmo::name_validator::min_length << " and "
                    << bmmo::name_validator::max_length << " characters in length.";
            nReason = k_ESteamNetConnectionEnd_App_Min + 3;
        }
        // validate nickname characters
        else if (size_t invalid_pos = bmmo::name_validator::get_invalid_char_pos(msg.nickname);
                invalid_pos != std::string::npos) {
            reason << "Invalid character '" << msg.nickname[invalid_pos] << "'; Nickname must contain only alphanumeric characters and underscores.";
            nReason = k_ESteamNetConnectionEnd_App_Min + 4;
        }

        if (nReason != k_ESteamNetConnectionEnd_Invalid) {
            bmmo::login_denied_msg new_msg;
            send(client, new_msg, k_nSteamNetworkingSend_Reliable);
            interface_->CloseConnection(client, nReason, reason.str().c_str(), true);
            return false;
        }

        return true;
    }

    void on_connection_status_changed(SteamNetConnectionStatusChangedCallback_t* pInfo) override {
        // Printf("Connection status changed: %d", pInfo->m_info.m_eState);
        switch (pInfo->m_info.m_eState) {
            case k_ESteamNetworkingConnectionState_None:
                // NOTE: We will get callbacks here when we destroy connections.  You can ignore these.
            case k_ESteamNetworkingConnectionState_ClosedByPeer:
            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
                // Ignore if they were not previously connected.  (If they disconnected
                // before we accepted the connection.)
                if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connected) {
                    // // Select appropriate log messages
                    // const char* pszDebugLogAction;
                    // if (pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
                    //     pszDebugLogAction = "problem detected locally";
                    // } else {
                    //     // Note that here we could check the reason code to see if
                    //     // it was a "usual" connection or an "unusual" one.
                    //     pszDebugLogAction = "closed by peer";
                    // }

                    // // Spew something to our own log.  Note that because we put their nick
                    // // as the connection description, it will show up, along with their
                    // // transport-specific data (e.g. their IP address)
                    // Printf( "Connection %s %s, reason %d: %s\n",
                    //         pInfo->m_info.m_szConnectionDescription,
                    //         pszDebugLogAction,
                    //         pInfo->m_info.m_eEndReason,
                    //         pInfo->m_info.m_szEndDebug
                    // );
                    
                    cleanup_disconnected_client(&pInfo->m_hConn);
                } else {
                    assert(pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting
                           || pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_None);
                }

                // Clean up the connection.  This is important!
                // The connection is "closed" in the network sense, but
                // it has not been destroyed.  We must close it on our end, too
                // to finish up.  The reason information do not matter in this case,
                // and we cannot linger because it's already closed on the other end,
                // so we just pass 0's.
                
                interface_->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
                break;
            }

            case k_ESteamNetworkingConnectionState_Connecting: {
                // This must be a new connection
                assert(clients_.find(pInfo->m_hConn) == clients_.end());

                Printf("Connection request from %s\n", pInfo->m_info.m_szConnectionDescription);

                // A client is attempting to connect
                // Try to accept the connection.
                if (interface_->AcceptConnection(pInfo->m_hConn) != k_EResultOK) {
                    // This could fail.  If the remote host tried to connect, but then
                    // disconnected, the connection may already be half closed.  Just
                    // destroy whatever we have on our side.
                    interface_->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
                    Printf("Can't accept connection.  (It was already closed?)\n");
                    break;
                }

                // Assign the poll group
                if (!interface_->SetConnectionPollGroup(pInfo->m_hConn, poll_group_)) {
                    interface_->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
                    Printf("Failed to set poll group?");
                    break;
                }

                // Generate a random nick.  A random temporary nick
                // is really dumb and not how you would write a real chat server.
                // You would want them to have some sort of signon message,
                // and you would keep their client in a state of limbo (connected,
                // but not logged on) until them.  I'm trying to keep this example
                // code really simple.
                char nick[64];
                sprintf(nick, "Unidentified%d", 10000 + (rand() % 90000));

                // DO NOT add client here.
                //clients_[pInfo->m_hConn] = {nick};
                interface_->SetConnectionName(pInfo->m_hConn, nick);
//                SetClientNick( pInfo->m_hConn, nick );
                break;
            }

            case k_ESteamNetworkingConnectionState_Connected:
                // We will get a callback immediately after accepting the connection.
                // Since we are the server, we can ignore this, it's not news to us.
                break;

            default:
                // Silences -Wswitch
                break;
        }
    }

    void on_message(ISteamNetworkingMessage* networking_msg) override {
        auto client_it = clients_.find(networking_msg->m_conn);

        auto* raw_msg = reinterpret_cast<bmmo::general_message*>(networking_msg->m_pData);
        if (!(raw_msg->code == bmmo::LoginRequest || raw_msg->code == bmmo::LoginRequestV2 || client_it != clients_.end())) { // ignore limbo clients message
            interface_->CloseConnection(networking_msg->m_conn, k_ESteamNetConnectionEnd_AppException_Min, "Invalid client", true);
            return;
        }

        switch (raw_msg->code) {
            case bmmo::LoginRequest: {
                bmmo::login_denied_msg msg;
                send(networking_msg->m_conn, msg, k_nSteamNetworkingSend_Reliable);
                interface_->CloseConnection(networking_msg->m_conn, k_ESteamNetConnectionEnd_App_Min + 1, "Outdated client", true);
                break;
            }
            case bmmo::LoginRequestV2: {
                bmmo::login_request_v2_msg msg;
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                interface_->SetConnectionName(networking_msg->m_conn, msg.nickname.c_str());

                if (!validate_client(networking_msg->m_conn, msg))
                    break;

                // accepting client
                Printf("%s (v%s) logged in with cheat mode %s!\n", msg.nickname.c_str(), msg.version.to_string().c_str(), msg.cheated ? "on" : "off");
                clients_[networking_msg->m_conn] = {msg.nickname, (bool)msg.cheated};  // add the client here
                username_[msg.nickname] = networking_msg->m_conn;

                // notify this client of other online players
                bmmo::login_accepted_v2_msg accepted_msg;
                for (auto it = clients_.begin(); it != clients_.end(); ++it) {
                    //if (client_it != it)
                    accepted_msg.online_players[it->first] = { it->second.name, it->second.cheated };
                }
                accepted_msg.serialize();
                send(networking_msg->m_conn, accepted_msg.raw.str().data(), accepted_msg.raw.str().size(), k_nSteamNetworkingSend_Reliable);
                
                // notify other client of the fact that this client goes online
                bmmo::player_connected_v2_msg connected_msg;
                connected_msg.connection_id = networking_msg->m_conn;
                connected_msg.name = msg.nickname;
                connected_msg.cheated = msg.cheated;
                connected_msg.serialize();
                broadcast_message(connected_msg.raw.str().data(), connected_msg.size(), k_nSteamNetworkingSend_Reliable, &networking_msg->m_conn);
                break;
            }
            case bmmo::LoginAccepted:
                break;
            case bmmo::LoginDenied:
                break;
            case bmmo::PlayerDisconnected:
                break;
            case bmmo::PlayerConnected:
                break;
            case bmmo::Ping:
                break;
            case bmmo::BallState: {
                auto* state_msg = reinterpret_cast<bmmo::ball_state_msg*>(networking_msg->m_pData);

                // Printf("%u: %d, (%f, %f, %f), (%f, %f, %f, %f)",
                //        networking_msg->m_conn,
                //        state_msg->content.type,
                //        state_msg->content.position.x,
                //        state_msg->content.position.y,
                //        state_msg->content.position.z,
                //        state_msg->content.rotation.x,
                //        state_msg->content.rotation.y,
                //        state_msg->content.rotation.z,
                //        state_msg->content.rotation.w);
//                std::cout << "(" <<
//                          state_msg->state.position.x << ", " <<
//                          state_msg->state.position.y << ", " <<
//                          state_msg->state.position.z << "), (" <<
//                          state_msg->state.quaternion.x << ", " <<
//                          state_msg->state.quaternion.y << ", " <<
//                          state_msg->state.quaternion.z << ", " <<
//                          state_msg->state.quaternion.w << ")" << std::endl;

                bmmo::owned_ball_state_msg new_msg;
                new_msg.content.state = state_msg->content;
//                std::memcpy(&(new_msg.content), &(state_msg->content), sizeof(state_msg->content));
                new_msg.content.player_id = networking_msg->m_conn;
                broadcast_message(&new_msg, sizeof(new_msg), k_nSteamNetworkingSend_UnreliableNoDelay, &networking_msg->m_conn);

                break;
            }
            case bmmo::Chat: {
                bmmo::chat_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                // Print chat message to console
                const std::string& current_player_name = client_it->second.name;
                const HSteamNetConnection current_player_id  = networking_msg->m_conn;
                Printf("(%u, %s): %s", current_player_id, current_player_name.c_str(), msg.chat_content.c_str());

                // Broatcast chat message to other player
                msg.player_id = current_player_id;
                msg.clear();
                msg.serialize();

                // No need to ignore the sender, 'cause we will send the message back
                broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);

                break;
            }
            case bmmo::LevelFinish: {
                auto* msg = reinterpret_cast<bmmo::level_finish_msg*>(networking_msg->m_pData);
                msg->content.player_id = networking_msg->m_conn;
                
                // Cheat check
                if (msg->content.currentLevel * 100 != msg->content.levelBouns || msg->content.levelBouns != 200) {
                    msg->content.cheated = true;
                }

                broadcast_message(*msg, k_nSteamNetworkingSend_Reliable);

                break;
            }
            case bmmo::CheatState: {
                auto* state_msg = reinterpret_cast<bmmo::cheat_state_msg*>(networking_msg->m_pData);
                client_it->second.cheated = state_msg->content.cheated;
                Printf("%s turned [%s] cheat!", client_it->second.name.c_str(), state_msg->content.cheated ? "on" : "off");
                bmmo::owned_cheat_state_msg new_msg{};
                new_msg.content.player_id = networking_msg->m_conn;
                new_msg.content.state.cheated = state_msg->content.cheated;
                new_msg.content.state.notify = state_msg->content.notify;
                broadcast_message(&new_msg, sizeof(new_msg), k_nSteamNetworkingSend_Reliable);

                break;
            }
            case bmmo::CheatToggle: {
                auto* state_msg = reinterpret_cast<bmmo::cheat_toggle_msg*>(networking_msg->m_pData);
                Printf("%s toggled cheat [%s] globally!", client_it->second.name.c_str(), state_msg->content.cheated ? "on" : "off");
                bmmo::owned_cheat_toggle_msg new_msg{};
                new_msg.content.player_id = client_it->first;
                new_msg.content.state.cheated = state_msg->content.cheated;
                broadcast_message(&new_msg, sizeof(new_msg), k_nSteamNetworkingSend_Reliable);

                break;
            }
            case bmmo::KickRequest: {
                bmmo::kick_request_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                HSteamNetConnection player_id = msg.player_id;
                if (msg.player_name != "") {
                    Printf("%s requested to kick player \"%s\"!", client_it->second.name.c_str(), msg.player_name.c_str());
                    player_id = get_client_id(msg.player_name);
                } else {
                    Printf("%s requested to kick player %u!", client_it->second.name.c_str(), msg.player_id);
                }
                kick_client(player_id, msg.reason, client_it->first);

                break;
            }
            case bmmo::KeyboardInput:
                break;
            default:
                FatalError("Invalid message with opcode %d received.", raw_msg->code);
        }

        // TODO: replace with actual message data structure handling
//        std::string str;
//        str.assign((const char*)networking_msg->m_pData, networking_msg->m_cbSize);
//
//        Printf("%s: %s", client_it->second.name.c_str(), str.c_str());
//        interface_->SendMessageToConnection(client_it->first, str.c_str(), str.length() + 1,
//                                            k_nSteamNetworkingSend_Reliable,
//                                            nullptr);
    }

    int poll_incoming_messages() override {
        int msg_count = interface_->ReceiveMessagesOnPollGroup(poll_group_, incoming_messages_, ONCE_RECV_MSG_COUNT);
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

    uint16_t port_ = 0;
    HSteamListenSocket listen_socket_ = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup poll_group_ = k_HSteamNetPollGroup_Invalid;
//    std::thread server_thread_;
    std::unordered_map<HSteamNetConnection, client_data> clients_;
    std::unordered_map<std::string, HSteamNetConnection> username_;
};

// parse arguments (optional port and help/version) with getopt
int parse_args(int argc, char** argv, uint16_t* port) {
    static struct option long_options[] = {
        {"port", required_argument, 0, 'p'},
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'v'},
        {0, 0, 0, 0}
    };
    int opt, opt_index = 0;
    while ((opt = getopt_long(argc, argv, "p:hv", long_options, &opt_index)) != -1) {
        switch (opt) {
            case 'p':
                *port = atoi(optarg);
                break;
            case 'h':
                printf("Usage: %s [OPTION]...\n", argv[0]);
                printf("Options:\n");
                printf("  -p, --port=PORT\t Use PORT as the server port instead (default: 26676).\n");
                printf("  -h, --help\t\t Display this help and exit.\n");
                printf("  -v, --version\t\t Display version information and exit.\n");
                return -1;
            case 'v':
                printf("Ballance MMO server by Swung0x48 and BallanceBug.\n");
                printf("Version: %s.\n", bmmo::version_t().to_string().c_str());
                printf("Minimum accepted client version: %s.\n", bmmo::minimum_client_version.to_string().c_str());
                printf("GitHub repository: https://github.com/Swung0x48/BallanceMMO\n");
                return -1;
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    uint16_t port = 26676;
    if (parse_args(argc, argv, &port) < 0)
        return 0;

    if (port == 0) {
        std::cerr << "Fatal: invalid port number." << std::endl;
        return 1;
    };

    printf("Initializing sockets...\n");
    server::init_socket();

    printf("Starting server at port %d.\n", port);
    server server(port);

    printf("Bootstrapping server...\n");
    std::thread server_thread([&server]() { server.run(); });

    printf("Server (v%s; client min. v%s) started.\n",
            bmmo::version_t().to_string().c_str(),
            bmmo::minimum_client_version.to_string().c_str());
    std::cout << std::flush;

    do {
        std::cout << "\r> " << std::flush;
        std::string cmd;
        std::cin >> cmd;
        if (cmd == "stop") {
            server.shutdown();
        } else if (cmd == "list") {
            server.print_clients();
        } else if (cmd == "say") {
            bmmo::chat_msg msg{};
            std::getline(std::cin, msg.chat_content);
            msg.chat_content.erase(0, 1);
            msg.serialize();

            server.broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        } else if (cmd == "cheat") {
            bool cheat_state = false;
            std::string cheat_state_string;
            std::cin >> cheat_state_string;
            if (cheat_state_string == "on")
                cheat_state = true;
            server.toggle_cheat(cheat_state);
        } else if (cmd == "ver" || cmd == "version") {
            server.print_version_info();
        } else if (cmd == "kick" || cmd == "kick-id" || cmd == "crash" || cmd == "crash-id") {
            std::string username = "", reason;
            HSteamNetConnection client = k_HSteamNetConnection_Invalid;
            if (cmd == "kick-id" || cmd == "crash-id") {
                std::string id_string;
                std::cin >> id_string;
                client = atoll(id_string.c_str());
                if (client == 0) {
                    server.Printf("Error: invalid connection id.");
                    continue;
                }
            } else {
                std::cin >> username;
                client = server.get_client_id(username);
            }
            std::getline(std::cin, reason);
            reason.erase(0, 1);
            if (reason.find_first_not_of(' ') == std::string::npos) {
                reason = "";
            }
            bool crash = false;
            if (cmd == "crash" || cmd == "crash-id")
                crash = true;
            server.kick_client(client, reason, k_HSteamNetConnection_Invalid, crash);
        }
    } while (server.running());

    std::cout << "Stopping..." << std::endl;
    if (server_thread.joinable())
        server_thread.join();

    server::destroy();
    printf("\r");
}