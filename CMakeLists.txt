cmake_minimum_required(VERSION 3.16)
set (CMAKE_CXX_STANDARD 20)
set(PROJECT_NAME BallanceMMO)

set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/submodule/vcpkg/scripts/buildsystems/vcpkg.cmake
           CACHE STRING "Vcpkg toolchain file")
set(VCPKG_MANIFEST_DIR ${CMAKE_CURRENT_LIST_DIR} CACHE STRING "Vcpkg manifest dir")
set(VCPKG_MANIFEST_MODE TRUE CACHE BOOL "Vcpkg manifest mode")
set(VCPKG_TARGET_TRIPLET "x86-windows" CACHE STRING "Vcpkg manifest dir")

project(${PROJECT_NAME})

if (CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "64-bit Virtools isn't real (yet).")
endif()

set(BMMO_VER_DEFINED 1)
set(BMMO_MAJOR_VER 3)
set(BMMO_MINOR_VER 5)
set(BMMO_SUBMINOR_VER 2)
set(BMMO_STAGE_VER beta)
set(BMMO_BUILD_VER 5)

option(BUILD_SERVER "Build BallanceMMO Server" ON)
option(BUILD_CLIENT "Build BallanceMMO Client" ON)

if(BUILD_SERVER)
	add_subdirectory(BallanceMMOServer)
endif()

if(BUILD_CLIENT)
	add_subdirectory(BallanceMMOClient)
endif()
