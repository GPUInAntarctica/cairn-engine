// Cairn Engine player - runtime loop for exported games.
// Loads project.cproj, boots the first scene, binds input and runs the frame
// loop against the native engine addon in ./bin.
'use strict';

const fs = require('fs');
const path = require('path');
const { ipcRenderer } = require('electron');

const errorBox = document.getElementById('error');
const canvas = document.getElementById('view');
const ctx = canvas.getContext('2d');

function fatal(message) {
    errorBox.style.display = 'block';
    errorBox.textContent = `Cairn Player\n\n${message}`;
}

try {
    const root = __dirname;
    const projectDoc = JSON.parse(fs.readFileSync(path.join(root, 'project.cproj'), 'utf8'));
    const addon = require(path.join(root, 'bin', 'cairn_engine_addon.node'));

    const engine = new addon.Engine();
    engine.setProjectRoot(root);

    // Start scene: first .cscene in Assets/Scenes (sorted).
    const scenesDir = path.join(root, 'Assets', 'Scenes');
    let sceneFile = '';
    if (fs.existsSync(scenesDir)) {
        const found = fs.readdirSync(scenesDir).filter(f => f.endsWith('.cscene')).sort();
        if (found.length) sceneFile = path.join(scenesDir, found[0]);
    }
    if (sceneFile) {
        const r = engine.loadScene(sceneFile);
        if (!r.ok) fatal(`failed to load scene: ${r.error}`);
    }

    // Compiled gameplay scripts.
    const libDir = path.join(root, 'Library');
    if (fs.existsSync(libDir)) {
        const lib = fs.readdirSync(libDir).find(f => /\.(so|dylib|dll)$/.test(f));
        if (lib) {
            const r = engine.loadScriptLibrary(path.join(libDir, lib));
            if (!r.ok) fatal(`failed to load gameplay scripts`);
        }
    }

    // Fullscreen canvas sizing (cap internal resolution for performance).
    function resize() {
        const dpr = Math.min(window.devicePixelRatio || 1, 2);
        const w = Math.min(1920, Math.floor(innerWidth * dpr));
        const h = Math.min(1080, Math.floor(innerHeight * dpr));
        if (canvas.width !== w || canvas.height !== h) {
            canvas.width = w;
            canvas.height = h;
            engine.setViewportSize(w, h);
        }
    }
    window.addEventListener('resize', resize);
    resize();

    // Input.
    const keyMap = buildKeyMap();
    window.addEventListener('keydown', ev => {
        if (ev.code === 'Escape') window.close();
        const code = keyMap[ev.code];
        if (code !== undefined) { engine.keyDown(code); ev.preventDefault(); }
    });
    window.addEventListener('keyup', ev => {
        const code = keyMap[ev.code];
        if (code !== undefined) { engine.keyUp(code); ev.preventDefault(); }
    });
    window.addEventListener('mousemove', ev => {
        engine.mouseMove(ev.clientX, canvas.height - ev.clientY);
    });
    window.addEventListener('mousedown', ev => engine.mouseDown(ev.button));
    window.addEventListener('mouseup', ev => engine.mouseUp(ev.button));
    window.addEventListener('contextmenu', ev => ev.preventDefault());

    // Audio events from gameplay code (WAV via WebAudio).
    const audioCache = new Map();
    let audioCtx = null;
    function playWav(buffer) {
        audioCtx = audioCtx || new AudioContext();
        audioCtx.decodeAudioData(buffer.slice(0)).then(decoded => {
            const src = audioCtx.createBufferSource();
            src.buffer = decoded;
            src.connect(audioCtx.destination);
            src.start();
        }).catch(() => {});
    }
    function pumpAudio() {
        for (const ev of engine.pollAudio()) {
            const clip = path.join(root, ev.clip);
            if (!audioCache.has(clip)) {
                try { audioCache.set(clip, fs.readFileSync(clip)); } catch { audioCache.set(clip, null); }
            }
            const data = audioCache.get(clip);
            if (data) playWav(data);
        }
    }

    // Frame loop.
    let last = performance.now();
    let imageData = null;
    let frameBuf = null;
    function frame(now) {
        const dt = Math.min(0.1, (now - last) / 1000);
        last = now;
        engine.update(dt);
        engine.renderGameFrame();
        pumpAudio();
        blit();
        engine.endFrameInput();
        requestAnimationFrame(frame);
    }

    function blit() {
        const buf = engine.getFrameBuffer();
        if (!buf) return;
        const w = canvas.width, h = canvas.height;
        try {
            if (!imageData || !frameBuf || frameBuf !== buf.buffer) {
                imageData = new ImageData(new Uint8ClampedArray(buf.buffer, buf.byteOffset, buf.byteLength), w, h);
                frameBuf = buf.buffer;
            }
            ctx.putImageData(imageData, 0, 0);
        } catch {
            const img = ctx.createImageData(w, h);
            img.data.set(buf);
            ctx.putImageData(img, 0, 0);
        }
    }

    requestAnimationFrame(frame);
} catch (err) {
    fatal(err.stack || String(err));
}

// KeyboardEvent.code -> Cairn KeyCode. Values mirror engine/include/cairn/scripting.h.
function buildKeyMap() {
    const names = ['Space', 'Apostrophe', 'Comma', 'Minus', 'Period', 'Slash', 'Semicolon', 'Equal',
        'BracketLeft', 'Backslash', 'BracketRight', 'Backquote',
        'Digit0', 'Digit1', 'Digit2', 'Digit3', 'Digit4', 'Digit5', 'Digit6', 'Digit7', 'Digit8', 'Digit9',
        'KeyA', 'KeyB', 'KeyC', 'KeyD', 'KeyE', 'KeyF', 'KeyG', 'KeyH', 'KeyI', 'KeyJ', 'KeyK', 'KeyL', 'KeyM',
        'KeyN', 'KeyO', 'KeyP', 'KeyQ', 'KeyR', 'KeyS', 'KeyT', 'KeyU', 'KeyV', 'KeyW', 'KeyX', 'KeyY', 'KeyZ',
        'Escape', 'Enter', 'Tab', 'Backspace', 'Insert', 'Delete',
        'ArrowRight', 'ArrowLeft', 'ArrowDown', 'ArrowUp', 'PageUp', 'PageDown', 'Home', 'End',
        'CapsLock', 'ScrollLock', 'NumLock', 'PrintScreen', 'Pause',
        'F1', 'F2', 'F3', 'F4', 'F5', 'F6', 'F7', 'F8', 'F9', 'F10', 'F11', 'F12',
        'ShiftLeft', 'ControlLeft', 'AltLeft', 'MetaLeft',
        'ShiftRight', 'ControlRight', 'AltRight', 'MetaRight'];
    const map = {};
    names.forEach((name, i) => { map[name] = i + 1; });
    map['Mouse0'] = 400; // engine MouseLeft etc. unused here
    return map;
}
