// Cairn Engine - version of this build.
//
// One definition travels with the sources: the addon reports it to the editor
// and the CLI, so a release cannot leave a stale number in the module. The
// build suite compares these values with package.json and with the
// project(... VERSION) line in engine/CMakeLists.txt.
#pragma once

#define CAIRN_VERSION_MAJOR 0
#define CAIRN_VERSION_MINOR 2
#define CAIRN_VERSION_PATCH 8
#define CAIRN_VERSION_STRING "0.2.8"

namespace Cairn {

inline constexpr const char* kEngineVersion = CAIRN_VERSION_STRING;

}  // namespace Cairn
