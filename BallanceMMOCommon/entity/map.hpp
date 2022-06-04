#ifndef BALLANCEMMOSERVER_ENTITY_MAP_HPP
#define BALLANCEMMOSERVER_ENTITY_MAP_HPP
#include <cstdint>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <cstring>

namespace bmmo {
    enum map_type : uint8_t {
        UnknownType,
        OriginalLevel,
        CustomMap
    };

    const std::vector<std::string> original_map_hashes = {
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

    void hex_chars_from_string(uint8_t* dest, const std::string& src) {
        for (unsigned int i = 0; i < src.length(); i += 2) {
            std::string byteString = src.substr(i, 2);
            uint8_t byte = (uint8_t) strtol(byteString.c_str(), NULL, 16);
            dest[i / 2] = byte;
        }
    };

    void string_from_hex_chars(std::string& dest, const uint8_t* src, const int length) {
        std::stringstream ss;
        ss << std::hex << std::setfill('0');
        for (int i = 0; i < length; i++)
          ss << std::setw(2) << (int)src[i];
        dest = ss.str();
    }

    struct map {
        map_type type = UnknownType;
        std::string name = "";
        uint8_t md5[16];
        uint32_t level = 0;

        bool is_original_level() {
            if (type != OriginalLevel) return false;
            try {
                uint8_t level_md5[16];
                hex_chars_from_string(level_md5, original_map_hashes[level]);
                if (memcmp(level_md5, md5, 16) == 0)
                    return true;
            }
            catch (...) {
                return false;
            }
            return false;
        }

        bool operator==(const map& that) const {
            if (type != that.type) return false;
            if (name != that.name) return false;
            if (memcmp(md5, that.md5, 16) != 0) return false;
            if (level != that.level) return false;
            return true;
        }

        bool operator!=(const map& that) const {
            return !(*this == that);
        }

        map& operator=(const map& that) {
            type = that.type;
            name = that.name;
            memcpy(md5, that.md5, 16);
            level = that.level;
            return *this;
        }

        std::string get_display_name() {
            std::string map_name;
            if (is_original_level()) {
                map_name = name;
                map_name.replace(5, 1, " ");
            }
            else {
                map_name = "\"" + name + "\"";
            }
            return map_name;
        }
    };
}

#endif //BALLANCEMMOSERVER_ENTITY_MAP_HPP