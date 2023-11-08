#ifndef BALLANCEMMOSERVER_SERVER_DATA_HPP
#define BALLANCEMMOSERVER_SERVER_DATA_HPP

struct client_data {
    std::string name;
    bool cheated = false;
    bool state_updated = true;
    bool timestamp_updated = true;
    bool ready = false;
    bmmo::timed_ball_state state{};
    bmmo::map current_map{};
    int32_t current_sector = 0;
    uint8_t uuid[16]{};
    int64_t login_time{};
};

struct map_data {
    int rank = 0;
    SteamNetworkingMicroseconds start_time = 0;
    bmmo::ranking_entry::player_rankings rankings{};
};

typedef std::unordered_map<std::string, map_data> map_data_collection;
typedef std::unordered_map<HSteamNetConnection, client_data> client_data_collection;

#endif //BALLANCEMMOSERVER_SERVER_DATA_HPP
