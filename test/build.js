#!/usr/bin/env node
// Cairn Engine - build-script test.
//
// scripts/build.js stages the freshly built engine library from CMake's build
// directory into build/, where the addon links against it. Multi-config
// generators (Visual Studio, Xcode, Ninja Multi-Config) write the library into a
// per-configuration subdirectory unless the project pins its output directories,
// so the staging step has to find it either way. These checks pin that layout
// handling against synthetic build trees, exercising the shapes CMake produces
// without needing a compiler.
'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');

const root = path.resolve(__dirname, '..');
const { engineOutputDirs, locateEngineLibrary, stageLibrary, nodeInstallRoots, findNodeDevInstall }
    = require(path.join(root, 'scripts', 'build.js'));

let failures = 0;
let checks = 0;
function check(cond, label, extra) {
    checks++;
    if (cond) { console.log(`  ok  ${label}`); }
    else { console.error(`FAIL  ${label}${extra ? '\n      ' + extra : ''}`); failures++; }
}

const scratch = fs.mkdtempSync(path.join(os.tmpdir(), 'cairn-build-'));
const mk = (...p) => { const d = path.join(scratch, ...p); fs.mkdirSync(d, { recursive: true }); return d; };
const touch = (p, body = 'x') => fs.writeFileSync(p, body);
const names = (found) => (found ? found.files.join(',') : String(found));

// ---------------------------------------------------------------------------
console.log('build: library discovery');
// ---------------------------------------------------------------------------
const dirs = engineOutputDirs('/proj/engine/build');
check(dirs[0] === '/proj/engine/build', 'the build root is searched first', dirs[0]);
check(dirs.includes(path.join('/proj/engine/build', 'Release')),
    'per-configuration subdirectories are searched too (Visual Studio, Xcode)', dirs.join(' '));

// A Visual Studio build with no pinned output directory: everything lives in
// Release/.
const vs = mk('vs');
touch(path.join(mk('vs', 'Release'), 'cairn_engine.dll'));
touch(path.join(mk('vs', 'Release'), 'cairn_engine.lib'));
const foundVs = locateEngineLibrary(engineOutputDirs(vs));
check(foundVs && foundVs.dir === path.join(vs, 'Release'),
    'finds a Visual Studio Release/ layout', names(foundVs));
check(foundVs && foundVs.files.join(',') === 'cairn_engine.dll,cairn_engine.lib',
    'stages the import library next to the DLL', names(foundVs));

// CMakeLists.txt now pins the output directory, so a clean build has no
// subdirectory at all; when both exist the flat one must win.
const flat = mk('flat');
touch(path.join(flat, 'cairn_engine.dll'));
touch(path.join(flat, 'cairn_engine.lib'));
touch(path.join(mk('flat', 'Release'), 'cairn_engine.dll'));
const foundFlat = locateEngineLibrary(engineOutputDirs(flat));
check(foundFlat && foundFlat.dir === flat, 'a flat layout beats a stale Release/ copy', foundFlat && foundFlat.dir);

const mac = mk('mac');
touch(path.join(mac, 'libcairn_engine.dylib'));
check(names(locateEngineLibrary(engineOutputDirs(mac))) === 'libcairn_engine.dylib',
    'finds a macOS dylib', names(locateEngineLibrary(engineOutputDirs(mac))));

check(locateEngineLibrary(engineOutputDirs(mk('empty'))) === null,
    'an empty build directory reports no library');

// ---------------------------------------------------------------------------
console.log('build: staging');
// ---------------------------------------------------------------------------
const linux = mk('linux');
let symlinks = true;
try {
    touch(path.join(linux, 'libcairn_engine.so.0.1.24'), 'ELF');
    fs.symlinkSync('libcairn_engine.so.0.1.24', path.join(linux, 'libcairn_engine.so.0'));
    fs.symlinkSync('libcairn_engine.so.0', path.join(linux, 'libcairn_engine.so'));
} catch (err) {
    symlinks = false;
    console.log(`  --  symlink chain skipped (${err.code})`);
}
if (symlinks) {
    const found = locateEngineLibrary(engineOutputDirs(linux));
    check(found && found.files.includes('libcairn_engine.so.0.1.24'),
        'the versioned file behind the symlink chain is staged', names(found));

    const dest = mk('linuxOut');
    // A leftover file from a previous version must not survive staging.
    touch(path.join(dest, 'libcairn_engine.so.0.1.23'), 'old');
    stageLibrary(found, dest);
    check(!fs.existsSync(path.join(dest, 'libcairn_engine.so.0.1.23')),
        'staging drops copies of an older version');
    check(fs.lstatSync(path.join(dest, 'libcairn_engine.so')).isSymbolicLink() &&
          fs.lstatSync(path.join(dest, 'libcairn_engine.so.0')).isSymbolicLink(),
        'staging recreates the .so symlink names');
    check(fs.readFileSync(path.join(dest, 'libcairn_engine.so')).toString() === 'ELF',
        'the staged chain resolves to the real library');
}

const winOut = mk('winOut');
if (foundVs) {
    stageLibrary(foundVs, winOut);
    check(fs.existsSync(path.join(winOut, 'cairn_engine.dll')) &&
          fs.existsSync(path.join(winOut, 'cairn_engine.lib')),
        'staging a Windows layout copies the DLL and its import library');
} else {
    check(false, 'staging a Windows layout copies the DLL and its import library',
        'no library located to stage');
}

// ---------------------------------------------------------------------------
console.log('build: node development install discovery');
// ---------------------------------------------------------------------------
// A Node installation puts node_api.h and node.lib in one of three shapes, and
// which shape a machine has decides whether the addon compiles. The old script
// checked exactly two paths next to the interpreter, which is why a Windows CI
// run reported "node headers not found" on a Node that had them elsewhere.
// The discovery is a pure function over a fake tree, so every shape is pinned.
const winNode = mk('win', 'nodejs');
touch(path.join(winNode, 'node.exe'));
touch(path.join(winNode, 'node.lib'));
touch(path.join(mk('win', 'nodejs', 'include', 'node'), 'node_api.h'));
const toolcache = mk('tc');
touch(path.join(mk('tc', '22.11.0', 'x64'), 'node.exe'));
touch(path.join(mk('tc', '22.11.0', 'include', 'node'), 'node_api.h'));
const nvmNode = mk('nvm');
touch(path.join(mk('nvm', 'v22.11.0', 'bin'), 'node'));
touch(path.join(mk('nvm', 'v22.11.0', 'include', 'node'), 'node_api.h'));
// A root with no node executable in it at all: only an explicit environment
// variable knows this is a Node installation.
const flatNode = mk('flat');
touch(path.join(mk('flat', 'include', 'node'), 'node_api.h'));

const find = (execPaths, env = {}) =>
    findNodeDevInstall({
        execPaths, env, existsSync: fs.existsSync,
        listDirs: (d) => {
            try { return fs.readdirSync(d, { withFileTypes: true }).filter(x => x.isDirectory()).map(x => x.name); }
            catch { return []; }
        },
    });

{
    const r = find([path.join(toolcache, '22.11.0', 'x64', 'node.exe')]);
    check(r.headers === path.join(toolcache, '22.11.0', 'include', 'node'),
        'finds headers in the versioned layout (tool cache, nvm)', r.headers);
    check(r.searched.length >= 2, 'the ancestor walk covers more than the executable directory',
        `${r.searched.length} roots`);
}
{
    const r = find([path.join(winNode, 'node.exe')]);
    check(r.lib === path.join(winNode, 'node.lib'), 'finds the import library', r.lib);
}
{
    const r = find([path.join(nvmNode, 'v22.11.0', 'bin', 'node')]);
    check(r.headers === path.join(nvmNode, 'v22.11.0', 'include', 'node'),
        'finds headers in the bin/ layout (Linux distro, nvm)', r.headers);
}
{
    const r = find([], { NODE_ROOT_DIR: flatNode });
    check(r.headers === path.join(flatNode, 'include', 'node'),
        'NODE_ROOT_DIR names an explicit installation root', r.headers);
}
{
    const r = find([path.join(mk('other', 'bin'), 'node')], { HOME: scratch });
    check(r.headers === null && r.lib === null,
        'a tree without node_api.h reports nothing rather than a wrong path',
        `headers=${r.headers} lib=${r.lib}`);
    check(r.searched.length > 0, 'the absence report lists the roots that were searched');
}

// ---------------------------------------------------------------------------
console.log('build: Vulkan headers are optional');
// ---------------------------------------------------------------------------
// The backend is compiled without CAIRN_HAS_VULKAN on machines that have no
// headers, and CMake turns the option off when they or glslc are missing. An
// include of <vulkan/vulkan.h> - directly or through vulkan_memory.h - at file
// scope turns that optional backend into a hard build dependency, which is what
// stopped CI on a runner without the Vulkan SDK. vk_loader.h guards its own
// include and is deliberately not part of the pattern. vulkan_memory.h is the
// one gateway that names <vulkan/vulkan.h> directly; it is allowed to, because
// every translation unit that includes the gateway must guard that include.
function unguardedVulkanIncludes(source, isGateway = false) {
    const stack = [];
    const found = [];
    source.split('\n').forEach((line, index) => {
        const text = line.trim();
        if (/^#ifdef\b/.test(text)) {
            stack.push({ guard: text.slice(6).trim() === 'CAIRN_HAS_VULKAN', inElse: false });
        } else if (/^#ifndef\b/.test(text)) {
            stack.push({ guard: false, inElse: false });
        } else if (/^#if\b/.test(text)) {
            const cond = text.slice(3);
            stack.push({
                guard: /defined\s*\(?\s*CAIRN_HAS_VULKAN\b/.test(cond) && !/!\s*defined/.test(cond),
                inElse: false,
            });
        } else if (/^#elif\b/.test(text)) {
            const frame = stack[stack.length - 1];
            if (frame) {
                const cond = text.slice(5);
                frame.guard = /defined\s*\(?\s*CAIRN_HAS_VULKAN\b/.test(cond) && !/!\s*defined/.test(cond);
                frame.inElse = false;
            }
        } else if (/^#else\b/.test(text)) {
            const frame = stack[stack.length - 1];
            if (frame) frame.inElse = true;
        } else if (/^#endif\b/.test(text)) {
            stack.pop();
        } else if (/#include\s*[<"](?:vulkan\/vulkan\.h|vulkan_memory\.h)[>"]/.test(text)) {
            if (isGateway && /<vulkan\/vulkan\.h>/.test(text)) return;
            if (!stack.some(f => f.guard && !f.inElse)) found.push({ line: index + 1, text });
        }
    });
    return found;
}

const engineRoot = path.join(root, 'engine');
const vulkanViolations = [];
for (const sub of ['src', 'tests', 'include']) {
    const dir = path.join(engineRoot, sub);
    for (const file of fs.readdirSync(dir)) {
        if (!/\.(?:cpp|cc|h|hpp)$/.test(file)) continue;
        const full = path.join(dir, file);
        for (const hit of unguardedVulkanIncludes(fs.readFileSync(full, 'utf8'),
                                                 file === 'vulkan_memory.h'))
            vulkanViolations.push(`${path.relative(root, full)}:${hit.line}`);
    }
}
check(vulkanViolations.length === 0,
    'every Vulkan include is behind CAIRN_HAS_VULKAN', vulkanViolations.slice(0, 6).join(', '));
check(unguardedVulkanIncludes('#include <vulkan/vulkan.h>\n').length === 1,
    'an unguarded include is detected');
check(unguardedVulkanIncludes('#ifdef CAIRN_HAS_VULKAN\n#include "vulkan_memory.h"\n#endif\n').length === 0,
    'a guarded include is accepted');
check(unguardedVulkanIncludes('#ifdef CAIRN_HAS_VULKAN\n#else\n#include "vulkan_memory.h"\n#endif\n').length === 1,
    'the else branch of the guard is not protected');

fs.rmSync(scratch, { recursive: true, force: true });

console.log(`build: ${checks - failures}/${checks} checks passed`);
process.exit(failures === 0 ? 0 : 1);
