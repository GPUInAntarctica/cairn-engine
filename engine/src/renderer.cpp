#include "cairn/renderer.h"

#include "cairn/engine.h"
#include "cairn/log.h"
#include "backend_vulkan.h"
#include "font_internal.h"
#include "gpu_types.h"
#include "scene_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace Cairn {

// ---------------------------------------------------------------------------
// Small persistent worker pool for band-parallel rasterization.
// ---------------------------------------------------------------------------
class JobPool {
public:
    void Start(int n) {
        std::lock_guard<std::mutex> lock(m_);
        for (int i = 0; i < std::max(1, n); ++i)
            threads_.emplace_back([this] { WorkerLoop(); });
    }
    void Stop() {
        {
            std::lock_guard<std::mutex> lock(m_);
            stop_ = true;
        }
        wake_.notify_all();
        for (auto& t : threads_) if (t.joinable()) t.join();
        threads_.clear();
    }
    // Runs f(0..count-1) across the pool and blocks until done. The calling
    // thread participates in the work.
    void ParallelFor(int count, const std::function<void(int)>& f) {
        if (count <= 0) return;
        if (threads_.empty()) {
            for (int i = 0; i < count; ++i) f(i);
            return;
        }
        // task_/jobCount_ are published atomically: a worker that is still
        // finishing the previous epoch can never observe a half-written
        // dispatch for the next one.
        task_.store(&f, std::memory_order_relaxed);
        jobCount_.store(count, std::memory_order_relaxed);
        next_.store(0, std::memory_order_relaxed);
        pending_.store(count, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(m_);
            epoch_++;
        }
        wake_.notify_all();
        Work();
        std::unique_lock<std::mutex> lock(m_);
        done_.wait(lock, [this] { return pending_.load(std::memory_order_acquire) == 0; });
    }

    int workerCount() const { return (int)threads_.size() + 1; }

private:
    void Work() {
        const auto* task = task_.load(std::memory_order_acquire);
        const int count = jobCount_.load(std::memory_order_acquire);
        while (true) {
            int i = next_.fetch_add(1, std::memory_order_relaxed);
            if (i >= count) break;
            (*task)(i);
            if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                std::lock_guard<std::mutex> lock(m_);
                done_.notify_all();
            }
        }
    }
    void WorkerLoop() {
        uint64_t seen = 0;
        while (true) {
            {
                std::unique_lock<std::mutex> lock(m_);
                wake_.wait(lock, [this, &seen] { return stop_ || epoch_ != seen; });
                if (stop_) return;
                seen = epoch_;
            }
            Work();
        }
    }

    std::vector<std::thread> threads_;
    std::mutex m_;
    std::condition_variable wake_, done_;
    std::atomic<const std::function<void(int)>*> task_{nullptr};
    std::atomic<int> jobCount_{0};
    std::atomic<int> next_{0};
    std::atomic<int> pending_{0};
    uint64_t epoch_ = 0;
    bool stop_ = false;
};

// ---------------------------------------------------------------------------
// View frustum extraction (Gribb/Hartmann). Rows of the column-major
// view-projection matrix give the six inward-pointing clip half-spaces.
// ---------------------------------------------------------------------------
Frustum Frustum::FromViewProj(const Mat4& m) {
    const float* r = m.m;  // column-major: element (row, col) = m[col * 4 + row]
    Frustum f;
    // left, right, bottom, top, near, far
    const Vec4 raw[6] = {
        {r[3] + r[0], r[7] + r[4], r[11] + r[8], r[15] + r[12]},
        {r[3] - r[0], r[7] - r[4], r[11] - r[8], r[15] - r[12]},
        {r[3] + r[1], r[7] + r[5], r[11] + r[9], r[15] + r[13]},
        {r[3] - r[1], r[7] - r[5], r[11] - r[9], r[15] - r[13]},
        {r[3] + r[2], r[7] + r[6], r[11] + r[10], r[15] + r[14]},
        {r[3] - r[2], r[7] - r[6], r[11] - r[10], r[15] - r[14]},
    };
    for (int i = 0; i < 6; ++i) {
        float len = std::sqrt(raw[i].x * raw[i].x + raw[i].y * raw[i].y + raw[i].z * raw[i].z);
        if (len > 1e-6f) {
            float inv = 1.0f / len;
            f.planes[i] = Vec4(raw[i].x * inv, raw[i].y * inv, raw[i].z * inv, raw[i].w * inv);
        } else {
            f.planes[i] = raw[i];
        }
    }
    return f;
}

bool Frustum::IntersectsAABB(const AABB& box) const {
    // Positive vertex test: if the box centre is behind a plane, the whole box
    // is outside as long as its projected radius on that plane is smaller.
    const Vec3 c = box.Center();
    const Vec3 e = box.Extents();
    for (int i = 0; i < 6; ++i) {
        const Vec4& p = planes[i];
        float center = p.x * c.x + p.y * c.y + p.z * c.z + p.w;
        float radius = std::fabs(p.x) * e.x + std::fabs(p.y) * e.y + std::fabs(p.z) * e.z;
        if (center + radius < 0.0f) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Rasterization data.
// ---------------------------------------------------------------------------
struct RasterVert {
    float x = 0, y = 0;    // screen pixels
    float z = 0;           // depth [0,1]
    float invW = 1;
    Vec3 pw;               // worldPos / w
    Vec3 nw;               // normal / w
    Vec3 tw;               // tangent xyz / w
    float th = 0;          // handedness (not divided; 0 = no tangent frame)
    Vec2 tuv;              // uv / w
};

// Appends one world-space quad (six vertices) to the shared 2D list. Both
// backends consume the result verbatim, so the winding and the derived plane
// normal are decided here exactly once.
uint32_t AppendQuad(FrameDrawList* list, const Vec3 world[4], const Vec2 uv[4]);

struct ShadowSampler {
        bool enabled = false;
        float strength = 0.85f;
        int size = 1024;
        int count = 0;
        Vec3 dir{0, 1, 0};  // towards the primary light, for the slope-scaled bias
        const float* splits = nullptr;
        const float* ranges = nullptr;
        const float* texels = nullptr;
        const Mat4* matrices = nullptr;
        const std::vector<std::vector<float>>* maps = nullptr;
    };

struct Renderer::Impl {
    JobPool pool;
    std::vector<RasterVert> triVerts;     // 3 per triangle job
    std::vector<MaterialGpu> triMats;     // 1 per triangle job
    std::vector<GpuLight> lights;
    ShadowSampler shadows;  // read by workers
    std::atomic<int> shadowedPixels{0};
    Vec3 camPos;
    Vec3 camRight, camUp, camForward;
    Mat4 viewProj;
    Mat4 invertViewProj;
    bool ortho = false;
    float tanHalfFov = 1.0f, aspect = 1.0f, orthoSize = 5.0f;
    // Scene atmosphere snapshot (read by workers).
    Color ambient;
    Color ambientLinear;
    bool fogEnabled = false;
    Color fogLinear;      // fog color decoded to linear
    float fogDensity = 0.02f;
    Tonemap tonemap = Tonemap::ACES;
    float exposure = 1.0f;
    // Framebuffers.
    int W = 0, H = 0;
    std::vector<uint8_t>* color = nullptr;
    std::vector<float>* depth = nullptr;

    // Directional shadow cascades.
    static constexpr int kMaxCascades = Cairn::kMaxCascades;
    int cascadeCount = 0;
    int cascadeSize = 1024;
    float cascadeSplits[kMaxCascades] = {0.0f, 0.0f, 0.0f, 0.0f};
    Mat4 cascadeMatrix[kMaxCascades];
    float cascadeRange[kMaxCascades] = {1.0f, 1.0f, 1.0f, 1.0f};
    float cascadeTexel[kMaxCascades] = {0.01f, 0.01f, 0.01f, 0.01f};
    std::vector<std::vector<float>> cascadeDepth;
    std::vector<uint8_t> shadowScratch;   // unused color target for the depth pass
    // Post-processing scratch (ping/pong), reused across frames: half res for
    // the bloom chain, full res when FXAA needs a source copy.
    std::vector<uint8_t> scratchA, scratchB;
    // Textures referenced by the current frame's materials. MaterialGpu holds
    // raw pointers; these shared_ptrs keep the objects alive against LRU
    // eviction until the frame has finished rasterizing.
    std::vector<std::shared_ptr<Texture>> liveTextures;
    float shadowStrength = 0.85f;
    Vec3 shadowToLight{0, 1, 0};    // towards the shadow-casting directional light
    // Eye of each fitted cascade; the depth pass culls back faces against it.
    Vec3 cascadeEye[kMaxCascades];
    // Shared draw list for the frame plus the lazily created GPU backend. The
    // Vulkan backend is only instantiated when a scene actually asks for it.
    FrameDrawList drawList;
    std::unique_ptr<VulkanBackend> vulkan;
    bool vulkanProbed = false;
    // World-space bounds of every shadow caster this frame, gathered once so
    // the cascade fits can extend toward geometry outside the view slice.
    std::vector<AABB> casterBounds;

    Impl() {
        int hw = (int)std::thread::hardware_concurrency();
        pool.Start(std::clamp(hw <= 0 ? 4 : hw, 1, 16));
    }
    ~Impl() { pool.Stop(); }

    // ------------------------------------------------------------------
    void ClearAndSky(const CameraView& cam) {
        const int n = W * H;
        float* d = depth->data();
        std::fill(d, d + n, 1.0f);
        uint8_t* px = color->data();
        if (!cam.skyEnabled) {
            uint8_t r = (uint8_t)std::clamp(cam.clearColor.r * 255.0f, 0.0f, 255.0f);
            uint8_t g = (uint8_t)std::clamp(cam.clearColor.g * 255.0f, 0.0f, 255.0f);
            uint8_t b = (uint8_t)std::clamp(cam.clearColor.b * 255.0f, 0.0f, 255.0f);
            for (int i = 0; i < n; ++i) {
                px[i * 4] = r; px[i * 4 + 1] = g; px[i * 4 + 2] = b; px[i * 4 + 3] = 255;
            }
            return;
        }
        // Vertical gradient by view ray elevation.
        for (int y = 0; y < H; ++y) {
            float ndcY = 1.0f - 2.0f * ((y + 0.5f) / H);  // up positive
            float tY;
            if (ortho) {
                float worldY = camPos.y + camUp.y * ndcY * orthoSize + camRight.y * 0.0f;
                // For ortho, elevation varies across rows only via camUp.
                float rayY = (camUp.y * ndcY) + camForward.y;
                (void)worldY;
                tY = rayY;
            } else {
                Vec3 ray = camForward + camUp * (ndcY * tanHalfFov);
                tY = ray.y;
            }
            float t = std::clamp((tY + 0.35f) * 1.5f, 0.0f, 1.0f);
            t = std::pow(t, 0.85f);
            Color c = Color::Lerp(cam.skyBottom, cam.skyTop, t);
            uint8_t r = (uint8_t)(c.r * 255.0f), g = (uint8_t)(c.g * 255.0f), b = (uint8_t)(c.b * 255.0f);
            for (int x = 0; x < W; ++x) {
                size_t i = ((size_t)y * W + x) * 4;
                px[i] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = 255;
            }
        }
    }

    // ------------------------------------------------------------------
    // Clip-space handling.
    struct ClipVert {
        Vec4 clip;
        Vec3 world;
        Vec3 normal;
        Vec4 tangent;      // xyz tangent, w handedness
        Vec2 uv;
    };

    static ClipVert ToClip(const Mat4& viewProj, const Mat4& world, const MeshVertex& v) {
        ClipVert out;
        out.world = world.TransformPoint(v.position);
        out.clip = viewProj * Vec4(out.world, 1.0f);
        out.normal = world.TransformDirection(v.normal);
        // Tangent transforms like a direction; handedness rides along untouched.
        out.tangent = Vec4(world.TransformDirection(v.tangent.XYZ()), v.tangent.w);
        out.uv = v.uv;
        return out;
    }

    // Clips the polygon against the six view-volume planes; appends the result
    // to `out`. Returns false when the polygon is entirely clipped.
    static bool ClipPolygon(const ClipVert* in, int count, std::vector<ClipVert>& out) {
        // Fast path: entirely inside (no clipping needed).
        bool allIn = true;
        for (int i = 0; i < count; ++i) {
            const Vec4& c = in[i].clip;
            if (c.w <= 1e-6f || std::fabs(c.x) > c.w || std::fabs(c.y) > c.w || std::fabs(c.z) > c.w) {
                allIn = false;
                break;
            }
        }
        if (allIn) {
            out.insert(out.end(), in, in + count);
            return true;
        }
        // Sutherland-Hodgman against x/y/z planes; w handled separately (near).
        struct Plane { int axis; float sign; };
        static const Plane planes[7] = {
            {3, 1},    // w > eps (near clip)
            {0, -1}, {0, 1}, {1, -1}, {1, 1}, {2, -1}, {2, 1},
        };
        std::vector<ClipVert> poly(in, in + count);
        std::vector<ClipVert> tmp;
        for (const Plane& pl : planes) {
            if (poly.size() < 3) break;
            tmp.clear();
            auto inside = [&](const ClipVert& v) {
                float c = v.clip[pl.axis];
                if (pl.axis == 3) return c > 1e-6f;  // near plane on w
                float w = v.clip.w;
                return pl.sign > 0 ? c < w : -c < w;
            };
            size_t n = poly.size();
            for (size_t i = 0; i < n; ++i) {
                const ClipVert& cur = poly[i];
                const ClipVert& nxt = poly[(i + 1) % n];
                bool curIn = inside(cur), nxtIn = inside(nxt);
                if (curIn) tmp.push_back(cur);
                if (curIn != nxtIn) {
                    float t;
                    if (pl.axis == 3) {
                        float den = nxt.clip.w - cur.clip.w;
                        t = std::fabs(den) > 1e-9f ? (1e-6f - cur.clip.w) / den : 0.5f;
                    } else {
                        // Solve cur + t*(nxt-cur) where axis == sign * w.
                        float a = cur.clip[pl.axis], wa = cur.clip.w;
                        float b = nxt.clip[pl.axis], wb = nxt.clip.w;
                        float den = (b - pl.sign * wb) - (a - pl.sign * wa);
                        t = std::fabs(den) > 1e-9f ? (pl.sign * wa - a) / den : 0.5f;
                    }
                    t = std::clamp(t, 0.0f, 1.0f);
                    ClipVert v;
                    v.clip = cur.clip + (nxt.clip - cur.clip) * t;
                    v.world = cur.world + (nxt.world - cur.world) * t;
                    v.normal = cur.normal + (nxt.normal - cur.normal) * t;
                    v.tangent = cur.tangent + (nxt.tangent - cur.tangent) * t;
                    v.uv = cur.uv + (nxt.uv - cur.uv) * t;
                    tmp.push_back(std::move(v));
                }
            }
            poly.swap(tmp);
        }
        if (poly.size() < 3) return false;
        out.insert(out.end(), poly.begin(), poly.end());
        return true;
    }

    // Appends screen-space triangle jobs for a clipped polygon (fan).
    void EmitPolygon(const std::vector<ClipVert>& poly) {
        if (poly.size() < 3) return;
        // Newell's method gives the polygon's plane normal; using it for every
        // fan triangle keeps back-face culling consistent for clipped polygons
        // (a fan sliver's own normal can be degenerate or flipped).
        Vec3 polyNormal;
        for (size_t i = 0; i < poly.size(); ++i) {
            const Vec3& cur = poly[i].world;
            const Vec3& nxt = poly[(i + 1) % poly.size()].world;
            polyNormal += Vec3::Cross(cur, nxt);
        }
        Vec3 centroid;
        for (const ClipVert& v : poly) centroid += v.world;
        centroid = centroid / static_cast<float>(poly.size());
        const bool frontFacing = Vec3::Dot(polyNormal, camPos - centroid) > 0.0f;
        for (size_t i = 1; i + 1 < poly.size(); ++i) {
            RasterVert rv[3];
            const ClipVert* tri[3] = {&poly[0], &poly[i], &poly[i + 1]};
            bool ok = true;
            for (int k = 0; k < 3; ++k) {
                const Vec4& c = tri[k]->clip;
                if (std::fabs(c.w) < 1e-9f) { ok = false; break; }
                float invW = 1.0f / c.w;
                float ndcX = c.x * invW, ndcY = c.y * invW, ndcZ = c.z * invW;
                rv[k].x = (ndcX * 0.5f + 0.5f) * W;
                rv[k].y = (1.0f - (ndcY * 0.5f + 0.5f)) * H;
                rv[k].z = std::clamp(ndcZ * 0.5f + 0.5f, 0.0f, 1.0f);
                rv[k].invW = invW;
                rv[k].pw = tri[k]->world * invW;
                rv[k].nw = tri[k]->normal * invW;
                rv[k].tw = tri[k]->tangent.XYZ() * invW;
                rv[k].th = tri[k]->tangent.w;
                rv[k].tuv = tri[k]->uv * invW;
            }
            if (!ok) continue;
            for (auto& v : rv) triVerts.push_back(v);
            // Material appended by caller (EmitMesh/EmitSprite set it before/after).
            MaterialGpu mat = pendingMat;
            mat.frontFacing = frontFacing;
            triMats.push_back(mat);
        }
    }

    MaterialGpu pendingMat;

    // Transforms + clips + emits one mesh. Vertices are transformed once per
    // mesh (previously once per triangle corner, i.e. 3x the work for shared
    // vertices) into a scratch buffer that is reused across meshes.
    void EmitMesh(const Mesh& mesh, const Mat4& world, const MaterialGpu& mat) {
        pendingMat = mat;
        std::vector<ClipVert>& poly = polyScratch;
        std::vector<ClipVert>& clipped = clipScratch;
        const auto& idx = mesh.indices;
        if (mesh.vertices.empty()) return;
        xformScratch.resize(mesh.vertices.size());
        for (size_t v = 0; v < mesh.vertices.size(); ++v) {
            xformScratch[v] = ToClip(viewProj, world, mesh.vertices[v]);
        }
        for (size_t i = 0; i + 2 < idx.size(); i += 3) {
            ClipVert cv[3] = {xformScratch[idx[i]], xformScratch[idx[i + 1]], xformScratch[idx[i + 2]]};
            poly.clear();
            clipped.clear();
            if (!ClipPolygon(cv, 3, clipped)) continue;
            EmitPolygon(clipped);
        }
    }

    std::vector<ClipVert> xformScratch, polyScratch, clipScratch;

    // Emits one quad (two triangles) from a shared 2D vertex range. The GPU
    // backend uploads the same vertices verbatim, so the software path only has
    // to project and clip them.
    void EmitQuadVerts(const Vertex2D* q, const MaterialGpu& mat) {
        pendingMat = mat;
        ClipVert cv[4];
        for (int i = 0; i < 4; ++i) {
            cv[i].world = q[i].pos;
            cv[i].clip = viewProj * Vec4(q[i].pos, 1.0f);
            cv[i].normal = q[i].normal;
            cv[i].uv = q[i].uv;
        }
        static const int quads[2][3] = {{0, 1, 2}, {0, 2, 3}};
        std::vector<ClipVert> clipped;
        for (int t = 0; t < 2; ++t) {
            ClipVert tri[3] = {cv[quads[t][0]], cv[quads[t][1]], cv[quads[t][2]]};
            clipped.clear();
            if (!ClipPolygon(tri, 3, clipped)) continue;
            EmitPolygon(clipped);
        }
    }

    // ------------------------------------------------------------------
    // Display transform: linear HDR -> tonemapped -> sRGB bytes.
    static float ToneMapChannel(float x, Tonemap mode) {
        switch (mode) {
            case Tonemap::None:
                return x;
            case Tonemap::Reinhard:
                return x / (1.0f + x);
            case Tonemap::Filmic: {
                // Uncharted 2 "Hable" curve, normalized to [0,1].
                x = std::max(x, 0.0f);
                const float A = 0.15f, B = 0.50f, C = 0.10f, D = 0.20f, E = 0.02f, F = 0.30f;
                return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
            }
            case Tonemap::ACES:
            default: {
                // Narkowicz ACES fit.
                x = std::max(x, 0.0f);
                const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
                return std::clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0f, 1.0f);
            }
        }
    }

    // Direction: light->surface vector, plus range and cone attenuation.
    // Returns false when the light does not reach the point at all.
    static bool LightFalloff(const GpuLight& li, const Vec3& P, Vec3* outL, float* outAtten) {
        if (li.type == 0) {  // directional
            *outL = -li.dir;
            *outAtten = 1.0f;
            return true;
        }
        Vec3 ld = li.pos - P;
        const float dist = ld.Length();
        if (dist > li.range || dist < 1e-5f) return false;
        *outL = ld / dist;
        float atten = 1.0f - dist / li.range;
        atten *= atten;
        if (li.type == 2) {  // spot cone
            const float cosAngle = Vec3::Dot(*outL, -li.dir);
            if (cosAngle <= li.cosOuter) return false;
            if (li.cosInner > li.cosOuter) {
                const float t = std::clamp((cosAngle - li.cosOuter) / (li.cosInner - li.cosOuter), 0.0f, 1.0f);
                atten *= t * t * (3.0f - 2.0f * t);  // smoothstep between the cones
            } else {
                atten *= std::clamp((cosAngle - li.cosOuter) / 1e-4f, 0.0f, 1.0f);
            }
        }
        *outAtten = atten;
        return true;
    }

    // Resolves the mesh a MeshRenderer points at (primitive or OBJ).
    std::shared_ptr<Mesh> ResolveMesh(const MeshRenderer* mr) {
        if (mr->mesh == MeshRenderer::MeshSource::Custom && !mr->meshPath.empty()) {
            return EngineMeshLibrary().GetObj(EngineResolvePath(mr->meshPath));
        }
        return EngineMeshLibrary().Get((PrimitiveMesh)(int)mr->mesh);
    }

    // ------------------------------------------------------------------
    // Directional shadows: N cascades fitted to the view frustum, sampled with
    // a 3x3 percentage-closer filter and a slope-scaled bias.

    // Builds the cascade split distances in view space (practical split scheme,
    // which keeps small near geometry sharp). out[i] is the FAR boundary of
    // cascade i: cascade 0 covers [nearZ, out[0]], cascade i covers
    // [out[i-1], out[i]] — SampleShadow picks the first i with
    // viewDepth <= out[i], so the boundaries must line up with FitCascade.
    static void ComputeCascadeSplits(int count, float nearZ, float farZ, float* out) {
        for (int i = 1; i <= count; ++i) {
            const float t = (float)i / (float)count;
            const float logSplit = nearZ * std::pow(farZ / nearZ, t);
            const float uniform = nearZ + (farZ - nearZ) * t;
            out[i - 1] = (i == count) ? farZ : 0.5f * (logSplit + uniform);  // blend
            // Keep the boundaries strictly increasing even for tiny ranges.
            if (i > 1 && out[i - 1] <= out[i - 2]) out[i - 1] = out[i - 2] + 0.01f * (farZ - nearZ);
        }
    }

    // Fits an orthographic light-space box around the cascade's frustum slice,
    // extended toward actual shadow casters, and snaps the lateral center to
    // the shadow texel grid so shadows do not shimmer when the camera moves.
    // The depth range is kept TIGHT (slice + casters + fixed world margins):
    // a world-space depth bias is only meaningful when the ortho depth span
    // matches the geometry's actual light-space extent.
    float lastCascadeRange = 1.0f;
    float lastCascadeTexel = 0.01f;  // world units per shadow texel
    Vec3 lightEye{0, 0, 0};          // eye of the last fit; the depth pass culls faces against it

    Mat4 FitCascade(const GpuLight& light, const Mat4& view, float nearDist, float farDist,
                    int cascadeSize, const std::vector<AABB>* casters) {
        // Frustum slice corners: constructed in VIEW space (the shape of a
        // perspective slice is depth-independent apart from the scale), then
        // lifted to world space with the camera's view matrix. Orthographic
        // cameras keep a constant cross-section instead of scaling with depth.
        const float hx = ortho ? orthoSize * aspect : tanHalfFov * aspect;
        const float hy = ortho ? orthoSize : tanHalfFov;
        std::vector<Vec3> corners;
        corners.reserve(8);
        const float zs[2] = {nearDist, farDist};
        for (float z : zs) {
            for (float y : {-hy, hy}) {
                for (float x : {-hx, hx}) {
                    const Vec3 viewPoint = ortho ? Vec3(x, y, -z) : Vec3(x * z, y * z, -z);
                    corners.push_back((view * Vec4(viewPoint, 1.0f)).XYZ());
                }
            }
        }
        AABB bounds;
        for (const Vec3& c : corners) bounds.Expand(c);

        Vec3 lightDir = light.dir.Normalized();
        if (lightDir.LengthSq() < 1e-9f) lightDir = Vec3(0, -1, 0);
        Vec3 up = std::fabs(lightDir.y) > 0.95f ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
        Vec3 right = Vec3::Cross(up, lightDir).Normalized();
        Vec3 realUp = Vec3::Cross(lightDir, right);

        // Project the slice onto the light basis to get the required extent.
        float minR = 1e30f, maxR = -1e30f, minU = 1e30f, maxU = -1e30f;
        float minD = 1e30f, maxD = -1e30f;
        for (const Vec3& c : corners) {
            const Vec3 d = c - bounds.Center();
            const float r = Vec3::Dot(d, right), u = Vec3::Dot(d, realUp), dd = Vec3::Dot(d, lightDir);
            minR = std::min(minR, r); maxR = std::max(maxR, r);
            minU = std::min(minU, u); maxU = std::max(maxU, u);
            minD = std::min(minD, dd); maxD = std::max(maxD, dd);
        }
        // Extend the fit toward real casters so geometry outside the slice
        // (a tall tower, a wall beside the view) still occludes receivers.
        // The extension is bounded to keep texel size and depth precision sane.
        if (casters && !casters->empty()) {
            float cMinR = 1e30f, cMaxR = -1e30f, cMinU = 1e30f, cMaxU = -1e30f;
            float cMinD = 1e30f, cMaxD = -1e30f;
            for (const AABB& b : *casters) {
                for (int i = 0; i < 8; ++i) {
                    const Vec3 corner((i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y,
                                      (i & 4) ? b.max.z : b.min.z);
                    const Vec3 d = corner - bounds.Center();
                    cMinR = std::min(cMinR, Vec3::Dot(d, right));
                    cMaxR = std::max(cMaxR, Vec3::Dot(d, right));
                    cMinU = std::min(cMinU, Vec3::Dot(d, realUp));
                    cMaxU = std::max(cMaxU, Vec3::Dot(d, realUp));
                    cMinD = std::min(cMinD, Vec3::Dot(d, lightDir));
                    cMaxD = std::max(cMaxD, Vec3::Dot(d, lightDir));
                }
            }
            const float spanR = std::max(0.0f, maxR - minR);
            const float spanU = std::max(0.0f, maxU - minU);
            const float spanD = std::max(0.0f, maxD - minD);
            minR = std::min(minR, std::max(cMinR, minR - 0.5f * spanR));
            maxR = std::max(maxR, std::min(cMaxR, maxR + 0.5f * spanR));
            minU = std::min(minU, std::max(cMinU, minU - 0.5f * spanU));
            maxU = std::max(maxU, std::min(cMaxU, maxU + 0.5f * spanU));
            minD = std::min(minD, std::max(cMinD, minD - spanD));
            maxD = std::max(maxD, std::min(cMaxD, maxD + spanD));
        }
        const float centerR = (minR + maxR) * 0.5f, centerU = (minU + maxU) * 0.5f;
        const float radius = std::max(maxR - minR, maxU - minU) * 0.5f + 0.5f;
        // Texel snapping keeps the shadow stable under camera motion. The snap
        // moves the center laterally only, so light-space depths are unchanged.
        const float texelWorld = (radius * 2.0f) / (float)cascadeSize;
        const float snappedR = std::round(centerR / texelWorld) * texelWorld;
        const float snappedU = std::round(centerU / texelWorld) * texelWorld;

        const Vec3 center = bounds.Center() + right * (snappedR - centerR) + realUp * (snappedU - centerU);
        // Tight depth range: the eye sits `front` world units before the
        // nearest content, and the far plane one unit behind the farthest.
        const float front = 1.0f, back = 1.0f;
        const float dSpan = std::max(0.0f, maxD - minD);
        lightEye = center - lightDir * (front - minD);
        Mat4 lightView = Mat4::LookAt(lightEye, lightEye + lightDir, realUp);
        const float range = front + dSpan + back;
        Mat4 lightProj = Mat4::Ortho(-radius, radius, -radius, radius, 0.01f, range);
        lastCascadeRange = range;
        lastCascadeTexel = texelWorld;
        return lightProj * lightView;
    }

    // Fits every cascade to its view-frustum slice. Shared by both backends:
    // only the depth rendering afterwards differs. Results land in the Impl
    // fields the software sampler reads and are mirrored into `list` for the
    // GPU backend, which rasterizes the maps itself.
    void FitCascadeSet(FrameDrawList& list, const CameraView& cam) {
        cascadeCount = std::clamp(cam.shadowCascades, 0, (int)kMaxCascades);
        cascadeSize = std::clamp(cam.shadowResolution, 256, 4096);
        shadowStrength = std::clamp(cam.shadowStrength, 0.0f, 1.0f);
        list.cascadeCount = cascadeCount;
        list.cascadeSize = cascadeSize;
        list.shadowStrength = shadowStrength;
        if (cascadeCount <= 0) return;

        const GpuLight* sun = nullptr;
        for (const GpuLight& li : list.lights) {
            if (li.type == 0 && li.castShadows) {
                sun = &li;
                break;
            }
        }
        if (!sun) {
            // Cascades configured but no directional light casts this frame:
            // keep the sampler off so stale maps can never darken pixels.
            cascadeCount = 0;
            list.cascadeCount = 0;
            return;
        }
        const GpuLight& light = *sun;
        const float nearZ = std::max(cam.nearPlane, 0.05f);
        // Cascades only cover the shadow distance: including the whole far
        // plane would stretch the depth range and destroy precision.
        const float farZ = cam.orthographic
                               ? cam.orthoSize * 4.0f
                               : std::min(cam.farPlane, std::max(cam.shadowDistance, nearZ + 1.0f));
        ComputeCascadeSplits(cascadeCount, nearZ, farZ, cascadeSplits);
        // Direction towards the light, for the slope-scaled depth bias.
        shadowToLight = -light.dir.Normalized();
        if (shadowToLight.LengthSq() < 1e-9f) shadowToLight = Vec3(0, 1, 0);
        list.shadowToLight = shadowToLight;

        for (int c = 0; c < cascadeCount; ++c) {
            // Cascade c covers [splits[c-1], splits[c]] - the same slice
            // SampleShadow selects with viewDepth <= splits[c].
            const float sliceNear = (c == 0) ? nearZ : cascadeSplits[c - 1];
            cascadeMatrix[c] = FitCascade(light, cam.view, sliceNear, cascadeSplits[c], cascadeSize,
                                          &casterBounds);
            cascadeRange[c] = std::max(1.0f, lastCascadeRange);
            cascadeTexel[c] = std::max(1e-4f, lastCascadeTexel);
            cascadeEye[c] = lightEye;
            list.cascadeMatrix[c] = cascadeMatrix[c];
            list.cascadeSplits[c] = cascadeSplits[c];
            list.cascadeRange[c] = cascadeRange[c];
            list.cascadeTexel[c] = cascadeTexel[c];
        }
    }

    // Software depth pass: rasterizes the fitted cascades into CPU float maps.
    void RenderShadowPassSoftware(const FrameDrawList& list) {
        if (cascadeCount <= 0) return;
        // Save the main frame state so the depth-only pass can reuse the
        // rasterizer with a different target.
        std::vector<uint8_t>* savedColor = color;
        std::vector<float>* savedDepth = depth;
        const int savedW = W, savedH = H;
        Mat4 savedViewProj = viewProj;
        Vec3 savedCamPos = camPos;

        invertViewProj = viewProj.Inverted();
        for (int c = 0; c < cascadeCount; ++c) {
            cascadeDepth[c].assign((size_t)cascadeSize * cascadeSize, 1.0f);
            W = H = cascadeSize;
            color = &shadowScratch;
            depth = &cascadeDepth[c];
            viewProj = cascadeMatrix[c];
            // Face culling inside EmitPolygon uses camPos: it must sit on the
            // light's side. The light node's own position is arbitrary for a
            // directional light, so use the fitted light-space eye instead.
            camPos = cascadeEye[c];

            triVerts.clear();
            triMats.clear();
            for (const auto& caster : list.casters) {
                if (!caster.mesh) continue;
                if (!IntersectsShadowVolume(cascadeMatrix[c], caster.bounds)) continue;
                MaterialGpu mat;
                mat.cullMode = caster.cullMode;
                mat.castShadows = true;
                mat.depthOnly = true;
                EmitMesh(*caster.mesh, caster.model, mat);
            }
            const int jobs = (int)(triVerts.size() / 3);
            RasterizeParallel(0, jobs);
        }

        color = savedColor;
        depth = savedDepth;
        W = savedW;
        H = savedH;
        viewProj = savedViewProj;
        camPos = savedCamPos;
        triVerts.clear();
        triMats.clear();
    }

    // Conservative test: the box's projected corner hull must overlap the
    // [-1,1]^3 clip cube. An interval test (unlike a per-corner point test)
    // keeps large casters that straddle the volume without any corner in it,
    // e.g. a floor spanning the whole slice.
    bool IntersectsShadowVolume(const Mat4& lightViewProj, const AABB& box) const {
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

    ShadowSampler shadowSampler() const {
        ShadowSampler s;
        s.enabled = cascadeCount > 0;
        s.strength = shadowStrength;
        s.size = cascadeSize;
        s.count = cascadeCount;
        s.splits = cascadeSplits;
        s.ranges = cascadeRange;
        s.texels = cascadeTexel;
        s.matrices = cascadeMatrix;
        s.maps = &cascadeDepth;
        s.dir = shadowToLight;
        return s;
    }

    // 3x3 PCF with a slope-scaled bias; returns 1 (lit) outside the map.
    float SampleShadow(const ShadowSampler& s, const Vec3& worldPos, float viewDepth, float ndotl) const {
        if (!s.enabled || s.count <= 0) return 1.0f;
        // Cascade i covers the view-depth range ending at splits[i] — the same
        // slice FitCascade built cascade i's volume from.
        int cascade = s.count - 1;
        for (int i = 0; i < s.count; ++i) {
            if (viewDepth <= s.splits[i]) { cascade = i; break; }
        }
        const std::vector<float>& map = (*s.maps)[cascade];
        if (map.empty()) return 1.0f;
        const Mat4& m = s.matrices[cascade];
        Vec4 clip = m * Vec4(worldPos, 1.0f);
        if (clip.w <= 1e-6f) return 1.0f;
        const Vec3 ndc = clip.XYZ() / clip.w;
        if (std::fabs(ndc.x) > 1.0f || std::fabs(ndc.y) > 1.0f || ndc.z > 1.0f || ndc.z < -1.0f) return 1.0f;

        // The bias is authored in WORLD units — 1.5 texels plus a slope term
        // that grows as the surface grazes the light — and converted with the
        // cascade's orthographic world depth span, so it stays correct at any
        // scene scale and never scales with the (tight) cascade depth range.
        const float range = s.ranges ? s.ranges[cascade] : 1.0f;
        const float texelWorld = s.texels ? s.texels[cascade] : 0.01f;
        const float worldBias = texelWorld * (1.5f + 2.0f * (1.0f - std::clamp(ndotl, 0.0f, 1.0f)));
        const float bias = worldBias / std::max(1e-3f, range);
        const float refDepth = ndc.z * 0.5f + 0.5f;
        const float fx = (ndc.x * 0.5f + 0.5f) * (float)s.size;
        const float fy = (1.0f - (ndc.y * 0.5f + 0.5f)) * (float)s.size;
        const int ix = (int)fx, iy = (int)fy;
        int hits = 0, taps = 0;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int x = std::clamp(ix + dx, 0, s.size - 1);
                const int y = std::clamp(iy + dy, 0, s.size - 1);
                const float depth = map[(size_t)y * s.size + x];
                if (refDepth - bias <= depth) ++hits;
                ++taps;
            }
        }
        const float lit = (float)hits / (float)taps;
        return 1.0f - (1.0f - lit) * s.strength;
    }

    void EncodePixel(uint8_t* p, float r, float g, float b) const {
        const float ex = exposure;
        r = ToneMapChannel(r * ex, tonemap);
        g = ToneMapChannel(g * ex, tonemap);
        b = ToneMapChannel(b * ex, tonemap);
        p[0] = (uint8_t)std::clamp(LinearToSrgbFast(r) * 255.0f + 0.5f, 0.0f, 255.0f);
        p[1] = (uint8_t)std::clamp(LinearToSrgbFast(g) * 255.0f + 0.5f, 0.0f, 255.0f);
        p[2] = (uint8_t)std::clamp(LinearToSrgbFast(b) * 255.0f + 0.5f, 0.0f, 255.0f);
    }

    // ------------------------------------------------------------------
    // Post-processing: fullscreen effects over the display-referred sRGB
    // framebuffer, run single-threaded after all rasterization and overlays
    // (the band-parallel rasterizer owns rows only while it runs, so nothing
    // may touch `color` concurrently). Every effect is individually switchable
    // and neutral defaults skip the chain entirely, keeping the frame
    // byte-identical to the un-post-processed render.
    //
    // Color space: shading happens in linear light and EncodePixel tonemaps +
    // sRGB-encodes, so these passes read and write sRGB bytes. Bloom's bright
    // pass therefore thresholds DISPLAY luma (Rec.709 on the encoded bytes),
    // not linear radiance — a deliberate, consistent choice for a CPU chain.

    void FullscreenPass(const std::function<void(int x, int y, uint8_t* px)>& fn) {
        uint8_t* base = color->data();
        for (int y = 0; y < H; ++y) {
            uint8_t* row = base + (size_t)y * W * 4;
            for (int x = 0; x < W; ++x) fn(x, y, row + (size_t)x * 4);
        }
    }

    static float DisplayLuma(const uint8_t* px) {
        return (0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2]) / 255.0f;
    }

    // Bilinear RGBA read at a pixel-space position (pixel i covers [i, i+1),
    // center i + 0.5); edges clamp.
    static void SampleBilinearRGB(const std::vector<uint8_t>& img, int w, int h,
                                  float px, float py, float out[3]) {
        px -= 0.5f;
        py -= 0.5f;
        const int x0 = (int)std::floor(px), y0 = (int)std::floor(py);
        const float tx = px - (float)x0, ty = py - (float)y0;
        auto fetch = [&](int sx, int sy, uint8_t* c) {
            sx = std::clamp(sx, 0, w - 1);
            sy = std::clamp(sy, 0, h - 1);
            const uint8_t* p = img.data() + ((size_t)sy * w + sx) * 4;
            c[0] = p[0]; c[1] = p[1]; c[2] = p[2];
        };
        uint8_t c00[3], c10[3], c01[3], c11[3];
        fetch(x0, y0, c00); fetch(x0 + 1, y0, c10);
        fetch(x0, y0 + 1, c01); fetch(x0 + 1, y0 + 1, c11);
        for (int i = 0; i < 3; ++i) {
            const float top = c00[i] + (c10[i] - c00[i]) * tx;
            const float bot = c01[i] + (c11[i] - c01[i]) * tx;
            out[i] = top + (bot - top) * ty;
        }
    }

    // Bright-pass at half resolution + horizontal/vertical Gaussian ping-pong
    // through the two half-res scratch buffers, then an additive composite
    // (bilinearly upsampled) into the framebuffer.
    void ApplyBloom(float strength, float threshold) {
        const int hw = std::max(1, W / 2), hh = std::max(1, H / 2);
        scratchA.assign((size_t)hw * hh * 4, 0);
        scratchB.assign((size_t)hw * hh * 4, 0);
        static const float kWeights[5] = {0.227027f, 0.194594f, 0.121621f, 0.054054f, 0.016216f};
        // 1) Bright pass: 2x2 box downsample; keep pixels above the display
        //    luma threshold with a smooth knee so the cutoff doesn't alias.
        for (int hy = 0; hy < hh; ++hy) {
            for (int hx = 0; hx < hw; ++hx) {
                int r = 0, g = 0, b = 0;
                for (int dy = 0; dy < 2; ++dy) {
                    const int sy = std::min(hy * 2 + dy, H - 1);
                    for (int dx = 0; dx < 2; ++dx) {
                        const int sx = std::min(hx * 2 + dx, W - 1);
                        const uint8_t* p = color->data() + ((size_t)sy * W + sx) * 4;
                        r += p[0]; g += p[1]; b += p[2];
                    }
                }
                const float cr = r / (4.0f * 255.0f), cg = g / (4.0f * 255.0f), cb = b / (4.0f * 255.0f);
                const float luma = 0.2126f * cr + 0.7152f * cg + 0.0722f * cb;
                float k = (luma - threshold) / std::max(1e-3f, 1.0f - threshold);
                k = std::clamp(k, 0.0f, 1.0f);
                k = k * k * (3.0f - 2.0f * k);
                uint8_t* o = scratchA.data() + ((size_t)hy * hw + hx) * 4;
                o[0] = (uint8_t)std::clamp(cr * k * 255.0f + 0.5f, 0.0f, 255.0f);
                o[1] = (uint8_t)std::clamp(cg * k * 255.0f + 0.5f, 0.0f, 255.0f);
                o[2] = (uint8_t)std::clamp(cb * k * 255.0f + 0.5f, 0.0f, 255.0f);
                o[3] = 255;
            }
        }
        // 2) Separable blur: horizontal into scratchB, vertical back into A.
        auto blur = [&](const std::vector<uint8_t>& src, std::vector<uint8_t>& dst, bool vertical) {
            for (int y = 0; y < hh; ++y) {
                for (int x = 0; x < hw; ++x) {
                    float acc[3] = {0.0f, 0.0f, 0.0f};
                    for (int t = -4; t <= 4; ++t) {
                        const int sx = vertical ? x : std::clamp(x + t, 0, hw - 1);
                        const int sy = vertical ? std::clamp(y + t, 0, hh - 1) : y;
                        const uint8_t* p = src.data() + ((size_t)sy * hw + sx) * 4;
                        const float w = kWeights[std::abs(t)];
                        acc[0] += p[0] * w; acc[1] += p[1] * w; acc[2] += p[2] * w;
                    }
                    uint8_t* o = dst.data() + ((size_t)y * hw + x) * 4;
                    o[0] = (uint8_t)std::clamp(acc[0] + 0.5f, 0.0f, 255.0f);
                    o[1] = (uint8_t)std::clamp(acc[1] + 0.5f, 0.0f, 255.0f);
                    o[2] = (uint8_t)std::clamp(acc[2] + 0.5f, 0.0f, 255.0f);
                    o[3] = 255;
                }
            }
        };
        blur(scratchA, scratchB, false);
        blur(scratchB, scratchA, true);
        // 3) Composite: add the upsampled glow under `strength`.
        FullscreenPass([&](int x, int y, uint8_t* px) {
            const float fx = (x + 0.5f) * 0.5f - 0.5f;
            const float fy = (y + 0.5f) * 0.5f - 0.5f;
            float glow[3];
            SampleBilinearRGB(scratchA, hw, hh, fx, fy, glow);
            for (int c = 0; c < 3; ++c) {
                px[c] = (uint8_t)std::clamp(px[c] + glow[c] * strength + 0.5f, 0.0f, 255.0f);
            }
        });
    }

    // Contrast around the 0.5 display pivot, then saturation around Rec.709
    // luma; both in one pass.
    void ApplyColorGrading(float contrast, float saturation) {
        FullscreenPass([&](int, int, uint8_t* px) {
            float c[3] = {px[0] / 255.0f, px[1] / 255.0f, px[2] / 255.0f};
            for (int i = 0; i < 3; ++i) c[i] = (c[i] - 0.5f) * contrast + 0.5f;
            const float luma = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
            for (int i = 0; i < 3; ++i) {
                c[i] = luma + (c[i] - luma) * saturation;
                px[i] = (uint8_t)std::clamp(c[i] * 255.0f + 0.5f, 0.0f, 255.0f);
            }
        });
    }

    // Radial darkening: strength 0..1, radius where it starts, softness as the
    // falloff width past the radius. Distance is in half-height units and
    // aspect-corrected, so the falloff stays circular at any resolution.
    void ApplyVignette(float strength, float radius, float softness) {
        const float aspect = (float)W / (float)H;
        FullscreenPass([&](int x, int y, uint8_t* px) {
            const float dx = ((x + 0.5f) / W * 2.0f - 1.0f) * aspect;
            const float dy = (y + 0.5f) / H * 2.0f - 1.0f;
            const float d = std::sqrt(dx * dx + dy * dy);
            float t = std::clamp((d - radius) / std::max(1e-3f, softness), 0.0f, 1.0f);
            t = t * t * (3.0f - 2.0f * t);
            const float f = 1.0f - strength * t;
            for (int c = 0; c < 3; ++c) {
                px[c] = (uint8_t)std::clamp(px[c] * f + 0.5f, 0.0f, 255.0f);
            }
        });
    }

    // Console-style FXAA over the final LDR image: luma-edge detection on the
    // 3x3 neighborhood, then two bilinear taps along the edge-perpendicular
    // direction. Reads a full-res copy in scratchA, writes in place.
    void ApplyFxaa() {
        scratchA.assign(color->begin(), color->end());
        constexpr float kEdgeMin = 0.0312f;       // absolute luma range floor
        constexpr float kEdgeThreshold = 0.063f;  // relative to the local max
        constexpr float kReduceMin = 1.0f / 128.0f;
        constexpr float kReduceMul = 1.0f / 8.0f;
        constexpr float kSpanMax = 8.0f;
        FullscreenPass([&](int x, int y, uint8_t* px) {
            auto lumaAt = [&](int sx, int sy) {
                sx = std::clamp(sx, 0, W - 1);
                sy = std::clamp(sy, 0, H - 1);
                return DisplayLuma(scratchA.data() + ((size_t)sy * W + sx) * 4);
            };
            const float lM = lumaAt(x, y);
            const float lNW = lumaAt(x - 1, y - 1), lNE = lumaAt(x + 1, y - 1);
            const float lSW = lumaAt(x - 1, y + 1), lSE = lumaAt(x + 1, y + 1);
            const float lMin = std::min({lM, lNW, lNE, lSW, lSE});
            const float lMax = std::max({lM, lNW, lNE, lSW, lSE});
            if (lMax - lMin < std::max(kEdgeMin, lMax * kEdgeThreshold)) return;  // flat area
            float dirX = -((lNW + lNE) - (lSW + lSE));
            float dirY = ((lNW + lSW) - (lNE + lSE));
            const float dirReduce = std::max((lNW + lNE + lSW + lSE) * 0.25f * kReduceMul, kReduceMin);
            const float rcpDirMin = 1.0f / (std::min(std::fabs(dirX), std::fabs(dirY)) + dirReduce);
            dirX = std::clamp(dirX * rcpDirMin, -kSpanMax, kSpanMax);
            dirY = std::clamp(dirY * rcpDirMin, -kSpanMax, kSpanMax);
            float a[3], b[3];
            SampleBilinearRGB(scratchA, W, H, x + 0.5f + dirX * (1.0f / 3.0f - 0.5f),
                              y + 0.5f + dirY * (1.0f / 3.0f - 0.5f), a);
            SampleBilinearRGB(scratchA, W, H, x + 0.5f + dirX * (2.0f / 3.0f - 0.5f),
                              y + 0.5f + dirY * (2.0f / 3.0f - 0.5f), b);
            for (int c = 0; c < 3; ++c) {
                px[c] = (uint8_t)std::clamp((a[c] + b[c]) * 0.5f + 0.5f, 0.0f, 255.0f);
            }
        });
    }

    // Runs the chain in order (bloom -> grading -> vignette -> FXAA) and
    // returns how many effects were actually applied.
    int RunPostChain(const CameraView& cam) {
        const float bloomStrength = std::clamp(cam.postBloomStrength, 0.0f, 2.0f);
        const float contrast = std::clamp(cam.postContrast, 0.0f, 2.0f);
        const float saturation = std::clamp(cam.postSaturation, 0.0f, 2.0f);
        const float vignette = std::clamp(cam.postVignette, 0.0f, 1.0f);
        const bool bloom = bloomStrength > 1e-4f;
        const bool grading = std::fabs(contrast - 1.0f) > 1e-4f || std::fabs(saturation - 1.0f) > 1e-4f;
        const bool hasVignette = vignette > 1e-4f;
        if (!bloom && !grading && !hasVignette && !cam.postFxaa) return 0;  // pure passthrough
        int passes = 0;
        if (bloom) {
            ApplyBloom(bloomStrength, std::clamp(cam.postBloomThreshold, 0.0f, 1.0f));
            ++passes;
        }
        if (grading) {
            ApplyColorGrading(contrast, saturation);
            ++passes;
        }
        if (hasVignette) {
            ApplyVignette(vignette, std::clamp(cam.postVignetteRadius, 0.0f, 1.0f),
                          std::clamp(cam.postVignetteSoftness, 0.0f, 1.0f));
            ++passes;
        }
        if (cam.postFxaa) {
            ApplyFxaa();
            ++passes;
        }
        return passes;
    }

    // ------------------------------------------------------------------
    // Per-pixel shading.
    void ShadeAndStore(int x, int y, const RasterVert* v, const float l[3], const MaterialGpu& mat,
                       float lod, float coverage) {
        size_t px = ((size_t)y * W + x) * 4;
        float invW = l[0] * v[0].invW + l[1] * v[1].invW + l[2] * v[2].invW;
        if (invW <= 1e-9f) return;
        float w = 1.0f / invW;
        Vec3 P = (v[0].pw * l[0] + v[1].pw * l[1] + v[2].pw * l[2]) * w;
        Vec2 uv = (v[0].tuv * l[0] + v[1].tuv * l[1] + v[2].tuv * l[2]) * w;
        Vec3 V = (camPos - P);
        float viewDist = V.Length();
        if (viewDist > 1e-6f) V = V / viewDist;

        Texture::Wrap wrap = mat.pixelPerfect ? Texture::Wrap::Clamp : Texture::Wrap::Repeat;
        // Trilinear where a mip chain exists; pixel-perfect sprites stay crisp.
        Texture::Filter filter = mat.pixelPerfect
                                     ? Texture::Filter::Nearest
                                     : (mat.albedo && mat.albedo->hasMipmaps() ? Texture::Filter::Trilinear
                                                                               : Texture::Filter::Bilinear);

        // Material colors are authored in sRGB; lighting math runs in linear.
        float albedo[4] = {SrgbToLinearFast(mat.color.r), SrgbToLinearFast(mat.color.g),
                           SrgbToLinearFast(mat.color.b), mat.color.a};
        if (mat.albedo) {
            float tex[4];
            mat.albedo->SampleLod(uv.x * mat.tiling.x, uv.y * mat.tiling.y, lod, wrap, filter, tex);
            albedo[0] *= tex[0]; albedo[1] *= tex[1]; albedo[2] *= tex[2]; albedo[3] *= tex[3];
        }

        // Directional shadow lookup (once per pixel, reused by every light).
        float shadowFactor = 1.0f;
        if (shadows.enabled && mat.receiveShadows) {
            // (counted below once the factor is known)
            // Distance along the view direction (positive in front of the camera).
            const float viewDepth = Vec3::Dot(P - camPos, camForward);
            // Geometric normal (needed for the slope-scaled bias before the
            // lighting branches compute their own shading normal).
            Vec3 geomN = (v[0].nw * l[0] + v[1].nw * l[1] + v[2].nw * l[2]) * w;
            if (geomN.LengthSq() > 1e-9f) {
                geomN = geomN.Normalized();
            } else {
                geomN = Vec3(0, 1, 0);
            }
            const float ndotl = std::max(0.0f, Vec3::Dot(geomN, shadows.dir));
            shadowFactor = SampleShadow(shadows, P, viewDepth, ndotl);
            if (shadowFactor < 0.98f) shadowedPixels.fetch_add(1, std::memory_order_relaxed);
        }

        float r, g, b;
        if (mat.unlit) {
            r = albedo[0]; g = albedo[1]; b = albedo[2];
        } else if (mat.spriteLighting) {
            // 2D lighting: normal from the sprite plane or its normal map.
            Vec3 N(0, 0, -1);
            if (mat.normalMap) {
                float nm[4];
                mat.normalMap->SampleLod(uv.x, uv.y, lod, Texture::Wrap::Clamp,
                                         mat.normalMap->hasMipmaps() && !mat.pixelPerfect ? Texture::Filter::Trilinear
                                                                                         : filter,
                                         nm);
                N = Vec3(nm[0] * 2.0f - 1.0f, -(nm[1] * 2.0f - 1.0f), nm[2] * 2.0f - 1.0f).Normalized();
            } else {
                N = (v[0].nw * l[0] + v[1].nw * l[1] + v[2].nw * l[2]) * w;
                N = N.Normalized();
                if (N.LengthSq() < 1e-9f) N = Vec3(0, 0, -1);
            }
            float diffAcc = 1.0f;  // ambient term
            r = g = b = 0.0f;
            Color amb = ambient;
            r = albedo[0] * amb.r; g = albedo[1] * amb.g; b = albedo[2] * amb.b;
            for (const GpuLight& li : lights) {
                Vec3 L;
                float atten = 1.0f;
                if (!LightFalloff(li, P, &L, &atten)) continue;
                float d = Vec3::Dot(N, L);
                if (d <= 0.0f) continue;
                float s = li.intensity * atten * d * (li.type == 0 ? shadowFactor : 1.0f);
                r += albedo[0] * li.color.r * s;
                g += albedo[1] * li.color.g * s;
                b += albedo[2] * li.color.b * s;
            }
        } else {
            Vec3 N = (v[0].nw * l[0] + v[1].nw * l[1] + v[2].nw * l[2]) * w;
            N = N.Normalized();
            if (N.LengthSq() < 1e-9f) N = Vec3(0, 1, 0);
            // Tangent-space normal mapping (mesh path; same remap as sprites).
            // Requires a valid interpolated tangent frame (handedness != 0).
            if (mat.normalMap) {
                const float hand = v[0].th * l[0] + v[1].th * l[1] + v[2].th * l[2];
                Vec3 T = (v[0].tw * l[0] + v[1].tw * l[1] + v[2].tw * l[2]) * w;
                if (std::fabs(hand) > 1e-6f && T.LengthSq() > 1e-12f) {
                    T = (T - N * Vec3::Dot(N, T)).Normalized();  // orthonormal to N
                    const Vec3 B = Vec3::Cross(N, T) * (hand >= 0.0f ? 1.0f : -1.0f);
                    float nm[4];
                    mat.normalMap->SampleLod(uv.x * mat.tiling.x, uv.y * mat.tiling.y, lod,
                                             Texture::Wrap::Repeat,
                                             mat.normalMap->hasMipmaps() ? Texture::Filter::Trilinear
                                                                         : Texture::Filter::Bilinear,
                                             nm);
                    // Same axis convention as the sprite path (top-down v).
                    const Vec3 nts((nm[0] * 2.0f - 1.0f) * mat.normalStrength,
                                   -(nm[1] * 2.0f - 1.0f) * mat.normalStrength,
                                   nm[2] * 2.0f - 1.0f);
                    N = (T * nts.x + B * nts.y + N * nts.z).Normalized();
                }
            }
            if (Vec3::Dot(N, V) < 0.0f) N = -N;  // two-sided

            // GGX / Cook-Torrance specular + Lambert diffuse, metallic workflow.
            // Roughness < 0 derives from shininess (GGX alpha = sqrt(2/(s+2)),
            // roughness = sqrt(alpha)) so pre-PBR scenes keep their highlight;
            // explicit roughness in [0,1] overrides it. Dielectric F0 comes from
            // the authored specular (F0 = specular * 0.16, so specular 0.5 =>
            // F0 0.08); metals take F0 from the albedo and lose their diffuse.
            // The specular lobe is scaled by PI because the engine's diffuse
            // units (Lambert without the 1/PI) are calibrated that way.
            const float metallic = std::clamp(mat.metallic, 0.0f, 1.0f);
            float roughness = mat.roughness;
            if (roughness < 0.0f) {
                roughness = std::pow(2.0f / (std::max(mat.shininess, 1.0f) + 2.0f), 0.25f);
            }
            roughness = std::clamp(roughness, 0.04f, 1.0f);
            const float alpha = roughness * roughness;
            const float alpha2 = alpha * alpha;
            const Vec3 albedoV(albedo[0], albedo[1], albedo[2]);
            const float specScale = 0.16f * std::clamp(mat.specular, 0.0f, 1.0f);
            const Vec3 F0 = albedoV * metallic + Vec3(specScale, specScale, specScale) * (1.0f - metallic);
            const float NdotV = std::max(Vec3::Dot(N, V), 1e-4f);
            const bool wantsSpecular = metallic > 0.0f || mat.specular > 0.0f;
            // Metals do not diffuse; dielectric Fresnel also bleeds a little.
            const Vec3 diffuseAlbedo = albedoV * (1.0f - metallic);

            float lr = diffuseAlbedo.x * ambient.r;
            float lg = diffuseAlbedo.y * ambient.g;
            float lb = diffuseAlbedo.z * ambient.b;
            for (const GpuLight& li : lights) {
                Vec3 L;
                float atten = 1.0f;
                if (!LightFalloff(li, P, &L, &atten)) continue;
                const float NdotL = Vec3::Dot(N, L);
                if (NdotL <= 0.0f) continue;
                // Only directional lights are shadowed by the cascade map;
                // point and spot lights keep their own falloff untouched.
                const float shadow = li.type == 0 ? shadowFactor : 1.0f;
                const float s = li.intensity * atten * shadow * NdotL;
                // Schlick Fresnel from the half-vector.
                Vec3 H = (L + V);
                float hl = H.Length();
                if (hl > 1e-5f) H = H / hl;
                const float LdotH = std::clamp(Vec3::Dot(L, H), 0.0f, 1.0f);
                Vec3 F = F0 + (Vec3(1, 1, 1) - F0) * std::pow(1.0f - LdotH, 5.0f);
                // Diffuse, Fresnel-weighted so no energy is gained at grazing.
                lr += li.color.r * s * diffuseAlbedo.x * (1.0f - F.x);
                lg += li.color.g * s * diffuseAlbedo.y * (1.0f - F.y);
                lb += li.color.b * s * diffuseAlbedo.z * (1.0f - F.z);
                if (wantsSpecular) {
                    // GGX distribution, Heitz Smith visibility (= G/(4 NdotL NdotV)).
                    const float NdotH = std::clamp(Vec3::Dot(N, H), 0.0f, 1.0f);
                    const float dd = NdotH * NdotH * (alpha2 - 1.0f) + 1.0f;
                    const float D = alpha2 / (kPi * dd * dd);
                    const float gv = NdotL * std::sqrt(NdotV * NdotV * (1.0f - alpha2) + alpha2);
                    const float gl = NdotV * std::sqrt(NdotL * NdotL * (1.0f - alpha2) + alpha2);
                    const float Vis = 0.5f / std::max(gv + gl, 1e-7f);
                    const float spec = D * Vis * kPi;
                    lr += li.color.r * s * F.x * spec;
                    lg += li.color.g * s * F.y * spec;
                    lb += li.color.b * s * F.z * spec;
                }
            }
            // Emissive is self-light: added after the light loop, in linear.
            lr += mat.emissive.r;
            lg += mat.emissive.g;
            lb += mat.emissive.b;
            r = lr; g = lg; b = lb;
        }

        if (fogEnabled) {
            // Exponential height fog, blended in linear space.
            float f = 1.0f - std::exp(-fogDensity * viewDist);
            r = r + (fogLinear.r - r) * f;
            g = g + (fogLinear.g - g) * f;
            b = b + (fogLinear.b - b) * f;
        }
        r = std::max(r, 0.0f);
        g = std::max(g, 0.0f);
        b = std::max(b, 0.0f);

        float z = l[0] * v[0].z + l[1] * v[1].z + l[2] * v[2].z;
        float* drow = depth->data();
        float& dz = drow[(size_t)y * W + x];
        uint8_t* p = color->data() + px;
        if (mat.blend) {
            // Blend in display space: the framebuffer holds encoded bytes, so
            // the source is encoded first (matches what the user sees).
            const float a = albedo[3] * coverage;
            uint8_t src[3];
            EncodePixel(src, r, g, b);
            p[0] = (uint8_t)std::clamp(src[0] * a + p[0] * (1.0f - a) + 0.5f, 0.0f, 255.0f);
            p[1] = (uint8_t)std::clamp(src[1] * a + p[1] * (1.0f - a) + 0.5f, 0.0f, 255.0f);
            p[2] = (uint8_t)std::clamp(src[2] * a + p[2] * (1.0f - a) + 0.5f, 0.0f, 255.0f);
            p[3] = 255;
        } else if (coverage >= 0.999f) {
            EncodePixel(p, r, g, b);
            p[3] = 255;
            dz = z;
        } else {
            // Partially covered edge pixel: blend with what is already there
            // and leave the depth buffer alone (it still belongs to the
            // neighbour that fully covers this texel, if any).
            uint8_t src[3];
            EncodePixel(src, r, g, b);
            for (int c = 0; c < 3; ++c) {
                p[c] = (uint8_t)std::clamp(src[c] * coverage + p[c] * (1.0f - coverage) + 0.5f, 0.0f, 255.0f);
            }
            p[3] = 255;
        }
    }

    // Rasterizes one triangle, restricted to the rows [rowBegin, rowEnd) that
    // the calling worker owns. Splitting the screen into owned bands removes all
    // cross-thread writes to the colour and depth buffers.
    void RasterTriangle(int job, int rowBegin, int rowEnd) {
        RasterVert t[3] = {triVerts[job * 3], triVerts[job * 3 + 1], triVerts[job * 3 + 2]};
        const MaterialGpu& mat = triMats[job];
        float area = (t[1].x - t[0].x) * (t[2].y - t[0].y) - (t[2].x - t[0].x) * (t[1].y - t[0].y);
        if (std::fabs(area) < 1e-7f) return;
        // Back-face culling, resolved per polygon when the triangle was emitted.
        if (mat.cullMode == 1 && !mat.frontFacing) return;
        if (mat.cullMode == 2 && mat.frontFacing) return;
        // Normalize winding so barycentric weights stay positive.
        if (area < 0.0f) {
            std::swap(t[1], t[2]);
            area = -area;
        }

        const RasterVert* v = t;
        float invArea = 1.0f / area;
        float minX = std::max(0.0f, std::min({v[0].x, v[1].x, v[2].x}));
        float maxX = std::min((float)W - 1.0f, std::max({v[0].x, v[1].x, v[2].x}));
        float minY = std::max(0.0f, std::min({v[0].y, v[1].y, v[2].y}));
        float maxY = std::min((float)H - 1.0f, std::max({v[0].y, v[1].y, v[2].y}));
        if (minX > maxX || minY > maxY) return;
        // Mip level for this triangle: how much texture area is compressed into
        // how many pixels. Constant per triangle, so no derivative seams inside
        // a triangle and no per-pixel cost.
        float lod = 0.0f;
        if (mat.albedo && mat.albedo->hasMipmaps() && !mat.pixelPerfect) {
            const float texArea =
                std::fabs((v[1].tuv.x - v[0].tuv.x) * (v[2].tuv.y - v[0].tuv.y) -
                          (v[2].tuv.x - v[0].tuv.x) * (v[1].tuv.y - v[0].tuv.y)) *
                mat.tiling.x * mat.tiling.y * mat.albedo->width() * mat.albedo->height();
            const float scrArea = area;  // pixels^2
            if (texArea > 1e-6f && scrArea > 1e-6f) {
                lod = 0.5f * std::log2(texArea / scrArea);
                lod = std::clamp(lod, 0.0f, (float)(mat.albedo->mipLevels() - 1));
            }
        }
        int x0 = (int)std::floor(minX), x1 = (int)std::ceil(maxX);
        int y0 = std::max((int)std::floor(minY), rowBegin);
        int y1 = std::min((int)std::ceil(maxY), rowEnd - 1);
        if (y0 > y1) return;
        // Signed distance (in pixels) from the nearest edge, used as coverage.
        // e0 is the edge function of the v1->v2 edge, and so on.
        const float len0 = std::hypot(v[2].x - v[1].x, v[2].y - v[1].y);
        const float len1 = std::hypot(v[0].x - v[2].x, v[0].y - v[2].y);
        const float len2 = std::hypot(v[1].x - v[0].x, v[1].y - v[0].y);
        for (int y = y0; y <= y1; ++y) {
            float py = y + 0.5f;
            for (int x = x0; x <= x1; ++x) {
                float px = x + 0.5f;
                float e0 = (v[1].x - v[0].x) * (py - v[0].y) - (v[1].y - v[0].y) * (px - v[0].x);
                float e1 = (v[2].x - v[1].x) * (py - v[1].y) - (v[2].y - v[1].y) * (px - v[1].x);
                float e2 = (v[0].x - v[2].x) * (py - v[2].y) - (v[0].y - v[2].y) * (px - v[2].x);
                if (e0 < 0 || e1 < 0 || e2 < 0) continue;
                float coverage = 1.0f;
                if (mat.antialias) {
                    const float d = std::min(std::min(e0 / std::max(len2, 1e-5f), e1 / std::max(len0, 1e-5f)),
                                             e2 / std::max(len1, 1e-5f));
                    coverage = std::clamp(d + 0.5f, 0.0f, 1.0f);
                    if (coverage <= 0.0f) continue;
                }
                float w0 = e1 * invArea, w1 = e2 * invArea;
                float w2 = 1.0f - w0 - w1;
                // e_i is the edge function of edge v_i -> v_{i+1}, which is
                // proportional to the barycentric weight of the OPPOSITE
                // vertex: e0 -> w2, e1 -> w0, e2 -> w1. Assigning w0 = e0
                // permutes the weights cyclically and scrambles every
                // interpolated attribute (depth, world pos, normals, uvs).
                // Depth pre-test before shading.
                float z = v[0].z * w0 + v[1].z * w1 + v[2].z * w2;
                float& dz = (*depth)[(size_t)y * W + x];
                // Blended geometry is depth-tested but never writes depth, so
                // sprites and text stay correctly ordered among themselves and
                // are still occluded by opaque geometry.
                if (z >= dz) continue;
                if (mat.depthOnly) {
                    dz = z;   // shadow pass: depth only, no shading
                    continue;
                }
                float L[3] = {w0, w1, w2};
                ShadeAndStore(x, y, v, L, mat, lod, coverage);
            }
        }
    }

    // Bins triangles into horizontal bands and rasterizes band-parallel. Each
    // worker owns a contiguous row range, so no two threads ever write the same
    // pixel: the previous implementation raced on the colour and depth buffers.
    void RasterizeParallel(int firstJob, int jobCount) {
        const int bands = std::clamp(pool.workerCount(), 1, std::max(1, H / 8));
        if (bands <= 1 || jobCount == 0) {
            for (int i = firstJob; i < firstJob + jobCount; ++i) RasterTriangle(i, 0, H);
            return;
        }
        bandRows.assign(bands, 0);
        const int rowsPerBand = (H + bands - 1) / bands;
        for (int b = 0; b < bands; ++b) {
            bandRows[b] = std::min(H, (b + 1) * rowsPerBand) - b * rowsPerBand;
        }
        // Bin: a triangle goes to every band its bounding box overlaps.
        bandJobs.assign(bands, {});
        for (int job = firstJob; job < firstJob + jobCount; ++job) {
            float minY = std::min({triVerts[job * 3].y, triVerts[job * 3 + 1].y, triVerts[job * 3 + 2].y});
            float maxY = std::max({triVerts[job * 3].y, triVerts[job * 3 + 1].y, triVerts[job * 3 + 2].y});
            int b0 = std::clamp((int)std::floor(minY) / rowsPerBand, 0, bands - 1);
            int b1 = std::clamp((int)std::floor(maxY) / rowsPerBand, 0, bands - 1);
            for (int b = b0; b <= b1; ++b) bandJobs[b].push_back(job);
        }
        pool.ParallelFor(bands, [this, rowsPerBand](int b) {
            const int begin = b * rowsPerBand;
            const int end = begin + bandRows[b];
            for (int job : bandJobs[b]) RasterTriangle(job, begin, end);
        });
        bandJobs.clear();
    }

    std::vector<int> bandRows;
    std::vector<std::vector<int>> bandJobs;

    // Depth-tested 3D line (no depth write). The segment is clipped against a
    // padded screen rect after projection, so near-tangent lines (whose
    // unclipped endpoints divide by ~0) can never explode the step count.
    void DrawLine3D(const Vec3& a, const Vec3& b, const Color& c) {
        Vec4 ca = viewProj * Vec4(a, 1.0f);
        Vec4 cb = viewProj * Vec4(b, 1.0f);
        if (ca.w < 1e-6f && cb.w < 1e-6f) return;
        if (ca.w < 1e-6f) {
            float t = (1e-6f - ca.w) / (cb.w - ca.w);
            ca = ca + (cb - ca) * t;
        } else if (cb.w < 1e-6f) {
            float t = (1e-6f - cb.w) / (ca.w - cb.w);
            cb = cb + (ca - cb) * t;
        }
        Vec3 pa(ca.x / ca.w, ca.y / ca.w, ca.z / ca.w);
        Vec3 pb(cb.x / cb.w, cb.y / cb.w, cb.z / cb.w);
        Vec2 sa((pa.x * 0.5f + 0.5f) * W, (1.0f - (pa.y * 0.5f + 0.5f)) * H);
        Vec2 sb((pb.x * 0.5f + 0.5f) * W, (1.0f - (pb.y * 0.5f + 0.5f)) * H);

        // Parametric clip against the padded screen rectangle.
        const float pad = 64.0f;
        float t0 = 0.0f, t1 = 1.0f;
        auto clipAxis = [&](float s0, float s1, float lo, float hi) {
            float d = s1 - s0;
            if (std::fabs(d) < 1e-9f) {
                if (s0 < lo || s0 > hi) t0 = 1.0f, t1 = 0.0f;  // fully outside
                return;
            }
            float ta = (lo - s0) / d, tb = (hi - s0) / d;
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
        };
        clipAxis(sa.x, sb.x, -pad, W + pad);
        clipAxis(sa.y, sb.y, -pad, H + pad);
        if (t0 >= t1) return;

        float x0 = sa.x + (sb.x - sa.x) * t0, y0 = sa.y + (sb.y - sa.y) * t0;
        float x1 = sa.x + (sb.x - sa.x) * t1, y1 = sa.y + (sb.y - sa.y) * t1;
        float dx = x1 - x0, dy = y1 - y0;
        float len = std::sqrt(dx * dx + dy * dy);
        int steps = std::max(1, (int)std::ceil(len * 2.0f));  // bounded by screen size
        float z0 = pa.z + (pb.z - pa.z) * t0;
        float z1 = pa.z + (pb.z - pa.z) * t1;
        for (int i = 0; i <= steps; ++i) {
            float t = (float)i / steps;
            int x = (int)(x0 + dx * t), y = (int)(y0 + dy * t);
            if (x < 0 || y < 0 || x >= W || y >= H) continue;
            float dzVal = std::clamp(z0 + (z1 - z0) * t, 0.0f, 1.0f) * 0.5f + 0.5f;
            if (dzVal > (*depth)[(size_t)y * W + x] + 1e-4f) continue;
            size_t px = ((size_t)y * W + x) * 4;
            uint8_t* p = color->data() + px;
            p[0] = (uint8_t)std::clamp(c.r * 255.0f, 0.0f, 255.0f);
            p[1] = (uint8_t)std::clamp(c.g * 255.0f, 0.0f, 255.0f);
            p[2] = (uint8_t)std::clamp(c.b * 255.0f, 0.0f, 255.0f);
        }
    }

    // Builds one glyph quad per character straight into the shared 2D vertex
    // list. The atlas has to stay alive for the frame, so the caller pins it.
    void AppendTextQuads(const TextRenderer& text, const Mat4& world, FrameDrawList* list) {
        const BitmapFont& font = BitmapFont::Get();
        const Texture* atlas = font.atlas();
        if (!atlas) return;
        float scale = text.fontSize / (float)BitmapFont::kGlyphH;
        float width = font.Measure(text.text) * scale;
        float x0 = 0.0f;
        if (text.alignment == TextRenderer::Alignment::Center) x0 = -width * 0.5f;
        else if (text.alignment == TextRenderer::Alignment::Right) x0 = -width;
        MaterialGpu mat;
        mat.albedo = atlas;
        mat.color = text.color;
        mat.blend = true;
        mat.unlit = true;
        mat.pixelPerfect = true;
        mat.cullMode = 0;    // text is visible from both sides
        mat.antialias = false;
        const uint32_t first = (uint32_t)list->verts2D.size();
        float pen = x0;
        for (unsigned char ch : text.text) {
            GlyphRect g;
            font.Glyph(ch, &g);
            float gw = (float)BitmapFont::kGlyphW * scale;
            float gh = (float)BitmapFont::kGlyphH * scale;
            Vec3 quad[4] = {world.TransformPoint(Vec3(pen, 0, 0)),
                            world.TransformPoint(Vec3(pen + gw, 0, 0)),
                            world.TransformPoint(Vec3(pen + gw, gh, 0)),
                            world.TransformPoint(Vec3(pen, gh, 0))};
            Vec2 uv[4] = {{g.u, g.v}, {g.u + g.w, g.v}, {g.u + g.w, g.v + g.h}, {g.u, g.v + g.h}};
            AppendQuad(list, quad, uv);
            pen += (float)g.advance * scale;
        }
        if (list->verts2D.size() > first) {
            FrameDrawList::Item2D item;
            item.mat = mat;
            item.firstVert = first;
            item.vertCount = (uint32_t)list->verts2D.size() - first;
            list->items2D.push_back(item);
        }
    }

    // ------------------------------------------------------------------
    // Scene gather: the backend-independent front end.
    //
    // One walk of the Scene produces the light set, the frustum-culled 3D
    // draws, the depth-sorted 2D quads and the shadow casters. Everything
    // after this point is pure execution: the software rasterizer walks the
    // same list through its clipping front end, the Vulkan backend uploads it
    // verbatim.
    struct SceneGather {
        int meshesSeen = 0;
        int frustumCulled = 0;
        int drawCalls2D = 0;
        int lightCount = 0;
        int clipRejected = 0;   // meshes that produced no drawable triangles
    };

    void CollectLights(const Scene& scene, std::vector<GpuLight>* out) {
        out->clear();
        bool warned = false;
        for (const auto& go : scene.objects) {
            if (!go->ActiveInHierarchy()) continue;
            auto* light = go->GetComponent<LightComponent>();
            if (!light || !light->enabled) continue;   // disabled lights emit nothing
            GpuLight gl;
            gl.type = (int)light->type;
            gl.pos = go->transform().WorldPosition();
            gl.dir = go->transform().Forward().Normalized();
            if (gl.dir.LengthSq() < 1e-9f) gl.dir = Vec3(0, -1, 0);
            gl.color = light->color;
            gl.intensity = light->intensity;
            gl.range = light->range;
            if ((int)light->type == (int)LightComponent::LightType::Spot) {
                gl.type = 2;
                gl.cosOuter = std::cos(Radians(std::clamp(light->spotAngle, 1.0f, 89.0f)));
                gl.cosInner =
                    std::cos(Radians(std::clamp(light->spotInnerAngle, 0.5f, light->spotAngle)));
            }
            gl.castShadows = light->castShadows;
            gl.shadowBias = light->shadowBias;
            out->push_back(gl);
            if (out->size() >= kMaxLights) {
                if (!warned) {
                    warned = true;
                    Log::Warning("light limit (%d) reached; further lights are ignored this frame",
                                 (int)kMaxLights);
                }
                break;
            }
        }
    }

    void GatherScene(Scene& scene, const Frustum& frustum, TextureLibrary& textures,
                     FrameDrawList* list, SceneGather* stats) {
        // ---- Opaque 3D drawables, plus the caster set for the shadow fits.
        for (const auto& go : scene.objects) {
            if (!go->ActiveInHierarchy()) continue;
            auto* mr = go->GetComponent<MeshRenderer>();
            if (!mr || !mr->enabled) continue;   // disabling the component hides the mesh
            stats->meshesSeen++;
            std::shared_ptr<Mesh> mesh = ResolveMesh(mr);
            if (!mesh) continue;
            const Mat4 world = go->transform().WorldMatrix();
            const AABB worldBounds = mesh->Bounds().Transformed(world);
            if (mr->castShadows) {
                FrameDrawList::ShadowCaster caster;
                caster.mesh = mesh;
                caster.model = world;
                caster.bounds = worldBounds;
                caster.cullMode = (int)mr->cullMode;
                list->casters.push_back(std::move(caster));
            }
            // Frustum culling: a mesh whose world bounds miss the view is
            // skipped before any vertex is transformed or clipped.
            if (!frustum.IntersectsAABB(worldBounds)) {
                stats->frustumCulled++;
                continue;
            }
            MaterialGpu mat;
            mat.color = mr->albedoColor;
            mat.tiling = mr->tiling;
            mat.specular = mr->specular;
            mat.shininess = mr->shininess;
            mat.metallic = mr->metallic;
            mat.roughness = mr->roughness;
            mat.normalStrength = std::max(0.0f, mr->normalStrength);
            // Emissive is authored in sRGB like every other color; light it as
            // data.
            mat.emissive = Color(SrgbToLinearFast(mr->emissiveColor.r),
                                 SrgbToLinearFast(mr->emissiveColor.g),
                                 SrgbToLinearFast(mr->emissiveColor.b), 1.0f);
            mat.unlit = mr->unlit;
            mat.cullMode = (int)mr->cullMode;
            mat.antialias = mr->antialias;
            mat.castShadows = mr->castShadows;
            mat.receiveShadows = mr->receiveShadows;
            if (!mr->albedoTexturePath.empty()) {
                std::shared_ptr<Texture> tex = textures.Get(EngineResolvePath(mr->albedoTexturePath));
                list->liveTextures.push_back(tex);   // pin: eviction must not free it
                mat.albedo = tex.get();
                mat.albedoSRGB = tex && tex->isSRGB();
            }
            if (!mr->normalMapPath.empty()) {
                // Normal maps are data: sample as linear (asset .meta import
                // settings will own color-space selection later).
                std::shared_ptr<Texture> tex =
                    textures.Get(EngineResolvePath(mr->normalMapPath), /*srgb=*/false);
                list->liveTextures.push_back(tex);
                mat.normalMap = tex.get();
            }
            if (mesh->indices.size() < 3) {
                stats->clipRejected++;
                continue;
            }
            FrameDrawList::OpaqueDraw draw;
            draw.mesh = mesh;
            draw.model = world;
            draw.mat = mat;
            list->opaque.push_back(std::move(draw));
        }

        // ---- 2D pass: sprites + text, sorted by layer then depth (far first).
        struct DrawItem {
            int sortingLayer;
            int order;
            float depthKey;   // distance along the view direction (far first)
            bool isText;
            GameObject* go;
        };
        std::vector<DrawItem> items;
        for (const auto& go : scene.objects) {
            if (!go->ActiveInHierarchy()) continue;
            const bool sprite =
                go->GetComponent<SpriteRenderer>() && go->GetComponent<SpriteRenderer>()->enabled;
            const bool text = go->GetComponent<TextRenderer>() && go->GetComponent<TextRenderer>()->enabled;
            if (!sprite && !text) continue;
            // Sprite/text bounds come from their component size; text uses a
            // rough estimate that only has to be conservative.
            AABB bounds;
            if (sprite) {
                auto* sp = go->GetComponent<SpriteRenderer>();
                float w = sp->pixelsPerUnit * 0.5f;
                bounds = AABB(Vec3(-w, -w, 0), Vec3(w, w, 0));
            } else {
                auto* tr = go->GetComponent<TextRenderer>();
                float w = tr->fontSize * 0.6f * static_cast<float>(std::max<size_t>(1, tr->text.size()));
                bounds = AABB(Vec3(-w, 0, 0), Vec3(w, tr->fontSize, 0));
            }
            if (!frustum.IntersectsAABB(bounds.Transformed(go->transform().WorldMatrix()))) {
                stats->frustumCulled++;
                continue;
            }
            // Depth is measured along the view direction so transparents sort
            // correctly for any camera orientation (world Z only worked for a
            // camera looking down an axis).
            const float viewDepth =
                Vec3::Dot(go->transform().WorldPosition() - list->camPos, list->camForward);
            if (sprite) {
                auto* sp = go->GetComponent<SpriteRenderer>();
                items.push_back({sp->sortingLayer, sp->orderInLayer, viewDepth, false, go.get()});
            } else {
                auto* tr = go->GetComponent<TextRenderer>();
                items.push_back({tr->sortingLayer, tr->orderInLayer, viewDepth, true, go.get()});
            }
        }
        std::sort(items.begin(), items.end(), [](const DrawItem& a, const DrawItem& b) {
            if (a.sortingLayer != b.sortingLayer) return a.sortingLayer < b.sortingLayer;
            if (a.order != b.order) return a.order < b.order;
            return a.depthKey > b.depthKey;  // far first
        });

        for (const DrawItem& item : items) {
            GameObject* go = item.go;
            if (!item.isText) {
                auto* sr = go->GetComponent<SpriteRenderer>();
                std::shared_ptr<Texture> tex;
                if (!sr->texturePath.empty()) tex = textures.Get(EngineResolvePath(sr->texturePath));
                else tex = textures.GetBuiltin("checker");
                if (!tex) continue;
                list->liveTextures.push_back(tex);  // pin against LRU eviction mid-frame
                float w = tex->width() / std::max(1.0f, sr->pixelsPerUnit);
                float h = tex->height() / std::max(1.0f, sr->pixelsPerUnit);
                if (sr->flipX) w = -w;
                if (sr->flipY) h = -h;
                MaterialGpu mat;
                mat.albedo = tex.get();
                mat.albedoSRGB = tex->isSRGB();
                mat.color = sr->color;
                mat.blend = true;
                mat.spriteLighting = true;
                mat.pixelPerfect = sr->pixelPerfect;
                mat.cullMode = 0;    // a flipped sprite must not vanish
                mat.antialias = !sr->pixelPerfect;
                if (!sr->normalMapPath.empty()) {
                    std::shared_ptr<Texture> nrm =
                        textures.Get(EngineResolvePath(sr->normalMapPath), /*srgb=*/false);
                    list->liveTextures.push_back(nrm);
                    mat.normalMap = nrm.get();
                }
                Vec2 uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
                Mat4 world = go->transform().WorldMatrix();
                Vec3 q[4];
                q[0] = world.TransformPoint(Vec3(-w * 0.5f, -h * 0.5f, 0));
                q[1] = world.TransformPoint(Vec3(w * 0.5f, -h * 0.5f, 0));
                q[2] = world.TransformPoint(Vec3(w * 0.5f, h * 0.5f, 0));
                q[3] = world.TransformPoint(Vec3(-w * 0.5f, h * 0.5f, 0));
                FrameDrawList::Item2D out;
                out.mat = mat;
                out.firstVert = AppendQuad(list, q, uv);
                out.vertCount = 6;
                list->items2D.push_back(out);
            } else {
                auto* tr = go->GetComponent<TextRenderer>();
                AppendTextQuads(*tr, go->transform().WorldMatrix(), list);
            }
        }
        stats->drawCalls2D = (int)items.size();
    }
};

// Appends one world-space quad (six vertices) to the shared 2D list. Both
// backends consume the result verbatim, so the winding and the derived plane
// normal are decided here exactly once.
uint32_t AppendQuad(FrameDrawList* list, const Vec3 world[4], const Vec2 uv[4]) {
    Vec3 normal = Vec3::Cross(world[1] - world[0], world[3] - world[0]).Normalized();
    if (normal.LengthSq() < 1e-9f) normal = Vec3(0, 0, -1);
    const uint32_t first = (uint32_t)list->verts2D.size();
    static const int kOrder[6] = {0, 1, 2, 0, 2, 3};
    for (int k = 0; k < 6; ++k) {
        Vertex2D v;
        v.pos = world[kOrder[k]];
        v.normal = normal;
        v.uv = uv[kOrder[k]];
        v.color = Color(1, 1, 1, 1);
        list->verts2D.push_back(v);
    }
    return first;
}

// ---------------------------------------------------------------------------
// Renderer public API.
// ---------------------------------------------------------------------------
Renderer::Renderer() : impl_(new Impl()) {}
Renderer::~Renderer() = default;

void Renderer::Resize(int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (target_.width == w && target_.height == h) return;
    target_.width = w;
    target_.height = h;
    target_.color.assign((size_t)w * h * 4, 0);
    target_.depth.assign((size_t)w * h, 1.0f);
    impl_->W = w;
    impl_->H = h;
    impl_->color = &target_.color;
    impl_->depth = &target_.depth;
}

RenderStats Renderer::RenderScene(Scene& scene, const CameraView& cam,
                                  const std::vector<DebugLine>& overlayLines) {
    Impl& R = *impl_;
    if (target_.width <= 0) return {};

    // Snapshot uniforms read by workers.
    R.camPos = cam.position;
    R.viewProj = cam.projection * cam.view;
    R.ortho = cam.orthographic;
    R.tanHalfFov = std::tan(Radians(cam.fov) * 0.5f);
    R.aspect = (float)target_.width / (float)target_.height;
    R.orthoSize = cam.orthoSize;
    Mat4 invView = cam.view.Inverted();
    R.camRight = invView.TransformDirection(Vec3::Right);
    R.camUp = invView.TransformDirection(Vec3::Up);
    R.camForward = invView.TransformDirection(Vec3::Forward);
    R.ambient = cam.ambient;
    R.ambientLinear = {SrgbToLinearFast(cam.ambient.r), SrgbToLinearFast(cam.ambient.g),
                       SrgbToLinearFast(cam.ambient.b), 1.0f};
    R.tonemap = cam.tonemap;
    R.exposure = cam.exposure;
    R.fogEnabled = cam.fogEnabled;
    R.fogLinear = {SrgbToLinearFast(cam.fogColor.r), SrgbToLinearFast(cam.fogColor.g),
                   SrgbToLinearFast(cam.fogColor.b), 1.0f};
    R.fogDensity = cam.fogDensity;

    MeshLibrary& meshes = EngineMeshLibrary();
    TextureLibrary& textures = EngineTextureLibrary();
    // Byte budget from the scene setting; eviction is LRU over cached files.
    textures.SetBudgetBytes((size_t)std::max(1, cam.textureMemoryMB) << 20);

    // ---- Shared front end: one walk of the scene, one draw list.
    FrameDrawList& list = R.drawList;
    list = FrameDrawList{};
    list.cam = &cam;
    list.width = target_.width;
    list.height = target_.height;
    list.viewProj = R.viewProj;
    list.camPos = R.camPos;
    list.camRight = R.camRight;
    list.camUp = R.camUp;
    list.camForward = R.camForward;
    list.ortho = R.ortho;
    list.tanHalfFov = R.tanHalfFov;
    list.aspect = R.aspect;
    list.orthoSize = R.orthoSize;
    list.lines = overlayLines;

    R.CollectLights(scene, &list.lights);
    Impl::SceneGather gather;
    R.GatherScene(scene, Frustum::FromViewProj(R.viewProj), textures, &list, &gather);
    gather.lightCount = (int)list.lights.size();

    // Cascade fitting is shared too; only the depth rendering differs.
    R.casterBounds.clear();
    R.casterBounds.reserve(list.casters.size());
    for (const auto& caster : list.casters) R.casterBounds.push_back(caster.bounds);
    R.FitCascadeSet(list, cam);

    // ---- Backend selection. `Auto` prefers the GPU and degrades silently to
    // software; an explicit Vulkan request logs why when it cannot be honoured.
    const bool vulkanWanted = cam.backend == RenderBackend::Vulkan ||
                              (cam.backend == RenderBackend::Auto && VulkanBackendAvailable());
    if (vulkanWanted && !R.vulkan) {
        std::string error;
        auto backend = std::make_unique<VulkanBackend>();
        if (backend->Initialize(&error)) {
            R.vulkan = std::move(backend);
        } else if (cam.backend == RenderBackend::Vulkan) {
            Log::Warning("render backend forced to Vulkan but it is unavailable (%s); using software",
                         error.c_str());
        }
    }
    if (R.vulkan && vulkanWanted) {
        RenderStats stats;
        R.vulkan->RenderFrame(list, &target_, &stats, cam.msaa);
        stats.meshesSeen = gather.meshesSeen;
        stats.clipRejected = gather.clipRejected;
        stats.frustumCulled = gather.frustumCulled;
        stats.drawCalls2D = gather.drawCalls2D;
        stats.lightCount = gather.lightCount;
        if (stats.backend == nullptr || std::string(stats.backend) != "vulkan") {
            // The backend bailed out mid-frame; fall through to software so the
            // host still gets a frame instead of the previous image.
            Log::Warning("Vulkan backend could not render this frame; falling back to software");
            R.vulkan.reset();
        } else {
            return stats;
        }
    }

    // ---- Software backend.
    R.lights = list.lights;
    R.ClearAndSky(cam);

    const int prevShadowSize = R.cascadeSize;
    if (R.cascadeSize != prevShadowSize ||
        R.shadowScratch.size() < (size_t)R.cascadeSize * R.cascadeSize * 4) {
        R.shadowScratch.assign((size_t)R.cascadeSize * R.cascadeSize * 4, 0);
    }
    if (R.cascadeCount > 0) {
        R.cascadeDepth.resize((size_t)kMaxCascades);
        R.RenderShadowPassSoftware(list);
    }
    R.shadows = R.shadowSampler();
    R.shadowedPixels.store(0, std::memory_order_relaxed);
    // Textures referenced this frame are pinned in the draw list; the software
    // rasterizer reads them through the same MaterialGpu pointers.
    R.liveTextures = list.liveTextures;

    R.triVerts.clear();
    R.triMats.clear();

    // ---- Opaque 3D drawables.
    int clipRejected = 0;
    for (const auto& draw : list.opaque) {
        if (!draw.mesh) continue;
        size_t before = R.triVerts.size();
        R.EmitMesh(*draw.mesh, draw.model, draw.mat);
        if (R.triVerts.size() == before) clipRejected++;
    }

    // ---- Rasterize opaque jobs across the pool (row-band owned).
    const int jobCount = (int)(R.triVerts.size() / 3);
    R.RasterizeParallel(0, jobCount);

    // ---- 2D pass: each item is rasterized immediately after its quads are
    // emitted so the depth sort stays exact (clipped quads can fan out).
    for (const auto& item : list.items2D) {
        size_t startIndex = R.triVerts.size();
        for (uint32_t v = 0; v < item.vertCount; v += 6) {
            const Vertex2D* quad = list.verts2D.data() + item.firstVert + v;
            if (v + 6 > item.vertCount) break;
            R.EmitQuadVerts(quad, item.mat);
        }
        int from = (int)(startIndex / 3);
        int to = (int)(R.triVerts.size() / 3);
        // Sorted per item and drawn on this thread so blend order is exact.
        for (int i = from; i < to; ++i) R.RasterTriangle(i, 0, R.H);
        R.triVerts.resize(startIndex);
        R.triMats.resize(from);
    }

    // ---- Overlay lines (grid, selection, Debug::DrawLine).
    for (const DebugLine& l : overlayLines) R.DrawLine3D(l.a, l.b, l.color);

    // ---- Post-processing (bloom -> color grading -> vignette -> FXAA), on
    // the finished display-referred frame and single-threaded. Skipped
    // entirely when every effect is at its neutral default.
    RenderStats stats;
    stats.backend = "software";
    stats.postPasses = R.RunPostChain(cam);
    stats.drawCalls = jobCount + gather.drawCalls2D + 1;
    stats.triangles = jobCount;
    stats.meshesSeen = gather.meshesSeen;
    stats.clipRejected = clipRejected;
    stats.frustumCulled = gather.frustumCulled;
    stats.drawCalls2D = gather.drawCalls2D;
    stats.lightCount = gather.lightCount;
    stats.shadowedPixels = R.shadowedPixels.load(std::memory_order_relaxed);
    return stats;
}

}  // namespace Cairn
