#pragma once
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif

#include <BML/BMLAll.h> // Just for logging purposes
#include <memory>

class client
{
public:
	client(ILogger* logger) {
		logger_ = logger;

		SteamDatagramErrMsg msg;
		if (!GameNetworkingSockets_Init(nullptr, msg)) {
			logger_->Error("GNS init failed: %s", msg);
		}

		init_timestamp_ = SteamNetworkingUtils()->GetLocalTimestamp();
		SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Msg, DebugOutput);
	}

	void connect(SteamNetworkingIPAddr address) {
		server_address_ = address;
		char szAddr[SteamNetworkingIPAddr::k_cchMaxString];
		server_address_.ToString(szAddr, sizeof(szAddr), true);

		logger_->Info("Connecting to server at ", szAddr);
		interface_ = SteamNetworkingSockets();
		SteamNetworkingConfigValue_t opt;
		opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)OnConnectionStatusChangedCallback);
	}

private:
	void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo) {
		
	}

	static void DebugOutput(ESteamNetworkingSocketsDebugOutputType eType, const char* pszMsg) {
		const char* fmt_string = "[%d] %10.6f %s\n";
		SteamNetworkingMicroseconds time = SteamNetworkingUtils()->GetLocalTimestamp() - init_timestamp_;
		switch (eType) {
			case k_ESteamNetworkingSocketsDebugOutputType_Bug:
			case k_ESteamNetworkingSocketsDebugOutputType_Error:
				logger_->Error(fmt_string, eType, time * 1e-6, pszMsg);
				break;
			case k_ESteamNetworkingSocketsDebugOutputType_Important:
			case k_ESteamNetworkingSocketsDebugOutputType_Warning:
				logger_->Warn(fmt_string, eType, time * 1e-6, pszMsg);
				break;
			default:
				logger_->Info(fmt_string, eType, time * 1e-6, pszMsg);
		}

		if (eType != k_ESteamNetworkingSocketsDebugOutputType_Bug) {
			logger_->Error("We've encountered a bug. Please contact developer with this piece of log.");
			logger_->Error("Nuking process...");
			exit(1);
		}
	}

	static void OnConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* pInfo) {
		this_instance_->OnConnectionStatusChanged(pInfo);
	}

private:
	static SteamNetworkingMicroseconds init_timestamp_;
	static ILogger* logger_;
	static inline client* this_instance_;

	SteamNetworkingIPAddr server_address_;
	ISteamNetworkingSockets* interface_;
};

