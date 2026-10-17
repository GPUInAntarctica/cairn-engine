// Cairn CLI - static engine vocabulary (primitives, components, key codes).
'use strict';

const PRIMITIVES = ['Cube', 'Sphere', 'Plane', 'Quad', 'Cylinder', 'Cone', 'Torus'];

const COMPONENTS = [
    'Camera', 'Light', 'MeshRenderer', 'SpriteRenderer', 'TextRenderer',
    'RigidBody', 'BoxCollider', 'SphereCollider', 'Collider2D',
    'CapsuleCollider', 'MeshCollider', 'CharacterController', 'DistanceJoint',
    'HingeJoint', 'FixedJoint', 'Script',
];

// Mirror of engine/include/cairn/scripting.h (enum class KeyCode).
const KEY_NAMES = [
    'None', 'Space', 'Apostrophe', 'Comma', 'Minus', 'Period', 'Slash', 'Semicolon', 'Equal',
    'LeftBracket', 'Backslash', 'RightBracket', 'Backquote',
    'D0', 'D1', 'D2', 'D3', 'D4', 'D5', 'D6', 'D7', 'D8', 'D9',
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
    'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    'Escape', 'Enter', 'Tab', 'Backspace', 'Insert', 'Delete',
    'Right', 'Left', 'Down', 'Up', 'PageUp', 'PageDown', 'Home', 'End',
    'CapsLock', 'ScrollLock', 'NumLock', 'PrintScreen', 'Pause',
    'F1', 'F2', 'F3', 'F4', 'F5', 'F6', 'F7', 'F8', 'F9', 'F10', 'F11', 'F12',
    'LeftShift', 'LeftControl', 'LeftAlt', 'LeftSuper',
    'RightShift', 'RightControl', 'RightAlt', 'RightSuper',
];

const KEY_CODES = {};
KEY_NAMES.forEach((name, code) => { KEY_CODES[name] = code; });
KEY_CODES.MouseLeft = 400;
KEY_CODES.MouseRight = 401;
KEY_CODES.MouseMiddle = 402;

const MOUSE_BUTTONS = { Left: 0, Middle: 1, Right: 2, 4: 3, 5: 4, 6: 5, 7: 6, 8: 7 };

const MOUSE_BUTTON_INDEX = {};
for (const [name, index] of Object.entries(MOUSE_BUTTONS)) MOUSE_BUTTON_INDEX[name.toLowerCase()] = index;

const SCENE_SETTINGS = ['name', 'gravity', 'ambient', 'background', 'fogEnabled', 'fogColor', 'fogDensity',
    'tonemap', 'exposure', 'shadows', 'shadowResolution', 'shadowDistance',
    'postVignette', 'postVignetteRadius', 'postVignetteSoftness', 'postContrast', 'postSaturation',
    'postBloomStrength', 'postBloomThreshold', 'postFxaa', 'textureMemoryMB',
    'renderBackend', 'msaa'];

const TONEMAPS = { none: 0, reinhard: 1, aces: 2, filmic: 3 };

// Scene setting renderBackend: auto prefers the GPU and falls back to software,
// vulkan forces the GPU, software forces the CPU rasteriser.
const RENDER_BACKENDS = { auto: 0, vulkan: 1, software: 2 };

const LOG_LEVELS = { 0: 'info', 1: 'warning', 2: 'error' };

// Translates a key name, alias or numeric code into the engine's KeyCode int.
function keyCode(value) {
    if (value === undefined || value === null) return undefined;
    const str = String(value).trim();
    if (/^\d+$/.test(str)) return Number(str);
    if (KEY_CODES[str] !== undefined) return KEY_CODES[str];
    const upper = str.toUpperCase();
    if (KEY_CODES[upper] !== undefined) return KEY_CODES[upper];
    const aliases = {
        ' ': 'Space', esc: 'Escape', return: 'Enter', enter: 'Enter', del: 'Delete',
        backspace: 'Backspace', spacebar: 'Space', arrowleft: 'Left', arrowright: 'Right',
        arrowup: 'Up', arrowdown: 'Down', shift: 'LeftShift', ctrl: 'LeftControl',
        control: 'LeftControl', alt: 'LeftAlt', cmd: 'LeftSuper', meta: 'LeftSuper',
        super: 'LeftSuper', lmb: 'MouseLeft', rmb: 'MouseRight', mmb: 'MouseMiddle',
    };
    const alias = aliases[str.toLowerCase()];
    if (alias) return KEY_CODES[alias];
    return undefined;
}

function mouseButton(value) {
    const str = String(value).trim();
    if (/^\d+$/.test(str)) return Number(str);
    if (MOUSE_BUTTON_INDEX[str.toLowerCase()] !== undefined) return MOUSE_BUTTON_INDEX[str.toLowerCase()];
    const alias = { l: 'left', m: 'middle', r: 'right' }[str.toLowerCase()];
    if (alias) return MOUSE_BUTTON_INDEX[alias];
    return undefined;
}

module.exports = {
    PRIMITIVES, COMPONENTS, KEY_NAMES, KEY_CODES, MOUSE_BUTTONS,
    SCENE_SETTINGS, TONEMAPS, RENDER_BACKENDS, LOG_LEVELS, keyCode, mouseButton,
};
