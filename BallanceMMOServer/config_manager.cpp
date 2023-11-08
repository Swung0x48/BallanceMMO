#include <fstream>

#define PICOJSON_USE_INT64
#include <picojson/picojson.h>

#include "common.hpp"
#include "config_manager.hpp"

bool config_manager::load() {
    std::string logging_level_string = "important";
    std::ifstream ifile("config.yml");
    if (ifile.is_open() && ifile.peek() != std::ifstream::traits_type::eof()) {
        try {
            config_ = YAML::Load(ifile);
            if (config_["op_list"])
                op_players = config_["op_list"].as<decltype(op_players)>();
            if (config_["enable_op_privileges"])
                op_mode = config_["enable_op_privileges"].as<bool>();
            if (config_["ban_list"])
                banned_players = config_["ban_list"].as<decltype(banned_players)>();
            if (config_["mute_list"]) {
                auto muted_vector = config_["mute_list"].as<std::vector<std::string>>();
                muted_players = std::unordered_set(muted_vector.begin(), muted_vector.end());
            }
            if (config_["restart_level_after_countdown"])
                restart_level = config_["restart_level_after_countdown"].as<bool>();
            if (config_["force_restart_after_countdown"])
                force_restart_level = config_["force_restart_after_countdown"].as<bool>();
            if (config_["logging_level"])
                logging_level_string = config_["logging_level"].as<std::string>();
            if (config_["save_player_status_to_file"])
                save_player_status_to_file_ = config_["save_player_status_to_file"].as<bool>();
        } catch (const std::exception& e) {
            role::Printf("Error: failed to parse config: %s", e.what());
            return false;
        }
    } else {
        role::Printf("Config is empty. Generating default config...");
        op_players = {{"example_player", "00000001-0002-0003-0004-000000000005"}};
        banned_players = {{"00000001-0002-0003-0004-000000000005", "You're banned from this server."}};
        muted_players = {"00000001-0002-0003-0004-000000000005"};
    }

    config_["enable_op_privileges"] = op_mode;
    config_["restart_level_after_countdown"] = restart_level;
    config_["force_restart_after_countdown"] = force_restart_level;
    config_["save_player_status_to_file"] = save_player_status_to_file_;
    if (logging_level_string == "msg")
        logging_level = k_ESteamNetworkingSocketsDebugOutputType_Msg;
    else if (logging_level_string == "warning")
        logging_level = k_ESteamNetworkingSocketsDebugOutputType_Warning;
    else {
        logging_level = k_ESteamNetworkingSocketsDebugOutputType_Important;
        config_["logging_level"] = "important";
    }
    role::set_logging_level(logging_level);
    if (config_["map_name_list"]) {
        default_map_names.clear();
        for (const auto& element: config_["map_name_list"]) {
            std::string hash(sizeof(bmmo::map::md5), 0);
            bmmo::string_utils::hex_chars_from_string(reinterpret_cast<uint8_t*>(hash.data()), element.first.as<std::string>());
            default_map_names.try_emplace(hash, element.second.as<std::string>());
        }
    } else
        config_["map_name_list"] = YAML::Node(YAML::NodeType::Map);

    ifile.close();
    save();
    role::Printf("Config loaded successfully.");
    return true;
}

void config_manager::print_bans() {
    for (const auto& [uuid, reason]: banned_players)
        role::Printf("%s: %s", uuid, reason);
    role::Printf("%d UUID%s banned in total.", banned_players.size(),
            banned_players.size() == 1 ? "" : "s");
}

void config_manager::print_mutes() {
    for (const auto& uuid: muted_players)
        role::Printf("%s", uuid);
    role::Printf("%d UUID%s muted in total.", muted_players.size(),
            muted_players.size() == 1 ? "" : "s");
}

void config_manager::save() {
    config_["op_list"] = op_players;
    config_["ban_list"] = banned_players;
    config_["mute_list"] = std::vector<std::string>(muted_players.begin(), muted_players.end());
    std::ofstream config_file("config.yml");
    if (!config_file.is_open()) {
        role::Printf("Error: failed to open config file for writing.");
        return;
    }
    auto current_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    config_file << "# Config file for Ballance MMO Server v" << bmmo::current_version.to_string() << " - "
                    << std::put_time(std::localtime(&current_time), "%F %T") << "\n"
                << "# Notes:\n"
                << "# - Op list player data style: \"playername: uuid\".\n"
                << "# - Ban list style: \"uuid: reason\".\n"
                << "# - Mute list style: \"- uuid\".\n"
                << "# - Map name list style: \"md5_hash: name\".\n"
                << "# - Level restart: whether to restart on clients' sides after \"Go!\". If not forced, only for clients on the same map.\n"
                << "# - Options for log levels: important, warning, msg.\n"
                << std::endl;
    config_file << config_;
    config_file << std::endl;
    config_file.close();
}

void config_manager::save_login_data(const std::string& ip_str, const std::string& uuid_str, const std::string& name) {
    YAML::Node login_data;
    std::ifstream ifile("login_data.yml");
    if (ifile.is_open() && ifile.peek() != std::ifstream::traits_type::eof()) {
        try {
            login_data = YAML::Load(ifile);
        } catch (const std::exception& e) {
            role::Printf("Error: failed to parse config: %s", e.what());
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



void config_manager::save_player_status(const client_data_collection& clients) {
    if (!save_player_status_to_file_)
        return;

    picojson::array player_list{};
    using picojson::value;
    for (const auto& [_, data]: clients) {
        player_list.emplace_back(picojson::object{
            {"name", value{data.name}},
            {"login_time", value{data.login_time}},
        });
    }
    std::ofstream player_status_file("player_status.json");
    if (player_status_file.is_open()) {
        player_status_file << value{player_list};
    }
}
