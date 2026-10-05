// Cairn CLI - entity creation, hierarchy and transform commands.
'use strict';

const { def } = require('../lib/registry');
const constants = require('../lib/constants');
const { invalidArg, notFound } = require('../lib/errors');

function requireEntity(ctx, id) {
    const entity = ctx.session.engine().getEntityJSON(id);
    if (!entity) throw notFound(`entity ${id} not found in the current scene`);
    return entity;
}

def('entity.create', {
    summary: 'Create an empty GameObject.',
    args: {
        name: { type: 'string', default: 'GameObject', desc: 'Entity name.' },
        parent: { type: 'int', default: 0, desc: 'Parent entity id (0 = scene root).' },
        position: { type: 'json', desc: '[x,y,z] local position.' },
    },
    returns: '{id, name, parent}',
    needsEngine: true,
    examples: ['cairn entity.create --name Player --position \'[0,1,0]\''],
    run(ctx, args) {
        const engine = ctx.session.engine();
        const id = engine.createEntity(args.name, args.parent);
        if (!id) throw invalidArg('engine refused to create the entity');
        if (args.position) setTransform(ctx, id, { position: args.position });
        return { id, name: args.name, parent: args.parent };
    },
});

def('entity.createPrimitive', {
    summary: 'Create a GameObject with a MeshRenderer using a built-in primitive mesh.',
    args: {
        mesh: { type: 'enum', enum: constants.PRIMITIVES, required: true, desc: 'Primitive mesh name.' },
        name: { type: 'string', default: 'GameObject', desc: 'Entity name.' },
        parent: { type: 'int', default: 0, desc: 'Parent entity id (0 = scene root).' },
        position: { type: 'json', desc: '[x,y,z] local position.' },
        scale: { type: 'json', desc: '[x,y,z] local scale.' },
    },
    returns: '{id, name, mesh, components:[string]}',
    needsEngine: true,
    examples: ['cairn entity.createPrimitive --mesh Cube --name Floor --position \'[0,-0.5,0]\''],
    run(ctx, args) {
        const engine = ctx.session.engine();
        const id = engine.createPrimitive(args.mesh, args.name, args.parent);
        if (!id) throw invalidArg('engine refused to create the entity');
        if (args.position || args.scale) setTransform(ctx, id, { position: args.position, scale: args.scale });
        const entity = requireEntity(ctx, id);
        return { id, name: entity.name, mesh: args.mesh, components: entity.components.map(c => c.type) };
    },
});

def('entity.createSprite', {
    summary: 'Create a GameObject with a SpriteRenderer (2D).',
    args: {
        name: { type: 'string', default: 'Sprite', desc: 'Entity name.' },
        parent: { type: 'int', default: 0, desc: 'Parent entity id (0 = scene root).' },
        texture: { type: 'string', desc: 'Project-relative texture path, e.g. Assets/Textures/player.png.' },
        position: { type: 'json', desc: '[x,y,z] local position.' },
        color: { type: 'color', desc: 'Tint color (hex or [r,g,b,a]).' },
    },
    returns: '{id, name, texture}',
    needsEngine: true,
    examples: ['cairn entity.createSprite --name Hero --texture Assets/Textures/player.png'],
    run(ctx, args) {
        const engine = ctx.session.engine();
        const id = engine.createSprite(args.name, args.parent);
        if (!id) throw invalidArg('engine refused to create the entity');
        if (args.position) setTransform(ctx, id, { position: args.position });
        if (args.texture) {
            engine.setComponentProp(id, 'SpriteRenderer', 'texturePath',
                ctx.session.toProjectRelative(ctx.session.resolveAssetPath(args.texture)));
        }
        if (args.color) engine.setComponentProp(id, 'SpriteRenderer', 'color', args.color);
        return { id, name: args.name, texture: args.texture || null };
    },
});

def('entity.duplicate', {
    summary: 'Duplicate an entity (deep copy including children).',
    args: { id: { type: 'int', required: true, desc: 'Entity id to duplicate.' } },
    returns: '{id, name, source}',
    needsEngine: true,
    examples: ['cairn entity.duplicate --id 3'],
    run(ctx, args) {
        requireEntity(ctx, args.id);
        const id = ctx.session.engine().duplicateEntity(args.id);
        if (!id) throw invalidArg('engine refused to duplicate the entity');
        return { id, source: args.id, name: requireEntity(ctx, id).name };
    },
});

def('entity.destroy', {
    summary: 'Destroy an entity and its children.',
    args: { id: { type: 'int', required: true, desc: 'Entity id.' } },
    returns: '{destroyed, id}',
    needsEngine: true,
    examples: ['cairn entity.destroy --id 4'],
    run(ctx, args) {
        requireEntity(ctx, args.id);
        ctx.session.engine().destroyEntity(args.id);
        return { destroyed: true, id: args.id };
    },
});

def('entity.get', {
    summary: 'Full description of one entity: transform, components, properties and editor metadata.',
    args: { id: { type: 'int', required: true, desc: 'Entity id.' } },
    returns: '{id, name, active, tag, parent, transform, components:[...]}',
    needsEngine: true,
    examples: ['cairn entity.get --id 1'],
    run(ctx, args) {
        return requireEntity(ctx, args.id);
    },
});

def('entity.set', {
    summary: 'Change entity name, active flag, tag, parent or several at once.',
    args: {
        id: { type: 'int', required: true, desc: 'Entity id.' },
        name: { type: 'string', desc: 'New name.' },
        active: { type: 'bool', desc: 'New active flag.' },
        tag: { type: 'string', desc: 'New tag (empty string clears it).' },
        parent: { type: 'int', desc: 'New parent entity id (0 detaches to the scene root).' },
        fields: { type: 'json', desc: 'Object with any of name/active/tag/parent applied together.' },
    },
    returns: '{id, name, active, tag, parent}',
    needsEngine: true,
    examples: ['cairn entity.set --id 3 --tag Enemy --no-active'],
    run(ctx, args) {
        const engine = ctx.session.engine();
        requireEntity(ctx, args.id);
        const fields = Object.assign({}, args.fields || {});
        if (args.name !== undefined) fields.name = args.name;
        if (args.active !== undefined) fields.active = args.active;
        if (args.tag !== undefined) fields.tag = args.tag;
        if (args.parent !== undefined) fields.parent = args.parent;
        for (const key of Object.keys(fields)) {
            if (!['name', 'active', 'tag', 'parent'].includes(key)) {
                throw invalidArg(`entity.set cannot change '${key}'`, { allowed: ['name', 'active', 'tag', 'parent'] });
            }
        }
        if (fields.name !== undefined) engine.setEntityName(args.id, String(fields.name));
        if (fields.active !== undefined) engine.setEntityActive(args.id, !!fields.active);
        if (fields.tag !== undefined) engine.setEntityTag(args.id, String(fields.tag));
        if (fields.parent !== undefined) engine.setEntityParent(args.id, Number(fields.parent) || 0);
        const entity = requireEntity(ctx, args.id);
        return { id: entity.id, name: entity.name, active: entity.active, tag: entity.tag, parent: entity.parent };
    },
});

def('entity.transform', {
    summary: 'Set local position, rotation (Euler degrees) and/or scale.',
    args: {
        id: { type: 'int', required: true, desc: 'Entity id.' },
        position: { type: 'json', desc: '[x,y,z] local position.' },
        rotation: { type: 'json', desc: '[x,y,z] Euler degrees (YXZ order, as the inspector shows).' },
        scale: { type: 'json', desc: '[x,y,z] local scale.' },
        move: { type: 'json', desc: '[dx,dy,dz] added to the current position.' },
    },
    returns: '{id, transform}',
    needsEngine: true,
    examples: ['cairn entity.transform --id 2 --position \'[0,1.6,0]\' --rotation \'[0,90,0]\''],
    run(ctx, args) {
        const id = args.id;
        const current = requireEntity(ctx, id).transform;
        const patch = {};
        if (args.position) patch.position = args.position;
        if (args.rotation) patch.rotation = args.rotation;
        if (args.scale) patch.scale = args.scale;
        if (args.move) patch.position = current.position.map((v, i) => v + Number(args.move[i] || 0));
        if (!Object.keys(patch).length) return { id, transform: current };
        setTransform(ctx, id, patch);
        return { id, transform: requireEntity(ctx, id).transform };
    },
});

def('entity.highlight', {
    summary: 'Set or clear the editor selection outline entity.',
    args: { id: { type: 'int', default: 0, desc: 'Entity id; 0 clears the highlight.' } },
    returns: '{highlight}',
    needsEngine: true,
    examples: ['cairn entity.highlight --id 2'],
    run(ctx, args) {
        ctx.session.engine().setHighlightEntity(args.id);
        return { highlight: args.id };
    },
});

function setTransform(ctx, id, patch) {
    const engine = ctx.session.engine();
    for (const [prop, value] of Object.entries(patch)) {
        if (value === undefined || value === null) continue;
        if (!Array.isArray(value) || value.length !== 3 || value.some(v => typeof v !== 'number')) {
            throw invalidArg(`transform.${prop} expects [x,y,z] numbers`);
        }
        const ok = engine.setComponentProp(id, 'Transform', prop, value);
        if (!ok) throw invalidArg(`engine rejected transform.${prop}`);
    }
}

module.exports = { requireEntity };
