#ifndef BALLANCEMMOSERVER_OWNED_BALL_STATE_MSG_HPP
#define BALLANCEMMOSERVER_OWNED_BALL_STATE_MSG_HPP

namespace bmmo {
    struct owned_ball_state {
        ball_state state{};
        HSteamNetConnection player_id = k_HSteamNetConnection_Invalid;
    };

    typedef struct message<owned_ball_state, OwnedBallState> owned_ball_state_msg;
}

#endif //BALLANCEMMOSERVER_OWNED_BALL_STATE_MSG_HPP
