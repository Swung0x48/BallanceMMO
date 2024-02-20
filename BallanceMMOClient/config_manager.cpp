#define PICOJSON_USE_INT64
#include <picojson/picojson.h>

#include <ShlObj.h>

#include "utility/string_utils.hpp"
#include "config_manager.h"

std::wstring config_manager::get_local_appdata_path() { // local appdata
    std::wstring path_str = L".";
    wchar_t* path_pchar{};
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path_pchar))) {
        path_str = path_pchar;
        CoTaskMemFree(path_pchar);
    }
    return path_str;
}

void config_manager::migrate_config() {
    constexpr const char* const config_path = "..\\ModLoader\\Config\\BallanceMMOClient.cfg";
    std::ifstream config(config_path);
    if (!config.is_open()) {
        config.clear();
        config.open("..\\ModLoader\\Configs\\BallanceMMOClient.cfg");
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
    auto* config = get_config();
    config->SetCategoryComment("Player", "Who are you?");
    auto* tmp_prop = config->GetProperty("Player", "Playername");
    tmp_prop->SetComment("Player name. Can only be changed once every 24 hours (countdown starting after joining a server).");
    tmp_prop->SetDefaultString(bmmo::name_validator::get_random_nickname().c_str());
    props_.emplace("playername", tmp_prop);
    tmp_prop = config->GetProperty("Player", "SpectatorMode");
    tmp_prop->SetComment("Whether to connect to the server as a spectator. Spectators are invisible to other players.");
    tmp_prop->SetDefaultBoolean(false);
    props_.emplace("spectator", tmp_prop);
    // Validation of player names fails at this stage of initialization
    // so we had to put it at the time of post startmenu events.
    load_external_config();
    config->SetCategoryComment("Gameplay", "Settings for your actual gameplay experience in multiplayer.");
    tmp_prop = config->GetProperty("Gameplay", "Extrapolation");
    tmp_prop->SetComment("Apply quadratic extrapolation to make movement of balls look smoother at a slight cost of accuracy.");
    tmp_prop->SetBoolean(true); // force extrapolation for now
    props_.emplace("extrapolation", tmp_prop);
    tmp_prop = config->GetProperty("Gameplay", "PlayerListColor");
    tmp_prop->SetComment("Text color of the player list (press Ctrl+Tab to toggle visibility) in hexadecimal RGB format. Default: FFE3A1");
    tmp_prop->SetDefaultString("FFE3A1");
    props_.emplace("player_list_color", tmp_prop);
    tmp_prop = config->GetProperty("Gameplay", "DynamicOpacity");
    tmp_prop->SetComment("Whether to dynamically adjust opacities of other spirit balls based on their distances to the current camera.");
    tmp_prop->SetDefaultBoolean(true);
    props_.emplace("dynamic_opacity", tmp_prop);
    tmp_prop = config->GetProperty("Gameplay", "SoundNotification");
    tmp_prop->SetComment("Whether to play beep sounds in addition to chat notifications on important server events.");
    tmp_prop->SetDefaultBoolean(true);
    props_.emplace("sound_notification", tmp_prop);
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

void config_manager::save_external_config(std::string uuid) {
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
