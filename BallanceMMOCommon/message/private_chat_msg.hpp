#ifndef BALLANCEMMOSERVER_PRIVATE_CHAT_MSG_HPP
#define BALLANCEMMOSERVER_PRIVATE_CHAT_MSG_HPP
#include "message.hpp"
#include "chat_msg.hpp"

namespace bmmo {
    // chat_msg::player_id: we're using this as the receiver when
    // sending from the client; then we swap it with the sender's
    // own id and send it back to our receiver.
    struct private_chat_msg: public chat_msg {
        private_chat_msg() {
            this->code = bmmo::PrivateChat;
        }
    };
}

#endif //BALLANCEMMOSERVER_PRIVATE_CHAT_MSG_HPP
