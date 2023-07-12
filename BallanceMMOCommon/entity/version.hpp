#ifndef BALLANCEMMOSERVER_VERSION_HPP
#define BALLANCEMMOSERVER_VERSION_HPP
#include <cstdint>
#include <sstream>
#include <cstring>
#include <map>

namespace bmmo {
    enum stage_t: uint8_t {
        Alpha,
        Beta,
        RC,
        Release
    };

    struct version_t {
        uint8_t major = 3;
        uint8_t minor = 4;
        uint8_t subminor = 7;
        stage_t stage = Alpha;
        uint8_t build = 8;

        const std::string to_string() const;
        static version_t from_string(const std::string& input);
        auto operator<=>(const version_t& that) const = default;
    };

    constexpr version_t minimum_client_version = {3, 4, 6, Alpha, 7};

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
