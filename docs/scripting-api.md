# Scripting API Reference

Gameplay code lives in `Assets/Scripts/*.cpp`, subclasses `Cairn::Component`, and registers itself with `CAIRN_SCRIPT(ClassName)`. Scripts are compiled by the editor (**Ctrl+B**) into a shared library that links `libcairn_engine`.

```cpp
#include <cairn/cairn.h>
using namespace Cairn;

class PlayerController : public Component {
public:
    void OnStart() override {}
    void OnUpdate(float deltaTime) override {}
    void FixedUpdate(float fixedDeltaTime) override {}
    void OnTriggerEnter(Component* other) override {}
    void OnTriggerExit(Component* other) override {}
    void OnDestroy() override {}

    void DescribeProps(ScriptMeta& meta) override {
        meta.Add("speed", PropType::Float, 6.0f);
    }
};

CAIRN_SCRIPT(PlayerController)
```

## Lifecycle

| Callback | When |
| --- | --- |
| `OnStart()` | Once, before the first update, after all instances exist |
| `FixedUpdate(float dt)` | Physics rate (1/60 s), before the physics step |
| `OnUpdate(float dt)` | Every rendered frame with the scaled delta time |
| `OnTriggerEnter/Exit(Component* other)` | A trigger overlap begins/ends; `other` is the collider component |
| `OnDestroy()` | Before the instance is destroyed (scene close, stop, reload) |

## Component

- `GameObject& gameObject()` / `Transform& transform()`
- `PropFloat/Int/Bool/String/Vec2/Vec3/Color(name)` — read inspector fields declared in `DescribeProps`
- `PropVec4(name)` → `Vec4`; `PropEnum(name)` → `int` index; `PropAsset(name)` → project-relative path; `PropObject(name)` → target GameObject's stable GUID
- `bool enabled` — disabled components skip lifecycle callbacks

## GameObject

- `transform()` — the transform component
- `GetComponent<T>()` — engine components (`RigidBody`, `BoxCollider`, …) by type; gameplay scripts by class via `dynamic_cast`
- `AddComponent<T>()` — engine components only (scripts attach via `ScriptComponent`)
- `name`, `active`, `tag` — `"MainCamera"` marks the play camera
- `id()` — runtime handle for the current session; `guid()` — stable 32-hex identity that survives save/load (use it for references that must outlive a reload)

## Transform

- `position` (Vec3), `rotation` (Quat), `scale` (Vec3) — local space
- `GetEulerDegrees()` / `SetEulerDegrees(Vec3)` — degrees, ZXY order (Unity-compatible)
- `WorldPosition()`, `WorldRotation()`, `Forward()`, `Right()`, `Up()`
- `SetParent(GameObject*)` — hierarchy; children follow

## Input

```cpp
Input::GetKey(KeyCode::W);          // held
Input::GetKeyDown(KeyCode::Space);  // pressed this frame
Input::GetKeyUp(KeyCode::Escape);   // released this frame
Input::GetAxis("Horizontal");       // -1..1 (arrows/WASD, configurable)
Input::GetMouseButton(0);           // 0 left, 1 right, 2 middle
Input::GetMousePosition();          // pixels, origin bottom-left
```

## Time

`Time::DeltaTime()` — last frame delta · `Time::Time()` — seconds since play ·
`Time::FixedDeltaTime()` — 1/60 · `Time::SetTimeScale(0.5f)` — slow motion.

## Physics

```cpp
auto* rb = gameObject().GetComponent<RigidBody>();
rb->velocity;             // read/write
rb->friction = 0.8f;      // tangential damping on contact (0..1)
rb->angularVelocity = Vec3(0, 3.14f, 0);  // radians/second
rb->AddForce(Vec3(0, 10, 0));   // accumulated, applied next step
rb->AddImpulse(Vec3(0, 5, 0));  // immediate velocity change
rb->AddTorque(Vec3(0, 1, 0));   // accumulated angular acceleration
rb->collisionDetection = RigidBody::CollisionDetection::Continuous;  // sweep fast movers
```

Colliders: `BoxCollider` (3D AABB), `SphereCollider`, `CapsuleCollider` (swept sphere along a Y/X/Z axis), `Collider2D` (XY plane), `MeshCollider` (below). A collider without a `RigidBody` is static. `isTrigger` colliders fire the trigger callbacks instead of resolving.

`MeshCollider` takes its geometry from a triangle mesh for level shapes the primitives cannot express - a ramp, a room, a corridor. `meshPath` is a project-relative `.obj`, resolved exactly like `MeshRenderer::meshPath`:

```cpp
auto* level = go->AddComponent<MeshCollider>();
level->meshPath = "Assets/Meshes/level.obj";   // project-relative
level->isTrigger = false;
```

It is a triangle soup (non-convex), so like Unity's non-convex MeshCollider it is static/kinematic-only: a mesh on a non-kinematic `RigidBody` is dropped and reported once in the engine log. Sphere, capsule and box colliders collide with it (the box contact is an exact 13-axis separating-axis test), `Physics::Raycast` hits the actual triangles rather than the bounding box, `Physics::OverlapSphere` is exact, `Continuous` CCD sweeps it, and a `CharacterController` stands on it. Two mesh colliders do not interact. The narrow phase walks the triangles with a distance prune and no BVH, so keep a mesh collider to level geometry rather than a dense model.

Collision detection: `Discrete` (default) moves the body by `velocity * dt` and then tests for overlap, so a body whose single-step displacement exceeds an obstacle's thickness passes through it. `Continuous` sweeps the body against solid static/kinematic colliders before moving it (translation only) and stops it at the first contact. Use it for bullets, launched projectiles and long falls; leave it off for bodies that never move fast, since it costs a few extra shape tests per step.

For player/agent movement use a `CharacterController`, a kinematic capsule that is moved by code rather than the solver:

```cpp
auto* cc = gameObject().GetComponent<CharacterController>();
cc->Move(Vec3(0.1f, 0, 0));   // sweep + resolve, climbs steps up to stepOffset
cc->isGrounded;               // true after a move that settled on ground
cc->SimpleMove(Vec3(x, 0, z));// per-second velocity + gravity (ignores input y)
```

Joints: `DistanceJoint` keeps two bodies a fixed distance apart (or a spring/damper with `spring`/`damping`). Set `connectedGuid` to the target GameObject's GUID, or leave it empty to anchor to a world point; `breakForce` snaps the joint.

`HingeJoint` pins one body to another (or to the world) and leaves rotation about `axis` free: `anchor`/`connectedAnchor` are the pinned points in each body's local space, `axis` is the hinge line in this body's local space. Optional `useLimits` + `limitsMin`/`limitsMax` (degrees) stop the swing, and `useMotor` + `motorVelocity` (degrees/second) + `motorForce` drive it - a force of 0 is a disabled motor. The angle is measured from the pose the joint captures on its first step, from the connected body's reference to this body's, so with a `connectedGuid` a positive speed turns *the connected body* and anchored to the world it turns *this* body.

`FixedJoint` welds two bodies into one rigid cluster by locking the relative pose it captures on its first step. It has no anchor - place the bodies where you want them, then add the joint; afterwards moving one drags the other. Both joints break on `breakForce` (0 = unbreakable), the same field and formula as `DistanceJoint`.

```cpp
auto* hinge = gameObject().AddComponent<HingeJoint>();
hinge->anchor = Vec3(-1, 0, 0);          // this body's edge
hinge->connectedAnchor = Vec3(0, 0, 0);  // empty connectedGuid -> world point
hinge->axis = Vec3(0, 1, 0);             // free spin line
hinge->useLimits = true;
hinge->limitsMin = -90.0f;
hinge->limitsMax = 0.0f;                 // a door that only opens one way
```

Two details make a joint feel solid rather than springy: the pinned points are held by an exact effective-mass solve each step (not a per-axis approximation), and the motor/limit turn the body *about the pin* rather than about its centre of mass, so the pin never visibly slips. Joined rigidbodies should not sit under a rotated parent - like the integrator, the solver reads `Transform::rotation` as a world rotation.

Sleeping: rigidbodies that stay slow for `sleepDelay` seconds stop integrating until disturbed (`AddForce`/`AddImpulse`/`AddTorque`, a written `velocity`, or a moving body touching them). Tune with `allowSleep`, `sleepThreshold`, `sleepDelay`.

Profiling: wrap hot code with a named sample and read the rolling window from the CLI (`engine.profile`).

```cpp
{
    Profiler::ScopedSample s("pathfinding");   // or Profiler::BeginSample / EndSample
    RebuildNavMesh();
}
```

Parallelism: `Jobs::ParallelFor(count, f)` runs `f(0..count-1)` across the shared worker pool (the calling thread participates). `f(i)` must not depend on other indices or write memory another index owns — write a per-index slot and merge in index order for a deterministic result. Do not call `ParallelFor` from inside a body.

```cpp
std::vector<float> cost(items.size(), 0.0f);
Jobs::ParallelFor((int)items.size(), [&](int i) { cost[i] = ScoreItem(items[i]); });
// cost is fully populated, in index order, regardless of worker count.
```

Queries run against colliders in the active scene:

```cpp
Physics::RaycastHit hit;
if (Physics::Raycast(origin, direction, 100.0f, &hit)) {
    // hit.entity, hit.guid, hit.name, hit.point, hit.normal, hit.distance
}
auto all = Physics::RaycastAll(origin, direction, 100.0f);  // nearest first
auto nearby = Physics::OverlapSphere(center, 2.0f);          // [EntityId]
```

Collision layers: `gameObject().layer` (0..31) plus the scene's layer matrix decide which pairs interact. Toggle a pair with the editor/CLI (`scene.layerMatrix.set`) or `Scene::SetLayerCollision`; all layers collide by default.

## Gameplay helpers

- `Gameplay::Find("Name")`, `Gameplay::FindWithTag("MainCamera")`, `Gameplay::FindAllWithTag(tag)`
- `Gameplay::Instantiate(*go)` — deep copy into the scene
- `Gameplay::Destroy(go)` — deferred to end of frame
- `Gameplay::ActiveScene()` — for custom queries

## Debug

```cpp
Debug::Log("score: %d", score);          // editor console
Debug::LogWarning(...); Debug::LogError(...);
Debug::DrawLine(from, to, color);        // one-frame viewport line
```

## Audio

`Audio::Play("Assets/Audio/jump.wav")` — queues a WAV clip; the host decodes and plays it (volume/pitch optional).

## Math

`Vec2/Vec3/Vec4` (`Dot`, `Cross`, `Normalized`, `Lerp`, `Min/Max`), `Quat` (`FromAxisAngle`, `FromEulerDegrees`, `Slerp`, `LookRotation`), `Mat4` (`TRS`, `Perspective`, `Ortho`, `LookAt`, `Inverted`, `TransformPoint/Direction`), `Color` (`FromHex`, `FromHSV`, `Lerp`), `AABB` (`RayIntersect`, `Transformed`).

## Inspector fields

`DescribeProps` declares what the inspector shows; values are stored on the component and survive reloads:

```cpp
void DescribeProps(ScriptMeta& meta) override {
    meta.Add("speed",   PropType::Float,  6.0f);
    meta.Add("health",  PropType::Int,    100);
    meta.Add("invincible", PropType::Bool, false);
    meta.Add("nickname", PropType::String, "player");
    meta.Add("offset",  PropType::Vec2,   Json::MakeArray({0.f, 1.f}));
    meta.Add("target",  PropType::Vec3,   Json::MakeArray({0.f, 0.f, 0.f}));
    meta.Add("bounds",  PropType::Vec4,   Json::MakeArray({0.f, 0.f, 1.f, 1.f}));
    meta.Add("tint",    PropType::Color,  Json::MakeArray({1.f, 1.f, 1.f, 1.f}));
    meta.Add("mode",    PropType::Enum,   0);   // integer index; label/options from the editor
    meta.Add("clip",    PropType::Asset,  "");  // project asset reference
    meta.Add("target",  PropType::Object, "");  // GameObject reference, stored as its GUID
}
```
