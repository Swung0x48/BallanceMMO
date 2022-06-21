cmake_minimum_required(VERSION 3.12)
project(BallanceMMOServer)
set(CMAKE_CXX_STANDARD 20)

if (CMAKE_SYSTEM_NAME MATCHES "Windows")
    # setting vcpkg toolchain file
#    set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake
#            CACHE STRING "Vcpkg toolchain file")

    if (MINGW)  # if mingw
        # setting vcpkg
        if (DEFINED ENV{VCPKG_DEFAULT_TRIPLET} AND NOT DEFINED VCPKG_TARGET_TRIPLET)
            set(VCPKG_TARGET_TRIPLET "$ENV{VCPKG_DEFAULT_TRIPLET}" CACHE STRING "")
        endif ()
    endif ()
    find_package(GameNetworkingSockets REQUIRED)
endif ()

#add_compile_options("$<$<CONFIG:DEBUG>:-DDEBUG>")
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../submodule/GameNetworkingSockets ${CMAKE_CURRENT_BINARY_DIR}/BallanceMMO)

include_directories(../submodule/asio/asio/include)
include_directories(../submodule/ya_getopt)

include(FetchContent)
FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp
    GIT_TAG 190ad502b5bc91b01fd2e73f5343cf7b40cfdc70
)
FetchContent_MakeAvailable(yaml-cpp)

add_executable(BallanceMMOServer server.cpp ../submodule/ya_getopt/ya_getopt.c)
target_link_libraries(BallanceMMOServer GameNetworkingSockets::static yaml-cpp)
add_executable(BallanceMMOMockClient client.cpp ../submodule/ya_getopt/ya_getopt.c)
target_link_libraries(BallanceMMOMockClient GameNetworkingSockets::static)

# os specific scripts
if (CMAKE_SYSTEM_NAME MATCHES "Windows")
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/powershell_wrapper.bat ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_BUILD_TYPE}/start_ballancemmo_loop.bat COPYONLY)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/start_ballancemmo_loop.ps1 ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_BUILD_TYPE}/bmmo_loop.ps1 COPYONLY)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/postbuild.bat ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_BUILD_TYPE}/postbuild.bat COPYONLY)
else()
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/start_ballancemmo_loop.sh ${CMAKE_CURRENT_BINARY_DIR}/start_ballancemmo_loop.sh COPYONLY)
endif()
