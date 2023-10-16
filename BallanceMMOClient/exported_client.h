#pragma once

#include "bml_includes.h"
#include "client.h"

namespace bmmo::exported:: inline _v2 {
    class client : public ::client {
    public:
        virtual std::pair<HSteamNetConnection, std::string> get_own_id() = 0;
        virtual bool is_spectator() { return false; }
        virtual named_map get_current_map() = 0;
        virtual std::unordered_map<HSteamNetConnection, std::string> get_client_list() = 0;

        virtual void register_login_callback(IMod* mod, std::function<void()> callback) = 0;
        virtual void register_logout_callback(IMod* mod, std::function<void()> callback) = 0;
    };
};
