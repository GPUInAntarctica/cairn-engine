// Cairn Engine - physics step (internal to the engine runtime; gameplay code
// interacts through RigidBody and the trigger callbacks on Component).
#pragma once

#include "cairn/scripting.h"

namespace Cairn {

class Scene;

void PhysicsStep(Scene& scene, float dt);
void PhysicsReset();

}  // namespace Cairn
