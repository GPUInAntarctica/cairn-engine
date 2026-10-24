// Cairn Engine - Node native addon bridging the editor (Electron) to the
// engine runtime. Built with node-addon-api (N-API, ABI-stable across
// Electron and Node releases).
#include "cairn/engine.h"
#include "scene_internal.h"

#include <napi.h>
#include <cstring>
#include <memory>
#include <string>

using namespace Cairn;

namespace {

std::string GetString(const Napi::CallbackInfo& info, size_t index, const char* def = "") {
    if (info.Length() > index && info[index].IsString()) return info[index].As<Napi::String>().Utf8Value();
    return def;
}

double GetNumber(const Napi::CallbackInfo& info, size_t index, double def = 0) {
    if (info.Length() > index && info[index].IsNumber()) return info[index].As<Napi::Number>().DoubleValue();
    return def;
}

bool GetBool(const Napi::CallbackInfo& info, size_t index, bool def = false) {
    if (info.Length() > index && info[index].IsBoolean()) return info[index].As<Napi::Boolean>().Value();
    return def;
}

Json JsonFromString(const std::string& text) {
    return Json::ParseOr(text, Json());
}

// Converts a JS value into engine JSON (numbers/bools/strings/arrays).
Json JsonFromValue(Napi::Env env, const Napi::Value& value) {
    if (value.IsNumber()) return Json(value.As<Napi::Number>().DoubleValue());
    if (value.IsBoolean()) return Json(value.As<Napi::Boolean>().Value());
    if (value.IsString()) return Json(value.As<Napi::String>().Utf8Value());
    if (value.IsArray()) {
        Napi::Array arr = value.As<Napi::Array>();
        Json out = Json::MakeArray();
        for (uint32_t i = 0; i < arr.Length(); ++i) out.push(JsonFromValue(env, arr[i]));
        return out;
    }
    if (value.IsObject()) {
        Napi::Object obj = value.As<Napi::Object>();
        Json out = Json::MakeObject();
        Napi::Array keys = obj.GetPropertyNames();
        for (uint32_t i = 0; i < keys.Length(); ++i) {
            Napi::Value k = keys[i];
            out.set(k.As<Napi::String>().Utf8Value(), JsonFromValue(env, obj.Get(k)));
        }
        return out;
    }
    return Json();
}

Napi::Value JsonToValue(Napi::Env env, const Json& value) {
    switch (value.type()) {
        case Json::Type::Null: return env.Null();
        case Json::Type::Bool: return Napi::Boolean::New(env, value.asBool());
        case Json::Type::Number: return Napi::Number::New(env, value.asNumber());
        case Json::Type::String: return Napi::String::New(env, value.asString());
        case Json::Type::Array: {
            Napi::Array arr = Napi::Array::New(env, value.size());
            const auto& items = value.arrayItems();
            for (size_t i = 0; i < items.size(); ++i) arr.Set((uint32_t)i, JsonToValue(env, items[i]));
            return arr;
        }
        case Json::Type::Object: {
            Napi::Object obj = Napi::Object::New(env);
            for (const auto& kv : value.objectItems()) obj.Set(kv.first, JsonToValue(env, kv.second));
            return obj;
        }
    }
    return env.Null();
}

const char* PropKindName(EditorPropKind kind) {
    switch (kind) {
        case EditorPropKind::Float: return "float";
        case EditorPropKind::Int: return "int";
        case EditorPropKind::Bool: return "bool";
        case EditorPropKind::String: return "string";
        case EditorPropKind::Vec2: return "vec2";
        case EditorPropKind::Vec3: return "vec3";
        case EditorPropKind::Vec4: return "vec4";
        case EditorPropKind::Color: return "color";
        case EditorPropKind::Enum: return "enum";
        case EditorPropKind::Asset: return "asset";
        case EditorPropKind::Object: return "object";
        case EditorPropKind::Json: return "json";
    }
    return "float";
}

Napi::Object MakeError(Napi::Env env, const std::string& message) {
    Napi::Object out = Napi::Object::New(env);
    out.Set("ok", false);
    out.Set("error", message);
    return out;
}

// ---------------------------------------------------------------------------
// Engine wrapper.
// ---------------------------------------------------------------------------
class EngineWrap : public Napi::ObjectWrap<EngineWrap> {
public:
    static Napi::FunctionReference constructor;

    static void Init(Napi::Env env, Napi::Object exports) {
        Napi::Function func = DefineClass(env, "Engine", {
            InstanceMethod("setProjectRoot", &EngineWrap::SetProjectRoot),
            InstanceMethod("setViewportSize", &EngineWrap::SetViewportSize),
            InstanceMethod("getFrameBuffer", &EngineWrap::GetFrameBuffer),
            InstanceMethod("renderEditorFrame", &EngineWrap::RenderEditorFrame),
            InstanceMethod("renderGameFrame", &EngineWrap::RenderGameFrame),
            InstanceMethod("profile", &EngineWrap::Profile),
            InstanceMethod("saveFramePNG", &EngineWrap::SaveFramePNG),
            InstanceMethod("setViewCamera", &EngineWrap::SetViewCamera),
            InstanceMethod("getViewCamera", &EngineWrap::GetViewCamera),
            InstanceMethod("newScene", &EngineWrap::NewScene),
            InstanceMethod("loadScene", &EngineWrap::LoadScene),
            InstanceMethod("saveScene", &EngineWrap::SaveScene),
            InstanceMethod("sceneJSON", &EngineWrap::SceneJSON),
            InstanceMethod("restoreSceneJSON", &EngineWrap::RestoreSceneJSON),
            InstanceMethod("createEntity", &EngineWrap::CreateEntity),
            InstanceMethod("createPrimitive", &EngineWrap::CreatePrimitive),
            InstanceMethod("createSprite", &EngineWrap::CreateSprite),
            InstanceMethod("destroyEntity", &EngineWrap::DestroyEntity),
            InstanceMethod("duplicateEntity", &EngineWrap::DuplicateEntity),
            InstanceMethod("setEntityName", &EngineWrap::SetEntityName),
            InstanceMethod("setEntityActive", &EngineWrap::SetEntityActive),
            InstanceMethod("setEntityTag", &EngineWrap::SetEntityTag),
            InstanceMethod("setEntityLayer", &EngineWrap::SetEntityLayer),
            InstanceMethod("setEntityParent", &EngineWrap::SetEntityParent),
            InstanceMethod("getEntityJSON", &EngineWrap::GetEntityJSON),
            InstanceMethod("getSceneTree", &EngineWrap::GetSceneTree),
            InstanceMethod("addComponent", &EngineWrap::AddComponent),
            InstanceMethod("removeComponent", &EngineWrap::RemoveComponent),
            InstanceMethod("setComponentProp", &EngineWrap::SetComponentProp),
            InstanceMethod("getComponentProp", &EngineWrap::GetComponentProp),
            InstanceMethod("setSceneSetting", &EngineWrap::SetSceneSetting),
            InstanceMethod("getSceneSettings", &EngineWrap::GetSceneSettings),
            InstanceMethod("reloadAssets", &EngineWrap::ReloadAssets),
            InstanceMethod("refreshAssets", &EngineWrap::RefreshAssets),
            InstanceMethod("assetGuid", &EngineWrap::AssetGuid),
            InstanceMethod("assetPath", &EngineWrap::AssetPath),
            InstanceMethod("assetMeta", &EngineWrap::AssetMeta),
            InstanceMethod("assetMetaSet", &EngineWrap::AssetMetaSet),
            InstanceMethod("assetImport", &EngineWrap::AssetImport),
            InstanceMethod("assetMove", &EngineWrap::AssetMove),
            InstanceMethod("assetDelete", &EngineWrap::AssetDelete),
            InstanceMethod("assetsList", &EngineWrap::AssetsList),
            InstanceMethod("createPrefab", &EngineWrap::CreatePrefab),
            InstanceMethod("instantiatePrefab", &EngineWrap::InstantiatePrefab),
            InstanceMethod("applyPrefab", &EngineWrap::ApplyPrefab),
            InstanceMethod("revertPrefab", &EngineWrap::RevertPrefab),
            InstanceMethod("refreshPrefabOverrides", &EngineWrap::RefreshPrefabOverrides),
            InstanceMethod("prefabOverrides", &EngineWrap::PrefabOverrides),
            InstanceMethod("syncPrefabs", &EngineWrap::SyncPrefabs),
            InstanceMethod("getLayerMatrix", &EngineWrap::GetLayerMatrix),
            InstanceMethod("setLayerCollision", &EngineWrap::SetLayerCollision),
            InstanceMethod("physicsRaycast", &EngineWrap::PhysicsRaycast),
            InstanceMethod("physicsOverlapSphere", &EngineWrap::PhysicsOverlapSphere),
            InstanceMethod("physicsOverlapBox", &EngineWrap::PhysicsOverlapBox),
            InstanceMethod("physicsSphereCast", &EngineWrap::PhysicsSphereCast),
            InstanceMethod("physicsBoxCast", &EngineWrap::PhysicsBoxCast),
            InstanceMethod("characterMove", &EngineWrap::CharacterMove),
            InstanceMethod("physicsStep", &EngineWrap::PhysicsStep),
            InstanceMethod("loadScriptLibrary", &EngineWrap::LoadScriptLibrary),
            InstanceMethod("play", &EngineWrap::Play),
            InstanceMethod("pause", &EngineWrap::Pause),
            InstanceMethod("resume", &EngineWrap::Resume),
            InstanceMethod("step", &EngineWrap::Step),
            InstanceMethod("stop", &EngineWrap::Stop),
            InstanceMethod("isPlaying", &EngineWrap::IsPlaying),
            InstanceMethod("isPaused", &EngineWrap::IsPaused),
            InstanceMethod("update", &EngineWrap::Update),
            InstanceMethod("endFrameInput", &EngineWrap::EndFrameInput),
            InstanceMethod("keyDown", &EngineWrap::KeyDown),
            InstanceMethod("keyUp", &EngineWrap::KeyUp),
            InstanceMethod("mouseDown", &EngineWrap::MouseDown),
            InstanceMethod("mouseUp", &EngineWrap::MouseUp),
            InstanceMethod("mouseMove", &EngineWrap::MouseMove),
            InstanceMethod("pick", &EngineWrap::Pick),
            InstanceMethod("worldToScreen", &EngineWrap::WorldToScreen),
            InstanceMethod("setHighlightEntity", &EngineWrap::SetHighlightEntity),
            InstanceMethod("debugCollect", &EngineWrap::DebugCollect),
            InstanceMethod("pollLogs", &EngineWrap::PollLogs),
            InstanceMethod("pollAudio", &EngineWrap::PollAudio),
            InstanceMethod("setLogEcho", &EngineWrap::SetLogEcho),
            InstanceMethod("getLogEcho", &EngineWrap::GetLogEcho),
        });
        constructor = Napi::Persistent(func);
        exports.Set("Engine", func);
    }

    explicit EngineWrap(const Napi::CallbackInfo& info)
        : Napi::ObjectWrap<EngineWrap>(info), engine_(new Engine()) {}

    Engine& engine() { return *engine_; }

private:
    // -- project / viewport ------------------------------------------------
    void SetProjectRoot(const Napi::CallbackInfo& info) {
        engine_->SetProjectRoot(GetString(info, 0, "."));
    }
    void SetViewportSize(const Napi::CallbackInfo& info) {
        engine_->SetViewportSize((int)GetNumber(info, 0, 960), (int)GetNumber(info, 1, 540));
    }
    Napi::Value GetFrameBuffer(const Napi::CallbackInfo& info) {
        FrameTarget& t = engine_->frame();
        if (t.color.empty()) return info.Env().Null();
        // Electron renderers disallow external buffers, so hand out a copy
        // (~2.7 MB at 960x540; a memcpy costs well under a millisecond).
        return Napi::Buffer<uint8_t>::Copy(info.Env(), t.color.data(), t.color.size());
    }
    static Napi::Object StatsToObject(Napi::Env env, const RenderStats& s) {
        Napi::Object o = Napi::Object::New(env);
        o.Set("drawCalls", s.drawCalls);
        o.Set("triangles", s.triangles);
        o.Set("meshesSeen", s.meshesSeen);
        o.Set("clipRejected", s.clipRejected);
        o.Set("frustumCulled", s.frustumCulled);
        o.Set("drawCalls2D", s.drawCalls2D);
        o.Set("lightCount", s.lightCount);
        o.Set("shadowedPixels", s.shadowedPixels);
        o.Set("postPasses", s.postPasses);
        o.Set("camera", s.camera);
        return o;
    }
    Napi::Value RenderEditorFrame(const Napi::CallbackInfo& info) {
        return StatsToObject(info.Env(), engine_->RenderEditorFrame());
    }
    Napi::Value RenderGameFrame(const Napi::CallbackInfo& info) {
        return StatsToObject(info.Env(), engine_->RenderGameFrame());
    }
    Napi::Value Profile(const Napi::CallbackInfo& info) {
        return JsonToValue(info.Env(), engine_->ProfileSnapshot());
    }
    Napi::Value SaveFramePNG(const Napi::CallbackInfo& info) {
        bool ok = engine_->SaveFramePNG(GetString(info, 0));
        return Napi::Boolean::New(info.Env(), ok);
    }
    void SetViewCamera(const Napi::CallbackInfo& info) {
        if (info.Length() < 1 || !info[0].IsObject()) return;
        Napi::Object o = info[0].As<Napi::Object>();
        ViewportCamera& cam = engine_->viewCamera();
        if (o.Has("position")) {
            Napi::Value p = o.Get("position");
            if (p.IsArray() && p.As<Napi::Array>().Length() >= 3) {
                Napi::Array a = p.As<Napi::Array>();
                cam.position = {a.Get((uint32_t)0).As<Napi::Number>().FloatValue(),
                                a.Get(1).As<Napi::Number>().FloatValue(),
                                a.Get(2).As<Napi::Number>().FloatValue()};
            }
        }
        if (o.Has("yaw")) cam.yaw = o.Get("yaw").As<Napi::Number>().FloatValue();
        if (o.Has("pitch")) cam.pitch = o.Get("pitch").As<Napi::Number>().FloatValue();
        if (o.Has("ortho")) cam.orthographic = o.Get("ortho").As<Napi::Boolean>().Value();
        if (o.Has("orthoSize")) cam.orthoSize = o.Get("orthoSize").As<Napi::Number>().FloatValue();
        if (o.Has("fov")) cam.fov = o.Get("fov").As<Napi::Number>().FloatValue();
    }
    Napi::Value GetViewCamera(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        const ViewportCamera& cam = engine_->viewCamera();
        Napi::Object o = Napi::Object::New(env);
        Napi::Array pos = Napi::Array::New(env, 3);
        pos[(uint32_t)0] = cam.position.x; pos[1] = cam.position.y; pos[2] = cam.position.z;
        o.Set("position", pos);
        o.Set("yaw", cam.yaw);
        o.Set("pitch", cam.pitch);
        o.Set("ortho", cam.orthographic);
        o.Set("orthoSize", cam.orthoSize);
        o.Set("fov", cam.fov);
        return o;
    }

    // -- scene -------------------------------------------------------------
    void NewScene(const Napi::CallbackInfo& info) { engine_->NewScene(GetString(info, 0, "Scene")); }
    Napi::Value LoadScene(const Napi::CallbackInfo& info) {
        std::string err;
        bool ok = engine_->LoadSceneFile(GetString(info, 0), &err);
        Napi::Object o = Napi::Object::New(info.Env());
        o.Set("ok", ok);
        if (!ok) o.Set("error", err);
        return o;
    }
    Napi::Value SaveScene(const Napi::CallbackInfo& info) {
        std::string err;
        bool ok = engine_->SaveSceneFile(GetString(info, 0), &err);
        Napi::Object o = Napi::Object::New(info.Env());
        o.Set("ok", ok);
        if (!ok) o.Set("error", err);
        return o;
    }
    Napi::Value SceneJSON(const Napi::CallbackInfo& info) {
        return Napi::String::New(info.Env(), engine_->SerializeSceneJson());
    }
    Napi::Value RestoreSceneJSON(const Napi::CallbackInfo& info) {
        std::string err;
        bool ok = engine_->RestoreSceneJson(GetString(info, 0), &err);
        Napi::Object o = Napi::Object::New(info.Env());
        o.Set("ok", ok);
        if (!ok) o.Set("error", err);
        return o;
    }

    // -- entities ----------------------------------------------------------
    Napi::Value CreateEntity(const Napi::CallbackInfo& info) {
        EntityId parent = (EntityId)GetNumber(info, 1, 0);
        EntityId id = engine_->CreateEntity(GetString(info, 0, "GameObject"), parent);
        return Napi::Number::New(info.Env(), (double)id);
    }
    Napi::Value CreatePrimitive(const Napi::CallbackInfo& info) {
        PrimitiveMesh mesh = PrimitiveMesh::Cube;
        PrimitiveMeshFromName(GetString(info, 0, "Cube"), &mesh);
        EntityId parent = (EntityId)GetNumber(info, 2, 0);
        EntityId id = engine_->CreatePrimitive(mesh, GetString(info, 1, "GameObject"), parent);
        return Napi::Number::New(info.Env(), (double)id);
    }
    Napi::Value CreateSprite(const Napi::CallbackInfo& info) {
        EntityId parent = (EntityId)GetNumber(info, 1, 0);
        EntityId id = engine_->CreateSpriteEntity(GetString(info, 0, "GameObject"), parent);
        return Napi::Number::New(info.Env(), (double)id);
    }
    void DestroyEntity(const Napi::CallbackInfo& info) {
        engine_->DestroyEntity((EntityId)GetNumber(info, 0, 0));
    }
    Napi::Value DuplicateEntity(const Napi::CallbackInfo& info) {
        EntityId id = engine_->DuplicateEntity((EntityId)GetNumber(info, 0, 0));
        return Napi::Number::New(info.Env(), (double)id);
    }
    void SetEntityName(const Napi::CallbackInfo& info) {
        if (GameObject* go = engine_->FindEntity((EntityId)GetNumber(info, 0, 0)))
            go->name = GetString(info, 1, go->name.c_str());
    }
    void SetEntityActive(const Napi::CallbackInfo& info) {
        if (GameObject* go = engine_->FindEntity((EntityId)GetNumber(info, 0, 0)))
            go->active = GetBool(info, 1, true);
    }
    void SetEntityTag(const Napi::CallbackInfo& info) {
        if (GameObject* go = engine_->FindEntity((EntityId)GetNumber(info, 0, 0)))
            go->tag = GetString(info, 1);
    }
    void SetEntityLayer(const Napi::CallbackInfo& info) {
        if (GameObject* go = engine_->FindEntity((EntityId)GetNumber(info, 0, 0))) {
            int layer = (int)GetNumber(info, 1, 0);
            go->layer = layer < 0 ? 0 : (layer > 31 ? 31 : layer);
        }
    }
    void SetEntityParent(const Napi::CallbackInfo& info) {
        GameObject* go = engine_->FindEntity((EntityId)GetNumber(info, 0, 0));
        GameObject* parent = GetNumber(info, 1, 0) > 0 ? engine_->FindEntity((EntityId)GetNumber(info, 1, 0)) : nullptr;
        if (go) go->SetParent(parent);
    }

    // Entity JSON used by the hierarchy and inspector.
    Napi::Value EntityToObject(Napi::Env env, const GameObject* go) {
        Napi::Object o = Napi::Object::New(env);
        o.Set("id", (double)go->m_id);
        o.Set("guid", go->m_guid);
        o.Set("name", go->name);
        o.Set("active", go->active);
        o.Set("tag", go->tag);
        o.Set("layer", go->layer);
        o.Set("parent", go->transform().m_parent ? (double)go->transform().m_parent->m_id : 0.0);
        const Transform& t = go->transform();
        Napi::Object tr = Napi::Object::New(env);
        Napi::Array pos = Napi::Array::New(env, 3);
        pos[(uint32_t)0] = t.position.x; pos[1] = t.position.y; pos[2] = t.position.z;
        Vec3 e = t.GetEulerDegrees();
        Napi::Array rot = Napi::Array::New(env, 3);
        rot[(uint32_t)0] = e.x; rot[1] = e.y; rot[2] = e.z;
        Napi::Array scl = Napi::Array::New(env, 3);
        scl[(uint32_t)0] = t.scale.x; scl[1] = t.scale.y; scl[2] = t.scale.z;
        tr.Set("position", pos);
        tr.Set("rotation", rot);
        tr.Set("scale", scl);
        o.Set("transform", tr);

        Napi::Array comps = Napi::Array::New(env);
        uint32_t ci = 0;
        for (const auto& c : go->GetComponents()) {
            Napi::Object cj = Napi::Object::New(env);
            cj.Set("type", c->TypeName());
            cj.Set("enabled", c->enabled);
            if (c->TypeId() == ComponentTypeId::Transform) {
                cj.Set("isTransform", true);
            } else if (c->TypeId() == ComponentTypeId::Script) {
                const ScriptComponent* sc = static_cast<const ScriptComponent*>(c.get());
                cj.Set("scriptClass", sc->scriptClass);
                Napi::Object props = Napi::Object::New(env);
                for (const auto& kv : sc->properties.objectItems())
                    props.Set(kv.first, JsonToValue(env, kv.second));
                cj.Set("props", props);
            } else if (const auto* defs = ComponentPropertyDefs(c.get())) {
                Napi::Object props = Napi::Object::New(env);
                Napi::Array meta = Napi::Array::New(env);
                uint32_t mi = 0;
                for (const auto& def : *defs) {
                    props.Set(def.name, JsonToValue(env, def.get(c.get())));
                    Napi::Object md = Napi::Object::New(env);
                    md.Set("name", def.name);
                    md.Set("label", def.label);
                    md.Set("kind", PropKindName(def.kind));
                    if (def.enumOptions) {
                        Napi::Array opts = Napi::Array::New(env);
                        for (int i = 0; i < def.enumCount; ++i) opts.Set((uint32_t)i, def.enumOptions[i]);
                        md.Set("options", opts);
                    }
                    if (def.assetKind && def.assetKind[0]) md.Set("assetKind", def.assetKind);
                    meta.Set(mi++, md);
                }
                cj.Set("props", props);
                cj.Set("meta", meta);
            }
            comps.Set(ci++, cj);
        }
        o.Set("components", comps);
        return o;
    }

    Napi::Value GetEntityJSON(const Napi::CallbackInfo& info) {
        GameObject* go = engine_->FindEntity((EntityId)GetNumber(info, 0, 0));
        if (!go) return info.Env().Null();
        return EntityToObject(info.Env(), go);
    }
    Napi::Value GetSceneTree(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        Napi::Array list = Napi::Array::New(env);
        uint32_t i = 0;
        for (const auto& go : engine_->scene().objects)
            list.Set(i++, EntityToObject(env, go.get()));
        Napi::Object o = Napi::Object::New(env);
        o.Set("name", engine_->scene().name);
        o.Set("entities", list);
        return o;
    }

    // -- components --------------------------------------------------------
    Napi::Value AddComponent(const Napi::CallbackInfo& info) {
        bool ok = engine_->AddComponent((EntityId)GetNumber(info, 0, 0), GetString(info, 1));
        return Napi::Boolean::New(info.Env(), ok);
    }
    Napi::Value RemoveComponent(const Napi::CallbackInfo& info) {
        bool ok = engine_->RemoveComponent((EntityId)GetNumber(info, 0, 0), GetString(info, 1));
        return Napi::Boolean::New(info.Env(), ok);
    }
    Napi::Value SetComponentProp(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        EntityId id = (EntityId)GetNumber(info, 0, 0);
        std::string type = GetString(info, 1);
        std::string prop = GetString(info, 2);
        Json value = JsonFromValue(env, info.Length() > 3 ? info[3] : env.Null());
        bool ok = engine_->SetComponentProp(id, type, prop, value);
        return Napi::Boolean::New(env, ok);
    }
    Napi::Value GetComponentProp(const Napi::CallbackInfo& info) {
        EntityId id = (EntityId)GetNumber(info, 0, 0);
        Json out;
        if (!engine_->GetComponentProp(id, GetString(info, 1), GetString(info, 2), &out))
            return info.Env().Null();
        return JsonToValue(info.Env(), out);
    }

    // -- scene settings ------------------------------------------------------
    Napi::Value SetSceneSetting(const Napi::CallbackInfo& info) {
        Scene& s = engine_->scene();
        std::string name = GetString(info, 0);
        Napi::Env env = info.Env();
        Napi::Value v = info.Length() > 1 ? info[1] : env.Null();
        auto readColor = [&](const Napi::Value& val) -> Color {
            if (!val.IsArray()) return Color();
            Napi::Array a = val.As<Napi::Array>();
            return {a.Get((uint32_t)0).As<Napi::Number>().FloatValue(), a.Get(1).As<Napi::Number>().FloatValue(),
                    a.Get(2).As<Napi::Number>().FloatValue(), a.Get(3).As<Napi::Number>().FloatValue()};
        };
        if (name == "gravity" && v.IsArray()) {
            Napi::Array a = v.As<Napi::Array>();
            s.gravity = {a.Get((uint32_t)0).As<Napi::Number>().FloatValue(), a.Get(1).As<Napi::Number>().FloatValue(),
                         a.Get(2).As<Napi::Number>().FloatValue()};
        } else if (name == "ambient" && v.IsArray()) s.ambient = readColor(v);
        else if (name == "background" && v.IsArray()) s.background = readColor(v);
        else if (name == "fogEnabled") s.fogEnabled = v.ToBoolean();
        else if (name == "fogColor" && v.IsArray()) s.fogColor = readColor(v);
        else if (name == "fogDensity") s.fogDensity = (float)v.ToNumber().DoubleValue();
        else if (name == "tonemap") s.tonemap = (int)v.ToNumber().DoubleValue();
        else if (name == "exposure") s.exposure = (float)v.ToNumber().DoubleValue();
        else if (name == "shadows") s.shadows = (int)v.ToNumber().DoubleValue();
        else if (name == "shadowResolution") s.shadowResolution = (int)v.ToNumber().DoubleValue();
        else if (name == "shadowDistance") s.shadowDistance = (float)v.ToNumber().DoubleValue();
        else if (name == "postVignette") s.postVignette = (float)v.ToNumber().DoubleValue();
        else if (name == "postVignetteRadius") s.postVignetteRadius = (float)v.ToNumber().DoubleValue();
        else if (name == "postVignetteSoftness") s.postVignetteSoftness = (float)v.ToNumber().DoubleValue();
        else if (name == "postContrast") s.postContrast = (float)v.ToNumber().DoubleValue();
        else if (name == "postSaturation") s.postSaturation = (float)v.ToNumber().DoubleValue();
        else if (name == "postBloomStrength") s.postBloomStrength = (float)v.ToNumber().DoubleValue();
        else if (name == "postBloomThreshold") s.postBloomThreshold = (float)v.ToNumber().DoubleValue();
        else if (name == "postFxaa") s.postFxaa = v.ToBoolean();
        else if (name == "textureMemoryMB") s.textureMemoryMB = (int)v.ToNumber().DoubleValue();
        else if (name == "renderBackend") s.renderBackend = (int)v.ToNumber().DoubleValue();
        else if (name == "msaa") s.msaa = (int)v.ToNumber().DoubleValue();
        else if (name == "name") s.name = v.ToString().Utf8Value();
        else return Napi::Boolean::New(env, false);
        return Napi::Boolean::New(env, true);
    }
    Napi::Value GetSceneSettings(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        const Scene& s = engine_->scene();
        Napi::Object o = Napi::Object::New(env);
        o.Set("name", s.name);
        Napi::Array g = Napi::Array::New(env, 3);
        g[(uint32_t)0] = s.gravity.x; g[1] = s.gravity.y; g[2] = s.gravity.z;
        o.Set("gravity", g);
        auto color = [&](const Color& c) {
            Napi::Array a = Napi::Array::New(env, 4);
            a[(uint32_t)0] = c.r; a[1] = c.g; a[2] = c.b; a[3] = c.a;
            return a;
        };
        o.Set("ambient", color(s.ambient));
        o.Set("background", color(s.background));
        o.Set("fogEnabled", s.fogEnabled);
        o.Set("fogColor", color(s.fogColor));
        o.Set("fogDensity", s.fogDensity);
        o.Set("tonemap", s.tonemap);
        o.Set("exposure", s.exposure);
        o.Set("shadows", s.shadows);
        o.Set("shadowResolution", s.shadowResolution);
        o.Set("shadowDistance", s.shadowDistance);
        o.Set("postVignette", s.postVignette);
        o.Set("postVignetteRadius", s.postVignetteRadius);
        o.Set("postVignetteSoftness", s.postVignetteSoftness);
        o.Set("postContrast", s.postContrast);
        o.Set("postSaturation", s.postSaturation);
        o.Set("postBloomStrength", s.postBloomStrength);
        o.Set("postBloomThreshold", s.postBloomThreshold);
        o.Set("postFxaa", s.postFxaa);
        o.Set("textureMemoryMB", s.textureMemoryMB);
        o.Set("renderBackend", s.renderBackend);
        o.Set("msaa", s.msaa);
        return o;
    }

    // -- assets --------------------------------------------------------------
    void ReloadAssets(const Napi::CallbackInfo& info) { engine_->ReloadAssets(); }
    void RefreshAssets(const Napi::CallbackInfo& info) { engine_->RefreshAssets(); }

    Napi::Value AssetGuid(const Napi::CallbackInfo& info) {
        const std::string path = GetString(info, 0);
        const std::string guid = engine_->assets().GuidForPath(path);
        if (guid.empty()) return info.Env().Null();
        return Napi::String::New(info.Env(), guid);
    }
    Napi::Value AssetPath(const Napi::CallbackInfo& info) {
        const std::string path = engine_->assets().PathForGuid(GetString(info, 0));
        if (path.empty()) return info.Env().Null();
        return Napi::String::New(info.Env(), path);
    }
    Napi::Value AssetMeta(const Napi::CallbackInfo& info) {
        Cairn::AssetMeta meta;
        if (!engine_->assets().ReadMeta(GetString(info, 0), &meta)) return info.Env().Null();
        Napi::Env env = info.Env();
        Napi::Object o = Napi::Object::New(env);
        o.Set("guid", meta.guid);
        o.Set("importer", meta.importer);
        o.Set("version", meta.version);
        o.Set("settings", JsonToValue(env, meta.settings));
        return o;
    }
    Napi::Value AssetMetaSet(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        const std::string path = GetString(info, 0);
        Cairn::AssetMeta meta;
        if (!engine_->assets().ReadMeta(path, &meta)) {
            meta.guid = engine_->assets().EnsureMeta(path);
            if (meta.guid.empty()) return MakeError(env, "cannot create meta for " + path);
            engine_->assets().ReadMeta(path, &meta);
        }
        if (info.Length() > 1 && info[1].IsObject()) meta.settings = JsonFromValue(env, info[1]);
        std::string err;
        bool ok = engine_->assets().WriteMeta(path, meta, &err);
        if (ok) engine_->RefreshAssets();
        Napi::Object o = Napi::Object::New(env);
        o.Set("ok", ok);
        if (!ok) o.Set("error", err);
        else o.Set("guid", meta.guid);
        return o;
    }
    Napi::Value AssetImport(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        std::string err;
        const std::string guid =
            engine_->assets().ImportFile(GetString(info, 0), GetString(info, 1), &err);
        Napi::Object o = Napi::Object::New(env);
        o.Set("ok", !guid.empty());
        if (guid.empty()) o.Set("error", err);
        o.Set("guid", guid);
        o.Set("path", GetString(info, 1));
        return o;
    }
    Napi::Value AssetMove(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        std::string err;
        bool ok = engine_->assets().MoveAsset(GetString(info, 0), GetString(info, 1), &err);
        Napi::Object o = Napi::Object::New(env);
        o.Set("ok", ok);
        if (!ok) o.Set("error", err);
        else { o.Set("guid", engine_->assets().GuidForPath(GetString(info, 1))); o.Set("path", GetString(info, 1)); }
        return o;
    }
    Napi::Value AssetDelete(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        std::string err;
        bool ok = engine_->assets().DeleteAsset(GetString(info, 0), &err);
        Napi::Object o = Napi::Object::New(env);
        o.Set("ok", ok);
        if (!ok) o.Set("error", err);
        return o;
    }
    Napi::Value AssetsList(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        Napi::Array arr = Napi::Array::New(env);
        uint32_t i = 0;
        for (const AssetEntry& e : engine_->assets().entries()) {
            Napi::Object o = Napi::Object::New(env);
            o.Set("guid", e.guid);
            o.Set("path", e.path);
            o.Set("importer", e.importer);
            arr.Set(i++, o);
        }
        return arr;
    }

    // -- prefabs -------------------------------------------------------------
    Napi::Value CreatePrefab(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        std::string err;
        bool ok = engine_->CreatePrefabFromEntity((EntityId)GetNumber(info, 0, 0), GetString(info, 1), &err);
        Napi::Object o = Napi::Object::New(env);
        o.Set("ok", ok);
        if (!ok) o.Set("error", err);
        else o.Set("guid", engine_->assets().GuidForPath(GetString(info, 1)));
        o.Set("path", GetString(info, 1));
        return o;
    }
    Napi::Value InstantiatePrefab(const Napi::CallbackInfo& info) {
        EntityId parent = (EntityId)GetNumber(info, 1, 0);
        EntityId id = engine_->InstantiatePrefab(GetString(info, 0), parent);
        return Napi::Number::New(info.Env(), (double)id);
    }
    Napi::Value ApplyPrefab(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        std::string err;
        bool ok = engine_->ApplyPrefab((EntityId)GetNumber(info, 0, 0), &err);
        Napi::Object o = Napi::Object::New(env);
        o.Set("ok", ok);
        if (!ok) o.Set("error", err);
        return o;
    }
    Napi::Value RevertPrefab(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        std::string err;
        EntityId id = engine_->RevertPrefab((EntityId)GetNumber(info, 0, 0), &err);
        Napi::Object o = Napi::Object::New(env);
        o.Set("ok", id != 0);
        if (id == 0) o.Set("error", err);
        o.Set("id", (double)id);
        return o;
    }
    // Per-property overrides: the differences between an instance and its asset.
    Napi::Value RefreshPrefabOverrides(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        std::string err;
        int n = engine_->RefreshPrefabOverrides((EntityId)GetNumber(info, 0, 0), &err);
        Napi::Object o = Napi::Object::New(env);
        o.Set("ok", n >= 0);
        if (n < 0) o.Set("error", err);
        else o.Set("count", (double)n);
        return o;
    }
    Napi::Value PrefabOverrides(const Napi::CallbackInfo& info) {
        Json overrides = engine_->PrefabOverrides((EntityId)GetNumber(info, 0, 0));
        return JsonToValue(info.Env(), overrides);
    }
    Napi::Value SyncPrefabs(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        std::string err;
        int n = engine_->SyncPrefabs((EntityId)GetNumber(info, 0, 0), &err);
        Napi::Object o = Napi::Object::New(env);
        o.Set("ok", n >= 0);
        if (n < 0) o.Set("error", err);
        else o.Set("count", (double)n);
        return o;
    }

    // -- physics queries + layers -------------------------------------------
    static bool ReadVec3(const Napi::Value& v, Vec3* out) {
        if (!v.IsArray()) return false;
        Napi::Array a = v.As<Napi::Array>();
        if (a.Length() < 3) return false;
        *out = Vec3(a.Get((uint32_t)0).As<Napi::Number>().FloatValue(),
                    a.Get(1).As<Napi::Number>().FloatValue(),
                    a.Get(2).As<Napi::Number>().FloatValue());
        return true;
    }
    Napi::Value PhysicsRaycast(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        Vec3 origin, dir;
        if (!ReadVec3(info[0], &origin) || !ReadVec3(info[1], &dir)) return env.Null();
        float maxDistance = (float)GetNumber(info, 2, 1e30);
        bool all = GetBool(info, 3, false);
        Json result = all ? engine_->QueryRaycastAll(origin, dir, maxDistance)
                          : engine_->QueryRaycast(origin, dir, maxDistance);
        return JsonToValue(env, result);
    }
    Napi::Value PhysicsOverlapSphere(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        Vec3 center;
        if (!ReadVec3(info[0], &center)) return env.Null();
        float radius = (float)GetNumber(info, 1, 1.0);
        return JsonToValue(env, engine_->QueryOverlapSphere(center, radius));
    }
    Napi::Value PhysicsOverlapBox(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        Vec3 center, half;
        if (!ReadVec3(info[0], &center) || !ReadVec3(info[1], &half)) return env.Null();
        return JsonToValue(env, engine_->QueryOverlapBox(center, half));
    }
    Napi::Value PhysicsSphereCast(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        Vec3 origin, dir;
        if (!ReadVec3(info[0], &origin) || !ReadVec3(info[2], &dir)) return env.Null();
        float radius = (float)GetNumber(info, 1, 1.0);
        float maxDistance = (float)GetNumber(info, 3, 1000.0);
        return JsonToValue(env, engine_->QuerySphereCast(origin, radius, dir, maxDistance));
    }
    Napi::Value PhysicsBoxCast(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        Vec3 center, half, dir;
        if (!ReadVec3(info[0], &center) || !ReadVec3(info[1], &half) || !ReadVec3(info[2], &dir))
            return env.Null();
        float maxDistance = (float)GetNumber(info, 3, 1000.0);
        return JsonToValue(env, engine_->QueryBoxCast(center, half, dir, maxDistance));
    }
    Napi::Value CharacterMove(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        EntityId id = (EntityId)GetNumber(info, 0, 0);
        Vec3 disp;
        if (!ReadVec3(info[1], &disp)) return env.Null();
        return JsonToValue(env, engine_->QueryCharacterMove(id, disp));
    }
    Napi::Value PhysicsStep(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        float dt = (float)GetNumber(info, 0, 1.0 / 60.0);
        int steps = (int)GetNumber(info, 1, 1);
        return JsonToValue(env, engine_->QueryPhysicsStep(dt, steps));
    }
    Napi::Value GetLayerMatrix(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        Napi::Array arr = Napi::Array::New(env, 32);
        for (int i = 0; i < 32; ++i) arr.Set((uint32_t)i, (double)engine_->scene().layerMatrix[(size_t)i]);
        return arr;
    }
    Napi::Value SetLayerCollision(const Napi::CallbackInfo& info) {
        int a = (int)GetNumber(info, 0, 0);
        int b = (int)GetNumber(info, 1, 0);
        bool on = GetBool(info, 2, true);
        engine_->scene().SetLayerCollision(a, b, on);
        return Napi::Boolean::New(info.Env(), true);
    }

    // -- scripting -----------------------------------------------------------
    Napi::Value LoadScriptLibrary(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        std::string err;
        bool ok = engine_->LoadScriptLibrary(GetString(info, 0), &err);
        Napi::Object o = Napi::Object::New(env);
        o.Set("ok", ok);
        if (!ok) o.Set("error", err);
        Napi::Array classes = Napi::Array::New(env);
        uint32_t i = 0;
        for (const ScriptClassInfo& c : engine_->ScriptClasses()) {
            Napi::Object cj = Napi::Object::New(env);
            cj.Set("name", c.name);
            Napi::Array props = Napi::Array::New(env);
            uint32_t pi = 0;
            for (const auto& p : c.props) {
                Napi::Object pj = Napi::Object::New(env);
                pj.Set("name", p.name);
                const char* typeNames[] = {"float", "int", "bool", "string", "vec2", "vec3", "color",
                                           "vec4", "enum", "asset", "object"};
                pj.Set("type", p.type >= 0 && p.type < 11 ? typeNames[p.type] : "float");
                pj.Set("default", JsonToValue(env, p.defaultValue));
                props.Set(pi++, pj);
            }
            cj.Set("props", props);
            classes.Set(i++, cj);
        }
        o.Set("classes", classes);
        return o;
    }

    // -- play mode -------------------------------------------------------------
    Napi::Value Play(const Napi::CallbackInfo& info) {
        engine_->Play();
        return Napi::Boolean::New(info.Env(), true);
    }
    void Pause(const Napi::CallbackInfo& info) { engine_->Pause(); }
    void Resume(const Napi::CallbackInfo& info) { engine_->Resume(); }
    void Step(const Napi::CallbackInfo& info) { engine_->Step(); }
    Napi::Value Stop(const Napi::CallbackInfo& info) {
        engine_->Stop();
        return Napi::Boolean::New(info.Env(), true);
    }
    Napi::Value IsPlaying(const Napi::CallbackInfo& info) {
        return Napi::Boolean::New(info.Env(), engine_->isPlaying());
    }
    Napi::Value IsPaused(const Napi::CallbackInfo& info) {
        return Napi::Boolean::New(info.Env(), engine_->isPaused());
    }
    void Update(const Napi::CallbackInfo& info) { engine_->Update((float)GetNumber(info, 0, 0.016)); }
    void EndFrameInput(const Napi::CallbackInfo& info) { engine_->EndFrameInput(); }

    // -- input ---------------------------------------------------------------
    void KeyDown(const Napi::CallbackInfo& info) {
        int code = (int)GetNumber(info, 0, 0);
        if (code >= 0 && code < 512) engine_->input().keyDown[code] = true;
    }
    void KeyUp(const Napi::CallbackInfo& info) {
        int code = (int)GetNumber(info, 0, 0);
        if (code >= 0 && code < 512) engine_->input().keyDown[code] = false;
    }
    void MouseDown(const Napi::CallbackInfo& info) {
        int b = (int)GetNumber(info, 0, 0);
        if (b >= 0 && b < 8) engine_->input().mouseDown[b] = true;
    }
    void MouseUp(const Napi::CallbackInfo& info) {
        int b = (int)GetNumber(info, 0, 0);
        if (b >= 0 && b < 8) engine_->input().mouseDown[b] = false;
    }
    void MouseMove(const Napi::CallbackInfo& info) {
        engine_->input().mouseX = (float)GetNumber(info, 0);
        engine_->input().mouseY = (float)GetNumber(info, 1);
    }

    // -- picking / projection -------------------------------------------------
    Napi::Value Pick(const Napi::CallbackInfo& info) {
        EntityId id = engine_->Pick((int)GetNumber(info, 0), (int)GetNumber(info, 1));
        return Napi::Number::New(info.Env(), (double)id);
    }
    Napi::Value WorldToScreen(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        Vec3 p((float)GetNumber(info, 0), (float)GetNumber(info, 1), (float)GetNumber(info, 2));
        bool ok = false;
        Vec2 s = engine_->WorldToScreen(p, &ok);
        Napi::Object o = Napi::Object::New(env);
        o.Set("x", s.x);
        o.Set("y", s.y);
        o.Set("ok", ok);
        return o;
    }
    void SetHighlightEntity(const Napi::CallbackInfo& info) {
        engine_->SetHighlightEntity((EntityId)GetNumber(info, 0, 0));
    }
    Napi::Value DebugCollect(const Napi::CallbackInfo& info) {
        return JsonToValue(info.Env(), engine_->DebugCollectStats());
    }

    // -- console --------------------------------------------------------------
    Napi::Value PollLogs(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        auto entries = engine_->PollLogs(pollAfter_);
        Napi::Array arr = Napi::Array::New(env);
        uint32_t i = 0;
        for (const LogEntry& e : entries) {
            Napi::Object o = Napi::Object::New(env);
            o.Set("id", (double)e.id);
            o.Set("level", (int)e.level);
            o.Set("text", e.text);
            arr.Set(i++, o);
            pollAfter_ = e.id;
        }
        return arr;
    }
    Napi::Value PollAudio(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        Napi::Array arr = Napi::Array::New(env);
        uint32_t i = 0;
        for (const AudioEvent& e : engine_->audioEvents()) {
            Napi::Object o = Napi::Object::New(env);
            o.Set("clip", e.clip);
            o.Set("volume", e.volume);
            o.Set("pitch", e.pitch);
            arr.Set(i++, o);
        }
        engine_->audioEvents().clear();
        return arr;
    }

    void SetLogEcho(const Napi::CallbackInfo& info) {
        engine_->SetLogEcho(GetBool(info, 0, true));
    }
    Napi::Value GetLogEcho(const Napi::CallbackInfo& info) {
        return Napi::Boolean::New(info.Env(), engine_->logEcho());
    }

    std::unique_ptr<Engine> engine_;
    uint64_t pollAfter_ = 0;
};

Napi::FunctionReference EngineWrap::constructor;

Napi::Object InitModule(Napi::Env env, Napi::Object exports) {
    EngineWrap::Init(env, exports);
    exports.Set("engineVersion", Napi::String::New(env, "0.2.2"));
    return exports;
}

NODE_API_MODULE(cairn_engine_addon, InitModule)

}  // namespace
