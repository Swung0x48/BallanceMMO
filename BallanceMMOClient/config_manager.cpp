#include <fstream>
#include "utility/string_utils.hpp"
#include "config_manager.h"

namespace {
    inline static const picojson::object DEFAULT_EXTRA_CONFIG {
        {"servers", picojson::value{picojson::array{}}},
        {"selected_server", picojson::value{0ll}},
        {"player_list_visible", picojson::value{false}},
    };
}

void config_manager::init_extra_config() {
    picojson::value v;
    std::ifstream ifile(get_extra_config_path());
    if (ifile.is_open()) ifile >> v;
    else v = picojson::value{DEFAULT_EXTRA_CONFIG};
    ifile.close();
    try { // just to validate the structure
        extra_config_ = v.get<picojson::object>();
        for (const auto& [key, val]: DEFAULT_EXTRA_CONFIG) {
            if (extra_config_.find(key) == extra_config_.end())
                extra_config_[key] = val;
        }
    }
    catch (const std::exception& e) {
        log_manager_->get_logger()->Info("Error parsing %s: %s", get_extra_config_path(), e.what());
        extra_config_ = DEFAULT_EXTRA_CONFIG;
    }
}

void config_manager::save_extra_config() const {
    std::ofstream ofile(get_extra_config_path());
    if (!ofile.is_open()) return;
    ofile << picojson::value{extra_config_}.serialize(true);
}

void config_manager::migrate_config() {
    constexpr const char* const config_name = "\\BallanceMMOClient.cfg";
    std::ifstream config(config_directory_path + config_name);
    if (!config.is_open()) {
        config.clear();
        config_directory_path += "s"; // configs
        config.open(config_directory_path + config_name);
        // BMLPlus parity issue
        if (!config.is_open()) return;
    }
    std::string temp_str;
    while (config >> temp_str) {
        if (temp_str == "BallanceMMOClient")
            break;
    }
    config >> temp_str >> temp_str;
    auto version = bmmo::version_t::from_string(temp_str);
    if (version >= bmmo::version_t{ 3, 4, 8, bmmo::Beta, 12 })
        return;

    log_manager_->get_logger()->Info("Migrating config data ...");
    if (version <= bmmo::version_t{ 3, 4, 5, bmmo::Alpha, 6 }) {
        while (config >> temp_str) {
            if (temp_str == "UUID")
                break;
        }
        if (config.eof())
            temp_str = boost::uuids::to_string(boost::uuids::random_generator()());
        else
            config >> temp_str;
    }
    else {
        {
            std::ifstream uuid_config(LEGACY_UUID_FILE_PATH);
            if (!uuid_config.is_open())
                return;
            uuid_config >> temp_str;
        }
        DeleteFileW(LEGACY_UUID_FILE_PATH.c_str());
    }

    save_external_config(temp_str);
}

void config_manager::init_config() {
    migrate_config();
    init_extra_config();
    auto* config = get_config();
    config->SetCategoryComment("Player", "Who are you?");
    auto* tmp_prop = config->GetProperty("Player", "Playername");
    tmp_prop->SetComment("Player name. Can only be changed once every 24 hours (countdown starting after joining a server).");
    tmp_prop->SetDefaultString(bmmo::name_validator::get_random_nickname().c_str());
    props_["playername"] = tmp_prop;
    tmp_prop = config->GetProperty("Player", "SpectatorMode");
    tmp_prop->SetComment("Whether to connect to the server as a spectator. Spectators are invisible to other players.");
    tmp_prop->SetDefaultBoolean(false);
    props_["spectator"] = tmp_prop;
    // Validation of player names fails at this stage of initialization
    // so we had to put it at the time of post startmenu events.
    load_external_config();
    config->SetCategoryComment("Gameplay", "Settings for your actual gameplay experience in multiplayer.");
    tmp_prop = config->GetProperty("Gameplay", "Extrapolation");
    tmp_prop->SetComment("Apply quadratic extrapolation to make movement of balls look smoother at a slight cost of accuracy.");
    tmp_prop->SetBoolean(true); // force extrapolation for now
    props_["extrapolation"] = tmp_prop;
    tmp_prop = config->GetProperty("Gameplay", "PlayerListColor");
    tmp_prop->SetComment("Text color of the player list (press Ctrl+Tab to toggle visibility) in hexadecimal RGB format. Default: FFE3A1");
    tmp_prop->SetDefaultString("FFE3A1");
    props_["player_list_color"] = tmp_prop;
    tmp_prop = config->GetProperty("Gameplay", "DynamicOpacity");
    tmp_prop->SetComment("Whether to dynamically adjust opacities of other spirit balls based on their distances to the current camera.");
    tmp_prop->SetDefaultBoolean(true);
    props_["dynamic_opacity"] = tmp_prop;
    tmp_prop = config->GetProperty("Gameplay", "SoundNotification");
    tmp_prop->SetComment("Whether to play sounds in addition to chat notifications on server events. Note that important events still play sounds even when this is disabled.");
    tmp_prop->SetDefaultBoolean(true);
    props_["sound_notification"] = tmp_prop;
    tmp_prop = config->GetProperty("Gameplay", "MuteEverything");
    tmp_prop->SetComment("Disable all sound events. Not recommended but still here just in case this is needed.");
    tmp_prop->SetDefaultBoolean(false);
    props_["mute_everything"] = tmp_prop;
}

void config_manager::load_external_config() {
    try {
        std::ifstream ifile(EXTERNAL_CONFIG_PATH);
        picojson::value external_config_v;
        ifile >> external_config_v;
        auto& external_config = external_config_v.get<picojson::object>();
        uuid_ = boost::lexical_cast<boost::uuids::uuid>(external_config["uuid"].get<std::string>());
        last_name_change_time_ = external_config["last_name_change"].get<int64_t>();
    }
    catch (...) {
        log_manager_->get_logger()->Warn("Invalid UUID. A new UUID has been generated.");
        uuid_ = boost::uuids::random_generator()();
        save_external_config();
    }
}

void config_manager::save_external_config(std::string uuid) const {
    if (uuid.empty())
        uuid = boost::uuids::to_string(uuid_);
    picojson::object external_config{
        {"uuid", picojson::value{uuid}},
        {"last_name_change", picojson::value{last_name_change_time_}},
    };
    std::ofstream ofile(EXTERNAL_CONFIG_PATH);
    ofile << picojson::value{external_config}.serialize(true);
}

// returns: if the name changed
bool config_manager::check_and_set_nickname(IProperty* prop, game_state& db) {
    if (bypass_name_check_) {
        bypass_name_check_ = false;
        return false;
    }
    using namespace std::chrono;
    auto last_name_change = sys_time(seconds(last_name_change_time_));
    if (std::chrono::system_clock::now() - last_name_change < 24h) {
        bypass_name_check_ = true;
        prop->SetString(bmmo::string_utils::utf8_to_ansi(db.get_nickname()).c_str());
        auto next_name_change = system_clock::to_time_t(last_name_change + 24h);
        char error_msg[128];
        std::strftime(error_msg, sizeof(error_msg),
            "Error: You can only change your name every 24 hours (after %F %T).",
            std::localtime(&next_name_change));
        log_manager_->send_ingame_message(error_msg, bmmo::ansi::BrightRed);
        return false;
    }
    std::string new_name = prop->GetString();
    if (new_name == db.get_nickname())
        return false;
    name_changed_ = true;
    validate_nickname();
    db.set_nickname(bmmo::string_utils::ansi_to_utf8(prop->GetString()));
    return true;
}

void config_manager::check_and_save_name_change_time() {
    if (!name_changed_) return;
    using namespace std::chrono;
    last_name_change_time_ = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    save_external_config();
    name_changed_ = false;
}

void config_manager::validate_nickname() {
    auto* name_prop = get_property("playername");
    std::string name = name_prop->GetString();
    if (!bmmo::name_validator::is_valid(name)) {
        std::string valid_name = bmmo::name_validator::get_valid_nickname(name);
        log_manager_->send_ingame_message(std::format(
            "Invalid player name \"{}\", replaced with \"{}\".", name, valid_name),
            bmmo::ansi::BrightRed);
        bypass_name_check_ = true;
        name_prop->SetString(valid_name.c_str());
    }
}

void config_manager::get_server_data(picojson::array& servers, size_t& server_index) {
    servers = extra_config_["servers"].get<picojson::array>();
    server_index = (size_t)extra_config_["selected_server"].get<int64_t>();
}

void config_manager::set_server_data(const picojson::array& servers, size_t server_index) {
    // we can use (picojson::) value::set<array> but using value::set(array)
    // gives value::set<array&> and linker error somehow
    // also there doesn't seem to be value::set<int64_t> for some reason
    extra_config_["servers"] = picojson::value{servers};
    extra_config_["selected_server"] = picojson::value{int64_t(server_index)};
    save_extra_config();
}

bool config_manager::get_player_list_visible() {
    return extra_config_["player_list_visible"].get<bool>();
}

void config_manager::set_player_list_visible(bool visible) {
    extra_config_["player_list_visible"] = picojson::value{visible};
    save_extra_config();
}
