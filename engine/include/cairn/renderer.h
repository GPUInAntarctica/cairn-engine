// Cairn Engine - renderers. Renders a Scene into a framebuffer (RGBA8 + depth)
// that the host blits. Two backends share one draw-list front end:
//   software - multithreaded CPU rasterizer (always available, deterministic);
//   vulkan   - offscreen GPU rendering via a dynamically loaded Vulkan loader
//              (optional at build time; falls back to software when the
//              machine has no Vulkan 1.3 driver).
// 3D path: PBR (GGX + Lambert), sky gradient, fog, cascaded shadow maps,
// debug lines. 2D path: alpha-blended sprites/text with optional normal maps.
#pragma once

#include "cairn/math.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace Cairn {

class Scene;

struct FrameTarget {
    int width = 0, height = 0;
    std::vector<uint8_t> color;  // RGBA8, row 0 = top
    std::vector<float> depth;    // [0, 1], 1 = far, sized w*h
};

// Display transform applied to shaded pixels (all math happens in linear
// space; these run once per shaded pixel before the RGBA8 store).
enum class Tonemap : int { None = 0, Reinhard = 1, ACES = 2, Filmic = 3 };

// Which renderer executes the frame. `Auto` prefers the Vulkan GPU backend
// and falls back to software when it is unavailable; the explicit values
// force one backend (an unavailable forced Vulkan degrades to software with
// a logged warning). RenderStats::backend always reports what actually ran.
enum class RenderBackend : int { Auto = 0, Vulkan = 1, Software = 2 };

// True when the build includes the Vulkan backend AND the runtime machine
// exposes a usable Vulkan 1.3 device. Cached after the first call.
bool VulkanBackendAvailable();

struct CameraView {
    Vec3 position;
    Mat4 view;
    Mat4 projection;
    bool orthographic = false;
    float fov = 60.0f;
    float nearPlane = 0.1f;
    float farPlane = 500.0f;
    float orthoSize = 5.0f;
    // Background & atmosphere (copied from Scene settings or the camera).
    Color clearColor{0.09f, 0.09f, 0.12f};
    bool skyEnabled = true;
    Color skyTop{0.18f, 0.20f, 0.28f};
    Color skyBottom{0.55f, 0.59f, 0.67f};
    bool fogEnabled = false;
    Color fogColor{0.62f, 0.66f, 0.72f};
    float fogDensity = 0.02f;
    Color ambient{0.26f, 0.28f, 0.33f};
    Tonemap tonemap = Tonemap::ACES;
    float exposure = 1.0f;
    // Animation clock handed to material shader programs (`time`). It follows
    // play time, so a screenshot of a stopped scene is reproducible and
    // runtime.step advances it.
    float time = 0.0f;
    // Directional shadow mapping: 0 disables, 1-4 cascades.
    int shadowCascades = 2;
    int shadowResolution = 1024;   // per cascade
    float shadowStrength = 0.85f;  // 0 = fully black shadows, 1 = unshadowed
    float shadowDistance = 60.0f;  // cascades cover this far, not the whole far plane
    // Post-processing on the display-referred sRGB frame, applied in the
    // order bloom -> color grading -> vignette -> FXAA. Neutral defaults keep
    // the framebuffer byte-identical to the un-post-processed render.
    float postVignette = 0.0f;         // 0..1 radial darkening strength
    float postVignetteRadius = 0.75f;  // 0..1 where darkening starts (half-height units)
    float postVignetteSoftness = 0.5f; // 0..1 falloff width past the radius
    float postContrast = 1.0f;         // 0..2 around the 0.5 display pivot, 1 = neutral
    float postSaturation = 1.0f;       // 0..2 around Rec.709 luma, 1 = neutral
    float postBloomStrength = 0.0f;    // 0..2 additive glow, 0 = off
    float postBloomThreshold = 0.85f;  // 0..1 bright-pass cutoff (display luma)
    bool postFxaa = false;             // luma-edge AA over the final image
    int textureMemoryMB = 256;         // texture cache budget (LRU above it)
    // Backend selection and MSAA sample count (0 = off; 2/4/8 are snapped to
    // what the GPU supports). MSAA only affects the Vulkan backend; the
    // software rasterizer always uses per-pixel coverage antialiasing.
    RenderBackend backend = RenderBackend::Auto;
    int msaa = 4;
};

struct RenderStats {
    int drawCalls = 0;    // rasterized triangles + 2D items + one sky pass
    int triangles = 0;    // triangles that survived clipping
    int meshesSeen = 0;   // mesh renderers submitted (after culling)
    int clipRejected = 0; // triangles fully clipped away (diagnostic)
    int frustumCulled = 0;  // mesh renderers skipped by frustum culling
    int drawCalls2D = 0;    // sprites + text items drawn
    int lightCount = 0;     // lights collected this frame
    int shadowedPixels = 0; // pixels darkened by the shadow map (software only)
    int postPasses = 0;     // post-processing effects applied this frame (0-4)
    const char* backend = "software";  // backend that actually ran this frame
    // Which camera produced the frame: "scene" (the scene's MainCamera),
    // "editor-fallback" (a game frame in a scene with no camera) or "editor".
    const char* camera = "editor";
};

// The six planes of a view frustum, extracted from a view-projection matrix.
// Planes point inwards; `Distance()` is positive inside the frustum.
struct Frustum {
    Vec4 planes[6];

    static Frustum FromViewProj(const Mat4& viewProj);
    // Conservative AABB test: false when the box is entirely outside.
    bool IntersectsAABB(const AABB& box) const;
};

struct DebugLine {
    Vec3 a, b;
    Color color;
};

class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void Resize(int w, int h);
    FrameTarget& target() { return target_; }
    const FrameTarget& target() const { return target_; }

    // Renders a full frame. `overlayLines` draws depth-tested lines (editor
    // grid, selection outline, Debug::DrawLine output).
    RenderStats RenderScene(Scene& scene, const CameraView& cam,
                            const std::vector<DebugLine>& overlayLines);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    FrameTarget target_;
};

}  // namespace Cairn
