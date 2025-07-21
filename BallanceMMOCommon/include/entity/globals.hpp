#ifndef BALLANCEMMOSERVER_GLOBALS_HPP
#define BALLANCEMMOSERVER_GLOBALS_HPP
#ifdef BMMO_INCLUDE_INTERNAL
#include <replxx.hxx>
#endif

namespace bmmo {

#ifdef _WIN32
    typedef std::wstring PATH_STRING;
    #define BMMO_PATH_LITERAL(x) L##x
#else
    typedef std::string PATH_STRING;
    #define BMMO_PATH_LITERAL(x) x
#endif

    extern const bool LOWER_THAN_WIN10;
    extern const PATH_STRING SHARED_CONFIG_PATH;

#ifdef BMMO_INCLUDE_INTERNAL
    extern replxx::Replxx replxx_instance;
#endif
}

#endif //BALLANCEMMOSERVER_GLOBALS_HPP
