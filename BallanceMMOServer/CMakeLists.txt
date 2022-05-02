cmake_minimum_required(VERSION 3.12)
project(BallanceMMOServer)
set(CMAKE_CXX_STANDARD 20)


add_compile_options("$<$<CONFIG:DEBUG>:-DDEBUG>")
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../submodule/GameNetworkingSockets ${CMAKE_CURRENT_BINARY_DIR}/BallanceMMO)

set(CMAKE_TOOLCHAIN_FILE ../submodule/vcpkg/scripts/buildsystems/vcpkg.cmake)
#include(../submodule/GameNetworkingSockets/examples/CMakeLists.txt)
#include(../submodule/GameNetworkingSockets/cmake/DefaultBuildType.cmake)

#include(../submodule/GameNetworkingSockets/CMakeLists.txt)
#include(../submodule/GameNetworkingSockets/src/CMakeLists.txt)
#set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR})
#find_package(GameNetworkingSockets CONFIG REQUIRED)
include_directories(../submodule/asio/asio/include)
include_directories(../submodule/ya_getopt)

add_executable(BallanceMMOServer server.cpp ../submodule/ya_getopt/ya_getopt.c)
target_link_libraries(BallanceMMOServer GameNetworkingSockets::GameNetworkingSockets)
add_executable(BallanceMMOMockClient client.cpp ../submodule/ya_getopt/ya_getopt.c)
target_link_libraries(BallanceMMOMockClient GameNetworkingSockets::GameNetworkingSockets)

# os specific scripts
if (WIN32 OR MSVC) 
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/powershell_wrapper.bat ${CMAKE_CURRENT_BINARY_DIR}/start_ballancemmo_loop.bat COPYONLY)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/start_ballancemmo_loop.ps1 ${CMAKE_CURRENT_BINARY_DIR}/bmmo_loop.ps1 COPYONLY)
else()
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/start_ballancemmo_loop.sh ${CMAKE_CURRENT_BINARY_DIR}/start_ballancemmo_loop.sh COPYONLY)
endif()
