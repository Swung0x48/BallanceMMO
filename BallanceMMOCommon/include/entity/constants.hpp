#ifndef BALLANCEMMOSERVER_CONSTANTS_HPP
#define BALLANCEMMOSERVER_CONSTANTS_HPP
#include <steam/steamnetworkingtypes.h>
#include <chrono>
#include "globals.hpp"

namespace bmmo {
    constexpr std::chrono::nanoseconds SERVER_TICK_DELAY{(int)1e9 / 198},
                                       SERVER_RECEIVE_INTERVAL{(int)1e9 / 66},
                                       CLIENT_RECEIVE_INTERVAL{(int)1e9 / 66};

	constexpr SteamNetworkingMicroseconds CLIENT_MINIMUM_UPDATE_INTERVAL_US = (int64_t)1e6 / 66;

    constexpr float LEVEL_RESTART_IGNORE_TIMEFRAME_MS = 15e3f;

    constexpr const char* RECORD_HEADER = "BallanceMMO FlightRecorder";

    constexpr const uint16_t DEFAULT_PORT = 26676;

    constexpr const int PING_INTERVAL_TICKS = 768;

    const PATH_STRING CLIENT_EXTERNAL_CONFIG_NAME = BMMO_PATH_LITERAL("BallanceMMOClient_external.json");

    // - 0~99: denied at joining
    // -- 0~49: denied from incorrect configuration
    // -- 50~99: banned
    // - 100~199: kicked when online
    // -- 150+n: auto reconnect in n seconds
    struct connection_end {
        // Not enum class: we need to use them as plain integers
        enum code {
            None = 0,

            LoginDenied_Min = k_ESteamNetConnectionEnd_App_Min,
            OutdatedClient, ExistingName, InvalidNameLength,
            InvalidNameCharacter, ReservedName,

            Banned_Min = LoginDenied_Min + 50,
            Banned = Banned_Min,

            LoginDenied_Max = LoginDenied_Min + 100,

            PlayerKicked_Min = k_ESteamNetConnectionEnd_App_Min + 100,
            Kicked, Crash, FatalError, SelfTriggeredFatalError,
            PlayerKicked_Max = PlayerKicked_Min + 10,

            AutoReconnection_Min = PlayerKicked_Min + 50,
            AutoReconnection_Max = AutoReconnection_Min + 50,
        };
    };
}

#endif //BALLANCEMMOSERVER_CONSTANTS_HPP
