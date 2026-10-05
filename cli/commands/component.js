// Cairn CLI - component and property commands, including the reflection schema
// that tells an agent which properties exist and what type each one takes.
'use strict';

const { def } = require('../lib/registry');
const constants = require('../lib/constants');
const { invalidArg, notFound, failed } = require('../lib/errors');
const { requireEntity } = require('./entity');

function requireComponent(ctx, id, type) {
    const entity = requireEntity(ctx, id);
    const component = entity.components.find(c => c.type === type);
    if (!component) {
        throw notFound(`entity ${id} has no ${type} component`, {
            available: entity.components.map(c => c.type),
        });
    }
    return component;
}

def('component.add', {
    summary: 'Add a component to an entity (properties get their defaults).',
    args: {
        id: { type: 'int', required: true, desc: 'Entity id.' },
        type: { type: 'enum', enum: constants.COMPONENTS, required: true, desc: 'Component type name.' },
        props: { type: 'json', desc: 'Object of initial property values, e.g. {"intensity":2}.' },
    },
    returns: '{id, type, component}',
    needsEngine: true,
    examples: ['cairn component.add --id 3 --type RigidBody --props \'{"mass":2}\''],
    run(ctx, args) {
        const engine = ctx.session.engine();
        requireEntity(ctx, args.id);
        const ok = engine.addComponent(args.id, args.type);
        if (!ok) throw invalidArg(`engine refused to add ${args.type} to entity ${args.id}`);
        if (args.props) setProps(ctx, args.id, args.type, args.props);
        return { id: args.id, type: args.type, component: requireComponent(ctx, args.id, args.type) };
    },
});

def('component.remove', {
    summary: 'Remove a component from an entity.',
    args: {
        id: { type: 'int', required: true, desc: 'Entity id.' },
        type: { type: 'string', required: true, desc: 'Component type name.' },
    },
    returns: '{id, type, removed:true}',
    needsEngine: true,
    examples: ['cairn component.remove --id 3 --type BoxCollider'],
    run(ctx, args) {
        const engine = ctx.session.engine();
        requireComponent(ctx, args.id, args.type);
        const ok = engine.removeComponent(args.id, args.type);
        if (!ok) throw invalidArg(`engine refused to remove ${args.type} from entity ${args.id}`);
        return { id: args.id, type: args.type, removed: true };
    },
});

def('component.list', {
    summary: 'List components of one entity, or of every entity in the scene.',
    args: {
        id: { type: 'int', desc: 'Entity id; omit to list components across the scene.' },
        type: { type: 'string', desc: 'Only report this component type.' },
    },
    returns: '[{id, name, type, enabled}]',
    needsEngine: true,
    examples: ['cairn component.list --id 3', 'cairn component.list --type Camera'],
    run(ctx, args) {
        const engine = ctx.session.engine();
        if (args.id) {
            const entity = requireEntity(ctx, args.id);
            return entity.components
                .filter(c => !args.type || c.type === args.type)
                .map(c => ({ id: entity.id, name: entity.name, type: c.type, enabled: c.enabled }));
        }
        const out = [];
        for (const entity of engine.getSceneTree().entities) {
            for (const c of entity.components) {
                if (args.type && c.type !== args.type) continue;
                out.push({ id: entity.id, name: entity.name, type: c.type, enabled: c.enabled });
            }
        }
        return out;
    },
});

def('component.get', {
    summary: 'Read one component with all its property values and editor metadata.',
    args: {
        id: { type: 'int', required: true, desc: 'Entity id.' },
        type: { type: 'string', required: true, desc: 'Component type name.' },
    },
    returns: '{type, enabled, props, meta}',
    needsEngine: true,
    examples: ['cairn component.get --id 3 --type MeshRenderer'],
    run(ctx, args) {
        return requireComponent(ctx, args.id, args.type);
    },
});

def('component.set', {
    summary: 'Set a single component property.',
    args: {
        id: { type: 'int', required: true, desc: 'Entity id.' },
        type: { type: 'string', required: true, desc: 'Component type name.' },
        prop: { type: 'string', required: true, desc: 'Property name (see component.get meta).' },
        value: { type: 'json', required: true, desc: 'Property value: number, bool, string, [x,y], [r,g,b,a] or object.' },
    },
    returns: '{id, type, prop, value, component}',
    needsEngine: true,
    notes: 'Transform properties use type "Transform" with props position/rotation/scale. ' +
        'Enum-valued props take integers; run component.schema to see the options.',
    examples: [
        'cairn component.set --id 3 --type MeshRenderer --prop albedoColor --value \'[0.9,0.4,0.2,1]\'',
        'cairn component.set --id 3 --type Script --prop class --value Rotator',
    ],
    run(ctx, args) {
        setProp(ctx, args.id, args.type, args.prop, args.value);
        return {
            id: args.id,
            type: args.type,
            prop: args.prop,
            value: args.value,
            component: requireComponent(ctx, args.id, args.type),
        };
    },
});

def('component.setProps', {
    summary: 'Set several properties of one component in a single call.',
    args: {
        id: { type: 'int', required: true, desc: 'Entity id.' },
        type: { type: 'string', required: true, desc: 'Component type name.' },
        props: { type: 'json', required: true, desc: 'Object of property name to value.' },
    },
    returns: '{id, type, component}',
    needsEngine: true,
    examples: ['cairn component.setProps --id 3 --type RigidBody --props \'{"mass":2,"useGravity":false}\''],
    run(ctx, args) {
        if (typeof args.props !== 'object' || Array.isArray(args.props)) {
            throw invalidArg('--props expects an object of property/value pairs');
        }
        setProps(ctx, args.id, args.type, args.props);
        return { id: args.id, type: args.type, component: requireComponent(ctx, args.id, args.type) };
    },
});

def('component.getProp', {
    summary: 'Read a single component property value.',
    args: {
        id: { type: 'int', required: true, desc: 'Entity id.' },
        type: { type: 'string', required: true, desc: 'Component type name.' },
        prop: { type: 'string', required: true, desc: 'Property name.' },
    },
    returns: '{id, type, prop, value}',
    needsEngine: true,
    examples: ['cairn component.getProp --id 3 --type Light --prop intensity'],
    run(ctx, args) {
        requireComponent(ctx, args.id, args.type);
        const value = ctx.session.engine().getComponentProp(args.id, args.type, args.prop);
        if (value === null) throw notFound(`property '${args.prop}' not found on ${args.type}`);
        return { id: args.id, type: args.type, prop: args.prop, value };
    },
});

def('component.enable', {
    summary: 'Enable or disable a component.',
    args: {
        id: { type: 'int', required: true, desc: 'Entity id.' },
        type: { type: 'string', required: true, desc: 'Component type name.' },
        enabled: { type: 'bool', default: true, desc: 'Target state.' },
    },
    returns: '{id, type, enabled}',
    needsEngine: true,
    examples: ['cairn component.enable --id 3 --type MeshRenderer --no-enabled'],
    run(ctx, args) {
        setProp(ctx, args.id, args.type, 'enabled', args.enabled);
        return { id: args.id, type: args.type, enabled: args.enabled };
    },
});

def('component.schema', {
    summary: 'Reflection schema: every component type with its properties, kinds and defaults.',
    args: {
        type: { type: 'string', desc: 'Only report this component type.' },
        includeTransform: { type: 'bool', default: true, desc: 'Include the Transform pseudo-component.' },
    },
    returns: '{components:[{type, props:[{name, label, kind, options, default}]}]}',
    needsEngine: true,
    notes: 'Probes a throwaway entity for each component type and restores the current scene afterwards, ' +
        'so it is safe to call at any time. Script component props depend on the loaded script library: ' +
        'load one first (script.load) to see them.',
    examples: ['cairn component.schema', 'cairn component.schema --type MeshRenderer'],
    run(ctx, args) {
        const engine = ctx.session.engine();
        const snapshot = engine.sceneJSON();
        const types = args.type ? [args.type] : (args.includeTransform ? ['Transform'].concat(constants.COMPONENTS) : constants.COMPONENTS);
        const out = [];
        try {
            engine.newScene('__schema__');
            for (const type of types) {
                const id = engine.createEntity('probe');
                if (type !== 'Transform' && !engine.addComponent(id, type)) continue;
                const entity = engine.getEntityJSON(id);
                const component = entity.components.find(c => c.type === type);
                if (component) out.push({ type, props: (component.meta || []).map(m => m.name), schema: component.meta || [], values: component.props || {} });
                engine.destroyEntity(id);
            }
        } finally {
            engine.restoreSceneJSON(snapshot);
        }
        return { components: out };
    },
});

function setProp(ctx, id, type, prop, value) {
    const engine = ctx.session.engine();
    requireEntity(ctx, id);
    const ok = engine.setComponentProp(id, type, prop, value);
    if (!ok) {
        throw failed('prop_rejected',
            `engine rejected ${type}.${prop} = ${JSON.stringify(value)}`,
            { id, type, prop, hint: 'run `cairn component.schema` to list valid properties' });
    }
}

function setProps(ctx, id, type, props) {
    for (const [prop, value] of Object.entries(props)) setProp(ctx, id, type, prop, value);
}

module.exports = { requireComponent };
