#ifndef BALLANCEMMOSERVER_COMMON_HPP
#define BALLANCEMMOSERVER_COMMON_HPP

#ifdef _WIN32
#undef min
#undef max
#define NOMINMAX
#endif

#include "message/message_all.hpp"
#include "entity/entity.hpp"
#include "entity/version.hpp"
#include "entity/map.hpp"
#include "role/role.hpp"
#include "utility/name_validator.hpp"
#include "utility/command_parser.hpp"
#include "utility/hostname_parser.hpp"

#endif //BALLANCEMMOSERVER_COMMON_HPP
