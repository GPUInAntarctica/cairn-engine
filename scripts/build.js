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
    // Extra -D flags from the command line reach CMake, so documented options
    // like `npm run build -- -DCAIRN_ENABLE_VULKAN=OFF` actually take effect.
    const defines = process.argv.slice(2).filter(a => a.startsWith('-D'));
    const cmakeArgs = ['-S', engineDir, '-B', buildDir, '-DCMAKE_BUILD_TYPE=Release', ...defines];
    if (defines.length && !force) {
        log(`cmake defines: ${defines.join(' ')} (reconfigure)`);
    }
    if (!fs.existsSync(path.join(buildDir, 'CMakeCache.txt')) || force || defines.length)
        run('cmake', cmakeArgs);
    run('cmake', ['--build', buildDir, '-j', String(Math.max(2, require('os').cpus().length - 1))]);

    // Stage only the current library chain. CMake leaves older versioned files
    // in the build dir across version bumps, so match the symlink chain rather
    // than every libcairn_engine.* file.
    const soLink = path.join(buildDir, 'libcairn_engine.so');
    const soMajor = path.join(buildDir, 'libcairn_engine.so.0');
    let toCopy;
    if (fs.existsSync(soLink) && fs.existsSync(soMajor)) {
        const real = path.basename(fs.realpathSync(soMajor));
        toCopy = ['libcairn_engine.so', 'libcairn_engine.so.0', real];
    } else if (fs.existsSync(path.join(buildDir, 'libcairn_engine.dylib'))) {
        toCopy = ['libcairn_engine.dylib'];
    } else if (fs.existsSync(path.join(buildDir, 'cairn_engine.dll'))) {
        toCopy = ['cairn_engine.dll', 'cairn_engine.lib'];
    } else {
        fail('engine library output not found');
    }
    // Drop stale copies from previous versions before staging.
    for (const f of fs.readdirSync(buildOut)) {
        if (f.startsWith('libcairn_engine.') || f === 'cairn_engine.dll' || f === 'cairn_engine.lib')
            fs.rmSync(path.join(buildOut, f), { force: true });
    }
    for (const f of toCopy) {
        // Resolve symlinks to their real file, then recreate the symlink name.
        const src = path.join(buildDir, f);
        const dest = path.join(buildOut, f);
        if (!fs.existsSync(src)) continue;
        try {
            if (fs.lstatSync(dest, { throwIfNoEntry: false })) fs.rmSync(dest, { force: true });
            if (fs.lstatSync(src).isSymbolicLink()) fs.symlinkSync(fs.readlinkSync(src), dest);
            else fs.copyFileSync(src, dest);
        } catch (err) {
            fail(`could not stage ${f}: ${err.message}`);
        }
    }
    const soName = toCopy.find(f => f.endsWith('.so') || f.endsWith('.dylib') || f.endsWith('.dll'));
    log(`engine -> build/${soName}`);
    return path.join(buildOut, soName);
}

// Locates cl.exe (MSVC). It is only on PATH inside a "Developer Command
// Prompt"; there is no reliable way to synthesize that environment here, so we
// fail with instructions instead of a cryptic linker error.
function findCl() {
    const r = spawnSync('where', ['cl'], { encoding: 'utf8', shell: true });
    if (r.status === 0 && r.stdout.trim()) return 'cl';
    return null;
}

// Directory holding node.lib (the N-API import library) for the running Node.
function nodeLibDirCandidates() {
    const home = path.dirname(process.execPath);
    const out = [home, path.join(home, 'lib')];
    if (process.env.LOCALAPPDATA) out.push(path.join(process.env.LOCALAPPDATA, 'node-gyp', 'Cache'));
    return out.filter(p => fs.existsSync(path.join(p, 'node.lib')));
}

function buildAddonWindows(out, inc) {
    const cl = findCl();
    if (!cl) {
        fail('Windows: cl.exe not found. Open a "Developer Command Prompt for VS"\n' +
             '       (or run vcvars64.bat) and re-run `npm run build`.');
    }
    const libDirs = nodeLibDirCandidates();
    if (libDirs.length === 0) {
        fail('Windows: node.lib not found. Install Node.js with the native module\n' +
             '       headers/npm tools, or set LOCALAPPDATA for the node-gyp cache.');
    }
    const objDir = path.join(buildOut, 'obj');
    fs.mkdirSync(objDir, { recursive: true });
    const winInc = inc.map(i => '/I' + i.replace(/^-I/, ''));
    // /LD builds a DLL; /Fe names the output; /Fo sends objects to a scratch dir.
    const args = [
        '/nologo', '/std:c++20', '/O2', '/EHsc', '/MD', '/LD',
        ...winInc,
        path.join(root, 'addon', 'cairn_addon.cc'),
        '/Fe:' + out,
        '/Fo:' + objDir + path.sep,
        '/link', '/DLL',
        '/LIBPATH:' + buildOut,
        'cairn_engine.lib',
        path.join(libDirs[0], 'node.lib'),
    ];
    run(cl, args);
}

function buildAddon(engineLib) {
    const out = path.join(buildOut, 'cairn_engine_addon.node');
    const inc = [
        path.join(root, 'engine', 'include'),
        path.join(root, 'engine', 'src'),
        path.join(root, 'engine', 'thirdparty'),
        path.join(root, 'node_modules', 'node-addon-api'),
        nodeIncludeDir(),
    ];
    if (process.platform === 'win32') {
        buildAddonWindows(out, inc);
        log(`addon  -> build/cairn_engine_addon.node`);
        return out;
    }
    const incFlags = inc.map(i => '-I' + i);
    const cxx = process.env.CXX || 'g++';
    const std = ['-std=c++20', '-O2', '-fPIC', '-fvisibility=hidden'];
    if (process.platform === 'linux') std.push('-fno-gnu-unique');
    let args;
    const isMac = process.platform === 'darwin';
    if (isMac) {
        args = [...std, '-dynamiclib', '-undefined', 'dynamic_lookup',
            ...incFlags, 'addon/cairn_addon.cc', '-o', out,
            `-L${buildOut}`, '-lcairn_engine', '-Wl,-rpath,@loader_path'];
    } else {
        args = [...std, '-shared', ...incFlags, 'addon/cairn_addon.cc', '-o', out,
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

// `--force` re-runs cmake configure; it is a flag, not a target, so it must
// not be mistaken for one (it silently built nothing before).
const rest = process.argv.slice(2).filter(a => !a.startsWith('-'));
const target = rest[0] || 'all';
fs.mkdirSync(buildOut, { recursive: true });

if (target === 'engine' || target === 'all') buildEngine();
if (target === 'addon' || target === 'all') buildAddon();
if (target === 'engine' || target === 'addon' || target === 'all' || target === 'renderer') buildRenderer();
log('done');
