// Cairn Engine - physics step (internal to the engine runtime; gameplay code
// interacts through RigidBody and the trigger callbacks on Component).
#pragma once

#include "cairn/scripting.h"

namespace Cairn {

class Scene;

void PhysicsStep(Scene& scene, float dt);
// Drops cached trigger/contact pairs for a scene (called on scene swap/reset).
void PhysicsReset(Scene* scene = nullptr);

}  // namespace Cairn
