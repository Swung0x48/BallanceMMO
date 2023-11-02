#ifndef BALLANCEMMOSERVER_COMMON_HPP
#define BALLANCEMMOSERVER_COMMON_HPP

#ifdef _WIN32
# if defined(min) || defined(max)
#  undef min
#  undef max
# endif
# ifndef NOMINMAX
#  define NOMINMAX
# endif
#endif

#include "message/message_all.hpp"
#include "entity/constants.hpp"
#include "entity/entity.hpp"
#include "entity/ranking_entry.hpp"
#include "entity/version.hpp"
#include "entity/map.hpp"
#include "role/role.hpp"
#include "utility/name_validator.hpp"
#include "utility/command_parser.hpp"
#include "utility/console.hpp"
#include "utility/hostname_parser.hpp"
#include "utility/string_utils.hpp"

#endif //BALLANCEMMOSERVER_COMMON_HPP
