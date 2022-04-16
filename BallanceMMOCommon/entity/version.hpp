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
        uint8_t minor = 1;
        uint8_t subminor = 2;
        stage_t stage = Alpha;
        uint8_t build = 8;

        std::string to_string() {
            std::string stage_s = "";
            switch (stage) {
                case Alpha: stage_s = "-alpha" + std::to_string((int)build); break;
                case Beta: stage_s = "-beta" + std::to_string((int)build); break;
                case RC: stage_s = "-rc" + std::to_string((int)build); break;
                case Release: break;
            };
            std::stringstream ss;
            ss << (int)major << "." << (int)minor << "." << (int)subminor << stage_s;
            return std::move(ss.str());
        }

        bool operator<(const version_t& that) {
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
    };
}

#endif //BALLANCEMMOSERVER_VERSION_HPP
