// Cairn Engine - meshes: procedural primitives, OBJ import, per-object cache.
#pragma once

#include "cairn/math.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Cairn {

struct MeshVertex {
    Vec3 position;
    Vec3 normal{0, 1, 0};
    Vec2 uv;
};

class Mesh {
public:
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;

    bool IsValid() const { return !indices.empty(); }
    AABB Bounds() const;  // over vertex positions
    void RecomputeNormals();

    // Procedural generators (unit sized, centered at origin).
    static std::shared_ptr<Mesh> Cube();
    static std::shared_ptr<Mesh> Sphere(int slices = 24, int stacks = 16);
    static std::shared_ptr<Mesh> Plane(int subdivisions = 1);   // XZ plane, +Y normal, 2x2 units
    static std::shared_ptr<Mesh> Quad();                        // XY plane, -Z normal, 1x1, top-left uv origin
    static std::shared_ptr<Mesh> Cylinder(int slices = 24);
    static std::shared_ptr<Mesh> Cone(int slices = 24);
    static std::shared_ptr<Mesh> Torus(float radius = 0.6f, float tube = 0.25f, int segments = 32, int rings = 16);
};

enum class PrimitiveMesh : int { Cube = 0, Sphere, Plane, Quad, Cylinder, Cone, Torus };

const char* PrimitiveMeshName(PrimitiveMesh p);
bool PrimitiveMeshFromName(const std::string& name, PrimitiveMesh* out);

// Loads triangle meshes from Wavefront .obj (v/vt/vn, fan triangulation).
std::shared_ptr<Mesh> LoadObjFile(const std::string& absolutePath);

// Caches procedural meshes by kind and OBJ meshes by path.
class MeshLibrary {
public:
    std::shared_ptr<Mesh> Get(PrimitiveMesh kind);
    std::shared_ptr<Mesh> GetObj(const std::string& absolutePath);
    void ClearObj();

private:
    std::map<int, std::shared_ptr<Mesh>> primitives_;
    std::map<std::string, std::shared_ptr<Mesh>> objs_;
};

}  // namespace Cairn
