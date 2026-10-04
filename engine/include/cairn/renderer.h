// Cairn Engine - multithreaded software rasterizer.
// Renders a Scene into a CPU framebuffer (RGBA8 + depth) that the host blits.
// 3D path: per-pixel Blinn-Phong, sky gradient, fog, debug lines.
// 2D path: alpha-blended sprites/text with optional tangent-space normal maps.
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
};

struct RenderStats {
    int drawCalls = 0;
    int triangles = 0;
    int meshesSeen = 0;   // diagnostic
    int clipRejected = 0; // diagnostic
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
