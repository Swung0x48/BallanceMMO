# The build id both sides of a physics session report to each other, shared by
# the client's physics_RT plugin and the server's static engine.
#
# It is regenerated before every build rather than resolved once at configure
# time: a configure-time value survives every later commit, so a binary built
# from today's sources would still name the revision the build tree happened to
# be created at - which is worse than no id at all once the server starts
# refusing clients over it.
#
#   bmmo_add_build_id(<target>)   puts <bmmo_build_id.h> on the target's
#                                 include path and makes it depend on the
#                                 generator.
#
# -DBMMO_BUILD_ID=<id> overrides it outright, for builds from an export that
# has no git history to read.

set(BMMO_BUILD_ID_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(bmmo_add_build_id target)
    get_filename_component(_root "${BMMO_BUILD_ID_CMAKE_DIR}/.." ABSOLUTE)
    set(_dir "${CMAKE_BINARY_DIR}/bmmo-generated")
    set(_header "${_dir}/bmmo_build_id.h")
    set(_script "${BMMO_BUILD_ID_CMAKE_DIR}/WriteBuildId.cmake")
    if (NOT TARGET bmmo_build_id)
        file(MAKE_DIRECTORY "${_dir}")
        # Once now as well, so the header exists for anything that reads the
        # tree before the first build (IDEs, compile_commands.json consumers).
        execute_process(COMMAND "${CMAKE_COMMAND}"
                "-DBMMO_ROOT=${_root}" "-DBMMO_HEADER=${_header}"
                "-DBMMO_BUILD_ID=${BMMO_BUILD_ID}" -P "${_script}")
        add_custom_target(bmmo_build_id
                BYPRODUCTS "${_header}"
                COMMAND "${CMAKE_COMMAND}"
                        "-DBMMO_ROOT=${_root}" "-DBMMO_HEADER=${_header}"
                        "-DBMMO_BUILD_ID=${BMMO_BUILD_ID}" -P "${_script}"
                COMMENT "Resolving the BallanceMMO build id")
    endif ()
    target_include_directories(${target} PRIVATE "${_dir}")
    add_dependencies(${target} bmmo_build_id)
endfunction()
