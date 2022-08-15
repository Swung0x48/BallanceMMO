#ifndef BALLANCEMMOSERVER_TIMESTAMP_MSG_HPP
#define BALLANCEMMOSERVER_TIMESTAMP_MSG_HPP
#include "message.hpp"

namespace bmmo {
    class timestamp_t {
        uint32_t v[2]{};
    public:
        operator int64_t() {
            return (int64_t)v[0] << 32 | v[1];
        }
        void operator=(int64_t t) {
            v[0] = t >> 32;
            v[1] = t;
        }
        int64_t operator+(const int64_t t) const {
            return (int64_t)v[0] << 32 | v[1] + t;
        }
        int64_t operator-(const int64_t t) const {
            return (int64_t)v[0] << 32 | v[1] - t;
        }
        bool operator<(const timestamp_t t) const {
            return ((int64_t)v[0] << 32 | v[1]) < ((int64_t)t.v[0] << 32 | t.v[1]);
        }
        bool operator==(const int64_t t) const {
            return ((int64_t)v[0] << 32 | v[1]) == t;
        }
    };

    typedef struct message<timestamp_t, Timestamp> timestamp_msg;
}

#endif //BALLANCEMMOSERVER_TIMESTAMP_MSG_HPP