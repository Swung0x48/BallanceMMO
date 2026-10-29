#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif

#include <vector>
#include <deque>
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
#include <session/io_pool.hpp>
#include "sim/late_tick.hpp"
#include <physics/physics_state.hpp>
#include <session/lifecycle.hpp>
#include <session/input_relay.hpp>
#include <session/member_lives.hpp>
#include <session/mechanism_units.hpp>
#include <session/spawn_impulse.hpp>
#include <cmath>
#endif

using bmmo::Printf, bmmo::Sprintf, bmmo::LogFileOutput, bmmo::FatalError;

class server: public role {
public:
    explicit server(uint16_t port) {
        port_ = port;
    }

    // --hold-member-ms MS (world-agreement design 7.3 J3, step M0; test
    // builds only, BMMO_SERVER_TEST_FLAGS): the SessionMember message of
    // step M1 - its reliable copy and its repeats - leaves MS ms late, so the
    // clients' parking of events that outrun it is exercised.  Step M0 only
    // carries the value; M1's send points read it.  0 (the default, and
    // every build without the flag): sent at once.
    void set_test_hold_member_ms(uint32_t ms) { test_hold_member_ms_ = ms; }
    uint32_t test_hold_member_ms() const { return test_hold_member_ms_; }

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

    // The start barrier is a wall-clock deadline, so it is checked from the
    // main loop rather than from tick(): a session whose members never answered
    // SessionReady has no ticks at all, and tick() is skipped entirely unless
    // the server is ticking.  Runs before the tick broadcast every iteration.
    bool update() override {
        const bool had_messages = role::update();
#if BMMO_BUILD_SIM
        {
            std::lock_guard lk(state_mutex_);
            check_start_barriers();
            send_assign_repeats();
            post_player_rtts();
        }
#endif
        return had_messages;
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

    static constexpr int kAssignRepeats = 3;   // unreliable copies of a SessionAssign after the reliable one
    static constexpr std::chrono::milliseconds kAssignRepeatSpacing{40};   // ...this far apart, each in its own packet

    struct physics_session_state {
        uint32_t id = 0, room = 0;
        bmmo::map map{};
        std::vector<HSteamNetConnection> members;           // join order
        std::set<HSteamNetConnection> ready;
        std::set<HSteamNetConnection> assigned;             // got their SessionAssign
        std::set<HSteamNetConnection> late;                 // joined a running session: no hash check
        bool world_ready = false, ticking = false;
        // Start barrier (design 8.3): armed when SessionStart goes out, so a
        // member that never reports SessionReady cannot pin the session at its
        // boot for ever.  Checked by check_start_barriers() from the main loop.
        std::chrono::steady_clock::time_point start_deadline{};
        bool start_barrier_armed = false;
        uint64_t anchor_hash = 0, anchor_surfaces = 0;
        // Protocol 12 (sector placement design 2.4): the world's placement
        // signature at its anchor, compared with every member's that declares
        // CAP_PLACEMENT (late joiners too).
        uint64_t anchor_placement = 0;
        // Protocol 16 (world-agreement-gaps-design.md 2.3, 5; step G3): the
        // world's mechanism-class probe signature, compared with every
        // member's anchor_probes under WORLD_PROXIMITY_UNION (late joiners
        // too).
        uint64_t anchor_probes = 0;
        float spawn_position[3] = {}, spawn_rotation[4] = {0, 0, 0, 1};
        // Every resetpoint of the level (physics_world::resetpoints): where the
        // event validation expects a spawn or a respawn.
        std::vector<VxVector> resetpoints;
        // Spawn kick speed sent in this session's SessionStart (design 9.10):
        // fixed at on_world_ready so a late joiner gets the same value.
        float spawn_impulse = 0;
        float spawn_spacing = 0;   // birth cell spacing (SessionStart), fixed with the impulse
        // The last relay_window ticks' applied frames: what every member's
        // relayed datagram repeats (session/input_relay.hpp, protocol 5).
        bmmo::session::relay_ring relay;
        bool relay_trim_logged = false;
        // Protocol 7 (late-player design 1.2): what each member's relayed
        // datagrams tell it about its OWN inputs.  `epoch` is the base of its
        // last SessionAssign and `held_total` counts its substituted ticks at
        // or after it: a resync or a late join re-anchors the cursor at the
        // base and the ticks up to it are applied held, but they precede the
        // client's own numbering, so they are never counted (nor noticed).
        // Both are reset by send_session_assign.  `acked` is its last tick
        // applied fresh; an assignment moves it to the base - 1, since every
        // tick before the base is outside the new numbering and an ack from
        // the old one would read as starvation for the whole lead.  `run` is
        // the S2 line being gathered: consecutive held ticks, logged once the
        // run ends.
        struct held_run {
            bool open = false;
            uint32_t first = 0, last = 0, count = 0, sent = 0;
            uint32_t source_min = 0, source_max = 0;
            // (tick - source, ticks) runs; delay 0xFFFFFFFF = the default frame
            std::vector<std::pair<uint32_t, uint32_t>> delays;
        };
        struct held_notice {
            bool assigned = false;
            uint32_t epoch = 0;
            uint32_t held_total = 0;
            uint32_t acked = 0;
            // Held records handed to the socket, every copy (a record rides
            // 32 ticks of datagrams), this epoch: the S2 line's `copies`.
            uint64_t copies = 0;
            held_run run;
        };
        std::map<HSteamNetConnection, held_notice> notices;
        uint32_t s2_lines = 0;
        // Lifecycle events (protocol 6): the copies members' events still owe
        // the others, one per tick from on_session_inputs, and which events
        // from each member this server has already taken (the members repeat
        // theirs the same way).
        bmmo::session::event_repeats event_repeats;
        bmmo::session::event_seen events_seen;
        uint64_t duplicate_events = 0;
        // Protocol 9 (late-player design 3.1): an event is relayed once the
        // runner has accepted it on the simulation thread and fixed the tick
        // it is applied at, not on arrival.  Its message waits here under the
        // token submit_event was given, until on_event_accepted takes it.
        struct awaiting_event {
            HSteamNetConnection from = k_HSteamNetConnection_Invalid;
            bmmo::session_event_msg msg;
        };
        std::map<uint64_t, awaiting_event> awaiting_events;
        uint64_t next_event_token = 1;
        uint64_t restamped_events = 0, superseded_births = 0;
        // Assignments still to be repeated unreliably, one copy every
        // kAssignRepeatSpacing from the main loop (send_session_assign,
        // send_assign_repeats).
        struct assign_repeat {
            HSteamNetConnection to;
            uint32_t first_tick;
            int remaining;
            std::chrono::steady_clock::time_point next_due;
            // Protocol 10: every copy carries the reliable one's flags
            // (ASSIGN_FLAG_STATE_FOLLOWS), whichever arrives first.
            uint8_t flags = 0;
        };
        std::vector<assign_repeat> assign_repeats;
        // Late-player design 2.2-2.5 (physics.lead_adjust): per member, what
        // its SessionReady declared, the lead of its first assignment (the
        // start base, or a late joiner's lead) and the offset its
        // acknowledged SessionLeads added - L_eff, the lead a resync
        // re-anchors at, is their sum - and the SessionLead it has not
        // acknowledged yet.  The offset survives a resync; the outstanding
        // decision does not (the client drops it by its epoch).
        struct member_lead {
            uint32_t caps = 0;
            uint32_t assigned_lead = 0;
            int offset = 0;
            uint32_t outstanding = 0;   // seq of the SessionLead not yet acknowledged; 0 = none
            int outstanding_k = 0;      // its delta: what an ack may apply at most (lateness_bound_applied)
        };
        std::map<HSteamNetConnection, member_lead> leads;
        // A SessionLead's unreliable copies, one every kAssignRepeatSpacing
        // after the reliable one, retired by its ack or a newer assignment.
        struct lead_repeat {
            HSteamNetConnection to;
            uint32_t seq;
            std::string bytes;
            int remaining;
            std::chrono::steady_clock::time_point next_due;
        };
        std::vector<lead_repeat> lead_repeats;
        // Ticks the server waits for this session's inputs, sized at the start
        // from the worst round trip among the members (session_input_delay).
        // Fixed for the session's life: the scheduler's deadlines are measured
        // from the start with this value in them, and a late joiner cannot be
        // allowed to move the timeline the running members are already on.
        uint32_t input_delay = 6;
        // Ticks ahead a member stamps the Sector event of a sector it enters
        // (session::sector_lead_for_ping, session/sector_union.hpp): the
        // relay has to reach every other member before they simulate that
        // tick.  Sized with the input delay from the same round trip, sent
        // in every SessionStart, fixed for the session's life.
        uint32_t sector_lead = 0;
        // The tick base the members present at the start are numbered from:
        // the same lead a late joiner gets, measured from tick 0 (design 9.2,
        // sim/late_tick.hpp session_start_tick_base).  Fixed here, with the
        // input delay, so every start member shares one numbering base however
        // far apart their level loads were.
        uint32_t start_tick_base = 0;
        // Stable per member: the lowest value not currently used by another
        // member of this session, assigned when the member enters and freed
        // when it leaves.  Both the SessionStart player_entry and the world's
        // per-player slot use this number, so the server and every side agree
        // on the spawn direction without exchanging it separately.
        std::map<HSteamNetConnection, uint8_t> join_orders;
        // Step M1 (world-agreement-gaps-design.md 2.7).  `lives`: each id's
        // current life in this session - the count of its adds, from 1; never
        // erased, so a rejoin on the same connection is the next life.
        // `retiring_orders` (J2): the join order of a life that left the
        // session but is still in the runner's world (until its removal, R),
        // keyed by (id, life) because a rejoin's new life may already hold
        // another order; assign_join_order treats them as held, and
        // on_player_removed releases one.  `started`: the members that have
        // their SessionStart, which SessionMember goes to.
        std::map<HSteamNetConnection, uint16_t> lives;
        std::map<std::pair<HSteamNetConnection, uint16_t>, uint8_t> retiring_orders;
        // Step Q7 (world-agreement-gaps-design.md 2.10 "The cap"): the
        // session's mechanism slot count K (0 without WORLD_PLAYER_MECHANISMS)
        // - join orders are handed out below K only - and the joiners that
        // found every order below K held or retiring, in arrival order: each
        // released order goes to the first of them (admit_waiting).
        uint8_t mechanism_slots = 0;
        std::deque<HSteamNetConnection> waiting_orders;
        std::set<HSteamNetConnection> started;
        // --hold-member-ms (test builds): SessionMember messages waiting to
        // leave, the reliable copy and then the repeats.
        struct held_member {
            std::chrono::steady_clock::time_point due{};
            std::vector<uint32_t> to;
            std::string bytes;
        };
        std::vector<held_member> held_members;
        uint64_t member_messages = 0;
        // Each member's last Physicalize as relayed (serialized, player, applied
        // tick and origin set), so a late joiner can build the bodies that
        // already exist, with its ORIGIN (late-player design 3.1): an
        // Unphysicalize erases it only when its origin is later, the guard
        // every receiver runs.  Updated when the runner accepts an event
        // (on_event_accepted), on the simulation thread.
        struct last_physicalize_entry { uint32_t tick = 0; std::string bytes; };
        std::map<HSteamNetConnection, last_physicalize_entry> last_physicalize;
        // The origin of each member's newest accepted Unphysicalize: a birth
        // older by origin that is accepted after it (still applied, before
        // it) is no ball a late joiner may be given - the death already
        // erased the entry, and the birth must not put it back.
        std::map<HSteamNetConnection, uint32_t> last_unphysicalize;
        // Every Sector event relayed in this session (serialized, player set),
        // in order: a late joiner's union has to grow by the same sectors the
        // running members' did (session/sector_union.hpp), and a resynced
        // member's already has them (a stamp of a sector that is up is a
        // no-op there).
        std::vector<std::string> sector_history;
        // The level's Physicalize_GameBall rows (from the world boot): the
        // reference a reported ball recipe is validated against.
        std::vector<bmmo::sim::physics_world::ball_row> ball_rows;
        // Protocol 11 (trafo-replication design 2.14): the session's world
        // options (SessionStart's, fixed at the start from the server's
        // config) and the level's P_Trafo_* positions (from the world boot),
        // which a client's Trafo explosion is validated against.
        uint32_t world_options = 0;
        std::vector<bmmo::session::trafo_point> trafo_points;
        uint64_t rejected_trafos = 0;
        // Every Trafo event relayed as applied (serialized, player set) with
        // its applied tick: a join or resync state of tick S carries the
        // ledger of S, and an event accepted before the member's join but
        // applied after S (a re-explosion moved behind a reset lands up to
        // the lead + kTrafoResetSpan ticks after its acceptance) reaches it
        // only from here.  Resent with the state, the ones applied at or
        // after the base; pruned when far in the past.
        std::vector<std::pair<uint32_t, std::string>> trafo_relayed;
        // Event validation (design 9.4): rate limit rejects, the rest is
        // logged and counted in M4 (no rejection: the retail scripts decide
        // respawn poses per player, the server only knows the union).  The
        // per-second cap is physics.event_rate_limit (0 = no limit).
        struct member_guard {
            int sector = 1;
            uint32_t events_in_window = 0;
            std::chrono::steady_clock::time_point window_start{};
            double last_position[3] = {};
            bool have_position = false;
            // Repeat-event guard (design 9.4 hardening).  BodyRevived is a
            // "this body is back" notification the client re-sends every frame
            // it still believes a body is missing; the stamps differ (measured:
            // up to 18 ticks apart inside one burst) while the (ball_type,
            // name) does not, so the dedup key must not contain the stamp.
            std::vector<bmmo::session::repeat_event_key> recent_revives;
            uint64_t dropped_duplicate = 0, late_events = 0;
            // Step G5: BodyRevived dropped under WORLD_PROXIMITY_UNION.
            uint64_t revived_ignored = 0;
        };
        std::map<HSteamNetConnection, member_guard> guards;
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
        rc.full_snapshot_interval = std::max<uint32_t>(1, config_.physics_full_snapshot_interval);
        rc.trace = config_.physics_debug_trace;
        rc.patience = config_.physics_patience;
        rc.lead_adjust = config_.physics_lead_adjust;
        // The headless engine chdir's into the game's Bin directory, so the
        // journal directory has to be resolved against the directory the
        // server was started from before the first world boots.
        if (!config_.physics_journal_dir.empty())
            rc.journal_dir = config_manager::resolve_path(config_.physics_journal_dir.c_str());
        rc.journal_max_bytes = static_cast<uint64_t>(config_.physics_journal_max_mb) << 20;
        rc.journal_checkpoint_ticks = config_.physics_journal_checkpoint_ticks;
        // Step SIO: the journals are written by the server's own one-thread
        // I/O pool, never on the runner thread; it flushes them as
        // session_io.journal says (step IOF, batch by default).
        io_pool_ = std::make_unique<bmmo::session::io_pool>(1);
        rc.io_strand = [pool = io_pool_.get()] { return pool->strand(); };
        rc.journal_flush = config_.session_io_journal_flush;
        physics_seed_ = rc.seed;
        bmmo::sim::session_callbacks callbacks;
        callbacks.log = [](const std::string& text) { Printf("[Sim] %s", text); };
        callbacks.on_world_ready = [this](const bmmo::sim::world_ready_info& info) { on_world_ready(info); };
        callbacks.on_snapshot = [this](const bmmo::sim::session_snapshot& snapshot) { on_session_snapshot(snapshot); };
        callbacks.on_resync_state = [this](const bmmo::sim::resync_state_out& out) { on_session_resync_state(out); };
        callbacks.on_resync_base_missed = [this](uint32_t session, uint32_t player, uint32_t base) {
            on_resync_base_missed(session, player, base);
        };
        callbacks.on_inputs = [this](uint32_t session, uint32_t tick,
                                     const std::vector<bmmo::session::applied_input>& applied) {
            on_session_inputs(session, tick, applied);
        };
        callbacks.on_lead = [this](const bmmo::sim::lead_notice& notice) { on_session_lead(notice); };
        callbacks.on_event_accepted = [this](const bmmo::sim::accepted_event& a) { on_event_accepted(a); };
        callbacks.on_player_added = [this](const bmmo::sim::member_change& c) { on_player_added(c); };
        callbacks.on_player_removed = [this](const bmmo::sim::member_change& c) { on_player_removed(c); };
        callbacks.on_player_add_failed = [this](const bmmo::sim::member_change& c, const std::string& error) {
            on_player_add_failed(c, error);
        };
        callbacks.on_failed = [this](uint32_t session, const std::string& reason) {
            std::lock_guard lk(state_mutex_);
            end_physics_session(session, "simulation failed: " + reason);
        };
        runner_ = std::make_unique<bmmo::sim::session_runner>(rc, callbacks);
        Printf("Physics sessions enabled (game root %s, input delay >= %u ticks (per session, from ping), "
               "snapshot interval %u, full every %u, journal %s, patience %s, lead adjust %s).",
               config_.physics_game_root,
               config_.physics_input_delay, rc.snapshot_interval, rc.full_snapshot_interval,
               rc.journal_dir.empty() ? std::string("off") : rc.journal_dir.string(), rc.patience ? "on" : "off",
               rc.lead_adjust ? "on" : "off");
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

    // Lowest join_order not currently held by another member of s; stable for
    // the member's stay in the session (design 9.10).
    // Step M1 (J2): an order held by a life the runner has not removed yet
    // (retiring_orders) is not free either.
    static uint8_t assign_join_order(physics_session_state& s, HSteamNetConnection c) {
        std::set<uint8_t> used, retiring;
        for (const auto& [member, order]: s.join_orders) used.insert(order);
        for (const auto& [life, order]: s.retiring_orders) retiring.insert(order);
        // Step Q7: below K when the session has mechanism slots (a joiner
        // reaches this only once capped_join_order found it an order; a
        // start member past K - a room of more than 64 - keeps the old rule).
        uint8_t order = bmmo::session::lowest_free_join_order(used, retiring);
        if (s.mechanism_slots) {
            const auto choice = bmmo::session::capped_join_order(used, retiring, s.mechanism_slots);
            if (choice.verdict == bmmo::session::join_order_verdict::order) order = choice.order;
            else Printf("Physics session %u: ERROR join order %u assigned at or above the %u mechanism slots.", s.id,
                        static_cast<unsigned>(order), static_cast<unsigned>(s.mechanism_slots));
        }
        s.join_orders[c] = order;
        return order;
    }

    // Step Q7 (2.10 "The cap"): what a joiner gets under the session's K -
    // an order, a wait for a retiring one, or the refusal.
    static bmmo::session::join_order_choice joiner_order(const physics_session_state& s) {
        std::set<uint8_t> used, retiring;
        for (const auto& [member, order]: s.join_orders) used.insert(order);
        for (const auto& [life, order]: s.retiring_orders) retiring.insert(order);
        return bmmo::session::capped_join_order(used, retiring, s.mechanism_slots ? s.mechanism_slots : 256u);
    }

    // Step M1: the member's next life in this session (its add's number).
    static uint16_t begin_life(physics_session_state& s, HSteamNetConnection c) {
        return ++s.lives[c];
    }

    // The session's latency budget, measured once, before the world exists: the
    // input delay tick_scheduler waits for a tick's inputs, and the worst
    // round trip among the members it was sized from - the same number the
    // members present at the start are numbered from.  One pass samples the
    // connections, so both budgets come out of it.
    struct session_latency {
        uint32_t input_delay = 6;
        uint32_t worst_rtt_ms = 0;
    };

    // The worst case among the members: the session's clock is one clock for
    // everyone, so the slack has to suit the member whose input has the
    // furthest to come.  `physics.input_delay` is the floor, for a link too
    // good (or too new) to have shown its latency yet.
    session_latency measure_session_latency(const bmmo::server_room& r) {
        int worst = 0;
        std::string detail;
        for (const auto& m: r.members) {
            SteamNetConnectionRealTimeStatus_t status{};
            interface_->GetConnectionRealTimeStatus(m.id, &status, 0, nullptr);
            auto it = clients_.find(m.id);
            const int peak = it == clients_.end() ? 0 : it->second.ping_peak_ms;
            const int ping = std::max(status.m_nPing < 0 ? 0 : status.m_nPing, peak);
            if (it != clients_.end())
                it->second.ping_peak_ms = static_cast<uint16_t>(std::min(ping, 65535));
            worst = std::max(worst, ping);
            detail += Sprintf("%s%s %d ms", detail.empty() ? "" : ", ", get_client_name(m.id), ping);
        }
        session_latency out;
        out.worst_rtt_ms = static_cast<uint32_t>(worst);
        out.input_delay = bmmo::session::input_delay_for_ping(worst, config_.physics_input_delay);
        Printf("Room %u: input delay %u ticks (%.0f ms) for a worst round trip of %d ms (%s).",
               r.id, out.input_delay, out.input_delay * 1000.0 / 66.0, worst, detail.c_str());
        return out;
    }

    uint32_t start_physics_session(const bmmo::server_room& r, const bmmo::map& map) {
        physics_session_state s;
        s.id = next_session_id_++;
        s.room = r.id;
        s.map = map;
        const session_latency latency = measure_session_latency(r);
        s.input_delay = latency.input_delay;
        s.sector_lead = bmmo::session::sector_lead_for_ping(s.input_delay, static_cast<int>(latency.worst_rtt_ms));
        Printf("Room %u: sector lead %u ticks (%.0f ms): a crossing starts its sector that far ahead, on every member.",
               r.id, s.sector_lead, s.sector_lead * 1000.0 / 66.0);
        // Design 9.2: the members present at the start are anchored at the same
        // lead a late joiner is, measured from tick 0 (the world has not ticked
        // when their SessionAssign goes out).  Their numbering is the server's
        // plus that lead, and each of them renumbers itself onto it when the
        // assignment arrives, so a slow level load no longer leaves its member
        // numbering from its own anchor and running that far ahead of the world
        // for the rest of the session (sim/late_tick.hpp,
        // session_start_tick_base).
        s.start_tick_base = bmmo::sim::session_start_tick_base(s.input_delay, latency.worst_rtt_ms);
        for (const auto& m: r.members) {
            s.members.push_back(m.id);
            client_session_[m.id] = s.id;
            assign_join_order(s, m.id);
            begin_life(s, m.id);   // 1: the runner's create_session numbers its members the same
        }
        // Design 9.10: one number for the whole session - the world is built
        // with it and every SessionStart carries it.  Decided here, before the
        // world exists, because world_options is frozen at create_session: a
        // member joining or leaving during the world's boot must not be able
        // to move it out from under the clients that were already told.
        // Step SPX: none while the spawn ghost covers every spawn birth (the
        // balls are born at the resetpoint itself, unkicked, and pass through
        // each other until they overlap none); the configured values only in
        // a build with the ghost off (session/spawn_impulse.hpp).
        {
            const auto separation = bmmo::session::session_spawn_separation(
                s.members.size(), config_.physics_spawn_impulse, config_.physics_spawn_spacing);
            s.spawn_impulse = separation.impulse;
            s.spawn_spacing = separation.spacing;
        }
        // Protocol 11: the world options, fixed here with the rest (the world
        // is built with them, every SessionStart carries them).  The trafo
        // design wanted WORLD_TRAFO_INPUTS "when every member has the
        // capability", but the capabilities arrive with SessionReady, after
        // SessionStart: the bit is the server's config, and a member that
        // cannot run it is refused at its SessionReady (handle_session_ready).
        s.world_options = config_.physics_trafo_inputs ? bmmo::session::WORLD_TRAFO_INPUTS : 0u;
        if (s.world_options & bmmo::session::WORLD_TRAFO_INPUTS)
            Printf("Room %u: the trafo is an input in this session (world option trafo inputs).", r.id);
        // Protocol 12 (sector placement design 2.4, step S5): the same rule
        // for the placement - the config's, refused at SessionReady for a
        // member without CAP_PLACEMENT.
        if (config_.physics_placement_clones) {
            s.world_options |= bmmo::session::WORLD_PLACEMENT_CLONES;
            Printf("Room %u: every PH row gets its own object in this session (world option placement clones).", r.id);
        }
        // Step G2 (world-agreement-gaps-design.md 2.4, 5): the proximity
        // union's rule in the server's world; from step G3 every client runs
        // it too and SessionReady's anchor_probes is compared (the config's
        // default on since G3).
        if (config_.physics_proximity_union) {
            s.world_options |= bmmo::session::WORLD_PROXIMITY_UNION;
            Printf("Room %u: every world places the mechanism proximity probes by the union's rule in this session "
                   "(world option proximity union; BodyRevived retired).", r.id);
        }
        // Step Q7 (world-agreement-gaps-design.md 2.10, 5): one mechanism
        // responds to every player - K slot copies of every per-player unit
        // in every world, K the room capacity clamped to [1, 64], fixed for
        // the session (SessionStart carries it).  With the union only.
        if (config_.physics_player_mechanisms && (s.world_options & bmmo::session::WORLD_PROXIMITY_UNION)) {
            s.world_options |= bmmo::session::WORLD_PLAYER_MECHANISMS;
            s.mechanism_slots = bmmo::session::units::mechanism_slot_count(
                config_.maximum_members, static_cast<uint32_t>(bmmo::session::MAX_PLAYERS_PER_SESSION));
            Printf("Room %u: one mechanism responds to every player in this session (world option player mechanisms, "
                   "%u slots; join orders below it).", r.id, static_cast<unsigned>(s.mechanism_slots));
        }
        // Step RK3 (record-format-gaps-design.md 8 row K3 = C4, option (c)):
        // on Level 1 every world switches the tutorial off at the same frame
        // after its anchor (game/tutorial_exit.hpp); the bit is harmless on
        // the other levels, so it is set whatever the level.
        if (config_.physics_session_clock) {
            s.world_options |= bmmo::session::WORLD_SESSION_CLOCK;
            Printf("Room %u: the Level 1 tutorial is switched off in every world of this session (world option "
                   "session clock).", r.id);
        }
        room_session_[r.id] = s.id;
        const uint32_t id = s.id;
        std::vector<std::pair<uint32_t, uint8_t>> players;
        std::vector<std::string> names;
        players.reserve(s.members.size());
        names.reserve(s.members.size());
        // What the journal's first note says: the room and who was in it when
        // the session started, so a file found weeks later still names names.
        std::string note = Sprintf("room %u \"%s\":", r.id, r.name);
        for (size_t i = 0; i < s.members.size(); ++i) {
            const auto m = s.members[i];
            players.emplace_back(m, s.join_orders[m]);
            names.push_back(get_client_name(m));
            note += Sprintf("%s %s(%u, join %u)", i == 0 ? "" : ",", names.back(),
                    static_cast<unsigned>(m), static_cast<unsigned>(s.join_orders[m]));
        }
        const uint32_t input_delay = s.input_delay;
        const uint32_t start_base = s.start_tick_base;
        const float spawn_impulse = s.spawn_impulse;
        const uint32_t sector_lead = s.sector_lead;
        const uint32_t world_options = s.world_options;
        const uint8_t mechanism_slots = s.mechanism_slots;
        physics_sessions_.emplace(id, std::move(s));
        // The world is numbered from the same base its members are (design 9.25
        // phase alignment): its first simulated tick is `start_base`, so the
        // server and every client take their first step from their anchor under
        // one tick number instead of the server being `start_base` steps ahead
        // of them at every number for the rest of the session.
        runner_->create_session(id, map.level, players, input_delay, start_base, spawn_impulse, note, std::move(names),
                                sector_lead, world_options, mechanism_slots);
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
        // A run of substitutions still open is logged with the rest (S2).
        for (auto& [m, n]: s.notices) close_held_run(s, m, n);
        for (const auto m: s.members) {
            client_session_.erase(m);
            send_session_end(session, m, reason);
            send_room_event(m, bmmo::room::event_type::SessionEnded, bmmo::room::error_code::None, s.room,
                    k_HSteamNetConnection_Invalid, k_HSteamNetConnection_Invalid, reason);
        }
        // Step Q7: the joiners still waiting for a join order are told too.
        for (const auto w: s.waiting_orders) {
            client_session_.erase(w);
            send_session_end(session, w, reason);
            send_room_event(w, bmmo::room::event_type::SessionEnded, bmmo::room::error_code::None, s.room,
                    k_HSteamNetConnection_Invalid, k_HSteamNetConnection_Invalid, reason);
        }
        room_session_.erase(s.room);
        rooms_.reset_session(s.room);
        if (runner_) runner_->destroy_session(session, reason);
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
        msg.input_delay = static_cast<uint8_t>(std::min<uint32_t>(255, s.input_delay));
        msg.sector_lead = static_cast<uint8_t>(std::min<uint32_t>(255, s.sector_lead));
        msg.first_tick = first_tick;
        msg.seed = physics_seed_;   // the seed the server's world runs with
        msg.spawn_impulse = s.spawn_impulse;
        msg.spawn_spacing = s.spawn_spacing;
        msg.world_options = s.world_options;
        msg.mechanism_slots = s.mechanism_slots;   // protocol 19 (step Q7)
        // Every member spawns at the retail resetpoint (design 9.10): the
        // kick set below moves same-tick spawns apart instead of a ring.
        for (size_t i = 0; i < s.members.size() && i < bmmo::session::MAX_PLAYERS_PER_SESSION; ++i) {
            bmmo::session::player_entry entry;
            entry.id = s.members[i];
            const auto jo = s.join_orders.find(entry.id);
            entry.join_order = jo != s.join_orders.end() ? jo->second : static_cast<uint8_t>(i);
            entry.life = s.lives.count(entry.id) ? s.lives.at(entry.id) : 1;   // step M1
            entry.ball_type = 0;
            for (int k = 0; k < 3; ++k) entry.spawn_position[k] = s.spawn_position[k];
            for (int k = 0; k < 4; ++k) entry.spawn_rotation[k] = s.spawn_rotation[k];
            msg.players.push_back(entry);
        }
        msg.serialize();
        send(to, msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        s.started.insert(to);   // step M1: SessionMember goes to it from now on
    }

    // Step M1 (world-agreement-gaps-design.md 2.7): a SessionMember to every
    // member that has its SessionStart, except the player itself, reliably
    // and through the lifecycle events' unreliable repeats (a lost reliable
    // copy is ~400 ms late by retransmit).  --hold-member-ms holds both.
    void send_session_member(physics_session_state& s, uint8_t kind, uint32_t tick, const bmmo::session::player_entry& entry) {
        bmmo::session_member_msg msg;
        msg.session = s.id;
        msg.kind = kind;
        msg.tick = tick;
        msg.entry = entry;
        msg.serialize();
        std::string bytes = msg.raw.str();
        std::vector<uint32_t> to;
        for (const auto m: s.started)
            if (m != static_cast<HSteamNetConnection>(entry.id)
                    && std::find(s.members.begin(), s.members.end(), m) != s.members.end())
                to.push_back(static_cast<uint32_t>(m));
        ++s.member_messages;
        Printf("Physics session %u: SessionMember %s p%u life %u join order %u at tick %u to %zu member(s)%s.", s.id,
               kind == bmmo::session::MEMBER_ADDED ? "added" : "removed", entry.id, static_cast<unsigned>(entry.life),
               static_cast<unsigned>(entry.join_order), tick, to.size(),
               test_hold_member_ms() ? Sprintf(" (held %u ms)", test_hold_member_ms()).c_str() : "");
        if (to.empty()) return;
        if (test_hold_member_ms() > 0) {
            s.held_members.push_back({std::chrono::steady_clock::now() + std::chrono::milliseconds(test_hold_member_ms()),
                                      std::move(to), std::move(bytes)});
            return;
        }
        for (const auto m: to)
            send(static_cast<HSteamNetConnection>(m), bytes.data(), bytes.size(), k_nSteamNetworkingSend_ReliableNoNagle);
        s.event_repeats.push(std::move(bytes), std::move(to));
    }

    bmmo::session::player_entry member_entry(const physics_session_state& s, const bmmo::sim::member_change& c) const {
        bmmo::session::player_entry entry;
        entry.id = c.player;
        entry.join_order = c.join_order;
        entry.life = c.life;
        entry.ball_type = 0;
        for (int k = 0; k < 3; ++k) entry.spawn_position[k] = s.spawn_position[k];
        for (int k = 0; k < 4; ++k) entry.spawn_rotation[k] = s.spawn_rotation[k];
        return entry;
    }

    // Simulation thread (step M1): a life entered the runner's world.  A late
    // joiner hears of the session only now (its SessionStart), so nothing of
    // it can precede its add; every member with a SessionStart is told.
    void on_player_added(const bmmo::sim::member_change& c) {
        std::lock_guard lk(state_mutex_);
        auto it = physics_sessions_.find(c.session);
        if (it == physics_sessions_.end()) return;
        auto& s = it->second;
        const auto member = static_cast<HSteamNetConnection>(c.player);
        const bool is_member = std::find(s.members.begin(), s.members.end(), member) != s.members.end();
        if (is_member && s.late.count(member) && !s.started.count(member)) send_session_start(s, member, 0);
        send_session_member(s, bmmo::session::MEMBER_ADDED, c.tick, member_entry(s, c));
    }

    // Simulation thread (step M1): a life left the runner's world (R), or a
    // life that never reached it was dropped.  Its join order is free again.
    void on_player_removed(const bmmo::sim::member_change& c) {
        std::lock_guard lk(state_mutex_);
        auto it = physics_sessions_.find(c.session);
        if (it == physics_sessions_.end()) return;
        auto& s = it->second;
        s.retiring_orders.erase({static_cast<HSteamNetConnection>(c.player), c.life});
        if (c.in_world) send_session_member(s, bmmo::session::MEMBER_REMOVED, c.tick, member_entry(s, c));
        admit_waiting(s);   // step Q7: the released order to the first waiting joiner
    }

    // Simulation thread (step M1): the world refused the add.  The member
    // gets SessionEnd instead of a SessionStart for a world with no slot for
    // it, and leaves the session; nothing held its order.
    void on_player_add_failed(const bmmo::sim::member_change& c, const std::string& error) {
        std::lock_guard lk(state_mutex_);
        auto it = physics_sessions_.find(c.session);
        if (it == physics_sessions_.end()) return;
        const auto member = static_cast<HSteamNetConnection>(c.player);
        Printf("Physics session %u: %s (life %u) could not be added to the world: %s", c.session,
               get_client_name(member), static_cast<unsigned>(c.life), error);
        // Only while the member is still on this life: one that left and
        // came back meanwhile is on a newer life, which this does not end.
        const auto cs = client_session_.find(member);
        const auto& lives = it->second.lives;
        const bool same_life = lives.count(member) && lives.at(member) == c.life;
        if (cs != client_session_.end() && cs->second == c.session && same_life) {
            send_session_end(c.session, member, "could not be added to the world: " + error);
            physics_session_member_left(member);
        }
        if (auto again = physics_sessions_.find(c.session); again != physics_sessions_.end()) {
            again->second.retiring_orders.erase({member, c.life});
            admit_waiting(again->second);   // step Q7
        }
    }

    // Step Q7 (world-agreement-gaps-design.md 2.10 "The cap"): the waiting
    // joiners, first come first served, while an order below K is free.  A
    // waiter that left the room meanwhile was taken off the list by
    // physics_session_member_left.
    void admit_waiting(physics_session_state& s) {
        while (!s.waiting_orders.empty()) {
            if (joiner_order(s).verdict != bmmo::session::join_order_verdict::order) return;
            const HSteamNetConnection c = s.waiting_orders.front();
            s.waiting_orders.pop_front();
            Printf("Physics session %u: %s gets a released join order.", s.id, get_client_name(c));
            admit_member(s, c);
        }
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
        s.anchor_placement = info.anchor_placement;
        s.anchor_probes = info.anchor_probes;   // step G3
        s.ball_rows = info.ball_rows;
        s.trafo_points = info.trafo_points;
        for (int k = 0; k < 3; ++k) s.spawn_position[k] = info.spawn_position[k];
        for (int k = 0; k < 4; ++k) s.spawn_rotation[k] = info.spawn_rotation[k];
        s.resetpoints = info.resetpoints;
        // s.spawn_impulse was fixed in start_physics_session, together with
        // the world's, and does not move for the session's lifetime.
        for (const auto m: s.members) send_session_start(s, m, 0);
        // The barrier starts now: from here on check_start_barriers() will drop
        // members that never answer with SessionReady once the window is up.
        s.start_barrier_armed = true;
        s.start_deadline = std::chrono::steady_clock::now() + bmmo::sim::kStartBarrierTimeout;
        Printf("Physics session %u: world ready (anchor %016llx, placement %016llx), SessionStart sent to %zu players.",
                s.id, static_cast<unsigned long long>(s.anchor_hash), static_cast<unsigned long long>(s.anchor_placement),
                s.members.size());
    }

    // Simulation thread: fan a snapshot out to the room.
    // Relay of the inputs the world applied at `tick` (design 9.1): every
    // member gets the other members' frames.
    //
    // Unreliable, with a redundancy window (protocol 5; session/input_relay.hpp).
    // This stream is what every client replays to reproduce the shared world,
    // so a frame that never arrives is a divergence only a snapshot repairs -
    // and reliable delivery (2026-10-19 .. 10-21) turned out to be the wrong
    // way to make it complete: a lost datagram held every frame behind it
    // until the retransmit, and under 10 % loss the corrections reached the
    // clients past the rollback's 48-tick reach, where they could only be
    // written as poses.  Now every datagram repeats the last relay_window
    // ticks per member, so a frame is lost only when that many datagrams in a
    // row are, and nothing waits behind a loss; the camera is omitted where
    // it did not move, which is most ticks, so the window costs 4 bytes a
    // frame.  The lifecycle events keep their own reliable ordered stream and
    // can now overtake the relayed inputs of earlier ticks; the clients land
    // every frame by its tick and park a frame for a mirror not yet made.
    void on_session_inputs(uint32_t session, uint32_t tick,
                           const std::vector<bmmo::session::applied_input>& applied) {
        std::lock_guard lk(state_mutex_);
        auto it = physics_sessions_.find(session);
        if (it == physics_sessions_.end()) return;
        auto& s = it->second;
        if (s.relay.window() != config_.physics_relay_window) s.relay.set_window(config_.physics_relay_window);
        s.relay.push(tick, applied);
        // Unreliable, no delay, with the window: a lost datagram costs nothing
        // unless `relay_window` in a row are lost, and no frame ever waits
        // behind a loss.  (Reliable and ordered, 2026-10-19 .. 10-21, a loss
        // held every frame behind it until the retransmit landed, and under
        // 10 % loss the corrections reached the clients past the rollback's
        // reach and were applied as pose writes.)
        // Members only: a step already running when a member left still has
        // its frame in `applied`, and a datagram about it after PlayerLeft
        // would re-announce it in every client's journal.  Whole entries per
        // datagram of one packet's worth: an unreliable message split over
        // packets is lost whole when any of them is, and one past 16.5 KB
        // GameNetworkingSockets quietly sends reliable.
        const auto is_member = [&s](uint32_t player) {
            return std::find(s.members.begin(), s.members.end(), static_cast<HSteamNetConnection>(player)) != s.members.end();
        };
        // Protocol 7: each member's own ack and substitutions, before its
        // datagrams are built from them.
        for (const auto& a: applied) {
            if (!is_member(a.player)) continue;
            auto& n = s.notices[static_cast<HSteamNetConnection>(a.player)];
            if (!a.held) {
                // Never backwards: a frame of the old numbering applied fresh
                // after the assignment (the runner re-anchors the cursor a
                // little later) must not undo the move to the base - 1.
                if (static_cast<int32_t>(tick - n.acked) > 0) n.acked = tick;
                close_held_run(s, static_cast<HSteamNetConnection>(a.player), n);
                continue;
            }
            if (!n.assigned || static_cast<int32_t>(tick - n.epoch) < 0) continue;
            ++n.held_total;
            note_held_tick(s, static_cast<HSteamNetConnection>(a.player), n, tick, a.source);
        }
        // Every member every tick, even with nothing else to say: the header
        // and the held section are the recipient's own, and a solo player,
        // or one whose window happens to be empty, needs them as much as
        // anyone (the header-only datagram is 26 bytes).
        // Protocol 13 (step R12): the recipient's wait, as the runner
        // measured it for this step; 0 for a member the step did not drive.
        const auto wait_of = [&applied](HSteamNetConnection m) -> uint8_t {
            for (const auto& a: applied)
                if (static_cast<HSteamNetConnection>(a.player) == m) return a.wait;
            return 0;
        };
        for (const auto m: s.members) {
            auto& n = s.notices[m];
            const uint8_t wait = wait_of(m);
            size_t trimmed = 0;
            const std::optional<uint32_t> held_from = n.assigned ? std::optional<uint32_t>(n.epoch) : std::nullopt;
            for (auto& datagram: s.relay.build_datagrams(m, bmmo::session::kRelayDatagramBudget, is_member, held_from,
                                                         &trimmed)) {
                bmmo::session_remote_input_msg msg;
                msg.session = session;
                msg.tick = tick;
                msg.acked = n.acked;
                msg.held_total = n.held_total;
                msg.epoch = n.epoch;
                msg.wait = wait;
                for (auto& w: datagram.entries) {
                    bmmo::session_remote_input_msg::entry e;
                    e.player = w.player;
                    e.frames = std::move(w.frames);
                    msg.entries.push_back(std::move(e));
                }
                for (const auto& h: datagram.held) {
                    bmmo::session_remote_input_msg::held_record r;
                    r.back = static_cast<uint8_t>(tick - h.tick);
                    if (bmmo::session::held_inline(h.tick, h.source, n.epoch)) {
                        r.has_frame = true;
                        r.frame = h.frame;
                    } else {
                        r.source = h.source;
                    }
                    msg.held.push_back(r);
                }
                msg.serialize();
                const EResult result = send(m, msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_UnreliableNoDelay);
                if (result != k_EResultOK || msg.held.empty()) continue;
                // A record's first trip is the datagram of its own tick (back
                // 0); the next 31 ticks' datagrams repeat it.  Counted only
                // once the socket took the datagram, so S2 `sent` below `n`
                // means first trips the server itself failed to send.
                n.copies += msg.held.size();
                if (n.run.open && n.run.last == tick)
                    for (const auto& r: msg.held) if (r.back == 0) ++n.run.sent;
            }
            if (trimmed && !s.relay_trim_logged) {
                s.relay_trim_logged = true;
                Printf("Physics session %u: a member's relayed input window did not fit one datagram (%u bytes); "
                       "its oldest frames were dropped.", s.id, static_cast<unsigned>(bmmo::session::kRelayDatagramBudget));
            }
        }
    }

    // The S2 lines (late-player design 5.1): one per run of consecutive
    // substituted ticks of a member, at or after its epoch, with the sources
    // the substitutes came from as (tick - source) runs, `d` the default
    // frame.  `n` is how many ticks the run held, `sent` how many of their
    // records the socket took in their own tick's datagram, `total` the
    // member's held_total, which every datagram's header carries, `copies`
    // every held record the socket took for the member this epoch, repeats
    // included.  The end-to-end check is per member: the S2 lines' n summed
    // against the client's subst recv + late + base.  A run longer than
    // a second is cut into lines of 66 ticks, so persistent lateness still
    // shows up while it lasts.
    static constexpr uint32_t kHeldRunLineTicks = 66;
    static constexpr uint32_t kHeldRunLines = 20000;   // per session; past it one line says so

    void note_held_tick(physics_session_state& s, HSteamNetConnection m, physics_session_state::held_notice& n,
                        uint32_t tick, uint32_t source) {
        auto& r = n.run;
        if (r.open && (r.last + 1 != tick || r.count >= kHeldRunLineTicks)) close_held_run(s, m, n);
        const uint32_t delay = source == bmmo::session::INPUT_SOURCE_DEFAULT ? 0xFFFFFFFFu : tick - source;
        if (!r.open) {
            r = physics_session_state::held_run{};
            r.open = true;
            r.first = tick;
            r.source_min = r.source_max = source;
        }
        r.last = tick;
        ++r.count;
        if (source != bmmo::session::INPUT_SOURCE_DEFAULT) {
            if (r.source_min == bmmo::session::INPUT_SOURCE_DEFAULT || source < r.source_min) r.source_min = source;
            if (r.source_max == bmmo::session::INPUT_SOURCE_DEFAULT || source > r.source_max) r.source_max = source;
        }
        if (!r.delays.empty() && r.delays.back().first == delay) ++r.delays.back().second;
        else r.delays.emplace_back(delay, 1u);
    }

    void close_held_run(physics_session_state& s, HSteamNetConnection m, physics_session_state::held_notice& n) {
        auto& r = n.run;
        if (!r.open) return;
        r.open = false;
        if (s.s2_lines > kHeldRunLines) return;
        if (++s.s2_lines > kHeldRunLines) {
            Printf("Physics session %u: S2 lines capped at %u.", s.id, kHeldRunLines);
            return;
        }
        std::string delays;
        for (size_t i = 0; i < r.delays.size(); ++i) {
            if (i == 12) { delays += ",+" + std::to_string(r.delays.size() - i); break; }
            if (i) delays += ',';
            delays += r.delays[i].first == 0xFFFFFFFFu ? std::string("d") : std::to_string(r.delays[i].first);
            delays += '*' + std::to_string(r.delays[i].second);
        }
        const auto source = [](uint32_t t) {
            return t == bmmo::session::INPUT_SOURCE_DEFAULT ? std::string("d") : std::to_string(t);
        };
        Printf("Physics session %u: S2 held player=%u ticks=%u..%u n=%u sent=%u total=%u copies=%llu epoch=%u src=%s..%s delay=%s",
               s.id, static_cast<unsigned>(m), r.first, r.last, r.count, r.sent, n.held_total,
               static_cast<unsigned long long>(n.copies), n.epoch, source(r.source_min), source(r.source_max), delays);
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

    // Step A3 (resync design 2.3): the server's state of tick base - 1, to
    // the resynced member alone, reliable (GameNetworkingSockets fragments
    // it).  The server never reads it back.
    void on_session_resync_state(const bmmo::sim::resync_state_out& out) {
        std::lock_guard lk(state_mutex_);
        auto it = physics_sessions_.find(out.session);
        if (it == physics_sessions_.end()) return;
        auto& s = it->second;
        const auto c = static_cast<HSteamNetConnection>(out.player);
        if (std::find(s.members.begin(), s.members.end(), c) == s.members.end()) return;
        bmmo::session_resync_state_msg msg;
        msg.session = out.session;
        msg.tick = out.tick;
        msg.base = out.base;
        msg.rows = out.state.rows;
        msg.clones = out.state.clones;
        msg.navs = out.state.navs;
        msg.union_state = out.state.union_state;
        msg.scripts = out.state.scripts;
        msg.savestate = out.state.savestate;
        msg.scripts_size = out.state.scripts_size;
        msg.savestate_size = out.state.savestate_size;
        msg.trafo_ledger = out.state.trafo_ledger;
        // serialize() caps the rows at MAX_BODIES_PER_SNAPSHOT and each byte
        // section at kMaxBytesField: a state that large is sent cut, and the
        // client's restore of a cut blob refuses it - said here, not silently.
        std::string cut;
        if (msg.rows.size() > bmmo::session::MAX_BODIES_PER_SNAPSHOT)
            cut += " rows " + std::to_string(msg.rows.size()) + " > " + std::to_string(bmmo::session::MAX_BODIES_PER_SNAPSHOT);
        for (const auto& [bytes, what]: {std::pair<const std::vector<uint8_t>*, const char*>{&msg.union_state, "union"},
                                         {&msg.scripts, "scripts"}, {&msg.savestate, "savestate"}})
            if (bytes->size() > bmmo::session_resync_state_msg::kMaxBytesField)
                cut += std::string(" ") + what + " " + std::to_string(bytes->size()) + " bytes";
        if (msg.trafo_ledger.size() > bmmo::session_resync_state_msg::kMaxTrafoLedger)
            cut += " trafo ledger " + std::to_string(msg.trafo_ledger.size()) + " > "
                 + std::to_string(bmmo::session_resync_state_msg::kMaxTrafoLedger) + " entries";
        if (!cut.empty())
            Printf("Physics session %u: the resync state for %s is cut on the wire (%s): the client will refuse what is cut.",
                   s.id, get_client_name(c), cut.c_str());
        msg.serialize();
        const EResult sent = send(c, msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        bmmo::session::sector_union::state_type u;
        std::string active;
        if (bmmo::session::sector_union::deserialize(out.state.union_state.data(), out.state.union_state.size(), u))
            for (int sector: u.active) active += (active.empty() ? "" : ",") + std::to_string(sector);
        std::string own_clone = "-";
        for (const auto& [player, name]: out.state.clones)
            if (player == out.player) own_clone = name;
        Printf("Physics session %u: resync state for %s: tick %u (base %u), %zu bodies, %u script bytes (%zu packed), %u "
               "savestate bytes (%zu packed), own clone %s, union {%s}, %zu bytes on the wire (result %d), emitted in %llu us%s%s.",
               s.id, get_client_name(c), out.tick, out.base, out.state.rows.size(),
               out.state.scripts_size ? out.state.scripts_size : static_cast<uint32_t>(out.state.scripts.size()),
               out.state.scripts.size(),
               out.state.savestate_size ? out.state.savestate_size : static_cast<uint32_t>(out.state.savestate.size()),
               out.state.savestate.size(), own_clone.c_str(), active.c_str(),
               static_cast<size_t>(msg.size()), static_cast<int>(sent), static_cast<unsigned long long>(out.emit_us),
               out.state.error.empty() ? "" : ": ", out.state.error.c_str());
    }

    // The runner had stepped the base's tick before the request reached it:
    // the member is answered again with a new base, counted on the session's
    // fixed deadline schedule (session_runner::reanchor_tick), floored at the
    // runner's current tick + 2, and a new assignment with the flag; the
    // client takes the newer one.  Only a member that applies the resync
    // state (CAP_RESYNC_STATE) is ever answered here.
    void on_resync_base_missed(uint32_t session, uint32_t player, uint32_t base) {
        std::lock_guard lk(state_mutex_);
        auto it = physics_sessions_.find(session);
        if (it == physics_sessions_.end()) return;
        auto& s = it->second;
        const auto c = static_cast<HSteamNetConnection>(player);
        if (std::find(s.members.begin(), s.members.end(), c) == s.members.end()) return;
        const uint32_t again = resync_member(s, c, 0);
        Printf("Physics session %u: resync base %u for %s was stepped before the request reached the runner: re-assigned at %u.",
               s.id, base, get_client_name(c), again);
    }

    void physics_session_member_joined(uint32_t room, HSteamNetConnection c) {
        auto rs = room_session_.find(room);
        if (rs == room_session_.end() || !runner_) return;
        auto it = physics_sessions_.find(rs->second);
        if (it == physics_sessions_.end()) return;
        auto& s = it->second;
        // Step Q7 (world-agreement-gaps-design.md 2.10 "The cap"): with K
        // mechanism slots a joiner takes an order below K; it waits for a
        // retiring one, or - every order below K held by a live life - is
        // refused the physics session and stays in the room.
        if (s.mechanism_slots) {
            const auto choice = joiner_order(s);
            if (choice.verdict == bmmo::session::join_order_verdict::refuse) {
                Printf("Physics session %u: %s refused: physics session full (%u mechanism slots).", s.id,
                       get_client_name(c), static_cast<unsigned>(s.mechanism_slots));
                send_session_end(s.id, c, Sprintf("physics session full (%u mechanism slots)",
                                                  static_cast<unsigned>(s.mechanism_slots)));
                return;
            }
            if (choice.verdict == bmmo::session::join_order_verdict::wait) {
                s.waiting_orders.push_back(c);
                client_session_[c] = s.id;
                Printf("Physics session %u: %s waits for a join order (every order below the %u mechanism slots is "
                       "held or retiring).", s.id, get_client_name(c), static_cast<unsigned>(s.mechanism_slots));
                return;
            }
        }
        admit_member(s, c);
    }

    // A joiner into the session's world (its order, life and add; step M1's
    // body of physics_session_member_joined, step Q7 calls it for a waiter).
    void admit_member(physics_session_state& s, HSteamNetConnection c) {
        s.members.push_back(c);
        client_session_[c] = s.id;
        // Sample this connection's round trip now.  The periodic latency
        // broadcast is up to PING_INTERVAL_TICKS away, and until it lands
        // ping_peak_ms is 0, which would give the first SessionAssign no
        // round-trip allowance at all.
        {
            SteamNetConnectionRealTimeStatus_t status{};
            interface_->GetConnectionRealTimeStatus(c, &status, 0, nullptr);
            const int ping_ms = std::max(status.m_nPing, 0);
            const auto ping = (uint16_t) std::min(ping_ms, (int) std::numeric_limits<uint16_t>::max());
            if (auto ci = clients_.find(c); ci != clients_.end())
                ci->second.ping_peak_ms = std::max(ci->second.ping_peak_ms, ping);
        }
        const uint8_t join_order = assign_join_order(s, c);
        const uint16_t life = begin_life(s, c);
        runner_->add_player(s.id, c, join_order, life, get_client_name(c));
        if (runner_->running(s.id)) {
            // Step M1 (world-agreement-gaps-design.md 2.7): its SessionStart
            // goes from on_player_added, once the runner holds this life (a
            // rejoin waits there for its old life's removal); the real tick
            // base follows in SessionAssign.
            s.late.insert(c);
        } else if (s.world_ready) {
            send_session_start(s, c, 0);
            // A member joining before the barrier lifted needs the window to
            // answer with SessionReady, but only the first such join arms it:
            // re-arming on every join would let a trickle of joiners postpone
            // the deadline for ever.  A join while the barrier is pending
            // shares the deadline already running.
            if (!s.start_barrier_armed) {
                s.start_barrier_armed = true;
                s.start_deadline = std::chrono::steady_clock::now() + bmmo::sim::kStartBarrierTimeout;
            }
        }
        Printf("Physics session %u: %s joined%s (life %u, join order %u).", s.id, get_client_name(c),
               s.late.count(c) ? " late" : "", static_cast<unsigned>(life), static_cast<unsigned>(join_order));
    }

    void physics_session_member_left(HSteamNetConnection c) {
        auto cs = client_session_.find(c);
        if (cs == client_session_.end()) return;
        const uint32_t session = cs->second;
        client_session_.erase(cs);
        auto it = physics_sessions_.find(session);
        if (it == physics_sessions_.end()) return;
        auto& s = it->second;
        // Step Q7: a joiner still waiting for a join order holds nothing else.
        if (const auto w = std::find(s.waiting_orders.begin(), s.waiting_orders.end(), c); w != s.waiting_orders.end()) {
            s.waiting_orders.erase(w);
            return;
        }
        s.members.erase(std::remove(s.members.begin(), s.members.end(), c), s.members.end());
        s.assign_repeats.erase(std::remove_if(s.assign_repeats.begin(), s.assign_repeats.end(),
                                              [c](const physics_session_state::assign_repeat& r) { return r.to == c; }),
                               s.assign_repeats.end());
        retire_lead_repeats(s, c);
        s.leads.erase(c);
        s.event_repeats.forget(c);
        s.ready.erase(c);
        s.assigned.erase(c);
        if (const auto notice = s.notices.find(c); notice != s.notices.end()) {
            close_held_run(s, c, notice->second);
            s.notices.erase(notice);
        }
        s.late.erase(c);
        s.started.erase(c);
        // Step M1 (J2): the order stays held until the runner removed this
        // life (on_player_removed), so no joiner can take it meanwhile.
        if (const auto jo = s.join_orders.find(c); jo != s.join_orders.end()) {
            s.retiring_orders[{c, s.lives.count(c) ? s.lives.at(c) : uint16_t{0}}] = jo->second;
            s.join_orders.erase(jo);
        }
        for (auto& h: s.held_members) h.to.erase(std::remove(h.to.begin(), h.to.end(), static_cast<uint32_t>(c)), h.to.end());
        s.last_physicalize.erase(c);
        s.last_unphysicalize.erase(c);
        // Its events still waiting for the runner stay: their acceptance was
        // posted before the removal, so they are relayed to the others first,
        // and the leaver's Unphysicalize after them.
        // The others still mirror this ball: the runner relays an
        // Unphysicalize on the leaver's behalf (design 9.3) through
        // on_event_accepted, at its own next tick and after the ball's newest
        // accepted Physicalize (late-player design 3.2) - no longer stamped
        // here from the network thread's view of the runner's tick.
        if (runner_) runner_->remove_player(session, c);
        if (s.members.empty()) end_physics_session(session, "everyone left");
        else assign_start_members(s);   // the leaver may have been the one everybody waited for
    }

    // With a join or resync state of S = base - 1 (trafo-replication design
    // 4.7, T2's review): the Trafo events applied at or after the base,
    // which the ledger of S cannot name.  One accepted before a joiner was a
    // member reaches it only from here (the relay went before it joined); a
    // member that has one already drops the copy by its key.  Entries far in
    // the past are pruned on the way.
    void resend_trafo_relayed(physics_session_state& s, HSteamNetConnection c, uint32_t base) {
        constexpr uint32_t kKeep = 4 * bmmo::session::kTrafoLife;
        s.trafo_relayed.erase(std::remove_if(s.trafo_relayed.begin(), s.trafo_relayed.end(),
                                             [base](const auto& e) { return static_cast<int32_t>(e.first + kKeep - base) < 0; }),
                              s.trafo_relayed.end());
        size_t sent = 0;
        for (const auto& [applied, bytes]: s.trafo_relayed)
            if (static_cast<int32_t>(applied - base) >= 0) {
                send(c, bytes.data(), bytes.size(), k_nSteamNetworkingSend_Reliable);
                ++sent;
            }
        if (sent)
            Printf("Physics session %u: %zu Trafo event(s) applied at or after tick %u resent to %s with its state.", s.id, sent,
                   base, get_client_name(c));
    }

    // The tick a resync is anchored at off the per-player path.  input_delay
    // ticks ahead of the server is not enough on its own: the SessionAssign
    // that carries the base is reliable, so the client only learns it a full
    // round trip after the server computed it, and the frames it stamps from
    // there would reach the server behind the ticks they name.  The member's
    // worst observed round trip, rounded up to whole ticks, pays for that
    // trip - up to the caps in sim/late_tick.hpp - and, since plan step 20,
    // clamped inside the rollback window exactly as the start base is
    // (resync_lead_ticks), so a resynced member's relay lag comes back to
    // about where the start put it instead of 4 ticks past it.  Since step
    // RL1 the lead is counted on the session's fixed deadline schedule, not
    // over the runner's current tick (session_runner::reanchor_tick): a
    // resync made while the server waits on that member's deadlines used to
    // leave every other member about 15 ticks of relay lag behind for the
    // rest of the session.  `resync_state`: the member applies the resync
    // state (CAP_RESYNC_STATE); one that does not keeps a floor of 3 ticks
    // over the runner (step RL1b), it has no base-missed re-answer.
    uint32_t resync_tick_base(const physics_session_state& s, HSteamNetConnection c, bool resync_state) const {
        const auto it = clients_.find(c);
        const uint32_t rtt_ms = it == clients_.end() ? 0u : it->second.ping_peak_ms;
        return runner_->reanchor_tick(s.id, bmmo::sim::resync_lead_ticks(s.input_delay, rtt_ms), resync_state);
    }

    // The lead a late joiner is anchored at.  On the per-player path
    // (physics.lead_adjust on and the joiner declared CAP_LEAD,
    // uses_player_lead) it is the player's own (late-player design 2.5,
    // late_join_lead_ticks): the whole CURRENT round trip, capped only at
    // the 80-tick sanity bound, so its frames arrive on time from its first
    // tick.  Not the peak: uncapped, one old spike in the lobby would anchor
    // the joiner at the sanity bound, far past its reach.  The caps of 12
    // and 34 stay with the start base, and with every other joiner: the
    // capped late-tick lead from the peak, as before the flag.
    uint32_t late_join_lead(const physics_session_state& s, HSteamNetConnection c, uint32_t caps) const {
        if (bmmo::sim::uses_player_lead(config_.physics_lead_adjust, caps))
            return bmmo::sim::late_join_lead_ticks(s.input_delay, current_rtt_ms(c));
        const auto it = clients_.find(c);
        const uint32_t rtt_ms = it == clients_.end() ? 0u : it->second.ping_peak_ms;
        return bmmo::sim::late_tick_lead_ticks(s.input_delay, rtt_ms);
    }

    // The member's current round trip (m_nPing), not ping_peak_ms: E is sized
    // with it (late-player design 2.3), and the peak only ever grows.
    uint32_t current_rtt_ms(HSteamNetConnection c) const {
        SteamNetConnectionRealTimeStatus_t status{};
        interface_->GetConnectionRealTimeStatus(c, &status, 0, nullptr);
        return static_cast<uint32_t>(std::max(status.m_nPing, 0));
    }

    // Once a second, every physics member's current round trip to the
    // runner (set_player_rtt), with lead_adjust on only: off, the runner
    // gets no extra commands and wakes exactly when it did before.
    void post_player_rtts() {
        if (!runner_ || !config_.physics_lead_adjust) return;
        const auto now = std::chrono::steady_clock::now();
        if (now < next_rtt_post_) return;
        next_rtt_post_ = now + std::chrono::seconds(1);
        for (auto& [id, s]: physics_sessions_)
            for (const auto& [m, lead]: s.leads)
                runner_->set_player_rtt(id, m, current_rtt_ms(m));
    }

    void retire_lead_repeats(physics_session_state& s, HSteamNetConnection c) {
        s.lead_repeats.erase(std::remove_if(s.lead_repeats.begin(), s.lead_repeats.end(),
                                            [c](const physics_session_state::lead_repeat& r) { return r.to == c; }),
                             s.lead_repeats.end());
    }

    // Simulation thread: a lead decision for a member that declared CAP_LEAD
    // (late-player design 2.2).  Reliable, then three unreliable copies 40 ms
    // apart from the main loop (the SessionAssign pattern), retired early by
    // the ack.  A decision taken under an assignment the member has since
    // left (a resync crossed it on its way here) is not sent: the client
    // would drop it by its epoch anyway, and nothing would ever ack it.
    void on_session_lead(const bmmo::sim::lead_notice& notice) {
        std::lock_guard lk(state_mutex_);
        auto it = physics_sessions_.find(notice.session);
        if (it == physics_sessions_.end()) return;
        auto& s = it->second;
        const auto c = static_cast<HSteamNetConnection>(notice.player);
        if (std::find(s.members.begin(), s.members.end(), c) == s.members.end()) return;
        const auto n = s.notices.find(c);
        auto lead = s.leads.find(c);
        if (lead == s.leads.end() || !(lead->second.caps & bmmo::session::CAP_LEAD)) return;
        if (n == s.notices.end() || !n->second.assigned || n->second.epoch != notice.epoch) {
            Printf("Physics session %u: SessionLead %u for %s decided under base %u, now %u: not sent.",
                   s.id, notice.id, get_client_name(c), notice.epoch,
                   n == s.notices.end() ? 0u : n->second.epoch);
            return;
        }
        bmmo::session_lead_msg msg;
        msg.session = s.id;
        msg.player = notice.player;
        msg.seq = notice.id;
        msg.delta = static_cast<int16_t>(std::clamp(notice.k, -32768, 32767));
        msg.held_from = notice.held_from;
        msg.effective = notice.effective;
        msg.held = notice.held;
        msg.held_source = notice.held_source;
        msg.epoch = notice.epoch;
        msg.serialize();
        send(c, msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        retire_lead_repeats(s, c);
        s.lead_repeats.push_back(physics_session_state::lead_repeat{
            c, notice.id, msg.raw.str(), kAssignRepeats, std::chrono::steady_clock::now() + kAssignRepeatSpacing});
        lead->second.outstanding = notice.id;
        lead->second.outstanding_k = notice.k;
        Printf("Physics session %u: SessionLead %u to %s: %+d ticks, held [%u, %u), base %u.", s.id, notice.id,
               get_client_name(c), notice.k, notice.held_from, notice.effective, notice.epoch);
    }

    // The client applied `applied` ticks of SessionLead `seq`.  Only the
    // outstanding one counts: lead_offset moves by what was applied, bounded
    // by the delta sent (lateness_bound_applied: between 0 and k, never the
    // other way - the ack is the client's word, and the offset is what a
    // resync anchors at) and never below 0, as the meter's own offset; the
    // runner's meter takes the ack for its cooldown and the restore
    // judgement, bounded the same way.
    void handle_session_lead_ack(HSteamNetConnection c, const bmmo::session_lead_ack_msg& msg) {
        auto cs = client_session_.find(c);
        if (cs == client_session_.end() || cs->second != msg.session || !runner_) return;
        auto it = physics_sessions_.find(msg.session);
        if (it == physics_sessions_.end()) return;
        auto& s = it->second;
        auto lead = s.leads.find(c);
        if (lead == s.leads.end() || lead->second.outstanding == 0 || lead->second.outstanding != msg.seq) return;
        const int applied = bmmo::session::lateness_bound_applied(msg.applied, lead->second.outstanding_k);
        lead->second.outstanding = 0;
        lead->second.offset = std::max(0, lead->second.offset + applied);
        retire_lead_repeats(s, c);
        runner_->acknowledge_lead(s.id, c, msg.seq, applied);
        Printf("Physics session %u: %s applied %+d ticks of SessionLead %u (%+d sent, %+d acked); lead offset %d, L_eff %u.",
               s.id, get_client_name(c), applied, msg.seq, lead->second.outstanding_k, static_cast<int>(msg.applied),
               lead->second.offset,
               bmmo::sim::effective_lead_ticks(lead->second.assigned_lead, lead->second.offset));
    }

    // (Protocol 14-16's SessionGhostPose relay, step BALA, is retired at 17
    // (step BALL): an airship ghost is shown at its owner's legacy ball
    // stream, which tick() relays room-scoped like every other legacy
    // position; the server's world never read the pose.)

    // A SessionAssign is what starts a member's frames: one that arrives late
    // starts them that late for good (every frame stale at the server, every
    // relayed frame in the member's future).  Measured at 10 % loss: the
    // reliable copy's retransmit came ~400 ms after the others' assignments
    // and left two members of four 26 ticks behind for the whole session.  So
    // the reliable copy is backed by unreliable ones 40 ms apart from the
    // main loop (send_assign_repeats): copies sent in the same instant share
    // the reliable one's packet and its loss, and the session's first tick -
    // start_base periods after the barrier - comes too late to carry them.
    // The clients apply the first to arrive and ignore the rest.
    void send_session_assign(physics_session_state& s, HSteamNetConnection c, uint32_t first_tick, uint8_t flags = 0) {
        bmmo::session_assign_msg assign;
        assign.session = s.id;
        assign.first_tick = first_tick;
        assign.flags = flags;
        assign.serialize();
        send(c, assign.raw.str().data(), assign.size(), k_nSteamNetworkingSend_Reliable);
        // A newer assignment retires the copies of the previous one still due.
        s.assign_repeats.erase(std::remove_if(s.assign_repeats.begin(), s.assign_repeats.end(),
                                              [c](const physics_session_state::assign_repeat& r) { return r.to == c; }),
                               s.assign_repeats.end());
        s.assign_repeats.push_back(physics_session_state::assign_repeat{
            c, first_tick, kAssignRepeats, std::chrono::steady_clock::now() + kAssignRepeatSpacing, flags});
        s.assigned.insert(c);
        // A SessionLead of the previous base is void (the client drops it by
        // its epoch), so its copies stop and nothing waits for its ack.
        retire_lead_repeats(s, c);
        if (auto lead = s.leads.find(c); lead != s.leads.end()) lead->second.outstanding = 0;
        // Protocol 7: the member's numbering starts at the base, and so do its
        // held notices.  Every datagram names the epoch, so a client still on
        // the previous one takes nothing but the ack from them.
        auto& n = s.notices[c];
        close_held_run(s, c, n);
        n.assigned = true;
        n.epoch = first_tick;
        n.held_total = 0;
        n.copies = 0;
        n.acked = first_tick - 1;
    }

    // The session's start base for every member present at the start, sent
    // together once the last of them is ready (design 9.2).  One number for all
    // of them, not per-member: they are renumbered onto it at the same moment,
    // so their numberings stay in step with each other and only the assignment
    // round trip separates their clocks.  It is the lead a late joiner gets
    // measured from tick 0 - the world has not ticked yet, so there is no
    // "current +" term - which keeps a member that anchored early from being
    // numbered from its own anchor for the rest of the session, running ahead
    // of the server by however long the slowest level load took: the relay lag
    // (design 9.1) and the rollback lag both grew by that amount, unbounded.
    void assign_start_members(physics_session_state& s) {
        for (const auto m: s.members)
            if (!s.late.count(m) && !s.ready.count(m)) return;
        for (const auto m: s.members)
            if (!s.late.count(m) && !s.assigned.count(m)) send_session_assign(s, m, s.start_tick_base);
    }

    // Start barrier (design 8.3): SessionStart went out, but a member that
    // never answers with SessionReady would pin the session at its boot for
    // ever - assign_start_members waits for every member, and
    // session_runner::player_ready only starts ticking when all of them are
    // ready.  Once the deadline has passed the silent members are dropped and
    // the rest start without them; a session that loses everybody ends.
    // The unreliable copies of recent assignments (send_session_assign) and of
    // recent lifecycle events (handle_session_event), each from its own
    // main-loop iteration so each rides its own packet - and from the main
    // loop rather than a tick, so a session with one member left (the
    // leaver's Unphysicalize) still gets them.
    void send_assign_repeats() {
        const auto now = std::chrono::steady_clock::now();
        for (auto& [id, s]: physics_sessions_) {
            // --hold-member-ms (step M1, test builds): a held SessionMember
            // leaves once its time is up, then repeats like any event.
            for (auto h = s.held_members.begin(); h != s.held_members.end();) {
                if (now < h->due) { ++h; continue; }
                for (const auto m: h->to)
                    send(static_cast<HSteamNetConnection>(m), h->bytes.data(), h->bytes.size(),
                         k_nSteamNetworkingSend_ReliableNoNagle);
                if (!h->to.empty()) s.event_repeats.push(std::move(h->bytes), std::move(h->to));
                h = s.held_members.erase(h);
            }
            s.event_repeats.send_due([this, &s](uint32_t to, const std::string& bytes) {
                if (std::find(s.members.begin(), s.members.end(), static_cast<HSteamNetConnection>(to)) != s.members.end())
                    send(to, bytes.data(), bytes.size(), k_nSteamNetworkingSend_UnreliableNoDelay);
            });
            for (auto r = s.assign_repeats.begin(); r != s.assign_repeats.end();) {
                if (now < r->next_due) { ++r; continue; }
                bmmo::session_assign_msg assign;
                assign.session = s.id;
                assign.first_tick = r->first_tick;
                assign.flags = r->flags;
                assign.serialize();
                send(r->to, assign.raw.str().data(), assign.size(), k_nSteamNetworkingSend_UnreliableNoDelay);
                r->next_due = now + kAssignRepeatSpacing;
                if (--r->remaining <= 0) r = s.assign_repeats.erase(r);
                else ++r;
            }
            for (auto r = s.lead_repeats.begin(); r != s.lead_repeats.end();) {
                if (now < r->next_due) { ++r; continue; }
                send(r->to, r->bytes.data(), r->bytes.size(), k_nSteamNetworkingSend_UnreliableNoDelay);
                r->next_due = now + kAssignRepeatSpacing;
                if (--r->remaining <= 0) r = s.lead_repeats.erase(r);
                else ++r;
            }
        }
    }

    void check_start_barriers() {
        if (!runner_) return;
        const auto now = std::chrono::steady_clock::now();
        std::vector<uint32_t> all_silent;
        std::vector<std::pair<uint32_t, HSteamNetConnection>> dropped;   // session, member
        for (auto& [id, s]: physics_sessions_) {
            if (!s.start_barrier_armed || !s.world_ready) continue;
            if (runner_->running(id)) { s.start_barrier_armed = false; continue; }
            if (now < s.start_deadline) continue;
            s.start_barrier_armed = false;
            size_t silent = 0;
            for (const auto m: s.members)
                if (!s.ready.count(m) && !s.late.count(m)) ++silent;
            if (silent == 0) continue;   // everybody answered; the barrier did its job
            Printf("Physics session %u: %zu of %zu members did not report ready within %lld s.",
                   id, silent, s.members.size(),
                   static_cast<long long>(bmmo::sim::kStartBarrierTimeout.count()));
            if (silent == s.members.size()) { all_silent.push_back(id); continue; }
            for (const auto m: s.members)
                if (!s.ready.count(m) && !s.late.count(m)) dropped.emplace_back(id, m);
        }
        // Outside the loop: ending a session or dropping a member erases from
        // physics_sessions_, which would invalidate the iteration above.
        for (const auto id: all_silent) end_physics_session(id, "nobody reported ready");
        for (const auto& [session, m]: dropped) {
            send_session_end(session, m, "did not report ready");
            // Out of the session but not out of the room: the rest start
            // without it (assign_start_members re-checks as it goes), and it
            // can rejoin the room to late-join what is now a running session.
            // Leaving it in the room matters - the client is told the session
            // ended and has no way to learn it was removed from the room.
            physics_session_member_left(m);
        }
    }

    // The engine half of a build id ("ballanced-<rev>" of
    // "ballanced-<rev>+bmmo-<rev>"), empty when it was never resolved.
    static std::string engine_revision(const std::string& build_id) {
        const auto plus = build_id.find('+');
        const std::string engine = (plus == std::string::npos) ? build_id : build_id.substr(0, plus);
        return engine.find("unknown") == std::string::npos ? engine : std::string{};
    }

    void handle_session_ready(client_data_collection::iterator client_it, const bmmo::session_ready_msg& msg) {
        const HSteamNetConnection c = client_it->first;
        auto cs = client_session_.find(c);
        if (cs == client_session_.end() || cs->second != msg.session || !runner_) return;
        auto& s = physics_sessions_[msg.session];
        // Same protocol, or the two builds do not compute and compare the same
        // things (entity/session.hpp).  Before everything else, so a mixed
        // pair is told exactly that instead of "world mismatch".
        if (msg.protocol != bmmo::session::PHYSICS_PROTOCOL_VERSION) {
            end_physics_session(msg.session, Sprintf("%s runs physics protocol %u, this server runs %u: "
                    "the client's BallanceMMO build is from a different protocol",
                    client_it->second.name, msg.protocol, bmmo::session::PHYSICS_PROTOCOL_VERSION));
            return;
        }
        // Same engine, or the two sides do not simulate alike: the session
        // would spend itself hard-correcting a client that never agrees.  Only
        // the engine half of the build id is compared - the repository half
        // moves on commits that cannot change the physics - and an id neither
        // side could resolve (a build from an export with no git) is not held
        // against anyone.
        {
            const std::string ours = engine_revision(bmmo::physics::build_id());
            const std::string theirs = engine_revision(msg.build_id);
            if (theirs.empty() || ours.empty()) {
                Printf("Physics session %u: %s reports build \"%s\" against this server's \"%s\"; "
                       "not comparable, letting it in.", msg.session, client_it->second.name,
                       msg.build_id.c_str(), bmmo::physics::build_id());
            } else if (theirs != ours) {
                end_physics_session(msg.session, Sprintf("%s runs engine %s, this server runs %s",
                        client_it->second.name, theirs.c_str(), ours.c_str()));
                return;
            }
        }
        // Who the session admits (entity/session.hpp session_ready_refusal):
        // out of the session, not out of the room - as the start barrier drops
        // a silent member - and the rest go on.
        const auto refusal = bmmo::session::session_ready_refusal(msg.caps, s.world_options);
        const auto refuse = [&](const char* what, const char* reason = nullptr) {
            Printf("Physics session %u: %s refused: %s (caps %08x, world options %08x).", s.id,
                   client_it->second.name, what, msg.caps, s.world_options);
            const uint32_t session = s.id;
            send_session_end(session, c, reason ? reason : bmmo::session::ready_refusal_reason(refusal));
            physics_session_member_left(c);
        };
        // Step B9DX (the user, 2026-10-28: refuse it outright, no more
        // compatibility): a client without the session core (no CAP_PLACEMENT:
        // the legacy retail path, session core off) is refused whatever the
        // world options - before 2026-10-28 only a session with placement
        // clones or trafo inputs refused it.
        if (refusal == bmmo::session::ready_refusal::no_session_core) {
            refuse("no session core (no CAP_PLACEMENT)");
            return;
        }
        // Protocol 12 (sector placement design 2.4, 5; step S5): the same
        // placement, or the two worlds do not hold the same objects.  For
        // late joiners too - unlike the anchor hashes below, the signature
        // does not depend on time.  Every member admitted here declares
        // CAP_PLACEMENT and sends its signature.
        if (s.world_ready && msg.anchor_placement != s.anchor_placement) {
            end_physics_session(msg.session, Sprintf("placement mismatch for %s (client %016llx, server %016llx)",
                    client_it->second.name, static_cast<unsigned long long>(msg.anchor_placement),
                    static_cast<unsigned long long>(s.anchor_placement)));
            return;
        }
        // Protocol 16 (world-agreement-gaps-design.md 2.3, 5; step G3): under
        // the proximity union every world rewired the same mechanism blocks,
        // or their sensors do not agree.  Late joiners too (the signature
        // does not depend on time).  A member whose host cannot place sends
        // 0.  Either way it "cannot join" (design 2.3): refused - out of the
        // session, not out of the room, and the rest go on (WG3's review: a
        // retail core member before G4 would otherwise end every headless
        // member's session).
        if (s.world_ready && (s.world_options & bmmo::session::WORLD_PROXIMITY_UNION)
                && msg.anchor_probes != s.anchor_probes) {
            const std::string what = Sprintf("probe mismatch (client %016llx, server %016llx)",
                    static_cast<unsigned long long>(msg.anchor_probes), static_cast<unsigned long long>(s.anchor_probes));
            refuse(what.c_str(), msg.anchor_probes
                    ? "this session's mechanism sensors (the proximity union) differ from this client's world"
                    : "this session runs the proximity union (world option) and this client's host cannot place "
                      "its probes");
            return;
        }
        // Protocol 11 (trafo-replication design 2.14): a session whose
        // world option makes the trafo an input refuses a member that cannot
        // run it (a core host without the trafo inputs).
        if (refusal == bmmo::session::ready_refusal::no_trafo_inputs) {
            refuse("no CAP_TRAFO_INPUTS in a session with trafo inputs");
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
        // Late joiners are numbered on the session's fixed deadline schedule
        // (step RL1, session_runner::reanchor_tick: the tick the schedule
        // names now plus the joiner's lead, floored at the runner's current
        // tick + 2); members present at the start from the session's start
        // base (protocol 2.2, session_assign_msg).  Both give the client
        // input_delay plus a round-trip allowance of deadline budget, so its
        // frames arrive before the deadline for the tick they name and every
        // snapshot refers to a tick it has already recorded.
        // The lead of this first assignment, and what the client can apply
        // (late-player design 2.2, 2.5): the start base for a start member,
        // the late join's lead for a joiner.  A resync keeps both and moves
        // only the base.  A repeated SessionReady keeps the first.  Off the
        // per-player path the joiner's lead is late_tick_lead_ticks, which
        // is not clamped to 44 - input_delay like the start base: up to
        // about 4 ticks more budget than the start members (D 17 at a 180 ms
        // peak: 31 against 27), as before RL1 (late-player design 2.8, "As
        // built (step RL1b)").
        const uint32_t join_lead = s.late.count(c) ? late_join_lead(s, c, msg.caps) : 0;
        const bool join_state = (msg.caps & bmmo::session::CAP_RESYNC_STATE) != 0;
        uint32_t assigned = s.late.count(c) ? runner_->reanchor_tick(s.id, join_lead, join_state) : s.start_tick_base;
        // Step M1 (J4, J-h): not before the removal of any life the server
        // still holds - the joiner was never told of a life that left before
        // it joined, and from its base on its world never needs one.
        // The lead the joiner is judged by grows with the base (review of
        // step M1): otherwise lead_adjust reads it as early by the delta.
        uint32_t join_clamp_delta = 0;
        if (s.late.count(c)) {
            const uint32_t horizon = runner_->leaving_horizon(s.id);
            const uint32_t clamped = bmmo::session::late_join_base(assigned, horizon);
            if (clamped != assigned)
                Printf("Physics session %u: %s's base %u moved to %u, the removal of a leaving life (J4).", s.id,
                       client_it->second.name, assigned, clamped);
            join_clamp_delta = clamped - assigned;
            assigned = clamped;
        }
        if (!s.leads.count(c)) {
            auto& lead = s.leads[c];
            lead.caps = msg.caps;
            lead.assigned_lead = s.late.count(c) ? join_lead + join_clamp_delta : s.start_tick_base;
            // Flag off, the runner gets no command it did not get before.
            if (config_.physics_lead_adjust) {
                runner_->set_player_lead(msg.session, c, msg.caps, lead.assigned_lead);
                runner_->set_player_rtt(msg.session, c, current_rtt_ms(c));
            }
        }
        // A late joiner that applies the resync state (CAP_RESYNC_STATE, the
        // session core; resync design 2.9, step A5) takes its first
        // assignment as a resync's: the flag on the assignment and on every
        // repeat, and the server's state of base - 1 to it alone after the
        // runner steps that tick.  One without it (the legacy retail path)
        // gets the full snapshot broadcast, as before.
        const bool late = s.late.count(c) != 0;
        const bool state = late && join_state;
        runner_->player_ready(msg.session, c, assigned);
        if (late) send_session_assign(s, c, assigned, state ? bmmo::session::ASSIGN_FLAG_STATE_FOLLOWS : 0);
        else assign_start_members(s);
        Printf("Physics session %u: %s ready (assigned tick %u; physics %s, %s; caps %08x).", s.id, client_it->second.name,
                assigned, msg.physics_sha256.substr(0, 12), msg.build_id, msg.caps);
        if (late) {
            // catch the late joiner up: everyone's current ball, every sector
            // the union has grown by, then the state of base - 1 (or a full
            // snapshot)
            for (const auto& [member, last]: s.last_physicalize)
                if (member != c) send(c, last.bytes.data(), last.bytes.size(), k_nSteamNetworkingSend_Reliable);
            for (const auto& bytes: s.sector_history) send(c, bytes.data(), bytes.size(), k_nSteamNetworkingSend_Reliable);
            resend_trafo_relayed(s, c, assigned);
            if (state) {
                runner_->request_resync_state(s.id, c, assigned);
                Printf("Physics session %u: the state of tick %u follows to the late joiner %s alone (protocol 10).", s.id,
                       assigned - 1, client_it->second.name);
            } else {
                runner_->request_full_snapshot(s.id);
            }
        }
    }

    // Resync (design 9.2): the client's tick numbering broke (pause, long
    // stall, repeated hard corrections).  Same path as a late join: a new
    // base counted on the session's fixed deadline schedule (step RL1,
    // session_runner::reanchor_tick; floored at the runner's current tick +
    // 2), everyone's ball, the sector history - and, since
    // protocol 10 (resync design 2.3, step A3), the server's state of tick
    // base - 1 to that member alone (SessionResyncState), announced by the
    // assignment's ASSIGN_FLAG_STATE_FOLLOWS, instead of a full snapshot
    // broadcast to every member.
    void handle_session_resync(HSteamNetConnection c, const bmmo::session_resync_msg& msg) {
        auto cs = client_session_.find(c);
        if (cs == client_session_.end() || cs->second != msg.session || !runner_) return;
        auto it = physics_sessions_.find(msg.session);
        if (it == physics_sessions_.end()) return;
        auto& s = it->second;
        if (!runner_->running(s.id)) return;
        resync_member(s, c, msg.last_full_tick);
    }

    // The resync answer (handle_session_resync, and again for a base the
    // runner stepped before the request reached it): returns the base.
    uint32_t resync_member(physics_session_state& s, HSteamNetConnection c, uint32_t last_full_tick) {
        // On the per-player path (lead_adjust on, the member declared
        // CAP_LEAD), at L_eff (late-player design 2.5, 2.8): the lead of the
        // member's first assignment plus its acknowledged offset, the lead
        // measured to work, instead of the capped late-tick lead from the
        // peak round trip.  The offset is kept; the runner measures the
        // reference anew.  Every other member at the start base's lead,
        // clamped inside the window (plan step 20, resync_lead_ticks).  The
        // ticks between the server's tick and that base are not waited for
        // (input_buffer::reanchored): the stall that used to cost every
        // member 22 ticks of relay lag here is gone.
        // A member that applies the resync state (CAP_RESYNC_STATE, the
        // session core) gets it; one that does not (the legacy retail path)
        // the pre-protocol-10 answer: no flag, a full snapshot to everyone,
        // and a base floored 3 ticks over the runner instead of 2 (it has no
        // base-missed re-answer; step RL1b).
        const auto lead = s.leads.find(c);
        const bool state = lead != s.leads.end() && (lead->second.caps & bmmo::session::CAP_RESYNC_STATE) != 0;
        uint32_t assigned = resync_tick_base(s, c, state);
        if (lead != s.leads.end() && bmmo::sim::uses_player_lead(config_.physics_lead_adjust, lead->second.caps))
            assigned = runner_->reanchor_tick(s.id,
                    bmmo::sim::effective_lead_ticks(lead->second.assigned_lead, lead->second.offset), state);
        // The newest tick whose held frames were counted: the old epoch's
        // held_total stops here, and the ticks after it up to the base are
        // applied held under no numbering the client has (its notices never
        // carry them).  Logged for the harness's per-epoch count.
        const uint32_t counted_through = s.relay.newest_tick();
        s.late.insert(c);
        s.ready.insert(c);
        runner_->player_ready(s.id, c, assigned);
        send_session_assign(s, c, assigned, state ? bmmo::session::ASSIGN_FLAG_STATE_FOLLOWS : 0);
        for (const auto& [member, last]: s.last_physicalize)
            if (member != c) send(c, last.bytes.data(), last.bytes.size(), k_nSteamNetworkingSend_Reliable);
        for (const auto& bytes: s.sector_history) send(c, bytes.data(), bytes.size(), k_nSteamNetworkingSend_Reliable);
        resend_trafo_relayed(s, c, assigned);
        // The state of base - 1, after the runner steps it (posted after
        // player_ready, on the same queue).
        if (state) runner_->request_resync_state(s.id, c, assigned);
        else runner_->request_full_snapshot(s.id);
        Printf("Physics session %u: %s resynced at tick %u (last full snapshot it had: %u; held counted through tick %u).",
               s.id, get_client_name(c), assigned, last_full_tick, counted_through);
        if (state)
            Printf("Physics session %u: the state of tick %u follows to %s alone (protocol 10).", s.id, assigned - 1,
                   get_client_name(c));
        return assigned;
    }

    // `received` is GameNetworkingSockets' own receive timestamp, taken on its
    // service thread when the datagram came in.  The main loop only polls
    // every 15 ms, so the time it handles a message at is up to a whole tick
    // later than its arrival, and the lateness meter (late-player design 2.1)
    // judges arrivals to within a tick.  The age is carried over to
    // steady_clock (both clocks are monotonic).  A main-loop stall is exactly
    // when the age matters - every member's queued datagrams would otherwise
    // read late together - so a long age is kept, clamped to 10 s; only a
    // missing or negative timestamp degrades to "now".
    void handle_session_input(HSteamNetConnection c, const bmmo::session_input_msg& msg,
                              SteamNetworkingMicroseconds received) {
        auto cs = client_session_.find(c);
        if (cs == client_session_.end() || cs->second != msg.session || !runner_) return;
        auto arrival = std::chrono::steady_clock::now();
        constexpr SteamNetworkingMicroseconds kMaxAge = 10000000;
        const SteamNetworkingMicroseconds age = SteamNetworkingUtils()->GetLocalTimestamp() - received;
        if (received > 0 && age > 0)
            arrival -= std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::microseconds(age < kMaxAge ? age : kMaxAge));
        runner_->submit_input(msg.session, c, msg.first_tick, msg.frames, arrival);
    }

    static void copy_name(char* out, size_t size, const std::string& text) {
        std::snprintf(out, size, "%s", text.c_str());
    }

    void handle_session_event(client_data_collection::iterator client_it, bmmo::session_event_msg& msg) {
        const HSteamNetConnection c = client_it->first;
        auto cs = client_session_.find(c);
        if (cs == client_session_.end() || cs->second != msg.session || !runner_) return;
        auto& s = physics_sessions_[msg.session];
        // On the uplink the stamp is the origin and nothing is re-stamped yet
        // (protocol 9): whatever the client put in the two fields, the server
        // goes by `tick`.
        msg.origin_tick = msg.tick;
        msg.state = 0;
        // The member sends every event several times (protocol 6): the first
        // arrival is the event, the rest are nothing - not a rate-limit hit,
        // not a second submit, not a second relay.
        if (!s.events_seen.first(bmmo::session::lifecycle_key_of(msg, c))) {
            ++s.duplicate_events;
            return;
        }
        // Step G5 (world-agreement-gaps-design.md 2.6): under the proximity
        // union every world wakes its own mechanisms, so a BodyRevived is no
        // input: neither submitted nor relayed, counted, and the first of
        // each member noted in the log and the journal.
        if (msg.type == bmmo::session::event_type::BodyRevived
                && (s.world_options & bmmo::session::WORLD_PROXIMITY_UNION)) {
            auto& guard = s.guards[c];
            if (guard.revived_ignored++ == 0) {
                const std::string text = Sprintf("BodyRevived from %s ignored (world option proximity union): %s at %u",
                                                 client_it->second.name, msg.name.c_str(), msg.tick);
                Printf("Physics session %u: %s.", s.id, text.c_str());
                runner_->journal_note(s.id, text);
            }
            return;
        }
        bmmo::sim::lifecycle_event event;
        event.type = msg.type;
        event.ball_type = msg.ball_type;
        event.flags = msg.flags;
        for (int k = 0; k < 3; ++k) event.position[k] = msg.position[k];
        for (int k = 0; k < 9; ++k) event.rotation[k] = msg.rotation[k];
        event.sector = msg.sector;
        event.name = msg.name;
        {
            // Design 9.4.  Rate limit: hard reject.  Everything else: log + count.
            auto& guard = s.guards[c];
            const uint32_t rate_limit = config_.physics_event_rate_limit;   // 0 = no limit
            const auto now = std::chrono::steady_clock::now();
            if (now - guard.window_start > std::chrono::seconds(1)) {
                guard.window_start = now;
                guard.events_in_window = 0;
            }
            ++guard.events_in_window;
            if (rate_limit != 0 && guard.events_in_window > rate_limit) {
                if (guard.events_in_window == rate_limit + 1) {
                    ++s.rejected_events;
                    Printf("Physics session %u: %s sends more than %u events per second; dropping the excess.",
                           s.id, client_it->second.name, rate_limit);
                }
                // A Trafo event never applied must reach its owner as
                // REJECTED, which undoes its local effect (decision 9.3).
                if (msg.type == bmmo::session::event_type::Trafo)
                    reject_trafo(s, client_it, msg, "more events per second than the rate limit",
                                 bmmo::session::trafo_validation{});
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
                // A dropped Physicalize costs the player its body for the rest
                // of the session (the retail scripts report a ball's life
                // once), so only a recipe the world cannot use at all is
                // rejected: an unknown ball type, or a number the physics
                // engine would choke on.  The rest is compared with the
                // level's Physicalize_GameBall row - what the retail scripts
                // hand the Physicalize block - and only flagged: the session
                // rests on both sides running the very call the client
                // reported, so the recipe is never rewritten here.
                // Fixed bounds got this wrong (Ball_Paper's Linear Damp is
                // 1.5, above the old 1.0 cap), and every trafo into a paper
                // ball was rejected.
                const size_t ball_types = s.ball_rows.empty() ? 3u : s.ball_rows.size();
                const bool finite = std::isfinite(r.mass) && std::isfinite(r.friction) && std::isfinite(r.elasticity)
                    && std::isfinite(r.linear_damp) && std::isfinite(r.rot_damp);
                if (msg.ball_type >= ball_types || !finite || r.mass <= 0.0f) {
                    ++s.rejected_events;
                    Printf("Physics session %u: rejected Physicalize from %s (ball type %u, malformed recipe).", s.id,
                           client_it->second.name, msg.ball_type);
                    return;
                }
                if (msg.ball_type < s.ball_rows.size()) {
                    const auto& row = s.ball_rows[msg.ball_type];
                    auto expect = [&](const char* what, float value, float expected) {
                        if (std::fabs(value - expected) <= 1e-3f * std::max(1.0f, std::fabs(expected))) return;
                        flag(std::string("Physicalize ") + what + " " + std::to_string(value) + " instead of "
                             + std::to_string(expected) + " (Physicalize_GameBall row " + std::to_string(msg.ball_type) + ")");
                    };
                    expect("friction", r.friction, row.friction);
                    expect("elasticity", r.elasticity, row.elasticity);
                    expect("mass", r.mass, row.mass);
                    expect("linear damping", r.linear_damp, row.linear_damp);
                    expect("rotation damping", r.rot_damp, row.rot_damp);
                }
                auto distance = [&](const double* a, const float* b) {
                    double d = 0.0;
                    for (int k = 0; k < 3; ++k) d += (a[k] - b[k]) * (a[k] - b[k]);
                    return std::sqrt(d);
                };
                const double pos[3] = {msg.position[0], msg.position[1], msg.position[2]};
                // A respawn is at the resetpoint of the last checkpoint the
                // player's own ball took, which can be any of the level's -
                // or, with a spawn spacing, in the player's birth cell around
                // it (session/spawn_impulse.hpp: the farthest of the 16 cells
                // is sqrt(5) spacings out).
                const double reach = 2.5 + (s.spawn_spacing > 0.0f ? 2.3 * static_cast<double>(s.spawn_spacing) : 0.0);
                bool near_spawn = distance(pos, s.spawn_position) <= reach;
                for (const VxVector& reset: s.resetpoints) {
                    const float at[3] = {reset.x, reset.y, reset.z};
                    near_spawn = near_spawn || distance(pos, at) <= reach;
                }
                bool near_last = false;
                if (guard.have_position) {
                    const float last[3] = {static_cast<float>(guard.last_position[0]),
                                           static_cast<float>(guard.last_position[1]),
                                           static_cast<float>(guard.last_position[2])};
                    near_last = distance(pos, last) <= 5.0;
                }
                if (!near_spawn && !near_last)
                    flag("Physicalize pose far from every resetpoint of the level and from the player's last known position");
            } else if (msg.type == bmmo::session::event_type::Sector) {
                if (msg.sector < 1 || msg.sector > guard.sector + 1)
                    flag("sector " + std::to_string(msg.sector) + " after sector " + std::to_string(guard.sector));
                guard.sector = std::max(guard.sector, msg.sector);
            } else if (msg.type == bmmo::session::event_type::Trafo) {
                // Trafo-replication design 2.2, decision 9.3: static level
                // data only.  A rejected one is never applied: its owner gets
                // REJECTED and undoes its local effect, and the rejection is
                // logged as possible cheating or corruption.
                const bool on = (s.world_options & bmmo::session::WORLD_TRAFO_INPUTS) != 0;
                const auto v = bmmo::session::trafo_validate(msg.flags, msg.ball_type, msg.position, s.trafo_points);
                if (!on || !v.ok()) {
                    reject_trafo(s, client_it, msg, on ? bmmo::session::trafo_check_name(v.verdict)
                                                       : "the session does not run the trafo as an input", v);
                    return;
                }
                // A reset's position is read by no world; zeros on the
                // relay, as the server's own resets carry, so every journal
                // records the same bytes for it.
                if (bmmo::session::trafo_is_reset(msg.flags))
                    for (int k = 0; k < 3; ++k) msg.position[k] = event.position[k] = 0.0f;
            }
        }
        if (msg.type == bmmo::session::event_type::BodyRevived) {
            auto& guard = s.guards[c];
            // Diagnostics only: a late event is legal and must be applied (the
            // production Level 8 journal carries 126 events at offset -29..-31,
            // Level 2 five BodyRevived 3058 ticks late).  Lateness is never a
            // reason to drop anything.
            const uint32_t now_tick = runner_->current_tick(msg.session);
            if (static_cast<int32_t>(now_tick - msg.tick) > 0) ++guard.late_events;
            if (bmmo::session::repeat_event_duplicate(guard.recent_revives, msg.ball_type, msg.name, msg.tick)) {
                ++guard.dropped_duplicate;
                return;   // no submit_event, no relay: members must not replay it either
            }
            bmmo::session::record_repeat_event(guard.recent_revives, msg.ball_type, msg.name, msg.tick);
        }
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
        // Relayed from the runner's acceptance (late-player design 3.1,
        // on_event_accepted), where the tick it is applied at is fixed on the
        // simulation thread: the message waits here until then.
        const uint64_t token = s.next_event_token++;
        const uint32_t stamp = msg.tick;
        auto& waiting = s.awaiting_events[token];
        waiting.from = c;
        waiting.msg = std::move(msg);
        runner_->submit_event(waiting.msg.session, c, stamp, std::move(event), token);
    }

    // A Trafo event refused at intake (trafo-replication design 2.2,
    // decision 9.3): REJECTED to its owner alone (reliable, then the
    // unreliable repeats), never submitted or relayed; logged as possible
    // cheating or corruption with what matters - the player, the tick, the
    // position, the nearest trafo and its distance, the ball types, the
    // session.
    void reject_trafo(physics_session_state& s, client_data_collection::iterator client_it, bmmo::session_event_msg& msg,
                      const char* why, const bmmo::session::trafo_validation& v) {
        const HSteamNetConnection c = client_it->first;
        ++s.rejected_events;
        ++s.rejected_trafos;
        // The player's ball type as the server knows it: its last relayed
        // Physicalize (the type the ball became; the event names the type
        // it was).
        std::string ball = "unknown";
        if (const auto last = s.last_physicalize.find(c); last != s.last_physicalize.end()) {
            bmmo::session_event_msg birth;
            birth.raw.str(last->second.bytes);
            if (birth.deserialize()) ball = std::to_string(static_cast<int>(birth.ball_type));
        }
        std::string nearest = "none";
        if (v.nearest >= 0 && static_cast<size_t>(v.nearest) < s.trafo_points.size())
            nearest = Sprintf("%s at %.3f m", s.trafo_points[static_cast<size_t>(v.nearest)].name.c_str(), v.distance);
        if (s.rejected_trafos <= 50)
            Printf("Physics session %u: REJECTED Trafo %s %s from %s (player %u) stamped for tick %u (server tick %u): %s; "
                   "position (%.3f, %.3f, %.3f), nearest trafo %s (reach %.2f m); piece type %u, the player's last ball "
                   "type %s - possible cheating or corruption.",
                   s.id, bmmo::session::trafo_is_reset(msg.flags) ? "reset" : "explosion",
                   bmmo::session::trafo_type_name(msg.ball_type), client_it->second.name, static_cast<unsigned>(c), msg.tick,
                   runner_->current_tick(s.id), why, static_cast<double>(msg.position[0]), static_cast<double>(msg.position[1]),
                   static_cast<double>(msg.position[2]), nearest.c_str(), static_cast<double>(bmmo::session::kTrafoReach),
                   static_cast<unsigned>(msg.ball_type), ball.c_str());
        msg.player = static_cast<uint32_t>(c);
        msg.origin_tick = msg.tick;
        msg.state = bmmo::session::EVENT_STATE_REJECTED;
        msg.clear();
        msg.serialize();
        const std::string bytes = msg.raw.str();
        send(c, bytes.data(), bytes.size(), k_nSteamNetworkingSend_ReliableNoNagle);
        s.event_repeats.push(bytes, {static_cast<uint32_t>(c)});
    }

    // Simulation thread: the runner accepted a lifecycle event and fixed the
    // tick it is applied at (late-player design 3.1).  Relayed from here to
    // EVERY member, the owner included - the owner's copy is its echo, a
    // plain ack when the event was on time - reliable without Nagle, then the
    // unreliable repeats two ticks apart (protocol 6).  Without Nagle, like
    // the sender did: a Sector event's relay has to reach the other members
    // before they simulate its tick, and the lead was sized for the trip, not
    // for a batching delay on top.  The late joiner's catch-up state
    // (last_physicalize, sector_history) is updated here too, on the origin.
    // A superseded birth goes to its owner only, flagged.
    //
    // The S5 line (plan step 8, the gate of step 17): how long the event took
    // from the intake (submit_event) to this send, which the posted command
    // can spend behind a catch-up pass of every session, and which eats into
    // the Sector lead.
    void on_event_accepted(const bmmo::sim::accepted_event& a) {
        std::lock_guard lk(state_mutex_);
        auto it = physics_sessions_.find(a.session);
        if (it == physics_sessions_.end()) return;
        auto& s = it->second;
        const auto owner = static_cast<HSteamNetConnection>(a.player);
        bmmo::session_event_msg msg;
        if (a.dropped) {   // the runner did not take it: only the parked message goes
            if (a.token != 0) s.awaiting_events.erase(a.token);
            return;
        }
        if (a.token != 0) {
            auto waiting = s.awaiting_events.find(a.token);
            if (waiting == s.awaiting_events.end()) return;   // its sender left meanwhile
            msg = std::move(waiting->second.msg);
            s.awaiting_events.erase(waiting);
        } else if (a.type == bmmo::session::event_type::Trafo) {
            // Produced by the runner (protocol 11): the server's own trafo
            // reset - before a re-explosion, at the deadline, a leaver's
            // final ones - with origin = applied.
            msg.session = a.session;
            msg.type = a.type;
            msg.ball_type = a.ball_type;
            msg.flags = a.flags;
            for (int k = 0; k < 3; ++k) msg.position[k] = a.position[k];
        } else {
            msg.session = a.session;   // produced by the runner: the leaver's Unphysicalize
            msg.type = a.type;
            // Every event of the leaver the runner was going to accept has
            // been (the commands run in order); one still waiting here was
            // dropped by it and never will be.
            for (auto e = s.awaiting_events.begin(); e != s.awaiting_events.end();)
                if (e->second.from == owner) e = s.awaiting_events.erase(e);
                else ++e;
        }
        msg.player = a.player;
        msg.tick = a.applied;
        msg.origin_tick = a.origin;
        msg.life = a.life;   // step M1: the life the runner accepted it for
        msg.state = static_cast<uint8_t>((a.applied != a.origin ? bmmo::session::EVENT_STATE_MOVED : 0)
                                         | (a.superseded ? bmmo::session::EVENT_STATE_SUPERSEDED : 0)
                                         | (a.rejected ? bmmo::session::EVENT_STATE_REJECTED : 0));
        msg.clear();
        msg.serialize();
        const std::string bytes = msg.raw.str();
        const auto is_member = [&s](HSteamNetConnection m) {
            return std::find(s.members.begin(), s.members.end(), m) != s.members.end();
        };
        std::vector<uint32_t> to;
        if (a.rejected) {
            ++s.rejected_events;
            if (is_member(owner)) to.push_back(owner);
        } else if (a.superseded) {
            if (a.type != bmmo::session::event_type::Trafo) ++s.superseded_births;
            if (is_member(owner)) to.push_back(owner);
        } else {
            // Re-stamped and applied (one definition with the runner's
            // describe: a superseded birth's applied tick is never used).
            if (a.applied != a.origin) ++s.restamped_events;
            if (msg.type == bmmo::session::event_type::Physicalize) {
                // Not when a death newer by origin was accepted first: the
                // birth is applied before it, and the death ends the ball.
                const auto death = s.last_unphysicalize.find(owner);
                const bool dead = death != s.last_unphysicalize.end()
                               && static_cast<int32_t>(death->second - a.origin) > 0;
                if (!dead) {
                    auto& last = s.last_physicalize[owner];
                    if (last.bytes.empty() || static_cast<int32_t>(a.origin - last.tick) >= 0) last = {a.origin, bytes};
                }
            } else if (msg.type == bmmo::session::event_type::Unphysicalize) {
                if (is_member(owner)) {   // not the leaver's own, after its entries went
                    auto [death, fresh] = s.last_unphysicalize.try_emplace(owner, a.origin);
                    if (!fresh && static_cast<int32_t>(a.origin - death->second) > 0) death->second = a.origin;
                }
                // Only a death newer by origin than the last birth ends it
                // (protocol 6: the copies of a beam's pair can arrive in
                // either order; design 3.1: re-stamped, both land at one tick).
                const auto last = s.last_physicalize.find(owner);
                if (last != s.last_physicalize.end() && static_cast<int32_t>(a.origin - last->second.tick) > 0)
                    s.last_physicalize.erase(last);
            } else if (msg.type == bmmo::session::event_type::Sector) {
                s.sector_history.push_back(bytes);
            } else if (msg.type == bmmo::session::event_type::Trafo) {
                // resend_trafo_relayed prunes by tick; a hard cap besides
                // (three events per trafo, 20 per second per member at most).
                if (s.trafo_relayed.size() >= 4096) s.trafo_relayed.erase(s.trafo_relayed.begin(), s.trafo_relayed.begin() + 2048);
                s.trafo_relayed.emplace_back(a.applied, bytes);
            }
            // BodyRevived is the server's own business (design 3.3: a wake of
            // a body the server's scripts also run), relayed to the others
            // and never echoed.
            for (const auto m: s.members)
                if (m != owner || msg.type != bmmo::session::event_type::BodyRevived) to.push_back(m);
        }
        for (const auto m: to)
            send(static_cast<HSteamNetConnection>(m), bytes.data(), bytes.size(), k_nSteamNetworkingSend_ReliableNoNagle);
        // ...and again, unreliably, on each of the next ticks: a lost
        // reliable copy is otherwise ~400 ms late by retransmit, past the
        // rollback's reach, and a mirror whose respawn or beam is applied that
        // late sits metres off until the next forced snapshot.
        const size_t recipients = to.size();
        s.event_repeats.push(bytes, std::move(to));
        const auto delay_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - a.intake).count();
        // Step F3 (LCM): the arrival margin goes last, after the flag word,
        // so the existing readers' pattern (arena_map_test.py RE_S5) still
        // matches the line as it was.
        char timing[64] = "";
        if (a.timed)
            std::snprintf(timing, sizeof(timing), " margin=%d slack=%.2f", static_cast<int>(a.margin), a.slack);
        Printf("Physics session %u: S5 event player=%u type=%s origin=%u applied=%u late=%d delay_us=%lld delay_ticks=%.2f "
               "to=%zu%s%s", s.id, a.player, session_event_type_name(msg.type), a.origin, a.applied,
               static_cast<int>(static_cast<int32_t>(a.applied - a.origin)), static_cast<long long>(delay_us),
               static_cast<double>(delay_us) / 15000.0, recipients,
               a.rejected ? " rejected" : a.superseded ? " superseded"
                   : (a.token == 0 ? (a.type == bmmo::session::event_type::Trafo ? " server" : " leaver") : ""),
               timing);
    }

    static const char* session_event_type_name(bmmo::session::event_type type) {
        switch (type) {
            case bmmo::session::event_type::Physicalize: return "Physicalize";
            case bmmo::session::event_type::Unphysicalize: return "Unphysicalize";
            case bmmo::session::event_type::Sector: return "Sector";
            case bmmo::session::event_type::Finish: return "Finish";
            case bmmo::session::event_type::BodyRevived: return "BodyRevived";
            case bmmo::session::event_type::Trafo: return "Trafo";
        }
        return "?";
    }

    void print_physics_sessions() {
        std::lock_guard lk(state_mutex_);
        if (!runner_) { Printf("Physics sessions are unavailable."); return; }
        if (physics_sessions_.empty()) Printf("No physics session is running.");
        for (const auto& [id, s]: physics_sessions_) {
            const std::string journal = runner_->journal_path(id);
            Printf("Session %u: room %u, level %d, %zu members, %zu ready, world %s, ticking %s, tick %u, "
                   "events flagged %llu / rejected %llu / re-stamped %llu / superseded %llu, journal %s.",
                    id, s.room, s.map.level, s.members.size(), s.ready.size(), s.world_ready ? "ready" : "booting",
                    s.ticking ? "yes" : "no", runner_->current_tick(id),
                    static_cast<unsigned long long>(s.flagged_events), static_cast<unsigned long long>(s.rejected_events),
                    static_cast<unsigned long long>(s.restamped_events), static_cast<unsigned long long>(s.superseded_births),
                    journal.empty() ? "off" : journal.c_str());
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
                send_room_event(c, event_type::RequestAccepted, error_code::None, rooms_.room_of(c), c);
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
                // The refreshed roster goes out first: it carries the new flag,
                // and the clients render the event from it.
                send_room_state_to_room(room);
                if (const auto* r = rooms_.find(room))
                    for (const auto& m: r->members)
                        send_room_event(m.id, event_type::ReadyChanged, error_code::None, room, c, c);
                send_room_event(c, event_type::RequestAccepted, error_code::None, room, c, c);
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
                send_room_event(c, event_type::RequestAccepted, error_code::None, room, c);
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
                handle_session_input(networking_msg->m_conn, msg, networking_msg->m_usecTimeReceived);
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
            case bmmo::SessionLeadAck: {
                auto msg = bmmo::message_utils::deserialize<bmmo::session_lead_ack_msg>(networking_msg);
                handle_session_lead_ack(networking_msg->m_conn, msg);
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
            for (auto& i: clients_) {
                SteamNetConnectionRealTimeStatus_t status{};
                interface_->GetConnectionRealTimeStatus(i.first, &status, 0, nullptr);
                const auto ping = (uint16_t) std::min(status.m_nPing, (int) std::numeric_limits<uint16_t>::max());
                i.second.ping_peak_ms = std::max(i.second.ping_peak_ms, ping);
                ping_msg.data.try_emplace(i.first, ping);
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
    // The runner's seed (runner_config::seed): every world's spawn impulse
    // and trafo seeds derive from it, so SessionStart carries this value.
    int physics_seed_ = 1;
    std::chrono::steady_clock::time_point next_rtt_post_{};   // post_player_rtts, once a second
    // Step SIO: the session journals' I/O pool.  Before the runner, so it is
    // destroyed after it: the runner's posted closes run (the pool joins)
    // once the runner is gone.
    std::unique_ptr<bmmo::session::io_pool> io_pool_;
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
    // --hold-member-ms (step M0, test builds): see set_test_hold_member_ms.
    uint32_t test_hold_member_ms_ = 0;

    std::unordered_map<std::string, std::string> map_names_;
    // Bounds on what clients may add to map_names_; the whole map is sent to
    // every joining client, so it must not be allowed to grow without limit.
    static constexpr size_t MAX_MAP_NAMES = 4096, MAX_MAP_NAME_LENGTH = 512;
};

// parse arguments (optional port and help/version/log) with getopt
// `hold_member_ms`: the test flag --hold-member-ms (BMMO_SERVER_TEST_FLAGS
// builds only; step M0).
static int parse_args(int argc, char** argv, uint16_t& port, std::string& log_path, bool& dry_run,
                      uint32_t& hold_member_ms) {
    enum option_values { DryRun = UINT8_MAX + 1, HoldMemberMs };
#if !defined(BMMO_SERVER_TEST_FLAGS)
    (void)hold_member_ms;  // the flag exists in test builds only
#endif
    static struct option long_options[] = {
        {"port", required_argument, 0, 'p'},
        {"log", required_argument, 0, 'l'},
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'v'},
        {"dry-run", no_argument, 0, DryRun},
#if defined(BMMO_SERVER_TEST_FLAGS)
        {"hold-member-ms", required_argument, 0, HoldMemberMs},
#endif
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
#if defined(BMMO_SERVER_TEST_FLAGS)
                puts("      --hold-member-ms=MS\t (test build) Send the SessionMember messages MS ms late.");
#endif
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
#if defined(BMMO_SERVER_TEST_FLAGS)
            case HoldMemberMs:
                hold_member_ms = static_cast<uint32_t>(std::strtoul(optarg, nullptr, 10));
                break;
#endif
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    uint16_t port = 26676;
    bool dry_run = false;
    std::string log_path;
    uint32_t hold_member_ms = 0;
    if (parse_args(argc, argv, port, log_path, dry_run, hold_member_ms) < 0)
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
    // Step SIO: a crash writes out what the journals' writer still holds
    // (best effort, bounded).
    bmmo::sim::set_crash_drain([] { bmmo::session::io::drain_all(std::chrono::milliseconds(1000)); });
#endif
    printf("Initializing sockets...\n");
    server::init_socket();

    printf("Starting server at port %u.\n", port);
    server server(port);
    if (hold_member_ms) {
        server.set_test_hold_member_ms(hold_member_ms);
        printf("Test flag: --hold-member-ms %u (the SessionMember messages leave %u ms late).\n", hold_member_ms,
               hold_member_ms);
    }

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
