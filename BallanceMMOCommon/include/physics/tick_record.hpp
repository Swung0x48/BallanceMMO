#pragma once

// Determinism harness record format (shared by the client recorder and the
// headless replay tool).  Little-endian, fixed layout.
//
//   header:  magic "BMRC", version u32, tick_rate f64, level i32,
//            physics_sha256[64] (hex, NUL padded), reserved[32]
//   frames:  { keys[256] u8, hash u64, ivp_time f64, cores i32, flags u32 }
//
// Frame 0 is the first tick at which the retail Gameplay_Ingame script is
// active (the level has started).  keys is the DirectInput keyboard state the
// game observed during that tick (0x80 = pressed).

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace bmmo::physics {
    struct tick_record_header {
        char magic[4] = {'B', 'M', 'R', 'C'};
        uint32_t version = 1;
        double tick_rate = 66.0;
        int32_t level = 0;
        char physics_sha256[64] = {};
        char reserved[32] = {};
    };
    static_assert(sizeof(tick_record_header) == 120);  // 116 bytes + tail padding to 8

    struct tick_record_frame {
        std::array<uint8_t, 256> keys{};
        uint64_t hash = 0;
        double ivp_time = 0.0;
        int32_t cores = 0;
        uint32_t flags = 0;
    };
    static_assert(sizeof(tick_record_frame) == 256 + 8 + 8 + 4 + 4);

    class tick_record_writer {
    public:
        bool open(const std::string& path, const tick_record_header& header) {
            stream_.open(path, std::ios::binary | std::ios::trunc);
            if (!stream_) return false;
            stream_.write(reinterpret_cast<const char*>(&header), sizeof(header));
            return static_cast<bool>(stream_);
        }
        bool write(const tick_record_frame& frame) {
            if (!stream_) return false;
            stream_.write(reinterpret_cast<const char*>(&frame), sizeof(frame));
            ++frames_;
            return static_cast<bool>(stream_);
        }
        void close() { stream_.close(); }
        bool is_open() const { return stream_.is_open(); }
        uint64_t frames() const { return frames_; }

    private:
        std::ofstream stream_;
        uint64_t frames_ = 0;
    };

    struct tick_record {
        tick_record_header header;
        std::vector<tick_record_frame> frames;

        bool load(const std::string& path, std::string& error) {
            std::ifstream stream(path, std::ios::binary);
            if (!stream) {
                error = "cannot open " + path;
                return false;
            }
            stream.read(reinterpret_cast<char*>(&header), sizeof(header));
            if (!stream || std::memcmp(header.magic, "BMRC", 4) != 0 || header.version != 1) {
                error = "not a BMRC v1 record: " + path;
                return false;
            }
            frames.clear();
            tick_record_frame frame;
            while (stream.read(reinterpret_cast<char*>(&frame), sizeof(frame)))
                frames.push_back(frame);
            return true;
        }
    };
}
