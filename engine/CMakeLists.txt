cmake_minimum_required(VERSION 3.16)
project(cairn-engine VERSION 0.2.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

# --------------------------------------------------------------------------
# Flat output directory for every generator.
#
# Multi-config generators (Visual Studio, Xcode, Ninja Multi-Config) append a
# per-configuration subdirectory to the output directory, so the library ends up
# in engine/build/Release/ instead of engine/build/ and scripts/build.js cannot
# stage it. Setting the per-configuration variables as well as the plain ones
# suppresses that subdirectory and keeps one layout everywhere.
# --------------------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
foreach(_cairn_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_cairn_cfg} ${CMAKE_BINARY_DIR})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_cairn_cfg} ${CMAKE_BINARY_DIR})
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_cairn_cfg} ${CMAKE_BINARY_DIR})
endforeach()
unset(_cairn_cfg)

# --------------------------------------------------------------------------
# Vulkan backend.
#
# The backend resolves libvulkan at runtime (dlopen), so it never links the
# Vulkan loader: the same binary runs on machines with no driver at all and
# simply reports the backend unavailable. What the build does need is the
# Vulkan *headers* (compile time only) and a GLSL->SPIR-V compiler to embed
# the shaders. When either is missing the engine still builds, the renderer
# falls back to the software rasterizer, and `engine.capabilities` says so.
# --------------------------------------------------------------------------
option(CAIRN_ENABLE_VULKAN "Build the offscreen Vulkan rendering backend" ON)

set(CAIRN_VULKAN_ENABLED OFF)
set(CAIRN_GLSLC "")
set(CAIRN_GLSLC_OPT "")
if(CAIRN_ENABLE_VULKAN)
    find_package(Vulkan QUIET)
    # Re-search every configure: installing the shader compiler after a first
    # build should turn the backend on without wiping the cache.
    unset(CAIRN_GLSLC)
    unset(CAIRN_GLSLC CACHE)
    find_program(CAIRN_GLSLC NAMES glslc HINTS ${Vulkan_GLSLC_EXECUTABLE})
    if(NOT CAIRN_GLSLC)
        find_program(CAIRN_GLSLC NAMES glslangValidator)
    endif()
    if(Vulkan_FOUND AND CAIRN_GLSLC)
        set(CAIRN_VULKAN_ENABLED ON)
        message(STATUS "cairn: Vulkan backend enabled (${CAIRN_GLSLC})")
    else()
        message(STATUS "cairn: Vulkan backend disabled "
                       "(vulkan headers: ${Vulkan_FOUND}, glslc: ${CAIRN_GLSLC})")
    endif()
endif()

# Extra sources (generated shader header + the backend) when Vulkan is on.
set(CAIRN_ENGINE_SOURCES "")

# Every GLSL stage in engine/shaders becomes one embedded SPIR-V blob.
set(CAIRN_SHADER_STAGES
    mesh.vert    mesh.frag
    shadow.vert  shadow.frag
    sprite.vert  sprite.frag
    sky.vert     sky.frag
    line.vert    line.frag
    bright.frag  blur.frag     composite.frag
    grading.frag vignette.frag fxaa.frag
)

if(CAIRN_VULKAN_ENABLED)
    set(CAIRN_SPIRV_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
    set(CAIRN_SHADERS_HEADER ${CMAKE_CURRENT_BINARY_DIR}/shaders_spirv.h)
    set(CAIRN_SPIRV_FILES "")
    foreach(rel IN LISTS CAIRN_SHADER_STAGES)
        if(rel MATCHES "\\.vert$")
            set(stage vertex)
        else()
            set(stage fragment)
        endif()
        set(src ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${rel})
        set(out ${CAIRN_SPIRV_DIR}/${rel}.spv)
        add_custom_command(
            OUTPUT ${out}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CAIRN_SPIRV_DIR}
            COMMAND ${CAIRN_GLSLC} -fshader-stage=${stage} --target-env=vulkan1.3 ${CAIRN_GLSLC_OPT}
                    ${src} -o ${out}
            DEPENDS ${src} ${CAIRN_GLSLC}
            COMMENT "glslc ${rel}"
            VERBATIM
        )
        list(APPEND CAIRN_SPIRV_FILES ${out})
    endforeach()
    # One generated header holds every SPIR-V word array plus the lookup table.
    # The stage list crosses the -D boundary comma-separated: a semicolon would
    # be re-split into a different list on the way.
    string(REPLACE ";" "," CAIRN_SHADER_STAGES_CSV "${CAIRN_SHADER_STAGES}")
    add_custom_command(
        OUTPUT ${CAIRN_SHADERS_HEADER}
        COMMAND ${CMAKE_COMMAND} -DSPV_DIR=${CAIRN_SPIRV_DIR}
                                 -DSPV_OUT=${CAIRN_SHADERS_HEADER}
                                 "-DSPV_LIST=${CAIRN_SHADER_STAGES_CSV}"
                                 -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake
        DEPENDS ${CAIRN_SPIRV_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake
        COMMENT "embed SPIR-V -> shaders_spirv.h"
        VERBATIM
    )
    list(APPEND CAIRN_ENGINE_SOURCES ${CAIRN_SHADERS_HEADER})
    set_source_files_properties(${CAIRN_SHADERS_HEADER} PROPERTIES
        GENERATED TRUE HEADER_FILE_ONLY TRUE)
endif()

# --------------------------------------------------------------------------
# Engine runtime shared library.
# --------------------------------------------------------------------------
add_library(cairn_engine SHARED
    src/json.cpp
    src/log.cpp
    src/asset.cpp
    src/texture.cpp
    src/mesh.cpp
    src/font.cpp
    src/scene.cpp
    src/components.cpp
    src/prefab.cpp
    src/renderer.cpp
    src/backend_vulkan.cpp
    src/physics.cpp
    src/jobs.cpp
    src/profiler.cpp
    src/script_host.cpp
    src/engine.cpp
    ${CAIRN_ENGINE_SOURCES}
)
target_include_directories(cairn_engine
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)
target_compile_definitions(cairn_engine PRIVATE CAIRN_ENGINE_BUILDING=1)
if(CAIRN_VULKAN_ENABLED)
    target_compile_definitions(cairn_engine PRIVATE CAIRN_HAS_VULKAN=1)
    target_include_directories(cairn_engine PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    # dlopen moved into libc on glibc >= 2.34; older Linux still needs -ldl.
    include(CheckLibraryExists)
    check_library_exists(dl dlopen "" CAIRN_NEEDS_LIBDL)
    if(CAIRN_NEEDS_LIBDL)
        target_link_libraries(cairn_engine PRIVATE ${CMAKE_DL_LIBS})
    endif()
endif()
# STB_GNU_UNIQUE statics survive dlclose, which breaks script hot reload
# (fresh libraries would register into a stale registry). See docs/architecture.
if(UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(cairn_engine PRIVATE -fno-gnu-unique)
endif()
set_target_properties(cairn_engine PROPERTIES
    OUTPUT_NAME cairn_engine
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
)

# Windows shared libraries export nothing by default; gameplay scripts and the
# addon link against cairn_engine.lib, so every public symbol must be exported.
if(WIN32)
    target_compile_definitions(cairn_engine PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    set_target_properties(cairn_engine PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

# --------------------------------------------------------------------------
# Native headless test binary (math, serialization, rasterizer, physics).
# --------------------------------------------------------------------------
enable_testing()
add_executable(cairn_engine_test tests/engine_test.cpp)
target_link_libraries(cairn_engine_test PRIVATE cairn_engine)
target_include_directories(cairn_engine_test PRIVATE src)
add_test(NAME engine COMMAND cairn_engine_test)