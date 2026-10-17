// Cairn Engine editor - headless raster imaging (main process).
//
// DOM/canvas-free image core shared by the image editor and the CLI: layered
// RGBA8 documents, brush/shape/fill tools, layer compositing and a
// height-to-normal-map generator. PNG and BMP codecs are implemented here so
// texture generation needs no third-party dependency.
'use strict';

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

// --------------------------------------------------------------------- color
function clamp(v, a, b) { return v < a ? a : v > b ? b : v; }

function hexToRgba(hex) {
    const m = /^#?([0-9a-fA-F]{6})$/.exec(String(hex).trim());
    if (!m) return null;
    const v = parseInt(m[1], 16);
    return [(v >> 16 & 255) / 255, (v >> 8 & 255) / 255, (v & 255) / 255, 1];
}

// CSS basic keywords plus the greys agents ask for most often.
const NAMED_COLORS = {
    black: '#000000', silver: '#c0c0c0', gray: '#808080', grey: '#808080', white: '#ffffff',
    maroon: '#800000', red: '#ff0000', purple: '#800080', fuchsia: '#ff00ff', magenta: '#ff00ff',
    green: '#008000', lime: '#00ff00', olive: '#808000', yellow: '#ffff00', navy: '#000080',
    blue: '#0000ff', teal: '#008080', aqua: '#00ffff', cyan: '#00ffff', orange: '#ffa500',
    darkgray: '#a9a9a9', darkgrey: '#a9a9a9', lightgray: '#d3d3d3', lightgrey: '#d3d3d3',
    dimgray: '#696969', dimgrey: '#696969', whitesmoke: '#f5f5f5', gold: '#ffd700',
    pink: '#ffc0cb', brown: '#a52a2a', transparent: '#00000000',
};

function namedColor(name) {
    const key = String(name).trim().toLowerCase();
    if (!Object.prototype.hasOwnProperty.call(NAMED_COLORS, key)) return null;
    const hex = NAMED_COLORS[key];
    if (hex.length === 9) {
        const v = parseInt(hex.slice(1), 16);
        return [(v >>> 24 & 255) / 255, (v >>> 16 & 255) / 255, (v >>> 8 & 255) / 255, (v & 255) / 255];
    }
    return hexToRgba(hex);
}

// Accepts "#rrggbb", "#rrggbbaa", "r,g,b[,a]", [r,g,b[,a]] (0-1 or 0-255).
function parseColor(value) {
    if (value === undefined || value === null) return null;
    if (Array.isArray(value)) {
        const comps = value.slice(0, 4).map(Number);
        if (comps.length < 3 || comps.some(Number.isNaN)) return null;
        const max = Math.max(...comps.slice(0, 3));
        const scale = max > 1.0001 ? 1 / 255 : 1;
        const alpha = comps.length > 3 ? (comps[3] > 1.0001 ? comps[3] / 255 : comps[3]) : 1;
        return comps.slice(0, 3).map(v => clamp(v * scale, 0, 1)).concat([clamp(alpha, 0, 1)]);
    }
    if (typeof value === 'string') {
        const raw = value.trim();
        if (raw.startsWith('[')) {
            try { return parseColor(JSON.parse(raw)); } catch { return null; }
        }
        if (!raw.startsWith('#') && raw.includes(',')) {
            const parts = raw.split(',').map(s => parseFloat(s.trim()));
            if (parts.some(Number.isNaN)) return null;
            return parseColor(parts);
        }
    }
    const s = String(value).trim();
    if (/^#?[0-9a-fA-F]{6}$/.test(s)) return hexToRgba(s);
    const m8 = /^#?([0-9a-fA-F]{8})$/.exec(s);
    if (m8) {
        const v = parseInt(m8[1], 16);
        return [(v >>> 24 & 255) / 255, (v >>> 16 & 255) / 255, (v >>> 8 & 255) / 255, (v & 255) / 255];
    }
    return namedColor(s);
}

function rgbaToHex(rgba) {
    const [r, g, b] = rgba.map(v => Math.round(clamp(v, 0, 1) * 255));
    return '#' + [r, g, b].map(v => v.toString(16).padStart(2, '0')).join('');
}

function rgbToHsv(r, g, b) {
    r /= 255; g /= 255; b /= 255;
    const max = Math.max(r, g, b), min = Math.min(r, g, b), d = max - min;
    let h = 0;
    if (d > 0) {
        if (max === r) h = ((g - b) / d) % 6;
        else if (max === g) h = (b - r) / d + 2;
        else h = (r - g) / d + 4;
        h /= 6;
        if (h < 0) h += 1;
    }
    return { h, s: max === 0 ? 0 : d / max, v: max };
}

function hsvToRgb(h, s, v) {
    const c = v * s, x = c * (1 - Math.abs((h * 6) % 2 - 1)), m = v - c;
    let r = 0, g = 0, b = 0;
    if (h < 1 / 6) { r = c; g = x; }
    else if (h < 2 / 6) { r = x; g = c; }
    else if (h < 3 / 6) { g = c; b = x; }
    else if (h < 4 / 6) { g = x; b = c; }
    else if (h < 5 / 6) { r = x; b = c; }
    else { r = c; b = x; }
    return [r + m, g + m, b + m];
}

// ------------------------------------------------------------------ documents
// doc = { width, height, layers: [{ name, visible, opacity, data: Buffer(RGBA) }] }

function newLayer(width, height, rgba) {
    const data = Buffer.alloc(width * height * 4);
    if (rgba) fillRect(data, width, height, 0, 0, width, height, rgba, 1);
    return { name: 'Layer', visible: true, opacity: 1, data };
}

function createDoc(width, height, options = {}) {
    const bg = options.background === undefined ? [1, 1, 1, 1] : options.background;
    const doc = { width, height, layers: [] };
    if (options.background !== null) {
        doc.layers.push(Object.assign(newLayer(width, height, bg), { name: 'Background' }));
    }
    return doc;
}

function composite(doc) {
    const out = Buffer.alloc(doc.width * doc.height * 4);
    for (const l of doc.layers) {
        if (!l.visible || l.opacity <= 0) continue;
        const a = clamp(l.opacity, 0, 1);
        for (let p = 0; p < doc.width * doc.height; ++p) {
            const o = p * 4;
            const sa = (l.data[o + 3] / 255) * a;
            if (sa <= 0) continue;
            const da = out[o + 3] / 255;
            const oa = sa + da * (1 - sa);
            if (oa <= 0) continue;
            for (let c = 0; c < 3; ++c) {
                const sc = (l.data[o + c] / 255) * sa;
                const dc = (out[o + c] / 255) * da * (1 - sa);
                out[o + c] = Math.round(clamp((sc + dc) / oa, 0, 1) * 255);
            }
            out[o + 3] = Math.round(clamp(oa, 0, 1) * 255);
        }
    }
    return out;
}

function resizeDoc(doc, width, height) {
    for (const l of doc.layers) l.data = resizeRgba(l.data, doc.width, doc.height, width, height);
    doc.width = width;
    doc.height = height;
    return doc;
}

function resizeRgba(data, w, h, nw, nh) {
    const out = Buffer.alloc(nw * nh * 4);
    for (let y = 0; y < nh; ++y) {
        const sy = Math.min(h - 1, Math.floor(y * h / nh));
        for (let x = 0; x < nw; ++x) {
            const sx = Math.min(w - 1, Math.floor(x * w / nw));
            const s = (sy * w + sx) * 4, d = (y * nw + x) * 4;
            out[d] = data[s]; out[d + 1] = data[s + 1]; out[d + 2] = data[s + 2]; out[d + 3] = data[s + 3];
        }
    }
    return out;
}

// -------------------------------------------------------------------- drawing
// All primitives operate on a raw RGBA buffer and clip to bounds.

// Straight-alpha source-over onto one layer. The colour is weighted by the
// destination's *own* alpha, so a 50% highlight painted onto a transparent layer
// stores white at 50% alpha - painting it as `src * a` against a transparent
// black instead stored grey at 50% alpha, which `composite()` then darkened a
// second time (a 50% white fill composited to 25%, and the same highlight looked
// duller on its own layer than drawn directly on the base). Over an opaque
// destination this is exactly the old `src * a + dst * (1 - a)`, and the alpha
// still uses max() rather than source-over so a repeated stroke does not build up
// coverage - that is what keeps a mirrored draw pixel-identical to its
// reflection.
function blendPixel(data, w, h, x, y, rgba, alpha) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    const a = clamp(alpha, 0, 1);
    if (a <= 0) return;
    const o = (y * w + x) * 4;
    const da = data[o + 3] / 255;
    const oa = Math.max(da, a);
    if (oa <= 0) return;
    for (let c = 0; c < 3; ++c) {
        const sc = rgba[c] * a;
        const dc = (data[o + c] / 255) * da * (1 - a);
        data[o + c] = Math.round(clamp((sc + dc) / oa, 0, 1) * 255);
    }
    data[o + 3] = Math.round(oa * 255);
}

function erasePixel(data, w, h, x, y, alpha) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    const a = clamp(alpha, 0, 1);
    if (a <= 0) return;
    const o = (y * w + x) * 4;
    data[o + 3] = Math.round(data[o + 3] * (1 - a));
}

function fillRect(data, w, h, x0, y0, rw, rh, rgba, alpha = 1) {
    const x1 = Math.round(x0), y1 = Math.round(y0);
    const x2 = Math.round(x0 + rw), y2 = Math.round(y0 + rh);
    for (let y = Math.max(0, y1); y < Math.min(h, y2); ++y) {
        for (let x = Math.max(0, x1); x < Math.min(w, x2); ++x) {
            blendPixel(data, w, h, x, y, rgba, alpha);
        }
    }
}

function strokeLine(data, w, h, ax, ay, bx, by, rgba, alpha = 1, width = 1) {
    const dx = bx - ax, dy = by - ay;
    const dist = Math.hypot(dx, dy);
    const steps = Math.max(1, Math.ceil(dist / Math.max(0.25, width * 0.25)));
    for (let i = 0; i <= steps; ++i) {
        const t = i / steps;
        stampDisc(data, w, h, ax + dx * t, ay + dy * t, width / 2, rgba, alpha, 1);
    }
}

function stampDisc(data, w, h, cx, cy, radius, rgba, alpha, hardness) {
    const r = Math.max(0.5, radius);
    const inner = r * clamp(hardness === undefined ? 1 : hardness, 0, 1) * 0.9;
    const x0 = Math.max(0, Math.floor(cx - r)), x1 = Math.min(w - 1, Math.ceil(cx + r));
    const y0 = Math.max(0, Math.floor(cy - r)), y1 = Math.min(h - 1, Math.ceil(cy + r));
    for (let y = y0; y <= y1; ++y) {
        for (let x = x0; x <= x1; ++x) {
            const d = Math.hypot(x + 0.5 - cx, y + 0.5 - cy);
            if (d > r) continue;
            const falloff = inner >= r ? 1 : 1 - (d - inner) / (r - inner);
            blendPixel(data, w, h, x, y, rgba, alpha * clamp(falloff, 0, 1));
        }
    }
}

function stampEraser(data, w, h, cx, cy, radius, alpha) {
    const r = Math.max(0.5, radius);
    const x0 = Math.max(0, Math.floor(cx - r)), x1 = Math.min(w - 1, Math.ceil(cx + r));
    const y0 = Math.max(0, Math.floor(cy - r)), y1 = Math.min(h - 1, Math.ceil(cy + r));
    for (let y = y0; y <= y1; ++y) {
        for (let x = x0; x <= x1; ++x) {
            if (Math.hypot(x + 0.5 - cx, y + 0.5 - cy) > r) continue;
            erasePixel(data, w, h, x, y, alpha);
        }
    }
}

function strokeRect(data, w, h, x0, y0, x1, y1, rgba, alpha = 1, width = 1) {
    const ax = Math.min(x0, x1), ay = Math.min(y0, y1);
    const bx = Math.max(x0, x1), by = Math.max(y0, y1);
    strokeLine(data, w, h, ax, ay, bx, ay, rgba, alpha, width);
    strokeLine(data, w, h, bx, ay, bx, by, rgba, alpha, width);
    strokeLine(data, w, h, bx, by, ax, by, rgba, alpha, width);
    strokeLine(data, w, h, ax, by, ax, ay, rgba, alpha, width);
}

function ellipsePoints(cx, cy, rx, ry) {
    const out = [];
    const steps = Math.max(16, Math.ceil((Math.abs(rx) + Math.abs(ry)) * 2));
    for (let i = 0; i < steps; ++i) {
        const t = i / steps * Math.PI * 2;
        out.push([cx + Math.cos(t) * rx, cy + Math.sin(t) * ry]);
    }
    return out;
}

function strokeEllipse(data, w, h, cx, cy, rx, ry, rgba, alpha = 1, width = 1) {
    const pts = ellipsePoints(cx, cy, Math.abs(rx), Math.abs(ry));
    for (let i = 0; i < pts.length; ++i) {
        const a = pts[i], b = pts[(i + 1) % pts.length];
        strokeLine(data, w, h, a[0], a[1], b[0], b[1], rgba, alpha, width);
    }
}

function fillEllipse(data, w, h, cx, cy, rx, ry, rgba, alpha = 1) {
    rx = Math.abs(rx); ry = Math.abs(ry);
    const x0 = Math.max(0, Math.floor(cx - rx)), x1 = Math.min(w - 1, Math.ceil(cx + rx));
    const y0 = Math.max(0, Math.floor(cy - ry)), y1 = Math.min(h - 1, Math.ceil(cy + ry));
    for (let y = y0; y <= y1; ++y) {
        for (let x = x0; x <= x1; ++x) {
            const dx = (x + 0.5 - cx) / (rx || 1), dy = (y + 0.5 - cy) / (ry || 1);
            if (dx * dx + dy * dy <= 1) blendPixel(data, w, h, x, y, rgba, alpha);
        }
    }
}

// Scanline flood fill with a tolerance, matching the editor's tool.
function floodFill(data, w, h, sx, sy, rgba, opacity = 1, tolerance = 24) {
    sx = clamp(Math.floor(sx), 0, w - 1);
    sy = clamp(Math.floor(sy), 0, h - 1);
    const start = (sy * w + sx) * 4;
    const target = [data[start], data[start + 1], data[start + 2], data[start + 3]];
    const match = o => Math.abs(data[o] - target[0]) <= tolerance
        && Math.abs(data[o + 1] - target[1]) <= tolerance
        && Math.abs(data[o + 2] - target[2]) <= tolerance
        && Math.abs(data[o + 3] - target[3]) <= tolerance;
    const seen = new Uint8Array(w * h);
    const stack = [sx, sy];
    while (stack.length) {
        const y = stack.pop(), x = stack.pop();
        if (x < 0 || y < 0 || x >= w || y >= h) continue;
        const p = y * w + x;
        if (seen[p]) continue;
        seen[p] = 1;
        const o = p * 4;
        if (!match(o)) continue;
        blendPixel(data, w, h, x, y, rgba, opacity);
        stack.push(x + 1, y, x - 1, y, x, y + 1, x, y - 1);
    }
}

function samplePixel(data, w, h, x, y) {
    x = clamp(Math.floor(x), 0, w - 1);
    y = clamp(Math.floor(y), 0, h - 1);
    const o = (y * w + x) * 4;
    return { x, y, r: data[o], g: data[o + 1], b: data[o + 2], a: data[o + 3], hex: rgbaToHex([data[o] / 255, data[o + 1] / 255, data[o + 2] / 255]) };
}

// ------------------------------------------------------------------ polygons
// Vertex parsing and the scanline machinery the path and outline tools share.

// Accepts "x,y x,y …", "x,y;x,y", "x y x y", [[x,y],…] or a flat [x,y,x,y].
function parsePoints(value) {
    if (value === undefined || value === null) return null;
    let raw = value;
    if (typeof raw === 'string') {
        const text = raw.trim();
        if (!text) return null;
        if (/^[[{]/.test(text)) {
            try { raw = JSON.parse(text); } catch { return null; }
        } else {
            const tokens = text.split(/[;\s]+/).filter(Boolean);
            raw = tokens.some(t => t.includes(','))
                ? tokens.map(t => t.split(',').map(Number))
                : tokens.map(Number);
        }
    }
    if (!Array.isArray(raw) || raw.length < 2) return null;
    if (typeof raw[0] === 'number') {
        if (raw.length % 2 !== 0) return null;
        const flat = [];
        for (let i = 0; i < raw.length; i += 2) flat.push([Number(raw[i]), Number(raw[i + 1])]);
        raw = flat;
    }
    const pts = [];
    for (const p of raw) {
        if (!Array.isArray(p) || p.length < 2) return null;
        const x = Number(p[0]), y = Number(p[1]);
        if (!Number.isFinite(x) || !Number.isFinite(y)) return null;
        pts.push([x, y]);
    }
    return pts.length >= 2 ? pts : null;
}

function polygonSignedArea(pts) {
    let area = 0;
    for (let i = 0; i < pts.length; ++i) {
        const a = pts[i], b = pts[(i + 1) % pts.length];
        area += a[0] * b[1] - b[0] * a[1];
    }
    return area / 2;
}

// Per-scanline fill intervals of a closed polygon using the non-zero winding
// rule, so self-intersecting shapes (a star drawn as a single outline) fill
// solid instead of leaving a hole. Each entry is a sorted list of disjoint
// [x0, x1) column intervals, or null for an empty row.
function polygonSpans(pts, w, h) {
    const spans = new Array(h).fill(null);
    if (pts.length < 3) return spans;
    let minY = Infinity, maxY = -Infinity;
    for (const p of pts) { if (p[1] < minY) minY = p[1]; if (p[1] > maxY) maxY = p[1]; }
    const y0 = Math.max(0, Math.floor(minY)), y1 = Math.min(h - 1, Math.ceil(maxY));
    for (let y = y0; y <= y1; ++y) {
        const cy = y + 0.5;
        const crossings = [];
        for (let i = 0; i < pts.length; ++i) {
            const a = pts[i], b = pts[(i + 1) % pts.length];
            if (a[1] === b[1]) continue;
            const t = (cy - a[1]) / (b[1] - a[1]);
            if (t < 0 || t >= 1) continue;
            crossings.push([a[0] + (b[0] - a[0]) * t, a[1] < b[1] ? 1 : -1]);
        }
        if (!crossings.length) continue;
        crossings.sort((p, q) => p[0] - q[0] || q[1] - p[1]);
        const row = [];
        let wind = 0, startX = 0;
        for (const [x, dir] of crossings) {
            if (wind === 0) startX = x;
            wind += dir;
            if (wind === 0) {
                const xs = Math.round(startX), xe = Math.round(x);
                if (xe > xs) row.push([xs, xe]);
            }
        }
        if (row.length) spans[y] = row;
    }
    return spans;
}

function fillRowRange(data, w, h, y, x0, x1, rgba, alpha) {
    const a = Math.max(0, Math.round(x0)), b = Math.min(w, Math.round(x1));
    for (let x = a; x < b; ++x) blendPixel(data, w, h, x, y, rgba, alpha);
}

function fillSpans(data, w, h, spans, rgba, alpha = 1) {
    for (let y = 0; y < h; ++y) {
        const row = spans[y];
        if (!row) continue;
        for (const [x0, x1] of row) fillRowRange(data, w, h, y, x0, x1, rgba, alpha);
    }
}

function fillPolygon(data, w, h, pts, rgba, alpha = 1) {
    fillSpans(data, w, h, polygonSpans(pts, w, h), rgba, alpha);
}

// Blends the scanline region `outer` minus `inner` -- the basis of the
// outside-the-boundary outlines.
function drawSpanRing(data, w, h, outer, inner, rgba, alpha = 1) {
    for (let y = 0; y < h; ++y) {
        const row = outer[y];
        if (!row) continue;
        const holes = inner[y] || [];
        for (const [x0, x1] of row) {
            let cur = x0;
            for (const [i0, i1] of holes) {
                if (i1 <= cur) continue;
                if (i0 >= x1) break;
                if (i0 > cur) fillRowRange(data, w, h, y, cur, i0, rgba, alpha);
                cur = Math.max(cur, i1);
                if (cur >= x1) break;
            }
            if (cur < x1) fillRowRange(data, w, h, y, cur, x1, rgba, alpha);
        }
    }
}

function edgeNormal(a, b, winding) {
    const dx = b[0] - a[0], dy = b[1] - a[1];
    const len = Math.hypot(dx, dy);
    if (len < 1e-9) return null;
    return [winding * dy / len, -winding * dx / len];
}

// Pushes every vertex out along its miter so the polygon grows by `dist`
// uniformly (so a square corner lands `dist` diagonally out).
function offsetPolygon(pts, dist) {
    const n = pts.length;
    if (n < 3) return null;
    const winding = polygonSignedArea(pts) >= 0 ? 1 : -1;
    const out = [];
    for (let i = 0; i < n; ++i) {
        const prev = pts[(i - 1 + n) % n], cur = pts[i], next = pts[(i + 1) % n];
        const n1 = edgeNormal(prev, cur, winding);
        const n2 = edgeNormal(cur, next, winding);
        if (!n1 || !n2) return null;
        let mx = n1[0] + n2[0], my = n1[1] + n2[1];
        const len = Math.hypot(mx, my);
        if (len < 1e-9) return null;
        mx /= len; my /= len;
        const denom = mx * n1[0] + my * n1[1];
        if (Math.abs(denom) < 1e-6) return null;
        const limit = dist * 4;
        const scale = clamp(dist / denom, -limit, limit);
        out.push([cur[0] + mx * scale, cur[1] + my * scale]);
    }
    return out;
}

// Outline of `width` pixels drawn outside the polygon boundary.
function strokePolygonRing(data, w, h, pts, width, rgba, alpha = 1) {
    const outer = offsetPolygon(pts, width);
    if (!outer) {
        strokePolygon(data, w, h, pts, rgba, alpha, width, true);
        return;
    }
    drawSpanRing(data, w, h, polygonSpans(outer, w, h), polygonSpans(pts, w, h), rgba, alpha);
}

// Centred stroke along the polygon edges (the closing edge optional).
function strokePolygon(data, w, h, pts, rgba, alpha = 1, width = 1, closed = true) {
    const edges = closed ? pts.length : pts.length - 1;
    for (let i = 0; i < edges; ++i) {
        const a = pts[i], b = pts[(i + 1) % pts.length];
        strokeLine(data, w, h, a[0], a[1], b[0], b[1], rgba, alpha, width);
    }
}

// Outline of `width` pixels drawn outside an axis-aligned rectangle.
function strokeRectRing(data, w, h, x0, y0, rw, rh, width, rgba, alpha = 1) {
    const ax = Math.round(x0), ay = Math.round(y0);
    const bx = Math.round(x0 + rw), by = Math.round(y0 + rh);
    const n = Math.max(1, Math.round(width));
    fillRect(data, w, h, ax - n, ay - n, (bx - ax) + 2 * n, n, rgba, alpha);   // top
    fillRect(data, w, h, ax - n, by, (bx - ax) + 2 * n, n, rgba, alpha);       // bottom
    fillRect(data, w, h, ax - n, ay, n, by - ay, rgba, alpha);                 // left
    fillRect(data, w, h, bx, ay, n, by - ay, rgba, alpha);                     // right
}

// Outline of `width` pixels drawn outside the ellipse boundary.
function strokeEllipseRing(data, w, h, cx, cy, rx, ry, width, rgba, alpha = 1) {
    const ax = Math.abs(rx), ay = Math.abs(ry);
    const ox = ax + width, oy = ay + width;
    const x0 = Math.max(0, Math.floor(cx - ox)), x1 = Math.min(w - 1, Math.ceil(cx + ox));
    const y0 = Math.max(0, Math.floor(cy - oy)), y1 = Math.min(h - 1, Math.ceil(cy + oy));
    for (let y = y0; y <= y1; ++y) {
        for (let x = x0; x <= x1; ++x) {
            const px = x + 0.5 - cx, py = y + 0.5 - cy;
            const outside = (px * px) / ((ox || 1) * (ox || 1)) + (py * py) / ((oy || 1) * (oy || 1));
            if (outside > 1) continue;
            const inside = (px * px) / ((ax || 1) * (ax || 1)) + (py * py) / ((ay || 1) * (ay || 1));
            if (inside <= 1) continue;
            blendPixel(data, w, h, x, y, rgba, alpha);
        }
    }
}

// Regular polygon vertices around (cx, cy); `inner` between 0 and 1 turns it
// into a `sides`-pointed star. rotation is in degrees, 0 points up.
function regularPolygonPoints(options) {
    const sides = Math.round(Number(options.sides));
    const cx = Number(options.cx), cy = Number(options.cy), radius = Number(options.radius);
    if (!(sides >= 3) || !Number.isFinite(cx) || !Number.isFinite(cy) || !Number.isFinite(radius)) return null;
    const rotation = Number(options.rotation) || 0;
    const inner = options.inner === undefined || options.inner === null ? 0 : Number(options.inner);
    const star = Number.isFinite(inner) && inner > 0;
    const count = star ? sides * 2 : sides;
    const step = (Math.PI * 2) / count;
    const start = (-90 + rotation) * Math.PI / 180;
    const ir = star ? radius * clamp(inner, 0.01, 1) : radius;
    const pts = [];
    for (let i = 0; i < count; ++i) {
        const r = star && i % 2 === 1 ? ir : radius;
        const a = start + i * step;
        pts.push([cx + Math.cos(a) * r, cy + Math.sin(a) * r]);
    }
    return pts;
}

// ---------------------------------------------------------------- transforms
// Image-level operations behind `image.flip/rotate/crop` and the `--mirror`
// draw option. `axis` strings accept x, y, both or xy.

function flipLayerData(data, w, h, axis) {
    const flipX = axis === 'x' || axis === 'both' || axis === 'xy';
    const flipY = axis === 'y' || axis === 'both' || axis === 'xy';
    if (flipX) {
        for (let y = 0; y < h; ++y) {
            for (let x = 0; x < (w >> 1); ++x) {
                const a = (y * w + x) * 4, b = (y * w + (w - 1 - x)) * 4;
                for (let c = 0; c < 4; ++c) { const t = data[a + c]; data[a + c] = data[b + c]; data[b + c] = t; }
            }
        }
    }
    if (flipY) {
        for (let y = 0; y < (h >> 1); ++y) {
            for (let x = 0; x < w; ++x) {
                const a = (y * w + x) * 4, b = ((h - 1 - y) * w + x) * 4;
                for (let c = 0; c < 4; ++c) { const t = data[a + c]; data[a + c] = data[b + c]; data[b + c] = t; }
            }
        }
    }
    return data;
}

// Rotates a buffer by `turns * 90` degrees clockwise. Returns the new size and
// a fresh buffer (0 and 180 keep the dimensions, 90 and 270 swap them).
function rotateRgba90(data, w, h, turns) {
    const t = ((Math.round(turns) % 4) + 4) % 4;
    if (t === 0) return { width: w, height: h, data: Buffer.from(data) };
    if (t === 2) {
        const out = Buffer.alloc(w * h * 4);
        for (let y = 0; y < h; ++y) {
            for (let x = 0; x < w; ++x) {
                const s = (y * w + x) * 4, d = ((h - 1 - y) * w + (w - 1 - x)) * 4;
                for (let c = 0; c < 4; ++c) out[d + c] = data[s + c];
            }
        }
        return { width: w, height: h, data: out };
    }
    const nw = h, nh = w;
    const out = Buffer.alloc(nw * nh * 4);
    for (let y = 0; y < h; ++y) {
        for (let x = 0; x < w; ++x) {
            const s = (y * w + x) * 4;
            const dx = t === 1 ? h - 1 - y : y;
            const dy = t === 1 ? x : w - 1 - x;
            const d = (dy * nw + dx) * 4;
            for (let c = 0; c < 4; ++c) out[d + c] = data[s + c];
        }
    }
    return { width: nw, height: nh, data: out };
}

// Copies a sub-rectangle out of a buffer, padding with transparency where the
// rectangle reaches past the source.
function cropRgba(data, w, h, x, y, cw, ch) {
    const out = Buffer.alloc(cw * ch * 4);
    for (let j = 0; j < ch; ++j) {
        const sy = y + j;
        if (sy < 0 || sy >= h) continue;
        for (let i = 0; i < cw; ++i) {
            const sx = x + i;
            if (sx < 0 || sx >= w) continue;
            const s = (sy * w + sx) * 4, d = (j * cw + i) * 4;
            out[d] = data[s]; out[d + 1] = data[s + 1]; out[d + 2] = data[s + 2]; out[d + 3] = data[s + 3];
        }
    }
    return out;
}

// Composites `src` ({width, height, data}) into `dst` at an offset, either
// source-over (the default) or as a hard replace that also copies alpha.
function stampImage(dst, dw, dh, src, options = {}) {
    const alpha = clamp(options.alpha === undefined ? 1 : options.alpha, 0, 1);
    const dx = Math.round(options.x === undefined ? 0 : options.x);
    const dy = Math.round(options.y === undefined ? 0 : options.y);
    const replace = options.blend === 'replace';
    let painted = 0;
    for (let y = 0; y < src.height; ++y) {
        const ty = dy + y;
        if (ty < 0 || ty >= dh) continue;
        for (let x = 0; x < src.width; ++x) {
            const tx = dx + x;
            if (tx < 0 || tx >= dw) continue;
            const s = (y * src.width + x) * 4, d = (ty * dw + tx) * 4;
            const sa = (src.data[s + 3] / 255) * alpha;
            if (replace) {
                dst[d] = src.data[s];
                dst[d + 1] = src.data[s + 1];
                dst[d + 2] = src.data[s + 2];
                dst[d + 3] = Math.round(sa * 255);
                painted++;
                continue;
            }
            if (sa <= 0) continue;
            const da = dst[d + 3] / 255;
            const oa = sa + da * (1 - sa);
            for (let c = 0; c < 3; ++c) {
                const sc = (src.data[s + c] / 255) * sa;
                const dc = (dst[d + c] / 255) * da * (1 - sa);
                dst[d + c] = Math.round(clamp((sc + dc) / oa, 0, 1) * 255);
            }
            dst[d + 3] = Math.round(clamp(oa, 0, 1) * 255);
            painted++;
        }
    }
    return painted;
}

// Reflects a draw call's geometry about the canvas centre lines so `--mirror`
// paints the shape and its reflection in one call. `box` marks commands whose
// x/y is the top-left corner of a width/height box (a rectangle) rather than a
// point. Returns a copy with `mirror` cleared.
function mirrorShapeArgs(args, w, h, box = false) {
    const out = Object.assign({}, args);
    const mode = String(args.mirror || 'none');
    const flipX = mode === 'x' || mode === 'both' || mode === 'xy';
    const flipY = mode === 'y' || mode === 'both' || mode === 'xy';
    if (flipX) {
        const f = v => w - v;
        for (const k of ['x', 'x1', 'x2', 'cx']) if (out[k] !== undefined) out[k] = f(out[k]);
        if (box && args.x !== undefined && args.width !== undefined) out.x = f(args.x + args.width);
        if (out.points !== undefined) {
            const pts = parsePoints(out.points);
            if (pts) out.points = pts.map(p => [f(p[0]), p[1]]);
        }
    }
    if (flipY) {
        const f = v => h - v;
        for (const k of ['y', 'y1', 'y2', 'cy']) if (out[k] !== undefined) out[k] = f(out[k]);
        if (box && args.y !== undefined && args.height !== undefined) out.y = f(args.y + args.height);
        if (out.points !== undefined) {
            const pts = parsePoints(out.points);
            if (pts) out.points = pts.map(p => [p[0], f(p[1])]);
        }
    }
    out.mirror = 'none';
    return out;
}

// ------------------------------------------------- noise, gradients, palettes
// Everything below is a pure function of its arguments: the same seed paints
// the same pixels, nothing reaches Math.random and there is no time or hidden
// state, so an agent can regenerate a texture bit for bit. With `tile` each
// noise octave's lattice is snapped to an integer number of cells across the
// canvas, which makes the field exactly periodic and the tile seam vanish.

// A pure integer hash, used as the noise lattice: the value at (x, y) for a
// seed and channel. Written out rather than taken from a library so the exact
// pixels are pinned by the tests.
function hashLattice(seed, x, y, channel) {
    let h = Math.imul(x | 0, 0x27d4eb2d) ^ Math.imul(y | 0, 0x165667b1)
        ^ Math.imul((((seed | 0) + Math.imul(channel | 0, 0x51ed270b)) | 0), 0x9e3779b9);
    h = Math.imul(h ^ (h >>> 15), 0x2c1b3c6d);
    h = Math.imul(h ^ (h >>> 12), 0x297a2d39);
    return ((h ^ (h >>> 15)) >>> 0) / 4294967296;
}

// Bilinear value noise on a lattice of cellsX x cellsY cells, summed into `out`
// with `weight`. With `tile` the lattice indexes wrap, so the field is periodic
// in the canvas size; without it the cell past the last one is just another
// lattice value (which is the whole difference between a tile and a patch).
function valueNoiseOctave(width, height, cellsX, cellsY, seed, tile, weight, out) {
    const stepX = cellsX / width, stepY = cellsY / height;
    const at = (ix, iy) => {
        const x = tile ? ((ix % cellsX) + cellsX) % cellsX : ix;
        const y = tile ? ((iy % cellsY) + cellsY) % cellsY : iy;
        return hashLattice(seed, x, y, 0);
    };
    for (let y = 0; y < height; ++y) {
        const fy = y * stepY;
        const iy = Math.floor(fy), ty = fy - iy;
        for (let x = 0; x < width; ++x) {
            const fx = x * stepX;
            const ix = Math.floor(fx), tx = fx - ix;
            const top = at(ix, iy) + (at(ix + 1, iy) - at(ix, iy)) * tx;
            const bot = at(ix, iy + 1) + (at(ix + 1, iy + 1) - at(ix, iy + 1)) * tx;
            out[y * width + x] += (top + (bot - top) * ty) * weight;
        }
    }
}

// One value in [0, 1] per pixel. `kind` is `value` (smooth blobs), `white`
// (independent grain) or `fractal` (value noise summed over octaves).
function noiseField(width, height, options = {}) {
    const out = new Float64Array(width * height);
    const seed = options.seed === undefined ? 0 : (options.seed | 0);
    const kind = options.kind || 'value';
    if (kind === 'white') {
        // Grain has no lattice to wrap, so `tile` is a no-op: independent
        // pixels already tile.
        for (let y = 0; y < height; ++y) {
            for (let x = 0; x < width; ++x) out[y * width + x] = hashLattice(seed, x, y, 0);
        }
        return out;
    }
    const cell = Math.max(1e-6, options.cell === undefined ? 16 : options.cell);
    const cells = (px, cellPx) => Math.max(1, Math.round(px / cellPx));
    if (kind !== 'fractal') {
        valueNoiseOctave(width, height, cells(width, cell), cells(height, cell), seed, !!options.tile, 1, out);
        return out;
    }
    const octaves = Math.max(1, options.octaves === undefined ? 4 : (options.octaves | 0));
    const lacunarity = Math.max(1, options.lacunarity === undefined ? 2 : options.lacunarity);
    const gain = clamp(options.gain === undefined ? 0.5 : options.gain, 0, 1);
    let total = 0, weight = 1, octaveCell = cell;
    for (let o = 0; o < octaves; ++o) {
        total += weight;
        // Each octave gets its own lattice (a shifted seed), otherwise the
        // finer octaves repeat the coarse one's zero crossings.
        valueNoiseOctave(width, height, cells(width, octaveCell), cells(height, octaveCell),
            (seed + Math.imul(o, 1013904223)) | 0, !!options.tile, weight, out);
        weight *= gain;
        octaveCell /= lacunarity;
    }
    if (total > 0) for (let i = 0; i < out.length; ++i) out[i] /= total;
    return out;
}

// Clamps an optional x/y/width/height region to the canvas; anything missing
// defaults to the whole canvas, so a fill can say `--x 8 --y 8 --width 16
// --height 16` or nothing at all.
function regionBounds(width, height, options = {}) {
    const x = options.x === undefined ? 0 : options.x;
    const y = options.y === undefined ? 0 : options.y;
    const rw = options.width === undefined ? width : options.width;
    const rh = options.height === undefined ? height : options.height;
    const x0 = clamp(Math.floor(x), 0, width);
    const y0 = clamp(Math.floor(y), 0, height);
    const x1 = clamp(Math.ceil(x + rw), 0, width);
    const y1 = clamp(Math.ceil(y + rh), 0, height);
    return { x0, y0, x1: Math.max(x0, x1), y1: Math.max(y0, y1) };
}

// Source-over on a straight-alpha RGBA buffer. The same composition the
// composite/stamp paths use, so a 50% ramp over a 50% pixel really blends
// instead of taking the larger alpha the brush path does.
function compositePixel(data, w, h, x, y, r, g, b, alpha) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    const sa = clamp(alpha, 0, 1);
    if (sa <= 0) return;
    const o = (y * w + x) * 4;
    const da = data[o + 3] / 255;
    const oa = sa + da * (1 - sa);
    if (oa <= 0) return;
    const src = [r, g, b];
    for (let c = 0; c < 3; ++c) {
        const sc = clamp(src[c], 0, 1) * sa;
        const dc = (data[o + c] / 255) * da * (1 - sa);
        data[o + c] = Math.round(clamp((sc + dc) / oa, 0, 1) * 255);
    }
    data[o + 3] = Math.round(clamp(oa, 0, 1) * 255);
}

// Paints the noise field through a `from` -> `to` colour ramp. Returns how many
// pixels were written (a fully transparent ramp leaves the region alone).
function drawNoise(data, w, h, options = {}) {
    const from = options.from || [0, 0, 0, 1];
    const to = options.to || [1, 1, 1, 1];
    const opacity = clamp(options.opacity === undefined ? 1 : options.opacity, 0, 1);
    const region = regionBounds(w, h, options);
    const field = noiseField(w, h, options);
    let painted = 0;
    for (let y = region.y0; y < region.y1; ++y) {
        for (let x = region.x0; x < region.x1; ++x) {
            const v = clamp(field[y * w + x], 0, 1);
            const a = (from[3] + (to[3] - from[3]) * v) * opacity;
            if (a <= 0) continue;
            compositePixel(data, w, h, x, y,
                from[0] + (to[0] - from[0]) * v,
                from[1] + (to[1] - from[1]) * v,
                from[2] + (to[2] - from[2]) * v, a);
            painted++;
        }
    }
    return painted;
}

// Paints a `from` -> `to` ramp: `linear` along an axis, or `radial` from a
// centre outwards. The linear axis runs through the canvas centre and is as
// long as the box is wide along it, so opposite corners land on 0 and 1
// whatever the angle (the CSS convention) - or give --x1/--y1/--x2/--y2 for
// explicit endpoints.
function drawGradient(data, w, h, options = {}) {
    const from = options.from || [0, 0, 0, 1];
    const to = options.to || [1, 1, 1, 1];
    const opacity = clamp(options.opacity === undefined ? 1 : options.opacity, 0, 1);
    const region = regionBounds(w, h, options);
    let position;
    if ((options.kind || 'linear') === 'radial') {
        const cx = options.cx === undefined ? w / 2 : options.cx;
        const cy = options.cy === undefined ? h / 2 : options.cy;
        const radius = Math.max(1e-6, Math.abs(options.radius === undefined ? Math.min(w, h) / 2 : options.radius));
        position = (x, y) => Math.hypot(x + 0.5 - cx, y + 0.5 - cy) / radius;
    } else {
        let p0x, p0y, dx, dy;
        if (options.x1 !== undefined || options.x2 !== undefined) {
            p0x = options.x1 === undefined ? 0 : options.x1;
            p0y = options.y1 === undefined ? 0 : options.y1;
            dx = (options.x2 === undefined ? w : options.x2) - p0x;
            dy = (options.y2 === undefined ? h : options.y2) - p0y;
        } else {
            const angle = (options.angle === undefined ? 90 : options.angle) * Math.PI / 180;
            const ux = Math.cos(angle), uy = Math.sin(angle);
            const len = Math.abs(w * ux) + Math.abs(h * uy);
            p0x = w / 2 - ux * len / 2;
            p0y = h / 2 - uy * len / 2;
            dx = ux * len;
            dy = uy * len;
        }
        const len2 = (dx * dx + dy * dy) || 1e-9;
        position = (x, y) => ((x + 0.5 - p0x) * dx + (y + 0.5 - p0y) * dy) / len2;
    }
    let painted = 0;
    for (let y = region.y0; y < region.y1; ++y) {
        for (let x = region.x0; x < region.x1; ++x) {
            const k = clamp(position(x, y), 0, 1);
            const a = (from[3] + (to[3] - from[3]) * k) * opacity;
            if (a <= 0) continue;
            compositePixel(data, w, h, x, y,
                from[0] + (to[0] - from[0]) * k,
                from[1] + (to[1] - from[1]) * k,
                from[2] + (to[2] - from[2]) * k, a);
            painted++;
        }
    }
    return painted;
}

// How many pixels carry each distinct colour (fully transparent pixels have no
// colour and are skipped). A Map keyed by the packed 0xRRGGBB.
function colourHistogram(data, w, h) {
    const counts = new Map();
    for (let p = 0; p < w * h; ++p) {
        const o = p * 4;
        if (data[o + 3] === 0) continue;
        const key = (data[o] << 16) | (data[o + 1] << 8) | data[o + 2];
        counts.set(key, (counts.get(key) || 0) + 1);
    }
    return counts;
}

// Median cut: repeatedly split the box whose colours span the widest range,
// until there are `maxColors` boxes, then report each box's average colour.
// The histogram is collapsed to distinct colours first, so a flat sprite comes
// back as exactly its own colours (never a duplicate entry) and a photo gets
// split by where the colours are, not by pixel order.
function medianCutPalette(histogram, maxColors) {
    const channel = (v, c) => (c === 0 ? (v >> 16) & 255 : c === 1 ? (v >> 8) & 255 : v & 255);
    const boxes = [];
    {
        const entries = [];
        let total = 0;
        for (const [v, c] of histogram) {
            entries.push([v, c]);
            total += c;
        }
        if (entries.length) boxes.push({ entries, total });
    }
    const sum = (entries) => entries.reduce((n, e) => n + e[1], 0);
    while (boxes.length < Math.max(1, maxColors)) {
        let best = -1, bestSpan = -1, bestAxis = 0;
        for (let i = 0; i < boxes.length; ++i) {
            const entries = boxes[i].entries;
            if (entries.length < 2) continue;
            const min = [255, 255, 255], max = [0, 0, 0];
            for (const [v] of entries) {
                for (let c = 0; c < 3; ++c) {
                    const x = channel(v, c);
                    if (x < min[c]) min[c] = x;
                    if (x > max[c]) max[c] = x;
                }
            }
            const span = [max[0] - min[0], max[1] - min[1], max[2] - min[2]];
            const axis = span[0] >= span[1] && span[0] >= span[2] ? 0 : (span[1] >= span[2] ? 1 : 2);
            if (span[axis] > bestSpan) { best = i; bestSpan = span[axis]; bestAxis = axis; }
        }
        if (best < 0 || bestSpan <= 0) break;
        const box = boxes[best];
        box.entries.sort((a, b) => channel(a[0], bestAxis) - channel(b[0], bestAxis));
        let acc = 0, cut = 1;
        for (let i = 0; i < box.entries.length; ++i) {
            acc += box.entries[i][1];
            if (acc * 2 >= box.total) { cut = clamp(i + 1, 1, box.entries.length - 1); break; }
        }
        const left = box.entries.slice(0, cut), right = box.entries.slice(cut);
        boxes.splice(best, 1, { entries: left, total: sum(left) }, { entries: right, total: sum(right) });
    }
    return boxes.map((box) => {
        let r = 0, g = 0, b = 0;
        for (const [v, c] of box.entries) {
            r += ((v >> 16) & 255) * c;
            g += ((v >> 8) & 255) * c;
            b += (v & 255) * c;
        }
        const n = box.total || 1;
        return { r: Math.round(r / n), g: Math.round(g / n), b: Math.round(b / n), count: box.total };
    }).sort((a, b) => b.count - a.count);
}

// The palette of an image, at most `maxColors` entries, most pixels first.
function paletteFromImage(data, w, h, maxColors) {
    return medianCutPalette(colourHistogram(data, w, h), maxColors);
}

function nearestPaletteIndex(palette, r, g, b) {
    let best = 0, bestDist = Infinity;
    for (let i = 0; i < palette.length; ++i) {
        const dr = r - palette[i].r, dg = g - palette[i].g, db = b - palette[i].b;
        const d = dr * dr + dg * dg + db * db;
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
}

// The average distance from a palette entry to its nearest neighbour: the
// scale of one quantization step, used to size an ordered dither.
function paletteStep(palette) {
    if (palette.length < 2) return 0;
    let total = 0;
    for (let i = 0; i < palette.length; ++i) {
        let best = Infinity;
        for (let j = 0; j < palette.length; ++j) {
            if (i === j) continue;
            const dr = palette[i].r - palette[j].r, dg = palette[i].g - palette[j].g, db = palette[i].b - palette[j].b;
            best = Math.min(best, Math.sqrt(dr * dr + dg * dg + db * db));
        }
        total += best;
    }
    return total / palette.length;
}

// A 4x4 Bayer threshold matrix, the classic ordered-dither pattern.
const BAYER4 = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5];

// Snaps every visible pixel of a layer to `palette` ([{r,g,b}]), optionally
// dithering: `floyd` diffuses the error to the neighbours, `ordered` adds a
// Bayer threshold scaled to the palette step. Both use one threshold/error per
// pixel on all three channels, so the dither does not shift the hue. Fully
// transparent pixels are left alone and semi-transparent pixels keep their
// alpha. Returns how many pixels changed.
function quantizeLayer(data, w, h, options = {}) {
    const palette = options.palette || [];
    const dither = options.dither || 'none';
    if (!palette.length) return 0;
    let changed = 0;
    const put = (o, p) => {
        if (data[o] !== p.r || data[o + 1] !== p.g || data[o + 2] !== p.b) changed++;
        data[o] = p.r;
        data[o + 1] = p.g;
        data[o + 2] = p.b;
    };
    if (dither === 'floyd') {
        // Two rolling error rows: the one being written and the one below it.
        const err = new Float64Array(3 * w * 2);
        const add = (base, i, e, weight) => {
            base[i * 3] += e[0] * weight;
            base[i * 3 + 1] += e[1] * weight;
            base[i * 3 + 2] += e[2] * weight;
        };
        for (let y = 0; y < h; ++y) {
            const cur = (y % 2) * 3 * w, nxt = ((y + 1) % 2) * 3 * w;
            err.fill(0, nxt, nxt + 3 * w);
            for (let x = 0; x < w; ++x) {
                const o = (y * w + x) * 4;
                if (data[o + 3] === 0) continue;
                const r = clamp(data[o] + err[cur + x * 3], 0, 255);
                const g = clamp(data[o + 1] + err[cur + x * 3 + 1], 0, 255);
                const b = clamp(data[o + 2] + err[cur + x * 3 + 2], 0, 255);
                const p = palette[nearestPaletteIndex(palette, r, g, b)];
                const e = [r - p.r, g - p.g, b - p.b];
                put(o, p);
                if (x + 1 < w) add(err, cur + x + 1, e, 7 / 16);
                if (y + 1 < h) {
                    if (x > 0) add(err, nxt + x - 1, e, 3 / 16);
                    add(err, nxt + x, e, 5 / 16);
                    if (x + 1 < w) add(err, nxt + x + 1, e, 1 / 16);
                }
            }
        }
        return changed;
    }
    const step = dither === 'ordered' ? paletteStep(palette) : 0;
    for (let y = 0; y < h; ++y) {
        for (let x = 0; x < w; ++x) {
            const o = (y * w + x) * 4;
            if (data[o + 3] === 0) continue;
            const bias = dither === 'ordered' ? (BAYER4[(y % 4) * 4 + (x % 4)] / 16 - 0.5) * step : 0;
            const p = palette[nearestPaletteIndex(palette,
                clamp(data[o] + bias, 0, 255),
                clamp(data[o + 1] + bias, 0, 255),
                clamp(data[o + 2] + bias, 0, 255))];
            put(o, p);
        }
    }
    return changed;
}

// How many pixels would land on each entry of an explicit palette.
function paletteCounts(data, w, h, palette) {
    const counts = new Array(palette.length).fill(0);
    if (!palette.length) return counts;
    for (let p = 0; p < w * h; ++p) {
        const o = p * 4;
        if (data[o + 3] === 0) continue;
        counts[nearestPaletteIndex(palette, data[o], data[o + 1], data[o + 2])]++;
    }
    return counts;
}

// --------------------------------------------------------------- normal maps
// Height field -> tangent-space normal map (Sobel), as used by the editor.
function normalMapFromHeight(rgba, w, h, options = {}) {
    const source = options.source === 'alpha' ? 'alpha' : 'luma';
    const strength = options.strength === undefined ? 6 : options.strength;
    const invertX = !!options.invertX;
    const invertY = !!options.invertY;
    const height = new Float32Array(w * h);
    for (let i = 0; i < w * h; ++i) {
        const o = i * 4;
        height[i] = source === 'alpha'
            ? rgba[o + 3] / 255
            : (0.2126 * rgba[o] + 0.7152 * rgba[o + 1] + 0.0722 * rgba[o + 2]) / 255;
    }
    const out = Buffer.alloc(w * h * 4);
    const at = (x, y) => height[((y + h) % h) * w + ((x + w) % w)];
    for (let y = 0; y < h; ++y) {
        for (let x = 0; x < w; ++x) {
            const dx =
                (at(x + 1, y - 1) + 2 * at(x + 1, y) + at(x + 1, y + 1)) -
                (at(x - 1, y - 1) + 2 * at(x - 1, y) + at(x - 1, y + 1));
            const dy =
                (at(x - 1, y + 1) + 2 * at(x, y + 1) + at(x + 1, y + 1)) -
                (at(x - 1, y - 1) + 2 * at(x, y - 1) + at(x + 1, y - 1));
            let nx = -dx * strength * 0.25, ny = -dy * strength * 0.25;
            const nz = 1;
            if (invertX) nx = -nx;
            if (invertY) ny = -ny;
            const len = Math.hypot(nx, ny, nz) || 1;
            const o = (y * w + x) * 4;
            out[o] = Math.round((nx / len * 0.5 + 0.5) * 255);
            out[o + 1] = Math.round((ny / len * 0.5 + 0.5) * 255);
            out[o + 2] = Math.round((nz / len * 0.5 + 0.5) * 255);
            out[o + 3] = 255;
        }
    }
    return out;
}

// ------------------------------------------------------------------- analysis
// Pixel statistics for `image.info`. Beyond the histogram this reports what an
// agent needs to check its own work without seeing the picture: the non
// transparent bounding box, coverage, edge clipping, mirror symmetry and the
// dominant colors. `options.layer` selects one layer, `options.spans` adds
// per-row/column content extents.
function analyze(doc, options = {}) {
    const w = doc.width, h = doc.height, n = w * h;
    const data = options.layer === undefined || options.layer === null
        ? composite(doc)
        : doc.layers[options.layer].data;
    const histogram = { r: new Array(16).fill(0), g: new Array(16).fill(0), b: new Array(16).fill(0), a: new Array(16).fill(0) };
    let opaque = 0, transparent = 0, sum = [0, 0, 0, 0];
    let minX = w, minY = h, maxX = -1, maxY = -1;
    const buckets = new Map();
    const rowSpans = options.spans ? new Array(h).fill(null) : null;
    const colSpans = options.spans ? new Array(w).fill(null) : null;
    for (let y = 0; y < h; ++y) {
        for (let x = 0; x < w; ++x) {
            const o = (y * w + x) * 4;
            const r = data[o], g = data[o + 1], b = data[o + 2], a = data[o + 3];
            histogram.r[r >> 4]++;
            histogram.g[g >> 4]++;
            histogram.b[b >> 4]++;
            histogram.a[a >> 4]++;
            if (a === 0) { transparent++; continue; }
            opaque++;
            sum[0] += r; sum[1] += g; sum[2] += b; sum[3] += a;
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
            if (y < minY) minY = y;
            if (y > maxY) maxY = y;
            const key = ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
            buckets.set(key, (buckets.get(key) || 0) + 1);
            if (rowSpans) {
                if (rowSpans[y] === null) rowSpans[y] = [x, x];
                else rowSpans[y][1] = x;
            }
            if (colSpans) {
                if (colSpans[x] === null) colSpans[x] = [y, y];
                else colSpans[x][1] = y;
            }
        }
    }
    const bounds = maxX >= 0 ? { x: minX, y: minY, width: maxX - minX + 1, height: maxY - minY + 1 } : null;

    // Mirror symmetry: left<->right about the vertical centre line, and
    // top<->bottom about the horizontal one. Two transparent pixels always
    // match, whatever color they carry.
    const same = (a, b) => (data[a + 3] === 0 && data[b + 3] === 0)
        || (data[a] === data[b] && data[a + 1] === data[b + 1] && data[a + 2] === data[b + 2] && data[a + 3] === data[b + 3]);
    let mirrorLeftRight = true, mirrorLeftRightMismatch = 0;
    for (let y = 0; y < h && mirrorLeftRight; ++y) {
        for (let x = 0; x < (w >> 1); ++x) {
            if (!same((y * w + x) * 4, (y * w + (w - 1 - x)) * 4)) { mirrorLeftRight = false; mirrorLeftRightMismatch++; }
        }
    }
    let mirrorTopBottom = true, mirrorTopBottomMismatch = 0;
    for (let y = 0; y < (h >> 1) && mirrorTopBottom; ++y) {
        for (let x = 0; x < w; ++x) {
            if (!same((y * w + x) * 4, ((h - 1 - y) * w + x) * 4)) { mirrorTopBottom = false; mirrorTopBottomMismatch++; }
        }
    }

    // Dominant colors, quantised to 4 bits per channel.
    const toHex = v => v.toString(16).padStart(2, '0');
    const topColors = [...buckets.entries()]
        .sort((a, b) => b[1] - a[1] || a[0] - b[0])
        .slice(0, 8)
        .map(([key, count]) => ({
            hex: '#' + toHex((key >> 8 & 15) * 17) + toHex((key >> 4 & 15) * 17) + toHex((key & 15) * 17),
            count,
        }));

    return {
        width: w,
        height: h,
        pixelCount: n,
        opaquePixels: opaque,
        transparentPixels: transparent,
        coverage: n ? opaque / n : 0,
        averageColor: sum.map(v => Math.round(v / n)),
        histogram,
        bounds,
        topColors,
        mirrorLeftRight,
        mirrorLeftRightMismatch,
        mirrorTopBottom,
        mirrorTopBottomMismatch,
        touchesEdge: bounds
            ? { left: minX === 0, right: maxX === w - 1, top: minY === 0, bottom: maxY === h - 1 }
            : { left: false, right: false, top: false, bottom: false },
        ...(options.spans ? { rowSpans, colSpans } : {}),
    };
}

// Renders the document (or one layer) as text so an agent can "look at" its
// work in a terminal or straight out of the JSON envelope. Blocks of
// `options.scale` pixels average into one character; the color is weighted by
// alpha so transparent pixels do not wash it out. `options.format` is ascii
// (a luminance ramp), blocks (a Unicode density ramp) or hex (one #rrggbb per
// cell).
function previewText(doc, options = {}) {
    const w = doc.width, h = doc.height;
    const data = options.layer === undefined || options.layer === null
        ? composite(doc)
        : doc.layers[options.layer].data;
    const scale = Math.max(1, Math.round(Number(options.scale) || 1));
    const cols = Math.ceil(w / scale), rows = Math.ceil(h / scale);
    const format = options.format || 'ascii';
    const ramp = options.chars === undefined || options.chars === null
        ? (format === 'blocks' ? ' \u2591\u2592\u2593\u2588' : ' .:-=+*#%@')
        : String(options.chars);
    const empty = options.empty === undefined || options.empty === null ? ' ' : String(options.empty).charAt(0);
    // A hex cell is 7 characters wide, so an empty cell has to be padded to
    // match or the grid stops lining up wherever there is transparency.
    const cellWidth = format === 'hex' ? 7 : 1;
    const emptyCell = empty.repeat(cellWidth);
    const sep = format === 'hex' && cols > 1 ? ' ' : '';
    const lines = [];
    for (let ry = 0; ry < rows; ++ry) {
        const y1 = Math.min(h, (ry + 1) * scale);
        let line = '';
        for (let rx = 0; rx < cols; ++rx) {
            const x1 = Math.min(w, (rx + 1) * scale);
            let sr = 0, sg = 0, sb = 0, sa = 0, cells = 0;
            for (let y = ry * scale; y < y1; ++y) {
                for (let x = rx * scale; x < x1; ++x) {
                    const o = (y * w + x) * 4;
                    const a = data[o + 3] / 255;
                    sr += data[o] * a;
                    sg += data[o + 1] * a;
                    sb += data[o + 2] * a;
                    sa += a;
                    cells++;
                }
            }
            const alpha = cells ? sa / cells : 0;
            if (alpha <= 0.002) { line += emptyCell + (rx < cols - 1 ? sep : ''); continue; }
            const r = sr / sa, g = sg / sa, b = sb / sa;
            if (format === 'hex') {
                line += rgbaToHex([r / 255, g / 255, b / 255]) + (rx < cols - 1 ? sep : '');
            } else {
                const luma = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255;
                const idx = clamp(Math.round(luma * (ramp.length - 1)), 0, ramp.length - 1);
                line += ramp.charAt(idx);
            }
        }
        lines.push(line);
    }
    return { width: w, height: h, scale, cols, rows, format, lines };
}

// ---------------------------------------------------------------- PNG codec
const CRC_TABLE = (() => {
    const table = new Int32Array(256);
    for (let n = 0; n < 256; ++n) {
        let c = n;
        for (let k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
        table[n] = c;
    }
    return table;
})();

function crc32(buf) {
    let c = 0xffffffff;
    for (let i = 0; i < buf.length; ++i) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
}

function pngChunk(type, data) {
    const len = Buffer.alloc(4);
    len.writeUInt32BE(data.length, 0);
    const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(body), 0);
    return Buffer.concat([len, body, crc]);
}

function encodePng(rgba, width, height) {
    const bytesPerRow = width * 4 + 1;
    const raw = Buffer.alloc(bytesPerRow * height);
    for (let y = 0; y < height; ++y) {
        raw[y * bytesPerRow] = 0;
        rgba.copy(raw, y * bytesPerRow + 1, y * width * 4, (y + 1) * width * 4);
    }
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(width, 0);
    ihdr.writeUInt32BE(height, 4);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    return Buffer.concat([
        Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
        pngChunk('IHDR', ihdr),
        pngChunk('IDAT', zlib.deflateSync(raw)),
        pngChunk('IEND', Buffer.alloc(0)),
    ]);
}

function paeth(a, b, c) {
    const p = a + b - c;
    const pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

function decodePng(buffer) {
    const signature = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
    if (buffer.length < 8 || !buffer.subarray(0, 8).equals(signature)) throw new Error('not a PNG file');
    let offset = 8;
    let header = null, palette = null, transparency = null;
    const idat = [];
    while (offset + 8 <= buffer.length) {
        const length = buffer.readUInt32BE(offset);
        const type = buffer.toString('ascii', offset + 4, offset + 8);
        const data = buffer.subarray(offset + 8, offset + 8 + length);
        if (type === 'IHDR') {
            header = {
                width: data.readUInt32BE(0),
                height: data.readUInt32BE(4),
                bitDepth: data[8],
                colorType: data[9],
                interlace: data[12],
            };
        } else if (type === 'PLTE') palette = Buffer.from(data);
        else if (type === 'tRNS') transparency = Buffer.from(data);
        else if (type === 'IDAT') idat.push(Buffer.from(data));
        else if (type === 'IEND') break;
        offset += 12 + length;
    }
    if (!header) throw new Error('PNG has no IHDR chunk');
    if (header.interlace !== 0) throw new Error('interlaced PNG is not supported');
    const channels = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 }[header.colorType];
    if (!channels) throw new Error(`unsupported PNG color type ${header.colorType}`);
    const depth = header.bitDepth;
    if (![8, 16].includes(depth)) throw new Error(`unsupported PNG bit depth ${depth}`);
    const sampleBytes = depth / 8;
    const { width, height } = header;
    const bpp = channels * sampleBytes;
    const stride = width * bpp;
    const raw = zlib.inflateSync(Buffer.concat(idat));
    if (raw.length < (stride + 1) * height) throw new Error('PNG data is truncated');

    const lines = Buffer.alloc(stride * height);
    for (let y = 0; y < height; ++y) {
        const filter = raw[y * (stride + 1)];
        const src = y * (stride + 1) + 1;
        const dst = y * stride;
        const up = dst - stride;
        for (let x = 0; x < stride; ++x) {
            const a = x >= bpp ? lines[dst + x - bpp] : 0;
            const b = y > 0 ? lines[up + x] : 0;
            const c = (x >= bpp && y > 0) ? lines[up + x - bpp] : 0;
            const v = raw[src + x];
            let out;
            switch (filter) {
                case 0: out = v; break;
                case 1: out = v + a; break;
                case 2: out = v + b; break;
                case 3: out = v + ((a + b) >> 1); break;
                case 4: out = v + paeth(a, b, c); break;
                default: throw new Error(`unknown PNG filter ${filter}`);
            }
            lines[dst + x] = out & 0xff;
        }
    }

    const rgba = Buffer.alloc(width * height * 4);
    const sample = (line, index) => {
        const o = line + index * sampleBytes;
        return depth === 16 ? lines[o] : lines[o];
    };
    for (let y = 0; y < height; ++y) {
        const line = y * stride;
        for (let x = 0; x < width; ++x) {
            const i = x * channels;
            const o = (y * width + x) * 4;
            let r, g, b, a = 255;
            if (header.colorType === 0) {
                r = g = b = sample(line, i);
                if (transparency && transparency.length >= 2 && r === transparency.readUInt16BE(0)) a = 0;
            } else if (header.colorType === 2) {
                r = sample(line, i); g = sample(line, i + 1); b = sample(line, i + 2);
                if (transparency && transparency.length >= 6) {
                    if (r === transparency.readUInt16BE(0) && g === transparency.readUInt16BE(2) && b === transparency.readUInt16BE(4)) a = 0;
                }
            } else if (header.colorType === 3) {
                const idx = lines[line + i];
                if (!palette) throw new Error('indexed PNG without PLTE');
                r = palette[idx * 3]; g = palette[idx * 3 + 1]; b = palette[idx * 3 + 2];
                a = transparency && idx < transparency.length ? transparency[idx] : 255;
            } else if (header.colorType === 4) {
                r = g = b = sample(line, i); a = sample(line, i + 1);
            } else {
                r = sample(line, i); g = sample(line, i + 1); b = sample(line, i + 2); a = sample(line, i + 3);
            }
            rgba[o] = r; rgba[o + 1] = g; rgba[o + 2] = b; rgba[o + 3] = a;
        }
    }
    return { width, height, rgba };
}

// ---------------------------------------------------------------- BMP codec
function decodeBmp(buffer) {
    if (buffer.length < 54 || buffer.toString('ascii', 0, 2) !== 'BM') throw new Error('not a BMP file');
    const dataOffset = buffer.readUInt32LE(10);
    const width = buffer.readInt32LE(18);
    const rawHeight = buffer.readInt32LE(22);
    const height = Math.abs(rawHeight);
    const bpp = buffer.readUInt16LE(28);
    const compression = buffer.readUInt32LE(30);
    if (compression !== 0) throw new Error(`compressed BMP is not supported (method ${compression})`);
    if (![24, 32].includes(bpp)) throw new Error(`unsupported BMP bit depth ${bpp}`);
    const flip = rawHeight > 0;
    const bytesPerPixel = bpp / 8;
    const stride = Math.floor((bpp * width + 31) / 32) * 4;
    const rgba = Buffer.alloc(width * height * 4);
    for (let y = 0; y < height; ++y) {
        const srcRow = flip ? height - 1 - y : y;
        let off = dataOffset + srcRow * stride;
        for (let x = 0; x < width; ++x) {
            const o = (y * width + x) * 4;
            rgba[o] = buffer[off + 2];
            rgba[o + 1] = buffer[off + 1];
            rgba[o + 2] = buffer[off];
            rgba[o + 3] = bytesPerPixel === 4 ? buffer[off + 3] : 255;
            off += bytesPerPixel;
        }
    }
    return { width, height, rgba };
}

function loadImage(filePath) {
    const buffer = fs.readFileSync(filePath);
    const ext = path.extname(filePath).toLowerCase();
    let decoded;
    if (ext === '.bmp' || buffer.toString('ascii', 0, 2) === 'BM') decoded = decodeBmp(buffer);
    else decoded = decodePng(buffer);
    return {
        width: decoded.width,
        height: decoded.height,
        layers: [{
            name: path.basename(filePath),
            visible: true,
            opacity: 1,
            data: decoded.rgba,
        }],
    };
}

function saveImage(doc, filePath, options = {}) {
    const rgba = options.flatten === false && doc.layers.length === 1 ? doc.layers[0].data : composite(doc);
    const buffer = encodePng(rgba, doc.width, doc.height);
    fs.mkdirSync(path.dirname(path.resolve(filePath)), { recursive: true });
    fs.writeFileSync(filePath, buffer);
    return { path: path.resolve(filePath), bytes: buffer.length, width: doc.width, height: doc.height };
}

module.exports = {
    clamp,
    parseColor,
    namedColor,
    hexToRgba,
    rgbaToHex,
    rgbToHsv,
    hsvToRgb,
    createDoc,
    newLayer,
    composite,
    resizeDoc,
    resizeRgba,
    fillRect,
    strokeLine,
    strokeRect,
    strokeRectRing,
    strokeEllipse,
    strokeEllipseRing,
    fillEllipse,
    floodFill,
    stampDisc,
    stampEraser,
    samplePixel,
    parsePoints,
    polygonSignedArea,
    polygonSpans,
    fillPolygon,
    fillSpans,
    strokePolygon,
    strokePolygonRing,
    offsetPolygon,
    regularPolygonPoints,
    flipLayerData,
    rotateRgba90,
    cropRgba,
    stampImage,
    mirrorShapeArgs,
    noiseField,
    drawNoise,
    drawGradient,
    regionBounds,
    colourHistogram,
    medianCutPalette,
    paletteFromImage,
    quantizeLayer,
    paletteCounts,
    normalMapFromHeight,
    analyze,
    previewText,
    encodePng,
    decodePng,
    decodeBmp,
    loadImage,
    saveImage,
};
