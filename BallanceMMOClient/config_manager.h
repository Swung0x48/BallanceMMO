#pragma once

#include <chrono>
#include <unordered_map>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/lexical_cast.hpp>

#ifndef PICOJSON_USE_INT64
# define PICOJSON_USE_INT64
#endif // !PICOJSON_USE_INT64
#include <picojson/picojson.h>

#include "bml_includes.h"
#include "log_manager.h"
#include "game_state.h"
#include "common.hpp"

// We have 3 config files:
// 1. BallanceMMOClient.cfg (the one provided by the BML API)
// 2. BallanceMMOClient_external.json (our own config file for uuid and last name change time,
//    stored in %LocalAppData% to be shared among all Ballance installations & prevent easy tampering)
// 3. BallanceMMOClient_extra.json (our own json config file for server list etc, stored alongside #1)
class config_manager {
private:
    log_manager* log_manager_;
    std::function<IConfig*()> config_getter_;

    // only use this after migrating config as it could be `Config` or `Configs`
    std::string config_directory_path = "..\\ModLoader\\Config";
    const std::wstring LEGACY_UUID_FILE_PATH = bmmo::SHARED_CONFIG_PATH + L"\\BallanceMMOClient_UUID.cfg";
    const std::wstring EXTERNAL_CONFIG_PATH = bmmo::SHARED_CONFIG_PATH + L"\\" + bmmo::CLIENT_EXTERNAL_CONFIG_NAME;
    inline std::string get_extra_config_path() const {
        return config_directory_path + "\\" + "BallanceMMOClient_extra.json";
    }

    std::unordered_map<std::string, IProperty*> props_;
    boost::uuids::uuid uuid_{};

    int64_t last_name_change_time_{};
    bool name_changed_ = false, bypass_name_check_ = false;

    picojson::object extra_config_;
    void init_extra_config();
    void save_extra_config() const;

public:
    config_manager(log_manager* log_manager, std::function<IConfig*()> config_getter):
            log_manager_(log_manager), config_getter_(config_getter) {}

    inline const boost::uuids::uuid& get_uuid() { return uuid_; }

    // we have to migrate our config before using bml's config manager
    // thus the getter instead of directly asking for IConfig* in our constructor
    inline auto get_config() { return config_getter_(); }
    inline IProperty* get_property(const std::string& name) { return props_[name]; }
    inline IProperty* operator[](const std::string& name) { return props_[name]; }

    // ver <= 3.4.5-alpha6: no external config
    // 3.4.5-alpha6 < ver < 3.4.8-beta12: external plain text uuid config
    // ver >= 3.4.8-alpha12: external json config
    void migrate_config();
    void init_config();
    void load_external_config();
    void save_external_config(std::string uuid = {}) const;

    // returns: if the name changed
    bool check_and_set_nickname(IProperty* prop, game_state& db);
    void check_and_save_name_change_time();
    void validate_nickname();

    // extra config accessors
    void get_server_data(picojson::array& servers, size_t& server_index);
    void set_server_data(const picojson::array& servers, size_t server_index);
    bool get_player_list_visible();
    void set_player_list_visible(bool visible);
};
