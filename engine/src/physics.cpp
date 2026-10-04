// Cairn Engine - lightweight rigidbody physics.
// 3D: AABB/sphere shapes, impulse resolution with restitution.
// 2D: axis-aligned boxes on the XY plane (2D shapes only interact with 2D).
// Trigger overlap events feed script OnTriggerEnter/OnTriggerExit.
#include "cairn/physics.h"

#include "cairn/engine.h"
#include "cairn/log.h"
#include "scene_internal.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace Cairn {

namespace {

struct Shape {
    enum Kind { Box3, Sphere3, Box2 } kind;
    GameObject* go = nullptr;
    Component* comp = nullptr;
    bool isTrigger = false;
    bool isStatic = true;
    RigidBody* rb = nullptr;
    // World-space bounds.
    Vec3 min, max;      // boxes
    Vec3 center;        // boxes: center; spheres: center
    float radius = 0;   // spheres

    bool Is2D() const { return kind == Box2; }
    bool IsSolid() const { return !isTrigger; }
};

AABB WorldBox(const BoxCollider& col, const Transform& t) {
    Vec3 half = Vec3(col.size.x * t.scale.x, col.size.y * t.scale.y, col.size.z * t.scale.z).Abs() * 0.5f;
    Vec3 c = t.position + Vec3(col.center.x * t.scale.x, col.center.y * t.scale.y, col.center.z * t.scale.z);
    return AABB(c - half, c + half);
}

AABB WorldBox2(const Collider2D& col, const Transform& t) {
    Vec3 half(std::fabs(col.size.x * t.scale.x) * 0.5f, std::fabs(col.size.y * t.scale.y) * 0.5f, 0.0f);
    Vec3 c = t.position + Vec3(col.center.x * t.scale.x, col.center.y * t.scale.y, 0.0f);
    return AABB(c - half, c + half);
}

struct Contact {
    Vec3 normal;      // points from B toward A
    float penetration = 0;
};

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

uint64_t PairKey(const Shape& a, const Shape& b) {
    uint64_t ka = a.comp ? a.comp->m_componentId : 0;
    uint64_t kb = b.comp ? b.comp->m_componentId : 0;
    if (ka > kb) std::swap(ka, kb);
    return (ka << 32) | kb;
}

std::unordered_map<uint64_t, bool>& TriggerPairs() {
    static std::unordered_map<uint64_t, bool> map;
    return map;
}

void NotifyTrigger(const Shape& self, const Shape& other, bool entered) {
    if (!self.go) return;
    for (Component* c : self.go->GetComponentList()) {
        if (c->TypeId() != ComponentTypeId::Script) continue;
        auto* sc = static_cast<ScriptComponent*>(c);
        if (sc->instance && sc->enabled) {
            if (entered) sc->instance->OnTriggerEnter(other.comp);
            else sc->instance->OnTriggerExit(other.comp);
        }
    }
}

void ResolveContact(Shape& a, Shape& b, const Contact& ct) {
    float invMassA = (a.rb && !a.isStatic && a.rb->mass > 0) ? 1.0f / a.rb->mass : 0.0f;
    float invMassB = (b.rb && !b.isStatic && b.rb->mass > 0) ? 1.0f / b.rb->mass : 0.0f;
    float invSum = invMassA + invMassB;
    if (invSum <= 0) return;

    Vec3 push = ct.normal * (ct.penetration / invSum);
    if (invMassA > 0) {
        a.go->transform().position += push * invMassA;
        a.go->transform().m_worldDirty = true;
    }
    if (invMassB > 0) {
        b.go->transform().position -= push * invMassB;
        b.go->transform().m_worldDirty = true;
    }

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

void PhysicsReset() { TriggerPairs().clear(); }

void PhysicsStep(Scene& scene, float dt) {
    if (dt <= 0) return;

    // Gather shapes.
    std::vector<Shape> shapes;
    for (const auto& go : scene.objects) {
        if (!go->ActiveInHierarchy()) continue;
        const Transform& t = go->transform();
        RigidBody* rb = go->GetComponent<RigidBody>();
        bool dynamic = rb && !rb->isKinematic;
        auto add = [&](Shape s) {
            s.go = go.get();
            s.isStatic = !dynamic;
            s.rb = rb;
            shapes.push_back(s);
        };
        if (auto* c = go->GetComponent<BoxCollider>()) {
            AABB box = WorldBox(*c, t);
            Shape s;
            s.kind = Shape::Box3;
            s.comp = c;
            s.isTrigger = c->isTrigger;
            s.min = box.min; s.max = box.max; s.center = box.Center();
            add(s);
        }
        if (auto* c = go->GetComponent<SphereCollider>()) {
            Shape s;
            s.kind = Shape::Sphere3;
            s.comp = c;
            s.isTrigger = c->isTrigger;
            s.center = t.position + Vec3(c->center.x * t.scale.x, c->center.y * t.scale.y, c->center.z * t.scale.z);
            s.radius = c->radius * std::max({t.scale.x, t.scale.y, std::fabs(t.scale.z)});
            add(s);
        }
        if (auto* c = go->GetComponent<Collider2D>()) {
            AABB box = WorldBox2(*c, t);
            Shape s;
            s.kind = Shape::Box2;
            s.comp = c;
            s.isTrigger = c->isTrigger;
            s.min = box.min; s.max = box.max; s.center = box.Center();
            add(s);
        }
    }

    // Integrate dynamic rigidbodies (semi-implicit Euler).
    for (const auto& go : scene.objects) {
        if (!go->ActiveInHierarchy()) continue;
        RigidBody* rb = go->GetComponent<RigidBody>();
        if (!rb || rb->isKinematic) continue;
        Transform& t = go->transform();
        if (rb->mass <= 0) rb->mass = 1.0f;
        if (rb->useGravity) rb->velocity += scene.gravity * rb->gravityScale * dt;
        rb->velocity += (rb->m_accumForce / rb->mass) * dt;
        rb->m_accumForce = Vec3();
        if (rb->drag > 0) rb->velocity *= std::max(0.0f, 1.0f - rb->drag * dt);
        t.position += rb->velocity * dt;
        if (rb->freezeZ) {
            t.position.z = 0.0f;
            rb->velocity.z = 0.0f;
        }
        t.m_worldDirty = true;
    }

    // Refresh world-space bounds after integration.
    {
        size_t i = 0;
        for (const auto& go : scene.objects) {
            if (!go->ActiveInHierarchy()) continue;
            const Transform& t = go->transform();
            if (auto* c = go->GetComponent<BoxCollider>()) {
                AABB box = WorldBox(*c, t);
                shapes[i].min = box.min; shapes[i].max = box.max; shapes[i].center = box.Center();
                ++i;
            }
            if (auto* c = go->GetComponent<SphereCollider>()) {
                shapes[i].center = t.position + Vec3(c->center.x * t.scale.x, c->center.y * t.scale.y, c->center.z * t.scale.z);
                shapes[i].radius = c->radius * std::max({t.scale.x, t.scale.y, std::fabs(t.scale.z)});
                ++i;
            }
            if (auto* c = go->GetComponent<Collider2D>()) {
                AABB box = WorldBox2(*c, t);
                shapes[i].min = box.min; shapes[i].max = box.max; shapes[i].center = box.Center();
                ++i;
            }
        }
    }

    // Narrow phase.
    std::unordered_set<uint64_t> overlappingNow;
    for (size_t a = 0; a < shapes.size(); ++a) {
        for (size_t b = a + 1; b < shapes.size(); ++b) {
            Shape& A = shapes[a];
            Shape& B = shapes[b];
            if (!A.comp || !B.comp) continue;
            bool triggerPair = A.isTrigger || B.isTrigger;
            if (!triggerPair && A.isStatic && B.isStatic) continue;

            Contact ct;
            if (!Collide(A, B, &ct)) continue;

            if (triggerPair) {
                uint64_t key = PairKey(A, B);
                overlappingNow.insert(key);
                bool was = TriggerPairs().count(key) > 0;
                if (!was) {
                    NotifyTrigger(A, B, true);
                    NotifyTrigger(B, A, true);
                    TriggerPairs()[key] = true;
                }
            } else {
                ResolveContact(A, B, ct);
            }
        }
    }

    // Trigger exits.
    for (auto it = TriggerPairs().begin(); it != TriggerPairs().end();) {
        if (overlappingNow.count(it->first) == 0) {
            for (size_t a = 0; a < shapes.size(); ++a) {
                for (size_t b = a + 1; b < shapes.size(); ++b) {
                    if (PairKey(shapes[a], shapes[b]) == it->first) {
                        NotifyTrigger(shapes[a], shapes[b], false);
                        NotifyTrigger(shapes[b], shapes[a], false);
                        a = shapes.size();
                        break;
                    }
                }
            }
            it = TriggerPairs().erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace Cairn
