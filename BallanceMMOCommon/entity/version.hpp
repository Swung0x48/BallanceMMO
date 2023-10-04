#ifndef BALLANCEMMOSERVER_VERSION_HPP
#define BALLANCEMMOSERVER_VERSION_HPP
#include <cstdint>
#include <sstream>
#include <cstring>
#include <map>
#include "../config/version_config.h"

#define STRINGIFY(x) #x
#define STR(x) STRINGIFY(x)
#if BMMO_VER_DEFINED
// Nothing needed here
#else
# define BMMO_MAJOR_VER 3
# define BMMO_MINOR_VER 1
# define BMMO_SUBMINOR_VER 4
# define BMMO_STAGE_VER alpha
# define BMMO_BUILD_VER 159
#endif

#define BMMO_VER_STRING STR(BMMO_MAJOR_VER) "." STR(BMMO_MINOR_VER) "." STR(BMMO_SUBMINOR_VER) "-" STR(BMMO_STAGE_VER) STR(BMMO_BUILD_VER)
#define BMMO_MIN_CLIENT_VER_STRING BMMO_VER_STRING


namespace bmmo {
    enum stage_t: uint8_t {
        Alpha,
        Beta,
        RC,
        Release
    };

    struct version_t {
        uint8_t major{};
        uint8_t minor{};
        uint8_t subminor{};
        stage_t stage{};
        uint8_t build{};

        const std::string to_string() const;
        static version_t from_string(const std::string& input);
        auto operator<=>(const version_t& that) const = default;
    };

    const static version_t
        current_version        = version_t::from_string(BMMO_VER_STRING),
        minimum_client_version = version_t::from_string(BMMO_MIN_CLIENT_VER_STRING);

    const std::string version_t::to_string() const {
        std::stringstream ss;
        ss << (int)major << '.' << (int)minor << '.' << (int)subminor;
        switch (stage) {
            case Alpha: ss << "-alpha" << (int)build; break;
            case Beta: ss << "-beta" << (int)build; break;
            case RC: ss << "-rc" << (int)build; break;
            case Release:
            default: break;
        };
        return ss.str();
    }

    version_t version_t::from_string(const std::string& input) {
        version_t v{};
        char stage_str[16]{};
        std::ignore = sscanf(input.c_str(), "%hhu.%hhu.%hhu-%15[^0123456789]%hhu",
            &v.major, &v.minor, &v.subminor, stage_str, &v.build);
        v.stage = std::map<std::string, stage_t>
            {{"alpha", Alpha}, {"beta", Beta}, {"rc", RC}, {"", Release}} [stage_str];
        return v;
    }

    // bool version_t::operator<(const version_t& that) const {
    //     if (major < that.major) return true;
    //     if (major > that.major) return false;
    //     if (minor < that.minor) return true;
    //     if (minor > that.minor) return false;
    //     if (subminor < that.subminor) return true;
    //     if (subminor > that.subminor) return false;
    //     if (stage < that.stage) return true;
    //     if (stage > that.stage) return false;
    //     if (build < that.build) return true;
    //     if (build > that.build) return false;
    //     return false;
    // }

    // bool version_t::operator>(const version_t& that) const {
    //     return that < *this;
    // }
}

#endif //BALLANCEMMOSERVER_VERSION_HPP
