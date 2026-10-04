// Cairn Engine - script host: loads compiled gameplay libraries (dlopen) and
// exposes the registered script classes. Supports hot reload via file copies.
#pragma once

#include "cairn/scripting.h"

#include <string>
#include <vector>

namespace Cairn {

class ScriptHost {
public:
    ScriptHost() = default;
    ~ScriptHost() { Unload(); }
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    // Loads (or hot-reloads) a compiled script shared library.
    bool Load(const std::string& absolutePath, std::string* error);
    void Unload();
    bool IsLoaded() const { return handle_ != nullptr; }

    const std::vector<ScriptClassRecord>& Records() const { return records_; }
    const ScriptClassRecord* Find(const std::string& className) const;
    Component* Create(const std::string& className) const;

private:
    void* handle_ = nullptr;
    std::string loadedCopy_;  // unique file copy backing this load
    std::vector<ScriptClassRecord> records_;
    int loadCounter_ = 0;
};

}  // namespace Cairn
