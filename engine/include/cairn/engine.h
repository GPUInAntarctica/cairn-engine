// Cairn Engine - engine facade. Owns the scene, renderer, physics, script
// host and asset libraries. This is the surface the editor addon drives.
#pragma once

#include "cairn/asset.h"
#include "cairn/log.h"
#include "cairn/mesh.h"
#include "cairn/renderer.h"
#include "cairn/scripting.h"
#include "cairn/texture.h"

#include <memory>
#include <string>
#include <vector>

namespace Cairn {

class ScriptHost;  // internal (engine/src/script_host.h)

struct AudioEvent {
    std::string clip;
    float volume = 1.0f;
    float pitch = 1.0f;
};

struct ScriptClassInfo {
    std::string name;
    struct Prop {
        std::string name;
        int type = 0;  // PropType
        Json defaultValue;
    };
    std::vector<Prop> props;
};

struct ViewportCamera {
    Vec3 position{6.0f, 5.0f, 8.0f};
    Vec3 pivot{0.0f, 0.5f, 0.0f};
    float yaw = 35.0f;    // degrees around Y
    float pitch = -30.0f; // degrees, negative looks down
    bool orthographic = false;
    float fov = 60.0f;
    float orthoSize = 5.0f;
    float nearPlane = 0.05f;
    float farPlane = 800.0f;

    Mat4 ViewMatrix() const;
    Mat4 ProjectionMatrix(float aspect) const;
};

struct InputState {
    bool keyDown[512] = {};
    bool keyWasDown[512] = {};
    bool mouseDown[8] = {};
    bool mouseWasDown[8] = {};
    float mouseX = 0, mouseY = 0;  // pixels, bottom-left origin
    struct Axis { const char* name; int negative, positive, altNegative, altPositive; };
    std::vector<Axis> axes = {
        {"Horizontal", (int)KeyCode::Left, (int)KeyCode::Right, (int)KeyCode::A, (int)KeyCode::D},
        {"Vertical", (int)KeyCode::Down, (int)KeyCode::Up, (int)KeyCode::S, (int)KeyCode::W},
    };
};

// The engine instance. Not thread-safe: all calls come from the host thread.
class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // --- Project root (absolute path) for resolving asset references.
    void SetProjectRoot(const std::string& absolutePath);
    const std::string& projectRoot() const { return projectRoot_; }

    // --- Viewport.
    void SetViewportSize(int w, int h);
    int viewportWidth() { return renderer_.target().width; }
    int viewportHeight() { return renderer_.target().height; }
    FrameTarget& frame() { return renderer_.target(); }
    RenderStats RenderEditorFrame();                  // editor camera + grid
    RenderStats RenderGameFrame();                    // active CameraComponent
    ViewportCamera& viewCamera() { return viewCam_; }

    // --- Scene.
    Scene& scene() { return *scene_; }
    void NewScene(const std::string& name);
    bool LoadSceneFile(const std::string& path, std::string* error);
    bool SaveSceneFile(const std::string& path, std::string* error);
    // Editor undo/redo snapshots.
    std::string SerializeSceneJson() const;
    bool RestoreSceneJson(const std::string& json, std::string* error = nullptr);

    // --- Entities.
    EntityId CreateEntity(const std::string& name, EntityId parent);
    EntityId CreatePrimitive(PrimitiveMesh mesh, const std::string& name, EntityId parent);
    EntityId CreateSpriteEntity(const std::string& name, EntityId parent);
    void DestroyEntity(EntityId id);
    EntityId DuplicateEntity(EntityId id);
    GameObject* FindEntity(EntityId id);

    // --- Prefabs. Prefab files live under Assets (usually Assets/Prefabs) and
    // hold a serialized subtree. An instance's root carries a PrefabInstance
    // component recording its source so it can be re-applied or reverted.
    EntityId InstantiatePrefab(const std::string& prefabProjectPath, EntityId parent);
    bool CreatePrefabFromEntity(EntityId id, const std::string& prefabProjectPath, std::string* error);
    bool ApplyPrefab(EntityId id, std::string* error);
    // Replaces the instance with a fresh copy; returns the new entity id.
    EntityId RevertPrefab(EntityId id, std::string* error);

    // --- Components.
    bool AddComponent(EntityId id, const std::string& typeName);
    bool RemoveComponent(EntityId id, const std::string& typeName);
    // Sets a component property; triggers transform dirty tracking as needed.
    bool SetComponentProp(EntityId id, const std::string& typeName, const std::string& prop,
                          const Json& value);
    bool GetComponentProp(EntityId id, const std::string& typeName, const std::string& prop, Json* out);

    // --- Asset hot reload (editor calls on file change).
    void ReloadAssets();
    void ReloadScriptLibraries();

    // --- Asset database (stable GUIDs + .meta import settings).
    AssetDatabase& assets() { return assets_; }
    const AssetDatabase& assets() const { return assets_; }
    // Re-scans <project>/Assets and (re)indexes every asset GUID.
    void RefreshAssets();

    // --- Scripting.
    // Loads/reloads the compiled script library. Returns false with a message.
    bool LoadScriptLibrary(const std::string& absolutePath, std::string* error);
    // Fills in inspector fields that the scene does not serialize yet with the
    // defaults declared by the loaded script classes.
    void ApplyScriptPropDefaults();
    std::vector<ScriptClassInfo> ScriptClasses() const;
    bool HasScriptClass(const std::string& name) const;

    // --- Play mode.
    bool Play();      // snapshots the scene, starts scripts & physics
    void Pause();
    void Resume();
    void Step();      // advance one frame while paused
    void Stop();      // restore snapshot
    bool isPlaying() const { return playing_; }
    bool isPaused() const { return paused_; }
    // Advances the simulation by dt seconds (host calls every frame).
    void Update(float dt);

    // --- Input injection (viewport focus feeds these).
    InputState& input() { return input_; }
    void EndFrameInput();  // called after Update: wasDown <- down

    // --- Picking & projection (editor viewport tools).
    EntityId Pick(int pixelX, int pixelY);            // 0 = none
    Vec2 WorldToScreen(const Vec3& world, bool* ok);  // editor/game camera in use
    Vec3 ScreenRayDir(int pixelX, int pixelY);        // direction through pixel
    Vec3 ScreenRayOrigin(int pixelX, int pixelY);

    // --- Console & telemetry.
    std::vector<LogEntry> PollLogs(uint64_t afterId) { return Log::Poll(afterId); }
    std::vector<AudioEvent>& audioEvents() { return audioEvents_; }
    RenderStats lastStats() const { return lastStats_; }

    // When off, log entries are only buffered (host tools read them via PollLogs).
    void SetLogEcho(bool on) { Log::SetEchoStdout(on); }
    bool logEcho() const { return Log::EchoStdout(); }

    // Editor selection outline in the viewport.
    void SetHighlightEntity(EntityId id) { highlightEntity_ = id; }
    EntityId highlightEntity() const { return highlightEntity_; }

    // Diagnostic: replicates the renderer's mesh-collection loop.
    Json DebugCollectStats();

    float playTime() const { return time_; }
    uint64_t frameCount() const { return frameCount_; }

    // Writes the last rendered frame as PNG (editor screenshots, tests).
    bool SaveFramePNG(const std::string& path) const;

    Renderer& renderer() { return renderer_; }

    // Camera used for rendering right now (game camera during play).
    bool BuildCameraView(CameraView* out) const;

private:
    void BuildOverlayLines(std::vector<DebugLine>* out);
    void StartScripts();
    void StopScripts();

    Renderer renderer_;
    std::shared_ptr<Scene> scene_;
    ViewportCamera viewCam_;
    InputState input_;
    AssetDatabase assets_;
    std::string projectRoot_ = ".";
    std::unique_ptr<ScriptHost> scriptHost_;
    EntityId highlightEntity_ = 0;

    // Script host state.
    std::vector<ScriptClassInfo> scriptClasses_;
    std::string scriptLibPath_;

    // Play state.
    bool playing_ = false;
    bool paused_ = false;
    std::string playSnapshot_;
    float timeScale_ = 1.0f;
    float time_ = 0.0f;
    float accumulator_ = 0.0f;
    uint64_t frameCount_ = 0;

    std::vector<DebugLine> debugLines_;   // consumed from Debug::DrawLine
    std::vector<AudioEvent> audioEvents_;
    RenderStats lastStats_;
    bool gameScriptsStarted_ = false;

    friend Scene* EngineActiveScene();
    friend MeshLibrary& EngineMeshLibrary();
    friend TextureLibrary& EngineTextureLibrary();
    friend std::string EngineResolvePath(const std::string& projectPath);
    friend void EngineQueueDebugLine(const Vec3& a, const Vec3& b, Color color);
    friend void EngineQueueAudio(const std::string& clip, float volume, float pitch);
    friend InputState* EngineInputState();
    friend float EngineTimeScale();
    friend void EngineSetTimeScale(float);
};

// Free functions gameplay/library code uses (resolved to the active Engine).
MeshLibrary& EngineMeshLibrary();
TextureLibrary& EngineTextureLibrary();
std::string EngineResolvePath(const std::string& projectPath);
void EngineQueueDebugLine(const Vec3& a, const Vec3& b, Color color);
void EngineQueueAudio(const std::string& clip, float volume, float pitch);
InputState* EngineInputState();
float EngineTimeScale();
void EngineSetTimeScale(float scale);

}  // namespace Cairn
