// Cairn Engine - internal scene model (not part of the scripting API).
#pragma once

#include "cairn/scripting.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace Cairn {

// Editor-facing property kinds (superset of PropType).
enum class EditorPropKind : int { Float, Int, Bool, String, Vec2, Vec3, Color, Enum, Asset };

struct PropertyDef {
    const char* name = nullptr;      // canonical key (serialization + editor)
    const char* label = nullptr;     // editor label (English; editor localizes by key)
    EditorPropKind kind = EditorPropKind::Float;
    Json (*get)(const Component*) = nullptr;
    void (*set)(Component*, const Json&) = nullptr;
    const char* const* enumOptions = nullptr;  // for Enum kinds
    int enumCount = 0;
    const char* assetKind = "";                // "texture" etc. for Asset kinds
};

class Scene {
public:
    std::string name = "Scene";
    std::vector<std::unique_ptr<GameObject>> objects;

    // World settings.
    Vec3 gravity{0.0f, -9.81f, 0.0f};
    Color ambient{0.26f, 0.28f, 0.33f, 1.0f};
    bool fogEnabled = false;
    Color fogColor{0.62f, 0.66f, 0.72f, 1.0f};
    float fogDensity = 0.02f;
    Color background{0.09f, 0.09f, 0.12f, 1.0f};

    uint32_t nextComponentId = 1;

    Scene() = default;
    ~Scene() = default;
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    GameObject* CreateEntity(const std::string& name, GameObject* parent = nullptr);
    void DestroyEntityNow(GameObject* go);  // removes subtree immediately
    void QueueDestroy(GameObject* go);
    void ProcessDeferred();
    GameObject* FindEntity(EntityId id);
    GameObject* FindByName(const std::string& name);
    std::vector<GameObject*> FindByTag(const std::string& tag);
    CameraComponent* MainCamera();

    // Serialization -------------------------------------------------------
    Json SerializeEntity(const GameObject* go) const;      // includes subtree
    GameObject* DeserializeEntitySubtree(const Json& entityJson, GameObject* parent,
                                         std::string* error = nullptr);
    Json Serialize() const;
    static std::shared_ptr<Scene> Deserialize(const Json& doc, std::string* error = nullptr);
    bool SaveToFile(const std::string& path, std::string* error = nullptr);
    static std::shared_ptr<Scene> LoadFromFile(const std::string& path, std::string* error = nullptr);

    // Gameplay lifecycle (set by Engine).
    bool started = false;
    uint64_t frame = 0;

private:
    std::vector<GameObject*> m_destroyQueue;
    uint32_t m_nextEntityId = 1;
};

// Owned by the Engine facade; resolves the scene gameplay code operates on.
Scene* EngineActiveScene();

// Per-component reflection registration. Implemented in components.cpp.
const std::vector<PropertyDef>* ComponentPropertyDefs(const Component* c);
const std::vector<PropertyDef>* PropertyDefsForType(const std::string& typeName);
bool ComponentGetProperty(const Component* c, const std::string& name, Json* out);
bool ComponentSetProperty(Component* c, const std::string& name, const Json& value);
Json ComponentSerialize(const Component* c);
bool ComponentDeserialize(Component* c, const Json& props);
std::vector<std::string> AddableComponentNames();

}  // namespace Cairn
