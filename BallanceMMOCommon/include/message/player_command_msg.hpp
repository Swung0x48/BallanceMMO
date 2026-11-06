#ifndef BALLANCEMMOSERVER_PLAYER_COMMAND_MSG_HPP
#define BALLANCEMMOSERVER_PLAYER_COMMAND_MSG_HPP
#include "message.hpp"
#include <cstdint>

namespace bmmo {
    // The server console's 'beam', 'sector' and 'tpsector' for one player: the server asks that
    // player's client, which runs its own automation verb (in a physics session stamped and sent
    // like a checkpoint or a beam, so every world applies it at the same tick).  The server's
    // approval itself: no operator permission is asked for.
    enum class player_command_kind : uint8_t {
        Beam = 0,            // the own ball to `position`, at rest
        Sector = 1,          // activate `sector`
        TeleportSector = 2,  // activate `sector`, then the own ball to that sector's reset point
    };

    struct player_command {
        float position[3] = {};
        int32_t sector = 0;
        player_command_kind kind = player_command_kind::Beam;
        uint8_t reserved[3] = {};
    };

    typedef struct message<player_command, PlayerCommand> player_command_msg;
    static_assert(sizeof(player_command_msg) == 24, "the wire layout: opcode, three floats, sector, kind, 3 reserved");
}

#endif //BALLANCEMMOSERVER_PLAYER_COMMAND_MSG_HPP
