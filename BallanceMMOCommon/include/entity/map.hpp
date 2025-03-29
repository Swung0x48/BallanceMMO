#ifndef BALLANCEMMOSERVER_MAP_HPP
#define BALLANCEMMOSERVER_MAP_HPP
#include <cstdint>
#include <vector>
#include <string>
#include <cstring>
#include <unordered_map>
#include "../message/message_utils.hpp"
#include "../utility/string_utils.hpp"

namespace bmmo {
    enum class map_type : uint8_t {
        Unknown,
        OriginalLevel,
        CustomMap
    };

    enum class level_mode : uint8_t {
        Speedrun = 0,
        Highscore = 1,
    };

    inline constexpr const char* get_level_mode_suffix(level_mode mode) {
        switch (mode) {
            case level_mode::Highscore: return " [HS]";
            case level_mode::Speedrun: return " [SR]";
            default: return "";
        }
    }
    
    inline constexpr const char* get_level_mode_label(level_mode mode) {
        switch (mode) {
            case level_mode::Highscore: return " <HS>";
            case level_mode::Speedrun:
            default:
                return "";
        }
    }

    // compatiblity
    using string_utils::hex_chars_from_string;
    using string_utils::string_from_hex_chars;

    struct map {
        map_type type = map_type::Unknown;
        uint8_t md5[16] = {};
        int32_t level = 0;

        static constexpr const char* const original_map_hashes[] = {
            "00000000000000000000000000000000", // 0
            "a364b408fffaab4344806b427e37f1a7",
            "e90b2f535c8bf881e9cb83129fba241d",
            "22f895812942f954bab73a2924181d0d",
            "478faf2e028a7f352694fb2ab7326fec",
            "5797e3a9489a1cd6213c38c7ffcfb02a",
            "0dde7ec92927563bb2f34b8799b49e4c",
            "3473005097612bd0c0e9de5c4ea2e5de",
            "8b81694d53e6c5c87a6c7a5fa2e39a8d",
            "21283cde62e0f6d3847de85ae5abd147",
            "d80f54ffaa19be193a455908f8ff6e1d",
            "47f936f45540d67a0a1865eac334d2db",
            "2a1d29359b9802d4c6501dd2088884db",
            "9b5be1ca6a92ce56683fa208dd3453b4"
        };

        bool is_original_level() const {
            if (type != map_type::OriginalLevel) return false;
            decltype(md5) level_md5;
            hex_chars_from_string(level_md5, original_map_hashes[std::clamp(level, 0, 13)]);
            if (memcmp(level_md5, md5, sizeof(md5)) == 0)
                return true;
            return false;
        }

        bool operator==(const map& that) const {
            return (is_original_level() == that.is_original_level() && memcmp(md5, that.md5, sizeof(md5)) == 0);
        }

        bool operator!=(const map& that) const {
            return !(*this == that);
        }

        std::string get_display_name(const std::string& name) const {
            std::string map_name;
            if (is_original_level()) {
                map_name = name;
                if (map_name.find('_') != std::string::npos)
                    map_name.replace(map_name.find('_'), 1, " ");
            }
            else {
                map_name = "\"" + name + "\"";
            }
            return map_name;
        }

        std::string get_display_name(const std::unordered_map<std::string, std::string>& map_names) const {
            if (auto it = map_names.find(get_hash_bytes_string()); it != map_names.end()) {
                return get_display_name(it->second);
            }
            std::string hash_string = get_hash_string();
            return get_display_name(
                (hash_string == std::string(sizeof(md5) * 2, '0')) ? "N/A" : hash_string.substr(0, 20).append(".."));
        };

        std::string get_hash_bytes_string() const {
            std::string bytes(reinterpret_cast<const char*>(md5), sizeof(md5));
            return bytes;
        };

        std::string get_hash_string() const {
            std::string hash_string;
            string_from_hex_chars(hash_string, md5, sizeof(md5));
            return hash_string;
        }
    };

    struct named_map : map {
        std::string name;

        std::string get_display_name() const {
            return map::get_display_name(name);
        }

        void serialize(std::stringstream& raw) {
            message_utils::write_string(name, raw);
            raw.write(reinterpret_cast<const char*>(&type), sizeof(type));
            raw.write(reinterpret_cast<const char*>(md5), sizeof(md5));
            raw.write(reinterpret_cast<const char*>(&level), sizeof(level));
        }

        bool deserialize(std::stringstream& raw) {
            if (!message_utils::read_string(raw, name)) return false;
            raw.read(reinterpret_cast<char*>(&type), sizeof(type));
            if (!raw.good() || raw.gcount() != sizeof(type)) return false;
            raw.read(reinterpret_cast<char*>(md5), sizeof(md5));
            if (!raw.good() || raw.gcount() != sizeof(md5)) return false;
            raw.read(reinterpret_cast<char*>(&level), sizeof(level));
            if (!raw.good() || raw.gcount() != sizeof(level)) return false;
            return true;
        }
    };

    inline std::string get_formatted_time(float time_elapsed) {
        int total = int(time_elapsed);
        int minutes = total / 60;
        int seconds = total % 60;
        int hours = minutes / 60;
        minutes = minutes % 60;
        int ms = int((time_elapsed - total) * 1000);
        std::string text(64, 0);
        text.resize(std::snprintf(text.data(), text.size(),
            "%02d:%02d:%02d.%03d", hours, minutes, seconds, ms));
        return text;
    }
}

#endif //BALLANCEMMOSERVER_MAP_HPP
