# Open-source physics_RT (Ballanced submodule) built as the drop-in plugin the
# BallanceMMO client ships for physics sessions.  It links against the
# Virtools SDK import libraries, so it loads into the retail engine exactly
# like the original DLL.  The engine sources are used unmodified; the only
# addition is the BallanceMMO bridge (BallanceMMOCommon/src/physics/*.cpp)
# compiled into the same module.
#
# Include from a directory scope that owns the VirtoolsSDK imported targets
# CK2 and VxMath (find_package(VirtoolsSDK)).

if (NOT TARGET CK2 OR NOT TARGET VxMath)
    message(FATAL_ERROR "PhysicsRTPlugin.cmake needs the VirtoolsSDK CK2 and VxMath targets")
endif ()

set(_bmmo_root "${CMAKE_CURRENT_LIST_DIR}/..")
set(_bmmo_bb "${_bmmo_root}/submodule/Ballanced/Source/BuildingBlocks")
if (NOT EXISTS "${_bmmo_bb}/physics_RT/CMakeLists.txt")
    message(FATAL_ERROR "Ballanced submodule is not checked out: git submodule update --init submodule/Ballanced")
endif ()

set(CKBB_BUILD_ALL_MODULES OFF CACHE BOOL "" FORCE)
set(CKBB_BUILD_physics_RT ON CACHE BOOL "" FORCE)
set(CKBB_BUILD_SHARED ON CACHE BOOL "" FORCE)
set(CKBB_BUILD_STATIC OFF CACHE BOOL "" FORCE)
set(CKBB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CKBB_INSTALL OFF CACHE BOOL "" FORCE)
# Convex hulls (every box / paper ball / module part) need qhull; the IVP
# tree leaves it off in some configurations, which silently drops those
# bodies at physicalize time.  The headless server builds with both on.
set(IVP_INCLUDE_QHULL ON CACHE BOOL "" FORCE)
set(IVP_INCLUDE_GEOMPACK ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF)
add_subdirectory("${_bmmo_bb}" "${CMAKE_BINARY_DIR}/Ballanced/BuildingBlocks")
if (NOT TARGET physics_RT)
    message(FATAL_ERROR "the Ballanced building-block tree did not define physics_RT")
endif ()

execute_process(COMMAND git -C "${_bmmo_root}/submodule/Ballanced" rev-parse --short=12 HEAD
        OUTPUT_VARIABLE _bmmo_engine_rev OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git -C "${_bmmo_root}" rev-parse --short=12 HEAD
        OUTPUT_VARIABLE _bmmo_rev OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if (NOT _bmmo_engine_rev)
    set(_bmmo_engine_rev "unknown")
endif ()
if (NOT _bmmo_rev)
    set(_bmmo_rev "unknown")
endif ()

file(GLOB _bmmo_ivp_dirs LIST_DIRECTORIES true "${_bmmo_bb}/physics_RT/ivp/*")
list(FILTER _bmmo_ivp_dirs EXCLUDE REGEX "\\.(cxx|hxx|txt|md)$")

target_sources(physics_RT PRIVATE
        "${_bmmo_root}/BallanceMMOCommon/src/physics/physics_state.cpp"
        "${_bmmo_root}/BallanceMMOCommon/src/physics/ball_navigation.cpp"
        "${_bmmo_root}/BallanceMMOCommon/src/physics/physics_rt_bridge.cpp")
target_include_directories(physics_RT PRIVATE
        "${_bmmo_root}/BallanceMMOCommon/include"
        "${_bmmo_bb}/physics_RT"
        ${_bmmo_ivp_dirs})
target_compile_definitions(physics_RT PRIVATE
        "BMMO_PHYSICS_BUILD_ID=\"ballanced-${_bmmo_engine_rev}+bmmo-${_bmmo_rev}\"")
if (WIN32)
    # Same platform defines the IVP libraries use (object layouts depend on them).
    target_compile_definitions(physics_RT PRIVATE WIN32 _WINDOWS)
endif ()
include("${CMAKE_CURRENT_LIST_DIR}/PhysicsFloatingPoint.cmake")
bmmo_apply_physics_fp_flags_to_all(physics_RT)
bmmo_apply_deterministic_sort(ivp_compact_builder "${_bmmo_root}/submodule/Ballanced/Source/VxMath/include")
include("${CMAKE_CURRENT_LIST_DIR}/PortableMath.cmake")
bmmo_apply_portable_math_to_all(physics_RT)
bmmo_link_portable_math(physics_RT PRIVATE)
set_target_properties(physics_RT PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/BuildingBlocks"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/BuildingBlocks")
message(STATUS "[BallanceMMO] physics_RT plugin: ballanced-${_bmmo_engine_rev}+bmmo-${_bmmo_rev}")
