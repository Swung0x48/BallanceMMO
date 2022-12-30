#ifndef BALLANCEMMOSERVER_PUBLIC_WARNING_MSG_HPP
#define BALLANCEMMOSERVER_PUBLIC_WARNING_MSG_HPP
#include "message.hpp"
#include "plain_text_msg.hpp"

namespace bmmo {
    struct public_warning_msg: public plain_text_msg {
        public_warning_msg() {
            this->code = bmmo::PublicWarning;
        }
    };
}

#endif //BALLANCEMMOSERVER_PUBLIC_WARNING_MSG_HPP
