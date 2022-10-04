#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif


#include "../BallanceMMOCommon/common.hpp"
#include "../BallanceMMOCommon/entity/record_entry.hpp"
#include <fstream>
#include <condition_variable>

template<typename T>
inline void read_variable(std::istream& stream, T& t) {
    stream.read(reinterpret_cast<char*>(&t), sizeof(T));
}

template<typename T>
inline T read_variable(std::istream& stream) {
    T t;
    stream.read(reinterpret_cast<char*>(&t), sizeof(T));
    return t;
}

enum class message_action: uint8_t { None, Broadcast, BroadcastNoDelay };

struct client_data {
    std::string name;
    uint8_t uuid[16]{};
};

class record_replayer: public role {
public:
    explicit record_replayer(uint16_t port, std::string filename) {
        port_ = port;
        record_stream_.open(filename, std::ios::binary);
        set_logging_level(k_ESteamNetworkingSocketsDebugOutputType_Important);
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

    bool setup() override {
        if (!record_stream_.is_open()) {
            FatalError("Error: cannot open record file.");
            return false;
        }
        std::string read_data;
        std::getline(record_stream_, read_data, '\0');
        if (read_data != "BallanceMMO FlightRecorder") {
            FatalError("Error: invalid record file.");
            return false;
        }
        Printf("BallanceMMO FlightRecorder Data");
        read_variable(record_stream_, record_version_);
        // record_stream_.read(reinterpret_cast<char*>(&version), sizeof(version));
        Printf("Version: \t\t%s\n", record_version_.to_string());
        auto start_time = read_variable<time_t>(record_stream_);
        // record_stream_.read(reinterpret_cast<char *>(&start_time), sizeof(start_time));
        char time_str[32];
        strftime(time_str, 32, "%F %T", localtime(&start_time));
        Printf("Record start time: \t%s\n", time_str);
        read_variable(record_stream_, record_start_time_);

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

        running_ = true;
        
        startup_cv_.notify_all();

        return true;
    }

    void run() override {
        while (running_) {
            auto next_update = std::chrono::steady_clock::now() + UPDATE_INTERVAL;
            update();
            std::this_thread::sleep_until(next_update);
        }
    }

    bool is_playing() {
        return playing_;
    }

    void start() {
        time_zero_ = std::chrono::steady_clock::now();
        Printf("Playing started.");
        play();
    }

    void play() {
        playing_ = true;
        while (running_ && playing_ && record_stream_.good() && record_stream_.peek() != std::ifstream::traits_type::eof()) {
            current_record_time_ = read_variable<SteamNetworkingMicroseconds>(record_stream_) - record_start_time_;
            auto size = read_variable<int32_t>(record_stream_);
            bmmo::record_entry entry(size);
            record_stream_.read(reinterpret_cast<char*>(entry.data), size);
            std::this_thread::sleep_until(time_zero_ + std::chrono::microseconds(current_record_time_));

            // auto* raw_msg = reinterpret_cast<bmmo::general_message*>(entry.data);
            // Printf("Time: %7.2lf | Code: %2u | Size: %4d\n", current_record_time_ / 1e6, raw_msg->code, entry.size);
            switch (parse_message(entry)) {
                case message_action::BroadcastNoDelay:
                    broadcast_message(entry.data, size, k_nSteamNetworkingSend_UnreliableNoDelay);
                    break;
                case message_action::None:
                    break;
                default:
                    broadcast_message(entry.data, size, k_nSteamNetworkingSend_Reliable);
            }
            
        }
    }

    void pause() {
        time_pause_ = std::chrono::steady_clock::now();
        playing_ = false;
        Printf("Playing paused at %.3lfs.", current_record_time_ / 1e6);
    }

    void resume() {
        time_zero_ += (std::chrono::steady_clock::now() - time_pause_);
        Printf("Playing resumed at %.3lfs.", current_record_time_ / 1e6);
        play();
    }

    void seek(double seconds) {
        SteamNetworkingMicroseconds dest_time = seconds * 1e6;
        if (dest_time <= current_record_time_) {
            Printf("Error: cannot seek to %.3lfs which is earlier than the current timestamp (%.3lfs)!", 
                seconds, current_record_time_ / 1e6);
            return;
        }
        if (playing_) {
            pause();
        }
        time_zero_ -= std::chrono::microseconds(dest_time - current_record_time_);
        while (running_ && current_record_time_ < dest_time
                && record_stream_.good() && record_stream_.peek() != std::ifstream::traits_type::eof()) {
            current_record_time_ = read_variable<SteamNetworkingMicroseconds>(record_stream_) - record_start_time_;
            auto size = read_variable<int32_t>(record_stream_);
            bmmo::record_entry entry(size);
            record_stream_.read(reinterpret_cast<char*>(entry.data), size);
            parse_message(entry);
        }
        Printf("Sought to %.3lfs.", current_record_time_ / 1e6);
    }

    void wait_till_started() {
        while (!running()) {
            std::unique_lock<std::mutex> lk(startup_mutex_);
            startup_cv_.wait(lk);
        }
    }

    void shutdown() {
        Printf("Shutting down...");
        for (auto& i: clients_) {
            interface_->CloseConnection(i.first, 0, "Server closed", true);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        running_ = false;
    }

    void print_clients() {
        Printf("%d client(s) online:", clients_.size());
        for (auto& i: clients_) {
            Printf("%u: %s",
                    i.first,
                    i.second.name);
        }
    }

    void print_current_record_time() {
        Printf("Current record is played to %.3lfs.", current_record_time_ / 1e6);
    }

    void poll_local_state_changes() override {}

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

private:
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
                    assert(pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting);
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
                char nick[32];
                sprintf(nick, "Unidentified%05d", rand() % 100000);

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

    void cleanup_disconnected_client(HSteamNetConnection* client) {
        // Locate the client.  Note that it should have been found, because this
        // is the only codepath where we remove clients (except on shutdown),
        // and connection change callbacks are dispatched in queue order.
        auto itClient = clients_.find(*client);
        //assert(itClient != clients_.end()); // It might in limbo state...So may not yet to be found
        if (itClient == clients_.end())
            return;

        bmmo::player_disconnected_msg msg;
        msg.content.connection_id = *client;
        broadcast_message(msg, k_nSteamNetworkingSend_Reliable, client);
        std::string name = itClient->second.name;
        if (auto itName = username_.find(name); itName != username_.end())
            username_.erase(itName);
        if (itClient != clients_.end())
            clients_.erase(itClient);
        Printf("%s (#%u) disconnected.", name, *client);
    }

    void on_message(ISteamNetworkingMessage* networking_msg) {
        auto client_it = clients_.find(networking_msg->m_conn);
        auto* raw_msg = reinterpret_cast<bmmo::general_message*>(networking_msg->m_pData);

        if (!(client_it != clients_.end() || raw_msg->code == bmmo::LoginRequest || raw_msg->code == bmmo::LoginRequestV2 || raw_msg->code == bmmo::LoginRequestV3)) { // ignore limbo clients message
            interface_->CloseConnection(networking_msg->m_conn, k_ESteamNetConnectionEnd_AppException_Min, "Invalid client", true);
            return;
        }

        switch (raw_msg->code) {
            case bmmo::LoginRequestV3: {
                auto msg = deserialize_message<bmmo::login_request_v3_msg>(networking_msg);

                interface_->SetConnectionName(networking_msg->m_conn, msg.nickname.c_str());
                if (std::memcmp(&msg.version, &record_version_, sizeof(bmmo::version_t)) != 0) {
                    interface_->CloseConnection(networking_msg->m_conn, k_ESteamNetConnectionEnd_App_Min + 1,
                        Sprintf("Incorrect version; please use BMMO v%s", record_version_.to_string()).c_str(), true);
                    break;
                }
                
                clients_[networking_msg->m_conn] = {msg.nickname, (bool)msg.cheated};  // add the client here
                memcpy(clients_[networking_msg->m_conn].uuid, msg.uuid, sizeof(uint8_t) * 16);
                username_[msg.nickname] = networking_msg->m_conn;
                Printf("%s (v%s) logged in!\n",
                        msg.nickname,
                        msg.version.to_string());
                
                bmmo::login_accepted_v2_msg accepted_msg{};
                accepted_msg.online_players = record_clients_;
                accepted_msg.serialize();
                send(networking_msg->m_conn, accepted_msg.raw.str().data(), accepted_msg.size(), k_nSteamNetworkingSend_Reliable);

                bmmo::map_names_msg names_msg;
                names_msg.maps = record_map_names_;
                names_msg.serialize();
                send(networking_msg->m_conn, names_msg.raw.str().data(), names_msg.size(), k_nSteamNetworkingSend_Reliable);
                break;
            }
            default:
                break;
        }
    }

    template<typename T>
    static inline T deserialize_message(void* data, int size) {
        if constexpr (std::is_trivially_copyable<T>()) {
            return *reinterpret_cast<T*>(data);
        }
        else {
            T msg{};
            msg.raw.write(reinterpret_cast<char*>(data), size);
            msg.deserialize();
            return msg;
        }
    }
    
    template<typename T>
    static inline T deserialize_message(ISteamNetworkingMessage* networking_msg) {
        return deserialize_message<T>(networking_msg->m_pData, networking_msg->m_cbSize);
    }

    message_action parse_message(bmmo::record_entry& entry /*, SteamNetworkingMicroseconds time*/) {
        auto* raw_msg = reinterpret_cast<bmmo::general_message*>(entry.data);
        // Printf("Time: %7.2lf | Code: %2u | Size: %4d\n", time / 1e6, raw_msg->code, entry.size);
        switch (raw_msg->code) {
            case bmmo::LoginAcceptedV2: {
                auto msg = deserialize_message<bmmo::login_accepted_v2_msg>(entry.data, entry.size);
                for (const auto& i: msg.online_players) {
                    record_clients_.insert({i.first, {i.second.name, i.second.cheated}});
                }
                broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
                // printf("Code: LoginAcceptedV2\n");
                // bmmo::login_accepted_v2_msg msg{};
                // msg.raw.write(reinterpret_cast<char*>(entry.data), entry.size);
                // msg.deserialize();
                // printf("\t%d player(s) online: \n", msg.online_players.size());
                return message_action::None;
            }
            case bmmo::PlayerDisconnected: {
                auto msg = deserialize_message<bmmo::player_disconnected_msg>(entry.data, entry.size);
                if (auto it = record_clients_.find(msg.content.connection_id); it != record_clients_.end()) {
                    record_clients_.erase(it);
                }
                broadcast_message(msg, k_nSteamNetworkingSend_Reliable);
                return message_action::None;
            }
            case bmmo::PlayerConnectedV2: {
                auto msg = deserialize_message<bmmo::player_connected_v2_msg>(entry.data, entry.size);
                record_clients_.insert({msg.connection_id, {msg.name, msg.cheated}});
                broadcast_message(entry.data, entry.size, k_nSteamNetworkingSend_Reliable);
                return message_action::None;
            }
            case bmmo::OwnedCheatState: {
                auto msg = deserialize_message<bmmo::owned_cheat_state_msg>(entry.data, entry.size);
                record_clients_[msg.content.player_id].cheated = msg.content.state.cheated;
                broadcast_message(entry.data, entry.size, k_nSteamNetworkingSend_Reliable);
                return message_action::None;
            }
            case bmmo::MapNames: {
                auto msg = deserialize_message<bmmo::map_names_msg>(entry.data, entry.size);
                record_map_names_.insert(msg.maps.begin(), msg.maps.end());
                broadcast_message(entry.data, entry.size, k_nSteamNetworkingSend_Reliable);
                return message_action::None;
            }
            case bmmo::OwnedTimedBallState: {
                return message_action::BroadcastNoDelay;
            }
            /*case bmmo::OwnedBallStateV2: {
                    bmmo::owned_ball_state_v2_msg msg;
                    msg.raw.write(reinterpret_cast<char*>(entry.data), entry.size);
                    msg.deserialize();

                    for (auto& ball : msg.balls) {
                        if (print_states)
                            Printf("%llu, %u, %d, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf\n",
                                time,
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
                case bmmo::OwnedTimedBallState: {
                    bmmo::owned_timed_ball_state_msg msg;
                msg.raw.write(reinterpret_cast<char*>(entry.data), entry.size);
                    msg.deserialize();

                    for (auto& ball : msg.balls) {
                        if (print_states)
                            Printf("%llu, %u, %d, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf\n",
                                time,
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
                */
            default: {
                // printf("Code: %d\n", raw_msg->code);
                return message_action::BroadcastNoDelay;
            }
        }
        // std::cout.write(entry.data, entry.size);
    }

    bmmo::version_t record_version_;
    std::ifstream record_stream_;
    SteamNetworkingMicroseconds record_start_time_;
    std::chrono::steady_clock::time_point time_zero_, time_pause_;

    uint16_t port_ = 0;
    std::mutex startup_mutex_;
    std::condition_variable startup_cv_;
    bool print_states = true;
    std::atomic_bool playing_ = false;

    SteamNetworkingMicroseconds current_record_time_;
    std::unordered_map<HSteamNetConnection, bmmo::player_status> record_clients_;
    std::unordered_map<std::string, std::string> record_map_names_;

    HSteamListenSocket listen_socket_ = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup poll_group_ = k_HSteamNetPollGroup_Invalid;

    std::unordered_map<HSteamNetConnection, client_data> clients_;
    std::unordered_map<std::string, HSteamNetConnection> username_;

    constexpr static inline std::chrono::nanoseconds UPDATE_INTERVAL{(int)1e9 / 66};
};



int main(int argc, char** argv) {
    std::string filename;
    if (argc <= 1) {
        std::cerr << "Please provide the filename of the record." << std::endl;
        exit(1);
    }
    else
        filename = argv[1];

    printf("Initializing sockets...\n");
    record_replayer::init_socket();

    uint16_t port = 26677;
    printf("Starting server at port %u.\n", port);
    record_replayer replayer(port, filename);

    printf("Bootstrapping server...\n");
    fflush(stdout);
    if (!replayer.setup())
        role::FatalError("Server failed on setup.");
    
    std::thread server_thread([&replayer]() { replayer.run(); });
    std::thread replayer_thread;

    bool started = false;

    replayer.wait_till_started();
    while (replayer.running()) {
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
        if (cmd == "play") {
            if (replayer.is_playing()) {
                replayer.Printf("Record is already playing.");
                continue;
            }
            if (replayer_thread.joinable())
                replayer_thread.join();
            if (started) {
                replayer_thread = std::thread([&replayer]() { replayer.resume(); });
            } else {
                replayer_thread = std::thread([&replayer]() { replayer.start(); });
                started = true;
            }
        } else if (cmd == "stop") {
            replayer.shutdown();
        } else if (cmd == "list") {
            replayer.print_clients();
        } else if (cmd == "time") {
            replayer.print_current_record_time();
        } else if (cmd == "pause") {
            if (!replayer.is_playing()) {
                replayer.Printf("Record is already not playing.");
                continue;
            }
            replayer.pause();
        } else if (cmd == "seek") {
            if (!started) {
                replayer_thread = std::thread([&replayer]() { replayer.start(); });
                started = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            replayer.seek(atof(parser.get_next_word().c_str()));
        } else if (!cmd.empty()) {
            replayer.Printf("Error: unknown command \"%s\".", cmd);
        }
    }

    std::cout << "Stopping..." << std::endl;
    if (replayer_thread.joinable())
        replayer_thread.join();
    if (server_thread.joinable())
        server_thread.join();

    record_replayer::destroy();
    printf("\r");
}
