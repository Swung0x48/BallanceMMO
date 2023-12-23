cmake_minimum_required(VERSION 3.12)

# set (CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
# set (CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(YAML_BUILD_SHARED_LIBS ON)

project(BallanceMMOServer)
set(CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD 20)

set(BMMO_LIB_DIR ${PROJECT_BINARY_DIR}/lib)
set(BMMO_RUNTIME_DIR ${PROJECT_BINARY_DIR})

# sets RPATH for *install*ed files
set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_RPATH}:\$ORIGIN:${BMMO_LIB_DIR}") # Note CMake escaping around '$' sign.
# *built* files will also use RPATH which has been set before
set(CMAKE_BUILD_WITH_INSTALL_RPATH on)
# if (CMAKE_SYSTEM_NAME MATCHES "Windows")
#     # setting vcpkg toolchain file
# #    set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake
# #            CACHE STRING "Vcpkg toolchain file")

#     if (MINGW)  # if mingw
#         # setting vcpkg
#         if (DEFINED ENV{VCPKG_DEFAULT_TRIPLET} AND NOT DEFINED VCPKG_TARGET_TRIPLET)
#             set(VCPKG_TARGET_TRIPLET "$ENV{VCPKG_DEFAULT_TRIPLET}" CACHE STRING "")
#         endif ()
#     endif ()
#     find_package(Protobuf REQUIRED)
#     find_package(GameNetworkingSockets REQUIRED)
# endif ()

#add_compile_options("$<$<CONFIG:DEBUG>:-DDEBUG>")
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../submodule/GameNetworkingSockets ${CMAKE_CURRENT_BINARY_DIR}/BallanceMMO)

include_directories(../submodule/asio/asio/include)
include_directories(../submodule/ya_getopt)
include_directories(../submodule/GameNetworkingSockets/src/external)

include_directories("${CMAKE_CURRENT_LIST_DIR}/../BallanceMMOCommon/include"
                    "${CMAKE_CURRENT_LIST_DIR}/../BallanceMMOCommon/include/config")

include(FetchContent)
FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp
    GIT_TAG 190ad502b5bc91b01fd2e73f5343cf7b40cfdc70
)
FetchContent_MakeAvailable(yaml-cpp)

set(BMMO_COMMON_SRC_DIRECTORY ../BallanceMMOCommon/src)
file(GLOB BMMO_COMMON_SRC ${BMMO_COMMON_SRC_DIRECTORY}/entity/*.cpp ${BMMO_COMMON_SRC_DIRECTORY}/utility/*.cpp)

add_executable(BallanceMMOServer server.cpp config_manager.cpp ${BMMO_COMMON_SRC} ../submodule/ya_getopt/ya_getopt.c)
target_include_directories(BallanceMMOServer PRIVATE)
target_link_libraries(BallanceMMOServer GameNetworkingSockets::shared yaml-cpp)
add_executable(BallanceMMOMockClient client.cpp ${BMMO_COMMON_SRC} ../submodule/ya_getopt/ya_getopt.c)
target_include_directories(BallanceMMOMockClient PRIVATE)
target_link_libraries(BallanceMMOMockClient GameNetworkingSockets::shared)
add_executable(BallanceMMORecordParser record_parser.cpp ${BMMO_COMMON_SRC} ../submodule/ya_getopt/ya_getopt.c)
target_include_directories(BallanceMMORecordParser PRIVATE)
target_link_libraries(BallanceMMORecordParser GameNetworkingSockets::shared)

get_target_property(_inc yaml-cpp INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(yaml-cpp SYSTEM INTERFACE ${_inc}) # suppress warnings
set_target_properties(GameNetworkingSockets yaml-cpp PROPERTIES
                            LIBRARY_OUTPUT_DIRECTORY ${BMMO_LIB_DIR})
# set_target_properties(yaml-cpp PROPERTIES VERSION "" SOVERSION "")
if (NOT WIN32)
    target_link_libraries(BallanceMMOServer readline)
    target_link_libraries(BallanceMMOMockClient readline)
    target_link_libraries(BallanceMMORecordParser readline)
endif()

if (MSVC)
    set(compile_options /Wall)
else()
    set(compile_options -Wall -Wextra -Wno-unknown-pragmas)
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 10)
        list(APPEND compile_options -fconcepts)
    endif()
endif()

target_compile_options(BallanceMMOServer PRIVATE ${compile_options})
target_compile_options(BallanceMMOMockClient PRIVATE ${compile_options})
target_compile_options(BallanceMMORecordParser PRIVATE ${compile_options})
if (WIN32)
# Prevents Windows.h from adding unnecessary includes, and defining min/max as macros 
    target_compile_definitions(BallanceMMOServer PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_definitions(BallanceMMOMockClient PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_definitions(BallanceMMORecordParser PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    set_target_properties(GameNetworkingSockets yaml-cpp PROPERTIES
                            RUNTIME_OUTPUT_DIRECTORY ${BMMO_RUNTIME_DIR})
endif() 

# os specific scripts
if (CMAKE_SYSTEM_NAME MATCHES "Windows")
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/powershell_wrapper.bat ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_BUILD_TYPE}/start_ballancemmo_loop.bat COPYONLY)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/start_ballancemmo_loop.ps1 ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_BUILD_TYPE}/bmmo_loop.ps1 COPYONLY)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/postbuild.bat ${CMAKE_CURRENT_BINARY_DIR}/postbuild.bat COPYONLY)
else()
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/start_ballancemmo_loop.sh ${CMAKE_CURRENT_BINARY_DIR}/start_ballancemmo_loop.sh COPYONLY)
endif()
