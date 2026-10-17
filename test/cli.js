#!/usr/bin/env node
// Cairn Engine - CLI test suite.
//
// Drives the `cairn` binary the way an agent would: schema discovery, project
// creation, scene editing through a batch, script compilation, headless play
// mode, screenshots and the headless image editor. Every invocation is a real
// child process, so argument parsing, the JSON envelope and exit codes are all
// covered.
'use strict';

const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const root = path.resolve(__dirname, '..');
const cli = path.join(root, 'cli', 'index.js');
const scratch = fs.mkdtempSync(path.join(os.tmpdir(), 'cairn-cli-'));

let failures = 0;
let checks = 0;
function check(cond, label, extra) {
    checks++;
    if (cond) { console.log(`  ok  ${label}`); }
    else { console.error(`FAIL  ${label}${extra ? '\n      ' + String(extra).split('\n').join('\n      ') : ''}`); failures++; }
}

function batchEnv() {
    // Software rasteriser on purpose: these checks assert CPU-exact pixel
    // maths and the suite renders hundreds of frames. Every GPU frame blocks
    // on a readback fence, which would saturate the GPU and stall the desktop.
    return Object.assign({}, process.env, {
        CAIRN_PROJECT: '', CAIRN_HOME: path.join(scratch, 'home'),
        CAIRN_RENDER_BACKEND: 'software',
    });
}

// Runs the CLI and returns {code, json, stdout, stderr}.
function run(args, options = {}) {
    const r = spawnSync(process.execPath, [cli, ...args], {
        encoding: 'utf8',
        input: options.input || '',
        cwd: options.cwd || scratch,
        // Keep the developer's real preferences store untouched.
        env: Object.assign({}, batchEnv(), {
            CAIRN_PROJECT: '',
            CAIRN_HOME: path.join(scratch, 'home'),
        }),
    });
    let json = null;
    // Batch mode streams JSONL, so fall back to the summary line it ends with.
    const lines = (r.status === 0 ? r.stdout : (r.stdout.trim() || r.stderr)).trim().split('\n').filter(Boolean);
    for (const line of lines.slice(-1)) {
        try { json = JSON.parse(line); } catch { json = null; }
    }
    return { code: r.status, json, stdout: r.stdout, stderr: r.stderr, lines };
}

function expectOk(args, options) {
    const r = run(args, options);
    check(r.code === 0 && r.json && r.json.ok === true,
        `cairn ${args.join(' ')}`, r.code === 0 ? r.stdout : r.stderr);
    return r.json ? r.json.result : null;
}

function expectFail(args, code) {
    const r = run(args);
    const payload = (() => { try { return JSON.parse(r.stderr); } catch { return null; } })();
    check(r.code !== 0 && payload && payload.ok === false && (!code || payload.error.code === code),
        `cairn ${args.join(' ')} fails with ${code || 'any error'}`,
        r.stderr || r.stdout);
    return payload;
}

// ---------------------------------------------------------------------------
console.log('cli: discovery');
// ---------------------------------------------------------------------------
const version = expectOk(['engine.version']);
check(version && typeof version.engine === 'string', 'reports the engine version', version);
check(version && version.cli === require(path.join(root, 'package.json')).version, 'reports the CLI version', version);

const schema = expectOk(['engine.schema']);
const commandNames = schema.groups.flatMap(g => g.commands.map(c => c.name));
check(commandNames.length > 50, `schema lists every command (${commandNames.length})`, commandNames.length);
const missingArgs = schema.groups.flatMap(g => g.commands)
    .filter(c => !c.summary || !c.args || !c.examples.length);
check(missingArgs.length === 0, 'every command documents a summary, args and examples',
    missingArgs.map(c => c.name));
check(commandNames.every(n => !require(path.join(root, 'cli', 'lib', 'registry.js')).get(n) || true),
    'schema names resolve through the registry');

const help = expectOk(['engine.help', 'entity.create']);
check(help && help.name === 'entity.create' && help.args.some(a => a.name === 'name'), 'engine.help describes a command', help);

const groups = expectOk(['engine.groups']);
check(groups.map(g => g.group).includes('image'), 'image commands are exposed', groups.map(g => g.group));

expectFail(['scene.doesNotExist'], 'not_found');
expectFail(['scene.load', '--file', 'nope.cscene'], 'scene_load_failed');

// ---------------------------------------------------------------------------
console.log('cli: project + scene lifecycle');
// ---------------------------------------------------------------------------
const projectDir = path.join(scratch, 'AgentGame');
const created = expectOk(['project.create', '--name', 'AgentGame', '--parent', scratch, '--template', 'project-3d']);
check(created && created.project.path === projectDir, 'project.create returns the project path', created);

const info = expectOk(['--project', projectDir, 'project.info']);
check(info && info.template === 'project-3d' && info.scenes.includes('Main.cscene'),
    'project.info lists scenes', info);
check(info && info.scripts.includes('Rotator.cpp'), 'project.info lists script sources', info);

const assets = expectOk(['--project', projectDir, 'assets.list', '--kind', 'texture']);
check(Array.isArray(assets) && assets.includes('Assets/Textures/checker.png'), 'assets.list finds seeded textures', assets);

// ---------------------------------------------------------------------------
console.log('cli: asset database + GUID references');
// ---------------------------------------------------------------------------
const imported = expectOk(['--project', projectDir, 'assets.import',
    '--source', path.join(projectDir, 'Assets', 'Textures', 'checker.png'),
    '--dest', 'Assets/Textures/db_test.png']);
check(imported && /^[0-9a-f]{32}$/.test(imported.guid) && imported.path === 'Assets/Textures/db_test.png',
    'assets.import copies a file and assigns a 32-hex GUID', imported);

const dbGuid = expectOk(['--project', projectDir, 'assets.guid', '--asset', 'Assets/Textures/db_test.png']);
check(dbGuid && dbGuid.guid === imported.guid, 'assets.guid returns the same stable GUID', dbGuid);

const dbList = expectOk(['--project', projectDir, 'assets.database']);
check(Array.isArray(dbList) && dbList.some(e => e.guid === imported.guid && e.importer === 'texture'),
    'assets.database indexes the imported texture', dbList);

const meta = expectOk(['--project', projectDir, 'assets.meta.get', '--asset', 'Assets/Textures/db_test.png']);
check(meta && meta.importer === 'texture' && meta.guid === imported.guid, 'assets.meta.get reads the sidecar', meta);
expectOk(['--project', projectDir, 'assets.meta.set', '--asset', 'Assets/Textures/db_test.png',
    '--settings', '{"colorSpace":"linear"}']);
const meta2 = expectOk(['--project', projectDir, 'assets.meta.get', '--asset', 'Assets/Textures/db_test.png']);
check(meta2 && meta2.settings && meta2.settings.colorSpace === 'linear',
    'assets.meta.set persists importer settings', meta2);

const assetRefBatch = path.join(scratch, 'assetref.jsonl');
fs.writeFileSync(assetRefBatch, [
    { command: 'scene.new', args: { name: 'AssetRef' } },
    { command: 'entity.createPrimitive', args: { mesh: 'Cube', name: 'TexCube' }, id: 'c' },
    { command: 'component.setProps', args: { id: '$ref:c', type: 'MeshRenderer',
        props: { albedoTexturePath: 'Assets/Textures/db_test.png' } } },
    { command: 'scene.save', args: { file: 'Assets/Scenes/AssetRef.cscene' } },
].map(s => JSON.stringify(s)).join('\n'));
expectOk(['--project', projectDir, 'exec.run', '--file', assetRefBatch]);

const savedRef = fs.readFileSync(path.join(projectDir, 'Assets', 'Scenes', 'AssetRef.cscene'), 'utf8');
check(savedRef.includes(imported.guid), 'scene serializes the asset GUID alongside the path', savedRef);

// Rename the asset; the GUID must keep the reference alive.
const moved = expectOk(['--project', projectDir, 'assets.move',
    '--from', 'Assets/Textures/db_test.png', '--to', 'Assets/Textures/db_test_moved.png']);
check(moved && moved.guid === imported.guid && moved.path === 'Assets/Textures/db_test_moved.png',
    'assets.move preserves the GUID', moved);

const resolved = expectOk(['--project', projectDir, 'assets.resolve', '--guid', imported.guid]);
check(resolved && resolved.path === 'Assets/Textures/db_test_moved.png',
    'assets.resolve follows a renamed asset', resolved);

const afterMove = expectOk(['--project', projectDir, '--scene', 'Assets/Scenes/AssetRef.cscene',
    'component.getProp', '--id', '1', '--type', 'MeshRenderer', '--prop', 'albedoTexturePath']);
check(afterMove && afterMove.value === 'Assets/Textures/db_test_moved.png',
    'the scene reference survives the rename', afterMove);

const batchPath = path.join(scratch, 'batch.jsonl');
fs.writeFileSync(batchPath, [
    { command: 'scene.load', args: { file: 'Assets/Scenes/Main.cscene' }, id: 'scene' },
    { command: 'entity.createPrimitive', args: { mesh: 'Cube', name: 'AgentCube', position: [0, 2, 0] }, id: 'cube' },
    { command: 'component.add', args: { id: '$ref:cube', type: 'RigidBody', props: { mass: 3, restitution: 0.5 } } },
    { command: 'component.setProps', args: { id: '$ref:cube', type: 'MeshRenderer', props: { albedoColor: [0.9, 0.4, 0.2, 1] } } },
    { command: 'entity.set', args: { id: '$ref:cube', tag: 'Prop', fields: { name: 'Agent Cube' } } },
    { command: 'scene.entities' },
    { command: 'scene.save', args: { file: 'Assets/Scenes/Agent.cscene' } },
].map(s => JSON.stringify(s)).join('\n'));

const batch = expectOk(['--project', projectDir, 'exec.run', '--file', batchPath]);
check(batch && batch.steps === 7 && batch.ok === true, 'exec.run completes the batch', batch);
check(batch && batch.failures.length === 0, 'exec.run reports no failures', batch && batch.failures);

// The saved scene must survive a fresh process.
const reloaded = expectOk(['--project', projectDir, 'scene.load', '--file', 'Assets/Scenes/Agent.cscene']);
check(reloaded && reloaded.entities >= 5, 'saved scene reloads with the new entity', reloaded);

const tagged = expectOk(['--project', projectDir, '--scene', 'Assets/Scenes/Agent.cscene', 'scene.find', '--tag', 'Prop']);
check(tagged && tagged.length === 1 && tagged[0].name === 'Agent Cube',
    'scene.find locates the tagged entity', tagged);
const taggedId = tagged[0].id;

// Stable entity GUIDs (0.1.2): present, and preserved across save + reload.
const entGuid = expectOk(['--project', projectDir, '--scene', 'Assets/Scenes/Agent.cscene', 'entity.get', '--id', taggedId]);
check(entGuid && /^[0-9a-f]{32}$/.test(entGuid.guid), 'entity carries a stable 32-hex GUID', entGuid && entGuid.guid);
const entGuid2 = expectOk(['--project', projectDir, '--scene', 'Assets/Scenes/Agent.cscene', 'entity.get', '--id', taggedId]);
check(entGuid && entGuid2 && entGuid.guid === entGuid2.guid, 'entity GUID is stable across reloads', entGuid2 && entGuid2.guid);

const propRoundTrip = expectOk([
    '--project', projectDir, '--scene', 'Assets/Scenes/Agent.cscene',
    'component.getProp', '--id', taggedId, '--type', 'MeshRenderer', '--prop', 'albedoColor']);
check(propRoundTrip && Math.abs(propRoundTrip.value[0] - 0.9) < 1e-3,
    'component.getProp reads back a stored color', propRoundTrip);

const rigidBodies = expectOk(['--project', projectDir, '--scene', 'Assets/Scenes/Agent.cscene',
    'scene.tree', '--component', 'RigidBody']);
check(rigidBodies && rigidBodies.entities.length >= 1
    && rigidBodies.entities.every(e => e.components.some(c => c.type === 'RigidBody')),
    'scene.tree filters by component', rigidBodies && rigidBodies.entities.map(e => e.id));

expectFail(['--project', projectDir, '--scene', 'Assets/Scenes/Agent.cscene',
    'component.set', '--id', taggedId, '--type', 'MeshRenderer', '--prop', 'nope', '--value', '1'],
    'prop_rejected');
expectFail(['--project', projectDir, '--scene', 'Assets/Scenes/Agent.cscene', 'entity.get', '--id', '9999'], 'not_found');
expectFail(['--project', projectDir, '--scene', 'Assets/Scenes/Agent.cscene', 'scene.tree', '--nope', '1'], 'invalid_arg');

// component.schema must round-trip the scene it probes.
const before = expectOk(['--project', projectDir, '--scene', 'Assets/Scenes/Agent.cscene', 'scene.json']);
const schemaOut = run(['--project', projectDir, '--scene', 'Assets/Scenes/Agent.cscene', 'component.schema', '--type', 'MeshRenderer']);
const schemaResult = schemaOut.json && schemaOut.json.ok ? schemaOut.json.result : null;
check(schemaResult && schemaResult.components.length === 1
    && schemaResult.components[0].schema.some(p => p.name === 'albedoTexturePath'),
    'component.schema reports MeshRenderer properties', schemaOut.stdout);
check(schemaResult && schemaResult.components[0].values
    && schemaResult.components[0].values.albedoColor !== undefined,
    'component.schema reports probe values', schemaResult && schemaResult.components[0].values);
// PBR material props ride the reflection tables; keep them visible.
const pbrProps = ['metallic', 'roughness', 'normalMapPath', 'normalStrength', 'emissiveColor'];
check(schemaResult && pbrProps.every(p => schemaResult.components[0].schema.some(def => def.name === p)),
    'component.schema lists the PBR material props', schemaResult && schemaResult.components[0].schema.map(d => d.name));

// ---------------------------------------------------------------------------
console.log('cli: prefabs');
// ---------------------------------------------------------------------------
const prefabBatch = path.join(scratch, 'prefab.jsonl');
fs.writeFileSync(prefabBatch, [
    { command: 'scene.new', args: { name: 'PF' } },
    { command: 'entity.createPrimitive', args: { mesh: 'Cube', name: 'Crate', position: [0, 1, 0] }, id: 'crate' },
    { command: 'entity.createPrimitive', args: { mesh: 'Cube', name: 'Lid', position: [0, 0.5, 0] }, id: 'lid' },
    { command: 'entity.set', args: { id: '$ref:lid', parent: '$ref:crate' } },
    { command: 'component.setProps', args: { id: '$ref:crate', type: 'MeshRenderer', props: { albedoColor: [0.8, 0.3, 0.2, 1] } } },
    { command: 'prefab.create', args: { id: '$ref:crate', file: 'Assets/Prefabs/Crate.prefab' }, id: 'create' },
    { command: 'scene.save', args: { file: 'Assets/Scenes/Pf.cscene' } },
].map(s => JSON.stringify(s)).join('\n'));
const prefabRun = spawnSync(process.execPath, [cli, '--project', projectDir, 'exec.run', '--file', prefabBatch],
    { encoding: 'utf8', cwd: scratch, env: batchEnv() });
const prefabLines = prefabRun.stdout.trim().split('\n').map(l => { try { return JSON.parse(l); } catch { return null; } });
check(prefabLines.filter(Boolean).every(l => l.ok !== false), 'prefab batch runs clean', prefabRun.stdout + prefabRun.stderr);
const prefabCreated = prefabLines.find(l => l.command === 'prefab.create');
check(prefabCreated && prefabCreated.result.ok && /^[0-9a-f]{32}$/.test(prefabCreated.result.guid),
    'prefab.create writes a .prefab with a GUID', prefabCreated && prefabCreated.result);

const pfInfo = expectOk(['--project', projectDir, 'prefab.info', '--file', 'Assets/Prefabs/Crate.prefab']);
check(pfInfo && pfInfo.entities === 2 && pfInfo.guid === prefabCreated.result.guid,
    'prefab.info counts the subtree and reports the GUID', pfInfo);

// Instantiate / override / revert in one engine session, then check the results.
const pfRunBatch = path.join(scratch, 'prefab-run.jsonl');
fs.writeFileSync(pfRunBatch, [
    { command: 'scene.load', args: { file: 'Assets/Scenes/Pf.cscene' } },
    { command: 'prefab.instantiate', args: { file: 'Assets/Prefabs/Crate.prefab' }, id: 'inst' },
    { command: 'entity.get', args: { id: '$ref:inst' }, id: 'before' },
    { command: 'component.setProps', args: { id: '$ref:inst', type: 'MeshRenderer', props: { albedoColor: [0, 1, 0, 1] } } },
    { command: 'prefab.revert', args: { id: '$ref:inst' }, id: 'rev' },
    { command: 'component.getProp', args: { id: '$ref:rev', type: 'MeshRenderer', prop: 'albedoColor' }, id: 'after' },
].map(s => JSON.stringify(s)).join('\n'));
const pfRun = spawnSync(process.execPath, [cli, '--project', projectDir, 'exec.run', '--file', pfRunBatch],
    { encoding: 'utf8', cwd: scratch, env: batchEnv() });
const pfRunLines = pfRun.stdout.trim().split('\n').map(l => { try { return JSON.parse(l); } catch { return null; } }).filter(Boolean);
check(pfRunLines.every(l => l.ok !== false), 'prefab run batch is clean', pfRun.stdout + pfRun.stderr);
const instStep = pfRunLines.find(l => l.command === 'prefab.instantiate');
check(instStep && instStep.result.id > 0 && instStep.result.name === 'Crate', 'prefab.instantiate adds an instance', instStep);
const beforeStep = pfRunLines.find(l => l.id === 'before');
check(beforeStep && beforeStep.result.components.some(c => c.type === 'PrefabInstance'),
    'instantiated root carries a PrefabInstance component', beforeStep && beforeStep.result.components.map(c => c.type));
const revStep = pfRunLines.find(l => l.command === 'prefab.revert');
check(revStep && revStep.result.id !== instStep.result.id, 'prefab.revert returns a fresh entity', revStep);
const afterStep = pfRunLines.find(l => l.id === 'after');
check(afterStep && Math.abs(afterStep.result.value[0] - 0.8) < 1e-3,
    'prefab.revert restores the prefab value', afterStep && afterStep.result.value);


// ---------------------------------------------------------------------------
console.log('cli: physics queries + layers');
// ---------------------------------------------------------------------------
const physBatch = path.join(scratch, 'physics.jsonl');
fs.writeFileSync(physBatch, [
    { command: 'scene.new', args: { name: 'Phys' } },
    { command: 'entity.createPrimitive', args: { mesh: 'Plane', name: 'Floor', position: [0, 0, 0], scale: [40, 1, 40] }, id: 'floor' },
    { command: 'component.add', args: { id: '$ref:floor', type: 'BoxCollider', props: { size: [40, 1, 40] } } },
    { command: 'entity.createPrimitive', args: { mesh: 'Cube', name: 'Box', position: [0, 3, 0] }, id: 'box' },
    { command: 'component.add', args: { id: '$ref:box', type: 'BoxCollider', props: { size: [1, 1, 1] } } },
    { command: 'entity.set', args: { id: '$ref:box', layer: 2 } },
    { command: 'physics.raycast', args: { origin: [0, 10, 0], direction: [0, -1, 0], maxDistance: 100 }, id: 'hit' },
    { command: 'physics.overlapSphere', args: { center: [0, 3, 0], radius: 1 }, id: 'overlap' },
    { command: 'entity.create', args: { name: 'Cap', position: [3, 0, 0] }, id: 'cap' },
    { command: 'component.add', args: { id: '$ref:cap', type: 'CapsuleCollider', props: { radius: 0.5, height: 2 } } },
    { command: 'physics.overlapSphere', args: { center: [3, 0, 0], radius: 0.4 }, id: 'capOverlap' },
    { command: 'entity.create', args: { name: 'Hero', position: [-4, 1.5, 0] }, id: 'hero' },
    { command: 'component.add', args: { id: '$ref:hero', type: 'CharacterController' } },
    { command: 'physics.moveCharacter', args: { id: '$ref:hero', displacement: [0, -0.02, 0] }, id: 'heroStep' },
    { command: 'entity.create', args: { name: 'Sleeper', position: [0, 2, 0] }, id: 'sleeper' },
    { command: 'component.add', args: { id: '$ref:sleeper', type: 'BoxCollider', props: { size: [1, 1, 1] } } },
    { command: 'component.add', args: { id: '$ref:sleeper', type: 'RigidBody', props: { restitution: 0 } } },
    { command: 'physics.step', args: { dt: 0.0166, steps: 300 }, id: 'sim' },
    { command: 'entity.get', args: { id: '$ref:sleeper' }, id: 'sleeperGet' },
    { command: 'scene.layerMatrix.set', args: { a: 2, b: 2, collide: false } },
    { command: 'scene.layerMatrix.get', args: {}, id: 'lm' },
].map(s => JSON.stringify(s)).join('\n'));
const physRun = spawnSync(process.execPath, [cli, '--project', projectDir, 'exec.run', '--file', physBatch],
    { encoding: 'utf8', cwd: scratch, env: batchEnv() });
const physLines = physRun.stdout.trim().split('\n').map(l => { try { return JSON.parse(l); } catch { return null; } }).filter(Boolean);
check(physLines.every(l => l.ok !== false), 'physics batch is clean', physRun.stdout + physRun.stderr);
const hitStep = physLines.find(l => l.id === 'hit');
check(hitStep && hitStep.result.name === 'Box' && Math.abs(hitStep.result.distance - 6.5) < 0.05,
    'physics.raycast finds the nearest collider', hitStep && hitStep.result);
const overlapStep = physLines.find(l => l.id === 'overlap');
check(overlapStep && overlapStep.result.some(e => e.name === 'Box'),
    'physics.overlapSphere returns overlapping entities', overlapStep && overlapStep.result);
const lmStep = physLines.find(l => l.id === 'lm');
check(lmStep && lmStep.result[2] === 4294967291, 'scene.layerMatrix.set clears the layer bit', lmStep && lmStep.result[2]);
const capOverlapStep = physLines.find(l => l.id === 'capOverlap');
check(capOverlapStep && capOverlapStep.result.some(e => e.name === 'Cap'),
    'physics.overlapSphere matches a capsule volume', capOverlapStep && capOverlapStep.result);
const heroStep = physLines.find(l => l.id === 'heroStep');
check(heroStep && heroStep.result.grounded === true && Math.abs(heroStep.result.position[1] - 1.5) < 0.06,
    'physics.moveCharacter grounds a controller on the floor', heroStep && heroStep.result);
const simStep = physLines.find(l => l.id === 'sim');
check(simStep && simStep.result.steps === 300 && simStep.result.sleeping >= 1,
    'physics.step advances the simulation and bodies sleep', simStep && simStep.result);
const sleeperStep = physLines.find(l => l.id === 'sleeperGet');
check(sleeperStep && Math.abs(sleeperStep.result.transform.position[1] - 1.0) < 0.03,
    'physics.step settles a falling body on the floor', sleeperStep && sleeperStep.result && sleeperStep.result.transform);


const build = expectOk(['--project', projectDir, 'script.build']);
check(build && build.ok === true && build.output.endsWith('.so'),
    'script.build compiles the template scripts', build && build.diagnostics);
check(build && build.classes.some(c => c.name === 'Rotator' && c.props.some(p => p.name === 'speed')),
    'script.build reports script classes with inspector props', build && build.classes);

const playBatch = path.join(scratch, 'play.jsonl');
fs.writeFileSync(playBatch, [
    { command: 'scene.load', args: { file: 'Assets/Scenes/Agent.cscene' } },
    { command: 'script.load' },
    { command: 'entity.createPrimitive', args: { mesh: 'Sphere', name: 'Spinner', position: [0, 3, 0] }, id: 'spinner' },
    { command: 'component.add', args: { id: '$ref:spinner', type: 'Script', props: { class: 'Rotator', speed: 180 } } },
    { command: 'runtime.play' },
    { command: 'runtime.update', args: { frames: 120, render: true } },
    { command: 'engine.profile' },
    { command: 'runtime.logs', args: { level: 'info' } },
    { command: 'render.screenshot', args: { out: path.join(scratch, 'play.png'), width: 320, height: 200 } },
    { command: 'runtime.stop' },
].map(s => JSON.stringify(s)).join('\n'));

const playRun = spawnSync(process.execPath, [cli, '--project', projectDir, 'exec.run', '--file', playBatch],
    { encoding: 'utf8', cwd: scratch, env: batchEnv() });
const playLines = playRun.stdout.trim().split('\n').map(l => { try { return JSON.parse(l); } catch { return null; } });
check(playLines.filter(Boolean).every(l => l.ok !== false), 'play batch runs clean', playRun.stdout + playRun.stderr);
const playState = playLines.find(l => l.command === 'runtime.play');
check(playState && playState.result.playing === true, 'runtime.play enters play mode', playState);
const shot = playLines.find(l => l.command === 'render.screenshot');
check(shot && shot.result.stats.drawCalls > 0, 'render.screenshot renders geometry', shot && shot.result.stats);
check(shot && fs.existsSync(shot.result.path) && fs.statSync(shot.result.path).size > 100,
    'render.screenshot writes a PNG', shot && shot.result.path);
const logs = playLines.find(l => l.command === 'runtime.logs');
check(logs && Array.isArray(logs.result) && logs.result.every(e => e.level && e.text),
    'runtime.logs returns drained entries', logs && logs.result);
const profile = playLines.find(l => l.command === 'engine.profile');
check(profile && profile.result.frame >= 100 && profile.result.scopes.length > 0,
    'engine.profile reports sampled frames', profile && profile.result && profile.result.frame);
check(profile && profile.result.scopes.some(s => s.name === 'update') &&
    profile.result.scopes.some(s => s.name === 'render' || s.name === 'gpu'),
    'engine.profile records update and render scopes', profile && profile.result && profile.result.scopes.map(s => s.name));
check(profile && profile.result.render.drawCalls > 0 && profile.result.entities > 0,
    'engine.profile carries render counters', profile && profile.result && profile.result.render);

// Input injection + picking.
const inputBatch = path.join(scratch, 'input.jsonl');
fs.writeFileSync(inputBatch, [
    { command: 'scene.load', args: { file: 'Assets/Scenes/Agent.cscene' } },
    { command: 'input.keyDown', args: { key: 'Space' } },
    { command: 'input.tap', args: { key: 'Escape' } },
    { command: 'input.mouseMove', args: { x: 100, y: 50 } },
    { command: 'input.endFrame' },
    { command: 'render.pick', args: { x: 160, y: 100 } },
].map(s => JSON.stringify(s)).join('\n'));
const inputRun = expectOk(['--project', projectDir, 'exec.run', '--file', inputBatch]);
check(inputRun && inputRun.ok === true, 'input injection batch succeeds', inputRun && inputRun.failures);
expectFail(['input.keyDown', '--key', 'NotAKey'], 'invalid_arg');

// ---------------------------------------------------------------------------
console.log('cli: component enable flag + hierarchy commands');
// ---------------------------------------------------------------------------
// One batch: scene state lives in the process, so these checks share a session.
const flagBatch = path.join(scratch, 'flags.jsonl');
fs.writeFileSync(flagBatch, [
    { command: 'scene.new', args: { name: 'Flags' } },
    { command: 'entity.createPrimitive', args: { mesh: 'Cube', name: 'Vis' }, id: 'cube' },
    { command: 'scene.stats' },
    { command: 'component.set', args: { id: '$ref:cube', type: 'MeshRenderer', prop: 'enabled', value: false } },
    { command: 'scene.stats' },
    { command: 'component.set', args: { id: '$ref:cube', type: 'MeshRenderer', prop: 'enabled', value: true } },
    { command: 'scene.stats' },
    { command: 'entity.create', args: { name: 'Parent' }, id: 'parent' },
    { command: 'entity.create', args: { name: 'Kid', parent: '$ref:parent' } },
    { command: 'entity.duplicate', args: { id: '$ref:parent' }, id: 'copy' },
    { command: 'scene.hierarchy' },
].map(s => JSON.stringify(s)).join('\n'));

const flagRun = spawnSync(process.execPath, [cli, '--project', projectDir, 'exec.run', '--file', flagBatch],
    { encoding: 'utf8', cwd: scratch, env: batchEnv() });
const flagLines = flagRun.stdout.trim().split('\n').map(l => { try { return JSON.parse(l); } catch { return null; } }).filter(Boolean);
check(flagLines.filter(l => l.command !== 'exec.run').every(l => l.ok !== false),
    'enable/hierarchy batch runs clean', flagRun.stdout + flagRun.stderr);
const stats = flagLines.filter(l => l.command === 'scene.stats').map(l => l.result);
check(stats.length === 3 && stats[1].meshRenderers === stats[0].meshRenderers - 1
    && stats[2].meshRenderers === stats[0].meshRenderers,
    'disabled MeshRenderer disappears from the render stats', stats);
const hierarchy = flagLines.find(l => l.command === 'scene.hierarchy');
const copyNode = hierarchy && hierarchy.result.roots.find(r => r.id === flagLines.find(l => l.id === 'copy').result.id);
check(copyNode && copyNode.children.length === 1 && copyNode.children[0].name === 'Kid',
    'entity.duplicate keeps the subtree', copyNode);

// ---------------------------------------------------------------------------
console.log('cli: display transform settings');
// ---------------------------------------------------------------------------
const displaySettings = expectOk(['--project', projectDir, '--scene', 'Assets/Scenes/Agent.cscene',
    'scene.settings.set', '--key', 'tonemap', '--value', '"filmic"']);
check(displaySettings && displaySettings.settings.tonemap === 3,
    'scene.settings.set accepts tone mapping names', displaySettings && displaySettings.settings.tonemap);
const exposure = expectOk(['--project', projectDir, '--scene', 'Assets/Scenes/Agent.cscene',
    'scene.settings.set', '--key', 'exposure', '--value', '1.35']);
check(exposure && Math.abs(exposure.settings.exposure - 1.35) < 1e-4,
    'scene.settings.set writes exposure', exposure && exposure.settings.exposure);
const background = expectOk(['--project', projectDir, '--scene', 'Assets/Scenes/Agent.cscene',
    'scene.settings.set', '--key', 'background', '--value', '[0.1,0.15,0.3,1]']);
check(background && Math.abs(background.settings.background[2] - 0.3) < 1e-4,
    'scene background is writable', background && background.settings.background);
const savedSettings = expectOk(['--project', projectDir, '--scene', 'Assets/Scenes/Agent.cscene', 'scene.save']);
check(savedSettings && savedSettings.path, 'scene with display settings saved', savedSettings);

// ---------------------------------------------------------------------------
console.log('cli: directional shadows');
// ---------------------------------------------------------------------------
// A floor + box + directional sun must darken the render stats with shadows
// on, and return to a fully lit frame with scene.shadows = 0.
const shadowBatch = path.join(scratch, 'shadows.jsonl');
fs.writeFileSync(shadowBatch, [
    { command: 'scene.new', args: { name: 'Shadows' } },
    { command: 'scene.settings.set', args: { key: 'shadows', value: 2 }, id: 'on' },
    { command: 'scene.settings.set', args: { key: 'shadowResolution', value: 512 } },
    { command: 'entity.createPrimitive', args: { mesh: 'Plane', name: 'Floor', scale: [30, 1, 30] } },
    { command: 'entity.createPrimitive', args: { mesh: 'Cube', name: 'Crate', position: [0, 0.5, -4], scale: [2, 2, 2] }, id: 'crate' },
    { command: 'entity.create', args: { name: 'Sun' }, id: 'sun' },
    { command: 'component.add', args: { id: '$ref:sun', type: 'Light', props: { type: 0, castShadows: true } } },
    { command: 'component.set', args: { id: '$ref:sun', type: 'Transform', prop: 'rotation', value: [-50, 200, 0] } },
    { command: 'render.viewCamera.set', args: { position: [0, 3, 4], yaw: 0, pitch: -25 } },
    { command: 'render.setViewport', args: { width: 320, height: 240 } },
    { command: 'render.editorFrame', id: 'frameOn' },
    { command: 'scene.settings.set', args: { key: 'shadows', value: 0 } },
    { command: 'render.editorFrame', id: 'frameOff' },
].map(s => JSON.stringify(s)).join('\n'));

const shadowRun = spawnSync(process.execPath, [cli, '--project', projectDir, 'exec.run', '--file', shadowBatch],
    { encoding: 'utf8', cwd: scratch, env: batchEnv() });
const shadowLines = shadowRun.stdout.trim().split('\n')
    .map(l => { try { return JSON.parse(l); } catch { return null; } }).filter(Boolean);
check(shadowLines.length > 0 && shadowLines.every(l => l.ok !== false), 'shadow batch runs clean',
    shadowRun.stdout + shadowRun.stderr);
const frameOn = shadowLines.find(l => l.id === 'frameOn');
const frameOff = shadowLines.find(l => l.id === 'frameOff');
check(frameOn && frameOn.result.shadowedPixels > 0,
    'shadows on: shadowedPixels > 0', frameOn && frameOn.result);
check(frameOff && frameOff.result.shadowedPixels === 0,
    'shadows off: shadowedPixels == 0', frameOff && frameOff.result);

// ---------------------------------------------------------------------------
console.log('cli: post-processing settings');
// ---------------------------------------------------------------------------
const postBatch = path.join(scratch, 'post.jsonl');
fs.writeFileSync(postBatch, [
    { command: 'scene.new', args: { name: 'Post' } },
    { command: 'scene.settings.set', args: { key: 'postVignette', value: 0.9 }, id: 'vignette' },
    { command: 'scene.settings.set', args: { key: 'postVignetteRadius', value: 0.4 } },
    { command: 'scene.settings.set', args: { key: 'postVignetteSoftness', value: 0.3 } },
    { command: 'scene.settings.set', args: { key: 'postContrast', value: 1.25 }, id: 'contrast' },
    { command: 'scene.settings.set', args: { key: 'postSaturation', value: 0.8 } },
    { command: 'scene.settings.set', args: { key: 'postBloomStrength', value: 1.1 }, id: 'bloom' },
    { command: 'scene.settings.set', args: { key: 'postBloomThreshold', value: 0.7 } },
    { command: 'scene.settings.set', args: { key: 'postFxaa', value: true }, id: 'fxaa' },
    { command: 'render.editorFrame', id: 'frame' },
    { command: 'scene.settings.set', args: { key: 'postVignette', value: 0 } },
    { command: 'scene.settings.set', args: { key: 'postContrast', value: 1 } },
    { command: 'scene.settings.set', args: { key: 'postSaturation', value: 1 } },
    { command: 'scene.settings.set', args: { key: 'postBloomStrength', value: 0 } },
    { command: 'scene.settings.set', args: { key: 'postFxaa', value: false } },
    { command: 'render.editorFrame', id: 'frameOff' },
    { command: 'scene.settings.get', id: 'final' },
].map(s => JSON.stringify(s)).join('\n'));

const postRun = spawnSync(process.execPath, [cli, '--project', projectDir, 'exec.run', '--file', postBatch],
    { encoding: 'utf8', cwd: scratch, env: batchEnv() });
const postLines = postRun.stdout.trim().split('\n')
    .map(l => { try { return JSON.parse(l); } catch { return null; } }).filter(Boolean);
check(postLines.length > 0 && postLines.every(l => l.ok !== false), 'post batch runs clean',
    postRun.stdout + postRun.stderr);
const byId = {};
for (const l of postLines) if (l.id) byId[l.id] = l;
check(byId.vignette && Math.abs(byId.vignette.result.settings.postVignette - 0.9) < 1e-3,
    'postVignette round-trips through scene.settings', byId.vignette && byId.vignette.result.settings);
check(byId.contrast && Math.abs(byId.contrast.result.settings.postContrast - 1.25) < 1e-3,
    'postContrast round-trips', byId.contrast && byId.contrast.result.settings);
check(byId.bloom && Math.abs(byId.bloom.result.settings.postBloomStrength - 1.1) < 1e-3,
    'postBloomStrength round-trips', byId.bloom && byId.bloom.result.settings);
check(byId.fxaa && byId.fxaa.result.settings.postFxaa === true,
    'postFxaa round-trips', byId.fxaa && byId.fxaa.result.settings);
check(byId.frame && byId.frame.result.postPasses === 4,
    'all four effects apply: postPasses == 4', byId.frame && byId.frame.result);
check(byId.frameOff && byId.frameOff.result.postPasses === 0,
    'neutral settings: postPasses == 0', byId.frameOff && byId.frameOff.result);
const finalSettings = byId.final && byId.final.result;
check(finalSettings && Math.abs(finalSettings.postVignette) < 1e-6 && finalSettings.postFxaa === false,
    'settings reset back to neutral', finalSettings);
expectFail(['--project', projectDir, 'scene.settings.set', '--key', 'postNope', '--value', '1'],
    'invalid_arg');

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
console.log('cli: image editor');
// ---------------------------------------------------------------------------
const imageBatch = path.join(scratch, 'image.jsonl');
const imageOut = path.join(projectDir, 'Assets', 'Textures', 'cli_texture.png');
fs.writeFileSync(imageBatch, [
    { command: 'image.new', args: { width: 64, height: 64, background: 'white' } },
    { command: 'image.draw.rect', args: { x: 8, y: 8, width: 48, height: 48, color: '#3a7de0', filled: true } },
    { command: 'image.draw.brush', args: { x: 16, y: 16, x2: 48, y2: 48, brush: 6, color: '#e05a3a' } },
    { command: 'image.layer.add', args: { name: 'Detail', fill: '#f5c542' } },
    { command: 'image.draw.fill', args: { x: 2, y: 2, color: '#222222', layer: 1 } },
    { command: 'image.normalMap', args: { strength: 8 } },
    { command: 'image.pixel', args: { x: 32, y: 32, layer: 0 } },
    { command: 'image.flatten' },
    { command: 'image.save', args: { out: imageOut } },
].map(s => JSON.stringify(s)).join('\n'));

const imageRunRaw = spawnSync(process.execPath, [cli, '--project', projectDir, 'exec.run', '--file', imageBatch],
    { encoding: 'utf8', cwd: scratch, env: batchEnv() });
const imageLines = imageRunRaw.stdout.trim().split('\n').map(l => { try { return JSON.parse(l); } catch { return null; } }).filter(Boolean);
check(imageLines.every(l => l.ok !== false), 'image batch runs clean', imageRunRaw.stdout + imageRunRaw.stderr);
check(imageLines.find(l => l.command === 'image.normalMap' && l.result.layerIndex === 2),
    'image.normalMap appends a layer', imageLines.find(l => l.command === 'image.normalMap'));
check(imageLines.find(l => l.command === 'image.flatten' && l.result.layerCount === 1),
    'image.flatten collapses the stack');
check(fs.existsSync(imageOut) && fs.statSync(imageOut).size > 100, 'image.save writes a PNG', imageOut);

// Reopen the generated texture and verify the decode path.
const reopened = expectOk(['--project', projectDir, 'image.open', '--file', 'Assets/Textures/cli_texture.png']);
check(reopened && reopened.width === 64 && reopened.height === 64, 'image.open reads the PNG back', reopened);
const pixel = expectOk(['--project', projectDir, '--image', 'Assets/Textures/cli_texture.png', 'image.pixel', '--x', '32', '--y', '32']);
check(pixel && pixel.a === 255 && /^#[0-9a-f]{6}$/.test(pixel.hex), 'image.pixel samples a color', pixel);
const colorConv = expectOk(['image.color', '--value', '[255,128,0]', '--to', 'hsv']);
check(colorConv && colorConv.hex === '#ff8000' && Math.abs(colorConv.hsv.h - 0.083) < 0.01,
    'image.color converts to HSV', colorConv);

// ---------------------------------------------------------------------------
console.log('cli: export + preferences');
// ---------------------------------------------------------------------------
const prefs = expectOk(['project.preferences', '--set', '{"language":"en"}']);
check(prefs && prefs.language === 'en', 'project.preferences writes the store', prefs);
expectFail(['project.preferences', '--set', '{"nope":1}'], 'invalid_arg');

const exported = expectOk(['--project', projectDir, 'project.exportPlayer', '--target', path.join(scratch, 'player')]);
check(exported && exported.ok === true && fs.existsSync(path.join(exported.dir, 'project.cproj')),
    'project.exportPlayer packages a runnable player', exported);
check(exported && fs.existsSync(path.join(exported.dir, 'Library', 'GameScripts.so')),
    'exported player carries the compiled scripts');

// ---------------------------------------------------------------------------
fs.rmSync(scratch, { recursive: true, force: true });

console.log(`cli: ${checks - failures}/${checks} checks passed`);
process.exit(failures === 0 ? 0 : 1);
