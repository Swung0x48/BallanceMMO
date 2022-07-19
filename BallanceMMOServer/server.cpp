#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif

#include "../BallanceMMOCommon/role/role.hpp"
#include <vector>
#include "../BallanceMMOCommon/common.hpp"

// #include <iomanip>
#include <mutex>
// #include <shared_mutex>
#include <fstream>

#include <ya_getopt.h>
#include <yaml-cpp/yaml.h>

struct client_data {
    std::string name;
    bool cheated = false;
    uint8_t uuid[16];
    bmmo::ball_state state;
    bool updated = true;
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
            auto next_update = std::chrono::system_clock::now() + UPDATE_INTERVAL;
            update();
            std::this_thread::sleep_until(next_update);
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

    int get_client_count() {
        return clients_.size();
    }

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

    bool load_config() {
        std::string logging_level_string = "important";
        std::ifstream ifile("config.yml");
        if (ifile.is_open() && ifile.peek() != std::ifstream::traits_type::eof()) {
            try {
                config_ = YAML::Load(ifile);
                if (config_["op_list"])
                    op_players_ = config_["op_list"].as<std::unordered_map<std::string, std::string>>();
                if (config_["enable_op_privileges"])
                    op_mode_ = config_["enable_op_privileges"].as<bool>();
                if (config_["restart_level_after_countdown"])
                    restart_level_ = config_["restart_level_after_countdown"].as<bool>();
                if (config_["force_restart_after_countdown"])
                    force_restart_level_ = config_["force_restart_after_countdown"].as<bool>();
                if (config_["logging_level"])
                    logging_level_string = config_["logging_level"].as<std::string>();
            } catch (const std::exception& e) {
                Printf("Error: failed to parse config: %s", e.what());
                return false;
            }
        } else {
            Printf("Config is empty. Generating default config...");
            op_players_ = {{"example_player", "00000001-0002-0003-0004-000000000005"}};
        }

        config_["enable_op_privileges"] = op_mode_;
        config_["restart_level_after_countdown"] = restart_level_;
        config_["force_restart_after_countdown"] = force_restart_level_;
        if (logging_level_string == "msg")
            logging_level_ = k_ESteamNetworkingSocketsDebugOutputType_Msg;
        else if (logging_level_string == "warning")
            logging_level_ = k_ESteamNetworkingSocketsDebugOutputType_Warning;
        else {
            logging_level_ = k_ESteamNetworkingSocketsDebugOutputType_Important;
            config_["logging_level"] = "important";
        }
        set_logging_level(logging_level_);

        ifile.close();
        save_config_to_file();
        Printf("Config loaded successfully.");
        return true;
    }

    void print_clients(bool print_uuid = false) {
        Printf("%d clients online:", clients_.size());
        for (auto& i: clients_) {
            Printf("%u: %s%s%s%s",
                    i.first,
                    i.second.name.c_str(),
                    print_uuid ? (" (" + get_uuid_string(i.second.uuid) + ")").c_str() : "",
                    i.second.cheated ? " [CHEAT]" : "",
                    is_op(i.first) ? " [OP]" : "");
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

    inline void pull_unupdated_ball_states(std::vector<bmmo::owned_ball_state>& balls) {
        for (auto& i: clients_) {
            if (i.second.updated)
                continue;
            bmmo::owned_ball_state obs;
            std::unique_lock<std::mutex> lock(client_data_mutex_);
            obs.player_id = i.first;
            obs.state = i.second.state;
            balls.push_back(obs);
            i.second.updated = true;
        }
    }

    void set_op(HSteamNetConnection client, bool op) {
        if (!client_exists(client))
            return;
        std::string name = clients_[client].name;
        if (op) {
            if (op_players_.find(name) != op_players_.end()) {
                if (op_players_[name] == get_uuid_string(clients_[client].uuid)) {
                    Printf("Error: client \"%s\" already has OP privileges.", name.c_str());
                    return;
                }
            }
            op_players_[name] = get_uuid_string(clients_[client].uuid);
            Printf("%s is now an operator.", name.c_str());
        } else {
            if (op_players_.find(name) == op_players_.end())
                return;
            op_players_.erase(name);
            Printf("%s is no longer an operator.", name.c_str());
        }
        save_config_to_file();
        bmmo::op_state_msg msg{};
        msg.content.op = op;
        send(client, msg, k_nSteamNetworkingSend_Reliable);
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
        if (ticking_)
            stop_ticking();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        running_ = false;
//        if (server_thread_.joinable())
//            server_thread_.join();
    }

protected:
    bool setup() {
        Printf("Loading config from config.yml...");
        if (!load_config()) {
            Printf("Error: failed to load config. Please try fixing or emptying it first.");
            return false;
        }

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

    void save_login_data(HSteamNetConnection client) {
        SteamNetConnectionInfo_t pInfo;
        interface_->GetConnectionInfo(client, &pInfo);
        SteamNetworkingIPAddr ip = pInfo.m_addrRemote;
        std::string ip_str,
                    uuid_str = get_uuid_string(clients_[client].uuid),
                    name = clients_[client].name;
        if (ip.IsIPv4()) {
            for (int i = 0; i < 4; ++i)
                ip_str.append("." + std::to_string(ip.m_ipv4.m_ip[i]));
        } else {
            std::stringstream ss;
            for (int i = 0; i < 8; ++i)
                ss << ":" << std::hex << std::setw(2) << std::setfill('0') << ip.m_ipv6[i];
            ip_str = ss.str();
        }
        ip_str.erase(0, 1);
        // snprintf(ipAddr, sizeof(ipAddr), "%u.%u.%u.%u", (ipAddress & 0xff000000) >> 24,
        //                                                (ipAddress & 0x00ff0000) >> 16,
        //                                                (ipAddress & 0x0000ff00) >> 8,
        //                                                (ipAddress & 0x000000ff));
        YAML::Node login_data;
        std::ifstream ifile("login_data.yml");
        if (ifile.is_open() && ifile.peek() != std::ifstream::traits_type::eof()) {
            try {
                login_data = YAML::Load(ifile);
            } catch (const std::exception& e) {
                Printf("Error: failed to parse config: %s", e.what());
                return;
            }
        }
        ifile.close();
        auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::string time_str(20, 0);
        time_str.resize(std::strftime(&time_str[0], time_str.size(),
            "%F %X", std::localtime(&time)));
        if (!login_data[uuid_str])
            login_data[uuid_str] = YAML::Node(YAML::NodeType::Map);
        if (!login_data[uuid_str][name])
            login_data[uuid_str][name] = YAML::Node(YAML::NodeType::Map);
        login_data[uuid_str][name][time_str] = ip_str;
        std::ofstream ofile("login_data.yml");
        if (ofile.is_open()) {
            ofile << login_data;
            ofile.close();
        }
    }

    inline void save_config_to_file() {
        config_["op_list"] = op_players_;
        std::ofstream config_file("config.yml");
        if (!config_file.is_open()) {
            Printf("Error: failed to open config file for writing.");
            return;
        }
        config_file << "# Config file for Ballance MMO Server" << std::endl;
        config_file << "# Notes:\n"
                    << "# - Op list player data style: \"playername: uuid\".\n"
                    << "# - Level restart: whether to restart on clients' sides after \"Go!\". If not forced, only for clients on the same map.\n"
                    << "# - Options for log levels: important, warning, msg."
                    << std::endl;
        config_file << config_;
        config_file.close();
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
        Printf("%s (#%u) disconnected.", name.c_str(), *client);
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

    bool deny_action(HSteamNetConnection client) {
        if (op_mode_ && op_online() && !is_op(client)) {
            bmmo::action_denied_msg denied_msg;
            denied_msg.content.reason = bmmo::NoPermission;
            send(client, denied_msg, k_nSteamNetworkingSend_Reliable);
            return true;
        }
        return false;
    }

    std::string get_uuid_string(uint8_t* uuid) {
        // std::stringstream ss;
        // for (int i = 0; i < 16; i++) {
        //     ss << std::hex << std::setfill('0') << std::setw(2) << (int)uuid[i];
        //     if (i == 3 || i == 5 || i == 7 || i == 9)
        //         ss << '-';
        // }
        // return ss.str();
        char str[37] = {};
        sprintf(str,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
            uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]
        );
        return {str};
    }

    bool is_op(HSteamNetConnection client) {
        if (!client_exists(client))
            return false;
        auto op_it = op_players_.find(clients_[client].name);
        if (op_it == op_players_.end())
            return false;
        if (op_it->second == get_uuid_string(clients_[client].uuid))
            return true;
        return false;
    }

    bool op_online() {
        for (auto& op : op_players_) {
            if (username_.find(op.first) != username_.end()) {
                if (op.second == get_uuid_string(clients_[username_[op.first]].uuid))
                    return true;
            }
        }
        return false;
    }

    bool validate_client(HSteamNetConnection client, bmmo::login_request_v3_msg& msg) {
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
            reason << "A player with the same username \"" << msg.nickname << "\" already exists on this serer.";
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
            reason << "Invalid character '" << msg.nickname[invalid_pos] << "'; nicknames can only contain alphanumeric characters and underscores.";
            nReason = k_ESteamNetConnectionEnd_App_Min + 4;
        }

        if (nReason != k_ESteamNetConnectionEnd_Invalid) {
            bmmo::simple_action_msg new_msg;
            new_msg.content.action = bmmo::LoginDenied;
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
                if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connected
                        && logging_level_ != k_ESteamNetworkingSocketsDebugOutputType_Msg) {
                    // Select appropriate log messages
                    const char* pszDebugLogAction;
                    if (pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
                        pszDebugLogAction = "problem detected locally";
                    } else if (pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer) {
                        // Note that here we could check the reason code to see if
                        // it was a "usual" connection or an "unusual" one.
                        pszDebugLogAction = "closed by peer";
                    } else {
                        pszDebugLogAction = "closed by app";
                    }

                    // Spew something to our own log.  Note that because we put their nick
                    // as the connection description, it will show up, along with their
                    // transport-specific data (e.g. their IP address)
                    Printf( "[%s] %s (%d): %s\n",
                            pInfo->m_info.m_szConnectionDescription,
                            pszDebugLogAction,
                            pInfo->m_info.m_eEndReason,
                            pInfo->m_info.m_szEndDebug
                    );

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

                if (ticking_ && get_client_count() <= 1)
                    stop_ticking();

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
                char nick[32];
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
        if (!(client_it != clients_.end() || raw_msg->code == bmmo::LoginRequest || raw_msg->code == bmmo::LoginRequestV2 || raw_msg->code == bmmo::LoginRequestV3)) { // ignore limbo clients message
            interface_->CloseConnection(networking_msg->m_conn, k_ESteamNetConnectionEnd_AppException_Min, "Invalid client", true);
            return;
        }

        switch (raw_msg->code) {
            case bmmo::LoginRequest: {
                bmmo::simple_action_msg msg;
                msg.content.action = bmmo::LoginDenied;
                send(networking_msg->m_conn, msg, k_nSteamNetworkingSend_Reliable);
                interface_->CloseConnection(networking_msg->m_conn, k_ESteamNetConnectionEnd_App_Min + 1, "Outdated client", true);
                break;
            }
            case bmmo::LoginRequestV2: {
                bmmo::login_request_v2_msg msg;
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                interface_->SetConnectionName(networking_msg->m_conn, msg.nickname.c_str());

                std::string reason = "Outdated client (client: " + msg.version.to_string()
                        + "; minimum: " + bmmo::minimum_client_version.to_string() + ")";
                bmmo::simple_action_msg new_msg;
                new_msg.content.action = bmmo::LoginDenied;
                send(networking_msg->m_conn, new_msg, k_nSteamNetworkingSend_Reliable);
                interface_->CloseConnection(networking_msg->m_conn, k_ESteamNetConnectionEnd_App_Min + 1, reason.c_str(), true);
                break;
            }
            case bmmo::LoginRequestV3: {
                bmmo::login_request_v3_msg msg;
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                interface_->SetConnectionName(networking_msg->m_conn, msg.nickname.c_str());

                if (!validate_client(networking_msg->m_conn, msg))
                    break;

                // accepting client
                clients_[networking_msg->m_conn] = {msg.nickname, (bool)msg.cheated};  // add the client here
                memcpy(clients_[networking_msg->m_conn].uuid, msg.uuid, sizeof(uint8_t) * 16);
                username_[msg.nickname] = networking_msg->m_conn;
                Printf("%s (%s; v%s) logged in with cheat mode %s!\n",
                        msg.nickname.c_str(),
                        get_uuid_string(msg.uuid).substr(0, 8).c_str(),
                        msg.version.to_string().c_str(),
                        msg.cheated ? "on" : "off");

                // notify this client of other online players
                bmmo::login_accepted_v2_msg accepted_msg;
                for (auto it = clients_.begin(); it != clients_.end(); ++it) {
                    //if (client_it != it)
                    accepted_msg.online_players[it->first] = { it->second.name, it->second.cheated };
                }
                accepted_msg.serialize();
                send(networking_msg->m_conn, accepted_msg.raw.str().data(), accepted_msg.raw.str().size(), k_nSteamNetworkingSend_Reliable);

                save_login_data(networking_msg->m_conn);

                // notify other client of the fact that this client goes online
                bmmo::player_connected_v2_msg connected_msg;
                connected_msg.connection_id = networking_msg->m_conn;
                connected_msg.name = msg.nickname;
                connected_msg.cheated = msg.cheated;
                connected_msg.serialize();
                broadcast_message(connected_msg.raw.str().data(), connected_msg.size(), k_nSteamNetworkingSend_Reliable, &networking_msg->m_conn);

                if (!map_names_.empty()) {
                    bmmo::map_names_msg names_msg;
                    names_msg.maps = map_names_;
                    names_msg.serialize();
		            send(networking_msg->m_conn, names_msg.raw.str().data(), names_msg.size(), k_nSteamNetworkingSend_Reliable);
                }

                if (!ticking_ && get_client_count() > 1)
                    start_ticking();

                break;
            }
            case bmmo::LoginAccepted:
                break;
            case bmmo::PlayerDisconnected:
                break;
            case bmmo::PlayerConnected:
                break;
            case bmmo::Ping:
                break;
            case bmmo::BallState: {
                auto* state_msg = reinterpret_cast<bmmo::ball_state_msg*>(networking_msg->m_pData);

                std::unique_lock<std::mutex> lock(client_data_mutex_);
                clients_[networking_msg->m_conn].state = state_msg->content;
                clients_[networking_msg->m_conn].updated = false;

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

//                 bmmo::owned_ball_state_msg new_msg;
//                 new_msg.content.state = state_msg->content;
// //                std::memcpy(&(new_msg.content), &(state_msg->content), sizeof(state_msg->content));
//                 new_msg.content.player_id = networking_msg->m_conn;
//                 broadcast_message(&new_msg, sizeof(new_msg), k_nSteamNetworkingSend_UnreliableNoDelay, &networking_msg->m_conn);

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
            case bmmo::Countdown: {
                if (deny_action(networking_msg->m_conn))
                    break;
                auto* msg = reinterpret_cast<bmmo::countdown_msg*>(networking_msg->m_pData);

                std::string map_name = msg->content.map.get_display_name(map_names_);
                switch (msg->content.type) {
                    case bmmo::CountdownType_Go: {
                        Printf("[%u, %s]: %s - Go!%s", networking_msg->m_conn, client_it->second.name.c_str(), map_name.c_str(), msg->content.force_restart ? " (rank reset)" : "");
                        if (force_restart_level_ || msg->content.force_restart)
                            map_ranks_.clear();
                        else
                            map_ranks_[msg->content.map.get_hash_bytes_string()] = 0;
                        msg->content.restart_level = restart_level_;
                        msg->content.force_restart = force_restart_level_;
                        break;
                    }
                    case bmmo::CountdownType_1:
                    case bmmo::CountdownType_2:
                    case bmmo::CountdownType_3:
                        Printf("[%u, %s]: %s - %u", networking_msg->m_conn, client_it->second.name.c_str(), map_name.c_str(), msg->content.type);
                        break;
                    case bmmo::CountdownType_Unknown:
                    default:
                        return;
                }

                msg->content.sender = networking_msg->m_conn;
                broadcast_message(*msg, k_nSteamNetworkingSend_Reliable);
                break;
            }
            case bmmo::DidNotFinish: {
                auto* msg = reinterpret_cast<bmmo::did_not_finish_msg*>(networking_msg->m_pData);
                msg->content.player_id = networking_msg->m_conn;
                Printf(
                    "%s(#%u, %s) did not finish %s (aborted at sector %d).",
                    msg->content.cheated ? "[CHEAT] " : "",
                    msg->content.player_id, clients_[msg->content.player_id].name.c_str(),
                    msg->content.map.get_display_name(map_names_).c_str(),
                    msg->content.sector
                );
                broadcast_message(*msg, k_nSteamNetworkingSend_Reliable);
                break;
            }
            case bmmo::LevelFinish:
                break;
            case bmmo::LevelFinishV2: {
                auto* msg = reinterpret_cast<bmmo::level_finish_v2_msg*>(networking_msg->m_pData);
                msg->content.player_id = networking_msg->m_conn;

                // Cheat check
                if (msg->content.map.level * 100 != msg->content.levelBonus || msg->content.lifeBonus != 200) {
                    msg->content.cheated = true;
                }

                // Prepare data...
                int score = msg->content.levelBonus + msg->content.points + msg->content.lives * msg->content.lifeBonus;

                int total = int(msg->content.timeElapsed);
                int minutes = total / 60;
                int seconds = total % 60;
                int hours = minutes / 60;
                minutes = minutes % 60;
                int ms = int((msg->content.timeElapsed - total) * 1000);

                // Prepare message
                std::string md5_str = msg->content.map.get_hash_bytes_string();
                msg->content.rank = ++map_ranks_[md5_str];
                Printf("%s(#%u, %s) finished %s in %d%s place (score: %d; real time: %02d:%02d:%02d.%03d).",
                    msg->content.cheated ? "[CHEAT] " : "",
                    msg->content.player_id, clients_[msg->content.player_id].name.c_str(),
                    msg->content.map.get_display_name(map_names_).c_str(), map_ranks_[md5_str], bmmo::get_ordinal_rank(msg->content.rank).c_str(),
                    score, hours, minutes, seconds, ms);

                broadcast_message(*msg, k_nSteamNetworkingSend_Reliable);

                break;
            }
            case bmmo::MapNames: {
                bmmo::map_names_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                map_names_.insert(msg.maps.begin(), msg.maps.end());

                msg.clear();
                msg.serialize();
                broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable, &networking_msg->m_conn);
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
                if (deny_action(networking_msg->m_conn))
                    break;
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
                if (!msg.player_name.empty()) {
                    Printf("%s requested to kick player \"%s\"!", client_it->second.name.c_str(), msg.player_name.c_str());
                    player_id = get_client_id(msg.player_name);
                } else {
                    Printf("%s requested to kick player %u!", client_it->second.name.c_str(), msg.player_id);
                }
                if (deny_action(networking_msg->m_conn))
                    break;

                if (!kick_client(player_id, msg.reason, client_it->first)) {
                    bmmo::action_denied_msg new_msg{};
                    new_msg.content.reason = bmmo::TargetNotFound;
                    send(networking_msg->m_conn, new_msg, k_nSteamNetworkingSend_Reliable);
                };

                break;
            }
            case bmmo::CurrentMap:  {
                auto* msg = reinterpret_cast<bmmo::current_map_msg*>(networking_msg->m_pData);
                Printf("(#%u, %s) is at the %d%s sector of %s.",
                    networking_msg->m_conn, clients_[networking_msg->m_conn].name.c_str(),
                    msg->content.sector, bmmo::get_ordinal_rank(msg->content.sector).c_str(),
                    msg->content.map.get_display_name(map_names_).c_str());
                // broadcast_message(*msg, k_nSteamNetworkingSend_Reliable);
                break;
            }
            case bmmo::SimpleAction: {
                auto* msg = reinterpret_cast<bmmo::simple_action_msg*>(networking_msg->m_pData);
                switch (msg->content.action) {
                    case bmmo::CurrentMapQuery: {
                        break;
                    }
                    default:
                        break;
                }
                break;
            }
            case bmmo::ActionDenied:
            case bmmo::OpState:
            case bmmo::KeyboardInput:
            case bmmo::PlainText:
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
        else if (msg_count < 0)
            FatalError("Error checking for messages.");
        assert(msg_count > 0);

        for (int i = 0; i < msg_count; ++i) {
            on_message(incoming_messages_[i]);
            incoming_messages_[i]->Release();
        }

        return msg_count;
    }

    inline void tick() {
        bmmo::owned_ball_state_v2_msg msg{};
        pull_unupdated_ball_states(msg.balls);
        if (msg.balls.empty())
            return;
        msg.serialize();
        broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_UnreliableNoDelay);
    };

    void start_ticking() {
        ticking_ = true;
        ticking_thread_ = std::thread([&]() {
            Printf("Ticking started.");
            while (ticking_) {
                auto next_tick = std::chrono::system_clock::now() + TICK_INTERVAL;
                tick();
                std::this_thread::sleep_until(next_tick);
            }
        });
    }
    void stop_ticking() {
        ticking_ = false;
        ticking_thread_.join();
        Printf("Ticking stopped.");
    }

    uint16_t port_ = 0;
    HSteamListenSocket listen_socket_ = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup poll_group_ = k_HSteamNetPollGroup_Invalid;
//    std::thread server_thread_;
    std::unordered_map<HSteamNetConnection, client_data> clients_;
    std::unordered_map<std::string, HSteamNetConnection> username_;
    std::mutex client_data_mutex_;
    constexpr static inline std::chrono::nanoseconds TICK_INTERVAL{(int)1e9 / 66},
                                                     UPDATE_INTERVAL{(int)1e9 / 66};
    std::thread ticking_thread_;
    std::atomic_bool ticking_ = false;
    YAML::Node config_;
    std::unordered_map<std::string, std::string> op_players_, map_names_;
    std::unordered_map<std::string, int> map_ranks_;
    bool op_mode_ = true, restart_level_ = false, force_restart_level_ = false;
    ESteamNetworkingSocketsDebugOutputType logging_level_ = k_ESteamNetworkingSocketsDebugOutputType_Important;
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
        std::string line, cmd;
#ifdef _WIN32
        std::wstring wline;
        std::getline(std::wcin, wline);
        line = bmmo::message_utils::ConvertWideToANSI(wline);
        if (auto pos = line.rfind('\r'); pos != std::string::npos)
            line.erase(pos);
#else
        std::getline(std::cin, line);
#endif
        bmmo::command_parser parser(line);

        cmd = parser.get_next_word();
        if (cmd == "stop") {
            server.shutdown();
        } else if (cmd == "list") {
            server.print_clients();
        } else if (cmd == "list-uuid") {
            server.print_clients(true);
        } else if (cmd == "say") {
            bmmo::chat_msg msg{};
            msg.chat_content = parser.get_rest_of_line();
            msg.serialize();

            server.broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
            server.Printf("([Server]): %s", msg.chat_content.c_str());
        } else if (cmd == "plaintext") {
            HSteamNetConnection client = server.get_client_id(parser.get_next_word());
            if (client == k_HSteamNetConnection_Invalid)
                continue;
            bmmo::plain_text_msg msg{};
            msg.text_content = parser.get_rest_of_line();
            msg.serialize();
            server.send(client, msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
            server.Printf("Sent \"%s\" to #%u.", msg.text_content.c_str(), client);
        } else if (cmd == "plaintext-broadcast") {
            bmmo::plain_text_msg msg{};
            msg.text_content = parser.get_rest_of_line();
            msg.serialize();
            server.broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
            server.Printf("Broadcast \"%s\".", msg.text_content.c_str());
        } else if (cmd == "cheat") {
            bool cheat_state = false;
            if (parser.get_next_word() == "on")
                cheat_state = true;
            server.toggle_cheat(cheat_state);
        } else if (cmd == "ver" || cmd == "version") {
            server.print_version_info();
        } else if (cmd == "getmap") {
            bmmo::simple_action_msg msg;
            msg.content.action = bmmo::CurrentMapQuery;
            server.broadcast_message(msg, k_nSteamNetworkingSend_Reliable);
        } else if (cmd == "kick" || cmd == "kick-id" || cmd == "crash" || cmd == "crash-id") {
            HSteamNetConnection client = k_HSteamNetConnection_Invalid;
            if (cmd == "kick-id" || cmd == "crash-id") {
                client = atoll(parser.get_next_word().c_str());
                if (client == 0) {
                    server.Printf("Error: invalid connection id.");
                    continue;
                }
            } else {
                client = server.get_client_id(parser.get_next_word());
            }
            std::string reason = parser.get_rest_of_line();
            bool crash = false;
            if (cmd == "crash" || cmd == "crash-id")
                crash = true;
            server.kick_client(client, reason, k_HSteamNetConnection_Invalid, crash);
        } else if (cmd == "op" || cmd == "op-id" || cmd == "deop" || cmd == "deop-id") {
            HSteamNetConnection client = k_HSteamNetConnection_Invalid;
            if (cmd == "op-id" || cmd == "deop-id") {
                client = atoll(parser.get_next_word().c_str());
                if (client == 0) {
                    server.Printf("Error: invalid connection id.");
                    continue;
                }
            } else {
                client = server.get_client_id(parser.get_next_word());
            }
            bool op = false;
            if (cmd == "op" || cmd == "op-id")
                op = true;
            server.set_op(client, op);
        } else if (cmd == "reload") {
            if (!server.load_config())
                server.Printf("Error: failed to reload config.");
        }
    } while (server.running());

    std::cout << "Stopping..." << std::endl;
    if (server_thread.joinable())
        server_thread.join();

    server::destroy();
    printf("\r");
}