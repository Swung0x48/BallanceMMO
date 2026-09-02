#ifndef BALLANCEMMOSERVER_SESSION_EVENT_MSG_HPP
#define BALLANCEMMOSERVER_SESSION_EVENT_MSG_HPP
#include "message.hpp"
#include "../entity/session.hpp"

namespace bmmo {
    // Bidirectional, reliable. A single session lifecycle event reported by a
    // client (player == 0 when sent) and relayed by the server (player set to
    // the origin). docs/rooms-and-sessions-protocol.md 2.2.
    struct session_event_msg : public serializable_message {
        uint32_t session = 0;
        uint32_t player = 0;   // origin when forwarded by the server, 0 when sent by a client
        uint32_t tick = 0;
        session::event_type type = session::event_type::Physicalize;

        // Union of optional payloads; serialize()/deserialize() only put the
        // fields the current `type` needs on the wire.
        uint8_t ball_type = 0;              // Physicalize
        float position[3] = {};             // Physicalize
        float rotation[9] = {};             // Physicalize: world matrix rows (right, up, dir)
        session::ball_recipe recipe;        // Physicalize
        int32_t sector = 0;                 // Sector
        std::string name;                   // BodyRevived, <= MAX_NAME

        session_event_msg() : serializable_message(bmmo::SessionEvent) {}

        static void write_recipe(const session::ball_recipe& r, std::stringstream& raw) {
            const uint8_t fixed = r.fixed ? 1 : 0;
            const uint8_t start_frozen = r.start_frozen ? 1 : 0;
            const uint8_t enable_collision = r.enable_collision ? 1 : 0;
            const uint8_t calc_mass_center = r.calc_mass_center ? 1 : 0;
            raw.write(reinterpret_cast<const char*>(&fixed), sizeof(fixed));
            raw.write(reinterpret_cast<const char*>(&r.friction), sizeof(r.friction));
            raw.write(reinterpret_cast<const char*>(&r.elasticity), sizeof(r.elasticity));
            raw.write(reinterpret_cast<const char*>(&r.mass), sizeof(r.mass));
            raw.write(reinterpret_cast<const char*>(&start_frozen), sizeof(start_frozen));
            raw.write(reinterpret_cast<const char*>(&enable_collision), sizeof(enable_collision));
            raw.write(reinterpret_cast<const char*>(&calc_mass_center), sizeof(calc_mass_center));
            raw.write(reinterpret_cast<const char*>(&r.linear_damp), sizeof(r.linear_damp));
            raw.write(reinterpret_cast<const char*>(&r.rot_damp), sizeof(r.rot_damp));
            raw.write(reinterpret_cast<const char*>(r.mass_center), sizeof(r.mass_center));
            message_utils::write_string<uint16_t>(r.collision_surface.substr(0, session::MAX_NAME), raw);

            const uint8_t convex_count = static_cast<uint8_t>(std::min<size_t>(r.convex_meshes.size(), session::MAX_CONVEX));
            raw.write(reinterpret_cast<const char*>(&convex_count), sizeof(convex_count));
            for (uint8_t i = 0; i < convex_count; ++i)
                message_utils::write_string<uint16_t>(r.convex_meshes[i].substr(0, session::MAX_NAME), raw);

            const uint8_t ball_count = static_cast<uint8_t>(std::min<size_t>(r.balls.size(), session::MAX_CONVEX));
            raw.write(reinterpret_cast<const char*>(&ball_count), sizeof(ball_count));
            for (uint8_t i = 0; i < ball_count; ++i) {
                const auto& b = r.balls[i];
                raw.write(reinterpret_cast<const char*>(b.center), sizeof(b.center));
                raw.write(reinterpret_cast<const char*>(&b.radius), sizeof(b.radius));
            }

            const uint8_t concave_count = static_cast<uint8_t>(std::min<size_t>(r.concave_meshes.size(), session::MAX_CONVEX));
            raw.write(reinterpret_cast<const char*>(&concave_count), sizeof(concave_count));
            for (uint8_t i = 0; i < concave_count; ++i)
                message_utils::write_string<uint16_t>(r.concave_meshes[i].substr(0, session::MAX_NAME), raw);
        }

        static bool read_recipe(session::ball_recipe& r, std::stringstream& raw) {
            uint8_t fixed = 0, start_frozen = 0, enable_collision = 0, calc_mass_center = 0;
            if (!message_utils::read_variable(raw, &fixed)) return false;
            if (!message_utils::read_variable(raw, &r.friction)) return false;
            if (!message_utils::read_variable(raw, &r.elasticity)) return false;
            if (!message_utils::read_variable(raw, &r.mass)) return false;
            if (!message_utils::read_variable(raw, &start_frozen)) return false;
            if (!message_utils::read_variable(raw, &enable_collision)) return false;
            if (!message_utils::read_variable(raw, &calc_mass_center)) return false;
            if (!message_utils::read_variable(raw, &r.linear_damp)) return false;
            if (!message_utils::read_variable(raw, &r.rot_damp)) return false;
            if (!raw.read(reinterpret_cast<char*>(r.mass_center), sizeof(r.mass_center))) return false;
            if (!message_utils::read_string<uint16_t>(raw, r.collision_surface)) return false;
            if (r.collision_surface.size() > session::MAX_NAME) return false;

            uint8_t convex_count = 0;
            if (!message_utils::read_variable(raw, &convex_count)) return false;
            if (convex_count > session::MAX_CONVEX) return false;
            r.convex_meshes.clear();
            r.convex_meshes.reserve(convex_count);
            for (uint8_t i = 0; i < convex_count; ++i) {
                std::string mesh;
                if (!message_utils::read_string<uint16_t>(raw, mesh)) return false;
                if (mesh.size() > session::MAX_NAME) return false;
                r.convex_meshes.push_back(std::move(mesh));
            }

            uint8_t ball_count = 0;
            if (!message_utils::read_variable(raw, &ball_count)) return false;
            if (ball_count > session::MAX_CONVEX) return false;
            r.balls.clear();
            r.balls.reserve(ball_count);
            for (uint8_t i = 0; i < ball_count; ++i) {
                session::ball_recipe::sphere b;
                if (!raw.read(reinterpret_cast<char*>(b.center), sizeof(b.center))) return false;
                if (!message_utils::read_variable(raw, &b.radius)) return false;
                r.balls.push_back(b);
            }

            uint8_t concave_count = 0;
            if (!message_utils::read_variable(raw, &concave_count)) return false;
            if (concave_count > session::MAX_CONVEX) return false;
            r.concave_meshes.clear();
            r.concave_meshes.reserve(concave_count);
            for (uint8_t i = 0; i < concave_count; ++i) {
                std::string mesh;
                if (!message_utils::read_string<uint16_t>(raw, mesh)) return false;
                if (mesh.size() > session::MAX_NAME) return false;
                r.concave_meshes.push_back(std::move(mesh));
            }

            r.fixed = fixed != 0;
            r.start_frozen = start_frozen != 0;
            r.enable_collision = enable_collision != 0;
            r.calc_mass_center = calc_mass_center != 0;
            return true;
        }

        bool serialize() override {
            serializable_message::serialize();
            raw.write(reinterpret_cast<const char*>(&session), sizeof(session));
            raw.write(reinterpret_cast<const char*>(&player), sizeof(player));
            raw.write(reinterpret_cast<const char*>(&tick), sizeof(tick));
            const auto t = static_cast<uint8_t>(type);
            raw.write(reinterpret_cast<const char*>(&t), sizeof(t));

            switch (type) {
                case session::event_type::Physicalize:
                    raw.write(reinterpret_cast<const char*>(&ball_type), sizeof(ball_type));
                    raw.write(reinterpret_cast<const char*>(position), sizeof(position));
                    raw.write(reinterpret_cast<const char*>(rotation), sizeof(rotation));
                    write_recipe(recipe, raw);
                    break;
                case session::event_type::Unphysicalize:
                case session::event_type::Finish:
                    break;
                case session::event_type::Sector:
                    raw.write(reinterpret_cast<const char*>(&sector), sizeof(sector));
                    break;
                case session::event_type::BodyRevived:
                    message_utils::write_string<uint16_t>(name.substr(0, session::MAX_NAME), raw);
                    break;
            }
            return raw.good();
        }

        bool deserialize() override {
            if (!serializable_message::deserialize())
                return false;
            if (!message_utils::read_variable(raw, &session)) return false;
            if (!message_utils::read_variable(raw, &player)) return false;
            if (!message_utils::read_variable(raw, &tick)) return false;
            uint8_t t = 0;
            if (!message_utils::read_variable(raw, &t)) return false;
            if (t > static_cast<uint8_t>(session::event_type::BodyRevived)) return false;
            type = static_cast<session::event_type>(t);

            switch (type) {
                case session::event_type::Physicalize:
                    if (!message_utils::read_variable(raw, &ball_type)) return false;
                    if (!raw.read(reinterpret_cast<char*>(position), sizeof(position))) return false;
                    if (!raw.read(reinterpret_cast<char*>(rotation), sizeof(rotation))) return false;
                    if (!read_recipe(recipe, raw)) return false;
                    break;
                case session::event_type::Unphysicalize:
                case session::event_type::Finish:
                    break;
                case session::event_type::Sector:
                    if (!message_utils::read_variable(raw, &sector)) return false;
                    break;
                case session::event_type::BodyRevived:
                    if (!message_utils::read_string<uint16_t>(raw, name)) return false;
                    if (name.size() > session::MAX_NAME) return false;
                    break;
            }
            return true;
        }
    };
}

#endif //BALLANCEMMOSERVER_SESSION_EVENT_MSG_HPP
