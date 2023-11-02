#ifndef BALLANCEMMOSERVER_PERMANENT_NOTIFICATION_MSG_HPP
#define BALLANCEMMOSERVER_PERMANENT_NOTIFICATION_MSG_HPP
#include "message.hpp"
#include "popup_box_msg.hpp"

namespace bmmo {
    struct permanent_notification_msg: public popup_box_msg {
        permanent_notification_msg() {
            this->code = bmmo::PermanentNotification;
        }
    };
}

#endif //BALLANCEMMOSERVER_PERMANENT_NOTIFICATION_MSG_HPP
