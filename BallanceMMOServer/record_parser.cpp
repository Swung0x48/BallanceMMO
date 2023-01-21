#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif

#include "../BallanceMMOCommon/common.hpp"
#include "../BallanceMMOCommon/entity/record_entry.hpp"
#include <fstream>
#include <condition_variable>
#include <cinttypes>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <filesystem>

#include <ya_getopt.h>

#include "console.hpp"

using bmmo::message_utils::read_variable;

enum class message_action_t: uint8_t { None, Broadcast, BroadcastNoDelay };

struct client_data {
    std::string name;
    uint8_t uuid[16]{};
};

class record_replayer: public role {
public:
    explicit record_replayer(uint16_t port) {
        port_ = port;
        set_logging_level(k_ESteamNetworkingSocketsDebugOutputType_Important);
    }

    EResult send(const HSteamNetConnection destination, void* buffer, size_t size, int send_flags, int64* out_message_number = nullptr) {
        return interface_->SendMessageToConnection(destination,
                                                   buffer,
                                                   size,
                                                   send_flags,
                                                   out_message_number);

    }

    template<bmmo::trivially_copyable_msg T>
    EResult send(const HSteamNetConnection destination, T msg, int send_flags, int64* out_message_number = nullptr) {
        static_assert(std::is_trivially_copyable<T>());
        return send(destination,
                    &msg,
                    sizeof(msg),
                    send_flags,
                    out_message_number);
    }

    void broadcast_message(void* buffer, size_t size, int send_flags, const HSteamNetConnection ignored_client = k_HSteamNetConnection_Invalid) {
        for (auto& i: clients_)
            if (ignored_client != i.first)
                send(i.first, buffer, size,
                                                    send_flags,
                                                    nullptr);
    }

    template<bmmo::trivially_copyable_msg T>
    void broadcast_message(T msg, int send_flags, const HSteamNetConnection ignored_client = k_HSteamNetConnection_Invalid) {
        static_assert(std::is_trivially_copyable<T>());

        broadcast_message(&msg, sizeof(msg), send_flags, ignored_client);
    }

#pragma region FileIO
    void set_record_file(const std::string& filename) {
        if (record_stream_.is_open()) {
            record_stream_.close();
            record_stream_.clear();
        }
        file_size_ = std::filesystem::file_size(filename.c_str());
        record_stream_.open(filename, std::ios::binary);
    }
    uintmax_t file_size_ = 0;
#pragma endregion
#pragma region SeekStateManagement
    enum player_mode_t: uint32_t {
        None = 0,
        Online = (1 << 0),
        Cheating = (1 << 1)
    };
    struct player_state_t {
        player_mode_t mode = None;
        bmmo::map map{};
        int32_t sector{};
    };

    struct time_period_t {
        time_period_t(SteamNetworkingMicroseconds begin, SteamNetworkingMicroseconds end, HSteamNetConnection id, player_state_t state)
            : begin(begin), end(end), id(id), state(state) {}

        SteamNetworkingMicroseconds begin = 0;
        SteamNetworkingMicroseconds end = 0;
        HSteamNetConnection id = k_HSteamNetConnection_Invalid;
        player_state_t state{};
    };

    SteamNetworkingMicroseconds get_current_record_time() { return current_record_time_; }
    time_t get_record_start_world_time() { return record_start_world_time_; }

    int get_segment_index(SteamNetworkingMicroseconds time) {
        return time / (int)1e7;
    }

    bool build_index(int64_t position) {
        record_stream_.seekg(position);
        Printf("Start building seek index...");
        SteamNetworkingMicroseconds last_segmented_timestamp = 0;
        segments_.emplace_back(segment_info_t{ position, 0 });
        while (record_stream_.good() && record_stream_.peek() != std::ifstream::traits_type::eof()) {
            current_record_time_ = read_variable<SteamNetworkingMicroseconds>(record_stream_) - record_start_time_;
            bool print_status = false;
            if (get_segment_index(current_record_time_) > get_segment_index(last_segmented_timestamp)) {
                last_segmented_timestamp = current_record_time_;
                segments_.emplace_back(segment_info_t{ (int64_t)record_stream_.tellg() - (int64_t)sizeof(SteamNetworkingMicroseconds), current_record_time_ });
                print_status = true;
            }
            auto size = read_variable<int32_t>(record_stream_);
            bmmo::record_entry entry(size);
            record_stream_.read(reinterpret_cast<char*>(entry.data), size);
            auto* raw_msg = reinterpret_cast<bmmo::general_message*>(entry.data);
            switch (raw_msg->code) {
                case bmmo::LoginAcceptedV3: {
                    auto msg = bmmo::message_utils::deserialize<bmmo::login_accepted_v3_msg>(entry.data, entry.size);
                    for (const auto& [id, data]: msg.online_players) {
                        player_state_t state = {
                            static_cast<player_mode_t>(Online | ((data.cheated) ? Cheating : None)),
                            data.map,
                            data.sector,
                        };
                        timeline_[data.name].emplace_back(current_record_time_, std::numeric_limits<int64_t>::max(), id, state);
                        record_clients_.insert({id, {data.name, data.cheated}});
                    }
                    break;
                }
                case bmmo::PlayerDisconnected: {
                    auto msg = bmmo::message_utils::deserialize<bmmo::player_disconnected_msg>(entry.data, entry.size);
                    auto it = record_clients_.find(msg.content.connection_id);
                    if (it != record_clients_.end()) {
                        auto& time_period = timeline_[it->second.name].back(); // it should exist ahead of time
                        time_period.end = current_record_time_; // end this period
                        record_clients_.erase(msg.content.connection_id);
                    }
                    break;
                }
                case bmmo::PlayerConnectedV2: {
                    auto msg = bmmo::message_utils::deserialize<bmmo::player_connected_v2_msg>(entry.data, entry.size);
                    player_state_t state = { static_cast<player_mode_t>(Online | ((msg.cheated) ? Cheating : None)) };
                    timeline_[msg.name].emplace_back(current_record_time_, std::numeric_limits<int64_t>::max(), msg.connection_id, state);
                    record_clients_.insert({msg.connection_id, {msg.name, msg.cheated}});
                    break;
                }
                case bmmo::OwnedCheatState: {
                    auto msg = bmmo::message_utils::deserialize<bmmo::owned_cheat_state_msg>(entry.data, entry.size);
                    auto username = record_clients_[msg.content.player_id].name;
                    auto& last_time_period = timeline_[username].back();
                    last_time_period.end = current_record_time_;

                    auto state(last_time_period.state);
                    state.mode = static_cast<player_mode_t>(Online | ((msg.content.state.cheated) ? Cheating : None));
                    timeline_[username].emplace_back(current_record_time_ + 1, std::numeric_limits<int64_t>::max(), msg.content.player_id, state);
                    record_clients_[msg.content.player_id].cheated = msg.content.state.cheated;
                    break;
                }
                case bmmo::CurrentMap: {
                    auto msg = bmmo::message_utils::deserialize<bmmo::current_map_msg>(entry.data, entry.size);
                    if (msg.content.type == bmmo::current_map_state::Announcement) break;
                    auto username = record_clients_[msg.content.player_id].name;
                    auto& last_time_period = timeline_[username].back();
                    last_time_period.end = current_record_time_;

                    auto state(last_time_period.state);
                    state.map = msg.content.map;
                    state.sector = msg.content.sector;
                    timeline_[username].emplace_back(current_record_time_ + 1, std::numeric_limits<int64_t>::max(), msg.content.player_id, state);
                    break;
                }
                case bmmo::CurrentSector: {
                    auto msg = bmmo::message_utils::deserialize<bmmo::current_sector_msg>(entry.data, entry.size);
                    auto username = record_clients_[msg.content.player_id].name;
                    auto& last_time_period = timeline_[username].back();
                    last_time_period.end = current_record_time_;

                    auto state(last_time_period.state);
                    state.sector = msg.content.sector;
                    timeline_[username].emplace_back(current_record_time_ + 1, std::numeric_limits<int64_t>::max(), msg.content.player_id, state);
                    break;
                }
                case bmmo::MapNames: {
                    auto msg = bmmo::message_utils::deserialize<bmmo::map_names_msg>(entry.data, entry.size);
                    record_map_names_.insert(msg.maps.begin(), msg.maps.end());
                    break;
                }
                case bmmo::PermanentNotification: {
                    auto msg = bmmo::message_utils::deserialize<bmmo::permanent_notification_msg>(entry.data, entry.size);
                    permanent_notification_timeline_.try_emplace(current_record_time_, msg.title, msg.text_content);
                    break;
                }
                default: {
                    break;
                }
            }

            if (print_status)
                printf("\rBuilding seek index...[%" PRIu64 "/%" PRIu64 "] %.2lf%%",
                        (uint64_t)record_stream_.tellg() - position,
                        (uint64_t)(file_size_ - position),
                        ((double)((uint64_t)record_stream_.tellg() - position) / (double)(file_size_ - position)) * 100.0);
        }

        if (!(record_stream_.good() && record_stream_.peek() != std::ifstream::traits_type::eof())) {
            Printf("Seek index built successfully.");
        }
        duration_ = current_record_time_;
        current_record_time_ = 0;

        // Reset position to the beginning
        record_stream_.seekg(position);

        return true;
    }

    // begin_time, <username (title), text>
    std::map<SteamNetworkingMicroseconds, std::pair<std::string, std::string>> permanent_notification_timeline_{{0, {}}};
    std::pair<std::string, std::string> record_permanent_notification_;

    // username - time_period
    std::unordered_map<std::string, std::vector<time_period_t>> timeline_;
    struct segment_info_t {
        int64_t position = 0;
        SteamNetworkingMicroseconds time = 0;
    };
    // record byte position for every 10 second
    std::vector<segment_info_t> segments_;
    SteamNetworkingMicroseconds duration_;
#pragma endregion

    bool setup() override {
        if (!record_stream_.is_open()) {
            FatalError("Error: cannot open record file.");
            return false;
        }
        record_stream_.seekg(0);
        std::string read_data;
        std::getline(record_stream_, read_data, '\0');
        if (read_data != bmmo::RECORD_HEADER) {
            FatalError("Error: invalid record file.");
            return false;
        }
        Printf("BallanceMMO FlightRecorder Data");
        record_version_ = read_variable<decltype(record_version_)>(record_stream_);
        // record_stream_.read(reinterpret_cast<char*>(&version), sizeof(version));
        Printf("Version: \t\t%s\n", record_version_.to_string());
        record_start_world_time_ = read_variable<int64_t>(record_stream_);
        // record_stream_.read(reinterpret_cast<char *>(&start_time), sizeof(start_time));
        char time_str[32];
        strftime(time_str, sizeof(time_str), "%F %T", localtime(&record_start_world_time_));
        Printf("Record start time: \t%s\n", time_str);
        record_start_time_ = read_variable<decltype(record_start_time_)>(record_stream_);

        if (!build_index(record_stream_.tellg())) {
            Printf("Seek index build failed.");
            return false;
        }
        Printf("Record length: \t%.3lfs", duration_ / 1e6);

        if (!init_) {
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

            init_ = true;
        }

        running_ = true;
        Printf("Record file loaded successfully.");
        startup_cv_.notify_all();
        Printf("Fake server started at port %u.", port_);

        return true;
    }

    void run() override {
        while (running_) {
            auto next_update = std::chrono::steady_clock::now() + bmmo::SERVER_RECEIVE_INTERVAL;
            update();
            std::this_thread::sleep_until(next_update);
        }
    }

    bool playing() { return playing_; }
    bool started() { return started_; }

    void play() {
        if (!started_) {
            time_zero_ = std::chrono::steady_clock::now();
            Printf("Playing started.");
            play_record();
            started_ = true;
        } else {
            time_zero_ = (std::chrono::steady_clock::now() - std::chrono::microseconds(current_record_time_));
            Printf("Playing resumed at %.3lfs.", current_record_time_ / 1e6);
            play_record();
        }
    }

    void pause() {
        playing_ = false;
        time_pause_ = std::chrono::steady_clock::now();
        Printf("Playing paused at %.3lfs.", current_record_time_ / 1e6);
    }

    void seek(double seconds) {
        if (!started_) {
            time_zero_ = std::chrono::steady_clock::now();
            started_ = true;
        }
        bool was_playing = false;
        if (playing_) {
            pause();
            was_playing = true;
        }
        SteamNetworkingMicroseconds dest_time = seconds * 1e6;
        if (dest_time > duration_) {
            Printf("Cannot seek: Seek destination goes beyond the end of the record! (%.3lfs / %.3lfs)", seconds,
                   duration_ / 1e6);
            if (was_playing)
                play();
            return;
        }
        seeking_ = true;

        { // actually seeking
            std::unique_lock lk(record_data_mutex_);
            // place stream read pointer to appropriate place
            int index = get_segment_index(dest_time);
            auto& segment = segments_[index];
            current_record_time_ = segment.time;
            record_stream_.seekg(segment.position);
        }
        forward_seek(dest_time, false);

        // figure out states at that timepoint and rebuild it
        record_clients_.clear();
        for (auto& [username, periods]: timeline_) {
            auto it = std::lower_bound(periods.begin(), periods.end(), dest_time,
                [](const time_period_t period, SteamNetworkingMicroseconds time) {
                    return period.end < time;
            });
            if (it != periods.end()) {
                bool valid = false;
                if (it->begin <= dest_time && dest_time <= it->end) {
                    valid = true;
                }
                if (valid) {
                    record_clients_[it->id] = {
                        username,
                        static_cast<uint8_t>((it->state.mode & Cheating) ? 1 : 0),
                        it->state.map,
                        it->state.sector,
                    };
                }
                // std::cout << "\rRebuilding state for " << username << " #" << it->id << " [" << it->begin << ", " << current_record_time_
                //               << ", " << it->end << "] " << (valid ? "valid" : "invalid");
            }
        }

        // broadcast LoginAccepted message for client to rebuild states
        bmmo::login_accepted_v3_msg msg;
        msg.online_players = record_clients_;
        msg.serialize();
        broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);

        if (!permanent_notification_timeline_.empty()) {
            record_permanent_notification_ =
                std::prev(permanent_notification_timeline_.upper_bound(current_record_time_))->second;
            bmmo::permanent_notification_msg bulletin_msg{};
            std::tie(bulletin_msg.title, bulletin_msg.text_content) = record_permanent_notification_;
            bulletin_msg.serialize();
            broadcast_message(bulletin_msg.raw.str().data(), bulletin_msg.size(), k_nSteamNetworkingSend_Reliable);
        }

        seeking_ = false;
        Printf("Sought to %.3lfs successfully.", current_record_time_ / 1e6);
        if (was_playing)
            play();
    }

    void seek_legacy(double seconds) {
        if (!started_) {
            time_zero_ = std::chrono::steady_clock::now();
            started_ = true;
        }
        bool was_playing = false;
        if (playing_) {
            pause();
            was_playing = true;
        }
        SteamNetworkingMicroseconds dest_time = seconds * 1e6;
        for (auto& i: clients_) {
            interface_->CloseConnection(i.first, k_ESteamNetConnectionEnd_App_Min + 150 + 3, "Seeking data, please wait", true);
        }
        seeking_ = true;
        if (dest_time <= current_record_time_) {
            backward_seek(dest_time);
        } else {
            forward_seek(dest_time);
        }
        seeking_ = false;
        Printf("Sought to %.3lfs successfully.", current_record_time_ / 1e6);
        if (was_playing)
            play();
    }

    void wait_till_started() {
        while (!running()) {
            std::unique_lock<std::mutex> lk(startup_mutex_);
            startup_cv_.wait(lk);
        }
    }

    void shutdown(int reconnection_delay = 0) {
        Printf("Shutting down...");
        int nReason = reconnection_delay == 0 ? 0 : k_ESteamNetConnectionEnd_App_Min + 150 + reconnection_delay;
        for (auto& i: clients_) {
            interface_->CloseConnection(i.first, nReason, "Server closed", true);
        }
        running_ = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (player_thread_.joinable())
            player_thread_.join();
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
        auto current_world_time = time_t(current_record_time_ / 1e6 + record_start_world_time_);
        char time_str[32];
        std::strftime(time_str, sizeof(time_str), "%F %T", std::localtime(&current_world_time));
        Printf("Real world time: %s.", time_str);
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
    void play_record() {
        if (player_thread_.joinable())
            player_thread_.join();
        playing_ = true;
        player_thread_ = std::thread([this]() {
            while (running_ && playing_ && record_stream_.good()
                    && record_stream_.peek() != std::ifstream::traits_type::eof()) {
                bmmo::record_entry entry;
                int size = 0;
                {
                    std::unique_lock<std::mutex> lk(record_data_mutex_);
                    current_record_time_ =
                            read_variable<SteamNetworkingMicroseconds>(record_stream_) - record_start_time_;
                    size = read_variable<int32_t>(record_stream_);
                    bmmo::record_entry e(size);
                    record_stream_.read(reinterpret_cast<char*>(e.data), size);
                    entry = std::move(e);
                }
                if (!running_ || !playing_)
                    break;
                std::this_thread::sleep_until(time_zero_ + std::chrono::microseconds(current_record_time_));

                // auto* raw_msg = reinterpret_cast<bmmo::general_message*>(entry.data);
                // Printf("Time: %7.2lf | Code: %2u | Size: %4d\n", current_record_time_ / 1e6, raw_msg->code, entry.size);
                switch (parse_message(entry)) {
                    case message_action_t::BroadcastNoDelay:
                        broadcast_message(entry.data, size, k_nSteamNetworkingSend_UnreliableNoDelay);
                        break;
                    case message_action_t::Broadcast:
                        broadcast_message(entry.data, size, k_nSteamNetworkingSend_Reliable);
                    default:
                        break;
                }
            }
            if (!(record_stream_.good() && record_stream_.peek() != std::ifstream::traits_type::eof())) {
                Printf("Playing finished at %.3lfs.", current_record_time_ / 1e6);
                playing_ = false;
            }
        });
    }

    void forward_seek(SteamNetworkingMicroseconds dest_time, bool parse = true) {
        std::unique_lock<std::mutex> lk(record_data_mutex_);
        time_zero_ -= std::chrono::microseconds(dest_time - current_record_time_);
        while (running_ && current_record_time_ < dest_time
                && record_stream_.good() && record_stream_.peek() != std::ifstream::traits_type::eof()) {
            current_record_time_ = read_variable<SteamNetworkingMicroseconds>(record_stream_) - record_start_time_;
            auto size = read_variable<int32_t>(record_stream_);
            if (parse) {
                bmmo::record_entry entry(size);
                record_stream_.read(reinterpret_cast<char*>(entry.data), size);
                parse_message(entry);
            } else
                record_stream_.seekg(size, std::ios::cur);
        }
    }

    void backward_seek(SteamNetworkingMicroseconds dest_time) {
        {
            std::unique_lock<std::mutex> lk(record_data_mutex_);
            record_stream_.seekg(strlen(bmmo::RECORD_HEADER) + 1 + sizeof(bmmo::version_t) + sizeof(int64_t) + sizeof(SteamNetworkingMicroseconds));
            started_ = false;
            record_clients_.clear();
            record_map_names_.clear();
            time_zero_ = std::chrono::steady_clock::now();
            current_record_time_ = 0;
            started_ = true;
        }
        forward_seek(dest_time);
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

                    cleanup_disconnected_client(pInfo->m_hConn);
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
        if (auto itName = username_.find(name); itName != username_.end())
            username_.erase(itName);
        if (itClient != clients_.end())
            clients_.erase(itClient);
        Printf("%s (#%u) disconnected.", name, client);
        
        bmmo::plain_text_msg text_msg;
        text_msg.text_content.resize(128);
        Sprintf(text_msg.text_content, "[Reality] %s disconnected.", name);
        text_msg.serialize();
        broadcast_message(text_msg.raw.str().data(), text_msg.size(), k_nSteamNetworkingSend_Reliable);
    }

    void on_message(ISteamNetworkingMessage* networking_msg) override {
        auto client_it = clients_.find(networking_msg->m_conn);
        auto* raw_msg = reinterpret_cast<bmmo::general_message*>(networking_msg->m_pData);

        if (!(client_it != clients_.end() || raw_msg->code == bmmo::LoginRequest || raw_msg->code == bmmo::LoginRequestV2 || raw_msg->code == bmmo::LoginRequestV3)) { // ignore limbo clients message
            interface_->CloseConnection(networking_msg->m_conn, k_ESteamNetConnectionEnd_AppException_Min, "Invalid client", true);
            return;
        }

        switch (raw_msg->code) {
            case bmmo::LoginRequestV3: {
                auto msg = bmmo::message_utils::deserialize<bmmo::login_request_v3_msg>(networking_msg);

                interface_->SetConnectionName(networking_msg->m_conn, msg.nickname.c_str());
                // only major and minor is required to be the same
                if (std::memcmp(&msg.version, &record_version_, sizeof(msg.version.major) + sizeof(msg.version.minor)) != 0) {
                    interface_->CloseConnection(networking_msg->m_conn, k_ESteamNetConnectionEnd_App_Min + 1,
                        Sprintf("Incorrect version; please use BMMO v%s", record_version_.to_string()).c_str(), true);
                    break;
                }
                if (seeking_) {
                    interface_->CloseConnection(networking_msg->m_conn, k_ESteamNetConnectionEnd_App_Min + 150 + 2,
                        "Seeking data, please wait", true);
                    break;
                }
                
                clients_[networking_msg->m_conn] = {msg.nickname, {}};  // add the client here
                memcpy(clients_[networking_msg->m_conn].uuid, msg.uuid, sizeof(msg.uuid));
                username_[msg.nickname] = networking_msg->m_conn;
                Printf("%s (v%s) logged in!\n",
                        msg.nickname,
                        msg.version.to_string());

                bmmo::map_names_msg names_msg;
                names_msg.maps = record_map_names_;
                names_msg.serialize();
                send(networking_msg->m_conn, names_msg.raw.str().data(), names_msg.size(), k_nSteamNetworkingSend_Reliable);
                
                bmmo::login_accepted_v3_msg accepted_msg{};
                accepted_msg.online_players = record_clients_;
                accepted_msg.serialize();
                send(networking_msg->m_conn, accepted_msg.raw.str().data(), accepted_msg.size(), k_nSteamNetworkingSend_Reliable);

                if (!record_permanent_notification_.second.empty()) {
                    bmmo::permanent_notification_msg bulletin_msg{};
                    std::tie(bulletin_msg.title, bulletin_msg.text_content) = record_permanent_notification_;
                    bulletin_msg.serialize();
                    send(networking_msg->m_conn, bulletin_msg.raw.str().data(), bulletin_msg.size(), k_nSteamNetworkingSend_Reliable);
                }

                bmmo::plain_text_msg text_msg;
                text_msg.text_content.resize(128);
                Sprintf(text_msg.text_content, "[Reality] %s joined the game.", msg.nickname);
                text_msg.serialize();
                broadcast_message(text_msg.raw.str().data(), text_msg.size(), k_nSteamNetworkingSend_Reliable);
                break;
            }
            case bmmo::Chat: {
                auto msg = bmmo::message_utils::deserialize<bmmo::chat_msg>(networking_msg);
                Printf("(%u, %s): %s", networking_msg->m_conn, client_it->second.name, msg.chat_content);
                bmmo::plain_text_msg text_msg;
                text_msg.text_content.resize(2048);
                Sprintf(text_msg.text_content, "[Reality] %s: %s", client_it->second.name, msg.chat_content);
                text_msg.serialize();
                broadcast_message(text_msg.raw.str().data(), text_msg.size(), k_nSteamNetworkingSend_Reliable);
                break;
            }
            default:
                break;
        }
    }

    message_action_t parse_message(bmmo::record_entry& entry /*, SteamNetworkingMicroseconds time*/) {
        auto* raw_msg = reinterpret_cast<bmmo::general_message*>(entry.data);
        // std::unique_lock<std::mutex> lk(record_data_mutex_);
        // Printf("Time: %7.2lf | Code: %2u | Size: %4d\n", time / 1e6, raw_msg->code, entry.size);
        switch (raw_msg->code) {
            case bmmo::LoginAcceptedV3: {
                auto msg = bmmo::message_utils::deserialize<bmmo::login_accepted_v3_msg>(entry.data, entry.size);
                record_clients_ = msg.online_players;
                // printf("Code: LoginAcceptedV2\n");
                // bmmo::login_accepted_v2_msg msg{};
                // msg.raw.write(reinterpret_cast<char*>(entry.data), entry.size);
                // msg.deserialize();
                // printf("\t%d player(s) online: \n", msg.online_players.size());
                break;
            }
            case bmmo::PlayerDisconnected: {
                auto msg = bmmo::message_utils::deserialize<bmmo::player_disconnected_msg>(entry.data, entry.size);
                record_clients_.erase(msg.content.connection_id);
                break;
            }
            case bmmo::PlayerConnectedV2: {
                auto msg = bmmo::message_utils::deserialize<bmmo::player_connected_v2_msg>(entry.data, entry.size);
                record_clients_.insert({msg.connection_id, {msg.name, msg.cheated}});
                break;
            }
            case bmmo::OwnedCheatState: {
                auto msg = bmmo::message_utils::deserialize<bmmo::owned_cheat_state_msg>(entry.data, entry.size);
                record_clients_[msg.content.player_id].cheated = msg.content.state.cheated;
                break;
            }
            case bmmo::MapNames: {
//                auto msg = bmmo::message_utils::deserialize<bmmo::map_names_msg>(entry.data, entry.size);
//                record_map_names_.insert(msg.maps.begin(), msg.maps.end());
                break;
            }
            case bmmo::CurrentMap: {
                auto msg = bmmo::message_utils::deserialize<bmmo::current_map_msg>(entry.data, entry.size);
                if (msg.content.type != msg.content.EnteringMap) break;
                record_clients_[msg.content.player_id].map = msg.content.map;
                record_clients_[msg.content.player_id].sector = msg.content.sector;
                break;
            }
            case bmmo::CurrentSector: {
                auto msg = bmmo::message_utils::deserialize<bmmo::current_sector_msg>(entry.data, entry.size);
                record_clients_[msg.content.player_id].sector = msg.content.sector;
                break;
            }
            case bmmo::PermanentNotification: {
                auto msg = bmmo::message_utils::deserialize<bmmo::permanent_notification_msg>(entry.data, entry.size);
                record_permanent_notification_ = {msg.title, msg.text_content};
                break;
            }
            case bmmo::OwnedTimedBallState:
            case bmmo::OwnedCompressedBallState: {
                return message_action_t::BroadcastNoDelay;
            }
            default: {
                // printf("Code: %d\n", raw_msg->code);
                break;
            }
        }
        return message_action_t::Broadcast;
        // std::cout.write(entry.data, entry.size);
    }

    bmmo::version_t record_version_;
    std::ifstream record_stream_;
    SteamNetworkingMicroseconds record_start_time_;
    time_t record_start_world_time_;
    std::chrono::steady_clock::time_point time_zero_, time_pause_;

    uint16_t port_ = 0;
    std::mutex startup_mutex_, record_data_mutex_;
    std::condition_variable startup_cv_;
    // bool print_states = true;
    std::atomic_bool playing_ = false, started_ = false, seeking_ = false, init_ = false;
    std::thread player_thread_;

    SteamNetworkingMicroseconds current_record_time_{};
    std::unordered_map<HSteamNetConnection, bmmo::player_status_v3> record_clients_;
    std::unordered_map<std::string, std::string> record_map_names_;

    HSteamListenSocket listen_socket_ = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup poll_group_ = k_HSteamNetPollGroup_Invalid;

    std::unordered_map<HSteamNetConnection, client_data> clients_;
    std::unordered_map<std::string, HSteamNetConnection> username_;
};

// parse arguments (optional port and help/version) with getopt
int parse_args(int argc, char** argv, uint16_t& port, std::string& filename) {
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
                port = atoi(optarg);
                break;
            case 'h':
                printf("Usage: %s [RECORD_FILE] [OPTION]...\n", argv[0]);
                puts("Options:");
                puts("  -p, --port=PORT\t Use PORT as the server port instead (default: 26677).");
                puts("  -h, --help\t\t Display this help and exit.");
                puts("  -v, --version\t\t Display version information and exit.");
                return -1;
            case 'v':
                puts("Ballance MMO record replayer by Swung0x48 and BallanceBug.");
                printf("Build time: \t%s %s.\n", __DATE__, __TIME__);
                printf("Version: \t%s.\n", bmmo::version_t().to_string().c_str());
                puts("GitHub repository: https://github.com/Swung0x48/BallanceMMO");
                return -1;
        }
    }
    if (optind != argc) {
        filename = argv[optind];
        printf("Using %s as the record file.\n", filename.c_str());
        return 0;
    }
    printf("Error: please set a record file (use \"%s <record_file>\") before starting the replayer!\n", argv[0]);
    return 1;
}

int main(int argc, char** argv) {
    uint16_t port = 26677;
    std::string filename;
    if (int v = parse_args(argc, argv, port, filename); v != 0)
        return std::max(v, 0);

    printf("Initializing sockets...\n");
    record_replayer::init_socket();

    printf("Starting fake server at port %u.\n", port);
    record_replayer replayer(port);

    printf("Bootstrapping server...\n");
    fflush(stdout);

    replayer.set_record_file(filename);
    if (!replayer.setup())
        role::FatalError("Fake server failed on setup.");
    std::thread server_thread([&replayer]() { replayer.run(); });

    console console;
    console.register_command("help", [&]() { role::Printf(console.get_help_string().c_str()); });
    console.register_command("play", [&]() {
        if (replayer.playing()) {
            replayer.Printf("Record is already playing.");
            return;
        }
        replayer.play();
    });
    console.register_command("stop", [&]() {
        replayer.shutdown(console.get_next_int());
    });
    console.register_command("list", std::bind(&record_replayer::print_clients, &replayer));
    console.register_command("time", std::bind(&record_replayer::print_current_record_time, &replayer));
    console.register_command("pause", [&]() {
        if (!replayer.playing()) {
            replayer.Printf("Already not playing.");
            return;
        }
        replayer.pause();
    });
    console.register_command("seek", [&]() {
        constexpr auto print_hint = [] {
            role::Printf("Usage: \"seek <time>\".");
            role::Printf("Examples:\t seek 11.4 || seek -51.4 || seek +19:19.810");
            role::Printf("\t\t seek 2019-08-10 11:45:14");
        };
        if (console.empty()) { print_hint(); return; }
        auto time_string = console.get_rest_of_line();
        double time_value = 0;
        switch (std::count(time_string.begin(), time_string.end(), ':')) {
            default:
            case 0: // precise time point measured in seconds
                time_value = atof(time_string.c_str());
                break;
            case 1: // [sign]<minute>:<second>
                double minutes, seconds;
                if (sscanf(time_string.c_str(), "%lf:%lf", &minutes, &seconds) != 2) {
                    print_hint(); return;
                };
                time_value = minutes * 60 + seconds * (minutes < 0 ? -1 : 1);
                break;
            case 2: // real-world time format
                std::stringstream time_stream(time_string); std::tm time_struct;
                time_stream >> std::get_time(&time_struct, "%Y-%m-%d %H:%M:%S");
                if (time_stream.fail()) { print_hint(); return; };
                time_value = std::mktime(&time_struct) - replayer.get_record_start_world_time();
                break;
        }
        if (time_string.starts_with('+') || time_string.starts_with('-'))
            time_value += replayer.get_current_record_time() / 1e6;
        replayer.seek(time_value);
    });
    console.register_command("seek_legacy", [&]() {
        replayer.seek_legacy(atof(console.get_next_word().c_str()));
    });
    console.register_command("say", [&]() {
        bmmo::plain_text_msg text_msg;
        auto text = console.get_rest_of_line();
        text_msg.text_content = "[Reality] [Server]: " + text;
        text_msg.serialize();
        replayer.broadcast_message(text_msg.raw.str().data(), text_msg.size(), k_nSteamNetworkingSend_Reliable);
        replayer.Printf("[Server]: %s", text);
    });
    console.register_command("load", [&]() {
        replayer.shutdown(4);
        if (server_thread.joinable())
            server_thread.join();
        replayer.set_record_file(console.get_rest_of_line());
        if (!replayer.setup())
            role::FatalError("Fake server failed on setup.");
        server_thread = std::thread([&replayer]() { replayer.run(); });
        replayer.wait_till_started();
    });

    role::Printf("To see all available commands, type \"help\".");

    replayer.wait_till_started();

    while (replayer.running()) {
        std::cout << "\r> " << std::flush;
        std::string line;
        if (!console.read_input(line)) {
            puts("stop");
            replayer.shutdown();
            break;
        };

        // bmmo::command_parser parser(line);
        if (!console.execute(line)) {
            role::Printf("Error: unknown command \"%s\".", console.get_command_name());
        };
    }

    std::cout << "Stopping..." << std::endl;
    if (server_thread.joinable())
        server_thread.join();

    record_replayer::destroy();
    printf("\r");
}
