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

add_executable(BallanceMMOServer server.cpp)
target_link_libraries(BallanceMMOServer GameNetworkingSockets::GameNetworkingSockets)
add_executable(BallanceMMOMockClient client.cpp)
target_link_libraries(BallanceMMOMockClient GameNetworkingSockets::GameNetworkingSockets)
