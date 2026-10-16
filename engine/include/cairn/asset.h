// Cairn Engine - asset database: stable GUIDs and .meta import settings.
//
// Every file under <project>/Assets gets a sidecar .meta file holding a stable
// 32-hex GUID and importer settings. References stored on components can then
// survive a rename or move of the file: the scene keeps the GUID alongside the
// path and resolves it through this index at load time. This is the foundation
// prefabs, addressables and per-asset import settings build on.
#pragma once

#include "cairn/json.h"

#include <map>
#include <string>
#include <vector>

namespace Cairn {

// The parsed contents of one asset's .meta sidecar.
struct AssetMeta {
    std::string guid;                     // 32 lowercase hex chars
    std::string importer;                 // texture | model | audio | script | scene | prefab | other
    int version = 1;
    Json settings = Json::MakeObject();   // importer-specific (colorSpace, wrap, ...)
};

struct AssetEntry {
    std::string guid;
    std::string path;        // project-relative POSIX path
    std::string importer;
};

// Owns the guid<->path index for one project and reads/writes .meta files.
class AssetDatabase {
public:
    AssetDatabase() = default;
    explicit AssetDatabase(std::string projectRoot) { SetProjectRoot(std::move(projectRoot)); }

    void SetProjectRoot(std::string root);
    const std::string& projectRoot() const { return projectRoot_; }

    // Walks <root>/Assets, creating a .meta for every asset that lacks one and
    // rebuilding the guid<->path index. Safe to call repeatedly; cheap when the
    // tree is unchanged.
    void Scan();

    bool HasGuid(const std::string& guid) const;
    bool HasPath(const std::string& projectPath) const;

    // Project-relative POSIX paths in and out. Empty string when unknown.
    std::string GuidForPath(const std::string& projectPath) const;
    std::string PathForGuid(const std::string& guid) const;

    const std::vector<AssetEntry>& entries() const { return entries_; }

    bool ReadMeta(const std::string& projectPath, AssetMeta* out) const;
    bool WriteMeta(const std::string& projectPath, const AssetMeta& meta, std::string* error = nullptr);
    // Creates a .meta when missing (generating a fresh GUID); returns the GUID
    // or an empty string on failure.
    std::string EnsureMeta(const std::string& projectPath, std::string* error = nullptr);

    // Copies `absoluteSource` to projectRelativeDest (creating directories),
    // then creates its .meta. Returns the GUID, or "" on failure.
    std::string ImportFile(const std::string& absoluteSource, const std::string& projectRelativeDest,
                           std::string* error = nullptr);
    // Moves an asset together with its .meta; the GUID is preserved.
    bool MoveAsset(const std::string& fromRel, const std::string& toRel, std::string* error = nullptr);
    bool DeleteAsset(const std::string& projectPath, std::string* error = nullptr);

    static std::string NewGuid();
    static std::string ImporterForPath(const std::string& projectPath);
    static std::string MetaPathFor(const std::string& projectPath);   // path + ".meta"
    static bool IsMetaFile(const std::string& path);

private:
    void IndexAsset(const std::string& projectPath);
    void RebuildIndex();
    std::string Absolute(const std::string& projectPath) const;

    std::string projectRoot_;
    std::vector<AssetEntry> entries_;
    std::map<std::string, std::string> guidByPath_;
    std::map<std::string, std::string> pathByGuid_;
};

// The active engine's database, or null when no engine exists.
AssetDatabase* EngineAssetDatabase();

// Asset-reference JSON helpers used by scene serialization. A reference is
// serialized as an object {"path","guid"} when the database knows the GUID, and
// as a bare path string otherwise. AssetRefFromJson accepts either form and
// prefers the GUID (so a moved/renamed file still resolves).
Json AssetRefToJson(const std::string& projectPath);
std::string AssetRefFromJson(const Json& value);

}  // namespace Cairn
