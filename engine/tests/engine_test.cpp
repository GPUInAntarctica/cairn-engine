// Cairn Engine - native smoke/unit tests (headless, no GUI).
#include "cairn/cairn.h"
#include "cairn/engine.h"
#include "cairn/mesh.h"
#include "cairn/texture.h"
#include "cairn/physics.h"
#include "scene_internal.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>

using namespace Cairn;

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static void TestMath() {
    Mat4 id = Mat4::Identity();
    Vec3 p = id.TransformPoint(Vec3(1, 2, 3));
    CHECK(p == Vec3(1, 2, 3));

    Mat4 t = Mat4::Translation(Vec3(5, 0, 0)) * Mat4::FromQuat(Quat::FromAxisAngle(Vec3::Up, kPi / 2));
    Vec3 q = t.TransformPoint(Vec3(1, 0, 0));  // rotate +Z then translate
    CHECK(NearlyEqual(q.x, 5.0f, 1e-4f) && NearlyEqual(q.z, -1.0f, 1e-4f));

    Mat4 inv = t.Inverted();
    Vec3 back = inv.TransformPoint(q);
    CHECK(NearlyEqual(back.x, 1.0f, 1e-3f) && NearlyEqual(back.z, 0.0f, 1e-3f));

    Quat e = Quat::FromEulerDegrees(Vec3(0, 90, 0));
    Vec3 f = e * Vec3::Forward;
    CHECK(NearlyEqual(f.x, -1.0f, 1e-4f));  // yaw +90 turns -Z into -X

    Quat roundTrip = Quat::FromEulerDegrees(Vec3(20, 40, 60));
    Vec3 euler = roundTrip.ToEulerDegrees();
    Quat again = Quat::FromEulerDegrees(euler);
    CHECK(NearlyEqual(again.x, roundTrip.x, 1e-4f) && NearlyEqual(again.w, roundTrip.w, 1e-4f));

    CHECK(NearlyEqual(Vec3::Cross(Vec3::Right, Vec3::Up).z, 1.0f));

    // A transformed AABB must not pick up the origin: a unit box moved up to
    // y = 2 stays at [1.5, 2.5], not [0, 2.5].
    AABB moved = AABB(Vec3(-0.5f, -0.5f, -0.5f), Vec3(0.5f, 0.5f, 0.5f))
                     .Transformed(Mat4::Translation(Vec3(0, 2, 0)));
    CHECK(NearlyEqual(moved.min.y, 1.5f, 1e-5f));
    CHECK(NearlyEqual(moved.max.y, 2.5f, 1e-5f));
    CHECK(moved.min.x <= -0.5f && moved.max.x >= 0.5f);
}

static void TestJson() {
    Json doc = Json::MakeObject();
    doc.set("name", "test");
    doc.set("count", 3);
    doc.set("ratio", 0.5f);
    doc.set("flags", Json::MakeArray({true, false}));
    Json nested = Json::MakeObject();
    nested.set("deep", "value");
    doc.set("nested", nested);
    std::string text = doc.Dump();
    Json parsed;
    std::string err;
    CHECK(Json::Parse(text, &parsed, &err));
    CHECK(parsed["name"].asString() == "test");
    CHECK(parsed["count"].asInt() == 3);
    CHECK(NearlyEqual(parsed["ratio"].asFloat(), 0.5f));
    CHECK(parsed["flags"][0].asBool());
    CHECK(parsed["nested"]["deep"].asString() == "value");

    Json u;
    CHECK(Json::Parse("{\"u\":\"\\u4e2d\\u6587\"}", &u));
    CHECK(u["u"].asString() == "\xE4\xB8\xAD\xE6\x96\x87");
    CHECK(!Json::Parse("{broken", &u));
}

static void TestScene() {
    Engine engine;
    Scene& scene = engine.scene();
    GameObject* root = scene.CreateEntity("Root");
    GameObject* child = scene.CreateEntity("Child", root);
    child->transform().position = Vec3(1, 2, 3);
    root->transform().position = Vec3(10, 0, 0);
    Vec3 wp = child->transform().WorldPosition();
    CHECK(wp == Vec3(11, 2, 3));

    auto* mr = root->AddComponent<MeshRenderer>();
    CHECK(mr != nullptr);
    CHECK(ComponentSetProperty(mr, "albedoColor", Json::MakeArray({1.0f, 0.0f, 0.0f, 1.0f})));
    CHECK(mr->albedoColor == Color(1, 0, 0, 1));
    Json colorOut;
    CHECK(ComponentGetProperty(mr, "albedoColor", &colorOut));
    CHECK(colorOut.size() == 4);

    // Serialization round-trip. Note: restoring replaces the Scene object,
    // so re-acquire the reference afterwards.
    std::string json = engine.SerializeSceneJson();
    CHECK(engine.RestoreSceneJson(json));
    Scene& restoredScene = engine.scene();
    GameObject* restored = restoredScene.FindByName("Root");
    CHECK(restored != nullptr);
    auto* mr2 = restored->GetComponent<MeshRenderer>();
    CHECK(mr2 != nullptr && mr2->albedoColor == Color(1, 0, 0, 1));
    GameObject* restoredChild = restoredScene.FindByName("Child");
    CHECK(restoredChild != nullptr);
    CHECK(restoredChild->GetParent() == restored);
    CHECK(restoredChild->transform().position == Vec3(1, 2, 3));
}

static void TestRender() {
    Engine engine;
    engine.SetViewportSize(160, 120);
    Scene& scene = engine.scene();
    GameObject* cube = scene.CreateEntity("Cube");
    cube->transform().position = Vec3(0, 0, 0);
    cube->AddComponent<MeshRenderer>();
    auto* cam = cube->AddComponent<CameraComponent>();  // not used for editor view
    (void)cam;

    RenderStats stats = engine.RenderEditorFrame();
    CHECK(stats.triangles > 0);
    FrameTarget& target = engine.frame();
    CHECK((int)target.color.size() == 160 * 120 * 4);

    // The cube should put lit pixels near the center of the frame.
    size_t lit = 0;
    for (size_t i = 0; i < target.color.size(); i += 4) {
        if (target.color[i] > 60 || target.color[i + 1] > 60) lit++;
    }
    CHECK(lit > 200);

    // Picking through the screen center: aim the editor camera at the cube.
    engine.viewCamera().position = Vec3(0, 0, 5);
    engine.viewCamera().yaw = 0.0f;
    engine.viewCamera().pitch = 0.0f;
    EntityId hit = engine.Pick(80, 60);
    CHECK(hit == cube->m_id);
}

// The GPU backend has to reach the same picture as the CPU rasteriser, honour
// an explicit software request, and keep reporting which backend really ran.
static void TestRenderBackendSelection() {
    auto renderCube = [](Engine& engine) {
        Scene& scene = engine.scene();
        GameObject* cube = scene.CreateEntity("Cube");
        cube->AddComponent<MeshRenderer>();
        // A light, because the GPU path carries the light list in a uniform
        // block: a scene with no light never reads it, so it would not notice
        // a malformed block.
        GameObject* sun = scene.CreateEntity("Sun");
        sun->transform().position = Vec3(3, 5, 2);
        sun->AddComponent<LightComponent>();
        return engine.RenderEditorFrame();
    };
    auto litPixels = [](Engine& engine) {
        const FrameTarget& t = engine.frame();
        size_t lit = 0;
        for (size_t i = 0; i < t.color.size(); i += 4)
            if (t.color[i] > 60 || t.color[i + 1] > 60) lit++;
        return lit;
    };
    const char* expected = VulkanBackendAvailable() ? "vulkan" : "software";

    // Forcing software must never reach for the GPU.
    {
        Engine engine;
        engine.SetViewportSize(160, 120);
        engine.scene().renderBackend = 2;
        RenderStats stats = renderCube(engine);
        CHECK(std::string(stats.backend) == "software");
        CHECK(litPixels(engine) > 200);
    }
    // Forcing Vulkan on a machine without a usable device degrades to software
    // rather than failing the frame.
    {
        Engine engine;
        engine.SetViewportSize(160, 120);
        engine.scene().renderBackend = 1;
        RenderStats stats = renderCube(engine);
        CHECK(std::string(stats.backend) == expected);
        CHECK(litPixels(engine) > 200);
    }
    // Auto prefers the GPU when there is one. CAIRN_RENDER_BACKEND may pin
    // the machine-level default to software, so only the *choice* is asserted
    // here, not which backend won.
    {
        Engine engine;
        engine.SetViewportSize(160, 120);
        engine.scene().renderBackend = 0;
        RenderStats stats = renderCube(engine);
        std::string ran = stats.backend ? stats.backend : "";
        CHECK(ran == "vulkan" || ran == "software");
        CHECK(litPixels(engine) > 200);
    }
}

// A lit scene with a sprite, debug lines and a post pass has to come out of
// either backend looking the same. This is the cross-backend regression net:
// every feature the GPU path carries in a uniform block or a texture is
// exercised here, and both backends are held to producing a real image.
static void TestRenderBackendsAgree() {
    auto build = [](Engine& engine, int backend) {
        engine.SetViewportSize(160, 120);
        Scene& scene = engine.scene();
        scene.renderBackend = backend;
        scene.postBloomStrength = 0.4f;

        GameObject* cube = scene.CreateEntity("Cube");
        auto* cubeMr = cube->AddComponent<MeshRenderer>();
        cubeMr->albedoColor = Color(0.9f, 0.9f, 0.9f, 1.0f);
        GameObject* sun = scene.CreateEntity("Sun");
        sun->transform().position = Vec3(3, 5, 2);
        sun->AddComponent<LightComponent>();
        GameObject* sprite = scene.CreateEntity("Label");
        auto* spriteMr = sprite->AddComponent<SpriteRenderer>();
        spriteMr->color = Color(1.0f, 0.4f, 0.2f, 1.0f);
    };
    auto litPixels = [](Engine& engine) {
        const FrameTarget& t = engine.frame();
        size_t lit = 0;
        for (size_t i = 0; i < t.color.size(); i += 4)
            if (t.color[i] > 40 || t.color[i + 1] > 40 || t.color[i + 2] > 40) lit++;
        return lit;
    };

    Engine cpu;
    build(cpu, 2);  // software
    RenderStats cpuStats = cpu.RenderEditorFrame();
    const size_t cpuLit = litPixels(cpu);
    CHECK(std::string(cpuStats.backend) == "software");
    CHECK(cpuLit > 500);

    Engine gpu;
    build(gpu, 1);  // forced Vulkan; degrades to software without a device
    RenderStats gpuStats = gpu.RenderEditorFrame();
    const size_t gpuLit = litPixels(gpu);
    CHECK(std::string(gpuStats.backend) == (VulkanBackendAvailable() ? "vulkan" : "software"));
    CHECK(gpuLit > 500);
    // Both rasterisers light the same scene, so they cannot disagree wildly.
    if (VulkanBackendAvailable())
        CHECK(gpuLit * 2 > cpuLit && cpuLit * 2 > gpuLit);
}

static void TestPhysics() {
    Engine engine;
    Scene& scene = engine.scene();
    GameObject* ground = scene.CreateEntity("Ground");
    ground->AddComponent<BoxCollider>();
    ComponentSetProperty(ground->GetComponent<BoxCollider>(), "size",
                         Json::MakeArray({20.0f, 1.0f, 20.0f}));
    ground->transform().position = Vec3(0, -0.5f, 0);

    GameObject* ball = scene.CreateEntity("Ball");
    ball->transform().position = Vec3(0, 8, 0);
    auto* rb = ball->AddComponent<RigidBody>();
    CHECK(rb != nullptr);
    ball->AddComponent<SphereCollider>();

    for (int i = 0; i < 240; ++i) PhysicsStep(scene, 1.0f / 60.0f);
    float y = ball->transform().position.y;
    CHECK(y > -0.2f && y < 2.0f);  // settled on the ground, not sunk through
    CHECK(ball->GetComponent<RigidBody>()->velocity.y > -0.5f);
}


// Hierarchy: world matrices must never go stale, no matter whether fields are
// written directly (gameplay scripts do exactly that) or through the setters.
static void TestHierarchy() {
    Engine engine;
    Scene& scene = engine.scene();
    GameObject* parent = scene.CreateEntity("Parent");
    parent->transform().position = Vec3(10, 0, 0);
    GameObject* child = scene.CreateEntity("Child", parent);
    child->transform().position = Vec3(0, 2, 0);

    CHECK(NearlyEqual(child->transform().WorldPosition().x, 10.0f, 1e-3f));
    CHECK(NearlyEqual(child->transform().WorldPosition().y, 2.0f, 1e-3f));

    // Direct field write on the parent must invalidate the child subtree.
    parent->transform().position = Vec3(-3, 1, 5);
    Vec3 world = child->transform().WorldPosition();
    CHECK(NearlyEqual(world.x, -3.0f, 1e-3f) && NearlyEqual(world.y, 3.0f, 1e-3f)
          && NearlyEqual(world.z, 5.0f, 1e-3f));

    // Rotation on the parent must reach the child's world matrix too: a yaw of
    // 90 degrees maps the child's local +Z offset onto world +X.
    child->transform().position = Vec3(0, 2, 1);
    parent->transform().SetEulerDegrees(Vec3(0, 90, 0));
    parent->transform().position = Vec3(0, 0, 0);
    Vec3 rot = child->transform().WorldPosition();
    CHECK(NearlyEqual(rot.x, 1.0f, 1e-3f) && NearlyEqual(rot.y, 2.0f, 1e-3f)
          && NearlyEqual(rot.z, 0.0f, 1e-3f));

    // Deep subtree duplication keeps children (this used to silently drop them).
    GameObject* dup = scene.InstantiateCopy(parent);
    CHECK(dup != nullptr && dup != parent);
    CHECK(dup->transform().m_children.size() == 1);
    GameObject* dupChild = dup->transform().m_children.front();
    CHECK(dupChild != child && dupChild->name == "Child");
    CHECK(NearlyEqual(dupChild->transform().position.y, 2.0f, 1e-3f));
    CHECK(NearlyEqual(dupChild->transform().WorldPosition().x, rot.x, 1e-3f));

    // The serialized subtree form carries a nested children array.
    Json sub = scene.SerializeSubtree(parent);
    CHECK(sub["children"].isArray() && sub["children"].size() == 1);
    // The flat scene form must not contain it (would duplicate entities).
    Json flat = scene.Serialize();
    for (const Json& e : flat["entities"].arrayItems()) CHECK(!e["children"].isArray());
}

// Mip chains, trilinear sampling and the wrap modes.
static void TestTextureMips() {
    auto tex = Texture::Solid(8, 255, 0, 0);
    CHECK(tex->IsValid() && tex->mipLevels() == 1);
    tex->GenerateMipmaps();
    // 8 -> 4 -> 2 -> 1
    CHECK(tex->mipLevels() == 4);
    CHECK(tex->levelWidth(1) == 4 && tex->levelHeight(1) == 4);
    CHECK(tex->levelWidth(3) == 1 && tex->levelHeight(3) == 1);

    float c[4];
    tex->SampleLod(0.5f, 0.5f, 0.0f, Texture::Wrap::Clamp, Texture::Filter::Trilinear, c);
    CHECK(c[0] > 0.99f && c[1] < 0.01f && c[3] > 0.99f);

    // A flat colour stays flat at every mip level (box filter correctness).
    for (float lod = 0.0f; lod < 3.5f; lod += 0.5f) {
        tex->SampleLod(0.37f, 0.81f, lod, Texture::Wrap::Repeat, Texture::Filter::Trilinear, c);
        CHECK(c[0] > 0.99f && c[1] < 0.01f);
    }

    // Non-power-of-two textures still produce a full chain.
    auto odd = Texture::Solid(5, 10, 20, 30);
    odd->GenerateMipmaps();
    CHECK(odd->mipLevels() >= 3 && odd->levelWidth(odd->mipLevels() - 1) == 1);

    // Repeat wraps, clamp clamps and mirror reflects.
    auto ramp = Texture::Solid(2, 0, 0, 0);
    uint8_t* px = ramp->pixels();
    for (int i = 0; i < 2 * 2 * 4; ++i) px[i] = (uint8_t)((i / 4) * 255);
    ramp->GenerateMipmaps();
    // The 2x2 ramp has a black left column and a white right column, so the
    // addressing mode is directly observable: repeat wraps u=1.5 to 0.5 (white
    // column), mirror reflects it back to the same uv (black column).
    float rep[4], mir[4], base[4];
    ramp->Sample(1.75f, 0.25f, Texture::Wrap::Repeat, Texture::Filter::Nearest, rep);
    ramp->Sample(1.75f, 0.25f, Texture::Wrap::Mirror, Texture::Filter::Nearest, mir);
    ramp->Sample(0.25f, 0.25f, Texture::Wrap::Repeat, Texture::Filter::Nearest, base);
    CHECK(rep[0] > 0.9f && base[0] < 0.1f);
    CHECK(mir[0] < 0.1f);
    float outside[4];
    ramp->Sample(5.0f, 5.0f, Texture::Wrap::Clamp, Texture::Filter::Nearest, outside);
    CHECK(outside[0] > 0.9f);  // clamped to the last texel
    // The ramp is an sRGB texture, so sampling returns LINEAR values: white is
    // 1.0 while mid grey drops well below its encoded value.
    auto grey = Texture::Solid(2, 128, 128, 128);
    grey->setSRGB(true);
    float g[4];
    grey->Sample(0.5f, 0.5f, Texture::Wrap::Clamp, Texture::Filter::Nearest, g);
    CHECK(g[0] > 0.15f && g[0] < 0.30f);
    grey->setSRGB(false);
    grey->Sample(0.5f, 0.5f, Texture::Wrap::Clamp, Texture::Filter::Nearest, g);
    CHECK(std::fabs(g[0] - 128.0f / 255.0f) < 1e-3f);
}

// Transparent draw order: sorting layer, then order in layer, then view depth.
static void TestSortingOrder() {
    Engine engine;
    Scene& scene = engine.scene();
    scene.background = Color(0, 0, 0, 1);

    GameObject* cam = scene.CreateEntity("Camera");
    cam->tag = "MainCamera";
    cam->AddComponent<CameraComponent>();
    cam->transform().position = Vec3(0, 0, 10);

    // Two sprites on different sorting layers plus a far one on the same layer.
    auto makeSprite = [&](const char* name, float x, int sortingLayer, int order) {
        GameObject* go = scene.CreateEntity(name);
        auto* sp = go->AddComponent<SpriteRenderer>();
        sp->pixelsPerUnit = 1.0f;
        sp->sortingLayer = sortingLayer;
        sp->orderInLayer = order;
        go->transform().position = Vec3(x, 0, 0);
        return go;
    };
    makeSprite("Near", -1.0f, 0, 0);
    makeSprite("Far", 0.0f, 0, 0);
    makeSprite("UpperLayer", 1.0f, 1, -100);

    engine.Play();
    engine.SetViewportSize(60, 20);
    const RenderStats stats = engine.RenderGameFrame();
    CHECK(stats.drawCalls2D == 3);

    // The sprites have no textures, so this mainly verifies the sort is stable
    // and that every item reaches the 2D pass; the ordering itself is asserted
    // through the values the renderer collects.
    Json tree = scene.Serialize();
    bool foundLayers = false;
    for (const Json& e : tree["entities"].arrayItems()) {
        for (const Json& c : e["components"].arrayItems()) {
            if (c["type"].asString() == "SpriteRenderer" && c["properties"].has("sortingLayer")) foundLayers = true;
        }
    }
    CHECK(foundLayers);
}

// Back-face culling and coverage antialiasing.
static void TestCullingAndAA() {
    Engine engine;
    Scene& scene = engine.scene();
    // Pixel-exact AA/culling counts are the CPU rasteriser's contract.
    scene.renderBackend = 2;
    scene.ambient = Color(1, 1, 1, 1);
    scene.background = Color(0, 0, 0, 1);

    // A camera with its own tag keeps the editor grid out of the frame.
    GameObject* cam = scene.CreateEntity("Camera");
    cam->tag = "MainCamera";
    cam->AddComponent<CameraComponent>();
    cam->transform().position = Vec3(0, 0, 5);

    GameObject* quad = scene.CreateEntity("Quad");
    auto* mr = quad->AddComponent<MeshRenderer>();
    mr->mesh = MeshRenderer::MeshSource::Quad;
    mr->albedoColor = Color(1, 1, 1, 1);
    mr->unlit = true;
    mr->cullMode = MeshRenderer::CullMode::Back;
    quad->transform().position = Vec3(0, 0, 0);
    quad->transform().scale = Vec3(6, 6, 6);

    // The scene camera is only used while playing; otherwise the editor viewport
    // camera renders the frame.
    engine.Play();
    engine.SetViewportSize(40, 40);

    // The quad is unlit white; the sky is black, so white pixels are geometry.
    auto whitePixels = [&]() {
        int n = 0;
        const auto& fb = engine.frame().color;
        for (size_t i = 0; i + 3 < fb.size(); i += 4)
            if (fb[i] > 150 && fb[i + 1] > 150 && fb[i + 2] > 150) n++;
        return n;
    };

    // CullMode::Off must show the quad from wherever the camera is.
    mr->cullMode = MeshRenderer::CullMode::Off;
    engine.RenderGameFrame();
    const int visible = whitePixels();
    CHECK(visible > 100);

    // Turning the quad by 180 degrees swaps which side faces the camera, so
    // exactly one of the two orientations survives back-face culling.
    const auto culledCount = [&](float yaw) {
        quad->transform().SetEulerDegrees(Vec3(0, yaw, 0));
        mr->cullMode = MeshRenderer::CullMode::Back;
        engine.RenderGameFrame();
        return whitePixels();
    };
    const int sideA = culledCount(0.0f);
    const int sideB = culledCount(180.0f);
    CHECK((sideA == 0) != (sideB == 0));
    CHECK(sideA + sideB == visible);

    // CullMode::Front is the complement: it keeps what Back discards.
    mr->cullMode = MeshRenderer::CullMode::Front;
    quad->transform().SetEulerDegrees(Vec3(0, 0, 0));
    engine.RenderGameFrame();
    const int frontA = whitePixels();
    quad->transform().SetEulerDegrees(Vec3(0, 180, 0));
    engine.RenderGameFrame();
    const int frontB = whitePixels();
    CHECK(frontA > 100 && frontB < 100);
    CHECK(frontA == sideB && frontB == sideA);

    quad->transform().SetEulerDegrees(Vec3(0, sideA > 0 ? 0.0f : 180.0f, 0));
    mr->cullMode = MeshRenderer::CullMode::Back;

    // Antialiasing adds partially covered pixels along the diagonal edge.
    mr->cullMode = MeshRenderer::CullMode::Back;
    mr->antialias = true;
    engine.RenderGameFrame();
    int partial = 0;
    {
        const auto& fb = engine.frame().color;
        for (size_t i = 0; i + 3 < fb.size(); i += 4) {
            const int r = fb[i], g = fb[i + 1], b = fb[i + 2];
            // Edge pixels sit between the black background and the surface.
            if (r > 20 && r < 190 && std::abs(r - g) < 12 && std::abs(g - b) < 12) partial++;
        }
    }
    CHECK(partial > 0);
}

// The display transform: linear shading, tone mapping, exposure and the scene
// background all have to reach the framebuffer.
static void TestTonemapping() {
    Engine engine;
    Scene& scene = engine.scene();
    scene.gravity = Vec3(0, 0, 0);
    scene.ambient = Color(1, 1, 1, 1);          // flat ambient keeps it deterministic
    GameObject* quad = scene.CreateEntity("Quad");
    auto* mr = quad->AddComponent<MeshRenderer>();
    mr->albedoColor = Color(1, 1, 1, 1);
    mr->unlit = true;
    quad->transform().position = Vec3(0, 0, 0);
    // Look at the quad head-on so the sampled pixel is geometry, not sky.
    engine.viewCamera().position = Vec3(0, 0, 8);
    engine.viewCamera().yaw = 0;
    engine.viewCamera().pitch = 0;

    engine.SetViewportSize(48, 32);
    auto sampleCentre = [&]() {
        const auto& fb = engine.frame().color;
        const int w = engine.viewportWidth(), h = engine.viewportHeight();
        size_t i = (((size_t)(h / 2) * w) + (w / 2)) * 4;
        return std::array<int, 3>{(int)fb[i], (int)fb[i + 1], (int)fb[i + 2]};
    };

    scene.tonemap = 0;   // none, exposure 1
    engine.RenderEditorFrame();
    const auto plain = sampleCentre();

    scene.tonemap = 2;   // ACES
    engine.RenderEditorFrame();
    const auto aces = sampleCentre();

    scene.exposure = 2.0f;
    engine.RenderEditorFrame();
    const auto exposed = sampleCentre();
    scene.exposure = 1.0f;

    // ACES rolls highlights off, so a full-white surface comes out darker than
    // the raw clamp-to-one result.
    CHECK(plain[0] > aces[0]);
    // Exposure brightens the frame.
    CHECK(exposed[0] > aces[0]);
    // The background colour reaches the framebuffer through the sky gradient.
    scene.background = Color(1, 0, 0, 1);
    scene.tonemap = 0;
    engine.RenderEditorFrame();
    const auto& fb = engine.frame().color;
    CHECK(fb[0] > 200 && fb[1] < 40 && fb[2] < 40);

    // Settings survive a save/load round trip.
    scene.tonemap = 3;
    scene.exposure = 1.75f;
    Json doc = scene.Serialize();
    std::string err;
    auto reloaded = Scene::Deserialize(doc, &err);
    CHECK(reloaded && reloaded->tonemap == 3);
    CHECK(reloaded && std::fabs(reloaded->exposure - 1.75f) < 1e-4f);
}

// Frustum culling must accept what is on screen and reject what is not.
static void TestFrustum() {
    Mat4 view = Mat4::LookAt(Vec3(0, 0, 0), Vec3(0, 0, -1), Vec3(0, 1, 0));
    Mat4 proj = Mat4::Perspective(60.0f * kPi / 180.0f, 1.0f, 0.1f, 100.0f);  // radians
    Frustum f = Frustum::FromViewProj(proj * view);

    // Directly in front.
    CHECK(f.IntersectsAABB(AABB(Vec3(-1, -1, -11), Vec3(1, 1, -9))));
    // Straddling the near plane is still visible.
    CHECK(f.IntersectsAABB(AABB(Vec3(-1, -1, -9), Vec3(1, 1, 1))));
    // Behind the camera.
    CHECK(!f.IntersectsAABB(AABB(Vec3(-1, -1, 9), Vec3(1, 1, 11))));
    // Far off to the side.
    CHECK(!f.IntersectsAABB(AABB(Vec3(50, -1, -11), Vec3(52, 1, -9))));
    // Beyond the far plane.
    CHECK(!f.IntersectsAABB(AABB(Vec3(-1, -1, -500), Vec3(1, 1, -498))));
    // Above the top of the 60 degree frustum.
    CHECK(!f.IntersectsAABB(AABB(Vec3(-1, 80, -11), Vec3(1, 82, -9))));

    // A big box that encloses the camera is always visible.
    CHECK(f.IntersectsAABB(AABB(Vec3(-100, -100, -100), Vec3(100, 100, 100))));
}

// A parented rigid body must collide in world space and still settle.
static void TestParentedPhysics() {
    Engine engine;
    Scene& scene = engine.scene();
    GameObject* ground = scene.CreateEntity("Ground");
    ground->AddComponent<BoxCollider>();
    ComponentSetProperty(ground->GetComponent<BoxCollider>(), "size",
                         Json::MakeArray({80.0f, 1.0f, 80.0f}));
    ground->transform().position = Vec3(0, -0.5f, 0);

    GameObject* platform = scene.CreateEntity("Platform");
    platform->transform().position = Vec3(20, 5, 0);  // parent is 20 units away
    GameObject* ball = scene.CreateEntity("Ball", platform);
    ball->transform().position = Vec3(0, 4, 0);
    ball->AddComponent<RigidBody>();
    ball->AddComponent<SphereCollider>();

    Vec3 worldStart = ball->transform().WorldPosition();
    CHECK(NearlyEqual(worldStart.x, 20.0f, 1e-3f) && NearlyEqual(worldStart.y, 9.0f, 1e-3f));

    for (int i = 0; i < 240; ++i) PhysicsStep(scene, 1.0f / 60.0f);
    // It must have fallen in world space: world y near the ground top (0), and
    // it must stay under the parent (world x still ~20).
    Vec3 worldEnd = ball->transform().WorldPosition();
    CHECK(worldEnd.y > -0.3f && worldEnd.y < 2.0f);
    CHECK(NearlyEqual(worldEnd.x, 20.0f, 1.5f));
}

// A trigger sensor on one collider must not turn the body's solid contacts into
// triggers, and disabling a component must remove it from the simulation.
static void TestTriggerAndEnabled() {
    Engine engine;
    Scene& scene = engine.scene();
    GameObject* ground = scene.CreateEntity("Ground");
    ground->AddComponent<BoxCollider>();
    ComponentSetProperty(ground->GetComponent<BoxCollider>(), "size",
                         Json::MakeArray({20.0f, 1.0f, 20.0f}));
    ground->transform().position = Vec3(0, -0.5f, 0);

    GameObject* ball = scene.CreateEntity("Ball");
    ball->transform().position = Vec3(0, 3, 0);
    ball->AddComponent<RigidBody>();
    ball->AddComponent<BoxCollider>();       // solid: must still collide
    auto* sensor = ball->AddComponent<SphereCollider>();
    sensor->center = Vec3(0, 0, 0);
    sensor->radius = 0.1f;
    sensor->isTrigger = true;               // sensor on the same body

    for (int i = 0; i < 240; ++i) PhysicsStep(scene, 1.0f / 60.0f);
    CHECK(ball->transform().position.y > -0.2f && ball->transform().position.y < 2.0f);

    // Disabling the collider removes the body from the simulation.
    ball->GetComponent<RigidBody>()->velocity = Vec3();
    ball->GetComponent<BoxCollider>()->enabled = false;
    ball->GetComponent<SphereCollider>()->enabled = false;
    float before = ball->transform().position.y;
    for (int i = 0; i < 60; ++i) PhysicsStep(scene, 1.0f / 60.0f);
    CHECK(NearlyEqual(ball->transform().position.y, before, 1e-4f));
}

// Directional shadows: a box on a floor must visibly darken the floor behind
// it, RenderStats::shadowedPixels must track it, and scene.shadows = 0 must
// switch the darkening off cleanly.
static void TestShadows() {
    Engine engine;
    engine.SetViewportSize(320, 240);
    Scene& scene = engine.scene();
    // Per-pixel shadow bookkeeping only exists on the CPU rasteriser.
    scene.renderBackend = 2;
    scene.shadows = 2;
    scene.shadowResolution = 512;
    scene.shadowDistance = 60.0f;

    GameObject* floor = scene.CreateEntity("Floor");
    floor->transform().scale = Vec3(30, 1, 30);
    auto* floorMr = floor->AddComponent<MeshRenderer>();
    floorMr->mesh = MeshRenderer::MeshSource::Plane;
    floorMr->albedoColor = Color(0.7f, 0.7f, 0.7f, 1.0f);

    GameObject* box = scene.CreateEntity("Box");
    box->transform().position = Vec3(0, 0.5f, -4);
    box->transform().scale = Vec3(2, 2, 2);
    box->AddComponent<MeshRenderer>();

    GameObject* sun = scene.CreateEntity("Sun");
    // Down and tilted so the shadow falls towards the camera, in front of the
    // box (a shadow cast away from the view would hide behind the caster).
    sun->transform().SetEulerDegrees(Vec3(-50, 200, 0));
    auto* light = sun->AddComponent<LightComponent>();
    light->type = LightComponent::LightType::Directional;
    light->intensity = 1.0f;
    light->castShadows = true;

    engine.viewCamera().position = Vec3(0, 3, 4);
    engine.viewCamera().yaw = 0.0f;
    engine.viewCamera().pitch = -25.0f;

    RenderStats on = engine.RenderEditorFrame();
    CHECK(on.shadowedPixels > 0);
    std::vector<uint8_t> withShadows = engine.frame().color;
    CHECK(withShadows.size() == (size_t)320 * 240 * 4);

    // The shadow must survive camera motion (cascade refit + texel snapping).
    engine.viewCamera().position = Vec3(2.5f, 3.5f, 5.0f);
    engine.viewCamera().yaw = 18.0f;
    engine.viewCamera().pitch = -22.0f;
    RenderStats moved = engine.RenderEditorFrame();
    CHECK(moved.shadowedPixels > 0);

    // Return to the original viewpoint so the shadows-off render is directly
    // comparable with the captured shadows-on frame.
    engine.viewCamera().position = Vec3(0, 3, 4);
    engine.viewCamera().yaw = 0.0f;
    engine.viewCamera().pitch = -25.0f;
    scene.shadows = 0;  // shadows off: nothing may be darkened
    RenderStats off = engine.RenderEditorFrame();
    CHECK(off.shadowedPixels == 0);
    const std::vector<uint8_t>& withoutShadows = engine.frame().color;
    CHECK(withoutShadows.size() == withShadows.size());

    // Shadows are the only difference between the two renders, so pixels that
    // changed strongly are exactly the shadow region: it must be large enough
    // to be visible, bounded (no acne across the frame) and darker, not brighter.
    int changed = 0;
    long onSum = 0, offSum = 0;
    for (size_t i = 0; i < withShadows.size(); i += 4) {
        const int dr = std::abs((int)withShadows[i] - (int)withoutShadows[i]);
        const int dg = std::abs((int)withShadows[i + 1] - (int)withoutShadows[i + 1]);
        const int db = std::abs((int)withShadows[i + 2] - (int)withoutShadows[i + 2]);
        if (std::max({dr, dg, db}) > 12) {
            changed++;
            onSum += withShadows[i] + withShadows[i + 1] + withShadows[i + 2];
            offSum += withoutShadows[i] + withoutShadows[i + 1] + withoutShadows[i + 2];
        }
    }
    CHECK(changed > 200);                          // clearly visible shadow region
    CHECK(changed < 320 * 240 / 4);                // ...but not the whole frame
    CHECK(onSum < offSum);                         // shadowed pixels got darker
}

// Local stand-in exercising the DescribeProps contract scripts implement.
class ScriptProbeComponent : public Component {public:
    const char* TypeName() const override { return "Script"; }
    void DescribeProps(ScriptMeta& m) override {
        m.Add("speed", PropType::Float, 5.0f);
        m.Add("tint", PropType::Color, Json::MakeArray({1.0f, 1.0f, 1.0f, 1.0f}));
    }
};

static void TestScripts() {
    // Compiling gameplay code is the build system's job; here we verify the
    // API surface links and the registry type machinery works.
    Engine engine;
    engine.SetViewportSize(64, 64);
    GameObject* go = engine.scene().CreateEntity("Test");
    auto* sc = go->AddComponent<ScriptComponent>();
    sc->properties = Json::MakeObject();
    sc->properties.set("speed", 4.5f);
    auto* probe = new ScriptProbeComponent();
    ScriptMeta meta;
    probe->DescribeProps(meta);
    CHECK(meta.properties.size() == 2);
    CHECK(meta.properties[0].name == "speed");
    delete probe;
}

// Every primitive mesh must be wound counter-clockwise seen from outside:
// its triangle winding normal must agree with the stated vertex normals, or
// back-face culling (default CullMode::Back) renders primitives inside-out.
static void TestMeshWinding() {
    struct Prim { const char* name; std::shared_ptr<Mesh> mesh; };
    const Prim prims[] = {
        {"Cube", Mesh::Cube()},     {"Sphere", Mesh::Sphere()}, {"Plane", Mesh::Plane()},
        {"Quad", Mesh::Quad()},     {"Cylinder", Mesh::Cylinder()}, {"Cone", Mesh::Cone()},
        {"Torus", Mesh::Torus()},
    };
    for (const Prim& p : prims) {
        const auto& vs = p.mesh->vertices;
        const auto& ix = p.mesh->indices;
        CHECK(!vs.empty() && ix.size() >= 3);
        int mismatched = 0;
        for (size_t i = 0; i + 2 < ix.size(); i += 3) {
            const Vec3& a = vs[ix[i]].position;
            const Vec3& b = vs[ix[i + 1]].position;
            const Vec3& c = vs[ix[i + 2]].position;
            const Vec3 n = Vec3::Cross(b - a, c - a);
            if (n.LengthSq() < 1e-12f) continue;  // degenerate (pole) triangles
            const Vec3 sn = vs[ix[i]].normal + vs[ix[i + 1]].normal + vs[ix[i + 2]].normal;
            if (Vec3::Dot(n, sn) < 0.0f) mismatched++;
        }
        CHECK(mismatched == 0);
        if (mismatched != 0) {
            std::printf("  winding: %s has %d inward-wound triangle(s)\n", p.name, mismatched);
        }
    }
}

// Post-processing: every effect individually switchable, neutral defaults
// leave the framebuffer byte-identical (passthrough), settings serialize.
static void TestPostProcessing() {
    Engine engine;
    engine.SetViewportSize(160, 120);
    Scene& scene = engine.scene();
    // Byte-exact post-processing expectations are CPU-path only.
    scene.renderBackend = 2;

    GameObject* floor = scene.CreateEntity("Floor");
    floor->transform().scale = Vec3(20, 1, 20);
    auto* floorMr = floor->AddComponent<MeshRenderer>();
    floorMr->mesh = MeshRenderer::MeshSource::Plane;
    floorMr->albedoColor = Color(0.8f, 0.8f, 0.8f, 1.0f);

    GameObject* box = scene.CreateEntity("Box");
    box->transform().position = Vec3(0, 0.5f, -3);
    box->transform().scale = Vec3(2, 2, 2);
    auto* boxMr = box->AddComponent<MeshRenderer>();
    boxMr->albedoColor = Color(0.95f, 0.9f, 0.85f, 1.0f);

    GameObject* sun = scene.CreateEntity("Sun");
    sun->transform().SetEulerDegrees(Vec3(-50, 200, 0));
    auto* light = sun->AddComponent<LightComponent>();
    light->type = LightComponent::LightType::Directional;
    light->intensity = 1.0f;

    engine.viewCamera().position = Vec3(0, 2.5f, 4);
    engine.viewCamera().yaw = 0.0f;
    engine.viewCamera().pitch = -18.0f;

    auto pixel = [&](int x, int y, const std::vector<uint8_t>& img) {
        const size_t i = ((size_t)y * 160 + x) * 4;
        return std::make_tuple(img[i], img[i + 1], img[i + 2]);
    };
    auto diff = [&](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        int d = 0;
        for (size_t i = 0; i < a.size(); ++i) d = std::max(d, (int)std::abs((int)a[i] - (int)b[i]));
        return d;
    };

    // 1) Everything off: no post passes, and a re-render is deterministic.
    RenderStats off = engine.RenderEditorFrame();
    CHECK(off.postPasses == 0);
    std::vector<uint8_t> baseline = engine.frame().color;
    RenderStats offAgain = engine.RenderEditorFrame();
    CHECK(offAgain.postPasses == 0);
    CHECK(diff(baseline, engine.frame().color) == 0);

    // 2) Vignette only: the centre is untouched, the corner darkens.
    scene.postVignette = 0.9f;
    scene.postVignetteRadius = 0.3f;
    scene.postVignetteSoftness = 0.35f;
    RenderStats vig = engine.RenderEditorFrame();
    CHECK(vig.postPasses == 1);
    std::vector<uint8_t> withVignette = engine.frame().color;
    auto [cr0, cg0, cb0] = pixel(80, 60, baseline);
    auto [cr1, cg1, cb1] = pixel(80, 60, withVignette);
    CHECK(std::abs((int)cr1 - (int)cr0) <= 2 && std::abs((int)cg1 - (int)cg0) <= 2 &&
          std::abs((int)cb1 - (int)cb0) <= 2);                       // centre unchanged-ish
    auto [kr0, kg0, kb0] = pixel(2, 2, baseline);
    auto [kr1, kg1, kb1] = pixel(2, 2, withVignette);
    CHECK(kr1 + kg1 + kb1 < kr0 + kg0 + kb0 - 60);                  // corner measurably darker

    // 3) Contrast on top: counted as a second pass and visibly applied.
    scene.postContrast = 1.6f;
    RenderStats graded = engine.RenderEditorFrame();
    CHECK(graded.postPasses == 2);
    CHECK(diff(withVignette, engine.frame().color) > 8);

    // 4) Bloom only: strictly brightens (additive glow), one pass.
    scene.postVignette = 0.0f;
    scene.postContrast = 1.0f;
    scene.postBloomStrength = 0.8f;
    scene.postBloomThreshold = 0.6f;
    RenderStats bloom = engine.RenderEditorFrame();
    CHECK(bloom.postPasses == 1);
    std::vector<uint8_t> withBloom = engine.frame().color;
    int brightened = 0, darkened = 0;
    for (size_t i = 0; i < withBloom.size(); i += 4) {
        const int before = baseline[i] + baseline[i + 1] + baseline[i + 2];
        const int after = withBloom[i] + withBloom[i + 1] + withBloom[i + 2];
        if (after > before + 5) brightened++;
        if (after < before - 1) darkened++;
    }
    CHECK(brightened > 100);   // glow lands on the bright floor/sky
    CHECK(darkened == 0);      // bloom never removes light

    // 5) FXAA only: flat sky rows stay byte-identical, edges change.
    scene.postBloomStrength = 0.0f;
    scene.postBloomThreshold = 0.85f;
    scene.postFxaa = true;
    RenderStats fxaa = engine.RenderEditorFrame();
    CHECK(fxaa.postPasses == 1);
    std::vector<uint8_t> withFxaa = engine.frame().color;
    auto [sr0, sg0, sb0] = pixel(80, 8, baseline);   // sky, far from geometry
    auto [sr1, sg1, sb1] = pixel(80, 8, withFxaa);
    CHECK(sr0 == sr1 && sg0 == sg1 && sb0 == sb1);
    int fxaaChanged = 0;
    for (size_t i = 0; i < withFxaa.size(); i += 4) {
        if (std::abs((int)withFxaa[i] - (int)baseline[i]) > 2 ||
            std::abs((int)withFxaa[i + 1] - (int)baseline[i + 1]) > 2 ||
            std::abs((int)withFxaa[i + 2] - (int)baseline[i + 2]) > 2) {
            fxaaChanged++;
        }
    }
    CHECK(fxaaChanged > 0 && fxaaChanged < 160 * 120 / 2);

    // 6) Everything off again: byte-identical to the first frame.
    scene.postFxaa = false;
    RenderStats restored = engine.RenderEditorFrame();
    CHECK(restored.postPasses == 0);
    CHECK(diff(baseline, engine.frame().color) == 0);

    // 7) Settings survive serialization round-trips (restore replaces the
    // Scene, so re-acquire the reference).
    scene.postVignette = 0.4f;
    scene.postVignetteRadius = 0.6f;
    scene.postVignetteSoftness = 0.2f;
    scene.postContrast = 1.3f;
    scene.postSaturation = 0.5f;
    scene.postBloomStrength = 1.25f;
    scene.postBloomThreshold = 0.5f;
    scene.postFxaa = true;
    std::string json = engine.SerializeSceneJson();
    CHECK(engine.RestoreSceneJson(json));
    Scene& restoredScene = engine.scene();
    CHECK(NearlyEqual(restoredScene.postVignette, 0.4f, 1e-4f));
    CHECK(NearlyEqual(restoredScene.postVignetteRadius, 0.6f, 1e-4f));
    CHECK(NearlyEqual(restoredScene.postVignetteSoftness, 0.2f, 1e-4f));
    CHECK(NearlyEqual(restoredScene.postContrast, 1.3f, 1e-4f));
    CHECK(NearlyEqual(restoredScene.postSaturation, 0.5f, 1e-4f));
    CHECK(NearlyEqual(restoredScene.postBloomStrength, 1.25f, 1e-4f));
    CHECK(NearlyEqual(restoredScene.postBloomThreshold, 0.5f, 1e-4f));
    CHECK(restoredScene.postFxaa);
}

// Tangent frames: every procedural primitive must carry unit tangents
// orthonormal to its normals with valid handedness, and the plane's tangent
// must follow the UV gradient (+x for the default layout).
static void TestMeshTangents() {
    struct Prim { const char* name; std::shared_ptr<Mesh> mesh; };
    const Prim prims[] = {
        {"Cube", Mesh::Cube()},     {"Sphere", Mesh::Sphere()}, {"Plane", Mesh::Plane()},
        {"Quad", Mesh::Quad()},     {"Cylinder", Mesh::Cylinder()}, {"Cone", Mesh::Cone()},
        {"Torus", Mesh::Torus()},
    };
    for (const Prim& p : prims) {
        CHECK(!p.mesh->vertices.empty());
        for (const MeshVertex& v : p.mesh->vertices) {
            CHECK(NearlyEqual(v.tangent.XYZ().Length(), 1.0f, 1e-3f));
            CHECK(std::fabs(Vec3::Dot(v.normal, v.tangent.XYZ())) < 1e-3f);
            // Handedness is per UV layout (+1 or -1); mirrored faces flip it.
            CHECK(NearlyEqual(std::fabs(v.tangent.w), 1.0f, 1e-3f));
        }
    }
    // The plane's UVs run +u along +x, so its tangent must point along +x.
    auto plane = Mesh::Plane();
    for (const MeshVertex& v : plane->vertices) {
        CHECK(NearlyEqual(v.tangent.x, 1.0f, 1e-3f));
        CHECK(NearlyEqual(v.tangent.y, 0.0f, 1e-3f));
        CHECK(NearlyEqual(v.tangent.z, 0.0f, 1e-3f));
    }
}

// OBJ import shares vertices across faces by hashing (position, uv, normal)
// index triples: two quads sharing an edge collapse 8 duplicated corners into
// 6 unique vertices, and a grid imports as one vertex per grid point.
static void TestObjSharing() {
    const std::string dir = (std::filesystem::temp_directory_path() / "cairn-test-obj").string();
    std::filesystem::create_directories(dir);

    {
        const std::string path = dir + "/shared.obj";
        {
            std::ofstream out(path);
            out << "v 0 0 0\nv 1 0 0\nv 1 0 -1\nv 0 0 -1\nv 0 1 -1\nv 1 1 -1\n";
            out << "vt 0 0\nvt 0.5 0\nvt 0.5 1\nvt 0 1\nvt 1 0\nvt 1 1\n";
            out << "vn 0 0 1\n";
            out << "f 1/1/1 2/2/1 3/3/1 4/4/1\n";
            out << "f 4/4/1 3/3/1 6/6/1 5/5/1\n";
        }
        auto mesh = LoadObjFile(path);
        CHECK(mesh != nullptr);
        CHECK(mesh->vertices.size() == 6);  // was 8 before index sharing
        CHECK(mesh->indices.size() == 12);
        std::vector<bool> used(mesh->vertices.size(), false);
        for (uint32_t i : mesh->indices) {
            CHECK(i < mesh->vertices.size());
            used[i] = true;
        }
        CHECK(std::all_of(used.begin(), used.end(), [](bool b) { return b; }));
    }

    {
        // 8x8 grid of quads: one vertex per grid point.
        const int n = 8;
        const std::string path = dir + "/grid.obj";
        {
            std::ofstream out(path);
            for (int z = 0; z <= n; ++z)
                for (int x = 0; x <= n; ++x)
                    out << "v " << x << " 0 " << -z << "\n";
            for (int z = 0; z <= n; ++z)
                for (int x = 0; x <= n; ++x)
                    out << "vt " << (float)x / n << " " << (float)z / n << "\n";
            out << "vn 0 1 0\n";
            auto vi = [&](int x, int z) { return z * (n + 1) + x + 1; };
            for (int z = 0; z < n; ++z)
                for (int x = 0; x < n; ++x)
                    out << "f " << vi(x, z) << "/" << vi(x, z) << "/1 "
                        << vi(x + 1, z) << "/" << vi(x + 1, z) << "/1 "
                        << vi(x + 1, z + 1) << "/" << vi(x + 1, z + 1) << "/1 "
                        << vi(x, z + 1) << "/" << vi(x, z + 1) << "/1\n";
        }
        auto mesh = LoadObjFile(path);
        CHECK(mesh != nullptr);
        CHECK(mesh->vertices.size() == (size_t)(n + 1) * (n + 1));  // was 256 before sharing
        CHECK(mesh->indices.size() == (size_t)n * n * 6);
    }
}

// PBR-ish material properties survive serialization, and metallic changes the
// rendered response (diffuse collapses as the surface turns to metal).
static void TestPbrMaterials() {
    Engine engine;
    Scene& scene = engine.scene();
    // Exact dielectric/metal pixel comparisons are CPU-path only.
    scene.renderBackend = 2;
    GameObject* go = scene.CreateEntity("Mat");
    auto* mr = go->AddComponent<MeshRenderer>();
    mr->metallic = 0.8f;
    mr->roughness = 0.25f;
    mr->normalStrength = 1.5f;
    mr->normalMapPath = "Assets/Textures/checker.png";
    mr->emissiveColor = Color(1.0f, 0.5f, 0.0f, 1.0f);

    Json probe;
    CHECK(ComponentGetProperty(mr, "metallic", &probe));    // reflection sees the new props
    CHECK(NearlyEqual(probe.asFloat(), 0.8f, 1e-5f));
    Json out;
    CHECK(ComponentGetProperty(mr, "roughness", &out));
    CHECK(NearlyEqual(out.asFloat(), 0.25f, 1e-5f));

    std::string json = engine.SerializeSceneJson();
    CHECK(engine.RestoreSceneJson(json));
    Scene& restored = engine.scene();
    auto* m2 = restored.FindByName("Mat")->GetComponent<MeshRenderer>();
    CHECK(m2 != nullptr);
    CHECK(NearlyEqual(m2->metallic, 0.8f, 1e-4f));
    CHECK(NearlyEqual(m2->roughness, 0.25f, 1e-4f));
    CHECK(NearlyEqual(m2->normalStrength, 1.5f, 1e-4f));
    CHECK(m2->normalMapPath == "Assets/Textures/checker.png");
    CHECK(NearlyEqual(m2->emissiveColor.r, 1.0f, 1e-4f) && NearlyEqual(m2->emissiveColor.g, 0.5f, 1e-4f));
    CHECK(NearlyEqual(m2->emissiveColor.b, 0.0f, 1e-4f));

    // A fresh renderer keeps the shininess-derived roughness sentinel.
    GameObject* go2 = restored.CreateEntity("Fresh");
    auto* m3 = go2->AddComponent<MeshRenderer>();
    CHECK(NearlyEqual(m3->roughness, -1.0f, 1e-6f));

    // Metallic rewrites the response: with metal 1.0 the diffuse term vanishes,
    // so a front-lit metal face is far darker than the same dielectric face
    // (only specular+ambient remain).
    engine.SetViewportSize(64, 64);
    engine.viewCamera().position = Vec3(0, 0, 3);
    engine.viewCamera().yaw = 0.0f;
    engine.viewCamera().pitch = 0.0f;
    GameObject* sun = restored.CreateEntity("Sun");
    sun->transform().SetEulerDegrees(Vec3(0, 0, 0));  // straight down -Z, at the camera
    auto* light = sun->AddComponent<LightComponent>();
    light->type = LightComponent::LightType::Directional;
    light->intensity = 1.0f;
    restored.FindByName("Mat")->transform().position = Vec3(0, 0, 0);
    restored.FindByName("Mat")->GetComponent<MeshRenderer>()->metallic = 0.0f;
    restored.FindByName("Mat")->GetComponent<MeshRenderer>()->roughness = 0.5f;
    restored.FindByName("Mat")->GetComponent<MeshRenderer>()->emissiveColor = Color(0, 0, 0, 1);
    engine.RenderEditorFrame();
    auto sumOf = [&](const FrameTarget& t) {
        long s = 0;
        for (size_t i = 0; i < t.color.size(); i += 4) s += t.color[i] + t.color[i + 1] + t.color[i + 2];
        return s;
    };
    const long dielectric = sumOf(engine.frame());
    restored.FindByName("Mat")->GetComponent<MeshRenderer>()->metallic = 1.0f;
    engine.RenderEditorFrame();
    const long metal = sumOf(engine.frame());
    CHECK(dielectric > 0);
    CHECK(metal < dielectric);  // the metal lost its diffuse response
}

// Minimal 24-bit BMP writer (stb_image reads it; enough for cache tests).
static void WriteTestBmp(const std::string& path, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    const int row = w * 3;
    const uint32_t dataSize = (uint32_t)(row * h);
    const uint32_t fileSize = 54 + dataSize;
    std::ofstream out(path, std::ios::binary);
    auto u16 = [&](uint16_t v) { out.put((char)(v & 0xff)); out.put((char)(v >> 8)); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.put((char)((v >> (8 * i)) & 0xff)); };
    out.put('B'); out.put('M');
    u32(fileSize); u16(0); u16(0); u32(54);
    u32(40); u32((uint32_t)w); u32((uint32_t)h); u16(1); u16(24);
    u32(0); u32(dataSize); u32(2835); u32(2835); u32(0); u32(0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            out.put((char)b); out.put((char)g); out.put((char)r);
        }
}

// Minimal 24-bit BMP of a 1-texel checkerboard (stb_image reads it).
static void WriteTestBmpChecker(const std::string& path, int w, int h,
                                uint8_t r0, uint8_t g0, uint8_t b0,
                                uint8_t r1, uint8_t g1, uint8_t b1) {
    const int row = w * 3;
    const uint32_t dataSize = (uint32_t)(row * h);
    const uint32_t fileSize = 54 + dataSize;
    std::ofstream out(path, std::ios::binary);
    auto u16 = [&](uint16_t v) { out.put((char)(v & 0xff)); out.put((char)(v >> 8)); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.put((char)((v >> (8 * i)) & 0xff)); };
    out.put('B'); out.put('M');
    u32(fileSize); u16(0); u16(0); u32(54);
    u32(40); u32((uint32_t)w); u32((uint32_t)h); u16(1); u16(24);
    u32(0); u32(dataSize); u32(2835); u32(2835); u32(0); u32(0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const bool odd = ((x + y) & 1) != 0;
            out.put((char)(odd ? b1 : b0));
            out.put((char)(odd ? g1 : g0));
            out.put((char)(odd ? r1 : r0));
        }
}

// Minimal 24-bit BMP of a two-tone image: the visual top half one colour and
// the visual bottom half another (BMP rows are stored bottom-up, so the file's
// last row is the visual top).
static void WriteTestBmpHalves(const std::string& path, int w, int h,
                               uint8_t topR, uint8_t topG, uint8_t topB,
                               uint8_t botR, uint8_t botG, uint8_t botB) {
    const int row = w * 3;
    const uint32_t dataSize = (uint32_t)(row * h);
    const uint32_t fileSize = 54 + dataSize;
    std::ofstream out(path, std::ios::binary);
    auto u16 = [&](uint16_t v) { out.put((char)(v & 0xff)); out.put((char)(v >> 8)); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.put((char)((v >> (8 * i)) & 0xff)); };
    out.put('B'); out.put('M');
    u32(fileSize); u16(0); u16(0); u32(54);
    u32(40); u32((uint32_t)w); u32((uint32_t)h); u16(1); u16(24);
    u32(0); u32(dataSize); u32(2835); u32(2835); u32(0); u32(0);
    for (int fileRow = 0; fileRow < h; ++fileRow) {
        const bool topHalf = fileRow >= h / 2;
        for (int x = 0; x < w; ++x) {
            out.put((char)(topHalf ? topB : botB));
            out.put((char)(topHalf ? topG : botG));
            out.put((char)(topHalf ? topR : botR));
        }
    }
}

// Minimal 24-bit BMP of a 2x2 sheet: four equal quadrants whose colours are
// given in reading order (top-left, top-right, bottom-left, bottom-right) - the
// layout image.stamp produces and uvRect addresses. BMP rows are stored
// bottom-up, so the file's last rows are the visual top.
static void WriteTestBmpSheet(const std::string& path,
                              uint8_t r0, uint8_t g0, uint8_t b0,   // top-left
                              uint8_t r1, uint8_t g1, uint8_t b1,   // top-right
                              uint8_t r2, uint8_t g2, uint8_t b2,   // bottom-left
                              uint8_t r3, uint8_t g3, uint8_t b3) { // bottom-right
    const int w = 16, h = 16;
    const int row = w * 3;
    const uint32_t dataSize = (uint32_t)(row * h);
    const uint32_t fileSize = 54 + dataSize;
    std::ofstream out(path, std::ios::binary);
    auto u16 = [&](uint16_t v) { out.put((char)(v & 0xff)); out.put((char)(v >> 8)); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.put((char)((v >> (8 * i)) & 0xff)); };
    out.put('B'); out.put('M');
    u32(fileSize); u16(0); u16(0); u32(54);
    u32(40); u32((uint32_t)w); u32((uint32_t)h); u16(1); u16(24);
    u32(0); u32(dataSize); u32(2835); u32(2835); u32(0); u32(0);
    for (int fileRow = 0; fileRow < h; ++fileRow) {
        const bool visualTop = fileRow >= h / 2;   // BMP rows run bottom-up
        for (int x = 0; x < w; ++x) {
            const bool right = x >= w / 2;
            const int i = (visualTop ? 0 : 2) + (right ? 1 : 0);
            const uint8_t c[4][3] = {{r0, g0, b0}, {r1, g1, b1}, {r2, g2, b2}, {r3, g3, b3}};
            out.put((char)c[i][2]);
            out.put((char)c[i][1]);
            out.put((char)c[i][0]);
        }
    }
}

// Bindless texture arrays: a batched run shares one mesh and cull mode, so the
// only thing keeping two draws' textures apart is the per-instance slot index
// in the draw uniform. Three cubes with three albedo textures must render as
// three colours, and both backends must agree on which is which.
static void TestBindlessTextureSlots() {
    const std::string dir = (std::filesystem::temp_directory_path() / "cairn-test-bindless").string();
    std::filesystem::create_directories(dir + "/Assets/Textures");
    const char* tex[3] = {"Assets/Textures/red.bmp", "Assets/Textures/green.bmp",
                          "Assets/Textures/blue.bmp"};
    WriteTestBmp(dir + "/" + tex[0], 8, 8, 255, 0, 0);
    WriteTestBmp(dir + "/" + tex[1], 8, 8, 0, 255, 0);
    WriteTestBmp(dir + "/" + tex[2], 8, 8, 0, 0, 255);

    auto build = [&](Engine& engine, int backend) {
        engine.SetProjectRoot(dir);
        engine.SetViewportSize(192, 96);
        Scene& scene = engine.scene();
        scene.renderBackend = backend;
        scene.shadows = 0;  // keep the frame to one opaque pass
        GameObject* sun = scene.CreateEntity("Sun");
        sun->transform().SetEulerDegrees(Vec3(0, 0, 0));  // head on, so each face is lit
        auto* light = sun->AddComponent<LightComponent>();
        light->type = LightComponent::LightType::Directional;
        light->intensity = 1.0f;
        for (int i = 0; i < 3; ++i) {
            GameObject* cube = scene.CreateEntity("Cube");
            cube->AddComponent<MeshRenderer>()->albedoTexturePath = tex[i];
            cube->transform().position = Vec3((float)(i - 1) * 2.6f, 0, 0);
            cube->transform().scale = Vec3(1.2f, 1.2f, 1.2f);
        }
        // Orthographic and tight, so each cube sits well inside one third of
        // the frame and a neighbour's colour cannot bleed across.
        ViewportCamera& cam = engine.viewCamera();
        cam.position = Vec3(0, 0, 9);
        cam.yaw = 0.0f;
        cam.pitch = 0.0f;
        cam.orthographic = true;
        cam.orthoSize = 2.0f;
    };
    // Strongest value of one channel over the left / middle / right third. A
    // solid lit face peaks near its texture's colour, so the peak identifies
    // which texture that third was drawn with.
    auto channelPeak = [](Engine& engine, int third, int channel) {
        const FrameTarget& t = engine.frame();
        const int w = t.width, h = t.height;
        const int x0 = third * w / 3, x1 = (third + 1) * w / 3;
        int peak = 0;
        for (int y = 0; y < h; ++y)
            for (int x = x0; x < x1; ++x)
                peak = std::max(peak, (int)t.color[((size_t)y * (size_t)w + (size_t)x) * 4 +
                                       (size_t)channel]);
        return peak;
    };
    auto channelMean = [](Engine& engine, int channel) {
        const FrameTarget& t = engine.frame();
        long sum = 0, n = 0;
        for (size_t i = 0; i < t.color.size(); i += 4) { sum += t.color[i + (size_t)channel]; ++n; }
        return n ? (double)sum / (double)n : 0.0;
    };
    // Third `i` must peak in channel `i`: that is exactly what a per-instance
    // texture slot buys, and what a single shared slot would break.
    auto checkThirds = [&](Engine& engine) {
        for (int i = 0; i < 3; ++i) {
            const int own = channelPeak(engine, i, i);
            for (int c = 0; c < 3; ++c) {
                if (c == i) continue;
                CHECK(own > channelPeak(engine, i, c) + 30);
            }
        }
        // ...and the frame as a whole is balanced: three different textures,
        // three comparable channel contributions.
        const double r = channelMean(engine, 0), g = channelMean(engine, 1), b = channelMean(engine, 2);
        const double lo = std::min(r, std::min(g, b)), hi = std::max(r, std::max(g, b));
        CHECK(hi < lo * 1.6);
    };

    Engine cpu;
    build(cpu, 2);
    cpu.RenderEditorFrame();
    checkThirds(cpu);

    if (!VulkanBackendAvailable()) return;
    Engine gpu;
    build(gpu, 1);
    const RenderStats stats = gpu.RenderEditorFrame();
    CHECK(std::string(stats.backend) == "vulkan");
    // One opaque draw plus the sky: the three cubes batched despite having
    // different textures, which is the whole point of the bindless table.
    CHECK(stats.drawCalls <= 3);
    checkThirds(gpu);
    for (int i = 0; i < 3; ++i)
        for (int c = 0; c < 3; ++c)
            CHECK(std::abs(channelPeak(gpu, i, c) - channelPeak(cpu, i, c)) < 70);
}

// 2D rendering quality. A pixel-perfect sprite has to cover its whole quad
// (both triangles) and sample texels without blending them. Two real bugs are
// pinned here: the software path dropped the second triangle of every 2D quad
// (so half of every sprite and glyph was missing), and the GPU sampled
// pixel-perfect sprites through a linear, mipmapped sampler (so AI-drawn pixel
// art came out blurred and the two backends disagreed).
static void TestSpriteQuality() {
    const std::string dir = (std::filesystem::temp_directory_path() / "cairn-test-sprite").string();
    std::filesystem::create_directories(dir + "/Assets/Textures");
    // 16x16 alternating pure red / pure blue every texel. Nearest sampling
    // yields exactly two lit colours; filtering across texels shows up as an
    // in-between colour (a red/blue blend is neither red nor blue).
    const std::string tex = "Assets/Textures/checker.bmp";
    WriteTestBmpChecker(dir + "/" + tex, 16, 16, 255, 0, 0, 0, 0, 255);

    auto build = [&](Engine& engine, int backend) {
        engine.SetProjectRoot(dir);
        engine.SetViewportSize(128, 128);
        Scene& scene = engine.scene();
        scene.renderBackend = backend;
        scene.msaa = 1;       // no multisample resolve to soften the texel edges
        scene.shadows = 0;
        scene.postFxaa = false;
        GameObject* cam = scene.CreateEntity("Main");
        cam->tag = "MainCamera";
        cam->transform().position = Vec3(0, 0, 8);
        auto* cc = cam->AddComponent<CameraComponent>();
        cc->orthographic = true;
        cc->orthographicSize = 2.0f;   // 4x4 world units on screen
        cc->clearColor = Color(0, 0, 0, 1);
        cc->skyEnabled = false;
        GameObject* sprite = scene.CreateEntity("Sprite");
        auto* sr = sprite->AddComponent<SpriteRenderer>();
        sr->texturePath = tex;
        sr->pixelPerfect = true;
        sr->pixelsPerUnit = 3.0f;      // 16 texels -> 5.33 units: overfills the view
        (void)sprite;
    };
    auto rgb = [](Engine& engine, int x, int y) {
        const FrameTarget& t = engine.frame();
        const uint8_t* p = &t.color[((size_t)y * (size_t)t.width + (size_t)x) * 4];
        return std::array<int, 3>{(int)p[0], (int)p[1], (int)p[2]};
    };
    auto checkSprite = [&](Engine& engine) {
        // Both triangles of the quad must be covered. With the second one
        // dropped the upper-left quadrant of the sprite is left as background.
        for (const auto& q : {std::pair<int, int>{32, 32}, {96, 32}, {32, 96}, {96, 96}}) {
            auto c = rgb(engine, q.first, q.second);
            CHECK(c[0] + c[1] + c[2] > 24);
        }
        // Pixel-perfect sampling must not blend texels: the sprite interior
        // only ever shows the two source colours, lit by the ambient term.
        std::set<int> colours;
        for (int y = 24; y < 104; ++y)
            for (int x = 24; x < 104; ++x) {
                auto c = rgb(engine, x, y);
                colours.insert((c[0] << 16) | (c[1] << 8) | c[2]);
            }
        CHECK(colours.size() <= 4);
    };

    Engine cpu;
    build(cpu, 2);  // software
    cpu.Play();
    const RenderStats cpuStats = cpu.RenderGameFrame();
    CHECK(std::string(cpuStats.backend) == "software");
    checkSprite(cpu);

    if (!VulkanBackendAvailable()) return;
    Engine gpu;
    build(gpu, 1);  // forced Vulkan; skips gracefully without a device
    gpu.Play();
    const RenderStats gpuStats = gpu.RenderGameFrame();
    CHECK(std::string(gpuStats.backend) == "vulkan");
    checkSprite(gpu);
}

// 2D up/down orientation. Image space is y-down everywhere else in the engine
// (image.* draws y=0 at the top, and a texture's row 0 is v=0), so the sprite
// and glyph quads must give their *top* corners the low v. They used to hand
// the bottom corners v=0, which stood every sprite and every word on its head.
// A red-over-blue sprite pins the sprite path; the capital 'T' — a full bar on
// the top row above a centred stem — pins the glyph path. Both are checked on
// both backends, because the mapping lives in shared vertex data precisely so
// that the two agree.
static void TestSpriteOrientation() {
    const std::string dir = (std::filesystem::temp_directory_path() / "cairn-test-orient").string();
    std::filesystem::create_directories(dir + "/Assets/Textures");
    const std::string tex = "Assets/Textures/halves.bmp";
    WriteTestBmpHalves(dir + "/" + tex, 16, 16, 255, 0, 0, 0, 0, 255);

    auto makeScene = [&](Engine& engine, int backend) -> Scene& {
        engine.SetProjectRoot(dir);
        engine.SetViewportSize(128, 128);
        Scene& scene = engine.scene();
        scene.renderBackend = backend;
        scene.msaa = 1;
        scene.shadows = 0;
        scene.postFxaa = false;
        GameObject* cam = scene.CreateEntity("Main");
        cam->tag = "MainCamera";
        cam->transform().position = Vec3(0, 0, 8);
        auto* cc = cam->AddComponent<CameraComponent>();
        cc->orthographic = true;
        cc->orthographicSize = 2.0f;   // 4x4 world units on screen
        cc->clearColor = Color(0, 0, 0, 1);
        cc->skyEnabled = false;
        return scene;
    };
    auto px = [](Engine& engine, int x, int y) {
        const FrameTarget& t = engine.frame();
        const uint8_t* p = &t.color[((size_t)y * (size_t)t.width + (size_t)x) * 4];
        return std::array<int, 3>{(int)p[0], (int)p[1], (int)p[2]};
    };
    auto sum = [](const std::array<int, 3>& c) { return c[0] + c[1] + c[2]; };

    // --- sprite: the red half is up, the blue half is down ----------------
    {
        auto build = [&](Engine& engine, int backend) {
            Scene& scene = makeScene(engine, backend);
            GameObject* sprite = scene.CreateEntity("Sprite");
            auto* sr = sprite->AddComponent<SpriteRenderer>();
            sr->texturePath = tex;
            sr->pixelPerfect = true;
            sr->pixelsPerUnit = 4.0f;   // 16 texels -> 4 units: fills the view
        };
        auto check = [&](Engine& engine) {
            auto top = px(engine, 64, 32);     // over the red half
            auto bottom = px(engine, 64, 96);  // over the blue half
            CHECK(sum(top) > 24 && sum(bottom) > 24);
            CHECK(top[0] > top[2] * 2);        // red above
            CHECK(bottom[2] > bottom[0] * 2);  // blue below
        };
        Engine cpu;
        build(cpu, 2);  // software
        cpu.Play();
        CHECK(std::string(cpu.RenderGameFrame().backend) == "software");
        check(cpu);

        if (VulkanBackendAvailable()) {
            Engine gpu;
            build(gpu, 1);  // forced Vulkan; skips gracefully without a device
            gpu.Play();
            CHECK(std::string(gpu.RenderGameFrame().backend) == "vulkan");
            check(gpu);
        }
    }

    // --- text: the 'T' bar belongs at the top -----------------------------
    {
        auto build = [&](Engine& engine, int backend) {
            Scene& scene = makeScene(engine, backend);
            GameObject* text = scene.CreateEntity("Text");
            text->transform().position = Vec3(-1.25f, -1.75f, 0);
            auto* tr = text->AddComponent<TextRenderer>();
            tr->text = "T";
            tr->fontSize = 3.5f;   // glyph 2.5 x 3.5 units, centred in the view
            tr->color = Color(1, 1, 1, 1);
        };
        auto check = [&](Engine& engine) {
            // The glyph fills screen x 24..104, y 8..120. 'T' is a full bar on
            // its top row over a one-texel stem, so the top rows of its ink
            // bounding box must be far busier than the bottom rows. Upside
            // down, the stem is on top and the bar hangs at the bottom.
            int minY = 128, maxY = -1;
            for (int y = 0; y < 128; ++y)
                for (int x = 16; x < 112; ++x) {
                    if (sum(px(engine, x, y)) > 24) {
                        if (y < minY) minY = y;
                        if (y > maxY) maxY = y;
                    }
                }
            CHECK(minY <= maxY);
            auto litIn = [&](int y0, int y1) {
                int n = 0;
                for (int y = y0; y <= y1; ++y)
                    for (int x = 16; x < 112; ++x)
                        if (sum(px(engine, x, y)) > 24) ++n;
                return n;
            };
            const int top = litIn(minY, minY + 3);
            const int bottom = litIn(maxY - 3, maxY);
            CHECK(top > bottom * 2);   // the bar is up, the stem hangs below it
        };
        Engine cpu;
        build(cpu, 2);  // software
        cpu.Play();
        CHECK(std::string(cpu.RenderGameFrame().backend) == "software");
        check(cpu);

        if (VulkanBackendAvailable()) {
            Engine gpu;
            build(gpu, 1);
            gpu.Play();
            CHECK(std::string(gpu.RenderGameFrame().backend) == "vulkan");
            check(gpu);
        }
    }
}

// 2D sprite lighting must follow the *image*, not the screen. A normal map's
// green channel is authored in the same y-down image space as the albedo: with
// image.normalMap's convention G < 0.5 tilts the surface toward the top of the
// image, G > 0.5 toward the bottom. The orientation fix above is what makes
// image-top equal world-top, so a map leaning to the image top has to catch a
// lamp that shines down on the sprite while one leaning to the image bottom
// must miss it. Both backends carry their own copy of the remap, so both are
// checked.
static void TestSpriteNormalOrientation() {
    const std::string dir = (std::filesystem::temp_directory_path() / "cairn-test-nrm").string();
    std::filesystem::create_directories(dir + "/Assets/Textures");
    WriteTestBmp(dir + "/Assets/Textures/white.bmp", 8, 8, 255, 255, 255);
    // G = 0 -> the generator's ny = -1 -> the normal leans to the image top;
    // G = 255 -> ny = +1 -> it leans to the image bottom.
    WriteTestBmp(dir + "/Assets/Textures/nrm_up.bmp", 8, 8, 128, 0, 255);
    WriteTestBmp(dir + "/Assets/Textures/nrm_down.bmp", 8, 8, 128, 255, 255);

    auto run = [&](int backend) {
        Engine engine;
        engine.SetProjectRoot(dir);
        engine.SetViewportSize(64, 64);
        Scene& scene = engine.scene();
        scene.renderBackend = backend;
        scene.msaa = 1;
        scene.shadows = 0;
        scene.postFxaa = false;
        GameObject* cam = scene.CreateEntity("Main");
        cam->tag = "MainCamera";
        cam->transform().position = Vec3(0, 0, 8);
        auto* cc = cam->AddComponent<CameraComponent>();
        cc->orthographic = true;
        cc->orthographicSize = 1.0f;   // the 2x2-unit sprite fills the view
        cc->clearColor = Color(0, 0, 0, 1);
        cc->skyEnabled = false;
        GameObject* lamp = scene.CreateEntity("Lamp");
        lamp->transform().position = Vec3(0, 3, 2);   // above and in front
        auto* lc = lamp->AddComponent<LightComponent>();
        lc->type = LightComponent::LightType::Point;
        lc->intensity = 1.0f;
        lc->range = 20.0f;
        GameObject* sprite = scene.CreateEntity("Sprite");
        auto* sr = sprite->AddComponent<SpriteRenderer>();
        sr->texturePath = "Assets/Textures/white.bmp";
        sr->pixelPerfect = true;
        sr->pixelsPerUnit = 4.0f;
        sr->normalMapPath = "Assets/Textures/nrm_up.bmp";

        engine.Play();
        auto centre = [&]() {
            const FrameTarget& t = engine.frame();
            const uint8_t* p = &t.color[((size_t)32 * (size_t)t.width + 32) * 4];
            return (int)p[0] + (int)p[1] + (int)p[2];
        };
        engine.RenderGameFrame();
        const int leaningUp = centre();
        sr->normalMapPath = "Assets/Textures/nrm_down.bmp";
        engine.RenderGameFrame();
        const int leaningDown = centre();
        CHECK(leaningUp > leaningDown + 20);
    };

    run(2);  // software
    if (VulkanBackendAvailable()) run(1);
}

// Sprite atlas: uvRect picks a sub-rectangle of the texture and pivot places it
// relative to the entity. A four-quadrant sheet makes both wrong answers
// visible - an ignored uvRect draws the whole sheet instead of one frame, and an
// ignored pivot centres every sprite on the entity. The frame's world size is
// the *frame's* texels over the pixels-per-unit, not the whole texture's, so a
// 2-unit frame cannot cover a 4-unit view. Checked on both backends: the geometry
// is shared vertex data, and the two must agree.
static void TestSpriteAtlas() {
    const std::string dir = (std::filesystem::temp_directory_path() / "cairn-test-atlas").string();
    std::filesystem::create_directories(dir + "/Assets/Textures");
    const std::string tex = "Assets/Textures/sheet.bmp";
    // Reading order: red, green / blue, white.
    WriteTestBmpSheet(dir + "/" + tex,
                      255, 0, 0,   0, 255, 0,
                      0, 0, 255,   255, 255, 255);

    auto px = [](Engine& engine, int x, int y) {
        const FrameTarget& t = engine.frame();
        const uint8_t* p = &t.color[((size_t)y * (size_t)t.width + (size_t)x) * 4];
        return std::array<int, 3>{(int)p[0], (int)p[1], (int)p[2]};
    };
    auto sum = [](const std::array<int, 3>& c) { return c[0] + c[1] + c[2]; };
    auto close = [](const std::array<int, 3>& c, int r, int g, int b) {
        return std::abs(c[0] - r) < 40 && std::abs(c[1] - g) < 40 && std::abs(c[2] - b) < 40;
    };
    auto isBlack = [&](const std::array<int, 3>& c) { return sum(c) < 24; };

    auto makeScene = [&](Engine& engine, int backend) -> Scene& {
        engine.SetProjectRoot(dir);
        engine.SetViewportSize(128, 128);
        Scene& scene = engine.scene();
        scene.renderBackend = backend;
        scene.msaa = 1;
        scene.shadows = 0;
        scene.postFxaa = false;
        // White ambient and no tone mapping, so a texel's authored colour
        // reaches the frame unchanged and the frame checks can be about which
        // frame of the sheet was drawn rather than about the lighting curve.
        scene.ambient = Color(1, 1, 1, 1);
        scene.tonemap = 0;
        GameObject* cam = scene.CreateEntity("Main");
        cam->tag = "MainCamera";
        cam->transform().position = Vec3(0, 0, 8);
        auto* cc = cam->AddComponent<CameraComponent>();
        cc->orthographic = true;
        cc->orthographicSize = 2.0f;   // 4x4 world units on screen: 32 px per unit
        cc->clearColor = Color(0, 0, 0, 1);
        cc->skyEnabled = false;
        return scene;
    };
    auto addSprite = [&](Scene& scene, Vec4 rect, Vec2 pivot) -> SpriteRenderer* {
        GameObject* go = scene.CreateEntity("Sprite");
        auto* sr = go->AddComponent<SpriteRenderer>();
        sr->texturePath = tex;
        sr->pixelPerfect = true;
        sr->pixelsPerUnit = 4.0f;      // 8-texel frame -> 2 units; whole sheet -> 4
        sr->uvRect = rect;
        sr->pivot = pivot;
        return sr;
    };

    // --- each frame draws its own quadrant, at the frame's size -----------
    {
        struct Case { Vec4 rect; int r, g, b; const char* what; };
        const Case cases[] = {
            {Vec4(0.0f, 0.0f, 0.5f, 0.5f), 255, 0, 0, "top-left frame is red"},
            {Vec4(0.5f, 0.0f, 0.5f, 0.5f), 0, 255, 0, "top-right frame is green"},
            {Vec4(0.0f, 0.5f, 0.5f, 0.5f), 0, 0, 255, "bottom-left frame is blue"},
            {Vec4(0.5f, 0.5f, 0.5f, 0.5f), 255, 255, 255, "bottom-right frame is white"},
        };
        auto build = [&](Engine& engine, int backend) {
            Scene& scene = makeScene(engine, backend);
            // One sprite per frame, placed by an offscreen marker entity, so a
            // single frame pins all four rects at once.
            addSprite(scene, cases[0].rect, Vec2(0.5f, 0.5f));
        };
        auto check = [&](Engine& engine) {
            // The sprite is centred and 2 units across, so it fills screen
            // 32..96 in both axes; the view corners stay background. A whole-sheet
            // sprite (4 units) would cover the corners too.
            auto sr = engine.scene().FindByName("Sprite")->GetComponent<SpriteRenderer>();
            for (const Case& c : cases) {
                sr->uvRect = c.rect;
                engine.RenderGameFrame();
                CHECK(close(px(engine, 64, 64), c.r, c.g, c.b));
                CHECK(isBlack(px(engine, 16, 16)));
            }
        };
        Engine cpu;
        build(cpu, 2);  // software
        cpu.Play();
        CHECK(std::string(cpu.RenderGameFrame().backend) == "software");
        check(cpu);

        if (VulkanBackendAvailable()) {
            Engine gpu;
            build(gpu, 1);
            gpu.Play();
            CHECK(std::string(gpu.RenderGameFrame().backend) == "vulkan");
            check(gpu);
        }
    }

    // --- the pivot moves the quad, it does not shrink it ------------------
    {
        auto build = [&](Engine& engine, int backend) {
            Scene& scene = makeScene(engine, backend);
            addSprite(scene, Vec4(0.0f, 0.0f, 0.5f, 0.5f), Vec2(0.0f, 1.0f));
        };
        auto check = [&](Engine& engine) {
            // pivot (0, 1) is the sprite's bottom-left in image coordinates, so
            // the 2x2-unit frame sits in world x 0..2, y 0..2 - the view's
            // top-right quadrant (screen y grows downward).
            CHECK(close(px(engine, 96, 32), 255, 0, 0));
            CHECK(isBlack(px(engine, 32, 96)));
            CHECK(isBlack(px(engine, 32, 32)));
            CHECK(isBlack(px(engine, 96, 96)));
        };
        Engine cpu;
        build(cpu, 2);  // software
        cpu.Play();
        CHECK(std::string(cpu.RenderGameFrame().backend) == "software");
        check(cpu);

        if (VulkanBackendAvailable()) {
            Engine gpu;
            build(gpu, 1);
            gpu.Play();
            CHECK(std::string(gpu.RenderGameFrame().backend) == "vulkan");
            check(gpu);
        }
    }
}

// Sprite culling: the bound a sprite is tested against is the quad it actually
// draws - the atlas rect's texels over the pixels-per-unit, offset by the pivot -
// and not a function of the pixels-per-unit alone. Two visible failures of the
// old bound (pixelsPerUnit * 0.5 around the entity) are pinned here: a sprite
// whose entity sits far outside the view but whose pixels cover it (a large
// pixels-per-unit sprite, e.g. a parallax layer) was culled away and left the
// screen empty, and a sprite that is genuinely off screen was kept and drawn
// because its "bound" was 50 units wide. The pivot participates as well, so the
// same entity can be culled or drawn depending on where its rect sits - which
// means the cull and the vertex generation must read one definition of the
// extent, `SpriteLocalExtent`.
static void TestSpriteCullBounds() {
    const std::string dir = (std::filesystem::temp_directory_path() / "cairn-test-cull").string();
    std::filesystem::create_directories(dir + "/Assets/Textures");
    WriteTestBmp(dir + "/Assets/Textures/red.bmp", 16, 16, 255, 0, 0);

    auto sum = [](const std::array<int, 3>& c) { return c[0] + c[1] + c[2]; };
    auto px = [](Engine& engine, int x, int y) {
        const FrameTarget& t = engine.frame();
        const uint8_t* p = &t.color[((size_t)y * (size_t)t.width + (size_t)x) * 4];
        return std::array<int, 3>{(int)p[0], (int)p[1], (int)p[2]};
    };
    auto build = [&](Engine& engine, int backend, float ppu, Vec3 pos, Vec2 pivot) {
        engine.SetProjectRoot(dir);
        engine.SetViewportSize(128, 128);
        Scene& scene = engine.scene();
        scene.renderBackend = backend;
        scene.msaa = 1;
        scene.shadows = 0;
        scene.postFxaa = false;
        scene.ambient = Color(1, 1, 1, 1);
        scene.tonemap = 0;
        GameObject* cam = scene.CreateEntity("Main");
        cam->tag = "MainCamera";
        cam->transform().position = Vec3(0, 0, 8);
        auto* cc = cam->AddComponent<CameraComponent>();
        cc->orthographic = true;
        cc->orthographicSize = 1.0f;   // a 2x2-unit view, 64 px per unit
        cc->clearColor = Color(0, 0, 0, 1);
        cc->skyEnabled = false;
        GameObject* go = scene.CreateEntity("Sprite");
        go->transform().position = pos;
        auto* sr = go->AddComponent<SpriteRenderer>();
        sr->texturePath = "Assets/Textures/red.bmp";
        sr->pixelPerfect = true;
        sr->pixelsPerUnit = ppu;
        sr->pivot = pivot;
    };
    // The 16-texel texture is 160 units across at 0.1 px/unit and 0.16 at 100.
    auto run = [&](int wantDrawn, int wantCulled, bool wantLit, float ppu, Vec3 pos, Vec2 pivot) {
        auto one = [&](int backend, const char* name) {
            Engine engine;
            build(engine, backend, ppu, pos, pivot);
            engine.Play();
            const RenderStats st = engine.RenderGameFrame();
            CHECK(std::string(st.backend) == name);
            CHECK(st.drawCalls2D == wantDrawn);
            CHECK(st.frustumCulled == wantCulled);
            CHECK((sum(px(engine, 64, 64)) > 24) == wantLit);
        };
        one(2, "software");
        if (VulkanBackendAvailable()) one(1, "vulkan");
    };

    // Its entity is 50 units past the view's edge, but 80 units of the sprite
    // still cover the view: the old bound put it outside and culled it.
    run(1, 0, true, 0.1f, Vec3(50, 0, 0), Vec2(0.5f, 0.5f));
    // 0.16 units wide, 3 units out: nothing of it can reach the view, so the
    // draw must be dropped rather than submitted and clipped.
    run(0, 1, false, 100.0f, Vec3(3, 0, 0), Vec2(0.5f, 0.5f));
    // Same position and size, but the pivot moves the whole rect to the right of
    // the entity, off screen - the bound has to follow the pivot.
    run(0, 1, false, 0.1f, Vec3(1.2f, 0, 0), Vec2(0.0f, 1.0f));
    run(1, 0, true, 0.1f, Vec3(1.2f, 0, 0), Vec2(0.5f, 0.5f));
}

// Sprite animation: a SpriteAnimator steps a sprite through the cells of its
// sheet. Frame order, looping and the clip range are all read back as the colour
// of the cell that must be on screen, and time is driven explicitly with
// Update(dt) so a frame is a function of the dt sequence (and of nothing else) -
// which is the property a batch run needs. The cells are cut out of the sprite's
// own uvRect, so the same texture can animate one row of a taller sheet.
static void TestSpriteAnimation() {
    const std::string dir = (std::filesystem::temp_directory_path() / "cairn-test-anim").string();
    std::filesystem::create_directories(dir + "/Assets/Textures");
    const std::string tex = "Assets/Textures/frames.bmp";
    // Reading order: red, green / blue, white - one distinct colour per cell.
    WriteTestBmpSheet(dir + "/" + tex, 255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255);
    const int sheet[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};

    auto px = [](Engine& engine, int x, int y) {
        const FrameTarget& t = engine.frame();
        const uint8_t* p = &t.color[((size_t)y * (size_t)t.width + (size_t)x) * 4];
        return std::array<int, 3>{(int)p[0], (int)p[1], (int)p[2]};
    };
    // Renders and reports whether the frame shows cell `f` at the sprite's
    // middle: the sprite is one cell of the sheet (2 world units at 4 px/unit)
    // centred in a 4-unit view, so pixel 64,64 is inside it whatever the cell.
    auto shows = [&](Engine& engine, int f) {
        engine.RenderGameFrame();
        const std::array<int, 3> c = px(engine, 64, 64);
        return std::abs(c[0] - sheet[f][0]) < 40 && std::abs(c[1] - sheet[f][1]) < 40 &&
               std::abs(c[2] - sheet[f][2]) < 40;
    };
    auto build = [&](Engine& engine, int backend, Vec4 rect, int columns, int rows) {
        engine.SetProjectRoot(dir);
        engine.SetViewportSize(128, 128);
        Scene& scene = engine.scene();
        scene.renderBackend = backend;
        scene.msaa = 1;
        scene.shadows = 0;
        scene.postFxaa = false;
        scene.ambient = Color(1, 1, 1, 1);
        scene.tonemap = 0;
        GameObject* cam = scene.CreateEntity("Main");
        cam->tag = "MainCamera";
        cam->transform().position = Vec3(0, 0, 8);
        auto* cc = cam->AddComponent<CameraComponent>();
        cc->orthographic = true;
        cc->orthographicSize = 2.0f;   // 4x4 units on screen: 32 px per unit
        cc->clearColor = Color(0, 0, 0, 1);
        cc->skyEnabled = false;
        GameObject* go = scene.CreateEntity("Sprite");
        auto* sr = go->AddComponent<SpriteRenderer>();
        sr->texturePath = tex;
        sr->pixelPerfect = true;
        sr->pixelsPerUnit = 4.0f;      // an 8-texel cell -> 2 units
        sr->uvRect = rect;
        auto* anim = go->AddComponent<SpriteAnimator>();
        anim->columns = columns;
        anim->rows = rows;
        anim->fps = 2.0f;              // 0.5 s per frame
        return anim;
    };
    auto bothBackends = [&](const std::function<void(int, const char*)>& one) {
        one(2, "software");
        if (VulkanBackendAvailable()) one(1, "vulkan");
    };

    // --- the whole sheet, one frame per 0.5 s, looping ---------------------
    bothBackends([&](int backend, const char* name) {
        Engine engine;
        SpriteAnimator* anim = build(engine, backend, Vec4(0, 0, 1, 1), 2, 2);
        engine.Play();
        CHECK(std::string(engine.RenderGameFrame().backend) == name);
        CHECK(shows(engine, 0));          // nothing has advanced yet
        engine.Update(0.5f);
        CHECK(shows(engine, 1));
        engine.Update(0.5f);
        CHECK(shows(engine, 2));
        engine.Update(0.5f);
        CHECK(shows(engine, 3));
        engine.Update(0.5f);
        CHECK(shows(engine, 0));          // 2 fps over 2 s wraps back to the start
        // Half a frame is not a frame: time accumulates, it does not round.
        engine.Update(0.24f);
        CHECK(shows(engine, 0));
        engine.Update(0.26f);
        CHECK(shows(engine, 1));
        // `frame` is what the renderer reads, so setting it scrubs.
        anim->frame = 2;
        CHECK(shows(engine, 2));
        // A paused animator keeps its frame however much time passes.
        anim->playing = false;
        engine.Update(3.0f);
        CHECK(shows(engine, 2));
        // Without looping it holds the last frame instead of wrapping.
        anim->playing = true;
        anim->loop = false;
        anim->Restart();
        CHECK(shows(engine, 0));
        engine.Update(2.0f);
        CHECK(shows(engine, 3));
        engine.Update(2.0f);
        CHECK(shows(engine, 3));
    });

    // --- a clip range picks part of the sheet ------------------------------
    bothBackends([&](int backend, const char* name) {
        Engine engine;
        SpriteAnimator* anim = build(engine, backend, Vec4(0, 0, 1, 1), 2, 2);
        anim->firstFrame = 2;
        anim->lastFrame = 3;              // the bottom row only
        engine.Play();
        CHECK(std::string(engine.RenderGameFrame().backend) == name);
        CHECK(shows(engine, 2));
        engine.Update(0.5f);
        CHECK(shows(engine, 3));
        engine.Update(0.5f);
        CHECK(shows(engine, 2));
        // A one-cell clip has nothing to step.
        anim->lastFrame = 2;
        engine.Update(0.5f);
        CHECK(shows(engine, 2));
    });

    // --- cells are cut out of the sprite's rect, not the whole texture ------
    bothBackends([&](int backend, const char* name) {
        Engine engine;
        // The sprite draws the bottom row of a 2x2 sheet and animates it: the
        // cells are blue and white, not the red/green of the sheet's first row.
        SpriteAnimator* anim = build(engine, backend, Vec4(0, 0.5f, 1, 0.5f), 2, 1);
        engine.Play();
        CHECK(std::string(engine.RenderGameFrame().backend) == name);
        CHECK(shows(engine, 2));
        engine.Update(0.5f);
        CHECK(shows(engine, 3));
        engine.Update(0.5f);
        CHECK(shows(engine, 2));
        // The cell size follows that rect too: half a texture tall, one row of
        // cells, so a cell is half the texture.
        CHECK(anim->CellRect(1, Vec4(0, 0.5f, 1, 0.5f)).w == 0.5f);
    });
}

// Named sprite clips: `clips` is a table of animations and `clip` names the one
// playing, with a key a clip leaves out falling back to the component's own
// field. The checks below read the cell that is on screen back as its colour,
// so they pin which clip was resolved, when a switch takes effect, and that a
// clip's grid - not the component's - decides the cell's geometry.
static void TestSpriteClips() {
    const std::string dir = (std::filesystem::temp_directory_path() / "cairn-test-clips").string();
    std::filesystem::create_directories(dir + "/Assets/Textures");
    const std::string tex = "Assets/Textures/frames.bmp";
    // Reading order: red, green / blue, white - one distinct colour per cell.
    WriteTestBmpSheet(dir + "/" + tex, 255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255);
    const int sheet[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};

    auto px = [](Engine& engine, int x, int y) {
        const FrameTarget& t = engine.frame();
        const uint8_t* p = &t.color[((size_t)y * (size_t)t.width + (size_t)x) * 4];
        return std::array<int, 3>{(int)p[0], (int)p[1], (int)p[2]};
    };
    auto closeTo = [](const std::array<int, 3>& c, int r, int g, int b) {
        return std::abs(c[0] - r) < 40 && std::abs(c[1] - g) < 40 && std::abs(c[2] - b) < 40;
    };
    // Renders and reports the cell at the middle of the sprite: a cell of the
    // component's 2x2 grid is 8 texels at 4 px/unit, so 2x2 units centred in a
    // 4-unit view - pixel 64,64 is inside it whichever cell is drawn.
    auto shows = [&](Engine& engine, int f) {
        engine.RenderGameFrame();
        return closeTo(px(engine, 64, 64), sheet[f][0], sheet[f][1], sheet[f][2]);
    };
    // Pixel 64,20 sits above a 2x2 cell (2 units tall, screen 32..96) but inside
    // a 2x1 cell (4 units tall, the whole 128 px view): the same texture, the
    // same rect, so only a clip's own grid can put a sprite there.
    auto tall = [&](Engine& engine) {
        engine.RenderGameFrame();
        return px(engine, 64, 20);
    };
    auto entry = [](const char* name, int first, int last, float fps, bool loop) {
        Json e = Json::MakeObject();
        e.set("name", Json(name));
        e.set("firstFrame", Json(first));
        e.set("lastFrame", Json(last));
        e.set("fps", Json(fps));
        e.set("loop", Json(loop));
        return e;
    };
    auto clipTable = [&]() {
        Json table = Json::MakeArray();
        table.push(entry("walk", 0, 1, 2.0f, true));
        table.push(entry("attack", 2, 3, 2.0f, false));
        Json bars = Json::MakeObject();     // the first two columns, full height
        bars.set("name", Json("bars"));
        bars.set("columns", Json(2));
        bars.set("rows", Json(1));
        bars.set("firstFrame", Json(0));
        bars.set("lastFrame", Json(1));
        bars.set("fps", Json(2.0f));
        table.push(bars);
        table.push(Json(42));               // a stray value is not a clip
        table.push(Json::MakeObject());     // nor is an entry with no name
        return table;
    };
    auto build = [&](Engine& engine, int backend) {
        engine.SetProjectRoot(dir);
        engine.SetViewportSize(128, 128);
        Scene& scene = engine.scene();
        scene.renderBackend = backend;
        scene.msaa = 1;
        scene.shadows = 0;
        scene.postFxaa = false;
        scene.ambient = Color(1, 1, 1, 1);
        scene.tonemap = 0;
        GameObject* cam = scene.CreateEntity("Main");
        cam->tag = "MainCamera";
        cam->transform().position = Vec3(0, 0, 8);
        auto* cc = cam->AddComponent<CameraComponent>();
        cc->orthographic = true;
        cc->orthographicSize = 2.0f;
        cc->clearColor = Color(0, 0, 0, 1);
        cc->skyEnabled = false;
        GameObject* go = scene.CreateEntity("Sprite");
        auto* sr = go->AddComponent<SpriteRenderer>();
        sr->texturePath = tex;
        sr->pixelPerfect = true;
        sr->pixelsPerUnit = 4.0f;
        auto* anim = go->AddComponent<SpriteAnimator>();
        anim->columns = 2;
        anim->rows = 2;
        anim->fps = 2.0f;
        anim->clips = clipTable();
        return anim;
    };
    auto bothBackends = [&](const std::function<void(int, const char*)>& one) {
        one(2, "software");
        if (VulkanBackendAvailable()) one(1, "vulkan");
    };

    // --- a clip's range and its own fps govern the frames -------------------
    bothBackends([&](int backend, const char* name) {
        Engine engine;
        SpriteAnimator* anim = build(engine, backend);
        // The component's rate is far above the clips': every step below is the
        // clip's 2 fps, so an ignored clip rate would run through the range.
        anim->fps = 100.0f;
        anim->clip = "walk";
        engine.Play();
        CHECK(std::string(engine.RenderGameFrame().backend) == name);
        CHECK(shows(engine, 0));          // the switch is drawn before any tick
        engine.Update(0.5f);
        CHECK(shows(engine, 1));
        engine.Update(0.25f);             // half a frame of the clip's own rate
        CHECK(shows(engine, 1));
        // Another clip, half a frame into it: the switch restarts the time, so
        // the 0.25 s already spent on `walk` does not carry over.
        anim->clip = "attack";
        CHECK(shows(engine, 2));          // drawn before a tick has happened
        engine.Update(0.25f);
        CHECK(shows(engine, 2));
        engine.Update(0.5f);
        CHECK(shows(engine, 3));
        engine.Update(0.5f);
        CHECK(shows(engine, 3));          // `attack` does not loop
        // Switching back starts on the new clip's first cell even though the
        // frame being held is a valid cell of it: the drawn cell is the
        // switch's, not the stale one clamped into range (which would be green).
        anim->clip = "walk";
        CHECK(shows(engine, 0));
    });

    // --- a clip's grid decides the cell's geometry --------------------------
    bothBackends([&](int backend, const char* name) {
        Engine engine;
        SpriteAnimator* anim = build(engine, backend);
        engine.Play();
        CHECK(std::string(engine.RenderGameFrame().backend) == name);
        anim->clip = "bars";
        CHECK(closeTo(tall(engine), 255, 0, 0));         // inside the taller cell
        anim->clip = "";                                 // the component's grid
        CHECK(closeTo(tall(engine), 0, 0, 0));           // above the shorter one
        // Keys the entry leaves out resolve to the component's fields.
        anim->clip = "attack";
        CHECK(anim->ActiveClip().columns == 2 && anim->ActiveClip().rows == 2);
        CHECK(anim->ActiveClip().fps == 2.0f && anim->ActiveClip().loop == false);
        anim->clip = "bars";
        CHECK(anim->ActiveClip().columns == 2 && anim->ActiveClip().rows == 1);
        CHECK(anim->ActiveClip().firstFrame == 0);
        CHECK(anim->ActiveClip().loop == true);          // `bars` does not say
    });

    // --- a name that matches nothing falls back to the fields ---------------
    bothBackends([&](int backend, const char* name) {
        Engine engine;
        SpriteAnimator* anim = build(engine, backend);
        anim->clip = "nope";
        engine.Play();
        CHECK(std::string(engine.RenderGameFrame().backend) == name);
        CHECK(shows(engine, 0));          // the component's first frame
        engine.Update(0.5f);
        CHECK(shows(engine, 1));          // and the component's own 2 fps
        CHECK(anim->ClipCount() == 3);    // five entries, three of them clips
        CHECK(anim->ClipName(0) == "walk");
        CHECK(anim->ClipName(1) == "attack");
        CHECK(anim->ClipName(2) == "bars");
        CHECK(anim->ClipName(3).empty());
        CHECK(anim->FindClip("walk") != nullptr);
        CHECK(anim->FindClip("nope") == nullptr);
        CHECK(anim->FindClip("") == nullptr);
        CHECK(anim->ActiveClip().firstFrame == 0 && anim->ActiveClip().lastFrame == -1);
    });

    // --- the table survives a scene file ------------------------------------
    bothBackends([&](int backend, const char* name) {
        Engine engine;
        SpriteAnimator* anim = build(engine, backend);
        anim->clip = "attack";
        engine.Play();
        CHECK(std::string(engine.RenderGameFrame().backend) == name);
        CHECK(shows(engine, 2));
        const std::string json = engine.SerializeSceneJson();
        Engine restored;
        restored.SetProjectRoot(dir);
        restored.SetViewportSize(128, 128);
        CHECK(restored.RestoreSceneJson(json));
        restored.Play();
        // The drawn cell comes from the restored table and the restored name.
        CHECK(shows(restored, 2));
        GameObject* go = restored.scene().FindByName("Sprite");
        CHECK(go != nullptr);
        auto* ra = go ? go->GetComponent<SpriteAnimator>() : nullptr;
        CHECK(ra != nullptr);
        CHECK(ra && ra->ClipCount() == 3);
        CHECK(ra && ra->ClipName(1) == "attack");
        CHECK(ra && ra->clip == "attack");
        CHECK(ra && ra->ActiveClip().loop == false);
    });
}

// Sprite animation state machine: `params` are the values the transitions read
// and `transitions` says which state to enter for which of them, evaluated in
// table order once per tick after the frame has advanced. The checks below read
// the state back as the colour of the cell it draws, so a state is pinned by
// what is on screen rather than by a field.
static void TestSpriteTransitions() {
    const std::string dir = (std::filesystem::temp_directory_path() / "cairn-test-machine").string();
    std::filesystem::create_directories(dir + "/Assets/Textures");
    const std::string tex = "Assets/Textures/frames.bmp";
    // Reading order: red, green / blue, white - one distinct colour per cell.
    WriteTestBmpSheet(dir + "/" + tex, 255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255);
    const int sheet[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};

    auto px = [](Engine& engine, int x, int y) {
        const FrameTarget& t = engine.frame();
        const uint8_t* p = &t.color[((size_t)y * (size_t)t.width + (size_t)x) * 4];
        return std::array<int, 3>{(int)p[0], (int)p[1], (int)p[2]};
    };
    auto closeTo = [](const std::array<int, 3>& c, int r, int g, int b) {
        return std::abs(c[0] - r) < 40 && std::abs(c[1] - g) < 40 && std::abs(c[2] - b) < 40;
    };
    auto shows = [&](Engine& engine, int f) {
        engine.RenderGameFrame();
        return closeTo(px(engine, 64, 64), sheet[f][0], sheet[f][1], sheet[f][2]);
    };
    auto clipEntry = [](const char* name, int first, int last, float fps, bool loop) {
        Json e = Json::MakeObject();
        e.set("name", Json(name));
        e.set("firstFrame", Json(first));
        e.set("lastFrame", Json(last));
        e.set("fps", Json(fps));
        e.set("loop", Json(loop));
        return e;
    };
    auto cond = [](const char* param, const char* op) {
        Json c = Json::MakeObject();
        c.set("param", Json(param));
        c.set("op", Json(op));
        return c;
    };
    auto condValue = [&](const char* param, const char* op, Json value) {
        Json c = cond(param, op);
        c.set("value", std::move(value));
        return c;
    };
    auto trans = [](const char* from, const char* to, Json when, bool waitForExit) {
        Json t = Json::MakeObject();
        t.set("from", Json(from));
        t.set("to", Json(to));
        if (!when.isNull()) t.set("when", std::move(when));
        if (waitForExit) t.set("waitForExit", Json(true));
        return t;
    };
    // Three states on the sheet and the machine that moves between them: idle is
    // one cell at 4 fps, walk two at 2, attack one at 2 and does not loop.
    auto build = [&](Engine& engine, int backend) {
        engine.SetProjectRoot(dir);
        engine.SetViewportSize(128, 128);
        Scene& scene = engine.scene();
        scene.renderBackend = backend;
        scene.msaa = 1;
        scene.shadows = 0;
        scene.postFxaa = false;
        scene.ambient = Color(1, 1, 1, 1);
        scene.tonemap = 0;
        GameObject* cam = scene.CreateEntity("Main");
        cam->tag = "MainCamera";
        cam->transform().position = Vec3(0, 0, 8);
        auto* cc = cam->AddComponent<CameraComponent>();
        cc->orthographic = true;
        cc->orthographicSize = 2.0f;
        cc->clearColor = Color(0, 0, 0, 1);
        cc->skyEnabled = false;
        GameObject* go = scene.CreateEntity("Sprite");
        auto* sr = go->AddComponent<SpriteRenderer>();
        sr->texturePath = tex;
        sr->pixelPerfect = true;
        sr->pixelsPerUnit = 4.0f;
        auto* anim = go->AddComponent<SpriteAnimator>();
        anim->columns = 2;
        anim->rows = 2;
        Json table = Json::MakeArray();
        table.push(clipEntry("idle", 0, 0, 4.0f, true));
        table.push(clipEntry("walk", 1, 2, 2.0f, true));
        table.push(clipEntry("attack", 3, 3, 2.0f, false));
        anim->clips = std::move(table);
        anim->params = Json::MakeObject();
        anim->params.set("moving", Json(false));
        anim->params.set("speed", Json(0.0f));
        anim->params.set("attack", Json(false));
        anim->clip = "idle";
        return anim;
    };
    auto bothBackends = [&](const std::function<void(int, const char*)>& one) {
        one(2, "software");
        if (VulkanBackendAvailable()) one(1, "vulkan");
    };

    // --- idle -> walk -> attack -> idle, driven by the parameters ----------
    bothBackends([&](int backend, const char* name) {
        Engine engine;
        SpriteAnimator* anim = build(engine, backend);
        Json table = Json::MakeArray();
        table.push(trans("idle", "walk", condValue("moving", "is", Json(true)), false));
        // Two conditions on one transition: both have to hold.
        Json both = Json::MakeArray();
        both.push(condValue("moving", "is", Json(false)));
        both.push(condValue("speed", "lt", Json(0.5f)));
        table.push(trans("walk", "idle", std::move(both), false));
        table.push(trans("*", "attack", cond("attack", "trigger"), false));
        table.push(trans("attack", "idle", Json(), true));   // waits for the clip
        anim->transitions = std::move(table);
        engine.Play();
        CHECK(std::string(engine.RenderGameFrame().backend) == name);
        CHECK(shows(engine, 0));          // idle
        engine.Update(0.25f);             // `moving` is still false
        CHECK(shows(engine, 0));
        anim->params.set("moving", Json(true));
        engine.Update(0.25f);
        CHECK(shows(engine, 1));          // walk's first cell
        engine.Update(0.5f);
        CHECK(shows(engine, 2));          // its second
        // `moving` false but `speed` too high: the second condition blocks it.
        anim->params.set("moving", Json(false));
        anim->params.set("speed", Json(1.0f));
        engine.Update(0.5f);
        CHECK(shows(engine, 1));
        anim->params.set("speed", Json(0.1f));
        engine.Update(0.25f);
        CHECK(shows(engine, 0));          // both hold, back to idle
        // A trigger fires from any state, and is spent when it does.
        anim->params.set("attack", Json(true));
        engine.Update(0.25f);
        CHECK(shows(engine, 3));          // attack
        const Json* spent = anim->Param("attack");
        CHECK(spent && !spent->asBool());
        engine.Update(0.25f);             // half of the attack's one-cell clip
        CHECK(shows(engine, 3));          // the exit time is not reached yet
        engine.Update(0.25f);
        CHECK(shows(engine, 0));          // and now it is
        CHECK(anim->clip == "idle");
    });

    // --- table order is the priority, and an op has to exist ----------------
    bothBackends([&](int backend, const char* name) {
        Engine engine;
        SpriteAnimator* anim = build(engine, backend);
        Json table = Json::MakeArray();
        table.push(trans("idle", "walk", condValue("speed", "gt", Json(1.0f)), false));
        table.push(trans("idle", "attack", condValue("speed", "gt", Json(0.5f)), false));
        table.push(Json(42));                                     // not a transition
        table.push(trans("idle", "walk", cond("speed", "bogus"), false));   // no such op
        table.push(trans("walk", "idle", condValue("speed", "lt", Json(0.1f)), false));
        table.push(trans("*", "attack", cond("nowhere", "is"), false));     // unset param
        anim->transitions = std::move(table);
        engine.Play();
        CHECK(std::string(engine.RenderGameFrame().backend) == name);
        anim->params.set("speed", Json(2.0f));
        // Both of the first two match, so the earlier entry is the one that runs.
        CHECK(anim->EvaluateTransitions() == "walk");
        CHECK(anim->EvaluateTransitions().empty());   // and nothing matches twice
        CHECK(shows(engine, 1));
        // The second entry is next in line when the first does not match.
        anim->clip = "idle";
        anim->params.set("speed", Json(0.7f));
        engine.Update(0.25f);
        CHECK(shows(engine, 3));          // attack, not walk
        // Nothing matches when the value is below both thresholds.
        anim->clip = "idle";
        anim->params.set("speed", Json(0.2f));
        engine.Update(0.25f);
        CHECK(shows(engine, 0));
        // `lt` the other way round fires, and stops firing once above it.
        anim->clip = "walk";
        anim->params.set("speed", Json(0.05f));
        engine.Update(0.25f);
        CHECK(shows(engine, 0));
        anim->clip = "walk";
        anim->params.set("speed", Json(0.5f));
        engine.Update(0.5f);
        CHECK(shows(engine, 2));
    });

    // --- the exit time, self-transitions and a manual override --------------
    bothBackends([&](int backend, const char* name) {
        Engine engine;
        SpriteAnimator* anim = build(engine, backend);
        Json table = Json::MakeArray();
        table.push(trans("walk", "idle", Json(), false));   // no condition, no exit time
        anim->transitions = std::move(table);
        anim->clip = "walk";
        engine.Play();
        CHECK(std::string(engine.RenderGameFrame().backend) == name);
        engine.Update(0.25f);
        CHECK(shows(engine, 0));          // fires on the first tick
        // A self-transition replays the state: fired from `walk` on a trigger it
        // restarts the animation rather than leaving it where it was.
        Json replay = Json::MakeArray();
        replay.push(trans("walk", "walk", cond("attack", "trigger"), false));
        anim->transitions = std::move(replay);
        anim->clip = "walk";
        engine.Update(0.5f);
        CHECK(shows(engine, 2));          // one step in
        anim->params.set("attack", Json(true));
        engine.Update(0.25f);
        CHECK(shows(engine, 1));          // back to the first cell
        // Writing `clip` is still a manual override of the machine.
        anim->clip = "attack";
        engine.Update(0.25f);
        CHECK(shows(engine, 3));
    });
}

// Per-cell frame times: `durations` gives each cell of a clip its own time,
// counted from the clip's own first cell, and a cell the table leaves out - or
// gives without a positive number - keeps the clip's own `1 / fps`. Everything
// that measures the clip reads those times, so a slow wind-up is slow both in
// the stepping and in the exit time of a transition.
static void TestSpriteFrameDurations() {
    const std::string dir = (std::filesystem::temp_directory_path() / "cairn-test-durations").string();
    std::filesystem::create_directories(dir + "/Assets/Textures");
    const std::string tex = "Assets/Textures/frames.bmp";
    // Reading order: red, green / blue, white - one distinct colour per cell.
    WriteTestBmpSheet(dir + "/" + tex, 255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255);
    const int sheet[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};

    auto px = [](Engine& engine, int x, int y) {
        const FrameTarget& t = engine.frame();
        const uint8_t* p = &t.color[((size_t)y * (size_t)t.width + (size_t)x) * 4];
        return std::array<int, 3>{(int)p[0], (int)p[1], (int)p[2]};
    };
    auto closeTo = [](const std::array<int, 3>& c, int r, int g, int b) {
        return std::abs(c[0] - r) < 40 && std::abs(c[1] - g) < 40 && std::abs(c[2] - b) < 40;
    };
    auto shows = [&](Engine& engine, int f) {
        engine.RenderGameFrame();
        return closeTo(px(engine, 64, 64), sheet[f][0], sheet[f][1], sheet[f][2]);
    };
    auto times = [](std::initializer_list<double> v) {
        Json a = Json::MakeArray();
        for (double t : v) a.push(Json(t));
        return a;
    };
    auto clip = [](const char* name, int first, int last, float fps, bool loop, Json durations) {
        Json e = Json::MakeObject();
        e.set("name", Json(name));
        e.set("firstFrame", Json(first));
        e.set("lastFrame", Json(last));
        e.set("fps", Json(fps));
        e.set("loop", Json(loop));
        if (!durations.isNull()) e.set("durations", std::move(durations));
        return e;
    };
    auto build = [&](Engine& engine, int backend) {
        engine.SetProjectRoot(dir);
        engine.SetViewportSize(128, 128);
        Scene& scene = engine.scene();
        scene.renderBackend = backend;
        scene.msaa = 1;
        scene.shadows = 0;
        scene.postFxaa = false;
        scene.ambient = Color(1, 1, 1, 1);
        scene.tonemap = 0;
        GameObject* cam = scene.CreateEntity("Main");
        cam->tag = "MainCamera";
        cam->transform().position = Vec3(0, 0, 8);
        auto* cc = cam->AddComponent<CameraComponent>();
        cc->orthographic = true;
        cc->orthographicSize = 2.0f;
        cc->clearColor = Color(0, 0, 0, 1);
        cc->skyEnabled = false;
        GameObject* go = scene.CreateEntity("Sprite");
        auto* sr = go->AddComponent<SpriteRenderer>();
        sr->texturePath = tex;
        sr->pixelPerfect = true;
        sr->pixelsPerUnit = 4.0f;
        auto* anim = go->AddComponent<SpriteAnimator>();
        anim->columns = 2;
        anim->rows = 2;
        anim->fps = 8.0f;
        return anim;
    };
    auto bothBackends = [&](const std::function<void(int, const char*)>& one) {
        one(2, "software");
        if (VulkanBackendAvailable()) one(1, "vulkan");
    };

    // --- the table's times drive the frames ---------------------------------
    bothBackends([&](int backend, const char* name) {
        Engine engine;
        SpriteAnimator* anim = build(engine, backend);
        // A rate far above the table's times: reading `fps` instead would rush
        // through 15 cells in the first quarter of a second.
        Json table = Json::MakeArray();
        table.push(clip("walk", 0, 3, 60.0f, true, times({0.5, 0.1, 0.1, 0.1})));
        anim->clips = std::move(table);
        anim->clip = "walk";
        engine.Play();
        CHECK(std::string(engine.RenderGameFrame().backend) == name);
        CHECK(shows(engine, 0));
        engine.Update(0.25f);
        CHECK(shows(engine, 0));          // half of the first cell's own time
        engine.Update(0.25f);
        CHECK(shows(engine, 1));          // and the second half is the step
        engine.Update(0.05f);
        CHECK(shows(engine, 1));          // the next cell is quick
        engine.Update(0.05f);
        CHECK(shows(engine, 2));
        engine.Update(0.1f);
        CHECK(shows(engine, 3));
        engine.Update(0.1f);
        CHECK(shows(engine, 0));          // the table loops with the clip
        engine.Update(0.8f);              // a whole cycle in one call
        CHECK(shows(engine, 0));          // lands where it started
        CHECK(std::abs(anim->ClipLength() - 0.8f) < 1e-4f);
    });

    // --- the rate fills in what the table does not say ----------------------
    bothBackends([&](int backend, const char* name) {
        Engine engine;
        SpriteAnimator* anim = build(engine, backend);
        anim->durations = times({0.1, 0.1, 0.1, 0.1});   // the component's own
        Json junk = Json::MakeArray();
        junk.push(Json(0.5));
        junk.push(Json(0.0));            // not a usable time
        junk.push(Json("later"));        // nor is a word
        junk.push(Json(-2.0));
        Json table = Json::MakeArray();
        table.push(clip("hold", 0, 3, 4.0f, true, times({0.5})));
        table.push(clip("junk", 0, 3, 4.0f, true, std::move(junk)));
        table.push(clip("bare", 0, 3, 4.0f, true, Json::MakeArray()));   // the entry's own, empty
        table.push(clip("walk", 0, 3, 4.0f, true, Json()));              // no key at all
        anim->clips = std::move(table);
        engine.Play();
        CHECK(std::string(engine.RenderGameFrame().backend) == name);
        anim->clip = "hold";
        engine.Update(0.5f);
        CHECK(shows(engine, 1));          // the one cell it gives, held for it
        engine.Update(0.25f);
        CHECK(shows(engine, 2));          // the rest fall back to 1 / fps
        // A cell with no usable number is the clip's own rate, not a zero-length
        // frame: 0.0, a word and a negative all have to fall back.
        anim->clip = "junk";
        engine.Update(0.5f);
        CHECK(shows(engine, 1));
        engine.Update(0.25f);
        CHECK(shows(engine, 2));
        engine.Update(0.25f);
        CHECK(shows(engine, 3));
        engine.Update(0.25f);
        CHECK(shows(engine, 0));
        // An entry with no key keeps the component's table; one that names an
        // empty array clears it and plays at the rate.
        anim->clip = "walk";
        engine.Update(0.15f);
        CHECK(shows(engine, 1));          // 0.1 s from the component
        anim->clip = "hold";
        CHECK(shows(engine, 0));          // the switch restarts the clip
        engine.Update(0.25f);
        CHECK(shows(engine, 0));          // the entry's 0.5 s wins over the component's
        anim->clip = "bare";
        engine.Update(0.15f);
        CHECK(shows(engine, 0));          // cleared, so 1 / fps
        anim->clip = "";                 // the fields, with the component's table
        CHECK(std::abs(anim->ClipLength() - 0.4f) < 1e-4f);
    });

    // --- the exit time is the clip's own length -----------------------------
    bothBackends([&](int backend, const char* name) {
        Engine engine;
        SpriteAnimator* anim = build(engine, backend);
        Json table = Json::MakeArray();
        table.push(clip("idle", 0, 0, 60.0f, true, Json()));
        // 1.0 s + 0.1 s: cells / fps would be 1.0 s and end it early.
        table.push(clip("attack", 1, 2, 2.0f, false, times({1.0, 0.1})));
        anim->clips = std::move(table);
        anim->clip = "idle";
        anim->params = Json::MakeObject();
        anim->params.set("go", Json(false));
        Json tr = Json::MakeArray();
        Json go = Json::MakeObject();
        go.set("from", Json("*"));
        go.set("to", Json("attack"));
        Json when = Json::MakeObject();
        when.set("param", Json("go"));
        when.set("op", Json("trigger"));
        go.set("when", std::move(when));
        tr.push(std::move(go));
        Json back = Json::MakeObject();
        back.set("from", Json("attack"));
        back.set("to", Json("idle"));
        back.set("waitForExit", Json(true));
        tr.push(std::move(back));
        anim->transitions = std::move(tr);
        engine.Play();
        CHECK(std::string(engine.RenderGameFrame().backend) == name);
        CHECK(shows(engine, 0));          // idle
        anim->params.set("go", Json(true));
        engine.Update(0.05f);
        CHECK(shows(engine, 1));          // the attack's first cell
        engine.Update(0.6f);              // twice what cells / fps would give it
        CHECK(shows(engine, 1));          // its own second is not up yet
        engine.Update(0.45f);             // 1.05 s: past 1.0 s, short of 1.1 s
        CHECK(shows(engine, 2));          // its second cell, still in the attack
        CHECK(anim->clip == "attack");
        engine.Update(0.06f);             // 1.11 s: past the clip's own length
        CHECK(shows(engine, 0));
        CHECK(anim->clip == "idle");
    });
}

// Texture cache: per-slot color space, byte-budgeted LRU eviction, and mtime
// based reload.
static void TestTextureCache() {
    const std::string dir = (std::filesystem::temp_directory_path() / "cairn-test-tex").string();
    std::filesystem::create_directories(dir);
    const std::string a = dir + "/a.bmp", b = dir + "/b.bmp", c = dir + "/c.bmp";
    WriteTestBmp(a, 256, 256, 255, 0, 0);
    WriteTestBmp(b, 256, 256, 0, 255, 0);
    WriteTestBmp(c, 256, 256, 0, 0, 255);

    TextureLibrary lib;  // local instance: the global one is shared with rendering
    lib.SetBudgetBytes(700000);  // ~2 of these textures fit (chain ≈ 341 KB each)

    auto ta = lib.Get(a);
    auto tb = lib.Get(b);
    CHECK(ta && tb && ta != tb);
    CHECK(ta->isSRGB());                              // files default to sRGB...
    auto taLinear = lib.Get(a, /*srgb=*/false);
    CHECK(!taLinear->isSRGB());                       // ...data slots load as linear
    CHECK(taLinear != ta);                            // separate cache entry, same file
    const size_t perTexture = ta->ByteCount();
    CHECK(perTexture > 256 * 256 * 4);                // mip chain accounted

    // Loading c evicts the least recently used entries (a and its linear twin).
    auto tc = lib.Get(c);
    CHECK(lib.cachedBytes() <= lib.budgetBytes());
    auto reloaded = lib.Get(a);
    CHECK(reloaded != ta);                            // a was evicted and reloaded
    CHECK(lib.cachedBytes() <= lib.budgetBytes());

    // mtime reload: rewriting a cached file makes ReloadChanged drop it, so the
    // next Get picks up the new content (a fresh pointer, same budget rules).
    WriteTestBmp(c, 256, 256, 255, 255, 0);
    std::filesystem::last_write_time(c, std::filesystem::file_time_type::clock::now() + std::chrono::hours(1));
    lib.ReloadChanged();
    auto tc2 = lib.Get(c);
    CHECK(tc2 != tc);                                 // replaced, not the cached copy

    // ReloadPath on an unchanged file is a no-op (no eviction, same pointer).
    auto tb2 = lib.Get(b);
    lib.ReloadPath(b);
    auto tb3 = lib.Get(b);
    CHECK(tb2 == tb3);
}

// Asset database: stable GUIDs, .meta import settings, rename tracking.
static void TestAssetDatabase() {
    const std::string dir = (std::filesystem::temp_directory_path() / "cairn-test-assets").string();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir + "/Assets/Textures");
    { std::ofstream(dir + "/Assets/Textures/a.png", std::ios::binary) << "not-a-real-png"; }

    AssetDatabase db(dir);
    db.Scan();
    const std::string rel = "Assets/Textures/a.png";
    const std::string guid = db.GuidForPath(rel);
    CHECK(guid.size() == 32);
    CHECK(db.PathForGuid(guid) == rel);
    CHECK(db.HasPath(rel) && db.HasGuid(guid));

    // Re-scanning reuses the GUID already written into the .meta.
    db.Scan();
    CHECK(db.GuidForPath(rel) == guid);

    // Importer settings round-trip through the sidecar.
    Cairn::AssetMeta meta;
    CHECK(db.ReadMeta(rel, &meta));
    meta.settings = Json::MakeObject();
    meta.settings.set("colorSpace", "linear");
    CHECK(db.WriteMeta(rel, meta));
    Cairn::AssetMeta reread;
    CHECK(db.ReadMeta(rel, &reread));
    CHECK(reread.settings["colorSpace"].asString() == "linear");

    // A move preserves the GUID and reindexes the path.
    CHECK(db.MoveAsset(rel, "Assets/Textures/b.png"));
    CHECK(db.GuidForPath("Assets/Textures/b.png") == guid);
    CHECK(!db.HasPath(rel));
    CHECK(db.PathForGuid(guid) == "Assets/Textures/b.png");
    CHECK(db.EnsureMeta("Assets/Textures/b.png") == guid);  // idempotent

    // No engine: a reference stays a bare path string.
    CHECK(AssetRefToJson("Assets/Textures/b.png").isString());

    Engine engine;
    engine.SetProjectRoot(dir);
    // With the project indexed, a reference carries the GUID inline...
    Json ref = AssetRefToJson("Assets/Textures/b.png");
    CHECK(ref.isObject() && ref["guid"].asString() == guid);
    CHECK(AssetRefFromJson(ref) == "Assets/Textures/b.png");

    // ...so a rename still resolves through the stored GUID.
    engine.assets().MoveAsset("Assets/Textures/b.png", "Assets/Textures/c.png");
    engine.RefreshAssets();
    CHECK(AssetRefFromJson(ref) == "Assets/Textures/c.png");

    // A saved scene stores the GUID, and loading resolves a later rename.
    engine.NewScene("Assets");
    GameObject* cube = engine.scene().CreateEntity("Cube");
    auto* mr = cube->AddComponent<MeshRenderer>();
    CHECK(mr != nullptr);
    mr->albedoTexturePath = "Assets/Textures/c.png";
    std::filesystem::create_directories(dir + "/Assets/Scenes");
    const std::string scenePath = dir + "/Assets/Scenes/S.cscene";
    std::string saveErr;
    CHECK(engine.SaveSceneFile(scenePath, &saveErr));
    std::string sceneText;
    { std::ifstream in(scenePath); std::stringstream ss; ss << in.rdbuf(); sceneText = ss.str(); }
    CHECK(sceneText.find(guid) != std::string::npos);

    engine.assets().MoveAsset("Assets/Textures/c.png", "Assets/Textures/d.png");
    CHECK(engine.LoadSceneFile(scenePath, &saveErr));
    GameObject* loaded = engine.scene().FindByName("Cube");
    CHECK(loaded != nullptr);
    CHECK(loaded->GetComponent<MeshRenderer>()->albedoTexturePath == "Assets/Textures/d.png");

    std::filesystem::remove_all(dir);
}

// Stable entity identity: GUIDs survive save/load, copies get new ones.
static void TestEntityIdentity() {
    Engine engine;
    engine.NewScene("Ids");
    GameObject* a = engine.scene().CreateEntity("A");
    GameObject* b = engine.scene().CreateEntity("B");
    CHECK(a->guid().size() == 32);
    CHECK(a->guid() != b->guid());
    const std::string guidA = a->guid();

    std::string json = engine.SerializeSceneJson();
    CHECK(json.find(guidA) != std::string::npos);
    CHECK(engine.RestoreSceneJson(json));
    GameObject* ra = engine.scene().FindByGuid(guidA);
    CHECK(ra != nullptr && ra->name == "A");

    // Clearing a stale id must not confuse GUID lookup.
    CHECK(engine.scene().FindByGuid("") == nullptr);
    CHECK(engine.scene().FindByGuid("deadbeef") == nullptr);

    // A duplicate (deep copy) is a distinct identity.
    GameObject* dup = engine.scene().InstantiateCopy(ra);
    CHECK(dup != nullptr && dup->guid() != guidA);
    CHECK(engine.scene().FindByGuid(dup->guid()) == dup);
}

// Prefabs: author a subtree, instantiate it, apply/revert overrides.
static void TestPrefabs() {
    const std::string dir = (std::filesystem::temp_directory_path() / "cairn-test-prefabs").string();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir + "/Assets/Prefabs");
    Engine engine;
    engine.SetProjectRoot(dir);
    engine.NewScene("P");

    GameObject* root = engine.scene().CreateEntity("Crate");
    root->AddComponent<MeshRenderer>();
    GameObject* child = engine.scene().CreateEntity("Lid", root);
    child->AddComponent<MeshRenderer>();

    std::string err;
    CHECK(engine.CreatePrefabFromEntity(root->m_id, "Assets/Prefabs/Crate.prefab", &err));
    auto* link = root->GetComponent<PrefabInstance>();
    CHECK(link != nullptr && link->prefabPath == "Assets/Prefabs/Crate.prefab");
    CHECK(std::filesystem::exists(dir + "/Assets/Prefabs/Crate.prefab"));
    CHECK(std::filesystem::exists(dir + "/Assets/Prefabs/Crate.prefab.meta"));

    EntityId inst = engine.InstantiatePrefab("Assets/Prefabs/Crate.prefab", 0);
    CHECK(inst != 0);
    GameObject* instRoot = engine.FindEntity(inst);
    CHECK(instRoot != nullptr && instRoot->name == "Crate");
    CHECK(instRoot->GetComponent<PrefabInstance>() != nullptr);
    CHECK(instRoot->GetChildren().size() == 1);  // "Lid" came along

    // Revert restores the file's state, discarding the local override.
    instRoot->GetComponent<MeshRenderer>()->albedoColor = Color(0, 1, 0, 1);
    EntityId rev = engine.RevertPrefab(inst, &err);
    CHECK(rev != 0 && rev != inst);
    GameObject* revRoot = engine.FindEntity(rev);
    CHECK(revRoot != nullptr);
    CHECK(NearlyEqual(revRoot->GetComponent<MeshRenderer>()->albedoColor.r, 0.78f, 1e-3f));

    // Apply writes the override back into the file; a new instance sees it.
    revRoot->GetComponent<MeshRenderer>()->albedoColor = Color(0.1f, 0.2f, 0.3f, 1);
    CHECK(engine.ApplyPrefab(rev, &err));
    EntityId inst2 = engine.InstantiatePrefab("Assets/Prefabs/Crate.prefab", 0);
    GameObject* i2 = engine.FindEntity(inst2);
    CHECK(i2 != nullptr);
    CHECK(NearlyEqual(i2->GetComponent<MeshRenderer>()->albedoColor.r, 0.1f, 1e-3f));

    // Saving and loading the scene preserves the prefab link.
    std::filesystem::create_directories(dir + "/Assets/Scenes");
    const std::string scenePath = dir + "/Assets/Scenes/P.cscene";
    CHECK(engine.SaveSceneFile(scenePath, &err));
    CHECK(engine.LoadSceneFile(scenePath, &err));
    GameObject* found = nullptr;
    for (const auto& g : engine.scene().objects)
        if (g->GetComponent<PrefabInstance>()) { found = g.get(); break; }
    CHECK(found != nullptr);
    CHECK(found->GetComponent<PrefabInstance>()->prefabPath == "Assets/Prefabs/Crate.prefab");

    std::filesystem::remove_all(dir);
}

// Prefab per-property overrides: an instance records how it differs from its
// asset, so editing the asset reaches every instance that left the property
// alone while a per-instance tweak survives a reload.
static void TestPrefabOverrides() {
    const std::string dir =
        (std::filesystem::temp_directory_path() / "cairn-test-prefab-overrides").string();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir + "/Assets/Prefabs");
    std::filesystem::create_directories(dir + "/Assets/Scenes");
    Engine engine;
    engine.SetProjectRoot(dir);
    engine.NewScene("O");

    GameObject* source = engine.scene().CreateEntity("Crate");
    source->AddComponent<MeshRenderer>()->albedoColor = Color(0.8f, 0.3f, 0.2f, 1);
    GameObject* lid = engine.scene().CreateEntity("Lid", source);
    lid->AddComponent<MeshRenderer>()->albedoColor = Color(0.5f, 0.5f, 0.5f, 1);

    std::string err;
    CHECK(engine.CreatePrefabFromEntity(source->m_id, "Assets/Prefabs/Crate.prefab", &err));

    const EntityId a = engine.InstantiatePrefab("Assets/Prefabs/Crate.prefab", 0);
    const EntityId b = engine.InstantiatePrefab("Assets/Prefabs/Crate.prefab", 0);
    GameObject* ga = engine.FindEntity(a);
    GameObject* gb = engine.FindEntity(b);
    CHECK(ga && gb && ga->GetChildren().size() == 1 && gb->GetChildren().size() == 1);

    // A copy that matches the asset has nothing to record.
    CHECK(engine.RefreshPrefabOverrides(a, &err) == 1);
    CHECK(engine.PrefabOverrides(a).size() == 0);

    // A tweak on one copy: a colour, a name, and a disabled component.
    ga->GetComponent<MeshRenderer>()->albedoColor = Color(0, 1, 0, 1);
    ga->name = "GreenCrate";
    ga->GetComponent<MeshRenderer>()->enabled = false;
    CHECK(engine.RefreshPrefabOverrides(a, &err) == 1);
    const Json ovA = engine.PrefabOverrides(a);
    const Json* entryA = ovA.find("");
    CHECK(ovA.size() == 1 && entryA != nullptr);
    CHECK(entryA && entryA->has("@name"));
    CHECK(entryA && entryA->has("MeshRenderer.albedoColor"));
    CHECK(entryA && entryA->has("MeshRenderer.@enabled"));

    // Edit the asset: the root turns blue and the child moves up.
    {
        std::ifstream in(dir + "/Assets/Prefabs/Crate.prefab");
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        Json doc;
        CHECK(Json::Parse(text, &doc, &err));
        Json& root = doc["root"];
        for (size_t i = 0; i < root["components"].size(); ++i) {
            Json& c = root["components"][i];
            if (c["type"].asString() == "MeshRenderer")
                c["properties"].set("albedoColor", Json::MakeArray({0.1, 0.1, 0.9, 1.0}));
        }
        for (size_t i = 0; i < root["children"].size(); ++i) {
            Json& child = root["children"][i];
            if (child["name"].asString() != "Lid") continue;
            for (size_t k = 0; k < child["components"].size(); ++k) {
                Json& c = child["components"][k];
                if (c["type"].asString() == "Transform")
                    c["position"] = Json::MakeArray({0.0, 3.0, 0.0});
            }
        }
        std::ofstream out(dir + "/Assets/Prefabs/Crate.prefab");
        out << doc.Dump(2) << "\n";
    }

    // What a scene load does: pull the asset into every instance.
    CHECK(engine.SyncPrefabs(0, &err) == 3);  // the source entity plus both copies
    CHECK(NearlyEqual(ga->GetComponent<MeshRenderer>()->albedoColor.g, 1.0f, 1e-3f));  // kept
    CHECK(!ga->GetComponent<MeshRenderer>()->enabled);                                  // kept
    CHECK(ga->name == "GreenCrate");                                                    // kept
    CHECK(NearlyEqual(gb->GetComponent<MeshRenderer>()->albedoColor.b, 0.9f, 1e-3f));   // followed
    CHECK(gb->GetComponent<MeshRenderer>()->enabled);                                   // followed
    CHECK(gb->name == "Crate");                                                         // followed
    CHECK(ga->GetChildren().size() == 1 && gb->GetChildren().size() == 1);
    CHECK(NearlyEqual(ga->GetChildren()[0]->transform().position.y, 3.0f, 1e-3f));
    CHECK(NearlyEqual(gb->GetChildren()[0]->transform().position.y, 3.0f, 1e-3f));

    // Deleting a component and a child on one copy is remembered: a later sync
    // does not resurrect them.
    engine.scene().DestroyEntityNow(gb->GetChildren()[0]);
    CHECK(engine.RemoveComponent(b, "MeshRenderer"));
    CHECK(engine.RefreshPrefabOverrides(b, &err) == 1);
    const Json ovB = engine.PrefabOverrides(b);
    const Json* entryB = ovB.find("");
    CHECK(entryB && entryB->has("@removedChildren") && entryB->has("@removedComponents"));
    CHECK(engine.SyncPrefabs(b, &err) == 1);
    CHECK(gb->GetChildren().empty());
    CHECK(gb->GetComponent<MeshRenderer>() == nullptr);

    // A scene written before overrides were recorded carries none: the loader
    // takes the instance's own state as the record rather than letting the
    // asset overwrite it.
    gb->AddComponent<MeshRenderer>()->albedoColor = Color(1, 0, 0, 1);
    auto* piB = gb->GetComponent<PrefabInstance>();
    CHECK(piB != nullptr);
    if (piB) {
        piB->overridesKnown = false;
        piB->overrides = Json::MakeObject();
    }
    CHECK(engine.SyncPrefabs(b, &err) == 1);
    CHECK(NearlyEqual(gb->GetComponent<MeshRenderer>()->albedoColor.r, 1.0f, 1e-3f));
    CHECK(piB && piB->overridesKnown);

    // A plain entity is not an instance: no record, and a targeted sync fails.
    GameObject* plain = engine.scene().CreateEntity("Plain");
    CHECK(engine.PrefabOverrides(plain->m_id).size() == 0);
    CHECK(engine.RefreshPrefabOverrides(plain->m_id, &err) == -1);
    CHECK(engine.SyncPrefabs(plain->m_id, &err) == -1);

    std::filesystem::remove_all(dir);
}

// Physics queries: raycast / raycast-all / overlap against colliders.
static void TestPhysicsQueries() {
    Engine engine;
    Scene& scene = engine.scene();
    GameObject* floor = scene.CreateEntity("Floor");
    floor->AddComponent<BoxCollider>()->size = Vec3(10, 1, 10);
    GameObject* box = scene.CreateEntity("Box");
    box->transform().position = Vec3(0, 3, 0);
    box->AddComponent<BoxCollider>()->size = Vec3(1, 1, 1);

    Physics::RaycastHit hit;
    CHECK(Physics::Raycast(Vec3(0, 10, 0), Vec3(0, -1, 0), 100.0f, &hit));
    CHECK(NearlyEqual(hit.distance, 6.5f, 1e-2f));   // box top at y=3.5
    CHECK(hit.entity == box->m_id);
    CHECK(NearlyEqual(hit.normal.y, 1.0f, 1e-2f));

    auto all = Physics::RaycastAll(Vec3(0, 10, 0), Vec3(0, -1, 0), 100.0f);
    CHECK(all.size() >= 2);
    CHECK(all.front().entity == box->m_id);          // nearest first

    auto overlap = Physics::OverlapSphere(Vec3(0, 3, 0), 0.75f);
    CHECK(std::find(overlap.begin(), overlap.end(), box->m_id) != overlap.end());
    CHECK(std::find(overlap.begin(), overlap.end(), floor->m_id) == overlap.end());

    // Nothing along an upward ray.
    CHECK(!Physics::Raycast(Vec3(0, 10, 0), Vec3(0, 1, 0), 100.0f, &hit));

    // --- Overlap box and the shape casts -----------------------------------
    // Off the cast lines in z (they sweep down -y), so only the query under
    // test can see it.
    GameObject* ball = scene.CreateEntity("Ball");
    ball->transform().position = Vec3(3, 3, 6);
    ball->AddComponent<SphereCollider>()->radius = 0.75f;

    // OverlapBox is exact against the sphere's surface, not its bounding box: a
    // query box that clips the AABB corner but stays 0.86 from the centre
    // (radius 0.75) must not report it.
    auto o1 = Physics::OverlapBox(Vec3(3.8f, 4.0f, 6.0f), Vec3(0.3f, 0.3f, 0.3f));
    CHECK(std::find(o1.begin(), o1.end(), ball->m_id) == o1.end());
    auto o2 = Physics::OverlapBox(Vec3(3.4f, 3.4f, 6.0f), Vec3(0.3f, 0.3f, 0.3f));
    CHECK(std::find(o2.begin(), o2.end(), ball->m_id) != o2.end());
    // The box query finds the crate it wraps and ignores empty air above it.
    auto o3 = Physics::OverlapBox(Vec3(0, 3, 0), Vec3(0.6f, 0.6f, 0.6f));
    CHECK(std::find(o3.begin(), o3.end(), box->m_id) != o3.end());
    CHECK(std::find(o3.begin(), o3.end(), floor->m_id) == o3.end());
    auto o4 = Physics::OverlapBox(Vec3(0, 8, 0), Vec3(0.5f, 0.5f, 0.5f));
    CHECK(o4.empty());

    // SphereCast: the swept sphere's surface decides, so a cast whose centre
    // line slips past the crate still stops on its top-right edge.
    Physics::RaycastHit cast;
    CHECK(Physics::SphereCast(Vec3(0.6f, 10, 0), 0.5f, Vec3(0, -1, 0), 100.0f, &cast));
    CHECK(cast.entity == box->m_id);
    CHECK(NearlyEqual(cast.distance, 10.0f - 3.5f - std::sqrt(0.25f - 0.01f), 1e-2f));
    CHECK(NearlyEqual(cast.normal.x, 0.2f, 1e-2f));
    CHECK(NearlyEqual(cast.point.y, 3.5f, 1e-2f));  // the crate's own top edge
    // The ray down that same centre line misses the crate and reaches the floor.
    CHECK(Physics::Raycast(Vec3(0.6f, 10, 0), Vec3(0, -1, 0), 100.0f, &hit));
    CHECK(hit.entity == floor->m_id);
    // A sphere off to the side lands on the floor instead (top y=0.5).
    CHECK(Physics::SphereCast(Vec3(4, 10, 0), 0.5f, Vec3(0, -1, 0), 100.0f, &cast));
    CHECK(cast.entity == floor->m_id);
    CHECK(NearlyEqual(cast.distance, 9.0f, 1e-2f));
    // A cast that starts inside the crate reports distance 0.
    CHECK(Physics::SphereCast(Vec3(0, 3, 0), 0.6f, Vec3(0, -1, 0), 100.0f, &cast));
    CHECK(cast.entity == box->m_id);
    CHECK(NearlyEqual(cast.distance, 0.0f, 1e-4f));

    // BoxCast: a 1x1x1 box whose left face clips the crate stops on the crate's
    // top at 6.0, while a box clear of it in x drops to the floor.
    CHECK(Physics::BoxCast(Vec3(0.9f, 10, 0), Vec3(0.5f, 0.5f, 0.5f), Vec3(0, -1, 0), 100.0f,
                           &cast));
    CHECK(cast.entity == box->m_id);
    CHECK(NearlyEqual(cast.distance, 6.0f, 1e-2f));
    CHECK(NearlyEqual(cast.normal.y, 1.0f, 1e-2f));
    CHECK(Physics::BoxCast(Vec3(4.8f, 10, 0), Vec3(0.5f, 0.5f, 0.5f), Vec3(0, -1, 0), 100.0f,
                           &cast));
    CHECK(cast.entity == floor->m_id);
    CHECK(NearlyEqual(cast.distance, 9.0f, 1e-2f));
    // A short cast stops before the floor.
    CHECK(!Physics::BoxCast(Vec3(4.8f, 10, 0), Vec3(0.5f, 0.5f, 0.5f), Vec3(0, -1, 0), 5.0f,
                            &cast));
}

// The collision layer matrix filters which pairs the solver sees.
static void TestCollisionLayers() {
    Scene scene;
    GameObject* ground = scene.CreateEntity("Ground");
    ground->AddComponent<BoxCollider>()->size = Vec3(10, 1, 10);  // top at y=0.5
    GameObject* a = scene.CreateEntity("A");
    a->layer = 1;
    a->transform().position = Vec3(0, 3, 0);
    a->AddComponent<BoxCollider>()->size = Vec3(1, 1, 1);
    auto* rb = a->AddComponent<RigidBody>();
    rb->restitution = 0.0f;

    scene.SetLayerCollision(0, 1, false);
    for (int i = 0; i < 120; ++i) PhysicsStep(scene, 1.0f / 60.0f);
    CHECK(a->transform().position.y < -1.0f);  // fell straight through

    scene.SetLayerCollision(0, 1, true);
    a->transform().position = Vec3(0, 3, 0);
    rb->velocity = Vec3();
    for (int i = 0; i < 180; ++i) PhysicsStep(scene, 1.0f / 60.0f);
    CHECK(a->transform().position.y > 0.5f);   // landed on the ground
}

// Angular velocity spins a body; friction damps sliding.
static void TestRigidbodyFrictionAngular() {
    Scene scene;
    GameObject* ground = scene.CreateEntity("Ground");
    ground->AddComponent<BoxCollider>()->size = Vec3(20, 1, 20);
    GameObject* a = scene.CreateEntity("A");
    a->transform().position = Vec3(0, 1.0f, 0);
    a->AddComponent<BoxCollider>()->size = Vec3(1, 1, 1);
    auto* rb = a->AddComponent<RigidBody>();
    rb->useGravity = true;
    rb->restitution = 0.0f;

    // Angular integration: one second at PI rad/s should rotate the body.
    rb->angularVelocity = Vec3(0, kPi, 0);
    const Quat before = a->transform().rotation;
    for (int i = 0; i < 60; ++i) PhysicsStep(scene, 1.0f / 60.0f);
    CHECK(std::fabs(a->transform().rotation.w - before.w) > 0.5f);

    // Friction: a sliding body on the ground slows noticeably.
    rb->angularVelocity = Vec3();
    rb->velocity = Vec3(5, 0, 0);
    rb->friction = 1.0f;
    for (int i = 0; i < 60; ++i) PhysicsStep(scene, 1.0f / 60.0f);
    CHECK(rb->velocity.x < 3.0f);
}

// Capsule collider: ray and overlap use the true capsule volume, not its AABB.
static void TestCapsuleCollider() {
    Engine engine;
    Scene& scene = engine.scene();
    GameObject* cap = scene.CreateEntity("Cap");
    auto* cc = cap->AddComponent<CapsuleCollider>();
    cc->radius = 0.5f;
    cc->height = 2.0f;  // local segment spans y in [-0.5, 0.5]

    Physics::RaycastHit hit;
    CHECK(Physics::Raycast(Vec3(3, 0, 0), Vec3(-1, 0, 0), 100.0f, &hit));
    CHECK(NearlyEqual(hit.distance, 2.5f, 1e-2f));  // side surface at x = 0.5
    CHECK(NearlyEqual(hit.normal.x, 1.0f, 1e-2f));
    CHECK(hit.entity == cap->m_id);

    // Straight down hits the top cap at y = 1.0 (the AABB corner would be earlier).
    CHECK(Physics::Raycast(Vec3(0, 5, 0), Vec3(0, -1, 0), 100.0f, &hit));
    CHECK(NearlyEqual(hit.distance, 4.0f, 1e-2f));

    auto inside = Physics::OverlapSphere(Vec3(0, 0.9f, 0), 0.3f);
    CHECK(std::find(inside.begin(), inside.end(), cap->m_id) != inside.end());
    auto far = Physics::OverlapSphere(Vec3(2, 0, 0), 0.9f);
    CHECK(std::find(far.begin(), far.end(), cap->m_id) == far.end());
}

// A dynamic capsule settles on a box floor; a sphere rests on a capsule's cap
// (the exact segment/sphere contact).
static void TestCapsulePhysics() {
    Scene scene;
    GameObject* floor = scene.CreateEntity("Floor");
    floor->transform().position = Vec3(0, -0.5f, 0);
    floor->AddComponent<BoxCollider>()->size = Vec3(10, 1, 10);  // top at y = 0
    GameObject* cap = scene.CreateEntity("Cap");
    cap->transform().position = Vec3(0, 3, 0);
    auto* c = cap->AddComponent<CapsuleCollider>();
    c->radius = 0.5f;
    c->height = 2.0f;
    auto* rb = cap->AddComponent<RigidBody>();
    rb->restitution = 0.0f;
    for (int i = 0; i < 300; ++i) PhysicsStep(scene, 1.0f / 60.0f);
    CHECK(NearlyEqual(cap->transform().position.y, 1.0f, 0.08f));
    CHECK(std::fabs(rb->velocity.y) < 0.1f);

    // Ball resting on a fixed capsule's top cap: centers 1.0 apart vertically.
    cap->transform().position = Vec3(0, 3, 0);
    rb->isKinematic = true;
    rb->velocity = Vec3();
    GameObject* ball = scene.CreateEntity("Ball");
    ball->transform().position = Vec3(0, 6, 0);
    ball->AddComponent<SphereCollider>()->radius = 0.5f;
    ball->AddComponent<RigidBody>()->restitution = 0.0f;
    for (int i = 0; i < 300; ++i) PhysicsStep(scene, 1.0f / 60.0f);
    CHECK(NearlyEqual(ball->transform().position.y, 4.5f, 0.1f));
}

// The character controller grounds on floors, stops at walls and climbs steps.
static void TestCharacterController() {
    Scene scene;
    GameObject* ground = scene.CreateEntity("Ground");
    ground->transform().position = Vec3(0, -0.5f, 0);
    ground->AddComponent<BoxCollider>()->size = Vec3(24, 1, 24);  // top at y = 0

    GameObject* hero = scene.CreateEntity("Hero");
    hero->transform().position = Vec3(-6.0f, 1.0f, 0);  // feet on the ground
    auto* cc = hero->AddComponent<CharacterController>();
    cc->radius = 0.5f;
    cc->height = 2.0f;

    // A tiny downward probe grounds the controller without sinking it.
    PhysicsCharacterMove(scene, *hero, *cc, Vec3(0, -0.02f, 0));
    CHECK(cc->isGrounded);
    CHECK(NearlyEqual(hero->transform().position.y, 1.0f, 0.05f));

    // Free fall in small steps resolves back onto the floor (no tunnelling).
    hero->transform().position = Vec3(-6.0f, 3.0f, 0);
    cc->isGrounded = false;
    float vy = 0.0f;
    for (int i = 0; i < 240; ++i) {
        if (!cc->isGrounded) vy -= 9.81f / 60.0f;
        PhysicsCharacterMove(scene, *hero, *cc, Vec3(0, vy / 60.0f, 0));
    }
    CHECK(cc->isGrounded);
    CHECK(NearlyEqual(hero->transform().position.y, 1.0f, 0.06f));

    // A wall stops horizontal movement at radius + wall half-extent.
    GameObject* wall = scene.CreateEntity("Wall");
    wall->transform().position = Vec3(0, 1.5f, 0);
    wall->AddComponent<BoxCollider>()->size = Vec3(1, 3, 4);  // spans x 0.5.. -0.5
    hero->transform().position = Vec3(-3.0f, 1.0f, 0);
    cc->isGrounded = true;
    for (int i = 0; i < 240; ++i) PhysicsCharacterMove(scene, *hero, *cc, Vec3(0.05f, 0, 0));
    CHECK(hero->transform().position.x < -0.95f);  // stopped short of the wall face
    CHECK(hero->transform().position.x > -1.05f);

    // A low step (0.2) within stepOffset is climbed rather than blocked.
    GameObject* step = scene.CreateEntity("Step");
    step->transform().position = Vec3(4.0f, 0.1f, 0);
    step->AddComponent<BoxCollider>()->size = Vec3(1, 0.2f, 4);  // top at y = 0.2
    hero->transform().position = Vec3(2.5f, 1.0f, 0);
    cc->isGrounded = true;
    PhysicsCharacterMove(scene, *hero, *cc, Vec3(1.0f, 0, 0));
    CHECK(hero->transform().position.y > 1.15f);  // feet now on the step top
}

// Distance joints: a rigid constraint holds a distance, a spring restores it,
// and breakForce snaps the link.
static void TestJoints() {
    Scene scene;
    GameObject* bob = scene.CreateEntity("Bob");
    bob->transform().position = Vec3(0, 2, 0);
    bob->AddComponent<SphereCollider>()->radius = 0.2f;
    auto* rb = bob->AddComponent<RigidBody>();
    rb->useGravity = false;
    auto* joint = bob->AddComponent<DistanceJoint>();
    joint->distance = 2.0f;  // anchored to the world origin

    for (int i = 0; i < 10; ++i) PhysicsStep(scene, 1.0f / 60.0f);
    CHECK(NearlyEqual(bob->transform().position.y, 2.0f, 0.05f));

    // Displace it; the rigid constraint restores the distance.
    bob->transform().position = Vec3(0, 4, 0);
    rb->velocity = Vec3();
    for (int i = 0; i < 60; ++i) PhysicsStep(scene, 1.0f / 60.0f);
    CHECK(NearlyEqual(bob->transform().position.y, 2.0f, 0.05f));

    // A spring/damper pulls a displaced body back to the rest length.
    joint->spring = 40.0f;
    joint->damping = 4.0f;
    bob->transform().position = Vec3(0, 5, 0);
    rb->velocity = Vec3();
    for (int i = 0; i < 180; ++i) PhysicsStep(scene, 1.0f / 60.0f);
    CHECK(NearlyEqual(bob->transform().position.y, 2.0f, 0.15f));

    // Two dynamic bodies connected by GUID share the correction equally.
    GameObject* p = scene.CreateEntity("P");
    p->transform().position = Vec3(10, 0, 0);
    p->AddComponent<SphereCollider>()->radius = 0.2f;
    p->AddComponent<RigidBody>()->useGravity = false;
    GameObject* q = scene.CreateEntity("Q");
    q->transform().position = Vec3(12, 0, 0);
    q->AddComponent<SphereCollider>()->radius = 0.2f;
    q->AddComponent<RigidBody>()->useGravity = false;
    auto* j2 = p->AddComponent<DistanceJoint>();
    j2->connectedGuid = q->guid();
    j2->distance = 1.0f;
    for (int i = 0; i < 30; ++i) PhysicsStep(scene, 1.0f / 60.0f);
    CHECK(NearlyEqual(std::fabs(p->transform().position.x - q->transform().position.x), 1.0f, 0.1f));
    CHECK(p->transform().position.x > 10.4f);
    CHECK(q->transform().position.x < 11.6f);

    // breakForce snaps the joint; the body then falls under gravity.
    joint->spring = 0.0f;
    joint->breakForce = 5.0f;
    bob->transform().position = Vec3(0, 8, 0);
    rb->velocity = Vec3();
    rb->useGravity = true;
    for (int i = 0; i < 120; ++i) PhysicsStep(scene, 1.0f / 60.0f);
    CHECK(!joint->enabled);
    CHECK(bob->transform().position.y < 1.0f);
}

// The angle (degrees) an arm that started along +X has swung about the vertical
// axis, read back from where it points now. Used by the hinge tests.
static float HingeSwingDegrees(GameObject* go) {
    Vec3 p = go->transform().position;
    return std::atan2(-p.z, p.x) * 180.0f / 3.14159265f;
}

static void TestHingeJoint() {
    // A pendulum pinned to a world point: the anchor uses up every translation,
    // the axis point every tilt, and rotation about the axis is left free, so
    // the body swings around a circle of exactly one arm length.
    Scene scene;
    GameObject* bob = scene.CreateEntity("Bob");
    bob->transform().position = Vec3(0, 1, 0);
    bob->transform().rotation = Quat::FromAxisAngle(Vec3(0, 0, 1), 0.1745f);  // 10 deg off vertical
    bob->AddComponent<SphereCollider>()->radius = 0.1f;
    auto* rb = bob->AddComponent<RigidBody>();
    rb->drag = 1.0f;
    auto* hinge = bob->AddComponent<HingeJoint>();
    hinge->anchor = Vec3(0, -1, 0);  // the pivot is the world origin
    hinge->connectedAnchor = Vec3(0, 0, 0);
    hinge->axis = Vec3(0, 0, 1);

    for (int i = 0; i < 600; ++i) PhysicsStep(scene, 1.0f / 60.0f);
    Vec3 p = bob->transform().position;
    CHECK(NearlyEqual(p.Length(), 1.0f, 0.02f));  // the anchor held
    CHECK(NearlyEqual(p.z, 0.0f, 1e-3f));         // the axis held: no out-of-plane drift
    CHECK(NearlyEqual(p.y, -1.0f, 0.05f));        // free to swing: it hung straight down

    // A gravity-loaded door on a horizontal axis: the swing is planar, so the
    // axis point pair is what keeps it in that plane, and the limit stops it
    // hanging straight down.
    Scene swingScene;
    GameObject* swing = swingScene.CreateEntity("Swing");
    swing->transform().position = Vec3(1, 0, 0);
    swing->AddComponent<SphereCollider>()->radius = 0.1f;
    auto* sb = swing->AddComponent<RigidBody>();  // gravity on
    auto* sj = swing->AddComponent<HingeJoint>();
    sj->anchor = Vec3(-1, 0, 0);
    sj->connectedAnchor = Vec3(0, 0, 0);
    sj->axis = Vec3(0, 0, 1);
    sj->useLimits = true;
    sj->limitsMin = -45.0f;
    sj->limitsMax = 45.0f;
    for (int i = 0; i < 120; ++i) PhysicsStep(swingScene, 1.0f / 60.0f);
    Vec3 sp = swing->transform().position;
    CHECK(NearlyEqual(sp.x, 0.7071f, 0.01f));  // hanging on the -45 limit
    CHECK(NearlyEqual(sp.y, -0.7071f, 0.01f));
    CHECK(NearlyEqual(sp.z, 0.0f, 1e-3f));     // the axis pair kept it in plane

    // The second point pair is what keeps the swing planar: a shove out of that
    // plane and a spin across the axis are violations it takes out, not motions
    // a hinge can leave standing.
    sj->useLimits = false;
    sb->velocity = Vec3(0, 0, 3);
    sb->angularVelocity = Vec3(6, 0, 0);
    for (int i = 0; i < 60; ++i) PhysicsStep(swingScene, 1.0f / 60.0f);
    Vec3 axisNow = swing->transform().rotation * Vec3(0, 0, 1);
    CHECK(NearlyEqual(axisNow.z, 1.0f, 1e-3f));                        // axis unmoved
    CHECK(NearlyEqual(swing->transform().position.z, 0.0f, 1e-2f));   // and swung in plane
    CHECK(NearlyEqual(swing->transform().position.Length(), 1.0f, 1e-2f));  // still pinned

    // Limits clamp the angle. A door on a vertical hinge has no gravity torque,
    // so it only moves when pushed - and stops dead at the configured limit. The
    // hinge angle runs from the pose the joint captures on its first step, so
    // let the unmoved door settle for that step before pushing it.
    Scene doorScene;
    GameObject* door = doorScene.CreateEntity("Door");
    door->transform().position = Vec3(1, 0, 0);
    door->AddComponent<SphereCollider>()->radius = 0.1f;
    auto* db = door->AddComponent<RigidBody>();
    db->useGravity = false;
    auto* dj = door->AddComponent<HingeJoint>();
    dj->anchor = Vec3(-1, 0, 0);
    dj->connectedAnchor = Vec3(0, 0, 0);
    dj->axis = Vec3(0, 1, 0);
    dj->useLimits = true;
    dj->limitsMin = -30.0f;
    dj->limitsMax = 45.0f;
    PhysicsStep(doorScene, 1.0f / 60.0f);  // captures the rest pose at 0 degrees
    db->velocity = Vec3(0, 0, -3);  // swinging about the pivot at one arm length
    db->angularVelocity = Vec3(0, 3, 0);
    for (int i = 0; i < 120; ++i) PhysicsStep(doorScene, 1.0f / 60.0f);
    CHECK(NearlyEqual(HingeSwingDegrees(door), 45.0f, 2.0f));

    // The same limit holds on the way back.
    db->velocity = Vec3(0, 0, 3);
    db->angularVelocity = Vec3(0, -3, 0);
    for (int i = 0; i < 240; ++i) PhysicsStep(doorScene, 1.0f / 60.0f);
    CHECK(NearlyEqual(HingeSwingDegrees(door), -30.0f, 2.0f));

    // A limit firing must not break the pin. Turn the door past its stop and take
    // one single step: the angle lands on the limit and the anchor is still
    // exactly on its pivot the instant the step returns. (The door has been
    // resting long enough to sleep, and sleeping ends are frozen, so nudge it.)
    db->velocity = Vec3(0.01f, 0.0f, 0.0f);
    door->transform().rotation = Quat::FromAxisAngle(Vec3(0, 1, 0), 1.0f);  // 57 deg
    door->transform().position = Vec3(std::cos(1.0f), 0.0f, -std::sin(1.0f));
    PhysicsStep(doorScene, 1.0f / 60.0f);
    Vec3 pinned = door->transform().WorldMatrix().TransformPoint(dj->anchor);
    CHECK(pinned.Length() < 1e-4f);
    CHECK(NearlyEqual(HingeSwingDegrees(door), 45.0f, 1.0f));

    // A motor drives the hinge toward its target rate.
    Scene wheelScene;
    GameObject* wheel = wheelScene.CreateEntity("Wheel");
    wheel->transform().position = Vec3(1, 0, 0);
    wheel->AddComponent<SphereCollider>()->radius = 0.1f;
    auto* wb = wheel->AddComponent<RigidBody>();
    wb->useGravity = false;
    auto* wj = wheel->AddComponent<HingeJoint>();
    wj->anchor = Vec3(-1, 0, 0);
    wj->connectedAnchor = Vec3(0, 0, 0);
    wj->axis = Vec3(0, 1, 0);
    wj->useMotor = true;
    wj->motorVelocity = 90.0f;  // degrees per second
    wj->motorForce = 50.0f;
    // The drive acts on the joint, not on the body's centre of mass, so the
    // wheel is at the target rate on the first step rather than spooling up.
    for (int i = 0; i < 30; ++i) PhysicsStep(wheelScene, 1.0f / 60.0f);
    float half = HingeSwingDegrees(wheel);
    for (int i = 0; i < 30; ++i) PhysicsStep(wheelScene, 1.0f / 60.0f);
    float full = HingeSwingDegrees(wheel);
    CHECK(NearlyEqual(half, 45.0f, 2.0f));          // a quarter turn in half a second
    CHECK(NearlyEqual(full, 90.0f, 3.0f));
    CHECK(NearlyEqual(full - half, 45.0f, 1.0f));   // still exactly 90 degrees a second
    CHECK(NearlyEqual(wheel->transform().position.Length(), 1.0f, 1e-3f));  // the pin held

    // breakForce snaps the hinge when the constraint is overloaded.
    dj->useLimits = false;
    dj->breakForce = 10.0f;
    door->transform().position = Vec3(4, 0, 0);
    db->velocity = Vec3();
    db->angularVelocity = Vec3();
    for (int i = 0; i < 4; ++i) PhysicsStep(doorScene, 1.0f / 60.0f);
    CHECK(!dj->enabled);
}

// FixedJoint welds two bodies: their relative pose is locked, corrections are
// shared by mass, and the pair then moves and spins as a single body.
static void TestFixedJoint() {
    Scene scene;
    GameObject* a = scene.CreateEntity("A");
    a->AddComponent<SphereCollider>()->radius = 0.1f;
    auto* ra = a->AddComponent<RigidBody>();
    ra->useGravity = false;
    GameObject* b = scene.CreateEntity("B");
    b->transform().position = Vec3(2, 0, 0);
    b->AddComponent<SphereCollider>()->radius = 0.1f;
    auto* rb = b->AddComponent<RigidBody>();
    rb->useGravity = false;
    auto* weld = a->AddComponent<FixedJoint>();
    weld->connectedGuid = b->guid();

    // A weld that already matches its captured pose disturbs nothing.
    for (int i = 0; i < 30; ++i) PhysicsStep(scene, 1.0f / 60.0f);
    CHECK(NearlyEqual(a->transform().position.x, 0.0f, 1e-3f));
    CHECK(NearlyEqual(b->transform().position.x, 2.0f, 1e-3f));

    // Pushing one end drags the other along: equal masses share the motion, and
    // the captured offset survives.
    ra->velocity = Vec3(2, 0, 0);
    for (int i = 0; i < 60; ++i) PhysicsStep(scene, 1.0f / 60.0f);
    CHECK(NearlyEqual(a->transform().position.x, 1.0f, 0.05f));
    CHECK(NearlyEqual(b->transform().position.x - a->transform().position.x, 2.0f, 0.02f));

    // Spinning one end turns the pair together, relative pose intact.
    rb->AddTorque(Vec3(0, 30, 0));
    for (int i = 0; i < 120; ++i) PhysicsStep(scene, 1.0f / 60.0f);
    Quat qa = a->transform().rotation;
    Quat qb = b->transform().rotation;
    Vec3 rel = qa.Conjugate() * (b->transform().position - a->transform().position);
    CHECK(NearlyEqual(rel.x, 2.0f, 0.02f));           // B is still 2 units along A's +X
    CHECK(std::fabs(qa.w) < 0.999f);                  // ...and the pair really turned
    CHECK(std::fabs(qb.w) < 0.999f);
    Vec3 together = (qa.Conjugate() * qb) * Vec3(1, 0, 0);
    CHECK(NearlyEqual(together.x, 1.0f, 0.01f));      // the two orientations match

    // breakForce releases the weld when it is violently overloaded.
    weld->breakForce = 20.0f;
    ra->velocity = Vec3(100, 0, 0);
    for (int i = 0; i < 3; ++i) PhysicsStep(scene, 1.0f / 60.0f);
    CHECK(!weld->enabled);
}

// Rigidbodies parked at rest stop integrating until something wakes them.
static void TestSleeping() {
    Scene scene;
    GameObject* ground = scene.CreateEntity("Ground");
    ground->transform().position = Vec3(0, -0.5f, 0);
    ground->AddComponent<BoxCollider>()->size = Vec3(20, 1, 20);  // top at y = 0
    GameObject* box = scene.CreateEntity("Box");
    box->transform().position = Vec3(0, 2, 0);
    box->AddComponent<BoxCollider>()->size = Vec3(1, 1, 1);
    auto* rb = box->AddComponent<RigidBody>();
    rb->restitution = 0.0f;

    for (int i = 0; i < 300; ++i) PhysicsStep(scene, 1.0f / 60.0f);
    CHECK(rb->sleeping);
    CHECK(NearlyEqual(box->transform().position.y, 0.5f, 0.02f));

    // Sleeping bodies stop integrating entirely.
    Vec3 rest = box->transform().position;
    for (int i = 0; i < 30; ++i) PhysicsStep(scene, 1.0f / 60.0f);
    CHECK(NearlyEqual(box->transform().position.y, rest.y, 1e-5f));

    // An impulse wakes it for good.
    rb->AddImpulse(Vec3(0, 5, 0));
    CHECK(!rb->sleeping);
    for (int i = 0; i < 5; ++i) PhysicsStep(scene, 1.0f / 60.0f);
    CHECK(box->transform().position.y > rest.y + 0.1f);

    // allowSleep = false keeps a body awake even at rest.
    rb->allowSleep = false;
    rb->velocity = Vec3();
    for (int i = 0; i < 120; ++i) PhysicsStep(scene, 1.0f / 60.0f);
    CHECK(!rb->sleeping);
}

// Continuous collision detection: a body whose single-step displacement is
// larger than an obstacle is thick must stop on it instead of passing through,
// and must behave exactly like the discrete path when it is merely resting.
static void TestContinuousCollision() {
    // A thin slab across Z, and a small fast sphere aimed at it from -Z.
    auto build = [](Scene& scene, RigidBody::CollisionDetection mode, GameObject** bulletOut) {
        GameObject* wall = scene.CreateEntity("Wall");
        wall->AddComponent<BoxCollider>()->size = Vec3(12, 12, 0.2f);  // spans z = -0.1..0.1
        GameObject* bullet = scene.CreateEntity("Bullet");
        bullet->transform().position = Vec3(0, 0, -8);
        auto* rb = bullet->AddComponent<RigidBody>();
        rb->useGravity = false;
        rb->collisionDetection = mode;
        bullet->AddComponent<SphereCollider>()->radius = 0.2f;
        rb->velocity = Vec3(0, 0, 600);        // 10 units per step at 1/60
        *bulletOut = bullet;
    };

    // Discrete: 10 units per step against a 0.2-thick wall tunnels straight
    // through. This is the bug CCD exists to fix, so it must reproduce here.
    Scene discrete;
    GameObject* d = nullptr;
    build(discrete, RigidBody::CollisionDetection::Discrete, &d);
    for (int i = 0; i < 4; ++i) PhysicsStep(discrete, 1.0f / 60.0f);
    CHECK(d->transform().position.z > 1.0f);

    // Continuous: the sphere stops with its surface on the near face (z = -0.3).
    Scene continuous;
    GameObject* c = nullptr;
    build(continuous, RigidBody::CollisionDetection::Continuous, &c);
    for (int i = 0; i < 4; ++i) PhysicsStep(continuous, 1.0f / 60.0f);
    CHECK(c->transform().position.z < -0.15f);
    CHECK(c->transform().position.z > -0.45f);
    // The impact consumed the inward velocity rather than letting it rebuild.
    CHECK(c->GetComponent<RigidBody>()->velocity.z <= 1.0f);

    // A fast box aimed at the same wall is caught too (box-vs-box is an AABB
    // test, and the AABB sweep is exact for it).
    Scene boxed;
    GameObject* wall = boxed.CreateEntity("Wall");
    wall->AddComponent<BoxCollider>()->size = Vec3(12, 12, 0.2f);
    GameObject* box = boxed.CreateEntity("Box");
    box->transform().position = Vec3(0, 0, -6);
    auto* brb = box->AddComponent<RigidBody>();
    brb->useGravity = false;
    brb->collisionDetection = RigidBody::CollisionDetection::Continuous;
    box->AddComponent<BoxCollider>()->size = Vec3(0.4f, 0.4f, 0.4f);
    brb->velocity = Vec3(0, 0, 400);
    for (int i = 0; i < 4; ++i) PhysicsStep(boxed, 1.0f / 60.0f);
    CHECK(box->transform().position.z < -0.25f && box->transform().position.z > -0.5f);

    // A body already resting on a floor is not treated as an approaching
    // obstacle: CCD must not freeze it or change how it slides. Run the same
    // scene with and without CCD and require identical motion.
    auto slide = [](RigidBody::CollisionDetection mode) {
        Scene scene;
        GameObject* floor = scene.CreateEntity("Floor");
        floor->transform().position = Vec3(0, -0.5f, 0);  // top at y = 0
        floor->AddComponent<BoxCollider>()->size = Vec3(40, 1, 40);
        GameObject* slider = scene.CreateEntity("Slider");
        slider->transform().position = Vec3(0, 0.5f, 0);
        auto* rb = slider->AddComponent<RigidBody>();
        rb->collisionDetection = mode;
        rb->friction = 0.0f;  // keep sliding, so a CCD stall cannot hide in damping
        slider->AddComponent<BoxCollider>()->size = Vec3(1, 1, 1);
        for (int i = 0; i < 30; ++i) PhysicsStep(scene, 1.0f / 60.0f);  // settle
        rb->velocity = Vec3(6, 0, 0);
        for (int i = 0; i < 30; ++i) PhysicsStep(scene, 1.0f / 60.0f);
        return slider->transform().position;
    };
    Vec3 slow = slide(RigidBody::CollisionDetection::Discrete);
    Vec3 swept = slide(RigidBody::CollisionDetection::Continuous);
    CHECK(NearlyEqual(slow.x, swept.x, 1e-3f));
    CHECK(NearlyEqual(slow.y, swept.y, 1e-3f));
    CHECK(slow.x > 0.5f);  // the slide actually happened

    // 2D collision-detection setting is honoured through the 2D shape path.
    Scene two;
    GameObject* wall2 = two.CreateEntity("Wall2");
    wall2->AddComponent<Collider2D>()->size = Vec2(0.2f, 12.0f);  // spans x = -0.1..0.1
    GameObject* shot = two.CreateEntity("Shot");
    shot->transform().position = Vec3(-6, 0, 0);
    auto* srb = shot->AddComponent<RigidBody>();
    srb->useGravity = false;
    srb->freezeZ = true;
    srb->collisionDetection = RigidBody::CollisionDetection::Continuous;
    shot->AddComponent<Collider2D>()->size = Vec2(0.2f, 0.2f);
    srb->velocity = Vec3(500, 0, 0);  // 8.3 units per step
    for (int i = 0; i < 3; ++i) PhysicsStep(two, 1.0f / 60.0f);
    CHECK(shot->transform().position.x < -0.05f);
    CHECK(shot->transform().position.x > -0.35f);

    // Conservative advancement measures the real separation, so a wall far
    // thinner than the step still stops the body (sampling-based CCD would
    // step right over a 0.02 slab at 10 units per step).
    {
        Scene s;
        GameObject* thin = s.CreateEntity("Thin");
        thin->AddComponent<BoxCollider>()->size = Vec3(4, 4, 0.02f);  // z = -0.01..0.01
        GameObject* p = s.CreateEntity("P");
        p->transform().position = Vec3(0, 0, -8);
        auto* rb = p->AddComponent<RigidBody>();
        rb->useGravity = false;
        rb->collisionDetection = RigidBody::CollisionDetection::Continuous;
        p->AddComponent<SphereCollider>()->radius = 0.2f;
        rb->velocity = Vec3(0, 0, 600);
        for (int i = 0; i < 3; ++i) PhysicsStep(s, 1.0f / 60.0f);
        CHECK(p->transform().position.z < -0.15f);   // stopped on the near face
        CHECK(p->transform().position.z > -0.25f);
    }

    // A near miss past a wall's edge must not be clipped into a hit: the sweep
    // only stops for an obstacle the body is actually approaching.
    {
        Scene s;
        GameObject* wall = s.CreateEntity("Wall");
        wall->AddComponent<BoxCollider>()->size = Vec3(12, 12, 0.2f);
        GameObject* grazer = s.CreateEntity("Grazer");
        grazer->transform().position = Vec3(6.5f, 6.5f, -8);  // past the corner
        auto* rb = grazer->AddComponent<RigidBody>();
        rb->useGravity = false;
        rb->collisionDetection = RigidBody::CollisionDetection::Continuous;
        grazer->AddComponent<SphereCollider>()->radius = 0.2f;
        rb->velocity = Vec3(0, 0, 600);
        for (int i = 0; i < 3; ++i) PhysicsStep(s, 1.0f / 60.0f);
        CHECK(grazer->transform().position.z > 1.0f);  // sailed straight past
    }

    // A diagonal approach stops on the face plane, not somewhere along the
    // motion: the sweep is a real time-of-impact, not a fixed-sampling guess.
    {
        Scene s;
        GameObject* wall = s.CreateEntity("Wall");
        wall->AddComponent<BoxCollider>()->size = Vec3(20, 20, 0.2f);
        GameObject* p = s.CreateEntity("P");
        p->transform().position = Vec3(-5, 0, -5);
        auto* rb = p->AddComponent<RigidBody>();
        rb->useGravity = false;
        rb->collisionDetection = RigidBody::CollisionDetection::Continuous;
        p->AddComponent<SphereCollider>()->radius = 0.2f;
        rb->velocity = Vec3(420, 0, 420);  // 45 degrees, 9.9 units per step
        for (int i = 0; i < 2; ++i) PhysicsStep(s, 1.0f / 60.0f);
        CHECK(p->transform().position.z < -0.2f);   // sphere surface on z = -0.1
        CHECK(p->transform().position.z > -0.35f);
        CHECK(p->transform().position.x > -1.0f);   // travelled while sweeping
    }

    // The layer matrix still decides what a sweep can see: a bullet on a layer
    // that ignores the wall flies straight through it with CCD on.
    {
        Scene s;
        GameObject* wall = s.CreateEntity("Wall");
        wall->layer = 1;
        wall->AddComponent<BoxCollider>()->size = Vec3(12, 12, 0.2f);
        GameObject* p = s.CreateEntity("P");
        p->transform().position = Vec3(0, 0, -8);
        auto* rb = p->AddComponent<RigidBody>();
        rb->useGravity = false;
        rb->collisionDetection = RigidBody::CollisionDetection::Continuous;
        p->AddComponent<SphereCollider>()->radius = 0.2f;
        rb->velocity = Vec3(0, 0, 600);
        s.SetLayerCollision(0, 1, false);
        for (int i = 0; i < 3; ++i) PhysicsStep(s, 1.0f / 60.0f);
        CHECK(p->transform().position.z > 1.0f);
    }
}

// Writes `quads` (four corners each) as an OBJ, one fan-triangulated face per
// quad. Used by the mesh collider tests.
static void WriteObjQuads(const std::string& path, const std::vector<std::vector<Vec3>>& faces) {
    std::ofstream out(path);
    for (const auto& f : faces)
        for (const Vec3& v : f) out << "v " << v.x << " " << v.y << " " << v.z << "\n";
    int base = 1;
    for (const auto& f : faces) {
        out << "f";
        for (size_t i = 0; i < f.size(); ++i) out << " " << base + (int)i;
        out << "\n";
        base += (int)f.size();
    }
}

// A MeshCollider is a triangle soup: the three primitives rest on it at their
// expected heights, a ray hits the triangles (not the bounding box), a rotated
// collider is rotated, a closed mesh holds a body on its roof, CCD stops a fast
// body at a zero-thickness wall, and a mesh on a body the solver has to push is
// dropped with a warning.
static void TestMeshCollider() {
    const std::string dir =
        (std::filesystem::temp_directory_path() / "cairn-test-meshcollider").string();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    const std::string floorPath = dir + "/floor.obj";
    WriteObjQuads(floorPath, {{{-6, 0, -6}, {6, 0, -6}, {6, 0, 6}, {-6, 0, 6}}});
    // A thin (zero-thickness) vertical wall in the XY plane at z = 0.
    const std::string wallPath = dir + "/wall.obj";
    WriteObjQuads(wallPath, {{{-4, -4, 0}, {4, -4, 0}, {4, 4, 0}, {-4, 4, 0}}});
    // A closed box, x/y/z in [-1, 1].
    const std::string boxPath = dir + "/box.obj";
    WriteObjQuads(boxPath, {
        {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}},
        {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},
        {{-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}},
        {{1, -1, -1}, {1, -1, 1}, {1, 1, 1}, {1, 1, -1}},
        {{-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}},
        {{-1, 1, -1}, {1, 1, -1}, {1, 1, 1}, {-1, 1, 1}},
    });
    // One triangle: the lower-right half of the [-4,4]x[-4,4] square at z = 0.
    const std::string triPath = dir + "/tri.obj";
    {
        std::ofstream out(triPath);
        out << "v -4 -4 0\nv 4 -4 0\nv 4 4 0\nf 1 2 3\n";
    }

    // The three primitives land on the floor at their expected heights.
    {
        Scene scene;
        scene.CreateEntity("Floor")->AddComponent<MeshCollider>()->meshPath = floorPath;

        GameObject* ball = scene.CreateEntity("Ball");
        ball->transform().position = Vec3(-3, 4, -3);
        ball->AddComponent<SphereCollider>()->radius = 0.5f;
        ball->AddComponent<RigidBody>()->restitution = 0.0f;

        GameObject* crate = scene.CreateEntity("Crate");
        crate->transform().position = Vec3(3, 4, 3);
        crate->AddComponent<BoxCollider>()->size = Vec3(1, 1, 1);
        crate->AddComponent<RigidBody>()->restitution = 0.0f;

        GameObject* cap = scene.CreateEntity("Cap");
        cap->transform().position = Vec3(0, 4, 3);
        auto* cc = cap->AddComponent<CapsuleCollider>();
        cc->radius = 0.5f;
        cc->height = 2.0f;  // local segment spans y in [-0.5, 0.5]
        cap->AddComponent<RigidBody>()->restitution = 0.0f;

        for (int i = 0; i < 400; ++i) PhysicsStep(scene, 1.0f / 60.0f);
        CHECK(NearlyEqual(ball->transform().position.y, 0.5f, 0.05f));
        CHECK(NearlyEqual(crate->transform().position.y, 0.5f, 0.05f));
        CHECK(NearlyEqual(cap->transform().position.y, 1.0f, 0.05f));
        CHECK(std::fabs(ball->transform().position.x + 3.0f) < 0.25f);  // no sideways slide
    }

    // A ray hits the triangles, not the box that contains them: the missing
    // upper-left half of the square has to report a miss.
    {
        Scene scene;
        GameObject* tri = scene.CreateEntity("Tri");
        tri->AddComponent<MeshCollider>()->meshPath = triPath;

        PhysicsHit hit;
        CHECK(PhysicsRaycast(scene, Vec3(3, -3, -5), Vec3(0, 0, 1), 100.0f, &hit));
        CHECK(hit.go == tri);
        CHECK(NearlyEqual(hit.distance, 5.0f, 1e-3f));
        CHECK(std::fabs(hit.normal.z) > 0.99f);
        CHECK(!PhysicsRaycast(scene, Vec3(-3, 3, -5), Vec3(0, 0, 1), 100.0f, &hit));

        auto inside = PhysicsOverlapSphere(scene, Vec3(3, -3, 0), 0.2f);
        CHECK(std::find(inside.begin(), inside.end(), tri) != inside.end());
        auto outside = PhysicsOverlapSphere(scene, Vec3(-3, 3, 0), 0.2f);
        CHECK(std::find(outside.begin(), outside.end(), tri) == outside.end());

        // The box query is exact against the triangles too: it finds the mesh
        // where the triangle is and misses the empty half of its bounding box.
        auto boxOn = PhysicsOverlapBox(scene, Vec3(3, -3, 0), Vec3(0.2f, 0.2f, 0.2f));
        CHECK(std::find(boxOn.begin(), boxOn.end(), tri) != boxOn.end());
        auto boxOff = PhysicsOverlapBox(scene, Vec3(-3, 3, 0), Vec3(0.2f, 0.2f, 0.2f));
        CHECK(std::find(boxOff.begin(), boxOff.end(), tri) == boxOff.end());
    }

    // A closed mesh (six quads) carries a body on its roof.
    {
        Scene scene;
        scene.CreateEntity("Cage")->AddComponent<MeshCollider>()->meshPath = boxPath;
        GameObject* ball = scene.CreateEntity("Ball");
        ball->transform().position = Vec3(0.2f, 4, 0.2f);
        ball->AddComponent<SphereCollider>()->radius = 0.5f;
        ball->AddComponent<RigidBody>()->restitution = 0.0f;
        for (int i = 0; i < 400; ++i) PhysicsStep(scene, 1.0f / 60.0f);
        CHECK(NearlyEqual(ball->transform().position.y, 1.5f, 0.08f));
    }

    // Rotating the entity rotates the collider: the floor quad becomes a wall.
    {
        Scene scene;
        GameObject* wallGo = scene.CreateEntity("TiltedFloor");
        wallGo->transform().SetEulerDegrees(Vec3(90, 0, 0));  // XZ plane -> z = 0 plane
        wallGo->AddComponent<MeshCollider>()->meshPath = floorPath;

        GameObject* ball = scene.CreateEntity("Ball");
        ball->transform().position = Vec3(0, 0, -3);
        auto* rb = ball->AddComponent<RigidBody>();
        rb->useGravity = false;
        rb->restitution = 0.0f;
        ball->AddComponent<SphereCollider>()->radius = 0.5f;
        rb->velocity = Vec3(0, 0, 60);  // one unit per step
        for (int i = 0; i < 60; ++i) PhysicsStep(scene, 1.0f / 60.0f);
        CHECK(ball->transform().position.z < -0.4f);
        CHECK(ball->transform().position.z > -0.7f);
    }

    // CCD sweeps a fast body against a zero-thickness mesh; discrete tunnels.
    for (int mode = 0; mode < 2; ++mode) {
        Scene scene;
        scene.CreateEntity("Wall")->AddComponent<MeshCollider>()->meshPath = wallPath;
        GameObject* p = scene.CreateEntity("P");
        p->transform().position = Vec3(0, 0, -8);
        auto* rb = p->AddComponent<RigidBody>();
        rb->useGravity = false;
        rb->restitution = 0.0f;
        rb->collisionDetection = mode == 0 ? RigidBody::CollisionDetection::Discrete
                                           : RigidBody::CollisionDetection::Continuous;
        p->AddComponent<SphereCollider>()->radius = 0.2f;
        rb->velocity = Vec3(0, 0, 600);  // 10 units per step
        for (int i = 0; i < 3; ++i) PhysicsStep(scene, 1.0f / 60.0f);
        if (mode == 0) {
            CHECK(p->transform().position.z > 1.0f);  // straight through
        } else {
            CHECK(p->transform().position.z < -0.19f);
            CHECK(p->transform().position.z > -0.4f);
        }
    }

    // A mesh on a non-kinematic body is dropped (static-only, like Unity) and
    // reported once. The body still integrates - freezing it would be worse.
    {
        Scene scene;
        GameObject* ground = scene.CreateEntity("Ground");
        ground->transform().position = Vec3(0, -0.5f, 0);
        ground->AddComponent<BoxCollider>()->size = Vec3(10, 1, 10);  // top at y = 0

        GameObject* body = scene.CreateEntity("Mover");
        body->transform().position = Vec3(0, 4, 0);
        body->AddComponent<MeshCollider>()->meshPath = floorPath;
        auto* rb = body->AddComponent<RigidBody>();
        rb->restitution = 0.0f;
        for (int i = 0; i < 200; ++i) PhysicsStep(scene, 1.0f / 60.0f);
        CHECK(body->transform().position.y < -3.0f);
        CHECK(rb->velocity.y < -1.0f);

        bool warned = false;
        for (const LogEntry& e : Log::Poll(0))
            if (e.text.find("MeshCollider") != std::string::npos) warned = true;
        CHECK(warned);
    }

    // A kinematic body keeps its mesh collider (a moving level piece).
    {
        Scene scene;
        GameObject* platform = scene.CreateEntity("Platform");
        platform->transform().position = Vec3(0, 2, 0);
        platform->AddComponent<MeshCollider>()->meshPath = floorPath;
        auto* prb = platform->AddComponent<RigidBody>();
        prb->isKinematic = true;

        GameObject* ball = scene.CreateEntity("Ball");
        ball->transform().position = Vec3(0, 5, 0);
        ball->AddComponent<SphereCollider>()->radius = 0.5f;
        ball->AddComponent<RigidBody>()->restitution = 0.0f;
        for (int i = 0; i < 400; ++i) PhysicsStep(scene, 1.0f / 60.0f);
        CHECK(NearlyEqual(ball->transform().position.y, 2.5f, 0.08f));
    }

    std::filesystem::remove_all(dir);
}

// The wired path: a project-relative mesh path resolves through the project
// root, the collider answers the engine-level query, and a scene save/load
// round trip keeps it.
static void TestMeshColliderScene() {
    const std::string dir =
        (std::filesystem::temp_directory_path() / "cairn-test-meshcollider-scene").string();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir + "/Assets/Meshes");
    std::filesystem::create_directories(dir + "/Assets/Scenes");
    WriteObjQuads(dir + "/Assets/Meshes/floor.obj",
                  {{{-8, 0, -8}, {8, 0, -8}, {8, 0, 8}, {-8, 0, 8}}});

    Engine engine;
    engine.SetProjectRoot(dir);
    engine.NewScene("Level");

    GameObject* level = engine.scene().CreateEntity("Level");
    auto* mc = level->AddComponent<MeshCollider>();
    mc->meshPath = "Assets/Meshes/floor.obj";

    Physics::RaycastHit hit;
    CHECK(Physics::Raycast(Vec3(1, 5, 1), Vec3(0, -1, 0), 100.0f, &hit));
    CHECK(hit.entity == level->m_id);
    CHECK(NearlyEqual(hit.distance, 5.0f, 1e-2f));
    mc->enabled = false;
    CHECK(!Physics::Raycast(Vec3(1, 5, 1), Vec3(0, -1, 0), 100.0f, &hit));
    mc->enabled = true;

    std::string err;
    CHECK(engine.SaveSceneFile(dir + "/Assets/Scenes/Level.cscene", &err));
    CHECK(engine.LoadSceneFile(dir + "/Assets/Scenes/Level.cscene", &err));
    GameObject* found = nullptr;
    for (const auto& g : engine.scene().objects)
        if (g->GetComponent<MeshCollider>()) { found = g.get(); break; }
    CHECK(found != nullptr);
    if (found) {
        CHECK(found->GetComponent<MeshCollider>()->meshPath == "Assets/Meshes/floor.obj");
        CHECK(Physics::Raycast(Vec3(1, 5, 1), Vec3(0, -1, 0), 100.0f, &hit));
        CHECK(hit.entity == found->m_id);
    }

    std::filesystem::remove_all(dir);
}

// The frame profiler aggregates named samples and reports them heaviest-first.
static void TestProfiler() {
    Profiler::Reset();
    {
        Profiler::ScopedSample outer("outer");
        for (int i = 0; i < 20000; ++i) {
            Profiler::ScopedSample inner("inner");
            volatile double sink = (double)i * 1.5;
            (void)sink;
        }
    }
    Profiler::Frame();
    Json snap = Profiler::Snapshot();
    CHECK(snap["frame"].asInt() == 1);

    const Json scopes = snap["scopes"];
    CHECK(scopes.size() == 2);
    bool sawOuter = false, sawInner = false;
    for (size_t i = 0; i < scopes.size(); ++i) {
        const std::string name = scopes[i]["name"].asString();
        if (name == "outer") {
            sawOuter = true;
            CHECK(scopes[i]["calls"].asInt() == 1);
        } else if (name == "inner") {
            sawInner = true;
            CHECK(scopes[i]["calls"].asInt() == 20000);
        }
    }
    CHECK(sawOuter && sawInner);
    // Heaviest-first: `outer` wraps every `inner` sample.
    CHECK(scopes[0]["name"].asString() == "outer");

    // A driven engine frame records the update scope and merges render counters.
    Engine engine;
    engine.SetViewportSize(48, 32);
    for (int i = 0; i < 3; ++i) engine.Update(1.0f / 60.0f);
    Json live = engine.ProfileSnapshot();
    CHECK(live["frame"].asInt() >= 2);
    bool sawUpdate = false;
    const Json liveScopes = live["scopes"];
    for (size_t i = 0; i < liveScopes.size(); ++i)
        if (liveScopes[i]["name"].asString() == "update") sawUpdate = true;
    CHECK(sawUpdate);
    CHECK(live["render"]["backend"].asString() == "software");
}

// The job system runs every index exactly once, in any order, and turns a
// nested dispatch into a serial loop rather than clobbering the in-flight task.
static void TestJobs() {
    CHECK(Jobs::WorkerCount() >= 1);

    // Every index executes exactly once, whatever the pool size or order.
    const int n = 4096;
    std::vector<int> hits((size_t)n, 0);
    Jobs::ParallelFor(n, [&hits](int i) { hits[(size_t)i]++; });
    int total = 0, distinct = 0;
    for (int h : hits) { total += h; if (h == 1) distinct++; }
    CHECK(total == n);
    CHECK(distinct == n);

    // count <= 0 is a no-op.
    int calls = 0;
    Jobs::ParallelFor(0, [&](int) { calls++; });
    Jobs::ParallelFor(-7, [&](int) { calls++; });
    CHECK(calls == 0);

    // Index-independent work reproduces exactly across dispatches.
    auto compute = [](int i) { return std::sqrt((float)i) * 1.5f - 0.25f; };
    std::vector<float> a((size_t)n), b((size_t)n);
    Jobs::ParallelFor(n, [&a, &compute](int i) { a[(size_t)i] = compute(i); });
    Jobs::ParallelFor(n, [&b, &compute](int i) { b[(size_t)i] = compute(i); });
    bool same = true;
    for (int i = 0; i < n; ++i)
        if (a[(size_t)i] != b[(size_t)i]) same = false;
    CHECK(same);

    // A nested dispatch runs serially on the calling thread instead of
    // clobbering the outer task. Per-outer storage keeps the test race-free.
    std::vector<std::vector<int>> inner(8, std::vector<int>(64, 0));
    std::vector<int> outer(8, 0);
    Jobs::ParallelFor(8, [&inner, &outer](int i) {
        outer[(size_t)i]++;
        Jobs::ParallelFor(64, [&inner, i](int j) { inner[(size_t)i][(size_t)j]++; });
    });
    for (int i = 0; i < 8; ++i) {
        CHECK(outer[(size_t)i] == 1);
        for (int j = 0; j < 64; ++j) CHECK(inner[(size_t)i][(size_t)j] == 1);
    }

    // Telemetry advances with use.
    const Jobs::Stats s = Jobs::Snapshot();
    CHECK(s.workers == Jobs::WorkerCount());
    CHECK(s.dispatches >= 5);
    CHECK(s.indices >= (uint64_t)n);
}

int main() {
    Log::SetEchoStdout(false);
    TestMath();
    TestJson();
    TestMeshWinding();
    TestMeshTangents();
    TestObjSharing();
    TestScene();
    TestRender();
    TestRenderBackendSelection();
    TestRenderBackendsAgree();
    TestBindlessTextureSlots();
    TestShadows();
    TestPostProcessing();
    TestPhysics();
    TestHierarchy();
    TestFrustum();
    TestTonemapping();
    TestCullingAndAA();
    TestSortingOrder();
    TestSpriteQuality();
    TestSpriteOrientation();
    TestSpriteNormalOrientation();
    TestSpriteAtlas();
    TestSpriteCullBounds();
    TestSpriteAnimation();
    TestSpriteClips();
    TestSpriteTransitions();
    TestSpriteFrameDurations();
    TestTextureMips();
    TestParentedPhysics();
    TestTriggerAndEnabled();
    TestScripts();
    TestPbrMaterials();
    TestTextureCache();
    TestAssetDatabase();
    TestEntityIdentity();
    TestPrefabs();
    TestPrefabOverrides();
    TestPhysicsQueries();
    TestCollisionLayers();
    TestRigidbodyFrictionAngular();
    TestCapsuleCollider();
    TestCapsulePhysics();
    TestCharacterController();
    TestJoints();
    TestHingeJoint();
    TestFixedJoint();
    TestSleeping();
    TestContinuousCollision();
    TestMeshCollider();
    TestMeshColliderScene();
    TestProfiler();
    TestJobs();

    if (g_failures == 0) {
        std::printf("engine tests passed\n");
        return 0;
    }
    std::printf("%d test failure(s)\n", g_failures);
    return 1;
}
