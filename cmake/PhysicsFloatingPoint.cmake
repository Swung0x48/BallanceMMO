# Floating-point discipline for the physics code (IVP + physics_RT), applied
# identically to the client's physics_RT plugin and the server's static
# engine.  Lockstep needs bit-identical arithmetic on every platform:
#
#   * no fused multiply-add contraction (results differ between FMA and
#     non-FMA code, and between compilers' contraction heuristics),
#   * no excess precision (x87 80-bit intermediates on 32-bit builds),
#   * source-order evaluation, no reassociation / fast-math,
#   * SSE2 (or the platform's IEEE unit) for every float and double.
#
# Transcendental functions are a separate problem (each C runtime has its
# own sin/cos/...); see BMMO_PHYSICS_PORTABLE_MATH.

option(BMMO_PHYSICS_FP_STRICT "Compile the physics code with strict IEEE semantics (no contraction, no excess precision)" ON)

function(bmmo_apply_physics_fp_flags target)
    if (NOT TARGET ${target})
        return()
    endif ()
    if (NOT BMMO_PHYSICS_FP_STRICT)
        return()
    endif ()
    if (MSVC)
        # /fp:precise: source order, no contraction, no excess precision with
        # SSE2 code (verified bit-identical x86 vs x64).  /fp:strict is not
        # used: on x86 it miscompiled the collision code (crash) and changed
        # nothing about determinism.  /arch:SSE2 is the x86 default since
        # VS2012 but state it anyway.
        target_compile_options(${target} PRIVATE /fp:precise)
        if (CMAKE_SIZEOF_VOID_P EQUAL 4)
            target_compile_options(${target} PRIVATE /arch:SSE2)
        endif ()
    else ()
        target_compile_options(${target} PRIVATE
                -ffp-contract=off -fexcess-precision=standard -fno-fast-math
                -fno-associative-math -fno-reciprocal-math)
        if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(i[3-6]86|x86)$")
            target_compile_options(${target} PRIVATE -msse2 -mfpmath=sse)
        endif ()
    endif ()
endfunction()

# The IVP library targets defined by physics_RT/ivp/CMakeLists.txt.
set(BMMO_IVP_TARGETS ivp_utility ivp_surface_manager ivp_collision ivp_intern
        ivp_physics ivp_controller ivp_compact_builder)

function(bmmo_apply_physics_fp_flags_to_all)
    foreach (_target IN LISTS BMMO_IVP_TARGETS ARGN)
        bmmo_apply_physics_fp_flags(${_target})
    endforeach ()
endfunction()

# qhull (inside ivp_compact_builder) sorts merge sets and neighbour sets with
# the C library's qsort(); tied keys come out in a runtime-specific order that
# changes the compact surfaces.  Force-include a shim that routes qsort() to
# the engine's XDeterministicQSort (Microsoft-runtime order everywhere).
# VXMATH_INCLUDE_DIR must point at the engine's VxMath headers.
function(bmmo_apply_deterministic_sort target vxmath_include_dir)
    if (NOT TARGET ${target})
        return()
    endif ()
    get_filename_component(_common_include "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../BallanceMMOCommon/include" ABSOLUTE)
    set(_shim "${_common_include}/physics/deterministic_qsort_shim.h")
    target_include_directories(${target} PRIVATE "${vxmath_include_dir}" "${_common_include}")
    if (MSVC)
        target_compile_options(${target} PRIVATE "/FI${_shim}")
    else ()
        # SHELL: keeps the pair together; CMake would otherwise de-duplicate
        # this -include against the portable-math one and leave a stray path.
        target_compile_options(${target} PRIVATE "SHELL:-include ${_shim}")
    endif ()
endfunction()
