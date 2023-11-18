#ifndef BALLANCEMMOSERVER_OWNED_COMPRESSED_BALL_STATE_MSG
#define BALLANCEMMOSERVER_OWNED_COMPRESSED_BALL_STATE_MSG
#include "message.hpp"
#include "message_utils.hpp"
#include "owned_timed_ball_state_msg.hpp"
#include <vector>
#include <type_traits>
#include <cmath>
#include <cassert>
#include <numbers>

namespace bmmo {
    struct owned_compressed_ball_state_msg: public serializable_message {
        std::vector<owned_timed_ball_state> balls;
        std::vector<owned_timestamp> unchanged_balls;

        owned_compressed_ball_state_msg(): serializable_message(OwnedCompressedBallState) {}

        struct compressed_flag {
            enum flag_t : uint8_t {
                BallSwitched = 1 << 0,
                Quat_XOmit = 0 << 1,
                Quat_YOmit = 1 << 1,
                Quat_ZOmit = 2 << 1,
                Quat_WOmit = 3 << 1,
            };
            static constexpr int used_bits = 3;
        };

        // [-0.7, 0.7]
        // 256 8bit, 0.7 * 2 / 256 = 0.0027 * 2, 1 * 2 / 256 = 0.0039 * 2

        static constexpr int ROTATION_LENGTH = sizeof(quaternion::v) / sizeof(std::remove_all_extents_t<decltype(quaternion::v)>);
        static constexpr int ROTATION_BIT_LENGTH = 9;
        static constexpr int ROTATION_COUNT = 512;
        static constexpr float ROTATION_STEP = std::numbers::sqrt2_v<float> / (ROTATION_COUNT - 2);
        static constexpr int TIMESTAMP_SIZE = 6;

        struct compressed_bitfield {
            signed int flag: compressed_flag::used_bits,
                rotation0: ROTATION_BIT_LENGTH, rotation1: ROTATION_BIT_LENGTH, rotation2: ROTATION_BIT_LENGTH;
        };

        bool serialize() override {
            serializable_message::serialize();

            auto size = (uint16_t)balls.size();
            raw.write(reinterpret_cast<const char*>(&size), sizeof(size));
            for (const auto& ball: balls) {
                const uint8_t type = ball.state.type;
                raw.write(reinterpret_cast<const char*>(&type), sizeof(type));
                raw.write(reinterpret_cast<const char*>(&ball.state.position), sizeof(ball.state.position));

                // Compress rotation
                int max_value_index = 0;
                float max_value = std::abs(ball.state.rotation.v[0]);
                for (int i = 1; i < ROTATION_LENGTH; ++i) {
                    float f = std::abs(ball.state.rotation.v[i]);
                    if (f > max_value) {
                        max_value_index = i;
                        max_value = f;
                    }
                }
                assert(max_value_index >= 0 && max_value_index < ROTATION_LENGTH);
                const bool sign_inverted = (ball.state.rotation.v[max_value_index] < 0);

                compressed_bitfield bits;
                static_assert(sizeof(bits) == 4);

                bits.flag = static_cast<std::underlying_type_t<compressed_flag::flag_t>>(max_value_index << 1);

                // TODO: Implement ball trafo compression
                // flag |= compressed_flag::BallSwitched; // Short circuit for now 

                int compressed_rotation[ROTATION_LENGTH - 1], i = 0;
                for (auto& v: compressed_rotation) {
                    if (i == max_value_index) ++i;
                    v = int(((sign_inverted) ? -1 : 1) * std::round(ball.state.rotation.v[i] / ROTATION_STEP));
                    ++i;
                }
                bits.rotation0 = compressed_rotation[0];
                bits.rotation1 = compressed_rotation[1];
                bits.rotation2 = compressed_rotation[2];

                raw.write(reinterpret_cast<const char*>(&bits), sizeof(bits));

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
                
                compressed_bitfield bits;
                raw.read(reinterpret_cast<char*>(&bits), sizeof(bits));
                if (!raw.good() || raw.gcount() != sizeof(bits))
                    return false;
                int max_value_index = (bits.flag >> 1) & 0b11;
                float max_value_squared = 1;

                int compressed_rotation[] = {bits.rotation0, bits.rotation1, bits.rotation2}, i = 0;
                for (const int v: compressed_rotation) {
                    if (i == max_value_index) ++i;
                    ball.state.rotation.v[i] = v * ROTATION_STEP;
                    max_value_squared -= ball.state.rotation.v[i] * ball.state.rotation.v[i];
                    ++i;
                }
                ball.state.rotation.v[max_value_index] = std::sqrt(max_value_squared);

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
