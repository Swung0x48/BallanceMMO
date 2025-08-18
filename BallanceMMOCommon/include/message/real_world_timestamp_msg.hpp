#ifndef BALLANCEMMOSERVER_REAL_WORLD_TIMESTAMP_MSG_HPP
#define BALLANCEMMOSERVER_REAL_WORLD_TIMESTAMP_MSG_HPP
#include "message.hpp"

namespace bmmo {
    // timestamp in microseconds since unix epoch;
    // used by the server and record parser to hint the real world time
    typedef struct message<int64_t, RealWorldTimestamp> real_world_timestamp_msg;
}

#endif //BALLANCEMMOSERVER_REAL_WORLD_TIMESTAMP_MSG_HPP
