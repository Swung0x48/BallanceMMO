#ifndef BALLANCEMMOSERVER_ROLE_HPP
#define BALLANCEMMOSERVER_ROLE_HPP

#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <cassert>
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif
#include "../entity/globals.hpp"
#include "../utility/ansi_colors.hpp"

#include "../utility/misc.hpp"

static constexpr inline size_t ONCE_RECV_MSG_COUNT = 1024;

class role {
public:
    static void init_socket() {
#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
        SteamDatagramErrMsg err_msg;
        if (!GameNetworkingSockets_Init(nullptr, err_msg))
            bmmo::FatalError("GameNetworkingSockets_Init failed.  %s", err_msg);
#else
        SteamDatagramClient_SetAppID(570); // Just set something, doesn't matter what
        //SteamDatagramClient_SetUniverse( k_EUniverseDev );

        SteamDatagramErrMsg errMsg;
        if (!SteamDatagramClient_Init(true, errMsg))
            FatalError("SteamDatagramClient_Init failed.  %s", errMsg);

        // Disable authentication when running with Steam, for this
        // example, since we're not a real app.
        //
        // Authentication is disabled automatically in the open-source
        // version since we don't have a trusted third party to issue
        // certs.
        SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);
#endif
#ifdef _WIN32
        DWORD mode;
        GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode);
        SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        // std::ignore = _setmode(_fileno(stdin), _O_U16TEXT);
#endif
        init_timestamp_ = SteamNetworkingUtils()->GetLocalTimestamp();
        init_time_t_ = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Msg,
                bmmo::DebugOutput);
    }

    role() {
        interface_ = SteamNetworkingSockets();
    }

    virtual bool setup() { return true; };
    virtual void run() = 0;

    virtual bool running() {
        return running_;
    }

    static SteamNetworkingConfigValue_t generate_opt() {
        SteamNetworkingConfigValue_t opt{};
        SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_TimeoutConnected, 7200);
        opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                   (void*)SteamNetConnectionStatusChangedCallbackWrapper);
        return opt;
    }

    virtual bool update() {
        int msg_count = poll_incoming_messages();
        poll_connection_state_changes();
//        poll_local_state_changes();
        return msg_count > 0;
    }

    virtual void poll_local_state_changes() = 0;

    static void destroy() {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
        GameNetworkingSockets_Kill();
#else
        SteamDatagramClient_Kill();
#endif
        bmmo::close_log();
    }

protected:
    ISteamNetworkingSockets* interface_ = nullptr;
    static inline role* this_instance_ = nullptr;
    static inline SteamNetworkingMicroseconds init_timestamp_;
    static inline time_t init_time_t_;
    std::atomic_bool running_ = false;
    ISteamNetworkingMessage* incoming_messages_[ONCE_RECV_MSG_COUNT];

    virtual int poll_incoming_messages() = 0;

    virtual void poll_connection_state_changes() {
        this_instance_ = this;
        interface_->RunCallbacks();
    }

    virtual void on_connection_status_changed(SteamNetConnectionStatusChangedCallback_t* pInfo) = 0;

    virtual void on_message(ISteamNetworkingMessage* msg) = 0;

    static void SteamNetConnectionStatusChangedCallbackWrapper(SteamNetConnectionStatusChangedCallback_t* pInfo) {
        this_instance_->on_connection_status_changed(pInfo);
    }

    // triggers an actual segmentation fault; I was too lazy to fake one
    static void trigger_fatal_error() {
        *(volatile int*) 0 = 0;
    }

public:
    static void set_logging_level(ESteamNetworkingSocketsDebugOutputType eType) {
        SteamNetworkingUtils()->SetDebugOutputFunction(eType, bmmo::DebugOutput);
    }
};

#endif //BALLANCEMMOSERVER_ROLE_HPP
