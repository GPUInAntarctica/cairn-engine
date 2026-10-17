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
```

Colliders: `BoxCollider` (3D AABB), `SphereCollider`, `CapsuleCollider` (swept sphere along a Y/X/Z axis), `Collider2D` (XY plane). A collider without a `RigidBody` is static. `isTrigger` colliders fire the trigger callbacks instead of resolving.

For player/agent movement use a `CharacterController`, a kinematic capsule that is moved by code rather than the solver:

```cpp
auto* cc = gameObject().GetComponent<CharacterController>();
cc->Move(Vec3(0.1f, 0, 0));   // sweep + resolve, climbs steps up to stepOffset
cc->isGrounded;               // true after a move that settled on ground
cc->SimpleMove(Vec3(x, 0, z));// per-second velocity + gravity (ignores input y)
```

Joints: `DistanceJoint` keeps two bodies a fixed distance apart (or a spring/damper with `spring`/`damping`). Set `connectedGuid` to the target GameObject's GUID, or leave it empty to anchor to a world point; `breakForce` snaps the joint.

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
