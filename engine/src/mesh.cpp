#include "cairn/mesh.h"

#include "cairn/log.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <tuple>

namespace Cairn {

AABB Mesh::Bounds() const {
    // Cached: culling, picking and selection all ask for this every frame, and
    // the answer only changes when the vertex buffer does.
    if (m_boundsValid) return m_bounds;
    AABB bounds;
    bool first = true;
    for (const auto& v : vertices) {
        if (first) {
            bounds.min = bounds.max = v.position;
            first = false;
        } else {
            bounds.Expand(v.position);
        }
    }
    m_bounds = bounds;
    m_boundsValid = true;
    return bounds;
}

void Mesh::RecomputeNormals() {
    InvalidateBounds();
    for (auto& v : vertices) v.normal = Vec3(0, 0, 0);
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        MeshVertex& a = vertices[indices[i]];
        MeshVertex& b = vertices[indices[i + 1]];
        MeshVertex& c = vertices[indices[i + 2]];
        Vec3 n = Vec3::Cross(b.position - a.position, c.position - a.position);
        a.normal += n;
        b.normal += n;
        c.normal += n;
    }
    for (auto& v : vertices) v.normal = v.normal.Normalized();
    // Existing callers expect a usable tangent frame, not stale data.
    RecomputeTangents();
}

// Lengyel's method: accumulate per-triangle tangent/bitangent from the UV
// gradient, then orthonormalize against the vertex normal. Handedness records
// which side the bitangent falls on so shading can rebuild B = cross(N,T)*w.
void Mesh::RecomputeTangents() {
    for (auto& v : vertices) v.tangent = Vec4(0, 0, 0, 0);
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        MeshVertex& a = vertices[indices[i]];
        MeshVertex& b = vertices[indices[i + 1]];
        MeshVertex& c = vertices[indices[i + 2]];
        const Vec3 e1 = b.position - a.position;
        const Vec3 e2 = c.position - a.position;
        const float du1 = b.uv.x - a.uv.x, dv1 = b.uv.y - a.uv.y;
        const float du2 = c.uv.x - a.uv.x, dv2 = c.uv.y - a.uv.y;
        const float det = du1 * dv2 - du2 * dv1;
        if (std::fabs(det) < 1e-12f) continue;  // degenerate UV layout: skip
        const float r = 1.0f / det;
        const Vec3 t = (e1 * dv2 - e2 * dv1) * r;
        const Vec3 bt = (e2 * du1 - e1 * du2) * r;
        a.tangent.x += t.x; a.tangent.y += t.y; a.tangent.z += t.z;
        b.tangent.x += t.x; b.tangent.y += t.y; b.tangent.z += t.z;
        c.tangent.x += t.x; c.tangent.y += t.y; c.tangent.z += t.z;
        const Vec3 n = (a.normal + b.normal + c.normal);
        const float hand = Vec3::Dot(Vec3::Cross(n, t), bt) >= 0.0f ? 1.0f : -1.0f;
        a.tangent.w += hand;
        b.tangent.w += hand;
        c.tangent.w += hand;
    }
    for (auto& v : vertices) {
        Vec3 t(v.tangent.x, v.tangent.y, v.tangent.z);
        const Vec3 n = v.normal.Normalized();  // custom meshes may have non-unit normals
        const float along = Vec3::Dot(n, t);
        t = t - n * along;                      // orthonormal to the normal
        if (t.LengthSq() > 1e-12f) {
            t = t.Normalized();
            v.tangent = Vec4(t, v.tangent.w >= 0.0f ? 1.0f : -1.0f);
        } else {
            // No usable UV gradient at this vertex; fall back to any vector
            // perpendicular to the normal so the frame stays valid.
            Vec3 up = std::fabs(n.y) > 0.95f ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
            t = Vec3::Cross(n, up).Normalized();
            v.tangent = Vec4(t, 1.0f);
        }
    }
}

void Mesh::RecomputeNormalsAndTangents() {
    RecomputeNormals();  // recomputes tangents as well
}

std::shared_ptr<Mesh> Mesh::Cube() {
    auto m = std::make_shared<Mesh>();
    // 6 faces, flat normals, uv per face.
    struct Face { Vec3 n, up, right; };
    const Face faces[6] = {
        {Vec3(0, 0, -1), Vec3(0, 1, 0), Vec3(1, 0, 0)},   // front  (-Z)
        {Vec3(0, 0, 1), Vec3(0, 1, 0), Vec3(-1, 0, 0)},   // back
        {Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1)},    // right
        {Vec3(-1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, -1)},  // left
        {Vec3(0, 1, 0), Vec3(0, 0, 1), Vec3(1, 0, 0)},    // top
        {Vec3(0, -1, 0), Vec3(0, 0, -1), Vec3(1, 0, 0)},  // bottom
    };
    for (const Face& f : faces) {
        Vec3 c = f.n * 0.5f;
        Vec3 u = f.right * 0.5f;
        Vec3 v = f.up * 0.5f;
        uint32_t base = (uint32_t)m->vertices.size();
        const float cornerU[4] = {0, 1, 1, 0};
        const float cornerV[4] = {0, 0, 1, 1};
        const int cornerSign[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (int i = 0; i < 4; ++i) {
            MeshVertex vert;
            vert.position = c + u * (float)cornerSign[i][0] + v * (float)cornerSign[i][1];
            vert.normal = f.n;
            vert.uv = {cornerU[i], cornerV[i]};
            m->vertices.push_back(vert);
        }
        // Winding matches the outward face normals (counter-clockwise seen
        // from outside), so back-face culling keeps the visible faces.
        m->indices.insert(m->indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
    }
    m->RecomputeTangents();
    return m;
}

std::shared_ptr<Mesh> Mesh::Sphere(int slices, int stacks) {
    auto m = std::make_shared<Mesh>();
    for (int s = 0; s <= stacks; ++s) {
        float v = (float)s / stacks;
        float phi = v * kPi;  // 0 at top
        for (int i = 0; i <= slices; ++i) {
            float u = (float)i / slices;
            float theta = u * kTwoPi;
            float sp = std::sin(phi), cp = std::cos(phi);
            float st = std::sin(theta), ct = std::cos(theta);
            Vec3 n(sp * ct, cp, sp * st);
            m->vertices.push_back({n * 0.5f, n, {u, v}});
        }
    }
    int row = slices + 1;
    for (int s = 0; s < stacks; ++s) {
        for (int i = 0; i < slices; ++i) {
            uint32_t a = (uint32_t)(s * row + i);
            uint32_t b = (uint32_t)(s * row + i + 1);
            uint32_t c = (uint32_t)((s + 1) * row + i);
            uint32_t d = (uint32_t)((s + 1) * row + i + 1);
            m->indices.insert(m->indices.end(), {a, d, c, a, b, d});
        }
    }
    m->RecomputeTangents();
    return m;
}

std::shared_ptr<Mesh> Mesh::Plane(int subdivisions) {
    auto m = std::make_shared<Mesh>();
    int n = std::max(1, subdivisions);
    for (int z = 0; z <= n; ++z) {
        for (int x = 0; x <= n; ++x) {
            float fx = (float)x / n, fz = (float)z / n;
            m->vertices.push_back({Vec3(fx - 0.5f, 0, fz - 0.5f) * 2.0f, Vec3(0, 1, 0), {fx, fz}});
        }
    }
    int row = n + 1;
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            uint32_t a = (uint32_t)(z * row + x);
            uint32_t b = (uint32_t)(z * row + x + 1);
            uint32_t c = (uint32_t)((z + 1) * row + x);
            uint32_t d = (uint32_t)((z + 1) * row + x + 1);
            m->indices.insert(m->indices.end(), {a, c, d, a, d, b});
        }
    }
    m->RecomputeTangents();
    return m;
}

std::shared_ptr<Mesh> Mesh::Quad() {
    // XY plane facing -Z (toward the default camera), 1x1 unit, origin center.
    // uv origin at top-left so images map naturally.
    auto m = std::make_shared<Mesh>();
    m->vertices = {
        {{-0.5f, 0.5f, 0}, Vec3(0, 0, -1), {0, 0}},
        {{0.5f, 0.5f, 0}, Vec3(0, 0, -1), {1, 0}},
        {{0.5f, -0.5f, 0}, Vec3(0, 0, -1), {1, 1}},
        {{-0.5f, -0.5f, 0}, Vec3(0, 0, -1), {0, 1}},
    };
    m->indices = {0, 1, 2, 0, 2, 3};
    m->RecomputeTangents();
    return m;
}

std::shared_ptr<Mesh> Mesh::Cylinder(int slices) {
    auto m = std::make_shared<Mesh>();
    const float h = 0.5f;
    // Side.
    for (int i = 0; i <= slices; ++i) {
        float u = (float)i / slices;
        float theta = u * kTwoPi;
        float st = std::sin(theta), ct = std::cos(theta);
        Vec3 n(ct, 0, st);
        m->vertices.push_back({Vec3(ct * 0.5f, h, st * 0.5f), n, {u, 0}});
        m->vertices.push_back({Vec3(ct * 0.5f, -h, st * 0.5f), n, {u, 1}});
    }
    for (int i = 0; i < slices; ++i) {
        uint32_t a = (uint32_t)(i * 2);
        // Outward winding: seen from outside, top_i -> bottom_{i+1} -> bottom_i.
        m->indices.insert(m->indices.end(), {a, a + 3, a + 1, a, a + 2, a + 3});
    }
    // Caps.
    for (int cap = 0; cap < 2; ++cap) {
        float y = cap == 0 ? h : -h;
        Vec3 n(0, cap == 0 ? 1.0f : -1.0f, 0);
        uint32_t center = (uint32_t)m->vertices.size();
        m->vertices.push_back({Vec3(0, y, 0), n, {0.5f, 0.5f}});
        for (int i = 0; i <= slices; ++i) {
            float u = (float)i / slices;
            float theta = u * kTwoPi;
            float st = std::sin(theta), ct = std::cos(theta);
            m->vertices.push_back({Vec3(ct * 0.5f, y, st * 0.5f), n, {0.5f + ct * 0.5f, 0.5f + st * 0.5f}});
        }
        for (int i = 0; i < slices; ++i) {
            uint32_t a = center + 1 + (uint32_t)i;
            // Counter-clockwise seen from outside the cap.
            if (cap == 0) m->indices.insert(m->indices.end(), {center, a + 1, a});
            else m->indices.insert(m->indices.end(), {center, a, a + 1});
        }
    }
    m->RecomputeTangents();
    return m;
}

std::shared_ptr<Mesh> Mesh::Cone(int slices) {
    auto m = std::make_shared<Mesh>();
    const float h = 0.5f;
    for (int i = 0; i <= slices; ++i) {
        float u = (float)i / slices;
        float theta = u * kTwoPi;
        float st = std::sin(theta), ct = std::cos(theta);
        // Slope normal for a unit cone.
        Vec3 n = Vec3(ct, 0.5f, st).Normalized();
        m->vertices.push_back({Vec3(0, h, 0), n, {u, 0}});
        m->vertices.push_back({Vec3(ct * 0.5f, -h, st * 0.5f), n, {u, 1}});
    }
    for (int i = 0; i < slices; ++i) {
        uint32_t a = (uint32_t)(i * 2);
        // Outward winding on the slope: apex -> base_{i+1} -> base_i.
        m->indices.insert(m->indices.end(), {a, a + 3, a + 1});
    }
    // Base cap.
    Vec3 n(0, -1, 0);
    uint32_t center = (uint32_t)m->vertices.size();
    m->vertices.push_back({Vec3(0, -h, 0), n, {0.5f, 0.5f}});
    for (int i = 0; i <= slices; ++i) {
        float u = (float)i / slices;
        float theta = u * kTwoPi;
        float st = std::sin(theta), ct = std::cos(theta);
        m->vertices.push_back({Vec3(ct * 0.5f, -h, st * 0.5f), n, {0.5f + ct * 0.5f, 0.5f + st * 0.5f}});
    }
    for (int i = 0; i < slices; ++i) {
        uint32_t a = center + 1 + (uint32_t)i;
        m->indices.insert(m->indices.end(), {center, a, a + 1});
    }
    m->RecomputeTangents();
    return m;
}

std::shared_ptr<Mesh> Mesh::Torus(float radius, float tube, int segments, int rings) {
    auto m = std::make_shared<Mesh>();
    for (int j = 0; j <= rings; ++j) {
        float v = (float)j / rings;
        float phi = v * kTwoPi;
        float cp = std::cos(phi), sp = std::sin(phi);
        for (int i = 0; i <= segments; ++i) {
            float u = (float)i / segments;
            float theta = u * kTwoPi;
            float ct = std::cos(theta), st = std::sin(theta);
            // Tube normal: from the ring center to the surface point, i.e.
            // (ct*cp, st, ct*sp) — unit length by construction. (This used to
            // mix the ring angle into the z term, which skewed shading.)
            Vec3 n(cp * ct, st, sp * ct);
            Vec3 p((radius + tube * ct) * cp, tube * st, (radius + tube * ct) * sp);
            m->vertices.push_back({p, n, {u, v}});
        }
    }
    int row = segments + 1;
    for (int j = 0; j < rings; ++j) {
        for (int i = 0; i < segments; ++i) {
            uint32_t a = (uint32_t)(j * row + i);
            uint32_t b = (uint32_t)(j * row + i + 1);
            uint32_t c = (uint32_t)((j + 1) * row + i);
            uint32_t d = (uint32_t)((j + 1) * row + i + 1);
            m->indices.insert(m->indices.end(), {a, b, d, a, d, c});
        }
    }
    m->RecomputeTangents();
    return m;
}

const char* PrimitiveMeshName(PrimitiveMesh p) {
    switch (p) {
        case PrimitiveMesh::Cube: return "Cube";
        case PrimitiveMesh::Sphere: return "Sphere";
        case PrimitiveMesh::Plane: return "Plane";
        case PrimitiveMesh::Quad: return "Quad";
        case PrimitiveMesh::Cylinder: return "Cylinder";
        case PrimitiveMesh::Cone: return "Cone";
        case PrimitiveMesh::Torus: return "Torus";
    }
    return "Cube";
}

bool PrimitiveMeshFromName(const std::string& name, PrimitiveMesh* out) {
    if (name == "Cube") { *out = PrimitiveMesh::Cube; return true; }
    if (name == "Sphere") { *out = PrimitiveMesh::Sphere; return true; }
    if (name == "Plane") { *out = PrimitiveMesh::Plane; return true; }
    if (name == "Quad") { *out = PrimitiveMesh::Quad; return true; }
    if (name == "Cylinder") { *out = PrimitiveMesh::Cylinder; return true; }
    if (name == "Cone") { *out = PrimitiveMesh::Cone; return true; }
    if (name == "Torus") { *out = PrimitiveMesh::Torus; return true; }
    return false;
}

std::shared_ptr<Mesh> LoadObjFile(const std::string& absolutePath) {
    std::ifstream file(absolutePath);
    if (!file.is_open()) {
        Log::Warning("cannot open obj: %s", absolutePath.c_str());
        return nullptr;
    }
    auto mesh = std::make_shared<Mesh>();
    std::vector<Vec3> positions;
    std::vector<Vec2> uvs;
    std::vector<Vec3> normals;
    // Index sharing: OBJ corners repeat (position, uv, normal) triples across
    // faces; hashing the triple keeps one vertex per unique combination, which
    // is what smooth shading and tangent computation both need.
    std::map<std::tuple<int, int, int>, uint32_t> cornerToVertex;
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream ss(line);
        std::string token;
        ss >> token;
        if (token == "v") {
            float x, y, z;
            ss >> x >> y >> z;
            positions.push_back({x, y, z});
        } else if (token == "vt") {
            float u, v;
            ss >> u >> v;
            uvs.push_back({u, 1.0f - v});  // obj v is bottom-up; ours is top-down
        } else if (token == "vn") {
            float x, y, z;
            ss >> x >> y >> z;
            normals.push_back(Vec3(x, y, z).Normalized());
        } else if (token == "f") {
            std::vector<uint32_t> face;
            std::string corner;
            while (ss >> corner) {
                int pi = 0, ti = 0, ni = 0;
                if (std::sscanf(corner.c_str(), "%d/%d/%d", &pi, &ti, &ni) >= 1 ||
                    std::sscanf(corner.c_str(), "%d//%d", &pi, &ni) >= 1) {
                    auto resolve = [](int idx, size_t count) -> int {
                        if (idx > 0) return idx - 1;
                        return (int)count + idx;
                    };
                    int p = resolve(pi, positions.size());
                    if (p < 0 || p >= (int)positions.size()) continue;
                    int t = 0, n = 0;
                    MeshVertex vert;
                    vert.position = positions[p];
                    if (ti != 0) {
                        t = resolve(ti, uvs.size());
                        if (t >= 0 && t < (int)uvs.size()) vert.uv = uvs[t];
                        else t = 0;
                    }
                    if (ni != 0) {
                        n = resolve(ni, normals.size());
                        if (n >= 0 && n < (int)normals.size()) vert.normal = normals[n];
                        else n = 0;
                    }
                    auto [it, inserted] =
                        cornerToVertex.try_emplace(std::make_tuple(p, t, n), (uint32_t)mesh->vertices.size());
                    if (inserted) mesh->vertices.push_back(vert);
                    face.push_back(it->second);
                }
            }
            // Fan triangulation.
            for (size_t i = 1; i + 1 < face.size(); ++i)
                mesh->indices.insert(mesh->indices.end(), {face[0], face[i], face[i + 1]});
        }
    }
    // Missing normals -> recompute (which also builds tangents); otherwise the
    // tangent frame is all that is missing.
    bool hasNormals = false;
    for (const auto& v : mesh->vertices)
        if (v.normal.LengthSq() > 1e-9f) { hasNormals = true; break; }
    if (hasNormals) {
        mesh->RecomputeTangents();
    } else {
        mesh->RecomputeNormalsAndTangents();
    }
    if (mesh->indices.empty()) {
        Log::Warning("obj has no usable faces: %s", absolutePath.c_str());
        return nullptr;
    }
    return mesh;
}

std::shared_ptr<Mesh> MeshLibrary::Get(PrimitiveMesh kind) {
    auto it = primitives_.find((int)kind);
    if (it != primitives_.end()) return it->second;
    std::shared_ptr<Mesh> m;
    switch (kind) {
        case PrimitiveMesh::Cube: m = Mesh::Cube(); break;
        case PrimitiveMesh::Sphere: m = Mesh::Sphere(); break;
        case PrimitiveMesh::Plane: m = Mesh::Plane(); break;
        case PrimitiveMesh::Quad: m = Mesh::Quad(); break;
        case PrimitiveMesh::Cylinder: m = Mesh::Cylinder(); break;
        case PrimitiveMesh::Cone: m = Mesh::Cone(); break;
        case PrimitiveMesh::Torus: m = Mesh::Torus(); break;
    }
    primitives_[(int)kind] = m;
    return m;
}

std::shared_ptr<Mesh> MeshLibrary::GetObj(const std::string& absolutePath) {
    auto it = objs_.find(absolutePath);
    if (it != objs_.end()) return it->second;
    auto m = LoadObjFile(absolutePath);
    objs_[absolutePath] = m;
    return m;
}

void MeshLibrary::ClearObj() { objs_.clear(); }

}  // namespace Cairn
