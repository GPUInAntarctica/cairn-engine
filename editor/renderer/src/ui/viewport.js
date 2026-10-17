// Cairn Engine editor - 3D/2D viewport.
// Owns the scene canvas, editor camera, selection gizmos and play controls.
'use strict';

import { editor, engineInstance, selectEntity, entityList, markDirty, pushUndo } from '../state.js';
import { emit, on } from '../events.js';
import { t } from '../i18n.js';

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

        this.bindPointer();
        this.bindKeyboard();
        on('selection-changed', () => this.drawOverlay());
        on('scene-changed', () => this.drawOverlay());
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
        if (this.sceneCanvas.width !== w || this.sceneCanvas.height !== h) {
            this.sceneCanvas.width = w;
            this.sceneCanvas.height = h;
            this.overlay.width = rect.width;
            this.overlay.height = rect.height;
            engineInstance().setViewportSize(w, h);
            this.drawOverlay();
        }
    }

    // ---- camera -----------------------------------------------------------
    // World-space view direction for yaw/pitch (must mirror the engine's
    // Quat::FromEulerDegrees([pitch, yaw, 0]) * Forward convention).
    camForward() {
        const yaw = this.cam.yaw * Math.PI / 180;
        const pitch = this.cam.pitch * Math.PI / 180;
        const cp = Math.cos(pitch);
        return [-cp * Math.sin(yaw), Math.sin(pitch), -cp * Math.cos(yaw)];
    }

    camPosition() {
        const { distance, pivot } = this.cam;
        const f = this.camForward();
        return [pivot[0] - f[0] * distance, pivot[1] - f[1] * distance, pivot[2] - f[2] * distance];
    }

    camRight() {
        const yaw = this.cam.yaw * Math.PI / 180;
        return [Math.cos(yaw), 0, -Math.sin(yaw)];
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
            if (ev.button === 0) {
                // Gizmo handles first.
                if (this.gizmoHit && this.gizmoHit(pt)) return;
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
            if (this.entityDrag) { this.updateEntityDrag(pt); return; }
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
        const p = ent.transform.position;
        this.cam.pivot = [p[0], p[1], p[2]];
    }

    // ---- entity drag (move/rotate/scale on gizmo axes) ----------------------
    beginEntityDrag(id, pt) {
        const ent = entityList().find(e => e.id === id);
        if (!ent) return;
        pushUndo();
        const pos = ent.transform.position;
        const screen = worldToScreen(pos);
        if (!screen.ok) return;
        this.entityDrag = {
            id,
            mode: editor.tool,
            startPos: [...pos],
            startRot: [...ent.transform.rotation],
            startScale: [...ent.transform.scale],
            startScreen: screen,
            startPointer: pt,
            pivotScreen: screen,
        };
        if (this.entityDrag.mode === 'move') this.setupAxisHandles(ent, pos, screen);
    }

    setupAxisHandles(ent, pos, screen) {
        // Project unit axes to screen space to build draggable axis handles.
        const len = this.gizmoLength();
        const axes = [
            { axis: 0, color: 'var(--axis-x)', dir: [1, 0, 0] },
            { axis: 1, color: 'var(--axis-y)', dir: [0, 1, 0] },
            { axis: 2, color: 'var(--axis-z)', dir: [0, 0, 1] },
        ].map(a => {
            const tip = worldToScreen([pos[0] + a.dir[0] * len, pos[1] + a.dir[1] * len, pos[2] + a.dir[2] * len]);
            return {
                ...a,
                origin: { x: screen.x, y: screen.y },
                tip: tip.ok ? tip : null,
            };
        }).filter(a => a.tip);
        this.axisHandles = axes;
    }

    updateEntityDrag(pt) {
        const eng = engineInstance();
        const drag = this.entityDrag;
        if (drag.mode === 'move') {
            // Move on the camera-facing plane; snap to the dominant axis with Shift.
            const scale = this.cam.ortho
                ? this.cam.orthoSize * 2 / this.box.height
                : this.cam.distance * 0.0018;
            const dx = (pt.x - drag.startPointer.x) * scale;
            const dy = -(pt.y - drag.startPointer.y) * scale;
            const right = this.camRight();
            const up = cameraUp(this.cam.yaw, this.cam.pitch);
            const world = [
                drag.startPos[0] + dx * right[0] + dy * -up[0],
                drag.startPos[1] + dy * up[1],
                drag.startPos[2] + dx * right[2] + dy * -up[2],
            ];
            eng.setComponentProp(drag.id, 'Transform', 'position', world);
        } else if (drag.mode === 'rotate') {
            const delta = (pt.x - drag.startPointer.x) + (pt.y - drag.startPointer.y);
            const rot = [...drag.startRot];
            rot[1] = drag.startRot[1] + delta * 0.5;
            eng.setComponentProp(drag.id, 'Transform', 'rotation', rot);
        } else if (drag.mode === 'scale') {
            const delta = -(pt.y - drag.startPointer.y) * 0.01;
            const s = drag.startScale.map(v => Math.max(0.01, v * (1 + delta)));
            eng.setComponentProp(drag.id, 'Transform', 'scale', s);
        }
        markDirty();
        emit('entity-edited', drag.id);
    }

    endEntityDrag() {
        this.entityDrag = null;
        this.axisHandles = null;
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
        this.axisHandles = null;
        if (tool === 'move' && editor.selection) {
            const ent = entityList().find(e => e.id === editor.selection);
            if (ent) {
                const pos = ent.transform.position;
                const screen = worldToScreen(pos);
                if (screen.ok) this.setupAxisHandles(ent, pos, screen);
            }
        }
        this.drawOverlay();
    }
}

// ---------------------------------------------------------------------------
function clamp(v, a, b) { return v < a ? a : v > b ? b : v; }

function cameraUp(yawDeg, pitchDeg) {
    // World-space up of the editor camera: up = right × forward.
    const yaw = yawDeg * Math.PI / 180, pitch = pitchDeg * Math.PI / 180;
    const right = [Math.cos(yaw), 0, -Math.sin(yaw)];
    const cp = Math.cos(pitch);
    const fwd = [-cp * Math.sin(yaw), Math.sin(pitch), -cp * Math.cos(yaw)];
    const up = [
        right[1] * fwd[2] - right[2] * fwd[1],
        right[2] * fwd[0] - right[0] * fwd[2],
        right[0] * fwd[1] - right[1] * fwd[0],
    ];
    const len = Math.hypot(up[0], up[1], up[2]) || 1;
    return [up[0] / len, up[1] / len, up[2] / len];
}

function worldToScreen(p) {
    return engineInstance().worldToScreen(p[0], p[1], p[2]);
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
