#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif

#include <vector>
#include <unordered_set>
#include <condition_variable>
#include "../BallanceMMOCommon/common.hpp"

// #include <iomanip>
#include <mutex>
// #include <shared_mutex>
#include <fstream>
#include <filesystem>

#include <ya_getopt.h>
#include <yaml-cpp/yaml.h>

#define PICOJSON_USE_INT64
#include <picojson/picojson.h>
#include "server_data.hpp"
#include "config_manager.hpp"

class server: public role {
public:
    explicit server(uint16_t port) {
        port_ = port;
    }

    void run() override {
        while (running_) {
            auto update_begin = std::chrono::steady_clock::now();
            update();
            if (ticking_) {
                std::this_thread::sleep_until(update_begin + bmmo::SERVER_TICK_DELAY);
                tick();
            }
            std::this_thread::sleep_until(update_begin + bmmo::SERVER_RECEIVE_INTERVAL);
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

    EResult send(const HSteamNetConnection destination, const void* buffer, size_t size, int send_flags = k_nSteamNetworkingSend_Reliable, int64* out_message_number = nullptr) {
        return interface_->SendMessageToConnection(destination,
                                                   buffer,
                                                   size,
                                                   send_flags,
                                                   out_message_number);

    }

    template<bmmo::trivially_copyable_msg T>
    EResult send(const HSteamNetConnection destination, const T& msg, int send_flags = k_nSteamNetworkingSend_Reliable, int64* out_message_number = nullptr) {
        static_assert(std::is_trivially_copyable<T>());
        return send(destination,
                    &msg,
                    sizeof(msg),
                    send_flags,
                    out_message_number);
    }

    void broadcast_message(const void* buffer, size_t size, int send_flags = k_nSteamNetworkingSend_Reliable, const HSteamNetConnection ignored_client = k_HSteamNetConnection_Invalid) {
        for (auto& i: clients_)
            if (ignored_client != i.first)
                send(i.first, buffer, size,
                                                    send_flags,
                                                    nullptr);
    }

    template<bmmo::trivially_copyable_msg T>
    void broadcast_message(const T& msg, int send_flags = k_nSteamNetworkingSend_Reliable, const HSteamNetConnection ignored_client = k_HSteamNetConnection_Invalid) {
        static_assert(std::is_trivially_copyable<T>());

        broadcast_message(&msg, sizeof(msg), send_flags, ignored_client);
    }

    void receive(void* data, size_t size, HSteamNetConnection client = k_HSteamNetConnection_Invalid) {
        if (get_client_count() == 0) { Printf("Error: no online players found."); return; }
        auto* networking_msg = SteamNetworkingUtils()->AllocateMessage(0);
        if (client == k_HSteamNetConnection_Invalid) // random player
            client = std::next(clients_.begin(), rand() % clients_.size())->first;
        networking_msg->m_conn = client;
        networking_msg->m_pData = data;
        networking_msg->m_cbSize = size;
        on_message(networking_msg);
        networking_msg->Release();
    }

    auto& get_bulletin() { return permanent_notification_; }

    HSteamNetConnection get_client_id(std::string username, bool suppress_error = false) const {
        if (username.empty()) return k_HSteamNetConnection_Invalid;
        std::string begin_name(bmmo::message_utils::to_lower(username));
        std::string end_name(begin_name);
        ++end_name[end_name.length() - 1];
        auto username_it = username_.lower_bound(begin_name);
        const auto username_it_end = username_.lower_bound(end_name);
        if (username_it == username_it_end) {
            if (!suppress_error)
                Printf("Error: client \"%s\" not found.", username);
            return k_HSteamNetConnection_Invalid;
        }
        else if (std::next(username_it) == username_it_end) {
            return username_it->second;
        }
        std::string names;
        for (; username_it != username_it_end; ++username_it)
            names += ", " + username_it->first;
        if (!suppress_error)
            Printf("Error: multiple possible players: %s.", names.erase(0, 2));
        return k_HSteamNetConnection_Invalid;
    };

    inline int get_client_count() const noexcept { return clients_.size(); }

    inline config_manager get_config() { return config_; }
    inline const bmmo::map& get_last_countdown_map() { return last_countdown_map_; }

    const bmmo::ranking_entry::player_rankings* get_map_rankings(const bmmo::map& map) {
        auto map_it = maps_.find(map.get_hash_bytes_string());
        if (map_it == maps_.end() || (map_it->second.rankings.first.empty() && map_it->second.rankings.second.empty()))
            return nullptr;
        return &(map_it->second.rankings);
    }

    bool kick_client(HSteamNetConnection client, std::string reason = "",
            HSteamNetConnection executor = k_HSteamNetConnection_Invalid,
            bmmo::connection_end::code type = bmmo::connection_end::Kicked) {
        if (!client_exists(client))
                // || type < bmmo::connection_end::PlayerKicked_Min || type >= bmmo::connection_end::PlayerKicked_Max
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

        if (type == bmmo::connection_end::FatalError) {
            // Triggers a segmentation fault explicitly on client's side.
            // A dirty hack, but it works and we don't need to
            // worry about the outcome; client will just terminate immediately.
            bmmo::simple_action_msg fatal_error_msg{};
            fatal_error_msg.content = bmmo::simple_action::TriggerFatalError;
            send(client, fatal_error_msg, k_nSteamNetworkingSend_Reliable);
            reason = "fatal error";
        }

        if (!reason.empty()) {
            kick_notice.append(" (" + reason + ")");
            msg.reason = reason;
        }
        kick_notice.append(".");

        msg.crashed = (type >= bmmo::connection_end::Crash && type < bmmo::connection_end::PlayerKicked_Max);

        interface_->CloseConnection(client, type, kick_notice.c_str(), true);
        msg.serialize();
        broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);

        return true;
    }

    bool load_config() {
        if (!config_.load())
            return false;
        if (get_client_count() < 1) map_names_.clear();
        map_names_.insert(config_.default_map_names.begin(), config_.default_map_names.end());
        if (get_client_count() > 0 && !map_names_.empty()) {
            bmmo::map_names_msg name_msg;
            name_msg.maps = map_names_;
            name_msg.serialize();
            broadcast_message(name_msg.raw.str().data(), name_msg.size(), k_nSteamNetworkingSend_Reliable);
        }
        return true;
    }

    void print_clients(bool print_uuid = false) {
        decltype(username_) spectators;
        const auto max_name_length = (int) std::min(bmmo::name_validator::max_length + 1,
            std::ranges::max_element(
                username_,
                [](const auto& p1, decltype(p1) p2) { return p1.first.length() < p2.first.length(); }
            )->first.length());
        static const auto print_client = [&](auto id, auto data) {
            SteamNetConnectionRealTimeStatus_t status{};
            interface_->GetConnectionRealTimeStatus(id, &status, 0, nullptr);
            char quality_str[32]{};
            if (std::abs(status.m_flConnectionQualityLocal) != 1)
                Sprintf(quality_str, "  %5.2f%% quality", 100 * status.m_flConnectionQualityLocal);
            Printf("%10u  %*s%s  %4dms%s %s%s%s",
                    id, -max_name_length, data.name,
                    print_uuid ? ("  " + get_uuid_string(data.uuid)) : "",
                    status.m_nPing, quality_str,
                    data.cheated ? " [CHEAT]" : "", is_op(id) ? " [OP]" : "",
                    is_muted(data.uuid) ? " [Muted]" : "");
        };
        for (const auto& i: username_) {
            if (bmmo::name_validator::is_spectator(i.first))
                spectators.insert(i);
            else
                print_client(i.second, clients_[i.second]);
        }
        for (const auto& i: spectators)
            print_client(i.second, clients_[i.second]);
        Printf("%d client(s) online: %d player(s), %d spectator(s).",
            clients_.size(), clients_.size() - spectators.size(), spectators.size());
    }

    void print_maps() const {
        std::multimap<decltype(map_names_)::mapped_type, decltype(map_names_)::key_type> map_names_inverted;
        for (const auto& [hash, name]: map_names_) map_names_inverted.emplace(name, hash);
        for (const auto& [name, hash]: map_names_inverted) {
            std::string hash_string;
            bmmo::string_from_hex_chars(hash_string, reinterpret_cast<const uint8_t*>(hash.c_str()), sizeof(bmmo::map::md5));
            Printf("%s: %s", hash_string, name);
        }
    }

    void print_player_maps() {
        for (const auto& [_, id]: username_) {
            const auto& data = clients_[id];
            Printf("%s(#%u, %s) is at the %d%s sector of %s.",
                data.cheated ? "[CHEAT] " : "", id, data.name,
                data.current_sector, bmmo::string_utils::get_ordinal_suffix(data.current_sector),
                data.current_map.get_display_name(map_names_));
        }
    }

    void print_positions() {
        for (const auto& [_, id]: username_) {
            const auto& data = clients_[id];
            Printf("(%u, %s) is at %.2f, %.2f, %.2f with %s ball.",
                    id, data.name,
                    data.state.position.x, data.state.position.y, data.state.position.z,
                    data.state.get_type_name()
            );
        }
    }

    void print_scores(bool hs_mode, bmmo::map map) {
        auto map_it = maps_.find(map.get_hash_bytes_string());
        if (map_it == maps_.end() || (map_it->second.rankings.first.empty() && map_it->second.rankings.second.empty())) {
            Printf(bmmo::ansi::BrightRed, "Error: ranking info not found for the specified map.");
            return;
        }
        auto& ranks = map_it->second.rankings;
        bmmo::ranking_entry::sort_rankings(ranks, hs_mode);
        auto formatted_texts = bmmo::ranking_entry::get_formatted_rankings(
                ranks, map.get_display_name(map_names_), hs_mode);
        for (const auto& line: formatted_texts)
            Printf(line.c_str());
    }

    void print_version_info() const {
        Printf("Server version: %s; minimum accepted client version: %s.",
                        bmmo::current_version.to_string(),
                        bmmo::minimum_client_version.to_string());
        auto uptime = SteamNetworkingUtils()->GetLocalTimestamp() - init_timestamp_;
        std::string time_str(20, 0);
        time_str.resize(std::strftime(&time_str[0], time_str.size(),
            "%F %T", std::localtime(&init_time_t_)));
        Printf("Server uptime: %.2f seconds since %s.",
                        uptime * 1e-6, time_str);
    }

    inline void pull_ball_states(std::vector<bmmo::owned_timed_ball_state>& balls) {
        for (auto& i: clients_) {
            std::unique_lock<std::mutex> lock(client_data_mutex_);
            if (i.second.state.timestamp.is_zero())
                continue;
            balls.emplace_back(i.second.state, i.first);
        }
    }

    inline void pull_unupdated_ball_states(std::vector<bmmo::owned_timed_ball_state>& balls, std::vector<bmmo::owned_timestamp>& unchanged_balls) {
        for (auto& i: clients_) {
            std::unique_lock<std::mutex> lock(client_data_mutex_);
            if (!i.second.state_updated) {
                balls.emplace_back(i.second.state, i.first);
                i.second.state_updated = true;
            }
            if (!i.second.timestamp_updated) {
                unchanged_balls.emplace_back(i.second.state.timestamp, i.first);
                i.second.timestamp_updated = true;
            }
        }
    }

    void set_ban(HSteamNetConnection client, const std::string& reason) {
        if (!client_exists(client))
            return;
        const std::string uuid_string = get_uuid_string(clients_[client].uuid);
        config_.banned_players[uuid_string] = reason;
        Printf(bmmo::color_code(bmmo::OpState), "Banned %s (%s)%s.",
                clients_[client].name, uuid_string, reason.empty() ? "" : ": " + reason);
        kick_client(client, "Banned" + (reason.empty() ? "" : ": " + reason));
        config_.save();
    }

    void set_mute(HSteamNetConnection client, bool action) {
        if (!client_exists(client))
            return;
        const std::string uuid_string = get_uuid_string(clients_[client].uuid);
        if (action) {
            Printf(bmmo::color_code(bmmo::OpState), "Muted %s (%s)%s.",
                clients_[client].name, uuid_string,
                config_.muted_players.insert(uuid_string).second
                ? "" : " (client was already muted previously)");
        } else {
            Printf(bmmo::color_code(bmmo::OpState), "Unmuted %s (%s)%s.",
                clients_[client].name, uuid_string,
                config_.muted_players.erase(uuid_string) != 0
                ? "" : " (client is already not muted)");
        }
        config_.save();
    }

    void set_op(HSteamNetConnection client, bool action) {
        if (!client_exists(client))
            return;
        std::string name = bmmo::name_validator::get_real_nickname(clients_[client].name);
        if (action) {
            if (auto it = config_.op_players.find(name); it != config_.op_players.end()) {
                if (it->second == get_uuid_string(clients_[client].uuid)) {
                    Printf("Error: client \"%s\" already has OP privileges.", name);
                    return;
                }
            }
            config_.op_players[name] = get_uuid_string(clients_[client].uuid);
            Printf(bmmo::color_code(bmmo::OpState), "%s is now an operator.", name);
        } else {
            if (!config_.op_players.erase(name))
                return;
            Printf(bmmo::color_code(bmmo::OpState), "%s is no longer an operator.", name);
        }
        config_.save();
        bmmo::op_state_msg msg{};
        msg.content.op = action;
        send(client, msg, k_nSteamNetworkingSend_Reliable);
    }

    void set_unban(std::string uuid_string) {
        auto it = config_.banned_players.find(uuid_string);
        if (it == config_.banned_players.end()) {
            Printf("Error: %s is not banned.", uuid_string);
            return;
        }
        config_.banned_players.erase(it);
        Printf(bmmo::color_code(bmmo::OpState), "Unbanned %s.", uuid_string);
        config_.save();
    }

    void toggle_cheat(bool cheat) {
        bmmo::cheat_toggle_msg msg;
        msg.content.cheated = (uint8_t)cheat;
        broadcast_message(msg, k_nSteamNetworkingSend_Reliable);
        Printf(bmmo::color_code(bmmo::CheatToggle), "Toggled cheat [%s] globally.", cheat ? "on" : "off");
    }

    void shutdown(int reconnection_delay = 0) {
        Printf("Shutting down...");
        int nReason = reconnection_delay == 0 ? 0 : bmmo::connection_end::AutoReconnection_Min + reconnection_delay;
        for (auto& i: clients_) {
            interface_->CloseConnection(i.first, nReason, "Server closed", true);
        }
        if (ticking_)
            stop_ticking();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        running_ = false;
//        if (server_thread_.joinable())
//            server_thread_.join();
    }

    void wait_till_started() {
        while (!running()) {
            std::unique_lock<std::mutex> lk(startup_mutex_);
            startup_cv_.wait(lk);
        }
    }

    bool setup() override {
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

        bmmo::console::set_completion_callback([this](const std::vector<std::string>& args) -> std::vector<std::string> {
            switch (args.size()) {
                case 0: return {};
                case 1: return bmmo::console::instance->get_command_hints(false, args[0].c_str());
            }
            if (args[0] == "playstream" || (args.size() > 2 && args[0] == "playstream#"))
                return bmmo::string_utils::get_file_matches(args[args.size() - 1]);
            else {
                std::lock_guard lk(client_data_mutex_);
                std::vector<std::string> player_hints;
                player_hints.reserve(clients_.size());
                bool hint_client_id = args[args.size() - 1].starts_with('#');
                for (const auto& [id, data]: clients_) {
                    if (hint_client_id)
                        player_hints.emplace_back('#' + std::to_string(id));
                    else
                        player_hints.emplace_back(data.name);
                }
                return player_hints;
            }
        });

        running_ = true;
        startup_cv_.notify_all();

        Printf(bmmo::ansi::BrightYellow,
                "Server (v%s; client min. v%s) started at port %u.\n",
                bmmo::current_version.to_string(),
                bmmo::minimum_client_version.to_string(), port_);

        return true;
    }

protected:
    void save_login_data(HSteamNetConnection client) {
        SteamNetConnectionInfo_t pInfo;
        interface_->GetConnectionInfo(client, &pInfo);
        config_.save_login_data(pInfo.m_addrRemote, get_uuid_string(clients_[client].uuid), clients_[client].name);
    }

    // Fail silently if the client doesn't exist.
    void cleanup_disconnected_client(HSteamNetConnection client) {
        // Locate the client.  Note that it should have been found, because this
        // is the only codepath where we remove clients (except on shutdown),
        // and connection change callbacks are dispatched in queue order.
        auto itClient = clients_.find(client);
        //assert(itClient != clients_.end()); // It might in limbo state...So may not yet to be found
        if (itClient == clients_.end())
            return;

        bmmo::player_disconnected_msg msg;
        msg.content.connection_id = client;
        broadcast_message(msg, k_nSteamNetworkingSend_Reliable, client);
        std::string name = itClient->second.name;
        {
            std::lock_guard lk(client_data_mutex_);
            username_.erase(bmmo::message_utils::to_lower(name));
            clients_.erase(itClient);
        }
        Printf(bmmo::color_code(msg.code), "%s (#%u) disconnected.", name, client);

        switch (get_client_count()) {
            case 0:
                maps_.clear();
                map_names_ = config_.default_map_names;
                permanent_notification_ = {};
                [[fallthrough]];
            case 1:
                if (ticking_)
                    stop_ticking();
                break;
            default:
                break;
        }
        config_.save_player_status(clients_);
    }

    bool client_exists(HSteamNetConnection client, bool suppress_error = false) const {
        if (client == k_HSteamNetConnection_Invalid)
            return false;
        if (!clients_.contains(client)) {
            if (!suppress_error)
                Printf("Error: client #%u not found.", client);
            return false;
        }
        return true;
    }

    bool deny_action(HSteamNetConnection client) {
        if (config_.op_mode && op_online() && !is_op(client)) {
            bmmo::action_denied_msg denied_msg{.content = {bmmo::deny_reason::NoPermission}};
            send(client, denied_msg, k_nSteamNetworkingSend_Reliable);
            return true;
        }
        return false;
    }

    std::string get_uuid_string(uint8_t* uuid) const {
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

    inline bool is_muted(HSteamNetConnection client) { return is_muted(clients_[client].uuid); }
    inline bool is_muted(uint8_t* uuid) const {
        return config_.muted_players.contains(get_uuid_string(uuid));
    }

    bool is_op(HSteamNetConnection client) {
        if (!client_exists(client))
            return false;
        std::string name = bmmo::name_validator::get_real_nickname(clients_[client].name);
        auto op_it = config_.op_players.find(name);
        if (op_it == config_.op_players.end())
            return false;
        if (op_it->second == get_uuid_string(clients_[client].uuid))
            return true;
        return false;
    }

    bool op_online() {
        for (auto& client : clients_) {
            if (is_op(client.first))
                return true;
        }
        return false;
    }

    bool process_forced_cheat_mode(std::pair<const HSteamNetConnection, client_data>& data, bool new_cheat_mode) {
        bool forced_cheat;
        if (!config_.get_forced_cheat_mode(get_uuid_string(data.second.uuid), forced_cheat)
                || forced_cheat == new_cheat_mode)
            return false;
        send(data.first, bmmo::cheat_toggle_msg{.content = {.cheated = forced_cheat, .notify = false}},
                k_nSteamNetworkingSend_Reliable);
        Printf(bmmo::color_code(bmmo::CheatToggle), "Forcing cheat mode of (#%u, %s) to [%s].",
                data.first, data.second.name, forced_cheat ? "on" : "off");
        return true;
    }

    bool validate_client(HSteamNetConnection client, bmmo::login_request_v3_msg& msg) {
        int nReason = k_ESteamNetConnectionEnd_Invalid;
        std::stringstream reason;
        std::string real_nickname = bmmo::name_validator::get_real_nickname(msg.nickname);

        // check if client is banned
        if (auto it = config_.banned_players.find(get_uuid_string(msg.uuid)); it != config_.banned_players.end()) {
            reason << "You are banned from this server";
            if (!it->second.empty())
                reason << ": " << it->second;
            nReason = bmmo::connection_end::Banned;
        }
        // verify client version
        else if (msg.version < bmmo::minimum_client_version) {
            reason << "Outdated client (client: " << msg.version.to_string()
                    << "; minimum: " << bmmo::minimum_client_version.to_string() << ").";
            nReason = bmmo::connection_end::OutdatedClient;
        }
        // check if name exists
        else if (username_.contains(bmmo::string_utils::to_lower(msg.nickname))) {
            reason << "A player with the same username \"" << msg.nickname << "\" already exists on this server.";
            nReason = bmmo::connection_end::ExistingName;
        }
        // validate nickname length
        else if (!bmmo::name_validator::is_of_valid_length(real_nickname)) {
            reason << "Nickname must be between "
                    << bmmo::name_validator::min_length << " and "
                    << bmmo::name_validator::max_length << " characters in length.";
            nReason = bmmo::connection_end::InvalidNameLength;
        }
        // validate nickname characters
        else if (size_t invalid_pos = bmmo::name_validator::get_invalid_char_pos(real_nickname);
                invalid_pos != std::string::npos) {
            reason << "Invalid character '" << real_nickname[invalid_pos] << "' at position "
                    << invalid_pos << "; nicknames can only contain alphanumeric characters and underscores.";
            nReason = bmmo::connection_end::InvalidNameCharacter;
        }

        if (nReason != k_ESteamNetConnectionEnd_Invalid) {
            bmmo::simple_action_msg new_msg{.content = bmmo::simple_action::LoginDenied};
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
                    // Select appropriate log messages
                    if (config_.logging_level < k_ESteamNetworkingSocketsDebugOutputType_Msg) {
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
                    }

                    cleanup_disconnected_client(pInfo->m_hConn);
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

    void on_message(ISteamNetworkingMessage* networking_msg) override {
        auto client_it = clients_.find(networking_msg->m_conn);
        auto* raw_msg = reinterpret_cast<bmmo::general_message*>(networking_msg->m_pData);

        if (networking_msg->m_cbSize < static_cast<decltype(networking_msg->m_cbSize)>(sizeof(bmmo::opcode))) {
            Printf("Error: invalid message with size %d received from #%u.",
                    networking_msg->m_cbSize, networking_msg->m_conn);
            return;
        }
        if (!(client_it != clients_.end() || raw_msg->code == bmmo::LoginRequest || raw_msg->code == bmmo::LoginRequestV2 || raw_msg->code == bmmo::LoginRequestV3)) { // ignore limbo clients message
            interface_->CloseConnection(networking_msg->m_conn, k_ESteamNetConnectionEnd_AppException_Min, "Invalid client", true);
            return;
        }

        switch (raw_msg->code) {
            case bmmo::LoginRequest: {
                bmmo::simple_action_msg msg{.content = bmmo::simple_action::LoginDenied};
                send(networking_msg->m_conn, msg, k_nSteamNetworkingSend_Reliable);
                interface_->CloseConnection(networking_msg->m_conn, bmmo::connection_end::OutdatedClient, "Outdated client", true);
                break;
            }
            case bmmo::LoginRequestV2: {
                bmmo::login_request_v2_msg msg;
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                interface_->SetConnectionName(networking_msg->m_conn, msg.nickname.c_str());

                std::string reason = "Outdated client (client: " + msg.version.to_string()
                        + "; minimum: " + bmmo::minimum_client_version.to_string() + ")";
                bmmo::simple_action_msg new_msg{.content = bmmo::simple_action::LoginDenied};
                send(networking_msg->m_conn, new_msg, k_nSteamNetworkingSend_Reliable);
                interface_->CloseConnection(networking_msg->m_conn, bmmo::connection_end::OutdatedClient, reason.c_str(), true);
                break;
            }
            case bmmo::LoginRequestV3: {
                bmmo::login_request_v3_msg msg;
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                interface_->SetConnectionName(networking_msg->m_conn, msg.nickname.c_str());

                if (!validate_client(networking_msg->m_conn, msg))
                    break;

                std::string uuid_string = get_uuid_string(msg.uuid);
                if (config_.has_forced_name(uuid_string)) {
                    std::string new_name = config_.get_forced_name(uuid_string);
                    bmmo::name_update_msg nu_msg;
                    nu_msg.text_content = new_name;
                    nu_msg.serialize();
                    send(networking_msg->m_conn, nu_msg.raw.str().data(), nu_msg.size(), k_nSteamNetworkingSend_Reliable);
                    if (bmmo::name_validator::is_spectator(msg.nickname))
                        new_name = bmmo::name_validator::get_spectator_nickname(new_name);
                    Printf("Forced name change - #%u: \"%s\" -> \"%s\"",
                            networking_msg->m_conn, msg.nickname, new_name);
                    interface_->SetConnectionName(networking_msg->m_conn, new_name.c_str());
                    msg.nickname = new_name;
                }

                // accepting client and adding it to the client list
                {
                    std::lock_guard lk(client_data_mutex_);
                    client_it = clients_.insert({networking_msg->m_conn, {msg.nickname, (bool)msg.cheated}}).first;
                }
                memcpy(client_it->second.uuid, msg.uuid, sizeof(msg.uuid));
                client_it->second.login_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                username_[bmmo::message_utils::to_lower(msg.nickname)] = networking_msg->m_conn;
                Printf(bmmo::color_code(bmmo::LoginAcceptedV3),
                        "%s (%s; v%s) logged in with cheat mode %s!\n",
                        msg.nickname,
                        uuid_string.substr(0, 8),
                        msg.version.to_string(),
                        msg.cheated ? "on" : "off");

                if (!map_names_.empty()) { // do this before login_accepted_msg since the latter contains map info
                    bmmo::map_names_msg name_msg;
                    name_msg.maps = map_names_;
                    name_msg.serialize();
                    send(networking_msg->m_conn, name_msg.raw.str().data(), name_msg.size(), k_nSteamNetworkingSend_Reliable);
                }

                // notify this client of other online players
                bmmo::login_accepted_v3_msg accepted_msg;
                accepted_msg.online_players.reserve(clients_.size());
                for (const auto& [id, data]: clients_) {
                    //if (client_it != it)
                    accepted_msg.online_players.insert({id, {data.name, data.cheated, data.current_map, data.current_sector}});
                }
                accepted_msg.serialize();
                send(networking_msg->m_conn, accepted_msg.raw.str().data(), accepted_msg.size(), k_nSteamNetworkingSend_Reliable);

                save_login_data(networking_msg->m_conn);

                // notify other client of the fact that this client goes online
                bmmo::player_connected_v2_msg connected_msg;
                connected_msg.connection_id = networking_msg->m_conn;
                connected_msg.name = msg.nickname;
                connected_msg.cheated = msg.cheated;
                if (process_forced_cheat_mode(*client_it, msg.cheated))
                    connected_msg.cheated = !msg.cheated;
                connected_msg.serialize();
                broadcast_message(connected_msg.raw.str().data(), connected_msg.size(), k_nSteamNetworkingSend_Reliable, networking_msg->m_conn);

                bmmo::owned_compressed_ball_state_msg state_msg{};
                pull_ball_states(state_msg.balls);
                state_msg.serialize();
                send(networking_msg->m_conn, state_msg.raw.str().data(), state_msg.size(), k_nSteamNetworkingSend_ReliableNoNagle);

                if (!permanent_notification_.second.empty()) {
                    bmmo::permanent_notification_msg bulletin_msg{};
                    std::tie(bulletin_msg.title, bulletin_msg.text_content) = permanent_notification_;
                    bulletin_msg.serialize();
                    send(networking_msg->m_conn, bulletin_msg.raw.str().data(), bulletin_msg.size(), k_nSteamNetworkingSend_Reliable);
                }

                if (!ticking_ && get_client_count() > 1)
                    start_ticking();
                config_.save_player_status(clients_);

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
                client_it->second.state = {state_msg->content, networking_msg->m_usecTimeReceived};
                client_it->second.state_updated = false;

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
            case bmmo::TimedBallState: {
                auto* state_msg = reinterpret_cast<bmmo::timed_ball_state_msg*>(networking_msg->m_pData);
                std::unique_lock<std::mutex> lock(client_data_mutex_);
                if (state_msg->content.timestamp < client_it->second.state.timestamp)
                    break;
                client_it->second.state = state_msg->content;
                client_it->second.state_updated = false;
                break;
            }
            case bmmo::Timestamp: {
                auto* timestamp_msg = reinterpret_cast<bmmo::timestamp_msg*>(networking_msg->m_pData);
                std::unique_lock<std::mutex> lock(client_data_mutex_);
                if (timestamp_msg->content < client_it->second.state.timestamp)
                    break;
                client_it->second.state.timestamp = timestamp_msg->content;
                client_it->second.timestamp_updated = false;
                break;
            }
            case bmmo::Chat: {
                bmmo::chat_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                bmmo::string_utils::sanitize_string(msg.chat_content);
                const bool muted = is_muted(client_it->second.uuid);

                // Print chat message to console
                const std::string& current_player_name = client_it->second.name;
                const HSteamNetConnection current_player_id  = networking_msg->m_conn;
                Printf(muted ? bmmo::ansi::Strikethrough : bmmo::ansi::Reset, "%s(%u, %s): %s",
                        muted ? "[Muted] " : "", current_player_id, current_player_name, msg.chat_content);

                if (muted) {
                    bmmo::action_denied_msg msg{.content = {bmmo::deny_reason::PlayerMuted}};
                    send(networking_msg->m_conn, msg, k_nSteamNetworkingSend_Reliable);
                    break;
                }

                // Broatcast chat message to other player
                msg.player_id = current_player_id;
                msg.clear();
                msg.serialize();

                // No need to ignore the sender, 'cause we will send the message back
                broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);

                break;
            }
            case bmmo::PrivateChat: {
                bmmo::private_chat_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                bmmo::string_utils::sanitize_string(msg.chat_content);
                const HSteamNetConnection receiver = msg.player_id;
                msg.player_id = networking_msg->m_conn;

                if (client_exists(receiver, true)) {
                    Printf(bmmo::color_code(msg.code), "(%u, %s) -> (%u, %s): %s",
                        msg.player_id, client_it->second.name, receiver, clients_[receiver].name, msg.chat_content);
                    msg.clear();
                    msg.serialize();
                    send(receiver, msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
                } else {
                    Printf(bmmo::color_code(msg.code), "(%u, %s) -> (%u, %s): %s",
                        msg.player_id, client_it->second.name, receiver, "[Server]", msg.chat_content);
                    if (receiver != k_HSteamNetConnection_Invalid) {
                        bmmo::action_denied_msg denied_msg{.content = {bmmo::deny_reason::TargetNotFound}};
                        send(msg.player_id, denied_msg, k_nSteamNetworkingSend_Reliable);
                    }
                };
                break;
            }
            case bmmo::ImportantNotification: {
                if (deny_action(networking_msg->m_conn))
                    break;
                bmmo::important_notification_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                bmmo::string_utils::sanitize_string(msg.chat_content);
                msg.player_id = networking_msg->m_conn;
                const bool muted = is_muted(client_it->second.uuid);

                Printf(msg.get_ansi_color() | (muted ? bmmo::ansi::Strikethrough : bmmo::ansi::Reset),
                    "%s[%s] (%u, %s): %s", muted ? "[Muted] " : "",
                    msg.get_type_name(), msg.player_id, client_it->second.name, msg.chat_content);
                if (muted) break;
                msg.clear();
                msg.serialize();
                broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
                break;
            }
            case bmmo::PlayerReady: {
                auto* msg = reinterpret_cast<bmmo::player_ready_msg*>(networking_msg->m_pData);
                msg->content.player_id = networking_msg->m_conn;
                client_it->second.ready = msg->content.ready;
                msg->content.count = std::count_if(clients_.begin(), clients_.end(),
                    [](const auto& i) { return i.second.ready; });
                Printf("(#%u, %s) is%s ready to start (%u player%s ready).",
                    networking_msg->m_conn, client_it->second.name,
                    msg->content.ready ? "" : " not",
                    msg->content.count, msg->content.count == 1 ? "" : "s");
                broadcast_message(*msg, k_nSteamNetworkingSend_Reliable);
                break;
            }
            case bmmo::Countdown: {
                if (deny_action(networking_msg->m_conn))
                    break;
                auto* msg = reinterpret_cast<bmmo::countdown_msg*>(networking_msg->m_pData);

                std::string map_name = msg->content.map.get_display_name(map_names_);
                last_countdown_map_ = msg->content.map;
                switch (msg->content.type) {
                    using ct = bmmo::countdown_type;
                    case ct::Go: {
                        Printf(bmmo::color_code(msg->code), "[%u, %s]: %s%s - Go!%s",
                            networking_msg->m_conn, client_it->second.name, map_name,
                            msg->content.get_level_mode_label(),
                            msg->content.force_restart ? " (rank reset)" : "");
                        if (config_.force_restart_level || msg->content.force_restart) {
                            maps_.clear();
                            for (const auto& map: map_names_)
                                maps_[map.first] = {0, networking_msg->m_usecTimeReceived, msg->content.mode, {}};
                        } else {
                            maps_[msg->content.map.get_hash_bytes_string()] = {0, networking_msg->m_usecTimeReceived, msg->content.mode, {}};
                        }
                        msg->content.restart_level = config_.restart_level;
                        msg->content.force_restart = config_.force_restart_level;
                        for (auto& i: clients_) {
                            i.second.ready = i.second.dnf = false;
                        }
                        break;
                    }
                    case ct::Countdown_1:
                    case ct::Countdown_2:
                    case ct::Countdown_3:
                    case ct::Ready:
                    case ct::ConfirmReady:
                        Printf("[%u, %s]: %s%s - %s",
                            networking_msg->m_conn, client_it->second.name, map_name,
                            msg->content.get_level_mode_label(),
                            std::map<ct, std::string>{
                                {ct::Countdown_1, "1"}, {ct::Countdown_2, "2"}, {ct::Countdown_3, "3"},
                                {ct::Ready, "Get ready"},
                                {ct::ConfirmReady, "Please use \"/mmo ready\" to confirm if you are ready"},
                            }[msg->content.type]);
                        maps_[msg->content.map.get_hash_bytes_string()].mode = msg->content.mode;
                        break;
                    case ct::Unknown:
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
                std::string& player_name = client_it->second.name;
                Printf(
                    "%s(#%u, %s) did not finish %s (furthest reach: sector %d).",
                    msg->content.cheated ? "[CHEAT] " : "",
                    msg->content.player_id, player_name,
                    msg->content.map.get_display_name(map_names_),
                    msg->content.sector
                );
                client_it->second.dnf = true;
                broadcast_message(*msg, k_nSteamNetworkingSend_Reliable);
                maps_[msg->content.map.get_hash_bytes_string()].rankings.second.push_back({
                    (bool)msg->content.cheated, player_name, msg->content.sector});
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
                std::string md5_str = msg->content.map.get_hash_bytes_string(),
                    & player_name = client_it->second.name,
                    formatted_score = msg->content.get_formatted_score();
                auto& current_map = maps_[md5_str];

                // Use server-side timing if available and under 2.5 hours
                auto local_time_elapsed = networking_msg->m_usecTimeReceived - current_map.start_time;
                if (current_map.start_time != 0 && local_time_elapsed < int64_t(2.5 * 3600 * 1e6))
                    msg->content.timeElapsed = local_time_elapsed / 1e6f;

                // Prepare message
                msg->content.rank = ++current_map.rank;
                Printf("%s(#%u, %s) finished %s%s in %d%s place (score: %s; real time: %s).",
                    msg->content.cheated ? "[CHEAT] " : "",
                    msg->content.player_id, player_name,
                    msg->content.map.get_display_name(map_names_), get_level_mode_label(msg->content.mode),
                    current_map.rank, bmmo::string_utils::get_ordinal_suffix(current_map.rank),
                    formatted_score, msg->content.get_formatted_time());

                broadcast_message(*msg, k_nSteamNetworkingSend_Reliable);

                current_map.rankings.first.push_back({
                    (bool)msg->content.cheated, player_name, msg->content.mode,
                    current_map.rank, msg->content.timeElapsed, formatted_score});

                break;
            }
            case bmmo::MapNames: {
                bmmo::map_names_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                map_names_.insert(msg.maps.begin(), msg.maps.end());

                msg.clear();
                msg.serialize();
                broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable, networking_msg->m_conn);
                break;
            }
            case bmmo::CheatState: {
                auto* state_msg = reinterpret_cast<bmmo::cheat_state_msg*>(networking_msg->m_pData);
                if (process_forced_cheat_mode(*client_it, state_msg->content.cheated))
                    return;

                client_it->second.cheated = state_msg->content.cheated;
                Printf("(#%u, %s) turned cheat [%s]!",
                    networking_msg->m_conn, client_it->second.name, state_msg->content.cheated ? "on" : "off");
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
                Printf(bmmo::color_code(state_msg->code), "(#%u, %s) toggled cheat [%s] globally!",
                    networking_msg->m_conn, client_it->second.name, state_msg->content.cheated ? "on" : "off");
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
                    Printf(bmmo::color_code(msg.code), "%s requested to kick player \"%s\"!",
                            client_it->second.name, msg.player_name);
                    player_id = get_client_id(msg.player_name);
                } else {
                    Printf(bmmo::color_code(msg.code), "%s requested to kick player #%u!",
                            client_it->second.name, msg.player_id);
                }
                if (deny_action(networking_msg->m_conn))
                    break;

                bmmo::string_utils::sanitize_string(msg.reason);

                if (!kick_client(player_id, msg.reason, client_it->first,
                        msg.crash ? bmmo::connection_end::Crash : bmmo::connection_end::Kicked)) {
                    bmmo::action_denied_msg new_msg{};
                    new_msg.content.reason = bmmo::deny_reason::TargetNotFound;
                    send(networking_msg->m_conn, new_msg, k_nSteamNetworkingSend_Reliable);
                };

                break;
            }
            case bmmo::CurrentMap: {
                auto* msg = reinterpret_cast<bmmo::current_map_msg*>(networking_msg->m_pData);
                msg->content.player_id = networking_msg->m_conn;
                switch (msg->content.type) {
                    case bmmo::current_map_state::Announcement: {
                        broadcast_message(*msg, k_nSteamNetworkingSend_Reliable);
                        Printf(bmmo::color_code(msg->code), "%s(#%u, %s) is at the %d%s sector of %s.",
                            client_it->second.cheated ? "[CHEAT] " : "",
                            networking_msg->m_conn, client_it->second.name,
                            msg->content.sector, bmmo::string_utils::get_ordinal_suffix(msg->content.sector),
                            msg->content.map.get_display_name(map_names_));
                        break;
                    }
                    case bmmo::current_map_state::EnteringMap: {
                        auto client_map_it = maps_.find(msg->content.map.get_hash_bytes_string());
                        if (client_map_it != maps_.end() && client_map_it->second.mode == bmmo::level_mode::Highscore) {
                            bmmo::highscore_timer_calibration_msg hs_msg{.content = {
                                .map = msg->content.map,
                                .time_diff_microseconds = SteamNetworkingUtils()->GetLocalTimestamp() - client_map_it->second.start_time,
                            }};
                            send(networking_msg->m_conn, hs_msg, k_nSteamNetworkingSend_Reliable);
                        }
                        [[fallthrough]];
                    }
                    case bmmo::current_map_state::NameChange: {
                        client_it->second.current_map = msg->content.map;
                        client_it->second.current_sector = msg->content.sector;
                        broadcast_message(*msg, k_nSteamNetworkingSend_Reliable, networking_msg->m_conn);
                        break;
                    }
                    default: break;
                }
                break;
            }
            case bmmo::CurrentSector: {
                auto* msg = reinterpret_cast<bmmo::current_sector_msg*>(networking_msg->m_pData);
                msg->content.player_id = networking_msg->m_conn;
                client_it->second.current_sector = msg->content.sector;
                broadcast_message(*msg, k_nSteamNetworkingSend_Reliable, networking_msg->m_conn);
                break;
            }
            case bmmo::SimpleAction: {
                auto* msg = reinterpret_cast<bmmo::simple_action_msg*>(networking_msg->m_pData);
                switch (msg->content) {
                    using sa = bmmo::simple_action;
                    case sa::CurrentMapQuery: {
                        break;
                    }
                    case sa::FatalError: {
                        Printf("(#%u, %s) has encountered a fatal error!",
                            networking_msg->m_conn, client_it->second.name);
                        // they already got their own fatal error, so we don't need to induce one here.
                        kick_client(networking_msg->m_conn, "fatal error", k_HSteamNetConnection_Invalid, bmmo::connection_end::SelfTriggeredFatalError);
                        break;
                    }
                    case sa::BallOff: {
                        if (!config_.log_ball_offs)
                            break;
                        char text[256];
                        std::snprintf(text, sizeof(text), "(#%u, %s) just fell at sector %d of %s.",
                                networking_msg->m_conn, client_it->second.name.c_str(),
                                client_it->second.current_sector,
                                client_it->second.current_map.get_display_name(map_names_).c_str());
                        LogFileOutput(text);
                        break;
                    }
                    default:
                        break;
                }
                break;
            }
            case bmmo::PermanentNotification: {
                if (deny_action(networking_msg->m_conn))
                    break;
                auto msg = bmmo::message_utils::deserialize<bmmo::permanent_notification_msg>(networking_msg);
                bmmo::string_utils::sanitize_string(msg.text_content);
                const bool muted = is_muted(client_it->second.uuid);

                Printf(bmmo::color_code(msg.code) | (muted ? bmmo::ansi::Strikethrough : bmmo::ansi::Reset),
                        "%s[Bulletin] %s%s", muted ? "[Muted] " : "", client_it->second.name,
                        msg.text_content.empty() ? " - Content cleared" : ": " + msg.text_content);
                if (muted) break;
                msg.title = client_it->second.name;
                permanent_notification_ = {msg.title, msg.text_content};
                msg.clear();
                msg.serialize();
                broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
                break;
            }
            case bmmo::PlainText: {
                bmmo::plain_text_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();
                Printf("[Plain] (%u, %s): %s", networking_msg->m_conn, client_it->second.name, msg.text_content);
                break;
            }
            case bmmo::PublicNotification: {
                auto msg = bmmo::message_utils::deserialize<bmmo::public_notification_msg>(networking_msg);
                Printf(msg.get_ansi_color_code(), "[%s] (%u, %s): %s",
                        msg.get_type_name(), networking_msg->m_conn, client_it->second.name, msg.text_content);
                broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
                if (msg.type == bmmo::public_notification_type::SeriousWarning
                        && config_.serious_warning_as_dnf && !client_it->second.dnf) {
                    auto client_map_it = maps_.find(client_it->second.current_map.get_hash_bytes_string());
                    if (client_map_it == maps_.end() || SteamNetworkingUtils()->GetLocalTimestamp()
                            - maps_[client_it->second.current_map.get_hash_bytes_string()].start_time > 20ll * 60 * 1000000)
                        break;
                    bmmo::did_not_finish_msg dnf_msg{.content = {
                        .cheated = client_it->second.cheated,
                        .map = client_it->second.current_map,
                        .sector = client_it->second.current_sector,
                    }};
                    receive(&dnf_msg, sizeof(dnf_msg), networking_msg->m_conn);
                }
                break;
            }
            case bmmo::RestartRequest: {
                if (deny_action(networking_msg->m_conn))
                    break;
                auto msg = bmmo::message_utils::deserialize<bmmo::restart_request_msg>(networking_msg);
                if (!client_exists(msg.content.victim, true)) {
                    Printf(bmmo::ansi::Italic, "(#%u, %s) requested to restart #%u's (not found) current level!",
                        networking_msg->m_conn, client_it->second.name,
                        msg.content.victim);
                    send(networking_msg->m_conn, bmmo::action_denied_msg{.content = {bmmo::deny_reason::TargetNotFound}});
                    break;
                }
                Printf(bmmo::ansi::Italic, "(#%u, %s) requested to restart (#%u, %s)'s current level!",
                    networking_msg->m_conn, client_it->second.name,
                    msg.content.victim, clients_[msg.content.victim].name);
                msg.content.requester = networking_msg->m_conn;
                broadcast_message(msg);
                break;
            }
            case bmmo::ScoreList: {
                auto msg = bmmo::message_utils::deserialize<bmmo::score_list_msg>(networking_msg);
                auto* rankings = get_map_rankings(msg.map);
                Printf(bmmo::color_code(msg.code), "(%u, %s) queried the score list of %s%s.",
                        networking_msg->m_conn, client_it->second.name,
                        msg.map.get_display_name(map_names_),
                        rankings ? "" : " [Not found]");
                msg.clear();
                if (!rankings) {
                    send(networking_msg->m_conn, bmmo::action_denied_msg{.content = {bmmo::deny_reason::TargetNotFound}},
                            k_nSteamNetworkingSend_Reliable);
                    break;
                }
                msg.serialize(*rankings);
                send(networking_msg->m_conn, msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
                break;
            }
            case bmmo::HashData: {
                bmmo::hash_data_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                for (const auto* file_data: bmmo::HASHES_TO_CHECK) {
                    if (!msg.data.contains(file_data[0]) || msg.is_same_data(file_data[0], file_data[1]))
                        continue;
                    bmmo::public_notification_msg new_msg{};
                    new_msg.type = bmmo::public_notification_type::Warning;
                    std::string file_name{file_data[0]}, md5_string;
                    const auto& md5 = msg.data[file_name];
                    bmmo::string_from_hex_chars(md5_string, md5.data(), md5.size());
                    if (auto slash_pos = file_name.rfind('\\'); slash_pos != std::string::npos)
                        file_name.erase(0, slash_pos + 1);
                    new_msg.text_content = client_it->second.name + " has a modified " + file_name + " (MD5 " + md5_string.substr(0, 12) + "..)! This could be problematic.";
                    Printf("[%s] %s", new_msg.get_type_name(), new_msg.text_content);
                    new_msg.serialize();
                    broadcast_message(new_msg.raw.str().data(), new_msg.size(), k_nSteamNetworkingSend_Reliable);
                }
                break;
            }
            case bmmo::ModList: { // TODO: configurable mod blacklist/whitelist handling
                auto msg = bmmo::message_utils::deserialize<bmmo::mod_list_msg>(networking_msg);
                if (config_.log_installed_mods)
                    config_.log_mod_list(msg.mods);
                break;
            }
            case bmmo::SoundData:
            case bmmo::SoundStream:
            case bmmo::OwnedBallState:
            case bmmo::OwnedBallStateV2:
            case bmmo::OwnedTimedBallState:
            case bmmo::OwnedCompressedBallState:
            case bmmo::LoginAcceptedV2:
            case bmmo::LoginAcceptedV3:
            case bmmo::PlayerConnectedV2:
            case bmmo::HighscoreTimerCalibration:
            case bmmo::NameUpdate:
            case bmmo::OwnedCheatState:
            case bmmo::OwnedCheatToggle:
            case bmmo::PlayerKicked:
            case bmmo::ActionDenied:
            case bmmo::OpState:
            case bmmo::KeyboardInput:
                break;
            default:
                Printf("Error: invalid message with opcode %d received from #%u.",
                        raw_msg->code, networking_msg->m_conn);
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
        bmmo::owned_compressed_ball_state_msg msg{};
        pull_unupdated_ball_states(msg.balls, msg.unchanged_balls);
        if (msg.balls.empty() && msg.unchanged_balls.empty())
            return;
        msg.serialize();
        broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_UnreliableNoDelay);
    };

    void start_ticking() {
        ticking_ = true;
        // ticking_thread_ = std::thread([&]() {
        Printf("Ticking started.");
        //     while (ticking_) {
        //         auto next_tick = std::chrono::steady_clock::now() + TICK_INTERVAL;
        //         tick();
        //         std::this_thread::sleep_until(next_tick);
    }
    void stop_ticking() {
        ticking_ = false;
        // ticking_thread_.join();
        Printf("Ticking stopped.");
    }

    uint16_t port_ = 0;
    HSteamListenSocket listen_socket_ = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup poll_group_ = k_HSteamNetPollGroup_Invalid;
//    std::thread server_thread_;
    client_data_collection clients_;
    std::map<std::string, HSteamNetConnection> username_; // Note: this stores names converted to all-lowercases
    std::mutex client_data_mutex_;
    std::pair<std::string, std::string> permanent_notification_; // <username (title), text>

    std::mutex startup_mutex_;
    std::condition_variable startup_cv_;

    // std::thread ticking_thread_;
    std::atomic_bool ticking_ = false;
    map_data_collection maps_;
    bmmo::map last_countdown_map_{};

    config_manager config_;

    std::unordered_map<std::string, std::string> map_names_;
};

// parse arguments (optional port and help/version/log) with getopt
int parse_args(int argc, char** argv, uint16_t& port, std::string& log_path, bool& dry_run) {
    enum option_values { DryRun = UINT8_MAX + 1 };
    static struct option long_options[] = {
        {"port", required_argument, 0, 'p'},
        {"log", required_argument, 0, 'l'},
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'v'},
        {"dry-run", no_argument, 0, DryRun},
        {0, 0, 0, 0}
    };
    int opt, opt_index = 0;
    while ((opt = getopt_long(argc, argv, "p:l:hv", long_options, &opt_index)) != -1) {
        switch (opt) {
            case 'p':
                port = atoi(optarg);
                break;
            case 'l':
                log_path = optarg;
                break;
            case 'h':
                printf("Usage: %s [OPTION]...\n", argv[0]);
                puts("Options:");
                puts("  -p, --port=PORT\t Use PORT as the server port instead (default: 26676).");
                puts("  -l, --log=PATH\t Write log to the file at PATH in addition to stdout.");
                puts("  -h, --help\t\t Display this help and exit.");
                puts("  -v, --version\t\t Display version information and exit.");
                puts("      --dry-run\t\t Test the server by starting it and exiting immediately.");
                return -1;
            case 'v':
                puts("Ballance MMO server by Swung0x48 and BallanceBug.");
                printf("Build time: \t%s.\n", bmmo::string_utils::get_build_time_string().c_str());
                printf("Version: \t%s.\n", bmmo::current_version.to_string().c_str());
                printf("Minimum accepted client version: %s.\n", bmmo::minimum_client_version.to_string().c_str());
                puts("GitHub repository: https://github.com/Swung0x48/BallanceMMO");
                return -1;
            case DryRun:
                dry_run = true;
                break;
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    uint16_t port = 26676;
    bool dry_run = false;
    std::string log_path;
    if (parse_args(argc, argv, port, log_path, dry_run) < 0)
        return 0;

    if (port == 0) {
        std::cerr << "Fatal: invalid port number." << std::endl;
        return 1;
    };

    if (!log_path.empty()) {
        FILE* log_file = fopen(log_path.c_str(), "a");
        if (log_file == nullptr) {
            std::cerr << "Fatal: failed to open the log file." << std::endl;
            return 1;
        }
        server::set_log_file(log_file);
    }

    printf("Initializing sockets...\n");
    server::init_socket();

    printf("Starting server at port %u.\n", port);
    server server(port);

    printf("Bootstrapping server...\n");
    fflush(stdout);
    if (!server.setup())
        server::FatalError("Server failed on setup.");
    std::thread server_thread([&server]() { server.run(); });

    bmmo::console console;
    console.register_command("stop", [&] { server.shutdown(console.get_next_int()); });
    console.register_command("list", [&] { server.print_clients(); });
    console.register_command("list-uuid", [&] { server.print_clients(true); });
    console.register_command("say", [&] {
        bmmo::chat_msg msg{};
        msg.chat_content = console.get_rest_of_line();
        msg.serialize();

        server.broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        server.Printf("([Server]): %s", msg.chat_content);
    });
    auto get_client_id_from_console = [&]() -> HSteamNetConnection {
        std::string client_input = console.get_next_word();
        HSteamNetConnection client = (client_input.length() > 0 && client_input[0] == '#')
                ? atoll(client_input.substr(1).c_str()) : server.get_client_id(client_input);
        if (client == 0)
            server.Printf("Error: invalid connection id.");
        return client;
    };
    auto send_plain_text_msg = [&](bool broadcast = true) {
        HSteamNetConnection client = broadcast ? k_HSteamNetConnection_Invalid : get_client_id_from_console();
        if (!broadcast && client == k_HSteamNetConnection_Invalid) return;
        bmmo::plain_text_msg msg{};
        msg.text_content = console.get_rest_of_line();
        msg.serialize();
        if (broadcast) {
            server.broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
            server.Printf("[Plain]: %s", msg.text_content);
        } else {
            server.send(client, msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
            server.Printf("[Plain -> #%u]: %s", client, msg.text_content);
        }
    };
    console.register_command("plaintext", send_plain_text_msg);
    console.register_command("plaintext#", std::bind(send_plain_text_msg, false));
    auto send_popup_msg = [&](bool broadcast = true) {
        HSteamNetConnection client = broadcast ? k_HSteamNetConnection_Invalid : get_client_id_from_console();
        if (!broadcast && client == k_HSteamNetConnection_Invalid) return;
        bmmo::popup_box_msg msg{};
        msg.title = "BallanceMMO - Message";
        msg.text_content = console.get_rest_of_line();
        try {
            YAML::Node idata = YAML::Load(msg.text_content);
            switch (idata.Type()) {
                case YAML::NodeType::Map:
                    std::tie(msg.title, msg.text_content) =
                        *idata.as<std::unordered_map<std::string, std::string>>().begin();
                    break;
                case YAML::NodeType::Scalar:
                    msg.text_content = idata.as<std::string>();
                    break;
                default:
                    std::stringstream ss; ss << idata;
                    msg.text_content = ss.str();
            }
        } catch (const std::exception& e) {
            server.Printf(e.what());
            return;
        }
        msg.serialize();
        if (broadcast)
            server.broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        else
            server.send(client, msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        server.Printf(bmmo::color_code(msg.code), "[Popup -> %s] {%s}: %s",
                client == k_HSteamNetConnection_Invalid ? "[all]" : std::to_string(client),
                msg.title, msg.text_content);
    };
    console.register_command("popup", send_popup_msg);
    console.register_command("popup#", std::bind(send_popup_msg, false));
    using in_msg = bmmo::important_notification_msg;
    auto send_important_notification = [&](bool broadcast = true, in_msg::notification_type type = in_msg::Announcement) {
        bmmo::important_notification_msg msg{};
        HSteamNetConnection client = broadcast ? k_HSteamNetConnection_Invalid : get_client_id_from_console();
        if (!broadcast && client == k_HSteamNetConnection_Invalid) return;
        msg.chat_content = console.get_rest_of_line();
        msg.type = type;
        msg.serialize();
        if (broadcast)
            server.broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        else
            server.send(client, msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        server.Printf(msg.get_ansi_color(), "[%s] ([Server])%s: %s",
                msg.get_type_name(), broadcast ? "" : " -> #" + std::to_string(client),
                msg.chat_content);
    };
    console.register_command("announce", send_important_notification);
    console.register_command("announce#", std::bind(send_important_notification, false));
    console.register_command("notice", std::bind(send_important_notification, true, in_msg::Notice));
    console.register_command("notice#", std::bind(send_important_notification, false, in_msg::Notice));
    console.register_command("cheat", [&] {
        bool cheat_state = (console.get_next_word(true) == "on");
        server.toggle_cheat(cheat_state);
    });
    console.register_command("version", [&] { server.print_version_info(); });
    console.register_aliases("version", {"ver"});
    console.register_command("getmap", [&] { server.print_player_maps(); });
    console.register_command("getpos", [&] { server.print_positions(); });
    console.register_command("kick", [&] {
        if (console.empty() && console.get_command_name() == "kick#") {
            server.Printf("Usage: \"kick# <code> <player> <reason>\".");
            return;
        }
        bmmo::connection_end::code end_code = bmmo::connection_end::Kicked;
        if (console.get_command_name() == "kick#")
            end_code = static_cast<decltype(end_code)>(console.get_next_int());
        else if (console.get_command_name() == "crash")
            end_code = bmmo::connection_end::Crash;
        else if (console.get_command_name() == "fatalerror")
            end_code = bmmo::connection_end::FatalError;
        auto client = get_client_id_from_console();
        if (client == k_HSteamNetConnection_Invalid) return;
        std::string text = console.get_rest_of_line();
        server.kick_client(client, text, k_HSteamNetConnection_Invalid, end_code);
    });
    console.register_aliases("kick", {"crash", "fatalerror", "kick#"});
    console.register_command("whisper", [&] {
        auto client = get_client_id_from_console();
        if (client == k_HSteamNetConnection_Invalid) return;
        std::string text = console.get_rest_of_line();
        bmmo::private_chat_msg msg{};
        msg.chat_content = text;
        msg.serialize();
        server.send(client, msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        server.Printf(bmmo::color_code(msg.code), "([Server]) -> #%u: %s", client, msg.chat_content);
    });
    console.register_command("ban", [&] {
        auto client = get_client_id_from_console();
        std::string text = console.get_rest_of_line();
        server.set_ban(client, text);
    });
    console.register_command("op", [&] {
        auto client = get_client_id_from_console();
        if (client == k_HSteamNetConnection_Invalid) return;
        auto cmd = console.get_command_name();
        bool action = (cmd == "op" || cmd == "mute");
        if (cmd == "op" || cmd == "deop")
            server.set_op(client, action);
        else
            server.set_mute(client, action);
    });
    console.register_aliases("op", {"deop", "mute", "unmute"});
    console.register_command("listban", [&] { server.get_config().print_bans(); });
    console.register_command("listmute", [&] { server.get_config().print_mutes(); });
    console.register_command("unban", [&] { server.set_unban(console.get_next_word()); });
    console.register_command("reload", [&] {
        if (!server.load_config())
            server.Printf("Error: failed to reload config.");
    });
    console.register_command("listmap", [&] { server.print_maps(); });
    console.register_command("countdown", [&] {
        auto print_hint = [] {
            role::Printf("Error: please specify the map to countdown (hint: use \"getmap\" and \"listmap\").");
            role::Printf("Usage: \"countdown <client id> level|<hash> <level number> [mode] [type]\".");
            role::Printf("<type>: {\"4\": \"Get ready\", \"5\": \"Confirm ready\", \"\": \"auto countdown\"}");
        };
        if (console.empty()) { print_hint(); return; }
        auto client = get_client_id_from_console();
        if (console.empty()) { print_hint(); return; }
        std::string hash = console.get_next_word(true);
        if (console.empty()) { print_hint(); return; }
        bmmo::map map{.type = bmmo::map_type::OriginalLevel, .level = std::clamp(console.get_next_int(), 0, 13)};
        if (hash == "level")
            bmmo::hex_chars_from_string(map.md5, bmmo::map::original_map_hashes[map.level]);
        else
            bmmo::hex_chars_from_string(map.md5, hash);
        bmmo::countdown_msg msg{.content = {.map = map}};
        if (!console.empty() && console.get_next_word(true) == "hs")
            msg.content.mode = bmmo::level_mode::Highscore;
        if (console.empty()) {
            for (int i = 3; i >= 0; --i) {
                msg.content.type = static_cast<bmmo::countdown_type>(i);
                server.receive(&msg, sizeof(msg), client);
                if (i != 0) std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        } else {
            msg.content.type = static_cast<bmmo::countdown_type>(console.get_next_int());
            server.receive(&msg, sizeof(msg), client);
        }
    });
    console.register_command("countdown-forced", [&] {
        bmmo::countdown_msg msg{};
        msg.content.restart_level = msg.content.force_restart = true;
        if (!console.empty() && console.get_next_word(true) == "hs")
            msg.content.mode = bmmo::level_mode::Highscore;
        msg.content.map.type = bmmo::map_type::OriginalLevel;
        for (int i = 3; i >= 0; --i) {
            msg.content.type = static_cast<bmmo::countdown_type>(i);
            server.broadcast_message(msg, k_nSteamNetworkingSend_Reliable);
            server.Printf(bmmo::color_code(msg.code), "[[Server]]: Countdown%s - %s",
                msg.content.mode == bmmo::level_mode::Highscore ? " <HS>" : "",
                i == 0 ? "Go!" : std::to_string(i));
            if (i != 0)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });
    console.register_command("bulletin", [&] {
        auto& bulletin = server.get_bulletin();
        if (console.get_command_name() == "bulletin") {
            bulletin = {"[Server]", console.get_rest_of_line()};
            bmmo::permanent_notification_msg msg{};
            std::tie(msg.title, msg.text_content) = bulletin;
            msg.serialize();
            server.broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        }
        server.Printf(bmmo::color_code(bmmo::PermanentNotification), "[Bulletin] %s%s", bulletin.first,
            bulletin.second.empty() ? " - Empty" : ": " + bulletin.second);
    });
    console.register_aliases("bulletin", {"getbulletin"});
    console.register_command("playsound", [&] {
        try {
            auto sounds = YAML::Load(console.get_rest_of_line());
            bmmo::sound_data_msg msg{};
            if (sounds.IsSequence() && sounds.begin() != sounds.end() && sounds.begin()->IsSequence()) {
                msg.sounds = sounds.as<decltype(msg.sounds)>();
            } else if (sounds.IsMap() && sounds.begin() != sounds.end() && sounds.begin()->second.IsSequence() && sounds.begin()->second.begin() != sounds.begin()->second.end() && sounds.begin()->second.begin()->IsSequence()) {
                msg.caption = sounds.begin()->first.as<decltype(msg.caption)>();
                msg.sounds = sounds.begin()->second.as<decltype(msg.sounds)>();
            } else {
                server.Printf("Usage: playsound <caption>: [[frequency, duration], [freq2, dur2]] (caption can be omitted).");
                return;
            }
            std::stringstream temp; temp << sounds;
            server.Printf(bmmo::ansi::WhiteInverse, "Playing sound - %s", temp.str());
            msg.serialize();
            server.broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        } catch (const std::exception& e) { server.Printf(e.what()); return; }
    });
    console.register_command("playstream", [&] {
        auto client = k_HSteamNetConnection_Invalid;
        if (console.get_command_name() == "playstream#") {
            client = get_client_id_from_console();
            if (client == k_HSteamNetConnection_Invalid) return;
        }
        const bool broadcast = (client == k_HSteamNetConnection_Invalid);
        try {
            auto idata = YAML::Load(console.get_rest_of_line());
            bmmo::sound_stream_msg msg{};
            switch (idata.Type()) {
                case YAML::NodeType::Map:
                    if (idata["caption"]) msg.caption = idata["caption"].as<std::string>();
                    if (idata["path"]) msg.path = idata["path"].as<std::string>();
                    if (idata["duration"]) msg.duration_ms = idata["duration"].as<decltype(msg.duration_ms)>();
                    if (idata["gain"]) msg.gain = idata["gain"].as<decltype(msg.gain)>();
                    if (idata["pitch"]) msg.pitch = idata["pitch"].as<decltype(msg.pitch)>();
                    break;
                case YAML::NodeType::Scalar:
                    msg.path = idata.as<std::string>();
                    break;
                default:
                    server.Printf("Usage: playstream {caption: <name>, path: <path>, duration: <duration_ms = 0>, gain: <1.0 ∈ [0, 1]>, pitch: <1.0 ∈ [0.1, 4.1]>}");
                    server.Printf("Usage: playstream <path>");
                    server.Printf("Current working directory: %s", std::filesystem::current_path().string());
                    server.Printf("Maximum file size: %lld bytes", msg.get_max_stream_size());
                    return;
            }
            msg.type = bmmo::sound_stream_msg::sound_type::Wave;
            if (!msg.serialize()) {
                server.Printf("Error serializing message.");
                return;
            }
            server.Printf(bmmo::ansi::WhiteInverse, "Sound <%s> (size: %d) sent to %s",
                    msg.path, (uint32_t) msg.size(), broadcast ? "[all]" : std::to_string(client));
            if (broadcast) server.broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
            else server.send(client, msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        } catch (const std::exception& e) { server.Printf(e.what()); return; }
    });
    console.register_aliases("playstream", {"playstream#"});
    console.register_command("scores", [&] {
        if (console.empty()) { role::Printf("Usage: \"scores <hs|sr> [map]\""); return; }
        bool hs_mode = (console.get_next_word(true) == "hs");
        server.print_scores(hs_mode, console.empty() ? server.get_last_countdown_map() : static_cast<bmmo::map>(console.get_next_map()));
    });
    console.register_command("sendscores", [&] {
        HSteamNetConnection client{};
        if (console.get_command_name() == "sendscores#") {
            client = get_client_id_from_console();
            if (client == k_HSteamNetConnection_Invalid) return;
        }
        bmmo::score_list_msg msg{};
        msg.mode = (console.get_next_word(true) == "hs") ? bmmo::level_mode::Highscore : bmmo::level_mode::Speedrun;
        msg.map = console.empty() ? server.get_last_countdown_map() : static_cast<bmmo::map>(console.get_next_map());
        auto* rankings = server.get_map_rankings(msg.map);
        if (!rankings) {
            server.Printf(bmmo::ansi::BrightRed, "Error: ranking info not found for the specified map.");
            return;
        }
        msg.serialize(*rankings);
        if (client == 0)
            server.broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        else
            server.send(client, msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        server.Printf(bmmo::color_code(msg.code), "Score list data sent to #%u.", client);
    });
    console.register_aliases("sendscores", {"sendscores#"});
    console.register_command("restartlevel", [&] {
        if (console.empty()) return server.Printf("Usage: \"restartlevel <player>\".");
        auto client = get_client_id_from_console();
        if (client == k_HSteamNetConnection_Invalid) return;
        bmmo::restart_request_msg msg{.content = {.victim = client}};
        server.broadcast_message(msg);
        server.Printf(bmmo::color_code(msg.code), "Requested to restart #%u's current level.", client);
    });
    console.register_command("help", [&] { server.Printf(console.get_help_string().c_str()); });

    server.wait_till_started();

    if (dry_run)
        server.shutdown();

    while (server.running()) {
        std::string line;
        if (!console.read_input(line)) {
            puts("stop");
            server.shutdown();
            break;
        };
        server.LogFileOutput(("> " + line).c_str());

        if (!console.execute(line) && !console.get_command_name().empty()) {
            std::string extra_text;
            if (auto hints = console.get_command_hints(true); !hints.empty())
                extra_text = " Did you mean: " + bmmo::string_utils::join_strings(hints, 0, ", ") + "?";
            server.Printf("Error: unknown command \"%s\".%s", console.get_command_name(), extra_text);
        }
    }

    std::cout << "Stopping..." << std::endl;
    if (server_thread.joinable())
        server_thread.join();

    server::destroy();
    printf("\r");
}
