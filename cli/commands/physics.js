// Cairn CLI - physics queries against scene colliders.
'use strict';

const { def } = require('../lib/registry');
const { invalidArg } = require('../lib/errors');

function requireVec3(value, name) {
    if (!Array.isArray(value) || value.length !== 3 || value.some(v => typeof v !== 'number')) {
        throw invalidArg(`${name} expects [x,y,z] numbers`);
    }
    return value;
}

def('physics.raycast', {
    summary: 'Cast a ray against colliders and return the nearest hit (or every hit with --all).',
    args: {
        origin: { type: 'json', required: true, desc: '[x,y,z] world-space origin.' },
        direction: { type: 'json', required: true, desc: '[x,y,z] direction (normalized internally).' },
        maxDistance: { type: 'number', default: 1000, desc: 'Maximum distance.' },
        all: { type: 'bool', default: false, desc: 'Return every hit, nearest first.' },
    },
    returns: '{entity, guid, name, point, normal, distance} or [ ... ] (null when nothing is hit)',
    needsEngine: true,
    examples: ['cairn --project ./game physics.raycast --origin \'[0,5,0]\' --direction \'[0,-1,0]\''],
    run(ctx, args) {
        const origin = requireVec3(args.origin, 'origin');
        const direction = requireVec3(args.direction, 'direction');
        return ctx.session.engine().physicsRaycast(origin, direction, args.maxDistance, args.all);
    },
});

def('physics.overlapSphere', {
    summary: 'Return the entities whose collider overlaps a sphere.',
    args: {
        center: { type: 'json', required: true, desc: '[x,y,z] world-space center.' },
        radius: { type: 'number', required: true, desc: 'Sphere radius.' },
    },
    returns: '[{id, guid, name}]',
    needsEngine: true,
    examples: ['cairn --project ./game physics.overlapSphere --center \'[0,0,0]\' --radius 3'],
    run(ctx, args) {
        const center = requireVec3(args.center, 'center');
        return ctx.session.engine().physicsOverlapSphere(center, args.radius);
    },
});

def('physics.overlapBox', {
    summary: 'Return the entities whose collider overlaps an axis-aligned box.',
    args: {
        center: { type: 'json', required: true, desc: '[x,y,z] world-space center.' },
        size: { type: 'json', required: true, desc: '[x,y,z] full box size, as on BoxCollider.' },
    },
    returns: '[{id, guid, name}]',
    needsEngine: true,
    examples: ['cairn --project ./game physics.overlapBox --center \'[0,1,0]\' --size \'[2,2,2]\''],
    run(ctx, args) {
        const center = requireVec3(args.center, 'center');
        const size = requireVec3(args.size, 'size');
        return ctx.session.engine().physicsOverlapBox(center, size.map(v => v / 2));
    },
});

def('physics.sphereCast', {
    summary: 'Sweep a sphere along a direction and return the first hit (null when clear).',
    args: {
        origin: { type: 'json', required: true, desc: '[x,y,z] world-space center of the sphere at the start.' },
        radius: { type: 'number', required: true, desc: 'Sphere radius.' },
        direction: { type: 'json', required: true, desc: '[x,y,z] direction (normalized internally).' },
        maxDistance: { type: 'number', default: 1000, desc: 'Maximum sweep distance.' },
    },
    returns: '{entity, guid, name, point, normal, distance} (null when nothing is hit)',
    needsEngine: true,
    examples: ['cairn --project ./game physics.sphereCast --origin \'[0,5,0]\' --radius 0.4 --direction \'[0,-1,0]\''],
    run(ctx, args) {
        const origin = requireVec3(args.origin, 'origin');
        const direction = requireVec3(args.direction, 'direction');
        return ctx.session.engine().physicsSphereCast(origin, args.radius, direction, args.maxDistance);
    },
});

def('physics.boxCast', {
    summary: 'Sweep an axis-aligned box along a direction and return the first hit (null when clear).',
    args: {
        center: { type: 'json', required: true, desc: '[x,y,z] world-space center of the box at the start.' },
        size: { type: 'json', required: true, desc: '[x,y,z] full box size, as on BoxCollider.' },
        direction: { type: 'json', required: true, desc: '[x,y,z] direction (normalized internally).' },
        maxDistance: { type: 'number', default: 1000, desc: 'Maximum sweep distance.' },
    },
    returns: '{entity, guid, name, point, normal, distance} (null when nothing is hit)',
    needsEngine: true,
    examples: ['cairn --project ./game physics.boxCast --center \'[0,5,0]\' --size \'[1,1,1]\' --direction \'[0,-1,0]\''],
    run(ctx, args) {
        const center = requireVec3(args.center, 'center');
        const size = requireVec3(args.size, 'size');
        const direction = requireVec3(args.direction, 'direction');
        return ctx.session.engine().physicsBoxCast(center, size.map(v => v / 2), direction, args.maxDistance);
    },
});

def('physics.moveCharacter', {
    summary: 'Move an entity\'s CharacterController capsule and report the resolved pose.',
    args: {
        id: { type: 'int', required: true, desc: 'Entity id of a CharacterController.' },
        displacement: { type: 'json', required: true, desc: '[x,y,z] world-space movement for this call.' },
    },
    returns: '{id, grounded, position, localPosition}',
    needsEngine: true,
    examples: ['cairn --project ./game physics.moveCharacter --id 4 --displacement \'[0.1,-0.2,0]\''],
    run(ctx, args) {
        const displacement = requireVec3(args.displacement, 'displacement');
        return ctx.session.engine().characterMove(args.id, displacement);
    },
});

def('physics.step', {
    summary: 'Advance the scene physics by N fixed steps (headless simulation).',
    args: {
        dt: { type: 'number', default: 0.0166667, desc: 'Fixed timestep in seconds.' },
        steps: { type: 'int', default: 1, desc: 'Number of steps to run.' },
    },
    returns: '{steps, dt, dynamic, sleeping}',
    needsEngine: true,
    examples: ['cairn --project ./game physics.step --steps 120'],
    run(ctx, args) {
        return ctx.session.engine().physicsStep(args.dt, args.steps);
    },
});
