// Cairn Engine - rigidbody physics.
//
// Shapes are gathered in world space from the entity's world matrix, integrated
// with semi-implicit Euler and resolved with impulses plus restitution. Trigger
// overlap events feed script OnTriggerEnter/OnTriggerExit; solid contacts feed
// OnCollisionEnter/Stay/Exit. 3D and 2D shapes never mix.
#include "cairn/physics.h"

#include "cairn/engine.h"
#include "cairn/log.h"
#include "scene_internal.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace Cairn {

namespace {

struct Shape {
    enum Kind { Box3, Sphere3, Box2 } kind;
    GameObject* go = nullptr;
    Component* comp = nullptr;
    RigidBody* rb = nullptr;
    bool isTrigger = false;
    bool isStatic = true;
    Vec3 min, max;      // boxes, world space
    Vec3 center;        // boxes: center; spheres: center
    float radius = 0;   // spheres

    bool Is2D() const { return kind == Box2; }
    bool IsSolid() const { return !isTrigger; }
    // Effective mass; a non-positive mass behaves as unit mass without the
    // engine ever rewriting the value the user authored.
    float InvMass() const {
        if (!rb || isStatic || !rb->enabled || rb->isKinematic) return 0.0f;
        return 1.0f / (rb->mass > 0.0f ? rb->mass : 1.0f);
    }
};

struct Contact {
    Vec3 normal;      // points from B toward A
    float penetration = 0;
};

Vec3 TransformPoint(const Mat4& m, const Vec3& p) { return m.TransformPoint(p); }

AABB WorldBox(const BoxCollider& col, const Mat4& world) {
    Vec3 half = col.size.Abs() * 0.5f;
    return AABB(col.center - half, col.center + half).Transformed(world);
}

AABB WorldBox2D(const Collider2D& col, const Mat4& world) {
    Vec3 half(std::fabs(col.size.x) * 0.5f, std::fabs(col.size.y) * 0.5f, 0.0f);
    return AABB(Vec3(col.center.x - half.x, col.center.y - half.y, 0.0f),
                Vec3(col.center.x + half.x, col.center.y + half.y, 0.0f))
        .Transformed(world);
}

float WorldMaxScale(Transform& t) {
    Vec3 s = t.WorldScale();
    return std::max({std::fabs(s.x), std::fabs(s.y), std::fabs(s.z)});
}

bool Collide(const Shape& a, const Shape& b, Contact* out) {
    if (a.Is2D() != b.Is2D()) return false;  // never mix 2D and 3D shapes

    if (a.kind != Shape::Sphere3 && b.kind != Shape::Sphere3) {
        // AABB vs AABB.
        float ox = std::min(a.max.x, b.max.x) - std::max(a.min.x, b.min.x);
        float oy = std::min(a.max.y, b.max.y) - std::max(a.min.y, b.min.y);
        float oz = a.Is2D() ? 1e9f : (std::min(a.max.z, b.max.z) - std::max(a.min.z, b.min.z));
        if (ox <= 0 || oy <= 0 || oz <= 0) return false;
        if (ox <= oy && ox <= oz) {
            out->normal = Vec3(a.center.x >= b.center.x ? 1.0f : -1.0f, 0, 0);
            out->penetration = ox;
        } else if (oy <= oz) {
            out->normal = Vec3(0, a.center.y >= b.center.y ? 1.0f : -1.0f, 0);
            out->penetration = oy;
        } else {
            out->normal = Vec3(0, 0, a.center.z >= b.center.z ? 1.0f : -1.0f);
            out->penetration = oz;
        }
        return true;
    }
    if (a.kind == Shape::Sphere3 && b.kind == Shape::Sphere3) {
        Vec3 d = a.center - b.center;
        float dist = d.Length();
        float sum = a.radius + b.radius;
        if (dist >= sum) return false;
        out->penetration = sum - dist;
        out->normal = dist > 1e-6f ? d / dist : Vec3(0, 1, 0);
        return true;
    }
    const Shape* box = a.kind != Shape::Sphere3 ? &a : &b;
    const Shape* sphere = a.kind != Shape::Sphere3 ? &b : &a;
    Vec3 closest(std::clamp(sphere->center.x, box->min.x, box->max.x),
                 std::clamp(sphere->center.y, box->min.y, box->max.y),
                 std::clamp(sphere->center.z, box->min.z, box->max.z));
    Vec3 d = sphere->center - closest;
    float dist = d.Length();
    if (dist >= sphere->radius) return false;
    if (dist > 1e-6f) {
        out->normal = d / dist;          // points from the box toward the sphere
        out->penetration = sphere->radius - dist;
    } else {
        out->normal = Vec3(0, 1, 0);
        out->penetration = sphere->radius;
    }
    if (sphere == &b) out->normal = -out->normal;  // convention: normal points toward A
    return true;
}

uint64_t PairKey(Component* a, Component* b) {
    uint64_t ka = a ? a->m_componentId : 0;
    uint64_t kb = b ? b->m_componentId : 0;
    if (ka > kb) std::swap(ka, kb);
    return (ka << 32) | kb;
}

void NotifyTrigger(const Shape& self, const Shape& other, bool entered) {
    if (!self.go) return;
    for (Component* c : self.go->GetComponentList()) {
        if (c->TypeId() != ComponentTypeId::Script || !c->enabled) continue;
        auto* sc = static_cast<ScriptComponent*>(c);
        if (sc->instance) {
            if (entered) sc->instance->OnTriggerEnter(other.comp);
            else sc->instance->OnTriggerExit(other.comp);
        }
    }
}

void NotifyCollision(Component* receiver, const Shape& self, const Shape& other, CollisionPhase phase) {
    if (!self.go) return;
    for (Component* c : self.go->GetComponentList()) {
        if (c->TypeId() != ComponentTypeId::Script || !c->enabled) continue;
        auto* sc = static_cast<ScriptComponent*>(c);
        if (!sc->instance) continue;
        switch (phase) {
            case CollisionPhase::Enter: sc->instance->OnCollisionEnter(other.comp); break;
            case CollisionPhase::Stay: sc->instance->OnCollisionStay(other.comp); break;
            case CollisionPhase::Exit: sc->instance->OnCollisionExit(other.comp); break;
        }
    }
    (void)receiver;
}

// Moves a GameObject by a world-space delta, converting to the parent's local
// space so parented rigidbodies behave like their unparented counterparts.
void MoveWorld(Transform& t, const Vec3& worldDelta) {
    GameObject* parent = t.m_parent;
    if (!parent) {
        t.position += worldDelta;
    } else {
        Mat4 inv = parent->transform().WorldMatrix().Inverted();
        Vec3 worldPos = t.WorldMatrix().GetTranslation() + worldDelta;
        t.position = inv.TransformPoint(worldPos);
    }
    t.MarkWorldDirty();
}

// Gather every enabled collider in world space. Called once per step, after
// integration, so shape data can never drift out of sync with the objects.
void GatherShapes(Scene& scene, std::vector<Shape>* out) {
    for (const auto& go : scene.objects) {
        if (!go->ActiveInHierarchy()) continue;
        Transform& t = go->transform();
        Mat4 world = t.WorldMatrix();
        RigidBody* rb = go->GetComponent<RigidBody>();
        bool dynamic = rb && rb->enabled && !rb->isKinematic;
        auto add = [&](Shape s) {
            s.go = go.get();
            s.isStatic = !dynamic;
            s.rb = dynamic ? rb : nullptr;
            out->push_back(s);
        };
        if (auto* c = go->GetComponent<BoxCollider>(); c && c->enabled) {
            AABB box = WorldBox(*c, world);
            Shape s;
            s.kind = Shape::Box3;
            s.comp = c;
            s.isTrigger = c->isTrigger;
            s.min = box.min; s.max = box.max; s.center = box.Center();
            add(s);
        }
        if (auto* c = go->GetComponent<SphereCollider>(); c && c->enabled) {
            Shape s;
            s.kind = Shape::Sphere3;
            s.comp = c;
            s.isTrigger = c->isTrigger;
            s.center = TransformPoint(world, c->center);
            s.radius = c->radius * WorldMaxScale(t);
            add(s);
        }
        if (auto* c = go->GetComponent<Collider2D>(); c && c->enabled) {
            AABB box = WorldBox2D(*c, world);
            Shape s;
            s.kind = Shape::Box2;
            s.comp = c;
            s.isTrigger = c->isTrigger;
            s.min = box.min; s.max = box.max; s.center = box.Center();
            add(s);
        }
    }
}

bool HasEnabledCollider(const GameObject& go) {
    for (Component* c : go.GetComponentList()) {
        switch (c->TypeId()) {
            case ComponentTypeId::BoxCollider:
            case ComponentTypeId::SphereCollider:
            case ComponentTypeId::Collider2D:
                if (c->enabled) return true;
                break;
            default:
                break;
        }
    }
    return false;
}

void ResolveContact(Shape& a, Shape& b, const Contact& ct) {
    float invMassA = a.InvMass();
    float invMassB = b.InvMass();
    float invSum = invMassA + invMassB;
    if (invSum <= 0) return;

    if (a.go) MoveWorld(a.go->transform(), ct.normal * (ct.penetration / invSum) * invMassA);
    if (b.go) MoveWorld(b.go->transform(), -ct.normal * (ct.penetration / invSum) * invMassB);

    Vec3 relVel;
    if (invMassA > 0 && a.rb) relVel += a.rb->velocity;
    if (invMassB > 0 && b.rb) relVel -= b.rb->velocity;
    float vn = Vec3::Dot(relVel, ct.normal);
    if (vn < 0) {
        float restitution = std::max(a.rb ? a.rb->restitution : 0.0f, b.rb ? b.rb->restitution : 0.0f);
        float j = -(1.0f + restitution) * vn / invSum;
        Vec3 impulse = ct.normal * j;
        if (invMassA > 0 && a.rb) a.rb->velocity += impulse * invMassA;
        if (invMassB > 0 && b.rb) b.rb->velocity -= impulse * invMassB;
    }
}

}  // namespace

void PhysicsReset(Scene* scene) {
    if (scene) scene->physicsPairs.clear();
}

void PhysicsStep(Scene& scene, float dt) {
    if (dt <= 0) return;

    // Integrate dynamic rigidbodies (semi-implicit Euler, world space).
    for (const auto& go : scene.objects) {
        if (!go->ActiveInHierarchy()) continue;
        RigidBody* rb = go->GetComponent<RigidBody>();
        if (!rb || !rb->enabled || rb->isKinematic) continue;
        if (!HasEnabledCollider(*go)) continue;
        float mass = rb->mass > 0.0f ? rb->mass : 1.0f;
        if (rb->useGravity) rb->velocity += scene.gravity * rb->gravityScale * dt;
        rb->velocity += (rb->m_accumForce / mass) * dt;
        rb->m_accumForce = Vec3();
        if (rb->drag > 0) rb->velocity *= std::max(0.0f, 1.0f - rb->drag * dt);
        if (rb->freezeZ) rb->velocity.z = 0.0f;
        MoveWorld(go->transform(), rb->velocity * dt);
    }

    // Shapes are gathered once, after integration.
    std::vector<Shape> shapes;
    GatherShapes(scene, &shapes);

    // Narrow phase: contacts and trigger overlaps in one sweep.
    std::unordered_map<uint64_t, PairRecord> seenPairs;
    for (size_t a = 0; a < shapes.size(); ++a) {
        for (size_t b = a + 1; b < shapes.size(); ++b) {
            Shape& A = shapes[a];
            Shape& B = shapes[b];
            if (!A.comp || !B.comp) continue;
            // Only two solid colliders push each other; a trigger reports
            // events but never turns the pair into a non-solid contact.
            bool solidPair = A.IsSolid() && B.IsSolid();
            if (!solidPair && A.isStatic && B.isStatic && A.Is2D() == B.Is2D()) continue;

            Contact ct;
            if (!Collide(A, B, &ct)) continue;

            uint64_t key = PairKey(A.comp, B.comp);
            if (solidPair) {
                bool first = seenPairs.find(key) == seenPairs.end();
                if (first) {
                    NotifyCollision(nullptr, A, B, CollisionPhase::Enter);
                    NotifyCollision(nullptr, B, A, CollisionPhase::Enter);
                } else {
                    NotifyCollision(nullptr, A, B, CollisionPhase::Stay);
                    NotifyCollision(nullptr, B, A, CollisionPhase::Stay);
                }
                seenPairs[key] = PairRecord{A.comp, B.comp, false};
                if (!(A.isStatic && B.isStatic)) ResolveContact(A, B, ct);
            } else {
                bool was = scene.physicsPairs.count(key) > 0;
                seenPairs[key] = PairRecord{A.comp, B.comp, true};
                if (!was) {
                    NotifyTrigger(A, B, true);
                    NotifyTrigger(B, A, true);
                }
            }
        }
    }

    // Exits: solid contacts that ended this step, plus pairs whose collider or
    // entity disappeared (stored pointers keep the exit event deliverable).
    for (auto it = scene.physicsPairs.begin(); it != scene.physicsPairs.end();) {
        uint64_t key = it->first;
        auto found = seenPairs.find(key);
        if (found == seenPairs.end()) {
            Shape a, b;
            a.comp = it->second.a;
            b.comp = it->second.b;
            a.go = a.comp ? a.comp->m_go : nullptr;
            b.go = b.comp ? b.comp->m_go : nullptr;
            if (it->second.isTrigger) {
                NotifyTrigger(a, b, false);
                NotifyTrigger(b, a, false);
            } else {
                NotifyCollision(nullptr, a, b, CollisionPhase::Exit);
                NotifyCollision(nullptr, b, a, CollisionPhase::Exit);
            }
            it = scene.physicsPairs.erase(it);
        } else {
            it->second.a = found->second.a;
            it->second.b = found->second.b;
            ++it;
        }
    }
    scene.physicsPairs.swap(seenPairs);
}

}  // namespace Cairn
