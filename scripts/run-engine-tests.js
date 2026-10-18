#!/usr/bin/env node
// Runs the C++ engine unit tests (engine/tests/engine_test.cpp), built by CMake
// alongside the engine library.
//
// The suite renders a few dozen frames through the real frame pipeline, so it
// pins the software rasteriser: on the GPU backend every frame blocks on a
// readback fence, which is needless contention for a test that only asserts
// pixel counts and material ordering.
'use strict';

const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');
// Shared with scripts/build.js: the same per-configuration fallback applies to
// the test executable, which a multi-config generator would otherwise put in
// engine/build/Release/ next to the library.
const { engineOutputDirs } = require('./build.js');

const root = path.resolve(__dirname, '..');

function findBinary() {
    const names = process.platform === 'win32'
        ? ['cairn_engine_test.exe']
        : ['cairn_engine_test'];
    const dirs = [...engineOutputDirs(path.join(root, 'engine', 'build')), path.join(root, 'build')];
    for (const dir of dirs) {
        for (const name of names) {
            const p = path.join(dir, name);
            if (fs.existsSync(p)) return p;
        }
    }
    return null;
}

const binary = findBinary();
if (!binary) {
    console.error('engine tests: cairn_engine_test not found.');
    console.error('  Build it first:  npm run build');
    process.exit(1);
}

const r = spawnSync(binary, [], {
    stdio: 'inherit',
    env: Object.assign({}, process.env, {
        // Only speaks for scenes left on Auto; tests that pin a backend keep it.
        CAIRN_RENDER_BACKEND: process.env.CAIRN_RENDER_BACKEND || 'software',
    }),
});
process.exit(r.status === null ? 1 : r.status);
