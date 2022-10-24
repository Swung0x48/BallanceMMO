#ifndef BALLANCEMMOSERVER_VERSION_HPP
#define BALLANCEMMOSERVER_VERSION_HPP
#include <cstdint>
#include <sstream>

namespace bmmo {
    enum stage_t: uint8_t {
        Alpha,
        Beta,
        RC,
        Release
    };

    struct version_t {
        uint8_t major = 3;
        uint8_t minor = 3;
        uint8_t subminor = 7;
        stage_t stage = Beta;
        uint8_t build = 14;

        const std::string to_string() const;
        bool operator<(const version_t& that) const;
        bool operator>(const version_t& that) const;
    };

    constexpr version_t minimum_client_version = {3, 3, 7, Beta, 12};

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

    bool version_t::operator<(const version_t& that) const {
        if (major < that.major) return true;
        if (major > that.major) return false;
        if (minor < that.minor) return true;
        if (minor > that.minor) return false;
        if (subminor < that.subminor) return true;
        if (subminor > that.subminor) return false;
        if (stage < that.stage) return true;
        if (stage > that.stage) return false;
        if (build < that.build) return true;
        if (build > that.build) return false;
        return false;
    }

    bool version_t::operator>(const version_t& that) const {
        return that < *this;
    }
}

#endif //BALLANCEMMOSERVER_VERSION_HPP
