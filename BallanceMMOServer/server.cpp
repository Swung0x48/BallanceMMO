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
#include <cassert>
#include <cstdarg>

struct client_data {
    std::string name;
};

class server {
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

    server(uint16_t port) {
        interface_ = SteamNetworkingSockets();
        port_ = port;
    }

    void update() {
        poll_incoming_messages();
        poll_connection_state_changes();
    }

    void run() {
        if (!setup())
            FatalError("Server failed on setup.");

        running_ = true;
        server_thread_ = std::thread([this]() {
            while (running_) {
                update();
            }
        });

        while (running_) {
            poll_local_user_input();
        }
    }

    void shutdown() {
        running_ = false;
        if (server_thread_.joinable())
            server_thread_.join();
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
    bool setup() {
        SteamNetworkingIPAddr local_address{};
        local_address.Clear();
        local_address.m_port = port_;
        SteamNetworkingConfigValue_t opt{};
        opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*) SteamNetConnectionStatusChangedCallbackWrapper);
        listen_socket_ = interface_->CreateListenSocketIP(local_address, 1, &opt);
        if (listen_socket_ == k_HSteamListenSocket_Invalid) {
            return false;
        }

        poll_group_ = interface_->CreatePollGroup();
        if (poll_group_ == k_HSteamNetPollGroup_Invalid) {
            return false;
        }

        return true;
    }

    void on_connection_status_changed(SteamNetConnectionStatusChangedCallback_t* pInfo) {
        switch ( pInfo->m_info.m_eState )
        {
            case k_ESteamNetworkingConnectionState_None:
                // NOTE: We will get callbacks here when we destroy connections.  You can ignore these.
                break;

            case k_ESteamNetworkingConnectionState_ClosedByPeer:
            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
            {
                // Ignore if they were not previously connected.  (If they disconnected
                // before we accepted the connection.)
                if ( pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connected )
                {

                    // Locate the client.  Note that it should have been found, because this
                    // is the only codepath where we remove clients (except on shutdown),
                    // and connection change callbacks are dispatched in queue order.
                    auto itClient = clients_.find( pInfo->m_hConn );
                    assert( itClient != clients_.end() );

                    // Select appropriate log messages
                    const char *pszDebugLogAction;
                    if ( pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally )
                    {
                        pszDebugLogAction = "problem detected locally";
                    }
                    else
                    {
                        // Note that here we could check the reason code to see if
                        // it was a "usual" connection or an "unusual" one.
                        pszDebugLogAction = "closed by peer";
                    }

                    // Spew something to our own log.  Note that because we put their nick
                    // as the connection description, it will show up, along with their
                    // transport-specific data (e.g. their IP address)


                    clients_.erase( itClient );
                }
                else
                {
                    assert( pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting );
                }

                // Clean up the connection.  This is important!
                // The connection is "closed" in the network sense, but
                // it has not been destroyed.  We must close it on our end, too
                // to finish up.  The reason information do not matter in this case,
                // and we cannot linger because it's already closed on the other end,
                // so we just pass 0's.
                interface_->CloseConnection( pInfo->m_hConn, 0, nullptr, false );
                break;
            }

            case k_ESteamNetworkingConnectionState_Connecting:
            {
                // This must be a new connection
                assert( clients_.find( pInfo->m_hConn ) == clients_.end() );

                printf( "Connection request from %s", pInfo->m_info.m_szConnectionDescription );

                // A client is attempting to connect
                // Try to accept the connection.
                if ( interface_->AcceptConnection( pInfo->m_hConn ) != k_EResultOK )
                {
                    // This could fail.  If the remote host tried to connect, but then
                    // disconnected, the connection may already be half closed.  Just
                    // destroy whatever we have on our side.
                    interface_->CloseConnection( pInfo->m_hConn, 0, nullptr, false );
                    printf( "Can't accept connection.  (It was already closed?)" );
                    break;
                }

                // Assign the poll group
                if ( !interface_->SetConnectionPollGroup( pInfo->m_hConn, poll_group_ ) )
                {
                    interface_->CloseConnection( pInfo->m_hConn, 0, nullptr, false );
                    printf( "Failed to set poll group?" );
                    break;
                }

                // Generate a random nick.  A random temporary nick
                // is really dumb and not how you would write a real chat server.
                // You would want them to have some sort of signon message,
                // and you would keep their client in a state of limbo (connected,
                // but not logged on) until them.  I'm trying to keep this example
                // code really simple.
                char nick[ 64 ];
                sprintf( nick, "BraveWarrior%d", 10000 + ( rand() % 100000 ) );

                // Add them to the client list, using std::map wacky syntax
                clients_[pInfo->m_hConn] = {nick};
//                SetClientNick( pInfo->m_hConn, nick );
                break;
            }

            case k_ESteamNetworkingConnectionState_Connected:
                // We will get a callback immediately after accepting the connection.
                // Since we are the server, we can ignore this, it's not news to us.
                break;

            default:
                // Silences -Wswitch
                break;
        }
    }

    void poll_incoming_messages() {
        while (running_) {
            ISteamNetworkingMessage* incoming_message = nullptr;
            int msg_count = interface_->ReceiveMessagesOnPollGroup(poll_group_, &incoming_message, 1);
            if (msg_count == 0)
                break;
            if (msg_count < 0)
                FatalError("Error checking for messages.");
            assert(msg_count == 1 && incoming_message);
            auto client_it = clients_.find(incoming_message->m_conn);
            assert(client_it != clients_.end());

            // TODO: replace with actual message data structure handling
            std::string msg;
            msg.assign((const char*)incoming_message->m_pData, incoming_message->m_cbSize);

            incoming_message->Release();
            std::cout << client_it->second.name << ": " << msg << std::endl;
            interface_->SendMessageToConnection(client_it->first, msg.c_str(), msg.length() + 1, k_nSteamNetworkingSend_Reliable,
                                                nullptr);
        }
    }

    void poll_connection_state_changes() {
        this_instance_ = this;
        interface_->RunCallbacks();
    }

    void poll_local_user_input() {
        std::string cmd;
        std::cin >> cmd;
        if (cmd == "stop") {
            shutdown();
        }
    }

    static void SteamNetConnectionStatusChangedCallbackWrapper(SteamNetConnectionStatusChangedCallback_t* pInfo) {
        this_instance_->on_connection_status_changed(pInfo);
    }

    static void DebugOutput( ESteamNetworkingSocketsDebugOutputType eType, const char *pszMsg )
    {
        SteamNetworkingMicroseconds time = SteamNetworkingUtils()->GetLocalTimestamp() - init_timestamp_;
        printf("%10.6f %s\n", time*1e-6, pszMsg);
        fflush(stdout);

        if (eType == k_ESteamNetworkingSocketsDebugOutputType_Bug)
        {
            fflush(stdout);
            fflush(stderr);
            exit(1);
        }
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

    uint16_t port_ = 0;
    ISteamNetworkingSockets* interface_ = nullptr;
    HSteamListenSocket listen_socket_ = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup poll_group_ = k_HSteamNetPollGroup_Invalid;
    std::atomic_bool running_ = false;
    std::thread server_thread_;
    std::unordered_map<HSteamNetConnection, client_data> clients_;

    static inline server* this_instance_;
    static inline SteamNetworkingMicroseconds init_timestamp_;
};

int main() {
    std::cout << "Initializing sockets..." << std::endl;
    server::init_socket();

    uint16_t port = 26676;
    std::cout << "Starting server at port " << port << std::endl;
    server server(port);

    std::cout << "Bootstrapping server..." << std::endl;
    server.run();

    std::cout << "Stopping..." << std::endl;
    server::destroy();
}