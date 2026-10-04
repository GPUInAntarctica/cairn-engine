#!/usr/bin/env node
// Cairn Engine - build orchestration.
//
//   node scripts/build.js           build engine library + native addon
//   node scripts/build.js engine    engine shared library only (CMake)
//   node scripts/build.js addon     native addon only (C++ -> .node)
//
// The engine is built with CMake; the addon is a plain shared library linked
// against it (rpath keeps the pair together), matching how gameplay scripts
// link the engine later.
'use strict';

const { spawnSync } = require('child_process');
const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..');
const engineDir = path.join(root, 'engine');
const buildDir = path.join(engineDir, 'build');
const buildOut = path.join(root, 'build');
const force = process.argv.includes('--force');

function log(msg) { console.log(`[cairn] ${msg}`); }
function fail(msg) { console.error(`[cairn] ${msg}`); process.exit(1); }

function run(cmd, args, opts = {}) {
    const r = spawnSync(cmd, args, { stdio: ['ignore', 'pipe', 'pipe'], encoding: 'utf8', ...opts });
    if (r.status !== 0) {
        const tail = `${r.stdout || ''}\n${r.stderr || ''}`.split('\n').filter(l => /error|错误|Error/i.test(l)).slice(-12).join('\n');
        fail(`${cmd} ${args.join(' ')} failed\n${tail || r.stderr || 'no output'}`);
    }
    return r;
}

function nodeIncludeDir() {
    // Node headers shipped with the running interpreter.
    const cand = path.join(path.dirname(process.execPath), 'include', 'node');
    if (fs.existsSync(cand)) return cand;
    // ...or from npm's node-headers layout inside a Node installation.
    const alt = path.join(path.dirname(process.execPath), '..', 'include', 'node');
    if (fs.existsSync(alt)) return alt;
    fail('node headers not found (expected <node>/include/node). Install nodejs with headers.');
}

function buildEngine() {
    fs.mkdirSync(buildDir, { recursive: true });
    const cmakeArgs = ['-S', engineDir, '-B', buildDir, '-DCMAKE_BUILD_TYPE=Release'];
    if (!fs.existsSync(path.join(buildDir, 'CMakeCache.txt')) || force) run('cmake', cmakeArgs);
    run('cmake', ['--build', buildDir, '-j', String(Math.max(2, require('os').cpus().length - 1))]);
    const soName = fs.existsSync(path.join(buildDir, 'libcairn_engine.so')) ? 'libcairn_engine.so'
        : fs.existsSync(path.join(buildDir, 'libcairn_engine.dylib')) ? 'libcairn_engine.dylib'
        : null;
    if (!soName) fail('engine library output not found');
    // Copy the library with its versioned SONAME symlinks intact so the
    // addon's DT_NEEDED (libcairn_engine.so.0) resolves inside build/.
    run('cp', ['-a',
        ...fs.readdirSync(buildDir).filter(f => f.startsWith('libcairn_engine.so')).map(f => path.join(buildDir, f)),
        buildOut + '/']);
    log(`engine -> build/${soName}`);
    return path.join(buildOut, soName);
}

function buildAddon(engineLib) {
    const out = path.join(buildOut, 'cairn_engine_addon.node');
    const inc = [
        '-I' + path.join(root, 'engine', 'include'),
        '-I' + path.join(root, 'engine', 'src'),
        '-I' + path.join(root, 'engine', 'thirdparty'),
        '-I' + path.join(root, 'node_modules', 'node-addon-api'),
        '-I' + nodeIncludeDir(),
    ];
    const cxx = process.env.CXX || 'g++';
    const std = ['-std=c++20', '-O2', '-fPIC', '-fvisibility=hidden'];
    if (process.platform === 'linux') std.push('-fno-gnu-unique');
    let args;
    const isMac = process.platform === 'darwin';
    if (isMac) {
        args = [...std, '-dynamiclib', '-undefined', 'dynamic_lookup',
            ...inc, 'addon/cairn_addon.cc', '-o', out,
            `-L${buildOut}`, '-lcairn_engine', '-Wl,-rpath,@loader_path'];
    } else if (process.platform === 'win32') {
        fail('Windows: build the addon with MSVC (see docs/building.md); node-gyp route planned.');
    } else {
        args = [...std, '-shared', ...inc, 'addon/cairn_addon.cc', '-o', out,
            `-L${buildOut}`, '-lcairn_engine', '-Wl,-rpath,$ORIGIN', '-pthread'];
    }
    run(cxx, args);
    log(`addon  -> build/cairn_engine_addon.node`);
    return out;
}

// ---------------------------------------------------------------------------
// Renderer bundle (editor UI). The renderer uses ES modules in source but the
// app loads a CommonJS bundle so Node built-ins resolve through the
// nodeIntegration bridge.
// ---------------------------------------------------------------------------
function buildRenderer() {
    const esbuild = require('esbuild');
    const outdir = path.join(root, 'editor', 'renderer', 'dist');
    fs.mkdirSync(outdir, { recursive: true });
    esbuild.buildSync({
        entryPoints: [
            path.join(root, 'editor', 'renderer', 'src', 'boot.js'),
            path.join(root, 'editor', 'renderer', 'src', 'worker.js'),
        ],
        outdir,
        bundle: true,
        format: 'cjs',
        platform: 'node',
        target: 'chrome126',
        sourcemap: 'inline',
        logLevel: 'warning',
        external: ['electron'],
        loader: { '.ttf': 'file', '.woff': 'file', '.woff2': 'file', '.svg': 'file', '.png': 'file' },
        define: { 'process.env.NODE_ENV': '"production"' },
    });
    log(`render -> editor/renderer/dist/{boot,worker}.js`);
}

// ---------------------------------------------------------------------------

const target = process.argv[2] || 'all';
fs.mkdirSync(buildOut, { recursive: true });

if (target === 'engine' || target === 'all') buildEngine();
if (target === 'addon' || target === 'all') buildAddon();
if (target === 'engine' || target === 'addon' || target === 'all' || target === 'renderer') buildRenderer();
log('done');
