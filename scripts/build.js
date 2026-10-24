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

function log(msg) { console.log(`[cairn] ${msg}`); }
function fail(msg) { console.error(`[cairn] ${msg}`); process.exit(1); }

// ---------------------------------------------------------------------------
// Library discovery / staging.
//
// Multi-config generators (Visual Studio, Xcode, Ninja Multi-Config) write into
// a per-configuration subdirectory unless the project pins its output
// directories. engine/CMakeLists.txt pins them; searching the subdirectories as
// well keeps the staging step working against a build tree configured by an
// older revision or by another generator.
// ---------------------------------------------------------------------------
function engineOutputDirs(dir) {
    return [
        dir,
        path.join(dir, 'Release'),
        path.join(dir, 'RelWithDebInfo'),
        path.join(dir, 'MinSizeRel'),
        path.join(dir, 'Debug'),
    ];
}

// First directory holding a built engine library, with the file chain to stage:
// the symlink names plus the real versioned file they point at.
function locateEngineLibrary(dirs) {
    for (const dir of dirs) {
        const soLink = path.join(dir, 'libcairn_engine.so');
        const soMajor = path.join(dir, 'libcairn_engine.so.0');
        if (fs.existsSync(soLink) && fs.existsSync(soMajor)) {
            const real = path.basename(fs.realpathSync(soMajor));
            return { dir, files: ['libcairn_engine.so', 'libcairn_engine.so.0', real] };
        }
        if (fs.existsSync(path.join(dir, 'libcairn_engine.dylib')))
            return { dir, files: ['libcairn_engine.dylib'] };
        if (fs.existsSync(path.join(dir, 'cairn_engine.dll')))
            return { dir, files: ['cairn_engine.dll', 'cairn_engine.lib'] };
    }
    return null;
}

// Copies a located library chain into destDir, recreating symlink names so the
// staged tree loads exactly like the build tree (the addon links against it).
function stageLibrary(found, destDir) {
    fs.mkdirSync(destDir, { recursive: true });
    // Drop stale copies from previous versions before staging.
    for (const f of fs.readdirSync(destDir)) {
        if (f.startsWith('libcairn_engine.') || f === 'cairn_engine.dll' || f === 'cairn_engine.lib')
            fs.rmSync(path.join(destDir, f), { force: true });
    }
    for (const f of found.files) {
        // Resolve symlinks to their real file, then recreate the symlink name.
        const src = path.join(found.dir, f);
        const dest = path.join(destDir, f);
        if (!fs.existsSync(src)) continue;
        try {
            if (fs.lstatSync(dest, { throwIfNoEntry: false })) fs.rmSync(dest, { force: true });
            if (fs.lstatSync(src).isSymbolicLink()) fs.symlinkSync(fs.readlinkSync(src), dest);
            else fs.copyFileSync(src, dest);
        } catch (err) {
            fail(`could not stage ${f}: ${err.message}`);
        }
    }
}

function run(cmd, args, opts = {}) {
    const r = spawnSync(cmd, args, { stdio: ['ignore', 'pipe', 'pipe'], encoding: 'utf8', ...opts });
    if (r.status !== 0) {
        const tail = `${r.stdout || ''}\n${r.stderr || ''}`.split('\n').filter(l => /error|错误|Error/i.test(l)).slice(-12).join('\n');
        fail(`${cmd} ${args.join(' ')} failed\n${tail || r.stderr || 'no output'}`);
    }
    return r;
}

// Node's headers and import library live inside an installation whose shape
// varies:
//
//   <root>/bin/node      + <root>/include/node     (Linux distro, nvm, fnm)
//   <root>/node.exe      + <root>/include/node     (Windows installer)
//   <root>/x64/node.exe  + <root>/include/node     (GitHub Actions tool cache)
//
// so the search walks the ancestors of every interpreter it knows about rather
// than assuming one shape, and it honours the environment variables the
// installers and tools hand out: NODE_ROOT_DIR (nvm4w), npm_config_nodedir
// (node-gyp), and the node-gyp cache where a previous build unpacked a tree.
function pathAncestors(dir) {
    const out = [];
    let cur = dir;
    while (cur && out.length < 8) {
        out.push(cur);
        const parent = path.dirname(cur);
        if (!parent || parent === cur) break;
        cur = parent;
    }
    return out;
}

function listDirsSafe(dir) {
    try {
        return fs.readdirSync(dir, { withFileTypes: true })
            .filter(d => d.isDirectory()).map(d => d.name);
    } catch { return []; }
}

function nodeInstallRoots(ctx) {
    const { execPaths = [], env = {}, listDirs = listDirsSafe } = ctx;
    const roots = [];
    const add = (dir) => { if (dir && !roots.includes(dir)) roots.push(dir); };
    for (const key of ['NODE_ROOT_DIR', 'npm_config_nodedir', 'NVM_HOME', 'NVM_SYMLINK'])
        if (env[key]) add(env[key]);
    for (const exe of execPaths) {
        if (!exe) continue;
        for (const ancestor of pathAncestors(path.dirname(exe))) add(ancestor);
    }
    // Paths no ancestor search reaches: system-wide installs on Windows.
    for (const base of [env['PROGRAMFILES'], env['ProgramFiles'], env['PROGRAMFILES(X86)'],
                        env['ProgramFiles(x86)'], env['PROGRAMDATA'], env.ChocolateyInstall])
        if (base) add(path.join(base, 'nodejs'));
    // node-gyp unpacks a headers tree per version it has built against.
    for (const cache of [env.LOCALAPPDATA ? path.join(env.LOCALAPPDATA, 'node-gyp', 'Cache') : null,
                          env.HOME ? path.join(env.HOME, '.cache', 'node-gyp') : null,
                          env.USERPROFILE ? path.join(env.USERPROFILE, 'AppData', 'Local', 'node-gyp', 'Cache') : null]) {
        if (!cache) continue;
        for (const entry of listDirs(cache)) add(path.join(cache, entry));
    }
    return roots;
}

// Locates the N-API headers and, on Windows, the import library. node_api.h is
// the marker: it is the header an N-API addon compiles against, and a Node
// installation without it has no headers worth putting on an include path.
function findNodeDevInstall(ctx = {}) {
    const { execPaths = [], env = {}, existsSync = fs.existsSync, listDirs = listDirsSafe } = ctx;
    const roots = nodeInstallRoots({ execPaths, env, listDirs });
    let headers = null;
    let lib = null;
    for (const root of roots) {
        if (!headers) {
            const inc = path.join(root, 'include', 'node');
            if (existsSync(path.join(inc, 'node_api.h'))) headers = inc;
        }
        if (!lib)
            for (const cand of [path.join(root, 'node.lib'), path.join(root, 'lib', 'node.lib')])
                if (existsSync(cand)) { lib = cand; break; }
        if (headers && lib) break;
    }
    return { headers, lib, searched: roots };
}

// Every node.exe on PATH plus the interpreter running this script. A build may
// use the headers of any of them, not only of the one npm happens to run.
function nodeExePaths() {
    const out = [process.execPath];
    const probe = process.platform === 'win32' ? ['where', ['node']] : ['which', ['-a', 'node']];
    const r = spawnSync(probe[0], probe[1], { encoding: 'utf8', shell: process.platform === 'win32' });
    if (r.status === 0)
        for (const line of (r.stdout || '').split('\n')) {
            const p = line.trim();
            if (p && !out.includes(p)) out.push(p);
        }
    return out;
}

function buildEngine() {
    fs.mkdirSync(buildDir, { recursive: true });
    const force = process.argv.includes('--force');
    // Extra -D flags from the command line reach CMake, so documented options
    // like `npm run build -- -DCAIRN_ENABLE_VULKAN=OFF` actually take effect.
    const defines = process.argv.slice(2).filter(a => a.startsWith('-D'));
    const cmakeArgs = ['-S', engineDir, '-B', buildDir, '-DCMAKE_BUILD_TYPE=Release', ...defines];
    if (defines.length && !force) {
        log(`cmake defines: ${defines.join(' ')} (reconfigure)`);
    }
    if (!fs.existsSync(path.join(buildDir, 'CMakeCache.txt')) || force || defines.length)
        run('cmake', cmakeArgs);
    // --config is ignored by single-config generators and required by
    // multi-config ones (Visual Studio defaults to Debug, not CMAKE_BUILD_TYPE).
    run('cmake', ['--build', buildDir, '--config', 'Release',
        '-j', String(Math.max(2, require('os').cpus().length - 1))]);

    // Stage only the current library chain. CMake leaves older versioned files
    // in the build dir across version bumps, so match the symlink chain rather
    // than every libcairn_engine.* file.
    const found = locateEngineLibrary(engineOutputDirs(buildDir));
    if (!found) fail('engine library output not found');
    stageLibrary(found, buildOut);
    const soName = found.files.find(f => f.endsWith('.so') || f.endsWith('.dylib') || f.endsWith('.dll'));
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

function buildAddonWindows(out, inc, install) {
    const cl = findCl();
    if (!cl) {
        fail('Windows: cl.exe not found. Open a "Developer Command Prompt for VS"\n' +
             '       (or run vcvars64.bat) and re-run `npm run build`.');
    }
    const libDirs = install.lib ? [path.dirname(install.lib)] : [];
    if (libDirs.length === 0) {
        fail(`Windows: node.lib not found. Searched ${install.searched.length} Node installation roots, ` +
             `nearest were: ${install.searched.slice(0, 6).join(', ')}. Install Node.js with its native ` +
             'module headers, or set NODE_ROOT_DIR to an installation that has them.');
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
    // One search answers both the include path and, on Windows, the import
    // library, so a machine whose Node keeps its headers somewhere other than
    // the running interpreter still builds.
    const install = findNodeDevInstall({ execPaths: nodeExePaths(), env: process.env });
    if (!install.headers)
        fail(`Node headers not found: expected <root>/include/node/node_api.h under one of the
  ${install.searched.length} Node installation roots searched:
  ${install.searched.slice(0, 12).join('\n  ')}${install.searched.length > 12 ? '\n  ...' : ''}
  Install Node.js with its native module headers, or set NODE_ROOT_DIR to an
  installation that has them.`);
    log(`node   -> headers ${install.headers}${install.lib ? ` / lib ${install.lib}` : ''}`);
    const inc = [
        path.join(root, 'engine', 'include'),
        path.join(root, 'engine', 'src'),
        path.join(root, 'engine', 'thirdparty'),
        path.join(root, 'node_modules', 'node-addon-api'),
        install.headers,
    ];
    if (process.platform === 'win32') {
        buildAddonWindows(out, inc, install);
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

function main() {
    // `--force` re-runs cmake configure; it is a flag, not a target, so it must
    // not be mistaken for one (it silently built nothing before).
    const rest = process.argv.slice(2).filter(a => !a.startsWith('-'));
    const target = rest[0] || 'all';
    fs.mkdirSync(buildOut, { recursive: true });

    if (target === 'engine' || target === 'all') buildEngine();
    if (target === 'addon' || target === 'all') buildAddon();
    if (target === 'engine' || target === 'addon' || target === 'all' || target === 'renderer') buildRenderer();
    log('done');
}

// Exported so test/build.js can exercise the staging logic, and so
// run-engine-tests.js shares one definition of where CMake writes its output.
module.exports = { engineOutputDirs, locateEngineLibrary, stageLibrary, nodeInstallRoots,
                 findNodeDevInstall, nodeExePaths };

if (require.main === module) main();
