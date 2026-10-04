cmake_minimum_required(VERSION 3.16)
project(cairn-engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

# --------------------------------------------------------------------------
# Engine runtime shared library.
# --------------------------------------------------------------------------
add_library(cairn_engine SHARED
    src/json.cpp
    src/log.cpp
    src/texture.cpp
    src/mesh.cpp
    src/font.cpp
    src/scene.cpp
    src/components.cpp
    src/renderer.cpp
    src/physics.cpp
    src/script_host.cpp
    src/engine.cpp
)
target_include_directories(cairn_engine
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)
target_compile_definitions(cairn_engine PRIVATE CAIRN_ENGINE_BUILDING=1)
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

# --------------------------------------------------------------------------
# Native headless test binary (math, serialization, rasterizer, physics).
# --------------------------------------------------------------------------
enable_testing()
add_executable(cairn_engine_test tests/engine_test.cpp)
target_link_libraries(cairn_engine_test PRIVATE cairn_engine)
target_include_directories(cairn_engine_test PRIVATE src)
add_test(NAME engine COMMAND cairn_engine_test)
