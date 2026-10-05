// Cairn CLI - rendering: viewport control, editor/game frames, picking,
// screenshots and the editor view camera.
'use strict';

const path = require('path');

const { def } = require('../lib/registry');
const { usage, invalidArg, notFound } = require('../lib/errors');

def('render.setViewport', {
    summary: 'Set the render target size used by subsequent frames and screenshots.',
    args: {
        width: { type: 'int', default: 960, desc: 'Width in pixels.' },
        height: { type: 'int', default: 540, desc: 'Height in pixels.' },
    },
    returns: '{width, height}',
    needsEngine: true,
    examples: ['cairn render.setViewport --width 1280 --height 720'],
    run(ctx, args) {
        if (args.width < 1 || args.height < 1 || args.width > 8192 || args.height > 8192) {
            throw invalidArg('viewport size must be between 1 and 8192 pixels');
        }
        ctx.session.width = args.width;
        ctx.session.height = args.height;
        ctx.session.engine().setViewportSize(args.width, args.height);
        return { width: args.width, height: args.height };
    },
});

def('render.editorFrame', {
    summary: 'Render one frame through the editor camera and return draw statistics.',
    args: {},
    returns: '{drawCalls, triangles, meshesSeen, clipRejected, frustumCulled, drawCalls2D, lightCount, shadowedPixels}',
    needsEngine: true,
    examples: ['cairn render.editorFrame'],
    run(ctx) {
        return ctx.session.engine().renderEditorFrame();
    },
});

def('render.gameFrame', {
    summary: 'Render one frame through the scene MainCamera (falls back to the editor view when playing without one).',
    args: {},
    returns: '{drawCalls, triangles, meshesSeen, clipRejected, frustumCulled, drawCalls2D, lightCount, shadowedPixels}',
    needsEngine: true,
    examples: ['cairn render.gameFrame'],
    run(ctx) {
        return ctx.session.engine().renderGameFrame();
    },
});

def('render.screenshot', {
    summary: 'Render a frame and write it to a PNG file.',
    args: {
        out: { type: 'path', required: true, desc: 'Output PNG path.' },
        mode: { type: 'enum', enum: ['editor', 'game'], default: 'editor', desc: 'Which camera to render from.' },
        width: { type: 'int', desc: 'Viewport width override.' },
        height: { type: 'int', desc: 'Viewport height override.' },
    },
    returns: '{path, width, height, stats}',
    needsEngine: true,
    examples: ['cairn --scene Assets/Scenes/Main.cscene render.screenshot --out /tmp/shot.png --width 1280 --height 720'],
    run(ctx, args) {
        const engine = ctx.session.engine();
        const width = args.width || ctx.session.width;
        const height = args.height || ctx.session.height;
        if (args.width || args.height) {
            engine.setViewportSize(width, height);
            ctx.session.width = width;
            ctx.session.height = height;
        }
        const stats = args.mode === 'game' ? engine.renderGameFrame() : engine.renderEditorFrame();
        const file = ctx.session.resolveOutputPath(args.out);
        const ok = engine.saveFramePNG(file);
        if (!ok) throw notFound(`could not write screenshot to ${file}`);
        return { path: file, width, height, stats };
    },
});

def('render.pick', {
    summary: 'Pick the entity under a viewport pixel (origin bottom-left).',
    args: {
        x: { type: 'int', required: true, desc: 'X in pixels.' },
        y: { type: 'int', required: true, desc: 'Y in pixels from the bottom.' },
    },
    returns: '{id, name} (id 0 = nothing hit)',
    needsEngine: true,
    examples: ['cairn render.pick --x 480 --y 270'],
    run(ctx, args) {
        const id = ctx.session.engine().pick(args.x, args.y);
        const entity = id ? ctx.session.engine().getEntityJSON(id) : null;
        return { id, name: entity ? entity.name : null };
    },
});

def('render.worldToScreen', {
    summary: 'Project a world position into viewport pixels.',
    args: {
        x: { type: 'number', required: true, desc: 'World X.' },
        y: { type: 'number', required: true, desc: 'World Y.' },
        z: { type: 'number', required: true, desc: 'World Z.' },
    },
    returns: '{x, y, ok}',
    needsEngine: true,
    examples: ['cairn render.worldToScreen --x 0 --y 1.5 --z 0'],
    run(ctx, args) {
        return ctx.session.engine().worldToScreen(args.x, args.y, args.z);
    },
});

def('render.viewCamera.get', {
    summary: 'Read the editor view camera (position, yaw, pitch, projection).',
    args: {},
    returns: '{position, yaw, pitch, ortho, orthoSize, fov}',
    needsEngine: true,
    examples: ['cairn render.viewCamera.get'],
    run(ctx) {
        return ctx.session.engine().getViewCamera();
    },
});

def('render.viewCamera.set', {
    summary: 'Move the editor view camera (affects editor frames and screenshots).',
    args: {
        position: { type: 'json', desc: '[x,y,z] camera position.' },
        yaw: { type: 'number', desc: 'Yaw in degrees.' },
        pitch: { type: 'number', desc: 'Pitch in degrees.' },
        ortho: { type: 'bool', desc: 'Use an orthographic projection.' },
        orthoSize: { type: 'number', desc: 'Orthographic half-height.' },
        fov: { type: 'number', desc: 'Vertical field of view in degrees.' },
    },
    returns: '{camera}',
    needsEngine: true,
    examples: ['cairn render.viewCamera.set --position \'[0,2,8]\' --yaw 30 --pitch -20'],
    run(ctx, args) {
        const patch = {};
        if (args.position !== undefined) {
            if (!Array.isArray(args.position) || args.position.length !== 3) {
                throw invalidArg('--position expects [x,y,z]');
            }
            patch.position = args.position.map(Number);
        }
        for (const key of ['yaw', 'pitch', 'ortho', 'orthoSize', 'fov']) {
            if (args[key] !== undefined) patch[key] = args[key];
        }
        if (!Object.keys(patch).length) throw usage('render.viewCamera.set needs at least one property');
        ctx.session.engine().setViewCamera(patch);
        return { camera: ctx.session.engine().getViewCamera() };
    },
});

def('render.viewCamera.reset', {
    summary: 'Reset the editor view camera to its default framing.',
    args: {},
    returns: '{camera}',
    needsEngine: true,
    examples: ['cairn render.viewCamera.reset'],
    run(ctx) {
        const engine = ctx.session.engine();
        engine.setViewCamera({ position: [6, 5, 8], yaw: 35, pitch: -30, ortho: false, fov: 60, orthoSize: 5 });
        return { camera: engine.getViewCamera() };
    },
});

def('render.preview', {
    summary: 'Set the viewport, render N editor frames and save the last one (one-shot preview render).',
    args: {
        out: { type: 'path', required: true, desc: 'Output PNG path.' },
        width: { type: 'int', default: 960, desc: 'Width in pixels.' },
        height: { type: 'int', default: 540, desc: 'Height in pixels.' },
        frames: { type: 'int', default: 1, desc: 'Number of frames to render before saving.' },
    },
    returns: '{path, width, height, stats}',
    needsEngine: true,
    examples: ['cairn render.preview --out preview.png --width 1600 --height 900 --frames 3'],
    run(ctx, args) {
        const engine = ctx.session.engine();
        engine.setViewportSize(args.width, args.height);
        ctx.session.width = args.width;
        ctx.session.height = args.height;
        let stats = null;
        for (let i = 0; i < Math.max(1, args.frames); ++i) stats = engine.renderEditorFrame();
        const file = ctx.session.resolveOutputPath(path.resolve(ctx.cwd, args.out));
        if (!engine.saveFramePNG(file)) throw notFound(`could not write preview to ${file}`);
        return { path: file, width: args.width, height: args.height, stats };
    },
});

module.exports = {};
