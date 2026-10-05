// Cairn CLI - scene document commands (create, load, save, inspect, settings).
'use strict';

const fs = require('fs');

const { def } = require('../lib/registry');
const constants = require('../lib/constants');
const { usage, invalidArg, failed } = require('../lib/errors');

def('scene.new', {
    summary: 'Create an empty in-memory scene.',
    args: {
        name: { type: 'string', default: 'Scene', desc: 'Scene name stored in scene settings.' },
    },
    returns: '{name, entities:0}',
    needsEngine: true,
    examples: ['cairn scene.new --name Sandbox'],
    run(ctx, args) {
        const engine = ctx.session.engine();
        engine.newScene(args.name);
        ctx.session._scenePath = null;
        return { name: args.name, entities: 0 };
    },
});

def('scene.load', {
    summary: 'Load a .cscene file into the session.',
    args: {
        file: { type: 'string', desc: 'Scene file (default: the --scene global option).' },
    },
    returns: '{path, name, entities}',
    needsEngine: true,
    examples: ['cairn --project ./game scene.load --file Assets/Scenes/Main.cscene'],
    run(ctx, args) {
        const engine = ctx.session.engine();
        if (!args.file) return ctx.session.loadScene(ctx.session.opts.scene);
        return ctx.session.loadScene(args.file);
    },
});

def('scene.save', {
    summary: 'Write the loaded scene back to a .cscene file.',
    args: {
        file: { type: 'string', desc: 'Target file (default: the file it was loaded from).' },
    },
    returns: '{path}',
    needsEngine: true,
    examples: ['cairn scene.save', 'cairn scene.save --file Assets/Scenes/Level2.cscene'],
    run(ctx, args) {
        return ctx.session.saveScene(args.file);
    },
});

def('scene.tree', {
    summary: 'Full scene tree: every entity with transform, components and properties.',
    args: {
        filter: { type: 'string', desc: 'Only include entities whose name contains this text.' },
        component: { type: 'string', desc: 'Only include entities that have this component.' },
    },
    returns: '{name, entities:[{id, name, active, tag, parent, transform, components:[...]}]}',
    needsEngine: true,
    notes: 'This is the canonical way for an agent to read the current scene state.',
    examples: ['cairn scene.tree', 'cairn scene.tree --component MeshRenderer'],
    run(ctx, args) {
        const engine = ctx.session.engine();
        const tree = engine.getSceneTree();
        let entities = tree.entities;
        if (args.filter) entities = entities.filter(e => e.name.toLowerCase().includes(args.filter.toLowerCase()));
        if (args.component) entities = entities.filter(e => e.components.some(c => c.type === args.component));
        return { name: tree.name, count: entities.length, entities };
    },
});

def('scene.hierarchy', {
    summary: 'Scene tree nested by parent/child relationships.',
    args: {},
    returns: '{name, roots:[{id, name, children:[...]}]}',
    needsEngine: true,
    examples: ['cairn scene.hierarchy'],
    run(ctx) {
        const engine = ctx.session.engine();
        const tree = engine.getSceneTree();
        const byId = new Map(tree.entities.map(e => [e.id, { id: e.id, name: e.name, active: e.active, children: [] }]));
        const roots = [];
        for (const e of tree.entities) {
            const node = byId.get(e.id);
            const parent = e.parent ? byId.get(e.parent) : null;
            (parent ? parent.children : roots).push(node);
        }
        return { name: tree.name, roots };
    },
});

def('scene.json', {
    summary: 'Serialize the scene to a .cscene JSON document.',
    args: {
        pretty: { type: 'bool', default: true, desc: 'Indent the JSON output.' },
        file: { type: 'path', desc: 'Also write the document to this file.' },
    },
    returns: '{json, path?}',
    needsEngine: true,
    examples: ['cairn scene.json --no-pretty'],
    run(ctx, args) {
        const engine = ctx.session.engine();
        const compact = engine.sceneJSON();
        const json = args.pretty ? JSON.stringify(JSON.parse(compact), null, 2) : compact;
        const out = { json };
        if (args.file) {
            fs.writeFileSync(args.file, json);
            out.path = args.file;
        }
        return out;
    },
});

def('scene.apply', {
    summary: 'Replace the current scene with a .cscene JSON document (undo/paste helper).',
    args: {
        json: { type: 'string', desc: 'Scene JSON string, @file path or @- for stdin.' },
        file: { type: 'path', desc: 'Read the scene JSON from this file.' },
    },
    returns: '{ok, entities}',
    needsEngine: true,
    examples: ['cairn scene.apply --file /tmp/scene.cscene', 'echo "$JSON" | cairn scene.apply --json @-'],
    run(ctx, args) {
        const engine = ctx.session.engine();
        let text;
        if (args.file) text = fs.readFileSync(args.file, 'utf8');
        else if (args.json) text = args.json.startsWith('@') ? readRef(args.json) : args.json;
        else throw usage('scene.apply needs --file or --json');
        const result = engine.restoreSceneJSON(text);
        if (!result.ok) throw failed('scene_apply_failed', result.error);
        return { ok: true, entities: engine.getSceneTree().entities.length };
    },
});

def('scene.snapshot', {
    summary: 'Capture the scene as JSON for later restore (undo helper).',
    args: {
        file: { type: 'path', desc: 'Write the snapshot to this file instead of returning it.' },
    },
    returns: '{json, path?}',
    needsEngine: true,
    examples: ['cairn scene.snapshot --file /tmp/before.json'],
    run(ctx, args) {
        const json = ctx.session.engine().sceneJSON();
        if (args.file) {
            fs.writeFileSync(args.file, json);
            return { path: args.file, bytes: Buffer.byteLength(json) };
        }
        return { json };
    },
});

def('scene.settings.get', {
    summary: 'Read scene settings (gravity, ambient, background, fog, name).',
    args: {},
    returns: '{name, gravity, ambient, background, fogEnabled, fogColor, fogDensity, tonemap, exposure}',
    needsEngine: true,
    examples: ['cairn scene.settings.get'],
    run(ctx) {
        return ctx.session.engine().getSceneSettings();
    },
});

def('scene.settings.set', {
    summary: 'Set one scene setting by name.',
    args: {
        key: { type: 'enum', enum: constants.SCENE_SETTINGS, required: true, desc: 'Setting name.' },
        value: { type: 'json', required: true, desc: 'Value: vector/array for colors and gravity, number, bool or string.' },
    },
    returns: '{key, value, settings}',
    needsEngine: true,
    examples: [
        'cairn scene.settings.set --key gravity --value \'[0,-20,0]\'',
        'cairn scene.settings.set --key fogEnabled --value true',
    ],
    run(ctx, args) {
        const engine = ctx.session.engine();
        let value = args.value;
        if (args.key === 'tonemap') {
            // Accept the operator name as well as its integer index.
            const named = String(value).toLowerCase();
            value = constants.TONEMAPS[named] !== undefined ? constants.TONEMAPS[named] : value;
            if (typeof value !== 'number') {
                throw invalidArg('--value must be a tone mapping name or index',
                    { names: Object.keys(constants.TONEMAPS) });
            }
        }
        if ((args.key === 'gravity' || args.key.startsWith('Color') || args.key === 'ambient' || args.key === 'background')
            && Array.isArray(value)) {
            value = value.map(Number);
            if (value.length !== (args.key === 'gravity' ? 3 : 4)) {
                throw invalidArg(`${args.key} expects ${args.key === 'gravity' ? 3 : 4} components`);
            }
        }
        const ok = engine.setSceneSetting(args.key, value);
        if (!ok) throw invalidArg(`engine rejected setting '${args.key}'`);
        return { key: args.key, value, settings: engine.getSceneSettings() };
    },
});

def('scene.entities', {
    summary: 'Compact entity index: id, name, tag, parent, active, component types.',
    args: {
        tag: { type: 'string', desc: 'Only entities with this tag.' },
    },
    returns: '[{id, name, tag, parent, active, components:[string]}]',
    needsEngine: true,
    examples: ['cairn scene.entities'],
    run(ctx, args) {
        let entities = ctx.session.engine().getSceneTree().entities;
        if (args.tag) entities = entities.filter(e => e.tag === args.tag);
        return entities.map(e => ({
            id: e.id,
            name: e.name,
            tag: e.tag,
            parent: e.parent,
            active: e.active,
            components: e.components.map(c => c.type),
        }));
    },
});

def('scene.find', {
    summary: 'Find entities by name or tag.',
    args: {
        name: { type: 'string', desc: 'Exact name match.' },
        tag: { type: 'string', desc: 'Exact tag match.' },
    },
    returns: '[{id, name, tag, active}]',
    needsEngine: true,
    examples: ['cairn scene.find --tag Enemy'],
    run(ctx, args) {
        const entities = ctx.session.engine().getSceneTree().entities;
        return entities
            .filter(e => (!args.name || e.name === args.name) && (!args.tag || e.tag === args.tag))
            .map(e => ({ id: e.id, name: e.name, tag: e.tag, active: e.active, parent: e.parent }));
    },
});

def('scene.stats', {
    summary: 'Renderer-side counters for the current scene (objects, mesh renderers, draw calls).',
    args: {},
    returns: '{objects, activeInHierarchy, meshRenderers, meshNull, emitted, emittedNames, viewport}',
    needsEngine: true,
    examples: ['cairn scene.stats'],
    run(ctx) {
        return ctx.session.engine().debugCollect();
    },
});

function readRef(ref) {
    return ref === '@-' ? fs.readFileSync(0, 'utf8') : fs.readFileSync(ref.slice(1), 'utf8');
}

module.exports = {};
