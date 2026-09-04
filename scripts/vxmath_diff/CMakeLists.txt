# Differential test of the Ballanced fork's VxMath against the retail
# VxMath.dll the game ships.  Must be built 32-bit, because that is what the
# retail DLL is.
#
#   cmake -S scripts/vxmath_diff -B build-vxdiff -G Ninja -A Win32 \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-vxdiff
#   build-vxdiff/vxmath_diff.exe "<game>/Bin/VxMath.dll"
#
# The retail DLL is loaded at run time with LoadLibrary rather than linked, so
# its exports never collide with the fork's identically named inline maths.
cmake_minimum_required(VERSION 3.20)
project(vxmath_diff LANGUAGES CXX)

if (NOT WIN32)
    message(FATAL_ERROR "vxmath_diff compares against a Windows DLL; build it on Windows")
endif ()
if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "vxmath_diff must be built 32-bit to load the retail VxMath.dll")
endif ()

set(CMAKE_CXX_STANDARD 17)
set(BALLANCED "${CMAKE_CURRENT_LIST_DIR}/../../submodule/Ballanced/Source")

# The fork's VxMath, as a static library, so the inline maths under test is the
# same code the headless engine and the physics plugin compile.
set(VXMATH_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(VXMATH_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(VXMATH_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(VXMATH_BUILD_PERF OFF CACHE BOOL "" FORCE)
set(VXMATH_INSTALL OFF CACHE BOOL "" FORCE)
if (NOT TARGET SDL3::SDL3)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(vxdiff_sdl3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG 96292a5b464258a2b926e0a3d72f8b98c2a81aa6)
    FetchContent_MakeAvailable(vxdiff_sdl3)
endif ()
add_subdirectory("${BALLANCED}/VxMath" "${CMAKE_BINARY_DIR}/VxMath")

# retail_side.cpp needs no headers at all: it resolves the shipped exports by
# decorated name and passes raw float arrays, which match VxMatrix/VxVector.
add_library(vxdiff_retail STATIC retail_side.cpp)

add_library(vxdiff_fork STATIC fork_side.cpp)
target_compile_definitions(vxdiff_fork PRIVATE VX_LIB)
target_link_libraries(vxdiff_fork PRIVATE VxMathStatic)

foreach (_tool vxmath_diff:main solve_mul:solve_mul solve_rest:solve_rest
               solve_inv:solve_inv probe_inv:probe_inv)
    string(REPLACE ":" ";" _parts "${_tool}")
    list(GET _parts 0 _name)
    list(GET _parts 1 _src)
    add_executable(${_name} "${_src}.cpp")
    target_link_libraries(${_name} PRIVATE vxdiff_retail)
    if (_name STREQUAL "vxmath_diff")
        target_link_libraries(${_name} PRIVATE vxdiff_fork VxMathStatic)
    endif ()
endforeach ()
