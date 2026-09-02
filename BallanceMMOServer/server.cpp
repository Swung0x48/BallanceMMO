#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif

#include <vector>
#include <unordered_set>
#include <condition_variable>
#include "../BallanceMMOCommon/common.hpp"

#include <mutex>
#include <optional>
#include <random>
#include <fstream>
#include <filesystem>

#include <ya_getopt.h>
#include <yaml-cpp/yaml.h>

#define PICOJSON_USE_INT64
#include <picojson/picojson.h>
#include "server_data.hpp"
#include "config_manager.hpp"
#include "room/room_manager.hpp"
#if BMMO_BUILD_SIM
#include "sim/session_runner.hpp"
#include "sim/crash_report.hpp"
#include <cmath>
#endif

using bmmo::Printf, bmmo::Sprintf, bmmo::LogFileOutput, bmmo::FatalError;

class server: public role {
public:
    explicit server(uint16_t port) {
        port_ = port;
    }

    // Main network loop. Each iteration:
    //   1. update(): drain incoming messages and run connection callbacks;
    //   2. if ticking, wait until SERVER_TICK_DELAY into the iteration and
    //      broadcast the collected ball states (tick());
    //   3. sleep out the remainder of SERVER_RECEIVE_INTERVAL.
    // Console commands run on the main thread and synchronize with this
    // loop through state_mutex_.
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
    }

    EResult send(const HSteamNetConnection destination, const void* buffer, size_t size, int send_flags = k_nSteamNetworkingSend_Reliable, int64* out_message_number = nullptr) const {
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
        std::lock_guard lk(state_mutex_);
        for (const auto& i: clients_)
            if (ignored_client != i.first)
                send(i.first, buffer, size, send_flags, nullptr);
    }

    template<bmmo::trivially_copyable_msg T>
    void broadcast_message(const T& msg, int send_flags = k_nSteamNetworkingSend_Reliable, const HSteamNetConnection ignored_client = k_HSteamNetConnection_Invalid) {
        static_assert(std::is_trivially_copyable<T>());

        broadcast_message(&msg, sizeof(msg), send_flags, ignored_client);
    }

    // Injects a message into on_message() as if it came from the given
    // client (or a random online client if none is specified).
    void receive(void* data, size_t size, HSteamNetConnection client = k_HSteamNetConnection_Invalid) {
        std::lock_guard lk(state_mutex_);
        if (clients_.empty()) { Printf("Error: no online players found."); return; }
        if (client == k_HSteamNetConnection_Invalid) { // random player
            static std::mt19937 gen{std::random_device{}()};
            std::uniform_int_distribution<size_t> dist(0, clients_.size() - 1);
            client = std::next(clients_.begin(), dist(gen))->first;
        }
        auto* networking_msg = SteamNetworkingUtils()->AllocateMessage(0);
        networking_msg->m_conn = client;
        networking_msg->m_pData = data;
        networking_msg->m_cbSize = static_cast<int>(size);
        // handlers use this to timestamp state (e.g. map start times); leaving
        // it at 0 would make injected messages look infinitely old
        networking_msg->m_usecTimeReceived = SteamNetworkingUtils()->GetLocalTimestamp();
        on_message(networking_msg);
        // AllocateMessage(0) leaves m_pfnFreeData null, so Release() will not
        // try to free the caller's buffer - but clear it anyway to be explicit
        networking_msg->m_pData = nullptr;
        networking_msg->m_cbSize = 0;
        networking_msg->Release();
    }

    std::pair<std::string, std::string> get_bulletin() {
        std::lock_guard lk(state_mutex_);
        return permanent_notification_;
    }

    void set_and_broadcast_bulletin(const std::string& title, const std::string& text) {
        bmmo::permanent_notification_msg msg{};
        {
            std::lock_guard lk(state_mutex_);
            permanent_notification_ = {title, text};
            std::tie(msg.title, msg.text_content) = permanent_notification_;
        }
        msg.serialize();
        broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
    }

    // Returns a copy; the previous reference-returning version dereferenced
    // the end() iterator when the client was not found.
    std::optional<client_data> get_client(HSteamNetConnection client) {
        std::lock_guard lk(state_mutex_);
        auto client_it = clients_.find(client);
        if (client_it == clients_.end()) {
            Printf("Error: client #%u not found.", client);
            return std::nullopt;
        }
        return client_it->second;
    }

    HSteamNetConnection get_client_id(const std::string& username, bool suppress_error = false) {
        if (username.empty()) return k_HSteamNetConnection_Invalid;
        std::lock_guard lk(state_mutex_);
        const std::string begin_name(bmmo::message_utils::to_lower(username));
        // Prefix range: [lower_bound(prefix), first key not starting with it).
        // Deriving the end by incrementing the last byte breaks when that byte
        // is 0xFF (it wraps to 0x00, putting "end" *before* "begin" and making
        // the loop below run off the end of the map) - a remotely reachable
        // crash, since names come from KickRequest messages.
        auto username_it = username_.lower_bound(begin_name);
        auto username_it_end = username_it;
        while (username_it_end != username_.end() && username_it_end->first.starts_with(begin_name))
            ++username_it_end;
        if (username_it == username_it_end) {
            if (!suppress_error)
                Printf("Error: client \"%s\" not found.", username);
            return k_HSteamNetConnection_Invalid;
        }
        else if (std::next(username_it) == username_it_end || username_it->first == begin_name) {
            return username_it->second;
        }
        std::string names;
        for (; username_it != username_it_end; ++username_it)
            names += ", " + username_it->first;
        if (!suppress_error)
            Printf("Error: multiple possible players: %s.", names.erase(0, 2));
        return k_HSteamNetConnection_Invalid;
    }

    std::string get_client_name(HSteamNetConnection id) {
        if (id == k_HSteamNetConnection_Invalid)
            return "[Server]";
        std::lock_guard lk(state_mutex_);
        if (auto client_it = clients_.find(id); client_it != clients_.end())
            return client_it->second.name;
        return "";
    }

    inline int get_client_count() {
        std::lock_guard lk(state_mutex_);
        return static_cast<int>(clients_.size());
    }

    // Narrow, locked accessors instead of handing out a mutable reference to
    // config_: the network thread mutates it (bans, mutes) under state_mutex_,
    // so console commands must not read it unsynchronized.
    void print_bans() {
        std::lock_guard lk(state_mutex_);
        config_.print_bans();
    }
    void print_mutes() {
        std::lock_guard lk(state_mutex_);
        config_.print_mutes();
    }
    inline bmmo::map get_last_countdown_map() {
        std::lock_guard lk(state_mutex_);
        return last_countdown_map_;
    }

    // Returns a copy so the caller doesn't hold a pointer into maps_
    // that the network thread may mutate concurrently.
    std::optional<bmmo::ranking_entry::player_rankings> get_map_rankings(const bmmo::map& map) {
        std::lock_guard lk(state_mutex_);
        auto map_it = maps_.find(map.get_hash_bytes_string());
        if (map_it == maps_.end() || (map_it->second.rankings.first.empty() && map_it->second.rankings.second.empty()))
            return std::nullopt;
        return map_it->second.rankings;
    }

    bool kick_client(HSteamNetConnection client, std::string reason = "",
            HSteamNetConnection executor = k_HSteamNetConnection_Invalid,
            bmmo::connection_end::code type = bmmo::connection_end::Kicked) {
        std::lock_guard lk(state_mutex_);
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
        std::lock_guard lk(state_mutex_);
        const bool prev_ghost_mode = config_.ghost_mode;
        if (!config_.load())
            return false;
        rooms_.max_rooms = config_.maximum_rooms;
        rooms_.max_members = static_cast<uint16_t>(std::clamp<uint32_t>(config_.maximum_members, 1, bmmo::room::MAX_MEMBERS_PER_MESSAGE));
        if (clients_.empty()) map_names_.clear();
        map_names_.insert(config_.default_map_names.begin(), config_.default_map_names.end());
        if (!clients_.empty()) {
            if (!map_names_.empty()) {
                bmmo::map_names_msg name_msg;
                name_msg.maps = map_names_;
                name_msg.serialize();
                broadcast_message(name_msg.raw.str().data(), name_msg.size());
            }
            bmmo::extra_life_msg life_msg;
            life_msg.life_count_goals = config_.initial_life_counts;
            life_msg.serialize();
            broadcast_message(life_msg.raw.str().data(), life_msg.size());
            if (config_.ghost_mode != prev_ghost_mode) {
                // send everyone except ghost spectators a message that sets parts of
                // other players' positions to infinity, effectively hiding them
                bmmo::owned_compressed_ball_state_msg ball_msg{};
                pull_ball_states(ball_msg.balls);
                if (config_.ghost_mode) {
                    for (auto& state: ball_msg.balls) {
                        state.state.position.y = std::numeric_limits<float>::infinity();
                        state.state.timestamp += bmmo::CLIENT_MINIMUM_UPDATE_INTERVAL_US;
                    }
                }
                ball_msg.serialize();
                for (const auto& [client, _]: clients_) {
                    if (!ghost_spectator_clients_.contains(client))
                        send(client, ball_msg.raw.str().data(), ball_msg.size());
                }
            }
        }
        return true;
    }

    void print_clients(bool print_uuid = false) {
        std::lock_guard lk(state_mutex_);
        decltype(username_) spectators;
        int max_name_length = 0;
        for (const auto& i: username_)
            max_name_length = std::max(max_name_length, (int) i.first.length());
        max_name_length = std::min((int) bmmo::name_validator::max_length + 1, max_name_length);
        // note: must not be a `static` lambda - it captures locals by reference,
        // and a static lambda would keep dangling references after the first call
        const auto print_client = [&](HSteamNetConnection id, client_data& data) {
            SteamNetConnectionRealTimeStatus_t status{};
            interface_->GetConnectionRealTimeStatus(id, &status, 0, nullptr);
            char quality_str[32]{};
            // note: snprintf, not bmmo::Sprintf - a char array binds to the
            // format-string overload, which would silently format nothing here
            if (std::abs(status.m_flConnectionQualityLocal) != 1)
                std::snprintf(quality_str, sizeof(quality_str), "  %5.2f%% quality",
                        100 * status.m_flConnectionQualityLocal);
            Printf("%10u  %*s%s  %4dms%s %s%s%s",
                    id, -max_name_length, data.name,
                    print_uuid ? ("  " + bmmo::string_utils::get_uuid_string(data.uuid)) : "",
                    status.m_nPing, quality_str,
                    data.cheated ? " [CHEAT]" : "", is_op(id) ? " [OP]" : "",
                    is_muted(data.uuid) ? " [Muted]" : "");
        };
        // find(), not operator[]: a stale username_ entry would otherwise
        // insert a blank client into clients_ and corrupt the online count
        for (const auto& i: username_) {
            auto client_it = clients_.find(i.second);
            if (client_it == clients_.end())
                continue;
            if (bmmo::name_validator::is_spectator(i.first))
                spectators.insert(i);
            else
                print_client(i.second, client_it->second);
        }
        for (const auto& i: spectators)
            if (auto client_it = clients_.find(i.second); client_it != clients_.end())
                print_client(i.second, client_it->second);
        Printf("%d client(s) online: %d player(s), %d spectator(s).",
            clients_.size(), clients_.size() - spectators.size(), spectators.size());
    }

    void print_maps() {
        std::lock_guard lk(state_mutex_);
        std::multimap<decltype(map_names_)::mapped_type, decltype(map_names_)::key_type> map_names_inverted;
        for (const auto& [hash, name]: map_names_) map_names_inverted.emplace(name, hash);
        for (const auto& [name, hash]: map_names_inverted) {
            std::string hash_string;
            bmmo::string_from_hex_chars(hash_string, reinterpret_cast<const uint8_t*>(hash.c_str()), sizeof(bmmo::map::md5));
            Printf("%s: %s", hash_string, name);
        }
    }

    void print_player_maps() {
        std::lock_guard lk(state_mutex_);
        for (const auto& [_, id]: username_) {
            auto client_it = clients_.find(id);
            if (client_it == clients_.end()) continue;
            const auto& data = client_it->second;
            Printf("%s(#%u, %s) is at the %d%s sector of %s.",
                data.cheated ? "[CHEAT] " : "", id, data.name,
                data.current_sector, bmmo::string_utils::get_ordinal_suffix(data.current_sector),
                data.current_map.get_display_name(map_names_));
        }
    }

    void print_positions() {
        std::lock_guard lk(state_mutex_);
        for (const auto& [_, id]: username_) {
            auto client_it = clients_.find(id);
            if (client_it == clients_.end()) continue;
            const auto& data = client_it->second;
            Printf("(%u, %s) is at %.2f, %.2f, %.2f with %s ball.",
                    id, data.name,
                    data.state.position.x, data.state.position.y, data.state.position.z,
                    data.state.get_type_name()
            );
        }
    }

    void print_scores(bool hs_mode, bmmo::map map) {
        auto ranks = get_map_rankings(map);
        if (!ranks) {
            Printf(bmmo::ansi::BrightRed, "Error: ranking info not found for the specified map.");
            return;
        }
        bmmo::ranking_entry::sort_rankings(*ranks, hs_mode);
        std::string map_name;
        {
            std::lock_guard lk(state_mutex_);
            map_name = map.get_display_name(map_names_);
        }
        auto formatted_texts = bmmo::ranking_entry::get_formatted_rankings(*ranks, map_name, hs_mode);
        for (const auto& [line, color]: formatted_texts)
            Printf(color, line.c_str());
    }

    static void print_version_info() {
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
        std::lock_guard lk(state_mutex_);
        for (const auto& i: clients_) {
            if (i.second.state.timestamp.is_zero())
                continue;
            balls.emplace_back(i.second.state, i.first);
        }
    }

    inline void pull_unupdated_ball_states(std::vector<bmmo::owned_timed_ball_state>& balls, std::vector<bmmo::owned_timestamp>& unchanged_balls) {
        std::lock_guard lk(state_mutex_);
        for (auto& i: clients_) {
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

    // ---- collision-overhaul room system (docs/rooms-and-sessions-protocol.md) ----
    // All of these run with state_mutex_ held (from on_message or a console command).

    void send_room_event(HSteamNetConnection to, bmmo::room::event_type type,
            bmmo::room::error_code error, uint32_t room,
            HSteamNetConnection actor = k_HSteamNetConnection_Invalid,
            HSteamNetConnection subject = k_HSteamNetConnection_Invalid,
            const std::string& reason = {}) {
        bmmo::room_event_msg msg;
        msg.type = type; msg.error = error; msg.room = room;
        msg.actor = actor; msg.subject = subject; msg.reason = reason;
        msg.serialize();
        send(to, msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
    }

    bmmo::room_state_msg build_room_state(HSteamNetConnection recipient) {
        bmmo::room_state_msg st;
        st.own_room = rooms_.room_of(recipient);
        for (const auto& [id, r]: rooms_.rooms()) {
            bmmo::room::room_info info;
            info.id = r.id; info.name = r.name; info.host = r.host;
            info.member_count = static_cast<uint16_t>(r.members.size());
            info.capacity = r.capacity; info.room_phase = r.phase; info.room_mode = r.mode;
            st.rooms.push_back(std::move(info));
        }
        if (st.own_room != 0) {
            if (const auto* r = rooms_.find(st.own_room)) {
                for (const auto& m: r->members) {
                    bmmo::room::room_member mm;
                    mm.id = m.id; mm.ready = m.ready; mm.is_host = (m.id == r->host);
                    auto ci = clients_.find(m.id);
                    if (ci != clients_.end()) { mm.name = ci->second.name; mm.map = ci->second.current_map; }
                    st.members.push_back(std::move(mm));
                }
            }
        }
        return st;
    }

    void send_room_state(HSteamNetConnection to) {
        auto st = build_room_state(to);
        st.serialize();
        send(to, st.raw.str().data(), st.size(), k_nSteamNetworkingSend_Reliable);
    }

    void send_room_state_to_room(uint32_t room) {
        const auto* r = rooms_.find(room);
        if (!r) return;
        for (const auto& m: r->members) send_room_state(m.id);
    }

    // Room membership / list changed: refresh everyone's view (rare events).
    void broadcast_room_states() {
        for (const auto& [conn, _]: clients_) send_room_state(conn);
    }

    bool room_members_same_map(const bmmo::server_room& r) {
        const bmmo::map* first = nullptr;
        for (const auto& m: r.members) {
            auto ci = clients_.find(m.id);
            if (ci == clients_.end()) continue;
            if (!first) first = &ci->second.current_map;
            else if (ci->second.current_map != *first) return false;
        }
        return true;
    }

    // Removes a client from its room (voluntary Leave or a disconnect) and
    // notifies the remaining members. `ack` sends the leaver a RequestAccepted.
    void room_remove_and_notify(HSteamNetConnection c, bool ack) {
#if BMMO_BUILD_SIM
        physics_session_member_left(c);
#endif
        auto rr = rooms_.leave(c);
        if (!rr.was_member) {
            if (ack) send_room_event(c, bmmo::room::event_type::RequestDenied,
                    bmmo::room::error_code::NotInRoom, 0);
            return;
        }
        if (ack && clients_.contains(c))
            send_room_event(c, bmmo::room::event_type::RequestAccepted,
                    bmmo::room::error_code::None, rr.room);
        for (auto m: rr.remaining) {
            send_room_event(m, bmmo::room::event_type::PlayerLeft,
                    bmmo::room::error_code::None, rr.room, c, c);
            if (rr.new_host != k_HSteamNetConnection_Invalid)
                send_room_event(m, bmmo::room::event_type::HostChanged,
                        bmmo::room::error_code::None, rr.room, rr.new_host);
        }
        broadcast_room_states();
    }

#if BMMO_BUILD_SIM
    // ---- physics sessions (design section 8.3) ----
    // All of these run with state_mutex_ held; the runner callbacks take it
    // themselves because they arrive on the simulation thread.

    struct physics_session_state {
        uint32_t id = 0, room = 0;
        bmmo::map map{};
        std::vector<HSteamNetConnection> members;           // join order
        std::set<HSteamNetConnection> ready;
        std::set<HSteamNetConnection> assigned;             // got their SessionAssign
        std::set<HSteamNetConnection> late;                 // joined a running session: no hash check
        bool world_ready = false, ticking = false;
        uint64_t anchor_hash = 0, anchor_surfaces = 0;
        float spawn_position[3] = {}, spawn_rotation[4] = {0, 0, 0, 1};
        // last Physicalize event of each member (serialized, player set) so a
        // late joiner can build the bodies that already exist
        std::map<HSteamNetConnection, std::string> last_physicalize;
        // Event validation (design 9.4): rate limit rejects, the rest is
        // logged and counted in M4 (no rejection: the retail scripts decide
        // respawn poses per player, the server only knows the union).
        struct member_guard {
            int sector = 1;
            uint32_t events_in_window = 0;
            std::chrono::steady_clock::time_point window_start{};
            double last_position[3] = {};
            bool have_position = false;
        };
        std::map<HSteamNetConnection, member_guard> guards;
        std::map<HSteamNetConnection, std::array<float, 3>> spawns;   // ring slot per member
        uint64_t rejected_events = 0, flagged_events = 0;
    };

    void init_physics_runner() {
        if (!config_.physics_enabled) return;
        if (config_.physics_game_root.empty()) {
            Printf("Physics sessions: physics.game_root is not set; sessions stay unavailable.");
            return;
        }
        std::error_code ec;
        const std::filesystem::path root(config_.physics_game_root);
        if (!std::filesystem::is_regular_file(root / "base.cmo", ec)) {
            Printf("Physics sessions: %s does not contain base.cmo; sessions stay unavailable.",
                    config_.physics_game_root);
            return;
        }
        bmmo::sim::runner_config rc;
        rc.game_root = root;
        rc.input_delay = config_.physics_input_delay;
        rc.snapshot_interval = std::max<uint32_t>(1, config_.physics_snapshot_interval);
        rc.trace = config_.physics_debug_trace;
        bmmo::sim::session_callbacks callbacks;
        callbacks.log = [](const std::string& text) { Printf("[Sim] %s", text); };
        callbacks.on_world_ready = [this](const bmmo::sim::world_ready_info& info) { on_world_ready(info); };
        callbacks.on_snapshot = [this](const bmmo::sim::session_snapshot& snapshot) { on_session_snapshot(snapshot); };
        callbacks.on_inputs = [this](uint32_t session, uint32_t tick,
                                     const std::vector<std::pair<uint32_t, bmmo::session::input_frame>>& applied) {
            on_session_inputs(session, tick, applied);
        };
        callbacks.on_failed = [this](uint32_t session, const std::string& reason) {
            std::lock_guard lk(state_mutex_);
            end_physics_session(session, "simulation failed: " + reason);
        };
        runner_ = std::make_unique<bmmo::sim::session_runner>(rc, callbacks);
        Printf("Physics sessions enabled (game root %s, input delay %u ticks, snapshot interval %u).",
                config_.physics_game_root, rc.input_delay, rc.snapshot_interval);
    }

    bmmo::room::error_code check_physics_mods(const bmmo::server_room& r) {
        if (config_.physics_allowed_mods.empty()) return bmmo::room::error_code::None;
        for (const auto& m: r.members) {
            auto ci = clients_.find(m.id);
            if (ci == clients_.end()) continue;
            for (const auto& [id, version]: ci->second.mods) {
                auto allowed = config_.physics_allowed_mods.find(id);
                if (allowed == config_.physics_allowed_mods.end() || allowed->second != version) {
                    Printf("Physics session denied: %s has %s %s (not whitelisted).", ci->second.name, id, version);
                    return bmmo::room::error_code::ModMismatch;
                }
            }
        }
        return bmmo::room::error_code::None;
    }

    uint32_t start_physics_session(const bmmo::server_room& r, const bmmo::map& map) {
        physics_session_state s;
        s.id = next_session_id_++;
        s.room = r.id;
        s.map = map;
        for (const auto& m: r.members) {
            s.members.push_back(m.id);
            client_session_[m.id] = s.id;
        }
        room_session_[r.id] = s.id;
        const uint32_t id = s.id;
        physics_sessions_.emplace(id, std::move(s));
        runner_->create_session(id, map.level, physics_sessions_[id].members);
        return id;
    }

    void send_session_end(uint32_t session, HSteamNetConnection to, const std::string& reason) {
        bmmo::session_end_msg msg;
        msg.session = session;
        msg.reason = reason;
        msg.serialize();
        send(to, msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
    }

    void end_physics_session(uint32_t session, const std::string& reason) {
        auto it = physics_sessions_.find(session);
        if (it == physics_sessions_.end()) return;
        auto s = std::move(it->second);
        physics_sessions_.erase(it);
        for (const auto m: s.members) {
            client_session_.erase(m);
            send_session_end(session, m, reason);
            send_room_event(m, bmmo::room::event_type::SessionEnded, bmmo::room::error_code::None, s.room,
                    k_HSteamNetConnection_Invalid, k_HSteamNetConnection_Invalid, reason);
        }
        room_session_.erase(s.room);
        rooms_.reset_session(s.room);
        if (runner_) runner_->destroy_session(session);
        broadcast_room_states();
        Printf("Physics session %u (room %u) ended: %s", session, s.room, reason);
    }

    void send_session_start(physics_session_state& s, HSteamNetConnection to, uint32_t first_tick) {
        bmmo::session_start_msg msg;
        msg.room = s.room;
        msg.session = s.id;
        msg.mode = bmmo::room::mode::Physics;
        msg.map = s.map;
        msg.tick_rate = 66;
        msg.snapshot_interval = static_cast<uint8_t>(std::min<uint32_t>(255, std::max<uint32_t>(1, config_.physics_snapshot_interval)));
        msg.input_delay = static_cast<uint8_t>(std::min<uint32_t>(255, config_.physics_input_delay));
        msg.first_tick = first_tick;
        msg.seed = 1;
        // Spawn ring around the retail spawn (design 3.4): a single player keeps
        // the retail spot so solo play reproduces the recording exactly.
        const size_t count = s.members.size();
        const float radius = count > 1 ? 6.0f : 0.0f;
        for (size_t i = 0; i < count && i < bmmo::session::MAX_PLAYERS_PER_SESSION; ++i) {
            bmmo::session::player_entry entry;
            entry.id = s.members[i];
            entry.join_order = static_cast<uint8_t>(i);
            entry.ball_type = 0;
            const double angle = 2.0 * 3.14159265358979323846 * static_cast<double>(i) / static_cast<double>(count);
            entry.spawn_position[0] = s.spawn_position[0] + radius * static_cast<float>(std::cos(angle));
            entry.spawn_position[1] = s.spawn_position[1];
            entry.spawn_position[2] = s.spawn_position[2] + radius * static_cast<float>(std::sin(angle));
            for (int k = 0; k < 4; ++k) entry.spawn_rotation[k] = s.spawn_rotation[k];
            for (int k = 0; k < 3; ++k) s.spawns[entry.id][k] = entry.spawn_position[k];
            msg.players.push_back(entry);
        }
        msg.serialize();
        send(to, msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
    }

    // Simulation thread: the world finished booting.
    void on_world_ready(const bmmo::sim::world_ready_info& info) {
        std::lock_guard lk(state_mutex_);
        auto it = physics_sessions_.find(info.session);
        if (it == physics_sessions_.end()) return;
        if (!info.ok) {
            end_physics_session(info.session, "world boot failed: " + info.error);
            return;
        }
        auto& s = it->second;
        s.world_ready = true;
        s.anchor_hash = info.anchor_hash;
        s.anchor_surfaces = info.anchor_surfaces;
        for (int k = 0; k < 3; ++k) s.spawn_position[k] = info.spawn_position[k];
        for (int k = 0; k < 4; ++k) s.spawn_rotation[k] = info.spawn_rotation[k];
        for (const auto m: s.members) send_session_start(s, m, 0);
        Printf("Physics session %u: world ready (anchor %016llx), SessionStart sent to %zu players.",
                s.id, static_cast<unsigned long long>(s.anchor_hash), s.members.size());
    }

    // Simulation thread: fan a snapshot out to the room.
    // Relay of the inputs the world applied at `tick` (design 9.1): every
    // member gets the other members' frames, unreliable, one message per tick.
    void on_session_inputs(uint32_t session, uint32_t tick,
                           const std::vector<std::pair<uint32_t, bmmo::session::input_frame>>& applied) {
        std::lock_guard lk(state_mutex_);
        auto it = physics_sessions_.find(session);
        if (it == physics_sessions_.end() || applied.size() < 2) return;
        auto& s = it->second;
        for (const auto m: s.members) {
            bmmo::session_remote_input_msg msg;
            msg.session = session;
            msg.tick = tick;
            for (const auto& [player, frame]: applied) {
                if (player == m) continue;
                bmmo::session_remote_input_msg::entry e;
                e.player = player;
                e.frame = frame;
                msg.entries.push_back(e);
            }
            if (msg.entries.empty()) continue;
            msg.serialize();
            send(m, msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_UnreliableNoDelay);
        }
    }

    void on_session_snapshot(const bmmo::sim::session_snapshot& snapshot) {
        std::lock_guard lk(state_mutex_);
        auto it = physics_sessions_.find(snapshot.session);
        if (it == physics_sessions_.end()) return;
        auto& s = it->second;
        if (!s.ticking) {
            s.ticking = true;
            Printf("Physics session %u: ticking (first snapshot at tick %u).", s.id, snapshot.tick);
        }
        for (const auto& body: snapshot.bodies) {
            if (body.kind != bmmo::session::body_kind::Ball) continue;
            auto& guard = s.guards[body.owner];
            for (int k = 0; k < 3; ++k) guard.last_position[k] = body.position[k];
            guard.have_position = true;
        }
        for (const auto m: s.members) {
            bmmo::session_snapshot_msg msg;
            msg.session = snapshot.session;
            msg.tick = snapshot.tick;
            msg.full = snapshot.full ? 1 : 0;
            msg.acked_input_tick = 0;
            for (const auto& [player, tick]: snapshot.acked_inputs)
                if (player == m) msg.acked_input_tick = tick;
            msg.bodies = snapshot.bodies;
            msg.serialize();
            send(m, msg.raw.str().data(), msg.size(),
                    snapshot.full ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_UnreliableNoDelay);
        }
    }

    void physics_session_member_joined(uint32_t room, HSteamNetConnection c) {
        auto rs = room_session_.find(room);
        if (rs == room_session_.end() || !runner_) return;
        auto it = physics_sessions_.find(rs->second);
        if (it == physics_sessions_.end()) return;
        auto& s = it->second;
        s.members.push_back(c);
        client_session_[c] = s.id;
        runner_->add_player(s.id, c);
        if (runner_->running(s.id)) {
            s.late.insert(c);
            send_session_start(s, c, 0);   // the real tick base follows in SessionAssign
        } else if (s.world_ready) {
            send_session_start(s, c, 0);
        }
        Printf("Physics session %u: %s joined%s.", s.id, get_client_name(c), s.late.count(c) ? " late" : "");
    }

    void physics_session_member_left(HSteamNetConnection c) {
        auto cs = client_session_.find(c);
        if (cs == client_session_.end()) return;
        const uint32_t session = cs->second;
        client_session_.erase(cs);
        auto it = physics_sessions_.find(session);
        if (it == physics_sessions_.end()) return;
        auto& s = it->second;
        s.members.erase(std::remove(s.members.begin(), s.members.end(), c), s.members.end());
        s.ready.erase(c);
        s.assigned.erase(c);
        s.late.erase(c);
        if (s.last_physicalize.erase(c) && runner_) {
            // The others still mirror this ball: relay an Unphysicalize on the
            // leaver's behalf (design 9.3).
            bmmo::session_event_msg gone;
            gone.session = session;
            gone.player = c;
            gone.tick = runner_->current_tick(session);
            gone.type = bmmo::session::event_type::Unphysicalize;
            gone.serialize();
            for (const auto m: s.members)
                send(m, gone.raw.str().data(), gone.size(), k_nSteamNetworkingSend_Reliable);
        }
        if (runner_) runner_->remove_player(session, c);
        if (s.members.empty()) end_physics_session(session, "everyone left");
        else assign_start_members(s);   // the leaver may have been the one everybody waited for
    }

    uint32_t late_tick_base(const physics_session_state& s) const {
        return runner_->current_tick(s.id) + std::max<uint32_t>(1, config_.physics_input_delay) + 2;
    }

    void send_session_assign(physics_session_state& s, HSteamNetConnection c, uint32_t first_tick) {
        bmmo::session_assign_msg assign;
        assign.session = s.id;
        assign.first_tick = first_tick;
        assign.serialize();
        send(c, assign.raw.str().data(), assign.size(), k_nSteamNetworkingSend_Reliable);
        s.assigned.insert(c);
    }

    // Tick 0 for every start member, sent together once the last of them is
    // ready: a client that anchored early would otherwise run ahead of the
    // server by the time it spent waiting, and the relay lag (design 9.1)
    // grows by the same amount.
    void assign_start_members(physics_session_state& s) {
        for (const auto m: s.members)
            if (!s.late.count(m) && !s.ready.count(m)) return;
        for (const auto m: s.members)
            if (!s.late.count(m) && !s.assigned.count(m)) send_session_assign(s, m, 0);
    }

    void handle_session_ready(client_data_collection::iterator client_it, const bmmo::session_ready_msg& msg) {
        const HSteamNetConnection c = client_it->first;
        auto cs = client_session_.find(c);
        if (cs == client_session_.end() || cs->second != msg.session || !runner_) return;
        auto& s = physics_sessions_[msg.session];
        if (!config_.physics_require_sha.empty() && msg.physics_sha256 != config_.physics_require_sha
                && msg.physics_sha256.rfind("headless-", 0) != 0) {
            end_physics_session(msg.session, Sprintf("%s runs physics_RT %s, this server requires %s",
                    client_it->second.name, msg.physics_sha256.substr(0, 12), config_.physics_require_sha.substr(0, 12)));
            return;
        }
        if (!s.late.count(c) && s.world_ready
                && (msg.anchor_hash != s.anchor_hash || msg.anchor_surfaces != s.anchor_surfaces)) {
            end_physics_session(msg.session, Sprintf("world mismatch for %s (client %016llx/%016llx, server %016llx/%016llx)",
                    client_it->second.name,
                    static_cast<unsigned long long>(msg.anchor_hash), static_cast<unsigned long long>(msg.anchor_surfaces),
                    static_cast<unsigned long long>(s.anchor_hash), static_cast<unsigned long long>(s.anchor_surfaces)));
            return;
        }
        s.ready.insert(c);
        // Late joiners are numbered from the server's current tick; members
        // present at the start from 0 (protocol 2.2, session_assign_msg).
        // A late joiner starts input_delay ahead of the server like everybody
        // else (the server simulates tick T only after the inputs for T), so
        // every snapshot refers to a tick the client has already recorded.
        const uint32_t assigned = s.late.count(c) ? late_tick_base(s) : 0;
        runner_->player_ready(msg.session, c, assigned);
        if (s.late.count(c)) send_session_assign(s, c, assigned);
        else assign_start_members(s);
        Printf("Physics session %u: %s ready (assigned tick %u; physics %s, %s).", s.id, client_it->second.name,
                assigned, msg.physics_sha256.substr(0, 12), msg.build_id);
        if (s.late.count(c)) {
            // catch the late joiner up: everyone's current ball, then a full snapshot
            for (const auto& [member, bytes]: s.last_physicalize)
                if (member != c) send(c, bytes.data(), bytes.size(), k_nSteamNetworkingSend_Reliable);
            runner_->request_full_snapshot(s.id);
        }
    }

    // Resync (design 9.2): the client's tick numbering broke (pause, long
    // stall, repeated hard corrections).  Same path as a late join: current
    // tick as the new base, everyone's ball, then a full snapshot.
    void handle_session_resync(HSteamNetConnection c, const bmmo::session_resync_msg& msg) {
        auto cs = client_session_.find(c);
        if (cs == client_session_.end() || cs->second != msg.session || !runner_) return;
        auto it = physics_sessions_.find(msg.session);
        if (it == physics_sessions_.end()) return;
        auto& s = it->second;
        if (!runner_->running(s.id)) return;
        const uint32_t assigned = late_tick_base(s);
        s.late.insert(c);
        s.ready.insert(c);
        runner_->player_ready(s.id, c, assigned);
        send_session_assign(s, c, assigned);
        for (const auto& [member, bytes]: s.last_physicalize)
            if (member != c) send(c, bytes.data(), bytes.size(), k_nSteamNetworkingSend_Reliable);
        runner_->request_full_snapshot(s.id);
        Printf("Physics session %u: %s resynced at tick %u (last full snapshot it had: %u).", s.id, get_client_name(c),
               assigned, msg.last_full_tick);
    }

    void handle_session_input(HSteamNetConnection c, const bmmo::session_input_msg& msg) {
        auto cs = client_session_.find(c);
        if (cs == client_session_.end() || cs->second != msg.session || !runner_) return;
        runner_->submit_input(msg.session, c, msg.first_tick, msg.frames);
    }

    static void copy_name(char* out, size_t size, const std::string& text) {
        std::snprintf(out, size, "%s", text.c_str());
    }

    void handle_session_event(client_data_collection::iterator client_it, bmmo::session_event_msg& msg) {
        const HSteamNetConnection c = client_it->first;
        auto cs = client_session_.find(c);
        if (cs == client_session_.end() || cs->second != msg.session || !runner_) return;
        auto& s = physics_sessions_[msg.session];
        bmmo::sim::lifecycle_event event;
        event.type = msg.type;
        event.ball_type = msg.ball_type;
        for (int k = 0; k < 3; ++k) event.position[k] = msg.position[k];
        for (int k = 0; k < 9; ++k) event.rotation[k] = msg.rotation[k];
        event.sector = msg.sector;
        event.name = msg.name;
        if (msg.type == bmmo::session::event_type::Physicalize) {
            const auto& r = msg.recipe;
            auto& p = event.recipe;
            p.fixed = r.fixed; p.start_frozen = r.start_frozen; p.enable_collision = r.enable_collision;
            p.calc_mass_center = r.calc_mass_center;
            p.friction = r.friction; p.elasticity = r.elasticity; p.mass = r.mass;
            p.linear_damp = r.linear_damp; p.rot_damp = r.rot_damp;
            for (int k = 0; k < 3; ++k) p.mass_center[k] = r.mass_center[k];
            copy_name(p.collision_surface, sizeof(p.collision_surface), r.collision_surface);
            p.convex_count = static_cast<int32_t>(std::min<size_t>(r.convex_meshes.size(), BMMO_PHYSICS_MAX_CONVEX));
            for (int i = 0; i < p.convex_count; ++i) copy_name(p.convex[i], sizeof(p.convex[i]), r.convex_meshes[i]);
            p.ball_count = static_cast<int32_t>(std::min<size_t>(r.balls.size(), BMMO_PHYSICS_MAX_BALLS));
            for (int i = 0; i < p.ball_count; ++i) {
                for (int k = 0; k < 3; ++k) p.ball_center[i][k] = r.balls[i].center[k];
                p.ball_radius[i] = r.balls[i].radius;
            }
            p.concave_count = static_cast<int32_t>(std::min<size_t>(r.concave_meshes.size(), BMMO_PHYSICS_MAX_CONCAVE));
            for (int i = 0; i < p.concave_count; ++i) copy_name(p.concave[i], sizeof(p.concave[i]), r.concave_meshes[i]);
        }
        {
            // Design 9.4.  Rate limit: hard reject.  Everything else: log + count.
            auto& guard = s.guards[c];
            const auto now = std::chrono::steady_clock::now();
            if (now - guard.window_start > std::chrono::seconds(1)) {
                guard.window_start = now;
                guard.events_in_window = 0;
            }
            if (++guard.events_in_window > 20) {
                if (guard.events_in_window == 21) {
                    ++s.rejected_events;
                    Printf("Physics session %u: %s sends more than 20 events per second; dropping the excess.",
                           s.id, client_it->second.name);
                }
                return;
            }
            auto flag = [&](const std::string& why) {
                ++s.flagged_events;
                if (s.flagged_events <= 50)
                    Printf("Physics session %u: suspicious event from %s at tick %u: %s.", s.id,
                           client_it->second.name, msg.tick, why);
            };
            if (msg.type == bmmo::session::event_type::Physicalize) {
                const auto& r = msg.recipe;
                const bool numbers_ok = r.mass > 0.0f && r.mass <= 100.0f && r.friction >= 0.0f && r.friction <= 10.0f
                    && r.elasticity >= 0.0f && r.elasticity <= 10.0f && r.linear_damp >= 0.0f && r.linear_damp <= 1.0f
                    && r.rot_damp >= 0.0f && r.rot_damp <= 1.0f && r.balls.size() + r.convex_meshes.size() > 0;
                if (msg.ball_type > 2 || !numbers_ok) {
                    ++s.rejected_events;
                    Printf("Physics session %u: rejected Physicalize from %s (ball type %u, malformed recipe).", s.id,
                           client_it->second.name, msg.ball_type);
                    return;
                }
                auto distance = [&](const double* a, const float* b) {
                    double d = 0.0;
                    for (int k = 0; k < 3; ++k) d += (a[k] - b[k]) * (a[k] - b[k]);
                    return std::sqrt(d);
                };
                const double pos[3] = {msg.position[0], msg.position[1], msg.position[2]};
                bool near_spawn = false;
                for (const auto& [member, slot]: s.spawns) near_spawn = near_spawn || distance(pos, slot.data()) <= 2.5;
                bool near_last = false;
                if (guard.have_position) {
                    const float last[3] = {static_cast<float>(guard.last_position[0]),
                                           static_cast<float>(guard.last_position[1]),
                                           static_cast<float>(guard.last_position[2])};
                    near_last = distance(pos, last) <= 5.0;
                }
                if (!near_spawn && !near_last)
                    flag("Physicalize pose far from every spawn slot and from the player's last known position");
            } else if (msg.type == bmmo::session::event_type::Sector) {
                if (msg.sector < 1 || msg.sector > guard.sector + 1)
                    flag("sector " + std::to_string(msg.sector) + " after sector " + std::to_string(guard.sector));
                guard.sector = std::max(guard.sector, msg.sector);
            }
        }
        runner_->submit_event(msg.session, c, msg.tick, std::move(event));
        // relay to the other members with the origin filled in
        msg.player = c;
        msg.clear();
        msg.serialize();
        const std::string bytes = msg.raw.str();
        if (msg.type == bmmo::session::event_type::Physicalize) s.last_physicalize[c] = bytes;
        else if (msg.type == bmmo::session::event_type::Unphysicalize) s.last_physicalize.erase(c);
        for (const auto m: s.members)
            if (m != c) send(m, bytes.data(), bytes.size(), k_nSteamNetworkingSend_Reliable);
    }

    void print_physics_sessions() {
        std::lock_guard lk(state_mutex_);
        if (!runner_) { Printf("Physics sessions are unavailable."); return; }
        if (physics_sessions_.empty()) Printf("No physics session is running.");
        for (const auto& [id, s]: physics_sessions_) {
            Printf("Session %u: room %u, level %d, %zu members, %zu ready, world %s, ticking %s, tick %u, "
                   "events flagged %llu / rejected %llu.",
                    id, s.room, s.map.level, s.members.size(), s.ready.size(), s.world_ready ? "ready" : "booting",
                    s.ticking ? "yes" : "no", runner_->current_tick(id),
                    static_cast<unsigned long long>(s.flagged_events), static_cast<unsigned long long>(s.rejected_events));
            runner_->describe(id);
        }
    }
#endif

    void handle_room_request(client_data_collection::iterator client_it,
            const bmmo::room_request_msg& msg) {
        using bmmo::room::action;
        using bmmo::room::event_type;
        using bmmo::room::error_code;
        const HSteamNetConnection c = client_it->first;
        rooms_.max_rooms = config_.maximum_rooms;
        rooms_.max_members = static_cast<uint16_t>(std::clamp<uint32_t>(
                config_.maximum_members, 1, bmmo::room::MAX_MEMBERS_PER_MESSAGE));
        if (!config_.rooms_enabled) {
            send_room_event(c, event_type::RequestDenied, error_code::Unsupported, 0);
            return;
        }
        switch (msg.action) {
            case action::List:
                send_room_state(c);
                break;
            case action::Create: {
                std::string name = msg.name;
                bmmo::string_utils::sanitize_string(name);
                if (name.size() > bmmo::room::MAX_ROOM_NAME) name.resize(bmmo::room::MAX_ROOM_NAME);
                uint32_t id = 0;
                auto err = rooms_.create(c, name, id);
                if (err != error_code::None) {
                    send_room_event(c, event_type::RequestDenied, err, 0);
                    break;
                }
                if (name.empty())
                    if (auto* r = rooms_.find(id)) r->name = "Room #" + std::to_string(id);
                send_room_event(c, event_type::RequestAccepted, error_code::None, id, c);
                broadcast_room_states();
                Printf("%s (#%u) created room %u.", client_it->second.name, c, id);
                break;
            }
            case action::Join: {
                auto err = rooms_.join(c, msg.room);
                if (err != error_code::None) {
                    send_room_event(c, event_type::RequestDenied, err, msg.room);
                    break;
                }
                send_room_event(c, event_type::RequestAccepted, error_code::None, msg.room, c);
#if BMMO_BUILD_SIM
                physics_session_member_joined(msg.room, c);
#endif
                if (const auto* r = rooms_.find(msg.room))
                    for (const auto& m: r->members)
                        if (m.id != c)
                            send_room_event(m.id, event_type::PlayerJoined, error_code::None, msg.room, c, c);
                broadcast_room_states();
                break;
            }
            case action::Leave:
                room_remove_and_notify(c, true);
                break;
            case action::Ready:
            case action::Unready: {
                auto err = rooms_.set_ready(c, msg.action == action::Ready);
                if (err != error_code::None) {
                    send_room_event(c, event_type::RequestDenied, err, 0);
                    break;
                }
                const uint32_t room = rooms_.room_of(c);
                if (const auto* r = rooms_.find(room))
                    for (const auto& m: r->members)
                        send_room_event(m.id, event_type::ReadyChanged, error_code::None, room, c, c);
                send_room_state_to_room(room);
                break;
            }
            case action::Start: {
                const uint32_t room = rooms_.room_of(c);
                auto* r = rooms_.find(room);
                if (!r) { send_room_event(c, event_type::RequestDenied, error_code::NotInRoom, 0); break; }
                if (r->host != c) { send_room_event(c, event_type::RequestDenied, error_code::NotHost, room); break; }
                if (!r->all_ready()) { send_room_event(c, event_type::RequestDenied, error_code::NotReady, room); break; }
                if (!room_members_same_map(*r)) { send_room_event(c, event_type::RequestDenied, error_code::MapMismatch, room); break; }
                if (msg.mode == bmmo::room::mode::Physics) {
#if BMMO_BUILD_SIM
                    if (!runner_) { send_room_event(c, event_type::RequestDenied, error_code::PhysicsUnavailable, room); break; }
                    const int level = client_it->second.current_map.level;
                    if (!client_it->second.current_map.is_original_level() || level < 1 || level > 13) {
                        send_room_event(c, event_type::RequestDenied, error_code::MapMismatch, room, c, 0,
                                "physics sessions need an original level");
                        break;
                    }
                    if (auto mods = check_physics_mods(*r); mods != error_code::None) {
                        send_room_event(c, event_type::RequestDenied, mods, room);
                        break;
                    }
                    if (auto existing = room_session_.find(room); existing != room_session_.end()) {
                        end_physics_session(existing->second, "restarted by the host");
                        // reset_session() un-readied everyone; the restart was
                        // validated against the ready set a moment ago, keep it.
                        for (auto& m: r->members) m.ready = true;
                    }
                    if (physics_sessions_.size() >= config_.maximum_physics_rooms) {
                        send_room_event(c, event_type::RequestDenied, error_code::ServerBusy, room, c, 0,
                                "no free physics world");
                        break;
                    }
                    auto err = rooms_.start(c, msg.mode);
                    if (err != error_code::None) { send_room_event(c, event_type::RequestDenied, err, room); break; }
                    const uint32_t session = start_physics_session(*r, client_it->second.current_map);
                    send_room_event(c, event_type::RequestAccepted, error_code::None, room, c);
                    for (const auto& m: r->members)
                        send_room_event(m.id, event_type::SessionStarting, error_code::None, room, c);
                    broadcast_room_states();
                    Printf("Room %u started physics session %u on level %d (world booting).", room, session, level);
#else
                    send_room_event(c, event_type::RequestDenied, error_code::PhysicsUnavailable, room);
#endif
                    break;
                }
                auto err = rooms_.start(c, msg.mode);
                if (err != error_code::None) { send_room_event(c, event_type::RequestDenied, err, room); break; }
                send_room_event(c, event_type::RequestAccepted, error_code::None, room, c);
                for (const auto& m: r->members)
                    send_room_event(m.id, event_type::SessionStarting, error_code::None, room, c);
                broadcast_room_states();
                Printf("Room %u started a shadow session.", room);
                break;
            }
            case action::Kick: {
                bmmo::room_manager::removal_result rr;
                auto err = rooms_.kick(c, msg.target, rr);
                if (err != error_code::None) {
                    send_room_event(c, event_type::RequestDenied, err, rooms_.room_of(c));
                    break;
                }
#if BMMO_BUILD_SIM
                physics_session_member_left(msg.target);
#endif
                send_room_event(msg.target, event_type::Kicked, error_code::None, rr.room, c, msg.target);
                for (auto m: rr.remaining) {
                    send_room_event(m, event_type::PlayerLeft, error_code::None, rr.room, c, msg.target);
                    if (rr.new_host != k_HSteamNetConnection_Invalid)
                        send_room_event(m, event_type::HostChanged, error_code::None, rr.room, rr.new_host);
                }
                send_room_event(c, event_type::RequestAccepted, error_code::None, rr.room, c, msg.target);
                broadcast_room_states();
                break;
            }
            case action::Close: {
                std::vector<HSteamNetConnection> members;
                uint32_t room = 0;
#if BMMO_BUILD_SIM
                if (const uint32_t own = rooms_.room_of(c); own != 0 && rooms_.find(own) && rooms_.find(own)->host == c)
                    if (auto existing = room_session_.find(own); existing != room_session_.end())
                        end_physics_session(existing->second, "room closed");
#endif
                auto err = rooms_.close(c, members, room);
                if (err != error_code::None) {
                    send_room_event(c, event_type::RequestDenied, err, rooms_.room_of(c));
                    break;
                }
                for (auto m: members)
                    send_room_event(m, event_type::RoomClosed, error_code::None, room, c);
                broadcast_room_states();
                Printf("Room %u closed by #%u.", room, c);
                break;
            }
            default:
                send_room_event(c, event_type::RequestDenied, error_code::Unsupported, 0);
                break;
        }
    }

    void set_ban(HSteamNetConnection client, const std::string& reason) {
        std::lock_guard lk(state_mutex_);
        if (!client_exists(client))
            return;
        const std::string uuid_string = bmmo::string_utils::get_uuid_string(clients_[client].uuid);
        config_.banned_players[uuid_string] = reason;
        Printf(bmmo::color_code(bmmo::OpState), "Banned %s (%s)%s.",
                clients_[client].name, uuid_string, reason.empty() ? "" : ": " + reason);
        kick_client(client, "Banned" + (reason.empty() ? "" : ": " + reason));
        config_.save();
    }

    void set_mute(HSteamNetConnection client, bool action) {
        std::lock_guard lk(state_mutex_);
        if (!client_exists(client))
            return;
        const std::string uuid_string = bmmo::string_utils::get_uuid_string(clients_[client].uuid);
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
        std::lock_guard lk(state_mutex_);
        if (!client_exists(client))
            return;
        std::string name = bmmo::name_validator::get_real_nickname(clients_[client].name);
        if (action) {
            if (auto it = config_.op_players.find(name); it != config_.op_players.end()) {
                if (it->second == bmmo::string_utils::get_uuid_string(clients_[client].uuid)) {
                    Printf("Error: client \"%s\" already has OP privileges.", name);
                    return;
                }
            }
            config_.op_players[name] = bmmo::string_utils::get_uuid_string(clients_[client].uuid);
            ghost_spectator_clients_.insert(client);
            Printf(bmmo::color_code(bmmo::OpState), "%s is now an operator.", name);
        } else {
            if (!config_.op_players.erase(name))
                return;
            ghost_spectator_clients_.erase(client);
            Printf(bmmo::color_code(bmmo::OpState), "%s is no longer an operator.", name);
        }
        config_.save();
        bmmo::op_state_msg msg{};
        msg.content.op = action;
        send(client, msg, k_nSteamNetworkingSend_Reliable);
        // just kick them and let them autoreconnect
        if (config_.ghost_mode) {
            interface_->CloseConnection(client, bmmo::connection_end::AutoReconnection_Min, "Operator status changed", true);
        }
    }

    void set_unban(const std::string& uuid_string) {
        std::lock_guard lk(state_mutex_);
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
        {
            std::lock_guard lk(state_mutex_);
            for (const auto& i: clients_) {
                interface_->CloseConnection(i.first, nReason, "Server closed", true);
            }
        }
        if (ticking_)
            stop_ticking();
        // give the close packets a moment to flush before stopping the loop
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        running_ = false;
        // the main thread is parked inside replxx waiting for a command and
        // would not notice running_ turning false until the user hit enter
        bmmo::console::end_input();
    }

    bool setup() override {
        Printf("Loading config from config.yml...");
        if (!load_config()) {
            Printf("Error: failed to load config. Please try fixing or emptying it first.");
            return false;
        }
#if BMMO_BUILD_SIM
        init_physics_runner();
#endif

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
                std::lock_guard lk(state_mutex_);
                std::vector<std::string> player_hints;
                player_hints.reserve(clients_.size());
                std::string last_word = args[args.size() - 1];
                auto last_separator = last_word.find_last_of(bmmo::console::valid_nonspace_delims);
                if (last_separator != std::string::npos)
                    last_word.erase(0, last_separator + 1);
                bool hint_client_id = last_word.starts_with('#');
                for (const auto& [id, data]: clients_) {
                    if (hint_client_id)
                        player_hints.emplace_back('#' + std::to_string(id));
                    else
                        player_hints.emplace_back(data.name);
                }
                return player_hints;
            }
        });

        mark_running();

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
        config_.save_login_data(pInfo.m_addrRemote, bmmo::string_utils::get_uuid_string(clients_[client].uuid),
                                clients_[client].name);
    }

    // Fail silently if the client doesn't exist (e.g. still in limbo state).
    // Caller must hold state_mutex_.
    void cleanup_disconnected_client(HSteamNetConnection client) {
        auto itClient = clients_.find(client);
        if (itClient == clients_.end())
            return;

        bmmo::player_disconnected_msg msg;
        msg.content.connection_id = client;
        broadcast_message(msg, k_nSteamNetworkingSend_Reliable, client);
        std::string name = itClient->second.name;
        username_.erase(bmmo::message_utils::to_lower(name));
        clients_.erase(itClient);
        ghost_spectator_clients_.erase(client);
        room_remove_and_notify(client, false); // notify roommates a member left

        Printf(bmmo::color_code(msg.code), "%s (#%u) disconnected.", name, client);

        switch (clients_.size()) {
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

    inline bool is_muted(const uint8_t* uuid) const {
        return config_.muted_players.contains(bmmo::string_utils::get_uuid_string(uuid));
    }

    bool is_op(HSteamNetConnection client) {
        if (!client_exists(client))
            return false;
        std::string name = bmmo::name_validator::get_real_nickname(clients_[client].name);
        auto op_it = config_.op_players.find(name);
        if (op_it == config_.op_players.end())
            return false;
        if (op_it->second == bmmo::string_utils::get_uuid_string(clients_[client].uuid))
            return true;
        return false;
    }

    bool op_online() {
        return std::ranges::any_of(clients_, [&](auto& client) { return is_op(client.first); });
    }

    bool process_forced_cheat_mode(std::pair<const HSteamNetConnection, client_data>& data, bool new_cheat_mode) {
        bool forced_cheat;
        if (!config_.get_forced_cheat_mode(bmmo::string_utils::get_uuid_string(data.second.uuid), forced_cheat)
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
        const std::string real_nickname = bmmo::name_validator::get_real_nickname(msg.nickname);

        // check if client is banned
        if (auto it = config_.banned_players.find(bmmo::string_utils::get_uuid_string(msg.uuid));
                it != config_.banned_players.end()) {
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
        // check if name is reserved for another player
        else if (config_.is_name_reserved(real_nickname, bmmo::string_utils::get_uuid_string(msg.uuid))) {
            reason << "The name \"" << real_nickname << "\" is reserved for another player.";
            nReason = bmmo::connection_end::ReservedName;
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
        std::lock_guard lk(state_mutex_);
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
        std::lock_guard lk(state_mutex_);
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
                if (!msg.deserialize()) {
                    // not connection_end::None: GNS turns a reason of 0 into
                    // App_Generic, which lands on LoginDenied_Min by accident
                    interface_->CloseConnection(networking_msg->m_conn, bmmo::connection_end::MalformedLogin, "Malformed login message", true);
                    break;
                }

                interface_->SetConnectionName(networking_msg->m_conn, msg.nickname.c_str());

                std::string reason = "Outdated client (client: " + msg.version.to_string()
                        + "; minimum: " + bmmo::minimum_client_version.to_string() + ")";
                bmmo::simple_action_msg new_msg{.content = bmmo::simple_action::LoginDenied};
                send(networking_msg->m_conn, new_msg, k_nSteamNetworkingSend_Reliable);
                interface_->CloseConnection(networking_msg->m_conn, bmmo::connection_end::OutdatedClient, reason.c_str(), true);
                break;
            }
            case bmmo::LoginRequestV3: {
                // A second login on a live connection would insert nothing
                // (the connection is already a key in clients_) but still add
                // a second username_ entry pointing at it; cleanup only erases
                // the one matching the current name, leaving the other behind
                // to resolve forever to a disconnected client.
                if (client_it != clients_.end()) {
                    Printf("Error: ignoring duplicate login request from (#%u, %s).",
                            networking_msg->m_conn, client_it->second.name);
                    break;
                }
                bmmo::login_request_v3_msg msg;
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                if (!msg.deserialize()) {
                    // not connection_end::None: GNS turns a reason of 0 into
                    // App_Generic, which lands on LoginDenied_Min by accident
                    interface_->CloseConnection(networking_msg->m_conn, bmmo::connection_end::MalformedLogin, "Malformed login message", true);
                    break;
                }

                interface_->SetConnectionName(networking_msg->m_conn, msg.nickname.c_str());

                if (!validate_client(networking_msg->m_conn, msg))
                    break;

                std::string uuid_string = bmmo::string_utils::get_uuid_string(msg.uuid);
                if (config_.has_forced_name(uuid_string)) {
                    std::string new_name = config_.get_forced_name(uuid_string);
                    bmmo::name_update_msg nu_msg;
                    nu_msg.text_content = new_name;
                    nu_msg.serialize();
                    send(networking_msg->m_conn, nu_msg.raw.str().data(), nu_msg.size(), k_nSteamNetworkingSend_Reliable);
                    if (bmmo::name_validator::is_spectator(msg.nickname))
                        new_name = bmmo::name_validator::get_spectator_nickname(new_name);
                    Printf(R"(Forced name change - #%u: "%s" -> "%s")",
                            networking_msg->m_conn, msg.nickname, new_name);
                    interface_->SetConnectionName(networking_msg->m_conn, new_name.c_str());
                    msg.nickname = new_name;
                }

                // accepting client and adding it to the client list
                // (on_message already holds state_mutex_)
                client_it = clients_.insert({networking_msg->m_conn, {msg.nickname, (bool)msg.cheated}}).first;
                memcpy(client_it->second.uuid, msg.uuid, sizeof(msg.uuid));
                client_it->second.login_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                username_[bmmo::message_utils::to_lower(msg.nickname)] = networking_msg->m_conn;
                const bool is_ghost_spectator = bmmo::name_validator::is_spectator(msg.nickname) || is_op(networking_msg->m_conn);
                if (is_ghost_spectator)
                    ghost_spectator_clients_.insert(networking_msg->m_conn);
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

                if (!config_.ghost_mode || is_ghost_spectator) {
                    bmmo::owned_compressed_ball_state_msg state_msg{};
                    pull_ball_states(state_msg.balls);
                    state_msg.serialize();
                    send(networking_msg->m_conn, state_msg.raw.str().data(), state_msg.size(), k_nSteamNetworkingSend_ReliableNoNagle);
                }

                if (!permanent_notification_.second.empty()) {
                    bmmo::permanent_notification_msg bulletin_msg{};
                    std::tie(bulletin_msg.title, bulletin_msg.text_content) = permanent_notification_;
                    bulletin_msg.serialize();
                    send(networking_msg->m_conn, bulletin_msg.raw.str().data(), bulletin_msg.size(), k_nSteamNetworkingSend_Reliable);
                }

                bmmo::extra_life_msg life_msg{};
                life_msg.life_count_goals = config_.initial_life_counts;
                life_msg.serialize();
                send(networking_msg->m_conn, life_msg.raw.str().data(), life_msg.size());

                if (!ticking_ && get_client_count() > 1)
                    start_ticking();
                config_.save_player_status(clients_);

                break;
            }
            case bmmo::LoginAccepted:
            case bmmo::PlayerDisconnected:
            case bmmo::PlayerConnected:
            case bmmo::Ping:
                break;
            case bmmo::BallState: {
                auto* state_msg = bmmo::message_utils::view_as<bmmo::ball_state_msg>(networking_msg);
                if (!state_msg) break;
                client_it->second.state = {state_msg->content, networking_msg->m_usecTimeReceived};
                client_it->second.state_updated = false;
                break;
            }
            case bmmo::TimedBallState: {
                auto* state_msg = bmmo::message_utils::view_as<bmmo::timed_ball_state_msg>(networking_msg);
                if (!state_msg) break;
                if (state_msg->content.timestamp < client_it->second.state.timestamp)
                    break;
                client_it->second.state = state_msg->content;
                client_it->second.state_updated = false;
                break;
            }
            case bmmo::Timestamp: {
                auto* timestamp_msg = bmmo::message_utils::view_as<bmmo::timestamp_msg>(networking_msg);
                if (!timestamp_msg) break;
                if (timestamp_msg->content < client_it->second.state.timestamp)
                    break;
                client_it->second.state.timestamp = timestamp_msg->content;
                client_it->second.timestamp_updated = false;
                break;
            }
            case bmmo::Chat: {
                bmmo::chat_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                if (!msg.deserialize()) break;

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

                // Broadcast chat message to other player
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
                if (!msg.deserialize()) break;

                bmmo::string_utils::sanitize_string(msg.chat_content);
                const bool muted = is_muted(client_it->second.uuid);
                const HSteamNetConnection receiver = msg.player_id;
                msg.player_id = networking_msg->m_conn;

                if (client_exists(receiver, true)) {
                    Printf((muted ? bmmo::ansi::Strikethrough : bmmo::ansi::Reset) | bmmo::color_code(msg.code),
                        "%s(%u, %s) -> (%u, %s): %s",
                        muted ? "[Muted] " : "", msg.player_id, client_it->second.name, receiver, clients_[receiver].name, msg.chat_content);
                    if (muted) {
                        bmmo::action_denied_msg denied_msg{.content = {bmmo::deny_reason::PlayerMuted}};
                        send(networking_msg->m_conn, denied_msg, k_nSteamNetworkingSend_Reliable);
                        break;
                    }
                    msg.clear();
                    msg.serialize();
                    send(receiver, msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
                } else {
                    Printf((muted ? bmmo::ansi::Strikethrough : bmmo::ansi::Reset) | bmmo::color_code(msg.code),
                        "%s(%u, %s) -> (%u, %s): %s",
                        muted ? "[Muted] " : "", msg.player_id, client_it->second.name, receiver, "[Server]", msg.chat_content);
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
                if (!msg.deserialize()) break;

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
                auto* msg = bmmo::message_utils::view_as<bmmo::player_ready_msg>(networking_msg);
                if (!msg) break;
                msg->content.player_id = networking_msg->m_conn;
                client_it->second.ready = msg->content.ready;
                msg->content.count = std::ranges::count_if(clients_,
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
                auto* msg = bmmo::message_utils::view_as<bmmo::countdown_msg>(networking_msg);
                if (!msg) break;

                std::string map_name = msg->content.map.get_display_name(map_names_);
                last_countdown_map_ = msg->content.map;
                switch (msg->content.type) {
                    using ct = bmmo::countdown_type;
                    case ct::Unknown:
                        return;
                    case ct::Go: {
                        if (config_.force_restart_level || msg->content.force_restart) {
                            maps_.clear();
                            for (const auto& map: map_names_)
                                maps_[map.first] = {0, networking_msg->m_usecTimeReceived, msg->content.mode, {}};
                        } else {
                            maps_[msg->content.map.get_hash_bytes_string()] = {0, networking_msg->m_usecTimeReceived, msg->content.mode, {}};
                        }
                        msg->content.restart_level = config_.restart_level;
                        msg->content.force_restart |= config_.force_restart_level;
                        for (auto& i: clients_) {
                            i.second.ready = i.second.dnf = false;
                        }
                        Printf(bmmo::color_code(msg->code), "[%u, %s]: %s%s - %s",
                            networking_msg->m_conn, client_it->second.name, map_name,
                            msg->content.get_level_mode_label(), msg->content.get_type_label());
                        break;
                    }
                    case ct::Countdown_1:
                    case ct::Countdown_2:
                    case ct::Countdown_3:
                    case ct::Ready:
                    case ct::ConfirmReady:
                    default:
                        Printf("[%u, %s]: %s%s - %s",
                            networking_msg->m_conn, client_it->second.name, map_name,
                            msg->content.get_level_mode_label(), msg->content.get_type_label());
                        break;
                }

                msg->content.sender = networking_msg->m_conn;
                broadcast_message(*msg, k_nSteamNetworkingSend_ReliableNoNagle);
                break;
            }
            case bmmo::DidNotFinish: {
                auto* msg = bmmo::message_utils::view_as<bmmo::did_not_finish_msg>(networking_msg);
                if (!msg) break;
                msg->content.player_id = networking_msg->m_conn;
                std::string& player_name = client_it->second.name;
                Printf(
                    bmmo::color_code(msg->code),
                    "%s(#%u, %s) did not finish %s (furthest reach: sector %d).",
                    msg->content.cheated ? "[CHEAT] " : "",
                    msg->content.player_id, player_name,
                    msg->content.map.get_display_name(map_names_),
                    msg->content.sector
                );
                client_it->second.dnf = true;
                broadcast_message(*msg, k_nSteamNetworkingSend_ReliableNoNagle);
                maps_[msg->content.map.get_hash_bytes_string()].rankings.second.push_back({
                    {(bool)msg->content.cheated, player_name}, msg->content.sector});
                break;
            }
            case bmmo::LevelFinish:
                break;
            case bmmo::LevelFinishV2: {
                auto* msg = bmmo::message_utils::view_as<bmmo::level_finish_v2_msg>(networking_msg);
                if (!msg) break;
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
                Printf(bmmo::color_code(msg->code),
                    "%s(#%u, %s) finished %s%s in %d%s place (score: %s; real time: %s).",
                    msg->content.cheated ? "[CHEAT] " : "",
                    msg->content.player_id, player_name,
                    msg->content.map.get_display_name(map_names_), get_level_mode_label(msg->content.mode),
                    current_map.rank, bmmo::string_utils::get_ordinal_suffix(current_map.rank),
                    formatted_score, msg->content.get_formatted_time());

                broadcast_message(*msg, k_nSteamNetworkingSend_ReliableNoNagle);

                current_map.rankings.first.push_back({
                    {(bool)msg->content.cheated, player_name}, msg->content.mode,
                    current_map.rank, msg->content.timeElapsed, formatted_score});

                break;
            }
            case bmmo::MapNames: {
                bmmo::map_names_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                if (!msg.deserialize()) break;

                // Client-supplied names are kept for the whole session and
                // resent to every player who joins, so they need bounding and
                // sanitizing: without a cap a single client can grow this map
                // (and every future login packet) without limit.
                decltype(msg.maps) accepted_maps;
                for (auto& [hash, name]: msg.maps) {
                    if (map_names_.size() >= MAX_MAP_NAMES && !map_names_.contains(hash))
                        continue;
                    if (name.length() > MAX_MAP_NAME_LENGTH)
                        name.erase(MAX_MAP_NAME_LENGTH);
                    bmmo::string_utils::sanitize_string(name);
                    if (map_names_.try_emplace(hash, name).second)
                        accepted_maps.emplace(hash, name);
                }
                if (accepted_maps.empty()) break;

                msg.maps = std::move(accepted_maps);
                msg.clear();
                msg.serialize();
                broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable, networking_msg->m_conn);
                break;
            }
            case bmmo::CheatState: {
                auto* state_msg = bmmo::message_utils::view_as<bmmo::cheat_state_msg>(networking_msg);
                if (!state_msg) break;
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
                auto* state_msg = bmmo::message_utils::view_as<bmmo::cheat_toggle_msg>(networking_msg);
                if (!state_msg) break;
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
                if (!msg.deserialize()) break;

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
                auto* msg = bmmo::message_utils::view_as<bmmo::current_map_msg>(networking_msg);
                if (!msg) break;
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
                            send(networking_msg->m_conn, hs_msg, k_nSteamNetworkingSend_ReliableNoNagle);
                        }
                        [[fallthrough]];
                    }
                    case bmmo::current_map_state::NameChange: {
                        client_it->second.current_map = msg->content.map;
                        client_it->second.current_sector = msg->content.sector;
                        broadcast_message(*msg, k_nSteamNetworkingSend_ReliableNoNagle, networking_msg->m_conn);
                        break;
                    }
                    default: break;
                }
                break;
            }
            case bmmo::RoomRequest: {
                bmmo::room_request_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                if (!msg.deserialize())
                    break;
                handle_room_request(client_it, msg);
                break;
            }
            case bmmo::CurrentSector: {
                auto* msg = bmmo::message_utils::view_as<bmmo::current_sector_msg>(networking_msg);
                if (!msg) break;
                msg->content.player_id = networking_msg->m_conn;
                client_it->second.current_sector = msg->content.sector;
                broadcast_message(*msg, k_nSteamNetworkingSend_ReliableNoNagle, networking_msg->m_conn);
                break;
            }
            case bmmo::SimpleAction: {
                auto* msg = bmmo::message_utils::view_as<bmmo::simple_action_msg>(networking_msg);
                if (!msg) break;
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
                    case sa::LevelRestarted: {
                        if (!config_.log_level_restarts)
                            break;
                        char text[256];
                        std::snprintf(text, sizeof(text), "(#%u, %s) restarted at sector %d of %s.",
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
            case bmmo::OwnedSimpleAction: {
                auto* msg = bmmo::message_utils::view_as<bmmo::owned_simple_action_msg>(networking_msg);
                if (!msg) break;
                switch (msg->content.type) {
                    using osa = bmmo::owned_simple_action_type;
                    case osa::RestartRequestFailed: {
                        Printf(bmmo::ansi::Italic, "(#%u, %s)'s restart request failed.",
                            msg->content.player_id, get_client_name(msg->content.player_id));
                        if (!client_exists(msg->content.player_id, true)
                                && msg->content.player_id != k_HSteamNetConnection_Invalid)
                            break;
                        broadcast_message(*msg);
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
                if (!msg.deserialize()) break;
                Printf("[Plain] (%u, %s): %s", networking_msg->m_conn, client_it->second.name, msg.text_content);
                break;
            }
            case bmmo::PublicNotification: {
                auto msg = bmmo::message_utils::deserialize<bmmo::public_notification_msg>(networking_msg);
                // treated like chat: muted players may not broadcast text, and
                // control characters are stripped before anyone else sees it
                bmmo::string_utils::sanitize_string(msg.text_content);
                const bool muted = is_muted(client_it->second.uuid);
                Printf(msg.get_ansi_color_code() | (muted ? bmmo::ansi::Strikethrough : bmmo::ansi::Reset),
                        "%s[%s] (%u, %s): %s", muted ? "[Muted] " : "",
                        msg.get_type_name(), networking_msg->m_conn, client_it->second.name, msg.text_content);
                if (muted) {
                    send(networking_msg->m_conn,
                            bmmo::action_denied_msg{.content = {bmmo::deny_reason::PlayerMuted}},
                            k_nSteamNetworkingSend_Reliable);
                    break;
                }
                // re-serialize instead of forwarding the client's raw bytes,
                // so what we broadcast is what we just validated
                msg.clear();
                msg.serialize();
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
                const auto rankings = get_map_rankings(msg.map);
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
                if (!msg.deserialize()) break;

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
                client_it->second.mods = msg.mods;   // physics-session whitelist check
                break;
            }
#if BMMO_BUILD_SIM
            case bmmo::SessionReady: {
                auto msg = bmmo::message_utils::deserialize<bmmo::session_ready_msg>(networking_msg);
                handle_session_ready(client_it, msg);
                break;
            }
            case bmmo::SessionInput: {
                auto msg = bmmo::message_utils::deserialize<bmmo::session_input_msg>(networking_msg);
                handle_session_input(networking_msg->m_conn, msg);
                break;
            }
            case bmmo::SessionEvent: {
                auto msg = bmmo::message_utils::deserialize<bmmo::session_event_msg>(networking_msg);
                handle_session_event(client_it, msg);
                break;
            }
            case bmmo::SessionResync: {
                auto msg = bmmo::message_utils::deserialize<bmmo::session_resync_msg>(networking_msg);
                handle_session_resync(networking_msg->m_conn, msg);
                break;
            }
#endif
            case bmmo::RealWorldTimestamp:
            case bmmo::SoundData:
            case bmmo::SoundStream:
            case bmmo::OwnedBallState:
            case bmmo::OwnedBallStateV2:
            case bmmo::OwnedTimedBallState:
            case bmmo::OwnedCompressedBallState:
            case bmmo::LatencyData:
            case bmmo::LoginAcceptedV2:
            case bmmo::LoginAcceptedV3:
            case bmmo::PlayerConnectedV2:
            case bmmo::HighscoreTimerCalibration:
            case bmmo::NameUpdate:
            case bmmo::OwnedCheatState:
            case bmmo::OwnedCheatToggle:
            case bmmo::PlayerKicked:
            case bmmo::ActionDenied:
            case bmmo::ExtraLife:
            case bmmo::OpState:
            case bmmo::KeyboardInput:
                break;
            default:
                Printf("Error: invalid message with opcode %d received from #%u.",
                        raw_msg->code, networking_msg->m_conn);
        }
    }

    int poll_incoming_messages() override {
        const int msg_count = interface_->ReceiveMessagesOnPollGroup(poll_group_, incoming_messages_, ONCE_RECV_MSG_COUNT);
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

    // Runs on the network thread SERVER_TICK_DELAY into each loop iteration:
    // broadcasts ball states collected since the last tick and, periodically,
    // latency / real-world-time data.
    inline void tick() {
        bmmo::owned_compressed_ball_state_msg ball_msg{};
        pull_unupdated_ball_states(ball_msg.balls, ball_msg.unchanged_balls);
        if (ball_msg.balls.empty() && ball_msg.unchanged_balls.empty())
            return;
        ball_msg.serialize();

        std::lock_guard lk(state_mutex_);
        if (config_.ghost_mode) {
            for (const auto& i: ghost_spectator_clients_)
                send(i, ball_msg.raw.str().data(), ball_msg.size(), k_nSteamNetworkingSend_UnreliableNoDelay);
        } else {
            // Room-scoped fan-out: a room's members see only their room's balls;
            // players not in any room (room 0) still see one another, as before.
            std::unordered_map<uint32_t, bmmo::owned_compressed_ball_state_msg> per_room;
            for (const auto& b: ball_msg.balls)
                per_room[rooms_.room_of(b.player_id)].balls.push_back(b);
            for (const auto& u: ball_msg.unchanged_balls)
                per_room[rooms_.room_of(u.player_id)].unchanged_balls.push_back(u);
            for (auto& [_, m]: per_room) m.serialize();
            for (const auto& [conn, _]: clients_) {
                auto it = per_room.find(rooms_.room_of(conn));
                if (it != per_room.end())
                    send(conn, it->second.raw.str().data(), it->second.size(), k_nSteamNetworkingSend_UnreliableNoDelay);
            }
        }

        ++ping_data_counter_;
        if (ping_data_counter_ >= bmmo::PING_INTERVAL_TICKS) {
            bmmo::latency_data_msg ping_msg{};
            ping_msg.data.reserve(clients_.size());
            for (const auto& i: clients_) {
                SteamNetConnectionRealTimeStatus_t status{};
                interface_->GetConnectionRealTimeStatus(i.first, &status, 0, nullptr);
                ping_msg.data.try_emplace(i.first,
                        (uint16_t) std::min(status.m_nPing, (int) std::numeric_limits<uint16_t>::max()));
            }
            ping_msg.serialize();
            ping_data_counter_ = 0;
            broadcast_message(ping_msg.raw.str().data(), ping_msg.size(), k_nSteamNetworkingSend_Reliable);
            using namespace std::chrono;
            const int64_t now_micros = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
            broadcast_message(bmmo::real_world_timestamp_msg{.content = now_micros}, k_nSteamNetworkingSend_ReliableNoNagle);
        }
    };

    void start_ticking() {
        ticking_ = true;
        Printf("Ticking started.");
    }
    void stop_ticking() {
        ticking_ = false;
        Printf("Ticking stopped.");
    }

    uint16_t port_ = 0;
    HSteamListenSocket listen_socket_ = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup poll_group_ = k_HSteamNetPollGroup_Invalid;
    client_data_collection clients_;
    std::map<std::string, HSteamNetConnection> username_; // Note: this stores names converted to all-lowercases
    std::unordered_set<HSteamNetConnection> ghost_spectator_clients_; // ghost mode - only operators and spectators can see other players
    bmmo::room_manager rooms_; // collision-overhaul room system (guarded by state_mutex_)
#if BMMO_BUILD_SIM
    std::map<uint32_t, physics_session_state> physics_sessions_;        // by session id (state_mutex_)
    std::unordered_map<uint32_t, uint32_t> room_session_;               // room -> session
    std::unordered_map<HSteamNetConnection, uint32_t> client_session_;  // member -> session
    uint32_t next_session_id_ = 1;
    // Declared last so it is destroyed first: its thread calls back into
    // the members above.
    std::unique_ptr<bmmo::sim::session_runner> runner_;
#endif
    // Guards all server state shared between the network thread and the
    // console thread (clients_, username_, maps_, map_names_,
    // ghost_spectator_clients_, permanent_notification_, last_countdown_map_,
    // config_). Recursive because locked entry points (on_message,
    // console-facing methods) call each other.
    std::recursive_mutex state_mutex_;
    std::pair<std::string, std::string> permanent_notification_; // <username (title), text>

    std::atomic_bool ticking_ = false;
    int ping_data_counter_ = 0;
    map_data_collection maps_;
    bmmo::map last_countdown_map_{};

    config_manager config_;

    std::unordered_map<std::string, std::string> map_names_;
    // Bounds on what clients may add to map_names_; the whole map is sent to
    // every joining client, so it must not be allowed to grow without limit.
    static constexpr size_t MAX_MAP_NAMES = 4096, MAX_MAP_NAME_LENGTH = 512;
};

// parse arguments (optional port and help/version/log) with getopt
static int parse_args(int argc, char** argv, uint16_t& port, std::string& log_path, bool& dry_run) {
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
                port = std::atoi(optarg);
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
        bmmo::set_log_file(log_file);
    }

#if BMMO_BUILD_SIM
    bmmo::sim::install_crash_reporter();   // backtraces from the simulation thread
#endif
    printf("Initializing sockets...\n");
    server::init_socket();

    printf("Starting server at port %u.\n", port);
    server server(port);

    printf("Bootstrapping server...\n");
    fflush(stdout);
    if (!server.setup())
        FatalError("Server failed on setup.");
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
        Printf("([Server]): %s", msg.chat_content);
    });
    auto parse_client_id = [&](const std::string& client_input) -> HSteamNetConnection {
        return (client_input.length() > 0 && client_input[0] == '#')
                ? std::atoll(client_input.substr(1).c_str()) : server.get_client_id(client_input);
    };
    auto get_client_id_from_console = [&]() -> HSteamNetConnection {
        std::string client_input = console.get_next_word();
        HSteamNetConnection client = parse_client_id(client_input);
        if (client == 0)
            Printf("Error: invalid connection id \"%s\".", client_input.c_str());
        return client;
    };
    using client_list_t = std::unordered_set<HSteamNetConnection>;
    // separated by `,` without spaces
    auto get_multiple_client_ids_from_console = [&]() -> client_list_t {
        std::string client_input = console.get_next_word();
        client_list_t clients;
        for (const auto& client_str: bmmo::string_utils::split_strings(client_input, ',')) {
            HSteamNetConnection client = parse_client_id(client_str);
            if (client != 0)
                clients.insert(client);
            else
                Printf("Error: invalid connection id \"%s\".", client_str.c_str());
        }
        return clients;
    };
    auto get_multiple_client_names = [&](const client_list_t& clients) -> std::string {
        std::vector<std::string> names; names.reserve(clients.size());
        for (const auto& client : clients) names.push_back(server.get_client_name(client));
        return bmmo::string_utils::join_strings(names, 0, ", ");
    };
    auto send_plain_text_msg = [&](bool broadcast = true) {
        client_list_t clients = broadcast ? client_list_t{} : get_multiple_client_ids_from_console();
        if (!broadcast && clients.empty()) return;
        bmmo::plain_text_msg msg{};
        msg.text_content = console.get_rest_of_line();
        if (msg.text_content.empty()) return;
        if (msg.text_content.front() == '"' && msg.text_content.back() == '"') {
            try { // we are using YAML to handle escape sequences and unicode properly
                YAML::Node idata = YAML::Load(msg.text_content);
                if (idata.Type() == YAML::NodeType::Scalar)
                    msg.text_content = idata.as<std::string>();
                else throw std::runtime_error("Error: Invalid YAML content (not a scalar string).");
            } catch (const std::exception& e) {
                Printf(e.what());
                return;
            }
        }
        if (msg.text_content.find("\033[") != std::string::npos) 
            msg.text_content += "\033[0m"; // reset formatting
        msg.serialize();
        if (broadcast)
            server.broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        else for (const auto& client: clients)
            server.send(client, msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        Printf("[Plain%s]: %s",
            broadcast ? "" : " -> " + get_multiple_client_names(clients), msg.text_content);
    };
    console.register_command("plaintext", send_plain_text_msg);
    console.register_command("plaintext#", std::bind(send_plain_text_msg, false));
    auto send_popup_msg = [&](bool broadcast = true) {
        client_list_t clients = broadcast ? client_list_t{} : get_multiple_client_ids_from_console();
        if (!broadcast && clients.empty()) return;
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
            Printf(e.what());
            return;
        }
        msg.serialize();
        if (broadcast)
            server.broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        else for (const auto& client: clients)
            server.send(client, msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        Printf(bmmo::color_code(msg.code), "[Popup%s] {%s}: %s",
                broadcast ? "" : " -> " + get_multiple_client_names(clients),
                msg.title, msg.text_content);
    };
    console.register_command("popup", send_popup_msg);
    console.register_command("popup#", std::bind(send_popup_msg, false));
    using in_msg = bmmo::important_notification_msg;
    auto send_important_notification = [&](bool broadcast = true, in_msg::notification_type type = in_msg::Announcement) {
        bmmo::important_notification_msg msg{};
        client_list_t clients = broadcast ? client_list_t{} : get_multiple_client_ids_from_console();
        if (!broadcast && clients.empty()) return;
        msg.chat_content = console.get_rest_of_line();
        msg.type = type;
        msg.serialize();
        if (broadcast)
            server.broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        else for (const auto& client: clients)
            server.send(client, msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        Printf(msg.get_ansi_color(), "[%s] ([Server])%s: %s",
                msg.get_type_name(), broadcast ? "" : " -> " + get_multiple_client_names(clients),
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
            Printf("Usage: \"kick# <code> <player> <reason>\".");
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
        client_list_t clients = get_multiple_client_ids_from_console();
        if (clients.empty()) return;
        std::string text = console.get_rest_of_line();
        bmmo::private_chat_msg msg{};
        msg.chat_content = text;
        msg.serialize();
        for (const auto& client: clients)
            server.send(client, msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        Printf(bmmo::color_code(msg.code), "([Server]) -> %s: %s",
                get_multiple_client_names(clients), msg.chat_content);
    });
    console.register_command("ban", [&] {
        auto client = get_client_id_from_console();
        std::string text = console.get_rest_of_line();
        server.set_ban(client, text);
    });
    console.register_command("op", [&] {
        auto client = get_client_id_from_console();
        if (client == k_HSteamNetConnection_Invalid) return;
        const auto& cmd = console.get_command_name();
        bool action = (cmd == "op" || cmd == "mute");
        if (cmd == "op" || cmd == "deop")
            server.set_op(client, action);
        else
            server.set_mute(client, action);
    });
    console.register_aliases("op", {"deop", "mute", "unmute"});
    console.register_command("listban", [&] { server.print_bans(); });
    console.register_command("listmute", [&] { server.print_mutes(); });
    console.register_command("unban", [&] { server.set_unban(console.get_next_word()); });
    console.register_command("reload", [&] {
        if (!server.load_config())
            Printf("Error: failed to reload config.");
    });
    console.register_command("listmap", [&] { server.print_maps(); });
#if BMMO_BUILD_SIM
    console.register_command("sessions", [&] { server.print_physics_sessions(); });
#endif
    console.register_command("countdown", [&] {
        auto print_hint = [] {
            Printf(R"(Error: please specify the map to countdown (hint: use "getmap" and "listmap").)");
            Printf("Usage: \"countdown <client id> level|<hash> <level number> [mode] [type]\".");
            Printf(R"(<type>: {"4": "Get ready", "5": "Confirm ready", "": "auto countdown"})");
        };
        if (console.empty()) { print_hint(); return; }
        const auto client = get_client_id_from_console();
        // receive() treats an invalid id as "pick a random online player", so
        // without this a mistyped id sends the countdown as someone else
        if (client == k_HSteamNetConnection_Invalid) return;
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
            msg.content.type = static_cast<bmmo::countdown_type>(std::clamp(console.get_next_int(), 0, 255));
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
            Printf(bmmo::color_code(msg.code), "[[Server]]: Countdown%s - %s",
                msg.content.mode == bmmo::level_mode::Highscore ? " <HS>" : "",
                i == 0 ? "Go!" : std::to_string(i));
            if (i != 0)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });
    console.register_command("dnf", [&] {
        auto client = get_client_id_from_console();
        if (client == k_HSteamNetConnection_Invalid) return;
        const auto data = server.get_client(client);
        if (!data) return;
        bmmo::did_not_finish_msg msg{};
        msg.content = {.cheated = data->cheated, .map = data->current_map, .sector = data->current_sector};
        server.receive(&msg, sizeof(msg), client);
    });
    console.register_command("bulletin", [&] {
        if (console.get_command_name() == "bulletin")
            server.set_and_broadcast_bulletin("[Server]", console.get_rest_of_line());
        const auto bulletin = server.get_bulletin();
        Printf(bmmo::color_code(bmmo::PermanentNotification), "[Bulletin] %s%s", bulletin.first,
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
                Printf("Usage: playsound <caption>: [[frequency, duration], [freq2, dur2]] (caption can be omitted).");
                return;
            }
            std::stringstream temp; temp << sounds;
            Printf(bmmo::ansi::WhiteInverse, "Playing sound - %s", temp.str());
            msg.serialize();
            server.broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        } catch (const std::exception& e) { Printf(e.what()); return; }
    });
    console.register_command("playstream", [&] {
        const bool broadcast = (console.get_command_name() == "playstream");
        client_list_t clients = broadcast ? client_list_t{} : get_multiple_client_ids_from_console();
        if (!broadcast && clients.empty()) return;
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
                    Printf("Usage: playstream {caption: <name>, path: <path>, duration: <duration_ms = 0>, gain: <1.0 ∈ [0, 1]>, pitch: <1.0 ∈ [0.1, 4.1]>}");
                    Printf("Usage: playstream <path>");
                    Printf("Current working directory: %s", std::filesystem::current_path().string());
                    Printf("Maximum file size: %lld bytes", msg.get_max_stream_size());
                    return;
            }
            msg.type = bmmo::sound_stream_msg::sound_type::Wave;
            if (!msg.serialize()) {
                Printf("Error serializing message.");
                return;
            }
            if (broadcast) server.broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
            else for (const auto& client: clients)
                server.send(client, msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
            Printf(bmmo::ansi::WhiteInverse, "Sound <%s> (size: %d) sent to %s",
                    msg.path, (uint32_t) msg.size(), broadcast ? "[all]" : get_multiple_client_names(clients));
        } catch (const std::exception& e) { Printf(e.what()); return; }
    });
    console.register_aliases("playstream", {"playstream#"});
    console.register_command("scores", [&] {
        if (console.empty()) { Printf("Usage: \"scores <hs|sr> [map]\""); return; }
        bool hs_mode = (console.get_next_word(true) == "hs");
        server.print_scores(hs_mode, console.empty() ? server.get_last_countdown_map() : static_cast<bmmo::map>(console.get_next_map()));
    });
    console.register_command("sendscores", [&] {
        const bool broadcast = (console.get_command_name() == "sendscores");
        client_list_t clients = broadcast ? client_list_t{} : get_multiple_client_ids_from_console();
        if (!broadcast && clients.empty()) return;
        bmmo::score_list_msg msg{};
        msg.mode = (console.get_next_word(true) == "hs") ? bmmo::level_mode::Highscore : bmmo::level_mode::Speedrun;
        msg.map = console.empty() ? server.get_last_countdown_map() : static_cast<bmmo::map>(console.get_next_map());
        const auto rankings = server.get_map_rankings(msg.map);
        if (!rankings) {
            Printf(bmmo::ansi::BrightRed, "Error: ranking info not found for the specified map.");
            return;
        }
        msg.serialize(*rankings);
        if (broadcast)
            server.broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        else for (const auto& client: clients)
            server.send(client, msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        Printf(bmmo::color_code(msg.code), "Score list data sent to %s.",
                broadcast ? "[all]" : get_multiple_client_names(clients));
    });
    console.register_aliases("sendscores", {"sendscores#"});
    console.register_command("restartlevel", [&] {
        if (console.empty()) return Printf("Usage: \"restartlevel <player>\".");
        auto clients = get_multiple_client_ids_from_console();
        if (clients.empty()) return;
        for (const auto& client: clients) {
            bmmo::restart_request_msg msg{.content = {.victim = client}};
            server.broadcast_message(msg);
            Printf(bmmo::color_code(msg.code), "Requested to restart %s's current level.", server.get_client_name(client));
        }
    });
    console.register_command("flushlog", bmmo::flush_log);
    console.register_command("help", [&] { Printf(console.get_help_string().c_str()); });

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
        LogFileOutput(("> " + line).c_str());

        if (!console.execute(line) && !console.get_command_name().empty()) {
            std::string extra_text;
            if (auto hints = console.get_command_hints(true); !hints.empty())
                extra_text = " Did you mean: " + bmmo::string_utils::join_strings(hints, 0, ", ") + "?";
            Printf("Error: unknown command \"%s\".%s", console.get_command_name(), extra_text);
        }
    }

    std::cout << "Stopping..." << std::endl;
    if (server_thread.joinable())
        server_thread.join();

    server::destroy();
    printf("\r");
}
