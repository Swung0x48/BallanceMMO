cmake_minimum_required(VERSION 3.16)
set (CMAKE_CXX_STANDARD 20)
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

set(BMMO_VER_DEFINED 1)
set(BMMO_VER            3.5.8-beta18)
set(BMMO_MIN_CLIENT_VER 3.5.8-beta18)
set(CONFIG_DIR ${CMAKE_CURRENT_LIST_DIR}/BallanceMMOCommon/include/config)
configure_file(${CONFIG_DIR}/version_config.h.in ${CONFIG_DIR}/version_config.h @ONLY)

option(BUILD_SERVER "Build BallanceMMO Server" ON)
option(BUILD_CLIENT "Build BallanceMMO Client" ON)

if(BUILD_SERVER)
	add_subdirectory(BallanceMMOServer)
endif()

if(BUILD_CLIENT)
	add_subdirectory(BallanceMMOClient)
endif()
