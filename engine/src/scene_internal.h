// Cairn Engine - internal scene model (not part of the scripting API).
#pragma once

#include "cairn/scripting.h"
#include "cairn/asset.h"

#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Cairn {

// Editor-facing property kinds (superset of PropType).
enum class EditorPropKind : int { Float, Int, Bool, String, Vec2, Vec3, Vec4, Color, Enum, Asset, Object,
    // Added in 0.1.28. Appended so existing numeric values stay stable.
    Json };  // a structured value the engine stores and passes through opaquely

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

enum class CollisionPhase : int { Enter, Stay, Exit };

// One tracked physics pair; the collider pointers are kept so an exit event can
// still be delivered after a collider or entity was removed mid-step.
struct PairRecord {
    Component* a = nullptr;
    Component* b = nullptr;
    bool isTrigger = false;
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
    // Display transform: tone mapping operator (0 none, 1 Reinhard, 2 ACES,
    // 3 filmic) and exposure multiplier applied before the sRGB encode.
    int tonemap = 2;
    float exposure = 1.0f;
    // Directional shadows: cascade count (0 = off) and per-cascade resolution.
    int shadows = 2;
    int shadowResolution = 1024;
    float shadowDistance = 60.0f;
    // Post-processing, applied at the end of a render to the display-referred
    // sRGB framebuffer. All defaults are neutral: with everything off the
    // frame is byte-identical to the un-post-processed image.
    float postVignette = 0.0f;         // 0..1 radial darkening strength
    float postVignetteRadius = 0.75f;  // 0..1 where darkening starts (half-height units)
    float postVignetteSoftness = 0.5f; // 0..1 falloff width past the radius
    float postContrast = 1.0f;         // 0..2 around the 0.5 display pivot, 1 = neutral
    float postSaturation = 1.0f;       // 0..2 around Rec.709 luma, 1 = neutral
    float postBloomStrength = 0.0f;    // 0..2 additive glow, 0 = off
    float postBloomThreshold = 0.85f;  // 0..1 bright-pass cutoff (display luma)
    bool postFxaa = false;             // luma-edge AA over the final image
    // Texture cache budget in megabytes (LRU eviction above it).
    int textureMemoryMB = 256;
    // Rasteriser selection: 0 = auto (prefer the GPU, fall back to software),
    // 1 = force Vulkan, 2 = force software. Mirrors RenderBackend.
    int renderBackend = 0;
    // MSAA sample count for the GPU backend (1, 2, 4 or 8). The software
    // rasteriser is single-sampled and ignores this.
    int msaa = 4;

    uint32_t nextComponentId = 1;
    std::unordered_map<uint64_t, PairRecord> physicsPairs;  // triggers + contacts

    // Collision layer matrix: bit j of layerMatrix[i] set means layer i
    // collides with layer j. All pairs collide by default.
    std::array<uint32_t, 32> layerMatrix = AllCollide();
    bool LayerCollides(int a, int b) const;
    void SetLayerCollision(int a, int b, bool on);
    bool layerMatrixIsDefault() const;

    static std::array<uint32_t, 32> AllCollide();

    Scene() = default;
    ~Scene() = default;
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    GameObject* CreateEntity(const std::string& name, GameObject* parent = nullptr);
    void DestroyEntityNow(GameObject* go);  // removes subtree immediately
    void QueueDestroy(GameObject* go);
    void ProcessDeferred();
    GameObject* FindEntity(EntityId id);
    GameObject* FindByGuid(const std::string& guid);
    GameObject* FindByName(const std::string& name);
    std::vector<GameObject*> FindByTag(const std::string& tag);
    CameraComponent* MainCamera();

    // Serialization -------------------------------------------------------
    Json SerializeEntity(const GameObject* go) const;      // single entity, flat
    // Same as SerializeEntity but nests descendants under a "children" array;
    // this is the form duplication, Instantiate() and .prefab files use.
    Json SerializeSubtree(const GameObject* go) const;
    GameObject* DeserializeEntitySubtree(const Json& entityJson, GameObject* parent,
                                         std::string* error = nullptr);
    // Creates a deep copy of `go` (including children) under `parent`.
    GameObject* InstantiateCopy(const GameObject* go, GameObject* parent = nullptr);
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

// Applies one serialized component entry ({"type", ...props, "enabled"}) to an
// entity, updating the component in place or adding it when absent. Shared by
// the scene loaders and the prefab sync. Implemented in scene.cpp.
bool ApplyComponentEntry(GameObject* go, const Json& cj, std::string* error);

}  // namespace Cairn
