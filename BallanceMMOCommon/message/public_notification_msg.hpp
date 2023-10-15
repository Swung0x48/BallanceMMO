#ifndef BALLANCEMMOSERVER_PUBLIC_NOTIFICATION_MSG_HPP
#define BALLANCEMMOSERVER_PUBLIC_NOTIFICATION_MSG_HPP
#include "message.hpp"
#include "../utility/ansi_colors.hpp"

namespace bmmo {
    enum class public_notification_type: uint8_t { Info, Warning, Error };

    struct public_notification_msg: public serializable_message {
        public_notification_msg(): serializable_message(bmmo::PublicNotification) {}

        public_notification_type type = public_notification_type::Info;
        std::string text_content;

        std::string get_type_name() const {
            using pn = public_notification_type;
            switch (type) {
                case pn::Info: return "Info";
                case pn::Warning: return "Warning";
                case pn::Error: return "Error";
                default: return "Unknown";
            }
        }

        int get_ansi_color_code() const {
            using pn = public_notification_type;
            switch (type) {
                case pn::Info: return ansi::Default;
                case pn::Warning: return ansi::Xterm256 | 216;
                case pn::Error: return ansi::Xterm256 | 202;
                default: return ansi::Reset;
            }
        }

        bool serialize() override {
            if (!serializable_message::serialize()) return false;

            message_utils::write_variable(&type, raw);
            message_utils::write_string(text_content, raw);
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize()) return false;

            if (!message_utils::read_variable(raw, &type))
                return false;
            if (!message_utils::read_string(raw, text_content))
                return false;
            return raw.good();
        }
    };
}

#endif //BALLANCEMMOSERVER_PUBLIC_NOTIFICATION_MSG_HPP
