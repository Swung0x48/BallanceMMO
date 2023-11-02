#ifndef BALLANCEMMOSERVER_IMPORTANT_NOTIFICATION_MSG_HPP
#define BALLANCEMMOSERVER_IMPORTANT_NOTIFICATION_MSG_HPP
#include "message.hpp"
#include "chat_msg.hpp"

namespace bmmo {
    struct important_notification_msg: public chat_msg {
        important_notification_msg() {
            this->code = bmmo::ImportantNotification;
        }
    };
}

#endif //BALLANCEMMOSERVER_IMPORTANT_NOTIFICATION_MSG_HPP
