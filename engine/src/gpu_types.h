// Cairn Engine - render data shared by the software and Vulkan backends.
// The per-frame draw list is built once by Renderer::RenderScene and consumed
// by whichever backend the scene's `backend` setting selects.
#pragma once

#include "cairn/mesh.h"
#include "cairn/renderer.h"
#include "cairn/texture.h"

#include <memory>
#include <vector>

namespace Cairn {

static constexpr int kMaxLights = 16;          // lights evaluated per pixel
static constexpr int kMaxCascades = 4;         // directional shadow cascades

// Material state for one draw. The software rasterizer reads it directly and
// the Vulkan backend packs it into a UBO; fields must stay POD.
struct MaterialGpu {
    const Texture* albedo = nullptr;
    bool albedoSRGB = true;        // decode texture samples on the shader path
    Vec2 tiling{1, 1};
    Color color{1, 1, 1, 1};
    float specular = 0.5f;
    float shininess = 32.0f;
    float metallic = 0.0f;         // 0 dielectric, 1 metal
    float roughness = -1.0f;       // <0 derives from shininess
    float normalStrength = 1.0f;
    Color emissive{0, 0, 0, 1};    // decoded to LINEAR at fill time
    bool unlit = false;
    const Texture* normalMap = nullptr;
    bool pixelPerfect = false;
    bool blend = false;        // alpha blend instead of opaque overwrite
    bool spriteLighting = false;  // 2D lighting using normalMap / plain sprites
    int cullMode = 1;          // 0 off, 1 back faces, 2 front faces
    bool antialias = true;     // edge coverage antialiasing
    bool frontFacing = true;   // resolved per polygon in EmitPolygon (software)
    bool castShadows = false;
    bool receiveShadows = true;
    bool depthOnly = false;    // shadow pass: write depth, skip shading
    // 2D item (sprite/text quad) rasterized as one draw: its triangles may be
    // composited once per pixel even where they share the quad's diagonal.
    bool item2D = false;
};

struct GpuLight {
    int type = 0;         // 0 directional, 1 point, 2 spot
    Vec3 pos;
    Vec3 dir{0, -1, 0};   // normalized, points from light toward scene
    Color color{1, 1, 1};
    float intensity = 1.0f;
    float range = 15.0f;
    float cosOuter = -1.0f;  // spot cone
    float cosInner = -1.0f;
    bool castShadows = false;
    float shadowBias = 0.02f;
};

// One 2D vertex (sprite / text quads), pre-transformed to world space.
struct Vertex2D {
    Vec3 pos;
    Vec3 normal;
    Vec2 uv;
    Color color;
};

// The Vulkan backend's view of one frame. Built by Renderer::RenderScene; the
// backend owns no scene access, so both backends see identical inputs.
struct FrameDrawList {
    const CameraView* cam = nullptr;
    int width = 0, height = 0;
    Mat4 viewProj;
    Vec3 camPos;
    Vec3 camRight, camUp, camForward;
    bool ortho = false;
    float tanHalfFov = 1.0f, aspect = 1.0f, orthoSize = 5.0f;

    std::vector<GpuLight> lights;

    // Directional shadow cascades (already fitted by the shared CPU code).
    int cascadeCount = 0;
    int cascadeSize = 1024;
    Mat4 cascadeMatrix[kMaxCascades];
    float cascadeSplits[kMaxCascades] = {0, 0, 0, 0};
    float cascadeRange[kMaxCascades] = {1, 1, 1, 1};
    float cascadeTexel[kMaxCascades] = {0.01f, 0.01f, 0.01f, 0.01f};
    float shadowStrength = 0.85f;
    Vec3 shadowToLight{0, 1, 0};

    struct OpaqueDraw {
        std::shared_ptr<Mesh> mesh;   // keeps the mesh alive until submit
        Mat4 model;
        MaterialGpu mat;
    };
    std::vector<OpaqueDraw> opaque;

    struct ShadowCaster {
        std::shared_ptr<Mesh> mesh;
        Mat4 model;
        AABB bounds;   // world-space, for per-cascade culling
        int cullMode = 1;
    };
    std::vector<ShadowCaster> casters;

    struct Item2D {
        MaterialGpu mat;
        uint32_t firstVert = 0;
        uint32_t vertCount = 0;   // 6 per quad
    };
    std::vector<Vertex2D> verts2D;
    std::vector<Item2D> items2D;

    std::vector<DebugLine> lines;

    // Pins: textures referenced by materials must outlive the submit.
    std::vector<std::shared_ptr<Texture>> liveTextures;
};

// Conservative clip-cube overlap test for shadow casters. An interval test
// (unlike a per-corner point test) keeps large casters that straddle the
// volume without any corner in it, e.g. a floor spanning the whole slice.
inline bool IntersectsShadowVolume(const Mat4& lightViewProj, const AABB& box) {
    float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f, minZ = 1e30f, maxZ = -1e30f;
    for (int i = 0; i < 8; ++i) {
        const Vec3 corner((i & 1) ? box.max.x : box.min.x, (i & 2) ? box.max.y : box.min.y,
                          (i & 4) ? box.max.z : box.min.z);
        const Vec4 clip = lightViewProj * Vec4(corner, 1.0f);
        if (clip.w <= 1e-6f) continue;  // ortho projections keep w = 1
        const Vec3 ndc = clip.XYZ() / clip.w;
        minX = std::min(minX, ndc.x); maxX = std::max(maxX, ndc.x);
        minY = std::min(minY, ndc.y); maxY = std::max(maxY, ndc.y);
        minZ = std::min(minZ, ndc.z); maxZ = std::max(maxZ, ndc.z);
    }
    if (minX > maxX) return false;  // all corners behind the near plane
    return minX <= 1.0f && maxX >= -1.0f && minY <= 1.0f && maxY >= -1.0f &&
           minZ <= 1.0f && maxZ >= -1.0f;
}

}  // namespace Cairn
