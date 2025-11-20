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
    std::unordered_map<std::string, std::string> forced_names_, reserved_names_;
    std::unordered_map<std::string, bool> forced_cheat_modes_;

public:
    std::unordered_map<std::string, std::string> op_players, banned_players, default_map_names;
    std::unordered_map<std::string, int> initial_life_counts;
    std::unordered_set<std::string> muted_players;
    bool op_mode = true, restart_level = true, force_restart_level = false;
    bool log_installed_mods = false, log_ball_offs = false, serious_warning_as_dnf = false;
    bool ghost_mode = false, log_level_restarts = false;
    ESteamNetworkingSocketsDebugOutputType logging_level = k_ESteamNetworkingSocketsDebugOutputType_Important;

    bool load();

    void print_bans();
    void print_mutes();
    void log_mod_list(const std::map<std::string, std::string>& mod_list);

    bool has_forced_name(const std::string& uuid_string);
    const std::string& get_forced_name(const std::string& uuid_string);
    bool is_name_reserved(const std::string& name, const std::string& uuid_string);

    // @returns `true` if the client's cheat mode should be forced
    bool get_forced_cheat_mode(const std::string& uuid_string, bool& cheat_mode);

    void save(bool reload_values = true);
    void save_login_data(const SteamNetworkingIPAddr& ip, const std::string& uuid_str, const std::string& name);
    void save_player_status(const client_data_collection& clients);
};

#endif //BALLANCEMMOSERVER_CONFIG_MANAGER_HPP
