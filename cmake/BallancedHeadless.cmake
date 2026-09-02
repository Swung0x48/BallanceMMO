# Static, headless build of the Ballanced engine for the BallanceMMO server.
#
# The engine sources stay byte-for-byte the pinned submodule; this file only
# selects static targets and generates the static plugin registry the same way
# BallancePlayer does (Source/Player/cmake/PlayerStaticModules.cmake).  Input,
# sound and rasterizer modules are replaced by BallanceMMO null managers and
# the engine's built-in NULL rasterizer, so no window, audio device or GPU is
# ever touched.

function(bmmo_add_ballanced_headless BALLANCED_ROOT OUT_TARGET)
    if (TARGET BallanceMMO::BallancedHeadless)
        set(${OUT_TARGET} BallanceMMO::BallancedHeadless PARENT_SCOPE)
        return()
    endif ()

    cmake_path(ABSOLUTE_PATH BALLANCED_ROOT NORMALIZE)
    set(_src "${BALLANCED_ROOT}/Source")
    foreach (_required IN ITEMS
            "${_src}/CK2/CMakeLists.txt"
            "${_src}/BuildingBlocks/physics_RT/CMakeLists.txt"
            "${_src}/Player/cmake/PlayerStaticModules.cmake")
        if (NOT EXISTS "${_required}")
            message(FATAL_ERROR "Ballanced submodule is incomplete: missing ${_required}")
        endif ()
    endforeach ()

    # VxMath uses SDL3 for portable primitives.  Build it statically with every
    # device subsystem disabled; the server never opens a window or audio device.
    if (NOT TARGET SDL3::SDL3)
        include(FetchContent)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
        foreach (_subsystem IN ITEMS AUDIO VIDEO GPU RENDER CAMERA JOYSTICK HAPTIC SENSOR DIALOG)
            set("SDL_${_subsystem}" OFF CACHE BOOL "" FORCE)
        endforeach ()
        FetchContent_Declare(
            bmmo_sdl3
            GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
            GIT_TAG 96292a5b464258a2b926e0a3d72f8b98c2a81aa6)
        FetchContent_MakeAvailable(bmmo_sdl3)
    endif ()

    set(VXMATH_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(VXMATH_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(VXMATH_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(VXMATH_BUILD_PERF OFF CACHE BOOL "" FORCE)
    set(VXMATH_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory("${_src}/VxMath" "${CMAKE_BINARY_DIR}/Ballanced/VxMath")

    set(CK2_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(CK2_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(CK2_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(CK2_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory("${_src}/CK2" "${CMAKE_BINARY_DIR}/Ballanced/CK2")

    # CK2_3D registers the render classes needed to deserialize CMO/NMO files
    # and falls back to its NULL rasterizer when no rasterizer plugin exists.
    set(CKRE_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(CKRE_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(CKRE_BUILD_BGFX_RASTERIZER OFF CACHE BOOL "" FORCE)
    set(CKRE_GENERATE_SHADERS OFF CACHE BOOL "" FORCE)
    set(CKRE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(CKRE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CKRE_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory("${_src}/RenderEngine" "${CMAKE_BINARY_DIR}/Ballanced/RenderEngine")

    set(CKPARAMOP_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(CKPARAMOP_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(CKPARAMOP_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory("${_src}/Managers/ParameterOperations"
                     "${CMAKE_BINARY_DIR}/Ballanced/ParameterOperations")

    set(CKPLUGINS_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(CKPLUGINS_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(CKPLUGINS_BUILD_AVIREADER OFF CACHE BOOL "" FORCE)
    set(CKPLUGINS_BUILD_IMAGEREADER OFF CACHE BOOL "" FORCE)
    set(CKPLUGINS_BUILD_WAVREADER ON CACHE BOOL "" FORCE)
    set(CKPLUGINS_BUILD_VIRTOOLSLOADER ON CACHE BOOL "" FORCE)
    set(CKPLUGINS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(CKPLUGINS_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory("${_src}/Plugins" "${CMAKE_BINARY_DIR}/Ballanced/Plugins")

    set(CKBB_BUILD_ALL_MODULES OFF CACHE BOOL "" FORCE)
    set(CKBB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(CKBB_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(CKBB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(CKBB_INSTALL OFF CACHE BOOL "" FORCE)
    set(_bb_modules
            3DTrans BuildingBlocksAddons1 Cameras Characters Collision Controllers
            Grids Interface Lights Logics Materials-Textures MeshModifiers Narratives
            Sounds Visuals WorldEnvironment physics_RT TT_DatabaseManager_RT
            TT_Gravity_RT TT_InterfaceManager_RT TT_ParticleSystems_RT TT_Toolbox_RT)
    foreach (_module IN LISTS _bb_modules)
        set("CKBB_BUILD_${_module}" ON CACHE BOOL "" FORCE)
    endforeach ()
    set(CKBB_BUILD_MidiManager OFF CACHE BOOL "" FORCE)
    add_subdirectory("${_src}/BuildingBlocks" "${CMAKE_BINARY_DIR}/Ballanced/BuildingBlocks")

    # physics_RT constructs IVP objects whose layouts depend on the platform
    # macros IVP compiles with privately; mirror them at this boundary.
    if (WIN32 AND TARGET physics_RTStatic)
        target_compile_definitions(physics_RTStatic PRIVATE WIN32 _WINDOWS)
    endif ()

    # Generate the static plugin registry from the Player's declarative list,
    # excluding the device-bound modules BallanceMMO replaces.
    include("${_src}/Player/cmake/PlayerStaticModules.cmake")
    set(_skip_modules SdlInputManager SdlSoundManager AVIReader ImageReader
                      MidiManager CKBgfxRasterizer)
    set(_declarations "")
    set(_entries "")
    set(_link_targets)
    foreach (_module IN LISTS PLAYER_STATIC_MODULES)
        if (_module IN_LIST _skip_modules)
            continue()
        endif ()
        set(_prefix "PLAYER_STATIC_MODULE_${_module}")
        if (${_prefix}_LINK_ONLY)
            continue()
        endif ()
        set(_static_target "${${_prefix}_STATIC_TARGET}")
        if (NOT TARGET "${_static_target}")
            message(FATAL_ERROR "Ballanced static target is missing: ${_static_target} (${_module})")
        endif ()
        list(APPEND _link_targets "${_static_target}")
        set(_info_count "${${_prefix}_GET_INFO_COUNT}")
        set(_info "${${_prefix}_GET_INFO}")
        set(_reader "${${_prefix}_GET_READER}")
        set(_register "${${_prefix}_REGISTER_DECLARATIONS}")
        if (_info_count)
            string(APPEND _declarations "extern int ${_info_count}();\n")
        endif ()
        string(APPEND _declarations "extern CKPluginInfo *${_info}(int);\n")
        if (_reader)
            string(APPEND _declarations "extern CKDataReader *${_reader}(int);\n")
        endif ()
        if (_register)
            string(APPEND _declarations "extern void ${_register}(XObjectDeclarationArray *);\n")
        endif ()
        foreach (_field IN ITEMS _info_count _info _reader _register)
            if (NOT ${_field})
                set(${_field}_value "nullptr")
            else ()
                set(${_field}_value "${${_field}}")
            endif ()
        endforeach ()
        string(APPEND _entries
            "    {\"${${_prefix}_DISPLAY_NAME}\", ${_info_count_value}, ${_info_value}, ${_reader_value}, ${_register_value}},\n")
    endforeach ()
    set(BMMO_HEADLESS_PLUGIN_DECLARATIONS "${_declarations}")
    set(BMMO_HEADLESS_PLUGIN_ENTRIES "${_entries}")
    set(_generated_dir "${CMAKE_BINARY_DIR}/Ballanced/generated")
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/headless_plugin_registry.h.in"
                   "${_generated_dir}/bmmo_headless_plugin_registry.h" @ONLY)

    file(GLOB _ivp_include_dirs LIST_DIRECTORIES true
         "${_src}/BuildingBlocks/physics_RT/ivp/*")
    list(FILTER _ivp_include_dirs EXCLUDE REGEX "\.(cxx|hxx|txt|md)$")

    add_library(BallanceMMO_BallancedHeadless INTERFACE)
    add_library(BallanceMMO::BallancedHeadless ALIAS BallanceMMO_BallancedHeadless)
    target_include_directories(BallanceMMO_BallancedHeadless INTERFACE
        "${_generated_dir}"
        "${_src}/BuildingBlocks/physics_RT"
        "${_src}/BuildingBlocks/TT_InterfaceManager_RT"
        ${_ivp_include_dirs})
    target_link_libraries(BallanceMMO_BallancedHeadless INTERFACE
        ${_link_targets}
        CK2_3DStatic
        CK2Static
        VxMathStatic
        SDL3::SDL3-static)
    set(${OUT_TARGET} BallanceMMO::BallancedHeadless PARENT_SCOPE)
endfunction()
