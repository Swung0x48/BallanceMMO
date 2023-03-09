#ifndef BALLANCEMMOSERVER_CONSTANTS_HPP
#define BALLANCEMMOSERVER_CONSTANTS_HPP
#include <steam/steamnetworkingtypes.h>
#include <chrono>

namespace bmmo {
    constexpr std::chrono::nanoseconds SERVER_TICK_DELAY{(int)1e9 / 198},
                                       SERVER_RECEIVE_INTERVAL{(int)1e9 / 66},
                                       CLIENT_RECEIVE_INTERVAL{(int)1e9 / 66};

	constexpr SteamNetworkingMicroseconds CLIENT_MINIMUM_UPDATE_INTERVAL_MS = (int64_t)1e6 / 66;

    constexpr const char* RECORD_HEADER = "BallanceMMO FlightRecorder";

    constexpr const uint16_t DEFAULT_PORT = 26676;

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
            OutdatedClient, ExistingName, InvalidNameLength, InvalidNameCharacter,

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
