#include "script_host.h"

#include "cairn/engine.h"
#include "cairn/log.h"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace Cairn {

namespace {

// The engine shared library must be in the global symbol scope so gameplay
// libraries (loaded later) can bind to its exports.
void PromoteEngineLibraryToGlobal() {
#if !defined(_WIN32)
    Dl_info info;
    if (dladdr((void*)(void*)&EngineSetTimeScale, &info) && info.dli_fname) {
        // RTLD_NOLOAD promotes the already-loaded library to RTLD_GLOBAL.
        void* h = dlopen(info.dli_fname, RTLD_NOLOAD | RTLD_LAZY | RTLD_GLOBAL);
        if (h) dlclose(h);  // drop the extra refcount; promotion persists
    }
#endif
}

void* OpenLibrary(const char* path) {
#if defined(_WIN32)
    return (void*)LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

void CloseLibrary(void* handle) {
#if defined(_WIN32)
    if (handle) FreeLibrary((HMODULE)handle);
#else
    if (handle) dlclose(handle);
#endif
}

void* FindSymbol(void* handle, const char* name) {
#if defined(_WIN32)
    return (void*)GetProcAddress((HMODULE)handle, name);
#else
    return dlsym(handle, name);
#endif
}

// Copies the library to a unique path so a rebuilt file is never served from
// the dynamic loader's cache (the standard hot-reload trick).
bool CopyFileBytes(const std::string& from, const std::string& to) {
    FILE* in = std::fopen(from.c_str(), "rb");
    if (!in) return false;
    FILE* out = std::fopen(to.c_str(), "wb");
    if (!out) { std::fclose(in); return false; }
    char buf[65536];
    size_t n;
    bool ok = true;
    while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) {
        if (std::fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    std::fclose(in);
    std::fclose(out);
    return ok;
}

void RemoveFile(const std::string& path) { std::remove(path.c_str()); }

}  // namespace

bool ScriptHost::Load(const std::string& absolutePath, std::string* error) {
    Unload();
    PromoteEngineLibraryToGlobal();

    std::string copyPath = absolutePath;
#if !defined(_WIN32)
    // Unique copy per load.
    char suffix[64];
    std::snprintf(suffix, sizeof(suffix), ".%d.so", ++loadCounter_ + (int)getpid() % 100000);
    copyPath = absolutePath + suffix;
    if (!CopyFileBytes(absolutePath, copyPath)) {
        if (error) *error = "cannot copy script library: " + absolutePath;
        return false;
    }
    loadedCopy_ = copyPath;
#endif

    void* handle = OpenLibrary(copyPath.c_str());
    if (!handle) {
#if defined(_WIN32)
        if (error) *error = "LoadLibrary failed for " + absolutePath;
#else
        const char* err = dlerror();
        if (error) *error = err ? err : ("dlopen failed for " + absolutePath);
#endif
        if (!loadedCopy_.empty()) RemoveFile(loadedCopy_);
        loadedCopy_.clear();
        return false;
    }
    handle_ = handle;

    using QueryFn = const std::vector<ScriptClassRecord>* (*)();
    auto query = (QueryFn)FindSymbol(handle_, "CairnQueryScriptRegistry");
    records_.clear();
    if (!query) {
        if (error) *error = "script library exports no CairnQueryScriptRegistry (no scripts registered?)";
        return true;  // library loaded; simply no classes
    }
    const std::vector<ScriptClassRecord>* reg = query();
    if (reg) records_ = *reg;
    return true;
}

void ScriptHost::Unload() {
    if (handle_) {
        CloseLibrary(handle_);
        handle_ = nullptr;
    }
    if (!loadedCopy_.empty()) {
        RemoveFile(loadedCopy_);
        loadedCopy_.clear();
    }
    records_.clear();
}

const ScriptClassRecord* ScriptHost::Find(const std::string& className) const {
    for (const auto& r : records_)
        if (className == r.name) return &r;
    return nullptr;
}

Component* ScriptHost::Create(const std::string& className) const {
    const ScriptClassRecord* r = Find(className);
    if (!r || !r->create) return nullptr;
    return r->create();
}

}  // namespace Cairn
