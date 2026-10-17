// Cairn CLI - prefab commands: author, instantiate, apply and revert.
//
// A prefab is a .prefab JSON file holding a serialized subtree. Instantiating
// it adds a PrefabInstance component to the new root recording the source, so
// the instance can later be re-applied (push changes back to the file) or
// reverted (pull a fresh copy from the file).
'use strict';

const fs = require('fs');
const path = require('path');

const { def } = require('../lib/registry');
const { invalidArg, notFound } = require('../lib/errors');
const { requireEntity } = require('./entity');

function toProjectRelative(ctx, file) {
    return ctx.session.toProjectRelative(path.resolve(ctx.session.tryProject(), file));
}

def('prefab.create', {
    summary: 'Save an entity (and its children) as a .prefab asset and link it.',
    args: {
        id: { type: 'int', required: true, desc: 'Entity id to save as a prefab.' },
        file: { type: 'string', required: true, desc: 'Project-relative .prefab path, e.g. Assets/Prefabs/Crate.prefab.' },
    },
    returns: '{ok, path, guid}',
    needsEngine: true,
    needsProject: true,
    examples: ['cairn --project ./game prefab.create --id 3 --file Assets/Prefabs/Crate.prefab'],
    run(ctx, args) {
        requireEntity(ctx, args.id);
        const rel = toProjectRelative(ctx, args.file);
        const result = ctx.session.engine().createPrefab(args.id, rel);
        if (!result.ok) throw invalidArg(result.error, { id: args.id, file: args.file });
        return { ok: true, path: rel, guid: result.guid };
    },
});

def('prefab.instantiate', {
    summary: 'Instantiate a .prefab asset into the current scene (rooted at an optional parent).',
    args: {
        file: { type: 'string', required: true, desc: 'Project-relative .prefab path.' },
        parent: { type: 'int', default: 0, desc: 'Parent entity id (0 = scene root).' },
    },
    returns: '{id, name, prefab}',
    needsEngine: true,
    needsProject: true,
    examples: ['cairn --project ./game prefab.instantiate --file Assets/Prefabs/Crate.prefab'],
    run(ctx, args) {
        const rel = toProjectRelative(ctx, args.file);
        if (!fs.existsSync(ctx.session.resolveAssetPath(rel))) {
            throw notFound(`prefab not found: ${args.file}`, { file: args.file });
        }
        const id = ctx.session.engine().instantiatePrefab(rel, args.parent);
        if (!id) throw invalidArg('failed to instantiate prefab', { file: args.file });
        return { id, name: requireEntity(ctx, id).name, prefab: rel };
    },
});

def('prefab.apply', {
    summary: 'Push an instance\'s per-property overrides back into its .prefab file (its siblings follow).',
    args: {
        id: { type: 'int', required: true, desc: 'Prefab instance root entity id.' },
    },
    returns: '{ok, path}',
    needsEngine: true,
    examples: ['cairn --project ./game prefab.apply --id 5'],
    run(ctx, args) {
        const entity = requireEntity(ctx, args.id);
        const prefabComp = entity.components.find(c => c.type === 'PrefabInstance');
        if (!prefabComp) throw invalidArg(`entity ${args.id} is not a prefab instance`);
        const result = ctx.session.engine().applyPrefab(args.id);
        if (!result.ok) throw invalidArg(result.error, { id: args.id });
        return { ok: true, path: prefabComp.props.prefabPath };
    },
});

def('prefab.overrides', {
    summary: 'List the properties an instance overrides relative to its .prefab asset.',
    args: {
        id: { type: 'int', required: true, desc: 'Prefab instance root entity id.' },
    },
    returns: '{id, path, count, nodes, overrides:[{node,key,value}]}',
    needsEngine: true,
    examples: ['cairn --project ./game prefab.overrides --id 5'],
    run(ctx, args) {
        const entity = requireEntity(ctx, args.id);
        const prefabComp = entity.components.find(c => c.type === 'PrefabInstance');
        if (!prefabComp) throw invalidArg(`entity ${args.id} is not a prefab instance`);
        const engine = ctx.session.engine();
        const refreshed = engine.refreshPrefabOverrides(args.id);
        if (!refreshed.ok) throw invalidArg(refreshed.error, { id: args.id });
        const overrides = engine.prefabOverrides(args.id) || {};
        const rows = flattenOverrides(overrides);
        return {
            id: args.id,
            path: prefabComp.props.prefabPath,
            count: rows.length,
            nodes: Object.keys(overrides).length,
            overrides: rows,
        };
    },
});

def('prefab.sync', {
    summary: 'Pull the .prefab assets back into their instances, keeping each instance\'s overrides.',
    args: {
        id: { type: 'int', default: 0, desc: 'Instance root entity id, or 0 for every instance in the scene.' },
    },
    returns: '{id, synced}',
    needsEngine: true,
    needsProject: true,
    examples: ['cairn --project ./game prefab.sync', 'cairn --project ./game prefab.sync --id 5'],
    run(ctx, args) {
        const result = ctx.session.engine().syncPrefabs(args.id);
        if (!result.ok) throw invalidArg(result.error, { id: args.id });
        return { id: args.id, synced: result.count };
    },
});

def('prefab.revert', {
    summary: 'Replace a prefab instance with a fresh copy from its .prefab file.',
    args: {
        id: { type: 'int', required: true, desc: 'Prefab instance root entity id.' },
    },
    returns: '{id, name, path}',
    needsEngine: true,
    examples: ['cairn --project ./game prefab.revert --id 5'],
    run(ctx, args) {
        const entity = requireEntity(ctx, args.id);
        const prefabComp = entity.components.find(c => c.type === 'PrefabInstance');
        if (!prefabComp) throw invalidArg(`entity ${args.id} is not a prefab instance`);
        const result = ctx.session.engine().revertPrefab(args.id);
        if (!result.ok) throw invalidArg(result.error, { id: args.id });
        return { id: result.id, name: requireEntity(ctx, result.id).name, path: prefabComp.props.prefabPath };
    },
});

def('prefab.info', {
    summary: 'Inspect a .prefab asset: name, top-level entity count and asset GUID.',
    args: {
        file: { type: 'string', required: true, desc: 'Project-relative .prefab path.' },
    },
    returns: '{path, name, root, entities, guid}',
    needsProject: true,
    needsEngine: true,
    examples: ['cairn --project ./game prefab.info --file Assets/Prefabs/Crate.prefab'],
    run(ctx, args) {
        const rel = toProjectRelative(ctx, args.file);
        const abs = ctx.session.resolveAssetPath(rel);
        if (!fs.existsSync(abs)) throw notFound(`prefab not found: ${args.file}`, { file: args.file });
        let doc;
        try { doc = JSON.parse(fs.readFileSync(abs, 'utf8')); } catch (err) {
            throw invalidArg(`prefab is not valid JSON: ${err.message}`, { file: args.file });
        }
        const root = doc.root || {};
        const entities = countEntities(root);
        return {
            path: rel,
            name: doc.name || root.name || '',
            root: root.name || '',
            entities,
            guid: ctx.session.engine().assetGuid(rel),
        };
    },
});

// One row per overridden property, so an agent can read the list without
// knowing the node-path shape of the stored record.
function flattenOverrides(overrides) {
    const rows = [];
    for (const [node, entry] of Object.entries(overrides || {})) {
        for (const [key, value] of Object.entries(entry || {})) rows.push({ node, key, value });
    }
    return rows;
}

function countEntities(node) {
    if (!node || typeof node !== 'object') return 0;
    let count = 1;
    if (Array.isArray(node.children)) for (const child of node.children) count += countEntities(child);
    return count;
}
