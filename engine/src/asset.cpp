#include "cairn/asset.h"

#include "cairn/engine.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace fs = std::filesystem;

namespace Cairn {

namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string ExtensionOf(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    return Lower(ext);
}

bool ReadFileText(const std::string& absolute, std::string* out, std::string* error) {
    std::ifstream in(absolute, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open file: " + absolute;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

bool WriteFileText(const std::string& absolute, const std::string& text, std::string* error) {
    fs::path p(absolute);
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream out(absolute, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "cannot write file: " + absolute;
        return false;
    }
    out.write(text.data(), (std::streamsize)text.size());
    return out.good();
}

}  // namespace

// ---------------------------------------------------------------------------
// Static helpers.
// ---------------------------------------------------------------------------
std::string AssetDatabase::NewGuid() {
    // 128 random bits rendered as 32 lowercase hex chars. The value lives in the
    // .meta sidecar (committed with the project), so it only has to be unique,
    // not reproducible.
    static thread_local std::mt19937_64 rng([] {
        std::random_device rd;
        return (uint64_t)rd() ^ ((uint64_t)rd() << 32);
    }());
    uint64_t hi = rng(), lo = rng();
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (int i = 0; i < 16; ++i) out.push_back(hex[(hi >> (60 - 4 * i)) & 0xF]);
    for (int i = 0; i < 16; ++i) out.push_back(hex[(lo >> (60 - 4 * i)) & 0xF]);
    return out;
}

std::string AssetDatabase::ImporterForPath(const std::string& projectPath) {
    const std::string ext = ExtensionOf(projectPath);
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga") return "texture";
    if (ext == ".obj" || ext == ".gltf" || ext == ".glb" || ext == ".fbx") return "model";
    if (ext == ".wav" || ext == ".ogg" || ext == ".mp3") return "audio";
    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".h" || ext == ".hpp") return "script";
    if (ext == ".cscene") return "scene";
    if (ext == ".prefab") return "prefab";
    return "other";
}

std::string AssetDatabase::MetaPathFor(const std::string& projectPath) { return projectPath + ".meta"; }

bool AssetDatabase::IsMetaFile(const std::string& path) { return ExtensionOf(path) == ".meta"; }

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------
void AssetDatabase::SetProjectRoot(std::string root) {
    projectRoot_ = std::move(root);
}

std::string AssetDatabase::Absolute(const std::string& projectPath) const {
    if (projectPath.empty()) return projectRoot_;
    if (projectPath[0] == '/' || (projectPath.size() > 1 && projectPath[1] == ':')) return projectPath;
    if (projectRoot_.empty()) return projectPath;
    return (fs::path(projectRoot_) / projectPath).string();
}

void AssetDatabase::Scan() {
    entries_.clear();
    guidByPath_.clear();
    pathByGuid_.clear();

    const fs::path assets = fs::path(projectRoot_) / "Assets";
    std::error_code ec;
    if (!fs::exists(assets, ec) || !fs::is_directory(assets, ec)) return;

    std::vector<std::string> files;
    for (fs::recursive_directory_iterator it(assets, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        const std::string abs = it->path().string();
        if (IsMetaFile(abs)) continue;
        std::error_code relEc;
        std::string rel = fs::relative(it->path(), fs::path(projectRoot_), relEc).generic_string();
        if (!relEc && !rel.empty()) files.push_back(rel);
    }
    // Stable order keeps the index (and any serialized listing) deterministic.
    std::sort(files.begin(), files.end());
    for (const std::string& rel : files) IndexAsset(rel);
}

void AssetDatabase::IndexAsset(const std::string& projectPath) {
    AssetMeta meta;
    const std::string abs = Absolute(projectPath);
    const std::string metaAbs = abs + ".meta";
    bool haveMeta = false;
    std::error_code ec;
    if (fs::exists(metaAbs, ec)) {
        std::string text;
        std::string err;
        if (ReadFileText(metaAbs, &text, &err)) {
            Json doc;
            if (Json::Parse(text, &doc, nullptr)) {
                meta.guid = doc["guid"].asString();
                meta.importer = doc["importer"].asString(ImporterForPath(projectPath).c_str());
                meta.version = doc["version"].asInt(1);
                if (doc["settings"].isObject()) meta.settings = doc["settings"];
                haveMeta = !meta.guid.empty();
            }
        }
    }
    if (!haveMeta || meta.guid.empty() || pathByGuid_.count(meta.guid)) {
        meta.guid = NewGuid();
        meta.importer = ImporterForPath(projectPath);
        WriteMeta(projectPath, meta, nullptr);
    } else if (meta.importer.empty()) {
        meta.importer = ImporterForPath(projectPath);
        WriteMeta(projectPath, meta, nullptr);
    }
    entries_.push_back({meta.guid, projectPath, meta.importer});
    guidByPath_[projectPath] = meta.guid;
    pathByGuid_[meta.guid] = projectPath;
}

void AssetDatabase::RebuildIndex() {
    entries_.clear();
    guidByPath_.clear();
    pathByGuid_.clear();
}

bool AssetDatabase::HasGuid(const std::string& guid) const { return pathByGuid_.count(guid) > 0; }
bool AssetDatabase::HasPath(const std::string& projectPath) const { return guidByPath_.count(projectPath) > 0; }

std::string AssetDatabase::GuidForPath(const std::string& projectPath) const {
    auto it = guidByPath_.find(projectPath);
    return it == guidByPath_.end() ? std::string() : it->second;
}

std::string AssetDatabase::PathForGuid(const std::string& guid) const {
    auto it = pathByGuid_.find(guid);
    return it == pathByGuid_.end() ? std::string() : it->second;
}

// ---------------------------------------------------------------------------
// .meta I/O.
// ---------------------------------------------------------------------------
bool AssetDatabase::ReadMeta(const std::string& projectPath, AssetMeta* out) const {
    if (!out) return false;
    const std::string abs = Absolute(projectPath) + ".meta";
    std::error_code ec;
    if (!fs::exists(abs, ec)) return false;
    std::string text;
    std::string err;
    if (!ReadFileText(abs, &text, &err)) return false;
    Json doc;
    if (!Json::Parse(text, &doc, nullptr)) return false;
    out->guid = doc["guid"].asString();
    out->importer = doc["importer"].asString(ImporterForPath(projectPath).c_str());
    out->version = doc["version"].asInt(1);
    out->settings = doc["settings"].isObject() ? doc["settings"] : Json::MakeObject();
    return !out->guid.empty();
}

bool AssetDatabase::WriteMeta(const std::string& projectPath, const AssetMeta& meta, std::string* error) {
    Json doc = Json::MakeObject();
    doc.set("format", "cairn-asset");
    doc.set("version", 1);
    doc.set("guid", meta.guid);
    doc.set("importer", meta.importer.empty() ? ImporterForPath(projectPath) : meta.importer);
    doc.set("settings", meta.settings.isObject() ? meta.settings : Json::MakeObject());
    return WriteFileText(Absolute(projectPath) + ".meta", doc.Dump(2) + "\n", error);
}

std::string AssetDatabase::EnsureMeta(const std::string& projectPath, std::string* error) {
    if (projectPath.empty()) {
        if (error) *error = "empty asset path";
        return "";
    }
    AssetMeta meta;
    if (ReadMeta(projectPath, &meta)) return meta.guid;
    meta.guid = NewGuid();
    meta.importer = ImporterForPath(projectPath);
    meta.settings = Json::MakeObject();
    if (!WriteMeta(projectPath, meta, error)) return "";
    return meta.guid;
}

// ---------------------------------------------------------------------------
// File operations.
// ---------------------------------------------------------------------------
std::string AssetDatabase::ImportFile(const std::string& absoluteSource, const std::string& projectRelativeDest,
                                      std::string* error) {
    std::error_code ec;
    if (!fs::exists(absoluteSource, ec)) {
        if (error) *error = "source not found: " + absoluteSource;
        return "";
    }
    const std::string destAbs = Absolute(projectRelativeDest);
    fs::path destPath(destAbs);
    if (destPath.has_parent_path()) fs::create_directories(destPath.parent_path(), ec);
    fs::copy_file(absoluteSource, destAbs, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        if (error) *error = "copy failed: " + ec.message();
        return "";
    }
    std::string guid = EnsureMeta(projectRelativeDest, error);
    Scan();
    return guid;
}

bool AssetDatabase::MoveAsset(const std::string& fromRel, const std::string& toRel, std::string* error) {
    if (fromRel.empty() || toRel.empty()) {
        if (error) *error = "source and destination are required";
        return false;
    }
    const std::string fromAbs = Absolute(fromRel);
    const std::string toAbs = Absolute(toRel);
    std::error_code ec;
    if (!fs::exists(fromAbs, ec)) {
        if (error) *error = "asset not found: " + fromRel;
        return false;
    }
    if (fs::exists(toAbs, ec)) {
        if (error) *error = "destination exists: " + toRel;
        return false;
    }
    fs::path toPath(toAbs);
    if (toPath.has_parent_path()) fs::create_directories(toPath.parent_path(), ec);
    fs::rename(fromAbs, toAbs, ec);
    if (ec) {
        if (error) *error = "move failed: " + ec.message();
        return false;
    }
    // Carry the .meta along so the GUID is preserved.
    const std::string fromMeta = fromAbs + ".meta";
    const std::string toMeta = toAbs + ".meta";
    if (fs::exists(fromMeta, ec)) {
        if (fs::exists(toMeta, ec)) fs::remove(toMeta, ec);
        fs::rename(fromMeta, toMeta, ec);
    }
    Scan();
    return true;
}

bool AssetDatabase::DeleteAsset(const std::string& projectPath, std::string* error) {
    const std::string abs = Absolute(projectPath);
    std::error_code ec;
    if (!fs::exists(abs, ec)) {
        if (error) *error = "asset not found: " + projectPath;
        return false;
    }
    fs::remove(abs, ec);
    fs::remove(abs + ".meta", ec);
    Scan();
    return true;
}

// ---------------------------------------------------------------------------
// Reference JSON.
// ---------------------------------------------------------------------------
Json AssetRefToJson(const std::string& projectPath) {
    if (projectPath.empty()) return Json("");
    AssetDatabase* db = EngineAssetDatabase();
    const std::string guid = db ? db->GuidForPath(projectPath) : std::string();
    if (guid.empty()) return Json(projectPath);  // no .meta yet: keep the plain path
    Json ref = Json::MakeObject();
    ref.set("path", projectPath);
    ref.set("guid", guid);
    return ref;
}

std::string AssetRefFromJson(const Json& value) {
    if (value.isString()) return value.asString();
    if (value.isObject()) {
        const std::string guid = value["guid"].asString();
        if (!guid.empty()) {
            if (AssetDatabase* db = EngineAssetDatabase()) {
                const std::string resolved = db->PathForGuid(guid);
                if (!resolved.empty()) return resolved;
            }
        }
        return value["path"].asString();
    }
    return {};
}

}  // namespace Cairn
