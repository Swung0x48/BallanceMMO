#ifndef BALLANCEMMOSERVER_SESSION_WORLD_FORK_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_WORLD_FORK_MSG_HPP
#include "message.hpp"
#include "message_utils.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace bmmo {
    // server -> client, reliable (protocol 42, fork detection step FD1,
    // the fork detection design, 14.2): an episode of the
    // member's world differing from the server's - opened (the client answers
    // with a DETAIL SessionWorldReport), decided (the server's would-heal tier
    // and its summary of the difference) and closed.  Report only: in FD1
    // nothing heals.  The enumerations are session/world_check.hpp's.
    //
    //   schema u8, session u32, episode u32, tick u32, action u8, kinds u8,
    //   u8 n x owner u32, buckets u8, tier u8, rule u8, tick2 u32, outcome u8,
    //   differing u16, worst_m f32, own_m f32, worst str16
    struct session_world_fork_msg : public serializable_message {
        static constexpr uint8_t kSchema = 1;

        uint32_t session = 0;
        uint32_t episode = 0;
        uint32_t tick = 0;             // the episode's first differing sample T
        uint8_t action = 0;            // world_check::fork_action
        uint8_t kinds = 0;             // world_check::KIND_*
        std::vector<uint32_t> owners;  // the differing balls
        uint8_t buckets = 0;           // the differing mechanism buckets
        uint8_t tier = 0;              // decided: world_check::fork_tier
        uint8_t rule = 0;              // decided: world_check::fork_rule
        uint32_t tick2 = 0;            // decided: the would-heal tick; closed: the sample that closed it
        uint8_t outcome = 0;           // closed: world_check::fork_outcome
        uint16_t differing = 0;        // decided: rows differing
        float worst_m = 0.0f;          // decided: the worst body's position error
        float own_m = 0.0f;            // decided: the member's own ball's
        std::string worst;             // decided: the worst body, a mechanism's name or "p<owner>"

        session_world_fork_msg() : serializable_message(bmmo::SessionWorldFork) {}

        bool serialize() override {
            serializable_message::serialize();
            auto put = [this](const auto& v) { raw.write(reinterpret_cast<const char*>(&v), sizeof(v)); };
            const uint8_t n = static_cast<uint8_t>(std::min<size_t>(owners.size(), 0xFF));
            put(kSchema); put(session); put(episode); put(tick); put(action); put(kinds); put(n);
            for (size_t i = 0; i < n; ++i) put(owners[i]);
            put(buckets); put(tier); put(rule); put(tick2); put(outcome); put(differing); put(worst_m); put(own_m);
            message_utils::write_string<uint16_t>(worst, raw);
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize()) return false;
            auto get = [this](auto& v) { return message_utils::read_variable(raw, &v); };
            uint8_t schema = 0, n = 0;
            if (!get(schema) || schema != kSchema) return false;
            if (!(get(session) && get(episode) && get(tick) && get(action) && get(kinds) && get(n))) return false;
            if (static_cast<size_t>(message_utils::bytes_remaining(raw)) < n * 4u) return false;
            owners.resize(n);
            for (uint32_t& o: owners) if (!get(o)) return false;
            return get(buckets) && get(tier) && get(rule) && get(tick2) && get(outcome) && get(differing) && get(worst_m)
                && get(own_m) && message_utils::read_string<uint16_t>(raw, worst);
        }
    };
}

#endif //BALLANCEMMOSERVER_SESSION_WORLD_FORK_MSG_HPP
