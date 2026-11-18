#!/usr/bin/env node
// Cairn Engine - editor transform maths.
//
// The viewport's drag rules live in editor/renderer/src/ui/transform_drag.mjs
// so they can be checked without a browser, an engine or a GPU. Each assertion
// here pins a sign or a clamp that a real pointer drag depends on.
'use strict';

const path = require('path');
const fs = require('fs');
const { pathToFileURL } = require('url');
const imaging = require('../editor/main/imaging.js');

let failures = 0;
function check(cond, label) {
    if (cond) { console.log(`  ok  ${label}`); }
    else { console.error(`FAIL  ${label}`); failures++; }
}
function near(a, b, eps = 1e-6) { return Math.abs(a - b) <= eps; }
function nearVec(a, b, eps = 1e-6) {
    return a.length === b.length && a.every((v, i) => near(v, b[i], eps));
}

async function main() {
    const m = await import(pathToFileURL(path.join(
        __dirname, '..', 'editor', 'renderer', 'src', 'ui', 'transform_drag.mjs')).href);

    console.log('editor: camera basis');
    check(nearVec(m.cameraRight(0), [1, 0, 0]), 'yaw 0 right is +X');
    check(nearVec(m.cameraForward(0, 0), [0, 0, -1]), 'yaw 0 pitch 0 forward is -Z');
    check(nearVec(m.cameraUp(0, 0), [0, 1, 0]), 'yaw 0 pitch 0 up is +Y');
    check(nearVec(m.cameraRight(90), [0, 0, -1]), 'yaw 90 right is -Z');
    const right = m.cameraRight(40);
    const fwd = m.cameraForward(40, -32);
    const up = m.cameraUp(40, -32);
    check(near(Math.hypot(...up), 1), 'camera up is normalised at pitch -32');
    check(near(right[0] * up[0] + right[1] * up[1] + right[2] * up[2], 0), 'camera up is perpendicular to right');
    check(near(fwd[0] * up[0] + fwd[1] * up[1] + fwd[2] * up[2], 0), 'camera up is perpendicular to forward');
    check(up[1] > 0, 'camera up points above the horizon at pitch -32');

    console.log('editor: world units per pixel');
    const perspective = { depth: 10, fovDeg: 60, viewportHeight: 500 };
    check(near(m.worldPerPixel(perspective), 2 * 10 * Math.tan(Math.PI / 6) / 500),
        'perspective scale matches the object-depth plane');
    check(near(m.worldPerPixel({ ...perspective, depth: 20 }), 2 * m.worldPerPixel(perspective)),
        'doubling the object depth doubles world units per pixel');
    check(near(m.worldPerPixel({ ortho: true, orthoSize: 6, viewportHeight: 500 }), 0.024),
        'orthographic scale is 2 * orthoSize / height');
    check(m.worldPerPixel({ viewportHeight: 0 }) === 0, 'zero-height viewport has no scale');

    console.log('editor: camera-plane move');
    const view = { yaw: 0, pitch: 0, ortho: false, orthoSize: 6, depth: 10, fovDeg: 60, viewportHeight: 500 };
    const px = m.worldPerPixel(view);
    check(nearVec(m.moveInCameraPlane([0, 0, 0], 10, 0, view), [10 * px, 0, 0]),
        'horizontal drag moves along camera right');
    check(nearVec(m.moveInCameraPlane([0, 0, 0], 0, 10, view), [0, -10 * px, 0]),
        'dragging down moves along camera down');
    check(nearVec(m.moveInCameraPlane([0, 0, 0], 0, -10, view), [0, 10 * px, 0]),
        'dragging up moves along camera up');
    const side = { ...view, yaw: 90 };
    check(nearVec(m.moveInCameraPlane([0, 0, 0], 10, 0, side), [0, 0, -10 * px]),
        'at yaw 90, screen right is world -Z');
    const snapped = m.moveInCameraPlane([0, 0, 0], 13, 7, view, 0.25);
    check(snapped.every(v => near(v / 0.25, Math.round(v / 0.25))), 'Shift move lands on the 0.25 grid');

    console.log('editor: axis-constrained move');
    check(nearVec(m.axisMove([1, 2, 3], [0, 1, 0], 25, 50, 2), [1, 3, 3]),
        '25 px along a 50 px handle for 2 world units moves 1 unit');
    check(nearVec(m.axisMove([1, 2, 3], [0, 1, 0], -25, 50, 2), [1, 1, 3]),
        'dragging back along the handle moves the other way');
    check(nearVec(m.axisMove([1, 2, 3], [0, 1, 0], 0, 50, 2), [1, 2, 3]),
        'a perpendicular drag changes no coordinate');
    check(nearVec(m.axisMove([1, 2, 3], [0, 1, 0], 25, 0, 2), [1, 2, 3]),
        'a zero-length handle cannot produce movement');
    check(nearVec(m.axisMove([1, 2, 3], [1, 0, 0], 10, 50, 2), [1.4, 2, 3]),
        'the axis direction selects which coordinate moves');
    check(nearVec(m.axisMove([1, 2, 3], [0, 1, 0], 10, 50, 2, 0.25), [1, 2.5, 3]),
        'Shift axis drag snaps the result to the grid');
    check(nearVec(m.axisMove([0.12, 2.03, 3.07], [0, 1, 0], 12.5, 50, 2, 0.25), [0.12, 2.5, 3.07]),
        'axis snapping leaves the two off-axis coordinates untouched');

    console.log('editor: rotate drag');
    check(nearVec(m.rotateFromDrag([10, 20, 30], 10, 0), [10, 25, 30]),
        'horizontal drag changes yaw only');
    check(nearVec(m.rotateFromDrag([10, 20, 30], 0, 10), [15, 20, 30]),
        'vertical drag changes pitch only');
    check(nearVec(m.rotateFromDrag([10, 20, 30], 10, 10), [15, 25, 30]),
        'diagonal drag uses both axes independently');
    check(nearVec(m.rotateFromDrag([10, 20, 30], 0, -10), [5, 20, 30]),
        'dragging up turns the visible face up');
    check(nearVec(m.rotateFromDrag([10, 20, 30], 40, 40, { snapStep: 15 }), [25, 35, 30]),
        'Shift rotation snaps the delta to 15 degrees');
    check(nearVec(m.rotateFromDrag([10, 20, 30], 10, 0, { sensitivity: 1 }), [10, 30, 30]),
        'rotation sensitivity scales the drag');

    console.log('editor: scale drag');
    check(nearVec(m.scaleFromDrag([2, 4, 6], -100), [4, 8, 12]), 'dragging up doubles the scale');
    check(nearVec(m.scaleFromDrag([2, 4, 6], 50), [1, 2, 3]), 'dragging down halves the scale');
    check(nearVec(m.scaleFromDrag([2, 4, 6], 1000), [0.02, 0.04, 0.06]),
        'a long downward drag clamps the factor positive instead of flipping the object');
    check(nearVec(m.scaleFromDrag([-2, 1, 1], -100), [-4, 2, 2]),
        'a mirrored entity stays mirrored');
    check(nearVec(m.scaleFromDrag([2, 4, 6], -13, { snapStep: 0.1 }), [2.2, 4.4, 6.6]),
        'Shift scale snaps the delta to 10 percent');

    console.log('editor: gizmo handles');
    const line = m.handleScreen({ x: 0, y: 0 }, { x: 30, y: 40 });
    check(nearVec(line.dir, [0.6, 0.8]) && near(line.length, 50), 'handle reports direction and length');
    check(m.handleScreen({ x: 5, y: 5 }, { x: 5, y: 5 }) === null, 'a zero-length handle is rejected');
    const css = m.deviceToCss(300, 150, 1.5, 1.5);
    check(near(css.x, 200) && near(css.y, 100), 'device pixels divide by the overlay scale');
    check(nearVec([...Object.values(m.deviceToCss(300, 150, 1, 1))], [300, 150]),
        'a CSS-sized overlay leaves the projection unchanged');
    const guarded = m.deviceToCss(300, 150, 0, 0);
    check(near(guarded.x, 300) && near(guarded.y, 150), 'a zero scale is ignored rather than dividing by zero');
    const handles = [
        { origin: { x: 0, y: 0 }, tip: { x: 100, y: 0 } },
        { origin: { x: 0, y: 0 }, tip: { x: 0, y: 100 } },
    ];
    check(m.hitHandle({ x: 50, y: 3 }, handles) === 0, 'a point near the horizontal handle selects it');
    check(m.hitHandle({ x: 3, y: 50 }, handles) === 1, 'a point near the vertical handle selects it');
    check(m.hitHandle({ x: 50, y: 30 }, handles) === -1, 'a point away from every handle selects none');
    check(m.hitHandle({ x: 5, y: 2 }, handles) === -1,
        'the shared origin is left to the camera-plane drag');
    check(m.hitHandle({ x: 100, y: 2 }, handles) === 0, 'the tip remains part of the handle');
    check(m.hitHandle({ x: 50, y: 3 }, handles, 2) === -1, 'the threshold is honoured');

    console.log('editor: pointer projection');
    check(near(m.alongHandle(10, 5, [1, 0]), 10), 'projection ignores the perpendicular component');
    check(near(m.alongHandle(10, 5, [0, 1]), 5), 'projection keeps the parallel component');
    check(near(m.alongHandle(10, 5, [0.6, 0.8]), 10), 'projection handles a diagonal handle');
    check(near(m.snap(0.37, 0.25), 0.25) && near(m.snap(0.12, 0.25), 0),
        'snap rounds to the nearest step');

    console.log('editor: scene defaults');
    const defaults = await import(pathToFileURL(path.join(
        __dirname, '..', 'editor', 'renderer', 'src', 'scene_defaults.mjs')).href);
    const scenePath = path.join(__dirname, '..', 'templates', 'project-3d', 'Assets', 'Scenes', 'Main.cscene');
    const scene = JSON.parse(fs.readFileSync(scenePath, 'utf8'));
    const entity = name => scene.entities.find(e => e.name === name);
    const transform = name => entity(name).components.find(c => c.type === 'Transform');
    const sun = transform('Directional Light');
    check(defaults.DEFAULT_SUN_ROTATION[0] < 0, 'the default sun looks down rather than up');
    check(nearVec(sun.rotation, defaults.DEFAULT_SUN_ROTATION),
        'the 3D template and the editor create the same sun pose');
    const groundTop = transform('Ground').position[1] + transform('Ground').scale[1] / 2;
    const cube = transform('Cube');
    const cubeBottom = cube.position[1] - cube.scale[1] / 2;
    check(near(groundTop, 0), 'the 3D template ground has its top at y = 0');
    check(near(cubeBottom, groundTop), 'the 3D template cube rests on the ground');

    console.log('editor imaging: integer brush coordinates');
    const red = [1, 0, 0];
    const blank = () => new Uint8Array(16 * 8 * 4);
    const alphaAt = (data, x, y) => data[(y * 16 + x) * 4 + 3];
    const painted = data => {
        let n = 0;
        for (let i = 3; i < data.length; i += 4) if (data[i]) n++;
        return n;
    };

    const dot = blank();
    imaging.stampDisc(dot, 16, 8, 4, 4, 0.5, red, 1, 1);
    check(painted(dot) === 1 && alphaAt(dot, 4, 4) === 255,
        'a 1px brush paints exactly the pixel named by its coordinates');

    const horizontal = blank();
    imaging.strokeLine(horizontal, 16, 8, 0, 4, 7, 4, red, 1, 1);
    let horizontalRows = [];
    for (let y = 0; y < 8; ++y) {
        let n = 0;
        for (let x = 0; x < 16; ++x) if (alphaAt(horizontal, x, y)) n++;
        if (n) horizontalRows.push([y, n]);
    }
    check(JSON.stringify(horizontalRows) === '[[4,8]]',
        'the default-width line paints one row between its endpoints', horizontalRows);

    const diagonal = blank();
    imaging.strokeLine(diagonal, 16, 8, 0, 0, 7, 7, red, 1, 1);
    check(painted(diagonal) === 8 && alphaAt(diagonal, 7, 7) === 255,
        'a 1px diagonal keeps all eight pixels including its endpoint');

    const wide = blank();
    imaging.strokeLine(wide, 16, 8, 0, 4, 7, 4, red, 1, 3);
    const wideRows = [3, 4, 5].every(y => [0, 4, 7].every(x => alphaAt(wide, x, y) === 255));
    check(wideRows && !alphaAt(wide, 4, 2) && !alphaAt(wide, 4, 6),
        'a 3px line covers three whole rows and no more');

    const erased = blank();
    imaging.stampDisc(erased, 16, 8, 4, 4, 2, red, 1, 1);
    const beforeErase = painted(erased);
    imaging.stampEraser(erased, 16, 8, 4, 4, 0.5, 1);
    check(alphaAt(erased, 4, 4) === 0 && painted(erased) === beforeErase - 1,
        'a 1px eraser removes exactly the pixel a 1px brush would paint');

    console.log(failures ? `\n${failures} failure(s)` : '\neditor transform maths: all checks passed');
    process.exit(failures ? 1 : 0);
}

main().catch(err => {
    console.error(err);
    process.exit(1);
});
