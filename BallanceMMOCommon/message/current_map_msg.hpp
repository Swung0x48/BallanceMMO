#ifndef BALLANCEMMOSERVER_CURRENT_MAP_MSG_HPP
#define BALLANCEMMOSERVER_CURRENT_MAP_MSG_HPP
#include "message.hpp"
#include "../entity/map.hpp"
#include "did_not_finish_msg.hpp"

namespace bmmo {
    std::string get_ordinal_rank(uint32_t rank);
    
    typedef struct message<current_map_state, CurrentMap> current_map_msg;
}

#endif //BALLANCEMMOSERVER_CURRENT_MAP_MSG_HPP
