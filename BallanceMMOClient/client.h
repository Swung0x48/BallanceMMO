#pragma once
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif

//#include <BML/BMLAll.h> // Just for logging purposes
#include <memory>
#include <functional>

class client
{
public:
	client(std::function<void(ESteamNetworkingSocketsDebugOutputType, const char*)> logging_callback,
		std::function<void(SteamNetConnectionStatusChangedCallback_t*)> on_connection_status_changed_callback):
		logging_callback_(std::move(logging_callback)),
		on_connection_status_changed_callback_(std::move(on_connection_status_changed_callback))
	{
		//logger_ = logger;
		//bml_ = bml;
		this_instance_ = this;

		SteamDatagramErrMsg msg;
		if (!GameNetworkingSockets_Init(nullptr, msg)) {
			logging_callback_(k_ESteamNetworkingSocketsDebugOutputType_Error, "GNS init failed");
			logging_callback_(k_ESteamNetworkingSocketsDebugOutputType_Error, msg);
			//logger_->Error("GNS init failed: %s", msg);
		}

		init_timestamp_ = SteamNetworkingUtils()->GetLocalTimestamp();
		SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Msg, LoggingWrapper);

		interface_ = SteamNetworkingSockets();
	}

	bool connect(const char* address) {
		SteamNetworkingIPAddr addr;
		addr.ParseString(address);
		return connect(addr);
	}

	bool connect(SteamNetworkingIPAddr address) {
		server_address_ = address;
		char sz_addr[SteamNetworkingIPAddr::k_cchMaxString];
		server_address_.ToString(sz_addr, sizeof(sz_addr), true);

		//logger_->Info("Connecting to server at %s", sz_addr);
		logging_callback_(k_ESteamNetworkingSocketsDebugOutputType_Msg, "Connecting to server...");
		logging_callback_(k_ESteamNetworkingSocketsDebugOutputType_Msg, sz_addr);
		
		SteamNetworkingConfigValue_t opt;
		opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)OnConnectionStatusChangedWrapper);
		connection_ = interface_->ConnectByIPAddress(server_address_, 1, &opt);

		if (connection_ == k_HSteamNetConnection_Invalid) {
			logging_callback_(k_ESteamNetworkingSocketsDebugOutputType_Error, "Failed to create connection.");

			//logger_->Error("Failed to create connection.");
			return false;
		}

		return true;
	}

	bool connected() {
		return state_ == k_ESteamNetworkingConnectionState_Connected;
	}

	ESteamNetworkingConnectionState get_state() const {
		return state_;
	}

	void poll_connection_state_changes() {
		this_instance_ = this;
		interface_->RunCallbacks();
	}

	SteamNetworkingMicroseconds get_init_timestamp() {
		return init_timestamp_;
	}

	void close_connection(HSteamNetConnection connection) {
		interface_->CloseConnection(connection, 0, nullptr, false);
		connection_ = k_HSteamNetConnection_Invalid;
	}

private:
	void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo) {
		state_ = pInfo->m_info.m_eState;
		on_connection_status_changed_callback_(pInfo);
		//logger_->Info("Connection status changed. %d -> %d", pInfo->m_eOldState, pInfo->m_info.m_eState);
		//switch (pInfo->m_info.m_eState) {
		//case k_ESteamNetworkingConnectionState_None:
		//	// NOTE: We will get callbacks here when we destroy connections.  You can ignore these.
		//	break;

		//case k_ESteamNetworkingConnectionState_ClosedByPeer:
		//case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		//{
		//	// Print an appropriate message
		//	if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting) {
		//		// Note: we could distinguish between a timeout, a rejected connection,
		//		// or some other transport problem.
		//		//bml_->SendIngameMessage("Connect failed. (ProblemDetectedLocally)");
		//		//logger_->Warn(pInfo->m_info.m_szEndDebug);
		//	} else {
		//		// NOTE: We could check the reason code for a normal disconnection
		//		//bml_->SendIngameMessage("Connect failed. (UnknownError)");
		//		//logger_->Warn("Unknown error. (%d->%d) %s", pInfo->m_eOldState, pInfo->m_info.m_eState, pInfo->m_info.m_szEndDebug);
		//	}

		//	// Clean up the connection.  This is important!
		//	// The connection is "closed" in the network sense, but
		//	// it has not been destroyed.  We must close it on our end, too
		//	// to finish up.  The reason information do not matter in this case,
		//	// and we cannot linger because it's already closed on the other end,
		//	// so we just pass 0's.

		//	interface_->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
		//	connection_ = k_HSteamNetConnection_Invalid;
		//	break;
		//}

		//case k_ESteamNetworkingConnectionState_Connecting:
		//	// We will get this callback when we start connecting.
		//	// We can ignore this.
		//	break;

		//case k_ESteamNetworkingConnectionState_Connected:
		//	//logger_->Info("Connected to server.");
		//	break;

		//default:
		//	// Silences -Wswitch
		//	break;
		//}
	}

	static void DebugOutput(ESteamNetworkingSocketsDebugOutputType eType, const char* pszMsg) {
		const char* fmt_string = "[%d] %10.6f %s\n";
//		SteamNetworkingMicroseconds time = SteamNetworkingUtils()->GetLocalTimestamp() - init_timestamp_;
		switch (eType) {
			case k_ESteamNetworkingSocketsDebugOutputType_Bug:
			case k_ESteamNetworkingSocketsDebugOutputType_Error:
				//logger_->Error(fmt_string, eType, time * 1e-6, pszMsg);
				break;
			case k_ESteamNetworkingSocketsDebugOutputType_Important:
			case k_ESteamNetworkingSocketsDebugOutputType_Warning:
				//logger_->Warn(fmt_string, eType, time * 1e-6, pszMsg);
				break;
			default:
				break;
				//logger_->Info(fmt_string, eType, time * 1e-6, pszMsg);
		}

		if (eType == k_ESteamNetworkingSocketsDebugOutputType_Bug) {
			//logger_->Error("We've encountered a bug. Please contact developer with this piece of log.");
			//logger_->Error("Nuking process...");
			exit(1);
		}
	}

	static void OnConnectionStatusChangedWrapper(SteamNetConnectionStatusChangedCallback_t* pInfo) {
		//this_instance_->OnConnectionStatusChanged(pInfo);
		this_instance_->on_connection_status_changed_callback_(pInfo);
	}

	static void LoggingWrapper(ESteamNetworkingSocketsDebugOutputType eType, const char* pszMsg) {
		this_instance_->logging_callback_(eType, pszMsg);
	}

private:
	SteamNetworkingIPAddr server_address_ = SteamNetworkingIPAddr();
	ISteamNetworkingSockets* interface_ = nullptr;
	HSteamNetConnection connection_ = k_HSteamNetConnection_Invalid;
	ESteamNetworkingConnectionState state_;
	std::function<void(ESteamNetworkingSocketsDebugOutputType, const char*)> logging_callback_;
	std::function<void(SteamNetConnectionStatusChangedCallback_t*)> on_connection_status_changed_callback_;
	SteamNetworkingMicroseconds init_timestamp_;
	
	//static inline ILogger* logger_;
	//static inline IBML* bml_;
	static inline client* this_instance_;
};