#include "cairn/renderer.h"

#include "cairn/engine.h"
#include "font_internal.h"
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
        {
            std::lock_guard<std::mutex> lock(m_);
            task_ = &f;
            jobCount_ = count;
            next_.store(0, std::memory_order_relaxed);
            pending_.store(count, std::memory_order_relaxed);
            epoch_++;
        }
        wake_.notify_all();
        Work();
        std::unique_lock<std::mutex> lock(m_);
        done_.wait(lock, [this] { return pending_.load(std::memory_order_acquire) == 0; });
    }

private:
    void Work() {
        while (true) {
            int i = next_.fetch_add(1, std::memory_order_relaxed);
            if (i >= jobCount_) break;
            (*task_)(i);
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
    const std::function<void(int)>* task_ = nullptr;
    int jobCount_ = 0;
    std::atomic<int> next_{0};
    std::atomic<int> pending_{0};
    uint64_t epoch_ = 0;
    bool stop_ = false;
};

// ---------------------------------------------------------------------------
// Rasterization data.
// ---------------------------------------------------------------------------
struct RasterVert {
    float x = 0, y = 0;    // screen pixels
    float z = 0;           // depth [0,1]
    float invW = 1;
    Vec3 pw;               // worldPos / w
    Vec3 nw;               // normal / w
    Vec2 tuv;              // uv / w
};

struct MaterialGpu {
    const Texture* albedo = nullptr;
    Vec2 tiling{1, 1};
    Color color{1, 1, 1, 1};
    float specular = 0.5f;
    float shininess = 32.0f;
    bool unlit = false;
    const Texture* normalMap = nullptr;
    bool pixelPerfect = false;
    bool blend = false;        // alpha blend instead of opaque overwrite
    bool spriteLighting = false;  // 2D lighting using normalMap / plain sprites
};

struct GpuLight {
    int type = 0;         // 0 directional, 1 point
    Vec3 pos;
    Vec3 dir{0, -1, 0};   // normalized, points from light toward scene
    Color color{1, 1, 1};
    float intensity = 1.0f;
    float range = 15.0f;
};

struct Renderer::Impl {
    JobPool pool;
    std::vector<RasterVert> triVerts;     // 3 per triangle job
    std::vector<MaterialGpu> triMats;     // 1 per triangle job
    std::vector<GpuLight> lights;
    Vec3 camPos;
    Vec3 camRight, camUp, camForward;
    Mat4 viewProj;
    bool ortho = false;
    float tanHalfFov = 1.0f, aspect = 1.0f, orthoSize = 5.0f;
    // Scene atmosphere snapshot (read by workers).
    Color ambient;
    bool fogEnabled = false;
    Color fogColor;
    float fogDensity = 0.02f;
    // Framebuffers.
    int W = 0, H = 0;
    std::vector<uint8_t>* color = nullptr;
    std::vector<float>* depth = nullptr;

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
        Vec2 uv;
    };

    static ClipVert ToClip(const Mat4& viewProj, const Mat4& world, const MeshVertex& v) {
        ClipVert out;
        out.world = world.TransformPoint(v.position);
        out.clip = viewProj * Vec4(out.world, 1.0f);
        out.normal = world.TransformDirection(v.normal);
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
                rv[k].tuv = tri[k]->uv * invW;
            }
            if (!ok) continue;
            for (auto& v : rv) triVerts.push_back(v);
            // Material appended by caller (EmitMesh/EmitSprite set it before/after).
            triMats.push_back(pendingMat);
        }
    }

    MaterialGpu pendingMat;

    // Transforms + clips + emits one mesh.
    void EmitMesh(const Mesh& mesh, const Mat4& world, const MaterialGpu& mat) {
        pendingMat = mat;
        std::vector<ClipVert> poly, clipped;
        const auto& idx = mesh.indices;
        for (size_t i = 0; i + 2 < idx.size(); i += 3) {
            const MeshVertex& a = mesh.vertices[idx[i]];
            const MeshVertex& b = mesh.vertices[idx[i + 1]];
            const MeshVertex& c = mesh.vertices[idx[i + 2]];
            ClipVert cv[3] = {ToClip(viewProj, world, a), ToClip(viewProj, world, b),
                              ToClip(viewProj, world, c)};
            poly.clear();
            clipped.clear();
            if (!ClipPolygon(cv, 3, clipped)) continue;
            EmitPolygon(clipped);
        }
    }

    // Builds one screen-space quad (two triangles) from world corners.
    void EmitQuad(const Vec3 world[4], const Vec2 uv[4], const MaterialGpu& mat) {
        pendingMat = mat;
        Vec3 normal = Vec3::Cross(world[1] - world[0], world[3] - world[0]).Normalized();
        if (normal.LengthSq() < 1e-9f) normal = Vec3(0, 0, -1);
        ClipVert cv[4];
        for (int i = 0; i < 4; ++i) {
            cv[i].world = world[i];
            cv[i].clip = viewProj * Vec4(world[i], 1.0f);
            cv[i].normal = normal;
            cv[i].uv = uv[i];
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
    // Per-pixel shading.
    void ShadeAndStore(int x, int y, const RasterVert* v, const float l[3], const MaterialGpu& mat) {
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
        Texture::Filter filter = mat.pixelPerfect ? Texture::Filter::Nearest : Texture::Filter::Bilinear;

        float albedo[4] = {mat.color.r, mat.color.g, mat.color.b, mat.color.a};
        if (mat.albedo) {
            float tex[4];
            mat.albedo->Sample(uv.x * mat.tiling.x, uv.y * mat.tiling.y, wrap, filter, tex);
            albedo[0] *= tex[0]; albedo[1] *= tex[1]; albedo[2] *= tex[2]; albedo[3] *= tex[3];
        }

        float r, g, b;
        if (mat.unlit) {
            r = albedo[0]; g = albedo[1]; b = albedo[2];
        } else if (mat.spriteLighting) {
            // 2D lighting: normal from the sprite plane or its normal map.
            Vec3 N(0, 0, -1);
            if (mat.normalMap) {
                float nm[4];
                mat.normalMap->Sample(uv.x, uv.y, Texture::Wrap::Clamp, filter, nm);
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
                if (li.type == 0) {
                    L = -li.dir;
                } else {
                    Vec3 ld = li.pos - P;
                    float dist = ld.Length();
                    if (dist > li.range || dist < 1e-5f) continue;
                    L = ld / dist;
                    atten = 1.0f - dist / li.range;
                    atten *= atten;
                }
                float d = Vec3::Dot(N, L);
                if (d <= 0.0f) continue;
                float s = li.intensity * atten * d;
                r += albedo[0] * li.color.r * s;
                g += albedo[1] * li.color.g * s;
                b += albedo[2] * li.color.b * s;
            }
        } else {
            Vec3 N = (v[0].nw * l[0] + v[1].nw * l[1] + v[2].nw * l[2]) * w;
            N = N.Normalized();
            if (N.LengthSq() < 1e-9f) N = Vec3(0, 1, 0);
            if (Vec3::Dot(N, V) < 0.0f) N = -N;  // two-sided
            float lr = albedo[0] * ambient.r;
            float lg = albedo[1] * ambient.g;
            float lb = albedo[2] * ambient.b;
            for (const GpuLight& li : lights) {
                Vec3 L;
                float atten = 1.0f;
                if (li.type == 0) {
                    L = -li.dir;
                } else {
                    Vec3 ld = li.pos - P;
                    float dist = ld.Length();
                    if (dist > li.range || dist < 1e-5f) continue;
                    L = ld / dist;
                    atten = 1.0f - dist / li.range;
                    atten *= atten;
                }
                float diff = Vec3::Dot(N, L);
                if (diff <= 0.0f) continue;
                float s = li.intensity * atten;
                lr += albedo[0] * li.color.r * s * diff;
                lg += albedo[1] * li.color.g * s * diff;
                lb += albedo[2] * li.color.b * s * diff;
                if (mat.specular > 0.0f) {
                    Vec3 H = (L + V);
                    float hl = H.Length();
                    if (hl > 1e-5f) H = H / hl;
                    float sp = std::pow(std::max(Vec3::Dot(N, H), 0.0f), mat.shininess) * mat.specular * s;
                    lr += li.color.r * sp;
                    lg += li.color.g * sp;
                    lb += li.color.b * sp;
                }
            }
            r = lr; g = lg; b = lb;
        }

        if (fogEnabled) {
            float f = 1.0f - std::exp(-fogDensity * viewDist);
            r = r + (fogColor.r - r) * f;
            g = g + (fogColor.g - g) * f;
            b = b + (fogColor.b - b) * f;
        }

        r = std::clamp(r, 0.0f, 1.0f);
        g = std::clamp(g, 0.0f, 1.0f);
        b = std::clamp(b, 0.0f, 1.0f);

        float z = l[0] * v[0].z + l[1] * v[1].z + l[2] * v[2].z;
        float* drow = depth->data();
        float& dz = drow[(size_t)y * W + x];
        uint8_t* p = color->data() + px;
        if (mat.blend) {
            float a = albedo[3];
            if (a >= 0.999f && !mat.spriteLighting) {
                // Fully opaque blended pixel still respects depth ordering.
            }
            p[0] = (uint8_t)std::clamp(r * 255.0f * a + p[0] * (1.0f - a), 0.0f, 255.0f);
            p[1] = (uint8_t)std::clamp(g * 255.0f * a + p[1] * (1.0f - a), 0.0f, 255.0f);
            p[2] = (uint8_t)std::clamp(b * 255.0f * a + p[2] * (1.0f - a), 0.0f, 255.0f);
            p[3] = 255;
        } else {
            p[0] = (uint8_t)(r * 255.0f + 0.5f);
            p[1] = (uint8_t)(g * 255.0f + 0.5f);
            p[2] = (uint8_t)(b * 255.0f + 0.5f);
            p[3] = 255;
            dz = z;
        }
    }

    void RasterTriangle(int job) {
        RasterVert t[3] = {triVerts[job * 3], triVerts[job * 3 + 1], triVerts[job * 3 + 2]};
        const MaterialGpu& mat = triMats[job];
        float area = (t[1].x - t[0].x) * (t[2].y - t[0].y) - (t[2].x - t[0].x) * (t[1].y - t[0].y);
        if (std::fabs(area) < 1e-7f) return;
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
        int x0 = (int)std::floor(minX), x1 = (int)std::ceil(maxX);
        int y0 = (int)std::floor(minY), y1 = (int)std::ceil(maxY);
        for (int y = y0; y <= y1; ++y) {
            float py = y + 0.5f;
            for (int x = x0; x <= x1; ++x) {
                float px = x + 0.5f;
                float e0 = (v[1].x - v[0].x) * (py - v[0].y) - (v[1].y - v[0].y) * (px - v[0].x);
                float e1 = (v[2].x - v[1].x) * (py - v[1].y) - (v[2].y - v[1].y) * (px - v[1].x);
                float e2 = (v[0].x - v[2].x) * (py - v[2].y) - (v[0].y - v[2].y) * (px - v[2].x);
                if (e0 < 0 || e1 < 0 || e2 < 0) continue;
                float w0 = e0 * invArea, w1 = e1 * invArea;
                float w2 = 1.0f - w0 - w1;
                // Depth pre-test before shading.
                float z = v[0].z * w0 + v[1].z * w1 + v[2].z * w2;
                float& dz = (*depth)[(size_t)y * W + x];
                if (!mat.blend) {
                    if (z >= dz) continue;
                }
                float L[3] = {w0, w1, w2};
                ShadeAndStore(x, y, v, L, mat);
            }
        }
    }

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

    void DrawTextQuads(const TextRenderer& text, const Mat4& world) {
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
        Vec2 uv[4];
        float pen = x0;
        for (unsigned char ch : text.text) {
            GlyphRect g;
            font.Glyph(ch, &g);
            float gw = (float)BitmapFont::kGlyphW * scale;
            float gh = (float)BitmapFont::kGlyphH * scale;
            Vec3 w0 = world.TransformPoint(Vec3(pen, 0, 0));
            Vec3 w1 = world.TransformPoint(Vec3(pen + gw, 0, 0));
            Vec3 w2 = world.TransformPoint(Vec3(pen + gw, gh, 0));
            Vec3 w3 = world.TransformPoint(Vec3(pen, gh, 0));
            Vec3 quad[4] = {w0, w1, w2, w3};
            uv[0] = {g.u, g.v};
            uv[1] = {g.u + g.w, g.v};
            uv[2] = {g.u + g.w, g.v + g.h};
            uv[3] = {g.u, g.v + g.h};
            EmitQuad(quad, uv, mat);
            pen += (float)g.advance * scale;
        }
    }
};

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
    R.fogEnabled = cam.fogEnabled;
    R.fogColor = cam.fogColor;
    R.fogDensity = cam.fogDensity;

    R.ClearAndSky(cam);

    // Lights.
    R.lights.clear();
    for (const auto& go : scene.objects) {
        if (!go->ActiveInHierarchy()) continue;
        auto* light = go->GetComponent<LightComponent>();
        if (!light) continue;
        GpuLight gl;
        gl.type = (int)light->type;
        gl.pos = go->transform().WorldPosition();
        gl.dir = go->transform().Forward().Normalized();
        if (gl.dir.LengthSq() < 1e-9f) gl.dir = Vec3(0, -1, 0);
        gl.color = light->color;
        gl.intensity = light->intensity;
        gl.range = light->range;
        R.lights.push_back(gl);
        if (R.lights.size() >= 16) break;
    }

    MeshLibrary& meshes = EngineMeshLibrary();
    TextureLibrary& textures = EngineTextureLibrary();

    R.triVerts.clear();
    R.triMats.clear();

    // ---- Opaque 3D drawables.
    int meshesSeen = 0, clipRejected = 0;
    for (const auto& go : scene.objects) {
        if (!go->ActiveInHierarchy()) continue;
        auto* mr = go->GetComponent<MeshRenderer>();
        if (!mr) continue;
        meshesSeen++;
        std::shared_ptr<Mesh> mesh;
        if (mr->mesh == MeshRenderer::MeshSource::Custom && !mr->meshPath.empty()) {
            mesh = meshes.GetObj(EngineResolvePath(mr->meshPath));
        } else {
            mesh = meshes.Get((PrimitiveMesh)(int)mr->mesh);
        }
        if (!mesh) continue;
        MaterialGpu mat;
        mat.color = mr->albedoColor;
        mat.tiling = mr->tiling;
        mat.specular = mr->specular;
        mat.shininess = mr->shininess;
        mat.unlit = mr->unlit;
        if (!mr->albedoTexturePath.empty()) {
            mat.albedo = textures.Get(EngineResolvePath(mr->albedoTexturePath)).get();
        }
        size_t before = R.triVerts.size();
        R.EmitMesh(*mesh, go->transform().WorldMatrix(), mat);
        if (R.triVerts.size() == before) clipRejected++;
    }

    // ---- Rasterize opaque jobs across the pool.
    const int jobCount = (int)(R.triVerts.size() / 3);
    R.pool.ParallelFor(jobCount, [&R](int i) { R.RasterTriangle(i); });

    // ---- 2D pass: sprites + text, sorted by layer then depth (far first).
    struct DrawItem {
        int order;
        float depthKey;
        bool isText;
        GameObject* go;
    };
    std::vector<DrawItem> items;
    for (const auto& go : scene.objects) {
        if (!go->ActiveInHierarchy()) continue;
        if (go->GetComponent<SpriteRenderer>()) items.push_back({go->GetComponent<SpriteRenderer>()->orderInLayer, go->transform().WorldPosition().z, false, go.get()});
        if (go->GetComponent<TextRenderer>()) items.push_back({go->GetComponent<TextRenderer>()->orderInLayer, go->transform().WorldPosition().z, true, go.get()});
    }
    std::sort(items.begin(), items.end(), [](const DrawItem& a, const DrawItem& b) {
        if (a.order != b.order) return a.order < b.order;
        return a.depthKey > b.depthKey;  // far first
    });

    for (const DrawItem& item : items) {
        GameObject* go = item.go;
        // Sprites and text are rasterized immediately after their quads are
        // emitted so layer sorting stays exact (clipped quads can fan out).
        size_t startIndex = R.triVerts.size();
        if (!item.isText) {
            auto* sr = go->GetComponent<SpriteRenderer>();
            std::shared_ptr<Texture> tex;
            if (!sr->texturePath.empty()) tex = textures.Get(EngineResolvePath(sr->texturePath));
            else tex = textures.GetBuiltin("checker");
            if (!tex) continue;
            float w = tex->width() / std::max(1.0f, sr->pixelsPerUnit);
            float h = tex->height() / std::max(1.0f, sr->pixelsPerUnit);
            if (sr->flipX) w = -w;
            if (sr->flipY) h = -h;
            MaterialGpu mat;
            mat.albedo = tex.get();
            mat.color = sr->color;
            mat.blend = true;
            mat.spriteLighting = true;
            mat.pixelPerfect = sr->pixelPerfect;
            if (!sr->normalMapPath.empty()) {
                mat.normalMap = textures.Get(EngineResolvePath(sr->normalMapPath)).get();
            }
            Vec2 uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            Mat4 world = go->transform().WorldMatrix();
            Vec3 q[4];
            q[0] = world.TransformPoint(Vec3(-w * 0.5f, -h * 0.5f, 0));
            q[1] = world.TransformPoint(Vec3(w * 0.5f, -h * 0.5f, 0));
            q[2] = world.TransformPoint(Vec3(w * 0.5f, h * 0.5f, 0));
            q[3] = world.TransformPoint(Vec3(-w * 0.5f, h * 0.5f, 0));
            R.EmitQuad(q, uv, mat);
        } else {
            auto* tr = go->GetComponent<TextRenderer>();
            R.DrawTextQuads(*tr, go->transform().WorldMatrix());
        }
        int from = (int)(startIndex / 3);
        int to = (int)(R.triVerts.size() / 3);
        for (int i = from; i < to; ++i) R.RasterTriangle(i);
        R.triVerts.resize(startIndex);
        R.triMats.resize(from);
    }

    // ---- Overlay lines (grid, selection, Debug::DrawLine).
    for (const DebugLine& l : overlayLines) R.DrawLine3D(l.a, l.b, l.color);

    RenderStats stats;
    stats.drawCalls = jobCount + (int)items.size() + 1;
    stats.triangles = jobCount;
    stats.meshesSeen = meshesSeen;
    stats.clipRejected = clipRejected;
    return stats;
}

}  // namespace Cairn
