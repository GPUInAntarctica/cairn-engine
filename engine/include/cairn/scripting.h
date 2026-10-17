// Cairn Engine - public scripting API.
//
// This header is the stable contract between the engine runtime and gameplay
// code. Gameplay scripts include <cairn/cairn.h>, subclass Cairn::Component,
// and register themselves with CAIRN_SCRIPT(ClassName). The design follows
// the component model popularized by Unity: behaviour lives in components
// attached to GameObjects, driven by OnStart / OnUpdate / FixedUpdate /
// OnDestroy lifecycle callbacks.
#pragma once

#include "json.h"
#include "log.h"   // CAIRN_PRINTF
#include "math.h"

#include <cstdint>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

namespace Cairn {

using EntityId = uint32_t;
constexpr EntityId kInvalidEntity = 0;

// ---------------------------------------------------------------------------
// Keyboard / mouse codes shared between the engine and the editor host.
// Values are stable; the editor maps platform key names onto them.
// ---------------------------------------------------------------------------
enum class KeyCode : uint32_t {
    None = 0,
    Space, Apostrophe, Comma, Minus, Period, Slash, Semicolon, Equal,
    LeftBracket, Backslash, RightBracket, Backquote,
    D0, D1, D2, D3, D4, D5, D6, D7, D8, D9,
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Escape, Enter, Tab, Backspace, Insert, Delete,
    Right, Left, Down, Up, PageUp, PageDown, Home, End,
    CapsLock, ScrollLock, NumLock, PrintScreen, Pause,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    LeftShift, LeftControl, LeftAlt, LeftSuper,
    RightShift, RightControl, RightAlt, RightSuper,
    MouseLeft = 400, MouseRight, MouseMiddle,
};

// ---------------------------------------------------------------------------
// Forward declarations.
// ---------------------------------------------------------------------------
class Scene;
class GameObject;
class Component;
class Transform;
class CameraComponent;
class LightComponent;
class MeshRenderer;
class SpriteRenderer;
class TextRenderer;
class RigidBody;
class BoxCollider;
class SphereCollider;
class Collider2D;
class CapsuleCollider;
class MeshCollider;
class CharacterController;
class DistanceJoint;
class HingeJoint;
class FixedJoint;
class ScriptComponent;
class PrefabInstance;
struct ScriptMeta;

// ---------------------------------------------------------------------------
// Component metadata (script-exposed fields shown in the editor inspector).
// ---------------------------------------------------------------------------
enum class PropType : int {
    Float, Int, Bool, String, Vec2, Vec3, Color,
    // Added in 0.1.2. Appended so existing numeric values stay stable.
    Vec4,     // four-component vector
    Enum,     // integer index into a named option list
    Asset,    // project asset reference (path or {path,guid})
    Object,   // GameObject reference, stored as the target's stable GUID
};

struct ScriptProperty {
    std::string name;
    PropType type = PropType::Float;
    Json defaultValue;
};

struct ScriptClassRecord {
    const char* name = nullptr;
    Component* (*create)() = nullptr;
};

namespace detail {
inline std::vector<ScriptClassRecord>& ScriptRegistry() {
    static std::vector<ScriptClassRecord> registry;
    return registry;
}
inline bool RegisterScriptRecord(const char* name, Component* (*create)()) {
    ScriptRegistry().push_back({name, create});
    return true;
}
}  // namespace detail

// Registers a gameplay script class. Place once in the class's .cpp, outside
// any namespace: `CAIRN_SCRIPT(PlayerController)`.
#define CAIRN_SCRIPT(CLASS)                                                        \
    static ::Cairn::Component* CairnCreateFn_##CLASS() { return new (std::nothrow) CLASS(); } \
    static const bool CairnRegOk_##CLASS =                                         \
        ::Cairn::detail::RegisterScriptRecord(#CLASS, &CairnCreateFn_##CLASS);

// ---------------------------------------------------------------------------
// Components - identity used by GetComponent<T> and serialization.
// ---------------------------------------------------------------------------
enum class ComponentTypeId : int {
    Transform = 1, Camera, Light, MeshRenderer, SpriteRenderer, TextRenderer,
    RigidBody, BoxCollider, SphereCollider, Collider2D, Script, PrefabInstance,
    // Added in 0.1.6. Appended so existing numeric values stay stable.
    CapsuleCollider, CharacterController,
    // Added in 0.1.7.
    DistanceJoint,
    // Added in 0.1.16.
    HingeJoint, FixedJoint,
    // Added in 0.1.19.
    MeshCollider,
    Other = 99,
};

template <typename T>
struct ComponentTypeOf {
    // Gameplay script types resolve to Script and are matched via dynamic_cast.
    static constexpr ComponentTypeId id = ComponentTypeId::Script;
};
#define CAIRN_COMPONENT_TYPE(CLASS, ID) \
    template <> struct ComponentTypeOf<CLASS> { static constexpr ComponentTypeId id = ComponentTypeId::ID; };

CAIRN_COMPONENT_TYPE(Transform, Transform)
CAIRN_COMPONENT_TYPE(CameraComponent, Camera)
CAIRN_COMPONENT_TYPE(LightComponent, Light)
CAIRN_COMPONENT_TYPE(MeshRenderer, MeshRenderer)
CAIRN_COMPONENT_TYPE(SpriteRenderer, SpriteRenderer)
CAIRN_COMPONENT_TYPE(TextRenderer, TextRenderer)
CAIRN_COMPONENT_TYPE(RigidBody, RigidBody)
CAIRN_COMPONENT_TYPE(BoxCollider, BoxCollider)
CAIRN_COMPONENT_TYPE(SphereCollider, SphereCollider)
CAIRN_COMPONENT_TYPE(Collider2D, Collider2D)
CAIRN_COMPONENT_TYPE(ScriptComponent, Script)
CAIRN_COMPONENT_TYPE(PrefabInstance, PrefabInstance)
CAIRN_COMPONENT_TYPE(CapsuleCollider, CapsuleCollider)
CAIRN_COMPONENT_TYPE(MeshCollider, MeshCollider)
CAIRN_COMPONENT_TYPE(CharacterController, CharacterController)
CAIRN_COMPONENT_TYPE(DistanceJoint, DistanceJoint)
CAIRN_COMPONENT_TYPE(HingeJoint, HingeJoint)
CAIRN_COMPONENT_TYPE(FixedJoint, FixedJoint)

// ---------------------------------------------------------------------------
// Component - base class for all behaviour. Gameplay scripts subclass this.
// ---------------------------------------------------------------------------
class Component {
public:
    Component() = default;
    virtual ~Component() = default;

    // Engine components return their stable type name ("Transform", ...).
    // Gameplay script classes should not override this; registration names them.
    virtual const char* TypeName() const { return "Script"; }
    virtual ComponentTypeId TypeId() const { return ComponentTypeId::Script; }

    // Lifecycle - invoked by the engine.
    virtual void OnStart() {}
    virtual void OnUpdate(float deltaTime) { (void)deltaTime; }
    virtual void FixedUpdate(float fixedDeltaTime) { (void)fixedDeltaTime; }
    virtual void OnDestroy() {}
    // Trigger callbacks fire when a collider marked isTrigger overlaps.
    virtual void OnTriggerEnter(Component* other) { (void)other; }
    virtual void OnTriggerExit(Component* other) { (void)other; }
    // Collision callbacks fire for solid contacts (Enter once per touching
    // pair, Stay every step while touching, Exit when the contact ends).
    virtual void OnCollisionEnter(Component* other) { (void)other; }
    virtual void OnCollisionStay(Component* other) { (void)other; }
    virtual void OnCollisionExit(Component* other) { (void)other; }

    bool enabled = true;  // disabled components skip lifecycle and callbacks

    GameObject& gameObject() const { return *m_go; }
    Transform& transform() const;

    // Declare inspector-editable fields (called once on a probe instance by
    // the editor). Example:
    //   void DescribeProps(ScriptMeta& m) override {
    //       m.Add("speed", PropType::Float, 6.0f);
    //   }
    virtual void DescribeProps(ScriptMeta& meta) { (void)meta; }

    // Read the current value of an inspector field. Values are supplied by
    // the ScriptComponent holding this instance and fall back to defaults.
    float PropFloat(const std::string& name) const { return GetProp(name).asFloat(); }
    int PropInt(const std::string& name) const { return GetProp(name).asInt(); }
    bool PropBool(const std::string& name) const { return GetProp(name).asBool(); }
    std::string PropString(const std::string& name) const { return GetProp(name).asString(); }
    Vec2 PropVec2(const std::string& name) const {
        const Json& j = GetProp(name);
        return {j[0].asFloat(), j[1].asFloat()};
    }
    Vec3 PropVec3(const std::string& name) const {
        const Json& j = GetProp(name);
        return {j[0].asFloat(), j[1].asFloat(), j[2].asFloat()};
    }
    Color PropColor(const std::string& name) const {
        const Json& j = GetProp(name);
        return {j[0].asFloat(), j[1].asFloat(), j[2].asFloat(), j[3].asFloat()};
    }
    Vec4 PropVec4(const std::string& name) const {
        const Json& j = GetProp(name);
        return {j[0].asFloat(), j[1].asFloat(), j[2].asFloat(), j[3].asFloat()};
    }
    // Enum fields are stored as their integer index.
    int PropEnum(const std::string& name) const { return GetProp(name).asInt(); }
    // Asset fields read back as a project-relative path string.
    std::string PropAsset(const std::string& name) const { return GetProp(name).asString(); }
    // Object fields read back as the target GameObject's stable GUID.
    std::string PropObject(const std::string& name) const { return GetProp(name).asString(); }

    // Structured helper used by ScriptMeta below.
    const Json& GetProp(const std::string& name) const {
        static const Json kNull;
        const Json* v = m_propSource ? m_propSource->find(name) : nullptr;
        return v ? *v : kNull;
    }

    // Internal wiring (engine-only).
    GameObject* m_go = nullptr;
    const Json* m_propSource = nullptr;  // ScriptComponent::properties of the owner
    uint32_t m_componentId = 0;
};

struct ScriptMeta {
    std::vector<ScriptProperty> properties;
    void Add(const std::string& name, PropType type, Json defaultValue) {
        properties.push_back({name, type, std::move(defaultValue)});
    }
};

// ---------------------------------------------------------------------------
// Transform - position/rotation/scale with parent hierarchy. Every GameObject
// owns exactly one; it cannot be removed.
// ---------------------------------------------------------------------------
class Transform : public Component {
public:
    Vec3 position{0, 0, 0};
    Quat rotation{};       // local rotation
    Vec3 scale{1, 1, 1};

    // Euler angles in degrees (YXZ order) - convenient for editor/UI code.
    Vec3 GetEulerDegrees() const { return rotation.ToEulerDegrees(); }
    void SetEulerDegrees(const Vec3& euler) { rotation = Quat::FromEulerDegrees(euler); }

    GameObject* GetParent() const { return m_parent; }
    const std::vector<GameObject*>& GetChildren() const { return m_children; }
    void SetParent(GameObject* parent);  // nullptr detaches; must live in same scene

    Mat4 LocalMatrix() const;
    Mat4 WorldMatrix();
    Vec3 WorldPosition();
    Quat WorldRotation();
    Vec3 WorldScale();
    Vec3 Forward() { return WorldRotation() * Vec3::Forward; }
    Vec3 Right() { return WorldRotation() * Vec3::Right; }
    Vec3 Up() { return WorldRotation() * Vec3::Up; }

    const char* TypeName() const override { return "Transform"; }
    ComponentTypeId TypeId() const override { return ComponentTypeId::Transform; }

    // Internal.
    GameObject* m_parent = nullptr;
    std::vector<GameObject*> m_children;
    bool m_worldDirty = true;
    Mat4 m_world;

    // Change tracking. Position/rotation/scale stay public fields so gameplay
    // code can assign them directly; the world matrix is refreshed lazily by
    // comparing the cached local matrix and the parent's world version, so a
    // raw field write can never leave a stale world matrix behind.
    Mat4 m_localCache = Mat4::Identity();
    uint32_t m_worldVersion = 0;
    uint32_t m_seenParentVersion = 0;
    bool m_worldValid = false;

    // Marks this object and its descendants as needing a world-matrix rebuild.
    void MarkWorldDirty();
};

// ---------------------------------------------------------------------------
// Camera - the view used when the game runs (tag "MainCamera" or the first one).
// ---------------------------------------------------------------------------
class CameraComponent : public Component {
public:
    float fov = 60.0f;             // vertical FOV, degrees
    float nearPlane = 0.1f;
    float farPlane = 500.0f;
    bool orthographic = false;
    float orthographicSize = 5.0f; // half height of the view in world units
    Color clearColor = Color::FromHex(0x101018);
    bool skyEnabled = true;
    Color skyTop = Color::FromHex(0x2e3547);
    Color skyBottom = Color::FromHex(0x8b97ab);

    const char* TypeName() const override { return "Camera"; }
    ComponentTypeId TypeId() const override { return ComponentTypeId::Camera; }
};

// ---------------------------------------------------------------------------
// Light - directional or point.
// ---------------------------------------------------------------------------
class LightComponent : public Component {
public:
    enum class LightType : int { Directional = 0, Point, Spot };
    LightType type = LightType::Directional;
    Color color = Color(1, 1, 1);
    float intensity = 1.0f;
    float range = 15.0f;        // point and spot lights
    float spotAngle = 45.0f;    // outer cone, degrees
    float spotInnerAngle = 25.0f;  // inner (full intensity) cone, degrees
    bool castShadows = true;    // directional lights only
    float shadowBias = 0.02f;   // depth offset, fraction of the cascade depth

    const char* TypeName() const override { return "Light"; }
    ComponentTypeId TypeId() const override { return ComponentTypeId::Light; }
};

// ---------------------------------------------------------------------------
// MeshRenderer - 3D primitive or imported .obj mesh with a simple material.
// ---------------------------------------------------------------------------
class MeshRenderer : public Component {
public:
    enum class MeshSource : int { Cube = 0, Sphere, Plane, Quad, Cylinder, Cone, Torus, Custom };
    // Which faces are discarded before rasterization.
    enum class CullMode : int { Off = 0, Back = 1, Front = 2 };
    MeshSource mesh = MeshSource::Cube;
    std::string meshPath;            // project-relative .obj when mesh == Custom
    Color albedoColor = Color(0.78f, 0.78f, 0.80f);
    std::string albedoTexturePath;   // project-relative; empty = solid color
    Vec2 tiling{1, 1};
    float specular = 0.5f;
    float shininess = 32.0f;
    // PBR-ish surface response. metallic: 0 = dielectric, 1 = metal.
    // roughness < 0 means "derive from shininess" so scenes authored before
    // PBR keep their Blinn-Phong look; explicit values in [0,1] override it.
    float metallic = 0.0f;
    float roughness = -1.0f;
    std::string normalMapPath;       // project-relative tangent-space normal map
    float normalStrength = 1.0f;
    Color emissiveColor{0, 0, 0, 1}; // added in linear space, unaffected by lights
    bool unlit = false;
    CullMode cullMode = CullMode::Back;
    bool antialias = true;   // edge coverage antialiasing
    bool castShadows = true;
    bool receiveShadows = true;

    const char* TypeName() const override { return "MeshRenderer"; }
    ComponentTypeId TypeId() const override { return ComponentTypeId::MeshRenderer; }
};

// ---------------------------------------------------------------------------
// SpriteRenderer - textured quad for 2D. Texture drawn with
// worldHeight = texturePixelHeight / pixelsPerUnit.
// ---------------------------------------------------------------------------
class SpriteRenderer : public Component {
public:
    std::string texturePath;        // project-relative (png/jpg/bmp)
    Color color = Color::White;
    float pixelsPerUnit = 100.0f;
    int sortingLayer = 0;     // coarse group, drawn low to high
    int orderInLayer = 0;     // fine order inside a sorting layer
    bool flipX = false;
    bool flipY = false;
    std::string normalMapPath;      // optional tangent-space normal map (2D lighting)
    bool pixelPerfect = false;      // nearest sampling

    const char* TypeName() const override { return "SpriteRenderer"; }
    ComponentTypeId TypeId() const override { return ComponentTypeId::SpriteRenderer; }
};

// ---------------------------------------------------------------------------
// TextRenderer - bitmap text for 2D overlays and world-space labels.
// ---------------------------------------------------------------------------
class TextRenderer : public Component {
public:
    std::string text = "Text";
    int sortingLayer = 0;     // coarse group, drawn low to high
    float fontSize = 24.0f;         // pixels of glyph height per world unit
    Color color = Color::White;
    int orderInLayer = 0;
    enum class Alignment : int { Left = 0, Center, Right };
    Alignment alignment = Alignment::Left;

    const char* TypeName() const override { return "TextRenderer"; }
    ComponentTypeId TypeId() const override { return ComponentTypeId::TextRenderer; }
};

// ---------------------------------------------------------------------------
// RigidBody - velocity integration driven by the physics step.
// ---------------------------------------------------------------------------
class RigidBody : public Component {
public:
    float mass = 1.0f;
    Vec3 velocity{0, 0, 0};
    bool useGravity = true;
    float gravityScale = 1.0f;
    bool isKinematic = false;       // collides but is not pushed
    float drag = 0.0f;              // linear damping per second
    float restitution = 0.0f;       // bounciness 0..1
    float friction = 0.4f;          // tangential damping on contact 0..1
    Vec3 angularVelocity{0, 0, 0};  // radians/second (integrated each step)
    bool freezeZ = false;           // lock motion along Z (2D games)

    // How the step finds collisions for this body. `Discrete` moves the body by
    // `velocity * dt` and only then tests for overlap, so a body whose
    // single-step displacement is larger than an obstacle is thick (a bullet, a
    // long fall) passes straight through it. `Continuous` sweeps the body
    // against solid static/kinematic colliders before moving it and stops it at
    // the first contact, at the cost of a few extra shape tests per step
    // (Unity's CollisionDetectionMode.Continuous).
    enum class CollisionDetection : int { Discrete = 0, Continuous = 1 };
    CollisionDetection collisionDetection = CollisionDetection::Discrete;

    // Sleeping: a body at rest for `sleepDelay` seconds stops integrating and
    // is treated as static by the solver until something disturbs it. AddForce/
    // AddImpulse/AddTorque wake it, and so does writing a non-zero velocity.
    bool allowSleep = true;
    float sleepThreshold = 0.05f;   // speed below which the sleep timer grows
    float sleepDelay = 0.5f;        // seconds below the threshold before sleeping
    bool sleeping = false;          // runtime state

    void AddForce(const Vec3& force) { m_accumForce += force; Wake(); }
    void AddImpulse(const Vec3& impulse) { velocity += impulse; Wake(); }
    void AddTorque(const Vec3& torque) { m_accumTorque += torque; Wake(); }
    // Clears the sleep state and timer (called on any disturbance).
    void Wake() { sleeping = false; m_sleepTimer = 0.0f; }

    const char* TypeName() const override { return "RigidBody"; }
    ComponentTypeId TypeId() const override { return ComponentTypeId::RigidBody; }

    Vec3 m_accumForce{0, 0, 0};     // engine-internal accumulator
    Vec3 m_accumTorque{0, 0, 0};    // engine-internal accumulator
    float m_sleepTimer = 0.0f;      // engine-internal
};

// ---------------------------------------------------------------------------
// Colliders - 3D box/sphere and 2D axis-aligned box. Bodies without any
// collider fall through other bodies; colliders without a RigidBody are static.
// ---------------------------------------------------------------------------
class BoxCollider : public Component {
public:
    Vec3 center{0, 0, 0};
    Vec3 size{1, 1, 1};             // full extents, scaled by transform scale
    bool isTrigger = false;

    const char* TypeName() const override { return "BoxCollider"; }
    ComponentTypeId TypeId() const override { return ComponentTypeId::BoxCollider; }
};

class SphereCollider : public Component {
public:
    Vec3 center{0, 0, 0};
    float radius = 0.5f;            // scaled by max transform scale
    bool isTrigger = false;

    const char* TypeName() const override { return "SphereCollider"; }
    ComponentTypeId TypeId() const override { return ComponentTypeId::SphereCollider; }
};

class Collider2D : public Component {
public:
    Vec2 center{0, 0};
    Vec2 size{1, 1};                // world units, scaled by transform scale XY
    bool isTrigger = false;

    const char* TypeName() const override { return "Collider2D"; }
    ComponentTypeId TypeId() const override { return ComponentTypeId::Collider2D; }
};

// Capsule collider: a swept sphere between two points on the local `direction`
// axis. Radius and height are scaled by the transform (the perpendicular axes
// scale the radius, the chosen axis scales the height). Height is the total
// length including the two hemispherical caps; below 2*radius it degenerates to
// a sphere.
class CapsuleCollider : public Component {
public:
    Vec3 center{0, 0, 0};
    float radius = 0.5f;            // scaled by the two axes perpendicular to direction
    float height = 2.0f;            // total height along direction (>= 2*radius to be a capsule)
    int direction = 0;              // 0 = Y, 1 = X, 2 = Z (Unity convention)
    bool isTrigger = false;

    const char* TypeName() const override { return "CapsuleCollider"; }
    ComponentTypeId TypeId() const override { return ComponentTypeId::CapsuleCollider; }
};

// ---------------------------------------------------------------------------
// MeshCollider - collision geometry taken from a triangle mesh (.obj), for
// level geometry the primitives cannot express: a ramp, a room, a heightfield.
// It is a triangle soup (non-convex) and therefore static/kinematic-only, the
// same rule Unity applies to a non-convex MeshCollider: a body the solver has
// to push cannot carry one, and a mesh on a non-kinematic RigidBody is ignored
// with a warning. `meshPath` is project-relative, resolved like MeshRenderer's.
// ---------------------------------------------------------------------------
class MeshCollider : public Component {
public:
    std::string meshPath;           // project-relative .obj
    bool isTrigger = false;

    const char* TypeName() const override { return "MeshCollider"; }
    ComponentTypeId TypeId() const override { return ComponentTypeId::MeshCollider; }
};

// ---------------------------------------------------------------------------
// CharacterController - a kinematic capsule moved by gameplay code (not by the
// rigidbody solver). Move() sweeps the capsule through the scene, resolves it
// against solid colliders, climbs steps up to stepOffset and reports whether it
// is standing on ground. The capsule is always aligned with the local Y axis.
// ---------------------------------------------------------------------------
class CharacterController : public Component {
public:
    Vec3 center{0, 0, 0};
    float radius = 0.5f;
    float height = 2.0f;
    float slopeLimit = 45.0f;       // degrees; steeper contacts are treated as walls
    float stepOffset = 0.3f;        // max step height climbed while grounded
    float skinWidth = 0.08f;        // ground-probe tolerance
    bool useGravity = true;
    float gravityScale = 1.0f;

    // Runtime state (refreshed by Move/SimpleMove; not serialized).
    bool isGrounded = false;
    Vec3 velocity{0, 0, 0};         // last applied velocity (informational)

    // Moves the controller by `displacement` world units and resolves collisions.
    void Move(const Vec3& displacement);
    // Applies `velocity * Time::DeltaTime` (ignoring velocity.y) plus gravity.
    void SimpleMove(const Vec3& velocity);

    const char* TypeName() const override { return "CharacterController"; }
    ComponentTypeId TypeId() const override { return ComponentTypeId::CharacterController; }
};

// ---------------------------------------------------------------------------
// DistanceJoint - keeps two bodies a fixed distance apart. With `spring` > 0 it
// becomes a spring/damper instead of a rigid constraint. `connectedGuid` is the
// target GameObject's stable GUID; an empty GUID anchors to a fixed world point
// at `connectedAnchor`. At least one of the two bodies needs a RigidBody; a
// body without one acts as a static anchor.
// ---------------------------------------------------------------------------
class DistanceJoint : public Component {
public:
    std::string connectedGuid;      // target GameObject GUID ("" = fixed world point)
    Vec3 anchor{0, 0, 0};           // local anchor on this body
    Vec3 connectedAnchor{0, 0, 0};  // local anchor on the target (world point if unconnected)
    float distance = 0.0f;          // target separation; 0 = captured on the first step
    float spring = 0.0f;            // >0: spring constant; 0 = rigid constraint
    float damping = 0.0f;           // damps relative speed for a spring
    float breakForce = 0.0f;        // 0 = unbreakable; otherwise disables the joint above this force

    const char* TypeName() const override { return "DistanceJoint"; }
    ComponentTypeId TypeId() const override { return ComponentTypeId::DistanceJoint; }

    float m_captured = 0.0f;        // engine-internal: cached 0-distance target
};

// ---------------------------------------------------------------------------
// HingeJoint - allows rotation about a single axis (a door, a pendulum, a
// wheel). The anchor point of this body is pinned to the anchor point of the
// connected body and the two hinge axes are kept parallel, which leaves exactly
// one degree of freedom: rotation about `axis`. With `useLimits` the hinge
// angle (measured from the pose the joint first saw) is clamped to
// [limitsMin, limitsMax] degrees; with `useMotor` the joint drives itself toward
// `motorVelocity` degrees per second, using at most `motorForce`.
// `connectedGuid` is the target GameObject's stable GUID; an empty GUID anchors
// to a fixed world point. At least one of the two bodies needs a RigidBody; a
// body without one acts as a static anchor.
// ---------------------------------------------------------------------------
class HingeJoint : public Component {
public:
    std::string connectedGuid;      // target GameObject GUID ("" = fixed world point)
    Vec3 anchor{0, 0, 0};           // local anchor on this body (a point on the axis)
    Vec3 connectedAnchor{0, 0, 0};  // local anchor on the target (world point if unconnected)
    Vec3 axis{1, 0, 0};             // local hinge axis on this body
    bool useLimits = false;
    float limitsMin = -45.0f;       // degrees, relative to the captured pose
    float limitsMax = 45.0f;        // degrees
    bool useMotor = false;
    float motorVelocity = 0.0f;     // degrees per second
    float motorForce = 0.0f;        // torque cap; 0 disables the motor (as in Unity)
    float breakForce = 0.0f;        // 0 = unbreakable; otherwise disables the joint above this force

    const char* TypeName() const override { return "HingeJoint"; }
    ComponentTypeId TypeId() const override { return ComponentTypeId::HingeJoint; }

    // Engine-internal runtime state (captured on the first step, never
    // serialized): the matching hinge axis and angle reference on the target,
    // and the captured rest angle the limits are measured from.
    bool m_captured = false;
    Vec3 m_axisB{1, 0, 0};
    Vec3 m_refA{0, 0, 1};
    Vec3 m_refB{0, 0, 1};
    float m_restAngle = 0.0f;
};

// ---------------------------------------------------------------------------
// FixedJoint - welds two bodies into one rigid cluster. The relative position
// and rotation the joint first sees are locked, so the pair then translates and
// spins as a single body (corrections are shared by mass and inertia) until
// `breakForce` snaps it. `connectedGuid` is the target GameObject's stable GUID;
// an empty GUID anchors this body rigidly to a fixed world pose. At least one of
// the two bodies needs a RigidBody; a body without one is a static anchor.
// ---------------------------------------------------------------------------
class FixedJoint : public Component {
public:
    std::string connectedGuid;      // target GameObject GUID ("" = fixed world pose)
    float breakForce = 0.0f;        // 0 = unbreakable; otherwise disables the joint above this force

    const char* TypeName() const override { return "FixedJoint"; }
    ComponentTypeId TypeId() const override { return ComponentTypeId::FixedJoint; }

    // Engine-internal: three local point pairs (in each body's own frame) that
    // coincide at capture. Three non-collinear points lock all six degrees of
    // freedom, so one shared point-constraint solver covers the whole weld.
    bool m_captured = false;
    Vec3 m_localA[3] = {Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(0, 1, 0)};
    Vec3 m_localB[3] = {Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(0, 1, 0)};
};

// ---------------------------------------------------------------------------
// ScriptComponent - attaches a registered gameplay class to a GameObject and
// stores its inspector values.
// ---------------------------------------------------------------------------
class ScriptComponent : public Component {
public:
    std::string scriptClass;
    Json properties;                // object: field name -> value

    const char* TypeName() const override { return "Script"; }
    ComponentTypeId TypeId() const override { return ComponentTypeId::Script; }

    Component* instance = nullptr;  // engine-internal: the live script instance

    // Calls OnDestroy (when the instance is running) and frees it. Safe to call
    // more than once; the engine calls this whenever a Script component or its
    // GameObject leaves the scene so instances never leak.
    void DestroyInstance();
};

// ---------------------------------------------------------------------------
// PrefabInstance - marks the root of a subtree instantiated from a .prefab
// asset. It records the source so the instance can be re-applied or reverted.
// It is normally created automatically and should not be added by hand.
// ---------------------------------------------------------------------------
class PrefabInstance : public Component {
public:
    std::string prefabPath;  // project-relative .prefab path

    // The properties this instance has changed relative to its asset, keyed by
    // node path ("" is the instance root, "Lid/Bolt" a child) and then by
    // property key ("@name", "MeshRenderer.albedoColor", "Transform.position",
    // "Script.speed", "HingeJoint.@enabled"). Everything *not* listed here is
    // pulled from the .prefab file when the scene loads, so editing an asset
    // reaches every instance that left the property alone.
    Json overrides = Json::MakeObject();
    // False until the differences have been computed. A scene written before
    // 0.1.17 carries none, and there the instance's own state is the override
    // set (the loader diffs it first) rather than being overwritten by the
    // asset on first load.
    bool overridesKnown = false;

    const char* TypeName() const override { return "PrefabInstance"; }
    ComponentTypeId TypeId() const override { return ComponentTypeId::PrefabInstance; }
};

// ---------------------------------------------------------------------------
// GameObject.
// ---------------------------------------------------------------------------
class GameObject {
public:
    std::string name = "GameObject";
    bool active = true;
    std::string tag;                // "MainCamera" picks the play camera
    int layer = 0;                  // 0..31, used with the scene layer matrix

    EntityId id() const { return m_id; }
    // Stable identity that survives save/load (and which prefabs, object
    // references and scene merges key on). 32-hex, generated on creation.
    const std::string& guid() const { return m_guid; }
    Scene& scene() const { return *m_scene; }
    Transform& transform() const { return *m_transform; }

    bool ActiveInHierarchy() const;

    template <typename T>
    T* GetComponent() const {
        for (const auto& c : m_components) {
            constexpr ComponentTypeId want = ComponentTypeOf<T>::id;
            if (c->TypeId() == want) {
                if constexpr (want == ComponentTypeId::Script) {
                    if constexpr (std::is_same_v<T, ScriptComponent>) {
                        return static_cast<T*>(c.get());
                    } else {
                        ScriptComponent* sc = static_cast<ScriptComponent*>(c.get());
                        if (sc->instance) return dynamic_cast<T*>(sc->instance);
                    }
                } else {
                    return static_cast<T*>(c.get());
                }
            }
        }
        return nullptr;
    }

    template <typename T>
    T* AddComponent() {
        static_assert(std::is_base_of<Component, T>::value, "T must derive from Component");
        static_assert(!std::is_same_v<T, Transform>, "every GameObject already owns a Transform");
        constexpr ComponentTypeId want = ComponentTypeOf<T>::id;
        static_assert(want != ComponentTypeId::Script || std::is_same_v<T, ScriptComponent>,
                      "gameplay script classes are attached via ScriptComponent, not AddComponent<T>()");
        T* raw = new (std::nothrow) T();
        if (!raw) return nullptr;
        AttachComponent(std::unique_ptr<Component>(raw));
        return raw;
    }

    bool AddComponentByName(const std::string& typeName);   // "RigidBody", ...
    bool RemoveComponentByName(const std::string& typeName);
    Component* GetComponentByName(const std::string& typeName) const;
    const std::vector<std::unique_ptr<Component>>& GetComponents() const { return m_components; }
    std::vector<Component*> GetComponentList() const;       // raw pointers, editor-friendly

    GameObject* GetParent() const { return m_transform->m_parent; }
    void SetParent(GameObject* parent) { m_transform->SetParent(parent); }
    std::vector<GameObject*> GetChildren() const { return m_transform->m_children; }

    // Internal.
    EntityId m_id = kInvalidEntity;
    std::string m_guid;
    Scene* m_scene = nullptr;
    Transform* m_transform = nullptr;
    std::vector<std::unique_ptr<Component>> m_components;

    void AttachComponent(std::unique_ptr<Component> c);  // defined engine-side

private:
    friend class Scene;
    void SetIdAndScene(EntityId id, Scene* scene);       // defined engine-side
};

// ---------------------------------------------------------------------------
// Global gameplay helpers (resolve through the active scene).
// ---------------------------------------------------------------------------
namespace Gameplay {
GameObject* Find(const std::string& name);                       // first active match
GameObject* FindWithTag(const std::string& tag);
std::vector<GameObject*> FindAllWithTag(const std::string& tag);
GameObject* Instantiate(GameObject& original);                   // deep copy, added to scene
GameObject* CreateEmpty(const std::string& name);
void Destroy(GameObject* go);                                    // deferred to end of frame
void DestroyComponent(Component* c);
Scene* ActiveScene();
}  // namespace Gameplay

// Convenience free functions.
inline GameObject* Find(const std::string& name) { return Gameplay::Find(name); }

// ---------------------------------------------------------------------------
// Input - polled from gameplay code. State is injected by the host window.
// ---------------------------------------------------------------------------
namespace Input {
bool GetKey(KeyCode key);
bool GetKeyDown(KeyCode key);
bool GetKeyUp(KeyCode key);
float GetAxis(const std::string& axis);   // "Horizontal", "Vertical"
Vec2 GetMousePosition();                  // pixels, origin bottom-left of viewport
bool GetMouseButton(int button);          // 0 left, 1 right, 2 middle
bool GetMouseButtonDown(int button);
bool GetMouseButtonUp(int button);
}  // namespace Input

// ---------------------------------------------------------------------------
// Time.
// ---------------------------------------------------------------------------
namespace Time {
float DeltaTime();
float FixedDeltaTime();
float Time();
uint64_t FrameCount();
float TimeScale();
void SetTimeScale(float scale);
}  // namespace Time

// ---------------------------------------------------------------------------
// Audio - the host plays queued clips (WAV) through its audio device.
// ---------------------------------------------------------------------------
namespace Audio {
void Play(const std::string& clipPath, float volume = 1.0f, float pitch = 1.0f);
}

// ---------------------------------------------------------------------------
// Physics queries - cast and overlap against colliders in the active scene.
// ---------------------------------------------------------------------------
namespace Physics {
struct RaycastHit {
    EntityId entity = kInvalidEntity;  // hit GameObject's runtime id
    std::string guid;                  // hit GameObject's stable GUID
    std::string name;                  // hit GameObject's name
    Vec3 point;
    Vec3 normal;
    float distance = 0.0f;
};
// Nearest collider along the ray; false when nothing is hit within maxDistance.
bool Raycast(const Vec3& origin, const Vec3& direction, float maxDistance, RaycastHit* hit);
// Every collider along the ray, nearest first.
std::vector<RaycastHit> RaycastAll(const Vec3& origin, const Vec3& direction, float maxDistance);
// Entities whose collider overlaps the sphere.
std::vector<EntityId> OverlapSphere(const Vec3& center, float radius);
// Entities whose collider overlaps an axis-aligned box. `halfExtents` is half
// the size (Unity's OverlapBox convention).
std::vector<EntityId> OverlapBox(const Vec3& center, const Vec3& halfExtents);
// Sweep a sphere or an axis-aligned box from its start pose along `direction`
// and report the first contact: the shape's surface, not a centre line, so a
// fat cast stops on an edge a thin ray would slip past. A cast that starts
// overlapping reports distance 0.
bool SphereCast(const Vec3& origin, float radius, const Vec3& direction, float maxDistance,
                RaycastHit* hit);
bool BoxCast(const Vec3& center, const Vec3& halfExtents, const Vec3& direction, float maxDistance,
             RaycastHit* hit);
}  // namespace Physics

// ---------------------------------------------------------------------------
// Debug.
// ---------------------------------------------------------------------------
namespace Debug {
// Goes to the editor console.
void Log(const char* fmt, ...) CAIRN_PRINTF(1, 2);
void LogWarning(const char* fmt, ...) CAIRN_PRINTF(1, 2);
void LogError(const char* fmt, ...) CAIRN_PRINTF(1, 2);
// Draws a line in the viewport for `duration` seconds (editor aid).
void DrawLine(const Vec3& from, const Vec3& to, Color color = Color::White, float duration = 0.0f);
}  // namespace Debug

}  // namespace Cairn

// Query entry point resolved by dlsym() in the engine's script host. Defined
// inline so every script translation unit shares one registry per shared lib.
#if defined(_WIN32)
#define CAIRN_SCRIPT_EXPORT __declspec(dllexport)
#else
#define CAIRN_SCRIPT_EXPORT __attribute__((visibility("default"), used))
#endif
extern "C" CAIRN_SCRIPT_EXPORT inline const std::vector<::Cairn::ScriptClassRecord>* CairnQueryScriptRegistry() {
    return &::Cairn::detail::ScriptRegistry();
}
