#ifndef BALLANCEMMOSERVER_PACKET_HPP
#define BALLANCEMMOSERVER_PACKET_HPP

#include "../entity/entity.hpp"
#include <steam/steamnetworkingtypes.h>
#include <vector>
#include <sstream>

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

    template<typename T>
    struct message {
        opcode opcode;
        T content;
    };

    typedef struct message<uint8_t[k_cbMaxSteamNetworkingSocketsMessageSizeSend - sizeof(opcode)]> general_message;

    struct serializable_message {
        explicit serializable_message(opcode code): code(code) {}

        opcode code;
        std::stringstream raw;

        // entity -> raw
        virtual void serialize() {
            raw.write(reinterpret_cast<const char*>(&code), sizeof(opcode));
        }

        // raw -> entity
        virtual void deserialize() {
            opcode c;
            raw.read(reinterpret_cast<char*>(&c), sizeof(opcode));
            assert(c == code);
        }
    };

//    struct general_message {
//         opcode opcode;
//         uint8_t content[k_cbMaxSteamNetworkingSocketsMessageSizeSend - sizeof(opcode)];
//    };

    struct ball_state_msg {
        opcode opcode = BallState;
        ball_state state;
    };
}

#endif //BALLANCEMMOSERVER_PACKET_HPP
