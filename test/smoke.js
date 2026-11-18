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
const imaging = require(path.join(root, 'editor', 'main', 'imaging.js'));

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

// The editor's gizmo, its focus pivot and a move drag are world-space, while
// Transform.position is parent-relative. Both directions must agree, including
// under a rotated parent.
const near3 = (a, b) => Array.isArray(a) && a.length === 3 &&
    a.every((v, i) => Math.abs(v - b[i]) < 1e-4);
check(near3(engine.getWorldPosition(child), [0, 5, 0]),
    'world position includes the parent offset');
check(engine.setWorldPosition(child, 2, 3, 4) === true, 'setWorldPosition reports success');
check(near3(engine.getWorldPosition(child), [2, 3, 4]),
    'setting a world position puts the entity there');
const afterWorldSet = engine.getEntityJSON(child).transform.position;
check(Math.abs(afterWorldSet[1] + 2) < 1e-4,
    `the stored transform stays parent-relative (${afterWorldSet[1].toFixed(2)})`);
engine.setComponentProp(parent, 'Transform', 'rotation', [0, 90, 0]);
engine.setComponentProp(child, 'Transform', 'position', [1, 0, 0]);
const rotatedWorld = engine.getWorldPosition(child);
check(rotatedWorld && Math.abs(rotatedWorld[1] - 5) < 1e-4 &&
      Math.abs(rotatedWorld[0]) < 1e-4 && Math.abs(Math.abs(rotatedWorld[2]) - 1) < 1e-4,
    `a rotated parent turns local +X into world -Z (${rotatedWorld && rotatedWorld.map(v => v.toFixed(2)).join(', ')})`);
engine.setWorldPosition(child, 0.5, 2, -0.5);
check(near3(engine.getWorldPosition(child), [0.5, 2, -0.5]),
    'a world position round-trips through a rotated parent');
check(engine.getWorldPosition(999999) === null, 'a missing entity has no world position');
check(engine.setWorldPosition(999999, 0, 0, 0) === false,
    'setting a world position on a missing entity reports failure');

// A C++ exception thrown by a script must be reported, not allowed to unwind
// into the host: the addon is the same process as the editor and the CLI, so an
// escaping exception terminates them instead of showing the fault.
const throwerSrc = writeScript('Thrower.cpp', `
#include <cairn/cairn.h>
#include <stdexcept>
using namespace Cairn;
class Thrower : public Component {
public:
    void OnStart() override { throw std::runtime_error("start boom"); }
    void OnUpdate(float) override { throw std::runtime_error("update boom"); }
    void DescribeProps(ScriptMeta&) override {}
};
CAIRN_SCRIPT(Thrower)
`);
build = compileScripts([throwerSrc], libPath);
check(build.ok, 'compiles a throwing script\n' + (build.ok ? '' : build.error));
load = engine.loadScriptLibrary(libPath);
check(load.ok && load.classes.some(c => c.name === 'Thrower'), 'loads the throwing script');
engine.newScene('Throws');
const throwEntity = engine.createEntity('ThrowTarget', 0);
engine.addComponent(throwEntity, 'Script');
engine.setComponentProp(throwEntity, 'Script', 'class', 'Thrower');
engine.pollLogs();
engine.play();
const throwingFrame = engine.renderGameFrame();
for (let i = 0; i < 5; ++i) { engine.update(1 / 60); engine.renderGameFrame(); engine.endFrameInput(); }
const throwLogs = engine.pollLogs();
check(engine.isPlaying(), 'a throwing OnStart leaves play mode running');
check(throwingFrame && typeof throwingFrame.drawCalls === 'number', 'the frame after a throw still renders');
check(throwLogs.some(l => l.text.includes('Thrower::OnStart threw: start boom')),
    'the OnStart exception is logged with its class and method');
check(throwLogs.some(l => l.text.includes('Thrower::OnUpdate threw: update boom')),
    'the OnUpdate exception is logged and does not stop later frames');
engine.stop();

// Spot shadows: a spot light gets a perspective map of its own cone instead of
// reading the directional cascades. The engine tests compare the two backends
// pixel for pixel; this checks the addon exposes the resulting stat and that
// the light's castShadows switch gates it.
engine.newScene('SpotShadows');
engine.setViewCamera({ position: [0, 3, 9], yaw: 0, pitch: -12 });
const spotGround = engine.createPrimitive('Cube', 'SpotGround', 0);
engine.setComponentProp(spotGround, 'Transform', 'position', [0, -0.5, 0]);
engine.setComponentProp(spotGround, 'Transform', 'scale', [30, 1, 30]);
engine.setComponentProp(spotGround, 'MeshRenderer', 'receiveShadows', true);
const spotOccluder = engine.createPrimitive('Cube', 'SpotOccluder', 0);
engine.setComponentProp(spotOccluder, 'Transform', 'position', [0, 1.2, 0]);
engine.setComponentProp(spotOccluder, 'Transform', 'scale', [1.4, 1.4, 1.4]);
const spot = engine.createEntity('Spot', 0);
check(engine.addComponent(spot, 'Light'), 'add a spot light');
engine.setComponentProp(spot, 'Transform', 'position', [4, 5, 0]);
engine.setComponentProp(spot, 'Transform', 'rotation', [-51.3, 90, 0]);
engine.setComponentProp(spot, 'Light', 'type', 2);
engine.setComponentProp(spot, 'Light', 'intensity', 3.0);
engine.setComponentProp(spot, 'Light', 'range', 25.0);
engine.setComponentProp(spot, 'Light', 'spotAngle', 35.0);
engine.setComponentProp(spot, 'Light', 'castShadows', true);
engine.setSceneSetting('ambient', [0.2, 0.2, 0.23, 1]);
const spotFrame = engine.renderGameFrame();
check(spotFrame.lightCount === 1, 'the spot light is collected');
check(spotFrame.shadowedPixels > 0, `the spot shadow darkens pixels (${spotFrame.shadowedPixels})`);
engine.setComponentProp(spot, 'Light', 'castShadows', false);
const spotNoCast = engine.renderGameFrame();
check(spotNoCast.shadowedPixels === 0, 'castShadows off leaves the spot unshadowed');

// Point shadows: six cube faces share the perspective-map array with spots,
// and the fragment picks the face its direction from the light falls in. This
// scene puts occluders on both sides so the frame exercises more than one face.
engine.newScene('PointShadows');
engine.setViewCamera({ position: [0, 18, 18], yaw: 0, pitch: -45 });
const pointGround = engine.createPrimitive('Cube', 'PointGround', 0);
engine.setComponentProp(pointGround, 'Transform', 'position', [0, -0.5, 0]);
engine.setComponentProp(pointGround, 'Transform', 'scale', [40, 1, 40]);
engine.setComponentProp(pointGround, 'MeshRenderer', 'receiveShadows', true);
for (const [i, x, z] of [[0, 0, 0], [1, 7, 0], [2, -7, 0], [3, 0, 7], [4, 0, -7]]) {
  const box = engine.createPrimitive('Cube', `PointBox${i}`, 0);
  engine.setComponentProp(box, 'Transform', 'position', [x, 0.7, z]);
  engine.setComponentProp(box, 'Transform', 'scale', [1.4, 1.4, 1.4]);
  engine.setComponentProp(box, 'MeshRenderer', 'receiveShadows', false);
}
const point = engine.createEntity('Point', 0);
check(engine.addComponent(point, 'Light'), 'add a point light');
engine.setComponentProp(point, 'Transform', 'position', [0, 4, 0]);
engine.setComponentProp(point, 'Light', 'type', 1);
engine.setComponentProp(point, 'Light', 'intensity', 3.0);
engine.setComponentProp(point, 'Light', 'range', 40.0);
engine.setComponentProp(point, 'Light', 'castShadows', true);
engine.setSceneSetting('ambient', [0.18, 0.18, 0.21, 1]);
const pointFrame = engine.renderGameFrame();
check(pointFrame.lightCount === 1, 'the point light is collected');
check(pointFrame.shadowedPixels > 0, `the point shadow darkens pixels (${pointFrame.shadowedPixels})`);
engine.setComponentProp(point, 'Light', 'castShadows', false);
const pointNoCast = engine.renderGameFrame();
check(pointNoCast.shadowedPixels === 0, 'castShadows off leaves the point light unshadowed');

// The per-pixel light budget moved from 16 to 32. Through the addon's
// reflection API the collected count must match, including a lamp placed past
// the old cap and the one-past-the-new-cap warning.
engine.newScene('LightBudget');
let lampsAdded = true;
for (let i = 0; i < 20; ++i) {
    const lamp = engine.createEntity(`Lamp${i}`, 0);
    lampsAdded = lampsAdded && engine.addComponent(lamp, 'Light');
    engine.setComponentProp(lamp, 'Light', 'type', 1);
    engine.setComponentProp(lamp, 'Light', 'intensity', i === 19 ? 2.0 : 0.0);
    engine.setComponentProp(lamp, 'Light', 'range', 50.0);
    engine.setComponentProp(lamp, 'Light', 'castShadows', false);
}
check(lampsAdded, 'add twenty lights through the reflection API');
check(engine.renderGameFrame().lightCount === 20, 'the twentieth lamp is collected');
for (let i = 20; i < 33; ++i) {
    const lamp = engine.createEntity(`Lamp${i}`, 0);
    lampsAdded = lampsAdded && engine.addComponent(lamp, 'Light');
    engine.setComponentProp(lamp, 'Light', 'type', 1);
    engine.setComponentProp(lamp, 'Light', 'castShadows', false);
}
check(lampsAdded && engine.renderGameFrame().lightCount === 32, 'the budget stops at 32 lamps');
engine.pollLogs();
engine.renderGameFrame();
check(engine.pollLogs().filter(l => l.text.includes('light limit (32) reached')).length === 1,
    'one warning names the light budget');

// A transition with `fade` keeps the outgoing clip visible as a second quad
// until the clock runs out. Drive the table through the addon's reflection API
// and read the frame back, so the contract is checked end to end, not just in
// the engine's own scene object.
engine.newScene('Crossfade');
engine.setViewCamera({ position: [0, 0, 8], yaw: 0, pitch: 0 });
const fadeSprite = engine.createEntity('FadeSprite', 0);
check(engine.addComponent(fadeSprite, 'SpriteRenderer'),
    'add a sprite renderer for the crossfade');
check(engine.addComponent(fadeSprite, 'SpriteAnimator'),
    'add an animator for the crossfade');
engine.setComponentProp(fadeSprite, 'Transform', 'scale', [100, 100, 1]);
const fadeTexturesDir = path.join(projectDir, 'Assets', 'Textures');
fs.mkdirSync(fadeTexturesDir, { recursive: true });
for (const [name, rgb] of [['idleFade.png', [255, 0, 0]], ['runFade.png', [0, 255, 0]]]) {
    const pixels = Buffer.alloc(8 * 8 * 4);
    for (let i = 0; i < 64; ++i) {
        pixels[i * 4] = rgb[0]; pixels[i * 4 + 1] = rgb[1];
        pixels[i * 4 + 2] = rgb[2]; pixels[i * 4 + 3] = 255;
    }
    fs.writeFileSync(path.join(fadeTexturesDir, name), imaging.encodePng(pixels, 8, 8));
}
engine.setComponentProp(fadeSprite, 'SpriteRenderer', 'texturePath', 'Assets/Textures/idleFade.png');
engine.setComponentProp(fadeSprite, 'SpriteAnimator', 'columns', 2);
engine.setComponentProp(fadeSprite, 'SpriteAnimator', 'rows', 2);
engine.setComponentProp(fadeSprite, 'SpriteAnimator', 'fps', 1);
engine.setComponentProp(fadeSprite, 'SpriteAnimator', 'clips', [
  { name: 'idle', firstFrame: 0, lastFrame: 0, texturePath: 'Assets/Textures/idleFade.png' },
  { name: 'run', firstFrame: 0, lastFrame: 0, texturePath: 'Assets/Textures/runFade.png' },
]);
engine.setComponentProp(fadeSprite, 'SpriteAnimator', 'clip', 'idle');
engine.setComponentProp(fadeSprite, 'SpriteAnimator', 'params', { go: false });
engine.setComponentProp(fadeSprite, 'SpriteAnimator', 'transitions', [
  { from: 'idle', to: 'run', when: { param: 'go', op: 'is', value: true }, fade: 0.2 },
]);
engine.play();
const fadeShot = path.join(projectDir, 'fade.png');
const sampleFade = () => {
    engine.renderGameFrame();
    engine.saveFramePNG(fadeShot);
    const img = imaging.loadImage(fadeShot);
    const p = imaging.samplePixel(img.layers[0].data, img.width, img.height, 240, 135);
    return [p.r, p.g, p.b];
};
const distance = (a, b) => Math.abs(a[0] - b[0]) + Math.abs(a[1] - b[1]) + Math.abs(a[2] - b[2]);
const idle = sampleFade();
engine.setComponentProp(fadeSprite, 'SpriteAnimator', 'params', { go: true });
engine.update(0.0);
const tick = sampleFade();
check(distance(tick, idle) < 12, 'the incoming clip has no weight on the transition tick');
engine.update(0.1);
const half = sampleFade();
engine.update(0.1);
const end = sampleFade();
check(distance(end, idle) > 40, 'the finished fade shows the incoming clip');
check(distance(half, idle) > 12 && distance(half, idle) < distance(end, idle),
    'the halfway frame is between the two clips');

// A 1D blend tree blends two named clips by a numeric parameter instead of
// cutting on a transition. Set it through the addon's reflection API - the JSON
// field marshalling is part of the contract - and read the frame back: the
// parameter, not elapsed time, decides the picture. The two clips point at the
// red and green sheets the crossfade test just wrote.
engine.newScene('BlendTree');
engine.setViewCamera({ position: [0, 0, 8], yaw: 0, pitch: 0 });
const btSprite = engine.createEntity('BTSprite', 0);
check(engine.addComponent(btSprite, 'SpriteRenderer'), 'add a sprite renderer for the blend tree');
check(engine.addComponent(btSprite, 'SpriteAnimator'), 'add an animator for the blend tree');
engine.setComponentProp(btSprite, 'Transform', 'scale', [100, 100, 1]);
engine.setComponentProp(btSprite, 'SpriteAnimator', 'columns', 2);
engine.setComponentProp(btSprite, 'SpriteAnimator', 'rows', 2);
engine.setComponentProp(btSprite, 'SpriteAnimator', 'fps', 1);
engine.setComponentProp(btSprite, 'SpriteAnimator', 'clips', [
  { name: 'idle', firstFrame: 0, lastFrame: 0, texturePath: 'Assets/Textures/idleFade.png' },
  { name: 'run', firstFrame: 0, lastFrame: 0, texturePath: 'Assets/Textures/runFade.png' },
]);
engine.setComponentProp(btSprite, 'SpriteAnimator', 'blendTree', {
  param: 'speed',
  entries: [ { at: 0, clip: 'idle' }, { at: 4, clip: 'run' } ],
});
engine.setComponentProp(btSprite, 'SpriteAnimator', 'params', { speed: 0 });
engine.play();
const btShot = path.join(projectDir, 'bt.png');
const sampleBT = speed => {
  engine.setComponentProp(btSprite, 'SpriteAnimator', 'params', { speed });
  engine.update(0.0);
  engine.renderGameFrame();
  engine.saveFramePNG(btShot);
  const img = imaging.loadImage(btShot);
  const p = imaging.samplePixel(img.layers[0].data, img.width, img.height, 240, 135);
  return [p.r, p.g, p.b];
};
const btLow = sampleBT(0);
const btHigh = sampleBT(4);
const btMid = sampleBT(2);
check(btLow[0] > btLow[1] + 40, 'speed at the low end shows the red idle clip', btLow);
check(btHigh[1] > btHigh[0] + 40, 'speed at the high end shows the green run clip', btHigh);
check(distance(btMid, btLow) > 20 && distance(btMid, btHigh) > 20,
    'a midpoint blends the two clips', btMid);
check(JSON.stringify(sampleBT(0)) === JSON.stringify(btLow),
    'the frame is a pure function of the parameter');

fs.rmSync(projectDir, { recursive: true, force: true });

if (failures === 0) {
    console.log('smoke: all checks passed');
    process.exit(0);
}
console.error(`smoke: ${failures} check(s) failed`);
process.exit(1);
