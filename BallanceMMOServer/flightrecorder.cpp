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

#include <asio/io_service.hpp>
#include <asio/ip/tcp.hpp>
#include <ya_getopt.h>

bool cheat = false;
struct client_data {
    std::string name;
    bool cheated;
    bmmo::ball_state state{};
};

class client: public role {
public:
    bool connect(const std::string& connection_string) {
        bmmo::hostname_parser hp(connection_string);
        using asio::ip::tcp; // tired of writing it out
        asio::io_context io_context;
        tcp::resolver resolver(io_context);
        tcp::resolver::query query(hp.get_address(), hp.get_port());
        tcp::resolver::iterator iter = resolver.resolve(query), end;
        while (iter != end) {
            tcp::endpoint ep = *iter++;
            std::string resolved_addr = ep.address().to_string() + ":" + std::to_string(ep.port());
            Printf("Trying %s...", resolved_addr);
            SteamNetworkingIPAddr server_address{};
            if (!server_address.ParseString(resolved_addr.c_str())) {
                return false;
            }
            SteamNetworkingConfigValue_t opt = generate_opt();
            connection_ = interface_->ConnectByIPAddress(server_address, 1, &opt);
            if (connection_ == k_HSteamNetConnection_Invalid)
                continue;
            io_context.stop();
            return true;
        }
        return false;
    }

    void run() override {
        running_ = true;
        startup_cv_.notify_all();
        while (running_) {
            if (!update())
                std::this_thread::sleep_for(std::chrono::nanoseconds((int)1e9 / 66));
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

    bmmo::timed_ball_state_msg& get_local_state_msg() {
        return local_state_msg_;
    }

    void print_clients() {
        Printf("%d clients online:", clients_.size());
        for (auto& i: clients_) {
            Printf("%u: %s%s, (%.2f, %.2f, %.2f)",
                    i.first,
                    i.second.name,
                    i.second.cheated ? " [CHEAT]" : "",
                    i.second.state.position.v[0], i.second.state.position.v[1], i.second.state.position.v[2]);
        }
    }

    void set_nickname(const std::string& name) {
        nickname_ = name;
    };

    void set_uuid(std::string& uuid) {
        while (auto pos = uuid.find('-') != std::string::npos) {
            uuid.erase(pos, 1);
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

    void teleport_to(const HSteamNetConnection player_id) {
        if (auto it = clients_.find(player_id); it != clients_.end()) {
            auto& client = it->second;
            local_state_msg_.content.position = client.state.position;
            local_state_msg_.content.rotation = client.state.rotation;
            // local_state_msg_.content.timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count(); // wtf is this from github copilot
            local_state_msg_.content.timestamp = SteamNetworkingUtils()->GetLocalTimestamp();
            send(local_state_msg_, k_nSteamNetworkingSend_Reliable);
            Printf("Teleported to %s at (%.3f, %.3f, %.3f).", client.name, client.state.position.x, client.state.position.y, client.state.position.z);
        }
        else {
            Printf("Player not found.");
        }
    }

    void wait_till_started() {
        while (!running()) {
            std::unique_lock<std::mutex> lk(startup_mutex_);
            startup_cv_.wait(lk);
        }
    }

    void whisper_to(const HSteamNetConnection player_id, const std::string& message) {
        if (auto it = clients_.find(player_id); it != clients_.end() || player_id == k_HSteamNetConnection_Invalid) {
            Printf("Whispering to (%u, %s): %s", player_id,
                (player_id == k_HSteamNetConnection_Invalid) ? "[Server]" : it->second.name, message);
            bmmo::private_chat_msg msg{};
            msg.player_id = player_id;
            msg.chat_content = message;
            msg.serialize();
            send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        }
        else {
            Printf("Player not found.");
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
        auto* raw_msg = reinterpret_cast<bmmo::general_message*>(networking_msg->m_pData);
        switch (raw_msg->code) {
            case bmmo::LoginAcceptedV2: {
                bmmo::login_accepted_v2_msg msg{};
                msg.raw.write(reinterpret_cast<char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();
                Printf("%d player(s) online:", msg.online_players.size());
                for (const auto& i: msg.online_players) {
                    Printf("%s (#%u)%s", i.second.name.c_str(), i.first, (i.second.cheated ? " [CHEAT]" : ""));
                    clients_[i.first] = { i.second.name, (bool) i.second.cheated };
                }
                break;
            }
            case bmmo::PlayerConnectedV2: {
                bmmo::player_connected_v2_msg msg{};
                msg.raw.write(reinterpret_cast<char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();
                Printf("%s (#%u) logged in with cheat mode %s.", msg.name.c_str(), msg.connection_id, (msg.cheated ? "on" : "off"));
                clients_[msg.connection_id] = { msg.name, (bool) msg.cheated };
                break;
            }
            case bmmo::PlayerDisconnected: {
                auto* msg = reinterpret_cast<bmmo::player_disconnected_msg*>(networking_msg->m_pData);
                if (auto it = clients_.find(msg->content.connection_id); it != clients_.end()) {
                    Printf("%s (#%u) disconnected.", it->second.name, it->first);
                    clients_.erase(it);
                }
                break;
            }
            case bmmo::OwnedBallState: {
                if (!print_states_)
                    break;
                assert(networking_msg->m_cbSize == sizeof(bmmo::owned_ball_state_msg));
                auto* obs = reinterpret_cast<bmmo::owned_ball_state_msg*>(networking_msg->m_pData);
                Printf("#%u: %d, (%.2lf, %.2lf, %.2lf), (%.2lf, %.2lf, %.2lf, %.2lf)",
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
                bmmo::owned_ball_state_v2_msg msg;
                msg.raw.write(reinterpret_cast<char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                for (auto& ball : msg.balls) {
                    if (print_states_)
                        Printf("#%u: %d, (%.2lf, %.2lf, %.2lf), (%.2lf, %.2lf, %.2lf, %.2lf)",
                            ball.player_id,
                            ball.state.type,
                            ball.state.position.x,
                            ball.state.position.y,
                            ball.state.position.z,
                            ball.state.rotation.x,
                            ball.state.rotation.y,
                            ball.state.rotation.z,
                            ball.state.rotation.w);
                    clients_[ball.player_id].state = ball.state;
                }

                break;
            }
            case bmmo::OwnedTimedBallState: {
                bmmo::owned_timed_ball_state_msg msg;
                msg.raw.write(reinterpret_cast<char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                for (auto& ball : msg.balls) {
                    if (print_states_)
                        Printf("%u: %d, (%.2f, %.2f, %.2f), (%.2f, %.2f, %.2f, %.2f)",
                            ball.player_id,
                            ball.state.type,
                            ball.state.position.x,
                            ball.state.position.y,
                            ball.state.position.z,
                            ball.state.rotation.x,
                            ball.state.rotation.y,
                            ball.state.rotation.z,
                            ball.state.rotation.w);
                    clients_[ball.player_id].state = ball.state;
                }

                break;
            }
            case bmmo::OwnedCheatState: {
                assert(networking_msg->m_cbSize == sizeof(bmmo::owned_cheat_state_msg));
                auto* ocs = reinterpret_cast<bmmo::owned_cheat_state_msg*>(networking_msg->m_pData);
                if (ocs->content.state.notify)
                    Printf("(%u, %s) turned cheat %s.", ocs->content.player_id, clients_[ocs->content.player_id].name, ocs->content.state.cheated ? "on" : "off");
                clients_[ocs->content.player_id].cheated = ocs->content.state.cheated;
                break;
            }
            case bmmo::Chat: {
                bmmo::chat_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                if (msg.player_id == k_HSteamNetConnection_Invalid)
                    Printf("[Server]: %s", msg.chat_content.c_str());
                else
                    Printf("(%u, %s): %s", msg.player_id, clients_[msg.player_id].name, msg.chat_content.c_str());
                break;
            }
            case bmmo::PrivateChat: {
                bmmo::private_chat_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                if (msg.player_id == k_HSteamNetConnection_Invalid)
                    Printf("[Server] whispers to you: %s", msg.chat_content.c_str());
                else
                    Printf("(%u, %s) whispers to you: %s", msg.player_id, clients_[msg.player_id].name, msg.chat_content.c_str());
                break;
            }
            case bmmo::ImportantNotification: {
                bmmo::important_notification_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                if (msg.player_id == k_HSteamNetConnection_Invalid)
                    Printf("[Announcement] [Server]: %s", msg.chat_content);
                else
                    Printf("[Announcement] (%u, %s): %s", msg.player_id, clients_[msg.player_id].name, msg.chat_content);
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
            case bmmo::Countdown: {
                auto* msg = reinterpret_cast<bmmo::countdown_msg*>(networking_msg->m_pData);
                Printf("[%u, %s]: %s - %s",
                    msg->content.sender,
                    clients_[msg->content.sender].name,
                    msg->content.map.get_display_name(map_names_),
                    msg->content.type == bmmo::countdown_type::Go ? "Go!" : std::to_string((int) msg->content.type));
                break;
            }
            case bmmo::LevelFinishV2: {
                auto* msg = reinterpret_cast<bmmo::level_finish_v2_msg*>(networking_msg->m_pData);
                int score = msg->content.levelBonus + msg->content.points + msg->content.lives * msg->content.lifeBonus;
                int total = int(msg->content.timeElapsed);
                int minutes = total / 60;
                int seconds = total % 60;
                int hours = minutes / 60;
                minutes = minutes % 60;
                int ms = int((msg->content.timeElapsed - total) * 1000);

                Printf("%s(#%u, %s) finished %s in %d%s place (score: %d; real time: %02d:%02d:%02d.%03d).",
                    msg->content.cheated ? "[CHEAT] " : "",
                    msg->content.player_id, clients_[msg->content.player_id].name,
                    msg->content.map.get_display_name(map_names_), msg->content.rank, bmmo::get_ordinal_rank(msg->content.rank),
                    score, hours, minutes, seconds, ms);
                break;
            }
            case bmmo::MapNames: {
                bmmo::map_names_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                map_names_.insert(msg.maps.begin(), msg.maps.end());
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
                    case bmmo::action_type::LoginDenied: {
                        Printf("Login denied.");
                        break;
                    }
                    case bmmo::action_type::CurrentMapQuery: {
                        bmmo::current_map_msg new_msg{};
                        send(new_msg, k_nSteamNetworkingSend_Reliable);
                        break;
                    }
                    case bmmo::action_type::Unknown: {
                        Printf("Unknown action request received.");
                        break;
                    }
                    default:
                        break;
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
    uint8_t uuid_[16]{};
    std::unordered_map<HSteamNetConnection, client_data> clients_;
    std::unordered_map<std::string, std::string> map_names_;
    bmmo::timed_ball_state_msg local_state_msg_{};
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
    std::string server_addr = "101.34.225.147:26676", username = "FlightRecorder",
                uuid = "00010002-0003-0004-0005-000600070008",
                log_path;
    bool print_states = false;
    if (parse_args(argc, argv, server_addr, username, uuid, log_path, &print_states) != 0)
        return 0;

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
        } else if (cmd == "move" || cmd == "translate") {
            bool translate = false;
            if (cmd == "translate") translate = true;
            auto msg = client.get_local_state_msg();
            float* pos = msg.content.position.v;
            for (int i = 0; i < 3; ++i) {
                if (!parser.empty())
                    pos[i] = atof(parser.get_next_word().c_str()) + ((translate) ? pos[i] : 0.0f);
                else
                    pos[i] = (rand() % 2000 - 1000) / 100.0f + ((translate) ? pos[i] : 0.0f);
            }
            float* rot = msg.content.rotation.v;
            for (int i = 0; i < 4; ++i) {
                if (!parser.empty())
                    rot[i] = atof(parser.get_next_word().c_str()) + ((translate) ? rot[i] : 0.0f);
                else
                    rot[i] = (rand() % 3600 - 1800) / 10.0f + ((translate) ? rot[i] : 0.0f);
            }
            msg.content.timestamp = SteamNetworkingUtils()->GetLocalTimestamp();
            client::Printf("Sending ball state message: (%.2f, %.2f, %.2f), (%.1f, %.1f, %.1f, %.1f)",
                pos[0], pos[1], pos[2], rot[0], rot[1], rot[2], rot[3]);
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
            
            if (!parser.empty())
                server_addr = parser.get_next_word();

            if (!client.connect(server_addr)) {
                std::cerr << "Cannot connect to server." << std::endl;
                return 1;
            }

            client_thread = std::move(std::thread([&client]() { client.run(); }));
            client.wait_till_started();
        } else if (cmd == "cheat") {
            if (!parser.empty())
                cheat = (parser.get_next_word() == "on") ? true : false;
            else
                cheat = !cheat;
            bmmo::cheat_state_msg msg;
            msg.content.cheated = cheat;
            client.send(msg, k_nSteamNetworkingSend_Reliable);
        } else if (cmd == "list") {
            client.print_clients();
        } else if (cmd == "print") {
            print_states = !print_states;
            client.set_print_states(print_states);
        } else if (cmd == "teleport") {
            client.teleport_to(atoll(parser.get_next_word().c_str()));
        } else if (cmd == "balltype") {
            auto& msg = client.get_local_state_msg();
            msg.content.type = atoi(parser.get_next_word().c_str());
            client.send(msg, k_nSteamNetworkingSend_Reliable);
        } else if (cmd == "whisper") {
            HSteamNetConnection dest = atoll(parser.get_next_word().c_str());
            client.whisper_to(dest, parser.get_rest_of_line());
        } else if (!cmd.empty()) {
            bmmo::chat_msg msg{};
            msg.chat_content = cmd + " " + parser.get_rest_of_line();
            msg.serialize();
            client.send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        }
    }

    std::cout << "Stopping..." << std::endl;
    if (client_thread.joinable())
        client_thread.join();
    client::destroy();
}
