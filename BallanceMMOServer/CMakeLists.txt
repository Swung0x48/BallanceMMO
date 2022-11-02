cmake_minimum_required(VERSION 3.12)

# set (CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
# set (CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

project(BallanceMMOServer)
set(CMAKE_CXX_STANDARD 20)

# sets RPATH for *install*ed files
set(CMAKE_INSTALL_RPATH "\$ORIGIN/bin") # Note CMake escaping around '$' sign.
# *built* files will also used RPATH which has been set before
set(CMAKE_BUILD_WITH_INSTALL_RPATH on)

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
    find_package(Protobuf REQUIRED)
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

add_executable(BallanceMMOServer server.cpp console.cpp ../submodule/ya_getopt/ya_getopt.c)
target_link_libraries(BallanceMMOServer GameNetworkingSockets::shared yaml-cpp)
add_executable(BallanceMMOMockClient client.cpp console.cpp ../submodule/ya_getopt/ya_getopt.c)
target_link_libraries(BallanceMMOMockClient GameNetworkingSockets::shared)
add_executable(BallanceMMORecordParser record_parser.cpp console.cpp ../submodule/ya_getopt/ya_getopt.c)
target_link_libraries(BallanceMMORecordParser GameNetworkingSockets::shared)

get_target_property(_inc yaml-cpp INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(yaml-cpp SYSTEM INTERFACE ${_inc}) # suppress warnings

if (MSVC)
    target_compile_options(BallanceMMOServer PRIVATE /Wall)
    target_compile_options(BallanceMMOMockClient PRIVATE /Wall)
    target_compile_options(BallanceMMORecordParser PRIVATE /Wall)
else()
    target_compile_options(BallanceMMOServer PRIVATE -Wall -Wextra)
    target_compile_options(BallanceMMOMockClient PRIVATE -Wall -Wextra)
    target_compile_options(BallanceMMORecordParser PRIVATE -Wall -Wextra -Wno-unknown-pragmas)
endif()


# os specific scripts
if (CMAKE_SYSTEM_NAME MATCHES "Windows")
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/powershell_wrapper.bat ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_BUILD_TYPE}/start_ballancemmo_loop.bat COPYONLY)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/start_ballancemmo_loop.ps1 ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_BUILD_TYPE}/bmmo_loop.ps1 COPYONLY)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/postbuild.bat ${CMAKE_CURRENT_BINARY_DIR}/postbuild.bat COPYONLY)
else()
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/start_ballancemmo_loop.sh ${CMAKE_CURRENT_BINARY_DIR}/start_ballancemmo_loop.sh COPYONLY)
endif()
