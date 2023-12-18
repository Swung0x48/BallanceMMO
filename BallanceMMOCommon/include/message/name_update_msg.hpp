#ifndef BALLANCEMMOSERVER_NAME_UPDATE_MSG_HPP
#define BALLANCEMMOSERVER_NAME_UPDATE_MSG_HPP
#include "message.hpp"
#include "message_utils.hpp"
#include "plain_text_msg.hpp"

namespace bmmo {
    struct name_update_msg: public plain_text_msg {
        name_update_msg() {
            this->code = bmmo::NameUpdate;
        }
    };
}

#endif //BALLANCEMMOSERVER_NAME_UPDATE_MSG_HPP
