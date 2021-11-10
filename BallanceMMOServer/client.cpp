#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <unordered_map>

struct client_data {
    std::string name;
};

class client {
public:
    static void init_socket() {
#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
        SteamDatagramErrMsg err_msg;
        if (!GameNetworkingSockets_Init(nullptr, err_msg))
            FatalError("GameNetworkingSockets_Init failed.  %s", err_msg);
#else
        SteamDatagramClient_SetAppID( 570 ); // Just set something, doesn't matter what
		//SteamDatagramClient_SetUniverse( k_EUniverseDev );

		SteamDatagramErrMsg errMsg;
		if ( !SteamDatagramClient_Init( true, errMsg ) )
			FatalError( "SteamDatagramClient_Init failed.  %s", errMsg );

		// Disable authentication when running with Steam, for this
		// example, since we're not a real app.
		//
		// Authentication is disabled automatically in the open-source
		// version since we don't have a trusted third party to issue
		// certs.
		SteamNetworkingUtils()->SetGlobalConfigValueInt32( k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1 );
#endif
        init_timestamp_ = SteamNetworkingUtils()->GetLocalTimestamp();
        SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Msg, DebugOutput);
    }

    client() {
        interface_ = SteamNetworkingSockets();
    }

    void update() {
        poll_incoming_messages();
        poll_connection_state_changes();
    }

    bool connect(std::string connection_string) {
        SteamNetworkingIPAddr server_address{};
        if (!server_address.ParseString(connection_string.c_str())) {
            return false;
        }
        SteamNetworkingConfigValue_t opt{};
        opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)SteamNetConnectionStatusChangedCallbackWrapper);
        connection_ = interface_->ConnectByIPAddress(server_address, 1, &opt);
        if (connection_ == k_HSteamNetConnection_Invalid)
            return false;

        running_ = true;
        client_thread_ = std::thread([this]() {
            while (running_) {
                update();
            }
        });

        while (running_) {
            poll_local_user_input();
        }

        return true;
    }

    void shutdown() {
        running_ = false;

        interface_->CloseConnection(connection_, 0, "Goodbye", true);


        if (client_thread_.joinable())
            client_thread_.join();
    }

    static void destroy() {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
        GameNetworkingSockets_Kill();
#else
        SteamDatagramClient_Kill();
#endif
    }
private:
    void on_connection_status_changed(SteamNetConnectionStatusChangedCallback_t* pInfo) {
        assert( pInfo->m_hConn == connection_ || connection_ == k_HSteamNetConnection_Invalid );

        // What's the state of the connection?
        switch ( pInfo->m_info.m_eState )
        {
            case k_ESteamNetworkingConnectionState_None:
                // NOTE: We will get callbacks here when we destroy connections.  You can ignore these.
                break;

            case k_ESteamNetworkingConnectionState_ClosedByPeer:
            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
            {
                running_ = false;

                // Print an appropriate message
                if ( pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting )
                {
                    // Note: we could distinguish between a timeout, a rejected connection,
                    // or some other transport problem.
                    printf( "We sought the remote host, yet our efforts were met with defeat.  (%s)", pInfo->m_info.m_szEndDebug );
                }
                else if ( pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally )
                {
                    printf( "Alas, troubles beset us; we have lost contact with the host.  (%s)", pInfo->m_info.m_szEndDebug );
                }
                else
                {
                    // NOTE: We could check the reason code for a normal disconnection
                    printf( "The host hath bidden us farewell.  (%s)", pInfo->m_info.m_szEndDebug );
                }

                // Clean up the connection.  This is important!
                // The connection is "closed" in the network sense, but
                // it has not been destroyed.  We must close it on our end, too
                // to finish up.  The reason information do not matter in this case,
                // and we cannot linger because it's already closed on the other end,
                // so we just pass 0's.
                interface_->CloseConnection( pInfo->m_hConn, 0, nullptr, false );
                connection_ = k_HSteamNetConnection_Invalid;
                break;
            }

            case k_ESteamNetworkingConnectionState_Connecting:
                // We will get this callback when we start connecting.
                // We can ignore this.
                break;

            case k_ESteamNetworkingConnectionState_Connected:
                printf( "Connected to server OK" );
                break;

            default:
                // Silences -Wswitch
                break;
        }
    }

    void poll_incoming_messages() {
        while (running_) {
            ISteamNetworkingMessage* incoming_message = nullptr;
            int msg_count = interface_->ReceiveMessagesOnConnection(connection_, &incoming_message, 1);
            if (msg_count == 0)
                break;
            if (msg_count < 0)
                FatalError("Error checking for messages.");
            assert(msg_count == 1 && incoming_message);

            fwrite(incoming_message->m_pData, 1, incoming_message->m_cbSize, stdout);
            fputc('\n', stdout);
            incoming_message->Release();
        }
    }

    void poll_connection_state_changes() {
        this_instance_ = this;
        interface_->RunCallbacks();
    }

    void poll_local_user_input() {
        std::string input;
        std::cin >> input;
        if (input == "stop") {
            shutdown();
        } else {
            interface_->SendMessageToConnection(connection_,
                                                input.c_str(),
                                                input.length() + 1,
                                                k_nSteamNetworkingSend_Reliable,
                                                nullptr);
        }
    }

    static void SteamNetConnectionStatusChangedCallbackWrapper(SteamNetConnectionStatusChangedCallback_t* pInfo) {
        this_instance_->on_connection_status_changed(pInfo);
    }

    static void DebugOutput( ESteamNetworkingSocketsDebugOutputType eType, const char *pszMsg )
    {
        SteamNetworkingMicroseconds time = SteamNetworkingUtils()->GetLocalTimestamp() - init_timestamp_;

        if (eType == k_ESteamNetworkingSocketsDebugOutputType_Bug)
        {
            fprintf(stderr, "%10.6f %s\n", time*1e-6, pszMsg);
            fflush(stdout);
            fflush(stderr);
            exit(1);
        }
        printf("%10.6f %s\n", time*1e-6, pszMsg);
        fflush(stdout);
    }

    static void FatalError( const char *fmt, ... )
    {
        char text[ 2048 ];
        va_list ap;
        va_start( ap, fmt );
        vsprintf( text, fmt, ap );
        va_end(ap);
        char *nl = strchr( text, '\0' ) - 1;
        if ( nl >= text && *nl == '\n' )
            *nl = '\0';
        DebugOutput( k_ESteamNetworkingSocketsDebugOutputType_Bug, text );
    }

    ISteamNetworkingSockets* interface_ = nullptr;
    HSteamNetConnection connection_ = k_HSteamNetConnection_Invalid;
    std::atomic_bool running_ = false;
    std::thread client_thread_;

    static inline client* this_instance_;
    static inline SteamNetworkingMicroseconds init_timestamp_;
};

int main() {
    std::cout << "Initializing sockets..." << std::endl;
    client::init_socket();

    std::cout << "Creating client instance..." << std::endl;
    client client;

    std::cout << "Connecting to server..." << std::endl;
    client.connect("127.0.0.1:26676");

    std::cout << "Stopping..." << std::endl;
    client::destroy();
}