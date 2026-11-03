#pragma once

#include "message_utils.hpp"
#include "login_accepted_v3_msg.hpp"

namespace bmmo {
    struct login_server_info {
        std::string version;
        bool physics_available = false;
        std::string physics_unavailable_reason;
        uint32_t physics_protocol = 0;
        bool rooms_enabled = false;
        uint32_t maximum_members = 0;
        uint32_t maximum_physics_rooms = 0;
        bool trafo_inputs = false;
        bool placement_clones = false;
    };

    struct login_accepted_v4_msg: public login_accepted_v3_msg {
        login_server_info server_info;

        login_accepted_v4_msg() { code = LoginAcceptedV4; }

        bool serialize() override {
            if (!login_accepted_v3_msg::serialize()) return false;
            message_utils::write_string(server_info.version, raw);
            raw.write(reinterpret_cast<const char*>(&server_info.physics_available), sizeof(server_info.physics_available));
            message_utils::write_string(server_info.physics_unavailable_reason, raw);
            raw.write(reinterpret_cast<const char*>(&server_info.physics_protocol), sizeof(server_info.physics_protocol));
            raw.write(reinterpret_cast<const char*>(&server_info.rooms_enabled), sizeof(server_info.rooms_enabled));
            raw.write(reinterpret_cast<const char*>(&server_info.maximum_members), sizeof(server_info.maximum_members));
            raw.write(reinterpret_cast<const char*>(&server_info.maximum_physics_rooms), sizeof(server_info.maximum_physics_rooms));
            raw.write(reinterpret_cast<const char*>(&server_info.trafo_inputs), sizeof(server_info.trafo_inputs));
            raw.write(reinterpret_cast<const char*>(&server_info.placement_clones), sizeof(server_info.placement_clones));
            return raw.good();
        }

        bool deserialize() override {
            if (!login_accepted_v3_msg::deserialize()) return false;
            if (!message_utils::read_string(raw, server_info.version)) return false;
            raw.read(reinterpret_cast<char*>(&server_info.physics_available), sizeof(server_info.physics_available));
            if (!message_utils::read_string(raw, server_info.physics_unavailable_reason)) return false;
            raw.read(reinterpret_cast<char*>(&server_info.physics_protocol), sizeof(server_info.physics_protocol));
            raw.read(reinterpret_cast<char*>(&server_info.rooms_enabled), sizeof(server_info.rooms_enabled));
            raw.read(reinterpret_cast<char*>(&server_info.maximum_members), sizeof(server_info.maximum_members));
            raw.read(reinterpret_cast<char*>(&server_info.maximum_physics_rooms), sizeof(server_info.maximum_physics_rooms));
            raw.read(reinterpret_cast<char*>(&server_info.trafo_inputs), sizeof(server_info.trafo_inputs));
            raw.read(reinterpret_cast<char*>(&server_info.placement_clones), sizeof(server_info.placement_clones));
            return raw.good();
        }
    };
}
