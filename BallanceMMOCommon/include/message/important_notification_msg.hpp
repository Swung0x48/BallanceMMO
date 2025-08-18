#ifndef BALLANCEMMOSERVER_IMPORTANT_NOTIFICATION_MSG_HPP
#define BALLANCEMMOSERVER_IMPORTANT_NOTIFICATION_MSG_HPP
#include "message.hpp"
#include "chat_msg.hpp"
#include "../utility/ansi_colors.hpp"

namespace bmmo {
    struct important_notification_msg: public chat_msg {
        constexpr static const uint8_t PLAIN_MSG_SHIFT = 1 << 6; // 64
        enum notification_type: uint8_t {
            Announcement, Notice,
            PlainAnnouncement = Announcement + PLAIN_MSG_SHIFT, PlainNotice,
        } ;
        notification_type type = Announcement;

        constexpr const char* get_type_name() {
            switch (type >= PlainAnnouncement ? type - PLAIN_MSG_SHIFT : type) {
                case Announcement: return "Announcement";
                default: return "Notice";
            }
        }
        
        constexpr int get_ansi_color() {
            switch (type >= PlainAnnouncement ? type - PLAIN_MSG_SHIFT : type) {
                case Announcement: return bmmo::ansi::BrightCyan | bmmo::ansi::Bold;
                default: return bmmo::ansi::BrightCyan;
            }
        }

        important_notification_msg() {
            this->code = bmmo::ImportantNotification;
        }

        bool serialize() override {
            if (!chat_msg::serialize()) return false;
            message_utils::write_variable(&type, raw);
            return raw.good();
        }

        bool deserialize() override {
            if (!chat_msg::deserialize()) return false;
            if (!message_utils::read_variable(raw, &type)) return false;
            return raw.good();
        }
    };
}

#endif //BALLANCEMMOSERVER_IMPORTANT_NOTIFICATION_MSG_HPP
