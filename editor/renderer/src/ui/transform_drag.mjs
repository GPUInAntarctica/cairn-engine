// Cairn Engine editor - pure math behind the viewport transform tools.
//
// The viewport owns the pointer and the engine owns the transform; everything
// in this module is a function of numbers so the same rules can be exercised
// without a browser. Values are in CSS pixels because that is what a pointer
// event carries. World units come out of `worldPerPixel`, which uses the
// viewport height in CSS pixels and the camera's distance to the dragged
// object, so a drag follows the cursor at the depth the object sits at.

export function cameraForward(yawDeg, pitchDeg) {
    const yaw = yawDeg * Math.PI / 180;
    const pitch = pitchDeg * Math.PI / 180;
    const cp = Math.cos(pitch);
    return [-cp * Math.sin(yaw), Math.sin(pitch), -cp * Math.cos(yaw)];
}

export function cameraRight(yawDeg) {
    const yaw = yawDeg * Math.PI / 180;
    return [Math.cos(yaw), 0, -Math.sin(yaw)];
}

// World-space up of the editor camera: up = right x forward, normalised.
export function cameraUp(yawDeg, pitchDeg) {
    const right = cameraRight(yawDeg);
    const fwd = cameraForward(yawDeg, pitchDeg);
    const up = [
        right[1] * fwd[2] - right[2] * fwd[1],
        right[2] * fwd[0] - right[0] * fwd[2],
        right[0] * fwd[1] - right[1] * fwd[0],
    ];
    const len = Math.hypot(up[0], up[1], up[2]) || 1;
    return [up[0] / len, up[1] / len, up[2] / len];
}

// World units per CSS pixel on the plane through an object at `depth` from the
// camera. Orthographic projection is depth-independent.
export function worldPerPixel({ ortho = false, orthoSize = 6, depth = 10, fovDeg = 60, viewportHeight = 540 }) {
    if (!(viewportHeight > 0)) return 0;
    if (ortho) return (2 * orthoSize) / viewportHeight;
    const halfFov = (fovDeg * Math.PI / 180) * 0.5;
    return (2 * Math.max(depth, 1e-4) * Math.tan(halfFov)) / viewportHeight;
}

export function snap(value, step) {
    if (!(step > 0)) return value;
    return Math.round(value / step) * step;
}

function snapVector(v, step) {
    return step > 0 ? v.map(x => snap(x, step)) : v;
}

// Drag on the plane facing the camera. `screenDy` is a raw pointer delta and
// grows downwards, so it is negated: dragging up moves along the camera's up
// vector on every axis, not just Y.
export function moveInCameraPlane(start, screenDx, screenDy, view, snapStep = 0) {
    const scale = worldPerPixel(view);
    const right = cameraRight(view.yaw);
    const up = cameraUp(view.yaw, view.pitch);
    const dx = screenDx * scale;
    const dy = -screenDy * scale;
    return snapVector([
        start[0] + right[0] * dx + up[0] * dy,
        start[1] + right[1] * dx + up[1] * dy,
        start[2] + right[2] * dx + up[2] * dy,
    ], snapStep);
}

// Move along one world axis. The screen length of a handle that represents
// `handleWorldLength` world units gives the scale, so the tip stays under the
// pointer when the drag started on the tip.
export function axisMove(start, axisDir, alongPixels, handleScreenLength, handleWorldLength, snapStep = 0) {
    const ratio = handleScreenLength > 0 ? handleWorldLength / handleScreenLength : 0;
    const delta = alongPixels * ratio;
    const out = [
        start[0] + axisDir[0] * delta,
        start[1] + axisDir[1] * delta,
        start[2] + axisDir[2] * delta,
    ];
    // Snapping is absolute, but only the dragged axis is on the grid: the
    // other two coordinates were not part of this drag and must not move.
    if (snapStep > 0) {
        const i = axisDir.findIndex(v => Math.abs(v) > 1e-9);
        if (i >= 0) out[i] = snap(out[i], snapStep);
    }
    return out;
}

// Horizontal drag turns around world Y; vertical drag pitches around local X.
// The two are kept apart: a vertical drag used to add to yaw as well, which
// made the object spin whenever the pointer left the horizontal line. Both
// deltas follow the visible surface: drag right and the near face turns right,
// drag up and it turns up.
export function rotateFromDrag(startEuler, screenDx, screenDy, { sensitivity = 0.5, snapStep = 0 } = {}) {
    let pitch = screenDy * sensitivity;
    let yaw = screenDx * sensitivity;
    if (snapStep > 0) {
        pitch = snap(pitch, snapStep);
        yaw = snap(yaw, snapStep);
    }
    return [startEuler[0] + pitch, startEuler[1] + yaw, startEuler[2]];
}

// Upward drag grows the object. The factor is clamped, not each component, so
// a long downward drag cannot pass through zero and collapse the entity.
export function scaleFromDrag(startScale, screenDy, { sensitivity = 0.01, snapStep = 0 } = {}) {
    let delta = -screenDy * sensitivity;
    if (snapStep > 0) delta = snap(delta, snapStep);
    const factor = Math.max(0.01, 1 + delta);
    return startScale.map(v => v * factor);
}

// Screen-space direction and length of a handle drawn from `origin` to `tip`.
export function handleScreen(origin, tip) {
    const dx = tip.x - origin.x;
    const dy = tip.y - origin.y;
    const length = Math.hypot(dx, dy);
    if (!(length > 0)) return null;
    return { dir: [dx / length, dy / length], length };
}

// Renderer-target pixels to CSS pixels. The overlay canvas is sized in CSS
// pixels while worldToScreen() reports in the renderer target's pixels.
export function deviceToCss(x, y, scaleX, scaleY) {
    return {
        x: scaleX > 0 ? x / scaleX : x,
        y: scaleY > 0 ? y / scaleY : y,
    };
}

export function distanceToSegment(point, a, b) {
    const dx = b.x - a.x;
    const dy = b.y - a.y;
    const len2 = dx * dx + dy * dy;
    if (!(len2 > 0)) return Math.hypot(point.x - a.x, point.y - a.y);
    let t = ((point.x - a.x) * dx + (point.y - a.y) * dy) / len2;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return Math.hypot(point.x - (a.x + t * dx), point.y - (a.y + t * dy));
}

// Index of the handle under `point`, or -1. The first `minAlong` pixels from
// the origin are ignored: all three lines meet there, and a click on the
// object itself must fall through to a camera-plane drag.
export function hitHandle(point, handles, threshold = 8, minAlong = 12) {
    let best = -1;
    let bestDistance = threshold;
    for (let i = 0; i < handles.length; ++i) {
        const h = handles[i];
        if (!h || !h.tip) continue;
        const along = Math.hypot(point.x - h.origin.x, point.y - h.origin.y);
        if (along < minAlong) continue;
        const d = distanceToSegment(point, h.origin, h.tip);
        if (d <= bestDistance) {
            bestDistance = d;
            best = i;
        }
    }
    return best;
}

// Projection of a pointer delta onto a screen direction.
export function alongHandle(screenDx, screenDy, screenDir) {
    return screenDx * screenDir[0] + screenDy * screenDir[1];
}
