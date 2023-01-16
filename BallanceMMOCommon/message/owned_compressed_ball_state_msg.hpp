#ifndef BALLANCEMMOSERVER_OWNED_COMPRESSED_BALL_STATE_MSG
#define BALLANCEMMOSERVER_OWNED_COMPRESSED_BALL_STATE_MSG
#include "message.hpp"
#include "message_utils.hpp"
#include "owned_timed_ball_state_msg.hpp"
#include <vector>
#include <type_traits>
#include <cmath>
#include <cassert>

namespace bmmo {
    struct compressed_flag {
        enum flag_t : uint8_t {
            BallSwitched = 1 << 0,
            Quat_XOmit = 0 << 1,
            Quat_YOmit = 1 << 1,
            Quat_ZOmit = 2 << 1,
            Quat_WOmit = 3 << 1,
        };
    };

    struct owned_compressed_ball_state_msg: public serializable_message {
        std::vector<owned_timed_ball_state> balls;
        std::vector<owned_timestamp> unchanged_balls;

        // [-0.7, 0.7]
        // 256 8bit, 0.7 * 2 / 256 = 0.0027 * 2, 1 * 2 / 256 = 0.0039 * 2

        static constexpr const int ROTATION_SIZE = sizeof(quaternion::v) / sizeof(std::remove_all_extents_t<decltype(quaternion::v)>);
        static constexpr const float SQRT_2_DIVIDED_BY_2 = 0.7071067811865475f;
        static constexpr const float ROTATION_STEP = SQRT_2_DIVIDED_BY_2 * 2 / (std::numeric_limits<uint8_t>::max() + 1);
        static constexpr const int TIMESTAMP_SIZE = 6;

        owned_compressed_ball_state_msg(): serializable_message(OwnedCompressedBallState) {}

        bool serialize() override {
            serializable_message::serialize();

            auto size = (uint16_t)balls.size();
            raw.write(reinterpret_cast<const char*>(&size), sizeof(size));
            for (const auto& ball: balls) {
                const uint8_t type = ball.state.type;
                raw.write(reinterpret_cast<const char*>(&type), sizeof(type));
                raw.write(reinterpret_cast<const char*>(&ball.state.position), sizeof(ball.state.position));

                // Compress rotation
                int index = 0;
                float max_value = std::abs(ball.state.rotation.v[0]);
                for (int i = 1; i < ROTATION_SIZE; ++i) {
                    float f = std::abs(ball.state.rotation.v[i]);
                    if (f > max_value) {
                        index = i;
                        max_value = f;
                    }
                }
                assert(index >= 0 && index < ROTATION_SIZE);
                const bool sign_inverted = (ball.state.rotation.v[index] < 0);
                auto flag = static_cast<std::underlying_type_t<compressed_flag::flag_t>>(index << 1);

                // TODO: Implement ball trafo compression
                // flag |= compressed_flag::BallSwitched; // Short circuit for now 

                raw.write(reinterpret_cast<const char*>(&flag), sizeof(flag));

                for (int i = 0; i < ROTATION_SIZE; ++i) {
                    if (i == index) continue;
                    auto v = int8_t(((sign_inverted) ? -1 : 1) * std::round(ball.state.rotation.v[i] / ROTATION_STEP));
                    raw.write(reinterpret_cast<const char*>(&v), sizeof(v));
                }

                // Compress timestamp
                const auto timestamp = int64_t(ball.state.timestamp);
                raw.write(reinterpret_cast<const char*>(&timestamp), TIMESTAMP_SIZE);

                raw.write(reinterpret_cast<const char*>(&ball.player_id), sizeof(ball.player_id));
            }

            size = (uint16_t)unchanged_balls.size();
            if (size == 0)
                return raw.good();
            raw.write(reinterpret_cast<const char*>(&size), sizeof(size));
            for (const auto& ball: unchanged_balls) {
                const auto timestamp = int64_t(ball.timestamp);
                raw.write(reinterpret_cast<const char*>(&timestamp), TIMESTAMP_SIZE);
                raw.write(reinterpret_cast<const char*>(&ball.player_id), sizeof(ball.player_id));
            }

            return raw.good();
        }

        bool deserialize() override {
            serializable_message::deserialize();
            
            uint16_t size = 0;
            raw.read(reinterpret_cast<char*>(&size), sizeof(size));
            if (!raw.good() || raw.gcount() != sizeof(size))
                return false;
            balls.resize(size);

            for (auto& ball: balls) {
                raw.read(reinterpret_cast<char*>(&ball.state.type), sizeof(uint8_t));
                if (!raw.good() || raw.gcount() != sizeof(uint8_t))
                    return false;

                raw.read(reinterpret_cast<char*>(&ball.state.position), sizeof(ball.state.position));
                if (!raw.good() || raw.gcount() != sizeof(ball.state.position))
                    return false;
                
                std::underlying_type_t<compressed_flag::flag_t> flag;
                raw.read(reinterpret_cast<char*>(&flag), sizeof(flag));
                if (!raw.good() || raw.gcount() != sizeof(flag))
                    return false;
                int index = (flag >> 1) & 0x3;
                float omitted_squared = 1;
                for (int i = 0; i < ROTATION_SIZE; ++i) {
                    if (i == index) continue;
                    int8_t v;
                    raw.read(reinterpret_cast<char*>(&v), sizeof(v));
                    if (!raw.good() || raw.gcount() != sizeof(v))
                        return false;
                    const float v_uncompressed = v * ROTATION_STEP;
                    omitted_squared -= v_uncompressed * v_uncompressed;
                    ball.state.rotation.v[i] = v_uncompressed;
                }
                ball.state.rotation.v[index] = std::sqrt(omitted_squared);

                int64_t timestamp{};
                raw.read(reinterpret_cast<char*>(&timestamp), TIMESTAMP_SIZE);
                if (!raw.good() || raw.gcount() != TIMESTAMP_SIZE)
                    return false;
                ball.state.timestamp = timestamp;
                raw.read(reinterpret_cast<char*>(&ball.player_id), sizeof(ball.player_id));
                if (!raw.good() || raw.gcount() != sizeof(ball.player_id))
                    return false;
            }

            if (raw.peek() == std::stringstream::traits_type::eof())
                return raw.good();
            raw.read(reinterpret_cast<char*>(&size), sizeof(size));
            if (!raw.good() || raw.gcount() != sizeof(size))
                return false;
            unchanged_balls.resize(size);

            for (auto& ball: unchanged_balls) {
                int64_t timestamp{};
                raw.read(reinterpret_cast<char*>(&timestamp), TIMESTAMP_SIZE);
                if (!raw.good() || raw.gcount() != TIMESTAMP_SIZE)
                    return false;
                ball.timestamp = timestamp;
                raw.read(reinterpret_cast<char*>(&ball.player_id), sizeof(ball.player_id));
                if (!raw.good() || raw.gcount() != sizeof(ball.player_id))
                    return false;
            }

            return raw.good();
        }
    };
}

#endif //BALLANCEMMOSERVER_OWNED_COMPRESSED_BALL_STATE_MSG



// A 0xfffff
// B 0x00001

// (A > B) && (A - B > (0xffff / 2))
// CompressFlag

// _A_________________B___
