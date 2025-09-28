#ifndef BALLANCEMMOSERVER_REMOTE_COMMAND_MSG_HPP
#define BALLANCEMMOSERVER_REMOTE_COMMAND_MSG_HPP
#include "message.hpp"
#include "plain_text_msg.hpp"

namespace bmmo {
    // only available from client -> replayer server at the moment
    // requires superuser status to prevent abuse
    // only success or failure feedback is given, no output of the command
    struct remote_command_msg: public plain_text_msg {
        remote_command_msg() {
            this->code = bmmo::RemoteCommand;
        }
    };
}

#endif //BALLANCEMMOSERVER_REMOTE_COMMAND_MSG_HPP
