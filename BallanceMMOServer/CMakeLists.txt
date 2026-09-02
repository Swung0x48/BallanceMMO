set(BMMO_CMAKE_MINIMUM_VERSION 3.12)
cmake_minimum_required(VERSION ${BMMO_CMAKE_MINIMUM_VERSION})

# set (CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
# set (CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(YAML_BUILD_SHARED_LIBS ON)

project(BallanceMMOServer)
set(CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD 20)

set(BMMO_LIB_DIR ${PROJECT_BINARY_DIR}/lib)
set(BMMO_RUNTIME_DIR ${PROJECT_BINARY_DIR})

# sets RPATH for *install*ed files
set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_RPATH}:\$ORIGIN:lib:../lib") # Note CMake escaping around '$' sign.
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
# BMMO_BUILD_SIM_ONLY: only the headless simulation library and tool (no
# networking); used for physics determinism work on platforms where the
# server's dependencies are not set up.
option(BMMO_BUILD_SIM_ONLY "Build only the headless simulation library and tool" OFF)
if (BMMO_BUILD_SIM_ONLY AND NOT BMMO_BUILD_SIM)
    message(FATAL_ERROR "BMMO_BUILD_SIM_ONLY needs BMMO_BUILD_SIM=ON")
endif ()
if (NOT BMMO_BUILD_SIM_ONLY)
if (NOT TARGET GameNetworkingSockets)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../submodule/GameNetworkingSockets ${CMAKE_CURRENT_BINARY_DIR}/GameNetworkingSockets)
endif ()
if (NOT TARGET replxx)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../submodule/replxx ${CMAKE_CURRENT_BINARY_DIR}/replxx)
endif ()

include_directories(../submodule/asio/asio/include)
include_directories(../submodule/ya_getopt)
include_directories(../submodule/GameNetworkingSockets/src/external)
include_directories(../submodule/replxx/include)

include_directories("${CMAKE_CURRENT_LIST_DIR}/../BallanceMMOCommon/include"
                    "${CMAKE_CURRENT_LIST_DIR}/../BallanceMMOCommon/include/config")

include(FetchContent)
FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp
    GIT_TAG 2f86d13775d119edbb69af52e5f566fd65c6953b
)
# temporary yaml-cpp cmake version workaround
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(yaml-cpp)
set(CMAKE_POLICY_VERSION_MINIMUM ${BMMO_CMAKE_MINIMUM_VERSION})

set(YA_GETOPT_SRC "../submodule/ya_getopt/ya_getopt.c")

set(BMMO_COMMON_SRC_DIRECTORY ../BallanceMMOCommon/src)
file(GLOB BMMO_COMMON_SRC ${BMMO_COMMON_SRC_DIRECTORY}/entity/*.cpp ${BMMO_COMMON_SRC_DIRECTORY}/utility/*.cpp)

endif ()  # NOT BMMO_BUILD_SIM_ONLY
if (NOT BMMO_BUILD_SIM_ONLY)
add_executable(BallanceMMOServer server.cpp config_manager.cpp ${BMMO_COMMON_SRC} ${YA_GETOPT_SRC})
target_include_directories(BallanceMMOServer PRIVATE)
target_link_libraries(BallanceMMOServer GameNetworkingSockets::shared yaml-cpp replxx)
add_executable(BallanceMMOMockClient client.cpp ${BMMO_COMMON_SRC} ${YA_GETOPT_SRC})
target_include_directories(BallanceMMOMockClient PRIVATE)
target_link_libraries(BallanceMMOMockClient GameNetworkingSockets::shared replxx)
add_executable(BallanceMMORecordParser record_parser.cpp ${BMMO_COMMON_SRC} ${YA_GETOPT_SRC})
target_include_directories(BallanceMMORecordParser PRIVATE)
target_link_libraries(BallanceMMORecordParser GameNetworkingSockets::shared replxx)

endif ()  # NOT BMMO_BUILD_SIM_ONLY (executables)
if (BMMO_BUILD_SIM)
    include(${CMAKE_CURRENT_LIST_DIR}/../cmake/BallancedHeadless.cmake)
    bmmo_add_ballanced_headless(${CMAKE_CURRENT_LIST_DIR}/../submodule/Ballanced BMMO_HEADLESS_TARGET)
    # The Ballanced Player edits the retail boot script at load time
    # (Hotfix.cpp: language, driver/screen-mode enumeration, opening skip).
    # The headless engine applies the identical edits with the Player's own
    # unmodified sources so client and server run the same scripts.
    set(BMMO_PLAYER_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../submodule/Ballanced/Source/Player/src")
    set(BMMO_PLAYER_GENERATED_DIR "${CMAKE_BINARY_DIR}/Ballanced/player-generated")
    set(PROJECT_VERSION_MAJOR 0)
    set(PROJECT_VERSION_MINOR 3)
    set(PROJECT_VERSION_PATCH 8)
    set(PROJECT_VERSION_TWEAK 0)
    set(PROJECT_VERSION "0.3.8")
    set(PROJECT_DESCRIPTION "BallanceMMO headless player")
    set(PLAYER_SCREEN_WIDTH 1024)
    set(PLAYER_SCREEN_HEIGHT 768)
    set(PLAYER_SCREEN_BPP 32)
    configure_file("${BMMO_PLAYER_SRC_DIR}/config.h.in" "${BMMO_PLAYER_GENERATED_DIR}/config.h" @ONLY)
    add_library(BallanceMMOSim STATIC
            sim/headless_engine.cpp sim/headless_engine.hpp
            sim/null_managers.cpp sim/null_managers.hpp
            sim/player_navigation.cpp sim/player_navigation.hpp
            sim/physics_world.cpp sim/physics_world.hpp
            sim/session_runner.cpp sim/session_runner.hpp
            "${CMAKE_CURRENT_LIST_DIR}/../BallanceMMOCommon/src/physics/physics_state.cpp"
            "${BMMO_PLAYER_SRC_DIR}/Hotfix.cpp"
            "${BMMO_PLAYER_SRC_DIR}/GameConfig.cpp"
            "${BMMO_PLAYER_SRC_DIR}/IniFile.cpp"
            "${BMMO_PLAYER_SRC_DIR}/Logger.cpp"
            "${BMMO_PLAYER_SRC_DIR}/Utils.cpp")
    target_include_directories(BallanceMMOSim PUBLIC ${CMAKE_CURRENT_LIST_DIR}
            "${CMAKE_CURRENT_LIST_DIR}/../BallanceMMOCommon/include"
            "${CMAKE_CURRENT_LIST_DIR}/../BallanceMMOCommon/include/config"
            "${BMMO_PLAYER_SRC_DIR}" "${BMMO_PLAYER_GENERATED_DIR}")
    target_link_libraries(BallanceMMOSim PUBLIC ${BMMO_HEADLESS_TARGET})
    target_compile_features(BallanceMMOSim PUBLIC cxx_std_20)
    if (WIN32)
        target_compile_definitions(BallanceMMOSim PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
    endif ()
    # The rasterizer stub answers a reference made by the static render
    # engine; it must be an object of the executable itself (an archive
    # member would be skipped by GNU ld, which scans libraries in order).
    add_executable(BallanceMMOSimTool sim/sim_tool.cpp sim/rasterizer_stub.cpp sim/crash_report.cpp)
    target_link_libraries(BallanceMMOSimTool PRIVATE BallanceMMOSim)
    if (WIN32)
        target_link_libraries(BallanceMMOSimTool PRIVATE dbghelp)
    elseif (ANDROID)
        # SDL's Android file layer uses the asset manager and the log API.
        target_link_libraries(BallanceMMOSimTool PRIVATE android log)
    endif ()
    if (NOT BMMO_BUILD_SIM_ONLY)
        # Headless physics-session client (design 8.6 item 3): headless engine
        # + GNS client that plays a session from a recorded keyboard.
        add_executable(BallanceMMOSessionClient sim/session_client.cpp sim/rasterizer_stub.cpp sim/crash_report.cpp
                ${BMMO_COMMON_SRC} ${YA_GETOPT_SRC})
        target_link_libraries(BallanceMMOSessionClient PRIVATE BallanceMMOSim GameNetworkingSockets::shared replxx)
        target_compile_definitions(BallanceMMOSessionClient PRIVATE BMMO_INCLUDE_INTERNAL)
        if (WIN32)
            target_link_libraries(BallanceMMOSessionClient PRIVATE dbghelp)
            target_compile_definitions(BallanceMMOSessionClient PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        endif ()
    endif ()
    if (NOT BMMO_BUILD_SIM_ONLY)
        target_sources(BallanceMMOServer PRIVATE sim/rasterizer_stub.cpp sim/crash_report.cpp)
        target_link_libraries(BallanceMMOServer BallanceMMOSim)
        if (WIN32)
            target_link_libraries(BallanceMMOServer dbghelp)
        endif ()
        if (MSVC)
            # Symbols for the crash reporter's backtraces (Release builds).
            target_compile_options(BallanceMMOServer PRIVATE /Zi)
            target_compile_options(BallanceMMOSim PRIVATE /Zi)
            target_link_options(BallanceMMOServer PRIVATE /DEBUG)
            target_link_options(BallanceMMOSimTool PRIVATE /DEBUG)
        endif ()
        target_compile_definitions(BallanceMMOServer PRIVATE BMMO_BUILD_SIM=1)
    endif ()
endif ()

if (NOT BMMO_BUILD_SIM_ONLY)
target_compile_definitions(BallanceMMOServer PRIVATE BMMO_INCLUDE_INTERNAL)
target_compile_definitions(BallanceMMOMockClient PRIVATE BMMO_INCLUDE_INTERNAL)
target_compile_definitions(BallanceMMORecordParser PRIVATE BMMO_INCLUDE_INTERNAL)

get_target_property(_inc yaml-cpp INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(yaml-cpp SYSTEM INTERFACE ${_inc}) # suppress warnings
set_target_properties(GameNetworkingSockets yaml-cpp replxx PROPERTIES
                            LIBRARY_OUTPUT_DIRECTORY ${BMMO_LIB_DIR})
# set_target_properties(yaml-cpp PROPERTIES VERSION "" SOVERSION "")

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
    # Prevent Windows.h from adding unnecessary includes, and defining min/max as macros 
    target_compile_definitions(BallanceMMOServer PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_definitions(BallanceMMOMockClient PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_definitions(BallanceMMORecordParser PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    set_target_properties(GameNetworkingSockets yaml-cpp replxx PROPERTIES
                            RUNTIME_OUTPUT_DIRECTORY ${BMMO_RUNTIME_DIR})
endif() 

# os specific scripts
if (CMAKE_SYSTEM_NAME MATCHES "Windows")
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/powershell_wrapper.bat ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_BUILD_TYPE}/start_ballancemmo_loop.bat COPYONLY)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/start_ballancemmo_loop.ps1 ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_BUILD_TYPE}/bmmo_loop.ps1 COPYONLY)
    # configure_file(${CMAKE_CURRENT_SOURCE_DIR}/postbuild.bat ${CMAKE_CURRENT_BINARY_DIR}/postbuild.bat COPYONLY)
else()
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/start_ballancemmo_loop.sh ${BMMO_RUNTIME_DIR}/start_ballancemmo_loop.sh COPYONLY)
    install(TARGETS BallanceMMOServer BallanceMMOMockClient BallanceMMORecordParser DESTINATION bin)
    install(TARGETS GameNetworkingSockets yaml-cpp DESTINATION lib)
endif()

option(BUILD_SERVER_TESTS "Build BallanceMMO server unit tests" OFF)

if (BUILD_SERVER_TESTS)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest
        GIT_TAG v1.14.0
    )
    # match the /MD runtime used by the rest of the build
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    enable_testing()
    add_executable(BallanceMMOMessageTests tests/message_layer_tests.cpp tests/room_manager_test.cpp tests/session_messages_test.cpp tests/session_logic_test.cpp)
    target_link_libraries(BallanceMMOMessageTests PRIVATE GTest::gtest_main GameNetworkingSockets::shared)
    if (WIN32)
        target_compile_definitions(BallanceMMOMessageTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    include(GoogleTest)
    gtest_discover_tests(BallanceMMOMessageTests DISCOVERY_MODE PRE_TEST)
endif()

option(BUILD_SERVER_APPIMAGE "Build BallanceMMO Server AppImage" OFF)

if (BUILD_SERVER_APPIMAGE)
    add_executable(BallanceMMOLaunchSelector appimage/launch_selector.cpp)
    install(TARGETS BallanceMMOLaunchSelector DESTINATION bin)
endif()

endif ()  # NOT BMMO_BUILD_SIM_ONLY (tail)
