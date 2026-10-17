// Cairn CLI - introspection commands: schema discovery, engine info and the
// stdin-driven batch runner (`cairn exec`) that lets an agent perform a whole
// multi-step workflow against one engine session.
'use strict';

const fs = require('fs');
const path = require('path');

const { def, schema, groups, list, lookup } = require('../lib/registry');
const { dispatch } = require('../lib/dispatch');
const constants = require('../lib/constants');
const { usage, notFound, invalidArg } = require('../lib/errors');
const { replacer } = require('../lib/output');
const { ROOT, ADDON_PATH } = require('../lib/session');
const packageJson = require(path.join(ROOT, 'package.json'));

def('engine.version', {
    summary: 'Print engine and CLI versions.',
    args: {},
    returns: '{engine, cli, node, platform}',
    examples: ['cairn engine.version'],
    run(ctx) {
        return {
            engine: ctx.session.engineVersion(),
            cli: packageJson.version,
            node: process.version,
            platform: `${process.platform}-${process.arch}`,
        };
    },
});

def('engine.info', {
    summary: 'Report the current session: project, viewport, loaded scene and play state.',
    args: {},
    returns: '{project, viewport, scene, playing, paused, addon}',
    needsEngine: false,
    examples: ['cairn engine.info --project ./game'],
    run(ctx) {
        const project = ctx.session.tryProject();
        let engine = null;
        let scene = null;
        let playing = null;
        let paused = null;
        let entities = null;
        try {
            engine = ctx.session.engine();
            playing = engine.isPlaying();
            paused = engine.isPaused();
            if (ctx.session._scenePath) {
                const tree = engine.getSceneTree();
                scene = tree.name;
                entities = tree.entities.length;
            }
        } catch (err) {
            engine = { error: err.message };
        }
        return {
            project,
            projectFile: project && fs.existsSync(path.join(project, 'project.cproj'))
                ? JSON.parse(fs.readFileSync(path.join(project, 'project.cproj'), 'utf8'))
                : null,
            viewport: { width: ctx.session.width, height: ctx.session.height },
            scene,
            scenePath: ctx.session._scenePath,
            entities,
            playing,
            paused,
            addon: { path: ADDON_PATH, available: fs.existsSync(ADDON_PATH) },
        };
    },
});

def('engine.schema', {
    summary: 'Machine-readable description of every command and argument schema.',
    args: {
        group: { type: 'string', desc: 'Only emit commands of this group.' },
    },
    returns: '{format:"cairn-cli-schema", groups:[{group, commands:[{name, args, ...}]}]}',
    notes: 'This is the discovery entry point for agents: read it once, then call commands by name.',
    examples: ['cairn engine.schema', 'cairn engine.schema --group scene'],
    run(ctx, args) {
        const full = schema();
        if (!args.group) return full;
        const groupsOut = full.groups.filter(g => g.group === args.group);
        if (groupsOut.length === 0) throw notFound(`no command group '${args.group}'`, {
            groups: full.groups.map(g => g.group),
        });
        return Object.assign({}, full, { groups: groupsOut });
    },
});

def('engine.commands', {
    summary: 'Flat list of command names and one-line summaries (compact discovery).',
    args: {},
    returns: '[{name, group, summary}]',
    examples: ['cairn engine.commands'],
    run() {
        return list().map(c => ({ name: c.name, group: c.group, summary: c.summary }));
    },
});

def('engine.groups', {
    summary: 'Command groups with their command names.',
    args: {},
    returns: '[{group, commands:[string]}]',
    examples: ['cairn engine.groups'],
    run() {
        return groups().map(({ group, commands }) => ({ group, commands: commands.map(c => c.name) }));
    },
});

def('engine.help', {
    summary: 'Usage detail for a single command (text or JSON via --format).',
    args: {
        command: { type: 'string', desc: 'Command name, e.g. entity.create or create.' },
    },
    returns: '{name, summary, args, returns, examples, notes}',
    examples: ['cairn engine.help entity.create'],
    run(ctx, args) {
        if (!args.command) {
            return {
                usage: 'cairn <command> [args] [--project DIR] [--scene FILE] [--format json|text]',
                commands: list().length,
                groups: groups().map(g => g.group),
            };
        }
        const cmd = lookup(args.command);
        if (!cmd) throw notFound(`unknown command '${args.command}'`);
        return {
            name: cmd.name,
            summary: cmd.summary,
            args: Object.entries(cmd.args).map(([name2, spec]) => Object.assign({ name: name2 }, spec)),
            returns: cmd.returns || null,
            needsProject: !!cmd.needsProject,
            notes: cmd.notes || null,
            examples: cmd.examples,
        };
    },
});

def('engine.capabilities', {
    summary: 'Static engine vocabulary: primitives, component types, key codes, scene settings.',
    args: {},
    returns: '{primitives, components, keys, mouseButtons, sceneSettings, logLevels}',
    examples: ['cairn engine.capabilities'],
    run() {
        return {
            primitives: constants.PRIMITIVES,
            components: constants.COMPONENTS,
            keys: constants.KEY_NAMES.map((name, code) => ({ name, code })),
            mouseButtons: constants.MOUSE_BUTTONS,
            sceneSettings: constants.SCENE_SETTINGS,
            logLevels: constants.LOG_LEVELS,
            frameBuffer: { format: 'RGBA8', order: 'top-left origin', note: 'getFrameBuffer returns a copy' },
        };
    },
});

def('engine.profile', {
    summary: 'Frame profiler snapshot: per-phase timings plus this frame\'s render counters.',
    args: {},
    returns: '{frame, fps, frameMs, lastFrameMs, entities, scopes:[{name, ms, avgMs, maxMs, calls}], render:{...}}',
    needsEngine: true,
    notes: 'Scopes cover simulation (update, scripts, physics) and rendering (render, gather, ' +
        'raster, shadows, post, gpu). Drive frames with runtime.update / runtime.simulate so the ' +
        'profiler has samples; avgMs is a rolling ~1s average, ms is the last frame.',
    examples: ['cairn --project ./game runtime.update --frames 120 --render',
               'cairn --project ./game engine.profile'],
    run(ctx) {
        return ctx.session.engine().profile();
    },
});

// -------------------------------------------------------------- batch runner
function readStdin() {
    try {
        return fs.readFileSync(0, 'utf8');
    } catch {
        return '';
    }
}

def('exec.run', {
    summary: 'Run a batch of commands from stdin (JSON array or JSONL) against one engine session.',
    args: {
        file: { type: 'path', desc: 'Read the batch from this file instead of stdin.' },
        stopOnError: { type: 'bool', default: true, desc: 'Abort the batch on the first failing step.' },
    },
    returns: '{steps, ok, failures:[{index, command, code, message}]}',
    notes: 'Each step is {"command":"entity.create","args":{...}}. A step may carry ' +
        '"session":{"project":"...","scene":"...","width":..,"height":..} to reconfigure the session. ' +
        'Results are streamed as JSONL, one object per step; ids returned by earlier steps can be ' +
        'referenced with {"$ref":"<id>"} placeholders.',
    examples: [
        'cairn --project ./game exec.run < steps.jsonl',
        'echo \'{"command":"scene.new","args":{"name":"Test"}}\' | cairn exec.run',
    ],
    async run(ctx, args) {
        const text = args.file ? fs.readFileSync(args.file, 'utf8') : readStdin();
        const steps = parseBatch(text);
        const failures = [];
        let ok = true;

        for (let i = 0; i < steps.length; ++i) {
            const step = steps[i];
            const record = await runStep(ctx, step, i, failures);
            process.stdout.write(JSON.stringify(record, replacer, ctx.pretty ? 2 : 0) + '\n');
            if (!record.ok) {
                ok = false;
                if (args.stopOnError) break;
            }
        }
        if (!ok) ctx.exitCode = 1;
        return { steps: steps.length, executed: ok ? steps.length : failures.length, ok, failures };
    },
});
async function runStep(ctx, step, index, failures) {
    if (!step || typeof step !== 'object' || !step.command) {
        const error = { code: 'invalid_arg', message: 'each step needs a "command" field' };
        failures.push(Object.assign({ index }, error));
        return { ok: false, index, error };
    }
    const cmd = lookup(step.command);
    if (!cmd) {
        const error = { code: 'not_found', message: `unknown command '${step.command}'` };
        failures.push(Object.assign({ index, command: step.command }, error));
        return { ok: false, index, command: step.command, error };
    }
    if (step.session) applySession(ctx, step.session);
    if (cmd.name === 'exec.run') {
        const error = { code: 'usage', message: 'exec.run cannot be nested inside a batch' };
        failures.push(Object.assign({ index, command: cmd.name }, error));
        return { ok: false, index, command: cmd.name, error };
    }
    try {
        const resolved = step.args === undefined ? step.args : resolveRefs(step.args, ctx.refs);
        const { result } = await dispatch(cmd.name, resolved, ctx, cmd);
        const idRef = step.id;
        if (idRef) {
            ctx.refs[idRef] = firstId(result);
        }
        return { ok: true, index, command: cmd.name, id: idRef || undefined, result };
    } catch (err) {
        const error = err && err.code
            ? { code: err.code, message: err.message, details: err.details }
            : { code: 'internal', message: String(err && err.message || err) };
        failures.push(Object.assign({ index, command: cmd.name }, error));
        return { ok: false, index, command: cmd.name, error };
    }
}

function firstId(result) {
    if (!result || typeof result !== 'object') return result;
    for (const key of ['id', 'entityId']) {
        if (result[key] !== undefined) return result[key];
    }
    return result;
}

function resolveRefs(args, refs) {
    if (args === null || typeof args !== 'object') return args;
    if (Array.isArray(args)) return args.map(v => resolveRefs(v, refs));
    const out = {};
    for (const [k, v] of Object.entries(args)) {
        if (typeof v === 'string' && v.startsWith('$ref:')) {
            const name = v.slice(5);
            if (refs[name] === undefined) {
                throw invalidArg(`unresolved reference '${v}'`, { known: Object.keys(refs) });
            }
            out[k] = refs[name];
        } else {
            out[k] = resolveRefs(v, refs);
        }
    }
    return out;
}

function applySession(ctx, session) {
    if (session.project) ctx.session.opts.project = session.project;
    if (session.width) ctx.session.width = Number(session.width);
    if (session.height) ctx.session.height = Number(session.height);
    if (session.viewport) {
        ctx.session.width = Number(session.viewport[0]);
        ctx.session.height = Number(session.viewport[1]);
    }
    if (session.scene) ctx.session._scenePath = null;
    if (session.scene) ctx.session.loadScene(session.scene);
}

function parseBatch(text) {
    const trimmed = text.trim();
    if (!trimmed) throw usage('no batch input on stdin');
    if (trimmed.startsWith('[')) {
        const parsed = JSON.parse(trimmed);
        if (!Array.isArray(parsed)) throw invalidArg('batch JSON must be an array of steps');
        return parsed;
    }
    return trimmed.split('\n')
        .map(line => line.trim())
        .filter(line => line && !line.startsWith('//') && !line.startsWith('#'))
        .map((line, i) => {
            try { return JSON.parse(line); }
            catch (err) { throw invalidArg(`line ${i + 1} is not valid JSON: ${err.message}`); }
        });
}

module.exports = { parseBatch };
