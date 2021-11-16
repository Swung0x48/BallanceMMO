#ifndef BALLANCEMMOSERVER_PACKET_HPP
#define BALLANCEMMOSERVER_PACKET_HPP

#include "../entity/entity.hpp"
#include <steam/steamnetworkingtypes.h>

namespace bmmo {
    enum opcode : uint32_t {
        LoginRequest,
        LoginAccepted,
        LoginDenied,
        PlayerDisconnect,
        PlayerConnected,

        Ping,
        BallState,
        KeyboardInput
    };

    struct message {
         opcode opcode;
         uint8_t content[k_cbMaxSteamNetworkingSocketsMessageSizeSend - sizeof(opcode)];
    };

    struct ball_state {
        vec3 position;
        quaternion quaternion;
    };

    struct ball_state_msg {
        opcode opcode = BallState;
        ball_state state;
    };
}

#endif //BALLANCEMMOSERVER_PACKET_HPP
