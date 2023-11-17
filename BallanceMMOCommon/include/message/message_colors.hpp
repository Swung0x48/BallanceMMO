#ifndef BALLANCEMMOSERVER_MSG_COLORS_HPP
#define BALLANCEMMOSERVER_MSG_COLORS_HPP
#include "message.hpp"
#include "../utility/ansi_colors.hpp"

namespace bmmo {
    inline int color_code(opcode msg_code) {
        using a = bmmo::ansi;
        switch (msg_code) {
            case PlayerConnected:
            case PlayerConnectedV2:
            case LoginAccepted:
            case LoginAcceptedV2:
            case LoginAcceptedV3:
                return a::BrightYellow;
            case PlayerDisconnected:
                return a::BrightYellow | a::Underline;
            case PrivateChat:
                return a::Xterm256 | 248;
            case PermanentNotification:
            case PopupBox:
            case ImportantNotification:
                return a::BrightCyan;
            case ActionDenied:
                return a::Red;
            case SoundData:
            case SoundStream:
            case CurrentMap:
            case ScoreList:
                return a::Italic;
            case CheatToggle:
            case OwnedCheatToggle:
                return a::BrightBlue;
            case OpState:
            case PlayerKicked:
                return a::WhiteInverse;
            case KickRequest:
                return a::Italic;
            case Countdown:
                return a::BrightGreen | a::Bold;
            default:
                break;
        }
        return ansi::Reset;
    }
}

#endif //BALLANCEMMOSERVER_MSG_COLORS_HPP
