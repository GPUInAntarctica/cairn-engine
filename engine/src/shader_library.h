// Cairn Engine - material shader cache (internal).
#pragma once

#include "material_shader.h"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

namespace Cairn {

// Caches compiled .cshader programs by absolute path and by "@preset" name.
// Compilation happens once per file revision: editing a shader and calling
// assetsReload drops the changed entry, so the next frame recompiles it.
class ShaderLibrary {
public:
    // Returns the compiled program, or nullptr when the file is missing or the
    // expressions do not compile. The error is reported once per revision; a
    // broken shader does not flood the log every frame.
    std::shared_ptr<MaterialShader> Get(const std::string& pathOrPreset,
                                        std::string* error = nullptr);
    void ReloadChanged();
    void Clear();

private:
    struct Entry {
        std::shared_ptr<MaterialShader> shader;
        std::filesystem::file_time_type mtime{};
        bool hasFile = false;
        std::string error;
    };
    std::shared_ptr<MaterialShader> Load(const std::string& key, Entry* entry);
    std::unordered_map<std::string, Entry> cache_;
};

// Process-wide cache, like the mesh and texture libraries. Declared by the
// engine translation unit so renderer code cannot create a second one.
ShaderLibrary& EngineShaderLibrary();

}  // namespace Cairn
