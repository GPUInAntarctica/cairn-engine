// Cairn Engine - math library (header-only).
// Linear algebra for 2D/3D rendering and gameplay code. Column-major Mat4,
// right-handed coordinates, Y up.
#pragma once

#include <cmath>
#include <cstdint>

namespace Cairn {

constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530717958f;
constexpr float kDegToRad = kPi / 180.0f;
constexpr float kRadToDeg = 180.0f / kPi;

// sRGB <-> linear transfer functions. The approximations below are the standard
// cheap fits (|error| < 0.002), which matters because a software rasterizer
// converts every shaded pixel.
inline float SrgbToLinear(float c) {
    if (c <= 0.04045f) return c / 12.92f;
    return std::pow((c + 0.055f) / 1.055f, 2.4f);
}
inline float LinearToSrgb(float c) {
    if (c <= 0.0031308f) return c * 12.92f;
    return 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}
// Polynomial fits (~3 multiplies) used on the hot shading path.
inline float SrgbToLinearFast(float c) {
    if (c <= 0.04045f) return c / 12.92f;
    return c * c * (c * 0.305306011f + 0.682171111f) + 0.012522878f;
}
inline float LinearToSrgbFast(float c) {
    if (c <= 0.0031308f) return c * 12.92f;
    return c * (c * 0.4127714f + 0.5276643f) + 0.0759182f;
}

inline float Radians(float deg) { return deg * kDegToRad; }
inline float Degrees(float rad) { return rad * kRadToDeg; }
inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float SmoothStep(float a, float b, float t) {
    t = Clamp((t - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
inline bool NearlyEqual(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

struct Vec2 {
    float x = 0.0f, y = 0.0f;
    Vec2() = default;
    Vec2(float px, float py) : x(px), y(py) {}
    explicit Vec2(float s) : x(s), y(s) {}
    Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
    Vec2 operator*(const Vec2& o) const { return {x * o.x, y * o.y}; }
    Vec2 operator/(float s) const { return {x / s, y / s}; }
    Vec2 operator-() const { return {-x, -y}; }
    Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(const Vec2& o) { x -= o.x; y -= o.y; return *this; }
    Vec2& operator*=(float s) { x *= s; y *= s; return *this; }
    bool operator==(const Vec2& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Vec2& o) const { return !(*this == o); }
    float Length() const { return std::sqrt(x * x + y * y); }
    float LengthSq() const { return x * x + y * y; }
    Vec2 Normalized() const {
        float len = Length();
        return len > 1e-12f ? Vec2(x / len, y / len) : Vec2(0, 0);
    }
    static float Dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }
    static float Cross(const Vec2& a, const Vec2& b) { return a.x * b.y - a.y * b.x; }
    static Vec2 Lerp(const Vec2& a, const Vec2& b, float t) { return {Cairn::Lerp(a.x, b.x, t), Cairn::Lerp(a.y, b.y, t)}; }
};

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    Vec3() = default;
    Vec3(float px, float py, float pz) : x(px), y(py), z(pz) {}
    explicit Vec3(float s) : x(s), y(s), z(s) {}
    explicit Vec3(Vec2 v, float pz = 0.0f) : x(v.x), y(v.y), z(pz) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator*(const Vec3& o) const { return {x * o.x, y * o.y, z * o.z}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
    bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator!=(const Vec3& o) const { return !(*this == o); }
    float Length() const { return std::sqrt(x * x + y * y + z * z); }
    float LengthSq() const { return x * x + y * y + z * z; }
    Vec3 Normalized() const {
        float len = Length();
        return len > 1e-12f ? Vec3(x / len, y / len, z / len) : Vec3(0, 0, 0);
    }
    Vec2 XY() const { return {x, y}; }
    Vec3 Abs() const { return {std::fabs(x), std::fabs(y), std::fabs(z)}; }
    float operator[](int i) const { return (&x)[i]; }
    float& operator[](int i) { return (&x)[i]; }
    static float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    static Vec3 Cross(const Vec3& a, const Vec3& b) {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }
    static Vec3 Lerp(const Vec3& a, const Vec3& b, float t) {
        return {Cairn::Lerp(a.x, b.x, t), Cairn::Lerp(a.y, b.y, t), Cairn::Lerp(a.z, b.z, t)};
    }
    static Vec3 Min(const Vec3& a, const Vec3& b) {
        return {std::fmin(a.x, b.x), std::fmin(a.y, b.y), std::fmin(a.z, b.z)};
    }
    static Vec3 Max(const Vec3& a, const Vec3& b) {
        return {std::fmax(a.x, b.x), std::fmax(a.y, b.y), std::fmax(a.z, b.z)};
    }
    // Euler angles in degrees applied in YXZ order (yaw, pitch, roll) -> direction vector.
    static Vec3 FromEulerDegrees(const Vec3& euler);
    static const Vec3 Zero, One, Up, Down, Forward, Back, Right, Left;
};

inline const Vec3 Vec3::Zero{0, 0, 0};
inline const Vec3 Vec3::One{1, 1, 1};
inline const Vec3 Vec3::Up{0, 1, 0};
inline const Vec3 Vec3::Down{0, -1, 0};
inline const Vec3 Vec3::Forward{0, 0, -1};  // -Z, matches camera forward
inline const Vec3 Vec3::Back{0, 0, 1};
inline const Vec3 Vec3::Right{1, 0, 0};
inline const Vec3 Vec3::Left{-1, 0, 0};

struct Vec4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
    Vec4() = default;
    Vec4(float px, float py, float pz, float pw) : x(px), y(py), z(pz), w(pw) {}
    explicit Vec4(float s) : x(s), y(s), z(s), w(s) {}
    Vec4(Vec3 v, float pw) : x(v.x), y(v.y), z(v.z), w(pw) {}
    Vec3 XYZ() const { return {x, y, z}; }
    float operator[](int i) const { return (&x)[i]; }
    float& operator[](int i) { return (&x)[i]; }
    Vec4 operator+(const Vec4& o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }
    Vec4 operator-(const Vec4& o) const { return {x - o.x, y - o.y, z - o.z, w - o.w}; }
    Vec4 operator*(float s) const { return {x * s, y * s, z * s, w * s}; }
};

// Quaternion, xyzw storage, w = cos(theta/2).
struct Quat {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
    Quat() = default;
    Quat(float px, float py, float pz, float pw) : x(px), y(py), z(pz), w(pw) {}

    static Quat FromAxisAngle(const Vec3& axis, float radians) {
        Vec3 a = axis.Normalized();
        float s = std::sin(radians * 0.5f);
        return {a.x * s, a.y * s, a.z * s, std::cos(radians * 0.5f)};
    }
    // Euler degrees, ZXY intrinsic order (yaw around Y, then pitch around X,
    // then roll around Z) - matches Unity's Quaternion.Euler composition:
    // q = qY(yaw) * qX(pitch) * qZ(roll).
    static Quat FromEulerDegrees(const Vec3& euler) {
        float cy = std::cos(Radians(euler.y) * 0.5f), sy = std::sin(Radians(euler.y) * 0.5f);
        float cp = std::cos(Radians(euler.x) * 0.5f), sp = std::sin(Radians(euler.x) * 0.5f);
        float cr = std::cos(Radians(euler.z) * 0.5f), sr = std::sin(Radians(euler.z) * 0.5f);
        return {
            cy * sp * cr + cp * sy * sr,
            cp * sy * cr - cy * sp * sr,
            cy * cp * sr - sy * sp * cr,
            cy * cp * cr + sy * sp * sr,
        };
    }
    // Returns Euler degrees in YXZ order.
    Vec3 ToEulerDegrees() const {
        // Rotation matrix entries (column-major storage, see Mat4::FromQuat):
        // R02 = m[8], R12 = m[9], R22 = m[10], R10 = m[1], R11 = m[5],
        // R00 = m[0], R20 = m[2].
        float sinP = 2.0f * (w * x - y * z);
        Vec3 e;
        if (std::fabs(sinP) < 0.99995f) {
            e.x = Degrees(std::asin(Clamp(sinP, -1.0f, 1.0f)));
            e.y = Degrees(std::atan2(2.0f * (w * y + x * z), 1.0f - 2.0f * (x * x + y * y)));
            e.z = Degrees(std::atan2(2.0f * (x * y + w * z), 1.0f - 2.0f * (x * x + z * z)));
        } else {
            // Gimbal lock (pitch +/-90): fold roll into yaw.
            e.x = Degrees(std::copysign(kPi / 2.0f, sinP));
            e.z = 0.0f;
            // R20 = 2(xz - wy), R00 = 1 - 2(y^2 + z^2): y = atan2(-R20, R00).
            e.y = Degrees(std::atan2(-2.0f * (x * z - w * y), 1.0f - 2.0f * (y * y + z * z)));
        }
        return e;
    }
    Quat operator*(const Quat& o) const {
        return {
            w * o.x + x * o.w + y * o.z - z * o.y,
            w * o.y - x * o.z + y * o.w + z * o.x,
            w * o.z + x * o.y - y * o.x + z * o.w,
            w * o.w - x * o.x - y * o.y - z * o.z,
        };
    }
    Vec3 operator*(const Vec3& v) const {  // rotate vector
        Vec3 q{x, y, z};
        Vec3 t = Vec3::Cross(q, v) * 2.0f;
        return v + t * w + Vec3::Cross(q, t);
    }
    Quat Normalized() const {
        float len = std::sqrt(x * x + y * y + z * z + w * w);
        if (len < 1e-12f) return Quat();
        return {x / len, y / len, z / len, w / len};
    }
    Quat Conjugate() const { return {-x, -y, -z, w}; }
    static Quat Slerp(const Quat& a, const Quat& b, float t) {
        float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        Quat end = b;
        if (dot < 0.0f) { dot = -dot; end = {-b.x, -b.y, -b.z, -b.w}; }
        if (dot > 0.9995f) {
            return Quat(a.x + (end.x - a.x) * t, a.y + (end.y - a.y) * t,
                        a.z + (end.z - a.z) * t, a.w + (end.w - a.w) * t).Normalized();
        }
        float theta0 = std::acos(Clamp(dot, -1.0f, 1.0f));
        float theta = theta0 * t;
        float s0 = std::cos(theta) - dot * std::sin(theta) / std::sin(theta0);
        float s1 = std::sin(theta) / std::sin(theta0);
        return Quat(a.x * s0 + end.x * s1, a.y * s0 + end.y * s1,
                    a.z * s0 + end.z * s1, a.w * s0 + end.w * s1);
    }
    static Quat LookRotation(const Vec3& forward, const Vec3& up = Vec3::Up);
};

// Column-major 4x4 matrix (m[col * 4 + row]), same convention as OpenGL.
struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    static Mat4 Identity() { return Mat4(); }
    float* Data() { return m; }
    const float* Data() const { return m; }

    static Mat4 Translation(const Vec3& t) {
        Mat4 r;
        r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
        return r;
    }
    static Mat4 Scale(const Vec3& s) {
        Mat4 r;
        r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z;
        return r;
    }
    static Mat4 FromQuat(const Quat& q) {
        Mat4 r;
        float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        r.m[0] = 1 - 2 * (yy + zz); r.m[1] = 2 * (xy + wz);     r.m[2] = 2 * (xz - wy);
        r.m[4] = 2 * (xy - wz);     r.m[5] = 1 - 2 * (xx + zz); r.m[6] = 2 * (yz + wx);
        r.m[8] = 2 * (xz + wy);     r.m[9] = 2 * (yz - wx);     r.m[10] = 1 - 2 * (xx + yy);
        return r;
    }
    static Mat4 TRS(const Vec3& t, const Quat& q, const Vec3& s) {
        Mat4 r = FromQuat(q);
        r.m[0] *= s.x; r.m[1] *= s.x; r.m[2] *= s.x;
        r.m[4] *= s.y; r.m[5] *= s.y; r.m[6] *= s.y;
        r.m[8] *= s.z; r.m[9] *= s.z; r.m[10] *= s.z;
        r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
        return r;
    }
    static Mat4 Perspective(float fovYRadians, float aspect, float zNear, float zFar) {
        Mat4 r{};
        float f = 1.0f / std::tan(fovYRadians * 0.5f);
        r.m[0] = f / aspect;
        r.m[5] = f;
        r.m[10] = (zFar + zNear) / (zNear - zFar);
        r.m[11] = -1.0f;
        r.m[14] = (2.0f * zFar * zNear) / (zNear - zFar);
        r.m[15] = 0.0f;
        return r;
    }
    static Mat4 Ortho(float left, float right, float bottom, float top, float zNear, float zFar) {
        Mat4 r{};
        r.m[0] = 2.0f / (right - left);
        r.m[5] = 2.0f / (top - bottom);
        r.m[10] = -2.0f / (zFar - zNear);
        r.m[12] = -(right + left) / (right - left);
        r.m[13] = -(top + bottom) / (top - bottom);
        r.m[14] = -(zFar + zNear) / (zFar - zNear);
        // Orthographic: w stays 1, so only the spatial rows are written.
        r.m[3] = r.m[7] = r.m[11] = 0.0f;
        r.m[15] = 1.0f;
        return r;
    }
    static Mat4 LookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
        Vec3 f = (target - eye).Normalized();  // forward
        Vec3 s = Vec3::Cross(f, up).Normalized();
        Vec3 u = Vec3::Cross(s, f);
        Mat4 r;
        r.m[0] = s.x;  r.m[4] = s.y;  r.m[8] = s.z;
        r.m[1] = u.x;  r.m[5] = u.y;  r.m[9] = u.z;
        r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
        r.m[12] = -Vec3::Dot(s, eye);
        r.m[13] = -Vec3::Dot(u, eye);
        r.m[14] = Vec3::Dot(f, eye);
        // View matrices are affine: the homogeneous row must stay (0,0,0,1).
        r.m[3] = r.m[7] = r.m[11] = 0.0f;
        r.m[15] = 1.0f;
        return r;
    }
    Mat4 operator*(const Mat4& o) const {
        Mat4 r{};
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) sum += m[k * 4 + row] * o.m[c * 4 + k];
                r.m[c * 4 + row] = sum;
            }
        return r;
    }
    Vec4 operator*(const Vec4& v) const {
        Vec4 r;
        for (int row = 0; row < 4; ++row)
            r[row] = m[0 * 4 + row] * v.x + m[1 * 4 + row] * v.y + m[2 * 4 + row] * v.z + m[3 * 4 + row] * v.w;
        return r;
    }
    Vec3 TransformPoint(const Vec3& p) const {
        Vec4 r = (*this) * Vec4(p, 1.0f);
        if (std::fabs(r.w) > 1e-12f) return r.XYZ() / r.w;
        return r.XYZ();
    }
    Vec3 TransformDirection(const Vec3& d) const {
        return Vec3(m[0] * d.x + m[4] * d.y + m[8] * d.z,
                    m[1] * d.x + m[5] * d.y + m[9] * d.z,
                    m[2] * d.x + m[6] * d.y + m[10] * d.z);
    }
    Mat4 Transposed() const {
        Mat4 r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) r.m[i * 4 + j] = m[j * 4 + i];
        return r;
    }
    Mat4 Inverted() const;
    Vec3 GetTranslation() const { return {m[12], m[13], m[14]}; }
    Vec3 GetScale() const {
        float sx = Vec3(m[0], m[1], m[2]).Length();
        float sy = Vec3(m[4], m[5], m[6]).Length();
        float sz = Vec3(m[8], m[9], m[10]).Length();
        return {sx, sy, sz};
    }
};

// Axis-aligned bounding box.
struct AABB {
    Vec3 min{0, 0, 0}, max{0, 0, 0};
    AABB() = default;
    AABB(const Vec3& mn, const Vec3& mx) : min(mn), max(mx) {}
    Vec3 Center() const { return (min + max) * 0.5f; }
    Vec3 Extents() const { return (max - min) * 0.5f; }
    Vec3 Size() const { return max - min; }
    void Expand(const Vec3& p) {
        min = Vec3::Min(min, p);
        max = Vec3::Max(max, p);
    }
    void Expand(const AABB& o) {
        min = Vec3::Min(min, o.min);
        max = Vec3::Max(max, o.max);
    }
    bool Intersects(const AABB& o) const {
        return min.x <= o.max.x && max.x >= o.min.x && min.y <= o.max.y && max.y >= o.min.y &&
               min.z <= o.max.z && max.z >= o.min.z;
    }
    bool Contains(const Vec3& p) const {
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z && p.z <= max.z;
    }
    AABB Transformed(const Mat4& m) const {
        // Fast exact transform of the 8 corners. Seed from the first corner so
        // the result never silently picks up the origin.
        AABB out;
        for (int i = 0; i < 8; ++i) {
            Vec3 c((i & 1) ? max.x : min.x, (i & 2) ? max.y : min.y, (i & 4) ? max.z : min.z);
            Vec3 p = m.TransformPoint(c);
            if (i == 0) {
                out.min = out.max = p;
            } else {
                out.Expand(p);
            }
        }
        return out;
    }
    // Ray vs AABB (slab method). Returns distance along ray or -1.
    float RayIntersect(const Vec3& origin, const Vec3& dir) const {
        float tmin = -1e30f, tmax = 1e30f;
        for (int i = 0; i < 3; ++i) {
            float o = origin[i], d = dir[i], lo = min[i], hi = max[i];
            if (std::fabs(d) < 1e-12f) {
                if (o < lo || o > hi) return -1.0f;
            } else {
                float t1 = (lo - o) / d, t2 = (hi - o) / d;
                if (t1 > t2) std::swap(t1, t2);
                tmin = std::fmax(tmin, t1);
                tmax = std::fmin(tmax, t2);
                if (tmin > tmax) return -1.0f;
            }
        }
        if (tmax < 0.0f) return -1.0f;
        return tmin >= 0.0f ? tmin : 0.0f;
    }
};

inline Vec3 Vec3::FromEulerDegrees(const Vec3& euler) {
    return Quat::FromEulerDegrees(euler) * Vec3::Forward;
}

inline Mat4 Mat4::Inverted() const {
    const float* m = this->m;
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (std::fabs(det) < 1e-12f) return Identity();
    float invDet = 1.0f / det;
    Mat4 r;
    for (int i = 0; i < 16; ++i) r.m[i] = inv[i] * invDet;
    return r;
}

inline Quat Quat::LookRotation(const Vec3& forward, const Vec3& up) {
    Vec3 f = forward.Normalized();
    if (f.LengthSq() < 1e-12f) f = Vec3::Forward;
    Vec3 s = Vec3::Cross(up, f);
    if (s.LengthSq() < 1e-12f) {
        Vec3 alt = std::fabs(f.y) > 0.99f ? Vec3::Right : Vec3::Up;
        s = Vec3::Cross(alt, f);
    }
    s = s.Normalized();
    Vec3 u = Vec3::Cross(f, s);
    // Build rotation matrix and extract quaternion.
    Mat4 m;
    m.m[0] = s.x; m.m[4] = s.y; m.m[8] = s.z;
    m.m[1] = u.x; m.m[5] = u.y; m.m[9] = u.z;
    m.m[2] = f.x; m.m[6] = f.y; m.m[10] = f.z;
    float trace = m.m[0] + m.m[5] + m.m[10];
    Quat q;
    if (trace > 0.0f) {
        float t = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * t;
        q.x = (m.m[9] - m.m[6]) / t;
        q.y = (m.m[2] - m.m[8]) / t;
        q.z = (m.m[4] - m.m[1]) / t;
    } else if (m.m[0] > m.m[5] && m.m[0] > m.m[10]) {
        float t = std::sqrt(1.0f + m.m[0] - m.m[5] - m.m[10]) * 2.0f;
        q.w = (m.m[9] - m.m[6]) / t;
        q.x = 0.25f * t;
        q.y = (m.m[1] + m.m[4]) / t;
        q.z = (m.m[2] + m.m[8]) / t;
    } else if (m.m[5] > m.m[10]) {
        float t = std::sqrt(1.0f + m.m[5] - m.m[0] - m.m[10]) * 2.0f;
        q.w = (m.m[2] - m.m[8]) / t;
        q.x = (m.m[1] + m.m[4]) / t;
        q.y = 0.25f * t;
        q.z = (m.m[6] + m.m[9]) / t;
    } else {
        float t = std::sqrt(1.0f + m.m[10] - m.m[0] - m.m[5]) * 2.0f;
        q.w = (m.m[4] - m.m[1]) / t;
        q.x = (m.m[2] + m.m[8]) / t;
        q.y = (m.m[6] + m.m[9]) / t;
        q.z = 0.25f * t;
    }
    return q.Normalized();
}

// RGBA color, components in [0, 1].
struct Color {
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
    Color() = default;
    Color(float pr, float pg, float pb, float pa = 1.0f) : r(pr), g(pg), b(pb), a(pa) {}
    static Color FromHex(uint32_t hex, float alpha = 1.0f) {
        return Color(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, alpha);
    }
    static Color FromHSV(float h, float s, float v, float a = 1.0f) {
        h = h - std::floor(h);
        float c = v * s, x = c * (1.0f - std::fabs(std::fmod(h * 6.0f, 2.0f) - 1.0f)), m = v - c;
        float r = 0, g = 0, b = 0;
        if (h < 1.0f / 6.0f) { r = c; g = x; }
        else if (h < 2.0f / 6.0f) { r = x; g = c; }
        else if (h < 3.0f / 6.0f) { g = c; b = x; }
        else if (h < 4.0f / 6.0f) { g = x; b = c; }
        else if (h < 5.0f / 6.0f) { r = x; b = c; }
        else { r = c; b = x; }
        return Color(r + m, g + m, b + m, a);
    }
    uint32_t ToRGBA8() const {
        auto c = [](float v) { return (uint8_t)Clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f; };
        return (uint32_t(c(r)) << 24) | (uint32_t(c(g)) << 16) | (uint32_t(c(b)) << 8) | uint32_t(c(a));
    }
    Color operator*(float s) const { return {r * s, g * s, b * s, a}; }
    Color operator*(const Color& o) const { return {r * o.r, g * o.g, b * o.b, a * o.a}; }
    Color operator+(const Color& o) const { return {r + o.r, g + o.g, b + o.b, a + o.a}; }
    bool operator==(const Color& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
    bool operator!=(const Color& o) const { return !(*this == o); }
    static Color Lerp(const Color& a, const Color& b, float t) {
        return {Cairn::Lerp(a.r, b.r, t), Cairn::Lerp(a.g, b.g, t), Cairn::Lerp(a.b, b.b, t), Cairn::Lerp(a.a, b.a, t)};
    }
    static const Color White, Black, Red, Green, Blue, Yellow, Gray;
};

inline const Color Color::White{1, 1, 1};
inline const Color Color::Black{0, 0, 0};
inline const Color Color::Red{0.90f, 0.25f, 0.25f};
inline const Color Color::Green{0.30f, 0.85f, 0.35f};
inline const Color Color::Blue{0.30f, 0.50f, 0.95f};
inline const Color Color::Yellow{0.95f, 0.85f, 0.25f};
inline const Color Color::Gray{0.5f, 0.5f, 0.5f};

}  // namespace Cairn
