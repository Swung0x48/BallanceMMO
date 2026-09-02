# Deterministic libm for the physics code (see
# BallanceMMOCommon/third_party/openlibm/bmmo_portable_math.c).  When
# BMMO_PHYSICS_PORTABLE_MATH is on, every IVP / physics_RT translation unit
# gets physics/portable_math_shim.h force-included and links the library.

# The IVP targets live in the submodule's directory scope.
cmake_policy(SET CMP0079 NEW)

option(BMMO_PHYSICS_PORTABLE_MATH "Route the physics code's sin/cos/... through the vendored OpenLibm subset" OFF)

set(_bmmo_pm_root "${CMAKE_CURRENT_LIST_DIR}/../BallanceMMOCommon")

function(bmmo_ensure_portable_math_target)
    if (TARGET bmmo_portable_math)
        return()
    endif ()
    set(_olm "${_bmmo_pm_root}/third_party/openlibm")
    set(_olm_sources
            s_fabs.c s_fabsf.c s_copysign.c s_copysignf.c s_floor.c s_floorf.c s_rint.c
            s_scalbn.c s_scalbnf.c k_rem_pio2.c e_rem_pio2.c e_rem_pio2f.c
            k_sin.c k_cos.c k_tan.c k_sinf.c k_cosf.c k_tanf.c
            s_sin.c s_cos.c s_tan.c s_sinf.c s_cosf.c s_tanf.c
            e_asin.c e_acos.c s_atan.c e_atan2.c e_asinf.c e_acosf.c s_atanf.c e_atan2f.c
            e_exp.c e_expf.c e_log.c e_logf.c e_pow.c e_powf.c e_fmod.c e_fmodf.c)
    list(TRANSFORM _olm_sources PREPEND "${_olm}/src/")
    add_library(bmmo_portable_math STATIC
            "${_olm}/bmmo_portable_math.c"
            ${_olm_sources}
            "${_bmmo_pm_root}/src/physics/portable_math_support.cpp")
    set(_olm_rename "${_olm}/compat/bmmo_olm_rename.h")
    if (MSVC)
        set_source_files_properties(${_olm_sources} PROPERTIES COMPILE_OPTIONS "/FI${_olm_rename}")
    else ()
        set_source_files_properties(${_olm_sources} PROPERTIES COMPILE_OPTIONS "-include;${_olm_rename}")
    endif ()
    target_include_directories(bmmo_portable_math
            PUBLIC "${_bmmo_pm_root}/include"
            PRIVATE "${_bmmo_pm_root}/third_party/openlibm/compat"
                    "${_bmmo_pm_root}/third_party/openlibm/include"
                    "${_bmmo_pm_root}/third_party/openlibm/src"
                    "${_bmmo_pm_root}/third_party/openlibm")
    set_target_properties(bmmo_portable_math PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99)
    # MSVC's C front end refuses to fold floating-point constant expressions
    # in static initializers under /fp:strict (C2099); /fp:precise still
    # means no contraction and no excess precision with SSE2 code.
    if (MSVC)
        target_compile_options(bmmo_portable_math PRIVATE /fp:precise /wd4273 /wd4244 /wd4305 /wd4996)
        if (CMAKE_SIZEOF_VOID_P EQUAL 4)
            target_compile_options(bmmo_portable_math PRIVATE /arch:SSE2)
        endif ()
    else ()
        target_compile_options(bmmo_portable_math PRIVATE
                -ffp-contract=off -fexcess-precision=standard -fno-fast-math -w)
    endif ()
endfunction()

function(bmmo_apply_portable_math target)
    if (NOT BMMO_PHYSICS_PORTABLE_MATH OR NOT TARGET ${target})
        return()
    endif ()
    bmmo_ensure_portable_math_target()
    set(_shim "${_bmmo_pm_root}/include/physics/portable_math_shim.h")
    if (MSVC)
        target_compile_options(${target} PRIVATE "/FI${_shim}")
    else ()
        target_compile_options(${target} PRIVATE "-include" "${_shim}")
    endif ()
    target_include_directories(${target} PRIVATE "${_bmmo_pm_root}/include")
endfunction()

# The IVP static libraries are exported by their own CMake tree, so they
# only receive the force-include; the library is linked where the objects
# are finally consumed (the physics_RT plugin / the headless engine).
function(bmmo_link_portable_math target visibility)
    if (NOT BMMO_PHYSICS_PORTABLE_MATH OR NOT TARGET ${target})
        return()
    endif ()
    bmmo_ensure_portable_math_target()
    target_link_libraries(${target} ${visibility} bmmo_portable_math)
endfunction()

function(bmmo_apply_portable_math_to_all)
    foreach (_target IN LISTS BMMO_IVP_TARGETS ARGN)
        bmmo_apply_portable_math(${_target})
    endforeach ()
endfunction()
