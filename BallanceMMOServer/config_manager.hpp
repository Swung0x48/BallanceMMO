#ifndef BALLANCEMMOSERVER_CONFIG_MANAGER_HPP
#define BALLANCEMMOSERVER_CONFIG_MANAGER_HPP
#include <yaml-cpp/yaml.h>
#include <unordered_map>
#include <unordered_set>
#include "server_data.hpp"

class config_manager {
private:
    YAML::Node config_;
    bool save_player_status_to_file_ = false;
    std::unordered_map<std::string, std::string> forced_names_;

public:
    std::unordered_map<std::string, std::string> op_players, banned_players, default_map_names;
    std::unordered_set<std::string> muted_players;
    bool op_mode = true, restart_level = true, force_restart_level = false;
    bool log_installed_mods = false, log_ball_offs = false, serious_warning_as_dnf = false;
    ESteamNetworkingSocketsDebugOutputType logging_level = k_ESteamNetworkingSocketsDebugOutputType_Important;

    bool load();

    void print_bans();
    void print_mutes();
    void log_mod_list(const std::unordered_map<std::string, std::string>& mod_list);

    bool has_forced_name(const std::string& uuid_string);
    const std::string& get_forced_name(const std::string& uuid_string);

    void save(bool reload_values = true);
    void save_login_data(const SteamNetworkingIPAddr& ip, const std::string& uuid_str, const std::string& name);
    void save_player_status(const client_data_collection& clients);
};

#endif //BALLANCEMMOSERVER_CONFIG_MANAGER_HPP
