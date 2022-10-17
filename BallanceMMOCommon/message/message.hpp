#ifndef BALLANCEMMOSERVER_PACKET_HPP
#define BALLANCEMMOSERVER_PACKET_HPP

#include "../entity/entity.hpp"
#include <steam/steamnetworkingtypes.h>
#include <vector>
#include <unordered_map>
#include <sstream>

namespace bmmo {
    enum opcode : uint32_t {
        None,
        LoginRequest,
        LoginAccepted,
        SimpleAction,
        PlayerDisconnected,
        PlayerConnected,

        Ping,
        BallState,
        OwnedBallState,
        KeyboardInput,

        Chat,

        LevelFinish,

        LoginRequestV2,
        LoginAcceptedV2,
        PlayerConnectedV2,

        CheatState,
        OwnedCheatState,
        CheatToggle,
        OwnedCheatToggle,

        KickRequest,
        PlayerKicked,

        OwnedBallStateV2,
        LoginRequestV3,
        LevelFinishV2,

        ActionDenied,
        OpState,

        Countdown,
        DidNotFinish,

        MapNames,
        PlainText,
        CurrentMap,
        HashData,

        TimedBallState,
        OwnedTimedBallState,
        Timestamp,

        PrivateChat,
        PlayerReady,
        ImportantNotification,
        ModList,
        PopupBox,
    };

    template<typename T, opcode C = None>
    struct message {
        opcode code = C;
        T content;
    };

    // DO NOT ALLOCATE THIS! Only for typing pointers
    typedef struct message<uint8_t[k_cbMaxSteamNetworkingSocketsMessageSizeSend - sizeof(opcode)]> general_message;

    struct serializable_message {
        explicit serializable_message(opcode code): code(code) {}

        opcode code;
        std::stringstream raw;

        size_t size() {
            // return raw.str().size();
            return static_cast<size_t>(raw.tellp()); // why this is non-const!!!
        }

        inline auto data() noexcept {
            return raw.str().data();
        }

        virtual void clear() {
            std::stringstream temp;
            raw.swap(temp);
        }

        // entity -> raw
        virtual bool serialize() {
            raw.write(reinterpret_cast<const char*>(&code), sizeof(opcode));
            return true;
        }

        // raw -> entity
        virtual bool deserialize() {
            opcode c;
            raw.read(reinterpret_cast<char*>(&c), sizeof(opcode));
            return c == code;
        }
    };
}

#endif //BALLANCEMMOSERVER_PACKET_HPP
