// Cairn Engine editor - gameplay script build pipeline (main process).
//
// Compiles Assets/Scripts/**/*.cpp into Library/GameScripts.<ext>, linking the
// engine shared library. Diagnostics (g++/clang file:line:col format) are
// returned so the code editor can place markers.
'use strict';

const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');

const ENGINE_INCLUDE = path.join(__dirname, '..', '..', 'engine', 'include');
const ENGINE_LIB_DIR = path.join(__dirname, '..', '..', 'build');

function scriptOutputPath(projectDir) {
    const ext = process.platform === 'win32' ? '.dll' : process.platform === 'darwin' ? '.dylib' : '.so';
    return path.join(projectDir, 'Library', 'GameScripts' + ext);
}

function collectSources(projectDir) {
    const root = path.join(projectDir, 'Assets', 'Scripts');
    const out = [];
    const walk = dir => {
        let entries;
        try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch { return; }
        for (const e of entries) {
            const full = path.join(dir, e.name);
            if (e.isDirectory()) walk(full);
            else if (/\.(cpp|cc|cxx)$/i.test(e.name)) out.push(full);
        }
    };
    walk(root);
    return out.sort();
}

// Parses "file:line:col: severity: message" (GCC/Clang) and
// "file(line,col): error C1234: message" (MSVC) diagnostics.
function parseDiagnostics(stderr, projectDir) {
    const out = [];
    for (const line of String(stderr).split('\n')) {
        const gcc = line.match(/^(.+?):(\d+):(\d+):\s*(fatal error|error|warning):\s*(.*)$/);
        if (gcc) {
            let file = gcc[1];
            if (fs.existsSync(file)) file = path.relative(projectDir, file) || file;
            out.push({ file, line: +gcc[2], column: +gcc[3], severity: gcc[4].startsWith('warn') ? 'warning' : 'error', message: gcc[5] });
            continue;
        }
        const msvc = line.match(/^(.*?)\((\d+),(\d+)\)\s*:\s*(fatal error|error|warning)\s*[A-Z]*\d*\s*:\s*(.*)$/);
        if (msvc) {
            let file = msvc[1].trim();
            if (fs.existsSync(file)) file = path.relative(projectDir, file) || file;
            out.push({ file, line: +msvc[2], column: +msvc[3], severity: msvc[4].startsWith('warn') ? 'warning' : 'error', message: msvc[5] });
        }
    }
    return out;
}

function detectCompiler(explicit) {
    if (explicit && fs.existsSync(explicit)) return explicit;
    const candidates = process.platform === 'win32'
        ? ['cl', 'clang++', 'g++']
        : ['g++', 'clang++'];
    for (const c of candidates) {
        const r = spawn(c, ['--version'], { shell: process.platform === 'win32' });
        // spawn errors are emitted; a successful exit means it exists.
        return new Promise(resolve => {
            r.on('error', () => resolve(null));
            r.on('exit', code => resolve(code === 0 ? c : null));
        });
    }
    return Promise.resolve(null);
}

// Returns {ok, diagnostics, output, durationMs}.
function buildScripts(projectDir, compiler) {
    return new Promise(resolve => {
        const sources = collectSources(projectDir);
        const output = scriptOutputPath(projectDir);
        if (sources.length === 0) {
            resolve({ ok: false, diagnostics: [], output: '', durationMs: 0, noSources: true });
            return;
        }
        fs.mkdirSync(path.dirname(output), { recursive: true });

        const msvc = process.platform === 'win32' && /(^|[\\/])cl(\.exe)?$/i.test(compiler || 'cl');
        let args;
        if (msvc) {
            const objDir = path.join(projectDir, 'Library', 'obj');
            fs.mkdirSync(objDir, { recursive: true });
            args = [
                '/nologo', '/std:c++17', '/O2', '/EHsc', '/MD', '/LD',
                `/I${ENGINE_INCLUDE}`,
                `/I${path.join(projectDir, 'Assets', 'Scripts')}`,
                ...sources,
                `/Fe:${output}`,
                `/Fo:${objDir}${path.sep}`,
                '/link', '/DLL', `/LIBPATH:${ENGINE_LIB_DIR}`, 'cairn_engine.lib',
            ];
        } else {
            args = [
                '-std=c++17', '-O2', '-fPIC', '-shared', '-fvisibility=hidden',
                '-fno-gnu-unique',   // registries must die with the library (hot reload)
                `-I${ENGINE_INCLUDE}`,
                `-I${path.join(projectDir, 'Assets', 'Scripts')}`,
                `-L${ENGINE_LIB_DIR}`, '-lcairn_engine',
                `-Wl,-rpath,${ENGINE_LIB_DIR}`,
                '-o', output,
                ...sources,
            ];
            if (process.platform === 'darwin') {
                args.splice(args.indexOf('-fno-gnu-unique'), 1);
                args.push('-undefined', 'dynamic_lookup');
            }
        }

        const started = Date.now();
        const child = spawn(compiler, args, { cwd: projectDir });
        let stderr = '';
        child.stderr.on('data', d => { stderr += d; });
        child.stdout.on('data', d => { stderr += d; });
        child.on('error', err => resolve({ ok: false, diagnostics: [{ file: '', line: 0, column: 0, severity: 'error', message: `compiler launch failed: ${err.message}` }], output, durationMs: 0 }));
        child.on('exit', code => {
            const diagnostics = parseDiagnostics(stderr, projectDir);
            resolve({
                ok: code === 0,
                diagnostics,
                output: code === 0 ? output : '',
                durationMs: Date.now() - started,
            });
        });
    });
}

module.exports = { buildScripts, collectSources, scriptOutputPath, detectCompiler, parseDiagnostics };
