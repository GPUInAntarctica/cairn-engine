// Cairn Engine - physics step (internal to the engine runtime; gameplay code
// interacts through RigidBody and the trigger callbacks on Component).
#pragma once

#include "cairn/scripting.h"

#include <vector>

namespace Cairn {

class Scene;

void PhysicsStep(Scene& scene, float dt);
// Drops cached trigger/contact pairs for a scene (called on scene swap/reset).
void PhysicsReset(Scene* scene = nullptr);

// Physics queries. A hit carries the GameObject and the world point/normal.
struct PhysicsHit {
    GameObject* go = nullptr;
    Vec3 point;
    Vec3 normal;
    float distance = 0.0f;
};
bool PhysicsRaycast(Scene& scene, const Vec3& origin, const Vec3& direction, float maxDistance,
                    PhysicsHit* hit);
std::vector<PhysicsHit> PhysicsRaycastAll(Scene& scene, const Vec3& origin, const Vec3& direction,
                                          float maxDistance);
std::vector<GameObject*> PhysicsOverlapSphere(Scene& scene, const Vec3& center, float radius);
// Axis-aligned box overlap. `halfExtents` is half the box size (Unity's
// OverlapBox convention); a negative component is treated as 0.
std::vector<GameObject*> PhysicsOverlapBox(Scene& scene, const Vec3& center,
                                           const Vec3& halfExtents);
// Shape casts: sweep a sphere or a box (axis-aligned, `halfExtents` as above)
// from its start pose along `direction` and report the first contact. A cast
// that starts overlapping reports distance 0. `maxDistance <= 0` means "far"
// (1e6 units, a finite bound rather than an infinite one so the sweep stays
// numerically well behaved).
bool PhysicsSphereCast(Scene& scene, const Vec3& origin, float radius, const Vec3& direction,
                       float maxDistance, PhysicsHit* hit);
bool PhysicsBoxCast(Scene& scene, const Vec3& center, const Vec3& halfExtents,
                    const Vec3& direction, float maxDistance, PhysicsHit* hit);

// Moves a CharacterController capsule by `displacement`, resolving it against
// solid colliders. Returns true when the controller is grounded afterwards.
bool PhysicsCharacterMove(Scene& scene, GameObject& go, CharacterController& cc,
                          const Vec3& displacement);

}  // namespace Cairn
