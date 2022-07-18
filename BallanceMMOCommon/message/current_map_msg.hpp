#ifndef BALLANCEMMOSERVER_CURRENT_MAP_MSG_HPP
#define BALLANCEMMOSERVER_CURRENT_MAP_MSG_HPP
#include "message.hpp"
#include "../entity/map.hpp"

namespace bmmo {
    std::string get_ordinal_rank(uint32_t rank);

    struct current_map {
        struct map map;
        uint32_t sector = 0;
    };
    
    typedef struct message<current_map, CurrentMap> current_map_msg;
}

#endif //BALLANCEMMOSERVER_CURRENT_MAP_MSG_HPP