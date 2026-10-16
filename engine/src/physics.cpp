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
    int layer = 0;
    Vec3 min, max;      // world AABB (boxes exact; spheres bounding box)
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

// Nearest ray hit against one shape. `d` must be normalized; returns the
// distance and outward normal at the entry point.
bool RayShape(const Shape& s, const Vec3& o, const Vec3& d, float maxT, float* tOut, Vec3* nOut) {
    if (s.kind == Shape::Sphere3) {
        Vec3 oc = o - s.center;
        float b = Vec3::Dot(oc, d);
        float c = Vec3::Dot(oc, oc) - s.radius * s.radius;
        float disc = b * b - c;
        if (disc < 0) return false;
        float sq = std::sqrt(disc);
        float t = -b - sq;
        if (t < 0) t = -b + sq;
        if (t < 0 || t > maxT) return false;
        *tOut = t;
        *nOut = (o + d * t - s.center).Normalized();
        return true;
    }
    // Slab test against the world AABB.
    auto inv = [](float v) { return std::fabs(v) > 1e-8f ? 1.0f / v : (v >= 0 ? 1e30f : -1e30f); };
    float ix = inv(d.x), iy = inv(d.y), iz = inv(d.z);
    float t1 = (s.min.x - o.x) * ix, t2 = (s.max.x - o.x) * ix;
    float t3 = (s.min.y - o.y) * iy, t4 = (s.max.y - o.y) * iy;
    float t5 = (s.min.z - o.z) * iz, t6 = (s.max.z - o.z) * iz;
    float tmin = std::max({std::min(t1, t2), std::min(t3, t4), std::min(t5, t6)});
    float tmax = std::min({std::max(t1, t2), std::max(t3, t4), std::max(t5, t6)});
    if (tmax < 0 || tmin > tmax || tmin > maxT) return false;
    float t = tmin < 0 ? 0.0f : tmin;
    Vec3 n(0, 0, 0);
    if (tmin == std::min(t1, t2)) n = Vec3(t1 < t2 ? -1.f : 1.f, 0, 0);
    else if (tmin == std::min(t3, t4)) n = Vec3(0, t3 < t4 ? -1.f : 1.f, 0);
    else n = Vec3(0, 0, t5 < t6 ? -1.f : 1.f);
    *tOut = t;
    *nOut = n;
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
            s.layer = go->layer;
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
            s.min = s.center - Vec3(s.radius);
            s.max = s.center + Vec3(s.radius);
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

    // Friction: damp the tangential relative velocity by the contact's
    // coefficient. Recomputed after the normal impulse so it acts on the
    // post-bounce sliding velocity.
    Vec3 rel2;
    if (invMassA > 0 && a.rb) rel2 += a.rb->velocity;
    if (invMassB > 0 && b.rb) rel2 -= b.rb->velocity;
    Vec3 tangent = rel2 - ct.normal * Vec3::Dot(rel2, ct.normal);
    float tl = tangent.Length();
    if (tl > 1e-5f) {
        float mu = std::clamp(std::max(a.rb ? a.rb->friction : 0.0f, b.rb ? b.rb->friction : 0.0f), 0.0f, 1.0f);
        Vec3 imp = tangent * (-mu / invSum);
        if (invMassA > 0 && a.rb) a.rb->velocity += imp * invMassA;
        if (invMassB > 0 && b.rb) b.rb->velocity -= imp * invMassB;
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

        // Angular motion: torque feeds angular velocity, which spins the
        // transform. A simple isotropic inertia keeps it stable.
        Vec3 w = rb->angularVelocity + (rb->m_accumTorque / std::max(1e-4f, mass * 0.4f)) * dt;
        rb->m_accumTorque = Vec3();
        if (rb->drag > 0) w *= std::max(0.0f, 1.0f - rb->drag * dt);
        rb->angularVelocity = w;
        if (w.LengthSq() > 1e-12f) {
            Transform& t = go->transform();
            t.rotation = (Quat::FromAxisAngle(w.Normalized(), w.Length() * dt) * t.rotation).Normalized();
            t.MarkWorldDirty();
        }
    }

    // Shapes are gathered once, after integration.
    std::vector<Shape> shapes;
    GatherShapes(scene, &shapes);

    // Broadphase: sort-and-sweep on the X axis. Sorting the candidate pairs
    // back into index order keeps callback ordering identical to the old
    // brute-force sweep while skipping distant pairs.
    std::unordered_map<uint64_t, PairRecord> seenPairs;
    std::vector<std::pair<size_t, size_t>> candidates;
    if (shapes.size() > 1) {
        std::vector<size_t> order(shapes.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(),
                  [&](size_t l, size_t r) { return shapes[l].min.x < shapes[r].min.x; });
        for (size_t i = 0; i < order.size(); ++i) {
            const size_t a = order[i];
            for (size_t j = i + 1; j < order.size(); ++j) {
                const size_t b = order[j];
                if (shapes[b].min.x > shapes[a].max.x) break;
                candidates.emplace_back(std::min(a, b), std::max(a, b));
            }
        }
        std::sort(candidates.begin(), candidates.end());
    }

    // Narrow phase: contacts and trigger overlaps over the broadphase pairs.
    for (const auto& pr : candidates) {
        Shape& A = shapes[pr.first];
        Shape& B = shapes[pr.second];
        if (!A.comp || !B.comp) continue;
        // Layer matrix: skip pairs the scene says never interact.
        if (!scene.LayerCollides(A.layer, B.layer)) continue;
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

// ---------------------------------------------------------------------------
// Queries.
// ---------------------------------------------------------------------------
bool PhysicsRaycast(Scene& scene, const Vec3& origin, const Vec3& direction, float maxDistance,
                    PhysicsHit* hit) {
    std::vector<Shape> shapes;
    GatherShapes(scene, &shapes);
    Vec3 d = direction.Normalized();
    float best = maxDistance > 0.0f ? maxDistance : 1e30f;
    bool found = false;
    for (const Shape& s : shapes) {
        float t;
        Vec3 n;
        if (!RayShape(s, origin, d, best, &t, &n)) continue;
        if (t <= best) {
            best = t;
            found = true;
            if (hit) {
                hit->go = s.go;
                hit->point = origin + d * t;
                hit->normal = n;
                hit->distance = t;
            }
        }
    }
    return found;
}

std::vector<PhysicsHit> PhysicsRaycastAll(Scene& scene, const Vec3& origin, const Vec3& direction,
                                          float maxDistance) {
    std::vector<Shape> shapes;
    GatherShapes(scene, &shapes);
    Vec3 d = direction.Normalized();
    const float maxT = maxDistance > 0.0f ? maxDistance : 1e30f;
    std::vector<PhysicsHit> hits;
    for (const Shape& s : shapes) {
        float t;
        Vec3 n;
        if (!RayShape(s, origin, d, maxT, &t, &n)) continue;
        hits.push_back({s.go, origin + d * t, n, t});
    }
    std::sort(hits.begin(), hits.end(),
              [](const PhysicsHit& a, const PhysicsHit& b) { return a.distance < b.distance; });
    return hits;
}

std::vector<GameObject*> PhysicsOverlapSphere(Scene& scene, const Vec3& center, float radius) {
    std::vector<Shape> shapes;
    GatherShapes(scene, &shapes);
    std::vector<GameObject*> out;
    for (const Shape& s : shapes) {
        bool overlap = false;
        if (s.kind == Shape::Sphere3) {
            overlap = (center - s.center).Length() <= radius + s.radius;
        } else {
            Vec3 closest(std::clamp(center.x, s.min.x, s.max.x),
                         std::clamp(center.y, s.min.y, s.max.y),
                         std::clamp(center.z, s.min.z, s.max.z));
            overlap = (center - closest).Length() <= radius;
        }
        if (!overlap || !s.go) continue;
        if (std::find(out.begin(), out.end(), s.go) == out.end()) out.push_back(s.go);
    }
    return out;
}

}  // namespace Cairn
