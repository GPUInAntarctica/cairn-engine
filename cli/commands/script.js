// Cairn CLI - gameplay script compilation and script class introspection.
'use strict';

const fs = require('fs');
const path = require('path');

const { def } = require('../lib/registry');
const { notFound, invalidArg } = require('../lib/errors');
const buildTools = require('../../editor/main/build.js');

def('script.sources', {
    summary: 'List the C++ sources that script.build will compile.',
    args: {},
    returns: '[{path, bytes}]',
    needsProject: true,
    examples: ['cairn --project ./game script.sources'],
    run(ctx) {
        const projectDir = ctx.session.requireProject();
        return buildTools.collectSources(projectDir).map(file => ({
            path: ctx.session.toProjectRelative(file),
            bytes: fs.statSync(file).size,
        }));
    },
});

def('script.output', {
    summary: 'Report the path of the compiled gameplay library.',
    args: {},
    returns: '{path, exists, bytes}',
    needsProject: true,
    examples: ['cairn --project ./game script.output'],
    run(ctx) {
        const file = buildTools.scriptOutputPath(ctx.session.requireProject());
        return {
            path: file,
            exists: fs.existsSync(file),
            bytes: fs.existsSync(file) ? fs.statSync(file).size : 0,
        };
    },
});

def('script.status', {
    summary: 'Report whether the compiled gameplay library is current with Assets/Scripts.',
    args: {},
    returns: '{status, output, exists, bytes, sources, headers, outputMtime, newestInput, newestInputMtime}',
    needsProject: true,
    notes: "status is 'missing' (no library built yet), 'stale' (a source or header is newer than " +
        "the library), 'current', or 'nosources' (nothing under Assets/Scripts to compile).",
    examples: ['cairn --project ./game script.status'],
    run(ctx) {
        return buildTools.scriptStatus(ctx.session.requireProject());
    },
});

def('script.build', {
    summary: 'Compile Assets/Scripts into the gameplay shared library and return compiler diagnostics.',
    args: {
        compiler: { type: 'string', desc: 'Compiler to use (default: preferences, then g++/clang++).' },
        load: { type: 'bool', default: true, desc: 'Load the freshly built library into the engine session.' },
        ifStale: { type: 'bool', default: false, desc: 'Skip the compile when script.status reports the library is current.' },
    },
    returns: '{ok, skipped, compiler, output, durationMs, diagnostics:[{file, line, column, severity, message}], classes?}',
    needsProject: true,
    notes: 'Scripts must be built before they can be attached to Script components. The engine copies the ' +
        'library to a unique file name before dlopen, so rebuilding while a scene is loaded works. ' +
        'With --ifStale the command is a no-op when the library is already newer than every source and ' +
        'header; the skipped result still names the library so it can be loaded.',
    examples: ['cairn --project ./game script.build', 'cairn --project ./game script.build --no-load',
        'cairn --project ./game script.build --ifStale'],
    async run(ctx, args) {
        const projectDir = ctx.session.requireProject();
        const result = await ctx.session.buildScripts(projectDir, args.compiler, { ifStale: args.ifStale });
        if (!result.ok && result.noSources) {
            throw notFound('no gameplay scripts found in Assets/Scripts', { output: result.output });
        }
        const payload = {
            ok: result.ok,
            skipped: result.skipped === true,
            compiler: result.compiler,
            output: result.output,
            durationMs: result.durationMs,
            diagnostics: result.diagnostics,
        };
        if (result.ok && args.load) {
            payload.classes = loadLibrary(ctx, result.output);
        }
        return payload;
    },
});

def('script.load', {
    summary: 'Load a compiled gameplay library into the engine and list its script classes.',
    args: {
        path: { type: 'path', desc: 'Library path (default: Library/GameScripts.* of the project).' },
    },
    returns: '{ok, path, classes:[{name, props:[{name, type, default}]}]}',
    needsEngine: true,
    examples: ['cairn --project ./game script.load'],
    run(ctx, args) {
        let file = args.path;
        if (!file) {
            const projectDir = ctx.session.requireProject();
            file = buildTools.scriptOutputPath(projectDir);
        }
        if (!fs.existsSync(file)) {
            throw notFound(`script library not found: ${file}`, { hint: 'run `cairn script.build` first' });
        }
        return { path: file, classes: loadLibrary(ctx, file) };
    },
});

def('script.classes', {
    summary: 'List the script classes currently loaded in the engine session.',
    args: {},
    returns: '{path, classes:[{name, props:[{name, type, default}]}]}',
    needsEngine: true,
    examples: ['cairn script.classes'],
    run(ctx) {
        if (!ctx.session._scriptPath) {
            const projectDir = ctx.session.requireProject();
            const file = buildTools.scriptOutputPath(projectDir);
            if (!fs.existsSync(file)) {
                throw invalidArg('no script library loaded or built', {
                    expected: file,
                    hint: 'run `cairn script.build` first',
                });
            }
            loadLibrary(ctx, file);
        }
        const used = new Set();
        for (const e of ctx.session.engine().getSceneTree().entities) {
            for (const c of e.components) {
                if (c.type === 'Script' && c.props && c.props.class) used.add(c.props.class);
            }
        }
        return { path: ctx.session._scriptPath, classes: ctx.session._scriptClasses || [], usedInScene: [...used] };
    },
});

def('script.compileCheck', {
    summary: 'Syntax-check gameplay scripts without producing a loadable library.',
    args: {
        compiler: { type: 'string', desc: 'Compiler to use.' },
    },
    returns: '{ok, diagnostics:[{file, line, column, severity, message}]}',
    needsProject: true,
    examples: ['cairn --project ./game script.compileCheck'],
    async run(ctx, args) {
        const projectDir = ctx.session.requireProject();
        const result = await ctx.session.buildScripts(projectDir, args.compiler);
        return { ok: result.ok, compiler: result.compiler, diagnostics: result.diagnostics };
    },
});

function loadLibrary(ctx, file) {
    const result = ctx.session.engine().loadScriptLibrary(file);
    if (!result.ok) throw invalidArg(`engine refused to load ${path.basename(file)}: ${result.error}`);
    ctx.session._scriptPath = file;
    ctx.session._scriptClasses = result.classes;
    return result.classes;
}

module.exports = {};
