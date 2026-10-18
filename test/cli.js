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
console.log('cli: prefab per-property overrides');
// ---------------------------------------------------------------------------
// Two instances: one tweaked, one left alone. The .prefab is then edited from
// outside and the scene reloaded: the tweaked instance keeps its change and
// picks up everything else, the untouched one follows the asset exactly.
function runProjectBatch(file) {
    const r = spawnSync(process.execPath, [cli, '--project', projectDir, 'exec.run', '--file', file],
        { encoding: 'utf8', cwd: scratch, env: batchEnv() });
    const lines = r.stdout.trim().split('\n').map(l => { try { return JSON.parse(l); } catch { return null; } }).filter(Boolean);
    return { stdout: r.stdout, stderr: r.stderr, lines, step: name => lines.find(l => l.id === name) };
}
function writeBatch(name, steps) {
    const file = path.join(scratch, name);
    fs.writeFileSync(file, steps.map(s => JSON.stringify(s)).join('\n'));
    return file;
}

const ov1 = runProjectBatch(writeBatch('ov1.jsonl', [
    { command: 'scene.load', args: { file: 'Assets/Scenes/Pf.cscene' } },
    { command: 'prefab.instantiate', args: { file: 'Assets/Prefabs/Crate.prefab' }, id: 'a' },
    { command: 'prefab.instantiate', args: { file: 'Assets/Prefabs/Crate.prefab' }, id: 'b' },
    { command: 'component.setProps', args: { id: '$ref:a', type: 'MeshRenderer', props: { albedoColor: [0, 1, 0, 1] } } },
    { command: 'entity.set', args: { id: '$ref:a', name: 'GreenCrate', layer: 3 } },
    { command: 'scene.save', args: { file: 'Assets/Scenes/Pf.cscene' } },
    { command: 'prefab.overrides', args: { id: '$ref:a' }, id: 'ovA' },
    { command: 'prefab.overrides', args: { id: '$ref:b' }, id: 'ovB' },
]));
check(ov1.lines.every(l => l.ok !== false), 'override batch is clean', ov1.stdout + ov1.stderr);
const instA = ov1.lines.filter(l => l.command === 'prefab.instantiate')[0].result.id;
const instB = ov1.lines.filter(l => l.command === 'prefab.instantiate')[1].result.id;
const ovA1 = ov1.step('ovA');
const ovB1 = ov1.step('ovB');
const keysA = ovA1 ? ovA1.result.overrides.map(o => o.key) : [];
check(ovA1 && ovA1.result.count === 3
    && keysA.includes('MeshRenderer.albedoColor') && keysA.includes('@name') && keysA.includes('@layer'),
    'prefab.overrides lists what the instance changed', ovA1 && ovA1.result);
check(ovB1 && ovB1.result.count === 0, 'an untouched instance overrides nothing', ovB1 && ovB1.result);

// Drop the tweaked instance's child, so the record also carries a removal.
const tree1 = runProjectBatch(writeBatch('ov-tree.jsonl', [
    { command: 'scene.load', args: { file: 'Assets/Scenes/Pf.cscene' } },
    { command: 'scene.tree', args: {} , id: 'tree' },
]));
const lidA = tree1.step('tree').result.entities.find(e => e.parent === instA).id;
const ov2 = runProjectBatch(writeBatch('ov2.jsonl', [
    { command: 'scene.load', args: { file: 'Assets/Scenes/Pf.cscene' } },
    { command: 'entity.destroy', args: { id: lidA } },
    { command: 'scene.save', args: { file: 'Assets/Scenes/Pf.cscene' } },
    { command: 'prefab.overrides', args: { id: instA }, id: 'ovA' },
]));
const ovA2 = ov2.step('ovA');
check(ov2.lines.every(l => l.ok !== false) && ovA2
    && JSON.stringify(ovA2.result.overrides.find(o => o.key === '@removedChildren') || {}).includes('Lid'),
    'a child deleted on the instance is recorded as removed', ovA2 && ovA2.result);

// Edit the asset itself: a new colour and a moved child.
const prefabFile = path.join(projectDir, 'Assets', 'Prefabs', 'Crate.prefab');
const prefabDoc = JSON.parse(fs.readFileSync(prefabFile, 'utf8'));
for (const c of prefabDoc.root.components) {
    if (c.type === 'MeshRenderer') c.properties.albedoColor = [0.1, 0.1, 0.9, 1];
}
prefabDoc.root.children.find(c => c.name === 'Lid').components
    .find(c => c.type === 'Transform').position = [0, 3, 0];
fs.writeFileSync(prefabFile, JSON.stringify(prefabDoc, null, 2) + '\n');

const ov3 = runProjectBatch(writeBatch('ov3.jsonl', [
    { command: 'scene.load', args: { file: 'Assets/Scenes/Pf.cscene' } },
    { command: 'component.getProp', args: { id: instA, type: 'MeshRenderer', prop: 'albedoColor' }, id: 'aColor' },
    { command: 'component.getProp', args: { id: instB, type: 'MeshRenderer', prop: 'albedoColor' }, id: 'bColor' },
    { command: 'entity.get', args: { id: instA }, id: 'aEnt' },
    { command: 'entity.get', args: { id: instB }, id: 'bEnt' },
    { command: 'scene.tree', args: {}, id: 'tree' },
]));
check(ov3.lines.every(l => l.ok !== false), 'reload after editing the asset is clean', ov3.stdout + ov3.stderr);
const aColor = ov3.step('aColor').result.value;
const bColor = ov3.step('bColor').result.value;
check(Math.abs(aColor[1] - 1) < 1e-3 && Math.abs(aColor[0]) < 1e-3,
    'the overridden colour survives an asset edit', aColor);
check(Math.abs(bColor[2] - 0.9) < 1e-3 && Math.abs(bColor[1] - 0.1) < 1e-3,
    'the untouched instance follows the asset', bColor);
check(ov3.step('aEnt').result.name === 'GreenCrate' && ov3.step('aEnt').result.layer === 3,
    'the overridden name and layer survive', ov3.step('aEnt').result);
check(ov3.step('bEnt').result.name === 'Crate', 'the untouched instance keeps the asset name',
    ov3.step('bEnt').result.name);
const ov3Tree = ov3.step('tree').result.entities;
const bLid = ov3Tree.find(e => e.parent === instB && e.name === 'Lid');
check(!ov3Tree.some(e => e.parent === instA) && bLid && Math.abs(bLid.transform.position[1] - 3) < 1e-3,
    'a deleted child stays deleted while the asset child move reaches the rest',
    { aChildren: ov3Tree.filter(e => e.parent === instA).map(e => e.name), bLid });

// Apply: the instance's record becomes the asset, and its siblings follow.
const ov4 = runProjectBatch(writeBatch('ov4.jsonl', [
    { command: 'scene.load', args: { file: 'Assets/Scenes/Pf.cscene' } },
    { command: 'prefab.apply', args: { id: instA } },
    { command: 'component.getProp', args: { id: instB, type: 'MeshRenderer', prop: 'albedoColor' }, id: 'bColor' },
    { command: 'entity.get', args: { id: instB }, id: 'bEnt' },
    { command: 'prefab.revert', args: { id: instB }, id: 'revB' },
    { command: 'scene.tree', args: {}, id: 'tree' },
]));
check(ov4.lines.every(l => l.ok !== false), 'prefab.apply batch is clean', ov4.stdout + ov4.stderr);
const bColor2 = ov4.step('bColor').result.value;
check(Math.abs(bColor2[1] - 1) < 1e-3, 'applying an instance pushes its override to the asset', bColor2);
check(ov4.step('bEnt').result.name === 'GreenCrate', 'the applied name reaches the sibling',
    ov4.step('bEnt').result.name);
const applied = JSON.parse(fs.readFileSync(prefabFile, 'utf8'));
check(!applied.root.children || !applied.root.children.some(c => c.name === 'Lid'),
    'the applied removal reaches the asset file', applied.root.children);
const revB = ov4.step('revB').result.id;
const tree4 = ov4.step('tree').result.entities;
check(!tree4.some(e => e.parent === revB), 'revert gives a fresh copy with the asset subtree',
    tree4.filter(e => e.parent === revB).map(e => e.name));


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


// ---------------------------------------------------------------------------
console.log('cli: overlap box + shape casts');
// ---------------------------------------------------------------------------
// The query family beyond the ray: an axis-aligned overlap box, and a swept
// sphere / box. The point of a shape cast (over a ray) is that its surface
// decides - so a cast whose centre line slips past the crate still stops on its
// edge - and the point of an overlap box is that it is exact against a sphere's
// surface rather than the sphere's bounding box.
const castBatch = writeBatch('casts.jsonl', [
    { command: 'scene.new', args: { name: 'Casts' } },
    { command: 'entity.create', args: { name: 'Floor', position: [0, 0, 0] }, id: 'floor' },
    { command: 'component.add', args: { id: '$ref:floor', type: 'BoxCollider', props: { size: [20, 1, 20] } } },
    { command: 'entity.create', args: { name: 'Crate', position: [0, 3, 0] }, id: 'crate' },
    { command: 'component.add', args: { id: '$ref:crate', type: 'BoxCollider', props: { size: [1, 1, 1] } } },
    // Off the cast lines in z, so only the sphere overlap check can see it.
    { command: 'entity.create', args: { name: 'Ball', position: [3, 3, 6] }, id: 'ball' },
    { command: 'component.add', args: { id: '$ref:ball', type: 'SphereCollider', props: { radius: 0.75 } } },
    { command: 'physics.overlapBox', args: { center: [0, 3, 0], size: [1.2, 1.2, 1.2] }, id: 'boxOn' },
    { command: 'physics.overlapBox', args: { center: [0, 8, 0], size: [1, 1, 1] }, id: 'boxOff' },
    { command: 'physics.overlapBox', args: { center: [3.8, 4, 6], size: [0.6, 0.6, 0.6] }, id: 'boxCorner' },
    { command: 'physics.sphereCast', args: { origin: [0.6, 10, 0], radius: 0.5, direction: [0, -1, 0], maxDistance: 100 }, id: 'sphereHit' },
    { command: 'physics.sphereCast', args: { origin: [8, 10, 0], radius: 0.5, direction: [0, -1, 0], maxDistance: 100 }, id: 'sphereFloor' },
    { command: 'physics.boxCast', args: { center: [0.9, 10, 0], size: [1, 1, 1], direction: [0, -1, 0], maxDistance: 100 }, id: 'boxCastHit' },
    { command: 'physics.raycast', args: { origin: [0.9, 10, 0], direction: [0, -1, 0], maxDistance: 100 }, id: 'raySameSpot' },
]);
const castRun = runProjectBatch(castBatch);
const castLines = castRun.stdout.trim().split('\n').map(l => { try { return JSON.parse(l); } catch { return null; } }).filter(Boolean);
check(castLines.every(l => l.ok !== false), 'cast batch is clean', castRun.stdout + castRun.stderr);
const castStep = id => castLines.find(l => l.id === id);
const boxOn = castStep('boxOn');
check(boxOn && boxOn.result.some(e => e.name === 'Crate') && !boxOn.result.some(e => e.name === 'Floor'),
    'physics.overlapBox finds the crate it wraps and not the floor', boxOn && boxOn.result);
const boxOff = castStep('boxOff');
check(boxOff && boxOff.result.length === 0, 'physics.overlapBox reports empty air as empty', boxOff && boxOff.result);
const boxCorner = castStep('boxCorner');
check(boxCorner && !boxCorner.result.some(e => e.name === 'Ball'),
    'physics.overlapBox is exact against a sphere, not its bounding box', boxCorner && boxCorner.result);
const sphereHit = castStep('sphereHit');
check(sphereHit && sphereHit.result && sphereHit.result.name === 'Crate' &&
    Math.abs(sphereHit.result.distance - (10 - 3.5 - Math.sqrt(0.24))) < 0.05,
    'physics.sphereCast stops the swept sphere on the crate edge', sphereHit && sphereHit.result);
check(sphereHit && sphereHit.result && Math.abs(sphereHit.result.normal[0] - 0.2) < 0.02,
    'physics.sphereCast reports the edge normal', sphereHit && sphereHit.result && sphereHit.result.normal);
check(sphereHit && sphereHit.result && Math.abs(sphereHit.result.point[1] - 3.5) < 0.02,
    'physics.sphereCast reports the contact point on the surface', sphereHit && sphereHit.result && sphereHit.result.point);
const sphereFloor = castStep('sphereFloor');
check(sphereFloor && sphereFloor.result && sphereFloor.result.name === 'Floor' &&
    Math.abs(sphereFloor.result.distance - 9.0) < 0.05,
    'physics.sphereCast slips past the crate and lands on the floor', sphereFloor && sphereFloor.result);
const boxCastHit = castStep('boxCastHit');
check(boxCastHit && boxCastHit.result && boxCastHit.result.name === 'Crate' &&
    Math.abs(boxCastHit.result.distance - 6.0) < 0.05,
    'physics.boxCast lands the box on the crate top', boxCastHit && boxCastHit.result);
const raySameSpot = castStep('raySameSpot');
check(raySameSpot && raySameSpot.result && raySameSpot.result.name === 'Floor' &&
    Math.abs(raySameSpot.result.distance - 9.5) < 0.05,
    'a ray down the same centre line misses the crate the cast hit',
    raySameSpot && raySameSpot.result);


// ---------------------------------------------------------------------------
console.log('cli: mesh colliders');
// ---------------------------------------------------------------------------
// A triangle-soup collider for level geometry: it stores a project-relative
// mesh, the ray hits its triangles (not the box around them), a body settles on
// it, it survives a scene reload, and a mesh on a body the solver has to push is
// dropped with a warning rather than misbehaving silently.
const meshDir = path.join(projectDir, 'Assets', 'Meshes');
fs.mkdirSync(meshDir, { recursive: true });
fs.writeFileSync(path.join(meshDir, 'ramp.obj'),
    ['v -6 0 -6', 'v 6 0 -6', 'v 6 0 6', 'v -6 0 6', 'f 1 2 3 4', ''].join('\n'));
// The lower-right half of the [-4,4] square in the XY plane at z = 0.
fs.writeFileSync(path.join(meshDir, 'tri.obj'),
    ['v -4 -4 0', 'v 4 -4 0', 'v 4 4 0', 'f 1 2 3', ''].join('\n'));

const meshSchema = expectOk(['--project', projectDir, 'component.schema', '--type', 'MeshCollider']);
check(meshSchema && meshSchema.components.length === 1 &&
    meshSchema.components[0].props.includes('meshPath') &&
    meshSchema.components[0].props.includes('isTrigger'),
    'component.schema exposes the mesh collider fields', meshSchema && meshSchema.components);

const meshRun = runProjectBatch(writeBatch('mesh.jsonl', [
    { command: 'scene.new', args: { name: 'Mesh' } },
    { command: 'entity.create', args: { name: 'Level' }, id: 'level' },
    { command: 'component.add', args: { id: '$ref:level', type: 'MeshCollider',
        props: { meshPath: 'Assets/Meshes/ramp.obj' } } },
    { command: 'component.getProp', args: { id: '$ref:level', type: 'MeshCollider', prop: 'meshPath' }, id: 'path' },
    { command: 'physics.raycast', args: { origin: [2, 5, 2], direction: [0, -1, 0], maxDistance: 100 }, id: 'hit' },
    { command: 'entity.create', args: { name: 'Tri' }, id: 'tri' },
    { command: 'component.add', args: { id: '$ref:tri', type: 'MeshCollider',
        props: { meshPath: 'Assets/Meshes/tri.obj' } } },
    { command: 'physics.raycast', args: { origin: [3, -3, -5], direction: [0, 0, 1], maxDistance: 100 }, id: 'triHit' },
    { command: 'physics.raycast', args: { origin: [-3, 3, -5], direction: [0, 0, 1], maxDistance: 100 }, id: 'triMiss' },
    { command: 'entity.create', args: { name: 'Ball', position: [2, 3, 2] }, id: 'ball' },
    { command: 'component.add', args: { id: '$ref:ball', type: 'SphereCollider', props: { radius: 0.5 } } },
    { command: 'component.add', args: { id: '$ref:ball', type: 'RigidBody', props: { restitution: 0 } } },
    { command: 'entity.create', args: { name: 'Mover', position: [20, 3, 20] }, id: 'mover' },
    { command: 'component.add', args: { id: '$ref:mover', type: 'MeshCollider',
        props: { meshPath: 'Assets/Meshes/ramp.obj' } } },
    { command: 'component.add', args: { id: '$ref:mover', type: 'RigidBody', props: {} } },
    { command: 'physics.step', args: { dt: 0.016667, steps: 240 }, id: 'sim' },
    { command: 'entity.get', args: { id: '$ref:ball' }, id: 'ballGet' },
    { command: 'entity.get', args: { id: '$ref:mover' }, id: 'moverGet' },
    { command: 'runtime.logs', args: { text: 'MeshCollider' }, id: 'logs' },
    { command: 'scene.save', args: { file: 'Assets/Scenes/Mesh.cscene' } },
]));
check(meshRun.lines.every(l => l.ok !== false), 'mesh collider batch is clean', meshRun.stdout + meshRun.stderr);

const meshPathStep = meshRun.step('path');
check(meshPathStep && meshPathStep.result.value === 'Assets/Meshes/ramp.obj',
    'component.add MeshCollider keeps the project-relative mesh', meshPathStep && meshPathStep.result);

const meshHit = meshRun.step('hit');
check(meshHit && meshHit.result.name === 'Level' && Math.abs(meshHit.result.distance - 5) < 0.02,
    'physics.raycast hits the mesh surface at the right distance', meshHit && meshHit.result);

const triHit = meshRun.step('triHit');
check(triHit && triHit.result.name === 'Tri' && Math.abs(triHit.result.distance - 5) < 0.02,
    'physics.raycast hits the triangle itself', triHit && triHit.result);
const triMiss = meshRun.step('triMiss');
check(triMiss && triMiss.result === null,
    'physics.raycast misses the empty half of the bounding box', triMiss && triMiss.result);

const ballGet = meshRun.step('ballGet');
check(ballGet && Math.abs(ballGet.result.transform.position[1] - 0.5) < 0.05,
    'a sphere settles on the mesh floor', ballGet && ballGet.result && ballGet.result.transform);

const moverGet = meshRun.step('moverGet');
check(moverGet && moverGet.result.transform.position[1] < -10,
    'a mesh on a dynamic body is dropped, so the body keeps falling', moverGet && moverGet.result && moverGet.result.transform);
const meshLogs = meshRun.step('logs');
check(meshLogs && meshLogs.result.some(l => l.level === 'warning' && l.text.includes('static-only')),
    'the dropped collider is reported in the engine log', meshLogs && meshLogs.result);

const savedMesh = fs.readFileSync(path.join(projectDir, 'Assets', 'Scenes', 'Mesh.cscene'), 'utf8');
check(savedMesh.includes('Assets/Meshes/ramp.obj') && savedMesh.includes('MeshCollider'),
    'the scene serializes the mesh collider', savedMesh);

const meshReload = runProjectBatch(writeBatch('mesh-reload.jsonl', [
    { command: 'scene.load', args: { file: 'Assets/Scenes/Mesh.cscene' } },
    // Away from the settled ball, which the first batch left on the floor.
    { command: 'physics.raycast', args: { origin: [4, 5, 4], direction: [0, -1, 0], maxDistance: 100 }, id: 'hit' },
    { command: 'physics.raycast', args: { origin: [3, -3, -5], direction: [0, 0, 1], maxDistance: 100 }, id: 'triHit' },
]));
const reloadHit = meshReload.step('hit');
check(reloadHit && reloadHit.result.name === 'Level' && Math.abs(reloadHit.result.distance - 5) < 0.02,
    'a reloaded scene keeps working mesh colliders', reloadHit && reloadHit.result);
const reloadTri = meshReload.step('triHit');
check(reloadTri && reloadTri.result.name === 'Tri',
    'a reloaded scene keeps the second mesh collider', reloadTri && reloadTri.result);


// ---------------------------------------------------------------------------
console.log('cli: hinge + fixed joints');
// ---------------------------------------------------------------------------
// A hand-written scene pins the whole path: the joint components must come back
// from their serialized properties, the weld must resolve its connected body by
// GUID, and the step must then actually constrain the bodies.
const jointScene = path.join(projectDir, 'Assets', 'Scenes', 'Joints.cscene');
fs.writeFileSync(jointScene, JSON.stringify({
    format: 'cairn-scene', version: 1, name: 'Joints',
    settings: { gravity: [0, -9.81, 0] },
    entities: [
        { id: 1, name: 'Deck', guid: 'aaaa1111aaaa1111aaaa1111aaaa1111', active: true, components: [
            { type: 'Transform', position: [0, 0, 0], rotation: [0, 0, 0], scale: [1, 1, 1] },
            { type: 'SphereCollider', properties: { radius: 0.1 } },
            { type: 'RigidBody', properties: { isKinematic: true, useGravity: false } },
            { type: 'FixedJoint', properties: { connectedGuid: 'bbbb2222bbbb2222bbbb2222bbbb2222' } },
        ] },
        { id: 2, name: 'Crate', guid: 'bbbb2222bbbb2222bbbb2222bbbb2222', active: true, components: [
            { type: 'Transform', position: [2, 0, 0], rotation: [0, 0, 0], scale: [1, 1, 1] },
            { type: 'SphereCollider', properties: { radius: 0.1 } },
            { type: 'RigidBody', properties: { useGravity: true } },
        ] },
        { id: 3, name: 'Door', guid: 'cccc3333cccc3333cccc3333cccc3333', active: true, components: [
            { type: 'Transform', position: [1, 0, 0], rotation: [0, 0, 0], scale: [1, 1, 1] },
            { type: 'SphereCollider', properties: { radius: 0.1 } },
            { type: 'RigidBody', properties: { useGravity: true } },
            { type: 'HingeJoint', properties: { anchor: [-1, 0, 0], connectedAnchor: [0, 0, 0],
                axis: [0, 0, 1], useLimits: true, limitsMin: -45, limitsMax: 45 } },
        ] },
        { id: 4, name: 'Wheel', guid: 'dddd4444dddd4444dddd4444dddd4444', active: true, components: [
            { type: 'Transform', position: [5, 1, 0], rotation: [0, 0, 0], scale: [1, 1, 1] },
            { type: 'SphereCollider', properties: { radius: 0.1 } },
            { type: 'RigidBody', properties: { useGravity: false } },
            { type: 'HingeJoint', properties: { anchor: [-1, 0, 0], connectedAnchor: [4, 1, 0],
                axis: [0, 1, 0], useMotor: true, motorVelocity: 90, motorForce: 50 } },
        ] },
    ],
}));
const jointBatch = path.join(scratch, 'joints.jsonl');
fs.writeFileSync(jointBatch, [
    { command: 'scene.load', args: { file: 'Assets/Scenes/Joints.cscene' } },
    { command: 'component.getProp', args: { id: 1, type: 'FixedJoint', prop: 'connectedGuid' }, id: 'weld' },
    { command: 'component.schema', args: { type: 'HingeJoint', includeTransform: false }, id: 'schema' },
    { command: 'scene.tree', args: { component: 'HingeJoint' }, id: 'tree' },
    { command: 'physics.step', args: { dt: 0.016667, steps: 61 }, id: 'sim' },
    { command: 'entity.get', args: { id: 4 }, id: 'wheel' },
    { command: 'physics.step', args: { dt: 0.016667, steps: 30 }, id: 'sim2' },
    { command: 'entity.get', args: { id: 2 }, id: 'crate' },
    { command: 'entity.get', args: { id: 3 }, id: 'door' },
].map(s => JSON.stringify(s)).join('\n'));
const jointRun = spawnSync(process.execPath, [cli, '--project', projectDir, 'exec.run', '--file', jointBatch],
    { encoding: 'utf8', cwd: scratch, env: batchEnv() });
const jointLines = jointRun.stdout.trim().split('\n').map(l => { try { return JSON.parse(l); } catch { return null; } }).filter(Boolean);
const jointSteps = jointLines.filter(l => typeof l.index === 'number');
check(jointSteps.length === 9 && jointSteps.every(l => l.ok === true), 'joint batch is clean',
    jointRun.stdout + jointRun.stderr);
const weldStep = jointLines.find(l => l.id === 'weld');
check(weldStep && weldStep.result.value === 'bbbb2222bbbb2222bbbb2222bbbb2222',
    'FixedJoint.connectedGuid round-trips through the scene file', weldStep && weldStep.result);
const schemaStep = jointLines.find(l => l.id === 'schema');
const hingeProps = schemaStep && schemaStep.result.components[0] ? schemaStep.result.components[0].props : [];
check(['connectedGuid', 'anchor', 'axis', 'useLimits', 'limitsMin', 'useMotor', 'motorForce']
    .every(p => hingeProps.includes(p)),
    'component.schema lists the hinge properties', hingeProps);
const treeJoints = jointLines.find(l => l.id === 'tree');
check(treeJoints && treeJoints.result.count === 2, 'scene.tree finds both hinged entities',
    treeJoints && treeJoints.result);
const simJoints = jointLines.find(l => l.id === 'sim');
check(simJoints && simJoints.result.steps === 61, 'physics.step runs with joints attached',
    simJoints && simJoints.result);
const wheelStep = jointLines.find(l => l.id === 'wheel');
// 61 steps of 90 degrees a second is a quarter turn: the motor reaches its
// target rate on its first step, and the pin keeps the arm exactly one unit
// from the pivot the whole time.
check(wheelStep && Math.abs(wheelStep.result.transform.rotation[1] - 90) < 3,
    'a hinge motor turns its body at the target rate', wheelStep && wheelStep.result.transform);
check(wheelStep && Math.abs(wheelStep.result.transform.position[0] - 4) < 0.05
    && Math.abs(wheelStep.result.transform.position[2] + 1) < 0.05,
    'the motorised arm stays on its circle', wheelStep && wheelStep.result.transform);
const crateStep = jointLines.find(l => l.id === 'crate');
check(crateStep && Math.abs(crateStep.result.transform.position[0] - 2) < 0.02
    && Math.abs(crateStep.result.transform.position[1]) < 0.02,
    'a weld holds a falling body against its kinematic partner', crateStep && crateStep.result.transform);
const doorStep = jointLines.find(l => l.id === 'door');
check(doorStep && Math.abs(doorStep.result.transform.position[0] - Math.SQRT1_2) < 0.02
    && Math.abs(doorStep.result.transform.position[1] + Math.SQRT1_2) < 0.02,
    'a hinge limit stops the swinging door', doorStep && doorStep.result.transform);

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
check(profile && profile.result.jobs.workers >= 1 && profile.result.jobs.dispatches > 0,
    'engine.profile reports job-system telemetry', profile && profile.result && profile.result.jobs);

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
console.log('cli: image preview + shapes');
// ---------------------------------------------------------------------------
// The agent-facing drawing surface: a text preview and pixel statistics it can
// iterate against, plus polygons and one-call fill+outline shapes.
function runBatch(file, extra = []) {
    const r = spawnSync(process.execPath, [cli, 'exec.run', '--file', file, ...extra], { encoding: 'utf8', cwd: scratch, env: batchEnv() });
    const lines = r.stdout.trim().split('\n').map(l => { try { return JSON.parse(l); } catch { return null; } }).filter(Boolean);
    const byId = {};
    for (const l of lines) if (l.id) byId[l.id] = l;
    return { byId, lines, stdout: r.stdout, stderr: r.stderr };
}

const plateBatch = path.join(scratch, 'image-plate.jsonl');
fs.writeFileSync(plateBatch, [
    { command: 'image.new', args: { width: 40, height: 40, background: 'transparent' }, id: 'new' },
    // Fill + outline in one call: the 3px rim sits outside the 20x20 fill.
    { command: 'image.draw.rect', args: { x: 10, y: 10, width: 20, height: 20, filled: true, stroke: 3, color: '#ffd700', strokeColor: '#000000' }, id: 'plate' },
    { command: 'image.pixel', args: { x: 20, y: 20 }, id: 'fill' },
    { command: 'image.pixel', args: { x: 10, y: 20 }, id: 'fillEdge' },
    { command: 'image.pixel', args: { x: 9, y: 20 }, id: 'rim' },
    { command: 'image.pixel', args: { x: 7, y: 20 }, id: 'rimOuter' },
    { command: 'image.pixel', args: { x: 6, y: 20 }, id: 'outside' },
    { command: 'image.info', args: {}, id: 'info' },
    { command: 'image.preview', args: { scale: 5 }, id: 'preview' },
    { command: 'image.preview', args: { scale: 5, format: 'hex', empty: '.' }, id: 'hexPreview' },
    { command: 'image.draw.path', args: { sides: 6, cx: 20, cy: 20, radius: 6, filled: true, stroke: 1, color: '#00ff00', strokeColor: '#004400' }, id: 'hexagon' },
    { command: 'image.info', args: { spans: true }, id: 'spans' },
].map(s => JSON.stringify(s)).join('\n'));
const plate = runBatch(plateBatch);
check(plate.lines.length && plate.lines.every(l => l.ok !== false), 'image preview/shape batch runs clean', plate.stdout + plate.stderr);

const plateStats = plate.byId.info && plate.byId.info.result.stats;
check(plate.byId.fillEdge && plate.byId.fillEdge.result.hex === '#ffd700',
    'the outline never eats into the fill', plate.byId.fillEdge);
check(plate.byId.fill && plate.byId.fill.result.hex === '#ffd700'
    && plate.byId.rim.result.hex === '#000000' && plate.byId.rim.result.a === 255
    && plate.byId.rimOuter.result.hex === '#000000' && plate.byId.rimOuter.result.a === 255
    && plate.byId.outside.result.a === 0,
    'the outline is drawn outside the shape boundary',
    [plate.byId.fill, plate.byId.rim, plate.byId.rimOuter, plate.byId.outside]);
check(plateStats && JSON.stringify(plateStats.bounds) === '{"x":7,"y":7,"width":26,"height":26}',
    'image.info reports the content bounding box', plateStats && plateStats.bounds);
check(plateStats && Math.abs(plateStats.coverage - 676 / 1600) < 1e-9,
    'image.info reports coverage', plateStats && plateStats.coverage);
check(plateStats && plateStats.mirrorLeftRight === true && plateStats.mirrorLeftRightMismatch === 0
    && plateStats.mirrorTopBottom === true && plateStats.mirrorTopBottomMismatch === 0,
    'image.info verifies mirror symmetry', plateStats && {
        lr: plateStats.mirrorLeftRight, lrm: plateStats.mirrorLeftRightMismatch,
        tb: plateStats.mirrorTopBottom, tbm: plateStats.mirrorTopBottomMismatch,
    });
check(plateStats && plateStats.touchesEdge && !plateStats.touchesEdge.left && !plateStats.touchesEdge.right
    && !plateStats.touchesEdge.top && !plateStats.touchesEdge.bottom,
    'image.info flags content clipping at the canvas edge', plateStats && plateStats.touchesEdge);
check(plateStats && plateStats.topColors.length === 2 && plateStats.topColors[0].hex === '#ffdd00'
    && plateStats.topColors[0].count === 400,
    'image.info lists the dominant colors', plateStats && plateStats.topColors);

const preview = plate.byId.preview && plate.byId.preview.result;
check(preview && preview.cols === 8 && preview.rows === 8 && preview.lines.length === 8
    && preview.lines.every(l => l.length === 8),
    'image.preview samples the image into a text grid', preview);
check(preview && preview.lines.join('').includes('#') && preview.lines[0][0] === ' ',
    'image.preview distinguishes content from transparency', preview && preview.lines);

// A hex cell is 7 characters wide, so empty cells are padded; otherwise a row
// with transparency in it is shorter than its neighbours and nothing lines up.
const hexPreview = plate.byId.hexPreview && plate.byId.hexPreview.result;
check(hexPreview && hexPreview.lines.every(l => l.length === hexPreview.cols * 8 - 1)
    && hexPreview.lines[0].startsWith('.......'),
    'image.preview --format hex pads transparent cells', hexPreview && hexPreview.lines.map(l => l.length));

check(plate.byId.hexagon && plate.byId.hexagon.result.pixels === 6,
    'image.draw.path builds a regular polygon from --sides', plate.byId.hexagon);
const spans = plate.byId.spans && plate.byId.spans.result.stats;
check(spans && spans.rowSpans.length === 40 && spans.colSpans.length === 40
    && spans.rowSpans[5] === null && Array.isArray(spans.rowSpans[20]),
    'image.info --spans reports per-row/column extents', spans && { r0: spans.rowSpans[5], r20: spans.rowSpans[20] });

// An open polyline is not closed; the same points with the default `closed`
// paint the closing edge. Sampling one pixel on that edge is the A/B proof.
const pathBatch = path.join(scratch, 'image-path.jsonl');
fs.writeFileSync(pathBatch, [
    { command: 'image.new', args: { width: 48, height: 48, background: 'transparent' }, id: 'new' },
    { command: 'image.draw.path', args: { points: '8,8 40,8 40,40', closed: false, stroke: 2, color: '#00ffff' }, id: 'open' },
    { command: 'image.pixel', args: { x: 24, y: 25 }, id: 'openPixel' },
    { command: 'image.draw.path', args: { points: '8,8 40,8 40,40', stroke: 2, color: '#ff00ff' }, id: 'closed' },
    { command: 'image.pixel', args: { x: 24, y: 25 }, id: 'closedPixel' },
    { command: 'image.draw.path', args: { points: '8,8 40,8 40,40', filled: true, color: '#224466' }, id: 'triangle' },
    { command: 'image.pixel', args: { x: 30, y: 20 }, id: 'insidePixel' },
].map(s => JSON.stringify(s)).join('\n'));
const pathRun = runBatch(pathBatch);
check(pathRun.lines.length && pathRun.lines.every(l => l.ok !== false), 'image path batch runs clean', pathRun.stdout + pathRun.stderr);
check(pathRun.byId.open && pathRun.byId.open.result.pixels === 3 && pathRun.byId.openPixel.result.a === 0,
    '--closed false leaves the path open', [pathRun.byId.open, pathRun.byId.openPixel]);
check(pathRun.byId.closedPixel && pathRun.byId.closedPixel.result.hex === '#ff00ff',
    'a closed path outlines the closing edge', pathRun.byId.closedPixel);
check(pathRun.byId.insidePixel && pathRun.byId.insidePixel.result.hex === '#224466' && pathRun.byId.insidePixel.result.a === 255,
    'image.draw.path fills the polygon interior', pathRun.byId.insidePixel);

expectFail(['image.info'], 'usage');
expectFail(['image.draw.path'], 'invalid_arg');
expectFail(['image.draw.path', '--points', '3,4'], 'invalid_arg');
expectFail(['--project', projectDir, '--image', 'Assets/Textures/cli_texture.png', 'image.info', '--layer', '7'], 'not_found');
expectFail(['--project', projectDir, '--image', 'Assets/Textures/cli_texture.png', 'image.preview', '--scale', '0'], 'invalid_arg');
expectFail(['--project', projectDir, '--image', 'Assets/Textures/cli_texture.png', 'image.draw.rect', '--x', '0', '--y', '0', '--width', '4', '--height', '4', '--stroke', '-1'], 'invalid_arg');

// ---------------------------------------------------------------------------
console.log('cli: image transforms + composition');
// ---------------------------------------------------------------------------
// Symmetry and composition: `--mirror` paints the reflection in the same call
// (a symmetric character is drawn from one half), flip/rotate/crop reshape the
// whole document, and image.stamp blits another image or layer in.
const mirrorBatch = path.join(scratch, 'image-mirror.jsonl');
fs.writeFileSync(mirrorBatch, [
    { command: 'image.new', args: { width: 48, height: 48, background: 'transparent' }, id: 'new' },
    { command: 'image.draw.rect', args: { x: 4, y: 20, width: 12, height: 8, filled: true, color: '#3a7de0', mirror: 'x' }, id: 'rect' },
    { command: 'image.info', args: {}, id: 'info' },
    { command: 'image.pixel', args: { x: 4, y: 24 }, id: 'left' },
    { command: 'image.pixel', args: { x: 43, y: 24 }, id: 'right' },
    { command: 'image.pixel', args: { x: 3, y: 24 }, id: 'before' },
    { command: 'image.pixel', args: { x: 44, y: 24 }, id: 'after' },
    // The same call without --mirror, as the A/B: it stays on one side.
    { command: 'image.new', args: { width: 48, height: 48, background: 'transparent' }, id: 'new2' },
    { command: 'image.draw.rect', args: { x: 4, y: 20, width: 12, height: 8, filled: true, color: '#3a7de0' }, id: 'rect2' },
    { command: 'image.info', args: {}, id: 'info2' },
    // --points reflection (a triangle whose mirror lands across the centre).
    { command: 'image.new', args: { width: 40, height: 40, background: 'transparent' }, id: 'new3' },
    { command: 'image.draw.path', args: { points: '4,30 12,8 20,30', filled: true, color: '#3a7de0', mirror: 'x' }, id: 'tri' },
    { command: 'image.info', args: {}, id: 'info3' },
].map(s => JSON.stringify(s)).join('\n'));
const mirrorRun = runBatch(mirrorBatch);
check(mirrorRun.lines.length && mirrorRun.lines.every(l => l.ok !== false), 'image mirror batch runs clean', mirrorRun.stdout + mirrorRun.stderr);
check(mirrorRun.byId.rect && mirrorRun.byId.rect.result.mirrored === 'x',
    'a mirrored draw reports which mirror it used', mirrorRun.byId.rect);
const mirrorStats = mirrorRun.byId.info && mirrorRun.byId.info.result.stats;
check(mirrorStats && mirrorStats.mirrorLeftRight === true && mirrorStats.mirrorLeftRightMismatch === 0,
    '--mirror x paints a left/right symmetric shape', mirrorStats && mirrorStats.mirrorLeftRightMismatch);
check(mirrorRun.byId.left.result.a === 255 && mirrorRun.byId.right.result.a === 255
    && mirrorRun.byId.before.result.a === 0 && mirrorRun.byId.after.result.a === 0,
    '--mirror x lands the copy exactly across the centre line',
    [mirrorRun.byId.left, mirrorRun.byId.right, mirrorRun.byId.before, mirrorRun.byId.after]);
check(mirrorRun.byId.info2.result.stats.mirrorLeftRight === false,
    'the same rect without --mirror stays one-sided', mirrorRun.byId.info2.result.stats.mirrorLeftRightMismatch);
check(mirrorRun.byId.info3.result.stats.mirrorLeftRight === true
    && mirrorRun.byId.info3.result.stats.mirrorLeftRightMismatch === 0,
    '--mirror x reflects --points geometry too', mirrorRun.byId.info3.result.stats.mirrorLeftRightMismatch);

const xformBatch = path.join(scratch, 'image-xform.jsonl');
fs.writeFileSync(xformBatch, [
    { command: 'image.new', args: { width: 8, height: 4, background: 'transparent' }, id: 'new' },
    { command: 'image.draw.rect', args: { x: 1, y: 0, width: 1, height: 1, filled: true, color: '#00ff00' }, id: 'mark' },
    { command: 'image.flip', args: { axis: 'x' }, id: 'flipx' },
    { command: 'image.pixel', args: { x: 6, y: 0 }, id: 'fx6' },
    { command: 'image.pixel', args: { x: 1, y: 0 }, id: 'fx1' },
    { command: 'image.rotate', args: { degrees: 90 }, id: 'rot' },
    { command: 'image.pixel', args: { x: 3, y: 6 }, id: 'r36' },
    { command: 'image.crop', args: { x: 2, y: 4, width: 2, height: 4 }, id: 'crop' },
    { command: 'image.pixel', args: { x: 1, y: 2 }, id: 'c12' },
    { command: 'image.info', args: {}, id: 'info' },
].map(s => JSON.stringify(s)).join('\n'));
const xformRun = runBatch(xformBatch);
check(xformRun.lines.length && xformRun.lines.every(l => l.ok !== false), 'image transform batch runs clean', xformRun.stdout + xformRun.stderr);
check(xformRun.byId.fx6.result.a === 255 && xformRun.byId.fx1.result.a === 0
    && xformRun.byId.flipx.result.axis === 'x',
    'image.flip --axis x swaps the columns', [xformRun.byId.flipx, xformRun.byId.fx6, xformRun.byId.fx1]);
check(xformRun.byId.rot.result.width === 4 && xformRun.byId.rot.result.height === 8
    && xformRun.byId.r36.result.hex === '#00ff00',
    'image.rotate 90 swaps the canvas size and turns the mark clockwise',
    [xformRun.byId.rot.result, xformRun.byId.r36]);
check(xformRun.byId.crop.result.width === 2 && xformRun.byId.crop.result.height === 4
    && xformRun.byId.c12.result.hex === '#00ff00',
    'image.crop re-frames the mark', [xformRun.byId.crop.result, xformRun.byId.c12]);
check(xformRun.byId.info.result.width === 2 && xformRun.byId.info.result.height === 4
    && xformRun.byId.info.result.stats.coverage === 0.125,
    'image.crop resizes the document every layer shares', xformRun.byId.info.result.stats.coverage);

const stampBatch = path.join(scratch, 'image-stamp.jsonl');
fs.writeFileSync(stampBatch, [
    { command: 'image.new', args: { width: 4, height: 4, background: '#00ff00' }, id: 'dot' },
    { command: 'image.save', args: { out: 'Assets/Textures/stamp_dot.png' }, id: 'sd' },
    { command: 'image.new', args: { width: 4, height: 4, background: 'transparent' }, id: 'hole' },
    { command: 'image.save', args: { out: 'Assets/Textures/stamp_hole.png' }, id: 'sh' },
    // Stamp a file at an offset, then the same file scaled down.
    { command: 'image.new', args: { width: 16, height: 16, background: '#ffffff' }, id: 'canvas' },
    { command: 'image.stamp', args: { file: 'Assets/Textures/stamp_dot.png', x: 4, y: 4 }, id: 'stamp' },
    { command: 'image.pixel', args: { x: 5, y: 5 }, id: 'inside' },
    { command: 'image.pixel', args: { x: 3, y: 3 }, id: 'outside' },
    { command: 'image.stamp', args: { file: 'Assets/Textures/stamp_dot.png', x: 0, y: 8, scale: 0.5 }, id: 'scaled' },
    { command: 'image.pixel', args: { x: 0, y: 8 }, id: 'scaledIn' },
    { command: 'image.pixel', args: { x: 2, y: 8 }, id: 'scaledOut' },
    // Stamp another layer with half opacity: 0.5 magenta over white.
    { command: 'image.layer.add', args: { name: 'Source', fill: '#ff00ff' }, id: 'add' },
    { command: 'image.stamp', args: { from: 1, layer: 0, opacity: 0.5 }, id: 'fromLayer' },
    { command: 'image.pixel', args: { x: 10, y: 10, layer: 0 }, id: 'blended' },
    // A fully transparent source: `replace` punches a hole, `over` is a no-op.
    { command: 'image.new', args: { width: 16, height: 16, background: '#ffffff' }, id: 'canvas2' },
    { command: 'image.stamp', args: { file: 'Assets/Textures/stamp_hole.png', x: 0, y: 0, blend: 'replace' }, id: 'punch' },
    { command: 'image.pixel', args: { x: 1, y: 1 }, id: 'punched' },
    { command: 'image.stamp', args: { file: 'Assets/Textures/stamp_hole.png', x: 12, y: 12 }, id: 'noop' },
    { command: 'image.pixel', args: { x: 13, y: 13 }, id: 'noopPixel' },
    // An offset that hangs off the canvas keeps only the visible part.
    { command: 'image.stamp', args: { file: 'Assets/Textures/stamp_dot.png', x: -2, y: -2 }, id: 'clipped' },
    { command: 'image.pixel', args: { x: 0, y: 0 }, id: 'clippedPixel' },
    { command: 'image.pixel', args: { x: 4, y: 4 }, id: 'clippedOutside' },
].map(s => JSON.stringify(s)).join('\n'));
const stampRun = runBatch(stampBatch);
check(stampRun.lines.length && stampRun.lines.every(l => l.ok !== false), 'image stamp batch runs clean', stampRun.stdout + stampRun.stderr);
check(stampRun.byId.stamp.result.painted === 16 && JSON.stringify(stampRun.byId.stamp.result.source) === '{"width":4,"height":4}'
    && stampRun.byId.inside.result.hex === '#00ff00' && stampRun.byId.outside.result.hex === '#ffffff',
    'image.stamp blits a file at an offset',
    [stampRun.byId.stamp.result, stampRun.byId.inside, stampRun.byId.outside]);
check(stampRun.byId.scaled.result.painted === 4 && stampRun.byId.scaled.result.source.width === 2
    && stampRun.byId.scaledIn.result.hex === '#00ff00' && stampRun.byId.scaledOut.result.hex === '#ffffff',
    'image.stamp --scale 0.5 halves the source',
    [stampRun.byId.scaled.result, stampRun.byId.scaledIn, stampRun.byId.scaledOut]);
check(stampRun.byId.fromLayer.result.painted === 256 && stampRun.byId.blended.result.hex === '#ff80ff'
    && stampRun.byId.blended.result.a === 255,
    'image.stamp --from composites another layer',
    [stampRun.byId.fromLayer.result, stampRun.byId.blended]);
check(stampRun.byId.punch.result.painted === 16 && stampRun.byId.punched.result.a === 0
    && stampRun.byId.noop.result.painted === 0 && stampRun.byId.noopPixel.result.a === 255,
    'image.stamp --blend replace overwrites alpha while over skips transparency',
    [stampRun.byId.punch, stampRun.byId.punched, stampRun.byId.noop, stampRun.byId.noopPixel]);
check(stampRun.byId.clipped.result.painted === 4 && stampRun.byId.clippedPixel.result.hex === '#00ff00'
    && stampRun.byId.clippedOutside.result.hex === '#ffffff',
    'image.stamp clips a source hanging off the canvas',
    [stampRun.byId.clipped.result, stampRun.byId.clippedPixel, stampRun.byId.clippedOutside]);

expectFail(['image.stamp'], 'invalid_arg');
expectFail(['--project', projectDir, 'image.stamp', '--file', 'Assets/Textures/stamp_dot.png', '--from', '0'], 'invalid_arg');
expectFail(['--project', projectDir, 'image.stamp', '--file', 'Assets/Textures/stamp_dot.png', '--scale', '0'], 'invalid_arg');
expectFail(['image.rotate', '--degrees', '45'], 'invalid_arg');
expectFail(['image.crop', '--x', '0', '--y', '0', '--width', '0', '--height', '4'], 'invalid_arg');
expectFail(['--project', projectDir, '--image', 'Assets/Textures/stamp_dot.png', 'image.flip', '--axis', 'x', '--layer', '9'], 'not_found');

// ---------------------------------------------------------------------------
console.log('cli: seeded noise + gradients + palette');
// ---------------------------------------------------------------------------
// Texture assets. The noise field is a pure function of its seed, so a
// generator is reproducible; --tile wraps the lattice so the two edge columns
// of the canvas meet like any two neighbouring columns do.

// One batch paints the same field with two seeds (on two layers) and samples
// the edge columns of every fourth row, which is where a tile seam shows up.
function noiseBatch(file, tile) {
    const steps = [
        { command: 'image.new', args: { width: 32, height: 32, background: 'transparent' }, id: 'new' },
        { command: 'image.draw.noise', args: { kind: 'value', seed: 4, cell: 8, tile, from: '#000000', to: '#ffffff' }, id: 'noise' },
        { command: 'image.preview', args: { format: 'hex', scale: 8 }, id: 'hex' },
        { command: 'image.layer.add', args: { name: 'Seed 5' }, id: 'layer' },
        { command: 'image.draw.noise', args: { kind: 'value', seed: 5, cell: 8, tile, from: '#000000', to: '#ffffff' }, id: 'noise5' },
        { command: 'image.preview', args: { format: 'hex', scale: 8, layer: 1 }, id: 'hex5' },
    ];
    for (const y of [3, 11, 19, 27]) {
        steps.push({ command: 'image.pixel', args: { x: 0, y, layer: 0 }, id: `l${y}` });
        steps.push({ command: 'image.pixel', args: { x: 1, y, layer: 0 }, id: `l${y}b` });
        steps.push({ command: 'image.pixel', args: { x: 30, y, layer: 0 }, id: `r${y}b` });
        steps.push({ command: 'image.pixel', args: { x: 31, y, layer: 0 }, id: `r${y}` });
    }
    fs.writeFileSync(file, steps.map(s => JSON.stringify(s)).join('\n'));
    return file;
}

const texRows = [3, 11, 19, 27];
const tileFile = noiseBatch(path.join(scratch, 'tex-tile.jsonl'), true);
const tileRun = runBatch(tileFile);
const tileAgain = runBatch(tileFile);
const patchRun = runBatch(noiseBatch(path.join(scratch, 'tex-patch.jsonl'), false));
check(tileRun.lines.every(l => l.ok !== false) && patchRun.lines.every(l => l.ok !== false),
    'the noise batch runs clean', tileRun.stdout + tileRun.stderr + patchRun.stdout + patchRun.stderr);
check(tileRun.byId.noise.result.painted === 1024 && tileRun.byId.noise.result.kind === 'value'
    && tileRun.byId.noise.result.tile === true && patchRun.byId.noise.result.tile === false,
    'image.draw.noise fills the canvas and reports the field', [tileRun.byId.noise, patchRun.byId.noise]);
check(tileRun.byId.hex.result.lines.join('\n') === tileAgain.byId.hex.result.lines.join('\n')
    && tileRun.byId.l11.result.hex === tileAgain.byId.l11.result.hex,
    'the same seed paints the same pixels', [tileRun.byId.l11.result, tileAgain.byId.l11.result]);
check(tileRun.byId.hex.result.lines.join('\n') !== tileRun.byId.hex5.result.lines.join('\n'),
    'a different seed paints a different field', tileRun.byId.hex5.result.lines);

// A wrapped lattice makes the gap across the seam the same as the gap between
// two neighbouring columns inside the image; an unwrapped one leaves the two
// edges unrelated.
const seam = (run, y) => Math.abs(run.byId[`l${y}`].result.r - run.byId[`r${y}`].result.r);
const inner = (run, y) => Math.max(
    Math.abs(run.byId[`l${y}`].result.r - run.byId[`l${y}b`].result.r),
    Math.abs(run.byId[`r${y}b`].result.r - run.byId[`r${y}`].result.r));
const tileSeams = texRows.map(y => ({ y, seam: seam(tileRun, y), inner: inner(tileRun, y) }));
// The wrapped seam is one lattice step, exactly like an inner neighbour pair;
// a grey level of slack covers 8-bit rounding.
check(tileSeams.every(t => t.seam <= t.inner + 1 && t.seam <= 16),
    '--tile makes the edge columns meet', tileSeams);
const tileMax = Math.max(...texRows.map(y => seam(tileRun, y)));
const patchMax = Math.max(...texRows.map(y => seam(patchRun, y)));
check(patchMax > tileMax, 'without --tile the two edges do not line up', { tile: tileMax, patch: patchMax });

// Gradients: the default axis runs top -> bottom, --angle 0 turns it left ->
// right, a radial one starts at its centre, and a region leaves the rest alone.
const gradBatch = path.join(scratch, 'tex-grad.jsonl');
fs.writeFileSync(gradBatch, [
    { command: 'image.new', args: { width: 16, height: 16, background: 'transparent' }, id: 'new' },
    { command: 'image.draw.gradient', args: { from: '#000000', to: '#ffffff' }, id: 'vertical' },
    { command: 'image.pixel', args: { x: 8, y: 0, layer: 0 }, id: 'top' },
    { command: 'image.pixel', args: { x: 8, y: 15, layer: 0 }, id: 'bottom' },
    { command: 'image.layer.add', args: { name: 'Horizontal' }, id: 'layerH' },
    { command: 'image.draw.gradient', args: { angle: 0, from: '#000000', to: '#ffffff' }, id: 'horizontal' },
    { command: 'image.pixel', args: { x: 0, y: 8, layer: 1 }, id: 'left' },
    { command: 'image.pixel', args: { x: 15, y: 8, layer: 1 }, id: 'right' },
    { command: 'image.layer.add', args: { name: 'Radial' }, id: 'layerR' },
    { command: 'image.draw.gradient', args: { kind: 'radial', cx: 8, cy: 8, radius: 8, from: '#000000', to: '#ffffff' }, id: 'radial' },
    { command: 'image.pixel', args: { x: 8, y: 8, layer: 2 }, id: 'centre' },
    { command: 'image.pixel', args: { x: 15, y: 8, layer: 2 }, id: 'radialEdge' },
    { command: 'image.layer.add', args: { name: 'Region' }, id: 'layerG' },
    { command: 'image.draw.gradient', args: { x: 4, y: 4, width: 8, height: 8, from: '#ff0000', to: '#00ff00' }, id: 'region' },
    { command: 'image.pixel', args: { x: 4, y: 4, layer: 3 }, id: 'inRegion' },
    { command: 'image.pixel', args: { x: 2, y: 2, layer: 3 }, id: 'outsideRegion' },
    { command: 'image.layer.add', args: { name: 'Diagonal' }, id: 'layerD' },
    { command: 'image.draw.gradient', args: { x1: 0, y1: 0, x2: 15, y2: 15, from: '#000000', to: '#ffffff' }, id: 'diagonal' },
    { command: 'image.pixel', args: { x: 0, y: 0, layer: 4 }, id: 'diagStart' },
    { command: 'image.pixel', args: { x: 15, y: 15, layer: 4 }, id: 'diagEnd' },
    { command: 'image.draw.gradient', args: { kind: 'radial', radius: 0 }, id: 'badRadius' },
    { command: 'image.draw.gradient', args: { x1: 5, y1: 5, x2: 5, y2: 5 }, id: 'badAxis' },
].map(s => JSON.stringify(s)).join('\n'));
// The two bad steps at the end are deliberate, so keep going past them; a
// failing step is reported by index without its id.
const gradRun = runBatch(gradBatch, ['--stopOnError=false']);
const gradFailures = gradRun.lines.find(l => l.command === 'exec.run').result.failures;
check(gradRun.lines.filter(l => l.id).every(l => l.ok !== false),
    'the gradient batch runs clean', gradRun.stdout + gradRun.stderr);
check(gradRun.byId.top.result.r < gradRun.byId.bottom.result.r
    && gradRun.byId.top.result.r < 16 && gradRun.byId.bottom.result.r > 240,
    'a linear gradient runs top to bottom by default', [gradRun.byId.top.result, gradRun.byId.bottom.result]);
check(gradRun.byId.left.result.r < gradRun.byId.right.result.r
    && gradRun.byId.left.result.r < 16 && gradRun.byId.right.result.r > 240,
    '--angle 0 turns the gradient left to right', [gradRun.byId.left.result, gradRun.byId.right.result]);
check(gradRun.byId.centre.result.r < 40 && gradRun.byId.radialEdge.result.r > 220,
    'a radial gradient starts dark at its centre', [gradRun.byId.centre.result, gradRun.byId.radialEdge.result]);
check(gradRun.byId.region.result.painted === 64 && gradRun.byId.inRegion.result.a === 255
    && gradRun.byId.inRegion.result.r > gradRun.byId.inRegion.result.g
    && gradRun.byId.outsideRegion.result.a === 0,
    'a gradient region leaves the rest of the canvas alone',
    [gradRun.byId.region.result, gradRun.byId.inRegion.result, gradRun.byId.outsideRegion.result]);
check(gradRun.byId.diagStart.result.r < gradRun.byId.diagEnd.result.r && gradRun.byId.diagEnd.result.r === 255,
    'explicit endpoints drive the gradient', [gradRun.byId.diagStart.result, gradRun.byId.diagEnd.result]);
check(gradFailures.length === 2 && gradFailures.every(f => f.code === 'invalid_arg')
    && gradFailures.map(f => f.index).join(',') === '20,21',
    'a degenerate gradient is rejected', gradFailures);

// Palette: median cut finds the colours an image actually uses (exactly, for a
// flat sprite), an explicit palette snaps to that palette, and dithering stays
// inside the palette while breaking the bands up.
const palBatch = path.join(scratch, 'tex-pal.jsonl');
fs.writeFileSync(palBatch, [
    { command: 'image.new', args: { width: 32, height: 32, background: 'transparent' }, id: 'new' },
    { command: 'image.draw.rect', args: { x: 0, y: 0, width: 16, height: 32, filled: true, color: '#ff0000' }, id: 'red' },
    { command: 'image.draw.rect', args: { x: 16, y: 0, width: 16, height: 16, filled: true, color: '#0000ff' }, id: 'blue' },
    { command: 'image.draw.rect', args: { x: 16, y: 16, width: 16, height: 16, filled: true, color: '#00ff00' }, id: 'green' },
    { command: 'image.palette', args: { colors: 8, list: true }, id: 'list' },
    { command: 'image.palette', args: { colors: 2 }, id: 'quant' },
    { command: 'image.pixel', args: { x: 8, y: 8, layer: 0 }, id: 'afterRed' },
    { command: 'image.pixel', args: { x: 24, y: 8, layer: 0 }, id: 'afterBlue' },
    { command: 'image.pixel', args: { x: 24, y: 24, layer: 0 }, id: 'afterGreen' },
    { command: 'image.palette', args: { colors: 256, list: true, layer: 0 }, id: 'afterList' },
    { command: 'image.layer.add', args: { name: 'Grey' }, id: 'greyLayer' },
    { command: 'image.draw.rect', args: { x: 0, y: 0, width: 32, height: 32, filled: true, color: '#808080' }, id: 'grey' },
    { command: 'image.palette', args: { palette: '#000000,#ffffff', dither: 'floyd', layer: 1 }, id: 'dither' },
    { command: 'image.palette', args: { palette: '#000000,#ffffff', list: true, layer: 1 }, id: 'ditherList' },
    { command: 'image.pixel', args: { x: 0, y: 0, layer: 1 }, id: 'd0' },
    { command: 'image.pixel', args: { x: 1, y: 0, layer: 1 }, id: 'd1' },
    { command: 'image.pixel', args: { x: 7, y: 7, layer: 1 }, id: 'd2' },
    { command: 'image.pixel', args: { x: 31, y: 31, layer: 1 }, id: 'd3' },
    { command: 'image.layer.add', args: { name: 'Empty' }, id: 'emptyLayer' },
    { command: 'image.palette', args: { colors: 4, list: true, layer: 2 }, id: 'emptyList' },
    { command: 'image.palette', args: { colors: 4, layer: 2 }, id: 'emptyQuant' },
].map(s => JSON.stringify(s)).join('\n'));
const palRun = runBatch(palBatch);
check(palRun.lines.every(l => l.ok !== false), 'the palette batch runs clean', palRun.stdout + palRun.stderr);

const palList = palRun.byId.list.result;
check(palList && palList.colors === 3 && palList.total === 1024
    && JSON.stringify(palList.palette) === JSON.stringify([
        { hex: '#ff0000', count: 512 }, { hex: '#0000ff', count: 256 }, { hex: '#00ff00', count: 256 },
    ]),
    'image.palette --list reports the exact colours of a flat image', palList);

const quant = palRun.byId.quant.result;
const quantHexes = quant && quant.palette;
check(quant && quant.colors === 2 && quant.dither === 'none' && quant.changed > 0
    && quantHexes.includes(palRun.byId.afterRed.result.hex)
    && quantHexes.includes(palRun.byId.afterBlue.result.hex)
    && quantHexes.includes(palRun.byId.afterGreen.result.hex),
    'image.palette snaps every pixel onto the palette',
    [quant, palRun.byId.afterRed.result, palRun.byId.afterBlue.result, palRun.byId.afterGreen.result]);
check(palRun.byId.afterList.result.colors === 2 && palRun.byId.afterList.result.total === 1024,
    'quantizing leaves no more colours than the palette has', palRun.byId.afterList.result);

const ditherList = palRun.byId.ditherList.result;
const ditherHexes = ['d0', 'd1', 'd2', 'd3'].map(id => palRun.byId[id].result.hex);
check(palRun.byId.dither.result.colors === 2 && ditherHexes.every(h => h === '#000000' || h === '#ffffff')
    && ditherList.palette.every(p => p.count > 0) && ditherList.total === 1024,
    'an explicit palette with Floyd-Steinberg dithers within the palette',
    [palRun.byId.dither.result, ditherHexes, ditherList]);

check(palRun.byId.emptyList.result.colors === 0 && palRun.byId.emptyList.result.total === 0
    && palRun.byId.emptyQuant.result.changed === 0,
    'a fully transparent layer has no palette', [palRun.byId.emptyList.result, palRun.byId.emptyQuant.result]);

expectFail(['image.draw.noise', '--cell', '0'], 'invalid_arg');
expectFail(['image.draw.noise', '--octaves', '0'], 'invalid_arg');
expectFail(['image.draw.noise', '--lacunarity', '0.5'], 'invalid_arg');
expectFail(['image.draw.noise', '--gain', '2'], 'invalid_arg');
expectFail(['image.palette', '--colors', '0'], 'invalid_arg');
expectFail(['image.palette', '--colors', '300'], 'invalid_arg');
expectFail(['image.palette', '--palette', '#zzzzzz'], 'invalid_arg');
expectFail(['image.draw.noise'], 'usage');

// ---------------------------------------------------------------------------
console.log('cli: opacity over transparency');
// ---------------------------------------------------------------------------
// `blendPixel` used to blend a semi-transparent stroke against a transparent
// pixel's RGB as `src * a`, so a 50% white fill on an empty layer stored grey at
// 50% alpha and `composite()` darkened it a second time (it came out 25% grey
// over black). The colour is now weighted by the destination's own alpha -
// identical for an opaque destination - while the alpha still uses max(), so a
// repeated stroke cannot build up coverage.
const opTransparent = runBatch(writeBatch('image-opacity.jsonl', [
    { command: 'image.new', args: { width: 8, height: 8, background: 'transparent' }, id: 'new' },
    { command: 'image.draw.rect', args: { x: 0, y: 0, width: 8, height: 8, filled: true, color: '#ffffff', opacity: 0.5 }, id: 'half' },
    { command: 'image.pixel', args: { x: 4, y: 4 }, id: 'stored' },
    { command: 'image.draw.rect', args: { x: 0, y: 0, width: 8, height: 8, filled: true, color: '#ffffff', opacity: 0.5 }, id: 'again' },
    { command: 'image.pixel', args: { x: 4, y: 4, layer: 0 }, id: 'storedAgain' },
    { command: 'image.layer.add', args: { name: 'Backdrop' }, id: 'backdrop' },
    { command: 'image.layer.move', args: { index: 1, to: 0 }, id: 'move' },
    { command: 'image.draw.rect', args: { x: 0, y: 0, width: 8, height: 8, filled: true, layer: 0, color: '#000000' }, id: 'black' },
    { command: 'image.pixel', args: { x: 4, y: 4 }, id: 'overBlack' },
    { command: 'image.draw.rect', args: { x: 0, y: 0, width: 8, height: 8, filled: true, layer: 1, color: '#ff0000' }, id: 'cover' },
    { command: 'image.pixel', args: { x: 4, y: 4 }, id: 'covered' },
]));
check(opTransparent.lines.every(l => l.ok !== false),
    'the opacity batch runs clean', opTransparent.stdout + opTransparent.stderr);
const storedHalf = opTransparent.byId.stored.result;
check(storedHalf.hex === '#ffffff' && storedHalf.a === 128,
    'a 50% fill on a transparent layer stores the colour, not the colour times the alpha',
    storedHalf);
const storedAgain = opTransparent.byId.storedAgain.result;
check(storedAgain.hex === '#ffffff' && storedAgain.a === 128,
    'a repeated 50% stroke does not build up alpha', storedAgain);
const overBlack = opTransparent.byId.overBlack.result;
check(overBlack.hex === '#808080' && overBlack.a === 255,
    '50% white over an opaque black backdrop composites to 50% grey', overBlack);
const covered = opTransparent.byId.covered.result;
check(covered.hex === '#ff0000' && covered.a === 255,
    'an opaque stroke still covers a semi-transparent one', covered);

const opOpaque = runBatch(writeBatch('image-opacity-opaque.jsonl', [
    { command: 'image.new', args: { width: 8, height: 8, background: '#000000' }, id: 'new' },
    { command: 'image.draw.rect', args: { x: 0, y: 0, width: 8, height: 8, filled: true, color: '#ffffff', opacity: 0.5 }, id: 'half' },
    { command: 'image.pixel', args: { x: 4, y: 4, layer: 0 }, id: 'blend' },
]));
const opaqueBlend = opOpaque.byId.blend.result;
check(opaqueBlend.hex === '#808080' && opaqueBlend.a === 255,
    'painting over an opaque layer blends exactly as it always did', opaqueBlend);

// ---------------------------------------------------------------------------
console.log('cli: drawing tutorial');
// ---------------------------------------------------------------------------
// docs/drawing.md is a walkthrough, not prose: both recipes are replayed here
// step for step, so the chapter cannot drift from the implementation. The coin
// is the loop (a flat highlight caught by image.preview/image.info and fixed
// with --mirror), the tile is a seam that is measured rather than eyeballed.

const tutorialCoin = writeBatch('tutorial-coin.jsonl', [
    { command: 'image.new', args: { width: 32, height: 32, background: 'transparent' }, id: 'new' },
    { command: 'image.draw.ellipse', args: { cx: 16, cy: 16, rx: 13, ry: 13, filled: true, stroke: 1, color: '#f2c14e', strokeColor: '#8a5a1a' }, id: 'disc' },
    { command: 'image.layer.add', args: { name: 'Gloss' }, id: 'glossLayer' },
    { command: 'image.draw.rect', args: { x: 6, y: 7, width: 20, height: 4, filled: true, color: '#fff0b3', opacity: 0.85 }, id: 'rect' },
    { command: 'image.preview', args: { scale: 1 }, id: 'prev1' },
    { command: 'image.pixel', args: { x: 16, y: 7 }, id: 'px1' },
    { command: 'image.info', args: {}, id: 'info1' },
    { command: 'image.layer.delete', args: { index: 1 }, id: 'dropGloss' },
    { command: 'image.layer.add', args: { name: 'Gloss' }, id: 'glossLayer2' },
    { command: 'image.draw.ellipse', args: { cx: 16, cy: 11, rx: 8, ry: 3, filled: true, color: '#fff0b3', opacity: 0.85, mirror: 'y' }, id: 'gloss' },
    { command: 'image.pixel', args: { x: 16, y: 7 }, id: 'px2' },
    { command: 'image.pixel', args: { x: 16, y: 10 }, id: 'topHighlight' },
    { command: 'image.pixel', args: { x: 16, y: 21 }, id: 'bottomHighlight' },
    { command: 'image.info', args: {}, id: 'info2' },
    { command: 'image.draw.path', args: { sides: 6, cx: 16, cy: 16, radius: 7, inner: 0.5, filled: true, layer: 0, color: '#a8701c' }, id: 'star' },
    { command: 'image.pixel', args: { x: 16, y: 14 }, id: 'emblem' },
    { command: 'image.info', args: {}, id: 'info3' },
    { command: 'image.preview', args: { scale: 1 }, id: 'prev2' },
    { command: 'image.save', args: { out: 'Assets/Sprites/coin.png' }, id: 'save' },
]);
const coin = runProjectBatch(tutorialCoin);
check(coin.lines.every(l => l.ok !== false),
    'the tutorial coin batch runs clean', coin.stdout + coin.stderr);

const coinStats = id => coin.step(id).result.stats;
const attempt = coinStats('info1');
check(attempt.mirrorLeftRight === true && attempt.mirrorLeftRightMismatch === 0
    && attempt.mirrorTopBottom === false && attempt.mirrorTopBottomMismatch > 0
    && Math.abs(attempt.coverage - 616 / 1024) < 1e-9,
    'the flat highlight is caught by the symmetry check', attempt);
const px1 = coin.step('px1').result;
const px2 = coin.step('px2').result;
check(px1.hex !== '#f2c14e' && px2.hex === '#f2c14e',
    'the fixed highlight leaves the coin body at the top edge', { before: px1, after: px2 });
const attemptRows = coin.step('prev1').result.lines;
check(attemptRows.length === 32 && attemptRows[7].length === 32
    && !attemptRows[7].includes('#') && attemptRows[7].includes('%'),
    'the preview shows the highlight flattening the top of the disc', attemptRows[7]);
const fixedRows = coin.step('prev2').result.lines;
check(fixedRows[7].includes('#') && !fixedRows[7].includes('%'),
    'after the fix the preview shows the rim again', fixedRows[7]);
const fixed = coinStats('info2');
check(fixed.mirrorLeftRight === true && fixed.mirrorLeftRightMismatch === 0
    && fixed.mirrorTopBottom === true && fixed.mirrorTopBottomMismatch === 0,
    'the mirrored highlight makes the coin top-bottom symmetric', fixed);
const top = coin.step('topHighlight').result;
const bottom = coin.step('bottomHighlight').result;
check(top.hex === bottom.hex && top.hex !== px2.hex,
    'the top and bottom highlights are the same colour', { top, bottom });
const withEmblem = coinStats('info3');
check(withEmblem.mirrorLeftRight === true && withEmblem.mirrorTopBottom === true
    && coin.step('emblem').result.hex === '#a8701c',
    'the six-point emblem keeps the coin symmetric', { withEmblem, emblem: coin.step('emblem').result });
const coinPng = path.join(projectDir, 'Assets', 'Sprites', 'coin.png');
const coinBytes = fs.readFileSync(coinPng);
check(coinBytes.slice(0, 8).equals(Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]))
    && coinBytes.readUInt32BE(16) === 32 && coinBytes.readUInt32BE(20) === 32,
    'image.save writes a 32x32 PNG', { bytes: coinBytes.length });
const coinAgain = runProjectBatch(tutorialCoin);
check(coinAgain.lines.every(l => l.ok !== false)
    && fs.readFileSync(coinPng).equals(coinBytes),
    'the tutorial coin is byte-identical when replayed', coinBytes.length);

// The tile: fractal shading plus per-pixel white grain, then the same shading
// with a periodic value lattice. The seam is the gap between the two edge
// columns; "seamless" means it is no wider than the gap between neighbouring
// columns inside the image (a grey level of slack covers 8-bit rounding).
const TILE_ROWS = [3, 11, 19, 27];
function tileBatch(file, grain) {
    const steps = [
        { command: 'image.new', args: { width: 32, height: 32, background: '#24421b' }, id: 'new' },
        { command: 'image.draw.noise', args: { kind: 'fractal', seed: 7, cell: 8, octaves: 4, tile: true, from: '#24421b', to: '#7fae4e' }, id: 'shade' },
        { command: 'image.draw.noise', args: Object.assign({ opacity: 0.1, from: '#000000', to: '#ffffff' }, grain), id: 'grain' },
    ];
    for (const y of TILE_ROWS) {
        for (const [x, tag] of [[0, 'L'], [1, 'Lb'], [30, 'Rb'], [31, 'R']]) {
            steps.push({ command: 'image.pixel', args: { x, y }, id: `g${tag}${y}` });
        }
    }
    steps.push({ command: 'image.palette', args: { colors: 8, dither: 'ordered', list: true }, id: 'palette' });
    for (const y of TILE_ROWS) {
        for (const [x, tag] of [[0, 'L'], [1, 'Lb'], [30, 'Rb'], [31, 'R']]) {
            steps.push({ command: 'image.pixel', args: { x, y }, id: `q${tag}${y}` });
        }
    }
    steps.push({ command: 'image.save', args: { out: 'Assets/Textures/grass.png' }, id: 'save' });
    return runProjectBatch(writeBatch(file, steps));
}
const gaps = (run, prefix) => TILE_ROWS.map(y => {
    const r = tag => run.step(`${prefix}${tag}${y}`).result.r;
    return {
        y,
        seam: Math.abs(r('L') - r('R')),
        inner: Math.max(Math.abs(r('L') - r('Lb')), Math.abs(r('Rb') - r('R'))),
    };
});

const roughTile = tileBatch('tutorial-tile-rough.jsonl', { kind: 'white', seed: 11 });
check(roughTile.lines.every(l => l.ok !== false),
    'the tile batch runs clean', roughTile.stdout + roughTile.stderr);
const roughGaps = gaps(roughTile, 'g');
check(roughGaps.some(g => g.seam > g.inner),
    'per-pixel grain breaks the tile seam', roughGaps);
check(Math.max(...roughGaps.map(g => g.seam)) === roughGaps[1].seam,
    'the worst wrap is the row the chapter quotes', roughGaps);

const tile = tileBatch('tutorial-tile.jsonl', { kind: 'value', seed: 11, cell: 2, tile: true });
check(tile.lines.every(l => l.ok !== false),
    'the corrected tile batch runs clean', tile.stdout + tile.stderr);
const tiledGaps = gaps(tile, 'g');
check(tiledGaps.every(g => g.seam <= g.inner + 1),
    'a periodic lattice keeps every wrap as tight as an interior neighbour gap', tiledGaps);
const quantGaps = gaps(tile, 'q');
check(quantGaps.every((g, i) => g.seam === tiledGaps[i].seam),
    'quantising does not reopen the seam', { before: tiledGaps, after: quantGaps });
const palette = tile.step('palette').result;
check(palette.colors === 8 && palette.total === 1024 && palette.palette.length === 8,
    'the tile palette reports eight colours for the whole canvas', palette);
const grassPng = path.join(projectDir, 'Assets', 'Textures', 'grass.png');
check(fs.existsSync(grassPng) && fs.readFileSync(grassPng).slice(0, 8)
    .equals(Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a])),
    'image.save writes the tile', grassPng);

// ---------------------------------------------------------------------------
console.log('cli: sprite 2D rendering quality');
// ---------------------------------------------------------------------------
// The loop the project is built around: the agent draws an asset and then sees
// it rendered. A four-quadrant sprite goes through a pixel-perfect 2D camera,
// which pins three shipped bugs end to end — the software rasteriser dropped
// one triangle of every 2D quad (half of every sprite and glyph was missing),
// pixel-perfect sprites were sampled through a filtering, mipmapped sampler,
// and the quad handed its top corners v=1 so every sprite came out upside down.
const spriteShot = path.join(scratch, 'sprite2d.png');
const spriteRun = runProjectBatch(writeBatch('sprite2d.jsonl', [
    { command: 'image.new', args: { width: 8, height: 8, background: 'transparent' } },
    { command: 'image.draw.rect', args: { x: 0, y: 0, width: 4, height: 4, filled: true, color: '#ff0000' } },
    { command: 'image.draw.rect', args: { x: 4, y: 0, width: 4, height: 4, filled: true, color: '#0000ff' } },
    { command: 'image.draw.rect', args: { x: 0, y: 4, width: 4, height: 4, filled: true, color: '#ffffff' } },
    { command: 'image.draw.rect', args: { x: 4, y: 4, width: 4, height: 4, filled: true, color: '#00ff00' } },
    { command: 'image.save', args: { out: 'Assets/Sprites/quad.png' }, id: 'tex' },
    { command: 'scene.new', args: { name: 'Sprite2D' } },
    { command: 'scene.settings.set', args: { key: 'renderBackend', value: 'software' } },
    { command: 'entity.create', args: { name: 'Main', position: [0, 0, 10] }, id: 'cam' },
    { command: 'component.add', args: { id: '$ref:cam', type: 'Camera', props: { orthographic: true, orthoSize: 0.5, clearColor: [0, 0, 0, 1], skyEnabled: false } } },
    { command: 'entity.create', args: { name: 'Sprite' }, id: 'sp' },
    { command: 'component.add', args: { id: '$ref:sp', type: 'SpriteRenderer', props: { texturePath: 'Assets/Sprites/quad.png', pixelsPerUnit: 6, pixelPerfect: true } } },
    { command: 'runtime.play' },
    { command: 'render.screenshot', args: { out: spriteShot, mode: 'game', width: 64, height: 64 }, id: 'shot' },
]));
check(spriteRun.lines.every(l => l.ok !== false), 'the sprite render batch runs clean', spriteRun.stdout + spriteRun.stderr);
const spriteShotStep = spriteRun.step('shot');
check(spriteShotStep && spriteShotStep.result.stats.drawCalls2D === 1,
    'the sprite reaches the 2D pass', spriteShotStep && spriteShotStep.result);

// The sprite overfills the 1x1-unit view, so every pixel is one of the four
// quadrant colours (lit by the ambient term). A missing triangle would leave
// background showing; a filtering sampler would blend neighbouring texels into
// in-between colours. `--list` reports exactly how many distinct colours the
// frame holds, so four is the whole contract in one number.
const spritePx = (x, y) => expectOk(['--image', spriteShot, 'image.pixel', '--x', String(x), '--y', String(y)]);
const spriteQuad = [spritePx(16, 16), spritePx(48, 16), spritePx(16, 48), spritePx(48, 48)];
const dominant = (p, ch) => {
    const v = [p.r, p.g, p.b];
    return v[ch] > v[(ch + 1) % 3] + 10 && v[ch] > v[(ch + 2) % 3] + 10;
};
check([0, 1, 2].every(ch => spriteQuad.some(p => dominant(p, ch))),
    'the sprite shows red, green and blue quadrants', spriteQuad);
check(new Set(spriteQuad.map(p => p.hex)).size === 4
    && spriteQuad.some(p => Math.abs(p.r - p.g) <= 8 && Math.abs(p.g - p.b) <= 14 && p.r > 60),
    'the fourth quadrant is the lit white corner', spriteQuad);
// Up is up. The drawn image is red/blue across its top and white/green across
// its bottom, so the frame must read back the same way round: image.* draws
// y-down and the image's top row has to land at the top of the screen. The
// quad used to hand its *bottom* corners v=0, mirroring every sprite and glyph.
check(dominant(spriteQuad[0], 0), "the sprite's top-left corner is the image's top-left", spriteQuad[0]);
check(dominant(spriteQuad[1], 2), "the sprite's top-right corner is the image's top-right", spriteQuad[1]);
check(spriteQuad[2].r > 60 && Math.abs(spriteQuad[2].r - spriteQuad[2].g) <= 8
    && Math.abs(spriteQuad[2].g - spriteQuad[2].b) <= 14,
    "the sprite's bottom-left corner is the image's (white) bottom-left", spriteQuad[2]);
check(dominant(spriteQuad[3], 1), "the sprite's bottom-right corner is the image's bottom-right", spriteQuad[3]);
const spritePalette = expectOk(['--image', spriteShot, 'image.palette', '--list']);
check(spritePalette && spritePalette.colors === 4,
    'pixel-perfect sampling keeps the sprite to four flat colours', spritePalette);
check(spritePalette && !spritePalette.palette.some(e => e.hex === '#000000'),
    'the sprite covers its whole quad, both triangles', spritePalette);

// ---------------------------------------------------------------------------
console.log('cli: 2D orientation (up is up)');
// ---------------------------------------------------------------------------
// Text and sprites share AppendQuad, but the glyph rect supplies its own uvs,
// so the two mappings need pinning separately. The capital 'T' is the sharpest
// probe there is: its top row is a full cross bar and every row below it is a
// one-texel stem, so a mirrored glyph is unmistakable. Until 0.1.23 the glyph
// quad gave its top corners v = g.v + g.h, which rendered every word upside
// down.
const textShot = path.join(scratch, 'text2d.png');
const textRun = runProjectBatch(writeBatch('text2d.jsonl', [
    { command: 'scene.new', args: { name: 'Text2D' } },
    { command: 'scene.settings.set', args: { key: 'renderBackend', value: 'software' } },
    { command: 'entity.create', args: { name: 'Main', position: [0, 0, 10] }, id: 'cam' },
    { command: 'component.add', args: { id: '$ref:cam', type: 'Camera', props: { orthographic: true, orthoSize: 2, clearColor: [0, 0, 0, 1], skyEnabled: false } } },
    { command: 'entity.create', args: { name: 'Text', position: [-1.25, -1.75, 0] }, id: 'txt' },
    { command: 'component.add', args: { id: '$ref:txt', type: 'TextRenderer', props: { text: 'T', fontSize: 3.5, color: [1, 1, 1, 1] } } },
    { command: 'runtime.play' },
    { command: 'render.screenshot', args: { out: textShot, mode: 'game', width: 64, height: 64 }, id: 'shot' },
]));
check(textRun.lines.every(l => l.ok !== false), 'the text render batch runs clean', textRun.stdout + textRun.stderr);
// The glyph fills screen x 12..52, y 4..60: its top row is the bar, its centre
// column the stem. Reading the bar's left end, the mirror of that point, and
// the stem fixes the glyph's way up.
const textPx = (x, y) => expectOk(['--image', textShot, 'image.pixel', '--x', String(x), '--y', String(y)]);
const litTexel = p => p.r + p.g + p.b > 60;
const barTop = textPx(16, 8);
const barMirror = textPx(16, 56);
const stemBottom = textPx(32, 56);
check(litTexel(barTop), "the 'T' cross bar is at the top", barTop);
check(!litTexel(barMirror), "nothing hangs below the bar's left end", barMirror);
check(litTexel(stemBottom), "the 'T' stem runs down the middle to the bottom", stemBottom);

// ---------------------------------------------------------------------------
console.log('cli: sprite atlas (uv rect + pivot)');
// ---------------------------------------------------------------------------
// A sheet is the prerequisite for 2D animation, and the drawing tools already
// assemble one: four 16x16 frames in a 32x32 PNG. `uvRect` selects one frame -
// (x, y, w, h) as fractions of the texture, rows counted from the top like every
// other image coordinate - and `pivot` places that frame relative to the entity,
// in the same image coordinates. The drawn size is the *frame's* texels over the
// pixels-per-unit, not the whole texture's.
const sheetSteps = [
    { command: 'image.new', args: { width: 32, height: 32, background: 'transparent' } },
    { command: 'image.draw.rect', args: { x: 0, y: 0, width: 16, height: 16, filled: true, color: '#ff0000' } },
    { command: 'image.draw.rect', args: { x: 16, y: 0, width: 16, height: 16, filled: true, color: '#00ff00' } },
    { command: 'image.draw.rect', args: { x: 0, y: 16, width: 16, height: 16, filled: true, color: '#0000ff' } },
    { command: 'image.draw.rect', args: { x: 16, y: 16, width: 16, height: 16, filled: true, color: '#ffffff' } },
    { command: 'image.save', args: { out: 'Assets/Sprites/sheet.png' }, id: 'sheet' },
];
// Both batches render a 2x2-world-unit view (orthoSize 1) at 64x64, so one
// 16-texel frame at 16 px/unit is exactly 32x32 screen pixels.
const atlasCam = orthoSize => ([
    { command: 'entity.create', args: { name: 'Main', position: [0, 0, 10] }, id: 'cam' },
    { command: 'component.add', args: { id: '$ref:cam', type: 'Camera', props: { orthographic: true, orthoSize, clearColor: [0, 0, 0, 1], skyEnabled: false } } },
]);

// One sprite showing the bottom-right frame: the frame the agent asked for has
// to be the one drawn (the sheet's top-left frame is red, a whole-sheet sprite
// would show that), and it has to be one frame big - the view corners stay
// background instead of being covered by the whole 2-unit sheet.
const atlasFrameShot = path.join(scratch, 'atlas_frame.png');
const atlasFrameRun = runProjectBatch(writeBatch('atlas_frame.jsonl', [
    ...sheetSteps,
    { command: 'scene.new', args: { name: 'AtlasFrame' } },
    { command: 'scene.settings.set', args: { key: 'renderBackend', value: 'software' } },
    ...atlasCam(1),
    { command: 'entity.create', args: { name: 'Sprite' }, id: 'sp' },
    { command: 'component.add', args: { id: '$ref:sp', type: 'SpriteRenderer', props: {
        texturePath: 'Assets/Sprites/sheet.png', pixelsPerUnit: 16, pixelPerfect: true,
        uvRect: [0.5, 0.5, 0.5, 0.5] } } },
    { command: 'runtime.play' },
    { command: 'render.screenshot', args: { out: atlasFrameShot, mode: 'game', width: 64, height: 64 }, id: 'shot' },
]));
check(atlasFrameRun.lines.every(l => l.ok !== false), 'the atlas batch runs clean', atlasFrameRun.stdout + atlasFrameRun.stderr);
const atlasPx = (x, y) => expectOk(['--image', atlasFrameShot, 'image.pixel', '--x', String(x), '--y', String(y)]);
const darkPx = p => p.r + p.g + p.b <= 30;
const whitePx = p => p.r > 60 && Math.abs(p.r - p.g) <= 8 && Math.abs(p.g - p.b) <= 14;
const atlasCentre = atlasPx(32, 32);
check(whitePx(atlasCentre), 'uvRect draws the frame it names, not the whole sheet', atlasCentre);
const atlasCorners = [atlasPx(8, 8), atlasPx(56, 8), atlasPx(8, 56), atlasPx(56, 56)];
check(atlasCorners.every(darkPx), 'the sprite is one frame big, not one sheet big', atlasCorners);

// Four tiles, one per frame, each pinned by pivot (0, 1) - the sprite's
// bottom-left in image coordinates - at the corner of its screen quadrant. The
// colour in each quadrant is then the frame that uvRect named: red top-left,
// green top-right, blue bottom-left, white bottom-right.
const atlasQuadShot = path.join(scratch, 'atlas_quad.png');
const tiles = [
    { name: 'Red', pos: [-1, 0, 0], rect: [0, 0, 0.5, 0.5] },
    { name: 'Green', pos: [0, 0, 0], rect: [0.5, 0, 0.5, 0.5] },
    { name: 'Blue', pos: [-1, -1, 0], rect: [0, 0.5, 0.5, 0.5] },
    { name: 'White', pos: [0, -1, 0], rect: [0.5, 0.5, 0.5, 0.5] },
];
const atlasQuadRun = runProjectBatch(writeBatch('atlas_quad.jsonl', [
    { command: 'scene.new', args: { name: 'AtlasQuad' } },
    { command: 'scene.settings.set', args: { key: 'renderBackend', value: 'software' } },
    ...atlasCam(1),
    ...tiles.flatMap((tile, i) => ([
        { command: 'entity.create', args: { name: tile.name, position: tile.pos }, id: `t${i}` },
        { command: 'component.add', args: { id: `$ref:t${i}`, type: 'SpriteRenderer', props: {
            texturePath: 'Assets/Sprites/sheet.png', pixelsPerUnit: 16, pixelPerfect: true,
            uvRect: tile.rect, pivot: [0, 1] } } },
    ])),
    { command: 'runtime.play' },
    { command: 'render.screenshot', args: { out: atlasQuadShot, mode: 'game', width: 64, height: 64 }, id: 'shot' },
]));
check(atlasQuadRun.lines.every(l => l.ok !== false), 'the atlas tiling batch runs clean', atlasQuadRun.stdout + atlasQuadRun.stderr);
const atlasQuadStep = atlasQuadRun.step('shot');
check(atlasQuadStep && atlasQuadStep.result.stats.drawCalls2D === 4,
    'all four frames reach the 2D pass', atlasQuadStep && atlasQuadStep.result);
const atlasQuadPx = (x, y) => expectOk(['--image', atlasQuadShot, 'image.pixel', '--x', String(x), '--y', String(y)]);
const atlasRed = atlasQuadPx(16, 16);
const atlasGreen = atlasQuadPx(48, 16);
const atlasBlue = atlasQuadPx(16, 48);
const atlasWhite = atlasQuadPx(48, 48);
check(dominant(atlasRed, 0), "the top-left frame is the sheet's red top-left quadrant", atlasRed);
check(dominant(atlasGreen, 1), "the top-right frame is the sheet's green top-right quadrant", atlasGreen);
check(dominant(atlasBlue, 2), "the bottom-left frame is the sheet's blue bottom-left quadrant", atlasBlue);
check(whitePx(atlasWhite), "the bottom-right frame is the sheet's white bottom-right quadrant", atlasWhite);

// ---------------------------------------------------------------------------
console.log('cli: sprite culling (the bound is the quad, not the ppu)');
// ---------------------------------------------------------------------------
// A sprite is culled against the quad it actually draws. That bound used to be
// `pixelsPerUnit * 0.5` around the entity, which threw away large sprites - a
// background layer at a pixels-per-unit below one - while their pixels covered
// the whole view, and kept tiny ones whose quad was nowhere near it. One
// texture, two entities: the giant at x = 50 covers the 2-unit view, the small
// one at x = 3 cannot reach it.
const cullShot = path.join(scratch, 'cull.png');
const cullRun = runProjectBatch(writeBatch('cull.jsonl', [
    { command: 'image.new', args: { width: 32, height: 32, background: '#ff0000' } },
    { command: 'image.save', args: { out: 'Assets/Sprites/big.png' }, id: 'tex' },
    { command: 'scene.new', args: { name: 'Cull' } },
    { command: 'scene.settings.set', args: { key: 'renderBackend', value: 'software' } },
    ...atlasCam(1),
    { command: 'entity.create', args: { name: 'Giant', position: [50, 0, 0] }, id: 'giant' },
    { command: 'component.add', args: { id: '$ref:giant', type: 'SpriteRenderer', props: {
        texturePath: 'Assets/Sprites/big.png', pixelsPerUnit: 0.1, pixelPerfect: true } } },
    { command: 'entity.create', args: { name: 'Small', position: [3, 0, 0] }, id: 'small' },
    { command: 'component.add', args: { id: '$ref:small', type: 'SpriteRenderer', props: {
        texturePath: 'Assets/Sprites/big.png', pixelsPerUnit: 100, pixelPerfect: true } } },
    { command: 'runtime.play' },
    { command: 'render.screenshot', args: { out: cullShot, mode: 'game', width: 64, height: 64 }, id: 'shot' },
]));
check(cullRun.lines.every(l => l.ok !== false), 'the culling batch runs clean', cullRun.stdout + cullRun.stderr);
const cullStep = cullRun.step('shot');
check(cullStep && cullStep.result.stats.drawCalls2D === 1 && cullStep.result.stats.frustumCulled === 1,
    'the off-screen sprite is culled and the covering one is drawn', cullStep && cullStep.result.stats);
const cullPx = (x, y) => expectOk(['--image', cullShot, 'image.pixel', '--x', String(x), '--y', String(y)]);
check(dominant(cullPx(32, 32), 0), 'the sprite whose entity is 50 units away still covers the centre', cullPx(32, 32));
check(dominant(cullPx(4, 4), 0), 'and the top-left corner', cullPx(4, 4));
check(dominant(cullPx(60, 60), 0), 'and the bottom-right corner', cullPx(60, 60));

// ---------------------------------------------------------------------------
console.log('cli: sprite animation (a cell per frame, advanced by runtime.step)');
// ---------------------------------------------------------------------------
// The sheet an agent draws is the animation: a SpriteAnimator walks the cells of
// the sprite's grid at `fps` and the renderer draws the current one. One
// `runtime.step` is exactly 1/60 s, so at 60 fps one step is exactly one frame -
// the same frames in the same order on every run.
const animShot = i => path.join(scratch, `anim${i}.png`);
const animSteps = [{ command: 'runtime.play' }];
for (let i = 0; i < 5; i++) {
    if (i > 0) animSteps.push({ command: 'runtime.step' });
    animSteps.push({ command: 'render.screenshot', args: { out: animShot(i), mode: 'game', width: 64, height: 64 }, id: `shot${i}` });
}
const animRun = runProjectBatch(writeBatch('anim.jsonl', [
    { command: 'image.new', args: { width: 32, height: 32, background: 'transparent' } },
    { command: 'image.draw.rect', args: { x: 0, y: 0, width: 16, height: 16, filled: true, color: '#ff0000' } },
    { command: 'image.draw.rect', args: { x: 16, y: 0, width: 16, height: 16, filled: true, color: '#00ff00' } },
    { command: 'image.draw.rect', args: { x: 0, y: 16, width: 16, height: 16, filled: true, color: '#0000ff' } },
    { command: 'image.draw.rect', args: { x: 16, y: 16, width: 16, height: 16, filled: true, color: '#ffffff' } },
    { command: 'image.save', args: { out: 'Assets/Sprites/frames.png' }, id: 'sheet' },
    { command: 'scene.new', args: { name: 'Anim' } },
    { command: 'scene.settings.set', args: { key: 'renderBackend', value: 'software' } },
    ...atlasCam(1),
    { command: 'entity.create', args: { name: 'Sprite' }, id: 'sp' },
    { command: 'component.add', args: { id: '$ref:sp', type: 'SpriteRenderer', props: {
        texturePath: 'Assets/Sprites/frames.png', pixelsPerUnit: 16, pixelPerfect: true } } },
    { command: 'component.add', args: { id: '$ref:sp', type: 'SpriteAnimator', props: {
        columns: 2, rows: 2, fps: 60 } } },
    ...animSteps,
    // Scrub: `frame` is what the renderer reads, so a screenshot with no step in
    // between is that exact cell.
    { command: 'component.setProps', args: { id: '$ref:sp', type: 'SpriteAnimator', props: { frame: 2 } } },
    { command: 'render.screenshot', args: { out: animShot(5), mode: 'game', width: 64, height: 64 }, id: 'scrub' },
]));
check(animRun.lines.every(l => l.ok !== false), 'the animation batch runs clean', animRun.stdout + animRun.stderr);
const animPx = (i, x, y) => expectOk(['--image', animShot(i), 'image.pixel', '--x', String(x), '--y', String(y)]);
check(dominant(animPx(0, 32, 32), 0), 'the first cell is the one drawn before time passes', animPx(0, 32, 32));
check(dominant(animPx(1, 32, 32), 1), 'one step shows the next cell', animPx(1, 32, 32));
check(dominant(animPx(2, 32, 32), 2), 'the third cell is on the second row', animPx(2, 32, 32));
check(whitePx(animPx(3, 32, 32)), 'the fourth cell is the last one of the sheet', animPx(3, 32, 32));
check(dominant(animPx(4, 32, 32), 0), 'the clip loops back to the first cell', animPx(4, 32, 32));
check(dominant(animPx(5, 32, 32), 2), 'setting frame scrubs to that cell', animPx(5, 32, 32));

// ---------------------------------------------------------------------------
console.log('cli: named sprite clips (the clip table picks the frames)');
// ---------------------------------------------------------------------------
// A character is several animations on one sheet, so they are named: `clips` is
// a table of ranges and `clip` names the one playing. Two `runtime.step` calls
// take `walk` through cells 0..1 and back, which is the range the table named -
// cells 2..3 are never shown - and switching to `attack` draws its first cell
// with no step in between, then holds its last one because that clip does not
// loop.
const clipShot = i => path.join(scratch, `clip${i}.png`);
const clipSteps = [{ command: 'runtime.play' }];
for (let i = 0; i < 6; i++) {
    if (i > 0) clipSteps.push({ command: 'runtime.step' });
    if (i === 3) {
        clipSteps.push({ command: 'component.setProps',
            args: { id: '$ref:sp', type: 'SpriteAnimator', props: { clip: 'attack' } } });
    }
    clipSteps.push({ command: 'render.screenshot', args: { out: clipShot(i), mode: 'game', width: 64, height: 64 }, id: `cshot${i}` });
}
const clipRun = runProjectBatch(writeBatch('clips.jsonl', [
    { command: 'image.new', args: { width: 32, height: 32, background: 'transparent' } },
    { command: 'image.draw.rect', args: { x: 0, y: 0, width: 16, height: 16, filled: true, color: '#ff0000' } },
    { command: 'image.draw.rect', args: { x: 16, y: 0, width: 16, height: 16, filled: true, color: '#00ff00' } },
    { command: 'image.draw.rect', args: { x: 0, y: 16, width: 16, height: 16, filled: true, color: '#0000ff' } },
    { command: 'image.draw.rect', args: { x: 16, y: 16, width: 16, height: 16, filled: true, color: '#ffffff' } },
    { command: 'image.save', args: { out: 'Assets/Sprites/clips.png' }, id: 'sheet' },
    { command: 'scene.new', args: { name: 'Clips' } },
    { command: 'scene.settings.set', args: { key: 'renderBackend', value: 'software' } },
    ...atlasCam(1),
    { command: 'entity.create', args: { name: 'Sprite' }, id: 'sp' },
    { command: 'component.add', args: { id: '$ref:sp', type: 'SpriteRenderer', props: {
        texturePath: 'Assets/Sprites/clips.png', pixelsPerUnit: 16, pixelPerfect: true } } },
    { command: 'component.add', args: { id: '$ref:sp', type: 'SpriteAnimator', props: {
        columns: 2, rows: 2, clip: 'walk',
        clips: [
            { name: 'walk', firstFrame: 0, lastFrame: 1, fps: 60 },
            { name: 'attack', firstFrame: 2, lastFrame: 3, fps: 60, loop: false },
        ] } }, id: 'anim' },
    ...clipSteps,
    // The table comes back the way it went in, which is what makes the clip set
    // a thing an agent can read, edit and write back.
    { command: 'component.get', args: { id: '$ref:sp', type: 'SpriteAnimator' }, id: 'read' },
]));
check(clipRun.lines.every(l => l.ok !== false), 'the clip batch runs clean', clipRun.stdout + clipRun.stderr);
const clipPx = (i, x, y) => expectOk(['--image', clipShot(i), 'image.pixel', '--x', String(x), '--y', String(y)]);
check(dominant(clipPx(0, 32, 32), 0), 'the named clip starts at its first cell', clipPx(0, 32, 32));
check(dominant(clipPx(1, 32, 32), 1), 'a step plays the clip\'s second cell', clipPx(1, 32, 32));
check(dominant(clipPx(2, 32, 32), 0), 'and wraps inside the clip, not past it', clipPx(2, 32, 32));
check(dominant(clipPx(3, 32, 32), 2), 'switching clips draws the new clip\'s first cell', clipPx(3, 32, 32));
check(whitePx(clipPx(4, 32, 32)), 'the new clip then plays from there', clipPx(4, 32, 32));
check(dominant(clipPx(5, 32, 32), 0) === false, 'the last cell is not the first one again', clipPx(5, 32, 32));
check(whitePx(clipPx(5, 32, 32)), 'a clip that does not loop holds its last cell', clipPx(5, 32, 32));
const clipRead = clipRun.step('read').result.props;
check(Array.isArray(clipRead.clips) && clipRead.clips.length === 2
    && clipRead.clips[1].name === 'attack' && clipRead.clips[1].loop === false,
    'the clip table round-trips through component.get', clipRead.clips);
check(clipRead.clip === 'attack', 'and so does the clip that is playing', clipRead.clip);
const clipSchema = expectOk(['component.schema', 'SpriteAnimator']);
const clipProp = clipSchema.components[0].schema.find(p => p.name === 'clips');
check(clipProp && clipProp.kind === 'json',
    'component.schema reports the clip table as a json prop', clipProp);

// ---------------------------------------------------------------------------
console.log('cli: sprite animation states (parameters drive the clips)');
// ---------------------------------------------------------------------------
// A state machine instead of a script: `params` are the values the animator
// reads, `transitions` says which named clip to enter for which of them. Each
// `runtime.step` advances one frame and then runs the table in order, so the
// same parameters across the same steps produce the same frames every run.
// `idle` is one cell, `walk` two at 60 fps, `attack` one at 30 fps that does
// not loop - the exit time therefore holds it for two steps before moving on.
const stateShot = i => path.join(scratch, `state${i}.png`);
const stateParams = over => ({ moving: false, speed: 0.25, attack: false, ...over });
const setParams = over => ({ command: 'component.setProps',
    args: { id: '$ref:sp', type: 'SpriteAnimator', props: { params: stateParams(over) } } });
const stateSteps = [{ command: 'runtime.play' }];
const stateShotStep = i => ({ command: 'render.screenshot',
    args: { out: stateShot(i), mode: 'game', width: 64, height: 64 }, id: `s${i}` });
const stateSeq = [
    [stateShotStep(0)],
    [setParams({ moving: true }), { command: 'runtime.step' }, stateShotStep(1)],
    [{ command: 'runtime.step' }, stateShotStep(2)],
    [setParams({ speed: 1 }), { command: 'runtime.step' }, stateShotStep(3)],
    [setParams({}), { command: 'runtime.step' }, stateShotStep(4)],
    [setParams({ attack: true }), { command: 'runtime.step' }, stateShotStep(5)],
    [{ command: 'runtime.step' }, stateShotStep(6)],
    [{ command: 'runtime.step' }, stateShotStep(7)],
];
for (const batch of stateSeq) stateSteps.push(...batch);
const stateRun = runProjectBatch(writeBatch('states.jsonl', [
    { command: 'image.new', args: { width: 32, height: 32, background: 'transparent' } },
    { command: 'image.draw.rect', args: { x: 0, y: 0, width: 16, height: 16, filled: true, color: '#ff0000' } },
    { command: 'image.draw.rect', args: { x: 16, y: 0, width: 16, height: 16, filled: true, color: '#00ff00' } },
    { command: 'image.draw.rect', args: { x: 0, y: 16, width: 16, height: 16, filled: true, color: '#0000ff' } },
    { command: 'image.draw.rect', args: { x: 16, y: 16, width: 16, height: 16, filled: true, color: '#ffffff' } },
    { command: 'image.save', args: { out: 'Assets/Sprites/states.png' }, id: 'sheet' },
    { command: 'scene.new', args: { name: 'States' } },
    { command: 'scene.settings.set', args: { key: 'renderBackend', value: 'software' } },
    ...atlasCam(1),
    { command: 'entity.create', args: { name: 'Sprite' }, id: 'sp' },
    { command: 'component.add', args: { id: '$ref:sp', type: 'SpriteRenderer', props: {
        texturePath: 'Assets/Sprites/states.png', pixelsPerUnit: 16, pixelPerfect: true } } },
    { command: 'component.add', args: { id: '$ref:sp', type: 'SpriteAnimator', props: {
        columns: 2, rows: 2, clip: 'idle',
        clips: [
            { name: 'idle', firstFrame: 0, lastFrame: 0, fps: 60 },
            { name: 'walk', firstFrame: 1, lastFrame: 2, fps: 60 },
            { name: 'attack', firstFrame: 3, lastFrame: 3, fps: 30, loop: false },
        ],
        params: stateParams({}),
        transitions: [
            { from: 'idle', to: 'walk', when: [{ param: 'moving', op: 'is', value: true }] },
            { from: 'walk', to: 'idle', when: [
                { param: 'moving', op: 'is', value: false },
                { param: 'speed', op: 'lt', value: 0.5 }] },
            { from: '*', to: 'attack', when: [{ param: 'attack', op: 'trigger' }] },
            { from: 'attack', to: 'idle', waitForExit: true },
        ] } }, id: 'anim' },
    ...stateSteps,
    { command: 'component.get', args: { id: '$ref:sp', type: 'SpriteAnimator' }, id: 'read' },
]));
check(stateRun.lines.every(l => l.ok !== false), 'the state machine batch runs clean', stateRun.stdout + stateRun.stderr);
const statePx = (i, x, y) => expectOk(['--image', stateShot(i), 'image.pixel', '--x', String(x), '--y', String(y)]);
check(dominant(statePx(0, 32, 32), 0), 'the animator starts in the clip it names', statePx(0, 32, 32));
check(dominant(statePx(1, 32, 32), 1), 'a parameter that holds moves it to that state', statePx(1, 32, 32));
check(dominant(statePx(2, 32, 32), 2), 'and the state plays its own clip from there', statePx(2, 32, 32));
check(dominant(statePx(3, 32, 32), 1), 'one of two conditions holding is not enough', statePx(3, 32, 32));
check(dominant(statePx(4, 32, 32), 0), 'both of them take the machine back', statePx(4, 32, 32));
check(whitePx(statePx(5, 32, 32)), 'a trigger moves it from any state', statePx(5, 32, 32));
check(whitePx(statePx(6, 32, 32)), 'the exit time holds that state while its clip runs', statePx(6, 32, 32));
check(dominant(statePx(7, 32, 32), 0), 'and lets it go once the clip is done', statePx(7, 32, 32));
const stateRead = stateRun.step('read').result.props;
check(stateRead.clip === 'idle', 'the state it ended in is the one it is playing', stateRead.clip);
check(stateRead.params && stateRead.params.attack === false,
    'the trigger is spent by the transition that fired', stateRead.params);
check(stateRead.params.moving === false && stateRead.params.speed === 0.25,
    'and the parameters it did not touch are left alone', stateRead.params);
check(Array.isArray(stateRead.transitions) && stateRead.transitions.length === 4
    && stateRead.transitions[3].waitForExit === true && stateRead.transitions[3].to === 'idle',
    'the transition table round-trips through component.get', stateRead.transitions);
const stateSchema = expectOk(['component.schema', 'SpriteAnimator']);
const jsonProps = stateSchema.components[0].schema.filter(p => p.kind === 'json').map(p => p.name);
check(jsonProps.includes('params') && jsonProps.includes('transitions'),
    'component.schema reports the parameters and the table as json props', jsonProps);

// ---------------------------------------------------------------------------
console.log('cli: per-cell frame times (each cell of a clip has its own time)');
// ---------------------------------------------------------------------------
// One rate for a whole animation cannot say that a wind-up is slow and the
// strike fast, so a clip carries `durations`: a time per cell, counted from the
// clip's own first cell. A rate of 60 fps would run this clip four times in the
// same span, and the component's own table says 0.2 s a cell, so the frames
// below can only be the clip entry's times.
const durShot = i => path.join(scratch, `dur${i}.png`);
const durSteps = [{ command: 'runtime.play' },
    { command: 'render.screenshot', args: { out: durShot(0), mode: 'game', width: 64, height: 64 }, id: 'd0' }];
const durAdvance = (frames, i) => {
    for (let k = 0; k < frames; k++) durSteps.push({ command: 'runtime.step' });   // 1/60 s each
    durSteps.push({ command: 'render.screenshot',
        args: { out: durShot(i), mode: 'game', width: 64, height: 64 }, id: `d${i}` });
};
durAdvance(29, 1);   // 0.483 s: the first cell's own 0.5 s is not up
durAdvance(2, 2);    // 0.517 s: it is
durAdvance(7, 3);    // 0.633 s: the 0.1 s cell after it has gone by
const durRun = runProjectBatch(writeBatch('durations.jsonl', [
    { command: 'image.new', args: { width: 32, height: 32, background: 'transparent' } },
    { command: 'image.draw.rect', args: { x: 0, y: 0, width: 16, height: 16, filled: true, color: '#ff0000' } },
    { command: 'image.draw.rect', args: { x: 16, y: 0, width: 16, height: 16, filled: true, color: '#00ff00' } },
    { command: 'image.draw.rect', args: { x: 0, y: 16, width: 16, height: 16, filled: true, color: '#0000ff' } },
    { command: 'image.draw.rect', args: { x: 16, y: 16, width: 16, height: 16, filled: true, color: '#ffffff' } },
    { command: 'image.save', args: { out: 'Assets/Sprites/times.png' }, id: 'sheet' },
    { command: 'scene.new', args: { name: 'Times' } },
    { command: 'scene.settings.set', args: { key: 'renderBackend', value: 'software' } },
    ...atlasCam(1),
    { command: 'entity.create', args: { name: 'Sprite' }, id: 'sp' },
    { command: 'component.add', args: { id: '$ref:sp', type: 'SpriteRenderer', props: {
        texturePath: 'Assets/Sprites/times.png', pixelsPerUnit: 16, pixelPerfect: true } } },
    { command: 'component.add', args: { id: '$ref:sp', type: 'SpriteAnimator', props: {
        columns: 2, rows: 2, clip: 'walk',
        durations: [0.2, 0.2, 0.2, 0.2],            // the component's own table
        clips: [{ name: 'walk', firstFrame: 0, lastFrame: 3, fps: 60,
                  durations: [0.5, 0.1, 0.1, 0.1] }] } }, id: 'anim' },   // and the clip's, which wins
    ...durSteps,
    { command: 'component.get', args: { id: '$ref:sp', type: 'SpriteAnimator' }, id: 'read' },
]));
check(durRun.lines.every(l => l.ok !== false), 'the per-cell times batch runs clean', durRun.stdout + durRun.stderr);
const durPx = i => expectOk(['--image', durShot(i), 'image.pixel', '--x', '32', '--y', '32']);
const dur0 = durPx(0), dur1 = durPx(1), dur2 = durPx(2), dur3 = durPx(3);
check(dominant(dur0, 0), 'the clip starts on its first cell', dur0);
check(dominant(dur1, 0), "a cell with a longer time is held past the clip's rate", dur1);
check(dominant(dur2, 1), 'its own time is what steps the clip', dur2);
check(dominant(dur3, 2), 'and the quick cell after it is quick', dur3);
const durRead = durRun.step('read').result.props;
check(Array.isArray(durRead.durations) && durRead.durations.length === 4 && durRead.durations[0] === 0.2,
    'the component\'s own table round-trips through component.get', durRead.durations);
const durClipRead = Array.isArray(durRead.clips) ? durRead.clips.find(c => c.name === 'walk') : null;
check(durClipRead && Array.isArray(durClipRead.durations) && durClipRead.durations[0] === 0.5
    && durClipRead.durations[1] === 0.1,
    'and so does the clip\'s, which is the one that governs', durClipRead && durClipRead.durations);
const durSchema = expectOk(['component.schema', 'SpriteAnimator']);
const durProp = durSchema.components[0].schema.find(p => p.name === 'durations');
check(durProp && durProp.kind === 'json', 'component.schema reports the times as a json prop', durProp);

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
