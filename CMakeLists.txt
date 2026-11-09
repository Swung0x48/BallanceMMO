cmake_minimum_required(VERSION 3.16)
set(CMAKE_CXX_STANDARD 20)
set(PROJECT_NAME BallanceMMO)

# setting vcpkg toolchain file
if (WIN32)
    set(CMAKE_TOOLCHAIN_FILE
        ${CMAKE_CURRENT_LIST_DIR}/submodule/vcpkg/scripts/buildsystems/vcpkg.cmake
        CACHE STRING "Vcpkg toolchain file")
    set(VCPKG_MANIFEST_DIR ${CMAKE_CURRENT_LIST_DIR} CACHE STRING "Vcpkg manifest dir")
    set(VCPKG_MANIFEST_MODE TRUE CACHE BOOL "Vcpkg manifest mode")
endif()

if (CMAKE_SYSTEM_NAME MATCHES "Windows")
    # set(VCPKG_TARGET_TRIPLET "x86-windows" CACHE STRING "Vcpkg target triplet")

    if (MINGW)  # if mingw
        # setting vcpkg
        if (DEFINED ENV{VCPKG_DEFAULT_TRIPLET} AND NOT DEFINED VCPKG_TARGET_TRIPLET)
            set(VCPKG_TARGET_TRIPLET "$ENV{VCPKG_DEFAULT_TRIPLET}" CACHE STRING "")
        endif ()
    endif ()
    find_package(Protobuf REQUIRED)
    find_package(GameNetworkingSockets REQUIRED)
endif ()

project(${PROJECT_NAME})

# if (CMAKE_SIZEOF_VOID_P EQUAL 8)
# 	message(FATAL_ERROR "64-bit Virtools isn't real (yet).")
# endif()

# Lockstep: every host must compute what the retail client computes (MSVC, no
# FMA, /fp:precise), for EVERY target - not only IVP and physics_RT: the
# engine's CK2_3D / VxMath matrices (a parent's inverse, a child's local
# matrix) feed the physics too.  GCC/Clang fuse a*b+c by default whenever the
# target has FMA, and a compiler may default to an FMA target: the 2026-11-08
# public test's Linux server (CI, GCC 15 on ubuntu-latest) came out as
# x86-64-v3 with 6966 FMA instructions and split from every client within
# seconds on L9 (and on L3, L7, L10, L11).  So: contraction off everywhere,
# and x86-64 pinned to the baseline ISA (aarch64 always has FMA; the
# contraction switch is what keeps it out there).  MSVC is unaffected:
# /fp:precise never contracts.  Directory scope, before any subdirectory, so
# the engine, qhull and BallanceMMO's own code all get it.
if (NOT MSVC)
    add_compile_options(-ffp-contract=off -fno-fast-math -fexcess-precision=standard)
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        add_compile_options(-march=x86-64 -mtune=generic)
    endif ()
endif ()

set(BMMO_VER_DEFINED 1)
set(BMMO_VER            4.0.0-alpha4)
set(BMMO_MIN_CLIENT_VER 4.0.0-alpha4)
set(CONFIG_DIR ${CMAKE_CURRENT_LIST_DIR}/BallanceMMOCommon/include/config)
configure_file(${CONFIG_DIR}/version_config.h.in ${CONFIG_DIR}/version_config.h @ONLY)

# The IVP sources key their assertions and their debug output off NDEBUG: a
# release tree that lost it (an empty CMAKE_CXX_FLAGS_RELEASE is enough) builds
# a physics engine that BREAKPOINTs the whole process on an assertion IVP's own
# bounds can trip - the trafo into a stone ball did, on 2026-09-02 - and prints
# a statistics line every second.  Warn at configure time; it is invisible
# afterwards.
if (NOT CMAKE_CONFIGURATION_TYPES AND CMAKE_BUILD_TYPE)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" BMMO_BUILD_TYPE_UPPER)
    if (NOT BMMO_BUILD_TYPE_UPPER STREQUAL "DEBUG"
            AND NOT "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${BMMO_BUILD_TYPE_UPPER}}" MATCHES "NDEBUG")
        message(WARNING "${CMAKE_BUILD_TYPE} build without NDEBUG: the IVP physics assertions stay in "
                        "(a failed one kills the server) and IVP prints its statistics every second. "
                        "Configure with -DCMAKE_CXX_FLAGS_${BMMO_BUILD_TYPE_UPPER}=\"/O2 /Ob2 /DNDEBUG\" "
                        "(MSVC) or \"-O2 -DNDEBUG\" (GCC/Clang).")
    endif()
endif()

option(BUILD_SERVER "Build BallanceMMO Server" ON)
option(BUILD_CLIENT "Build BallanceMMO Client" ON)
option(BMMO_BUILD_SIM "Build the headless Ballanced simulation runtime into the server" ON)
option(BMMO_BUILD_PHYSICS_RT "Build the open-source physics_RT plugin with the BallanceMMO bridge (client)" ON)

# zstd v1.5.7 (submodule/zstd): the session journal's compression stream
# (session/zstd_stream.hpp), built here and linked statically everywhere -
# Windows x86 / x64, Linux, Android - so no host needs a zstd of its own and
# the retail mod brings no new DLL.  Single-threaded, no assembly: the same
# bytes on every platform.
file(GLOB BMMO_ZSTD_SRC
        ${CMAKE_CURRENT_LIST_DIR}/submodule/zstd/lib/common/*.c
        ${CMAKE_CURRENT_LIST_DIR}/submodule/zstd/lib/compress/*.c
        ${CMAKE_CURRENT_LIST_DIR}/submodule/zstd/lib/decompress/*.c)
add_library(bmmo_zstd STATIC ${BMMO_ZSTD_SRC})
target_include_directories(bmmo_zstd PUBLIC ${CMAKE_CURRENT_LIST_DIR}/submodule/zstd/lib)
target_compile_definitions(bmmo_zstd PRIVATE ZSTD_DISABLE_ASM)
set_target_properties(bmmo_zstd PROPERTIES POSITION_INDEPENDENT_CODE ON)
if (MSVC)
    target_compile_options(bmmo_zstd PRIVATE /W0)
else ()
    target_compile_options(bmmo_zstd PRIVATE -w)
endif ()

if(BUILD_SERVER)
    add_subdirectory(BallanceMMOServer)
endif()

if(BUILD_CLIENT)
    add_subdirectory(BallanceMMOClient)
endif()
