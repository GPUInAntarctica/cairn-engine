// Cairn Engine - native smoke/unit tests (headless, no GUI).
#include "cairn/cairn.h"
#include "cairn/engine.h"
#include "cairn/physics.h"
#include "scene_internal.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

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

// Local stand-in exercising the DescribeProps contract scripts implement.
class ScriptProbeComponent : public Component {
public:
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

int main() {
    Log::SetEchoStdout(false);
    TestMath();
    TestJson();
    TestScene();
    TestRender();
    TestPhysics();
    TestScripts();
    if (g_failures == 0) {
        std::printf("engine tests passed\n");
        return 0;
    }
    std::printf("%d test failure(s)\n", g_failures);
    return 1;
}
