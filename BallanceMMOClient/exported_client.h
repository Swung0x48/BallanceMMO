#pragma once

#include "client.h"

namespace bmmo::exported:: inline _v1 {
    class client : public ::client {
    public:
        virtual std::string get_client_name() = 0;
        virtual bool is_spectator() { return false; }
        virtual named_map get_current_map() = 0;
    };
};
