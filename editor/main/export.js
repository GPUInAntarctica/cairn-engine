// Cairn Engine editor - player export (main process).
// Packages a project with the Electron-based runtime player so it can run
// without the editor:   npx electron <export>/player
'use strict';

const fs = require('fs');
const path = require('path');

const ROOT = path.join(__dirname, '..', '..');
const RUNTIME_DIR = path.join(ROOT, 'runtime');
const BUILD_DIR = path.join(ROOT, 'build');

function copyDir(src, dst) {
    fs.mkdirSync(dst, { recursive: true });
    for (const entry of fs.readdirSync(src, { withFileTypes: true })) {
        const s = path.join(src, entry.name);
        const d = path.join(dst, entry.name);
        if (entry.isDirectory()) copyDir(s, d);
        else fs.copyFileSync(s, d);
    }
}

// Returns {ok, error?, dir}.
async function exportPlayer(projectDir, targetDir, buildScriptsFn, compiler) {
    try {
        const build = await buildScriptsFn(projectDir, compiler);
        if (!build.ok) {
            return { ok: false, error: build.noSources ? 'no gameplay scripts found' : 'script build failed' };
        }
        if (fs.existsSync(targetDir)) fs.rmSync(targetDir, { recursive: true, force: true });
        copyDir(RUNTIME_DIR, targetDir);

        // Engine + addon binaries.
        const libDir = path.join(targetDir, 'bin');
        fs.mkdirSync(libDir, { recursive: true });
        for (const f of fs.readdirSync(BUILD_DIR)) {
            if (f.startsWith('libcairn_engine') || f.endsWith('.node')) {
                fs.copyFileSync(path.join(BUILD_DIR, f), path.join(libDir, f));
            }
        }

        // Project content: project file + Assets + compiled scripts.
        fs.copyFileSync(path.join(projectDir, 'project.cproj'), path.join(targetDir, 'project.cproj'));
        copyDir(path.join(projectDir, 'Assets'), path.join(targetDir, 'Assets'));
        fs.mkdirSync(path.join(targetDir, 'Library'), { recursive: true });
        fs.copyFileSync(build.output, path.join(targetDir, 'Library', path.basename(build.output)));

        fs.writeFileSync(path.join(targetDir, 'README.txt'), [
            `${JSON.parse(fs.readFileSync(path.join(projectDir, 'project.cproj'), 'utf8')).name} - Cairn Engine player build`,
            '',
            'Run with:   npx electron .',
            'Requires Electron (npm install electron) and a C++ runtime.',
        ].join('\n'));
        return { ok: true, dir: targetDir };
    } catch (err) {
        return { ok: false, error: err.message };
    }
}

module.exports = { exportPlayer };
