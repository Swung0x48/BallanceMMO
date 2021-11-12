#pragma once
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif
#include <thread>
#include <chrono>
#include <cassert>

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

	bool alive() {
		return connection_ != k_HSteamNetConnection_Invalid;
	}

	void close_connection() {
		close_connection(this->connection_);
	}

	void close_connection(HSteamNetConnection connection) {
		if (connection != k_HSteamNetConnection_Invalid) {
			interface_->CloseConnection(connection, 0, "Goodbye", true);
			assert(connection == this->connection_);
			connection_ = k_HSteamNetConnection_Invalid;
		}
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
	void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo) {
		state_ = pInfo->m_info.m_eState;
		on_connection_status_changed_callback_(pInfo);
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
	ESteamNetworkingConnectionState state_ = k_ESteamNetworkingConnectionState_None;
	std::function<void(ESteamNetworkingSocketsDebugOutputType, const char*)> logging_callback_;
	std::function<void(SteamNetConnectionStatusChangedCallback_t*)> on_connection_status_changed_callback_;
	SteamNetworkingMicroseconds init_timestamp_;

	//static inline ILogger* logger_;
	//static inline IBML* bml_;
	static inline client* this_instance_;
};