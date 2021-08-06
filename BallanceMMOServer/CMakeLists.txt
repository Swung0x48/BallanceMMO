cmake_minimum_required(VERSION 3.12)
project(BallanceMMOServer)
set(CMAKE_CXX_STANDARD 20)
set(THREADS_PREFER_PTHREAD_FLAG ON)

add_compile_options("$<$<CONFIG:DEBUG>:-DDEBUG>")
include (TestBigEndian)
TEST_BIG_ENDIAN(IS_BIG_ENDIAN)
if(IS_BIG_ENDIAN)
    message(STATUS "Big endian detected")
    add_definitions(-D__BYTE_ORDER=4321)
else()
    message(STATUS "Not big endian, assuming little endian")
    add_definitions(-D__BYTE_ORDER=1234)
endif()

find_package(Threads REQUIRED)

include_directories(submodules/AMMOProtocol/includes)
include_directories(submodules/AMMOProtocol/submodules/asio/asio/include)
include_directories(submodules/AMMOProtocol/submodules/crc32)

add_executable(BallanceMMOServer server.cpp submodules/AMMOProtocol/submodules/crc32/Crc32.cpp)
target_link_libraries(BallanceMMOServer PRIVATE Threads::Threads)
add_executable(BallanceMMOMockClient client.cpp submodules/AMMOProtocol/submodules/crc32/Crc32.cpp)
target_link_libraries(BallanceMMOMockClient PRIVATE Threads::Threads)

