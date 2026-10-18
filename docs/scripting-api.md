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
auto inBox = Physics::OverlapBox(center, Vec3(1.0f));        // half extents
```

Shape casts sweep a volume instead of a line, so a fat cast stops on an edge a ray
would slip past (the surface decides, not the centre line). `point` is the contact
point on the swept shape's own surface, and a cast that starts overlapping reports
distance 0:

```cpp
if (Physics::SphereCast(origin, 0.4f, direction, 100.0f, &hit)) { /* ... */ }
if (Physics::BoxCast(center, Vec3(0.5f), direction, 100.0f, &hit)) { /* ... */ }
```

Queries are layer-independent (the layer matrix is a property of the step, not of a
query) and report trigger colliders as well as solid ones.

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

## 2D sprites and text

A 2D game is an orthographic `Camera` plus `SpriteRenderer` / `TextRenderer` components — there is no separate 2D scene type, and the same scene can hold 3D meshes and 2D sprites.

```cpp
GameObject* cam = scene.CreateEntity("Main");
cam->tag = "MainCamera";                    // the play-mode camera
auto* cc = cam->AddComponent<CameraComponent>();
cc->orthographic = true;
cc->orthographicSize = 5.0f;                // half height of the view, in world units
cc->clearColor = Color::FromHex(0x101018);
cc->skyEnabled = false;                     // flat background instead of the gradient

GameObject* hero = scene.CreateEntity("Hero");
auto* sr = hero->AddComponent<SpriteRenderer>();
sr->texturePath = "Assets/Sprites/hero.png"; // project-relative png/jpg/bmp
sr->pixelsPerUnit = 16.0f;                   // a 32-texel sprite is 2 world units tall
sr->pixelPerfect = true;                     // nearest sampling for pixel art
sr->sortingLayer = 1;                        // coarse group, drawn low to high
sr->orderInLayer = -5;                       // fine order within the layer
```

- **Size.** A sprite's world size is `texturePixels / pixelsPerUnit` (with the atlas rect, `frameTexels / pixelsPerUnit`); place the camera back (for example `z = 10`, looking down `-Z`) so the sprites are in front of it. A `pixelsPerUnit` below one is honoured — a 32-texel texture at `0.5` is 64 units across, which is how a background layer is set up — and only zero or less falls back to one. The editor viewport and the game camera cull a sprite against exactly that quad.
- **`uvRect`** picks a sub-rectangle of the texture: `{x, y, w, h}` as fractions of the texture, where `(x, y)` is the rectangle's top-left corner. The default `{0, 0, 1, 1}` is the whole texture, and frame `(col, row)` of a sheet that is `cols` by `rows` frames wide is `{col / cols, row / rows, 1 / cols, 1 / rows}` — rows count from the top, which is the order `image.stamp` lays frames out in. The drawn size follows the frame (`frameTexels / pixelsPerUnit`), so one texture can hold a whole animation:

```cpp
sr->uvRect = Vec4(0.25f, 0.0f, 0.25f, 0.5f);  // column 1, row 0 of a 4x2 sheet
```

- **`pivot`** is the point of the sprite that lands on the entity's position, in the same image coordinates as `uvRect`: `(0, 0)` is the frame's top-left corner, `(1, 1)` its bottom-right. The default `(0.5, 0.5)` centres the sprite (what every sprite did before 0.1.25); `(0.5, 1)` puts the frame's bottom-centre — its feet — on the position. `flipX` / `flipY` mirror the geometry about this point.
- **`pixelPerfect`** disables anti-aliasing in the software reference and selects a nearest-neighbour, mip-less sampler on the GPU, which is what keeps AI-drawn pixel art crisp. Both backends are held to the same picture (0.1.22).
- **Sorting** is `sortingLayer` first, then `orderInLayer`, then distance along the view direction; text shares the same ordering.
- **`normalMapPath`** adds tangent-space 2D lighting. The map is authored in the same y-down image space as the albedo: a green channel below 0.5 tilts the surface towards the top of the image (which is the world's up), which is exactly what `image.normalMap` writes (0.1.23).
- `TextRenderer` takes `text`, `fontSize` (glyph height in the same world units), `color` and `alignment` (`Left` / `Center` / `Right`).

### Animating a sheet

A **`SpriteAnimator`** on the same entity as the sprite turns the sheet into an animation: `columns` × `rows` cuts the sprite's own `uvRect` into equal cells, and play time steps `frame` — the cell being drawn, counted row-major from the region's top-left, the order `image.stamp` lays frames out in.

```cpp
auto* anim = hero->AddComponent<SpriteAnimator>();
anim->columns = 4;                          // a 4x1 strip of 16-texel frames
anim->rows = 1;
anim->firstFrame = 0;                       // inclusive; -1 means "the last cell"
anim->lastFrame = 3;
anim->fps = 10.0f;
anim->loop = true;
anim->durations = Json::MakeArray({0.5, 0.1, 0.1, 0.1});   // per-cell seconds
anim->events = Json::ParseOr(
    R"([{"at": 1, "set": {"hit": true}}])", Json::MakeArray());   // cell 1 writes `hit`
anim->texturePath = "Assets/Textures/hero.png";   // the animator's default sheet
```

- **Clipping.** `firstFrame` / `lastFrame` select the cells that play, so a 4×3 character sheet animates its second row with `firstFrame = 4, lastFrame = 7` and a single cell is a still. Cells are cut out of the sprite's `uvRect` rather than the whole texture, so pointing the sprite at one row (`uvRect = {0, 1/3, 1, 1/3}`) with `rows = 1` says the same thing — both compose with 0.1.25. One definition decides the rect, and the cull and the vertex generation read the same cell (0.1.27).
- **A cell can have its own time.** `durations` is a time per cell of the clip, counted from its own first cell (`[0.5, 0.1, 0.1, 0.1]` is a slow wind-up and three quick cells), so one animation can change pace without being split in two. A cell the table leaves out - or gives without a positive number - is shown for the clip's own `1 / fps`, so an empty table is exactly the animation `fps` alone describes. Everything that measures the clip reads those times: the stepping and the exit time of a transition both treat the clip as the sum of its cells.
- **Time, not frames.** The frame follows accumulated play time, not the host frame count: the same sequence of `Update(dt)` calls always lands on the same frame, and a stall longer than a whole cycle costs a cycle, not a spin. At 60 fps one `runtime.step` is exactly one frame, which is how a batch script walks a clip.
- **Scrubbing.** `frame` is an inspector field, so setting it draws that cell — a batch can render any frame without simulating time. `playing = false` freezes the animation, `loop = false` holds the last cell instead of wrapping, a frame outside the clip is drawn as the nearest end of it, and `anim->Restart()` returns to `firstFrame` at zero frame time.

### Naming the animations

A character is several animations on one sheet, so `clips` is a table of them and `clip` names the one playing. Each entry carries a `name` and any of `columns`, `rows`, `texturePath`, `firstFrame`, `lastFrame`, `fps`, `loop`, `durations`, `events`; a key an entry leaves out comes from the matching field on the component, and `clip` naming no entry means those fields are the whole animation — so an animator with no table behaves exactly as the fields say.

```json
"clips": [
  { "name": "idle",   "firstFrame": 0, "lastFrame": 0, "fps": 4 },
  { "name": "walk",   "firstFrame": 1, "lastFrame": 4, "fps": 10 },
  { "name": "attack", "firstFrame": 5, "lastFrame": 7, "fps": 12, "loop": false }
]
```

```cpp
anim->clip = "attack";                       // a script, the CLI or the inspector
// the same thing from the CLI, in one call:
// cairn component.setProps --id 5 --type SpriteAnimator --props '{"clip":"attack"}'
```

- **A clip is an override.** `attack` above inherits the component's `columns`, `rows` and `loop` is given, so a whole character shares one grid; the entry that needs a different one says so (`{"name": "bars", "columns": 2, "rows": 1}`).
- **Switching draws the new clip's first cell immediately.** `DrawnCell` is the one answer to "which cell is on screen", and it is the new clip's first cell until the next tick has restarted the clip — so `component.setProps {"clip": "attack"}` followed by a screenshot is that clip's first frame, with no half-elapsed time carried over from the clip before it.
- **A clip's own `fps` and `loop` win** over the component's, which is what makes `attack` play at 12 fps and hold its last cell while `walk` wraps at 10.
- **The table is a `Json` property** (`EditorPropKind::Json`), so it travels through the scene file, `component.add` / `component.setProps` / `component.get` and the inspector's JSON field unchanged. `ClipCount()`, `ClipName(i)`, `FindClip(name)` and `ActiveClip()` read a resolved clip from a script; entries that are not objects, or have no name, are skipped rather than mistaken for clips.
- **A clip can name its own texture.** `texturePath` on an entry points that animation at a sheet of its own — an attack effect, a different costume, a whole other character — while the entity keeps one `SpriteRenderer`. The sheet it names brings its own texel size with it (the cell is still cut out of the sprite's `uvRect`, but the texels-per-unit comes from the named texture), and the key follows the usual rule: the entry's own value wins, an entry that leaves it out uses the component's `texturePath`, and naming `""` goes back to the sprite's own texture. The cull and the draw resolve the sheet through one function, so they cannot disagree about which texture is on screen.
- **A clip's `durations` and `events` win** over the component's, like every other key: an entry that names its own table plays at those times and fires its own events, while one that names `[]` clears the component's. The table becomes a state machine once the animator is given `params` and `transitions`, below, and `events` is what lets a clip talk back to it.

### Driving the animations

`params` are the values the animator reads and `transitions` says which clip to enter for which of them, so "walk while moving, attack when it is pressed, back to idle when the attack ends" is written on the component instead of in a script — and it replays identically from a scene file, a batch or a test.

```json
"params": { "moving": true, "speed": 2.5, "attack": false },
"transitions": [
  { "from": "idle", "to": "walk", "when": { "param": "moving", "op": "is" } },
  { "from": "walk", "to": "idle", "when": [
      { "param": "moving", "op": "is", "value": false },
      { "param": "speed",  "op": "lt", "value": 0.5 } ] },
  { "from": "*", "to": "attack", "when": { "param": "attack", "op": "trigger" } },
  { "from": "attack", "to": "idle", "waitForExit": true }
]
```

```cpp
anim->params.set("moving", Json(true));      // a script, the inspector, or:
// cairn component.setProps --id 5 --type SpriteAnimator --props \
//   '{"params":{"moving":true,"speed":2.5,"attack":false}}'
```

- **A condition is `{"param", "op", "value"}`.** `is` and `not` compare the value as a bool — a bare `"op": "is"` means "is true", so `value` is only needed for `false` — `gt` and `lt` compare it as a number, and `trigger` is a one-shot bool. `when` is either one condition or a list of them that all have to hold. A parameter the animator does not have holds nothing, and an `op` that does not exist never matches, so a half-edited table stays inert instead of firing.
- **A trigger is spent by the transition it fires.** Every `trigger` the entry watched is cleared when it runs, so one press moves the machine once and `params.attack` reads `false` afterwards — which is also how a batch confirms the press was consumed.
- **Table order is priority.** The first entry whose `from` is the state being played (or `"*"`, the default, so a `from`-less entry fires from anywhere) and whose conditions hold is the one that runs.
- **`waitForExit` waits for the clip.** The entry only fires once the state's clip has played for a whole length — its cells over its own `fps` — which is what sends `attack` back to `idle` when it finishes. Without it the entry fires as soon as its conditions hold; a clip with no frame rate has no length, so such an entry never fires.
- **A transition to the state already playing replays it** from its first cell, which is how a trigger restarts a one-shot clip.
- **Each tick advances the frame and then runs the table**, so an exit time sees the clip that just finished, and the state that tick enters is the cell the frame shows — its first. Writing `clip` by hand is still a manual override, and a state entered by the machine is drawn before any of its own ticks have run.
- **`params` is replaced wholesale** by `component.setProps`, like the clip table: send the whole bag, not one key.

### Telling the outside when a frame arrives

An animation usually has to say something part-way through: the wind-up has ended and a hitbox may open, the strike has landed, the clip is back to idle. Counting frames in a script to guess that is exactly the thing the machine was built to avoid, so a clip carries `events` — `[{"at", "set"}]` — and writes a parameter when a named cell is entered.

```json
{ "name": "attack", "firstFrame": 4, "lastFrame": 7, "fps": 12, "loop": false,
  "events": [ {"at": 1, "set": {"attackWindow": true}},
              {"at": 3, "set": {"attackWindow": false, "landed": true}} ] }
```

- **`at` counts the clip's own cells**, from its first, so `{"at": 1}` is the second cell of that clip wherever its range sits; `-1` is its last cell, and anything out of range is clamped the way a frame is. An event with no `at`, no `set`, or an `at` that is not a number is skipped rather than guessed at.
- **`set` is a bag of parameters to write**, and the parameters are the same ones `transitions` reads — so the wind-up above can hold a machine in `attack` for exactly as long as the window is open. Values are written as they are given: truth, a number, even a string.
- **Entering a cell is the only thing that fires an event.** The clip's first cell fires when the clip is entered (starting play, writing `clip`, or a transition entering the state) and every following cell fires as the frame steps into it; a looping clip fires them again on every pass. Nothing watches a clock, so the same ticks write the same values in the same order.
- **One tick can fire several events**, in table order, when a long `dt` passes through several cells at once; the last write of a tick is the one left in the parameter.
- **`Restart()` fires the first cell's events**, which is also why a transition to the state already playing re-announces the clip's start.
- **A trigger is spent by the transition it fires**, and the entered state's events run before that happens: an event that names the trigger which just fired is spent along with it rather than re-arming the machine on the next tick.
- **Reading one back is reading a parameter**: `anim->Param("landed")` from a script, `component.get` from the CLI. There is no separate event queue to drain, and a fired event leaves no trace beyond the values it wrote.

> **Orientation (0.1.23).** Image space is y-down everywhere in the engine: `image.*` draws `y = 0` at the top, and a texture's row 0 is `v = 0`. The 2D quad maps the image's top row to the **top** of the sprite, so what you draw is what you see — `flipY` is a plain "turn the picture upside down". Until 0.1.23 both the sprite and the glyph quad handed their top corners `v = 1`, which mirrored every sprite and every word (including the bundled `project-2d` template's title text).

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

A script's properties stop there. Engine components have one kind a script cannot declare — `EditorPropKind::Json`, a structured value the engine stores and passes through without interpreting it (the animator's `clips`, `params` and `transitions` are three) — so it appears in `component.schema` for an engine component while `PropType` has no matching value.
