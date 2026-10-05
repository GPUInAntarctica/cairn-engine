// Cairn CLI - play mode, frame stepping, input injection, logs and audio events.
'use strict';

const { def } = require('../lib/registry');
const constants = require('../lib/constants');
const { invalidArg } = require('../lib/errors');

def('runtime.play', {
    summary: 'Enter play mode (snapshots the scene so runtime.stop can restore it).',
    args: {},
    returns: '{playing:true, paused:false}',
    needsEngine: true,
    examples: ['cairn runtime.play'],
    run(ctx) {
        const engine = ctx.session.engine();
        engine.play();
        return state(engine);
    },
});

def('runtime.pause', {
    summary: 'Pause the simulation without leaving play mode.',
    args: {},
    returns: '{playing, paused}',
    needsEngine: true,
    examples: ['cairn runtime.pause'],
    run(ctx) {
        const engine = ctx.session.engine();
        engine.pause();
        return state(engine);
    },
});

def('runtime.resume', {
    summary: 'Resume a paused simulation.',
    args: {},
    returns: '{playing, paused}',
    needsEngine: true,
    examples: ['cairn runtime.resume'],
    run(ctx) {
        const engine = ctx.session.engine();
        engine.resume();
        return state(engine);
    },
});

def('runtime.step', {
    summary: 'Advance exactly one fixed step (1/60 s) while paused.',
    args: {},
    returns: '{playing, paused}',
    needsEngine: true,
    examples: ['cairn runtime.step'],
    run(ctx) {
        const engine = ctx.session.engine();
        engine.step();
        return state(engine);
    },
});

def('runtime.stop', {
    summary: 'Leave play mode and restore the pre-play scene snapshot.',
    args: {},
    returns: '{playing:false, paused:false}',
    needsEngine: true,
    examples: ['cairn runtime.stop'],
    run(ctx) {
        const engine = ctx.session.engine();
        engine.stop();
        return state(engine);
    },
});

def('runtime.state', {
    summary: 'Report play/pause state and the current play-mode time.',
    args: {},
    returns: '{playing, paused}',
    needsEngine: true,
    examples: ['cairn runtime.state'],
    run(ctx) {
        return state(ctx.session.engine());
    },
});

def('runtime.update', {
    summary: 'Advance the simulation: update -> optional render -> end-of-frame input.',
    args: {
        dt: { type: 'number', default: 1 / 60, desc: 'Delta time in seconds for this step.' },
        frames: { type: 'int', default: 1, desc: 'Number of frames to advance.' },
        render: { type: 'bool', default: false, desc: 'Render a game frame after each update.' },
    },
    returns: '{frames, dt, playing, stats?}',
    needsEngine: true,
    notes: 'Order matters: engine.update(dt) then engine.renderGameFrame() then engine.endFrameInput(). ' +
        'Update itself accumulates fixed 1/60 steps (max 5 per call) for FixedUpdate and physics.',
    examples: ['cairn runtime.update --frames 60', 'cairn runtime.update --frames 300 --render'],
    run(ctx, args) {
        const engine = ctx.session.engine();
        if (args.frames < 0) throw invalidArg('--frames must be >= 0');
        let stats = null;
        for (let i = 0; i < args.frames; ++i) {
            engine.update(args.dt);
            if (args.render) stats = engine.renderGameFrame();
            engine.endFrameInput();
        }
        return Object.assign({ frames: args.frames, dt: args.dt }, state(engine), stats ? { stats } : {});
    },
});

def('runtime.simulate', {
    summary: 'Run the simulation for a duration in seconds (convenience wrapper over runtime.update).',
    args: {
        seconds: { type: 'number', default: 1, desc: 'Simulated seconds.' },
        dt: { type: 'number', default: 1 / 60, desc: 'Fixed delta time per frame.' },
        render: { type: 'bool', default: false, desc: 'Render each frame.' },
        play: { type: 'bool', default: false, desc: 'Enter play mode first (snapshots the scene).' },
    },
    returns: '{frames, seconds, stats?}',
    needsEngine: true,
    examples: ['cairn runtime.simulate --seconds 5 --play --render'],
    run(ctx, args) {
        const engine = ctx.session.engine();
        const frames = Math.max(1, Math.round(args.seconds / args.dt));
        if (args.play) engine.play();
        let stats = null;
        for (let i = 0; i < frames; ++i) {
            engine.update(args.dt);
            if (args.render) stats = engine.renderGameFrame();
            engine.endFrameInput();
        }
        return { frames, seconds: args.seconds, playing: engine.isPlaying(), stats };
    },
});

def('runtime.logs', {
    summary: 'Drain the engine log ring buffer (id, level, text).',
    args: {
        level: { type: 'enum', enum: ['info', 'warning', 'error'], desc: 'Only return logs at or above this level.' },
        text: { type: 'string', desc: 'Only return logs containing this text.' },
    },
    returns: '[{id, level, text}]',
    needsEngine: true,
    notes: 'The engine log buffer is drained by this call: entries are removed once returned.',
    examples: ['cairn runtime.logs --level error'],
    run(ctx, args) {
        let entries = ctx.session.engine().pollLogs();
        const order = { info: 0, warning: 1, error: 2 };
        if (args.level) entries = entries.filter(e => e.level >= order[args.level]);
        if (args.text) entries = entries.filter(e => e.text.includes(args.text));
        return entries.map(e => ({ id: e.id, level: constants.LOG_LEVELS[e.level] || e.level, text: e.text }));
    },
});

def('runtime.logEcho', {
    summary: 'Enable or disable mirroring engine log lines onto stdout (off by default so stdout stays pure JSON).',
    args: { enabled: { type: 'bool', default: true, desc: 'Mirror buffered log entries to stdout.' } },
    returns: '{echo}',
    needsEngine: true,
    examples: ['cairn runtime.logEcho --no-enabled'],
    run(ctx, args) {
        ctx.session.engine().setLogEcho(args.enabled);
        return { echo: args.enabled };
    },
});

def('runtime.audio', {
    summary: 'Drain queued audio events emitted by scripts.',
    args: {},
    returns: '[{clip, volume, pitch}]',
    needsEngine: true,
    examples: ['cairn runtime.audio'],
    run(ctx) {
        return ctx.session.engine().pollAudio();
    },
});

def('input.keyDown', {
    summary: 'Press a key (held until keyUp or end of the next frame).',
    args: {
        key: { type: 'string', required: true, desc: 'Key name (Space, Escape, W, F1...) or numeric KeyCode.' },
    },
    returns: '{code, name}',
    needsEngine: true,
    examples: ['cairn input.keyDown --key Space'],
    run(ctx, args) {
        const code = constants.keyCode(args.key);
        if (code === undefined) throw invalidArg(`unknown key '${args.key}'`, { hint: 'see `cairn engine.capabilities`' });
        ctx.session.engine().keyDown(code);
        return { code, name: constants.KEY_NAMES[code] || args.key };
    },
});

def('input.keyUp', {
    summary: 'Release a key.',
    args: {
        key: { type: 'string', required: true, desc: 'Key name or numeric KeyCode.' },
    },
    returns: '{code, name}',
    needsEngine: true,
    examples: ['cairn input.keyUp --key Space'],
    run(ctx, args) {
        const code = constants.keyCode(args.key);
        if (code === undefined) throw invalidArg(`unknown key '${args.key}'`);
        ctx.session.engine().keyUp(code);
        return { code, name: constants.KEY_NAMES[code] || args.key };
    },
});

def('input.tap', {
    summary: 'Press and release a key, then run one frame so GetKeyDown/GetKeyUp fire.',
    args: {
        key: { type: 'string', required: true, desc: 'Key name or numeric KeyCode.' },
        hold: { type: 'number', default: 1, desc: 'Number of frames to hold the key down.' },
        dt: { type: 'number', default: 1 / 60, desc: 'Delta time per frame.' },
        render: { type: 'bool', default: false, desc: 'Render each held frame.' },
    },
    returns: '{code, name, frames}',
    needsEngine: true,
    examples: ['cairn input.tap --key Space --hold 2'],
    run(ctx, args) {
        const engine = ctx.session.engine();
        const code = constants.keyCode(args.key);
        if (code === undefined) throw invalidArg(`unknown key '${args.key}'`);
        engine.keyDown(code);
        for (let i = 0; i < Math.max(1, args.hold); ++i) {
            engine.update(args.dt);
            if (args.render) engine.renderGameFrame();
        }
        engine.keyUp(code);
        engine.update(args.dt);
        if (args.render) engine.renderGameFrame();
        engine.endFrameInput();
        return { code, name: constants.KEY_NAMES[code] || args.key, frames: Math.max(1, args.hold) + 1 };
    },
});

def('input.mouseDown', {
    summary: 'Press a mouse button (0 = left, 1 = middle, 2 = right).',
    args: { button: { type: 'string', default: 'left', desc: 'Button name or index 0-7.' } },
    returns: '{button}',
    needsEngine: true,
    examples: ['cairn input.mouseDown --button left'],
    run(ctx, args) {
        const button = constants.mouseButton(args.button);
        if (button === undefined) throw invalidArg(`unknown mouse button '${args.button}'`);
        ctx.session.engine().mouseDown(button);
        return { button };
    },
});

def('input.mouseUp', {
    summary: 'Release a mouse button.',
    args: { button: { type: 'string', default: 'left', desc: 'Button name or index 0-7.' } },
    returns: '{button}',
    needsEngine: true,
    examples: ['cairn input.mouseUp --button left'],
    run(ctx, args) {
        const button = constants.mouseButton(args.button);
        if (button === undefined) throw invalidArg(`unknown mouse button '${args.button}'`);
        ctx.session.engine().mouseUp(button);
        return { button };
    },
});

def('input.mouseMove', {
    summary: 'Move the mouse cursor (origin is bottom-left, y grows upward).',
    args: {
        x: { type: 'number', required: true, desc: 'X in pixels.' },
        y: { type: 'number', required: true, desc: 'Y in pixels from the bottom of the viewport.' },
    },
    returns: '{x, y}',
    needsEngine: true,
    examples: ['cairn input.mouseMove --x 320 --y 180'],
    run(ctx, args) {
        ctx.session.engine().mouseMove(args.x, args.y);
        return { x: args.x, y: args.y };
    },
});

def('input.endFrame', {
    summary: 'Close the input frame: wasDown <- down (required so GetKeyDown/GetKeyUp fire once).',
    args: {},
    returns: '{ended:true}',
    needsEngine: true,
    examples: ['cairn input.endFrame'],
    run(ctx) {
        ctx.session.engine().endFrameInput();
        return { ended: true };
    },
});

function state(engine) {
    return { playing: engine.isPlaying(), paused: engine.isPaused() };
}

module.exports = {};
