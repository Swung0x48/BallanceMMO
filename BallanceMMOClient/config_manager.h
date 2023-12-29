#pragma once

#include <chrono>
#include <unordered_map>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/lexical_cast.hpp>

#include "bml_includes.h"
#include "log_manager.h"
#include "game_state.h"

class config_manager {
private:
    log_manager* log_manager_;
    std::function<IConfig*()> config_getter_;

    std::wstring get_local_appdata_path();
    const std::wstring LOCAL_APPDATA_PATH = get_local_appdata_path();
    const std::wstring LEGACY_UUID_FILE_PATH = LOCAL_APPDATA_PATH + L"\\BallanceMMOClient_UUID.cfg";
    const std::wstring EXTERNAL_CONFIG_PATH = LOCAL_APPDATA_PATH + L"\\BallanceMMOClient_external.json";

    std::unordered_map<std::string, IProperty*> props_;
    boost::uuids::uuid uuid_{};

    int64_t last_name_change_time_{};
    bool name_changed_ = false, bypass_name_check_ = false;

public:
    config_manager(log_manager* log_manager, std::function<IConfig*()> config_getter):
        log_manager_(log_manager), config_getter_(config_getter) {}

    inline boost::uuids::uuid& get_uuid() { return uuid_; }

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
    void save_external_config(std::string uuid = {});

    // returns: if the name changed
    bool check_and_set_nickname(IProperty* prop, game_state& db);
    void check_and_save_name_change_time();
    void validate_nickname();
};
