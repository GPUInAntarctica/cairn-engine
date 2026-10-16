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

}  // namespace Cairn
