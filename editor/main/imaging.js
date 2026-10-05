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

function blendPixel(data, w, h, x, y, rgba, alpha) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    const a = clamp(alpha, 0, 1);
    if (a <= 0) return;
    const o = (y * w + x) * 4;
    data[o] = Math.round(rgba[0] * 255 * a + data[o] * (1 - a));
    data[o + 1] = Math.round(rgba[1] * 255 * a + data[o + 1] * (1 - a));
    data[o + 2] = Math.round(rgba[2] * 255 * a + data[o + 2] * (1 - a));
    data[o + 3] = Math.max(data[o + 3], Math.round(a * 255));
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
function analyze(doc) {
    const compositeData = composite(doc);
    const histogram = { r: new Array(16).fill(0), g: new Array(16).fill(0), b: new Array(16).fill(0), a: new Array(16).fill(0) };
    let opaque = 0, transparent = 0, sum = [0, 0, 0, 0];
    for (let p = 0; p < doc.width * doc.height; ++p) {
        const o = p * 4;
        histogram.r[compositeData[o] >> 4]++;
        histogram.g[compositeData[o + 1] >> 4]++;
        histogram.b[compositeData[o + 2] >> 4]++;
        histogram.a[compositeData[o + 3] >> 4]++;
        if (compositeData[o + 3] === 0) transparent++; else opaque++;
        sum[0] += compositeData[o]; sum[1] += compositeData[o + 1]; sum[2] += compositeData[o + 2]; sum[3] += compositeData[o + 3];
    }
    const n = doc.width * doc.height;
    return {
        width: doc.width,
        height: doc.height,
        pixelCount: n,
        opaquePixels: opaque,
        transparentPixels: transparent,
        averageColor: sum.map(v => Math.round(v / n)),
        histogram,
    };
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
    strokeEllipse,
    fillEllipse,
    floodFill,
    stampDisc,
    stampEraser,
    samplePixel,
    normalMapFromHeight,
    analyze,
    encodePng,
    decodePng,
    decodeBmp,
    loadImage,
    saveImage,
};
