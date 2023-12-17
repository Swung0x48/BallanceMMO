#pragma once

#include "bml_includes.h"
#include "client.h"

namespace bmmo::exported:: inline _v3 {
    class listener {
    public:
        virtual void on_login(const char* address, const char* name) {};
        virtual void on_logout() {};
        virtual void on_countdown(const bmmo::countdown_msg* msg) {};
        virtual void on_level_finish(const bmmo::level_finish_v2_msg* msg) {};
        virtual void on_player_login(HSteamNetConnection id) {};
        virtual void on_player_logout(HSteamNetConnection id) {};

        // return: whether to cancel the event
        virtual bool on_pre_login(const char* address, const char* name) { return true; };
        virtual bool on_pre_chat(const char* text, const size_t size) { return true; };
    };

    class client : public ::client {
    public:
        virtual std::pair<HSteamNetConnection, std::string> get_own_id() = 0;
        virtual bool is_spectator() { return false; }
        virtual named_map get_current_map() = 0;
        virtual std::unordered_map<HSteamNetConnection, std::string> get_client_list() = 0;
        virtual std::string get_username(HSteamNetConnection client_id) = 0;
        virtual std::string get_map_name(const bmmo::map& map) = 0;

        virtual bool register_listener(listener* listener) = 0;
        virtual bool remove_listener(listener* listener) = 0;

        virtual void connect_to_server(const char* address, const char* name = "") = 0;
        virtual void disconnect_from_server() = 0;
    };
};
