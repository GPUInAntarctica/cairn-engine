#include "cairn/engine.h"

#include "cairn/physics.h"
#include "cairn/jobs.h"
#include "cairn/profiler.h"
#include "scene_internal.h"
#include "script_host.h"
#include "shader_library.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <string>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

namespace Cairn {

// ---------------------------------------------------------------------------
// Global engine pointer (single engine per process, owned by the host).
// ---------------------------------------------------------------------------
static Engine* g_engine = nullptr;

Scene* EngineActiveScene() { return g_engine ? &g_engine->scene() : nullptr; }
MeshLibrary& EngineMeshLibrary() {
    static MeshLibrary lib;
    return lib;
}
TextureLibrary& EngineTextureLibrary() {
    static TextureLibrary lib;
    return lib;
}
ShaderLibrary& EngineShaderLibrary() {
    static ShaderLibrary lib;
    return lib;
}
std::string EngineResolvePath(const std::string& projectPath) {
    if (!g_engine) return projectPath;
    if (!projectPath.empty() && (projectPath[0] == '/' || projectPath[1] == ':')) return projectPath;
    if (projectPath.empty()) return g_engine->projectRoot();
    return g_engine->projectRoot() + "/" + projectPath;
}
AssetDatabase* EngineAssetDatabase() { return g_engine ? &g_engine->assets() : nullptr; }
void EngineQueueDebugLine(const Vec3& a, const Vec3& b, Color color) {
    if (g_engine) g_engine->debugLines_.push_back({a, b, color});
}
void EngineQueueAudio(const std::string& clip, float volume, float pitch) {
    if (g_engine) g_engine->audioEvents_.push_back({clip, volume, pitch});
}
InputState* EngineInputState() { return g_engine ? &g_engine->input_ : nullptr; }
float EngineTimeScale() { return g_engine ? g_engine->timeScale_ : 1.0f; }
void EngineSetTimeScale(float s) {
    if (g_engine) g_engine->timeScale_ = std::clamp(s, 0.0f, 100.0f);
}

// ---------------------------------------------------------------------------
// Scripting API implementations (bound to the active engine).
// ---------------------------------------------------------------------------
namespace Input {
static InputState* S() { return EngineInputState(); }
bool GetKey(KeyCode key) {
    InputState* s = S();
    return s && s->keyDown[(int)key & 511];
}
bool GetKeyDown(KeyCode key) {
    InputState* s = S();
    int i = (int)key & 511;
    return s && s->keyDown[i] && !s->keyWasDown[i];
}
bool GetKeyUp(KeyCode key) {
    InputState* s = S();
    int i = (int)key & 511;
    return s && !s->keyDown[i] && s->keyWasDown[i];
}
static float AxisValue(const InputState::Axis& axis, InputState* s) {
    float v = 0;
    if (s->keyDown[(uint32_t)axis.negative & 511]) v -= 1.0f;
    if (s->keyDown[(uint32_t)axis.positive & 511]) v += 1.0f;
    if (s->keyDown[(uint32_t)axis.altNegative & 511]) v -= 1.0f;
    if (s->keyDown[(uint32_t)axis.altPositive & 511]) v += 1.0f;
    return v;
}
float GetAxis(const std::string& axisName) {
    InputState* s = S();
    if (!s) return 0;
    for (const auto& axis : s->axes) {
        if (axis.name == axisName) return AxisValue(axis, s);
    }
    return 0;
}
Vec2 GetMousePosition() {
    InputState* s = S();
    return s ? Vec2(s->mouseX, s->mouseY) : Vec2();
}
bool GetMouseButton(int button) {
    InputState* s = S();
    return s && button >= 0 && button < 8 && s->mouseDown[button];
}
bool GetMouseButtonDown(int button) {
    InputState* s = S();
    return s && button >= 0 && button < 8 && s->mouseDown[button] && !s->mouseWasDown[button];
}
bool GetMouseButtonUp(int button) {
    InputState* s = S();
    return s && button >= 0 && button < 8 && !s->mouseDown[button] && s->mouseWasDown[button];
}
}  // namespace Input

static float g_lastDt = 0.0f;

namespace Time {
float DeltaTime() { return g_lastDt; }
float FixedDeltaTime() { return 1.0f / 60.0f; }
float Time() { return g_engine ? g_engine->playTime() : 0.0f; }
uint64_t FrameCount() { return g_engine ? g_engine->frameCount() : 0; }
float TimeScale() { return EngineTimeScale(); }
void SetTimeScale(float scale) { EngineSetTimeScale(scale); }
}  // namespace Time

namespace Audio {
void Play(const std::string& clipPath, float volume, float pitch) {
    EngineQueueAudio(clipPath, volume, pitch);
}
}  // namespace Audio

namespace Physics {
static void Fill(RaycastHit* out, const Cairn::PhysicsHit& h) {
    out->entity = h.go ? h.go->m_id : kInvalidEntity;
    out->guid = h.go ? h.go->m_guid : std::string();
    out->name = h.go ? h.go->name : std::string();
    out->point = h.point;
    out->normal = h.normal;
    out->distance = h.distance;
}
bool Raycast(const Vec3& origin, const Vec3& direction, float maxDistance, RaycastHit* hit) {
    Scene* s = EngineActiveScene();
    if (!s) return false;
    Cairn::PhysicsHit h;
    if (!Cairn::PhysicsRaycast(*s, origin, direction, maxDistance, &h)) return false;
    if (hit) Fill(hit, h);
    return true;
}
std::vector<RaycastHit> RaycastAll(const Vec3& origin, const Vec3& direction, float maxDistance) {
    std::vector<RaycastHit> out;
    Scene* s = EngineActiveScene();
    if (!s) return out;
    for (const Cairn::PhysicsHit& h : Cairn::PhysicsRaycastAll(*s, origin, direction, maxDistance)) {
        RaycastHit rh;
        Fill(&rh, h);
        out.push_back(rh);
    }
    return out;
}
std::vector<EntityId> OverlapSphere(const Vec3& center, float radius) {
    std::vector<EntityId> out;
    Scene* s = EngineActiveScene();
    if (!s) return out;
    for (GameObject* go : Cairn::PhysicsOverlapSphere(*s, center, radius))
        if (go) out.push_back(go->m_id);
    return out;
}
std::vector<EntityId> OverlapBox(const Vec3& center, const Vec3& halfExtents) {
    std::vector<EntityId> out;
    Scene* s = EngineActiveScene();
    if (!s) return out;
    for (GameObject* go : Cairn::PhysicsOverlapBox(*s, center, halfExtents))
        if (go) out.push_back(go->m_id);
    return out;
}
bool SphereCast(const Vec3& origin, float radius, const Vec3& direction, float maxDistance,
                RaycastHit* hit) {
    Scene* s = EngineActiveScene();
    if (!s) return false;
    Cairn::PhysicsHit h;
    if (!Cairn::PhysicsSphereCast(*s, origin, radius, direction, maxDistance, &h)) return false;
    if (hit) Fill(hit, h);
    return true;
}
bool BoxCast(const Vec3& center, const Vec3& halfExtents, const Vec3& direction, float maxDistance,
             RaycastHit* hit) {
    Scene* s = EngineActiveScene();
    if (!s) return false;
    Cairn::PhysicsHit h;
    if (!Cairn::PhysicsBoxCast(*s, center, halfExtents, direction, maxDistance, &h)) return false;
    if (hit) Fill(hit, h);
    return true;
}
}  // namespace Physics

namespace Debug {
void Log(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Cairn::Log::Info("%s", buf);
}
void LogWarning(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Cairn::Log::Warning("%s", buf);
}
void LogError(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Cairn::Log::Error("%s", buf);
}
void DrawLine(const Vec3& from, const Vec3& to, Color color, float duration) {
    (void)duration;  // one-frame lines; duration handled by the host later
    EngineQueueDebugLine(from, to, color);
}
}  // namespace Debug

// ---------------------------------------------------------------------------
// ViewportCamera.
// ---------------------------------------------------------------------------
Mat4 ViewportCamera::ViewMatrix() const {
    Vec3 dir = Quat::FromEulerDegrees(Vec3(pitch, yaw, 0)) * Vec3::Forward;
    return Mat4::LookAt(position, position + dir, Vec3::Up);
}

Mat4 ViewportCamera::ProjectionMatrix(float aspect) const {
    if (orthographic) {
        float hh = orthoSize, hw = orthoSize * aspect;
        return Mat4::Ortho(-hw, hw, -hh, hh, nearPlane, farPlane);
    }
    return Mat4::Perspective(Radians(fov), aspect, nearPlane, farPlane);
}

// ---------------------------------------------------------------------------
// Engine.
// ---------------------------------------------------------------------------
Engine::Engine() {
    g_engine = this;
    scene_ = std::make_shared<Scene>();
    SetViewportSize(960, 540);
}

Engine::~Engine() {
    StopScripts();
    if (g_engine == this) g_engine = nullptr;
}

void Engine::SetProjectRoot(const std::string& absolutePath) {
    projectRoot_ = absolutePath;
    assets_.SetProjectRoot(absolutePath);
    // Index the project up front so GUID references resolve during scene loads.
    assets_.Scan();
}

void Engine::RefreshAssets() { assets_.Scan(); }

void Engine::SetViewportSize(int w, int h) { renderer_.Resize(w, h); }

void Engine::NewScene(const std::string& name) {
    StopScripts();
    scene_ = std::make_shared<Scene>();
    scene_->name = name;
    PhysicsReset(scene_.get());
}

bool Engine::LoadSceneFile(const std::string& path, std::string* error) {
    // Pick up any assets added/renamed since the last scan so GUID references
    // resolve against the current project layout.
    assets_.Scan();
    auto loaded = Scene::LoadFromFile(path, error);
    if (!loaded) return false;
    StopScripts();
    scene_ = loaded;
    PhysicsReset(scene_.get());
    // Re-pull every prefab asset: an edited .prefab reaches the instances that
    // did not override the property, and each instance's own differences are
    // what the scene stored when it was last saved.
    SyncPrefabs(0, nullptr);
    ApplyScriptPropDefaults();
    return true;
}

// Fills in inspector fields that the loaded script classes declare but the
// serialized data predates, so a field added after authoring reads back with its
// declared default instead of 0.
void Engine::ApplyScriptPropDefaults() {
    if (scriptClasses_.empty()) return;
    for (const auto& go : scene_->objects) {
        for (Component* c : go->GetComponentList()) {
            if (c->TypeId() != ComponentTypeId::Script) continue;
            auto* sc = static_cast<ScriptComponent*>(c);
            if (sc->scriptClass.empty()) continue;
            for (const ScriptClassInfo& info : scriptClasses_) {
                if (info.name != sc->scriptClass) continue;
                if (!sc->properties.isObject()) sc->properties = Json::MakeObject();
                for (const ScriptClassInfo::Prop& p : info.props) {
                    if (!sc->properties.has(p.name)) sc->properties.set(p.name, p.defaultValue);
                }
                break;
            }
        }
    }
}

bool Engine::SaveSceneFile(const std::string& path, std::string* error) {
    assets_.Scan();  // ensure newly imported assets have a GUID to serialize
    // Record how each instance differs from its asset, so a later load can tell
    // an intentional per-instance tweak from a value the asset has since moved
    // on from.
    RefreshPrefabOverrides(0, nullptr);
    return scene_->SaveToFile(path, error);
}

std::string Engine::SerializeSceneJson() const { return scene_->Serialize().Dump(-1); }

bool Engine::RestoreSceneJson(const std::string& json, std::string* error) {
    Json doc;
    if (!Json::Parse(json, &doc, error)) return false;
    auto restored = Scene::Deserialize(doc, error);
    if (!restored) return false;
    StopScripts();
    scene_ = restored;
    PhysicsReset(scene_.get());
    ApplyScriptPropDefaults();
    return true;
}

EntityId Engine::CreateEntity(const std::string& name, EntityId parent) {
    GameObject* p = parent ? FindEntity(parent) : nullptr;
    GameObject* go = scene_->CreateEntity(name, p);
    return go ? go->m_id : kInvalidEntity;
}

EntityId Engine::CreatePrimitive(PrimitiveMesh mesh, const std::string& name, EntityId parent) {
    EntityId id = CreateEntity(name, parent);
    GameObject* go = FindEntity(id);
    if (!go) return kInvalidEntity;
    if (auto* mr = go->AddComponent<MeshRenderer>()) {
        mr->mesh = (MeshRenderer::MeshSource)(int)mesh;
    }
    return id;
}

EntityId Engine::CreateSpriteEntity(const std::string& name, EntityId parent) {
    EntityId id = CreateEntity(name, parent);
    GameObject* go = FindEntity(id);
    if (!go) return kInvalidEntity;
    go->AddComponent<SpriteRenderer>();
    return id;
}

void Engine::DestroyEntity(EntityId id) {
    GameObject* go = FindEntity(id);
    if (go) scene_->DestroyEntityNow(go);
}

EntityId Engine::DuplicateEntity(EntityId id) {
    GameObject* src = FindEntity(id);
    if (!src) return kInvalidEntity;
    GameObject* copy = scene_->InstantiateCopy(src, src->GetParent());
    if (copy) copy->name = src->name;  // identical copy, editor may rename
    return copy ? copy->m_id : kInvalidEntity;
}

GameObject* Engine::FindEntity(EntityId id) { return scene_->FindEntity(id); }

bool Engine::AddComponent(EntityId id, const std::string& typeName) {
    GameObject* go = FindEntity(id);
    if (!go) return false;
    if (go->GetComponentByName(typeName)) return false;
    return go->AddComponentByName(typeName);
}

bool Engine::RemoveComponent(EntityId id, const std::string& typeName) {
    GameObject* go = FindEntity(id);
    if (!go) return false;
    return go->RemoveComponentByName(typeName);
}

bool Engine::SetComponentProp(EntityId id, const std::string& typeName, const std::string& prop,
                              const Json& value) {
    GameObject* go = FindEntity(id);
    if (!go) return false;
    if (typeName == "Transform") {
        Transform& t = go->transform();
        if (prop == "position" && value.isArray() && value.size() >= 3) {
            t.position = {value[0].asFloat(), value[1].asFloat(), value[2].asFloat()};
            t.MarkWorldDirty();
            return true;
        }
        if (prop == "rotation" && value.isArray() && value.size() >= 3) {
            t.SetEulerDegrees({value[0].asFloat(), value[1].asFloat(), value[2].asFloat()});
            t.MarkWorldDirty();
            return true;
        }
        if (prop == "scale" && value.isArray() && value.size() >= 3) {
            t.scale = {value[0].asFloat(), value[1].asFloat(), value[2].asFloat()};
            t.MarkWorldDirty();
            return true;
        }
        return false;
    }
    Component* c = go->GetComponentByName(typeName);
    if (!c) return false;
    if (typeName == "Script") {
        auto* sc = static_cast<ScriptComponent*>(c);
        if (prop == "class") {
            sc->scriptClass = value.asString();
            // Fill inspector defaults for newly attached scripts.
            for (const ScriptClassInfo& info : ScriptClasses()) {
                if (info.name != sc->scriptClass) continue;
                for (const auto& p : info.props) {
                    if (!sc->properties.has(p.name)) sc->properties.set(p.name, p.defaultValue);
                }
                break;
            }
            return true;
        }
        if (prop == "enabled") {
            sc->enabled = value.asBool(true);
            return true;
        }
        sc->properties.set(prop, value);
        return true;
    }
    if (prop == "enabled") {
        c->enabled = value.asBool(true);
        return true;
    }
    return ComponentSetProperty(c, prop, value);
}

bool Engine::GetComponentProp(EntityId id, const std::string& typeName, const std::string& prop, Json* out) {
    GameObject* go = FindEntity(id);
    if (!go) return false;
    if (typeName == "Transform") {
        const Transform& t = go->transform();
        if (prop == "position") {
            *out = Json::MakeArray({t.position.x, t.position.y, t.position.z});
            return true;
        }
        if (prop == "rotation") {
            Vec3 e = t.GetEulerDegrees();
            *out = Json::MakeArray({e.x, e.y, e.z});
            return true;
        }
        if (prop == "scale") {
            *out = Json::MakeArray({t.scale.x, t.scale.y, t.scale.z});
            return true;
        }
        return false;
    }
    Component* c = go->GetComponentByName(typeName);
    if (!c) return false;
    if (typeName == "Script") {
        auto* sc = static_cast<ScriptComponent*>(c);
        if (prop == "class") { *out = sc->scriptClass; return true; }
        if (prop == "enabled") { *out = sc->enabled; return true; }
        const Json* v = sc->properties.find(prop);
        if (v) { *out = *v; return true; }
        return false;
    }
    if (prop == "enabled") { *out = c->enabled; return true; }
    return ComponentGetProperty(c, prop, out);
}

void Engine::ReloadAssets() {
    // Only drop textures whose file actually changed on disk; a blanket clear
    // would thrash the LRU cache for unchanged assets.
    assets_.Scan();
    EngineTextureLibrary().ReloadChanged();
    EngineMeshLibrary().ClearObj();
    EngineShaderLibrary().ReloadChanged();
}

void Engine::ReloadScriptLibraries() {
    if (scriptLibPath_.empty()) return;
    std::string err;
    LoadScriptLibrary(scriptLibPath_, &err);
}

bool Engine::LoadScriptLibrary(const std::string& absolutePath, std::string* error) {
    // Release everything owned by the previously loaded library BEFORE it is
    // unloaded: script instances hold code in it, and cached default values
    // hold shared_ptr control blocks whose deleters live in it.
    StopScripts();
    scriptClasses_.clear();
    if (!scriptHost_) scriptHost_.reset(new ScriptHost());
    if (!scriptHost_->Load(absolutePath, error)) {
        Log::Error("script library load failed: %s", error ? error->c_str() : "?");
        return false;
    }
    scriptLibPath_ = absolutePath;
    scriptClasses_.clear();
    for (const ScriptClassRecord& r : scriptHost_->Records()) {
        ScriptClassInfo info;
        info.name = r.name ? r.name : "";
        if (Component* probe = r.create()) {
            ScriptMeta meta;
            probe->DescribeProps(meta);
            for (const ScriptProperty& p : meta.properties) {
                info.props.push_back({p.name, (int)p.type, p.defaultValue});
            }
            delete probe;
        }
        scriptClasses_.push_back(std::move(info));
    }
    Log::Info("loaded %d script class(es) from %s", (int)scriptClasses_.size(), absolutePath.c_str());
    ApplyScriptPropDefaults();
    // A reload during play used to leave the game with dead scripts: re-enter
    // play mode's startup so the new classes are instantiated immediately.
    if (playing_) StartScripts();
    return true;
}

std::vector<ScriptClassInfo> Engine::ScriptClasses() const { return scriptClasses_; }

bool Engine::HasScriptClass(const std::string& name) const {
    for (const auto& c : scriptClasses_)
        if (c.name == name) return true;
    return false;
}

void Engine::StartScripts() {
    gameScriptsStarted_ = true;
    // Creation pass: instantiate every script class before OnStart so scripts
    // can look each other up during startup.
    for (const auto& go : scene_->objects) {
        if (!go->active) continue;
        for (Component* c : go->GetComponentList()) {
            if (c->TypeId() != ComponentTypeId::Script) continue;
            auto* sc = static_cast<ScriptComponent*>(c);
            if (sc->scriptClass.empty()) continue;
            Component* inst = scriptHost_ ? scriptHost_->Create(sc->scriptClass) : nullptr;
            if (!inst) {
                Log::Error("script class not found: %s (build scripts first)", sc->scriptClass.c_str());
                continue;
            }
            inst->m_go = go.get();
            inst->m_propSource = &sc->properties;
            sc->instance = inst;
        }
    }
    // OnStart pass after all instances exist.
    for (const auto& go : scene_->objects) {
        if (!go->active) continue;
        for (Component* c : go->GetComponentList()) {
            if (c->TypeId() != ComponentTypeId::Script) continue;
            auto* sc = static_cast<ScriptComponent*>(c);
            if (sc->instance && sc->enabled) sc->instance->OnStart();
        }
    }
}

void Engine::StopScripts() {
    if (!gameScriptsStarted_) return;
    for (const auto& go : scene_->objects) {
        for (Component* c : go->GetComponentList()) {
            if (c->TypeId() != ComponentTypeId::Script) continue;
            auto* sc = static_cast<ScriptComponent*>(c);
            sc->DestroyInstance();
        }
    }
    gameScriptsStarted_ = false;
}

bool Engine::Play() {
    if (playing_) return true;
    playSnapshot_ = SerializeSceneJson();
    playing_ = true;
    paused_ = false;
    accumulator_ = 0;
    time_ = 0;
    frameCount_ = 0;
    PhysicsReset(scene_.get());
    audioEvents_.clear();
    StartScripts();
    Log::Info("entered play mode");
    return true;
}

void Engine::Pause() {
    if (playing_) paused_ = true;
}

void Engine::Resume() {
    if (playing_) paused_ = false;
}

void Engine::Step() {
    if (!playing_) return;
    paused_ = false;
    Update(1.0f / 60.0f);
    EndFrameInput();
    paused_ = true;
}

void Engine::Stop() {
    if (!playing_) return;
    StopScripts();
    std::string err;
    RestoreSceneJson(playSnapshot_, &err);
    playing_ = false;
    paused_ = false;
    time_ = 0;
    PhysicsReset(scene_.get());
    audioEvents_.clear();
    Log::Info("stopped play mode");
}

void Engine::Update(float dt) {
    CAIRN_PROFILE("update");
    frameCount_++;
    g_lastDt = dt;
    float scaled = dt * timeScale_;
    if (playing_ && !paused_) {
        time_ += scaled;
        const float fixed = 1.0f / 60.0f;
        accumulator_ += scaled;
        int steps = 0;
        while (accumulator_ >= fixed && steps < 5) {
            {
                // FixedUpdate scripts.
                CAIRN_PROFILE("scripts");
                for (const auto& go : scene_->objects) {
                    if (!go->ActiveInHierarchy()) continue;
                    for (Component* c : go->GetComponentList()) {
                        if (c->TypeId() != ComponentTypeId::Script) continue;
                        auto* sc = static_cast<ScriptComponent*>(c);
                        if (sc->instance && sc->enabled) sc->instance->FixedUpdate(fixed);
                    }
                }
            }
            {
                CAIRN_PROFILE("physics");
                PhysicsStep(*scene_, fixed);
            }
            accumulator_ -= fixed;
            steps++;
        }
        {
            // Variable-rate update.
            CAIRN_PROFILE("scripts");
            for (const auto& go : scene_->objects) {
                if (!go->ActiveInHierarchy()) continue;
                for (Component* c : go->GetComponentList()) {
                    if (c->TypeId() != ComponentTypeId::Script) continue;
                    auto* sc = static_cast<ScriptComponent*>(c);
                    if (sc->instance && sc->enabled) sc->instance->OnUpdate(scaled);
                }
            }
        }
        {
            // Sprite animation advances on play time, after the scripts that
            // could have retimed or restarted it.
            CAIRN_PROFILE("animation");
            for (const auto& go : scene_->objects) {
                if (!go->ActiveInHierarchy()) continue;
                auto* anim = go->GetComponent<SpriteAnimator>();
                if (anim && anim->enabled) {
                    anim->AdvanceAnimation(scaled);
                    anim->EvaluateTransitions();
                }
            }
        }
        scene_->ProcessDeferred();
    }
    // Expire debug lines quickly (they render for the following frame).
    debugLines_.clear();
    Profiler::Frame();
}

void Engine::EndFrameInput() {
    std::memcpy(input_.keyWasDown, input_.keyDown, sizeof(input_.keyDown));
    std::memcpy(input_.mouseWasDown, input_.mouseDown, sizeof(input_.mouseDown));
}

// CAIRN_RENDER_BACKEND=auto|vulkan|software changes the machine-level default
// for the whole process — it only speaks for scenes left on Auto, so a project
// that names a backend still gets the one it asked for. The test suites set it
// to software: they assert CPU-exact pixel maths and render hundreds of frames,
// and every GPU frame blocks on a readback fence, which starves the desktop
// compositor on a busy machine.
static RenderBackend EnvRenderBackend(RenderBackend fromScene) {
    static const int override = [] {
        const char* v = std::getenv("CAIRN_RENDER_BACKEND");
        if (!v) return -1;
        std::string s(v);
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        if (s == "vulkan" || s == "gpu") return (int)RenderBackend::Vulkan;
        if (s == "software" || s == "cpu") return (int)RenderBackend::Software;
        return -1;
    }();
    if (override < 0 || fromScene != RenderBackend::Auto) return fromScene;
    return (RenderBackend)override;
}

bool Engine::BuildCameraView(CameraView* out, bool useSceneCamera) const {
    CameraComponent* cam = useSceneCamera ? scene_->MainCamera() : nullptr;
    float aspect = (float)renderer_.target().width / std::max(1, renderer_.target().height);
    if (cam && cam->m_go && cam->m_go->ActiveInHierarchy()) {
        Transform& t = cam->m_go->transform();
        out->position = t.WorldPosition();
        Quat rot = t.WorldRotation();
        Vec3 forward = rot * Vec3::Forward;
        out->view = Mat4::LookAt(out->position, out->position + forward, rot * Vec3::Up);
        out->orthographic = cam->orthographic;
        out->fov = cam->fov;
        out->nearPlane = cam->nearPlane;
        out->farPlane = cam->farPlane;
        out->orthoSize = cam->orthographicSize;
        out->clearColor = cam->clearColor;
        out->skyEnabled = cam->skyEnabled;
        out->skyTop = cam->skyTop;
        out->skyBottom = cam->skyBottom;
    } else {
        out->position = viewCam_.position;
        out->view = viewCam_.ViewMatrix();
        out->orthographic = viewCam_.orthographic;
        out->fov = viewCam_.fov;
        out->nearPlane = viewCam_.nearPlane;
        out->farPlane = viewCam_.farPlane;
        out->orthoSize = viewCam_.orthoSize;
        // The viewport camera follows the scene background so the setting is
        // actually visible outside play mode (it used to be ignored). The sky
        // gradient keeps the background's hue: darker overhead, hazy horizon.
        out->clearColor = scene_->background;
        out->skyEnabled = true;
        out->skyTop = scene_->background * 0.85f;
        out->skyBottom = Color::Lerp(scene_->background, Color(1, 1, 1, 1), 0.55f);
    }
    out->projection = out->orthographic
                          ? Mat4::Ortho(-out->orthoSize * aspect, out->orthoSize * aspect,
                                        -out->orthoSize, out->orthoSize, out->nearPlane, out->farPlane)
                          : Mat4::Perspective(Radians(out->fov), aspect, out->nearPlane, out->farPlane);
    out->fogEnabled = scene_->fogEnabled;
    out->fogColor = scene_->fogColor;
    out->fogDensity = scene_->fogDensity;
    out->ambient = scene_->ambient;
    out->tonemap = (Tonemap)std::clamp(scene_->tonemap, 0, 3);
    out->exposure = scene_->exposure;
    out->time = time_;
    out->shadowCascades = std::clamp(scene_->shadows, 0, 4);
    out->shadowResolution = std::clamp(scene_->shadowResolution, 256, 4096);
    out->shadowDistance = std::clamp(scene_->shadowDistance, 5.0f, 500.0f);
    // Post-processing, clamped to the documented effect ranges.
    out->postVignette = std::clamp(scene_->postVignette, 0.0f, 1.0f);
    out->postVignetteRadius = std::clamp(scene_->postVignetteRadius, 0.0f, 1.0f);
    out->postVignetteSoftness = std::clamp(scene_->postVignetteSoftness, 0.0f, 1.0f);
    out->postContrast = std::clamp(scene_->postContrast, 0.0f, 2.0f);
    out->postSaturation = std::clamp(scene_->postSaturation, 0.0f, 2.0f);
    out->postBloomStrength = std::clamp(scene_->postBloomStrength, 0.0f, 2.0f);
    out->postBloomThreshold = std::clamp(scene_->postBloomThreshold, 0.0f, 1.0f);
    out->postFxaa = scene_->postFxaa;
    out->textureMemoryMB = std::clamp(scene_->textureMemoryMB, 8, 4096);
    // Rasteriser selection and multisampling. Auto keeps the GPU when one is
    // usable and degrades to software otherwise, so the same scene file runs
    // everywhere; forcing software is what the CPU-exact tests use.
    out->backend = (RenderBackend)std::clamp(scene_->renderBackend, 0, 2);
    out->backend = EnvRenderBackend(out->backend);
    out->msaa = scene_->msaa == 1 ? 1 : scene_->msaa == 2 ? 2 : scene_->msaa == 8 ? 8 : 4;
    return cam != nullptr;
}

void Engine::BuildOverlayLines(std::vector<DebugLine>* out) {
    *out = debugLines_;
}

RenderStats Engine::RenderEditorFrame() {
    CameraView cam;
    BuildCameraView(&cam, playing_);
    std::vector<DebugLine> lines;
    BuildOverlayLines(&lines);

    // Editor grid on the XZ plane.
    const float extent = 40.0f;
    const Color gridMinor = Color::FromHex(0x33363e);
    const Color gridMajor = Color::FromHex(0x464a55);
    for (int i = -(int)extent; i <= (int)extent; ++i) {
        Color c = (i % 10 == 0) ? gridMajor : gridMinor;
        if (i == 0) continue;
        lines.push_back({Vec3((float)i, 0.001f, -extent), Vec3((float)i, 0.001f, extent), c});
        lines.push_back({Vec3(-extent, 0.001f, (float)i), Vec3(extent, 0.001f, (float)i), c});
    }
    lines.push_back({Vec3(-extent, 0.001f, 0), Vec3(extent, 0.001f, 0), Color::FromHex(0x7a4a52)});
    lines.push_back({Vec3(0, 0.001f, -extent), Vec3(0, 0.001f, extent), Color::FromHex(0x47607a)});

    // Selection outline.
    if (highlightEntity_) {
        if (GameObject* go = FindEntity(highlightEntity_)) {
            AABB box;
            bool has = false;
            if (auto* mr = go->GetComponent<MeshRenderer>(); mr && mr->enabled) {
                std::shared_ptr<Mesh> mesh;
                if (mr->mesh == MeshRenderer::MeshSource::Custom && !mr->meshPath.empty())
                    mesh = EngineMeshLibrary().GetObj(EngineResolvePath(mr->meshPath));
                else
                    mesh = EngineMeshLibrary().Get((PrimitiveMesh)(int)mr->mesh);
                if (mesh) {
                    box = mesh->Bounds().Transformed(go->transform().WorldMatrix());
                    has = true;
                }
            }
            if (!has && go->GetComponent<SpriteRenderer>()) {
                Vec3 s = go->transform().WorldScale();
                box = AABB(go->transform().WorldPosition() - s * 0.5f,
                           go->transform().WorldPosition() + s * 0.5f);
                has = true;
            }
            if (!has) {
                Vec3 p = go->transform().WorldPosition();
                box = AABB(p - Vec3(0.1f), p + Vec3(0.1f));
                has = true;
            }
            const Color sel(0.95f, 0.62f, 0.13f);
            Vec3 mn = box.min - Vec3(0.01f), mx = box.max + Vec3(0.01f);
            Vec3 c[8] = {
                {mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mn.y, mx.z}, {mn.x, mn.y, mx.z},
                {mn.x, mx.y, mn.z}, {mx.x, mx.y, mn.z}, {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z},
            };
            static const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                                             {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            for (auto& e : edges) lines.push_back({c[e[0]], c[e[1]], sel});
        }
    }

    lastStats_ = renderer_.RenderScene(*scene_, cam, lines);
    return lastStats_;
}

RenderStats Engine::RenderGameFrame() {
    // A game frame is the scene through its own MainCamera whether or not the
    // simulation is running: `render.screenshot --mode game` and the editor's
    // play view both want that, and the editor grid has no place in it. A
    // scene without a camera falls back to the editor pose, and the stats say
    // so rather than passing one for the other.
    CameraView cam;
    const bool usingGameCam = BuildCameraView(&cam, true);
    std::vector<DebugLine> lines;
    BuildOverlayLines(&lines);
    lastStats_ = renderer_.RenderScene(*scene_, cam, lines);
    lastStats_.camera = usingGameCam ? "scene" : "editor-fallback";
    return lastStats_;
}

Json Engine::DebugCollectStats() {
    Json out = Json::MakeObject();
    int objects = 0, active = 0, meshRenderers = 0, meshNull = 0, emitted = 0;
    Json names = Json::MakeArray();
    for (const auto& go : scene_->objects) {
        objects++;
        if (!go->ActiveInHierarchy()) continue;
        active++;
        auto* mr = go->GetComponent<MeshRenderer>();
        if (!mr || !mr->enabled) continue;
        meshRenderers++;
        std::shared_ptr<Mesh> m = (mr->mesh == MeshRenderer::MeshSource::Custom && !mr->meshPath.empty())
                                      ? EngineMeshLibrary().GetObj(EngineResolvePath(mr->meshPath))
                                      : EngineMeshLibrary().Get((PrimitiveMesh)(int)mr->mesh);
        if (!m) meshNull++;
        else {
            emitted++;
            names.push(go->name);
        }
    }
    out.set("objects", objects);
    out.set("activeInHierarchy", active);
    out.set("meshRenderers", meshRenderers);
    out.set("meshNull", meshNull);
    out.set("emitted", emitted);
    out.set("emittedNames", std::move(names));
    out.set("viewport", Json::MakeArray({(double)renderer_.target().width, (double)renderer_.target().height}));
    return out;
}

// A physics hit as the JSON a script or the CLI reads: the entity plus the
// world point/normal/distance.
static Json HitToJson(const PhysicsHit& h) {
    Json o = Json::MakeObject();
    o.set("entity", (double)(h.go ? h.go->m_id : 0));
    o.set("guid", h.go ? h.go->m_guid : std::string());
    o.set("name", h.go ? h.go->name : std::string());
    o.set("point", Json::MakeArray({h.point.x, h.point.y, h.point.z}));
    o.set("normal", Json::MakeArray({h.normal.x, h.normal.y, h.normal.z}));
    o.set("distance", (double)h.distance);
    return o;
}

static Json OverlapToJson(const std::vector<GameObject*>& hits) {
    Json arr = Json::MakeArray();
    for (GameObject* go : hits) {
        if (!go) continue;
        Json o = Json::MakeObject();
        o.set("id", (double)go->m_id);
        o.set("guid", go->m_guid);
        o.set("name", go->name);
        arr.push(std::move(o));
    }
    return arr;
}

Json Engine::QueryRaycast(const Vec3& origin, const Vec3& direction, float maxDistance) {
    PhysicsHit h;
    if (!PhysicsRaycast(*scene_, origin, direction, maxDistance, &h)) return Json();
    return HitToJson(h);
}

Json Engine::QueryRaycastAll(const Vec3& origin, const Vec3& direction, float maxDistance) {
    Json arr = Json::MakeArray();
    for (const PhysicsHit& h : PhysicsRaycastAll(*scene_, origin, direction, maxDistance))
        arr.push(HitToJson(h));
    return arr;
}

Json Engine::QueryOverlapSphere(const Vec3& center, float radius) {
    return OverlapToJson(PhysicsOverlapSphere(*scene_, center, radius));
}

Json Engine::QueryOverlapBox(const Vec3& center, const Vec3& halfExtents) {
    return OverlapToJson(PhysicsOverlapBox(*scene_, center, halfExtents));
}

Json Engine::QuerySphereCast(const Vec3& origin, float radius, const Vec3& direction,
                             float maxDistance) {
    PhysicsHit h;
    if (!PhysicsSphereCast(*scene_, origin, radius, direction, maxDistance, &h)) return Json();
    return HitToJson(h);
}

Json Engine::QueryBoxCast(const Vec3& center, const Vec3& halfExtents, const Vec3& direction,
                          float maxDistance) {
    PhysicsHit h;
    if (!PhysicsBoxCast(*scene_, center, halfExtents, direction, maxDistance, &h)) return Json();
    return HitToJson(h);
}

Json Engine::QueryCharacterMove(EntityId id, const Vec3& displacement) {
    Json o = Json::MakeObject();
    GameObject* go = scene_->FindEntity(id);
    if (!go) {
        o.set("error", "entity not found");
        return o;
    }
    CharacterController* cc = go->GetComponent<CharacterController>();
    if (!cc) {
        o.set("error", "entity has no CharacterController");
        return o;
    }
    bool grounded = PhysicsCharacterMove(*scene_, *go, *cc, displacement);
    Vec3 world = go->transform().WorldPosition();
    o.set("id", (double)go->m_id);
    o.set("grounded", grounded);
    o.set("position", Json::MakeArray({world.x, world.y, world.z}));
    o.set("localPosition", Json::MakeArray({go->transform().position.x, go->transform().position.y,
                                            go->transform().position.z}));
    return o;
}

Json Engine::QueryPhysicsStep(float dt, int steps) {
    if (dt <= 0.0f) dt = 1.0f / 60.0f;
    if (steps < 0) steps = 0;
    if (steps > 100000) steps = 100000;
    for (int i = 0; i < steps; ++i) PhysicsStep(*scene_, dt);
    int dynamic = 0, sleeping = 0;
    for (const auto& go : scene_->objects) {
        if (!go->ActiveInHierarchy()) continue;
        RigidBody* rb = go->GetComponent<RigidBody>();
        if (!rb || !rb->enabled || rb->isKinematic) continue;
        ++dynamic;
        if (rb->sleeping) ++sleeping;
    }
    Json o = Json::MakeObject();
    o.set("steps", (double)steps);
    o.set("dt", (double)dt);
    o.set("dynamic", (double)dynamic);
    o.set("sleeping", (double)sleeping);
    return o;
}

Json Engine::ProfileSnapshot() const {
    Json out = Profiler::Snapshot();
    const RenderStats s = lastStats_;
    Json render = Json::MakeObject();
    render.set("backend", std::string(s.backend ? s.backend : "software"));
    render.set("drawCalls", (double)s.drawCalls);
    render.set("triangles", (double)s.triangles);
    render.set("meshesSeen", (double)s.meshesSeen);
    render.set("frustumCulled", (double)s.frustumCulled);
    render.set("clipRejected", (double)s.clipRejected);
    render.set("drawCalls2D", (double)s.drawCalls2D);
    render.set("lightCount", (double)s.lightCount);
    render.set("shadowedPixels", (double)s.shadowedPixels);
    render.set("postPasses", (double)s.postPasses);
    out.set("render", render);
    out.set("entities", (double)scene_->objects.size());
    const Jobs::Stats jobs = Jobs::Snapshot();
    Json j = Json::MakeObject();
    j.set("workers", (double)jobs.workers);
    j.set("dispatches", (double)jobs.dispatches);
    j.set("indices", (double)jobs.indices);
    out.set("jobs", j);
    return out;
}

bool Engine::SaveFramePNG(const std::string& path) const {    const FrameTarget& t = renderer_.target();
    if (t.width <= 0 || t.height <= 0) return false;
    // stb expects top-down rows; our buffer already is.
    return stbi_write_png(path.c_str(), t.width, t.height, 4, t.color.data(), t.width * 4) != 0;
}

EntityId Engine::Pick(int pixelX, int pixelY) {    CameraView cam;
    BuildCameraView(&cam, playing_);
    Vec3 dir = ScreenRayDir(pixelX, pixelY);
    Vec3 origin = ScreenRayOrigin(pixelX, pixelY);
    float best = 1e30f;
    EntityId bestId = kInvalidEntity;
    MeshLibrary& meshes = EngineMeshLibrary();
    for (const auto& go : scene_->objects) {
        if (!go->ActiveInHierarchy()) continue;
        AABB box;
        bool has = false;
        if (auto* mr = go->GetComponent<MeshRenderer>(); mr && mr->enabled) {
            std::shared_ptr<Mesh> mesh;
            if (mr->mesh == MeshRenderer::MeshSource::Custom && !mr->meshPath.empty())
                mesh = meshes.GetObj(EngineResolvePath(mr->meshPath));
            else
                mesh = meshes.Get((PrimitiveMesh)(int)mr->mesh);
            if (mesh) {
                box = mesh->Bounds().Transformed(go->transform().WorldMatrix());
                has = true;
            }
        }
        if (!has && go->GetComponent<SpriteRenderer>()) {
            Vec3 s = go->transform().WorldScale();
            box = AABB(go->transform().WorldPosition() - s * 0.5f, go->transform().WorldPosition() + s * 0.5f);
            has = true;
        }
        if (!has) continue;
        float t = box.RayIntersect(origin, dir);
        if (t >= 0 && t < best) {
            best = t;
            bestId = go->m_id;
        }
    }
    return bestId;
}

Vec2 Engine::WorldToScreen(const Vec3& world, bool* ok) {
    CameraView cam;
    BuildCameraView(&cam, playing_);
    Mat4 vp = cam.projection * cam.view;
    Vec4 c = vp * Vec4(world, 1.0f);
    if (ok) *ok = c.w > 1e-6f;
    if (c.w <= 1e-6f) return Vec2();
    float ndcX = c.x / c.w, ndcY = c.y / c.w;
    return Vec2((ndcX * 0.5f + 0.5f) * renderer_.target().width,
                (1.0f - (ndcY * 0.5f + 0.5f)) * renderer_.target().height);
}

Vec3 Engine::ScreenRayDir(int pixelX, int pixelY) {
    CameraView cam;
    BuildCameraView(&cam, playing_);
    float ndcX = 2.0f * ((pixelX + 0.5f) / renderer_.target().width) - 1.0f;
    float ndcY = 1.0f - 2.0f * ((pixelY + 0.5f) / renderer_.target().height);
    float aspect = (float)renderer_.target().width / std::max(1, renderer_.target().height);
    Mat4 invView = cam.view.Inverted();
    Vec3 right = invView.TransformDirection(Vec3::Right);
    Vec3 up = invView.TransformDirection(Vec3::Up);
    Vec3 forward = invView.TransformDirection(Vec3::Forward);  // camera forward in world
    if (cam.orthographic) return forward.Normalized();
    float tanHalf = std::tan(Radians(cam.fov) * 0.5f);
    Vec3 dir = forward + right * (ndcX * tanHalf * aspect) + up * (ndcY * tanHalf);
    return dir.Normalized();
}

Vec3 Engine::ScreenRayOrigin(int pixelX, int pixelY) {
    CameraView cam;
    BuildCameraView(&cam, playing_);
    if (!cam.orthographic) return cam.position;
    float ndcX = 2.0f * ((pixelX + 0.5f) / renderer_.target().width) - 1.0f;
    float ndcY = 1.0f - 2.0f * ((pixelY + 0.5f) / renderer_.target().height);
    float aspect = (float)renderer_.target().width / std::max(1, renderer_.target().height);
    Mat4 invView = cam.view.Inverted();
    Vec3 right = invView.TransformDirection(Vec3::Right);
    Vec3 up = invView.TransformDirection(Vec3::Up);
    return cam.position + right * (ndcX * cam.orthoSize * aspect) + up * (ndcY * cam.orthoSize);
}

}  // namespace Cairn
