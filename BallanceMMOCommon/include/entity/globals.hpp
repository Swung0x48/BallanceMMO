#ifndef BALLANCEMMOSERVER_GLOBALS_HPP
#define BALLANCEMMOSERVER_GLOBALS_HPP
#ifdef BMMO_INCLUDE_INTERNAL
#include <replxx.hxx>
#endif

namespace bmmo {

    extern const bool LOWER_THAN_WIN10;

#ifdef BMMO_INCLUDE_INTERNAL
    extern replxx::Replxx replxx_instance;
#endif
}

#endif //BALLANCEMMOSERVER_GLOBALS_HPP
