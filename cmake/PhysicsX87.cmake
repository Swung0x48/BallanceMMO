# x87 arithmetic of the physics engine (IVP inside physics_RT), chosen before the BuildingBlocks tree is added.
#
#   BMMO_PHYSICS_X87=SOFT  doyaGu's x87 emulation with the portable software arithmetic and the software model of the
#                          transcendental instructions: the original game's arithmetic rules, identical on every
#                          platform (x86 builds stay SSE2: IVP_X87_MSVC_IA32 OFF).
#   BMMO_PHYSICS_X87=HARD  32-bit x86 only: the arithmetic on the host x87 (results identical to SOFT), the
#                          transcendentals still in software so Intel and AMD hosts agree.
#   BMMO_PHYSICS_X87=OFF   no emulation: ordinary IEEE code with the portable libm (the pre-x87 BallanceMMO engine).
set(BMMO_PHYSICS_X87 SOFT CACHE STRING "Physics engine x87 arithmetic: SOFT, HARD (32-bit x86) or OFF")
set_property(CACHE BMMO_PHYSICS_X87 PROPERTY STRINGS SOFT HARD OFF)
if (BMMO_PHYSICS_X87 STREQUAL "SOFT")
    set(IVP_X87_ARITHMETIC SOFT CACHE STRING "" FORCE)
    set(IVP_X87_ARITHMETIC_TRANSCENDENTAL SOFT CACHE STRING "" FORCE)
elseif (BMMO_PHYSICS_X87 STREQUAL "HARD")
    set(IVP_X87_ARITHMETIC HARD CACHE STRING "" FORCE)
    set(IVP_X87_ARITHMETIC_TRANSCENDENTAL SOFT CACHE STRING "" FORCE)
elseif (BMMO_PHYSICS_X87 STREQUAL "OFF")
    set(IVP_X87_ARITHMETIC OFF CACHE STRING "" FORCE)
    set(IVP_X87_ARITHMETIC_TRANSCENDENTAL OFF CACHE STRING "" FORCE)
else ()
    message(FATAL_ERROR "BMMO_PHYSICS_X87 must be SOFT, HARD or OFF (got '${BMMO_PHYSICS_X87}')")
endif ()
set(IVP_X87_MSVC_IA32 OFF CACHE BOOL "" FORCE)
