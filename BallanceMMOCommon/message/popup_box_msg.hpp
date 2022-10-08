#ifndef BALLANCEMMOSERVER_POPUP_BOX_MSG_HPP
#define BALLANCEMMOSERVER_POPUP_BOX_MSG_HPP
#include "message.hpp"
#include "message_utils.hpp"

namespace bmmo {
    struct popup_box_msg: public serializable_message {
        popup_box_msg(): serializable_message(bmmo::PopupBox) {}

        std::string title;
        std::string text_content;

        bool serialize() override {
            if (!serializable_message::serialize()) return false;

            message_utils::write_string(title, raw);
            message_utils::write_string(text_content, raw);
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize()) return false;

            if (!message_utils::read_string(raw, title))
                return false;
            if (!message_utils::read_string(raw, text_content))
                return false;
            return raw.good();
        }
    };
}

#endif //BALLANCEMMOSERVER_POPUP_BOX_MSG_HPP
