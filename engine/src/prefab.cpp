// Cairn Engine - prefabs and per-property overrides.
//
// A prefab file is a serialized subtree. An instance is a copy of that subtree
// plus a record of how it differs from the asset (PrefabInstance::overrides).
// The scene is saved with that record, and loading the scene pulls the asset
// back in for everything the instance did *not* change - so editing a .prefab
// reaches every instance that left the property alone, while a per-instance
// tweak survives both the edit and a round-trip through the scene file.
//
// The record is a plain diff of two serialized subtrees, which is why it also
// catches changes made through paths that never go through a property setter
// (scripts, physics, direct inspector writes). It is keyed by node path ("" is
// the instance root, "Lid/Bolt" a child) and then by property key:
//
//   "@name" / "@active" / "@tag" / "@layer"   the entity's own fields
//   "MeshRenderer.albedoColor"                a component property
//   "MeshRenderer.@enabled"                   a component's enabled flag
//   "Transform.position" / "Script.speed"     the type's special forms
//   "@removedComponents" / "@removedChildren" names the instance dropped
#include "cairn/engine.h"

#include "scene_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Cairn {

namespace {

// ---------------------------------------------------------------------------
// JSON helpers.
// ---------------------------------------------------------------------------
// Values are reference-counted, so anything taken from a document that outlives
// the write has to be cloned: two instances must not end up sharing a script
// property, and a stored override must not follow a later edit.
Json DeepCopy(const Json& src) {
    if (src.isArray()) {
        Json out = Json::MakeArray();
        for (const Json& v : src.arrayItems()) out.push(DeepCopy(v));
        return out;
    }
    if (src.isObject()) {
        Json out = Json::MakeObject();
        for (const auto& kv : src.objectItems()) out.set(kv.first, DeepCopy(kv.second));
        return out;
    }
    return src;
}

// Deep value equality: objects by key set and value, arrays elementwise.
// Numbers compare with a tolerance because the JSON writer prints ten
// significant digits - an exact compare would call every float in the file a
// difference the moment it is read back. The tolerance is four orders of
// magnitude above that write error and three below any change an author makes
// by hand, so it separates "the file was re-read" from "this was edited".
bool NumbersEqual(double a, double b) {
    double d = std::fabs(a - b);
    if (d <= 1e-9) return true;
    return d <= 1e-6 * std::max(std::fabs(a), std::fabs(b));
}

bool JsonEquals(const Json& a, const Json& b) {
    if (a.type() != b.type()) return false;
    switch (a.type()) {
        case Json::Type::Null: return true;
        case Json::Type::Bool: return a.asBool() == b.asBool();
        case Json::Type::Number: return NumbersEqual(a.asNumber(), b.asNumber());
        case Json::Type::String: return a.asString() == b.asString();
        case Json::Type::Array: {
            if (a.size() != b.size()) return false;
            const Json::Array& ia = a.arrayItems();
            const Json::Array& ib = b.arrayItems();
            for (size_t i = 0; i < ia.size(); ++i)
                if (!JsonEquals(ia[i], ib[i])) return false;
            return true;
        }
        case Json::Type::Object: {
            if (a.size() != b.size()) return false;
            for (const auto& kv : a.objectItems()) {
                const Json* other = b.find(kv.first);
                if (!other || !JsonEquals(kv.second, *other)) return false;
            }
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Prefab files.
// ---------------------------------------------------------------------------
bool ReadWholeFile(const std::string& path, std::string* out, std::string* error) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (error) *error = "cannot open file: " + path;
        return false;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);
    *out = std::move(text);
    return true;
}

bool WriteWholeFile(const std::string& path, const std::string& text, std::string* error) {
    std::error_code ec;
    std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        if (error) *error = "cannot write file: " + path;
        return false;
    }
    bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    if (!ok && error) *error = "write failed: " + path;
    return ok;
}

// A prefab's own root must not carry a PrefabInstance component (it would refer
// to itself); strip it before writing. Overrides never reach the asset either -
// they belong to the instance, not to the template.
void StripPrefabRootComponents(Json* rootEntity) {
    Json comps = Json::MakeArray();
    if ((*rootEntity)["components"].isArray()) {
        for (const Json& c : (*rootEntity)["components"].arrayItems()) {
            if (c["type"].asString() == "PrefabInstance") continue;
            comps.push(c);
        }
    }
    (*rootEntity).set("components", std::move(comps));
}

Json MakePrefabDoc(const std::string& name, Json root) {
    Json doc = Json::MakeObject();
    doc.set("format", "cairn-prefab");
    doc.set("version", 1);
    doc.set("name", name);
    doc.set("root", std::move(root));
    return doc;
}

// Reads a prefab's root node, cached by path (a scene usually holds several
// instances of one asset). Returns nullptr and fills `error` on failure.
const Json* LoadPrefabRoot(const std::string& prefabProjectPath,
                           std::unordered_map<std::string, Json>* cache, std::string* error) {
    auto it = cache->find(prefabProjectPath);
    if (it == cache->end()) {
        Json doc;
        std::string err;
        std::string text;
        const std::string abs = EngineResolvePath(prefabProjectPath);
        if (!ReadWholeFile(abs, &text, &err) || !Json::Parse(text, &doc, &err) ||
            !doc["root"].isObject()) {
            if (err.empty()) err = "invalid prefab: " + prefabProjectPath;
            if (error) *error = err;
            Log::Warning("prefab: %s", err.c_str());
            it = cache->emplace(prefabProjectPath, Json()).first;
            return nullptr;
        }
        it = cache->emplace(prefabProjectPath, doc["root"]).first;
    }
    if (!it->second.isObject()) return nullptr;
    return &it->second;
}

// ---------------------------------------------------------------------------
// One serialized component entry <-> the property keys the diff tracks.
// ---------------------------------------------------------------------------
const Json* FindComponentEntry(const Json& node, const std::string& type) {
    for (const Json& c : node["components"].arrayItems()) {
        if (c["type"].asString() == type) return &c;
    }
    return nullptr;
}

// Reads a component entry's tracked properties into `out`, keyed by property
// name ("@enabled" being the component's enabled flag).
void CollectDocProps(const Json& cj, Json* out) {
    out->set("@enabled", cj["enabled"].asBool(true));
    const std::string type = cj["type"].asString();
    if (type == "Transform") {
        out->set("position", cj["position"]);
        out->set("rotation", cj["rotation"]);
        out->set("scale", cj["scale"]);
        return;
    }
    if (type == "Script") out->set("class", cj["class"]);
    for (const auto& kv : cj["properties"].objectItems()) out->set(kv.first, kv.second);
}

bool ReadVec3(const Json& v, Vec3* out) {
    if (!v.isArray() || v.size() < 3) return false;
    *out = {v[0].asFloat(), v[1].asFloat(), v[2].asFloat()};
    return true;
}

// Writes one tracked property onto a live component.
bool ApplyLiveProp(Component* c, const std::string& key, const Json& value) {
    if (key == "@enabled") {
        c->enabled = value.asBool(true);
        return true;
    }
    if (c->TypeId() == ComponentTypeId::Transform) {
        Transform* t = static_cast<Transform*>(c);
        Vec3 v;
        if (key == "position" && ReadVec3(value, &v)) { t->position = v; t->MarkWorldDirty(); return true; }
        if (key == "rotation" && ReadVec3(value, &v)) { t->SetEulerDegrees(v); t->MarkWorldDirty(); return true; }
        if (key == "scale" && ReadVec3(value, &v)) { t->scale = v; t->MarkWorldDirty(); return true; }
        return false;
    }
    if (c->TypeId() == ComponentTypeId::Script) {
        auto* sc = static_cast<ScriptComponent*>(c);
        if (key == "class") { sc->scriptClass = value.asString(); return true; }
        if (!sc->properties.isObject()) sc->properties = Json::MakeObject();
        sc->properties.set(key, DeepCopy(value));
        return true;
    }
    return ComponentSetProperty(c, key, value);
}

// Writes one tracked property into a serialized component entry (the direction
// "apply the instance's differences into the asset file" needs).
void PatchDocProp(Json& cj, const std::string& key, const Json& value) {
    const std::string type = cj["type"].asString();
    if (key == "@enabled") { cj.set("enabled", value.asBool(true)); return; }
    if (type == "Transform") { cj.set(key, DeepCopy(value)); return; }
    if (type == "Script" && key == "class") { cj.set("class", value.asString()); return; }
    if (!cj["properties"].isObject()) cj.set("properties", Json::MakeObject());
    cj["properties"].set(key, DeepCopy(value));
}

// ---------------------------------------------------------------------------
// Entity fields.
// ---------------------------------------------------------------------------
struct EntityField {
    const char* key;   // the override key
    const char* json;  // the serialized name
};

const EntityField kEntityFields[] = {
    {"@name", "name"}, {"@active", "active"}, {"@tag", "tag"}, {"@layer", "layer"},
};

// Defaults are folded in, so a missing "tag" and an empty one compare equal and
// the diff never reports noise for fields the file omits.
Json ReadEntityField(const Json& node, const std::string& key) {
    if (key == "@name") return node["name"].asString("GameObject");
    if (key == "@active") return node["active"].asBool(true);
    if (key == "@tag") return node["tag"].asString();
    return (double)(int)node["layer"].asNumber(0.0);
}

void ApplyEntityField(GameObject* go, const std::string& key, const Json& value) {
    if (key == "@name") go->name = value.asString("GameObject");
    else if (key == "@active") go->active = value.asBool(true);
    else if (key == "@tag") go->tag = value.asString();
    else go->layer = (int)value.asNumber(0.0);
}

bool JsonListHas(const Json* obj, const char* key, const std::string& value) {
    if (!obj) return false;
    const Json* list = obj->find(key);
    if (!list || !list->isArray()) return false;
    for (const Json& v : list->arrayItems())
        if (v.asString() == value) return true;
    return false;
}

// ---------------------------------------------------------------------------
// The diff: what does this instance have that the asset does not?
// ---------------------------------------------------------------------------
void DiffNode(const Json& inst, const Json& tmpl, const std::string& path, Json* overrides) {
    Json entry = Json::MakeObject();

    for (const EntityField& f : kEntityFields) {
        Json mine = ReadEntityField(inst, f.key);
        if (!JsonEquals(mine, ReadEntityField(tmpl, f.key))) entry.set(f.key, DeepCopy(mine));
    }

    // Components both sides have: differences are per property. Types only the
    // instance has are instance additions (kept, not tracked here); types only
    // the asset has are recorded so a sync does not resurrect them.
    std::vector<std::string> removedComponents;
    for (const Json& ic : inst["components"].arrayItems()) {
        const std::string type = ic["type"].asString();
        if (type.empty() || type == "PrefabInstance") continue;
        const Json* tc = FindComponentEntry(tmpl, type);
        if (!tc) continue;
        Json mine, theirs;
        CollectDocProps(ic, &mine);
        CollectDocProps(*tc, &theirs);
        for (const auto& kv : theirs.objectItems()) {
            const Json* iv = mine.find(kv.first);
            if (iv && !JsonEquals(*iv, kv.second)) entry.set(type + "." + kv.first, DeepCopy(*iv));
        }
        // A key the asset does not declare (a script property added on the
        // instance) is a difference too.
        for (const auto& kv : mine.objectItems()) {
            if (!theirs.has(kv.first)) entry.set(type + "." + kv.first, DeepCopy(kv.second));
        }
    }
    for (const Json& tc : tmpl["components"].arrayItems()) {
        const std::string type = tc["type"].asString();
        if (type.empty() || type == "PrefabInstance") continue;
        if (!FindComponentEntry(inst, type)) removedComponents.push_back(type);
    }

    // Children, matched by name among siblings (in order, so twins line up).
    std::vector<const Json*> instChildren;
    for (const Json& c : inst["children"].arrayItems()) instChildren.push_back(&c);
    std::vector<bool> claimed(instChildren.size(), false);
    std::vector<std::string> removedChildren;
    for (const Json& tc : tmpl["children"].arrayItems()) {
        const std::string name = tc["name"].asString("GameObject");
        const Json* match = nullptr;
        for (size_t i = 0; i < instChildren.size(); ++i) {
            if (claimed[i]) continue;
            if (instChildren[i]->at("name").asString("GameObject") != name) continue;
            match = instChildren[i];
            claimed[i] = true;
            break;
        }
        if (!match) {
            removedChildren.push_back(name);
            continue;
        }
        DiffNode(*match, tc, path.empty() ? name : path + "/" + name, overrides);
    }

    if (!removedComponents.empty()) {
        Json arr = Json::MakeArray();
        for (const std::string& s : removedComponents) arr.push(s);
        entry.set("@removedComponents", std::move(arr));
    }
    if (!removedChildren.empty()) {
        Json arr = Json::MakeArray();
        for (const std::string& s : removedChildren) arr.push(s);
        entry.set("@removedChildren", std::move(arr));
    }
    if (entry.size() > 0) overrides->set(path, std::move(entry));
}

// ---------------------------------------------------------------------------
// The sync: pull the asset into a live instance, leaving overrides alone.
// ---------------------------------------------------------------------------
struct SyncStats {
    int props = 0;      // property writes
    int components = 0; // components added
    int entities = 0;   // entities added
};

void SyncNode(Scene& scene, GameObject* go, const Json& tmpl, const std::string& path,
              const Json& overrides, SyncStats* stats) {
    const Json* entry = overrides.find(path);
    const Json live = scene.SerializeEntity(go);

    for (const EntityField& f : kEntityFields) {
        if (entry && entry->has(f.key)) continue;
        Json want = ReadEntityField(tmpl, f.key);
        if (JsonEquals(want, ReadEntityField(live, f.key))) continue;
        ApplyEntityField(go, f.key, want);
        ++stats->props;
    }

    for (const Json& tc : tmpl["components"].arrayItems()) {
        const std::string type = tc["type"].asString();
        if (type.empty() || type == "PrefabInstance") continue;
        Component* c = go->GetComponentByName(type);
        if (!c) {
            if (JsonListHas(entry, "@removedComponents", type)) continue;
            if (!go->AddComponentByName(type)) continue;
            c = go->GetComponentByName(type);
            if (!c) continue;
            ++stats->components;
        }
        Json theirs;
        CollectDocProps(tc, &theirs);
        for (const auto& kv : theirs.objectItems()) {
            if (entry && entry->has(type + "." + kv.first)) continue;
            if (ApplyLiveProp(c, kv.first, kv.second)) ++stats->props;
        }
    }

    std::unordered_set<const GameObject*> claimed;
    for (const Json& tc : tmpl["children"].arrayItems()) {
        const std::string name = tc["name"].asString("GameObject");
        const std::string childPath = path.empty() ? name : path + "/" + name;
        GameObject* child = nullptr;
        for (GameObject* cand : go->GetChildren()) {
            if (claimed.count(cand)) continue;
            if (cand->name != name) continue;
            child = cand;
            break;
        }
        if (!child) {
            if (JsonListHas(entry, "@removedChildren", name)) continue;
            child = scene.DeserializeEntitySubtree(tc, go, nullptr);
            if (!child) continue;
            ++stats->entities;
        }
        claimed.insert(child);
        // A node that was just created still gets walked: an override recorded
        // for a path below it has to be re-applied over the asset's values.
        SyncNode(scene, child, tc, childPath, overrides, stats);
    }
}

// Resolves the instances a command works on: one entity (id != 0) or every
// prefab instance in the scene (id == 0).
bool CollectInstances(Scene& scene, EntityId id, std::vector<GameObject*>* out, std::string* error) {
    if (id) {
        GameObject* go = scene.FindEntity(id);
        if (!go) {
            if (error) *error = "entity not found";
            return false;
        }
        out->push_back(go);
        return true;
    }
    for (const auto& o : scene.objects) {
        if (o->GetComponent<PrefabInstance>()) out->push_back(o.get());
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Engine: prefab lifecycle.
// ---------------------------------------------------------------------------
EntityId Engine::InstantiatePrefab(const std::string& prefabProjectPath, EntityId parent) {
    const std::string abs = EngineResolvePath(prefabProjectPath);
    std::string text, err;
    if (!ReadWholeFile(abs, &text, &err)) {
        Log::Error("%s", err.c_str());
        return kInvalidEntity;
    }
    Json doc;
    if (!Json::Parse(text, &doc, &err) || !doc["root"].isObject()) {
        Log::Error("invalid prefab: %s", prefabProjectPath.c_str());
        return kInvalidEntity;
    }
    GameObject* p = parent ? FindEntity(parent) : nullptr;
    GameObject* go = scene_->DeserializeEntitySubtree(doc["root"], p, &err);
    if (!go) {
        Log::Error("prefab instantiate failed: %s", err.c_str());
        return kInvalidEntity;
    }
    if (auto* pi = go->AddComponent<PrefabInstance>()) {
        pi->prefabPath = prefabProjectPath;
        // A fresh copy *is* the asset, so it starts with nothing overridden.
        pi->overrides = Json::MakeObject();
        pi->overridesKnown = true;
    }
    return go->m_id;
}

bool Engine::CreatePrefabFromEntity(EntityId id, const std::string& prefabProjectPath, std::string* error) {
    GameObject* go = FindEntity(id);
    if (!go) {
        if (error) *error = "entity not found";
        return false;
    }
    Json root = scene_->SerializeSubtree(go);
    StripPrefabRootComponents(&root);
    const std::string abs = EngineResolvePath(prefabProjectPath);
    if (!WriteWholeFile(abs, MakePrefabDoc(go->name, std::move(root)).Dump(2) + "\n", error)) return false;
    assets_.EnsureMeta(prefabProjectPath);
    assets_.Scan();
    if (auto* pi = go->GetComponent<PrefabInstance>()) {
        pi->prefabPath = prefabProjectPath;
        // Authoring the asset from this entity makes it the template: nothing
        // is overridden any more
        pi->overrides = Json::MakeObject();
        pi->overridesKnown = true;
    } else if (auto* added = go->AddComponent<PrefabInstance>()) {
        added->prefabPath = prefabProjectPath;
        added->overridesKnown = true;
    }
    return true;
}

bool Engine::ApplyPrefab(EntityId id, std::string* error) {
    GameObject* go = FindEntity(id);
    if (!go) {
        if (error) *error = "entity not found";
        return false;
    }
    auto* pi = go->GetComponent<PrefabInstance>();
    if (!pi || pi->prefabPath.empty()) {
        if (error) *error = "entity is not a prefab instance";
        return false;
    }
    const std::string path = pi->prefabPath;
    // Record the local differences, then pull the asset in keeping them, so the
    // subtree written below cannot resurrect a value the asset already moved on
    // from. Everything the user did to this instance - including components and
    // children added by hand - survives that sync and lands in the file.
    if (RefreshPrefabOverrides(id, error) < 0) return false;
    if (SyncPrefabs(id, error) < 0) return false;
    Json root = scene_->SerializeSubtree(go);
    StripPrefabRootComponents(&root);
    if (!WriteWholeFile(EngineResolvePath(path), MakePrefabDoc(go->name, std::move(root)).Dump(2) + "\n",
                        error)) {
        return false;
    }
    pi->overrides = Json::MakeObject();
    pi->overridesKnown = true;
    // Siblings share the asset, so they follow right away.
    SyncPrefabs(0, nullptr);
    return true;
}

EntityId Engine::RevertPrefab(EntityId id, std::string* error) {
    GameObject* go = FindEntity(id);
    if (!go) {
        if (error) *error = "entity not found";
        return kInvalidEntity;
    }
    auto* pi = go->GetComponent<PrefabInstance>();
    if (!pi || pi->prefabPath.empty()) {
        if (error) *error = "entity is not a prefab instance";
        return kInvalidEntity;
    }
    const std::string path = pi->prefabPath;
    const std::string name = go->name;
    GameObject* parent = go->GetParent();
    const EntityId parentId = parent ? parent->m_id : 0;
    scene_->DestroyEntityNow(go);
    EntityId newId = InstantiatePrefab(path, parentId);
    if (newId) {
        if (GameObject* ng = FindEntity(newId)) ng->name = name;
    }
    return newId;
}

// ---------------------------------------------------------------------------
// Engine: overrides.
// ---------------------------------------------------------------------------
int Engine::RefreshPrefabOverrides(EntityId id, std::string* error) {
    std::vector<GameObject*> targets;
    if (!CollectInstances(*scene_, id, &targets, error)) return -1;
    std::unordered_map<std::string, Json> docs;
    int refreshed = 0;
    for (GameObject* go : targets) {
        auto* pi = go->GetComponent<PrefabInstance>();
        if (!pi || pi->prefabPath.empty()) {
            if (error) *error = "entity is not a prefab instance";
            if (id) return -1;
            continue;
        }
        std::string err;
        const Json* root = LoadPrefabRoot(pi->prefabPath, &docs, &err);
        if (!root) {
            if (error) *error = err;
            if (id) return -1;
            continue;
        }
        Json overrides = Json::MakeObject();
        DiffNode(scene_->SerializeSubtree(go), *root, "", &overrides);
        pi->overrides = std::move(overrides);
        pi->overridesKnown = true;
        ++refreshed;
    }
    return refreshed;
}

Json Engine::PrefabOverrides(EntityId id) {
    GameObject* go = FindEntity(id);
    auto* pi = go ? go->GetComponent<PrefabInstance>() : nullptr;
    if (!pi) return Json::MakeObject();
    return DeepCopy(pi->overrides);
}

int Engine::SyncPrefabs(EntityId id, std::string* error) {
    std::vector<GameObject*> targets;
    if (!CollectInstances(*scene_, id, &targets, error)) return -1;
    std::unordered_map<std::string, Json> docs;
    int synced = 0;
    for (GameObject* go : targets) {
        auto* pi = go->GetComponent<PrefabInstance>();
        if (!pi || pi->prefabPath.empty()) {
            if (error) *error = "entity is not a prefab instance";
            if (id) return -1;
            continue;
        }
        std::string err;
        const Json* root = LoadPrefabRoot(pi->prefabPath, &docs, &err);
        if (!root) {
            if (error) *error = err;
            if (id) return -1;
            continue;
        }
        // A scene written before overrides were recorded has no list: take the
        // instance's own state as the list, so the asset cannot silently
        // overwrite work done in an older version.
        if (!pi->overridesKnown) {
            Json overrides = Json::MakeObject();
            DiffNode(scene_->SerializeSubtree(go), *root, "", &overrides);
            pi->overrides = std::move(overrides);
            pi->overridesKnown = true;
        }
        SyncStats stats;
        SyncNode(*scene_, go, *root, "", pi->overrides, &stats);
        ++synced;
    }
    return synced;
}

}  // namespace Cairn
