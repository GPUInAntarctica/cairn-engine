// Cairn Engine editor - 3D/2D viewport.
// Owns the scene canvas, editor camera, selection gizmos and play controls.
'use strict';

import { editor, engineInstance, selectEntity, entityList, markDirty, pushUndo } from '../state.js';
import { emit, on } from '../events.js';
import { t } from '../i18n.js';
import {
    cameraForward, cameraRight, cameraUp,
    moveInCameraPlane, axisMove, rotateFromDrag, scaleFromDrag,
    handleScreen, hitHandle, alongHandle, deviceToCss,
} from './transform_drag.mjs';

const NAV_SPEED = 8;          // world units per second at speed 1
const ZOOM_SPEED = 1.15;

export class Viewport {
    constructor(pageEl) {
        this.root = document.createElement('div');
        this.root.className = 'viewport-root';
        pageEl.appendChild(this.root);

        this.sceneCanvas = document.createElement('canvas');
        this.sceneCanvas.className = 'scene';
        this.overlay = document.createElement('canvas');
        this.overlay.className = 'overlay';
        this.statsEl = document.createElement('div');
        this.statsEl.className = 'viewport-stats';
        this.focusBadge = document.createElement('div');
        this.focusBadge.className = 'viewport-focus-badge';
        this.root.append(this.sceneCanvas, this.overlay, this.statsEl, this.focusBadge);

        this.cam = { yaw: 40, pitch: -32, distance: 12, pivot: [0, 0.5, 0], ortho: false, orthoSize: 6 };
        this.ctx = this.overlay.getContext('2d');
        this.sceneCtx = this.sceneCanvas.getContext('2d');
        // Box of the viewport in client space. localPoint() runs on every
        // window-level mousemove, and getBoundingClientRect() there forces a
        // synchronous layout. The box only changes on resize / tab switch, so
        // it is cached instead.
        this.box = { left: 0, top: 0, width: 1, height: 1 };
        this.hoverId = 0;
        this.drag = null;
        this.focused = false;
        this.keys = {};
        this.tool = editor.tool;
        this.showStats = true;
        this.axisCamKey = '';

        this.bindPointer();
        this.bindKeyboard();
        on('selection-changed', () => { this.syncAxisHandles(true); this.drawOverlay(); });
        on('scene-changed', () => { this.syncAxisHandles(true); this.drawOverlay(); });
        on('entity-edited', () => this.syncAxisHandles(true));
        window.addEventListener('resize', () => this.resize());
        // The cached box is only exact if something tells us when the layout
        // moved. A ResizeObserver covers splitter drags, tab switches (the
        // scene page goes display:none and reports 0x0) and window resizes
        // without any of them having to remember to poke us.
        if (typeof ResizeObserver !== 'undefined') {
            this.observer = new ResizeObserver(() => this.resize());
            this.observer.observe(this.root);
        }
        this.resize();
    }

    // ---- sizing -----------------------------------------------------------
    resize() {
        const rect = this.root.getBoundingClientRect();
        // A hidden tab page reports a zero-sized box. Resizing to it would
        // shrink the backing store to the 2px floor and leave the viewport
        // blank when the page comes back, so a zero box is ignored: the
        // ResizeObserver fires again with real dimensions on re-show.
        if (rect.width < 1 || rect.height < 1) return;
        this.box.left = rect.left;
        this.box.top = rect.top;
        this.box.width = rect.width;
        this.box.height = rect.height;
        const dpr = Math.min(window.devicePixelRatio || 1, 1.5);
        const w = Math.max(2, Math.min(1920, Math.floor(rect.width * dpr)));
        const h = Math.max(2, Math.min(1200, Math.floor(rect.height * dpr)));
        const ow = Math.max(1, Math.floor(rect.width));
        const oh = Math.max(1, Math.floor(rect.height));
        let changed = false;
        if (this.sceneCanvas.width !== w || this.sceneCanvas.height !== h) {
            this.sceneCanvas.width = w;
            this.sceneCanvas.height = h;
            engineInstance().setViewportSize(w, h);
            changed = true;
        }
        if (this.overlay.width !== ow || this.overlay.height !== oh) {
            this.overlay.width = ow;
            this.overlay.height = oh;
            changed = true;
        }
        if (changed) {
            this.syncAxisHandles(true);
            this.drawOverlay();
        }
    }

    // ---- camera -----------------------------------------------------------
    // World-space view direction for yaw/pitch (must mirror the engine's
    // Quat::FromEulerDegrees([pitch, yaw, 0]) * Forward convention).
    camForward() {
        return cameraForward(this.cam.yaw, this.cam.pitch);
    }

    camPosition() {
        const { distance, pivot } = this.cam;
        const f = this.camForward();
        return [pivot[0] - f[0] * distance, pivot[1] - f[1] * distance, pivot[2] - f[2] * distance];
    }

    camRight() {
        return cameraRight(this.cam.yaw);
    }

    applyCamera() {
        const pos = this.camPosition();
        engineInstance().setViewCamera({
            position: pos,
            yaw: this.cam.yaw,
            pitch: this.cam.pitch,
            ortho: this.cam.ortho,
            orthoSize: this.cam.orthoSize,
            fov: 60,
        });
    }

    fly(dt) {
        const k = this.keys;
        if (!(k.w || k.a || k.s || k.d || k.e || k.q)) return;
        const speed = NAV_SPEED * (k.shift ? 3 : 1) * dt;
        const fwd = this.camForward();
        const right = this.camRight();
        const move = (v, s) => { this.cam.pivot[0] += v[0] * s; this.cam.pivot[1] += v[1] * s; this.cam.pivot[2] += v[2] * s; };
        if (k.w) move(fwd, speed);
        if (k.s) move(fwd, -speed);
        if (k.d) move(right, speed);
        if (k.a) move(right, -speed);
        if (k.e) this.cam.pivot[1] += speed;
        if (k.q) this.cam.pivot[1] -= speed;
    }

    // ---- pointer ----------------------------------------------------------
    localPoint(ev) {
        return { x: ev.clientX - this.box.left, y: ev.clientY - this.box.top };
    }

    pickAt(x, y) {
        const dprX = this.sceneCanvas.width / this.box.width;
        const dprY = this.sceneCanvas.height / this.box.height;
        return engineInstance().pick(x * dprX, y * dprY);
    }

    bindPointer() {
        const el = this.root;
        el.tabIndex = 0;

        el.addEventListener('mousedown', ev => {
            el.focus();
            const pt = this.localPoint(ev);
            if (editor.playing) { this.forwardMouse(ev, true); return; }
            if (this.entityDrag) return;
            if (ev.button === 0) {
                // Gizmo handles first; the lines meet at the origin, and
                // hitHandle ignores that region so a click on the entity still
                // starts a camera-plane drag.
                const handle = this.gizmoHit(pt);
                if (handle) {
                    this.beginEntityDrag(editor.selection, pt, handle);
                    ev.preventDefault();
                    return;
                }
                const hit = this.pickAt(pt.x, pt.y);
                selectEntity(hit);
                if (hit && editor.tool !== 'select') this.beginEntityDrag(hit, pt);
            } else if (ev.button === 1) {
                this.drag = { mode: 'pan', start: pt, pivot: [...this.cam.pivot] };
            } else if (ev.button === 2) {
                this.drag = { mode: 'orbit', start: pt, yaw: this.cam.yaw, pitch: this.cam.pitch };
            }
            ev.preventDefault();
        });

        window.addEventListener('mousemove', ev => {
            const pt = this.localPoint(ev);
            if (editor.playing) { this.forwardMouse(ev, false); return; }
            if (this.entityDrag) { this.updateEntityDrag(pt, ev.shiftKey); return; }
            if (!this.drag) return;
            const dx = pt.x - this.drag.start.x, dy = pt.y - this.drag.start.y;
            if (this.drag.mode === 'orbit') {
                this.cam.yaw = this.drag.yaw + dx * 0.4;
                this.cam.pitch = clamp(this.drag.pitch - dy * 0.4, -89, 89);
            } else if (this.drag.mode === 'pan') {
                const scale = this.cam.ortho ? this.cam.orthoSize * 2 / this.box.height
                                             : this.cam.distance * 0.0018;
                const right = this.camRight();
                const up = cameraUp(this.cam.yaw, this.cam.pitch);
                this.cam.pivot[0] = this.drag.pivot[0] - (dx * right[0] + dy * -up[0]) * scale * 1.2;
                this.cam.pivot[1] = this.drag.pivot[1] + (dy * up[1]) * scale * 1.2;
                this.cam.pivot[2] = this.drag.pivot[2] - (dx * right[2] + dy * -up[2]) * scale * 1.2;
            }
        });

        window.addEventListener('mouseup', ev => {
            if (this.entityDrag) this.endEntityDrag();
            if (editor.playing) this.forwardMouse(ev, false);
            this.drag = null;
        });

        el.addEventListener('wheel', ev => {
            ev.preventDefault();
            if (this.cam.ortho) {
                this.cam.orthoSize = clamp(this.cam.orthoSize * (ev.deltaY > 0 ? ZOOM_SPEED : 1 / ZOOM_SPEED), 0.5, 200);
            } else {
                this.cam.distance = clamp(this.cam.distance * (ev.deltaY > 0 ? ZOOM_SPEED : 1 / ZOOM_SPEED), 0.5, 400);
            }
        }, { passive: false });

        el.addEventListener('contextmenu', ev => ev.preventDefault());
    }

    forwardMouse(ev, down) {
        const eng = engineInstance();
        const pt = this.localPoint(ev);
        const dprY = this.sceneCanvas.height / this.box.height;
        eng.mouseMove(pt.x, this.sceneCanvas.height - pt.y * dprY);
        if (down) eng.mouseDown(ev.button);
        else eng.mouseUp(ev.button);
    }

    bindKeyboard() {
        this.root.addEventListener('keydown', ev => {
            const k = ev.key.toLowerCase();
            this.keys[k] = true;
            if (editor.playing) {
                const code = KEYMAP[ev.code];
                if (code !== undefined) { engineInstance().keyDown(code); ev.preventDefault(); }
                this.focusBadge.style.display = 'block';
                this.focused = true;
            }
        });
        this.root.addEventListener('keyup', ev => {
            this.keys[ev.key.toLowerCase()] = false;
            if (editor.playing) {
                const code = KEYMAP[ev.code];
                if (code !== undefined) { engineInstance().keyUp(code); ev.preventDefault(); }
            }
        });
        window.addEventListener('keyup', ev => { this.keys[ev.key.toLowerCase()] = false; });
        this.root.addEventListener('blur', () => {
            this.keys = {};
            if (editor.playing) {
                for (let i = 1; i < 300; i++) engineInstance().keyUp(i);
                this.focused = false;
                this.focusBadge.style.display = 'none';
            }
        });
    }

    focusSelected() {
        const ent = entityList().find(e => e.id === editor.selection);
        if (!ent) return;
        const p = entityWorldPosition(ent);
        this.cam.pivot = [p[0], p[1], p[2]];
    }

    // ---- entity drag (move/rotate/scale) -----------------------------------
    beginEntityDrag(id, pt, handle = null) {
        const ent = entityList().find(e => e.id === id);
        if (!ent) return;
        const pos = entityWorldPosition(ent);
        const screen = this.projectCss(pos);
        if (!screen.ok && !handle) return;
        const origin = screen.ok ? screen : handle.origin;
        this.entityDrag = {
            id,
            mode: editor.tool,
            startPos: [...pos],
            startRot: [...ent.transform.rotation],
            startScale: [...ent.transform.scale],
            startPointer: pt,
            depth: this.objectDepth(pos),
            // Captured at the start of the drag: a wheel zoom or a camera
            // orbit while the button is down must not change the ratio the
            // pointer is already driving.
            axis: handle ? {
                ...handle,
                origin: { ...handle.origin },
                tip: { ...handle.tip },
                screenDir: [...handle.screenDir],
                handleWorldLength: this.gizmoLength(),
            } : null,
            undoPushed: false,
        };
        if (this.entityDrag.mode === 'move') this.setupAxisHandles(pos, origin);
    }

    // Device pixels per CSS pixel. worldToScreen() reports in the renderer
    // target's pixels, while the overlay canvas is sized in CSS pixels.
    screenScale() {
        return {
            x: this.overlay.width ? this.sceneCanvas.width / this.overlay.width : 1,
            y: this.overlay.height ? this.sceneCanvas.height / this.overlay.height : 1,
        };
    }

    projectCss(p) {
        const s = worldToScreen(p);
        if (!s.ok) return s;
        const k = this.screenScale();
        return { ok: true, ...deviceToCss(s.x, s.y, k.x, k.y) };
    }

    objectDepth(pos) {
        const cam = this.camPosition();
        const f = this.camForward();
        return Math.max(0.05,
            (pos[0] - cam[0]) * f[0] + (pos[1] - cam[1]) * f[1] + (pos[2] - cam[2]) * f[2]);
    }

    setupAxisHandles(pos, screen) {
        // Project unit world axes to CSS space to build draggable handles.
        const len = this.gizmoLength();
        const axes = [
            { axis: 0, color: 'var(--axis-x)', dir: [1, 0, 0] },
            { axis: 1, color: 'var(--axis-y)', dir: [0, 1, 0] },
            { axis: 2, color: 'var(--axis-z)', dir: [0, 0, 1] },
        ].map(a => {
            const tip = this.projectCss([
                pos[0] + a.dir[0] * len,
                pos[1] + a.dir[1] * len,
                pos[2] + a.dir[2] * len,
            ]);
            if (!tip.ok) return null;
            const line = handleScreen(screen, tip);
            // An axis pointing at the camera has almost no screen extent;
            // drawing it would leave a dot that cannot be aimed at.
            if (!line || line.length < 8) return null;
            return {
                ...a,
                origin: { x: screen.x, y: screen.y },
                tip,
                screenDir: line.dir,
                screenLength: line.length,
            };
        }).filter(Boolean);
        this.axisHandles = axes;
    }

    syncAxisHandles(force = false) {
        const key = [
            editor.tool, editor.selection, editor.playing,
            this.cam.yaw, this.cam.pitch, this.cam.distance,
            this.cam.ortho, this.cam.orthoSize,
            this.cam.pivot[0], this.cam.pivot[1], this.cam.pivot[2],
            this.sceneCanvas.width, this.sceneCanvas.height,
            this.overlay.width, this.overlay.height,
        ].join('|');
        if (!force && key === this.axisCamKey) return;
        this.axisCamKey = key;
        if (editor.tool !== 'move' || editor.playing || !editor.selection) {
            this.axisHandles = null;
            return;
        }
        const ent = entityList().find(e => e.id === editor.selection);
        if (!ent) { this.axisHandles = null; return; }
        const pos = entityWorldPosition(ent);
        const screen = this.projectCss(pos);
        if (!screen.ok) { this.axisHandles = null; return; }
        this.setupAxisHandles(pos, screen);
    }

    gizmoHit(pt) {
        if (editor.tool !== 'move' || editor.playing || !editor.selection) return null;
        if (!this.axisHandles || !this.axisHandles.length) return null;
        const i = hitHandle(pt, this.axisHandles, 9, 14);
        return i >= 0 ? this.axisHandles[i] : null;
    }

    updateEntityDrag(pt, shift = false) {
        const eng = engineInstance();
        const drag = this.entityDrag;
        const dx = pt.x - drag.startPointer.x;
        const dy = pt.y - drag.startPointer.y;
        const snapStep = shift
            ? (drag.mode === 'move' ? 0.25 : drag.mode === 'rotate' ? 15 : 0.1)
            : 0;

        let next;
        let start;
        let prop;
        if (drag.mode === 'move') {
            prop = 'position';
            start = drag.startPos;
            if (drag.axis) {
                next = axisMove(
                    drag.startPos, drag.axis.dir,
                    alongHandle(dx, dy, drag.axis.screenDir),
                    drag.axis.screenLength, drag.axis.handleWorldLength,
                    snapStep);
            } else {
                next = moveInCameraPlane(drag.startPos, dx, dy, {
                    yaw: this.cam.yaw,
                    pitch: this.cam.pitch,
                    ortho: this.cam.ortho,
                    orthoSize: this.cam.orthoSize,
                    depth: drag.depth,
                    fovDeg: 60,
                    viewportHeight: this.box.height,
                }, snapStep);
            }
        } else if (drag.mode === 'rotate') {
            prop = 'rotation';
            start = drag.startRot;
            next = rotateFromDrag(drag.startRot, dx, dy, {
                sensitivity: 0.5,
                snapStep: snapStep,
            });
        } else if (drag.mode === 'scale') {
            prop = 'scale';
            start = drag.startScale;
            next = scaleFromDrag(drag.startScale, dy, {
                sensitivity: 0.01,
                snapStep: snapStep,
            });
        } else {
            return;
        }

        // A click that never moves must not leave an undo entry behind; the
        // snapshot is taken on the first frame that actually changes the
        // transform. Returning to the start after a drag still applies, since
        // the snapshot already exists.
        if (!drag.undoPushed) {
            if (sameVector(next, start)) return;
            pushUndo();
            drag.undoPushed = true;
        }
        // The drag computed a world-space target; a parented entity stores a
        // parent-relative position, so the engine converts it back.
        if (drag.mode === 'move') eng.setWorldPosition(drag.id, next[0], next[1], next[2]);
        else eng.setComponentProp(drag.id, 'Transform', prop, next);
        if (drag.mode === 'move') this.syncAxisHandles(true);
        markDirty();
        emit('entity-edited', drag.id);
    }

    endEntityDrag() {
        this.entityDrag = null;
        this.syncAxisHandles();
        this.drawOverlay();
    }

    gizmoLength() {
        return this.cam.ortho ? this.cam.orthoSize * 0.28 : this.cam.distance * 0.16;
    }

    // ---- per-frame ----------------------------------------------------------
    frame(dt) {
        if (!editor.playing) {
            this.fly(dt);
            this.applyCamera();
            this.lastRenderStats = engineInstance().renderEditorFrame();
        } else {
            this.lastRenderStats = engineInstance().renderGameFrame();
        }
        this.blit();
        this.drawOverlay();
    }

    blit() {
        const eng = engineInstance();
        const buf = eng.getFrameBuffer();
        if (!buf) return;
        const w = this.sceneCanvas.width, h = this.sceneCanvas.height;
        const ctx = this.sceneCtx;
        try {
            if (!this.imageData || this.imageData.width !== w || this.imageData.height !== h ||
                this.frameBufKey !== buf.buffer) {
                this.imageData = new ImageData(new Uint8ClampedArray(buf.buffer, buf.byteOffset, buf.byteLength), w, h);
                this.frameBufKey = buf.buffer;
            }
            ctx.putImageData(this.imageData, 0, 0);
        } catch {
            const img = ctx.createImageData(w, h);
            img.data.set(buf);
            ctx.putImageData(img, 0, 0);
        }
    }

    drawOverlay() {
        const ctx = this.ctx;
        const w = this.overlay.width, h = this.overlay.height;
        ctx.clearRect(0, 0, w, h);
        // Mouse orbit, pan and zoom have no event of their own that says
        // "the projection moved"; the camera key notices and re-projects.
        this.syncAxisHandles();
        const wantStats = this.showStats ? 'block' : 'none';
        if (wantStats !== this.statsShown) {
            this.statsEl.style.display = wantStats;
            this.statsShown = wantStats;
        }

        // Axis handles for the move tool.
        if (editor.selection && editor.tool === 'move' && !editor.playing && this.axisHandles) {
            for (const a of this.axisHandles) {
                if (!a.tip) continue;
                ctx.strokeStyle = axisColor(a.axis);
                ctx.lineWidth = 2;
                ctx.beginPath();
                ctx.moveTo(a.origin.x, a.origin.y);
                ctx.lineTo(a.tip.x, a.tip.y);
                ctx.stroke();
                ctx.beginPath();
                ctx.arc(a.tip.x, a.tip.y, 3.5, 0, Math.PI * 2);
                ctx.fillStyle = axisColor(a.axis);
                ctx.fill();
            }
        }

        // Stats readout (updated by app.js each frame).
    }

    updateStats(stats, fps, profile) {
        if (!this.showStats) return;
        let html = `${this.sceneCanvas.width}×${this.sceneCanvas.height} &nbsp; ${fps.toFixed(0)} fps` +
            `<br>${t('viewport.draw')} ${stats.drawCalls} · ${t('viewport.tris')} ${stats.triangles}`;
        if (profile && Array.isArray(profile.scopes)) {
            const parts = profile.scopes
                .filter(s => s.avgMs > 0.02)
                .slice(0, 4)
                .map(s => `${s.name} ${s.avgMs.toFixed(2)}`);
            if (parts.length) html += `<br>${t('viewport.profile')} ${parts.join(' · ')} ms`;
        }
        if (html !== this.statsHtml) {
            this.statsEl.innerHTML = html;
            this.statsHtml = html;
        }
    }

    setTool(tool) {
        this.tool = tool;
        this.syncAxisHandles();
        this.drawOverlay();
    }
}

// ---------------------------------------------------------------------------
function clamp(v, a, b) { return v < a ? a : v > b ? b : v; }

function sameVector(a, b) {
    return Math.abs(a[0] - b[0]) < 1e-6 &&
        Math.abs(a[1] - b[1]) < 1e-6 &&
        Math.abs(a[2] - b[2]) < 1e-6;
}

function worldToScreen(p) {
    return engineInstance().worldToScreen(p[0], p[1], p[2]);
}

// Transform.position is local; the gizmo, the focus pivot and a move drag all
// work in world space. The engine resolves the hierarchy so the editor does not
// carry a second copy of the parent composition rules.
function entityWorldPosition(ent) {
    const p = engineInstance().getWorldPosition(ent.id);
    return Array.isArray(p) ? [p[0], p[1], p[2]] : [...ent.transform.position];
}

// Axis colours live in theme.css as --ax-x/y/z, alongside the tick colours in
// the inspector. Resolved once from the stylesheet so the two cannot drift.
const AXIS_VARS = ['--ax-x', '--ax-y', '--ax-z'];
let axisPalette = null;
function axisColor(axis) {
    if (!axisPalette) {
        const cs = getComputedStyle(document.documentElement);
        axisPalette = AXIS_VARS.map(v => cs.getPropertyValue(v).trim() || '#888');
    }
    return axisPalette[axis];
}

// KeyboardEvent.code -> Cairn KeyCode (mirrors engine/include/cairn/scripting.h).
export const KEYMAP = (() => {
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
    return map;
})();
