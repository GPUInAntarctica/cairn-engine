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
