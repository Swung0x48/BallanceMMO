#ifndef BALLANCEMMOSERVER_ROLE_HPP
#define BALLANCEMMOSERVER_ROLE_HPP

#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <unordered_map>
#include <cassert>
#include <cstdarg>

class role {
public:
    static void init_socket() {
#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
        SteamDatagramErrMsg err_msg;
        if (!GameNetworkingSockets_Init(nullptr, err_msg))
            FatalError("GameNetworkingSockets_Init failed.  %s", err_msg);
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
        init_timestamp_ = SteamNetworkingUtils()->GetLocalTimestamp();
        SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Msg, DebugOutput);
    }

    role() {
        interface_ = SteamNetworkingSockets();
    }

    virtual void run() = 0;

    virtual bool running() {
        return running_;
    }

    static SteamNetworkingConfigValue_t generate_opt() {
        SteamNetworkingConfigValue_t opt{};
        opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                   (void*)SteamNetConnectionStatusChangedCallbackWrapper);
        return opt;
    }

    virtual void update() {
        poll_incoming_messages();
        poll_connection_state_changes();
//        poll_local_state_changes();
    }

    virtual void poll_local_state_changes() = 0;

    static void destroy() {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
        GameNetworkingSockets_Kill();
#else
        SteamDatagramClient_Kill();
#endif
    }

protected:
    ISteamNetworkingSockets* interface_ = nullptr;
    static inline role* this_instance_ = nullptr;
    static inline SteamNetworkingMicroseconds init_timestamp_;
    std::atomic_bool running_ = false;

    virtual void poll_incoming_messages() = 0;

    virtual void poll_connection_state_changes() {
        this_instance_ = this;
        interface_->RunCallbacks();
    }

    virtual void on_connection_status_changed(SteamNetConnectionStatusChangedCallback_t* pInfo) = 0;

    virtual void on_message(ISteamNetworkingMessage* msg) = 0;

    static void SteamNetConnectionStatusChangedCallbackWrapper(SteamNetConnectionStatusChangedCallback_t* pInfo) {
        this_instance_->on_connection_status_changed(pInfo);
    }

public:
    static void DebugOutput(ESteamNetworkingSocketsDebugOutputType eType, const char* pszMsg) {
        SteamNetworkingMicroseconds time = SteamNetworkingUtils()->GetLocalTimestamp() - init_timestamp_;

        if (eType == k_ESteamNetworkingSocketsDebugOutputType_Bug) {
            fprintf(stderr, "\r%10.2f %s\n> ", time * 1e-6, pszMsg);
            fflush(stdout);
            fflush(stderr);
            exit(1);
        } else {
            printf("\r%10.2f %s\n> ", time * 1e-6, pszMsg);
            fflush(stdout);
        }
    }

    static void Printf(const char* fmt, ...) {
        char text[2048];
        va_list ap;
        va_start(ap, fmt);
        vsprintf(text, fmt, ap);
        va_end(ap);
        char* nl = strchr(text, '\0') - 1;
        if (nl >= text && *nl == '\n')
            *nl = '\0';
        DebugOutput(k_ESteamNetworkingSocketsDebugOutputType_Msg, text);
    }

    static void FatalError(const char* fmt, ...) {
        char text[2048];
        va_list ap;
        va_start(ap, fmt);
        vsprintf(text, fmt, ap);
        va_end(ap);
        char* nl = strchr(text, '\0') - 1;
        if (nl >= text && *nl == '\n')
            *nl = '\0';
        DebugOutput(k_ESteamNetworkingSocketsDebugOutputType_Bug, text);
    }
};

#endif //BALLANCEMMOSERVER_ROLE_HPP
