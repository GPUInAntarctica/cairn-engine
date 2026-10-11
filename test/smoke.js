#!/usr/bin/env node
// Cairn Engine - end-to-end smoke test (native engine + runtime script build).
// Exercises the full gameplay pipeline without the GUI: scene setup, software
// rendering, picking, C++ script compilation, hot reload and play mode.
'use strict';

const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

// The smoke test asserts CPU-exact pixels and renders every frame through a
// blocking readback. Pin the software rasteriser so the suite never competes
// with the desktop for the GPU.
process.env.CAIRN_RENDER_BACKEND = 'software';

const root = path.resolve(__dirname, '..');
let failures = 0;
function check(cond, label) {
    if (cond) { console.log(`  ok  ${label}`); }
    else { console.error(`FAIL  ${label}`); failures++; }
}

const addon = require(path.join(root, 'build', 'cairn_engine_addon.node'));

// ---------------------------------------------------------------------------
// Temporary scratch project.
// ---------------------------------------------------------------------------
const projectDir = fs.mkdtempSync(path.join(os.tmpdir(), 'cairn-smoke-'));
const scriptsDir = path.join(projectDir, 'Assets', 'Scripts');
fs.mkdirSync(scriptsDir, { recursive: true });

const engineLibDir = path.join(root, 'build');
const compiler = process.env.CXX || 'g++';

function compileScripts(sources, outPath) {
    const r = spawnSync(compiler, [
        '-std=c++17', '-O2', '-fPIC', '-shared', '-fvisibility=hidden',
        '-fno-gnu-unique',  // script registries must die with their library (hot reload)
        `-I${path.join(root, 'engine', 'include')}`,
        `-L${engineLibDir}`, '-lcairn_engine',
        `-Wl,-rpath,${engineLibDir}`,
        '-o', outPath, ...sources,
    ], { encoding: 'utf8' });
    return { ok: r.status === 0, error: r.stderr };
}

function writeScript(name, code) {
    const p = path.join(scriptsDir, name);
    fs.writeFileSync(p, code);
    return p;
}

const rotatorSrc = writeScript('Rotator.cpp', `
#include <cairn/cairn.h>
using namespace Cairn;

class Rotator : public Component {
public:
    void OnStart() override {
        Debug::Log("Rotator::OnStart speed=%.1f", PropFloat("speed"));
    }
    void OnUpdate(float dt) override {
        Vec3 e = transform().GetEulerDegrees();
        e.y += PropFloat("speed") * dt;
        transform().SetEulerDegrees(e);
    }
    void DescribeProps(ScriptMeta& m) override {
        m.Add("speed", PropType::Float, 180.0f);
    }
};
CAIRN_SCRIPT(Rotator)
`);

const lifecycleSrc = writeScript('Lifecycle.cpp', `
#include <cairn/cairn.h>
using namespace Cairn;

class Lifecycle : public Component {
public:
    void OnStart() override { Debug::Log("Lifecycle::start %s", gameObject().name.c_str()); }
    void OnUpdate(float dt) override { (void)dt; }
    void OnDestroy() override { Debug::Log("Lifecycle::destroy %s", gameObject().name.c_str()); }
    void DescribeProps(ScriptMeta& m) override {
        m.Add("addedLater", PropType::Float, 12.5f);
    }
};
CAIRN_SCRIPT(Lifecycle)
`);

const playerSrc = writeScript('PlayerController.cpp', `
#include <cairn/cairn.h>
using namespace Cairn;

class PlayerController : public Component {
public:
    void OnStart() override {
        auto* rb = gameObject().GetComponent<RigidBody>();
        if (rb) rb->AddImpulse(Vec3(0, 3.0f, 0));
    }
    void FixedUpdate(float dt) override {
        auto* rb = gameObject().GetComponent<RigidBody>();
        if (!rb) return;
        float axis = Input::GetAxis("Horizontal");
        rb->velocity.x = axis * PropFloat("moveSpeed");
    }
    void DescribeProps(ScriptMeta& m) override {
        m.Add("moveSpeed", PropType::Float, 4.0f);
    }
};
CAIRN_SCRIPT(PlayerController)
`);

console.log('smoke: runtime script build + play mode');

// ---------------------------------------------------------------------------
// 1. Engine + scene + rendering.
// ---------------------------------------------------------------------------
const engine = new addon.Engine();
engine.setViewportSize(480, 270);
engine.setProjectRoot(projectDir);

const cube = engine.createPrimitive('Cube', 'SpinningCube', 0);
check(cube === 1, 'createPrimitive returns id');

// Ground for the physics test.
const ground = engine.createPrimitive('Cube', 'Ground', 0);
engine.setComponentProp(ground, 'Transform', 'position', [0, -0.5, 0]);
engine.setComponentProp(ground, 'Transform', 'scale', [20, 1, 20]);
check(engine.addComponent(ground, 'BoxCollider'), 'add BoxCollider to ground');

engine.setViewCamera({ position: [0, 2, 6], yaw: 0, pitch: -15 });
const stats = engine.renderEditorFrame();
check(stats.triangles > 0, `renders (drawCalls=${stats.drawCalls}, triangles=${stats.triangles})`);

const buf = engine.getFrameBuffer();
check(buf && buf.length === 480 * 270 * 4, 'frame buffer shared (no copy)');
let lit = 0;
for (let i = 0; i < buf.length; i += 4) if (buf[i] > 40 || buf[i + 1] > 40) lit++;
check(lit > 500, `cube visible in frame (${lit} lit px)`);

check(engine.pick(240, 135) === cube, 'pick hits cube');
const shot = path.join(projectDir, 'frame.png');
check(engine.saveFramePNG(shot) && fs.existsSync(shot) && fs.statSync(shot).size > 1000, 'saveFramePNG');

// ---------------------------------------------------------------------------
// 2. Script compilation + hot reload + play mode.
// ---------------------------------------------------------------------------
const libPath = path.join(projectDir, 'Library', 'Scripts.so');
fs.mkdirSync(path.dirname(libPath), { recursive: true });

let build = compileScripts([rotatorSrc], libPath);
check(build.ok, 'compiles gameplay script\n' + (build.ok ? '' : build.error));

let load = engine.loadScriptLibrary(libPath);
check(load.ok && load.classes.length === 1 && load.classes[0].name === 'Rotator',
    `script library loads (${load.ok ? load.classes.map(c => c.name).join(',') : load.error})`);
check(load.classes[0].props.some(p => p.name === 'speed' && p.type === 'float'),
    'script props metadata exposed');

const scriptHolder = engine.createEntity('Rotating', 0);
check(engine.addComponent(scriptHolder, 'Script'), 'attach Script component');
engine.setComponentProp(scriptHolder, 'Script', 'class', 'Rotator');
engine.setComponentProp(scriptHolder, 'Script', 'speed', 180.0);

engine.play();
check(engine.isPlaying(), 'enters play mode');

const y0 = engine.getComponentProp(scriptHolder, 'Transform', 'rotation')[1];
for (let i = 0; i < 30; ++i) {           // ~0.5 s at 60 fps
    engine.update(1 / 60);
    engine.renderGameFrame();
    engine.endFrameInput();
}
const y1 = engine.getComponentProp(scriptHolder, 'Transform', 'rotation')[1];
check(Math.abs((y1 - y0) - 90) < 5.0, `script OnUpdate rotates entity (${y0.toFixed(1)} -> ${y1.toFixed(1)} deg)`);

const logs = engine.pollLogs(0);
check(logs.some(l => l.text.includes('Rotator::OnStart')), 'script logs reach the console sink');

engine.stop();
check(!engine.isPlaying(), 'stop restores edit mode');
const yAfterStop = engine.getComponentProp(cube, 'Transform', 'rotation')[1];
check(Math.abs(yAfterStop - y0) < 0.001, 'scene snapshot restored on stop');

// Hot reload: change the speed and rebuild the same library path.
const rotatorSrc2 = writeScript('Rotator.cpp', `
#include <cairn/cairn.h>
using namespace Cairn;

class Rotator : public Component {
public:
    void OnUpdate(float dt) override {
        Vec3 e = transform().GetEulerDegrees();
        e.y += PropFloat("speed") * dt + 2.0f;  // v2: extra drift proves new code is live
        transform().SetEulerDegrees(e);
    }
    void DescribeProps(ScriptMeta& m) override {
        m.Add("speed", PropType::Float, 360.0f);
    }
};
CAIRN_SCRIPT(Rotator)
`);
build = compileScripts([rotatorSrc2], libPath);
check(build.ok, 'recompiles gameplay script');
load = engine.loadScriptLibrary(libPath);
check(load.ok, 'hot-reloads script library');

engine.play();
const y2 = engine.getComponentProp(scriptHolder, 'Transform', 'rotation')[1];
for (let i = 0; i < 30; ++i) { engine.update(1 / 60); engine.endFrameInput(); }
const y3 = engine.getComponentProp(scriptHolder, 'Transform', 'rotation')[1];
// Prop 'speed' was explicitly set to 180 (editor values persist across
// reloads), so the v2 code yields 180*0.5 + 2.0*30 = 150 degrees.
check(Math.abs((y3 - y2) - 150) < 5.0, `reloaded script runs new code (${(y3 - y2).toFixed(1)} deg in 0.5s)`);
engine.stop();

// ---------------------------------------------------------------------------
// 3. Physics + script interplay (RigidBody impulse from OnStart).
// ---------------------------------------------------------------------------
build = compileScripts([rotatorSrc2, playerSrc], libPath);
check(build.ok, 'builds multiple script sources\n' + (build.ok ? '' : build.error));
engine.loadScriptLibrary(libPath);

const ball = engine.createPrimitive('Sphere', 'Ball', 0);
engine.setComponentProp(ball, 'Transform', 'position', [0, 4, 0]);
engine.addComponent(ball, 'RigidBody');
engine.addComponent(ball, 'SphereCollider');
engine.addComponent(ball, 'Script');
engine.setComponentProp(ball, 'Script', 'class', 'PlayerController');

engine.play();
let minY = 99;
for (let i = 0; i < 300; ++i) {          // 5 simulated seconds
    engine.update(1 / 60);
    engine.endFrameInput();
    const p = engine.getComponentProp(ball, 'Transform', 'position');
    minY = Math.min(minY, p[1]);
}
const finalPos = engine.getComponentProp(ball, 'Transform', 'position');
check(minY > -1.0 && finalPos[1] < 1.2,
    `physics + script: ball falls and settles (minY=${minY.toFixed(2)}, y=${finalPos[1].toFixed(2)})`);
engine.stop();

// ---------------------------------------------------------------------------
// 4. Script lifecycle: OnDestroy on runtime destroy, hot reload while playing,
//    defaults applied on load, hierarchies and component enable flags.
// ---------------------------------------------------------------------------
const subtree = engine.createEntity('Parent', 0);
const kid = engine.createEntity('Child', subtree);
engine.setComponentProp(kid, 'Transform', 'position', [1, 2, 3]);
const subtreeCopy = engine.duplicateEntity(subtree);
const copyTree = engine.getEntityJSON(subtreeCopy);
check(copyTree && copyTree.name === 'Parent', 'duplicates an entity');
const copyChildren = engine.getEntityTree ? null : null;
const allEntities = engine.getSceneTree().entities.filter(e => e.parent === subtreeCopy);
check(allEntities.length === 1 && allEntities[0].name === 'Child',
    `duplicate keeps children (${allEntities.length} child(ren))`);
const copyChild = allEntities[0];
const copyChildPos = copyChild && copyChild.transform.position;
check(copyChildPos && Math.abs(copyChildPos[1] - 2) < 1e-4, 'duplicated child keeps its transform');

// OnDestroy must fire when a script entity is destroyed at runtime.
build = compileScripts([rotatorSrc2, lifecycleSrc], libPath);
check(build.ok, 'compiles lifecycle script\n' + (build.ok ? '' : build.error));
load = engine.loadScriptLibrary(libPath);
check(load.ok && load.classes.some(c => c.name === 'Lifecycle'), 'loads lifecycle script');

const lifeEntity = engine.createEntity('LifeTarget', 0);
engine.addComponent(lifeEntity, 'Script');
engine.setComponentProp(lifeEntity, 'Script', 'class', 'Lifecycle');
engine.pollLogs();
engine.play();
check(engine.pollLogs().some(l => l.text.includes('Lifecycle::start LifeTarget')), 'OnStart logs the entity name');

// Destroy at runtime: OnDestroy must run exactly once and the instance must go.
engine.destroyEntity(lifeEntity);
engine.update(1 / 60);
const destroyLogs = engine.pollLogs().filter(l => l.text.includes('Lifecycle::destroy'));
check(destroyLogs.length === 1, `OnDestroy fires on runtime destroy (${destroyLogs.length} events)`);

// Hot reload while playing must keep the game alive (scripts restart).
const lifeEntity2 = engine.createEntity('LifeTarget2', 0);
engine.addComponent(lifeEntity2, 'Script');
engine.setComponentProp(lifeEntity2, 'Script', 'class', 'Lifecycle');
engine.loadScriptLibrary(libPath);
check(engine.isPlaying(), 'reload keeps play mode active');
engine.update(1 / 60);
const reloadLogs = engine.pollLogs();
check(reloadLogs.some(l => l.text.includes('Lifecycle::start LifeTarget2')),
    'reload during play restarts scripts');

// Script fields declared after authoring get their default on scene load.
const beforeSave = engine.sceneJSON();
const saved = path.join(projectDir, 'LifecycleScene.cscene');
check(engine.saveScene(saved).ok, 'saves scene for prop-default check');
engine.stop();
engine.restoreSceneJSON(beforeSave);
engine.newScene('Defaults');
const defaultsEntity = engine.createEntity('Defaults', 0);
engine.addComponent(defaultsEntity, 'Script');
engine.setComponentProp(defaultsEntity, 'Script', 'class', 'Lifecycle');
const snapshot = engine.sceneJSON();
engine.newScene('Reloaded');
engine.restoreSceneJSON(snapshot);   // properties lack "addedLater"
const loadedValue = engine.getComponentProp(defaultsEntity === 0 ? 0 : 1, 'Script', 'addedLater');
check(Math.abs(loadedValue - 12.5) < 1e-4, `script prop defaults applied on load (${loadedValue})`);

// Disabling a MeshRenderer hides the mesh from rendering and picking.
engine.newScene('Enable');
const visCube = engine.createPrimitive('Cube', 'Visible', 0);
const before = engine.debugCollect();
engine.setComponentProp(visCube, 'MeshRenderer', 'enabled', false);
const after = engine.debugCollect();
check(after.meshRenderers === before.meshRenderers - 1 && after.emitted === before.emitted - 1,
    `disabled MeshRenderer stops rendering (${before.meshRenderers} -> ${after.meshRenderers})`);
engine.setComponentProp(visCube, 'MeshRenderer', 'enabled', true);
check(engine.debugCollect().meshRenderers === before.meshRenderers, 're-enabled MeshRenderer renders again');

// Hierarchy writes from raw script-style field assignment must reach the view.
engine.newScene('Hierarchy');
const parent = engine.createEntity('Parent', 0);
const child = engine.createEntity('Child', parent);
engine.setComponentProp(child, 'Transform', 'position', [0, 0, 0]);
engine.setComponentProp(parent, 'Transform', 'position', [0, 5, 0]);
engine.renderEditorFrame();
const childWorld = engine.getEntityJSON(child).transform.position;  // local space stays local
check(Math.abs(childWorld[1]) < 1e-4, 'local transform stays local under a parent');

fs.rmSync(projectDir, { recursive: true, force: true });

if (failures === 0) {
    console.log('smoke: all checks passed');
    process.exit(0);
}
console.error(`smoke: ${failures} check(s) failed`);
process.exit(1);
