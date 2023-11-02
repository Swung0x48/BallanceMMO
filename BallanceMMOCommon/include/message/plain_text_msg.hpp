#ifndef BALLANCEMMOSERVER_PLAIN_TEXT_MSG_HPP
#define BALLANCEMMOSERVER_PLAIN_TEXT_MSG_HPP
#include "message.hpp"
#include "message_utils.hpp"

namespace bmmo {
    struct plain_text_msg: public serializable_message {
        plain_text_msg(): serializable_message(bmmo::PlainText) {}

        std::string text_content;

        bool serialize() override {
            if (!serializable_message::serialize()) return false;

            message_utils::write_string(text_content, raw);
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize()) return false;

            if (!message_utils::read_string(raw, text_content))
                return false;
            return raw.good();
        }
    };
}

#endif //BALLANCEMMOSERVER_PLAIN_TEXT_MSG_HPP
