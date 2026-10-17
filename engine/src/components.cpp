#include "scene_internal.h"

namespace Cairn {

namespace {

// ---------------------------------------------------------------------------
// JSON <-> field value conversions.
// ---------------------------------------------------------------------------
inline Json ToJson(float v) { return v; }
inline Json ToJson(int v) { return (double)v; }
inline Json ToJson(bool v) { return v; }
inline Json ToJson(const std::string& v) { return v; }
inline Json ToJson(const Vec2& v) { return Json::MakeArray({v.x, v.y}); }
inline Json ToJson(const Vec3& v) { return Json::MakeArray({v.x, v.y, v.z}); }
inline Json ToJson(const Vec4& v) { return Json::MakeArray({v.x, v.y, v.z, v.w}); }
inline Json ToJson(const Color& c) { return Json::MakeArray({c.r, c.g, c.b, c.a}); }

template <typename T>
T FromJson(const Json& j, T def) {
    if (j.isNull()) return def;
    if constexpr (std::is_same_v<T, float>) return j.asFloat(def);
    else if constexpr (std::is_same_v<T, int>) return j.asInt(def);
    else if constexpr (std::is_same_v<T, bool>) return j.asBool(def);
    else if constexpr (std::is_same_v<T, std::string>) return j.isString() ? j.asString() : def;
    else if constexpr (std::is_enum_v<T>) return static_cast<T>(j.asInt((int)def));
    else if constexpr (std::is_same_v<T, Vec2>) {
        return j.isArray() && j.size() >= 2 ? Vec2{j[0].asFloat(), j[1].asFloat()} : def;
    } else if constexpr (std::is_same_v<T, Vec3>) {
        return j.isArray() && j.size() >= 3 ? Vec3{j[0].asFloat(), j[1].asFloat(), j[2].asFloat()} : def;
    } else if constexpr (std::is_same_v<T, Vec4>) {
        return j.isArray() && j.size() >= 4
                   ? Vec4{j[0].asFloat(), j[1].asFloat(), j[2].asFloat(), j[3].asFloat()}
                   : def;
    } else if constexpr (std::is_same_v<T, Color>) {
        return j.isArray() && j.size() >= 4
                   ? Color{j[0].asFloat(), j[1].asFloat(), j[2].asFloat(), j[3].asFloat()}
                   : def;
    }
    return def;
}

// Scoped enums serialize as integers.
template <typename E, std::enable_if_t<std::is_enum_v<E>, int> = 0>
Json ToJson(E e) { return (double)(int)e; }

// Member-object-pointer based accessors: one instantiation per field.
template <typename C, auto Member>
Json MemberGet(const Component* c) {
    const C* cc = static_cast<const C*>(c);
    return ToJson(cc->*Member);
}

template <typename C, auto Member>
void MemberSet(Component* c, const Json& j) {
    C* cc = static_cast<C*>(c);
    using T = std::remove_reference_t<decltype(cc->*Member)>;
    cc->*Member = FromJson<T>(j, cc->*Member);
}

template <typename C, auto Member>
constexpr PropertyDef Field(const char* name, const char* label, EditorPropKind kind) {
    return PropertyDef{name, label, kind, &MemberGet<C, Member>, &MemberSet<C, Member>, nullptr, 0, ""};
}

template <typename C, auto Member>
constexpr PropertyDef EnumField(const char* name, const char* label, const char* const* options, int count) {
    return PropertyDef{name, label, EditorPropKind::Enum, &MemberGet<C, Member>, &MemberSet<C, Member>,
                       options, count, ""};
}

template <typename C, auto Member>
void AssetMemberSet(Component* c, const Json& j) {
    // Accepts a bare path (inspector / CLI) or a {"path","guid"} scene reference.
    static_cast<C*>(c)->*Member = AssetRefFromJson(j);
}

template <typename C, auto Member>
constexpr PropertyDef AssetField(const char* name, const char* label, const char* assetKind) {
    return PropertyDef{name, label, EditorPropKind::Asset, &MemberGet<C, Member>, &AssetMemberSet<C, Member>,
                       nullptr, 0, assetKind};
}

// A GameObject reference stored as the target's stable GUID string.
template <typename C, auto Member>
constexpr PropertyDef ObjectField(const char* name, const char* label) {
    return PropertyDef{name, label, EditorPropKind::Object, &MemberGet<C, Member>, &MemberSet<C, Member>,
                       nullptr, 0, ""};
}

const char* const kMeshSources[] = {"Cube", "Sphere", "Plane", "Quad", "Cylinder", "Cone", "Torus", "Custom (OBJ)"};
const char* const kLightTypes[] = {"Directional", "Point", "Spot"};
const char* const kAlignments[] = {"Left", "Center", "Right"};
const char* const kCollisionDetections[] = {"Discrete", "Continuous"};

// ---------------------------------------------------------------------------
// Per-component property tables.
// ---------------------------------------------------------------------------
const std::vector<PropertyDef>& CameraDefs() {
    static const std::vector<PropertyDef> defs = {
        Field<CameraComponent, &CameraComponent::fov>("fov", "Field of View", EditorPropKind::Float),
        Field<CameraComponent, &CameraComponent::nearPlane>("nearPlane", "Near Plane", EditorPropKind::Float),
        Field<CameraComponent, &CameraComponent::farPlane>("farPlane", "Far Plane", EditorPropKind::Float),
        Field<CameraComponent, &CameraComponent::orthographic>("orthographic", "Orthographic", EditorPropKind::Bool),
        Field<CameraComponent, &CameraComponent::orthographicSize>("orthoSize", "Ortho Size", EditorPropKind::Float),
        Field<CameraComponent, &CameraComponent::clearColor>("clearColor", "Clear Color", EditorPropKind::Color),
        Field<CameraComponent, &CameraComponent::skyEnabled>("skyEnabled", "Sky", EditorPropKind::Bool),
        Field<CameraComponent, &CameraComponent::skyTop>("skyTop", "Sky Top", EditorPropKind::Color),
        Field<CameraComponent, &CameraComponent::skyBottom>("skyBottom", "Sky Bottom", EditorPropKind::Color),
    };
    return defs;
}

const std::vector<PropertyDef>& LightDefs() {
    static const std::vector<PropertyDef> defs = {
        EnumField<LightComponent, &LightComponent::type>("type", "Type", kLightTypes, 3),
        Field<LightComponent, &LightComponent::color>("color", "Color", EditorPropKind::Color),
        Field<LightComponent, &LightComponent::intensity>("intensity", "Intensity", EditorPropKind::Float),
        Field<LightComponent, &LightComponent::range>("range", "Range", EditorPropKind::Float),
        Field<LightComponent, &LightComponent::spotAngle>("spotAngle", "Spot Angle", EditorPropKind::Float),
        Field<LightComponent, &LightComponent::spotInnerAngle>("spotInnerAngle", "Spot Inner Angle", EditorPropKind::Float),
        Field<LightComponent, &LightComponent::castShadows>("castShadows", "Cast Shadows", EditorPropKind::Bool),
        Field<LightComponent, &LightComponent::shadowBias>("shadowBias", "Shadow Bias", EditorPropKind::Float),
    };
    return defs;
}

static const char* const kCullModes[] = {"Off", "Back", "Front"};

const std::vector<PropertyDef>& MeshRendererDefs() {
    static const std::vector<PropertyDef> defs = {
        EnumField<MeshRenderer, &MeshRenderer::mesh>("mesh", "Mesh", kMeshSources, 8),
        AssetField<MeshRenderer, &MeshRenderer::meshPath>("meshPath", "Mesh File", "model"),
        Field<MeshRenderer, &MeshRenderer::albedoColor>("albedoColor", "Albedo", EditorPropKind::Color),
        AssetField<MeshRenderer, &MeshRenderer::albedoTexturePath>("albedoTexturePath", "Albedo Texture", "texture"),
        Field<MeshRenderer, &MeshRenderer::tiling>("tiling", "Tiling", EditorPropKind::Vec2),
        Field<MeshRenderer, &MeshRenderer::specular>("specular", "Specular", EditorPropKind::Float),
        Field<MeshRenderer, &MeshRenderer::shininess>("shininess", "Shininess", EditorPropKind::Float),
        Field<MeshRenderer, &MeshRenderer::metallic>("metallic", "Metallic", EditorPropKind::Float),
        Field<MeshRenderer, &MeshRenderer::roughness>("roughness", "Roughness (-1 = from shininess)", EditorPropKind::Float),
        AssetField<MeshRenderer, &MeshRenderer::normalMapPath>("normalMapPath", "Normal Map", "texture"),
        Field<MeshRenderer, &MeshRenderer::normalStrength>("normalStrength", "Normal Strength", EditorPropKind::Float),
        Field<MeshRenderer, &MeshRenderer::emissiveColor>("emissiveColor", "Emissive", EditorPropKind::Color),
        Field<MeshRenderer, &MeshRenderer::unlit>("unlit", "Unlit", EditorPropKind::Bool),
        EnumField<MeshRenderer, &MeshRenderer::cullMode>("cullMode", "Cull Mode", kCullModes, 3),
        Field<MeshRenderer, &MeshRenderer::antialias>("antialias", "Antialias", EditorPropKind::Bool),
        Field<MeshRenderer, &MeshRenderer::castShadows>("castShadows", "Cast Shadows", EditorPropKind::Bool),
        Field<MeshRenderer, &MeshRenderer::receiveShadows>("receiveShadows", "Receive Shadows", EditorPropKind::Bool),
    };
    return defs;
}

const std::vector<PropertyDef>& SpriteRendererDefs() {
    static const std::vector<PropertyDef> defs = {
        AssetField<SpriteRenderer, &SpriteRenderer::texturePath>("texturePath", "Texture", "texture"),
        Field<SpriteRenderer, &SpriteRenderer::color>("color", "Color", EditorPropKind::Color),
        Field<SpriteRenderer, &SpriteRenderer::pixelsPerUnit>("pixelsPerUnit", "Pixels Per Unit", EditorPropKind::Float),
        Field<SpriteRenderer, &SpriteRenderer::sortingLayer>("sortingLayer", "Sorting Layer", EditorPropKind::Int),
        Field<SpriteRenderer, &SpriteRenderer::orderInLayer>("orderInLayer", "Order in Layer", EditorPropKind::Int),
        Field<SpriteRenderer, &SpriteRenderer::flipX>("flipX", "Flip X", EditorPropKind::Bool),
        Field<SpriteRenderer, &SpriteRenderer::flipY>("flipY", "Flip Y", EditorPropKind::Bool),
        AssetField<SpriteRenderer, &SpriteRenderer::normalMapPath>("normalMapPath", "Normal Map", "texture"),
        Field<SpriteRenderer, &SpriteRenderer::pixelPerfect>("pixelPerfect", "Pixel Perfect", EditorPropKind::Bool),
    };
    return defs;
}

const std::vector<PropertyDef>& TextRendererDefs() {
    static const std::vector<PropertyDef> defs = {
        Field<TextRenderer, &TextRenderer::text>("text", "Text", EditorPropKind::String),
        Field<TextRenderer, &TextRenderer::fontSize>("fontSize", "Font Size", EditorPropKind::Float),
        Field<TextRenderer, &TextRenderer::color>("color", "Color", EditorPropKind::Color),
        Field<TextRenderer, &TextRenderer::sortingLayer>("sortingLayer", "Sorting Layer", EditorPropKind::Int),
        Field<TextRenderer, &TextRenderer::orderInLayer>("orderInLayer", "Order in Layer", EditorPropKind::Int),
        EnumField<TextRenderer, &TextRenderer::alignment>("alignment", "Alignment", kAlignments, 3),
    };
    return defs;
}

const std::vector<PropertyDef>& RigidBodyDefs() {
    static const std::vector<PropertyDef> defs = {
        Field<RigidBody, &RigidBody::mass>("mass", "Mass", EditorPropKind::Float),
        Field<RigidBody, &RigidBody::useGravity>("useGravity", "Use Gravity", EditorPropKind::Bool),
        Field<RigidBody, &RigidBody::gravityScale>("gravityScale", "Gravity Scale", EditorPropKind::Float),
        Field<RigidBody, &RigidBody::isKinematic>("isKinematic", "Is Kinematic", EditorPropKind::Bool),
        Field<RigidBody, &RigidBody::drag>("drag", "Drag", EditorPropKind::Float),
        Field<RigidBody, &RigidBody::restitution>("restitution", "Bounciness", EditorPropKind::Float),
        Field<RigidBody, &RigidBody::friction>("friction", "Friction", EditorPropKind::Float),
        Field<RigidBody, &RigidBody::freezeZ>("freezeZ", "Freeze Z", EditorPropKind::Bool),
        EnumField<RigidBody, &RigidBody::collisionDetection>("collisionDetection",
                                                             "Collision Detection",
                                                             kCollisionDetections, 2),
        Field<RigidBody, &RigidBody::allowSleep>("allowSleep", "Allow Sleep", EditorPropKind::Bool),
        Field<RigidBody, &RigidBody::sleepThreshold>("sleepThreshold", "Sleep Threshold", EditorPropKind::Float),
        Field<RigidBody, &RigidBody::sleepDelay>("sleepDelay", "Sleep Delay", EditorPropKind::Float),
    };
    return defs;
}

const std::vector<PropertyDef>& BoxColliderDefs() {
    static const std::vector<PropertyDef> defs = {
        Field<BoxCollider, &BoxCollider::center>("center", "Center", EditorPropKind::Vec3),
        Field<BoxCollider, &BoxCollider::size>("size", "Size", EditorPropKind::Vec3),
        Field<BoxCollider, &BoxCollider::isTrigger>("isTrigger", "Is Trigger", EditorPropKind::Bool),
    };
    return defs;
}

const std::vector<PropertyDef>& SphereColliderDefs() {
    static const std::vector<PropertyDef> defs = {
        Field<SphereCollider, &SphereCollider::center>("center", "Center", EditorPropKind::Vec3),
        Field<SphereCollider, &SphereCollider::radius>("radius", "Radius", EditorPropKind::Float),
        Field<SphereCollider, &SphereCollider::isTrigger>("isTrigger", "Is Trigger", EditorPropKind::Bool),
    };
    return defs;
}

const std::vector<PropertyDef>& Collider2DDefs() {
    static const std::vector<PropertyDef> defs = {
        Field<Collider2D, &Collider2D::center>("center", "Center", EditorPropKind::Vec2),
        Field<Collider2D, &Collider2D::size>("size", "Size", EditorPropKind::Vec2),
        Field<Collider2D, &Collider2D::isTrigger>("isTrigger", "Is Trigger", EditorPropKind::Bool),
    };
    return defs;
}

static const char* const kCapsuleDirections[] = {"Y", "X", "Z"};

const std::vector<PropertyDef>& CapsuleColliderDefs() {
    static const std::vector<PropertyDef> defs = {
        Field<CapsuleCollider, &CapsuleCollider::center>("center", "Center", EditorPropKind::Vec3),
        Field<CapsuleCollider, &CapsuleCollider::radius>("radius", "Radius", EditorPropKind::Float),
        Field<CapsuleCollider, &CapsuleCollider::height>("height", "Height", EditorPropKind::Float),
        EnumField<CapsuleCollider, &CapsuleCollider::direction>("direction", "Direction", kCapsuleDirections, 3),
        Field<CapsuleCollider, &CapsuleCollider::isTrigger>("isTrigger", "Is Trigger", EditorPropKind::Bool),
    };
    return defs;
}

const std::vector<PropertyDef>& CharacterControllerDefs() {
    static const std::vector<PropertyDef> defs = {
        Field<CharacterController, &CharacterController::center>("center", "Center", EditorPropKind::Vec3),
        Field<CharacterController, &CharacterController::radius>("radius", "Radius", EditorPropKind::Float),
        Field<CharacterController, &CharacterController::height>("height", "Height", EditorPropKind::Float),
        Field<CharacterController, &CharacterController::slopeLimit>("slopeLimit", "Slope Limit", EditorPropKind::Float),
        Field<CharacterController, &CharacterController::stepOffset>("stepOffset", "Step Offset", EditorPropKind::Float),
        Field<CharacterController, &CharacterController::skinWidth>("skinWidth", "Skin Width", EditorPropKind::Float),
        Field<CharacterController, &CharacterController::useGravity>("useGravity", "Use Gravity", EditorPropKind::Bool),
        Field<CharacterController, &CharacterController::gravityScale>("gravityScale", "Gravity Scale", EditorPropKind::Float),
    };
    return defs;
}

const std::vector<PropertyDef>& DistanceJointDefs() {
    static const std::vector<PropertyDef> defs = {
        ObjectField<DistanceJoint, &DistanceJoint::connectedGuid>("connectedGuid", "Connected Body"),
        Field<DistanceJoint, &DistanceJoint::anchor>("anchor", "Anchor", EditorPropKind::Vec3),
        Field<DistanceJoint, &DistanceJoint::connectedAnchor>("connectedAnchor", "Connected Anchor", EditorPropKind::Vec3),
        Field<DistanceJoint, &DistanceJoint::distance>("distance", "Distance", EditorPropKind::Float),
        Field<DistanceJoint, &DistanceJoint::spring>("spring", "Spring", EditorPropKind::Float),
        Field<DistanceJoint, &DistanceJoint::damping>("damping", "Damping", EditorPropKind::Float),
        Field<DistanceJoint, &DistanceJoint::breakForce>("breakForce", "Break Force", EditorPropKind::Float),
    };
    return defs;
}

const std::vector<PropertyDef>& HingeJointDefs() {
    static const std::vector<PropertyDef> defs = {
        ObjectField<HingeJoint, &HingeJoint::connectedGuid>("connectedGuid", "Connected Body"),
        Field<HingeJoint, &HingeJoint::anchor>("anchor", "Anchor", EditorPropKind::Vec3),
        Field<HingeJoint, &HingeJoint::connectedAnchor>("connectedAnchor", "Connected Anchor", EditorPropKind::Vec3),
        Field<HingeJoint, &HingeJoint::axis>("axis", "Axis", EditorPropKind::Vec3),
        Field<HingeJoint, &HingeJoint::useLimits>("useLimits", "Use Limits", EditorPropKind::Bool),
        Field<HingeJoint, &HingeJoint::limitsMin>("limitsMin", "Min Limit", EditorPropKind::Float),
        Field<HingeJoint, &HingeJoint::limitsMax>("limitsMax", "Max Limit", EditorPropKind::Float),
        Field<HingeJoint, &HingeJoint::useMotor>("useMotor", "Use Motor", EditorPropKind::Bool),
        Field<HingeJoint, &HingeJoint::motorVelocity>("motorVelocity", "Motor Velocity", EditorPropKind::Float),
        Field<HingeJoint, &HingeJoint::motorForce>("motorForce", "Motor Force", EditorPropKind::Float),
        Field<HingeJoint, &HingeJoint::breakForce>("breakForce", "Break Force", EditorPropKind::Float),
    };
    return defs;
}

const std::vector<PropertyDef>& FixedJointDefs() {
    static const std::vector<PropertyDef> defs = {
        ObjectField<FixedJoint, &FixedJoint::connectedGuid>("connectedGuid", "Connected Body"),
        Field<FixedJoint, &FixedJoint::breakForce>("breakForce", "Break Force", EditorPropKind::Float),
    };
    return defs;
}

const std::vector<PropertyDef>& ScriptDefs() {
    static const std::vector<PropertyDef> defs = {
        PropertyDef{"class", "Script", EditorPropKind::String,
                    [](const Component* c) -> Json { return static_cast<const ScriptComponent*>(c)->scriptClass; },
                    [](Component* c, const Json& j) { static_cast<ScriptComponent*>(c)->scriptClass = j.asString(); }},
    };
    return defs;
}

const std::vector<PropertyDef>& PrefabInstanceDefs() {
    static const std::vector<PropertyDef> defs = {
        AssetField<PrefabInstance, &PrefabInstance::prefabPath>("prefabPath", "Prefab", "prefab"),
    };
    return defs;
}

const std::vector<PropertyDef>* DefsForTypeName(const std::string& typeName) {
    if (typeName == "Camera") return &CameraDefs();
    if (typeName == "Light") return &LightDefs();
    if (typeName == "MeshRenderer") return &MeshRendererDefs();
    if (typeName == "SpriteRenderer") return &SpriteRendererDefs();
    if (typeName == "TextRenderer") return &TextRendererDefs();
    if (typeName == "RigidBody") return &RigidBodyDefs();
    if (typeName == "BoxCollider") return &BoxColliderDefs();
    if (typeName == "SphereCollider") return &SphereColliderDefs();
    if (typeName == "Collider2D") return &Collider2DDefs();
    if (typeName == "CapsuleCollider") return &CapsuleColliderDefs();
    if (typeName == "CharacterController") return &CharacterControllerDefs();
    if (typeName == "DistanceJoint") return &DistanceJointDefs();
    if (typeName == "HingeJoint") return &HingeJointDefs();
    if (typeName == "FixedJoint") return &FixedJointDefs();
    if (typeName == "PrefabInstance") return &PrefabInstanceDefs();
    if (typeName == "Script") return &ScriptDefs();
    return nullptr;
}

}  // namespace

const std::vector<PropertyDef>* ComponentPropertyDefs(const Component* c) {
    if (!c || c->TypeId() == ComponentTypeId::Transform) return nullptr;
    return DefsForTypeName(c->TypeName());
}

const std::vector<PropertyDef>* PropertyDefsForType(const std::string& typeName) {
    return DefsForTypeName(typeName);
}

bool ComponentGetProperty(const Component* c, const std::string& name, Json* out) {
    const auto* defs = ComponentPropertyDefs(c);
    if (!defs) return false;
    for (const auto& def : *defs) {
        if (name == def.name) {
            *out = def.get(c);
            return true;
        }
    }
    return false;
}

bool ComponentSetProperty(Component* c, const std::string& name, const Json& value) {
    auto* defs = ComponentPropertyDefs(c);
    if (!defs) return false;
    for (const auto& def : *defs) {
        if (name == def.name) {
            def.set(c, value);
            return true;
        }
    }
    return false;
}

Json ComponentSerialize(const Component* c) {
    Json props = Json::MakeObject();
    if (const auto* defs = ComponentPropertyDefs(c)) {
        for (const auto& def : *defs) props.set(def.name, def.get(c));
    }
    return props;
}

bool ComponentDeserialize(Component* c, const Json& props) {
    if (!props.isObject()) return true;
    const auto* defs = ComponentPropertyDefs(c);
    if (!defs) return true;
    bool allOk = true;
    for (const auto& def : *defs) {
        const Json* v = props.find(def.name);
        if (v) {
            if (!ComponentSetProperty(c, def.name, *v)) allOk = false;
        }
    }
    return allOk;
}

std::vector<std::string> AddableComponentNames() {
    return {"Camera", "Light", "MeshRenderer", "SpriteRenderer", "TextRenderer",
            "RigidBody", "BoxCollider", "SphereCollider", "Collider2D",
            "CapsuleCollider", "CharacterController", "DistanceJoint",
            "HingeJoint", "FixedJoint", "Script"};
}

}  // namespace Cairn
