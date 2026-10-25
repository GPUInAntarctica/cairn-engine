#include "shader_library.h"

#include "cairn/log.h"

#include <fstream>
#include <sstream>

namespace Cairn {

namespace {

struct Preset {
    const char* name;
    const char* du;
    const char* dv;
    const char* emissive;
};

// Named effects that cover the common cases without a file. The parameters a
// preset reads are documented in docs/shaders.md; shaderParams defaults to
// (1,1,1,1), so a preset with no tuning does something visible.
const Preset kPresets[] = {
    // Water-like distortion: both axes move, driven by the other axis so the
    // pattern rolls instead of sliding. p0 = speed, p1 = amplitude scale.
    {"@ripple",
     "sin((v + time * p0) * 12.0) * (0.03 * p1)",
     "cos((u + time * p0) * 12.0) * (0.03 * p1)",
     "0"},
    // Scrolling UVs: p0 = u speed, p1 = v speed, in texture widths per second.
    {"@scroll", "time * 0.05 * p0", "time * 0.05 * p1", "0"},
    // Rim light: emissive colour appears on grazing angles. p0 = strength.
    {"@fresnel", "0", "0", "p0 * pow(1.0 - ndotv, 3.0)"},
    // Breathing glow on the material's emissive colour. p0 = strength.
    {"@pulse", "0", "0", "p0 * (0.5 + 0.5 * sin(time * 3.0))"},
};

bool ReadFile(const std::string& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

}  // namespace

std::shared_ptr<MaterialShader> ShaderLibrary::Load(const std::string& key, Entry* entry) {
    std::string error;
    auto shader = std::make_shared<MaterialShader>();
    shader->name = key;

    if (!key.empty() && key[0] == '@') {
        const Preset* found = nullptr;
        for (const Preset& p : kPresets) {
            if (key == p.name) { found = &p; break; }
        }
        if (!found) {
            error = "unknown shader preset '" + key + "'";
        } else if (!CompileShaderSource(found->du, found->dv, found->emissive, shader.get(),
                                        &error)) {
            error = key + ": " + error;
        }
        entry->hasFile = false;
    } else {
        std::error_code ec;
        entry->mtime = std::filesystem::last_write_time(key, ec);
        entry->hasFile = !ec;
        std::string text;
        if (!entry->hasFile) {
            error = "shader not found: " + key;
        } else if (!ReadFile(key, &text)) {
            error = "shader could not be read: " + key;
        } else {
            Json doc;
            std::string parseError;
            if (!Json::Parse(text, &doc, &parseError)) {
                error = key + ": " + parseError;
            } else if (!CompileShaderDocument(doc, shader.get(), &error)) {
                error = key + ": " + error;
            }
        }
    }

    if (!error.empty()) {
        entry->shader.reset();
        entry->error = error;
        Log::Warning("%s", error.c_str());
        return nullptr;
    }
    entry->error.clear();
    entry->shader = shader;
    return shader;
}

std::shared_ptr<MaterialShader> ShaderLibrary::Get(const std::string& pathOrPreset,
                                                   std::string* error) {
    const std::string key = pathOrPreset;
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        Entry& e = it->second;
        bool fresh = true;
        if (e.hasFile) {
            std::error_code ec;
            const auto mtime = std::filesystem::last_write_time(key, ec);
            fresh = !ec && mtime == e.mtime;
        }
        if (fresh) {
            if (error) *error = e.error;
            return e.shader;
        }
        cache_.erase(it);
    }

    Entry entry;
    auto shader = Load(key, &entry);
    if (entry.shader || !entry.error.empty()) cache_[key] = entry;
    if (error) *error = entry.error;
    return shader;
}

void ShaderLibrary::ReloadChanged() {
    std::error_code ec;
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (!it->second.hasFile) {
            // Presets never change; a path that failed to load is retried on
            // reload so creating the file next to a scene picks it up.
            if (!it->first.empty() && it->first[0] != '@') it = cache_.erase(it);
            else ++it;
            continue;
        }
        const auto mtime = std::filesystem::last_write_time(it->first, ec);
        if (ec || mtime != it->second.mtime) it = cache_.erase(it);
        else ++it;
    }
}

void ShaderLibrary::Clear() { cache_.clear(); }

}  // namespace Cairn
