#include "scene_internal.h"

#include <algorithm>
#include <cstring>

namespace Cairn {
namespace {
// Exact comparison: recomputing the same TRS must produce identical bits, so a
// plain memcmp is both correct and the cheapest way to detect "did it change".
bool Mat4Equals(const Mat4& a, const Mat4& b) {
    return std::memcmp(a.m, b.m, sizeof(a.m)) == 0;
}
}  // namespace

// ---------------------------------------------------------------------------
// Component base helpers declared in scripting.h.
// ---------------------------------------------------------------------------
Transform& Component::transform() const { return m_go->transform(); }

// ---------------------------------------------------------------------------
// Transform.
// ---------------------------------------------------------------------------
void Transform::SetParent(GameObject* parent) {
    if (parent == m_parent) return;
    if (parent) {
        // Reject cycles: parent must not be a descendant of this object.
        GameObject* walk = parent;
        while (walk) {
            if (walk == m_go) return;
            walk = walk->transform().m_parent;
        }
    }
    if (m_parent) {
        auto& siblings = m_parent->transform().m_children;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), m_go), siblings.end());
    }
    m_parent = parent;
    if (m_parent) m_parent->transform().m_children.push_back(m_go);
    MarkWorldDirty();
}

void Transform::MarkWorldDirty() {
    m_worldDirty = true;
    m_worldValid = false;
    for (GameObject* child : m_children) child->transform().MarkWorldDirty();
}

Mat4 Transform::LocalMatrix() const {
    return Mat4::TRS(position, rotation, scale);
}

Mat4 Transform::WorldMatrix() {
    // A parent's world matrix changing invalidates this subtree.
    if (m_parent) {
        m_parent->transform().WorldMatrix();
        if (m_parent->transform().m_worldVersion != m_seenParentVersion) {
            m_worldDirty = true;
            m_worldValid = false;
        }
    }
    Mat4 local = LocalMatrix();
    // Recompute when explicitly invalidated, when the parent moved, or when the
    // local fields were written directly (raw field assignment is allowed).
    if (m_worldDirty || !m_worldValid || !Mat4Equals(m_localCache, local)) {
        m_world = m_parent ? m_parent->transform().WorldMatrix() * local : local;
        m_localCache = local;
        m_worldDirty = false;
        m_worldValid = true;
        ++m_worldVersion;
        m_seenParentVersion = m_parent ? m_parent->transform().m_worldVersion : 0;
    }
    return m_world;
}

Vec3 Transform::WorldPosition() { return WorldMatrix().GetTranslation(); }
Quat Transform::WorldRotation() {
    Quat w = rotation;
    GameObject* p = m_parent;
    while (p) {
        w = p->transform().rotation * w;
        p = p->transform().m_parent;
    }
    return w.Normalized();
}
Vec3 Transform::WorldScale() {
    Vec3 s = scale;
    GameObject* p = m_parent;
    while (p) {
        s = Vec3(s.x * p->transform().scale.x, s.y * p->transform().scale.y, s.z * p->transform().scale.z);
        p = p->transform().m_parent;
    }
    return s;
}

// ---------------------------------------------------------------------------
// GameObject.
// ---------------------------------------------------------------------------
void GameObject::SetIdAndScene(EntityId id, Scene* scene) {
    m_id = id;
    m_scene = scene;
}

void GameObject::AttachComponent(std::unique_ptr<Component> c) {
    c->m_go = this;
    c->m_componentId = m_scene ? m_scene->nextComponentId++ : 0;
    if (c->TypeId() == ComponentTypeId::Transform) {
        m_transform = static_cast<Transform*>(c.get());
    }
    m_components.push_back(std::move(c));
}

bool GameObject::ActiveInHierarchy() const {
    if (!active) return false;
    const GameObject* p = m_transform->m_parent;
    while (p) {
        if (!p->active) return false;
        p = p->transform().m_parent;
    }
    return true;
}

bool GameObject::AddComponentByName(const std::string& typeName) {
    if (typeName == "Transform") return false;
    if (GetComponentByName(typeName)) return false;  // one per type (single-behaviour model)

    std::unique_ptr<Component> owned;
    if (typeName == "Camera") owned.reset(new (std::nothrow) CameraComponent());
    else if (typeName == "Light") owned.reset(new (std::nothrow) LightComponent());
    else if (typeName == "MeshRenderer") owned.reset(new (std::nothrow) MeshRenderer());
    else if (typeName == "SpriteRenderer") owned.reset(new (std::nothrow) SpriteRenderer());
    else if (typeName == "TextRenderer") owned.reset(new (std::nothrow) TextRenderer());
    else if (typeName == "RigidBody") owned.reset(new (std::nothrow) RigidBody());
    else if (typeName == "BoxCollider") owned.reset(new (std::nothrow) BoxCollider());
    else if (typeName == "SphereCollider") owned.reset(new (std::nothrow) SphereCollider());
    else if (typeName == "Collider2D") owned.reset(new (std::nothrow) Collider2D());
    else if (typeName == "Script") owned.reset(new (std::nothrow) ScriptComponent());
    if (!owned) return false;
    AttachComponent(std::move(owned));
    return true;
}

Component* GameObject::GetComponentByName(const std::string& typeName) const {
    for (const auto& c : m_components)
        if (typeName == c->TypeName()) return c.get();
    return nullptr;
}

bool GameObject::RemoveComponentByName(const std::string& typeName) {
    if (typeName == "Transform") return false;
    for (auto it = m_components.begin(); it != m_components.end(); ++it) {
        if (typeName == (*it)->TypeName()) {
            if (typeName == "Script") static_cast<ScriptComponent*>(it->get())->DestroyInstance();
            m_components.erase(it);
            return true;
        }
    }
    return false;
}

void ScriptComponent::DestroyInstance() {
    if (!instance) return;
    instance->OnDestroy();
    delete instance;
    instance = nullptr;
}

std::vector<Component*> GameObject::GetComponentList() const {
    std::vector<Component*> out;
    out.reserve(m_components.size());
    for (const auto& c : m_components) out.push_back(c.get());
    return out;
}

// ---------------------------------------------------------------------------
// Scene.
// ---------------------------------------------------------------------------
GameObject* Scene::CreateEntity(const std::string& name, GameObject* parent) {
    auto go = std::make_unique<GameObject>();
    go->m_id = m_nextEntityId++;
    go->m_scene = this;
    go->name = name;
    auto* raw = go.get();
    objects.emplace_back(std::move(go));
    // Transform is always the first component.
    auto t = std::unique_ptr<Component>(new (std::nothrow) Transform());
    if (t) {
        raw->AttachComponent(std::move(t));
        raw->transform().m_parent = parent;
        if (parent) parent->transform().m_children.push_back(raw);
    }
    return raw;
}

void Scene::DestroyEntityNow(GameObject* go) {
    if (!go) return;
    // Release gameplay script instances before the components go away so
    // OnDestroy runs and nothing leaks.
    for (Component* c : go->GetComponentList()) {
        if (c->TypeId() == ComponentTypeId::Script) static_cast<ScriptComponent*>(c)->DestroyInstance();
    }
    // Detach from parent.
    if (go->transform().m_parent) {
        auto& siblings = go->transform().m_parent->transform().m_children;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), go), siblings.end());
        go->transform().m_parent = nullptr;
    }
    // Destroy children first (they reference us only through their parent).
    for (GameObject* child : go->transform().m_children) DestroyEntityNow(child);
    go->transform().m_children.clear();
    objects.erase(std::remove_if(objects.begin(), objects.end(),
                                 [go](const std::unique_ptr<GameObject>& p) { return p.get() == go; }),
                  objects.end());
}

void Scene::QueueDestroy(GameObject* go) {
    if (go) m_destroyQueue.push_back(go);
}

void Scene::ProcessDeferred() {
    for (GameObject* go : m_destroyQueue) DestroyEntityNow(go);
    m_destroyQueue.clear();
}

GameObject* Scene::FindEntity(EntityId id) {
    for (const auto& go : objects)
        if (go->m_id == id) return go.get();
    return nullptr;
}

GameObject* Scene::FindByName(const std::string& name) {
    for (const auto& go : objects)
        if (go->name == name && go->active) return go.get();
    return nullptr;
}

std::vector<GameObject*> Scene::FindByTag(const std::string& tag) {
    std::vector<GameObject*> out;
    for (const auto& go : objects)
        if (go->tag == tag && go->active) out.push_back(go.get());
    return out;
}

CameraComponent* Scene::MainCamera() {
    // Prefer a camera tagged "MainCamera", else the first active camera.
    for (const auto& go : objects) {
        if (!go->active) continue;
        auto* cam = go->GetComponent<CameraComponent>();
        if (cam && go->tag == "MainCamera") return cam;
    }
    for (const auto& go : objects) {
        if (!go->active) continue;
        auto* cam = go->GetComponent<CameraComponent>();
        if (cam) return cam;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Entity serialization.
// ---------------------------------------------------------------------------
static Json Vec3ToJson(const Vec3& v) { return Json::MakeArray({v.x, v.y, v.z}); }
static Json Vec2ToJson(const Vec2& v) { return Json::MakeArray({v.x, v.y}); }
static Json ColorToJson(const Color& c) { return Json::MakeArray({c.r, c.g, c.b, c.a}); }
static Vec3 JsonToVec3(const Json& j, Vec3 def = Vec3()) {
    if (!j.isArray() || j.size() < 3) return def;
    return {j[0].asFloat(), j[1].asFloat(), j[2].asFloat()};
}
static Vec2 JsonToVec2(const Json& j, Vec2 def = Vec2()) {
    if (!j.isArray() || j.size() < 2) return def;
    return {j[0].asFloat(), j[1].asFloat()};
}
static Color JsonToColor(const Json& j, Color def = Color()) {
    if (!j.isArray() || j.size() < 4) return def;
    return {j[0].asFloat(), j[1].asFloat(), j[2].asFloat(), j[3].asFloat()};
}

Json Scene::SerializeEntity(const GameObject* go) const {
    Json e = Json::MakeObject();
    e.set("id", (double)go->m_id);
    e.set("name", go->name);
    e.set("active", go->active);
    if (!go->tag.empty()) e.set("tag", go->tag);
    if (go->transform().m_parent) e.set("parent", (double)go->transform().m_parent->m_id);

    Json comps = Json::MakeArray();
    for (const auto& c : go->GetComponents()) {
        Json cj = Json::MakeObject();
        cj.set("type", c->TypeName());
        if (c->TypeId() == ComponentTypeId::Transform) {
            const Transform* t = static_cast<const Transform*>(c.get());
            cj.set("position", Vec3ToJson(t->position));
            cj.set("rotation", Vec3ToJson(t->GetEulerDegrees()));
            cj.set("scale", Vec3ToJson(t->scale));
        } else if (c->TypeId() == ComponentTypeId::Script) {
            const ScriptComponent* sc = static_cast<const ScriptComponent*>(c.get());
            cj.set("class", sc->scriptClass);
            Json props = Json::MakeObject();
            for (const auto& kv : sc->properties.objectItems()) props.set(kv.first, kv.second);
            cj.set("properties", std::move(props));
        } else {
            Json props = Json::MakeObject();
            if (const auto* defs = ComponentPropertyDefs(c.get())) {
                for (const auto& def : *defs) props.set(def.name, def.get(c.get()));
            }
            cj.set("properties", std::move(props));
        }
        cj.set("enabled", c->enabled);
        comps.push(std::move(cj));
    }
    e.set("components", std::move(comps));
    return e;
}

Json Scene::SerializeSubtree(const GameObject* go) const {
    Json e = SerializeEntity(go);
    const auto& children = go->transform().m_children;
    if (children.empty()) return e;
    Json arr = Json::MakeArray();
    for (const GameObject* child : children) arr.push(SerializeSubtree(child));
    e.set("children", std::move(arr));
    return e;
}

GameObject* Scene::InstantiateCopy(const GameObject* go, GameObject* parent) {
    if (!go) return nullptr;
    return DeserializeEntitySubtree(SerializeSubtree(go), parent, nullptr);
}

GameObject* Scene::DeserializeEntitySubtree(const Json& entityJson, GameObject* parent, std::string* error) {
    if (!entityJson.isObject()) {
        if (error) *error = "entity is not an object";
        return nullptr;
    }
    GameObject* go = CreateEntity(entityJson["name"].asString("GameObject"), parent);
    go->active = entityJson["active"].asBool(true);
    go->tag = entityJson["tag"].asString();

    const Json& comps = entityJson["components"];
    if (comps.isArray()) {
        for (const Json& cj : comps.arrayItems()) {
            std::string type = cj["type"].asString();
            if (type == "Transform") {
                Transform& t = go->transform();
                t.position = JsonToVec3(cj["position"]);
                t.SetEulerDegrees(JsonToVec3(cj["rotation"]));
                t.scale = JsonToVec3(cj["scale"], Vec3(1, 1, 1));
            } else if (type == "Script") {
                ScriptComponent* sc = go->AddComponent<ScriptComponent>();
                if (sc) {
                    sc->scriptClass = cj["class"].asString();
                    sc->properties = cj["properties"];
                    if (!sc->properties.isObject()) sc->properties = Json::MakeObject();
                    sc->enabled = cj["enabled"].asBool(true);
                }
            } else if (!type.empty()) {
                if (go->AddComponentByName(type)) {
                    Component* c = go->GetComponentByName(type);
                    if (c) {
                        ComponentDeserialize(c, cj["properties"]);
                        c->enabled = cj["enabled"].asBool(true);
                    }
                } else if (error) {
                    *error = "unknown component type: " + type;
                }
            }
        }
    }
    // Children.
    // Serialized children appear as separate entities in the flat list; here we
    // only load the "children" array form used by subtree copy/paste.
    const Json& children = entityJson["children"];
    if (children.isArray()) {
        for (const Json& child : children.arrayItems())
            DeserializeEntitySubtree(child, go, error);
    }
    return go;
}

Json Scene::Serialize() const {
    Json doc = Json::MakeObject();
    doc.set("format", "cairn-scene");
    doc.set("version", 1);
    doc.set("name", name);
    Json settings = Json::MakeObject();
    settings.set("gravity", Vec3ToJson(gravity));
    settings.set("ambient", ColorToJson(ambient));
    settings.set("fogEnabled", fogEnabled);
    settings.set("fogColor", ColorToJson(fogColor));
    settings.set("fogDensity", fogDensity);
    settings.set("background", ColorToJson(background));
    settings.set("tonemap", (double)tonemap);
    settings.set("exposure", (double)exposure);
    settings.set("shadows", (double)shadows);
    settings.set("shadowResolution", (double)shadowResolution);
    settings.set("shadowDistance", (double)shadowDistance);
    settings.set("postVignette", (double)postVignette);
    settings.set("postVignetteRadius", (double)postVignetteRadius);
    settings.set("postVignetteSoftness", (double)postVignetteSoftness);
    settings.set("postContrast", (double)postContrast);
    settings.set("postSaturation", (double)postSaturation);
    settings.set("postBloomStrength", (double)postBloomStrength);
    settings.set("postBloomThreshold", (double)postBloomThreshold);
    settings.set("postFxaa", postFxaa);
    settings.set("textureMemoryMB", (double)textureMemoryMB);
    settings.set("renderBackend", (double)renderBackend);
    settings.set("msaa", (double)msaa);
    doc.set("settings", std::move(settings));

    Json entities = Json::MakeArray();
    for (const auto& go : objects) entities.push(SerializeEntity(go.get()));
    doc.set("entities", std::move(entities));
    return doc;
}

std::shared_ptr<Scene> Scene::Deserialize(const Json& doc, std::string* error) {
    auto scene = std::make_shared<Scene>();
    scene->name = doc["name"].asString("Scene");
    const Json& settings = doc["settings"];
    if (settings.isObject()) {
        scene->gravity = JsonToVec3(settings["gravity"], Vec3(0, -9.81f, 0));
        scene->ambient = JsonToColor(settings["ambient"], Color(0.26f, 0.28f, 0.33f));
        scene->fogEnabled = settings["fogEnabled"].asBool(false);
        scene->fogColor = JsonToColor(settings["fogColor"], Color(0.62f, 0.66f, 0.72f));
        scene->fogDensity = settings["fogDensity"].asFloat(0.02f);
        scene->background = JsonToColor(settings["background"], Color(0.09f, 0.09f, 0.12f));
        scene->tonemap = (int)settings["tonemap"].asNumber(2.0);
        scene->exposure = settings["exposure"].asFloat(1.0f);
        scene->shadows = (int)settings["shadows"].asNumber(2.0);
        scene->shadowResolution = (int)settings["shadowResolution"].asNumber(1024.0);
        scene->shadowDistance = settings["shadowDistance"].asFloat(60.0f);
        scene->postVignette = settings["postVignette"].asFloat(0.0f);
        scene->postVignetteRadius = settings["postVignetteRadius"].asFloat(0.75f);
        scene->postVignetteSoftness = settings["postVignetteSoftness"].asFloat(0.5f);
        scene->postContrast = settings["postContrast"].asFloat(1.0f);
        scene->postSaturation = settings["postSaturation"].asFloat(1.0f);
        scene->postBloomStrength = settings["postBloomStrength"].asFloat(0.0f);
        scene->postBloomThreshold = settings["postBloomThreshold"].asFloat(0.85f);
        scene->postFxaa = settings["postFxaa"].asBool(false);
        scene->textureMemoryMB = (int)settings["textureMemoryMB"].asNumber(256.0);
        scene->renderBackend = (int)settings["renderBackend"].asNumber(0.0);
        scene->msaa = (int)settings["msaa"].asNumber(4.0);
    }
    const Json& entities = doc["entities"];
    if (entities.isArray()) {
        std::unordered_map<uint32_t, GameObject*> byId;
        struct PendingLoad { const Json* e; GameObject* go; };
        std::vector<PendingLoad> pending;
        for (const Json& e : entities.arrayItems()) {
            GameObject* go = scene->CreateEntity(e["name"].asString("GameObject"), nullptr);
            go->m_id = (EntityId)e["id"].asUint(go->m_id);
            if (go->m_id >= scene->m_nextEntityId) scene->m_nextEntityId = go->m_id + 1;
            byId[go->m_id] = go;
            pending.push_back({&e, go});
        }
        // Apply parents and components.
        for (const PendingLoad& item : pending) {
            const Json& e = *item.e;
            GameObject* go = item.go;
            uint32_t parentId = e["parent"].asUint(0);
            if (parentId) {
                auto it = byId.find(parentId);
                if (it != byId.end()) go->transform().SetParent(it->second);
            }
            go->active = e["active"].asBool(true);
            go->tag = e["tag"].asString();
            const Json& comps = e["components"];
            if (comps.isArray()) {
                for (const Json& cj : comps.arrayItems()) {
                    std::string type = cj["type"].asString();
                    if (type == "Transform") {
                        Transform& t = go->transform();
                        t.position = JsonToVec3(cj["position"]);
                        t.SetEulerDegrees(JsonToVec3(cj["rotation"]));
                        t.scale = JsonToVec3(cj["scale"], Vec3(1, 1, 1));
                    } else if (type == "Script") {
                        ScriptComponent* sc = go->AddComponent<ScriptComponent>();
                        if (sc) {
                            sc->scriptClass = cj["class"].asString();
                            sc->properties = cj["properties"];
                            if (!sc->properties.isObject()) sc->properties = Json::MakeObject();
                            sc->enabled = cj["enabled"].asBool(true);
                        }
                    } else if (!type.empty()) {
                        if (go->AddComponentByName(type)) {
                            Component* c = go->GetComponentByName(type);
                            if (c) {
                                ComponentDeserialize(c, cj["properties"]);
                                c->enabled = cj["enabled"].asBool(true);
                            }
                        } else if (error) {
                            *error = "unknown component type: " + type;
                        }
                    }
                }
            }
        }
    }
    return scene;
}

bool Scene::SaveToFile(const std::string& path, std::string* error) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        if (error) *error = "cannot open file for writing: " + path;
        return false;
    }
    std::string text = Serialize().Dump(2);
    bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    if (!ok && error) *error = "write failed: " + path;
    return ok;
}

std::shared_ptr<Scene> Scene::LoadFromFile(const std::string& path, std::string* error) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (error) *error = "cannot open scene file: " + path;
        return nullptr;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);
    Json doc;
    std::string parseError;
    if (!Json::Parse(text, &doc, &parseError)) {
        if (error) *error = "scene parse error: " + parseError;
        return nullptr;
    }
    return Deserialize(doc, error);
}

// Gameplay helpers.
Scene* Gameplay::ActiveScene() { return EngineActiveScene(); }
GameObject* Gameplay::Find(const std::string& name) {
    Scene* s = ActiveScene();
    return s ? s->FindByName(name) : nullptr;
}
GameObject* Gameplay::FindWithTag(const std::string& tag) {
    Scene* s = ActiveScene();
    if (!s) return nullptr;
    auto list = s->FindByTag(tag);
    return list.empty() ? nullptr : list.front();
}
std::vector<GameObject*> Gameplay::FindAllWithTag(const std::string& tag) {
    Scene* s = ActiveScene();
    return s ? s->FindByTag(tag) : std::vector<GameObject*>{};
}
GameObject* Gameplay::CreateEmpty(const std::string& name) {
    Scene* s = ActiveScene();
    return s ? s->CreateEntity(name) : nullptr;
}
GameObject* Gameplay::Instantiate(GameObject& original) {
    Scene* s = ActiveScene();
    if (!s) return nullptr;
    GameObject* copy = s->InstantiateCopy(&original, original.GetParent());
    if (copy) copy->name = original.name;  // identical copy, editor may rename
    return copy;
}
void Gameplay::Destroy(GameObject* go) {
    Scene* s = ActiveScene();
    if (s) s->QueueDestroy(go);
}
void Gameplay::DestroyComponent(Component* c) {
    if (!c || !c->m_go) return;
    GameObject* go = c->m_go;
    if (c->TypeId() == ComponentTypeId::Script) static_cast<ScriptComponent*>(c)->DestroyInstance();
    for (auto it = go->m_components.begin(); it != go->m_components.end(); ++it) {
        if (it->get() == c) {
            go->m_components.erase(it);
            return;
        }
    }
}

}  // namespace Cairn
