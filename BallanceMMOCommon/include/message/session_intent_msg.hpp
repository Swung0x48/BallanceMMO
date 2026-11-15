#ifndef BALLANCEMMOSERVER_SESSION_INTENT_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_INTENT_MSG_HPP
#include "message.hpp"
#include "message_utils.hpp"

namespace bmmo {
    // client -> server, reliable (protocol 41, step INT): one player intent
    // (session/intent.hpp) as the member journaled it - the session tick it
    // applies at, the live tick a BMMO trigger asked at (INTENT_NONE for a
    // menu), the member's life, the kind and source, and the source's
    // argument (the requester's or countdown sender's connection, or
    // INTENT_NONE).  The server journals it (member_intent) and nothing
    // else: it is not relayed, acknowledged or applied to any world.
    struct session_intent_msg : public serializable_message {
        static constexpr uint8_t kSchema = 1;

        uint32_t session = 0;
        uint32_t tick = 0;
        uint32_t request = 0;
        uint32_t arg = 0;
        uint16_t life = 0;
        uint8_t kind = 0;     // session::intent_kind
        uint8_t source = 0;   // session::intent_source

        session_intent_msg() : serializable_message(bmmo::SessionIntent) {}

        bool serialize() override {
            serializable_message::serialize();
            const uint8_t schema = kSchema;
            auto put = [this](const auto& v) { raw.write(reinterpret_cast<const char*>(&v), sizeof(v)); };
            put(schema); put(session); put(tick); put(request); put(arg); put(life); put(kind); put(source);
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize()) return false;
            uint8_t schema = 0;
            auto get = [this](auto& v) { return message_utils::read_variable(raw, &v); };
            if (!get(schema) || schema != kSchema) return false;
            return get(session) && get(tick) && get(request) && get(arg) && get(life) && get(kind) && get(source);
        }
    };
}

#endif //BALLANCEMMOSERVER_SESSION_INTENT_MSG_HPP
