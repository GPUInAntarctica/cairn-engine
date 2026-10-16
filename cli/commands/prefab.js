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
    summary: 'Push an instance\'s current state back into its .prefab file.',
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

function countEntities(node) {
    if (!node || typeof node !== 'object') return 0;
    let count = 1;
    if (Array.isArray(node.children)) for (const child of node.children) count += countEntities(child);
    return count;
}
