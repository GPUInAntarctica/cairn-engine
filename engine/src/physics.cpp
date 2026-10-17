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
    enum Kind { Box3, Sphere3, Box2, Capsule3 } kind;
    GameObject* go = nullptr;
    Component* comp = nullptr;
    RigidBody* rb = nullptr;
    bool isTrigger = false;
    bool isStatic = true;
    int layer = 0;
    Vec3 min, max;      // world AABB (boxes exact; spheres/capsules bounding box)
    Vec3 center;        // boxes: center; spheres: center; capsules: segment midpoint
    float radius = 0;   // spheres; capsules (cap radius)
    Vec3 p0, p1;        // capsules: world-space segment endpoints

    bool Is2D() const { return kind == Box2; }
    bool IsSolid() const { return !isTrigger; }
    // Effective mass; a non-positive mass behaves as unit mass without the
    // engine ever rewriting the value the user authored.
    float InvMass() const {
        if (!rb || isStatic || !rb->enabled || rb->isKinematic || rb->sleeping) return 0.0f;
        return 1.0f / (rb->mass > 0.0f ? rb->mass : 1.0f);
    }
};

// Clears a body's sleep state so it integrates and responds to contacts again.
void WakeBody(RigidBody* rb) {
    if (!rb) return;
    rb->sleeping = false;
    rb->m_sleepTimer = 0.0f;
}

// Inverse mass used by the joint solver (kinematic and sleeping bodies are
// immovable until woken).
float BodyInvMass(RigidBody* rb) {
    if (!rb || !rb->enabled || rb->isKinematic || rb->sleeping) return 0.0f;
    return 1.0f / (rb->mass > 0.0f ? rb->mass : 1.0f);
}

// Inverse inertia, using the same isotropic approximation the angular
// integrator assumes (`mass * 0.4`), so a joint impulse and a torque agree on
// how much a given spin costs.
float BodyInvInertia(RigidBody* rb) {
    if (!rb || !rb->enabled || rb->isKinematic || rb->sleeping) return 0.0f;
    float mass = rb->mass > 0.0f ? rb->mass : 1.0f;
    return 1.0f / std::max(1e-4f, mass * 0.4f);
}

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

// World-space segment + radius of a capsule whose local axis is `dir`
// (0 = Y, 1 = X, 2 = Z). The perpendicular axes scale the radius, the chosen
// axis scales the height. Height below 2*radius collapses the segment to a
// point (a sphere of the scaled radius).
void WorldCapsule(float radius, float height, int dir, const Vec3& localCenter, const Mat4& world,
                  Vec3* a, Vec3* b, float* rOut) {
    Vec3 col0(world.m[0], world.m[1], world.m[2]);
    Vec3 col1(world.m[4], world.m[5], world.m[6]);
    Vec3 col2(world.m[8], world.m[9], world.m[10]);
    int ax = (dir == 1) ? 0 : (dir == 2) ? 2 : 1;
    float axisScale, rScale;
    if (ax == 0) { axisScale = col0.Length(); rScale = 0.5f * (col1.Length() + col2.Length()); }
    else if (ax == 2) { axisScale = col2.Length(); rScale = 0.5f * (col0.Length() + col1.Length()); }
    else { axisScale = col1.Length(); rScale = 0.5f * (col0.Length() + col2.Length()); }
    Vec3 axisLocal = ax == 0 ? Vec3(1, 0, 0) : ax == 2 ? Vec3(0, 0, 1) : Vec3(0, 1, 0);
    Vec3 axisW = world.TransformDirection(axisLocal);
    float al = axisW.Length();
    Vec3 axisN = al > 1e-8f ? axisW / al : Vec3(0, 1, 0);
    float r = radius * rScale;
    float half = std::max(0.0f, height * axisScale * 0.5f - r);
    Vec3 c = world.TransformPoint(localCenter);
    *a = c + axisN * half;
    *b = c - axisN * half;
    *rOut = r;
}

Vec3 ClosestOnSegment(const Vec3& p, const Vec3& a, const Vec3& b) {
    Vec3 ab = b - a;
    float l2 = ab.LengthSq();
    if (l2 < 1e-12f) return a;
    float t = std::clamp(Vec3::Dot(p - a, ab) / l2, 0.0f, 1.0f);
    return a + ab * t;
}

// Closest points between segments p1q1 and p2q2 (Ericson, Real-Time Collision
// Detection). Degenerate segments fall back to a point test.
void ClosestSegments(const Vec3& p1, const Vec3& q1, const Vec3& p2, const Vec3& q2, Vec3* c1,
                     Vec3* c2) {
    Vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    float a = Vec3::Dot(d1, d1), e = Vec3::Dot(d2, d2), f = Vec3::Dot(d2, r);
    if (a <= 1e-12f && e <= 1e-12f) { *c1 = p1; *c2 = p2; return; }
    if (a <= 1e-12f) {
        *c1 = p1;
        *c2 = p2 + d2 * std::clamp(f / e, 0.0f, 1.0f);
        return;
    }
    float c = Vec3::Dot(d1, r);
    if (e <= 1e-12f) {
        float t = std::clamp(-c / a, 0.0f, 1.0f);
        *c1 = p1 + d1 * t;
        *c2 = p2;
        return;
    }
    float b = Vec3::Dot(d1, d2);
    float denom = a * e - b * b;
    float s = denom > 1e-12f ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
    float t = (b * s + f) / e;
    if (t < 0.0f) { t = 0.0f; s = std::clamp(-c / a, 0.0f, 1.0f); }
    else if (t > 1.0f) { t = 1.0f; s = std::clamp((b - c) / a, 0.0f, 1.0f); }
    *c1 = p1 + d1 * s;
    *c2 = p2 + d2 * t;
}

bool Collide(const Shape& a, const Shape& b, Contact* out) {
    if (a.Is2D() != b.Is2D()) return false;  // never mix 2D and 3D shapes

    // Capsule pairs are resolved exactly (segment/segment and segment/sphere);
    // capsule vs box falls through to the AABB approximation below.
    const bool aCap = a.kind == Shape::Capsule3;
    const bool bCap = b.kind == Shape::Capsule3;
    if (aCap && bCap) {
        Vec3 c1, c2;
        ClosestSegments(a.p0, a.p1, b.p0, b.p1, &c1, &c2);
        Vec3 d = c1 - c2;  // from B toward A
        float dist = d.Length();
        float sum = a.radius + b.radius;
        if (dist >= sum) return false;
        out->penetration = sum - dist;
        out->normal = dist > 1e-6f ? d / dist : Vec3(0, 1, 0);
        return true;
    }
    if ((aCap && b.kind == Shape::Sphere3) || (bCap && a.kind == Shape::Sphere3)) {
        const Shape& cap = aCap ? a : b;
        const Shape& sph = aCap ? b : a;
        Vec3 cp = ClosestOnSegment(sph.center, cap.p0, cap.p1);
        Vec3 d = sph.center - cp;  // from capsule toward sphere
        float dist = d.Length();
        float sum = sph.radius + cap.radius;
        if (dist >= sum) return false;
        out->penetration = sum - dist;
        Vec3 n = dist > 1e-6f ? d / dist : Vec3(0, 1, 0);
        out->normal = aCap ? -n : n;  // must point from B toward A
        return true;
    }

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

// Signed separation between two shapes together with the unit normal pointing
// from `b` toward `a` (the convention Contact::normal uses). Mirrors the pairs
// Collide() resolves: exact for spheres and capsules, AABB gaps for boxes and
// for the capsule-vs-box approximation.
float ShapeSeparation(const Shape& a, const Shape& b, Vec3* nOut) {
    *nOut = Vec3(0, 1, 0);
    if (a.Is2D() != b.Is2D()) return 1e30f;  // 2D and 3D shapes never interact

    const bool aCap = a.kind == Shape::Capsule3;
    const bool bCap = b.kind == Shape::Capsule3;
    Vec3 d;  // from b toward a
    float sep;
    if (aCap && bCap) {
        Vec3 c1, c2;
        ClosestSegments(a.p0, a.p1, b.p0, b.p1, &c1, &c2);
        d = c1 - c2;
        sep = d.Length() - (a.radius + b.radius);
    } else if (aCap && b.kind == Shape::Sphere3) {
        d = b.center - ClosestOnSegment(b.center, a.p0, a.p1);
        sep = d.Length() - (a.radius + b.radius);
    } else if (bCap && a.kind == Shape::Sphere3) {
        d = a.center - ClosestOnSegment(a.center, b.p0, b.p1);
        sep = d.Length() - (a.radius + b.radius);
    } else if (a.kind == Shape::Sphere3 && b.kind == Shape::Sphere3) {
        d = a.center - b.center;
        sep = d.Length() - (a.radius + b.radius);
    } else if (a.kind == Shape::Sphere3 || b.kind == Shape::Sphere3) {
        const Shape& sph = a.kind == Shape::Sphere3 ? a : b;
        const Shape& box = a.kind == Shape::Sphere3 ? b : a;
        Vec3 closest(std::clamp(sph.center.x, box.min.x, box.max.x),
                     std::clamp(sph.center.y, box.min.y, box.max.y),
                     std::clamp(sph.center.z, box.min.z, box.max.z));
        d = sph.center - closest;              // from box toward sphere
        if (a.kind != Shape::Sphere3) d = -d;  // a is the box: reorient to b -> a
        sep = d.Length() - sph.radius;
    } else {
        // AABB vs AABB, the model Collide() uses for boxes (2D ignores Z).
        float gx = std::max(a.min.x - b.max.x, b.min.x - a.max.x);
        float gy = std::max(a.min.y - b.max.y, b.min.y - a.max.y);
        float gz = a.Is2D() ? -1e30f : std::max(a.min.z - b.max.z, b.min.z - a.max.z);
        float g = std::max({gx, gy, gz});
        if (g == gx) *nOut = Vec3(a.min.x > b.max.x ? 1.0f : -1.0f, 0, 0);
        else if (g == gy) *nOut = Vec3(0, a.min.y > b.max.y ? 1.0f : -1.0f, 0);
        else *nOut = Vec3(0, 0, a.min.z > b.max.z ? 1.0f : -1.0f);
        return g;
    }
    float len = d.Length();
    if (len > 1e-9f) *nOut = d / len;
    return sep;
}

// Translates a shape in world space (rotation is not swept by CCD).
void TranslateShape(Shape* s, const Vec3& off) {
    s->min += off;
    s->max += off;
    s->center += off;
    s->p0 += off;
    s->p1 += off;
}

// Nearest ray hit against one shape. `d` must be normalized; returns the
// distance and outward normal at the entry point.
bool RayShape(const Shape& s, const Vec3& o, const Vec3& d, float maxT, float* tOut, Vec3* nOut) {
    if (s.kind == Shape::Capsule3) {
        // Ray vs capsule: the entry on the infinite cylinder (clamped to the
        // segment) plus both cap spheres, nearest wins.
        Vec3 ba = s.p1 - s.p0, oa = o - s.p0;
        float baba = Vec3::Dot(ba, ba);
        float bard = Vec3::Dot(ba, d);
        float baoa = Vec3::Dot(ba, oa);
        float rdoa = Vec3::Dot(d, oa);
        float oaoa = Vec3::Dot(oa, oa);
        float rr = s.radius * s.radius;
        float A = baba - bard * bard;
        float B = baba * rdoa - baoa * bard;
        float C = baba * oaoa - baoa * baoa - rr * baba;
        float h = B * B - A * C;
        float best = maxT;
        bool found = false;
        if (h >= 0.0f && std::fabs(A) > 1e-9f) {
            float t = (-B - std::sqrt(h)) / A;
            float y = baoa + t * bard;
            if (y > 0.0f && y < baba && t >= 0.0f && t <= best) {
                best = t;
                found = true;
                Vec3 p = o + d * t;
                Vec3 cp = s.p0 + ba * (y / baba);
                *nOut = (p - cp).Normalized();
            }
        }
        for (int i = 0; i < 2; ++i) {
            Vec3 c = i == 0 ? s.p0 : s.p1;
            Vec3 oc = o - c;
            float b = Vec3::Dot(oc, d);
            float cc = Vec3::Dot(oc, oc) - rr;
            float disc = b * b - cc;
            if (disc < 0) continue;
            float sq = std::sqrt(disc);
            float t = -b - sq;
            if (t < 0) t = -b + sq;
            if (t < 0 || t > best) continue;
            best = t;
            found = true;
            *nOut = (o + d * t - c).Normalized();
        }
        if (!found) return false;
        *tOut = best;
        return true;
    }
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

// ---------------------------------------------------------------------------
// Joints.
//
// Every joint is built from one primitive: a *point constraint* that drives a
// world point on one body onto a world point on the other, in position and in
// velocity. Because those points sit away from the centre of mass, solving one
// also spins the body - which is what lets a handful of point pairs express
// rotation as well as translation:
//
//   * HingeJoint  two pairs along the hinge axis; rotation about that axis is
//                 the one motion that leaves both pairs together.
//   * FixedJoint  three non-collinear pairs; nothing is left free.
//
// Bodies are point masses with the isotropic inertia the angular integrator
// assumes (see BodyInvInertia), and orientations are read from
// `Transform::rotation` as if it were world space - the same simplification the
// integrator makes, so a joined rigidbody should not sit under a rotated
// parent. A joint with no connected body anchors to a fixed world pose.
// ---------------------------------------------------------------------------
constexpr float kJointArmScale = 1.0f;  // lever arm of an angular point pair
constexpr int kJointIterations = 6;
constexpr int kJointMaxPoints = 3;

// One end of a joint. A null transform marks a fixed world pose: it never
// moves, so every correction simply leaves it where it is.
struct JointEnd {
    RigidBody* rb = nullptr;
    Transform* tf = nullptr;
    Vec3 center;   // world position of the body origin
    Quat rot;      // orientation, treated as world space (see above)
    float invM = 0.0f;
    float invI = 0.0f;
};

JointEnd MakeJointEnd(GameObject* go) {
    JointEnd e;
    if (!go) return e;
    e.tf = &go->transform();
    RigidBody* rb = go->GetComponent<RigidBody>();
    if (rb && (!rb->enabled || rb->isKinematic)) rb = nullptr;
    e.rb = rb;
    e.invM = BodyInvMass(rb);
    e.invI = BodyInvInertia(rb);
    e.center = e.tf->WorldMatrix().GetTranslation();
    e.rot = e.tf->rotation;
    return e;
}

// A body-less end pinned to the origin, so its local offsets are world points.
JointEnd MakeFrozenEnd() {
    return JointEnd();
}

void RefreshJointEnd(JointEnd& e) {
    if (!e.tf) return;
    e.center = e.tf->WorldMatrix().GetTranslation();
    e.rot = e.tf->rotation;
}

// World position of a local offset. A frozen end stores world offsets.
Vec3 JointPoint(const JointEnd& e, const Vec3& local) {
    if (e.tf) return e.tf->WorldMatrix().TransformPoint(local);
    return e.center + local;
}

// World direction of a local direction (unit length; rotation preserves the
// perpendicularity the hinge relies on).
Vec3 JointDirection(const JointEnd& e, const Vec3& local) {
    return (e.rot * local).Normalized();
}

// World point -> local offset, so a captured world point can be replayed
// through that body's transform on later steps.
Vec3 JointLocal(const JointEnd& e, const Vec3& world) {
    if (e.tf) return e.tf->WorldMatrix().Inverted().TransformPoint(world);
    return world - e.center;
}

Vec3 PointVelocity(const JointEnd& e, const Vec3& p) {
    if (!e.rb) return Vec3();
    return e.rb->velocity + Vec3::Cross(e.rb->angularVelocity, p - e.center);
}

void ApplyPointImpulse(const JointEnd& e, const Vec3& p, const Vec3& imp) {
    if (!e.rb) return;
    e.rb->velocity += imp * e.invM;
    e.rb->angularVelocity += Vec3::Cross(p - e.center, imp) * e.invI;
}

// Changes an end's spin while keeping its pinned point still: a body that gains
// angular velocity about a point must also gain the linear velocity that turns
// it about that point instead of about its own centre. Both hinge point pairs
// stay satisfied, which is exactly the joint's free rotation.
void SpinAboutPoint(const JointEnd& e, const Vec3& p, const Vec3& spin) {
    if (!e.rb) return;
    e.rb->angularVelocity += spin;
    e.rb->velocity -= Vec3::Cross(spin, p - e.center);
}

// Turns a body about its centre of mass, the way the angular integrator does.
void RotateTransform(Transform& t, const Vec3& axis, float angle) {
    t.rotation = (Quat::FromAxisAngle(axis, angle) * t.rotation).Normalized();
    t.MarkWorldDirty();
}

// Turns a body about a world pivot instead: the spin is the same, but the
// origin is moved so `pivot` stays put. Rotating a hinge about its anchor this
// way leaves the pin satisfied while the free angle moves.
void RotateAboutPivot(Transform& t, const Vec3& pivot, const Vec3& axis, float angle) {
    const Vec3 before = t.WorldMatrix().GetTranslation();
    Quat turn = Quat::FromAxisAngle(axis, angle);
    t.rotation = (turn * t.rotation).Normalized();
    t.MarkWorldDirty();
    const Vec3 after = t.WorldMatrix().GetTranslation();
    MoveWorld(t, pivot + turn * (before - pivot) - after);
}

// A joint impulse used as a position correction: the linear part moves the
// body, the angular part rotates it about its centre of mass.
void ApplyPointCorrection(const JointEnd& e, const Vec3& p, const Vec3& corr) {
    if (!e.rb || !e.tf) return;
    MoveWorld(*e.tf, corr * e.invM);
    Vec3 spin = Vec3::Cross(p - e.center, corr) * e.invI;
    float angle = spin.Length();
    if (angle > 1e-9f) RotateTransform(*e.tf, spin / angle, angle);
}

// The world points of a joint's constraint set, rebuilt from the live
// transforms so every iteration sees the corrections the previous one made.
struct JointPoints {
    Vec3 a[kJointMaxPoints];
    Vec3 b[kJointMaxPoints];
    int count = 0;
};

// Inverse of the effective point mass matrix a pair presents,
//   M = (invM_A + invM_B) I - invI_A rA rA^T - invI_B rB rB^T,
// applied to `rhs`. Isotropic inertia makes the rotational response a rank-one
// outer product around each body's centre of mass, so M is a diagonal minus two
// rank-one terms; Cramer's rule on that small symmetric system is exact.
// False if M is somehow singular (impossible while an end has mass).
bool SolvePointMass(float d, float a, const Vec3& rA, float b, const Vec3& rB,
                    const Vec3& rhs, Vec3* out) {
    const float ra[3] = {rA.x, rA.y, rA.z};
    const float rb[3] = {rB.x, rB.y, rB.z};
    const float r[3] = {rhs.x, rhs.y, rhs.z};
    float m[3][3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            m[i][j] = (i == j ? d : 0.0f) - a * ra[i] * ra[j] - b * rb[i] * rb[j];
        }
    }
    auto det3 = [](const float q[3][3]) {
        return q[0][0] * (q[1][1] * q[2][2] - q[1][2] * q[2][1])
             - q[0][1] * (q[1][0] * q[2][2] - q[1][2] * q[2][0])
             + q[0][2] * (q[1][0] * q[2][1] - q[1][1] * q[2][0]);
    };
    float det = det3(m);
    if (!(std::fabs(det) > 1e-12f)) return false;
    float x[3];
    for (int col = 0; col < 3; ++col) {
        float c[3][3];
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) c[i][j] = (j == col) ? r[i] : m[i][j];
        }
        x[col] = det3(c) / det;
    }
    *out = Vec3(x[0], x[1], x[2]);
    return true;
}

// Brings one point pair together in one shot: the whole three-component
// position error, then the whole relative velocity, each against the pair's
// effective mass. Both ends share one impulse, so what moves one moves the
// other the opposite way and the pair ends up exactly coincident and still.
void SolvePointPair(const JointEnd& A, const Vec3& pA, const JointEnd& B, const Vec3& pB) {
    Vec3 rA = pA - A.center;
    Vec3 rB = pB - B.center;
    float d = A.invM + B.invM + A.invI * rA.LengthSq() + B.invI * rB.LengthSq();
    Vec3 j;
    if (!SolvePointMass(d, A.invI, rA, B.invI, rB, pB - pA, &j)) return;
    ApplyPointCorrection(A, pA, j);
    ApplyPointCorrection(B, pB, -j);
    Vec3 rel = PointVelocity(B, pB) - PointVelocity(A, pA);
    if (!SolvePointMass(d, A.invI, rA, B.invI, rB, rel, &j)) return;
    ApplyPointImpulse(A, pA, j);
    ApplyPointImpulse(B, pB, -j);
}

// How hard a joint's points are being pulled apart this step; the basis for
// waking sleeping ends and for `breakForce`.
struct JointLoad {
    float error = 0.0f;     // largest positional error (world units)
    float relSpeed = 0.0f;  // largest relative point speed (world units/second)
};

JointLoad MeasureJoint(const JointEnd& A, const JointEnd& B, const JointPoints& p) {
    JointLoad l;
    for (int i = 0; i < p.count; ++i) {
        l.error = std::max(l.error, (p.a[i] - p.b[i]).Length());
        l.relSpeed = std::max(l.relSpeed, (PointVelocity(A, p.a[i]) - PointVelocity(B, p.b[i])).Length());
    }
    return l;
}

// A joint that is off its pose (or carrying speed) is a disturbance: wake both
// ends so the constraint can actually be enforced, and reload their masses.
void WakeJointEnds(JointEnd* A, JointEnd* B, GameObject* goA, GameObject* goB, const JointLoad& load) {
    if (load.error <= 1e-4f && load.relSpeed <= 1e-4f) return;
    if (A->rb && A->rb->sleeping) { WakeBody(A->rb); *A = MakeJointEnd(goA); }
    if (B->rb && B->rb->sleeping) { WakeBody(B->rb); *B = MakeJointEnd(goB); }
}

// `breakForce` reads the same way DistanceJoint does: the corrective impulse
// the step needed, divided by the step.
bool JointBreaks(float breakForce, const JointLoad& load, float invSum, float dt) {
    if (breakForce <= 0.0f || invSum <= 1e-6f) return false;
    float step = dt > 0.0f ? dt : 1.0f;
    return (load.error + load.relSpeed * step) / (invSum * step) > breakForce;
}

// Any unit direction perpendicular to `v`, used as a hinge's angle reference.
Vec3 PerpendicularTo(const Vec3& v) {
    return std::fabs(v.x) < 0.9f ? Vec3::Cross(v, Vec3(1, 0, 0)).Normalized()
                                 : Vec3::Cross(v, Vec3(0, 1, 0)).Normalized();
}

// The two point pairs of a hinge: the anchor, and a point one arm length up the
// hinge axis. Rotation about the axis leaves both pairs together.
JointPoints HingePoints(const JointEnd& A, const JointEnd& B, const Vec3& localAnchor,
                        const Vec3& anchorB, const Vec3& localAxis, const Vec3& axisB) {
    JointPoints p;
    p.count = 2;
    p.a[0] = JointPoint(A, localAnchor);
    p.b[0] = JointPoint(B, anchorB);
    p.a[1] = p.a[0] + JointDirection(A, localAxis) * kJointArmScale;
    p.b[1] = p.b[0] + JointDirection(B, axisB) * kJointArmScale;
    return p;
}

// Solves one hinge pair, rebuilding its world points from the live transforms.
// The other pair of this iteration has already moved the bodies, so solving a
// pair against coordinates captured before that move would fight it.
void SolveHingePoint(const JointEnd& A, const JointEnd& B, const Vec3& localAnchor,
                     const Vec3& anchorB, const Vec3& localAxis, const Vec3& axisB, int index) {
    JointPoints p = HingePoints(A, B, localAnchor, anchorB, localAxis, axisB);
    SolvePointPair(A, p.a[index], B, p.b[index]);
}

// Signed angle from this body's reference direction to the target's, about the
// hinge axis.
float HingeAngle(const JointEnd& A, const JointEnd& B, const Vec3& localAxis,
                 const HingeJoint& joint) {
    Vec3 n = JointDirection(A, localAxis);
    Vec3 refA = JointDirection(A, joint.m_refA);
    Vec3 refB = JointDirection(B, joint.m_refB);
    // Both references are perpendicular to the axis by construction; re-project
    // against numerical drift before measuring.
    refA = (refA - n * Vec3::Dot(refA, n)).Normalized();
    refB = (refB - n * Vec3::Dot(refB, n)).Normalized();
    return std::atan2(Vec3::Dot(Vec3::Cross(refA, refB), n), Vec3::Dot(refA, refB));
}

// A hinge pins its anchor to the target's and keeps the two axes parallel,
// leaving rotation about the axis free. Limits and the motor act on the one
// remaining degree of freedom.
void SolveHingeJoint(Scene& scene, GameObject& go, HingeJoint& joint, float dt) {
    GameObject* target = joint.connectedGuid.empty() ? nullptr : scene.FindByGuid(joint.connectedGuid);
    JointEnd A = MakeJointEnd(&go);
    JointEnd B = target ? MakeJointEnd(target) : MakeFrozenEnd();
    if (!A.rb && !B.rb) return;  // nothing can move

    Vec3 localAxis = joint.axis.Normalized();
    if (localAxis.LengthSq() < 0.5f) localAxis = Vec3(1, 0, 0);

    if (!joint.m_captured) {
        joint.m_captured = true;
        Vec3 axisWorld = JointDirection(A, localAxis);
        Vec3 refLocal = PerpendicularTo(localAxis);
        joint.m_refA = refLocal;
        if (target) {
            // The target's axis and reference ride on its transform, so they
            // stay parallel to this body's as either one turns.
            joint.m_axisB = B.rot.Conjugate() * axisWorld;
            joint.m_refB = B.rot.Conjugate() * (A.rot * refLocal);
        } else {
            // Anchored to the world: the axis and reference are simply frozen.
            joint.m_axisB = axisWorld;
            joint.m_refB = A.rot * refLocal;
        }
        joint.m_restAngle = HingeAngle(A, B, localAxis, joint);
    }

    JointPoints pts = HingePoints(A, B, joint.anchor, joint.connectedAnchor, localAxis, joint.m_axisB);
    JointLoad load = MeasureJoint(A, B, pts);
    WakeJointEnds(&A, &B, &go, target, load);
    pts = HingePoints(A, B, joint.anchor, joint.connectedAnchor, localAxis, joint.m_axisB);

    float invSum = A.invM + B.invM;
    if (JointBreaks(joint.breakForce, load, invSum, dt)) {
        joint.enabled = false;  // the joint snaps; bodies fall free
        return;
    }

    for (int iter = 0; iter < kJointIterations; ++iter) {
        for (int i = 0; i < 2; ++i) {
            RefreshJointEnd(A);
            RefreshJointEnd(B);
            SolveHingePoint(A, B, joint.anchor, joint.connectedAnchor, localAxis, joint.m_axisB, i);
        }
    }
    RefreshJointEnd(A);
    RefreshJointEnd(B);

    float invISum = A.invI + B.invI;
    if (invISum <= 1e-9f) return;  // both ends pinned: the axis needs nothing
    const Vec3 n = JointDirection(A, localAxis);
    const Vec3 spinA = A.rb ? A.rb->angularVelocity : Vec3();
    const Vec3 spinB = B.rb ? B.rb->angularVelocity : Vec3();
    // The angle runs from the other end's reference to this body's. A world
    // anchor cannot turn, so for it the reference is this body's rest pose: the
    // angle is then the body's own swing, and a positive motor or limit turns
    // *this* body the way the axis' right-hand rule says. With a connected body
    // the angle is that body's rotation relative to this one, and a positive
    // motor or limit turns *it*. Both read limitsMin/limitsMax the same way, so
    // one sign flip covers the world-anchored case for angle, spin and torque.
    const float anchorSign = target ? 1.0f : -1.0f;
    float relSpin = anchorSign * Vec3::Dot(spinB - spinA, n);

    // Motor: drive the relative spin toward the target, capped by its force.
    if (joint.useMotor && joint.motorForce > 0.0f) {
        float j = (joint.motorVelocity * kDegToRad - relSpin) / invISum;
        j = std::clamp(j, -joint.motorForce * dt, joint.motorForce * dt);
        SpinAboutPoint(A, JointPoint(A, joint.anchor), n * (-anchorSign * j * A.invI));
        SpinAboutPoint(B, JointPoint(B, joint.connectedAnchor), n * (anchorSign * j * B.invI));
        relSpin += j * invISum;
    }

    // Limits: clamp the angle, and stop only the spin that pushed past it.
    if (joint.useLimits) {
        float lo = joint.limitsMin * kDegToRad;
        float hi = joint.limitsMax * kDegToRad;
        if (lo > hi) std::swap(lo, hi);
        float theta = anchorSign * (HingeAngle(A, B, localAxis, joint) - joint.m_restAngle);
        float delta = theta < lo ? lo - theta : (theta > hi ? hi - theta : 0.0f);
        if (delta != 0.0f) {
            // Turning the driven end forward by +d raises the measured angle by
            // exactly d, so share the correction out by inverse inertia. Each
            // body turns about the shared anchor, so the pin stays satisfied.
            Vec3 pivot = JointPoint(A, joint.anchor);
            if (B.tf) pivot = (pivot + JointPoint(B, joint.connectedAnchor)) * 0.5f;
            float turn = anchorSign * delta;
            if (A.rb && A.tf) RotateAboutPivot(*A.tf, pivot, n, -turn * (A.invI / invISum));
            if (B.rb && B.tf) RotateAboutPivot(*B.tf, pivot, n, turn * (B.invI / invISum));
            bool outward = (delta > 0.0f && relSpin < 0.0f) || (delta < 0.0f && relSpin > 0.0f);
            if (outward) {
                RefreshJointEnd(A);
                RefreshJointEnd(B);
                float j = -relSpin / invISum;
                SpinAboutPoint(A, JointPoint(A, joint.anchor), n * (-anchorSign * j * A.invI));
                SpinAboutPoint(B, JointPoint(B, joint.connectedAnchor), n * (anchorSign * j * B.invI));
            }
        }
    }
}

// A fixed joint welds two bodies into one rigid cluster. The relative pose it
// first sees is locked, so the pair then translates and spins as a single body
// (corrections shared by mass and inertia) until `breakForce` snaps it.
void SolveFixedJoint(Scene& scene, GameObject& go, FixedJoint& joint, float dt) {
    GameObject* target = joint.connectedGuid.empty() ? nullptr : scene.FindByGuid(joint.connectedGuid);
    JointEnd A = MakeJointEnd(&go);
    JointEnd B = target ? MakeJointEnd(target) : MakeFrozenEnd();
    if (!A.rb && !B.rb) return;

    if (!joint.m_captured) {
        joint.m_captured = true;
        // Three non-collinear points on this body, and the matching local
        // points on the target chosen so every pair already coincides: locking
        // all three pairs locks the whole relative pose, which is the weld.
        const Vec3 arm[3] = {Vec3(0, 0, 0), Vec3(kJointArmScale, 0, 0), Vec3(0, kJointArmScale, 0)};
        for (int i = 0; i < 3; ++i) {
            joint.m_localA[i] = arm[i];
            joint.m_localB[i] = JointLocal(B, JointPoint(A, arm[i]));
        }
    }

    JointPoints pts;
    pts.count = 3;
    for (int i = 0; i < 3; ++i) {
        pts.a[i] = JointPoint(A, joint.m_localA[i]);
        pts.b[i] = JointPoint(B, joint.m_localB[i]);
    }
    JointLoad load = MeasureJoint(A, B, pts);
    WakeJointEnds(&A, &B, &go, target, load);

    float invSum = A.invM + B.invM;
    if (invSum <= 1e-6f) return;  // both ends pinned
    if (JointBreaks(joint.breakForce, load, invSum, dt)) {
        joint.enabled = false;
        return;
    }

    for (int iter = 0; iter < kJointIterations; ++iter) {
        for (int i = 0; i < 3; ++i) {
            RefreshJointEnd(A);
            RefreshJointEnd(B);
            SolvePointPair(A, JointPoint(A, joint.m_localA[i]), B,
                           JointPoint(B, joint.m_localB[i]));
        }
    }
}

// Gather every enabled collider of one object in world space.
void GatherObjectShapes(GameObject& go, std::vector<Shape>* out) {
    if (!go.ActiveInHierarchy()) return;
    Transform& t = go.transform();
    Mat4 world = t.WorldMatrix();
    RigidBody* rb = go.GetComponent<RigidBody>();
    bool dynamic = rb && rb->enabled && !rb->isKinematic;
    auto add = [&](Shape s) {
        s.go = &go;
        s.isStatic = !dynamic;
        s.rb = dynamic ? rb : nullptr;
        s.layer = go.layer;
        out->push_back(s);
    };
    if (auto* c = go.GetComponent<BoxCollider>(); c && c->enabled) {
        AABB box = WorldBox(*c, world);
        Shape s;
        s.kind = Shape::Box3;
        s.comp = c;
        s.isTrigger = c->isTrigger;
        s.min = box.min; s.max = box.max; s.center = box.Center();
        add(s);
    }
    if (auto* c = go.GetComponent<SphereCollider>(); c && c->enabled) {
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
    if (auto* c = go.GetComponent<CapsuleCollider>(); c && c->enabled) {
        Shape s;
        s.kind = Shape::Capsule3;
        s.comp = c;
        s.isTrigger = c->isTrigger;
        WorldCapsule(c->radius, c->height, c->direction, c->center, world, &s.p0, &s.p1,
                     &s.radius);
        s.min = Vec3::Min(s.p0, s.p1) - Vec3(s.radius);
        s.max = Vec3::Max(s.p0, s.p1) + Vec3(s.radius);
        s.center = (s.p0 + s.p1) * 0.5f;
        add(s);
    }
    if (auto* c = go.GetComponent<Collider2D>(); c && c->enabled) {
        AABB box = WorldBox2D(*c, world);
        Shape s;
        s.kind = Shape::Box2;
        s.comp = c;
        s.isTrigger = c->isTrigger;
        s.min = box.min; s.max = box.max; s.center = box.Center();
        add(s);
    }
}

// Gather every enabled collider in world space. Called once per step for the
// narrow phase (after integration, so shape data can never drift out of sync
// with the objects) and lazily during integration for the CCD obstacle list.
void GatherShapes(Scene& scene, std::vector<Shape>* out) {
    for (const auto& go : scene.objects) GatherObjectShapes(*go, out);
}

bool HasEnabledCollider(const GameObject& go) {
    for (Component* c : go.GetComponentList()) {
        switch (c->TypeId()) {
            case ComponentTypeId::BoxCollider:
            case ComponentTypeId::SphereCollider:
            case ComponentTypeId::Collider2D:
            case ComponentTypeId::CapsuleCollider:
                if (c->enabled) return true;
                break;
            default:
                break;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Continuous collision detection (CCD).
//
// The discrete step moves a body by `velocity * dt` and only then runs the
// narrow phase, so a body whose single-step displacement is larger than an
// obstacle is thick (a bullet, a long fall) passes straight through it. A body
// that opts in with RigidBody::collisionDetection = Continuous is swept first:
// conservative advancement walks along the motion, advancing only by the
// current separation from the nearest approaching obstacle, which by
// construction can never step past a contact. The displacement is clamped to
// just past the time of impact so the ordinary narrow phase still produces the
// contact impulse, restitution and OnCollisionEnter events.
//
// Only translation is swept and only solid static/kinematic colliders are
// targets - the same scope as Unity's CollisionDetectionMode.Continuous.
// ---------------------------------------------------------------------------

// Obstacles this close at the start of the step are treated as existing
// contacts and left to the discrete narrow phase. Sweeping them would read a
// zero time of impact for a body merely resting on them - which would stop a
// body sliding along a floor dead, and a body co-tangent to a wall dead, both
// of which are ordinary states. The trade is that a body already touching an
// obstacle and *then* given a large velocity into it (a script write or an
// impulse, since the solver zeroes the approach on contact) is not caught by
// the sweep that step. Resting contacts stay cheap and correct; the escape
// hatch is the same one Unity's CCD leaves.
constexpr float kCcdContactSlop = 1e-3f;

// First time in [0,1] at which `mover` translating by `delta` reaches one of
// `obstacles`, or false when the motion ends clear. Each iteration advances by
// the separation from the nearest approaching obstacle, so the sweep can never
// overshoot a contact; the iteration budget only bounds grazing approaches.
bool CcdTimeOfImpact(const Shape& mover, const Vec3& delta, const std::vector<Shape>& obstacles,
                     const Scene& scene, float* tOut) {
    float t = 0.0f;
    for (int iter = 0; iter < 32; ++iter) {
        Shape cur = mover;
        TranslateShape(&cur, delta * t);
        float advance = 1e30f;
        for (const Shape& o : obstacles) {
            if (!scene.LayerCollides(cur.layer, o.layer)) continue;
            Vec3 n;
            float sep = ShapeSeparation(cur, o, &n);
            if (sep > 1e29f) continue;             // mismatched dimensionality
            if (sep <= 0.0f) { *tOut = t; return true; }
            float closing = -Vec3::Dot(delta, n);  // > 0 while approaching
            if (closing <= 1e-9f) continue;        // parallel or receding
            advance = std::min(advance, sep / closing);
        }
        if (advance > 1e29f) return false;         // nothing ahead
        t += advance;
        if (t >= 1.0f) return false;               // motion ends short of it
    }
    *tOut = t;  // budget spent: `t` is still a safe lower bound on the impact
    return true;
}

// Clamps a body's step displacement so it stops at the first solid
// static/kinematic collider it would otherwise tunnel through.
Vec3 CcdDisplacement(GameObject& go, const Vec3& delta, const std::vector<Shape>& obstacles,
                     const Scene& scene) {
    const float dist = delta.Length();
    if (dist < 1e-6f) return delta;
    std::vector<Shape> movers;
    GatherObjectShapes(go, &movers);
    if (movers.empty()) return delta;

    float travel = dist;
    for (const Shape& m : movers) {
        // Drop obstacles already in contact before sweeping (see the slop note
        // above); everything else that can interact is a candidate target.
        std::vector<Shape> ahead;
        ahead.reserve(obstacles.size());
        for (const Shape& o : obstacles) {
            if (!scene.LayerCollides(m.layer, o.layer)) continue;
            Vec3 n;
            if (ShapeSeparation(m, o, &n) < kCcdContactSlop) continue;
            ahead.push_back(o);
        }
        float t;
        if (!CcdTimeOfImpact(m, delta, ahead, scene, &t)) continue;
        // Stop just past the impact so a contact exists for the narrow phase,
        // scaled to the body so the nudge stays meaningful at any scene scale.
        float extent = std::max({m.max.x - m.min.x, m.max.y - m.min.y, m.max.z - m.min.z});
        float skin = std::max(1e-4f, 1e-3f * extent);
        travel = std::min(travel, t * dist + skin);
    }
    return delta / dist * std::min(travel, dist);
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

    // Obstacles for continuous collision detection. Statics never move during
    // a step, so this can be gathered lazily the first time a CCD body is
    // integrated - a scene where nobody opts in pays nothing at all.
    std::vector<Shape> ccdObstacles;
    bool ccdObstaclesReady = false;

    // Integrate dynamic rigidbodies (semi-implicit Euler, world space).
    for (const auto& go : scene.objects) {
        if (!go->ActiveInHierarchy()) continue;
        RigidBody* rb = go->GetComponent<RigidBody>();
        if (!rb || !rb->enabled || rb->isKinematic) continue;
        if (!HasEnabledCollider(*go)) continue;
        // A sleeping body stays put until something gives it velocity (a script
        // writing `velocity`, an impulse, or a contact with a moving body).
        if (rb->sleeping) {
            if (rb->velocity.LengthSq() > 1e-8f || rb->angularVelocity.LengthSq() > 1e-8f) {
                WakeBody(rb);
            } else {
                continue;
            }
        }
        float mass = rb->mass > 0.0f ? rb->mass : 1.0f;
        if (rb->useGravity) rb->velocity += scene.gravity * rb->gravityScale * dt;
        rb->velocity += (rb->m_accumForce / mass) * dt;
        rb->m_accumForce = Vec3();
        if (rb->drag > 0) rb->velocity *= std::max(0.0f, 1.0f - rb->drag * dt);
        if (rb->freezeZ) rb->velocity.z = 0.0f;
        Vec3 delta = rb->velocity * dt;
        if (rb->collisionDetection == RigidBody::CollisionDetection::Continuous) {
            if (!ccdObstaclesReady) {
                std::vector<Shape> all;
                GatherShapes(scene, &all);
                for (const Shape& s : all) {
                    if (s.isStatic && s.IsSolid()) ccdObstacles.push_back(s);
                }
                ccdObstaclesReady = true;
            }
            delta = CcdDisplacement(*go, delta, ccdObstacles, scene);
        }
        MoveWorld(go->transform(), delta);

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
            // A moving body wakes a sleeping one it touches.
            if (A.rb && A.rb->sleeping && B.rb && !B.rb->sleeping) WakeBody(A.rb);
            if (B.rb && B.rb->sleeping && A.rb && !A.rb->sleeping) WakeBody(B.rb);
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

    // Joints: a sequential (Gauss-Seidel) constraint pass after contacts. A
    // rigid joint corrects position and removes relative velocity along the
    // axis; a spring applies a force proportional to the length error.
    for (const auto& go : scene.objects) {
        if (!go->ActiveInHierarchy()) continue;
        auto* joint = go->GetComponent<DistanceJoint>();
        if (!joint || !joint->enabled) continue;
        RigidBody* rbA = go->GetComponent<RigidBody>();
        if (rbA && !rbA->enabled) rbA = nullptr;
        GameObject* target =
            joint->connectedGuid.empty() ? nullptr : scene.FindByGuid(joint->connectedGuid);
        RigidBody* rbB = target ? target->GetComponent<RigidBody>() : nullptr;
        if (rbB && !rbB->enabled) rbB = nullptr;
        if (!rbA && !rbB) continue;  // nothing can move

        Vec3 pa = go->transform().WorldMatrix().TransformPoint(joint->anchor);
        Vec3 pb = target ? target->transform().WorldMatrix().TransformPoint(joint->connectedAnchor)
                         : joint->connectedAnchor;
        Vec3 axis = pa - pb;
        float dist = axis.Length();
        if (dist < 1e-6f) continue;
        Vec3 n = axis / dist;
        float rest = joint->distance > 0.0f
                         ? joint->distance
                         : (joint->m_captured > 0.0f ? joint->m_captured : (joint->m_captured = dist));
        float over = dist - rest;

        // A joint that is off its rest length (or carrying speed) is a
        // disturbance: wake both ends so the constraint is enforced.
        bool disturbed = std::fabs(over) > 1e-4f;
        float invA = BodyInvMass(rbA);
        float invB = BodyInvMass(rbB);
        if (disturbed) {
            if (rbA && rbA->sleeping) { WakeBody(rbA); invA = BodyInvMass(rbA); }
            if (rbB && rbB->sleeping) { WakeBody(rbB); invB = BodyInvMass(rbB); }
        }
        float invSum = invA + invB;
        if (invSum <= 0.0f) continue;

        Vec3 vA = rbA ? rbA->velocity : Vec3();
        Vec3 vB = rbB ? rbB->velocity : Vec3();
        float rel = Vec3::Dot(vA - vB, n);

        if (joint->breakForce > 0.0f) {
            float force = (std::fabs(over) + std::fabs(rel) * dt) / (invSum * (dt > 0 ? dt : 1.0f));
            if (force > joint->breakForce) {
                joint->enabled = false;  // the joint snaps; bodies fall free
                continue;
            }
        }

        if (joint->spring > 0.0f) {
            float f = -joint->spring * over - joint->damping * rel;
            Vec3 imp = n * (f * dt);
            if (rbA) rbA->velocity += imp * invA;
            if (rbB) rbB->velocity -= imp * invB;
        } else {
            if (rbA) MoveWorld(go->transform(), -n * (over * (invA / invSum)));
            if (target && rbB) MoveWorld(target->transform(), n * (over * (invB / invSum)));
            float j = -rel / invSum;
            if (rbA) rbA->velocity += n * (j * invA);
            if (rbB) rbB->velocity -= n * (j * invB);
        }
    }

    // Hinge and fixed joints run in the same constraint pass, through the shared
    // point-constraint solver.
    for (const auto& go : scene.objects) {
        if (!go->ActiveInHierarchy()) continue;
        if (auto* hinge = go->GetComponent<HingeJoint>(); hinge && hinge->enabled) {
            SolveHingeJoint(scene, *go, *hinge, dt);
            continue;
        }
        if (auto* fixed = go->GetComponent<FixedJoint>(); fixed && fixed->enabled) {
            SolveFixedJoint(scene, *go, *fixed, dt);
        }
    }

    // Sleeping: a body that has stayed slow for `sleepDelay` seconds is parked.
    // Evaluated after contacts and joints so resting bodies (whose gravity was
    // just cancelled) are the ones that sleep.
    for (const auto& go : scene.objects) {
        if (!go->ActiveInHierarchy()) continue;
        RigidBody* rb = go->GetComponent<RigidBody>();
        if (!rb || !rb->enabled || rb->isKinematic) continue;
        if (!rb->allowSleep) {
            WakeBody(rb);
            continue;
        }
        if (rb->sleeping) continue;
        float speed = rb->velocity.Length() + rb->angularVelocity.Length();
        if (speed < rb->sleepThreshold) {
            rb->m_sleepTimer += dt;
            if (rb->m_sleepTimer >= rb->sleepDelay) {
                rb->sleeping = true;
                rb->velocity = Vec3();
                rb->angularVelocity = Vec3();
            }
        } else {
            rb->m_sleepTimer = 0.0f;
        }
    }
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
        } else if (s.kind == Shape::Capsule3) {
            Vec3 cp = ClosestOnSegment(center, s.p0, s.p1);
            overlap = (center - cp).Length() <= radius + s.radius;
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

// ---------------------------------------------------------------------------
// Character controller: a swept/penetration-resolved kinematic capsule.
// ---------------------------------------------------------------------------
bool PhysicsCharacterMove(Scene& scene, GameObject& go, CharacterController& cc,
                          const Vec3& displacement) {
    Transform& t = go.transform();
    const float slopeCos = std::cos(cc.slopeLimit * kDegToRad);

    // Colliders to resolve against. The controller is not itself a collider, so
    // it never shows up in this list.
    std::vector<Shape> others;
    GatherShapes(scene, &others);

    auto buildShape = [&](Shape* s) {
        Vec3 a, b;
        float r;
        WorldCapsule(cc.radius, cc.height, 0, cc.center, t.WorldMatrix(), &a, &b, &r);
        s->kind = Shape::Capsule3;
        s->go = &go;
        s->comp = &cc;
        s->rb = nullptr;
        s->isTrigger = false;
        s->isStatic = false;
        s->layer = go.layer;
        s->p0 = a;
        s->p1 = b;
        s->radius = r;
        s->center = (a + b) * 0.5f;
        s->min = Vec3::Min(a, b) - Vec3(r);
        s->max = Vec3::Max(a, b) + Vec3(r);
    };
    auto translate = [](Shape* s, const Vec3& off) {
        s->p0 += off;
        s->p1 += off;
        s->center += off;
        s->min += off;
        s->max += off;
    };

    bool grounded = false;

    // Pushes the controller out of every overlapping solid collider, up to a
    // few iterations so multi-contact corners settle. Contacts with an upward
    // normal mark the controller as grounded.
    auto resolve = [&]() {
        for (int it = 0; it < 6; ++it) {
            Shape ctrl;
            buildShape(&ctrl);
            bool moved = false;
            for (const Shape& o : others) {
                if (!o.comp || !o.IsSolid() || o.go == &go) continue;
                if (!scene.LayerCollides(ctrl.layer, o.layer)) continue;
                if (!AABB(ctrl.min, ctrl.max).Intersects(AABB(o.min, o.max))) continue;
                Contact ct;
                if (!Collide(ctrl, o, &ct) || ct.penetration <= 1e-6f) continue;
                MoveWorld(t, ct.normal * ct.penetration);
                moved = true;
                if (ct.normal.y > slopeCos) grounded = true;
            }
            if (!moved) break;
        }
    };
    // A capsule pushed down by the skin width detects a walkable surface when
    // the controller is resting on one (also used to seed step climbing).
    auto probeGrounded = [&]() -> bool {
        Shape ctrl;
        buildShape(&ctrl);
        translate(&ctrl, Vec3(0, -(cc.skinWidth + 0.02f), 0));
        for (const Shape& o : others) {
            if (!o.comp || !o.IsSolid() || o.go == &go) continue;
            if (!scene.LayerCollides(ctrl.layer, o.layer)) continue;
            if (!AABB(ctrl.min, ctrl.max).Intersects(AABB(o.min, o.max))) continue;
            Contact ct;
            if (Collide(ctrl, o, &ct) && ct.normal.y > slopeCos) return true;
        }
        return false;
    };

    const Vec3 horizontal(displacement.x, 0, displacement.z);
    const Vec3 vertical(0, displacement.y, 0);
    const bool wasGrounded = cc.isGrounded || probeGrounded();

    // Horizontal movement. While grounded we lift by stepOffset first so a low
    // step is climbed rather than treated as a wall, then settle back down.
    if (wasGrounded && cc.stepOffset > 0.0f && horizontal.LengthSq() > 1e-12f) {
        MoveWorld(t, Vec3(0, cc.stepOffset, 0));
        resolve();
        MoveWorld(t, horizontal);
        resolve();
        MoveWorld(t, Vec3(0, -cc.stepOffset, 0));
        resolve();
    } else if (horizontal.LengthSq() > 0.0f) {
        MoveWorld(t, horizontal);
        resolve();
    }

    if (vertical.LengthSq() > 0.0f) {
        MoveWorld(t, vertical);
        resolve();
    }

    grounded = grounded || probeGrounded();
    cc.isGrounded = grounded;
    return grounded;
}

void CharacterController::Move(const Vec3& displacement) {
    PhysicsCharacterMove(gameObject().scene(), gameObject(), *this, displacement);
}

void CharacterController::SimpleMove(const Vec3& v) {
    float dt = Time::DeltaTime();
    float vy = isGrounded ? 0.0f : velocity.y;
    if (useGravity) vy -= 9.81f * gravityScale * dt;
    velocity = Vec3(v.x, vy, v.z);
    Move(Vec3(v.x * dt, vy * dt, v.z * dt));
}

}  // namespace Cairn
