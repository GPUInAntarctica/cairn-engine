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
const { engineOutputDirs, locateEngineLibrary, stageLibrary } = require(path.join(root, 'scripts', 'build.js'));

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

fs.rmSync(scratch, { recursive: true, force: true });

console.log(`build: ${checks - failures}/${checks} checks passed`);
process.exit(failures === 0 ? 0 : 1);
