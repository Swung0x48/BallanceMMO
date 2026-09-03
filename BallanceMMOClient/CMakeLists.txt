cmake_minimum_required(VERSION 3.20)

project(BallanceMMOClient)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "BallanceMMOClient is a Win32 MSVC BMLPlus mod")
endif ()
if (NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "BallanceMMOClient must be configured with a Win32 (x86) toolchain")
endif ()

include_directories("${CMAKE_CURRENT_LIST_DIR}/../BallanceMMOCommon/include"
                    "${CMAKE_CURRENT_LIST_DIR}/../BallanceMMOCommon/include/config")

set(CONFIG_DIR ${CMAKE_CURRENT_LIST_DIR}/../BallanceMMOCommon/include/config)
configure_file(${CONFIG_DIR}/version_config.h.in ${CONFIG_DIR}/version_config.h @ONLY)

if (NOT TARGET GameNetworkingSockets)
    find_package(Protobuf REQUIRED)
    find_package(GameNetworkingSockets REQUIRED)
endif ()
find_package(OpenSSL REQUIRED)

# Boost's CMake wrapper in the pinned vcpkg baselines cannot resolve the
# compiled regex component with recent MSVC toolsets, so resolve it from the
# triplet root instead.  Boost.Regex is header-only from 1.87 onwards (the v5
# implementation), in which case there is no library to link and the headers
# found here are all the client needs; every other Boost module it uses has
# always been header-only.
find_path(BMMO_BOOST_INCLUDE_DIR NAMES boost/regex.hpp REQUIRED)
cmake_path(GET BMMO_BOOST_INCLUDE_DIR PARENT_PATH BMMO_VCPKG_TRIPLET_ROOT)
file(GLOB BMMO_BOOST_REGEX_RELEASE "${BMMO_VCPKG_TRIPLET_ROOT}/lib/boost_regex-*-mt.lib")
file(GLOB BMMO_BOOST_REGEX_DEBUG "${BMMO_VCPKG_TRIPLET_ROOT}/debug/lib/boost_regex-*-mt-gd.lib")
list(LENGTH BMMO_BOOST_REGEX_RELEASE _bmmo_regex_release_count)
list(LENGTH BMMO_BOOST_REGEX_DEBUG _bmmo_regex_debug_count)
if (_bmmo_regex_release_count GREATER 1 OR _bmmo_regex_debug_count GREATER 1)
    message(FATAL_ERROR
        "Expected at most one Boost.Regex release/debug library under ${BMMO_VCPKG_TRIPLET_ROOT}; "
        "got ${_bmmo_regex_release_count}/${_bmmo_regex_debug_count}")
endif ()
if (_bmmo_regex_release_count EQUAL 1 AND _bmmo_regex_debug_count EQUAL 1)
    set(BMMO_BOOST_REGEX_LIBRARIES
            optimized "${BMMO_BOOST_REGEX_RELEASE}"
            debug "${BMMO_BOOST_REGEX_DEBUG}")
    message(STATUS "[BallanceMMO] Boost.Regex: linking the compiled library")
else ()
    set(BMMO_BOOST_REGEX_LIBRARIES "")
    message(STATUS "[BallanceMMO] Boost.Regex: header-only")
endif ()

if (NOT TARGET replxx)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../submodule/replxx ${CMAKE_CURRENT_BINARY_DIR}/replxx)
endif ()

# Virtools SDK (CK2/VxMath import libraries and headers)
set(VIRTOOLS_SDK_PATH "${VIRTOOLS_SDK_PATH}" CACHE PATH "Path to the Virtools SDK")
set(VIRTOOLS_SDK_FETCH_FROM_GIT "${VIRTOOLS_SDK_FETCH_FROM_GIT}" CACHE BOOL "Fetch the Virtools SDK from git when not otherwise locatable")
set(VIRTOOLS_SDK_FETCH_FROM_GIT_PATH "${VIRTOOLS_SDK_FETCH_FROM_GIT_PATH}" CACHE FILEPATH "Location to download the SDK to")
if (NOT VIRTOOLS_SDK_PATH AND VIRTOOLS_SDK_FETCH_FROM_GIT)
    include(FetchContent)
    set(FETCHCONTENT_BASE_DIR_SAVE ${FETCHCONTENT_BASE_DIR})
    if (VIRTOOLS_SDK_FETCH_FROM_GIT_PATH)
        get_filename_component(FETCHCONTENT_BASE_DIR "${VIRTOOLS_SDK_FETCH_FROM_GIT_PATH}" REALPATH BASE_DIR "${CMAKE_SOURCE_DIR}")
    endif ()
    FetchContent_Declare(
            Virtools_SDK
            GIT_REPOSITORY https://github.com/doyaGu/Virtools-SDK-2.1.git
            GIT_TAG 175bcb12d829c687359b90615be53524cdba0132
            GIT_SHALLOW FALSE
    )
    message(STATUS "Downloading Virtools SDK")
    FetchContent_Populate(Virtools_SDK)
    set(VIRTOOLS_SDK_PATH "${virtools_sdk_SOURCE_DIR}" CACHE PATH "Path to the Virtools SDK" FORCE)
    set(FETCHCONTENT_BASE_DIR ${FETCHCONTENT_BASE_DIR_SAVE})
endif ()
find_package(VirtoolsSDK REQUIRED HINTS ${VIRTOOLS_SDK_PATH})

# BMLPlus SDK.  Only the latest BMLPlus is supported; the legacy BML 0.3.43
# target was dropped together with the shadow-only build.
set(BMMO_BMLPLUS_SDK_ROOT "" CACHE PATH "BMLPlus SDK root (contains include/ and lib/BMLPlus.lib)")
if (NOT BMMO_BMLPLUS_SDK_ROOT)
    message(FATAL_ERROR "BMMO_BMLPLUS_SDK_ROOT must point at an extracted BMLPlus SDK")
endif ()
cmake_path(ABSOLUTE_PATH BMMO_BMLPLUS_SDK_ROOT NORMALIZE OUTPUT_VARIABLE BMMO_BMLPLUS_SDK_ROOT)
set(BMLPLUS_INCLUDE_DIR "${BMMO_BMLPLUS_SDK_ROOT}/include")
set(BMLPLUS_LIBRARY "${BMMO_BMLPLUS_SDK_ROOT}/lib/BMLPlus.lib")
foreach (required_path IN ITEMS "${BMLPLUS_INCLUDE_DIR}/BML/BMLAll.h" "${BMLPLUS_LIBRARY}")
    if (NOT EXISTS "${required_path}")
        message(FATAL_ERROR "Required BMLPlus SDK file is missing: ${required_path}")
    endif ()
endforeach ()

set(ASIO_INCLUDE_DIR ../submodule/asio/asio/include)
set(GNS_EXT_INCLUDE_DIR ../submodule/GameNetworkingSockets/src/external)
set(PARALLEL_HASHMAP_INCLUDE_DIR ../submodule/parallel-hashmap)
set(REPLXX_INCLUDE_DIR ../submodule/replxx/include)

set(BMMO_CLIENT_HDR
        BallanceMMOClient.h
        bml_includes.h
        client.h
        CommandMMO.h
        config_manager.h
        console_window.h
        dumpfile.h
        exported_client.h
        game_objects.h
        game_repository.h
        game_state.h
        label_sprite.h
        local_state_handler_base.h
        local_state_handler_impl.h
        log_manager.h
        server_list.h
        sprite_base.h
        text_sprite.h
        utils.h
        automation/command_pipe.h
        session/physics_session.hpp
)
set(BMMO_CLIENT_SRC
        BallanceMMOClient.cpp
        game_repository.cpp
        config_manager.cpp
        console_window.cpp
        dumpfile.cpp
        server_list.cpp
        utils.cpp
        automation/command_pipe.cpp
        automation/client_automation.cpp
        session/fixed_tick.cpp
        session/input_injector.cpp
        physics/physics_view.cpp
        session/room_client.cpp
        session/physics_session_client.cpp
)

# MinHook (BSD-2-Clause) for the CKInputManager::PreProcess hook used by
# deterministic input injection (same technique as BallanceTAS).
include(FetchContent)
FetchContent_Declare(
        bmmo_minhook
        GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
        GIT_TAG v1.3.3
        GIT_SHALLOW TRUE)
FetchContent_GetProperties(bmmo_minhook)
if (NOT bmmo_minhook_POPULATED)
    FetchContent_Populate(bmmo_minhook)
endif ()
add_library(bmmo_minhook STATIC
        ${bmmo_minhook_SOURCE_DIR}/src/buffer.c
        ${bmmo_minhook_SOURCE_DIR}/src/hook.c
        ${bmmo_minhook_SOURCE_DIR}/src/trampoline.c
        ${bmmo_minhook_SOURCE_DIR}/src/hde/hde32.c
        ${bmmo_minhook_SOURCE_DIR}/src/hde/hde64.c)
target_include_directories(bmmo_minhook PUBLIC ${bmmo_minhook_SOURCE_DIR}/include)
set(BMMO_COMMON_SRC_DIRECTORY ../BallanceMMOCommon/src)
file(GLOB BMMO_COMMON_SRC ${BMMO_COMMON_SRC_DIRECTORY}/entity/*.cpp ${BMMO_COMMON_SRC_DIRECTORY}/utility/*.cpp)

add_library(BallanceMMOClient SHARED ${BMMO_CLIENT_HDR} ${BMMO_CLIENT_SRC} ${BMMO_COMMON_SRC})
set_target_properties(BallanceMMOClient PROPERTIES
                      OUTPUT_NAME "BallanceMMOClient"
                      SUFFIX ".bmodp"
                      ARCHIVE_OUTPUT_NAME "BallanceMMOClient")
target_include_directories(BallanceMMOClient PRIVATE
                            "${BMLPLUS_INCLUDE_DIR}"
                            "${BMMO_BOOST_INCLUDE_DIR}"
                            "${CMAKE_CURRENT_LIST_DIR}"
                            ${REPLXX_INCLUDE_DIR}
                            ${ASIO_INCLUDE_DIR}
                            ${GNS_EXT_INCLUDE_DIR}
                            ${PARALLEL_HASHMAP_INCLUDE_DIR})
target_link_libraries(BallanceMMOClient PRIVATE
                        bmmo_minhook
                        replxx
                        GameNetworkingSockets::shared
                        OpenSSL::Crypto
                        ${BMMO_BOOST_REGEX_LIBRARIES}
                        CK2
                        VxMath
                        "${BMLPLUS_LIBRARY}")
target_compile_definitions(BallanceMMOClient PRIVATE
        BMMO_USE_BML_PLUS
        BMMO_INCLUDE_INTERNAL
        REPLXX_STATIC
        BOOST_ALL_NO_LIB
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_WARNINGS)
if (NOT MSVC)
    target_compile_options(BallanceMMOClient PRIVATE -Wall -Wextra -Wno-unknown-pragmas)
endif ()

# Drop-in physics_RT plugin (open-source engine + BallanceMMO bridge); needs
# the VirtoolsSDK imported targets created above, hence included here.
if (BMMO_BUILD_PHYSICS_RT)
    include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PhysicsRTPlugin.cmake")
endif ()
