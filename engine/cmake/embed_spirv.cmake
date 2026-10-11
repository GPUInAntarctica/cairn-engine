# Cairn Engine - embeds compiled SPIR-V shader modules into a generated header.
#
# Run in script mode from engine/CMakeLists.txt:
#   cmake -DSPV_DIR=<dir> -DSPV_OUT=<header> -DSPV_LIST=a.vert,b.frag -P embed_spirv.cmake
#
# The header is self-contained (declarations + data + lookup) so the backend
# needs a single include and the build needs no tooling beyond cmake itself.
#
# Modules are stored as byte arrays rather than uint32_t arrays: file(READ
# ... HEX) already yields bytes in file order, so no endian fixup is needed and
# the generated source stays smaller. alignas(4) gives the pointer the 4-byte
# alignment VkShaderModuleCreateInfo::pCode requires.

if(NOT DEFINED SPV_DIR OR NOT DEFINED SPV_OUT)
    message(FATAL_ERROR "embed_spirv.cmake: SPV_DIR and SPV_OUT are required")
endif()

string(REPLACE "," ";" _stages "${SPV_LIST}")
set(_body "")
set(_table "")
foreach(_rel IN LISTS _stages)
    get_filename_component(_stage "${_rel}" EXT)
    string(REGEX REPLACE "^\\." "" _stage "${_stage}")
    set(_in "${SPV_DIR}/${_rel}.spv")
    if(NOT EXISTS "${_in}")
        message(FATAL_ERROR "embed_spirv.cmake: missing SPIR-V module ${_in}")
    endif()
    file(READ "${_in}" _hex HEX)
    string(LENGTH "${_hex}" _hexlen)
    if(_hexlen EQUAL 0)
        message(FATAL_ERROR "embed_spirv.cmake: empty SPIR-V module ${_in}")
    endif()
    math(EXPR _bytelen "${_hexlen} / 2")
    # The trailing comma is valid C++ inside a braced initializer list, which
    # keeps this free of a regex over a half-megabyte string (CMake's regex
    # path has an input length limit and would silently truncate).
    string(REGEX REPLACE "(..)" "0x\\1, " _bytes "${_hex}")
    # C identifier: "mesh.vert" -> kSpirv_mesh_vert
    string(REGEX REPLACE "[^A-Za-z0-9]" "_" _ident "${_rel}")
    if(_stage STREQUAL "vert")
        set(_vk "VK_SHADER_STAGE_VERTEX_BIT")
    else()
        set(_vk "VK_SHADER_STAGE_FRAGMENT_BIT")
    endif()
    string(APPEND _body "alignas(4) static const uint8_t kSpirv_${_ident}[] = {${_bytes}};\n\n")
    string(APPEND _table "    {\"${_rel}\", ${_vk}, kSpirv_${_ident}, ${_bytelen}},\n")
endforeach()

get_filename_component(_outdir "${SPV_OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_outdir}")
file(WRITE "${SPV_OUT}"
"// Cairn Engine - GENERATED FILE, do not edit.
// Produced by engine/cmake/embed_spirv.cmake from engine/shaders/*.glsl via glslc.
// Each entry is one compiled SPIR-V module plus the stage tag the backend
// matches against VkShaderStageFlagBits when it builds a VkShaderModule.
#pragma once

#include <cstddef>
#include <cstdint>

#include <vulkan/vulkan.h>

namespace Cairn {
namespace spirv {

${_body}struct Module {
    const char* name;      // \"mesh.vert\", \"mesh.frag\", ...
    VkShaderStageFlagBits stage;
    const void* code;
    size_t byteCount;
};

// All compiled stages, in a stable order.
inline const Module* Modules(size_t* count) {
    static const Module kModules[] = {
${_table}    };
    if (count) *count = sizeof(kModules) / sizeof(kModules[0]);
    return kModules;
}

// Returns nullptr when no stage with this name was compiled in.
inline const Module* Find(const char* name) {
    size_t count = 0;
    const Module* all = Modules(&count);
    for (size_t i = 0; i < count; ++i) {
        size_t k = 0;
        while (name[k] && all[i].name[k] && name[k] == all[i].name[k]) ++k;
        if (name[k] == '\\0' && all[i].name[k] == '\\0') return &all[i];
    }
    return nullptr;
}

}  // namespace spirv
}  // namespace Cairn
")